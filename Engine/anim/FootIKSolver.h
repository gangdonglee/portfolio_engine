#pragma once

#include "core/Types.h"

#include <DirectXMath.h>

#include <functional>
#include <string>
#include <vector>

namespace engine::render { class Skeleton; }
namespace engine::anim   { class AnimatorRuntime; }

namespace engine::anim
{
    // 두 발 Foot IK 솔버 — Two-bone IK (cosine 법칙).
    //
    // 입력 (per frame):
    //   - AnimatorRuntime (read BoneGlobal + write via SetBoneGlobal)
    //   - Skeleton (parent index + offsetMatrix(inv bind))
    //   - meshWorldMatrix: mesh-local → world (importTransform * instTransform)
    //   - groundSample(x, z) → world Y of ground at (x, z)
    //   - cfg (bone 이름, weight, etc)
    //
    // 출력:
    //   - AnimatorRuntime 의 boneGlobal[Hip/Knee/Ankle] + 자손 본들 (Toe 등) 갱신
    //   - palette 도 SetBoneGlobal 안에서 자동 갱신
    //   - lastIKResult: 디버그 마커 그리기용 (animation 발 위치 vs IK target 위치)
    struct FootIKConfig
    {
        // Mixamo 표준 이름 (단순 substring match — 좌/우 본 모두 동작):
        //   "LeftUpLeg" / "LeftLeg" / "LeftFoot"  |  "RightUpLeg" / "RightLeg" / "RightFoot"
        std::string leftHipBone   = "LeftUpLeg";
        std::string leftKneeBone  = "LeftLeg";
        std::string leftAnkleBone = "LeftFoot";

        std::string rightHipBone   = "RightUpLeg";
        std::string rightKneeBone  = "RightLeg";
        std::string rightAnkleBone = "RightFoot";

        // 보정 강도 0..1. 0 = IK 비활성, 1 = full IK.
        float weight = 1.0f;

        // ankle 발바닥 ↔ joint 사이 오프셋 (units). Mixamo X-Bot 의 LeftFoot bone 은 보통
        // 발등 근처 — ankle joint 가 ground 보다 5~7cm 위.
        float ankleHeightOffset = 7.0f;

        // 다리 너무 늘어남 방지 — Hip→Target 거리가 L1+L2 의 이 비율을 넘으면 clamp.
        float maxLegExtension = 0.99f;
    };

    // IK 적용 후 디버그용 결과 — 발마다 animation 위치 / target 위치 (world coords).
    struct FootIKDebug
    {
        bool             leftValid           = false;
        DirectX::XMFLOAT3 leftAnkleAnimWorld {};
        DirectX::XMFLOAT3 leftAnkleTargetWorld{};

        bool             rightValid          = false;
        DirectX::XMFLOAT3 rightAnkleAnimWorld {};
        DirectX::XMFLOAT3 rightAnkleTargetWorld{};
    };

    // 본 chain (hip/knee/ankle) 의 인덱스 캐시 — 매 프레임 본 이름 검색 회피.
    struct FootIKBoneIndices
    {
        engine::int32 leftHip   = -1;
        engine::int32 leftKnee  = -1;
        engine::int32 leftAnkle = -1;
        engine::int32 rightHip   = -1;
        engine::int32 rightKnee  = -1;
        engine::int32 rightAnkle = -1;

        bool LeftValid()  const noexcept { return leftHip  >= 0 && leftKnee  >= 0 && leftAnkle  >= 0; }
        bool RightValid() const noexcept { return rightHip >= 0 && rightKnee >= 0 && rightAnkle >= 0; }
    };

    // Skeleton 의 본 이름에서 IK chain 인덱스 추출. 이름이 정확 일치 또는 substring 포함.
    //   대소문자 구분 없이 비교.
    FootIKBoneIndices FindFootIKBones(const engine::render::Skeleton& skeleton,
                                       const FootIKConfig&             cfg);

    // 본 chain 의 자손 본들 — boneIdx 의 모든 descendants (BFS). IK 후 FK 갱신 대상.
    //   결과는 ancestor → descendant 순서로 정렬 (FK 가 top-down).
    std::vector<engine::int32> CollectDescendants(const engine::render::Skeleton& skeleton,
                                                   engine::int32                   boneIdx);

    // 메인 함수 — animator runtime 의 boneGlobal 을 in-place 수정.
    //   matInvWorld: world → mesh-local 의 역행렬 (caller 가 한 번 계산해 전달).
    void ApplyFootIK(AnimatorRuntime&                                 anim,
                     const engine::render::Skeleton&                  skeleton,
                     const FootIKBoneIndices&                         bones,
                     const FootIKConfig&                              cfg,
                     const DirectX::XMMATRIX&                         meshWorldMatrix,
                     const DirectX::XMMATRIX&                         meshWorldMatrixInv,
                     const std::function<float(float, float)>&        groundSample,
                     FootIKDebug&                                     outDebug);
}
