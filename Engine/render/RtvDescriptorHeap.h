#pragma once

#include <cstdint>
#include <wrl/client.h>

// D3D12_CPU_DESCRIPTOR_HANDLE 는 본 클래스의 핵심 반환 타입이라 헤더에 노출.
// 본 클래스를 사용하는 코드는 어차피 DX12 API 와 직접 거래하므로 노출이 정당화된다.
// (다른 헤더의 전방선언 원칙은 유지 — 이 파일은 의도된 예외.)
#include <d3d12.h>

struct ID3D12DescriptorHeap;

namespace engine::render
{
    class Device;

    // RTV(Render Target View) 전용 디스크립터 힙을 RAII 로 캡슐화.
    //
    // 역할:
    //   - 고정 용량(capacity) 만큼 RTV 슬롯을 미리 할당.
    //   - shader-visible 아님 (RTV 는 GPU 가 직접 읽지 않음).
    //   - 순차 Allocate 또는 인덱스 기반 CpuHandle 접근.
    //
    // Phase 1D-2 단계 용도: SwapChain 백버퍼 RTV (capacity = SWAP_CHAIN_BUFFER_COUNT).
    // 향후 단계: MRT G-Buffer, Shadow Map RT 등에 추가 디스크립터 슬롯 사용.
    //
    // 단일 소유 (복사·이동 금지).
    class RtvDescriptorHeap final
    {
    public:
        RtvDescriptorHeap(Device& device, std::uint32_t capacity);
        ~RtvDescriptorHeap();

        RtvDescriptorHeap(const RtvDescriptorHeap&)            = delete;
        RtvDescriptorHeap& operator=(const RtvDescriptorHeap&) = delete;
        RtvDescriptorHeap(RtvDescriptorHeap&&)                 = delete;
        RtvDescriptorHeap& operator=(RtvDescriptorHeap&&)      = delete;

        // 다음 빈 슬롯을 잡고 그 CPU 핸들을 반환. 카운터 증가.
        // 용량 초과 시 std::runtime_error.
        D3D12_CPU_DESCRIPTOR_HANDLE Allocate();

        // 미리 할당된 슬롯의 핸들. 인덱스가 capacity 미만일 책임은 호출자.
        D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle(std::uint32_t index) const noexcept;

        std::uint32_t Capacity() const noexcept { return m_capacity; }
        std::uint32_t Count()    const noexcept { return m_count; }

        // raw COM 노출 — engine::render 내부 사용 전용.
        ID3D12DescriptorHeap* Native() const noexcept;

    private:
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_heap;
        D3D12_CPU_DESCRIPTOR_HANDLE m_cpuStart{};
        std::uint32_t m_descriptorSize = 0;
        std::uint32_t m_capacity       = 0;
        std::uint32_t m_count          = 0;
    };
}
