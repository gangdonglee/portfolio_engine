#pragma once

#include "core/Types.h"

#include <wrl/client.h>

struct ID3D12Resource;

namespace engine::render
{
    class Device;

    // GPU 가 t-register SRV 로 읽는 동적 배열. CPU 가 매 프레임 element 수와 데이터를 갱신.
    //
    // ConstantBuffer 와의 차이:
    //   - cbuffer 는 256바이트 정렬, 슬롯당 b#. 일반적으로 작은 고정 구조체.
    //   - StructuredBuffer 는 element stride 기반. HLSL StructuredBuffer<T> 로 읽음. t#.
    //     캡 없이 갯수 가변(elementCapacity 만 사전 정의).
    //
    // 동작:
    //   - 생성 시 elementCapacity * elementStride 크기 Upload heap 리소스.
    //   - Map 을 유지.
    //   - UpdateRange(data, count) 로 N개 element 복사.
    //   - SRV root descriptor 로 바인딩 — SetGraphicsRootShaderResourceView(slot, GpuAddress()).
    //     (descriptor table 미사용 — 디스크립터 힙 자원 절약.)
    //
    // 한계:
    //   - 단일 슬롯 (N프레임 in-flight 시 프레임당 별도 인스턴스 권장).
    //   - elementCapacity 초과 시 throw.
    //
    // 단일 소유 (복사·이동 금지).
    class StructuredBuffer final
    {
    public:
        StructuredBuffer(Device& device, uint32 elementCapacity, uint32 elementStride);
        ~StructuredBuffer();

        StructuredBuffer(const StructuredBuffer&)            = delete;
        StructuredBuffer& operator=(const StructuredBuffer&) = delete;
        StructuredBuffer(StructuredBuffer&&)                 = delete;
        StructuredBuffer& operator=(StructuredBuffer&&)      = delete;

        // count 개 element 를 매핑된 GPU 메모리에 복사. count 가 capacity 초과면 throw.
        // count == 0 도 유효 — 빈 버퍼. (셰이더 측 lightCount==0 분기에서 루프 skip)
        void UpdateRange(const void* elements, uint32 count);

        // GPU 가상 주소 — SetGraphicsRootShaderResourceView 인자.
        uint64 GpuAddress() const noexcept;

        uint32 ElementCapacity() const noexcept;
        uint32 ElementStride()   const noexcept;

    private:
        Microsoft::WRL::ComPtr<ID3D12Resource> m_buffer;
        void*  m_mapped         = nullptr;
        uint64 m_gpuAddress     = 0;
        uint32 m_elementCapacity = 0;
        uint32 m_elementStride   = 0;
    };
}
