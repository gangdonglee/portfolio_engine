#include "render/RtvDescriptorHeap.h"

#include "render/Device.h"

#include <Windows.h>
#include <cassert>
#include <cstdio>
#include <stdexcept>

namespace engine::render
{
    namespace
    {
        void ThrowIfFailed(HRESULT hr, const char* what)
        {
            if (FAILED(hr))
            {
                char buf[256];
                std::snprintf(buf, sizeof(buf),
                              "%s failed: HRESULT=0x%08lX",
                              what,
                              static_cast<unsigned long>(hr));
                throw std::runtime_error(buf);
            }
        }
    }

    RtvDescriptorHeap::RtvDescriptorHeap(Device& device, std::uint32_t capacity)
        : m_capacity(capacity)
    {
        if (capacity == 0)
        {
            throw std::runtime_error("RtvDescriptorHeap: capacity must be > 0");
        }

        ID3D12Device* d3dDevice = device.Native();

        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.NumDescriptors = capacity;
        desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE; // RTV 는 GPU 가시 불필요
        desc.NodeMask       = 0;

        ThrowIfFailed(
            d3dDevice->CreateDescriptorHeap(
                &desc,
                IID_PPV_ARGS(m_heap.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateDescriptorHeap(RTV)");

        m_cpuStart       = m_heap->GetCPUDescriptorHandleForHeapStart();
        m_descriptorSize = d3dDevice->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        wchar_t line[128];
        std::swprintf(line, std::size(line),
                      L"[render] RtvDescriptorHeap created (capacity=%u, slot=%u bytes)\n",
                      m_capacity,
                      m_descriptorSize);
        ::OutputDebugStringW(line);
    }

    // 소멸자: ComPtr 가 자동 Release. 디스크립터 힙은 별도 cleanup 불필요.
    RtvDescriptorHeap::~RtvDescriptorHeap() = default;

    D3D12_CPU_DESCRIPTOR_HANDLE RtvDescriptorHeap::Allocate()
    {
        if (m_count >= m_capacity)
        {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "RtvDescriptorHeap::Allocate: capacity %u 초과",
                          m_capacity);
            throw std::runtime_error(buf);
        }
        // SIZE_T 양쪽 캐스트 — uint32×uint32 의 32비트 곱셈 오버플로 가능성 차단
        // (RTV 힙은 수~수십 슬롯이라 실해 0이나, CBV/SRV 힙으로 패턴 재사용 시 안전 자산).
        D3D12_CPU_DESCRIPTOR_HANDLE handle = m_cpuStart;
        handle.ptr += static_cast<SIZE_T>(m_count) * static_cast<SIZE_T>(m_descriptorSize);
        ++m_count;
        return handle;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE RtvDescriptorHeap::CpuHandle(std::uint32_t index) const noexcept
    {
        // Debug 빌드 인덱스 검증 — Release 에선 no-op. silent OOB 가 GPU 검증 레이어에서야
        // 잡히는 사고 사전 차단.
        assert(index < m_capacity);
        D3D12_CPU_DESCRIPTOR_HANDLE handle = m_cpuStart;
        handle.ptr += static_cast<SIZE_T>(index) * static_cast<SIZE_T>(m_descriptorSize);
        return handle;
    }

    ID3D12DescriptorHeap* RtvDescriptorHeap::Native() const noexcept
    {
        return m_heap.Get();
    }
}
