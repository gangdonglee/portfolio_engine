#pragma once

#include <string>
#include <wrl/client.h>

// ID3DBlob 은 d3dcommon.h 의 `typedef ID3D10Blob ID3DBlob` — typedef 라 단순 forward decl 불가.
// 가벼운 d3dcommon.h 만 include (d3d12.h / dxgi 전체는 .cpp 한정).
#include <d3dcommon.h>

namespace engine::render
{
    // HLSL 런타임 컴파일 헬퍼 (D3DCompileFromFile 기반, SM 5.0).
    //
    // 본 클래스는 정적 메서드만 제공 — 상태 없음. 인스턴스화 금지.
    //
    // 셰이더 경로 정책:
    //   - DefaultShaderDir() 가 실행 파일(Client.exe) 의 디렉토리 + "shaders\\" 를 반환.
    //   - Client.vcxproj 의 PostBuildEvent 가 shaders/*.hlsl 을 빌드 출력 폴더로 복사.
    //   - 결과적으로 어디서 실행해도 exe 옆 "shaders/" 가 셰이더 루트가 된다.
    //
    // 향후 dxc 마이그레이션:
    //   - SM 6.0+ 필요 시 IDxcCompiler3 기반 별도 클래스 또는 본 클래스에 백엔드 분기 추가.
    class ShaderCompiler final
    {
    public:
        enum class Stage
        {
            Vertex,
            Pixel,
            // Compute, Geometry, Hull, Domain — 등장 시 추가
        };

        // 실행 파일과 같은 폴더의 "shaders\\" 절대 경로 반환.
        // 호출 시점에 GetModuleFileNameW 로 결정 (캐시 없음 — 호출 빈도가 낮으므로 비용 무시).
        static std::wstring DefaultShaderDir();

        // file: 셰이더 절대 경로 (UTF-16). entryPoint: ASCII 함수명 (예: "VSMain").
        // stage 에 따라 컴파일 target (vs_5_0 / ps_5_0) 자동 결정.
        // 실패 시 std::runtime_error (D3DCompileFromFile 의 error blob 메시지 포함).
        static Microsoft::WRL::ComPtr<ID3DBlob> CompileFromFile(
            const wchar_t* file,
            const char*    entryPoint,
            Stage          stage);

    private:
        ShaderCompiler()  = delete;
        ~ShaderCompiler() = delete;
    };
}
