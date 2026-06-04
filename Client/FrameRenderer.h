#pragma once

#include "core/Types.h"
#include "render/SwapChain.h"

#include <Windows.h>
#include <d3d12.h>

#include <array>
#include <cstdint>
#include <memory>

namespace engine::render
{
    class Camera;
    class CommandList;
    class CommandQueue;
    class ConstantBuffer;
    class DebugRenderer;
    class DepthStencilBuffer;
    class Device;
    class PipelineState;
    class RenderTexture;
    class RootSignature;
    class ShadowMap;
    class SrvDescriptorHeap;
    class SwapChain;
    class Texture;
}

namespace client
{
    class SceneRuntime;

    // 매 프레임 명령 기록 + Present + N프레임 in-flight fence 관리.
    //
    // 보유:
    //   - kFrameCount 개의 CommandList (allocator+list 쌍).
    //   - kFrameCount 개의 frame fence value.
    //
    // 의존 (모두 참조 — Application 이 라이프타임 소유):
    //   - Device / CommandQueue / SwapChain / DepthStencilBuffer
    //   - RootSignature / PipelineState / SrvDescriptorHeap (디스크립터 힙 바인딩용)
    //   - Texture (fallback albedo — SceneRuntime.RecordDraw 에 전달)
    //
    // 매 프레임 흐름 (Render() 한 메서드):
    //   ① fence 대기 (이 슬롯의 직전 제출 완료).
    //   ② SceneRuntime.PrepareGpuResources(frameIndex, camera).
    //   ③ cmdList.Reset → Barrier PRESENT→RT → ClearRTV+DSV → 뷰포트/시저.
    //   ④ RootSig + PSO + SetDescriptorHeaps 바인딩.
    //   ⑤ SceneRuntime.RecordDraw(list, frameIndex, fallback).
    //   ⑥ Barrier RT→PRESENT → Close → Execute → Present.
    //   ⑦ commandQueue.Signal() → fence value 갱신.
    //
    // 리사이즈는 Application 책임 — FrameRenderer 는 *현 시점* 의 SwapChain/Depth 상태를 사용.
    class FrameRenderer final
    {
    public:
        // N프레임 in-flight — SwapChain::kBackBufferCount 와 1소스 통일.
        // SceneRuntime 의 인스턴스 CB / 라이트 SB 슬롯 수와 정확히 일치해야 frameIndex 가 안전.
        static constexpr engine::uint32 kFrameCount = engine::render::SwapChain::kBackBufferCount;

        struct InitInfo
        {
            engine::render::Device*             device         = nullptr;
            engine::render::CommandQueue*       queue          = nullptr;
            engine::render::SwapChain*          swapChain      = nullptr;
            engine::render::DepthStencilBuffer* depthBuffer    = nullptr;
            engine::render::RootSignature*      rootSig        = nullptr;
            engine::render::PipelineState*      pso            = nullptr;
            engine::render::SrvDescriptorHeap*  srvHeap        = nullptr;
            engine::render::Texture*            fallbackAlbedo = nullptr;
            // 그림자 (옵션 — 셋 다 있어야 그림자 패스 활성).
            engine::render::ShadowMap*          shadowMap      = nullptr;
            engine::render::RootSignature*      shadowRootSig  = nullptr;
            engine::render::PipelineState*      shadowPso      = nullptr;
            // 스카이박스 (옵션 — 둘 다 있어야 활성).
            engine::render::RootSignature*      skyboxRootSig  = nullptr;
            engine::render::PipelineState*      skyboxPso      = nullptr;
            // 포스트프로세싱 bloom (옵션 — 전부 있어야 활성). 없으면 백버퍼 직접 렌더.
            engine::render::RenderTexture*      hdrTarget      = nullptr;
            engine::render::RenderTexture*      bloomA         = nullptr;
            engine::render::RenderTexture*      bloomB         = nullptr;
            engine::render::RootSignature*      postRootSig    = nullptr;
            engine::render::PipelineState*      brightPso      = nullptr;
            engine::render::PipelineState*      blurPso        = nullptr;
            engine::render::PipelineState*      compositePso   = nullptr;
        };

