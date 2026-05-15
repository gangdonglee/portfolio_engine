#pragma once

#include "core/Types.h"

#include <wrl/client.h>

#include <d3d12.h>  // D3D12_*_DESCRIPTOR_HANDLE

struct ID3D12DescriptorHeap;

namespace engine::render
{
    class Device;

    // SRV/CBV/UAV 용 shader-visible 디스크립터 힙.
    // RtvDescriptorHeap 과 달리 GPU 가 직접 읽으므로 SHADER_VISIBLE 플래그 필요.
    //
    // 책임:
    //   - 고정 capacity SRV 슬롯 — 순차 Allocate.
    //   - CPU/GPU 핸들 둘 다 반환 — CreateSRV(CPU) + SetGraphicsRootDescriptorTable(GPU).
    //
    // 본 단계는 SRV 만 사용. 향후 CBV/UAV 디스크립터 테이블이 필요해질 때 같은 힙 또는 별도 분리.
    //
    // 단일 소유 (복사·이동 금지).
    class SrvDescriptorHeap final
    {
    public:
        struct Handle
        {
            D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
            D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
        };

        SrvDescriptorHeap(Device& device, uint32 capacity);
        ~SrvDescriptorHeap();

        SrvDescriptorHeap(const SrvDescriptorHeap&)            = delete;
        SrvDescriptorHeap& operator=(const SrvDescriptorHeap&) = delete;
        SrvDescriptorHeap(SrvDescriptorHeap&&)                 = delete;
        SrvDescriptorHeap& operator=(SrvDescriptorHeap&&)      = delete;

        Handle Allocate();
        Handle GetHandle(uint32 index) const noexcept;

        uint32 Capacity() const noexcept { return m_capacity; }
        uint32 Count()    const noexcept { return m_count; }

        // raw COM 노출 — SetDescriptorHeaps 에 전달용.
        ID3D12DescriptorHeap* Native() const noexcept;

    private:
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_heap;
        D3D12_CPU_DESCRIPTOR_HANDLE                  m_cpuStart{};
        D3D12_GPU_DESCRIPTOR_HANDLE                  m_gpuStart{};
        uint32 m_descriptorSize = 0;
        uint32 m_capacity       = 0;
        uint32 m_count          = 0;
    };
}
