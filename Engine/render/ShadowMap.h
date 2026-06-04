#pragma once

#include "core/Types.h"

#include <wrl/client.h>

#include <d3d12.h>

struct ID3D12Resource;
struct ID3D12DescriptorHeap;

namespace engine::render
{
    class Device;
    class SrvDescriptorHeap;

    // 그림자맵 — 라이트 시점 깊이 텍스처. DSV(렌더 타깃) + SRV(메인 패스에서 샘플) 둘 다 보유.
    //
    //   리소스 포맷은 R32_TYPELESS (DSV=D32_FLOAT, SRV=R32_FLOAT) — 깊이 쓰기 + 셰이더 읽기 둘 다 가능.
    //   자체 DSV 힙(슬롯 1) 보유. SRV 는 srvHeap 의 *예약 슬롯*(reservedSrvSlot) 에 고정 등록 —
    //   씬 전환 시 srvHeap 이 Reset(count=0) 후 슬롯 0 부터 재할당되므로, 씬이 닿지 않는 끝쪽 슬롯에
    //   고정하면 디스크립터가 덮어써지지 않아 그대로 살아남는다(재등록 불필요).
    //   초기 상태: PIXEL_SHADER_RESOURCE — FrameRenderer 가 매 프레임 DEPTH_WRITE↔PSR barrier.
    //
    // 단일 소유 (복사·이동 금지).
    class ShadowMap final
    {
    public:
        ShadowMap(Device& device, SrvDescriptorHeap& srvHeap, uint32 reservedSrvSlot, uint32 resolution);
        ~ShadowMap();

        ShadowMap(const ShadowMap&)            = delete;
        ShadowMap& operator=(const ShadowMap&) = delete;
        ShadowMap(ShadowMap&&)                 = delete;
        ShadowMap& operator=(ShadowMap&&)      = delete;

        D3D12_CPU_DESCRIPTOR_HANDLE DsvHandle()  const noexcept { return m_dsvHandle; }
        D3D12_GPU_DESCRIPTOR_HANDLE SrvGpu()     const noexcept { return m_srvGpu; }
        ID3D12Resource*             Native()     const noexcept;
        uint32                      Resolution() const noexcept { return m_resolution; }

    private:
        Microsoft::WRL::ComPtr<ID3D12Resource>       m_buffer;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
        D3D12_CPU_DESCRIPTOR_HANDLE                  m_dsvHandle{};
        D3D12_GPU_DESCRIPTOR_HANDLE                  m_srvGpu{};
        uint32                                       m_resolution = 0;
    };
}
