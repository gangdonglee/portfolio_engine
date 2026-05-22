#pragma once

#include "core/Types.h"
#include "render/SwapChain.h"
#include "scene/Scene.h"

#include <Windows.h>
#include <DirectXMath.h>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::anim
{
    struct AnimatorController;
    class  AnimatorRuntime;
}

namespace engine::render
{
    class AnimClip;
    class Animator;
    class Camera;
    class CommandList;
    class CommandQueue;
    class ConstantBuffer;
    class Device;
    class Mesh;
    class Skeleton;
    class SrvDescriptorHeap;
    class StructuredBuffer;
    class Texture;
}

struct ID3D12GraphicsCommandList;

namespace client
{
    // Scene 데이터 + 로드된 메시 자산 + Animator + 프레임당 GPU 자원 (cbuffer/SB) 의 묶음.
    // FrameRenderer 가 매 프레임 PrepareGpuResources / RecordDraw 를 호출하면 인스턴스별로
    // FrameConstants/BonePalette cbuffer 를 갱신하고 light StructuredBuffer 를 업로드 후
    // 메시별 draw 를 호출한다.
    //
    // 단일 소유. Application 이 라이프타임 보유.
    class SceneRuntime final
    {
    public:
        SceneRuntime(engine::render::Device&            device,
                     engine::render::CommandQueue&      queue,
                     engine::render::CommandList&       uploadList,
                     engine::render::SrvDescriptorHeap& srvHeap,
                     engine::scene::Scene               scene);
        ~SceneRuntime();

        SceneRuntime(const SceneRuntime&)            = delete;
        SceneRuntime& operator=(const SceneRuntime&) = delete;
        SceneRuntime(SceneRuntime&&)                 = delete;
        SceneRuntime& operator=(SceneRuntime&&)      = delete;

        // 매 프레임 1회 호출. Animator 시간 누적 + palette 갱신.
        void Tick(float dt);

        // InputController 가 키 입력에 응답해 호출.
        //   clipIdx >= 0  → loaded.clips[clipIdx] 활성
        //   clipIdx == -1 → animator nullptr (T-pose)
        void SetActiveClip(int clipIdx);

        // 활성화된 clip 인덱스 (-1 이면 T-pose).
        int  ActiveClip() const noexcept { return m_currentClipIdx; }

        // Animator 가 표시 가능한 clip 수 (FBX 의 클립 수, 미존재 시 0).
        size_t ClipCount() const noexcept;

        // FrameRenderer 가 호출 — light StructuredBuffer 업데이트 + 캐시된 view-proj/camPos 보관.
        void PrepareGpuResources(engine::uint32 frameIndex, const engine::render::Camera& camera);

        // FrameRenderer 가 호출 — 인스턴스별 cbuffer 갱신 + draw call.
        void RecordDraw(ID3D12GraphicsCommandList*    list,
                        engine::uint32                frameIndex,
                        const engine::render::Texture& fallbackAlbedo);

        // 카메라 초기 위치/대상 (Scene 의 CameraStart). Application 이 첫 Camera 구성에 사용.
        const engine::scene::CameraStart& InitialCameraStart() const noexcept { return m_scene.cameraStart; }

        // 게임 코드 측에서 점프 등 *임시 위치 오버라이드* 를 위해 mesh 인스턴스의 transform 에
        // write 권한 부여. Animator 활성 인스턴스 (= 첫 animatorControllerPath 인스턴스) 의
        // transform 을 반환 — 없으면 nullptr.
        engine::scene::Transform* AnimatorInstanceTransform() noexcept;

        // Animator 의 state 대표 duration (점프 물리 동기화 등). state 없거나 매핑 실패 → 0.
        float AnimatorStateDuration(std::string_view stateName) const noexcept;

        // 현재 Animator state 이름 / 현재 state 진행 시간 (sec).
        std::string AnimatorCurrentStateName() const;
        float       AnimatorCurrentStateTime() const noexcept;

        // 디버그 — Animator 시간 진행 일시정지 / frame 직접 이동.
        bool AnimatorIsPaused() const noexcept;
        void AnimatorSetPaused(bool paused) noexcept;
        void AnimatorSetCurrentStateTime(float t) noexcept;

        // 자동 floor align 용 — 본 이름 으로 *현재 frame 의 bone palette translation Y* 조회
        //   (= 본 origin 의 mesh-local Y). 본 없거나 Animator 없으면 0 반환.
        float AnimatorBoneMeshLocalY(std::wstring_view boneName) const;

