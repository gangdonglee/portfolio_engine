#include "core/Logger.h"

#include <Windows.h>

namespace engine::core
{
    void LogInfo(const wchar_t* message)
    {
        if (message == nullptr) return;
        ::OutputDebugStringW(message);
    }

    void LogInfoA(const char* message)
    {
        if (message == nullptr) return;
        ::OutputDebugStringA(message);
    }
}
