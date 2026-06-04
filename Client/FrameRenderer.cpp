#include "FrameRenderer.h"

#include "SceneRuntime.h"

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
#include "render/ShadowMap.h"
#include "render/SrvDescriptorHeap.h"
#include "render/SwapChain.h"
#include "render/Texture.h"

#include "imgui.h"
#include "backends/imgui_impl_dx12.h"

#include <stdexcept>
#include <string>

namespace client
{
    namespace
    {
        constexpr float kClearColor[4] = { 0.05f, 0.07f, 0.10f, 1.0f };

        struct SkyConstants
        {
            DirectX::XMFLOAT4X4 invViewProj;
            DirectX::XMFLOAT3   cameraPosWS;  float _p0;
            DirectX::XMFLOAT3   sunDirWS;     float _p1;
        };
        static_assert(sizeof(SkyConstants) % 16 == 0, "SkyConstants 정렬");

        struct PostConstants
        {
            DirectX::XMFLOAT2 texelSize;
            DirectX::XMFLOAT2 blurDir;
            float             threshold;
            float             bloomIntensity;
            DirectX::XMFLOAT2 _pad;
        };
        static_assert(sizeof(PostConstants) % 16 == 0, "PostConstants 정렬");

        // 리소스 상태 전이 barrier 헬퍼.
        void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* res,
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

        // 멤버 reference 초기화 *이전* 에 nullptr 검증.
        // ctor 본문에서 검증하면 멤버 초기화 식의 역참조가 이미 UB 였음 (riview Critical).
        template <typename T>
        T& Require(T* p, const char* name)
        {
            if (p == nullptr)
            {
                throw std::runtime_error(std::string{"FrameRenderer: InitInfo."} + name + " == nullptr");
            }
            return *p;
        }
    }

    FrameRenderer::FrameRenderer(const InitInfo& info)
        : m_device         (Require(info.device,         "device"))
        , m_queue          (Require(info.queue,          "queue"))
        , m_swapChain      (Require(info.swapChain,      "swapChain"))
        , m_depthBuffer    (Require(info.depthBuffer,    "depthBuffer"))
        , m_rootSig        (Require(info.rootSig,        "rootSig"))
        , m_pso            (Require(info.pso,            "pso"))
        , m_srvHeap        (Require(info.srvHeap,        "srvHeap"))
        , m_fallbackAlbedo (Require(info.fallbackAlbedo, "fallbackAlbedo"))
        , m_shadowMap      (info.shadowMap)       // 옵션 — nullptr 허용
        , m_shadowRootSig  (info.shadowRootSig)
        , m_shadowPso      (info.shadowPso)
        , m_skyboxRootSig  (info.skyboxRootSig)
        , m_skyboxPso      (info.skyboxPso)
        , m_hdrTarget      (info.hdrTarget)
        , m_bloomA         (info.bloomA)
        , m_bloomB         (info.bloomB)
        , m_postRootSig    (info.postRootSig)
        , m_brightPso      (info.brightPso)
        , m_blurPso        (info.blurPso)
        , m_compositePso   (info.compositePso)
    {
        for (engine::uint32 f = 0; f < kFrameCount; ++f)
        {
            m_cmdLists[f] = std::make_unique<engine::render::CommandList>(m_device);
            m_skyCBs[f]   = std::make_unique<engine::render::ConstantBuffer>(
                m_device, static_cast<engine::uint32>(sizeof(SkyConstants)));
            for (int pass = 0; pass < 4; ++pass)
            {
                m_postCBs[f][pass] = std::make_unique<engine::render::ConstantBuffer>(
                    m_device, static_cast<engine::uint32>(sizeof(PostConstants)));
            }
        }

        // 디버그 렌더러 — 메인 PSO 와 같은 RTV 포맷 + DSV 포맷 사용 (depth-test 자체는 OFF).
        m_debugRenderer = std::make_unique<engine::render::DebugRenderer>(
            m_device, DXGI_FORMAT_R8G8B8A8_UNORM, m_depthBuffer.Format());
    }

    FrameRenderer::~FrameRenderer() = default;

