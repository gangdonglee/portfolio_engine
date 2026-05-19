#pragma once

// WIN32_LEAN_AND_MEAN / NOMINMAX 는 vcxproj 전역 PreprocessorDefinitions 에서 정의.
#include <Windows.h>

#include <functional>
#include <string>
#include <string_view>

#include "platform/Input.h"

namespace engine::platform
{
    // Win32 윈도우를 RAII로 캡슐화한다.
    // - 생성자에서 윈도우 클래스 등록(최초 1회)·HWND 생성·표시까지 수행.
    // - 소멸자에서 HWND 파괴.
    // - 단일 소유(복사·이동 금지). HWND 는 하나의 인스턴스만 보유한다.
    class Window final
    {
    public:
        Window(int width, int height, std::wstring_view title);
        ~Window();

        Window(const Window&)            = delete;
        Window& operator=(const Window&) = delete;
        Window(Window&&)                 = delete;
        Window& operator=(Window&&)      = delete;

        // 메시지 큐의 모든 메시지를 처리한다.
        // WM_QUIT 를 수신하면 내부 상태를 닫힘으로 전환한다.
        void PumpMessages();

        bool IsOpen() const noexcept { return m_isOpen; }

        // 클라이언트 영역(렌더 가능 영역) 크기. 윈도우 프레임/타이틀바 제외.
        int  Width()  const noexcept { return m_width; }
        int  Height() const noexcept { return m_height; }

        // 키보드/마우스 입력 상태. 매 프레임 PumpMessages 직후 Input::BeginFrame() 권장.
        Input&       GetInput()       noexcept { return m_input; }
        const Input& GetInput() const noexcept { return m_input; }

        // 리사이즈 dirty 플래그 소비. true 반환 시 dirty 클리어 — 호출자는 새 Width()/Height() 로
        // 스왑체인/뎁스버퍼/뷰포트를 갱신해야 한다.
        // SIZE_MINIMIZED 와 0x0 크기는 dirty 를 켜지 않는다 (ResizeBuffers 가 거부).
        bool ConsumeResize() noexcept;

        // 외부(예: ImGui Win32 backend) 가 WndProc 메시지를 가로채야 할 때 사용.
        // 반환값이 0 이 아니면 메시지가 처리된 것으로 간주하고 Window 의 핸들러를 스킵한다.
        // 입력 메시지를 가로챈 경우 ImGui 가 키/마우스를 먹어 게임 입력으로 흘러가지 않게 됨.
        // 단일 훅 — 마지막 등록자가 우선. nullptr 로 해제.
        using WndProcHook = std::function<LRESULT(HWND, UINT, WPARAM, LPARAM)>;
        void SetWndProcHook(WndProcHook hook) { m_wndProcHook = std::move(hook); }

        // HWND 노출 — Editor/Tool 빌드에서 ImGui Win32 backend 가 직접 필요로 함.
        // 게임 코드(Client) 는 이 핸들을 사용하지 않도록 — engine::render::SwapChain 만 friend 로 접근.
        HWND NativeHwnd() const noexcept { return m_hwnd; }

        // 타이틀바 텍스트 갱신 — 씬 전환 / 로딩 진행 등 즉시 시각 피드백 용도.
        // SetWindowTextW 호출. HWND 가 nullptr 이면 no-op.
        void SetTitle(const std::wstring& title);

    private:
        static void EnsureClassRegistered();
        static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
        LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

        HWND        m_hwnd         = nullptr;
        int         m_width        = 0;
        int         m_height       = 0;
        bool        m_isOpen       = false;
        bool        m_resizeDirty  = false;
        Input       m_input;
        WndProcHook m_wndProcHook;  // Editor/Tool 전용 — 평소엔 비어 있음.
    };
}
