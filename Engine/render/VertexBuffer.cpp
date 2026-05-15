#include "render/VertexBuffer.h"

#include "core/HrCheck.h"
#include "core/Logger.h"

#include "render/Device.h"

#include <Windows.h>
#include <d3d12.h>

#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace engine::render
{
    using engine::core::ThrowIfFailed;

    namespace
    {
    }

    VertexBuffer::VertexBuffer(Device& device,
                               const void*   data,
                               std::uint32_t byteSize,
                               std::uint32_t stride)
        : m_byteSize(byteSize), m_stride(stride)
    {
        if (data == nullptr || byteSize == 0 || stride == 0)
        {
            throw std::runtime_error("VertexBuffer: data/byteSize/stride must be non-zero/non-null");
        }
        if (byteSize % stride != 0)
        {
            throw std::runtime_error("VertexBuffer: byteSize 가 stride 의 배수가 아님");
        }

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
            device.Native()->CreateCommittedResource(
                &heapProps,
                D3D12_HEAP_FLAG_NONE,
                &resDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ,  // Upload heap 의 초기 상태
                nullptr,
                IID_PPV_ARGS(m_buffer.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateCommittedResource(VertexBuffer Upload)");

        // CPU 쓰기 → GPU 읽기.
        void* mapped = nullptr;
        const D3D12_RANGE readRange{ 0, 0 };  // CPU 는 읽지 않음 — 명시
        ThrowIfFailed(m_buffer->Map(0, &readRange, &mapped), "ID3D12Resource::Map(VB)");
        std::memcpy(mapped, data, byteSize);
        m_buffer->Unmap(0, nullptr);

        m_gpuAddress = m_buffer->GetGPUVirtualAddress();

        wchar_t line[200];
        std::swprintf(line, std::size(line),
                      L"[render] VertexBuffer created (size=%u bytes, stride=%u, count=%u)\n",
                      m_byteSize, m_stride, m_byteSize / m_stride);
        engine::core::LogInfo(line);
    }

    VertexBuffer::~VertexBuffer() = default;

    void VertexBuffer::Bind(ID3D12GraphicsCommandList* list, std::uint32_t slot) const
    {
        D3D12_VERTEX_BUFFER_VIEW view{};
        view.BufferLocation = m_gpuAddress;
        view.SizeInBytes    = m_byteSize;
        view.StrideInBytes  = m_stride;
        list->IASetVertexBuffers(slot, 1, &view);
    }

    std::uint32_t VertexBuffer::VertexCount() const noexcept
    {
        return m_byteSize / m_stride;
    }
}
