#pragma once

#include "core/Types.h"
#include "render/SwapChain.h"

#include <DirectXMath.h>
#include <array>
#include <cstdint>
#include <memory>

#include <d3d12.h>          // ID3D12* 전방선언 + DXGI_FORMAT
#include <dxgiformat.h>
#include <wrl/client.h>

struct ID3D12GraphicsCommandList;
struct ID3D12PipelineState;
struct ID3D12RootSignature;

namespace engine::render
{
    class Device;
    class ConstantBuffer;
    class VertexBuffer;

    // 디버그용 라인 렌더러 — 좌표축, 본 시각화, 라이트 위치 등 시각 디버깅.
    //
    // 책임:
    //   - 자체 RootSig (b0 CBV — view-projection 매트릭스만, VS 가시).
    //   - 자체 PSO (POSITION+COLOR 입력 레이아웃, LineList topology, depth-test OFF).
    //   - 좌표축 정점 6개 (3 라인 — X 빨강 / Y 초록 / Z 파랑) VertexBuffer.
    //   - N프레임 in-flight ConstantBuffer.
    //
    // 호출 패턴 (FrameRenderer 안):
    //   ① RTV/DSV 바인딩 + 메인 씬 draw 완료 후.
    //   ② debugRenderer->DrawAxes(list, frameIndex, viewProj, axisLength).
    //   ③ Barrier RT→PRESENT 등 후속.
    //
    // depth-test OFF — 좌표축이 X-Bot 메시 뒤에 있어도 가시. 디버그 우선.
    //
    // 단일 소유 (복사·이동 금지).
    class DebugRenderer final
    {
    public:
        // rtvFormat: SwapChain 백버퍼 포맷과 일치.
        // dsvFormat: depth-test OFF 라 사용 안 함. DXGI_FORMAT_D32_FLOAT 같은 메인 dsv 와 *같이*
        //   바인딩되어도 동작 — depth write/test 둘 다 OFF.
        DebugRenderer(Device& device, DXGI_FORMAT rtvFormat, DXGI_FORMAT dsvFormat);
        ~DebugRenderer();

        DebugRenderer(const DebugRenderer&)            = delete;
        DebugRenderer& operator=(const DebugRenderer&) = delete;
        DebugRenderer(DebugRenderer&&)                 = delete;
        DebugRenderer& operator=(DebugRenderer&&)      = delete;

        // 원점 기준 좌표축 그리기. world 매트릭스 미사용 — 원점 = (0,0,0).
        // axisLength: 캐릭터 크기 기준 (X Bot 은 ~170, length=100 권장).
        void DrawAxes(ID3D12GraphicsCommandList*       list,
                      engine::uint32                   frameIndex,
                      const DirectX::XMMATRIX&         viewProj,
                      float                            axisLength = 100.0f);

    private:
        static constexpr engine::uint32 kFrameCount = engine::render::SwapChain::kBackBufferCount;

        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;
        std::unique_ptr<VertexBuffer>               m_axesVB;
        float                                       m_currentAxisLength = -1.0f;  // VB 재업로드 트리거
        std::array<std::unique_ptr<ConstantBuffer>, kFrameCount> m_cbs;
    };
}
