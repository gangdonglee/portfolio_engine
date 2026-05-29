#include "platform/Window.h"

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
            // Phase 1D-4(SwapChain Clear) 도입 완료 — 렌더러가 매 프레임 모든 픽셀을 채운다.
            // Windows 자동 배경 페인트는 불필요하며, 렌더 클리어와 충돌해 깜빡임을 유발할 수 있어
            // nullptr 로 설정한다.
            wc.hbrBackground = nullptr;
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
        : m_width(width), m_height(height)
    {
        EnsureClassRegistered();

        constexpr DWORD style   = WS_OVERLAPPEDWINDOW;
        constexpr DWORD exStyle = 0;

        // 요청한 width/height 는 클라이언트 영역 기준.
        // 윈도우 전체 크기를 역산해서 CreateWindowExW 에 넘긴다.
        RECT rect{0, 0, width, height};
        ::AdjustWindowRectEx(&rect, style, FALSE, exStyle);

        const std::wstring titleOwned(title);

        m_hwnd = ::CreateWindowExW(
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

        if (m_hwnd == nullptr)
        {
            throw std::runtime_error("CreateWindowExW failed");
        }

        ::ShowWindow(m_hwnd, SW_SHOW);
        ::UpdateWindow(m_hwnd);
        m_isOpen = true;
    }

    Window::~Window()
    {
        // 정상 흐름에선 WM_DESTROY 가 m_hwnd 를 nullptr 로 비움.
        // 이상 흐름(소멸자가 메시지 펌프보다 먼저 호출되는 경우 등) 대비:
        // ① nullptr 가드 ② IsWindow 가드(같은 HWND 가 이미 다른 경로로 파괴된 경우 보호).
        if (m_hwnd != nullptr && ::IsWindow(m_hwnd) != FALSE)
        {
            ::DestroyWindow(m_hwnd);
        }
        m_hwnd = nullptr;
    }

    void Window::SetTitle(const std::wstring& title)
    {
        if (m_hwnd != nullptr) { ::SetWindowTextW(m_hwnd, title.c_str()); }
    }

    bool Window::ConsumeResize() noexcept
    {
        if (!m_resizeDirty) { return false; }
        m_resizeDirty = false;
        return true;
    }

    void Window::PumpMessages()
    {
        MSG msg{};
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE) != FALSE)
        {
            if (msg.message == WM_QUIT)
            {
                m_isOpen = false;
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
        // 외부 훅(ImGui 등) 이 먼저 메시지를 본다. 0 이외의 반환값은 "처리 완료" 로 해석.
        // 단, 시스템 메시지(WM_CLOSE/WM_DESTROY/WM_SIZE) 는 Window 가 직접 처리해야
        // 라이프사이클·리사이즈 추적이 깨지지 않으므로 훅을 우회한다.
        const bool isSystemMsg =
            (msg == WM_CLOSE) || (msg == WM_DESTROY) || (msg == WM_SIZE) || (msg == WM_NCCREATE);
        if (!isSystemMsg && m_wndProcHook)
        {
            const LRESULT hookResult = m_wndProcHook(hwnd, msg, wParam, lParam);
            if (hookResult != 0)
            {
                return hookResult;
            }
        }

        switch (msg)
        {
            case WM_SIZE:
            {
                // SIZE_MINIMIZED 는 무시 — 0x0 으로 ResizeBuffers 호출 시 D3D12 가 거부.
                // 복귀 시 SIZE_RESTORED 가 다시 와서 정상 크기로 dirty 설정됨.
                const int newWidth  = static_cast<int>(LOWORD(lParam));
                const int newHeight = static_cast<int>(HIWORD(lParam));
                if (wParam != SIZE_MINIMIZED && newWidth > 0 && newHeight > 0)
                {
                    if (newWidth != m_width || newHeight != m_height)
                    {
                        m_width        = newWidth;
                        m_height       = newHeight;
                        m_resizeDirty  = true;
                    }
                }
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
                m_hwnd = nullptr;
                ::PostQuitMessage(0);
                return 0;
            }

            // === 키보드 입력 ===
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
            {
                m_input.OnKeyDown(static_cast<unsigned int>(wParam));
                return 0;
            }
            case WM_KEYUP:
            case WM_SYSKEYUP:
            {
                m_input.OnKeyUp(static_cast<unsigned int>(wParam));
                return 0;
            }

            // === 마우스 입력 ===
            case WM_MOUSEMOVE:
            {
                m_input.OnMouseMove(
                    static_cast<int>(static_cast<short>(LOWORD(lParam))),
                    static_cast<int>(static_cast<short>(HIWORD(lParam))));
                return 0;
            }
            case WM_LBUTTONDOWN: m_input.OnMouseButton(0, true);  return 0;
            case WM_LBUTTONUP:   m_input.OnMouseButton(0, false); return 0;
            case WM_RBUTTONDOWN: m_input.OnMouseButton(1, true);  return 0;
            case WM_RBUTTONUP:   m_input.OnMouseButton(1, false); return 0;
            case WM_MBUTTONDOWN: m_input.OnMouseButton(2, true);  return 0;
            case WM_MBUTTONUP:   m_input.OnMouseButton(2, false); return 0;
            case WM_MOUSEWHEEL:
            {
                // WHEEL_DELTA = 120 (Win32). 1 휠 노치 = ±120. notch 단위로 normalize.
                const short raw = static_cast<short>(HIWORD(wParam));
                m_input.OnMouseWheel(raw / WHEEL_DELTA);
                return 0;
            }

            default:
                return ::DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }
}
