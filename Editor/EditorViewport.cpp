#include "EditorViewport.h"

#include "render/Camera.h"
#include "render/CommandList.h"
#include "render/CommandQueue.h"
#include "render/ConstantBuffer.h"
#include "render/DebugRenderer.h"
#include "render/DepthStencilBuffer.h"
#include "render/Device.h"
#include "render/PipelineState.h"
#include "render/RenderTexture.h"
#include "render/RootSignature.h"
#include "render/ShaderCompiler.h"
#include "render/SrvDescriptorHeap.h"
#include "render/Texture.h"

#include "core/HrCheck.h"
#include "core/Logger.h"

#include "../Client/SceneRuntime.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

namespace editor
{
    namespace
    {
        constexpr DXGI_FORMAT kRttFormat   = DXGI_FORMAT_R8G8B8A8_UNORM;
        constexpr DXGI_FORMAT kHdrFormat   = DXGI_FORMAT_R16G16B16A16_FLOAT;
        constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;

        struct SkyConstants
        {
            DirectX::XMFLOAT4X4 invViewProj;
            DirectX::XMFLOAT3   cameraPosWS;  float _p0;
            DirectX::XMFLOAT3   sunDirWS;     float _p1;
        };
        struct PostConstants
        {
            DirectX::XMFLOAT2 texelSize;
            DirectX::XMFLOAT2 blurDir;
            float             threshold;
            float             bloomIntensity;
            DirectX::XMFLOAT2 _pad;
        };
        void TransitionRes(ID3D12GraphicsCommandList* list, ID3D12Resource* res,
                           D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
        {
            D3D12_RESOURCE_BARRIER b{};
            b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource   = res;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = before;
            b.Transition.StateAfter  = after;
            list->ResourceBarrier(1, &b);
        }
        constexpr float       kNearPlane   = 1.0f;
        constexpr float       kFarPlane    = 5000.0f;
        constexpr float       kClearColor[4] = { 0.05f, 0.07f, 0.10f, 1.0f };
        // orbit 카메라 디폴트 — Scene 의 cameraStart 기본값과 유사.
        constexpr std::uint32_t kInitialWidth  = 800;
        constexpr std::uint32_t kInitialHeight = 600;
    }

    EditorViewport::EditorViewport(engine::render::Device&            device,
                                   engine::render::CommandQueue&      queue,
                                   engine::render::SrvDescriptorHeap& srvHeap,
                                   std::uint32_t                      postSlotBase)
        : m_device (device)
        , m_queue  (queue)
        , m_srvHeap(srvHeap)
        , m_postSlotBase(postSlotBase)
    {
        // === RTV 디스크립터 힙 (1슬롯) ===
        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
        rtvDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvDesc.NumDescriptors = 1;
        rtvDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        engine::core::ThrowIfFailed(
            m_device.Native()->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_rtvHeap)),
            "EditorViewport: CreateDescriptorHeap(RTV) 실패");
        m_rtvCpu = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();

        // === SRV 슬롯 1개 SrvHeap 에서 예약 ===
        const auto srvHandle = m_srvHeap.Allocate();
        m_srvCpu = srvHandle.cpu;
        m_srvGpu = srvHandle.gpu;

        // === 초기 RTT + Depth (kInitialWidth × kInitialHeight) ===
        m_width  = kInitialWidth;
        m_height = kInitialHeight;
        CreateRtv();

        m_depth = std::make_unique<engine::render::DepthStencilBuffer>(
            m_device, m_width, m_height, kDepthFormat);

        // === 렌더 파이프라인 — Client 의 InitGraphicsPipeline 와 동일 ===
        const std::wstring shaderDir  = engine::render::ShaderCompiler::DefaultShaderDir();
        const std::wstring shaderPath = shaderDir + L"HelloTriangle.hlsl";
        m_vsBlob = engine::render::ShaderCompiler::CompileFromFile(
            shaderPath.c_str(), "VSMain", engine::render::ShaderCompiler::Stage::Vertex);
        m_psBlob = engine::render::ShaderCompiler::CompileFromFile(
            shaderPath.c_str(), "PSMain", engine::render::ShaderCompiler::Stage::Pixel);

