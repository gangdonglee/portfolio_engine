#include "render/ShaderCompiler.h"

#include "core/Logger.h"

#include <Windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>

#include <cstdio>
#include <stdexcept>

namespace engine::render
{
    namespace
    {
        const char* TargetForStage(ShaderCompiler::Stage stage) noexcept
        {
            switch (stage)
            {
                case ShaderCompiler::Stage::Vertex: return "vs_5_0";
                case ShaderCompiler::Stage::Pixel:  return "ps_5_0";
            }
            // 도달 불가 — enum 추가 시 컴파일러 경고로 누락 잡힘.
            return "";
        }
    }

    std::wstring ShaderCompiler::DefaultShaderDir()
    {
        wchar_t buf[MAX_PATH];
        const DWORD len = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
        if (len == 0 || len == MAX_PATH)
        {
            throw std::runtime_error("GetModuleFileNameW failed or path too long");
        }

        std::wstring path(buf, len);
        const auto sep = path.find_last_of(L"\\/");
        if (sep == std::wstring::npos)
        {
            throw std::runtime_error("Executable path has no directory separator");
        }
        return path.substr(0, sep + 1) + L"shaders\\";
    }

    Microsoft::WRL::ComPtr<ID3DBlob> ShaderCompiler::CompileFromFile(
        const wchar_t* file,
        const char*    entryPoint,
        Stage          stage)
    {
        UINT compileFlags = 0;
#if defined(_DEBUG)
        compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        compileFlags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
        compileFlags |= D3DCOMPILE_ENABLE_STRICTNESS;
        compileFlags |= D3DCOMPILE_WARNINGS_ARE_ERRORS;

        Microsoft::WRL::ComPtr<ID3DBlob> code;
        Microsoft::WRL::ComPtr<ID3DBlob> errors;

        const HRESULT hr = ::D3DCompileFromFile(
            file,
            nullptr,                              // 매크로 정의 — 현 단계 없음
            D3D_COMPILE_STANDARD_FILE_INCLUDE,    // #include 처리
            entryPoint,
            TargetForStage(stage),
            compileFlags,
            0,                                    // FX 플래그 (셰이더에는 의미 없음)
            code.GetAddressOf(),
            errors.GetAddressOf());

        if (FAILED(hr))
        {
            char buf[512];
            if (errors && errors->GetBufferSize() > 0)
            {
                const char* errMsg = static_cast<const char*>(errors->GetBufferPointer());
                std::snprintf(buf, sizeof(buf),
                              "D3DCompileFromFile failed (entry=%s, hr=0x%08lX): %.*s",
                              entryPoint,
                              static_cast<unsigned long>(hr),
                              static_cast<int>(errors->GetBufferSize()),
                              errMsg);
            }
            else
            {
                std::snprintf(buf, sizeof(buf),
                              "D3DCompileFromFile failed (entry=%s, hr=0x%08lX, no error blob)",
                              entryPoint,
                              static_cast<unsigned long>(hr));
            }
            throw std::runtime_error(buf);
        }

        // 경고가 있어도 컴파일 성공인 경우가 있다 — 경고 블롭이 비어있지 않으면 로그.
        if (errors && errors->GetBufferSize() > 0)
        {
            engine::core::LogInfoA("[render] Shader compile warnings:\n");
            engine::core::LogInfoA(static_cast<const char*>(errors->GetBufferPointer()));
            engine::core::LogInfoA("\n");
        }

        wchar_t line[256];
        std::swprintf(line, std::size(line),
                      L"[render] Shader compiled OK (entry=%hs, target=%hs, size=%zu bytes)\n",
                      entryPoint,
                      TargetForStage(stage),
                      static_cast<size_t>(code->GetBufferSize()));
        engine::core::LogInfo(line);

        return code;
    }
}
