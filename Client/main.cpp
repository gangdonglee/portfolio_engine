// WIN32_LEAN_AND_MEAN / NOMINMAX 는 vcxproj 전역 PreprocessorDefinitions 에서 정의.
#include <Windows.h>

#include "Application.h"
#include "core/Logger.h"

#include <exception>

// 부트만 책임. 모든 라이프타임 / 메인 루프 / 자원 소유는 client::Application 안.
int APIENTRY wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int)
{
    engine::core::LogInfo(L"[portfolio_engine] boot running\n");

    try
    {
        client::Application app(1280, 720, L"portfolio_engine");
        app.Run();
    }
    catch (const std::exception& e)
    {
        engine::core::LogInfoA("[portfolio_engine] fatal: ");
        engine::core::LogInfoA(e.what());
        engine::core::LogInfoA("\n");
        return 1;
    }

    engine::core::LogInfo(L"[portfolio_engine] exit clean\n");
    return 0;
}
