#include "FrameRenderer.h"

#include "SceneRuntime.h"

#include "render/Camera.h"
#include "render/CommandList.h"
#include "render/CommandQueue.h"
#include "render/DebugRenderer.h"
#include "render/DepthStencilBuffer.h"
#include "render/Device.h"
#include "render/PipelineState.h"
#include "render/RootSignature.h"
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
    {
        for (engine::uint32 f = 0; f < kFrameCount; ++f)
        {
            m_cmdLists[f] = std::make_unique<engine::render::CommandList>(m_device);
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

        // ② SceneRuntime — 라이트 SB 업로드 + view-proj/camPos 캐시.
        sceneRuntime.PrepareGpuResources(fi, camera);

        // ③ cmdList 리셋 + Barrier PRESENT→RT + Clear + 뷰포트/시저.
        engine::render::CommandList& cmdList = *m_cmdLists[fi];
        cmdList.Reset();
        ID3D12GraphicsCommandList* list = cmdList.Native();

        ID3D12Resource* const backBuffer = m_swapChain.CurrentBackBuffer();

        D3D12_RESOURCE_BARRIER toRT{};
        toRT.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toRT.Transition.pResource   = backBuffer;
        toRT.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        toRT.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        toRT.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(1, &toRT);

        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_swapChain.CurrentRtv();
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_depthBuffer.DsvHandle();
        list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        list->ClearRenderTargetView(rtv, kClearColor, 0, nullptr);
        list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);

        // ④ RootSig + PSO + SrvHeap.
        list->SetGraphicsRootSignature(m_rootSig.Native());
        list->SetPipelineState(m_pso.Native());
        ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Native() };
        list->SetDescriptorHeaps(1, heaps);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // ⑤ SceneRuntime — 인스턴스 cb 갱신 + draw.
        sceneRuntime.RecordDraw(list, fi, m_fallbackAlbedo);

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
