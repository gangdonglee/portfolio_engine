#include "render/Texture.h"

#include "core/HrCheck.h"
#include "core/Logger.h"
#include "render/CommandList.h"
#include "render/CommandQueue.h"
#include "render/Device.h"
#include "render/SrvDescriptorHeap.h"

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

    Texture::Texture(Device&       device,
                     CommandQueue& queue,
                     CommandList&  list,
                     const void*   rgba8Pixels,
                     uint32        width,
                     uint32        height)
        : m_width(width), m_height(height)
    {
        if (rgba8Pixels == nullptr || width == 0 || height == 0)
        {
            throw std::runtime_error("Texture: pixels/width/height must be non-null/non-zero");
        }

        ID3D12Device* d3dDevice = device.Native();

        // === 1. Default heap 텍스처 ===
        D3D12_HEAP_PROPERTIES defaultHeap{};
        defaultHeap.Type                 = D3D12_HEAP_TYPE_DEFAULT;
        defaultHeap.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        defaultHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        defaultHeap.CreationNodeMask     = 1;
        defaultHeap.VisibleNodeMask      = 1;

        D3D12_RESOURCE_DESC texDesc{};
        texDesc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Alignment          = 0;
        texDesc.Width              = width;
        texDesc.Height             = height;
        texDesc.DepthOrArraySize   = 1;
        texDesc.MipLevels          = 1;
        texDesc.Format             = DXGI_FORMAT_R8G8B8A8_UNORM;
        texDesc.SampleDesc.Count   = 1;
        texDesc.SampleDesc.Quality = 0;
        texDesc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texDesc.Flags              = D3D12_RESOURCE_FLAG_NONE;

        ThrowIfFailed(
            d3dDevice->CreateCommittedResource(
                &defaultHeap, D3D12_HEAP_FLAG_NONE,
                &texDesc, D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(m_buffer.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateCommittedResource(Texture Default)");

        // === 2. Upload heap 스테이징 + GetCopyableFootprints ===
        UINT64                              totalBytes  = 0;
        UINT                                numRows     = 0;
        UINT64                              rowSizeBytes = 0;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT  footprint{};
        d3dDevice->GetCopyableFootprints(
            &texDesc, 0, 1, 0,
            &footprint, &numRows, &rowSizeBytes, &totalBytes);

        D3D12_HEAP_PROPERTIES uploadHeap{};
        uploadHeap.Type                 = D3D12_HEAP_TYPE_UPLOAD;
        uploadHeap.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        uploadHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        uploadHeap.CreationNodeMask     = 1;
        uploadHeap.VisibleNodeMask      = 1;

        D3D12_RESOURCE_DESC uploadDesc{};
        uploadDesc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
        uploadDesc.Alignment          = 0;
        uploadDesc.Width              = totalBytes;
        uploadDesc.Height             = 1;
        uploadDesc.DepthOrArraySize   = 1;
        uploadDesc.MipLevels          = 1;
        uploadDesc.Format             = DXGI_FORMAT_UNKNOWN;
        uploadDesc.SampleDesc.Count   = 1;
        uploadDesc.SampleDesc.Quality = 0;
        uploadDesc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        uploadDesc.Flags              = D3D12_RESOURCE_FLAG_NONE;

        Microsoft::WRL::ComPtr<ID3D12Resource> staging;
        ThrowIfFailed(
            d3dDevice->CreateCommittedResource(
                &uploadHeap, D3D12_HEAP_FLAG_NONE,
                &uploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(staging.GetAddressOf())),
            "ID3D12Device::CreateCommittedResource(Texture Upload staging)");

        // === 3. 픽셀 데이터 → 스테이징 ===
        BYTE* mapped = nullptr;
        const D3D12_RANGE readRange{ 0, 0 };
        ThrowIfFailed(staging->Map(0, &readRange, reinterpret_cast<void**>(&mapped)),
                      "ID3D12Resource::Map(Texture staging)");

        BYTE* dest = mapped + footprint.Offset;
        const BYTE* src = static_cast<const BYTE*>(rgba8Pixels);
        const SIZE_T srcRowPitch = static_cast<SIZE_T>(width) * 4;  // RGBA8 = 4 bytes
        const SIZE_T destRowPitch = footprint.Footprint.RowPitch;
        for (UINT row = 0; row < numRows; ++row)
        {
            std::memcpy(dest + row * destRowPitch,
                        src  + row * srcRowPitch,
                        srcRowPitch);
        }
        staging->Unmap(0, nullptr);

        // === 4. CopyTextureRegion + barrier (record) ===
        // 직전 GPU 작업 완료 후 list 재사용 — 안전한 동기화 가정.
        queue.FlushGpu();
        list.Reset();
        ID3D12GraphicsCommandList* cmd = list.Native();

        D3D12_TEXTURE_COPY_LOCATION dstLoc{};
        dstLoc.pResource        = m_buffer.Get();
        dstLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLoc.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION srcLoc{};
        srcLoc.pResource       = staging.Get();
        srcLoc.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLoc.PlacedFootprint = footprint;

        cmd->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

        D3D12_RESOURCE_BARRIER toShader{};
        toShader.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toShader.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        toShader.Transition.pResource   = m_buffer.Get();
        toShader.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        toShader.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        toShader.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        cmd->ResourceBarrier(1, &toShader);

        // === 5. Execute + Flush (스테이징 ComPtr 살아있는 동안) ===
        list.Close();
        queue.Execute(list);
        queue.FlushGpu();  // 업로드 완료 — staging 안전 해제

        wchar_t line[160];
        std::swprintf(line, std::size(line),
                      L"[render] Texture created (%ux%u, RGBA8, %llu bytes upload)\n",
                      width, height, static_cast<unsigned long long>(totalBytes));
        engine::core::LogInfo(line);
    }

    Texture::~Texture() = default;

    void Texture::CreateSrv(Device& device, SrvDescriptorHeap& heap)
    {
        SrvDescriptorHeap::Handle slot = heap.Allocate();

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MostDetailedMip     = 0;
        srvDesc.Texture2D.MipLevels           = 1;
        srvDesc.Texture2D.PlaneSlice          = 0;
        srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

        device.Native()->CreateShaderResourceView(m_buffer.Get(), &srvDesc, slot.cpu);
        m_srvGpu = slot.gpu;
    }

    ID3D12Resource* Texture::Native() const noexcept
    {
        return m_buffer.Get();
    }
}
