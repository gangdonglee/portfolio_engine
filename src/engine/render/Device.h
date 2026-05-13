#pragma once

#include <wrl/client.h>

// DX12/DXGI 인터페이스 전방 선언. 헤더가 d3d12.h / dxgi*.h / Windows.h 를 끌어들이지 않음.
struct IDXGIFactory6;
struct IDXGIAdapter1;
struct ID3D12Device;
struct ID3D12InfoQueue;

namespace engine::render
{
    // D3D12 Device 와 DXGI Factory/Adapter 를 RAII 로 캡슐화.
    //
    // 생성자 단계:
    //   ① Debug 빌드면 D3D12 디버그 레이어 활성화
    //   ② DXGI Factory6 생성 (Debug 빌드면 DXGI 디버그 플래그)
    //   ③ FL 12.0 호환 하드웨어 어댑터 선택 — IDXGIFactory6::EnumAdapterByGpuPreference(HIGH_PERFORMANCE)
    //   ④ ID3D12Device 생성
    //   ⑤ Debug 빌드면 Info Queue 셋업 (Corruption/Error 시 break, INFO severity 필터링)
    //
    // 외부 노출: 현 단계 미정. Phase 1D 의 SwapChain 등 정말 필요한 호출처가 생길 때
    // friend 또는 좁힌 어댑터로 제한 노출한다 (캡슐화 최소 원칙).
    //
    // TODO(로깅 시스템 도입 시): OutputDebugStringW / LogAdapter 의 직접 호출을
    // 주입된 로거 인터페이스로 교체 (SRP/DIP).
    class Device final
    {
    public:
        Device();
        ~Device();

        Device(const Device&)            = delete;
        Device& operator=(const Device&) = delete;
        Device(Device&&)                 = delete;
        Device& operator=(Device&&)      = delete;

    private:
        static void EnableDebugLayer();
        void CreateFactory();
        void SelectAdapter();
        void CreateDevice();
        void ConfigureInfoQueue();

        Microsoft::WRL::ComPtr<IDXGIFactory6>    m_factory;
        Microsoft::WRL::ComPtr<IDXGIAdapter1>    m_adapter;
        Microsoft::WRL::ComPtr<ID3D12Device>     m_device;
        Microsoft::WRL::ComPtr<ID3D12InfoQueue>  m_infoQueue;  // Debug 빌드에서만 set
    };
}
