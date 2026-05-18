#include "InputController.h"

#include "platform/Input.h"

#include <Windows.h>   // VK_F1..VK_F9

#include <cstdint>
#include <utility>

namespace client
{
    void InputController::Tick(const engine::platform::Input& input)
    {
        // 키 → 클립 매핑. (VK code, clipIdx) — 0 키는 T-pose, 1..4 키는 clip 0..3.
        constexpr std::pair<std::uint32_t, int> kClipKeys[5] = {
            { static_cast<std::uint32_t>('0'), kClipTpose },
            { static_cast<std::uint32_t>('1'), 0 },
            { static_cast<std::uint32_t>('2'), 1 },
            { static_cast<std::uint32_t>('3'), 2 },
            { static_cast<std::uint32_t>('4'), 3 },
        };

        for (int i = 0; i < 5; ++i)
        {
            const bool cur = input.IsKeyDown(kClipKeys[i].first);
            if (cur && !m_prevClipDown[i]) { m_pendingClip = kClipKeys[i].second; }
            m_prevClipDown[i] = cur;
        }

        // F1..F9 → 씬 슬롯 0..8 다운 엣지.
        for (int i = 0; i < kSceneSwitchSlotCount; ++i)
        {
            const auto vk = static_cast<std::uint32_t>(VK_F1 + i);
            const bool cur = input.IsKeyDown(vk);
            if (cur && !m_prevSceneDown[i]) { m_pendingSceneSwitch = i; }
            m_prevSceneDown[i] = cur;
        }
    }

    int InputController::ConsumeClipChange()
    {
        const int result = m_pendingClip;
        m_pendingClip = kNoClipChange;
        return result;
    }

    int InputController::ConsumeSceneSwitch()
    {
        const int result = m_pendingSceneSwitch;
        m_pendingSceneSwitch = kNoSceneSwitch;
        return result;
    }
}
