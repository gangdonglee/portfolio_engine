// portfolio_engine Editor — M0 골격
//   목표: ImGui Win32+DX12 부트 + 도킹 활성화 + 빈 패널 3개(Hierarchy/Inspector/Viewport).
//   씬 데이터/뷰포트 렌더는 M1 이후 단계에서 추가.

#include <Windows.h>
#include <d3d12.h>
#include <dxgiformat.h>

#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

#include "platform/Window.h"
#include "render/CommandList.h"
#include "render/CommandQueue.h"
#include "render/Device.h"
#include "render/RtvDescriptorHeap.h"
#include "render/SrvDescriptorHeap.h"
#include "render/SwapChain.h"

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx12.h"

// ImGui 의 Win32 backend 가 제공하는 WndProc 핸들러 — Window 의 훅으로 등록.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
    // SRV 디스크립터 힙을 bump-allocate. M0 는 font 1개만 할당하지만 1.92+ 의 동적 텍스처 API
    // 가 추가 슬롯을 요구할 수 있어 알로케이터를 정식 제공한다. free 는 no-op (툴 수명 동안 누적).
    // 자산 수가 본격적으로 늘면 free-list 로 전환.
    struct ImGuiSrvAllocator
    {
        engine::render::SrvDescriptorHeap* heap = nullptr;

        static void Alloc(ImGui_ImplDX12_InitInfo* info,
                          D3D12_CPU_DESCRIPTOR_HANDLE* outCpu,
                          D3D12_GPU_DESCRIPTOR_HANDLE* outGpu)
        {
            auto* self = static_cast<ImGuiSrvAllocator*>(info->UserData);
            const auto handle = self->heap->Allocate();
            *outCpu = handle.cpu;
            *outGpu = handle.gpu;
        }

        static void Free(ImGui_ImplDX12_InitInfo* /*info*/,
                         D3D12_CPU_DESCRIPTOR_HANDLE /*cpu*/,
                         D3D12_GPU_DESCRIPTOR_HANDLE /*gpu*/)
        {
            // no-op (bump). 동일 슬롯은 재할당되지 않음.
        }
    };
}

