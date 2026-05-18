#pragma once

#include <wrl/client.h>

#include <d3dcommon.h>   // D3D_FEATURE_LEVEL — enum 이라 전방 선언 불가.

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

        // engine::render 모듈 내부 사용을 위한 raw COM 포인터 접근자.
        // 라이프타임은 Device 가 책임지며, 클라이언트는 호출 외 라이프사이클 조작 금지.
        // (Window::Handle 처럼 다중 호출처가 적은 경우와 달리, ID3D12Device 는
        //  렌더 서브시스템 거의 전부가 필요로 하므로 friend 폭증 회피 위해 공개 접근자 채택.)
        // Native() 명칭은 "raw COM 포인터를 그대로 노출한다"는 의도를 호출 측에 명시한다.
        ID3D12Device*  Native()  const noexcept;
        // SwapChain 등 DXGI 직접 사용처에서 필요 (CreateSwapChainForHwnd 등).
        // 실 사용처 등장 시점(Phase 1D-3 SwapChain) 에 추가됨.
        IDXGIFactory6* Factory() const noexcept;

        // VRR (가변 리프레시) 디스플레이에서 V-Sync OFF tearing 허용 지원 여부.
        // 생성자에서 IDXGIFactory6::CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING) 한 번 조회.
        bool SupportsTearing() const noexcept;

    private:
        static void EnableDebugLayer();
        void CreateFactory();
        void SelectAdapter();
        void CreateDevice();
        void ConfigureInfoQueue();
        void QueryTearingSupport();

        Microsoft::WRL::ComPtr<IDXGIFactory6>    m_factory;
        Microsoft::WRL::ComPtr<IDXGIAdapter1>    m_adapter;
        Microsoft::WRL::ComPtr<ID3D12Device>     m_device;
        Microsoft::WRL::ComPtr<ID3D12InfoQueue>  m_infoQueue;  // Debug 빌드에서만 set
        D3D_FEATURE_LEVEL m_selectedFeatureLevel = D3D_FEATURE_LEVEL_11_0;  // SelectAdapter 가 갱신
        bool m_supportsTearing = false;
    };
}
