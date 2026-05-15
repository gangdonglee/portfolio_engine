// WIN32_LEAN_AND_MEAN / NOMINMAX 는 vcxproj 전역 PreprocessorDefinitions 에서 정의.
#include <Windows.h>
#include <d3d12.h>
#include <DirectXMath.h>
// 주의: d3dx12.h 는 본 SDK(10.0.26100) 에 미포함 — DirectX-Headers 외부 패키지로 분리됨.
// 옵션 A 풀스크래치 원칙에 따라 외부 의존 회피, ResourceBarrier 는 D3D12_RESOURCE_BARRIER 를
// 수기로 채워 사용한다.

#include <chrono>
#include <cstdint>
#include <memory>
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
#include "render/Mesh.h"
#include "render/ObjLoader.h"
#include "render/PipelineState.h"
#include "render/RootSignature.h"
#include "render/RtvDescriptorHeap.h"
#include "render/ShaderCompiler.h"
#include "render/SrvDescriptorHeap.h"
#include "render/SwapChain.h"
#include "render/Texture.h"
#include "render/VertexBuffer.h"

#include <array>

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
        // N프레임 in-flight: 백버퍼 수와 동일 슬롯 수 (CPU 는 GPU 보다 N-1 프레임 앞서 record).
        // 슬롯당 보유 자원: CommandList(=allocator+list 쌍) + ConstantBuffer + fence value.
        // 공유 자원: Device, CommandQueue, Mesh, Texture, RootSig, PSO, RTV/DSV/SRV 힙
        //   (GPU 큐는 명령을 순차 실행 — frame N 종료 후 N+1 시작이므로 자원 공유 안전).
        constexpr std::uint32_t kFrameCount = engine::render::SwapChain::kBackBufferCount;

        engine::platform::Window           window(1280, 720, L"portfolio_engine");
        engine::render::Device             device;
        engine::render::CommandQueue       commandQueue(device);
        engine::render::RtvDescriptorHeap  rtvHeap(device, engine::render::SwapChain::kBackBufferCount);
        engine::render::SwapChain          swapChain(device, commandQueue, window, rtvHeap);

        // 프레임당 CommandList — 비이동 클래스라 unique_ptr 로 슬롯 채움.
        std::array<std::unique_ptr<engine::render::CommandList>, kFrameCount> cmdLists;
        for (std::uint32_t i = 0; i < kFrameCount; ++i)
        {
            cmdLists[i] = std::make_unique<engine::render::CommandList>(device);
        }

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

        // RootSignature: b0 CBV root descriptor (VS+PS 가시 — Phong) + t0 SRV table (PS 가시 — albedo) + s0 정적 샘플러.
        engine::render::RootSignature::Desc rsDesc{};
        rsDesc.cbvAtB0    = engine::render::RootSignature::Desc::CbvB0::All;
        rsDesc.srvT0Pixel = true;
        engine::render::RootSignature rootSig(device, rsDesc);

        // PSO: 깊이 활성 + HelloTriangle 입력 레이아웃.
        engine::render::PipelineState::Desc psoDesc{};
        psoDesc.vertexShader  = vsBlob.Get();
        psoDesc.pixelShader   = psBlob.Get();
        psoDesc.rootSignature = &rootSig;
        psoDesc.rtvFormat     = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoDesc.dsvFormat     = depthBuffer.Format();
        engine::render::PipelineState pso(device, psoDesc);

        // === Mesh 로드 — OBJ 파일에서 큐브 ===
        // 손코딩 정점/인덱스 배열 제거. assets/Cube.obj 에서 로드.
        // 정점 색상은 흰색 — 알베도 색은 텍스처에서 가져옴.
        const std::wstring assetsDir = engine::render::obj_loader::DefaultAssetsDir();
        const std::wstring cubePath  = assetsDir + L"Cube.obj";
        std::unique_ptr<engine::render::Mesh> cubeMesh =
            engine::render::obj_loader::LoadObj(
                device,
                cubePath.c_str(),
                { 1.0f, 1.0f, 1.0f });

        // === 체커보드 알베도 텍스처 (8x8 RGBA8) ===
        // 외부 이미지 로더 없이 셰이딩 확인용으로 코드 생성. 노랑/검정 8x8.
        constexpr engine::uint32 kTexW = 8;
        constexpr engine::uint32 kTexH = 8;
        std::array<engine::uint8, kTexW * kTexH * 4> checker{};
        for (engine::uint32 y = 0; y < kTexH; ++y)
        {
            for (engine::uint32 x = 0; x < kTexW; ++x)
            {
                const bool light = ((x ^ y) & 1) == 0;
                const engine::uint32 idx = (y * kTexW + x) * 4;
                checker[idx + 0] = light ? 255 : 30;   // R
                checker[idx + 1] = light ? 220 : 30;   // G
                checker[idx + 2] = light ? 60  : 30;   // B
                checker[idx + 3] = 255;
            }
        }
        // Texture 업로드는 1회성 — 메인 루프 시작 전이므로 첫 슬롯 cmdLists[0] 빌려 사용.
        // 내부에서 FlushGpu 후 list 가 Close 상태로 남으므로 메인 루프의 Reset 사이클과 호환.
        engine::render::Texture albedoTex(
            device, commandQueue, *cmdLists[0],
            checker.data(), kTexW, kTexH);

        // SRV 디스크립터 힙 (shader-visible, capacity 4 — 향후 텍스처 추가 여유).
        engine::render::SrvDescriptorHeap srvHeap(device, 4);
        albedoTex.CreateSrv(device, srvHeap);

        // 카메라: (0, 1, -5) 시작, 원점 근처. 45도 FoV.
        constexpr float kFovY     = DirectX::XM_PIDIV4;
        constexpr float kNearPlane = 0.1f;
        constexpr float kFarPlane  = 100.0f;
        engine::render::Camera camera;
        camera.SetPosition({ 0.0f, 1.0f, -5.0f });
        camera.SetTarget  ({ 0.0f, 0.0f,  0.0f });
        camera.SetUp      ({ 0.0f, 1.0f,  0.0f });
        camera.SetPerspective(
            kFovY,
            static_cast<float>(window.Width()) / static_cast<float>(window.Height()),
            kNearPlane, kFarPlane);

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

        // 프레임당 ConstantBuffer — CPU 가 frame N 의 데이터를 쓰는 동안 GPU 는 frame N-1 의 데이터를 읽음.
        // 단일 cbuffer 공유 시 race; 슬롯 N개로 분리하면 안전.
        std::array<std::unique_ptr<engine::render::ConstantBuffer>, kFrameCount> frameCBs;
        for (std::uint32_t i = 0; i < kFrameCount; ++i)
        {
            frameCBs[i] = std::make_unique<engine::render::ConstantBuffer>(
                device, static_cast<engine::uint32>(sizeof(FrameConstants)));
        }

        // 슬롯당 직전 제출의 fence value — 0 은 "아직 미사용 슬롯" 의미 (대기 skip).
        std::array<std::uint64_t, kFrameCount> frameFenceValues{};
        std::uint32_t frameIndex = 0;

        // 뷰포트 / 시저 — 윈도우 크기 기준. 매 리사이즈마다 갱신.
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

            // 윈도우 리사이즈 처리: WM_SIZE 가 dirty 플래그 설정 → 한 번에 모든 종속 자원 갱신.
            // 매 픽셀당 WM_SIZE 가 와도 ConsumeResize 가 직전 상태만 토대로 처리하므로 한 번만 Resize.
            if (window.ConsumeResize())
            {
                commandQueue.FlushGpu();   // GPU 가 백버퍼/뎁스를 더 이상 참조하지 않음을 보장
                // 모든 in-flight 슬롯이 완료된 상태 — fence 추적 값 reset.
                for (auto& v : frameFenceValues) { v = 0; }

                const auto w = static_cast<std::uint32_t>(window.Width());
                const auto h = static_cast<std::uint32_t>(window.Height());

                swapChain.Resize(device, w, h);
                depthBuffer.Resize(device, w, h);

                viewport.Width  = static_cast<float>(w);
                viewport.Height = static_cast<float>(h);
                scissor.right   = static_cast<LONG>(w);
                scissor.bottom  = static_cast<LONG>(h);

                camera.SetPerspective(
                    kFovY,
                    static_cast<float>(w) / static_cast<float>(h),
                    kNearPlane, kFarPlane);
            }

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

            // === N프레임 in-flight: 이 슬롯의 직전 제출 완료 대기 ===
            // frameFenceValues[i] == 0 은 "아직 사용 안 한 슬롯" — 첫 N프레임은 wait skip.
            // GPU 가 이미 완료한 값에 WaitForFenceValue 호출은 즉시 반환 (CommandQueue 내부 GetCompletedValue 분기).
            if (frameFenceValues[frameIndex] != 0)
            {
                commandQueue.WaitForFenceValue(frameFenceValues[frameIndex]);
            }

            // 이 슬롯의 자원 — allocator+list 쌍, 자기 영역의 cbuffer.
            engine::render::CommandList&    cmdList = *cmdLists[frameIndex];
            engine::render::ConstantBuffer& frameCB = *frameCBs[frameIndex];
            frameCB.Update(&cb, sizeof(cb));

            // === 매 프레임 명령 기록 ===
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

            // SRV 힙은 한 번에 하나만 바인딩. SetDescriptorHeaps → root table 바인딩.
            ID3D12DescriptorHeap* heaps[] = { srvHeap.Native() };
            list->SetDescriptorHeaps(1, heaps);

            list->SetGraphicsRootConstantBufferView(0, frameCB.GpuAddress());
            list->SetGraphicsRootDescriptorTable(1, albedoTex.SrvGpuHandle());

            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            cubeMesh->Bind(list);
            cubeMesh->Draw(list);

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

            // 이 슬롯의 fence value 갱신 — 다음에 같은 슬롯이 돌아왔을 때 대기 기준.
            frameFenceValues[frameIndex] = commandQueue.Signal();

            // 다음 슬롯으로 회전. SwapChain 의 백버퍼 인덱스 회전과 같은 cadence.
            frameIndex = (frameIndex + 1) % kFrameCount;
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