        engine::render::RootSignature::Desc rsDesc{};
        rsDesc.cbvAtB0     = engine::render::RootSignature::Desc::CbvB0::All;
        rsDesc.cbvB1Vertex = true;
        rsDesc.srvT0Pixel  = true;
        rsDesc.srvT1Pixel  = true;
        rsDesc.srvT2Pixel  = true;
        rsDesc.srvT3Pixel  = true;   // [5] t3 normal map table
        rsDesc.srvT4Pixel  = true;   // [6] t4 shadow map table (에디터는 그림자맵 없이 폴백 바인딩)
        m_rootSig = std::make_unique<engine::render::RootSignature>(m_device, rsDesc);

        engine::render::PipelineState::Desc psoDesc{};
        psoDesc.vertexShader  = m_vsBlob.Get();
        psoDesc.pixelShader   = m_psBlob.Get();
        psoDesc.rootSignature = m_rootSig.get();
        psoDesc.rtvFormat     = kHdrFormat;   // 씬을 HDR RT 에 → bloom
        psoDesc.dsvFormat     = kDepthFormat;
        m_pso = std::make_unique<engine::render::PipelineState>(m_device, psoDesc);

        // === 포스트프로세싱 (skybox + bloom) ===
        // HDR 씬 타깃 + bloom ping-pong (SRV 예약 슬롯: base, base-1, base-2).
        m_hdrScene = std::make_unique<engine::render::RenderTexture>(
            m_device, m_srvHeap, m_postSlotBase,     m_width, m_height, kHdrFormat);
        const std::uint32_t bw0 = (m_width  > 1) ? m_width  / 2 : 1;
        const std::uint32_t bh0 = (m_height > 1) ? m_height / 2 : 1;
        m_bloomA = std::make_unique<engine::render::RenderTexture>(
            m_device, m_srvHeap, m_postSlotBase - 1, bw0, bh0, kHdrFormat);
        m_bloomB = std::make_unique<engine::render::RenderTexture>(
            m_device, m_srvHeap, m_postSlotBase - 2, bw0, bh0, kHdrFormat);

        // skybox.
        m_skyVs = engine::render::ShaderCompiler::CompileFromFile(
            (shaderDir + L"Skybox.hlsl").c_str(), "VSMain", engine::render::ShaderCompiler::Stage::Vertex);
        m_skyPs = engine::render::ShaderCompiler::CompileFromFile(
            (shaderDir + L"Skybox.hlsl").c_str(), "PSMain", engine::render::ShaderCompiler::Stage::Pixel);
        engine::render::RootSignature::Desc skyRs{};
        skyRs.cbvAtB0 = engine::render::RootSignature::Desc::CbvB0::All;
        m_skyRootSig = std::make_unique<engine::render::RootSignature>(m_device, skyRs);
        {
            engine::render::PipelineState::Desc d{};
            d.vertexShader = m_skyVs.Get(); d.pixelShader = m_skyPs.Get();
            d.rootSignature = m_skyRootSig.get();
            d.rtvFormat = kHdrFormat; d.dsvFormat = kDepthFormat; d.fullscreenSky = true;
            m_skyPso = std::make_unique<engine::render::PipelineState>(m_device, d);
        }

        // bloom post.
        const std::wstring postPath = shaderDir + L"PostProcess.hlsl";
        m_postVs      = engine::render::ShaderCompiler::CompileFromFile(postPath.c_str(), "FullscreenVS", engine::render::ShaderCompiler::Stage::Vertex);
        m_brightPs    = engine::render::ShaderCompiler::CompileFromFile(postPath.c_str(), "BrightPassPS", engine::render::ShaderCompiler::Stage::Pixel);
        m_blurPs      = engine::render::ShaderCompiler::CompileFromFile(postPath.c_str(), "BlurPS",       engine::render::ShaderCompiler::Stage::Pixel);
        m_compositePs = engine::render::ShaderCompiler::CompileFromFile(postPath.c_str(), "CompositePS",  engine::render::ShaderCompiler::Stage::Pixel);
        engine::render::RootSignature::Desc postRs{};
        postRs.postProcess = true;
        m_postRootSig = std::make_unique<engine::render::RootSignature>(m_device, postRs);
        auto mkPost = [&](ID3DBlob* ps, DXGI_FORMAT rtv) {
            engine::render::PipelineState::Desc d{};
            d.vertexShader = m_postVs.Get(); d.pixelShader = ps;
            d.rootSignature = m_postRootSig.get(); d.rtvFormat = rtv; d.fullscreen = true;
            return std::make_unique<engine::render::PipelineState>(m_device, d);
        };
        m_brightPso    = mkPost(m_brightPs.Get(),    kHdrFormat);
        m_blurPso      = mkPost(m_blurPs.Get(),      kHdrFormat);
        m_compositePso = mkPost(m_compositePs.Get(), kRttFormat);

