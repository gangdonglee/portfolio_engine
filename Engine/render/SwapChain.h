#pragma once

#include <cstdint>
#include <wrl/client.h>

// 본 클래스는 D3D12_CPU_DESCRIPTOR_HANDLE 을 반환 타입으로 노출하므로
// d3d12.h 를 헤더에 포함 (RtvDescriptorHeap.h 와 같은 의도된 예외).
#include <d3d12.h>

struct IDXGISwapChain3;
struct ID3D12Resource;

namespace engine::platform { class Window; }

namespace engine::render
{
    class Device;
    class CommandQueue;
    class RtvDescriptorHeap;

    // DXGI Flip Model 스왑체인 + 백버퍼 RTV 등록을 RAII 로 캡슐화.
    //
    // 책임:
    //   - IDXGISwapChain3 생성 (Window HWND 와 결합, present queue 와 결합).
    //     * IDXGISwapChain3 는 Windows 10+ 인터페이스. 본 프로젝트 SDK(10.0.26100) 안에 포함.
    //   - 백버퍼 ID3D12Resource 2개 보유 (FLIP_DISCARD, kBackBufferCount = 2).
    //   - 각 백버퍼에 RTV 생성 후 RtvDescriptorHeap 에 등록.
    //   - 현재 백버퍼 인덱스 추적 + Present 후 회전.
    //
    // 단일 소유 (복사·이동 금지).
    //
    // Window 의 HWND 는 Window 의 friend 선언으로 NativeHwnd() 호출.
    //
    // TODO(추후):
    //   - DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING + DXGI_PRESENT_ALLOW_TEARING — V-Sync OFF 일관성 강화.
    //     단, IDXGIFactory5::CheckFeatureSupport(VARIABLE_REFRESH_RATE) 로 지원 여부 사전 확인 필요.
    //   - 리사이즈 처리 (WM_SIZE → SwapChain::Resize) — 백버퍼 재생성 + RTV 재등록.
    class SwapChain final
    {
    public:
        SwapChain(
            Device&                    device,
            CommandQueue&              presentQueue,
            engine::platform::Window&  window,
            RtvDescriptorHeap&         rtvHeap);
        ~SwapChain();

        SwapChain(const SwapChain&)            = delete;
        SwapChain& operator=(const SwapChain&) = delete;
        SwapChain(SwapChain&&)                 = delete;
        SwapChain& operator=(SwapChain&&)      = delete;

        // 현재 백버퍼를 Present. 내부 인덱스를 IDXGISwapChain3::GetCurrentBackBufferIndex 로 갱신.
        void Present();

        std::uint32_t CurrentBackBufferIndex() const noexcept { return m_currentIndex; }
        ID3D12Resource* CurrentBackBuffer() const noexcept;
        D3D12_CPU_DESCRIPTOR_HANDLE CurrentRtv() const noexcept;

        static constexpr std::uint32_t kBackBufferCount = 2;

        IDXGISwapChain3* Native() const noexcept;

    private:
        Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapChain;
        Microsoft::WRL::ComPtr<ID3D12Resource>  m_backBuffers[kBackBufferCount];
        D3D12_CPU_DESCRIPTOR_HANDLE             m_rtvHandles[kBackBufferCount]{};
        std::uint32_t                           m_currentIndex = 0;
    };
}
