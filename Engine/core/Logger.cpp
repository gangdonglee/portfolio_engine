#include "core/Logger.h"

#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace engine::core
{
    namespace
    {
        // exe 디렉토리에 error.txt 를 생성/갱신. 매 호출마다 fflush — 응답없음/크래시 직전까지
        // 디스크에 보존되어야 외부 진단(파일 읽기)으로 멈춤점을 짚을 수 있다.
        // 첫 호출 시 "wb" 로 truncate + UTF-8 BOM 기록, 이후 같은 FILE* 에 append.
        std::FILE* OpenLogFileOnce()
        {
            static std::once_flag once;
            static std::FILE*     file = nullptr;
            std::call_once(once, []()
            {
                wchar_t exePath[MAX_PATH];
                const DWORD len = ::GetModuleFileNameW(nullptr, exePath, MAX_PATH);
                if (len == 0 || len == MAX_PATH) { return; }

                std::wstring path(exePath, len);
                const auto sep = path.find_last_of(L"\\/");
                if (sep == std::wstring::npos) { return; }
                const std::wstring logPath = path.substr(0, sep + 1) + L"error.txt";

                std::FILE* f = nullptr;
                if (::_wfopen_s(&f, logPath.c_str(), L"wb") == 0 && f != nullptr)
                {
                    // UTF-8 BOM — Windows 한국어 환경 에디터가 CP949 로 오인식하지 않도록.
                    const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
                    std::fwrite(bom, 1, sizeof(bom), f);
                    file = f;
                }
            });
            return file;
        }

        std::mutex& FileMutex()
        {
            static std::mutex m;
            return m;
        }

        void WriteUtf8ToFile(const char* utf8, int byteLen)
        {
            if (utf8 == nullptr || byteLen <= 0) { return; }
            std::FILE* f = OpenLogFileOnce();
            if (f == nullptr) { return; }
            std::lock_guard<std::mutex> lock(FileMutex());
            std::fwrite(utf8, 1, static_cast<size_t>(byteLen), f);
            std::fflush(f);
        }
    }

    void LogInfo(const wchar_t* message)
    {
        if (message == nullptr) { return; }
        ::OutputDebugStringW(message);

        const int needed = ::WideCharToMultiByte(CP_UTF8, 0, message, -1, nullptr, 0, nullptr, nullptr);
        if (needed <= 1) { return; }   // 1 == NUL only
        std::string utf8;
        utf8.resize(static_cast<size_t>(needed - 1));
        ::WideCharToMultiByte(CP_UTF8, 0, message, -1, utf8.data(), needed - 1, nullptr, nullptr);
        WriteUtf8ToFile(utf8.data(), needed - 1);
    }

    void LogInfoA(const char* message)
    {
        if (message == nullptr) { return; }
        ::OutputDebugStringA(message);
        WriteUtf8ToFile(message, static_cast<int>(std::strlen(message)));
    }
}
