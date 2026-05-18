#include "InputController.h"

#include "platform/Input.h"

#include <cstdint>
#include <utility>

namespace client
{
    void InputController::Tick(const engine::platform::Input& input)
    {
        // 키 → 의미 매핑. (VK code, clipIdx) — 0 키는 T-pose, 1..4 키는 clip 0..3.
        constexpr std::pair<std::uint32_t, int> kKeyMap[5] = {
            { static_cast<std::uint32_t>('0'), kClipTpose },
            { static_cast<std::uint32_t>('1'), 0 },
            { static_cast<std::uint32_t>('2'), 1 },
            { static_cast<std::uint32_t>('3'), 2 },
            { static_cast<std::uint32_t>('4'), 3 },
        };

        for (int i = 0; i < 5; ++i)
        {
            const bool cur = input.IsKeyDown(kKeyMap[i].first);
            if (cur && !m_prevDown[i])
            {
                // 다운 엣지 — 직전 미요청 우선 (한 프레임 안에서 여러 키 동시 다운 엣지 시 마지막 우선,
                // 본 단계에선 큰 의미 없음).
                m_pendingClip = kKeyMap[i].second;
            }
            m_prevDown[i] = cur;
        }
    }

    int InputController::ConsumeClipChange()
    {
        const int result = m_pendingClip;
        m_pendingClip = kNoClipChange;
        return result;
    }
}
