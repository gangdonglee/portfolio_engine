#include "EditorViewport.h"

#include "render/Camera.h"
#include "render/CommandList.h"
#include "render/CommandQueue.h"
#include "render/DebugRenderer.h"
#include "render/DepthStencilBuffer.h"
#include "render/Device.h"
#include "render/PipelineState.h"
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

namespace editor
{
    namespace
    {
        constexpr DXGI_FORMAT kRttFormat   = DXGI_FORMAT_R8G8B8A8_UNORM;
        constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;
        constexpr float       kNearPlane   = 1.0f;
        constexpr float       kFarPlane    = 5000.0f;
        constexpr float       kClearColor[4] = { 0.05f, 0.07f, 0.10f, 1.0f };
        // orbit 카메라 디폴트 — Scene 의 cameraStart 기본값과 유사.
        constexpr std::uint32_t kInitialWidth  = 800;
        constexpr std::uint32_t kInitialHeight = 600;
    }

    EditorViewport::EditorViewport(engine::render::Device&            device,
                                   engine::render::CommandQueue&      queue,
                                   engine::render::SrvDescriptorHeap& srvHeap)
        : m_device (device)
        , m_queue  (queue)
        , m_srvHeap(srvHeap)
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
        m_rootSig = std::make_unique<engine::render::RootSignature>(m_device, rsDesc);

        engine::render::PipelineState::Desc psoDesc{};
        psoDesc.vertexShader  = m_vsBlob.Get();
        psoDesc.pixelShader   = m_psBlob.Get();
        psoDesc.rootSignature = m_rootSig.get();
        psoDesc.rtvFormat     = kRttFormat;
        psoDesc.dsvFormat     = kDepthFormat;
        m_pso = std::make_unique<engine::render::PipelineState>(m_device, psoDesc);

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
        // SceneRuntime — 라이트 SB / view-proj 캐시.
        sceneRuntime.PrepareGpuResources(frameIndex, *m_camera);

        // RTT 전이: SHADER_RESOURCE → RENDER_TARGET
        D3D12_RESOURCE_BARRIER toRT{};
        toRT.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toRT.Transition.pResource   = m_rttTexture.Get();
        toRT.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        toRT.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        toRT.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(1, &toRT);

        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_depth->DsvHandle();
        list->OMSetRenderTargets(1, &m_rtvCpu, FALSE, &dsv);
        list->ClearRenderTargetView(m_rtvCpu, kClearColor, 0, nullptr);
        list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

        D3D12_VIEWPORT vp{};
        vp.Width    = static_cast<float>(m_width);
        vp.Height   = static_cast<float>(m_height);
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        list->RSSetViewports(1, &vp);

        D3D12_RECT scissor{};
        scissor.right  = static_cast<LONG>(m_width);
        scissor.bottom = static_cast<LONG>(m_height);
        list->RSSetScissorRects(1, &scissor);

        list->SetGraphicsRootSignature(m_rootSig->Native());
        list->SetPipelineState(m_pso->Native());
        ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Native() };
        list->SetDescriptorHeaps(1, heaps);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        sceneRuntime.RecordDraw(list, frameIndex, *m_fallback);

        // 디버그 — Y=0 격자 + 좌표축 (배치 워크플로우 바닥 참조).
        // depth-test OFF — 어떤 메쉬에도 가려지지 않음. PSO 가 자체 RootSig 사용 → 호출자 RootSig 영향 없음.
        const DirectX::XMMATRIX viewProj = m_camera->ViewProjection();
        m_debug->DrawGrid(list, frameIndex, viewProj);
        m_debug->DrawAxes(list, frameIndex, viewProj, 100.0f);

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
}
