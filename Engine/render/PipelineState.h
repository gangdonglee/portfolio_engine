#pragma once

#include <wrl/client.h>

// PipelineState::Desc 가 ID3DBlob* 와 DXGI_FORMAT 을 노출하므로 헤더에서 d3dcommon.h + dxgiformat.h 필요.
// 두 헤더 모두 가볍다 — d3d12.h 풀 노출은 회피.
#include <d3dcommon.h>     // ID3DBlob (typedef ID3D10Blob)
#include <dxgiformat.h>    // DXGI_FORMAT

struct ID3D12PipelineState;

namespace engine::render
{
    class Device;
    class RootSignature;

    // D3D12 Graphics Pipeline State Object (PSO) 캡슐화.
    //
    // Phase 1E-2 단계는 **HelloTriangle 전용 고정 셋업**:
    //   - 입력 레이아웃: POSITION(float3) + COLOR(float3), 한 정점 스트림.
    //   - 디폴트 Rasterizer/Blend/DepthStencil (Depth OFF).
    //   - 단일 RTV (포맷 외부 지정), DSV 없음.
    //
    // 향후 단계:
    //   - 입력 레이아웃 매개변수화.
    //   - Rasterizer/Blend/Depth 상태 외부 지정.
    //   - MRT / DSV 지원.
    //
    // 단일 소유 (복사·이동 금지).
    class PipelineState final
    {
    public:
        struct Desc
        {
            // 컴파일된 셰이더 바이트코드 (소유 안 함, 호출자 라이프타임 보장).
            ID3DBlob* vertexShader = nullptr;
            ID3DBlob* pixelShader  = nullptr;

            // 루트 시그니처 (소유 안 함).
            RootSignature* rootSignature = nullptr;

            // 렌더 타겟 포맷 — 보통 SwapChain 백버퍼 포맷과 일치.
            DXGI_FORMAT rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

            // 깊이/스텐실 포맷 — DXGI_FORMAT_UNKNOWN 이면 깊이 비활성, 그 외엔 활성.
            // DepthStencilBuffer 의 Format() 과 일치시킬 것.
            DXGI_FORMAT dsvFormat = DXGI_FORMAT_UNKNOWN;
        };

        PipelineState(Device& device, const Desc& desc);
        ~PipelineState();

        PipelineState(const PipelineState&)            = delete;
        PipelineState& operator=(const PipelineState&) = delete;
        PipelineState(PipelineState&&)                 = delete;
        PipelineState& operator=(PipelineState&&)      = delete;

        ID3D12PipelineState* Native() const noexcept;

    private:
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;
    };
}
