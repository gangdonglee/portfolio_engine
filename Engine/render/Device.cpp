#include "render/Device.h"

#include "core/HrCheck.h"
#include "core/Logger.h"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstdio>
#include <cwchar>
#include <iterator>
#include <stdexcept>

namespace engine::render
{
    using engine::core::ThrowIfFailed;

    namespace
    {
        constexpr D3D_FEATURE_LEVEL kMinFeatureLevel = D3D_FEATURE_LEVEL_12_0;

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
            engine::core::LogInfo(line);
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
        QueryTearingSupport();
        engine::core::LogInfo(L"[render] D3D12 device created\n");
    }

    Device::~Device()
    {
        // ComPtr 들이 역순으로 자동 Release.
        // 멤버가 전방 선언 타입이지만 cpp 에서 정의된 소멸자이므로 이 시점에 완전 타입.
    }

    ID3D12Device*  Device::Native()         const noexcept { return m_device.Get(); }
    IDXGIFactory6* Device::Factory()        const noexcept { return m_factory.Get(); }
    bool           Device::SupportsTearing() const noexcept { return m_supportsTearing; }

    void Device::EnableDebugLayer()
    {
        Microsoft::WRL::ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(::D3D12GetDebugInterface(IID_PPV_ARGS(debugController.GetAddressOf()))))
        {
            debugController->EnableDebugLayer();
            engine::core::LogInfo(L"[render] D3D12 debug layer enabled\n");
        }
        else
        {
            // Graphics Tools (Windows 옵션 기능) 미설치 시 정상적으로 실패.
            engine::core::LogInfo(L"[render] D3D12 debug layer unavailable (Graphics Tools 미설치?)\n");
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
            ::CreateDXGIFactory2(flags, IID_PPV_ARGS(m_factory.ReleaseAndGetAddressOf())),
            "CreateDXGIFactory2(IDXGIFactory6)");
    }

    void Device::SelectAdapter()
    {
        // GPU 선호도 기반 어댑터 열거 — 노트북 iGPU+dGPU 환경에서 dGPU 선택을 결정적으로 만든다.
        for (UINT i = 0;; ++i)
        {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> candidate;
            const HRESULT enumHr = m_factory->EnumAdapterByGpuPreference(
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
                m_adapter = candidate;
                LogAdapter(m_adapter.Get());
                return;
            }
        }

        // 하드웨어 어댑터 부재 → WARP 폴백.
        engine::core::LogInfo(L"[render] No hardware adapter; falling back to WARP\n");
        Microsoft::WRL::ComPtr<IDXGIAdapter1> warpAdapter;
        ThrowIfFailed(
            m_factory->EnumWarpAdapter(IID_PPV_ARGS(warpAdapter.GetAddressOf())),
            "IDXGIFactory6::EnumWarpAdapter");
        m_adapter = warpAdapter;
        LogAdapter(m_adapter.Get());
    }

    void Device::CreateDevice()
    {
        ThrowIfFailed(
            ::D3D12CreateDevice(
                m_adapter.Get(),
                kMinFeatureLevel,
                IID_PPV_ARGS(m_device.ReleaseAndGetAddressOf())),
            "D3D12CreateDevice");
    }

    void Device::QueryTearingSupport()
    {
        // VRR (가변 리프레시) / 윈도우 모드 V-Sync OFF 시 tearing 허용 지원 여부.
        // 미지원 환경 (예: 일부 WARP / 구형 GPU) 에서는 조용히 false 유지.
        BOOL allowTearing = FALSE;
        const HRESULT hr = m_factory->CheckFeatureSupport(
            DXGI_FEATURE_PRESENT_ALLOW_TEARING,
            &allowTearing,
            sizeof(allowTearing));
        m_supportsTearing = SUCCEEDED(hr) && (allowTearing != FALSE);

        engine::core::LogInfo(
            m_supportsTearing
                ? L"[render] DXGI tearing support: yes\n"
                : L"[render] DXGI tearing support: no\n");
    }

    void Device::ConfigureInfoQueue()
    {
#if defined(_DEBUG)
        // Info Queue 인터페이스가 없는 환경(Graphics Tools 미설치 등)에서는 조용히 통과.
        if (FAILED(m_device.As(&m_infoQueue)))
        {
            engine::core::LogInfo(L"[render] ID3D12InfoQueue unavailable\n");
            return;
        }
        m_infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
        m_infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR,      TRUE);
        m_infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING,    FALSE);

        // INFO 메시지는 디버그 출력이 시끄러워지므로 저장 필터에서 제외.
        D3D12_MESSAGE_SEVERITY deniedSeverities[] = { D3D12_MESSAGE_SEVERITY_INFO };
        D3D12_INFO_QUEUE_FILTER filter{};
        filter.DenyList.NumSeverities = static_cast<UINT>(std::size(deniedSeverities));
        filter.DenyList.pSeverityList = deniedSeverities;
        m_infoQueue->PushStorageFilter(&filter);

        engine::core::LogInfo(L"[render] Info queue configured (break on Corruption/Error, INFO filtered)\n");
#endif
    }
}
