#include "render/DepthStencilBuffer.h"

#include "core/HrCheck.h"
#include "core/Logger.h"
#include "render/Device.h"

#include <Windows.h>
#include <d3d12.h>

#include <cstdio>
#include <cwchar>
#include <iterator>
#include <stdexcept>

namespace engine::render
{
    using engine::core::ThrowIfFailed;

    DepthStencilBuffer::DepthStencilBuffer(Device& device,
                                           std::uint32_t width,
                                           std::uint32_t height,
                                           DXGI_FORMAT   format)
        : m_format(format)
    {
        if (width == 0 || height == 0)
        {
            throw std::runtime_error("DepthStencilBuffer: width/height must be > 0");
        }

        ID3D12Device* d3dDevice = device.Native();

        // 1) Default heap 에 깊이 텍스처 리소스 생성.
        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type                 = D3D12_HEAP_TYPE_DEFAULT;
        heapProps.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        heapProps.CreationNodeMask     = 1;
        heapProps.VisibleNodeMask      = 1;

        D3D12_RESOURCE_DESC resDesc{};
        resDesc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        resDesc.Alignment          = 0;
        resDesc.Width              = width;
        resDesc.Height             = height;
        resDesc.DepthOrArraySize   = 1;
        resDesc.MipLevels          = 1;
        resDesc.Format             = format;
        resDesc.SampleDesc.Count   = 1;
        resDesc.SampleDesc.Quality = 0;
        resDesc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        resDesc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format               = format;
        clearValue.DepthStencil.Depth   = 1.0f;
        clearValue.DepthStencil.Stencil = 0;

        ThrowIfFailed(
            d3dDevice->CreateCommittedResource(
                &heapProps,
                D3D12_HEAP_FLAG_NONE,
                &resDesc,
                D3D12_RESOURCE_STATE_DEPTH_WRITE,  // 초기 상태 — barrier 없이 첫 프레임부터 사용
                &clearValue,
                IID_PPV_ARGS(m_buffer.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateCommittedResource(DepthStencil)");

        // 2) DSV 전용 디스크립터 힙 (슬롯 1).
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.NumDescriptors = 1;
        heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        heapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;  // DSV 는 GPU 가시 불필요
        heapDesc.NodeMask       = 0;

        ThrowIfFailed(
            d3dDevice->CreateDescriptorHeap(
                &heapDesc,
                IID_PPV_ARGS(m_dsvHeap.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateDescriptorHeap(DSV)");

        m_dsvHandle = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();

        // 3) DSV 생성. 기본 ViewDesc (포맷·차원 자동).
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format             = format;
        dsvDesc.ViewDimension      = D3D12_DSV_DIMENSION_TEXTURE2D;
        dsvDesc.Flags              = D3D12_DSV_FLAG_NONE;
        dsvDesc.Texture2D.MipSlice = 0;

        d3dDevice->CreateDepthStencilView(m_buffer.Get(), &dsvDesc, m_dsvHandle);

        wchar_t line[160];
        std::swprintf(line, std::size(line),
                      L"[render] DepthStencilBuffer created (%ux%u, format=0x%X)\n",
                      width, height, static_cast<unsigned int>(format));
        engine::core::LogInfo(line);
    }

    DepthStencilBuffer::~DepthStencilBuffer() = default;

    D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilBuffer::DsvHandle() const noexcept
    {
        return m_dsvHandle;
    }

    ID3D12Resource* DepthStencilBuffer::Native() const noexcept
    {
        return m_buffer.Get();
    }

    DXGI_FORMAT DepthStencilBuffer::Format() const noexcept
    {
        return m_format;
    }
}
