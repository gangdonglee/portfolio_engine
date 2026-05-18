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

    private:
        struct LoadedAsset
        {
            std::unique_ptr<engine::render::Mesh>                  mesh;
            std::unique_ptr<engine::render::Skeleton>              skeleton;
            std::vector<std::unique_ptr<engine::render::AnimClip>> clips;
        };

        engine::scene::Scene                                m_scene;
        std::unordered_map<std::string, LoadedAsset>        m_assetCache;

        // Animator 는 *첫 번째 FBX 인스턴스* 의 skeleton/clips 를 참조.
        // 본 단계 단순화 — 모든 인스턴스에 동일 palette 적용.
        engine::render::Skeleton*                                              m_animSkeleton = nullptr;
        const std::vector<std::unique_ptr<engine::render::AnimClip>>*          m_animClips    = nullptr;
        std::unique_ptr<engine::render::Animator>                              m_animator;
        int                                                                    m_currentClipIdx = -1;

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
