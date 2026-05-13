// WIN32_LEAN_AND_MEAN / NOMINMAX 는 vcxproj 전역 PreprocessorDefinitions 에서 정의.
#include <Windows.h>

#include <stdexcept>

#include "platform/Window.h"
#include "render/Device.h"
#include "render/CommandQueue.h"

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                      _In_opt_ HINSTANCE hPrevInstance,
                      _In_ LPWSTR lpCmdLine,
                      _In_ int nCmdShow)
{
    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

    ::OutputDebugStringW(L"[portfolio_engine] boot running\n");

    try
    {
        engine::platform::Window   window(1280, 720, L"portfolio_engine");
        engine::render::Device     device;
        engine::render::CommandQueue commandQueue(device);

        while (window.IsOpen())
        {
            window.PumpMessages();
            // TODO(phase1d): SwapChain Clear → Present
        }
    }
    catch (const std::exception& e)
    {
        ::OutputDebugStringA("[portfolio_engine] fatal: ");
        ::OutputDebugStringA(e.what());
        ::OutputDebugStringA("\n");
        return 1;
    }

    ::OutputDebugStringW(L"[portfolio_engine] exit clean\n");
    return 0;
}
