#include "render/IndexBuffer.h"

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
        uint32 BytesPerIndex(DXGI_FORMAT format)
        {
            switch (format)
            {
                case DXGI_FORMAT_R16_UINT: return 2;
                case DXGI_FORMAT_R32_UINT: return 4;
                default:
                    throw std::runtime_error("IndexBuffer: format 은 R16_UINT 또는 R32_UINT 만 허용");
            }
        }
    }

    IndexBuffer::IndexBuffer(Device&     device,
                             const void* data,
                             uint32      byteSize,
                             DXGI_FORMAT format)
        : m_byteSize(byteSize), m_format(format)
    {
        if (data == nullptr || byteSize == 0)
        {
            throw std::runtime_error("IndexBuffer: data/byteSize must be non-null/non-zero");
        }
        const uint32 stride = BytesPerIndex(format);
        if (byteSize % stride != 0)
        {
            throw std::runtime_error("IndexBuffer: byteSize 가 format 의 stride 의 배수가 아님");
        }

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
        resDesc.Width              = byteSize;
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
            "ID3D12Device::CreateCommittedResource(IndexBuffer Upload)");

        void* mapped = nullptr;
        const D3D12_RANGE readRange{ 0, 0 };
        ThrowIfFailed(m_buffer->Map(0, &readRange, &mapped), "ID3D12Resource::Map(IB)");
        std::memcpy(mapped, data, byteSize);
        m_buffer->Unmap(0, nullptr);

        m_gpuAddress = m_buffer->GetGPUVirtualAddress();

        wchar_t line[160];
        std::swprintf(line, std::size(line),
                      L"[render] IndexBuffer created (size=%u bytes, format=0x%X, count=%u)\n",
                      m_byteSize, static_cast<unsigned int>(m_format), m_byteSize / stride);
        engine::core::LogInfo(line);
    }

    IndexBuffer::~IndexBuffer() = default;

    void IndexBuffer::Bind(ID3D12GraphicsCommandList* list) const
    {
        D3D12_INDEX_BUFFER_VIEW view{};
        view.BufferLocation = m_gpuAddress;
        view.SizeInBytes    = m_byteSize;
        view.Format         = m_format;
        list->IASetIndexBuffer(&view);
    }

    uint32 IndexBuffer::IndexCount() const noexcept
    {
        return m_byteSize / BytesPerIndex(m_format);
    }
}
