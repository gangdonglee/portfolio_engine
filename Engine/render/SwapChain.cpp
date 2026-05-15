#include "render/SwapChain.h"

#include "core/HrCheck.h"
#include "core/Logger.h"

#include "platform/Window.h"
#include "render/CommandQueue.h"
#include "render/Device.h"
#include "render/RtvDescriptorHeap.h"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <cassert>
#include <cstdio>
#include <stdexcept>

namespace engine::render
{
    using engine::core::ThrowIfFailed;

    namespace
    {

        constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    }

    SwapChain::SwapChain(
        Device&                    device,
        CommandQueue&              presentQueue,
        engine::platform::Window&  window,
        RtvDescriptorHeap&         rtvHeap)
    {
        IDXGIFactory6* factory = device.Factory();

        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width              = static_cast<UINT>(window.Width());
        desc.Height             = static_cast<UINT>(window.Height());
        desc.Format             = kBackBufferFormat;
        desc.Stereo             = FALSE;
        desc.SampleDesc.Count   = 1;     // 멀티샘플 OFF (스왑체인은 FLIP 시 1만 허용)
        desc.SampleDesc.Quality = 0;
        desc.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount        = kBackBufferCount;
        desc.Scaling            = DXGI_SCALING_STRETCH;
        desc.SwapEffect         = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode          = DXGI_ALPHA_MODE_UNSPECIFIED;
        desc.Flags              = 0;

        Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain1;
        ThrowIfFailed(
            factory->CreateSwapChainForHwnd(
                presentQueue.Native(),    // 큐는 present 와 연결됨
                window.NativeHwnd(),       // friend 접근
                &desc,
                nullptr,                   // fullscreen desc — 윈도우 모드
                nullptr,                   // 출력 제한 없음
                swapChain1.GetAddressOf()),
            "IDXGIFactory6::CreateSwapChainForHwnd");

        // Alt+Enter 전체화면 토글 비활성화 — 우리가 직접 fullscreen 관리.
        // 주의: MakeWindowAssociation 은 SwapChain 을 생성한 동일 factory 에서 호출해야 한다.
        // 본 코드는 줄곧 device.Factory() 만 사용 — 향후 Device 가 Factory 를 재생성하는
        // 경로가 생기면 본 호출도 그 시점에 일관성 점검 필요.
        ThrowIfFailed(
            factory->MakeWindowAssociation(window.NativeHwnd(), DXGI_MWA_NO_ALT_ENTER),
            "IDXGIFactory6::MakeWindowAssociation");

        ThrowIfFailed(
            swapChain1.As(&m_swapChain),
            "IDXGISwapChain1 -> IDXGISwapChain3 QueryInterface");

        // 백버퍼 리소스 획득 + RTV 생성·등록.
        ID3D12Device* d3dDevice = device.Native();
        for (std::uint32_t i = 0; i < kBackBufferCount; ++i)
        {
            ThrowIfFailed(
                m_swapChain->GetBuffer(i, IID_PPV_ARGS(m_backBuffers[i].GetAddressOf())),
                "IDXGISwapChain3::GetBuffer");

            m_rtvHandles[i] = rtvHeap.Allocate();
            d3dDevice->CreateRenderTargetView(
                m_backBuffers[i].Get(),
                nullptr,                  // 디폴트 RTV 디스크립션
                m_rtvHandles[i]);
        }

        m_currentIndex = m_swapChain->GetCurrentBackBufferIndex();

        engine::core::LogInfo(L"[render] SwapChain created (FLIP_DISCARD, 2 back buffers, RTVs registered)\n");
    }

    SwapChain::~SwapChain() = default;

    void SwapChain::Present()
    {
        // SyncInterval=0 → V-Sync OFF. PresentFlags=0 → 기본 동작.
        ThrowIfFailed(m_swapChain->Present(0, 0), "IDXGISwapChain3::Present");
        m_currentIndex = m_swapChain->GetCurrentBackBufferIndex();
    }

    ID3D12Resource* SwapChain::CurrentBackBuffer() const noexcept
    {
        assert(m_currentIndex < kBackBufferCount);
        return m_backBuffers[m_currentIndex].Get();
    }

    D3D12_CPU_DESCRIPTOR_HANDLE SwapChain::CurrentRtv() const noexcept
    {
        assert(m_currentIndex < kBackBufferCount);
        return m_rtvHandles[m_currentIndex];
    }

    IDXGISwapChain3* SwapChain::Native() const noexcept
    {
        return m_swapChain.Get();
    }
}
