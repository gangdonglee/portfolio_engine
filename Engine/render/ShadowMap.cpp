#include "render/ShadowMap.h"

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

    ShadowMap::ShadowMap(Device& device, SrvDescriptorHeap& srvHeap, uint32 reservedSrvSlot, uint32 resolution)
        : m_resolution(resolution)
    {
        if (resolution == 0)
        {
            throw std::runtime_error("ShadowMap: resolution must be > 0");
        }

        ID3D12Device* d3dDevice = device.Native();

        // DSV 전용 힙(슬롯 1).
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.NumDescriptors = 1;
        heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        heapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        heapDesc.NodeMask       = 0;
        ThrowIfFailed(
            d3dDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(m_dsvHeap.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateDescriptorHeap(ShadowMap DSV)");
        m_dsvHandle = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();

        // R32_TYPELESS 깊이 텍스처 — DSV=D32_FLOAT, SRV=R32_FLOAT 둘 다 가능.
        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type                 = D3D12_HEAP_TYPE_DEFAULT;
        heapProps.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        heapProps.CreationNodeMask     = 1;
        heapProps.VisibleNodeMask      = 1;

        D3D12_RESOURCE_DESC resDesc{};
        resDesc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        resDesc.Alignment          = 0;
        resDesc.Width              = resolution;
        resDesc.Height             = resolution;
        resDesc.DepthOrArraySize   = 1;
        resDesc.MipLevels          = 1;
        resDesc.Format             = DXGI_FORMAT_R32_TYPELESS;
        resDesc.SampleDesc.Count   = 1;
        resDesc.SampleDesc.Quality = 0;
        resDesc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        resDesc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format               = DXGI_FORMAT_D32_FLOAT;
        clearValue.DepthStencil.Depth   = 1.0f;
        clearValue.DepthStencil.Stencil = 0;

        ThrowIfFailed(
            d3dDevice->CreateCommittedResource(
                &heapProps,
                D3D12_HEAP_FLAG_NONE,
                &resDesc,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,  // 초기 상태 (첫 프레임 barrier→DEPTH_WRITE)
                &clearValue,
                IID_PPV_ARGS(m_buffer.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateCommittedResource(ShadowMap)");

        // DSV (D32_FLOAT).
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format             = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension      = D3D12_DSV_DIMENSION_TEXTURE2D;
        dsvDesc.Flags              = D3D12_DSV_FLAG_NONE;
        dsvDesc.Texture2D.MipSlice = 0;
        d3dDevice->CreateDepthStencilView(m_buffer.Get(), &dsvDesc, m_dsvHandle);

        // SRV (R32_FLOAT) — 예약 슬롯에 고정 등록(씬 Reset 에도 살아남게). 디스크립터만 쓰고
        //   bump counter 는 건드리지 않음(GetHandle).
        if (reservedSrvSlot >= srvHeap.Capacity())
        {
            throw std::runtime_error("ShadowMap: reservedSrvSlot 이 srvHeap capacity 초과");
        }
        const SrvDescriptorHeap::Handle srv = srvHeap.GetHandle(reservedSrvSlot);
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format                  = DXGI_FORMAT_R32_FLOAT;
        srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels     = 1;
        d3dDevice->CreateShaderResourceView(m_buffer.Get(), &srvDesc, srv.cpu);
        m_srvGpu = srv.gpu;

        wchar_t line[160];
        std::swprintf(line, std::size(line),
                      L"[render] ShadowMap created (%ux%u, R32_TYPELESS)\n", resolution, resolution);
        engine::core::LogInfo(line);
    }

    ShadowMap::~ShadowMap() = default;

    ID3D12Resource* ShadowMap::Native() const noexcept { return m_buffer.Get(); }
}
