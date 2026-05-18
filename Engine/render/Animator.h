#pragma once

#include "core/Types.h"

#include <DirectXMath.h>
#include <vector>

namespace engine::render
{
    class Skeleton;
    class AnimClip;

    // 시간 → 본 팔레트 계산기.
    //
    // 매 프레임 Update(dt) 호출 → m_currentTime 진행 → 보간된 본 변환 계산 → palette 채움.
    //   palette[i] = animatedGlobal[i] * offsetMatrix[i]
    //   (offsetMatrix = inverse bind pose, Skeleton 에 보관)
    //
    // 본 단계는 single clip + 단순 nearest-frame 샘플 (보간 X).
    // 향후: Lerp/Slerp 보간, 블렌딩, IK.
    class Animator final
    {
    public:
        // skeleton/clip 은 non-owning 참조 (LoadedFbxModel 의 라이프타임 안에서 사용).
        Animator(const Skeleton& skeleton, const AnimClip& clip);

        // 매 프레임 시간 진행 + 본 팔레트 갱신. dt 초 단위.
        void Update(float dt);

        // 본 팔레트 (row-major float4x4 N개). HLSL b1 cbuffer 에 그대로 업로드.
        const std::vector<DirectX::XMFLOAT4X4>& Palette() const noexcept { return m_palette; }
        size_t                                  PaletteSize() const noexcept { return m_palette.size(); }

        void SetTimeSec(double t) noexcept { m_currentSec = t; }
        double CurrentSec() const noexcept { return m_currentSec; }

    private:
        const Skeleton&                   m_skeleton;
        const AnimClip&                   m_clip;
        double                            m_currentSec = 0.0;
        std::vector<DirectX::XMFLOAT4X4>  m_palette;     // 매 Update 후 갱신
    };
}
