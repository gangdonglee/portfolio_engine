#pragma once

#include <cstdint>
#include <wrl/client.h>

#include <d3d12.h>            // D3D12_CPU_DESCRIPTOR_HANDLE
#include <dxgiformat.h>       // DXGI_FORMAT

struct ID3D12Resource;
struct ID3D12DescriptorHeap;

namespace engine::render
{
    class Device;

    // 깊이/스텐실 버퍼 + 전용 DSV 디스크립터 힙(슬롯 1) RAII 묶음.
    //
    // 현 단계 단순화:
    //   - 내부에 자체 DSV 힙(capacity=1) 보유 — 첫 단계 단일 깊이 버퍼만 필요.
    //   - 향후 다중 DSV / 그림자맵 시점에 외부 DsvDescriptorHeap 클래스로 분리.
    //
    // 디폴트 포맷: DXGI_FORMAT_D32_FLOAT (32비트 float depth, 스텐실 없음).
    // 초기 상태: D3D12_RESOURCE_STATE_DEPTH_WRITE — 별도 barrier 없이 첫 프레임부터 사용 가능.
    //
    // 단일 소유 (복사·이동 금지).
    class DepthStencilBuffer final
    {
    public:
        DepthStencilBuffer(Device& device,
                           std::uint32_t width,
                           std::uint32_t height,
                           DXGI_FORMAT   format = DXGI_FORMAT_D32_FLOAT);
        ~DepthStencilBuffer();

        DepthStencilBuffer(const DepthStencilBuffer&)            = delete;
        DepthStencilBuffer& operator=(const DepthStencilBuffer&) = delete;
        DepthStencilBuffer(DepthStencilBuffer&&)                 = delete;
        DepthStencilBuffer& operator=(DepthStencilBuffer&&)      = delete;

        // 깊이 텍스처를 새 크기로 재생성. DSV 힙은 그대로 두고 같은 핸들에 view 만 재등록.
        // GPU 가 이 버퍼를 참조 중이 아니어야 함 — 호출자 책임 (FlushGpu 선행).
        void Resize(Device& device, std::uint32_t width, std::uint32_t height);

        D3D12_CPU_DESCRIPTOR_HANDLE DsvHandle() const noexcept;
        ID3D12Resource*             Native()    const noexcept;
        DXGI_FORMAT                 Format()    const noexcept;

    private:
        // 깊이 텍스처 + DSV 생성 또는 재생성. DSV 힙은 이미 존재한다고 가정.
        void CreateBufferAndView(Device& device, std::uint32_t width, std::uint32_t height);

        Microsoft::WRL::ComPtr<ID3D12Resource>       m_buffer;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
        D3D12_CPU_DESCRIPTOR_HANDLE                  m_dsvHandle{};
        DXGI_FORMAT                                  m_format = DXGI_FORMAT_D32_FLOAT;
    };
}
