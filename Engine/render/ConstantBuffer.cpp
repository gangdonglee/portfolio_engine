#include "render/ConstantBuffer.h"

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

    namespace
    {
        // 256바이트 정렬 — D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT 와 동등.
        constexpr uint32 kCbvAlignment = 256;

        uint32 AlignUp(uint32 value, uint32 alignment) noexcept
        {
            return (value + alignment - 1) & ~(alignment - 1);
        }
    }

    ConstantBuffer::ConstantBuffer(Device& device, uint32 byteSize)
        : m_byteSize(byteSize)
    {
        if (byteSize == 0)
        {
            throw std::runtime_error("ConstantBuffer: byteSize must be > 0");
        }
        m_aligned = AlignUp(byteSize, kCbvAlignment);

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
        resDesc.Width              = m_aligned;
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
            "ID3D12Device::CreateCommittedResource(ConstantBuffer Upload)");

        // Map 유지 — Unmap 안 함. 매 프레임 memcpy 비용 최소.
        const D3D12_RANGE readRange{ 0, 0 };
        ThrowIfFailed(m_buffer->Map(0, &readRange, &m_mapped), "ID3D12Resource::Map(CB)");

        m_gpuAddress = m_buffer->GetGPUVirtualAddress();

        wchar_t line[160];
        std::swprintf(line, std::size(line),
                      L"[render] ConstantBuffer created (requested=%u, aligned=%u)\n",
                      m_byteSize, m_aligned);
        engine::core::LogInfo(line);
    }

    ConstantBuffer::~ConstantBuffer()
    {
        // ComPtr 소멸이 implicit Unmap. 명시 호출 불필요지만 안전 위해.
        if (m_buffer && m_mapped != nullptr)
        {
            m_buffer->Unmap(0, nullptr);
            m_mapped = nullptr;
        }
    }

    void ConstantBuffer::Update(const void* data, uint32 byteSize)
    {
        if (data == nullptr) return;
        if (byteSize > m_aligned)
        {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "ConstantBuffer::Update: byteSize %u 가 aligned %u 초과",
                          byteSize, m_aligned);
            throw std::runtime_error(buf);
        }
        std::memcpy(m_mapped, data, byteSize);
    }

    uint64 ConstantBuffer::GpuAddress() const noexcept
    {
        return m_gpuAddress;
    }

    uint32 ConstantBuffer::AlignedByteSize() const noexcept
    {
        return m_aligned;
    }
}
