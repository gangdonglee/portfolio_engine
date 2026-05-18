#pragma once

#include "core/Types.h"

#include <DirectXMath.h>
#include <string>
#include <vector>

namespace engine::render
{
    // 키프레임 1개 — 시간(초) + 본의 글로벌 변환 (animatedGlobal[bone] @ time).
    struct KeyFrame
    {
        double               timeSec = 0.0;
        DirectX::XMFLOAT4X4  transform;   // row-major (FBX 측 column → 로딩 시 transpose)
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
