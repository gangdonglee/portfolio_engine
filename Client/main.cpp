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

#include "platform/Input.h"
#include "platform/Window.h"
#include "render/Camera.h"
#include "render/CommandList.h"
#include "render/FreeCamera.h"
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

        // RootSignature: b0 CBV root descriptor 1개 (VS + PS 모두 가시 — Phong 조명용).
        engine::render::RootSignature::Desc rsDesc{};
        rsDesc.cbvAtB0 = engine::render::RootSignature::Desc::CbvB0::All;
        engine::render::RootSignature rootSig(device, rsDesc);

        // PSO: 깊이 활성 + HelloTriangle 입력 레이아웃.
        engine::render::PipelineState::Desc psoDesc{};
        psoDesc.vertexShader  = vsBlob.Get();
        psoDesc.pixelShader   = psBlob.Get();
        psoDesc.rootSignature = &rootSig;
        psoDesc.rtvFormat     = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoDesc.dsvFormat     = depthBuffer.Format();
        engine::render::PipelineState pso(device, psoDesc);

        // === 회전 큐브 메시 (24 정점, 면별 normal, 6 면 색상) ===
        // 정점 = 4 * 6 = 24. 각 정점이 한 면에만 속하므로 normal 이 면별로 명확.
        // CullBack + FrontCCW=FALSE → CW(outward normal 방향에서 본 시계방향)가 정면.
        struct HelloVertex
        {
            float position[3];
            float normal[3];
            float color[3];
        };
        constexpr HelloVertex kCubeVertices[24] = {
            // Front  (z=-1, normal=-Z) — 빨강
            { { -1, -1, -1 }, {  0,  0, -1 }, { 1, 0, 0 } },  // 0 좌하
            { { +1, -1, -1 }, {  0,  0, -1 }, { 1, 0, 0 } },  // 1 우하
            { { +1, +1, -1 }, {  0,  0, -1 }, { 1, 0, 0 } },  // 2 우상
            { { -1, +1, -1 }, {  0,  0, -1 }, { 1, 0, 0 } },  // 3 좌상
            // Back   (z=+1, normal=+Z) — 초록
            { { +1, -1, +1 }, {  0,  0, +1 }, { 0, 1, 0 } },  // 4 좌하(시점 좌우 반전)
            { { -1, -1, +1 }, {  0,  0, +1 }, { 0, 1, 0 } },  // 5 우하
            { { -1, +1, +1 }, {  0,  0, +1 }, { 0, 1, 0 } },  // 6 우상
            { { +1, +1, +1 }, {  0,  0, +1 }, { 0, 1, 0 } },  // 7 좌상
            // Left   (x=-1, normal=-X) — 파랑
            { { -1, -1, +1 }, { -1,  0,  0 }, { 0, 0, 1 } },  // 8 좌하
            { { -1, -1, -1 }, { -1,  0,  0 }, { 0, 0, 1 } },  // 9 우하
            { { -1, +1, -1 }, { -1,  0,  0 }, { 0, 0, 1 } },  // 10 우상
            { { -1, +1, +1 }, { -1,  0,  0 }, { 0, 0, 1 } },  // 11 좌상
            // Right  (x=+1, normal=+X) — 노랑
            { { +1, -1, -1 }, { +1,  0,  0 }, { 1, 1, 0 } },  // 12 좌하
            { { +1, -1, +1 }, { +1,  0,  0 }, { 1, 1, 0 } },  // 13 우하
            { { +1, +1, +1 }, { +1,  0,  0 }, { 1, 1, 0 } },  // 14 우상
            { { +1, +1, -1 }, { +1,  0,  0 }, { 1, 1, 0 } },  // 15 좌상
            // Top    (y=+1, normal=+Y) — 자홍
            { { -1, +1, -1 }, {  0, +1,  0 }, { 1, 0, 1 } },  // 16 좌하
            { { +1, +1, -1 }, {  0, +1,  0 }, { 1, 0, 1 } },  // 17 우하
            { { +1, +1, +1 }, {  0, +1,  0 }, { 1, 0, 1 } },  // 18 우상
            { { -1, +1, +1 }, {  0, +1,  0 }, { 1, 0, 1 } },  // 19 좌상
            // Bottom (y=-1, normal=-Y) — 청록
            { { -1, -1, +1 }, {  0, -1,  0 }, { 0, 1, 1 } },  // 20 좌하
            { { +1, -1, +1 }, {  0, -1,  0 }, { 0, 1, 1 } },  // 21 우하
            { { +1, -1, -1 }, {  0, -1,  0 }, { 0, 1, 1 } },  // 22 우상
            { { -1, -1, -1 }, {  0, -1,  0 }, { 0, 1, 1 } },  // 23 좌상
        };
        // 각 면 4 정점(좌하·우하·우상·좌상 순)을 CW 삼각형 2개로:
        //   (좌하 → 우하 → 우상) + (좌하 → 우상 → 좌상)
        constexpr std::uint16_t kCubeIndices[36] = {
             0,  1,  2,   0,  2,  3,  // Front
             4,  5,  6,   4,  6,  7,  // Back
             8,  9, 10,   8, 10, 11,  // Left
            12, 13, 14,  12, 14, 15,  // Right
            16, 17, 18,  16, 18, 19,  // Top
            20, 21, 22,  20, 22, 23,  // Bottom
        };

        engine::render::VertexBuffer cubeVB(
            device, kCubeVertices,
            static_cast<engine::uint32>(sizeof(kCubeVertices)),
            static_cast<engine::uint32>(sizeof(HelloVertex)));
        engine::render::IndexBuffer cubeIB(
            device, kCubeIndices,
            static_cast<engine::uint32>(sizeof(kCubeIndices)),
            DXGI_FORMAT_R16_UINT);

        // 카메라: (0, 1, -5) 시작, 원점 근처. 45도 FoV.
        engine::render::Camera camera;
        camera.SetPosition({ 0.0f, 1.0f, -5.0f });
        camera.SetTarget  ({ 0.0f, 0.0f,  0.0f });
        camera.SetUp      ({ 0.0f, 1.0f,  0.0f });
        camera.SetPerspective(
            DirectX::XM_PIDIV4,
            static_cast<float>(window.Width()) / static_cast<float>(window.Height()),
            0.1f, 100.0f);

        // 자유 카메라 컨트롤러 (WASD + QE + 우클릭 hold 마우스 회전, Shift 부스트).
        engine::render::FreeCamera freeCamera(camera);

        // 상수 버퍼: MVP + World + 카메라 위치 + 라이트.
        // 각 float3 뒤 4바이트 패딩 = HLSL 의 float3 가 16바이트 정렬되도록 보장.
        struct FrameConstants
        {
            DirectX::XMFLOAT4X4 mvp;
            DirectX::XMFLOAT4X4 world;
            DirectX::XMFLOAT3   cameraPosWS;  float _pad0;
            DirectX::XMFLOAT3   lightDirWS;   float _pad1;
            DirectX::XMFLOAT3   lightColor;   float _pad2;
            DirectX::XMFLOAT3   ambient;      float _pad3;
        };
        static_assert(sizeof(FrameConstants) == 192, "FrameConstants 정렬 깨짐");
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

        // 시간 측정.
        const auto startTime = std::chrono::steady_clock::now();
        auto       prevFrame = startTime;

        while (window.IsOpen())
        {
            window.PumpMessages();
            if (!window.IsOpen()) { break; }
            // PumpMessages 가 마우스/키 이벤트 누적 후 — 이번 프레임 델타 확정.
            window.GetInput().BeginFrame();

            // 프레임 dt + 누적 시간.
            const auto now = std::chrono::steady_clock::now();
            const float dt = std::chrono::duration<float>(now - prevFrame).count();
            const float t  = std::chrono::duration<float>(now - startTime).count();
            prevFrame = now;

            // 입력으로 카메라 갱신.
            freeCamera.Update(window.GetInput(), dt);

            // World = 회전 큐브 자체 회전 (시각 흐름 유지).
            using namespace DirectX;
            const XMMATRIX world = XMMatrixRotationY(t) * XMMatrixRotationX(t * 0.7f);
            const XMMATRIX mvp   = world * camera.ViewProjection();

            FrameConstants cb{};
            XMStoreFloat4x4(&cb.mvp,   mvp);
            XMStoreFloat4x4(&cb.world, world);
            cb.cameraPosWS = camera.Position();
            // 라이트는 사선 위에서 내려오는 방향광. 정규화된 단위 벡터.
            const XMVECTOR lightDir = XMVector3Normalize(XMVectorSet(-0.5f, -1.0f, 0.4f, 0.0f));
            XMStoreFloat3(&cb.lightDirWS, lightDir);
            cb.lightColor = { 1.0f, 0.97f, 0.92f };  // 약간 따뜻한 흰색
            cb.ambient    = { 0.15f, 0.15f, 0.18f }; // 약간 푸른 앰비언트
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