        explicit FrameRenderer(const InitInfo& info);
        ~FrameRenderer();

        FrameRenderer(const FrameRenderer&)            = delete;
        FrameRenderer& operator=(const FrameRenderer&) = delete;
        FrameRenderer(FrameRenderer&&)                 = delete;
        FrameRenderer& operator=(FrameRenderer&&)      = delete;

        // 매 프레임 1회 호출. viewport/scissor 는 현재 윈도우 클라이언트 영역 크기 기준.
        void Render(SceneRuntime&                       sceneRuntime,
                    const engine::render::Camera&       camera,
                    const D3D12_VIEWPORT&               viewport,
                    const D3D12_RECT&                   scissor);

        // 윈도우 리사이즈 후 호출 — fence value reset (모든 슬롯 미사용 상태로).
        // 호출 전 CommandQueue::FlushGpu 선행 (Application 책임).
        void OnResize();

        // bloom 파라미터 — 게임 ImGui 슬라이더가 직접 바인딩(float&). 매 프레임 bright/composite 에 반영.
        float& BloomThreshold() noexcept { return m_bloomThreshold; }   // 이 휘도 이상만 bloom
        float& BloomIntensity() noexcept { return m_bloomIntensity; }   // composite 합성 가중
        bool   BloomAvailable() const noexcept { return m_compositePso != nullptr; }

    private:
        engine::render::Device&             m_device;
        engine::render::CommandQueue&       m_queue;
        engine::render::SwapChain&          m_swapChain;
        engine::render::DepthStencilBuffer& m_depthBuffer;
        engine::render::RootSignature&      m_rootSig;
        engine::render::PipelineState&      m_pso;
        engine::render::SrvDescriptorHeap&  m_srvHeap;
        engine::render::Texture&            m_fallbackAlbedo;
        engine::render::ShadowMap*          m_shadowMap     = nullptr;   // 옵션
        engine::render::RootSignature*      m_shadowRootSig = nullptr;
        engine::render::PipelineState*      m_shadowPso     = nullptr;
        engine::render::RootSignature*      m_skyboxRootSig = nullptr;   // 옵션
        engine::render::PipelineState*      m_skyboxPso     = nullptr;
        engine::render::RenderTexture*      m_hdrTarget   = nullptr;     // bloom 옵션
        engine::render::RenderTexture*      m_bloomA      = nullptr;
        engine::render::RenderTexture*      m_bloomB      = nullptr;
        engine::render::RootSignature*      m_postRootSig = nullptr;
        engine::render::PipelineState*      m_brightPso   = nullptr;
        engine::render::PipelineState*      m_blurPso     = nullptr;
        engine::render::PipelineState*      m_compositePso = nullptr;
        float                               m_bloomThreshold = 1.0f;   // ImGui 조절
        float                               m_bloomIntensity = 1.0f;
        engine::uint32                      m_frameIndex = 0;

        std::array<std::unique_ptr<engine::render::CommandList>,    kFrameCount> m_cmdLists;
        std::array<std::unique_ptr<engine::render::ConstantBuffer>, kFrameCount> m_skyCBs;    // 스카이박스 상수
        // bloom post 상수 — 패스별(0=bright,1=blurH,2=blurV,3=composite) × frame.
        std::array<std::array<std::unique_ptr<engine::render::ConstantBuffer>, 4>, kFrameCount> m_postCBs;
        std::array<std::uint64_t,                                kFrameCount> m_frameFenceValues{};

        // 디버그 라인 렌더러 — 원점 좌표축 그리기. SceneRuntime.RecordDraw 직후 호출.
        std::unique_ptr<engine::render::DebugRenderer>                        m_debugRenderer;
    };
}
