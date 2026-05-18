#include "render/Animator.h"

#include "render/AnimClip.h"
#include "render/Skeleton.h"

#include <DirectXMath.h>
#include <algorithm>

namespace engine::render
{
    using DirectX::XMFLOAT4X4;
    using DirectX::XMMATRIX;
    using DirectX::XMMatrixIdentity;
    using DirectX::XMMatrixMultiply;
    using DirectX::XMLoadFloat4x4;
    using DirectX::XMStoreFloat4x4;

    Animator::Animator(const Skeleton& skeleton, const AnimClip& clip)
        : m_skeleton(skeleton), m_clip(clip)
    {
        m_palette.resize(skeleton.BoneCount());
        // 초기 identity — 첫 Update 전 렌더되어도 bind pose 변환만 적용.
        const XMFLOAT4X4 identity = []{
            XMFLOAT4X4 m;
            XMStoreFloat4x4(&m, XMMatrixIdentity());
            return m;
        }();
        std::fill(m_palette.begin(), m_palette.end(), identity);
    }

    void Animator::Update(float dt)
    {
        const double duration = m_clip.DurationSec();
        if (duration <= 0.0 || m_palette.empty()) { return; }

        m_currentSec += static_cast<double>(dt);
        // 루프 — 0..duration 사이로 정규화.
        while (m_currentSec >= duration) { m_currentSec -= duration; }
        while (m_currentSec < 0.0)        { m_currentSec += duration; }

        // 가장 가까운 키프레임 인덱스 — 본 단계는 보간 없음.
        // 각 본의 keyFrames 가 동일 frameRate 가정 → 균등 매핑.
        for (size_t b = 0; b < m_palette.size(); ++b)
        {
            const auto& kfs = (b < m_clip.bonesKeyFrames.size()) ? m_clip.bonesKeyFrames[b] : std::vector<KeyFrame>{};
            if (kfs.empty())
            {
                // 이 본은 키프레임 없음 — offsetMatrix 의 역을 곱하면 identity, 즉 그대로.
                // palette[b] = animatedGlobal * offset = bindGlobal * offset = identity.
                XMStoreFloat4x4(&m_palette[b], XMMatrixIdentity());
                continue;
            }

            // 시간 → 키프레임 인덱스. nearest (floor).
            const double t01 = m_currentSec / duration;
            size_t idx = static_cast<size_t>(t01 * static_cast<double>(kfs.size()));
            if (idx >= kfs.size()) { idx = kfs.size() - 1; }

            // animatedGlobal[b] = keyFrame.transform
            // palette[b] = animatedGlobal[b] * offsetMatrix[b] (row-major)
            const XMMATRIX kfMat     = XMLoadFloat4x4(&kfs[idx].transform);
            const XMMATRIX offsetMat = XMLoadFloat4x4(&m_skeleton.Bones()[b].offsetMatrix);
            const XMMATRIX combined  = XMMatrixMultiply(offsetMat, kfMat);
            XMStoreFloat4x4(&m_palette[b], combined);
        }
    }
}
