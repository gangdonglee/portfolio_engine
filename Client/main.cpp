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
#include "render/AnimClip.h"
#include "render/Animator.h"
#include "render/Camera.h"
#include "render/CommandList.h"
#include "render/FbxLoader.h"
#include "render/FreeCamera.h"
#include "render/CommandQueue.h"
#include "render/ConstantBuffer.h"
#include "render/DepthStencilBuffer.h"
#include "render/Device.h"
#include "render/ImageLoader.h"
#include "render/IndexBuffer.h"
#include "render/Mesh.h"
#include "render/ObjLoader.h"
#include "render/Skeleton.h"
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

        // RootSignature: b0 CBV (All, FrameConstants) + b1 CBV (VS, BonePalette) + t0 SRV table (PS) + s0 정적 샘플러.
        // 파라미터 순서: [0]=b0, [1]=b1, [2]=t0 table.
        engine::render::RootSignature::Desc rsDesc{};
        rsDesc.cbvAtB0     = engine::render::RootSignature::Desc::CbvB0::All;
        rsDesc.cbvB1Vertex = true;
        rsDesc.srvT0Pixel  = true;
        engine::render::RootSignature rootSig(device, rsDesc);

        // PSO: 깊이 활성 + HelloTriangle 입력 레이아웃.
        engine::render::PipelineState::Desc psoDesc{};
        psoDesc.vertexShader  = vsBlob.Get();
        psoDesc.pixelShader   = psBlob.Get();
        psoDesc.rootSignature = &rootSig;
        psoDesc.rtvFormat     = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoDesc.dsvFormat     = depthBuffer.Format();
        engine::render::PipelineState pso(device, psoDesc);

        // === SRV 힙 + 폴백 알베도 + Dragon FBX 로드 ===
        // SRV 힙은 FbxLoader 가 머티리얼별 텍스처를 등록할 때 추가 슬롯 사용.
        // capacity 32 — Dragon 머티리얼 수 + 폴백 여유.
        engine::render::SrvDescriptorHeap srvHeap(device, 32);

        // 폴백 알베도 — FBX 머티리얼이 텍스처 없을 때 사용. Resources/Texture/Leather.jpg.
        const std::wstring texDir  = engine::render::fbx_loader::DefaultFbxDir() + L"..\\Texture\\";
        const std::wstring texPath = texDir + L"Leather.jpg";
        const engine::render::ImageData albedoImg =
            engine::render::image_loader::LoadImage(texPath.c_str());
        engine::render::Texture fallbackAlbedo(
            device, commandQueue, *cmdLists[0],
            albedoImg.pixels.data(), albedoImg.width, albedoImg.height);
        fallbackAlbedo.CreateSrv(device, srvHeap);   // 슬롯 0

        // Dragon FBX 로드 — 머티리얼별 sub-mesh + 텍스처 + 스키레톤 + 애니메이션 클립.
        const std::wstring fbxDir  = engine::render::fbx_loader::DefaultFbxDir();
        const std::wstring fbxPath = fbxDir + L"Dragon.fbx";
        engine::render::fbx_loader::LoadedFbxModel loaded =
            engine::render::fbx_loader::LoadFbx(
                device, commandQueue, *cmdLists[0], srvHeap,
                fbxPath.c_str(),
                { 0.85f, 0.85f, 0.92f });
        std::unique_ptr<engine::render::Mesh> mainMesh = std::move(loaded.mesh);

        // Animator — 스키레톤 + 첫 애니메이션 클립이 있으면 생성.
        std::unique_ptr<engine::render::Animator> animator;
        if (loaded.skeleton && !loaded.clips.empty())
        {
            animator = std::make_unique<engine::render::Animator>(
                *loaded.skeleton, *loaded.clips[0]);
        }

        // 카메라: Dragon.fbx 가 unit cm 기준(약 ±100 박스) — Cube(±1) 보다 멀리 + far plane 확대.
        constexpr float kFovY      = DirectX::XM_PIDIV4;
        constexpr float kNearPlane = 1.0f;
        constexpr float kFarPlane  = 5000.0f;
        engine::render::Camera camera;
        camera.SetPosition({ 0.0f, 100.0f, -300.0f });
        camera.SetTarget  ({ 0.0f, 50.0f,  0.0f });
        camera.SetUp      ({ 0.0f, 1.0f,   0.0f });
        camera.SetPerspective(
            kFovY,
            static_cast<float>(window.Width()) / static_cast<float>(window.Height()),
            kNearPlane, kFarPlane);

        // 자유 카메라 컨트롤러 (WASD + QE + 우클릭 hold 마우스 회전, Shift 부스트).
        // Dragon.fbx 단위(cm) 기준이라 기본 5m/s 속도는 너무 느림 → 100 m/s.
        engine::render::FreeCamera freeCamera(camera);
        freeCamera.SetMoveSpeed(100.0f);

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

        // 본 팔레트 cbuffer — HLSL `bones[MAX_BONES=128]` 와 1:1.
        // 매 프레임 in-flight 슬롯마다 독립 — frameCBs 와 동일 패턴.
        constexpr std::uint32_t kMaxBones = 128;
        struct BonePalette
        {
            DirectX::XMFLOAT4X4 bones[kMaxBones];
        };
        static_assert(sizeof(BonePalette) == kMaxBones * 64, "BonePalette 크기 깨짐");

        // 프레임당 ConstantBuffer — CPU 가 frame N 의 데이터를 쓰는 동안 GPU 는 frame N-1 의 데이터를 읽음.
        std::array<std::unique_ptr<engine::render::ConstantBuffer>, kFrameCount> frameCBs;
        std::array<std::unique_ptr<engine::render::ConstantBuffer>, kFrameCount> bonePaletteCBs;
        for (std::uint32_t i = 0; i < kFrameCount; ++i)
        {
            frameCBs[i] = std::make_unique<engine::render::ConstantBuffer>(
                device, static_cast<engine::uint32>(sizeof(FrameConstants)));
            bonePaletteCBs[i] = std::make_unique<engine::render::ConstantBuffer>(
                device, static_cast<engine::uint32>(sizeof(BonePalette)));
        }

        // 정적 모델용 identity 팔레트 — Animator 없을 때 cbuffer 에 한 번만 채우면 됨.
        BonePalette identityPalette{};
        for (std::uint32_t i = 0; i < kMaxBones; ++i)
        {
            DirectX::XMStoreFloat4x4(&identityPalette.bones[i], DirectX::XMMatrixIdentity());
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

            // World = Y축 천천히 자전 (Dragon 시각 흐름). Cube 시절보다 회전 속도 1/4.
            using namespace DirectX;
            const XMMATRIX world = XMMatrixRotationY(t * 0.25f);
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
            engine::render::CommandList&    cmdList     = *cmdLists[frameIndex];
            engine::render::ConstantBuffer& frameCB     = *frameCBs[frameIndex];
            engine::render::ConstantBuffer& bonePalette = *bonePaletteCBs[frameIndex];
            frameCB.Update(&cb, sizeof(cb));

            // 본 팔레트 갱신 — Animator 있으면 매 프레임 계산, 없으면 identity.
            BonePalette palette = identityPalette;   // 모두 identity 초기화
            if (animator)
            {
                animator->Update(dt);
                const auto& src = animator->Palette();
                const size_t n = (src.size() < kMaxBones) ? src.size() : kMaxBones;
                for (size_t i = 0; i < n; ++i)
                {
                    palette.bones[i] = src[i];
                }
            }
            bonePalette.Update(&palette, sizeof(palette));

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

            // RootParam: [0]=b0 FrameConstants, [1]=b1 BonePalette, [2]=t0 SRV table.
            list->SetGraphicsRootConstantBufferView(0, frameCB.GpuAddress());
            list->SetGraphicsRootConstantBufferView(1, bonePalette.GpuAddress());
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            // 머티리얼별 sub-draw — Mesh 가 매 SubMesh 마다 SetGraphicsRootDescriptorTable(2, ...) + DrawIndexed.
            mainMesh->BindVertexBuffer(list);
            mainMesh->DrawAll(list, /*rootParamMaterialTable*/2, fallbackAlbedo.SrvGpuHandle());

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
