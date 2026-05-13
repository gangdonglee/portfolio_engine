#include "engine/render/Device.h"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstdio>
#include <cwchar>
#include <iterator>
#include <stdexcept>

namespace engine::render
{
    namespace
    {
        constexpr D3D_FEATURE_LEVEL kMinFeatureLevel = D3D_FEATURE_LEVEL_12_0;

        void ThrowIfFailed(HRESULT hr, const char* what)
        {
            if (FAILED(hr))
            {
                char buf[256];
                std::snprintf(buf, sizeof(buf),
                              "%s failed: HRESULT=0x%08lX",
                              what,
                              static_cast<unsigned long>(hr));
                throw std::runtime_error(buf);
            }
        }

        // TODO(로깅 시스템 도입 시): 주입된 로거 인터페이스로 교체.
        void LogAdapter(IDXGIAdapter1* adapter)
        {
            if (adapter == nullptr) return;
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);

            const auto vramMB = static_cast<unsigned long long>(
                desc.DedicatedVideoMemory / (1024ULL * 1024ULL));

            wchar_t line[512];
            std::swprintf(line, std::size(line),
                          L"[render] Adapter: %ls (VRAM %llu MB, VendorId 0x%04X, DeviceId 0x%04X)\n",
                          desc.Description,
                          vramMB,
                          desc.VendorId,
                          desc.DeviceId);
            ::OutputDebugStringW(line);
        }
    }

    Device::Device()
    {
#if defined(_DEBUG)
        EnableDebugLayer();
#endif
        CreateFactory();
        SelectAdapter();
        CreateDevice();
#if defined(_DEBUG)
        ConfigureInfoQueue();
#endif
        ::OutputDebugStringW(L"[render] D3D12 device created\n");
    }

    Device::~Device()
    {
        // ComPtr 들이 역순으로 자동 Release.
        // 멤버가 전방 선언 타입이지만 cpp 에서 정의된 소멸자이므로 이 시점에 완전 타입.
    }

    ID3D12Device* Device::Native() const noexcept { return _device.Get(); }

    void Device::EnableDebugLayer()
    {
        Microsoft::WRL::ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(::D3D12GetDebugInterface(IID_PPV_ARGS(debugController.GetAddressOf()))))
        {
            debugController->EnableDebugLayer();
            ::OutputDebugStringW(L"[render] D3D12 debug layer enabled\n");
        }
        else
        {
            // Graphics Tools (Windows 옵션 기능) 미설치 시 정상적으로 실패.
            ::OutputDebugStringW(L"[render] D3D12 debug layer unavailable (Graphics Tools 미설치?)\n");
        }
    }

    void Device::CreateFactory()
    {
        UINT flags = 0;
#if defined(_DEBUG)
        flags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
        // IDXGIFactory6 직접 요청. EnumAdapterByGpuPreference 사용 위해.
        // Windows 10 1803+ 필요 — 본 프로젝트의 SDK(10.0.26100) 최소요건 안에 포함.
        ThrowIfFailed(
            ::CreateDXGIFactory2(flags, IID_PPV_ARGS(_factory.ReleaseAndGetAddressOf())),
            "CreateDXGIFactory2(IDXGIFactory6)");
    }

    void Device::SelectAdapter()
    {
        // GPU 선호도 기반 어댑터 열거 — 노트북 iGPU+dGPU 환경에서 dGPU 선택을 결정적으로 만든다.
        for (UINT i = 0;; ++i)
        {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> candidate;
            const HRESULT enumHr = _factory->EnumAdapterByGpuPreference(
                i,
                DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                IID_PPV_ARGS(candidate.GetAddressOf()));
            if (enumHr == DXGI_ERROR_NOT_FOUND)
            {
                break;
            }
            ThrowIfFailed(enumHr, "IDXGIFactory6::EnumAdapterByGpuPreference");

            DXGI_ADAPTER_DESC1 desc{};
            candidate->GetDesc1(&desc);
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
            {
                continue;  // WARP 는 폴백에서 별도 처리.
            }

            // 호환성만 확인: nullptr 디바이스 인수로 D3D12CreateDevice 드라이런.
            const HRESULT testHr = ::D3D12CreateDevice(
                candidate.Get(),
                kMinFeatureLevel,
                __uuidof(ID3D12Device),
                nullptr);
            if (SUCCEEDED(testHr))
            {
                _adapter = candidate;
                LogAdapter(_adapter.Get());
                return;
            }
        }

        // 하드웨어 어댑터 부재 → WARP 폴백.
        ::OutputDebugStringW(L"[render] No hardware adapter; falling back to WARP\n");
        Microsoft::WRL::ComPtr<IDXGIAdapter1> warpAdapter;
        ThrowIfFailed(
            _factory->EnumWarpAdapter(IID_PPV_ARGS(warpAdapter.GetAddressOf())),
            "IDXGIFactory6::EnumWarpAdapter");
        _adapter = warpAdapter;
        LogAdapter(_adapter.Get());
    }

    void Device::CreateDevice()
    {
        ThrowIfFailed(
            ::D3D12CreateDevice(
                _adapter.Get(),
                kMinFeatureLevel,
                IID_PPV_ARGS(_device.ReleaseAndGetAddressOf())),
            "D3D12CreateDevice");
    }

    void Device::ConfigureInfoQueue()
    {
#if defined(_DEBUG)
        // Info Queue 인터페이스가 없는 환경(Graphics Tools 미설치 등)에서는 조용히 통과.
        if (FAILED(_device.As(&_infoQueue)))
        {
            ::OutputDebugStringW(L"[render] ID3D12InfoQueue unavailable\n");
            return;
        }
        _infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
        _infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR,      TRUE);
        _infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING,    FALSE);

        // INFO 메시지는 디버그 출력이 시끄러워지므로 저장 필터에서 제외.
        D3D12_MESSAGE_SEVERITY deniedSeverities[] = { D3D12_MESSAGE_SEVERITY_INFO };
        D3D12_INFO_QUEUE_FILTER filter{};
        filter.DenyList.NumSeverities = static_cast<UINT>(std::size(deniedSeverities));
        filter.DenyList.pSeverityList = deniedSeverities;
        _infoQueue->PushStorageFilter(&filter);

        ::OutputDebugStringW(L"[render] Info queue configured (break on Corruption/Error, INFO filtered)\n");
#endif
    }
}
