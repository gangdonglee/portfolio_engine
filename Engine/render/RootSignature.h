#pragma once

#include <wrl/client.h>

struct ID3D12RootSignature;

namespace engine::render
{
    class Device;

    // D3D12 Root Signature 캡슐화.
    //
    // 현 단계 지원:
    //   - 비어있는 RS (Desc 디폴트)
    //   - b0 CBV root descriptor (Vertex 단계 가시) — Phase 2 의 MVP 상수 버퍼 용도
    //
    // 향후 단계: SRV/UAV/디스크립터 테이블/Static Sampler, 별도 builder 클래스.
    //
    // 단일 소유 (복사·이동 금지).
    class RootSignature final
    {
    public:
        struct Desc
        {
            // b0 슬롯의 CBV root descriptor 가시성.
            //   None: 비어있는 RS.
            //   Vertex: VS 만 접근.
            //   All: VS + PS 모두 접근 (Phong 조명처럼 같은 cbuffer 를 양 단계가 쓰는 경우).
            enum class CbvB0 { None, Vertex, All };
            CbvB0 cbvAtB0 = CbvB0::None;

            // true 이면 t0 슬롯에 SRV 디스크립터 테이블 (PS 가시) + s0 정적 샘플러 (linear/wrap) 추가.
            bool srvT0Pixel = false;
        };

        // ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT 플래그는 항상 켜짐.
        explicit RootSignature(Device& device, const Desc& desc = {});
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