        for (std::uint32_t f = 0; f < kPostFrames; ++f)
        {
            m_skyCBs[f] = std::make_unique<engine::render::ConstantBuffer>(m_device, sizeof(SkyConstants));
            for (int pass = 0; pass < 4; ++pass)
            {
                m_postCBs[f][pass] = std::make_unique<engine::render::ConstantBuffer>(m_device, sizeof(PostConstants));
            }
        }

        // === Boot CommandList + fallback texture ===
        m_bootCmdList = std::make_unique<engine::render::CommandList>(m_device);

        constexpr std::uint8_t kGray[4] = { 200, 200, 200, 255 };
        m_fallback = std::make_unique<engine::render::Texture>(
            m_device, m_queue, *m_bootCmdList, kGray, 1, 1);
        m_fallback->CreateSrv(m_device, m_srvHeap);

        // === Camera + orbit 초기 상태 ===
        m_camera = std::make_unique<engine::render::Camera>();
        m_camera->SetUp({ 0.0f, 1.0f, 0.0f });
        m_camera->SetPerspective(
            DirectX::XM_PIDIV4,
            static_cast<float>(m_width) / static_cast<float>(m_height),
            kNearPlane, kFarPlane);
        UpdateCameraFromOrbit();

        // === Debug Renderer — Y=0 격자 + 좌표축 ===
        m_debug = std::make_unique<engine::render::DebugRenderer>(m_device, kRttFormat, kDepthFormat);

