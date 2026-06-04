#pragma once

#include "core/Types.h"
#include "render/SwapChain.h"
#include "scene/Scene.h"

#include <Windows.h>
#include <DirectXMath.h>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::anim
{
    struct AnimatorController;
    class  AnimatorRuntime;
    struct FootIKConfig;
    struct FootIKBoneIndices;
    struct FootIKDebug;
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
    class ShadowMap;
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

        // 그림자 깊이 패스 — 라이트 시점에서 모든 인스턴스의 깊이만 기록. RecordDraw 이전에 호출.
        void RecordShadowDraw(ID3D12GraphicsCommandList* list, engine::uint32 frameIndex);

        // 그림자맵 주입 (nullptr=그림자 비활성). PrepareGpuResources 가 lightViewProj 계산.
        void SetShadowMap(const engine::render::ShadowMap* map) noexcept { m_shadowMap = map; }

        // 셰이더 톤맵 여부. true(기본)=셰이더에서 Reinhard(에디터). false=선형 HDR(게임, bloom composite 가 톤맵).
        void SetApplyTonemap(bool v) noexcept { m_applyTonemap = v; }

        // 태양을 향하는 방향(=-dirLights[0].directionWS, 정규화). 없으면 위쪽. 스카이박스 cbuffer 용.
        DirectX::XMFLOAT3 SunDirectionWS() const noexcept
        {
            if (m_scene.dirLights.empty()) { return DirectX::XMFLOAT3{ 0.0f, 1.0f, 0.0f }; }
            const auto& d = m_scene.dirLights[0].directionWS;
            DirectX::XMFLOAT3 r{};
            DirectX::XMStoreFloat3(
                &r, DirectX::XMVector3Normalize(DirectX::XMVectorSet(-d.x, -d.y, -d.z, 0.0f)));
            return r;
        }

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
        float AnimatorBoneMeshLocalX(std::wstring_view boneName) const;   // 진단용

        // 현재 Animator state 의 ballistic root motion Y 오프셋. 없으면 0.
        // Application 이 transform.position.y 에 가산 — Jump 코드측 하드코딩 제거용.
        float AnimatorRootMotionY() const;

        // AnimatorRuntime passthrough — Application 이 키 입력에 응답해 호출.
        // AnimatorRuntime 가 활성이 아니면 silent no-op.
        bool HasAnimatorRuntime() const noexcept;
        void SetAnimatorFloat   (std::string_view name, float value);
        void SetAnimatorBool    (std::string_view name, bool  value);
        void SetAnimatorTrigger (std::string_view name);

        // === Foot IK ===
        // Tick(dt) 안에서 AnimatorRuntime::Update 직후 ApplyFootIK 호출.
        //   활성 instance 의 importTransform * transform 으로 mesh world 계산.
        //   ground sampler 가 nullptr 이면 Y=0 평면 사용.
        void SetFootIKEnabled  (bool enabled) noexcept { m_footIKEnabled = enabled; }
        // Foot IK 전역 weight (0..1) — 호출자(게임)가 *이동 속도* 로 페이드. 빠른 달리기에선 낮춰
        //   IK 간섭을 줄임(발이 너무 빨라 IK 가 득보다 실 — 프로덕션 표준). 보정량·무릎굽힘에 곱해짐.
        void SetFootIKWeight   (float w) noexcept { m_footIKWeight = (w < 0.0f) ? 0.0f : (w > 1.0f ? 1.0f : w); }
        bool FootIKEnabled     () const noexcept       { return m_footIKEnabled; }
        void SetFootIKConfig   (const engine::anim::FootIKConfig& cfg);
        const engine::anim::FootIKConfig&  FootIKConfigRef() const noexcept;
        engine::anim::FootIKConfig&        FootIKConfigMutable() noexcept;
        void SetGroundSampler  (std::function<float(float, float)> fn) { m_groundSampler = std::move(fn); }
        const engine::anim::FootIKDebug&   LastFootIKDebug() const noexcept;

        // Foot IK 캘리브레이션 readout — 디버그 오버레이용. 마지막 프레임의 ankle world Y / 발밑 지면 Y.
        struct FootIKReadout
        {
            float leftAnkleY   = 0.0f;  float leftGroundY  = 0.0f;  float leftPlant  = 0.0f;
            float rightAnkleY  = 0.0f;  float rightGroundY = 0.0f;  float rightPlant = 0.0f;
            float bodyLower    = 0.0f;  // 낮은 발 닿게 몸(root) 하강량(pelvis IK)
            bool  valid = false;
        };
        const FootIKReadout& FootIKReadoutRef() const noexcept { return m_footIKReadout; }

        // 디버그 — 현재 state 이름 (UI 표시용).
        std::string CurrentAnimatorStateName() const;

        // === 스켈레톤 시각화 ===
        // 본 parent→child 의 *world-space* 선분 끝점 쌍을 outPairs 에 채움.
        //   각 본 b (parent>=0): {parentWorldPos, boneWorldPos}.
        //   world = boneMeshLocalPos * (importTransform * instTransform).
        //   animator 활성 instance 의 skeleton + BoneGlobal 사용. 없으면 false.
        // Foot IK 기반 작업 (관절 위치 파악) + 좌표 규약 진단용.
        bool GetSkeletonWorldSegments(
            std::vector<std::pair<DirectX::XMFLOAT3, DirectX::XMFLOAT3>>& outPairs) const;

        // 본 단위 데이터 — picking / 선택 / UI 용. 인덱스 = 본 인덱스 (정렬 일치).
        //   outPositions: 각 본의 world 위치.
        //   outParent:    각 본의 부모 인덱스 (-1 = 루트).
        //   outNames:     각 본의 이름 (ascii).
        // animator 활성 instance 없으면 false (셋 다 비움).
        bool GetSkeletonWorldJoints(
            std::vector<DirectX::XMFLOAT3>& outPositions,
            std::vector<int>&               outParent,
            std::vector<std::string>&       outNames) const;

        // === 본 수동 포징 (manual posing) ===
        // 선택 본을 모델공간 회전축(axisModel) 둘레로 angleDelta(rad) 만큼 누적 회전.
        //   boneGlobal 은 매 Tick BuildPalette 가 애니메이션에서 재생성하므로, 누적 회전을
        //   별도 맵에 저장하고 Tick 의 Update 직후 subtree 에 적용 (palette 덮어씀).
        //   같은 본에 반복 호출 시 회전 누적 (드래그 연속).
        //   axisWorld 는 *world-space* 회전축 (camera right/up 등) — 내부에서 mesh-local 로 변환.
        void AddBoneManualRotation(int boneIdx,
                                   const DirectX::XMFLOAT3& axisWorld,
                                   float angleDelta);
        void ClearBoneManualPosing() noexcept;
        bool HasBoneManualPosing() const noexcept;

        // === 끝 관절 IK (CCD) — Phase 3 ===
        // 끝 관절(endBoneIdx)을 targetWorld 로 끌어당기도록 그 *부모 체인* (chainLength 단계)을
        //   CCD (Cyclic Coordinate Descent) 로 굽힌다. 끝 관절 자체는 effector tip — 회전 안 함.
        //   각 체인 joint 의 증분 회전을 m_boneManualRot 에 누적 (FK 드래그와 동일 경로 →
        //   rigid subtree 회전이라 mesh 안 깨짐). 드래그 연속 호출로 매 프레임 커서로 수렴.
        //   targetWorld 는 내부에서 mesh-local(model) 공간으로 변환.
        void SolveBoneIK(int endBoneIdx,
                         const DirectX::XMFLOAT3& targetWorld,
                         int chainLength,
                         int iterations);

        // Editor 전용 — 외부 Scene 의 transform / importTransform / ambient / lights 를
        // 내부 m_scene 으로 cheap-copy (자산 재로드 없음). path 필드는 무시 — 자산 교체는
        // 호출자가 SceneRuntime 재생성으로 처리해야 한다.
        // meshes.size() 가 source 와 다르면 min 만큼만 매칭 인덱스 동기화 — caller 가 size 일치 책임.
        void SyncEditableFieldsFrom(const engine::scene::Scene& source) noexcept;

    private:
        // 수동 포징 적용 — Tick 의 Update 직후 호출. m_boneManualRot 의 각 본 subtree 에
        //   rotate-about-pivot (모델공간 column-convention) 적용 후 SetBoneGlobal.
        void ApplyManualBonePosing();

        // 본 boneIdx 의 누적 모델공간 회전에 dq(모델공간 quaternion)를 합성. AddBoneManualRotation
        //   과 SolveBoneIK 의 공통 누적 경로 — 둘 다 최종적으로 m_boneManualRot 에 기록.
        void AccumulateBoneModelQuat(int boneIdx, DirectX::FXMVECTOR dq);

        // 런타임 Foot IK — Tick 의 Update(BuildPalette) 직후 호출. 두 발(ankle)을 각자 발밑 지면
        //   높이로 끌어당기도록 leg chain(hip/knee)을 CCD(rotate-about-pivot, column-convention)로
        //   *직접 BoneGlobal 에* 적용 (누적 X, 매 프레임 애니 포즈에서 새로). SolveBoneIK 와 동일한
        //   검증된 기법이라 mesh 정상 변형. (예전 ApplyFootIK 의 rotation 재구성 결함 회피.)
        void ApplyFootIKRuntime();

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

        // Foot IK 상태 — Tick 안에서 AnimatorRuntime::Update 직후 ApplyFootIK 호출.
        //   m_footIKBones: 첫 controller 로드 시 1회 캐시 (본 이름 → 인덱스).
        //   m_groundSampler: nullable. nullptr 이면 Y=0 평면.
        //   default DISABLED — FBX 로더가 본 행렬을 비표준 규약(ConvertMatrix transpose +
        //   matReflect Y/Z swap, vertical=m[2][3])으로 저장. FootIK 의 rotation 재구성이
        //   표준 DirectX 행렬 연산을 이 비표준 행렬에 적용 → mesh 뒤틀림.
        //   제대로 하려면 skeleton 을 표준 좌표 공간으로 정규화하는 선행 작업 필요 (FBX 로더
        //   재작업 수준). 그 전까지 비활성. ground snap(CharacterController)은 정상 작동.
        bool                                            m_footIKEnabled  = false;
        float                                           m_footIKWeight   = 1.0f;   // 속도 기반 페이드 (게임이 설정)
        FootIKReadout                                   m_footIKReadout;           // 디버그 오버레이 calibration
        float                                           m_footIKCorrSmooth[2] = { 0.0f, 0.0f }; // 발[L,R] 보정 temporal lerp (plant/swing 전환 pop 방지)
        float                                           m_footIKBodyLower     = 0.0f; // 낮은 발이 닿게 몸(root) 하강량 — terrain-keyed, 강한 스무딩(보행 bob 방지)

        // 본 수동 포징 — boneIdx → 누적 모델공간 회전 (quaternion). Tick 의 Update 직후
        //   각 본 subtree 에 rotate-about-pivot 적용 (BuildPalette 결과 덮어씀).
        //   hierarchy 순서 (인덱스 오름차순 ≈ parent-first) 로 적용 — 중첩 조작 합성.
        std::unordered_map<int, DirectX::XMFLOAT4>      m_boneManualRot;
        std::unique_ptr<engine::anim::FootIKConfig>     m_footIKConfig;     // unique_ptr — incomplete type 회피
        std::unique_ptr<engine::anim::FootIKBoneIndices> m_footIKBones;
        std::unique_ptr<engine::anim::FootIKDebug>      m_footIKDebug;
        std::function<float(float, float)>              m_groundSampler;
        size_t                                          m_animatorInstanceIdx = 0;  // animator 활성 instance — world matrix 계산용

        // 인스턴스별 cbuffer (FrameConstants + BonePalette). 슬롯당 N프레임 in-flight.
        // SwapChain::kBackBufferCount 와 1소스 통일 — FrameRenderer 와의 frameIndex 정합 보장.
        static constexpr engine::uint32 kFrameCount = engine::render::SwapChain::kBackBufferCount;
        std::vector<std::array<std::unique_ptr<engine::render::ConstantBuffer>, kFrameCount>> m_instFrameCBs;
        std::vector<std::array<std::unique_ptr<engine::render::ConstantBuffer>, kFrameCount>> m_instBoneCBs;
        // 그림자 깊이 패스용 인스턴스별 cbuffer (worldLightMVP 1행렬). 메인과 별도 — 같은 프레임에
        //   두 패스가 다른 행렬을 쓰므로 cbuffer 를 공유하면 GPU 가 마지막 값만 봄.
        std::vector<std::array<std::unique_ptr<engine::render::ConstantBuffer>, kFrameCount>> m_instShadowCBs;

        // 라이트 StructuredBuffer (frame-shared, 모든 인스턴스에 동일).
        static constexpr engine::uint32 kDirLightCapacity   = 16;
        static constexpr engine::uint32 kPointLightCapacity = 64;
        std::array<std::unique_ptr<engine::render::StructuredBuffer>, kFrameCount> m_dirLightSBs;
        std::array<std::unique_ptr<engine::render::StructuredBuffer>, kFrameCount> m_pointLightSBs;

        // normal map 없는 머티리얼용 폴백 — 1x1 평탄 노멀(128,128,255 = 탄젠트 +Z). 효과 없음.
        std::unique_ptr<engine::render::Texture> m_flatNormalTex;

        // 매 프레임 PrepareGpuResources 에서 갱신, RecordDraw 에서 사용.
        DirectX::XMMATRIX m_cachedViewProj{};
        DirectX::XMFLOAT3 m_cachedCameraPos{};

        // 그림자 — PrepareGpuResources 에서 방향광+캐릭터 위치로 lightViewProj 계산.
        const engine::render::ShadowMap* m_shadowMap = nullptr;   // nullptr=비활성(에디터 등)
        DirectX::XMMATRIX m_cachedLightViewProj{};
        bool              m_shadowActive = false;   // 이번 프레임 그림자 샘플 여부
        bool              m_applyTonemap = true;    // 셰이더 톤맵(기본 on=에디터). 게임은 false 설정.
    };
}
