#include "render/SrvDescriptorHeap.h"

#include "core/HrCheck.h"
#include "core/Logger.h"
#include "render/Device.h"

#include <Windows.h>
#include <cassert>
#include <cstdio>
#include <cwchar>
#include <iterator>
#include <stdexcept>

namespace engine::render
{
    using engine::core::ThrowIfFailed;

    SrvDescriptorHeap::SrvDescriptorHeap(Device& device, uint32 capacity)
        : m_capacity(capacity)
    {
        if (capacity == 0)
        {
            throw std::runtime_error("SrvDescriptorHeap: capacity must be > 0");
        }

        ID3D12Device* d3dDevice = device.Native();

        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.NumDescriptors = capacity;
        desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        desc.NodeMask       = 0;

        ThrowIfFailed(
            d3dDevice->CreateDescriptorHeap(
                &desc,
                IID_PPV_ARGS(m_heap.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateDescriptorHeap(SRV, SHADER_VISIBLE)");

        m_cpuStart       = m_heap->GetCPUDescriptorHandleForHeapStart();
        m_gpuStart       = m_heap->GetGPUDescriptorHandleForHeapStart();
        m_descriptorSize = d3dDevice->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        wchar_t line[160];
        std::swprintf(line, std::size(line),
                      L"[render] SrvDescriptorHeap created (capacity=%u, slot=%u bytes, shader-visible)\n",
                      m_capacity, m_descriptorSize);
        engine::core::LogInfo(line);
    }

    SrvDescriptorHeap::~SrvDescriptorHeap() = default;

    SrvDescriptorHeap::Handle SrvDescriptorHeap::Allocate()
    {
        if (m_count >= m_capacity)
        {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "SrvDescriptorHeap::Allocate: capacity %u 초과",
                          m_capacity);
            throw std::runtime_error(buf);
        }
        Handle h = GetHandle(m_count);
        ++m_count;
        return h;
    }

    SrvDescriptorHeap::Handle SrvDescriptorHeap::GetHandle(uint32 index) const noexcept
    {
        assert(index < m_capacity);
        const SIZE_T offset = static_cast<SIZE_T>(index) * static_cast<SIZE_T>(m_descriptorSize);

        Handle h{};
        h.cpu.ptr = m_cpuStart.ptr + offset;
        h.gpu.ptr = m_gpuStart.ptr + offset;
        return h;
    }

    ID3D12DescriptorHeap* SrvDescriptorHeap::Native() const noexcept
    {
        return m_heap.Get();
    }
}
