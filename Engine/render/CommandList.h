#pragma once

#include <wrl/client.h>

struct ID3D12CommandAllocator;
struct ID3D12GraphicsCommandList;

namespace engine::render
{
    class Device;

    // Direct 큐용 CommandAllocator + GraphicsCommandList 1 페어 RAII 묶음.
    //
    // 단순 1프레임 in-flight 사용 흐름 (Phase 1D-4 기준):
    //   commandQueue.FlushGpu();   // 직전 프레임 GPU 완료 보장
    //   cmdList.Reset();            // allocator + list 리셋
    //   // ... record (barrier, clear 등) — list.Native() 로 직접 호출 ...
    //   cmdList.Close();
    //   commandQueue.Execute(cmdList);
    //
    // 향후 멀티 프레임 in-flight (예고):
    //   - N(=백버퍼 수) 개의 CommandList 인스턴스.
    //   - 각 프레임 fence 값 추적으로 해당 프레임의 GPU 완료만 대기.
    //
    // 단일 소유 (복사·이동 금지).
    class CommandList final
    {
    public:
        explicit CommandList(Device& device);
        ~CommandList();

        CommandList(const CommandList&)            = delete;
        CommandList& operator=(const CommandList&) = delete;
        CommandList(CommandList&&)                 = delete;
        CommandList& operator=(CommandList&&)      = delete;

        // Allocator + List 리셋. 직전 GPU 작업 완료 후 호출해야 함.
        // 초기 PipelineState 는 nullptr (PSO 가 도입되면 인자 추가).
        void Reset();

        // 기록 종료. Execute 직전 호출.
        void Close();

        // raw COM 노출 — record 메서드 직접 호출용 (engine::render 내부/Client 한정).
        ID3D12GraphicsCommandList* Native() const noexcept;

    private:
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    m_allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_list;
    };
}
