// WIN32_LEAN_AND_MEAN / NOMINMAX 는 vcxproj 전역 PreprocessorDefinitions 에서 정의.
#include <Windows.h>
#include <d3d12.h>
// 주의: d3dx12.h 는 본 SDK(10.0.26100) 에 미포함 — DirectX-Headers 외부 패키지로 분리됨.
// 옵션 A 풀스크래치 원칙에 따라 외부 의존 회피, ResourceBarrier 는 D3D12_RESOURCE_BARRIER 를
// 수기로 채워 사용한다.

#include <cstdint>
#include <stdexcept>
#include <string>

#include "platform/Window.h"
#include "render/CommandList.h"
#include "render/CommandQueue.h"
#include "render/Device.h"
#include "render/PipelineState.h"
#include "render/RootSignature.h"
#include "render/RtvDescriptorHeap.h"
#include "render/ShaderCompiler.h"
#include "render/SwapChain.h"

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
        engine::platform::Window          window(1280, 720, L"portfolio_engine");
        engine::render::Device            device;
        engine::render::CommandQueue      commandQueue(device);
        engine::render::RtvDescriptorHeap rtvHeap(device, engine::render::SwapChain::kBackBufferCount);
        engine::render::SwapChain         swapChain(device, commandQueue, window, rtvHeap);
        engine::render::CommandList       cmdList(device);

        // Phase 1E-1: HelloTriangle 셰이더 컴파일 (PSO 도입 전 컴파일 단독 검증).
        const std::wstring shaderDir = engine::render::ShaderCompiler::DefaultShaderDir();
        const std::wstring shaderPath = shaderDir + L"HelloTriangle.hlsl";

        const auto vsBlob = engine::render::ShaderCompiler::CompileFromFile(
            shaderPath.c_str(),
            "VSMain",
            engine::render::ShaderCompiler::Stage::Vertex);
        const auto psBlob = engine::render::ShaderCompiler::CompileFromFile(
            shaderPath.c_str(),
            "PSMain",
            engine::render::ShaderCompiler::Stage::Pixel);

        // Phase 1E-2: RootSignature(비어있음) + Graphics PSO (HelloTriangle 레이아웃).
        engine::render::RootSignature rootSig(device);

        engine::render::PipelineState::Desc psoDesc{};
        psoDesc.vertexShader  = vsBlob.Get();
        psoDesc.pixelShader   = psBlob.Get();
        psoDesc.rootSignature = &rootSig;
        psoDesc.rtvFormat     = DXGI_FORMAT_R8G8B8A8_UNORM;  // SwapChain 백버퍼 포맷
        engine::render::PipelineState pso(device, psoDesc);
        (void)pso;
        // TODO(phase1e-3): VertexBuffer + DrawInstanced 로 첫 삼각형 가시.

        // 첫 클리어 색상: 어두운 슬레이트 (RGB 0.05, 0.07, 0.10).
        constexpr float kClearColor[4] = { 0.05f, 0.07f, 0.10f, 1.0f };

        while (window.IsOpen())
        {
            window.PumpMessages();
            if (!window.IsOpen()) { break; }  // 펌프 도중 WM_QUIT 수신 시 즉시 종료

            // 단순 1프레임 in-flight: 매 프레임 직전 GPU 완료 대기 후 record/submit.
            // 향후 N 프레임 in-flight 로 개선 시 cmdList 를 N 개 풀로 운영.
            commandQueue.FlushGpu();
            cmdList.Reset();

            ID3D12GraphicsCommandList* list = cmdList.Native();

            ID3D12Resource* const backBuffer = swapChain.CurrentBackBuffer();

            // PRESENT → RENDER_TARGET
            D3D12_RESOURCE_BARRIER toRenderTarget{};
            toRenderTarget.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toRenderTarget.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            toRenderTarget.Transition.pResource   = backBuffer;
            toRenderTarget.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            toRenderTarget.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            toRenderTarget.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
            list->ResourceBarrier(1, &toRenderTarget);

            // 현재 백버퍼 RTV 를 클리어
            list->ClearRenderTargetView(swapChain.CurrentRtv(), kClearColor, 0, nullptr);

            // RENDER_TARGET → PRESENT
            D3D12_RESOURCE_BARRIER toPresent{};
            toPresent.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toPresent.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            toPresent.Transition.pResource   = backBuffer;
            toPresent.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            toPresent.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
            list->ResourceBarrier(1, &toPresent);

            cmdList.Close();
            commandQueue.Execute(cmdList);
            swapChain.Present();
        }

        // 루프 종료 직후 마지막 프레임의 GPU 작업 완료 대기.
        // 소멸 순서상 cmdList → swapChain → rtvHeap → commandQueue → device → window 인데,
        // cmdList(Allocator/List) 가 GPU 가 아직 사용 중인 상태에서 Release 되면 UB 위험 +
        // SwapChain 파괴가 큐된 프레임을 기다리느라 hang 할 수 있다.
        // 명시적 FlushGpu 로 모든 소멸자보다 먼저 GPU 동기화 보장.
        commandQueue.FlushGpu();
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
