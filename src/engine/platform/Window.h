#pragma once

// WIN32_LEAN_AND_MEAN / NOMINMAX 는 vcxproj 전역 PreprocessorDefinitions 에서 정의.
#include <Windows.h>

#include <string>
#include <string_view>

namespace engine::platform
{
    // Win32 윈도우를 RAII로 캡슐화한다.
    // - 생성자에서 윈도우 클래스 등록(최초 1회)·HWND 생성·표시까지 수행.
    // - 소멸자에서 HWND 파괴.
    // - 단일 소유(복사·이동 금지). HWND 는 하나의 인스턴스만 보유한다.
    class Window
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

        bool IsOpen() const noexcept { return _isOpen; }

        // 클라이언트 영역(렌더 가능 영역) 크기. 윈도우 프레임/타이틀바 제외.
        int  Width()  const noexcept { return _width; }
        int  Height() const noexcept { return _height; }

        // HWND 외부 노출은 의도적으로 차단. 렌더러(SwapChain) 등 OS 핸들이 필요한
        // 구성요소는 friend 선언 또는 좁힌 어댑터를 통해 접근하도록 한다.

    private:
        static void EnsureClassRegistered();
        static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
        LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

        HWND _hwnd   = nullptr;
        int  _width  = 0;
        int  _height = 0;
        bool _isOpen = false;
    };
}