int APIENTRY wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int)
{
    ::OutputDebugStringW(L"[editor] boot running\n");

    try
    {
        constexpr std::uint32_t kFrameCount = engine::render::SwapChain::kBackBufferCount;
        constexpr DXGI_FORMAT   kRtvFormat  = DXGI_FORMAT_R8G8B8A8_UNORM;

        engine::platform::Window           window(1600, 900, L"portfolio_engine Editor");
        engine::render::Device             device;
        engine::render::CommandQueue       commandQueue(device);
        engine::render::RtvDescriptorHeap  rtvHeap(device, engine::render::SwapChain::kBackBufferCount);
        engine::render::SwapChain          swapChain(device, commandQueue, window, rtvHeap);

        // ImGui 폰트 + 향후 동적 텍스처용 shader-visible SRV 힙. 용량은 여유 있게 64.
        engine::render::SrvDescriptorHeap srvHeap(device, 64);

        std::array<std::unique_ptr<engine::render::CommandList>, kFrameCount> cmdLists;
        for (std::uint32_t i = 0; i < kFrameCount; ++i)
        {
            cmdLists[i] = std::make_unique<engine::render::CommandList>(device);
        }

        // === ImGui 컨텍스트 ===
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        ImGui::StyleColorsDark();

        if (!ImGui_ImplWin32_Init(window.NativeHwnd()))
        {
            throw std::runtime_error("ImGui_ImplWin32_Init failed");
        }

        ImGuiSrvAllocator srvAllocator{ &srvHeap };

        ImGui_ImplDX12_InitInfo dxInit{};
        dxInit.Device               = device.Native();
        dxInit.CommandQueue         = commandQueue.Native();
        dxInit.NumFramesInFlight    = static_cast<int>(kFrameCount);
        dxInit.RTVFormat            = kRtvFormat;
        dxInit.DSVFormat            = DXGI_FORMAT_UNKNOWN; // M0 는 뎁스 미사용
        dxInit.SrvDescriptorHeap    = srvHeap.Native();
        dxInit.SrvDescriptorAllocFn = &ImGuiSrvAllocator::Alloc;
        dxInit.SrvDescriptorFreeFn  = &ImGuiSrvAllocator::Free;
        dxInit.UserData             = &srvAllocator;

        if (!ImGui_ImplDX12_Init(&dxInit))
        {
            throw std::runtime_error("ImGui_ImplDX12_Init failed");
        }

        // Window 의 WndProc 흐름에 ImGui Win32 핸들러를 끼워 넣음.
        window.SetWndProcHook(
            [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT
            {
                return ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
            });

        std::array<std::uint64_t, kFrameCount> frameFenceValues{};
        std::uint32_t frameIndex = 0;

        constexpr float kClearColor[4] = { 0.10f, 0.11f, 0.13f, 1.0f };

        while (window.IsOpen())
        {
            window.PumpMessages();
            if (!window.IsOpen()) { break; }

            if (window.ConsumeResize())
            {
                commandQueue.FlushGpu();
                for (auto& v : frameFenceValues) { v = 0; }

                const auto w = static_cast<std::uint32_t>(window.Width());
                const auto h = static_cast<std::uint32_t>(window.Height());
                swapChain.Resize(device, w, h);
            }

            // === ImGui 프레임 ===
            ImGui_ImplDX12_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            // 메인 뷰포트 전체에 도킹 공간 — Unity/Unreal 스타일 도킹 영역.
            ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

            // 메인 메뉴바.
            if (ImGui::BeginMainMenuBar())
            {
                if (ImGui::BeginMenu("File"))
                {
                    if (ImGui::MenuItem("New Scene"))  { /* M1 */ }
                    if (ImGui::MenuItem("Open Scene")) { /* M1 */ }
                    if (ImGui::MenuItem("Save Scene")) { /* M1 */ }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Exit")) { ::PostMessageW(window.NativeHwnd(), WM_CLOSE, 0, 0); }
                    ImGui::EndMenu();
                }
                ImGui::EndMainMenuBar();
            }

            // 빈 패널 3개 — M1 부터 내용 채움.
            if (ImGui::Begin("Hierarchy"))
            {
                ImGui::TextDisabled("(M1 에서 씬 트리)");
            }
            ImGui::End();

            if (ImGui::Begin("Inspector"))
            {
                ImGui::TextDisabled("(M2 에서 트랜스폼/속성 편집)");
            }
            ImGui::End();

            if (ImGui::Begin("Viewport"))
            {
                ImGui::TextDisabled("(M2 이후 뷰포트 렌더 텍스처)");
            }
            ImGui::End();

            ImGui::Render();

            // === 이 슬롯 fence 대기 → 명령 기록 ===
            if (frameFenceValues[frameIndex] != 0)
            {
                commandQueue.WaitForFenceValue(frameFenceValues[frameIndex]);
            }

            engine::render::CommandList& cmdList = *cmdLists[frameIndex];
            cmdList.Reset();
            ID3D12GraphicsCommandList* list = cmdList.Native();

            ID3D12Resource* const backBuffer = swapChain.CurrentBackBuffer();

            D3D12_RESOURCE_BARRIER toRenderTarget{};
            toRenderTarget.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toRenderTarget.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            toRenderTarget.Transition.pResource   = backBuffer;
            toRenderTarget.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            toRenderTarget.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            toRenderTarget.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
            list->ResourceBarrier(1, &toRenderTarget);

            const D3D12_CPU_DESCRIPTOR_HANDLE rtv = swapChain.CurrentRtv();
            list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            list->ClearRenderTargetView(rtv, kClearColor, 0, nullptr);

            ID3D12DescriptorHeap* heaps[] = { srvHeap.Native() };
            list->SetDescriptorHeaps(1, heaps);

            ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), list);

            D3D12_RESOURCE_BARRIER toPresent{};
            toPresent.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toPresent.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            toPresent.Transition.pResource   = backBuffer;
            toPresent.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            toPresent.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
            list->ResourceBarrier(1, &toPresent);

            cmdList.Close();
            commandQueue.Execute(cmdList);
            swapChain.Present();

            frameFenceValues[frameIndex] = commandQueue.Signal();
            frameIndex = (frameIndex + 1) % kFrameCount;
        }

        // 종료 — GPU 작업 완료 후 ImGui shutdown.
        commandQueue.FlushGpu();
        window.SetWndProcHook(nullptr);
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    catch (const std::exception& e)
    {
        ::OutputDebugStringA("[editor] fatal: ");
        ::OutputDebugStringA(e.what());
        ::OutputDebugStringA("\n");
        return 1;
    }

    ::OutputDebugStringW(L"[editor] exit clean\n");
    return 0;
}
