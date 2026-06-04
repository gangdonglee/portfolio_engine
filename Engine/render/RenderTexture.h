#pragma once

#include "core/Types.h"

#include <wrl/client.h>

#include <d3d12.h>
#include <dxgiformat.h>

struct ID3D12Resource;
struct ID3D12DescriptorHeap;

namespace engine::render
{
    class Device;
    class SrvDescriptorHeap;

    // 오프스크린 컬러 렌더 타깃 — RTV(렌더) + SRV(샘플) 둘 다. HDR 파이프라인/포스트프로세싱용.
    //
    //   자체 RTV 힙(슬롯 1) 보유. SRV 는 srvHeap 의 *예약 슬롯* 에 고정(씬 Reset 에도 생존 — ShadowMap 과
    //   동일 전략). 초기 상태 PIXEL_SHADER_RESOURCE — FrameRenderer 가 RENDER_TARGET↔PSR barrier.
    //   Resize 로 크기 재생성(같은 RTV/SRV 슬롯에 view 재등록).
    //
    // 단일 소유 (복사·이동 금지).
    class RenderTexture final
    {
    public:
        RenderTexture(Device& device, SrvDescriptorHeap& srvHeap, uint32 reservedSrvSlot,
                      uint32 width, uint32 height, DXGI_FORMAT format);
        ~RenderTexture();

        RenderTexture(const RenderTexture&)            = delete;
        RenderTexture& operator=(const RenderTexture&) = delete;
        RenderTexture(RenderTexture&&)                 = delete;
        RenderTexture& operator=(RenderTexture&&)      = delete;

        void Resize(Device& device, uint32 width, uint32 height);

        D3D12_CPU_DESCRIPTOR_HANDLE Rtv()    const noexcept { return m_rtvHandle; }
        D3D12_GPU_DESCRIPTOR_HANDLE SrvGpu() const noexcept { return m_srvGpu; }
        ID3D12Resource*             Native() const noexcept;
        uint32                      Width()  const noexcept { return m_width; }
        uint32                      Height() const noexcept { return m_height; }
        DXGI_FORMAT                 Format() const noexcept { return m_format; }

    private:
        void CreateResourceAndViews(Device& device, uint32 width, uint32 height);

        Microsoft::WRL::ComPtr<ID3D12Resource>       m_buffer;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
        D3D12_CPU_DESCRIPTOR_HANDLE                  m_rtvHandle{};
        D3D12_CPU_DESCRIPTOR_HANDLE                  m_srvCpu{};   // resize 시 view 재등록용
        D3D12_GPU_DESCRIPTOR_HANDLE                  m_srvGpu{};
        uint32      m_width  = 0;
        uint32      m_height = 0;
        DXGI_FORMAT m_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    };
}
