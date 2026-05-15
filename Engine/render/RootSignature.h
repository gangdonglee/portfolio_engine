#pragma once

#include <wrl/client.h>

struct ID3D12RootSignature;

namespace engine::render
{
    class Device;

    // D3D12 Root Signature 캡슐화.
    //
    // Phase 1E-2 단계는 **비어있는 루트 시그니처** (인자 0개). HelloTriangle 은 정점 입력만 받고
    // 상수 버퍼/디스크립터 테이블/Sampler 등이 모두 없음.
    //
    // 향후 단계:
    //   - 매개변수화: 루트 파라미터(CBV/SRV/UAV/디스크립터 테이블/상수) 추가.
    //   - 빌더/디스크립션 패턴 또는 별도 builder 클래스.
    //
    // 단일 소유 (복사·이동 금지).
    class RootSignature final
    {
    public:
        // 비어있는 루트 시그니처 생성. ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT 플래그 켜짐.
        explicit RootSignature(Device& device);
        ~RootSignature();

        RootSignature(const RootSignature&)            = delete;
        RootSignature& operator=(const RootSignature&) = delete;
        RootSignature(RootSignature&&)                 = delete;
        RootSignature& operator=(RootSignature&&)      = delete;

        ID3D12RootSignature* Native() const noexcept;

    private:
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    };
}
