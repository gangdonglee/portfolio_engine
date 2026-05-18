#include "render/StructuredBuffer.h"

#include "core/HrCheck.h"
#include "core/Logger.h"
#include "render/Device.h"

#include <Windows.h>
#include <d3d12.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <stdexcept>

namespace engine::render
{
    using engine::core::ThrowIfFailed;

    StructuredBuffer::StructuredBuffer(Device& device, uint32 elementCapacity, uint32 elementStride)
        : m_elementCapacity(elementCapacity)
        , m_elementStride(elementStride)
    {
        if (elementCapacity == 0 || elementStride == 0)
        {
            throw std::runtime_error("StructuredBuffer: capacity/stride must be > 0");
        }

        const uint32 totalBytes = elementCapacity * elementStride;

        ID3D12Device* d3dDevice = device.Native();

        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type                 = D3D12_HEAP_TYPE_UPLOAD;
        heapProps.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        heapProps.CreationNodeMask     = 1;
        heapProps.VisibleNodeMask      = 1;

        D3D12_RESOURCE_DESC resDesc{};
        resDesc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
        resDesc.Alignment          = 0;
        resDesc.Width              = totalBytes;
        resDesc.Height             = 1;
        resDesc.DepthOrArraySize   = 1;
        resDesc.MipLevels          = 1;
        resDesc.Format             = DXGI_FORMAT_UNKNOWN;
        resDesc.SampleDesc.Count   = 1;
        resDesc.SampleDesc.Quality = 0;
        resDesc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        resDesc.Flags              = D3D12_RESOURCE_FLAG_NONE;

        ThrowIfFailed(
            d3dDevice->CreateCommittedResource(
                &heapProps,
                D3D12_HEAP_FLAG_NONE,
                &resDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(m_buffer.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateCommittedResource(StructuredBuffer Upload)");

        const D3D12_RANGE readRange{ 0, 0 };
        ThrowIfFailed(m_buffer->Map(0, &readRange, &m_mapped), "ID3D12Resource::Map(SB)");

        m_gpuAddress = m_buffer->GetGPUVirtualAddress();

        wchar_t line[160];
        std::swprintf(line, std::size(line),
                      L"[render] StructuredBuffer created (capacity=%u, stride=%u, total=%u bytes)\n",
                      m_elementCapacity, m_elementStride, totalBytes);
        engine::core::LogInfo(line);
    }

    StructuredBuffer::~StructuredBuffer()
    {
        if (m_buffer && m_mapped != nullptr)
        {
            m_buffer->Unmap(0, nullptr);
            m_mapped = nullptr;
        }
    }

    void StructuredBuffer::UpdateRange(const void* elements, uint32 count)
    {
        if (count == 0) { return; }
        if (count > m_elementCapacity)
        {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "StructuredBuffer::UpdateRange: count %u 가 capacity %u 초과",
                          count, m_elementCapacity);
            throw std::runtime_error(buf);
        }
        if (elements == nullptr) { return; }
        std::memcpy(m_mapped, elements, static_cast<size_t>(count) * m_elementStride);
    }

    uint64 StructuredBuffer::GpuAddress() const noexcept
    {
        return m_gpuAddress;
    }

    uint32 StructuredBuffer::ElementCapacity() const noexcept
    {
        return m_elementCapacity;
    }

    uint32 StructuredBuffer::ElementStride() const noexcept
    {
        return m_elementStride;
    }
}
