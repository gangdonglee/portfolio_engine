#include "engine/platform/Window.h"

#include <stdexcept>

namespace engine::platform
{
    namespace
    {
        constexpr wchar_t kWindowClassName[] = L"engine::platform::Window";

        HINSTANCE CurrentInstance() noexcept
        {
            return ::GetModuleHandleW(nullptr);
        }
    }

    // 윈도우 클래스는 프로세스 수명 동안 한 번만 등록한다.
    // 멀티 윈도우 시나리오에서도 모두 같은 클래스를 공유한다.
    // private static 멤버 함수로 둬서 StaticWndProc(private)에 접근 가능.
    void Window::EnsureClassRegistered()
    {
        static const bool registered = []
        {
            WNDCLASSEXW wc{};
            wc.cbSize        = sizeof(wc);
            wc.style         = CS_HREDRAW | CS_VREDRAW;
            wc.lpfnWndProc   = &Window::StaticWndProc;
            wc.hInstance     = CurrentInstance();
            wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
            // Phase 1D(SwapChain Clear) 도입 시 nullptr 로 복원 예정.
            // 그 전까지는 흰색 백버퍼 잔상으로 혼동되지 않도록 어두운 회색을 임시 표시.
            wc.hbrBackground = reinterpret_cast<HBRUSH>(::GetStockObject(DKGRAY_BRUSH));
            wc.lpszClassName = kWindowClassName;

            if (::RegisterClassExW(&wc) == 0)
            {
                throw std::runtime_error("RegisterClassExW failed");
            }
            return true;
        }();
        (void)registered;
    }

    Window::Window(int width, int height, std::wstring_view title)
        : _width(width), _height(height)
    {
        EnsureClassRegistered();

        constexpr DWORD style   = WS_OVERLAPPEDWINDOW;
        constexpr DWORD exStyle = 0;

        // 요청한 width/height 는 클라이언트 영역 기준.
        // 윈도우 전체 크기를 역산해서 CreateWindowExW 에 넘긴다.
        RECT rect{0, 0, width, height};
        ::AdjustWindowRectEx(&rect, style, FALSE, exStyle);

        const std::wstring titleOwned(title);

        _hwnd = ::CreateWindowExW(
            exStyle,
            kWindowClassName,
            titleOwned.c_str(),
            style,
            CW_USEDEFAULT, CW_USEDEFAULT,
            rect.right - rect.left,
            rect.bottom - rect.top,
            nullptr,
            nullptr,
            CurrentInstance(),
            this); // lpParam → WM_NCCREATE 에서 GWLP_USERDATA 로 저장

        if (_hwnd == nullptr)
        {
            throw std::runtime_error("CreateWindowExW failed");
        }

        ::ShowWindow(_hwnd, SW_SHOW);
        ::UpdateWindow(_hwnd);
        _isOpen = true;
    }

    Window::~Window()
    {
        // 정상 흐름에선 WM_DESTROY 가 _hwnd 를 nullptr 로 비움.
        // 이상 흐름(소멸자가 메시지 펌프보다 먼저 호출되는 경우 등) 대비:
        // ① nullptr 가드 ② IsWindow 가드(같은 HWND 가 이미 다른 경로로 파괴된 경우 보호).
        if (_hwnd != nullptr && ::IsWindow(_hwnd) != FALSE)
        {
            ::DestroyWindow(_hwnd);
        }
        _hwnd = nullptr;
    }

    void Window::PumpMessages()
    {
        MSG msg{};
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE) != FALSE)
        {
            if (msg.message == WM_QUIT)
            {
                _isOpen = false;
                return;
            }
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
    }

    LRESULT CALLBACK Window::StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        // WM_NCCREATE 시점에 CreateWindow 의 lpParam(this) 을 GWLP_USERDATA 에 저장.
        // 그 후 모든 메시지는 인스턴스 멤버 HandleMessage 로 위임된다.
        if (msg == WM_NCCREATE)
        {
            const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        }

        auto* self = reinterpret_cast<Window*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self != nullptr)
        {
            return self->HandleMessage(hwnd, msg, wParam, lParam);
        }
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    LRESULT Window::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
            case WM_SIZE:
            {
                _width  = LOWORD(lParam);
                _height = HIWORD(lParam);
                return 0;
            }
            case WM_CLOSE:
            {
                ::DestroyWindow(hwnd);
                return 0;
            }
            case WM_DESTROY:
            {
                // OS 가 윈도우를 이미 파괴 — 소멸자의 중복 DestroyWindow 회피.
                _hwnd = nullptr;
                ::PostQuitMessage(0);
                return 0;
            }
            default:
                return ::DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }
}
