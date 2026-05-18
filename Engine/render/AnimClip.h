#pragma once

#include "core/Types.h"

#include <DirectXMath.h>
#include <string>
#include <vector>

namespace engine::render
{
    // 키프레임 1개 — 시간(초) + 본의 글로벌 변환 matrix.
    // Animator 가 frame N/N+1 의 matrix 를 element-wise lerp (rotation 부분이 짧은 frame
    // 간격에서는 근사 정확). 정밀 Slerp 는 reflect (det=-1) matrix 의 quaternion 분해 부정확
    // 문제로 보류 — 추후 reflect 적용 전 SQT 추출 또는 quaternion 일관성 보정으로 도입.
    struct KeyFrame
    {
        double               timeSec = 0.0;
        DirectX::XMFLOAT4X4  transform;
    };

    // 한 애니메이션 클립 — perBone keyframes.
    //   bonesKeyFrames[boneIdx][frameIdx] = KeyFrame.
    //   본별로 키프레임 수가 같을 필요는 없지만 본 단계는 일정 frameRate 가정 (FBX SDK 가 1초당 N프레임 균등 추출).
    class AnimClip final
    {
    public:
        std::wstring                          name;
        double                                startSec = 0.0;
        double                                endSec   = 0.0;
        std::vector<std::vector<KeyFrame>>    bonesKeyFrames;   // [boneIdx][frameIdx]

        double DurationSec() const noexcept { return endSec - startSec; }
    };
}