    void FrameRenderer::OnResize()
    {
        // 슬롯별 *기대* fence value 만 0 으로 비워 다음 Render() 의 wait 분기 skip.
        // CommandQueue 의 내부 fence counter 는 변경 X (단조 증가 보존).
        // 호출자(Application) 가 사전에 CommandQueue::FlushGpu 수행 — GPU 가 모든 슬롯의
        // 직전 제출을 이미 완료한 상태라 wait skip 안전.
        for (auto& v : m_frameFenceValues) { v = 0; }
        m_frameIndex = 0;
    }

    void FrameRenderer::Render(SceneRuntime&                       sceneRuntime,
                               const engine::render::Camera&       camera,
                               const D3D12_VIEWPORT&               viewport,
                               const D3D12_RECT&                   scissor)
    {
        const engine::uint32 fi = m_frameIndex;

        // ① 이 슬롯의 직전 제출 완료 대기. fence==0 은 미사용 슬롯.
        if (m_frameFenceValues[fi] != 0)
        {
            m_queue.WaitForFenceValue(m_frameFenceValues[fi]);
        }

        // ② SceneRuntime — 그림자맵 주입 후 라이트 SB 업로드 + view-proj/camPos/lightViewProj 캐시.
        sceneRuntime.SetShadowMap(m_shadowMap);
        sceneRuntime.PrepareGpuResources(fi, camera);

        // ③ cmdList 리셋.
        engine::render::CommandList& cmdList = *m_cmdLists[fi];
        cmdList.Reset();
        ID3D12GraphicsCommandList* list = cmdList.Native();

        // ③a 그림자 패스 — 라이트 시점 깊이 렌더 (메인 패스 이전). 셋 다 있을 때만.
        const bool shadowPass = (m_shadowMap && m_shadowRootSig && m_shadowPso);
        if (shadowPass)
        {
            ID3D12Resource* const shadowRes = m_shadowMap->Native();
            D3D12_RESOURCE_BARRIER toDepth{};
            toDepth.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toDepth.Transition.pResource   = shadowRes;
            toDepth.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            toDepth.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            toDepth.Transition.StateAfter  = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            list->ResourceBarrier(1, &toDepth);

            const D3D12_CPU_DESCRIPTOR_HANDLE sdsv = m_shadowMap->DsvHandle();
            list->OMSetRenderTargets(0, nullptr, FALSE, &sdsv);
            list->ClearDepthStencilView(sdsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

            const float res = static_cast<float>(m_shadowMap->Resolution());
            const D3D12_VIEWPORT svp{ 0.0f, 0.0f, res, res, 0.0f, 1.0f };
            const D3D12_RECT     ssc{ 0, 0, static_cast<LONG>(res), static_cast<LONG>(res) };
            list->RSSetViewports(1, &svp);
            list->RSSetScissorRects(1, &ssc);

            list->SetGraphicsRootSignature(m_shadowRootSig->Native());
            list->SetPipelineState(m_shadowPso->Native());
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            sceneRuntime.RecordShadowDraw(list, fi);

            D3D12_RESOURCE_BARRIER toSrv{};
            toSrv.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toSrv.Transition.pResource   = shadowRes;
            toSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            toSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            toSrv.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            list->ResourceBarrier(1, &toSrv);
        }

        // ③b SrvHeap (메인/bloom 디스크립터 테이블 공용).
        ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Native() };
        list->SetDescriptorHeaps(1, heaps);

        const bool bloom = (m_hdrTarget && m_bloomA && m_bloomB && m_postRootSig
                            && m_brightPso && m_blurPso && m_compositePso);
        ID3D12Resource* const backBuffer = m_swapChain.CurrentBackBuffer();
        const D3D12_CPU_DESCRIPTOR_HANDLE backRtv = m_swapChain.CurrentRtv();
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv     = m_depthBuffer.DsvHandle();

        // === 씬 패스 — bloom 이면 HDR RT(RGBA16F), 아니면 백버퍼 직접 ===
        D3D12_CPU_DESCRIPTOR_HANDLE sceneRtv;
        if (bloom)
        {
            Transition(list, m_hdrTarget->Native(),
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            sceneRtv = m_hdrTarget->Rtv();
        }
        else
        {
            Transition(list, backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
            sceneRtv = backRtv;
        }
        list->OMSetRenderTargets(1, &sceneRtv, FALSE, &dsv);
        list->ClearRenderTargetView(sceneRtv, kClearColor, 0, nullptr);
        list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);

        // ④ 메인 RootSig + PSO.
        list->SetGraphicsRootSignature(m_rootSig.Native());
        list->SetPipelineState(m_pso.Native());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // ⑤ SceneRuntime — 인스턴스 cb 갱신 + draw.
        sceneRuntime.RecordDraw(list, fi, m_fallbackAlbedo);

        // ⑤a 스카이박스 — 기하 뒤(빈 픽셀=far) 절차적 하늘. 같은 RTV/DSV. 풀스크린 삼각형(VB 없음).
        if (m_skyboxRootSig && m_skyboxPso)
        {
            using namespace DirectX;
            XMVECTOR det;
            const XMMATRIX invVP = XMMatrixInverse(&det, camera.ViewProjection());
            SkyConstants sc{};
            XMStoreFloat4x4(&sc.invViewProj, invVP);
            sc.cameraPosWS = camera.Position();
            sc.sunDirWS    = sceneRuntime.SunDirectionWS();
            m_skyCBs[fi]->Update(&sc, sizeof(sc));

            list->SetGraphicsRootSignature(m_skyboxRootSig->Native());
            list->SetPipelineState(m_skyboxPso->Native());
            list->SetGraphicsRootConstantBufferView(0, m_skyCBs[fi]->GpuAddress());
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->IASetVertexBuffers(0, 0, nullptr);
            list->DrawInstanced(3, 1, 0, 0);
        }

        // === ⑤ab bloom — HDR → bright → blur(H/V) → composite(+tonemap) → 백버퍼 ===
        if (bloom)
        {
            constexpr auto kRT  = D3D12_RESOURCE_STATE_RENDER_TARGET;
            constexpr auto kPSR = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            Transition(list, m_hdrTarget->Native(), kRT, kPSR);   // HDR 샘플 가능

            // post 상수 4종 업데이트.
            const float hw = static_cast<float>(m_hdrTarget->Width());
            const float hh = static_cast<float>(m_hdrTarget->Height());
            const float bw = static_cast<float>(m_bloomA->Width());
            const float bh = static_cast<float>(m_bloomA->Height());
            PostConstants pc{};
            pc.texelSize = { 1.0f / hw, 1.0f / hh }; pc.threshold = m_bloomThreshold; pc.bloomIntensity = 0.0f; pc.blurDir = { 0.0f, 0.0f };
            m_postCBs[fi][0]->Update(&pc, sizeof(pc));                                   // bright
            pc.texelSize = { 1.0f / bw, 1.0f / bh }; pc.threshold = 0.0f; pc.blurDir = { 1.0f, 0.0f };
            m_postCBs[fi][1]->Update(&pc, sizeof(pc));                                   // blurH
            pc.blurDir = { 0.0f, 1.0f };
            m_postCBs[fi][2]->Update(&pc, sizeof(pc));                                   // blurV
            pc.blurDir = { 0.0f, 0.0f }; pc.bloomIntensity = m_bloomIntensity;
            m_postCBs[fi][3]->Update(&pc, sizeof(pc));                                   // composite

            auto postPass = [&](engine::render::PipelineState* pso, int pass,
                                D3D12_GPU_DESCRIPTOR_HANDLE src, D3D12_GPU_DESCRIPTOR_HANDLE src2,
                                D3D12_CPU_DESCRIPTOR_HANDLE rtv, engine::uint32 w, engine::uint32 h)
            {
                const D3D12_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f };
                const D3D12_RECT     rc{ 0, 0, static_cast<LONG>(w), static_cast<LONG>(h) };
                list->RSSetViewports(1, &vp);
                list->RSSetScissorRects(1, &rc);
                list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                list->SetGraphicsRootSignature(m_postRootSig->Native());
                list->SetPipelineState(pso->Native());
                list->SetGraphicsRootConstantBufferView(0, m_postCBs[fi][pass]->GpuAddress());
                list->SetGraphicsRootDescriptorTable(1, src);
                list->SetGraphicsRootDescriptorTable(2, src2);
                list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                list->IASetVertexBuffers(0, 0, nullptr);
                list->DrawInstanced(3, 1, 0, 0);
            };

            const auto hdrSrv = m_hdrTarget->SrvGpu();
            const auto aSrv   = m_bloomA->SrvGpu();
            const auto bSrv   = m_bloomB->SrvGpu();

            Transition(list, m_bloomA->Native(), kPSR, kRT);                              // bright: HDR→A
            postPass(m_brightPso, 0, hdrSrv, hdrSrv, m_bloomA->Rtv(), m_bloomA->Width(), m_bloomA->Height());
            Transition(list, m_bloomA->Native(), kRT, kPSR);

            Transition(list, m_bloomB->Native(), kPSR, kRT);                              // blurH: A→B
            postPass(m_blurPso, 1, aSrv, aSrv, m_bloomB->Rtv(), m_bloomB->Width(), m_bloomB->Height());
            Transition(list, m_bloomB->Native(), kRT, kPSR);

            Transition(list, m_bloomA->Native(), kPSR, kRT);                              // blurV: B→A
            postPass(m_blurPso, 2, bSrv, bSrv, m_bloomA->Rtv(), m_bloomA->Width(), m_bloomA->Height());
            Transition(list, m_bloomA->Native(), kRT, kPSR);

            // composite: HDR + bloomA → 백버퍼.
            Transition(list, backBuffer, D3D12_RESOURCE_STATE_PRESENT, kRT);
            postPass(m_compositePso, 3, hdrSrv, aSrv, backRtv,
                     static_cast<engine::uint32>(viewport.Width), static_cast<engine::uint32>(viewport.Height));
        }

        // 디버그 + ImGui 는 백버퍼에 (bloom 이면 composite 가 이미 PRESENT→RT 전이 완료).
        list->OMSetRenderTargets(1, &backRtv, FALSE, &dsv);
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);

