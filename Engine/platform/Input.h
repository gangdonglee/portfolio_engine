#pragma once

#include "core/Types.h"

namespace engine::platform
{
    // 키보드 + 마우스 입력 상태 캡슐화.
    //
    // 라이프타임:
    //   - Window 가 멤버로 보유. window.GetInput() 으로 접근.
    //   - WndProc 에서 키/마우스 메시지를 OnXxx 콜백으로 전달.
    //
    // 매 프레임 흐름:
    //   1) window.PumpMessages() — WndProc 가 m_keys / m_mouseX 등 갱신.
    //   2) input.BeginFrame() — 이번 프레임 마우스 델타 계산 + prev 갱신.
    //   3) 게임 코드가 IsKeyDown / MouseDeltaX 등 조회.
    //
    // 키 코드: Win32 VK_* 상수 사용 (VK_W, VK_SPACE 등). 0~255 범위.
    // 마우스 버튼: 0=Left, 1=Right, 2=Middle.
    //
    // 단일 소유 (복사·이동 금지).
    class Input final
    {
    public:
        Input();
        ~Input();

        Input(const Input&)            = delete;
        Input& operator=(const Input&) = delete;
        Input(Input&&)                 = delete;
        Input& operator=(Input&&)      = delete;

        // 매 프레임 시작 시 (PumpMessages 직후) 호출.
        void BeginFrame();

        // WndProc 호출용 — Window 만 사용.
        void OnKeyDown(uint32 vkey);
        void OnKeyUp  (uint32 vkey);
        void OnMouseMove(int32 x, int32 y);
        void OnMouseButton(int32 button, bool down);
        // WM_MOUSEWHEEL 의 wheel notch. Win32 는 ±WHEEL_DELTA (=120) 단위로 누적 전달 —
        //   여기선 normalized notch 만 누적 (휠 한 칸 = +1, 반대 방향 = -1).
        //   같은 프레임 안에서 여러 번 호출 가능 → += 누적.
        void OnMouseWheel(int32 wheelNotches);

        // 게임 코드 조회.
        bool  IsKeyDown(uint32 vkey)         const noexcept;
        bool  IsMouseButtonDown(int32 button) const noexcept;
        int32 MouseX()                       const noexcept { return m_mouseX; }
        int32 MouseY()                       const noexcept { return m_mouseY; }
        int32 MouseDeltaX()                  const noexcept { return m_mouseDeltaX; }
        int32 MouseDeltaY()                  const noexcept { return m_mouseDeltaY; }
        // 이번 프레임에 누적된 마우스 휠 노치 수 (위 스크롤 = 양수). BeginFrame 시 0 으로 리셋.
        int32 MouseWheel()                   const noexcept { return m_wheelDelta; }

    private:
        bool  m_keys[256]{};            // VK_* 인덱싱
        bool  m_buttons[3]{};           // 0=L, 1=R, 2=M
        int32 m_mouseX      = 0;
        int32 m_mouseY      = 0;
        int32 m_prevMouseX  = 0;
        int32 m_prevMouseY  = 0;
        int32 m_mouseDeltaX = 0;
        int32 m_mouseDeltaY = 0;
        // m_wheelAccum 은 WndProc 가 OnMouseWheel 로 누적, BeginFrame 이 snapshot → m_wheelDelta
        // 로 옮기고 accum 을 0 으로 reset. PumpMessages → BeginFrame → 게임코드 read 순서에서
        // 이전 프레임 잔여 / 이번 프레임 신규를 깔끔히 분리.
        int32 m_wheelAccum  = 0;
        int32 m_wheelDelta  = 0;
    };
}
