#include "render/CommandQueue.h"

#include "core/HrCheck.h"
#include "core/Logger.h"

#include "render/CommandList.h"
#include "render/Device.h"

#include <Windows.h>
#include <d3d12.h>

#include <cstdio>
#include <stdexcept>

namespace engine::render
{
    using engine::core::ThrowIfFailed;

    namespace
    {
    }

    CommandQueue::CommandQueue(Device& device)
    {
        ID3D12Device* d3dDevice = device.Native();

        // Phase 1D-1 은 Direct 큐만. Compute/Copy 큐는 필요해질 때 별도 클래스 또는 ctor 오버로드 추가.
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
        desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        desc.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;
        desc.NodeMask = 0;

        ThrowIfFailed(
            d3dDevice->CreateCommandQueue(
                &desc,
                IID_PPV_ARGS(m_queue.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateCommandQueue");

        ThrowIfFailed(
            d3dDevice->CreateFence(
                m_fenceValue,
                D3D12_FENCE_FLAG_NONE,
                IID_PPV_ARGS(m_fence.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateFence");

        // manual-reset=FALSE (auto-reset), initial-state=FALSE (non-signaled).
        m_fenceEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (m_fenceEvent == nullptr)
        {
            throw std::runtime_error("CreateEventW (fence event) returned nullptr");
        }

        engine::core::LogInfo(L"[render] CommandQueue (Direct) created\n");
    }

    CommandQueue::~CommandQueue()
    {
        // GPU 가 큐에 남긴 작업을 모두 완료해야 안전하게 소멸 가능.
        // 큐 자체가 nullptr 이면 생성 도중 실패 — Flush 시도 불가.
        if (m_queue && m_fence && m_fenceEvent != nullptr)
        {
            try
            {
                FlushGpu();
            }
            catch (...)
            {
                // 소멸자에서 예외 전파 금지. 디버그 출력으로만 남김.
                engine::core::LogInfo(L"[render] CommandQueue dtor: FlushGpu threw — ignoring\n");
            }
        }

        if (m_fenceEvent != nullptr)
        {
            ::CloseHandle(m_fenceEvent);
            m_fenceEvent = nullptr;
        }
        // ComPtr 들은 역순으로 자동 Release.
    }

    std::uint64_t CommandQueue::Signal()
    {
        // 실패 시 m_fenceValue 가 비대칭 증가하지 않도록, 로컬 next 로 먼저 시그널하고
        // 성공 후에만 멤버를 갱신한다.
        const std::uint64_t next = m_fenceValue + 1;
        ThrowIfFailed(
            m_queue->Signal(m_fence.Get(), next),
            "ID3D12CommandQueue::Signal");
        m_fenceValue = next;
        return next;
    }

    void CommandQueue::WaitForFenceValue(std::uint64_t value)
    {
        if (m_fence->GetCompletedValue() >= value)
        {
            return;
        }
        ThrowIfFailed(
            m_fence->SetEventOnCompletion(value, static_cast<HANDLE>(m_fenceEvent)),
            "ID3D12Fence::SetEventOnCompletion");

        // INFINITE 대기는 GPU 행/TDR/디바이스 제거 시 영구 정지 위험.
        // 5초 타임아웃으로 제한 — 타임아웃 시 예외(소멸자 경로에선 try/catch 가 swallow).
        constexpr DWORD kWaitTimeoutMs = 5000;
        const DWORD waitResult = ::WaitForSingleObject(
            static_cast<HANDLE>(m_fenceEvent),
            kWaitTimeoutMs);

        if (waitResult == WAIT_OBJECT_0)
        {
            return;
        }
        if (waitResult == WAIT_TIMEOUT)
        {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "Fence wait timeout (%lu ms) for value %llu — GPU 행/TDR/디바이스 제거 의심",
                          static_cast<unsigned long>(kWaitTimeoutMs),
                          static_cast<unsigned long long>(value));
            throw std::runtime_error(buf);
        }
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "WaitForSingleObject 비정상 반환 0x%08lX (GetLastError=%lu)",
                      static_cast<unsigned long>(waitResult),
                      static_cast<unsigned long>(::GetLastError()));
        throw std::runtime_error(buf);
    }

    void CommandQueue::FlushGpu()
    {
        const std::uint64_t value = Signal();
        WaitForFenceValue(value);
    }

    void CommandQueue::Execute(CommandList& list)
    {
        // ID3D12GraphicsCommandList 는 ID3D12CommandList 의 파생. 암시 변환.
        ID3D12CommandList* lists[] = { list.Native() };
        m_queue->ExecuteCommandLists(_countof(lists), lists);
    }

    ID3D12CommandQueue* CommandQueue::Native() const noexcept
    {
        return m_queue.Get();
    }
}
