#include "FrameRenderer.h"

#include "SceneRuntime.h"

#include "render/Camera.h"
#include "render/CommandList.h"
#include "render/CommandQueue.h"
#include "render/DepthStencilBuffer.h"
#include "render/Device.h"
#include "render/PipelineState.h"
#include "render/RootSignature.h"
#include "render/SrvDescriptorHeap.h"
#include "render/SwapChain.h"
#include "render/Texture.h"

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