        engine::core::LogInfoA("[editor] Viewport 초기화 완료 (RTT + 파이프라인 + Debug grid)\n");
    }

    EditorViewport::~EditorViewport() = default;

    void EditorViewport::CreateRtv()
    {
        m_rttTexture.Reset();

        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC tex{};
        tex.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        tex.Alignment          = 0;
        tex.Width              = m_width;
        tex.Height             = m_height;
        tex.DepthOrArraySize   = 1;
        tex.MipLevels          = 1;
        tex.Format             = kRttFormat;
        tex.SampleDesc.Count   = 1;
        tex.SampleDesc.Quality = 0;
        tex.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        tex.Flags              = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE clearVal{};
        clearVal.Format   = kRttFormat;
        clearVal.Color[0] = kClearColor[0];
        clearVal.Color[1] = kClearColor[1];
        clearVal.Color[2] = kClearColor[2];
        clearVal.Color[3] = kClearColor[3];

        engine::core::ThrowIfFailed(
            m_device.Native()->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE,
                &tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                &clearVal, IID_PPV_ARGS(&m_rttTexture)),
            "EditorViewport: RTT 텍스처 생성 실패");

        // RTV view
        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.Format             = kRttFormat;
        rtvDesc.ViewDimension      = D3D12_RTV_DIMENSION_TEXTURE2D;
        rtvDesc.Texture2D.MipSlice = 0;
        m_device.Native()->CreateRenderTargetView(m_rttTexture.Get(), &rtvDesc, m_rtvCpu);

        // SRV view (ImGui::Image 가 sample)
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format                  = kRttFormat;
        srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels     = 1;
        m_device.Native()->CreateShaderResourceView(m_rttTexture.Get(), &srvDesc, m_srvCpu);
    }

    void EditorViewport::Resize(std::uint32_t width, std::uint32_t height)
    {
        if (width < 16)  width = 16;
        if (height < 16) height = 16;
        if (width == m_width && height == m_height) { return; }

        m_queue.FlushGpu();

        m_width  = width;
        m_height = height;
        CreateRtv();
        m_depth->Resize(m_device, m_width, m_height);
        if (m_hdrScene) { m_hdrScene->Resize(m_device, m_width, m_height); }
        const std::uint32_t bw = (m_width  > 1) ? m_width  / 2 : 1;
        const std::uint32_t bh = (m_height > 1) ? m_height / 2 : 1;
        if (m_bloomA) { m_bloomA->Resize(m_device, bw, bh); }
        if (m_bloomB) { m_bloomB->Resize(m_device, bw, bh); }

        if (m_camera)
        {
            m_camera->SetPerspective(
                DirectX::XM_PIDIV4,
                static_cast<float>(m_width) / static_cast<float>(m_height),
                kNearPlane, kFarPlane);
        }
    }

    void EditorViewport::UpdateInput(float mouseDeltaX, float mouseDeltaY,
                                     float wheelDelta, bool rmbHeld, bool hovered)
    {
        // 호버 안 된 패널은 입력 무시 — 다른 패널 위 클릭/휠과 충돌 방지.
        // 단 RMB 가 이미 눌린 채로 호버를 벗어났을 때도 회전 유지하려면 외부에서 state 관리.
        if (!hovered) { return; }

        // RMB drag → yaw/pitch 회전.
        if (rmbHeld)
        {
            constexpr float kRotSpeed = 0.006f;   // rad / pixel
            m_orbit.yaw   += mouseDeltaX * kRotSpeed;
            m_orbit.pitch += mouseDeltaY * kRotSpeed;
            constexpr float kPitchMax = 1.4f;
            constexpr float kPitchMin = -1.4f;
            m_orbit.pitch = std::clamp(m_orbit.pitch, kPitchMin, kPitchMax);
        }

        // 휠 → distance.
        if (wheelDelta != 0.0f)
        {
            constexpr float kZoomStep = 40.0f;
            m_orbit.distance -= wheelDelta * kZoomStep;
            m_orbit.distance = std::clamp(m_orbit.distance, 50.0f, 3000.0f);
        }

        UpdateCameraFromOrbit();
    }

    void EditorViewport::PanCamera(float forward, float right, float up, float dt) noexcept
    {
        if (forward == 0.0f && right == 0.0f && up == 0.0f) { return; }
        // 카메라 yaw 기준 수평 forward/right (pitch 무시 — 지면 위를 미끄러지듯 이동).
        const float cy = std::cos(m_orbit.yaw);
        const float sy = std::sin(m_orbit.yaw);
        const float speed = std::max(m_orbit.distance, 100.0f) * 0.8f * dt;   // 줌 거리 비례(멀수록 빠름)
        // forward(W) = (sy,0,cy), right(D) = (cy,0,-sy) (LH, yaw=0 → +Z 보고 +X 가 오른쪽).
        m_orbit.target.x += (sy * forward + cy * right) * speed;
        m_orbit.target.z += (cy * forward - sy * right) * speed;
        m_orbit.target.y += up * speed;
        UpdateCameraFromOrbit();
    }

    void EditorViewport::UpdateCameraFromOrbit()
    {
        // yaw=0, pitch=0 → 카메라가 target 의 -Z 쪽. yaw 증가 = top view 시계 반대.
        const float cy = std::cos(m_orbit.yaw);
        const float sy = std::sin(m_orbit.yaw);
        const float cp = std::cos(m_orbit.pitch);
        const float sp = std::sin(m_orbit.pitch);

        // back direction (target → camera)
        const float bx = -sy * cp;
        const float by =  sp;
        const float bz = -cy * cp;

        const DirectX::XMFLOAT3 camPos {
            m_orbit.target.x + bx * m_orbit.distance,
            m_orbit.target.y + by * m_orbit.distance,
            m_orbit.target.z + bz * m_orbit.distance
        };
        m_camera->SetPosition(camPos);
        m_camera->SetTarget  (m_orbit.target);
    }

    void EditorViewport::Render(ID3D12GraphicsCommandList* list,
                                client::SceneRuntime&      sceneRuntime,
                                std::uint32_t              frameIndex)
    {
        constexpr auto kPSR = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        constexpr auto kRT  = D3D12_RESOURCE_STATE_RENDER_TARGET;

        // SceneRuntime — 게임처럼 선형 HDR 출력(composite 가 톤맵) + 라이트/view-proj 캐시.
        sceneRuntime.SetApplyTonemap(false);
        sceneRuntime.PrepareGpuResources(frameIndex, *m_camera);

        ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Native() };
        list->SetDescriptorHeaps(1, heaps);

        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_depth->DsvHandle();
        const D3D12_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f };
        const D3D12_RECT     scissor{ 0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height) };

        // === 씬 + 스카이박스 → HDR RT ===
        TransitionRes(list, m_hdrScene->Native(), kPSR, kRT);
        const D3D12_CPU_DESCRIPTOR_HANDLE hdrRtv = m_hdrScene->Rtv();
        list->OMSetRenderTargets(1, &hdrRtv, FALSE, &dsv);
        list->ClearRenderTargetView(hdrRtv, kClearColor, 0, nullptr);
        list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        list->RSSetViewports(1, &vp);
        list->RSSetScissorRects(1, &scissor);

        list->SetGraphicsRootSignature(m_rootSig->Native());
        list->SetPipelineState(m_pso->Native());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        sceneRuntime.RecordDraw(list, frameIndex, *m_fallback);

        // 스카이박스 (절차적 하늘) → HDR RT 빈 픽셀.
        {
            using namespace DirectX;
            XMVECTOR det;
            const XMMATRIX invVP = XMMatrixInverse(&det, m_camera->ViewProjection());
            SkyConstants sc{};
            XMStoreFloat4x4(&sc.invViewProj, invVP);
            sc.cameraPosWS = m_camera->Position();
            sc.sunDirWS    = sceneRuntime.SunDirectionWS();
            m_skyCBs[frameIndex]->Update(&sc, sizeof(sc));
            list->SetGraphicsRootSignature(m_skyRootSig->Native());
            list->SetPipelineState(m_skyPso->Native());
            list->SetGraphicsRootConstantBufferView(0, m_skyCBs[frameIndex]->GpuAddress());
            list->IASetVertexBuffers(0, 0, nullptr);
            list->DrawInstanced(3, 1, 0, 0);
        }
        TransitionRes(list, m_hdrScene->Native(), kRT, kPSR);

        // === bloom — bright → blur(H/V) → composite → 표시 RTT ===
        {
            const float hw = static_cast<float>(m_hdrScene->Width());
            const float hh = static_cast<float>(m_hdrScene->Height());
            const float bw = static_cast<float>(m_bloomA->Width());
            const float bh = static_cast<float>(m_bloomA->Height());
            PostConstants pc{};
            pc.texelSize = { 1.0f/hw, 1.0f/hh }; pc.threshold = m_bloomThreshold; pc.bloomIntensity = 0.0f; pc.blurDir = { 0.0f, 0.0f };
            m_postCBs[frameIndex][0]->Update(&pc, sizeof(pc));
            pc.texelSize = { 1.0f/bw, 1.0f/bh }; pc.threshold = 0.0f; pc.blurDir = { 1.0f, 0.0f };
            m_postCBs[frameIndex][1]->Update(&pc, sizeof(pc));
            pc.blurDir = { 0.0f, 1.0f };
            m_postCBs[frameIndex][2]->Update(&pc, sizeof(pc));
            pc.blurDir = { 0.0f, 0.0f }; pc.bloomIntensity = m_bloomIntensity;
            m_postCBs[frameIndex][3]->Update(&pc, sizeof(pc));

            auto postPass = [&](engine::render::PipelineState* pso, int pass,
                                D3D12_GPU_DESCRIPTOR_HANDLE src, D3D12_GPU_DESCRIPTOR_HANDLE src2,
                                D3D12_CPU_DESCRIPTOR_HANDLE rtv, std::uint32_t w, std::uint32_t h)
            {
                const D3D12_VIEWPORT pv{ 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f };
                const D3D12_RECT     pr{ 0, 0, static_cast<LONG>(w), static_cast<LONG>(h) };
                list->RSSetViewports(1, &pv);
                list->RSSetScissorRects(1, &pr);
                list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                list->SetGraphicsRootSignature(m_postRootSig->Native());
                list->SetPipelineState(pso->Native());
                list->SetGraphicsRootConstantBufferView(0, m_postCBs[frameIndex][pass]->GpuAddress());
                list->SetGraphicsRootDescriptorTable(1, src);
                list->SetGraphicsRootDescriptorTable(2, src2);
                list->IASetVertexBuffers(0, 0, nullptr);
                list->DrawInstanced(3, 1, 0, 0);
            };
            const auto hdrSrv = m_hdrScene->SrvGpu();
            const auto aSrv   = m_bloomA->SrvGpu();
            const auto bSrv   = m_bloomB->SrvGpu();

            TransitionRes(list, m_bloomA->Native(), kPSR, kRT);
            postPass(m_brightPso.get(), 0, hdrSrv, hdrSrv, m_bloomA->Rtv(), m_bloomA->Width(), m_bloomA->Height());
            TransitionRes(list, m_bloomA->Native(), kRT, kPSR);
            TransitionRes(list, m_bloomB->Native(), kPSR, kRT);
            postPass(m_blurPso.get(), 1, aSrv, aSrv, m_bloomB->Rtv(), m_bloomB->Width(), m_bloomB->Height());
            TransitionRes(list, m_bloomB->Native(), kRT, kPSR);
            TransitionRes(list, m_bloomA->Native(), kPSR, kRT);
            postPass(m_blurPso.get(), 2, bSrv, bSrv, m_bloomA->Rtv(), m_bloomA->Width(), m_bloomA->Height());
            TransitionRes(list, m_bloomA->Native(), kRT, kPSR);

            TransitionRes(list, m_rttTexture.Get(), kPSR, kRT);
            postPass(m_compositePso.get(), 3, hdrSrv, aSrv, m_rtvCpu, m_width, m_height);
        }

        // === 디버그(격자/좌표축/스켈레톤) → 표시 RTT (composite 위에). 깊이 버퍼 그대로. ===
        list->OMSetRenderTargets(1, &m_rtvCpu, FALSE, &dsv);
        list->RSSetViewports(1, &vp);
        list->RSSetScissorRects(1, &scissor);
        const DirectX::XMMATRIX viewProj = m_camera->ViewProjection();
        m_debug->DrawGrid(list, frameIndex, viewProj);
        m_debug->DrawAxes(list, frameIndex, viewProj, 100.0f);

        // 스켈레톤 시각화 — 본 잡고 움직이는 기능의 기반. 노란 선분 + 선택 본 강조.
        if (m_showSkeleton)
        {
            std::vector<DirectX::XMFLOAT3> jp;
            std::vector<int>              jparent;
            std::vector<std::string>      jnames;
            if (sceneRuntime.GetSkeletonWorldJoints(jp, jparent, jnames))
            {
                std::vector<engine::render::DebugRenderer::LineVertex> lv;
                lv.reserve(jp.size() * 2 + 6);
                const DirectX::XMFLOAT3 boneCol{ 1.0f, 0.85f, 0.2f };
                // parent → child 선분.
                for (size_t b = 0; b < jp.size(); ++b)
                {
                    const int p = jparent[b];
                    if (p < 0 || static_cast<size_t>(p) >= jp.size()) { continue; }
                    lv.push_back({ jp[static_cast<size_t>(p)], boneCol });
                    lv.push_back({ jp[b], boneCol });
                }
                // 선택 본 강조 — 시안색 작은 3축 십자.
                if (m_selectedBone >= 0 && static_cast<size_t>(m_selectedBone) < jp.size())
                {
                    const DirectX::XMFLOAT3 c = jp[static_cast<size_t>(m_selectedBone)];
                    const DirectX::XMFLOAT3 hi{ 0.2f, 1.0f, 1.0f };
                    constexpr float k = 6.0f;
                    lv.push_back({ { c.x - k, c.y, c.z }, hi }); lv.push_back({ { c.x + k, c.y, c.z }, hi });
                    lv.push_back({ { c.x, c.y - k, c.z }, hi }); lv.push_back({ { c.x, c.y + k, c.z }, hi });
                    lv.push_back({ { c.x, c.y, c.z - k }, hi }); lv.push_back({ { c.x, c.y, c.z + k }, hi });
                }
                m_debug->DrawLines(list, frameIndex, viewProj,
                                   lv.data(), static_cast<engine::uint32>(lv.size()));
            }
        }

        // RTT 전이: RENDER_TARGET → SHADER_RESOURCE (ImGui::Image 가 sample 가능)
        D3D12_RESOURCE_BARRIER toSrv{};
        toSrv.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toSrv.Transition.pResource   = m_rttTexture.Get();
        toSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        toSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        toSrv.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1, &toSrv);
    }

    bool EditorViewport::RaycastToGround(float screenX, float screenY,
                                         DirectX::XMFLOAT3& outWorld) const noexcept
    {
        using namespace DirectX;
        if (m_width == 0 || m_height == 0) { return false; }

        // RTT 좌표 (좌상단 0,0) → NDC (-1..+1, Y flip)
        const float ndcX = (screenX / static_cast<float>(m_width))  * 2.0f - 1.0f;
        const float ndcY = 1.0f - (screenY / static_cast<float>(m_height)) * 2.0f;

        const XMMATRIX viewProj = m_camera->ViewProjection();
        XMVECTOR det;
        const XMMATRIX invVP = XMMatrixInverse(&det, viewProj);
        if (XMVectorGetX(det) == 0.0f) { return false; }

        // near (z=0), far (z=1) — D3D NDC depth range.
        XMVECTOR nearH = XMVectorSet(ndcX, ndcY, 0.0f, 1.0f);
        XMVECTOR farH  = XMVectorSet(ndcX, ndcY, 1.0f, 1.0f);
        XMVECTOR nearW = XMVector4Transform(nearH, invVP);
        XMVECTOR farW  = XMVector4Transform(farH,  invVP);
        nearW = XMVectorDivide(nearW, XMVectorSplatW(nearW));
        farW  = XMVectorDivide(farW,  XMVectorSplatW(farW));

        XMVECTOR rayDir = XMVector3Normalize(XMVectorSubtract(farW, nearW));
        const float dirY = XMVectorGetY(rayDir);
        if (std::abs(dirY) < 1e-6f) { return false; }   // 광선이 Y 축과 평행

        const float nearY = XMVectorGetY(nearW);
        const float t = -nearY / dirY;
        if (t < 0.0f) { return false; }                 // 카메라 뒤쪽

        const XMVECTOR hit = XMVectorAdd(nearW, XMVectorScale(rayDir, t));
        XMStoreFloat3(&outWorld, hit);
        outWorld.y = 0.0f;   // 평면 위 — 수치 노이즈 제거
        return true;
    }

    bool EditorViewport::RaycastToHeightField(
        float screenX, float screenY,
        const std::function<float(float, float)>& sampleHeight,
        DirectX::XMFLOAT3& outWorld) const noexcept
    {
        using namespace DirectX;
        if (m_width == 0 || m_height == 0) { return false; }

        const float ndcX = (screenX / static_cast<float>(m_width))  * 2.0f - 1.0f;
        const float ndcY = 1.0f - (screenY / static_cast<float>(m_height)) * 2.0f;

        const XMMATRIX viewProj = m_camera->ViewProjection();
        XMVECTOR det;
        const XMMATRIX invVP = XMMatrixInverse(&det, viewProj);
        if (XMVectorGetX(det) == 0.0f) { return false; }

        XMVECTOR nearW = XMVector4Transform(XMVectorSet(ndcX, ndcY, 0.0f, 1.0f), invVP);
        XMVECTOR farW  = XMVector4Transform(XMVectorSet(ndcX, ndcY, 1.0f, 1.0f), invVP);
        nearW = XMVectorDivide(nearW, XMVectorSplatW(nearW));
        farW  = XMVectorDivide(farW,  XMVectorSplatW(farW));
        const XMVECTOR rayDir = XMVector3Normalize(XMVectorSubtract(farW, nearW));
        const float dirY  = XMVectorGetY(rayDir);
        const float nearY = XMVectorGetY(nearW);
        if (std::abs(dirY) < 1e-6f) { return false; }

        // 수평 평면 y=Y 교차를 반복 정제 — 표면 높이로 평면을 갱신해 기복 지형에 수렴.
        float surfaceY = 0.0f;
        XMVECTOR p = XMVectorZero();
        for (int it = 0; it < 4; ++it)
        {
            const float t = (surfaceY - nearY) / dirY;
            if (t < 0.0f) { return false; }
            p = XMVectorAdd(nearW, XMVectorScale(rayDir, t));
            surfaceY = sampleHeight(XMVectorGetX(p), XMVectorGetZ(p));
        }
        XMStoreFloat3(&outWorld, p);
        outWorld.y = surfaceY;
        return true;
    }

    bool EditorViewport::ScreenToWorldAtDepth(float screenX, float screenY,
                                              const DirectX::XMFLOAT3& refWorld,
                                              DirectX::XMFLOAT3& outWorld) const noexcept
    {
        using namespace DirectX;
        if (m_width == 0 || m_height == 0) { return false; }

        // RTT 좌표 → NDC → world ray (RaycastToGround 와 동일 unproject).
        const float ndcX = (screenX / static_cast<float>(m_width))  * 2.0f - 1.0f;
        const float ndcY = 1.0f - (screenY / static_cast<float>(m_height)) * 2.0f;

        const XMMATRIX viewProj = m_camera->ViewProjection();
        XMVECTOR det;
        const XMMATRIX invVP = XMMatrixInverse(&det, viewProj);
        if (XMVectorGetX(det) == 0.0f) { return false; }

        XMVECTOR nearW = XMVector4Transform(XMVectorSet(ndcX, ndcY, 0.0f, 1.0f), invVP);
        XMVECTOR farW  = XMVector4Transform(XMVectorSet(ndcX, ndcY, 1.0f, 1.0f), invVP);
        nearW = XMVectorDivide(nearW, XMVectorSplatW(nearW));
        farW  = XMVectorDivide(farW,  XMVectorSplatW(farW));
        const XMVECTOR rayDir = XMVector3Normalize(XMVectorSubtract(farW, nearW));

        // 평면: refWorld 통과, 법선 = 카메라 forward (target - position).
        const XMFLOAT3 camPos = m_camera->Position();
        const XMVECTOR n = XMVector3Normalize(
            XMVectorSubtract(XMLoadFloat3(&m_orbit.target), XMLoadFloat3(&camPos)));
        const float denom = XMVectorGetX(XMVector3Dot(rayDir, n));
        if (std::abs(denom) < 1e-6f) { return false; }
        const float t = XMVectorGetX(
            XMVector3Dot(XMVectorSubtract(XMLoadFloat3(&refWorld), nearW), n)) / denom;
        if (t < 0.0f) { return false; }

        XMStoreFloat3(&outWorld, XMVectorAdd(nearW, XMVectorScale(rayDir, t)));
        return true;
    }

    bool EditorViewport::WorldToScreen(const DirectX::XMFLOAT3& world,
                                       float& outX, float& outY) const noexcept
    {
        using namespace DirectX;
        if (m_width == 0 || m_height == 0) { return false; }
        const XMMATRIX viewProj = m_camera->ViewProjection();
        const XMVECTOR p = XMVector3Transform(XMLoadFloat3(&world), viewProj);
        const float w = XMVectorGetW(p);
        if (w <= 1e-5f) { return false; }   // 카메라 뒤 / 평면
        const float ndcX = XMVectorGetX(p) / w;
        const float ndcY = XMVectorGetY(p) / w;
        outX = (ndcX * 0.5f + 0.5f) * static_cast<float>(m_width);
        outY = (1.0f - (ndcY * 0.5f + 0.5f)) * static_cast<float>(m_height);
        return true;
    }

    void EditorViewport::CameraRightUp(DirectX::XMFLOAT3& outRight,
                                       DirectX::XMFLOAT3& outUp) const noexcept
    {
        using namespace DirectX;
        const XMFLOAT3 camPos = m_camera->Position();
        const XMFLOAT3 tgt    = m_orbit.target;
        const XMVECTOR fwd    = XMVector3Normalize(
            XMVectorSubtract(XMLoadFloat3(&tgt), XMLoadFloat3(&camPos)));
        const XMVECTOR worldUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        XMVECTOR right = XMVector3Cross(worldUp, fwd);
        if (XMVectorGetX(XMVector3LengthSq(right)) < 1e-6f)
        {
            right = XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);   // fwd 가 거의 수직일 때 fallback
        }
        right = XMVector3Normalize(right);
        const XMVECTOR up = XMVector3Normalize(XMVector3Cross(fwd, right));
        XMStoreFloat3(&outRight, right);
        XMStoreFloat3(&outUp,    up);
    }

    void EditorViewport::FocusOn(const DirectX::XMFLOAT3& center, float radius) noexcept
    {
        m_orbit.target = center;
        const float fovY = DirectX::XM_PIDIV4;
        const float r    = (radius > 1.0f) ? radius : 50.0f;
        // 구체가 세로 FOV 안에 들어오는 거리 + 여유 마진.
        float d = r / std::tan(fovY * 0.5f) * 1.25f;
        m_orbit.distance = std::clamp(d, 50.0f, 3000.0f);
        UpdateCameraFromOrbit();
    }

    void EditorViewport::PickBone(client::SceneRuntime& sceneRuntime,
                                  float screenX, float screenY, float pixelRadius)
    {
        std::vector<DirectX::XMFLOAT3> jp;
        std::vector<int>              jparent;
        std::vector<std::string>      jnames;
        if (!sceneRuntime.GetSkeletonWorldJoints(jp, jparent, jnames))
        {
            m_selectedBone = -1;
            return;
        }

        int   best     = -1;
        float bestDist2 = pixelRadius * pixelRadius;
        for (size_t b = 0; b < jp.size(); ++b)
        {
            float sx, sy;
            if (!WorldToScreen(jp[b], sx, sy)) { continue; }
            const float dx = sx - screenX;
            const float dy = sy - screenY;
            const float d2 = dx * dx + dy * dy;
            if (d2 < bestDist2)
            {
                bestDist2 = d2;
                best      = static_cast<int>(b);
            }
        }
        m_selectedBone = best;   // 반경 내 본 없으면 -1 (선택 해제)
    }
}
