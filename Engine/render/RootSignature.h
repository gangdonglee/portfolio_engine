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

            // true 이면 b1 슬롯에 CBV root descriptor (Vertex 가시) 추가 — 본 팔레트용.
            // 자리 순서: [b0?] [b1?] [t0 table?] [t1?] [t2?] — 비어있는 슬롯은 건너뜀.
            bool cbvB1Vertex = false;

            // true 이면 t1 슬롯에 SRV root descriptor (PS 가시) 추가.
            // StructuredBuffer 등 매 프레임 갱신되는 가변 길이 데이터 — 디스크립터 힙 없이
            // GPU 가상주소 직접 바인딩 (SetGraphicsRootShaderResourceView). 라이트 배열 용도.
            bool srvT1Pixel = false;

            // true 이면 t2 슬롯에 SRV root descriptor (PS 가시) 추가. 두 번째 라이트 배열 등.
            bool srvT2Pixel = false;
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
