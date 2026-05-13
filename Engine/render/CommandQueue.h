#pragma once

#include <cstdint>
#include <wrl/client.h>

// DX12 / Win32 의존성은 .cpp 에 한정. 헤더는 전방 선언만.
struct ID3D12CommandQueue;
struct ID3D12Fence;

namespace engine::render
{
    class Device;
    class CommandList;

    // Windows HANDLE 등가. 헤더에서 Windows.h 의존을 회피하기 위한 타입 alias.
    using FenceEventHandle = void*;

    // Direct 큐 + 펜스 + 이벤트 핸들을 묶은 GPU 동기화 단위.
    //
    // 책임:
    //   - GPU 에 커맨드 리스트 제출(추후 단계)
    //   - 펜스로 CPU↔GPU 동기화 (Signal / WaitForFenceValue / FlushGpu)
    //
    // 본 Phase 1D-1 단계에선 큐 자체와 동기화 인프라만. ExecuteCommandLists 는
    // 커맨드 리스트가 등장하는 다음 sub-stage 에서 추가한다.
    //
    // 향후 분리 결정점:
    //   - ExecuteCommandLists 추가 시 펜스 부분을 `GpuFence` 클래스로 분리 검토.
    //     현 단계는 응집도 우선, SRP 약간 양보.
    //
    // 외부 노출:
    //   - Native() — engine::render 내부에서 SwapChain Present 등에 큐 raw 포인터 전달용.
    //
    // 단일 소유 (복사·이동 금지).
    class CommandQueue final
    {
    public:
        explicit CommandQueue(Device& device);
        ~CommandQueue();

        CommandQueue(const CommandQueue&)            = delete;
        CommandQueue& operator=(const CommandQueue&) = delete;
        CommandQueue(CommandQueue&&)                 = delete;
        CommandQueue& operator=(CommandQueue&&)      = delete;

        // 큐에 새 펜스 값을 시그널. 시그널된 값을 반환.
        // Signal 실패 시 내부 m_fenceValue 는 변경되지 않는다 (롤백 안전).
        std::uint64_t Signal();

        // 지정 펜스 값에 도달할 때까지 CPU 대기.
        // 5초 타임아웃 (GPU 행/TDR/디바이스 제거 대비) — 타임아웃 시 예외.
        void WaitForFenceValue(std::uint64_t value);

        // Signal + WaitForFenceValue. 모든 GPU 작업 완료를 보장.
        void FlushGpu();

        // CommandList 1개를 GPU 에 제출.
        // 호출 전 list 는 Close() 된 상태여야 한다 (호출자 책임).
        void Execute(CommandList& list);

        // raw COM 포인터 노출 — engine::render 내부 사용 전용.
        ID3D12CommandQueue* Native() const noexcept;

    private:
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
        Microsoft::WRL::ComPtr<ID3D12Fence>        m_fence;
        std::uint64_t    m_fenceValue = 0;
        FenceEventHandle m_fenceEvent = nullptr;
    };
}