        // AnimatorRuntime passthrough — Application 이 키 입력에 응답해 호출.
        // AnimatorRuntime 가 활성이 아니면 silent no-op.
        bool HasAnimatorRuntime() const noexcept;
        void SetAnimatorFloat   (std::string_view name, float value);
        void SetAnimatorBool    (std::string_view name, bool  value);
        void SetAnimatorTrigger (std::string_view name);

        // 디버그 — 현재 state 이름 (UI 표시용).
        std::string CurrentAnimatorStateName() const;

    private:
        struct LoadedAsset
        {
            std::unique_ptr<engine::render::Mesh>                  mesh;
            std::unique_ptr<engine::render::Skeleton>              skeleton;
            std::vector<std::unique_ptr<engine::render::AnimClip>> clips;
        };

        engine::scene::Scene                                m_scene;
        std::unordered_map<std::string, LoadedAsset>        m_assetCache;

        // 별도 클립 FBX 캐시 — Mixamo without-skin 등 메시 없이 클립만 있는 자산.
        // 키: MeshInstance.animationClipPath (빈 문자열이면 미사용).
        // 값: 베이스 메시의 스켈레톤 본 이름으로 매핑된 키프레임을 가진 AnimClip 들.
        std::unordered_map<std::string, std::vector<std::unique_ptr<engine::render::AnimClip>>>
            m_clipOnlyCache;

        // Animator 는 *첫 번째 FBX 인스턴스* 의 skeleton/clips 를 참조.
        // 본 단계 단순화 — 모든 인스턴스에 동일 palette 적용.
        engine::render::Skeleton*                                              m_animSkeleton = nullptr;
        const std::vector<std::unique_ptr<engine::render::AnimClip>>*          m_animClips    = nullptr;
        std::unique_ptr<engine::render::Animator>                              m_animator;
        int                                                                    m_currentClipIdx = -1;

        // Animator Controller 런타임 — Phase 5-M1.
        // animatorControllerPath 가 있는 *첫 번째* MeshInstance 의 controller 만 활성.
        // controller 의 모든 state.motionClipPath 는 m_controllerClipCache 안에 사전 로드되어
        // AnimatorRuntime 에 clipMap (path → AnimClip*) 으로 전달됨.
        //
        // **append-only**: AnimatorRuntime 의 ClipMap 이 m_controllerClipCache 의 unique_ptr 의
        //   raw 포인터(get())를 보유. erase / 재할당 도입 시 dangling 위험.
        //   다중 controller 또는 동적 자산 언로드 도입 시 weak_ptr 또는 핸들 ID 로 격상.
        std::unique_ptr<engine::anim::AnimatorController>                      m_loadedController;
        std::unordered_map<std::string,
            std::vector<std::unique_ptr<engine::render::AnimClip>>>            m_controllerClipCache;
        std::unique_ptr<engine::anim::AnimatorRuntime>                         m_animatorRuntime;

        // 인스턴스별 cbuffer (FrameConstants + BonePalette). 슬롯당 N프레임 in-flight.
        // SwapChain::kBackBufferCount 와 1소스 통일 — FrameRenderer 와의 frameIndex 정합 보장.
        static constexpr engine::uint32 kFrameCount = engine::render::SwapChain::kBackBufferCount;
        std::vector<std::array<std::unique_ptr<engine::render::ConstantBuffer>, kFrameCount>> m_instFrameCBs;
        std::vector<std::array<std::unique_ptr<engine::render::ConstantBuffer>, kFrameCount>> m_instBoneCBs;

        // 라이트 StructuredBuffer (frame-shared, 모든 인스턴스에 동일).
        static constexpr engine::uint32 kDirLightCapacity   = 16;
        static constexpr engine::uint32 kPointLightCapacity = 64;
        std::array<std::unique_ptr<engine::render::StructuredBuffer>, kFrameCount> m_dirLightSBs;
        std::array<std::unique_ptr<engine::render::StructuredBuffer>, kFrameCount> m_pointLightSBs;

        // 매 프레임 PrepareGpuResources 에서 갱신, RecordDraw 에서 사용.
        DirectX::XMMATRIX m_cachedViewProj{};
        DirectX::XMFLOAT3 m_cachedCameraPos{};
    };
}
