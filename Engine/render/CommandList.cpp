#include "render/CommandList.h"

#include "core/HrCheck.h"
#include "core/Logger.h"

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

    CommandList::CommandList(Device& device)
    {
        ID3D12Device* d3dDevice = device.Native();

        ThrowIfFailed(
            d3dDevice->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(m_allocator.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateCommandAllocator(DIRECT)");

        ThrowIfFailed(
            d3dDevice->CreateCommandList(
                0,                                 // node mask (단일 GPU)
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                m_allocator.Get(),
                nullptr,                           // 초기 PipelineState 없음
                IID_PPV_ARGS(m_list.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateCommandList(DIRECT)");

        // CreateCommandList 는 List 를 recording 상태로 둔다.
        // 일관된 Reset → record → Close 사이클을 위해 즉시 Close 해서 closed 상태로 맞춘다.
        ThrowIfFailed(m_list->Close(), "ID3D12GraphicsCommandList::Close (initial)");

        engine::core::LogInfo(L"[render] CommandList (Direct) created\n");
    }

    CommandList::~CommandList() = default;

    void CommandList::Reset()
    {
        // Allocator 는 GPU 가 이전 프레임 작업을 완료한 후에만 Reset 가능.
        // 호출자가 직전 CommandQueue::FlushGpu() 로 동기화했음을 전제로 한다.
        ThrowIfFailed(m_allocator->Reset(), "ID3D12CommandAllocator::Reset");
        ThrowIfFailed(
            m_list->Reset(m_allocator.Get(), nullptr),
            "ID3D12GraphicsCommandList::Reset");
    }

    void CommandList::Close()
    {
        ThrowIfFailed(m_list->Close(), "ID3D12GraphicsCommandList::Close");
    }

    ID3D12GraphicsCommandList* CommandList::Native() const noexcept
    {
        return m_list.Get();
    }
}