        // ⑤b 디버그 라인 (원점 좌표축 + 바닥 grid) — RTV/DSV 그대로 사용. PSO/RootSig 만 자체 교체.
        //    depth-test OFF 이므로 X-Bot 메시 뒤에 있어도 가시. Grid 는 Jump 등 Y 변동 모션
        //    의 *바닥 참조* 용.
        if (m_debugRenderer)
        {
            m_debugRenderer->DrawGrid(list, fi, camera.ViewProjection());
            m_debugRenderer->DrawAxes(list, fi, camera.ViewProjection(), 100.0f);

            // 스켈레톤 시각화 — 본 parent→child world 선분. Foot IK 기반 + 좌표 진단.
            //   각 선분을 LineVertex 2개 (노란색) 로. depth-off 라 mesh 뒤에서도 가시.
            std::vector<std::pair<DirectX::XMFLOAT3, DirectX::XMFLOAT3>> segs;
            if (sceneRuntime.GetSkeletonWorldSegments(segs))
            {
                std::vector<engine::render::DebugRenderer::LineVertex> lv;
                lv.reserve(segs.size() * 2);
                const DirectX::XMFLOAT3 boneCol{ 1.0f, 0.85f, 0.2f };
                for (const auto& s : segs)
                {
                    lv.push_back({ s.first,  boneCol });
                    lv.push_back({ s.second, boneCol });
                }
                m_debugRenderer->DrawLines(
                    list, fi, camera.ViewProjection(),
                    lv.data(), static_cast<engine::uint32>(lv.size()));
            }
        }

        // ⑤c ImGui draw data — Application 이 ImGui::Render() 호출 후라면 valid.
        //   동일 RTV/DSV 사용. ImGui 의 SrvDescriptorHeap 은 init 시 Application 의 srvHeap.
        //   ImGui 가 자체 PSO/RootSig 로 그려서 list 의 binding 을 변경 — 후속 코드 없음 (Present 만).
        if (ImDrawData* drawData = ImGui::GetDrawData())
        {
            ImGui_ImplDX12_RenderDrawData(drawData, list);
        }

        // ⑥ Barrier RT→PRESENT → Close → Execute → Present.
        D3D12_RESOURCE_BARRIER toPresent{};
        toPresent.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toPresent.Transition.pResource   = backBuffer;
        toPresent.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        toPresent.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
        list->ResourceBarrier(1, &toPresent);

        cmdList.Close();
        m_queue.Execute(cmdList);
        m_swapChain.Present();

        // ⑦ fence value 갱신 + 다음 슬롯.
        m_frameFenceValues[fi] = m_queue.Signal();
        m_frameIndex = (m_frameIndex + 1) % kFrameCount;
    }
}
