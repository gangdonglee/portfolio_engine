// WIN32_LEAN_AND_MEAN / NOMINMAX 는 vcxproj 전역 PreprocessorDefinitions 에서 정의.
#include <Windows.h>
#include <d3d12.h>
#include <DirectXMath.h>
// 주의: d3dx12.h 는 본 SDK(10.0.26100) 에 미포함 — DirectX-Headers 외부 패키지로 분리됨.
// 옵션 A 풀스크래치 원칙에 따라 외부 의존 회피, ResourceBarrier 는 D3D12_RESOURCE_BARRIER 를
// 수기로 채워 사용한다.

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "platform/Window.h"
#include "render/Camera.h"
#include "render/CommandList.h"
#include "render/CommandQueue.h"
#include "render/ConstantBuffer.h"
#include "render/DepthStencilBuffer.h"
#include "render/Device.h"
#include "render/IndexBuffer.h"
#include "render/PipelineState.h"
#include "render/RootSignature.h"
#include "render/RtvDescriptorHeap.h"
#include "render/ShaderCompiler.h"
#include "render/SwapChain.h"
#include "render/VertexBuffer.h"

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
        engine::platform::Window           window(1280, 720, L"portfolio_engine");
        engine::render::Device             device;
        engine::render::CommandQueue       commandQueue(device);
        engine::render::RtvDescriptorHeap  rtvHeap(device, engine::render::SwapChain::kBackBufferCount);
        engine::render::SwapChain          swapChain(device, commandQueue, window, rtvHeap);
        engine::render::CommandList        cmdList(device);
        engine::render::DepthStencilBuffer depthBuffer(
            device,
            static_cast<std::uint32_t>(window.Width()),
            static_cast<std::uint32_t>(window.Height()),
            DXGI_FORMAT_D32_FLOAT);

        // 셰이더 컴파일
        const std::wstring shaderDir  = engine::render::ShaderCompiler::DefaultShaderDir();
        const std::wstring shaderPath = shaderDir + L"HelloTriangle.hlsl";

        const auto vsBlob = engine::render::ShaderCompiler::CompileFromFile(
            shaderPath.c_str(), "VSMain", engine::render::ShaderCompiler::Stage::Vertex);
        const auto psBlob = engine::render::ShaderCompiler::CompileFromFile(
            shaderPath.c_str(), "PSMain", engine::render::ShaderCompiler::Stage::Pixel);

        // RootSignature: b0 CBV root descriptor 1개 (Vertex 단계 가시).
        engine::render::RootSignature::Desc rsDesc{};
        rsDesc.cbvAtB0Vertex = true;
        engine::render::RootSignature rootSig(device, rsDesc);

        // PSO: 깊이 활성 + HelloTriangle 입력 레이아웃.
        engine::render::PipelineState::Desc psoDesc{};
        psoDesc.vertexShader  = vsBlob.Get();
        psoDesc.pixelShader   = psBlob.Get();
        psoDesc.rootSignature = &rootSig;
        psoDesc.rtvFormat     = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoDesc.dsvFormat     = depthBuffer.Format();
        engine::render::PipelineState pso(device, psoDesc);

        // === 회전 큐브 메시 (8 정점, 36 인덱스, 12 삼각형) ===
        struct HelloVertex
        {
            float position[3];
            float color[3];
        };
        constexpr HelloVertex kCubeVertices[8] = {
            { { -1.0f, -1.0f, -1.0f }, { 0.0f, 0.0f, 0.0f } },  // 0: 좌하전
            { { +1.0f, -1.0f, -1.0f }, { 1.0f, 0.0f, 0.0f } },  // 1: 우하전 — 빨강
            { { +1.0f, +1.0f, -1.0f }, { 1.0f, 1.0f, 0.0f } },  // 2: 우상전 — 노랑
            { { -1.0f, +1.0f, -1.0f }, { 0.0f, 1.0f, 0.0f } },  // 3: 좌상전 — 초록
            { { -1.0f, -1.0f, +1.0f }, { 0.0f, 0.0f, 1.0f } },  // 4: 좌하후 — 파랑
            { { +1.0f, -1.0f, +1.0f }, { 1.0f, 0.0f, 1.0f } },  // 5: 우하후 — 자홍
            { { +1.0f, +1.0f, +1.0f }, { 1.0f, 1.0f, 1.0f } },  // 6: 우상후 — 흰색
            { { -1.0f, +1.0f, +1.0f }, { 0.0f, 1.0f, 1.0f } },  // 7: 좌상후 — 청록
        };
        // LH 좌표계 + CullMode=BACK + FrontCCW=FALSE → CW(시계방향) 가 정면.
        constexpr std::uint16_t kCubeIndices[36] = {
            // Front  (z=-1, 카메라에서 본 CW)
            0, 3, 2,  0, 2, 1,
            // Back   (z=+1)
            5, 6, 7,  5, 7, 4,
            // Left   (x=-1)
            4, 7, 3,  4, 3, 0,
            // Right  (x=+1)
            1, 2, 6,  1, 6, 5,
            // Top    (y=+1)
            3, 7, 6,  3, 6, 2,
            // Bottom (y=-1)
            4, 0, 1,  4, 1, 5,
        };

        engine::render::VertexBuffer cubeVB(
            device, kCubeVertices,
            static_cast<engine::uint32>(sizeof(kCubeVertices)),
            static_cast<engine::uint32>(sizeof(HelloVertex)));
        engine::render::IndexBuffer cubeIB(
            device, kCubeIndices,
            static_cast<engine::uint32>(sizeof(kCubeIndices)),
            DXGI_FORMAT_R16_UINT);

        // 카메라: (3, 2, -5) → 원점 바라보기, 45도 FoV.
        engine::render::Camera camera;
        camera.SetPosition({ 3.0f, 2.0f, -5.0f });
        camera.SetTarget  ({ 0.0f, 0.0f,  0.0f });
        camera.SetUp      ({ 0.0f, 1.0f,  0.0f });
        camera.SetPerspective(
            DirectX::XM_PIDIV4,
            static_cast<float>(window.Width()) / static_cast<float>(window.Height()),
            0.1f, 100.0f);

        // 상수 버퍼: row-major float4x4 MVP (64바이트, 256 정렬됨).
        struct FrameConstants
        {
            DirectX::XMFLOAT4X4 mvp;
        };
        engine::render::ConstantBuffer frameCB(
            device, static_cast<engine::uint32>(sizeof(FrameConstants)));

        // 뷰포트 / 시저 — 윈도우 크기 기준. 리사이즈 처리는 후속 단계.
        D3D12_VIEWPORT viewport{};
        viewport.TopLeftX = 0.0f;
        viewport.TopLeftY = 0.0f;
        viewport.Width    = static_cast<float>(window.Width());
        viewport.Height   = static_cast<float>(window.Height());
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;

        D3D12_RECT scissor{};
        scissor.left   = 0;
        scissor.top    = 0;
        scissor.right  = static_cast<LONG>(window.Width());
        scissor.bottom = static_cast<LONG>(window.Height());

        constexpr float kClearColor[4] = { 0.05f, 0.07f, 0.10f, 1.0f };

        // 시간 측정 — 회전 각도 = elapsed seconds.
        const auto startTime = std::chrono::steady_clock::now();

        while (window.IsOpen())
        {
            window.PumpMessages();
            if (!window.IsOpen()) { break; }

            // 경과 시간 (초).
            const auto now = std::chrono::steady_clock::now();
            const float t  = std::chrono::duration<float>(now - startTime).count();

            // World = RotationY(t) * RotationX(t * 0.7) — 두 축 회전으로 큐브의 모든 면 가시.
            using namespace DirectX;
            const XMMATRIX world = XMMatrixRotationY(t) * XMMatrixRotationX(t * 0.7f);
            const XMMATRIX mvp   = world * camera.ViewProjection();

            FrameConstants cb;
            XMStoreFloat4x4(&cb.mvp, mvp);
            frameCB.Update(&cb, sizeof(cb));

            // === 매 프레임 명령 기록 ===
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

            // RTV + DSV 바인딩 + 클리어
            const D3D12_CPU_DESCRIPTOR_HANDLE rtv = swapChain.CurrentRtv();
            const D3D12_CPU_DESCRIPTOR_HANDLE dsv = depthBuffer.DsvHandle();
            list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
            list->ClearRenderTargetView(rtv, kClearColor, 0, nullptr);
            list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

            // 파이프라인 + 큐브 그리기
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            list->SetGraphicsRootSignature(rootSig.Native());
            list->SetPipelineState(pso.Native());
            list->SetGraphicsRootConstantBufferView(0, frameCB.GpuAddress());
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            cubeVB.Bind(list);
            cubeIB.Bind(list);
            list->DrawIndexedInstanced(cubeIB.IndexCount(), 1, 0, 0, 0);

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

        // 마지막 GPU 작업 완료 대기 (소멸 순서 안전).
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
