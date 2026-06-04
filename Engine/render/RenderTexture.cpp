#include "render/RenderTexture.h"

#include "core/HrCheck.h"
#include "core/Logger.h"
#include "render/Device.h"
#include "render/SrvDescriptorHeap.h"

#include <Windows.h>
#include <d3d12.h>

#include <cwchar>
#include <iterator>
#include <stdexcept>

namespace engine::render
{
    using engine::core::ThrowIfFailed;

    RenderTexture::RenderTexture(Device& device, SrvDescriptorHeap& srvHeap, uint32 reservedSrvSlot,
                                 uint32 width, uint32 height, DXGI_FORMAT format)
        : m_format(format)
    {
        if (width == 0 || height == 0) { throw std::runtime_error("RenderTexture: 0 크기"); }
        if (reservedSrvSlot >= srvHeap.Capacity())
        {
            throw std::runtime_error("RenderTexture: reservedSrvSlot 이 srvHeap capacity 초과");
        }

        ID3D12Device* d3dDevice = device.Native();

        // RTV 전용 힙(슬롯 1).
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.NumDescriptors = 1;
        heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        ThrowIfFailed(
            d3dDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(m_rtvHeap.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateDescriptorHeap(RenderTexture RTV)");
        m_rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();

        const SrvDescriptorHeap::Handle srv = srvHeap.GetHandle(reservedSrvSlot);
        m_srvCpu = srv.cpu;
        m_srvGpu = srv.gpu;

        CreateResourceAndViews(device, width, height);

        wchar_t line[160];
        std::swprintf(line, std::size(line),
                      L"[render] RenderTexture created (%ux%u, fmt=0x%X)\n",
                      width, height, static_cast<unsigned int>(format));
        engine::core::LogInfo(line);
    }

    void RenderTexture::Resize(Device& device, uint32 width, uint32 height)
    {
        if (width == 0 || height == 0) { throw std::runtime_error("RenderTexture::Resize: 0 크기"); }
        CreateResourceAndViews(device, width, height);
    }

    void RenderTexture::CreateResourceAndViews(Device& device, uint32 width, uint32 height)
    {
        m_width  = width;
        m_height = height;
        ID3D12Device* d3dDevice = device.Native();

        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type             = D3D12_HEAP_TYPE_DEFAULT;
        heapProps.CreationNodeMask = 1;
        heapProps.VisibleNodeMask  = 1;

        D3D12_RESOURCE_DESC resDesc{};
        resDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        resDesc.Width            = width;
        resDesc.Height           = height;
        resDesc.DepthOrArraySize = 1;
        resDesc.MipLevels        = 1;
        resDesc.Format           = m_format;
        resDesc.SampleDesc.Count = 1;
        resDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        resDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE clear{};
        clear.Format   = m_format;
        clear.Color[0] = 0.0f; clear.Color[1] = 0.0f; clear.Color[2] = 0.0f; clear.Color[3] = 1.0f;

        ThrowIfFailed(
            d3dDevice->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
                IID_PPV_ARGS(m_buffer.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateCommittedResource(RenderTexture)");

        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.Format        = m_format;
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        d3dDevice->CreateRenderTargetView(m_buffer.Get(), &rtvDesc, m_rtvHandle);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format                  = m_format;
        srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels     = 1;
        d3dDevice->CreateShaderResourceView(m_buffer.Get(), &srvDesc, m_srvCpu);
    }

    RenderTexture::~RenderTexture() = default;

    ID3D12Resource* RenderTexture::Native() const noexcept { return m_buffer.Get(); }
}
