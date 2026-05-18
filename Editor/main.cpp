// portfolio_engine Editor — M0 골격 + M1 Scene Save 동작
//   M0: ImGui Win32+DX12 부트 + 도킹 활성화 + 빈 패널 3개.
//   M1: File→Save 시 하드코딩 Scene 을 JSON 으로 저장 — Scene/Serializer 라운드트립 검증.

#include <Windows.h>
#include <d3d12.h>
#include <dxgiformat.h>
#include <DirectXMath.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/Logger.h"
#include "platform/Window.h"
#include "render/CommandList.h"
#include "render/CommandQueue.h"
#include "render/Device.h"
#include "render/RtvDescriptorHeap.h"
#include "render/SrvDescriptorHeap.h"
#include "render/SwapChain.h"
#include "scene/Scene.h"
#include "scene/SceneSerializer.h"

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

    // 하드코딩 Scene — File→Save 클릭 시 이걸 JSON 으로 저장.
    // M2 에서 Hierarchy/Inspector 패널이 실제 편집을 지원하면 이 함수는 사라지고
    // 에디터 상태(activeScene) 를 직접 저장.
    engine::scene::Scene BuildHardcodedScene()
    {
        engine::scene::Scene s;
        s.name = "from_editor";
        s.ambient = { 0.10f, 0.12f, 0.15f };
        s.cameraStart.position = { 0.0f, 120.0f, -320.0f };
        s.cameraStart.target   = { 0.0f,  60.0f,    0.0f };
        s.cameraStart.fovYRad  = DirectX::XM_PIDIV4;

        engine::scene::MeshInstance dragon;
        dragon.name          = "Dragon";
        dragon.meshAssetPath = "Resources/FBX/Dragon.fbx";
        s.meshes.push_back(std::move(dragon));

        engine::scene::DirectionalLight sun;
        sun.name        = "Sun";
        sun.directionWS = { -0.4f, -1.0f,  0.3f };
        sun.color       = {  1.0f,  0.95f, 0.85f };
        sun.intensity   = 1.0f;
        s.dirLights.push_back(std::move(sun));

        engine::scene::PointLight rim;
        rim.name       = "RimLight";
        rim.positionWS = { 150.0f, 100.0f, -100.0f };
        rim.color      = { 0.3f,   0.7f,    1.0f };
        rim.intensity  = 2.5f;
        rim.range      = 500.0f;
        s.pointLights.push_back(std::move(rim));

        return s;
    }

    // 저장 위치: $(OutDir)assets/Scenes/from_editor.scene.json.
    // OutDir 은 Client 와 동일 — PostBuild 가 assets/ 폴더를 복사하므로 OutDir 기준 같은 트리.
    std::filesystem::path EditorSaveScenePath()
    {
        std::filesystem::path dir = std::filesystem::current_path() / "assets" / "Scenes";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir / "from_editor.scene.json";
    }
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

        // Save 결과 상태 — Inspector 패널에 마지막 저장 결과 표시.
        std::string lastSaveStatus = "(아직 저장 안 함)";

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
                    if (ImGui::MenuItem("New Scene"))  { /* M2 */ }
                    if (ImGui::MenuItem("Open Scene")) { /* M2 */ }
                    if (ImGui::MenuItem("Save Scene"))
                    {
                        // M1 검증용 — 하드코딩 Scene 을 JSON 으로 저장.
                        // M2 에서 패널 편집 결과를 저장하도록 교체.
                        const std::filesystem::path savePath = EditorSaveScenePath();
                        try
                        {
                            engine::scene::SaveJson(BuildHardcodedScene(), savePath.string());
                            lastSaveStatus = "Saved: " + savePath.string();
                            engine::core::LogInfoA("[editor] ");
                            engine::core::LogInfoA(lastSaveStatus.c_str());
                            engine::core::LogInfoA("\n");
                        }
                        catch (const std::exception& e)
                        {
                            lastSaveStatus = std::string{"Save FAILED: "} + e.what();
                            engine::core::LogInfoA("[editor] ");
                            engine::core::LogInfoA(lastSaveStatus.c_str());
                            engine::core::LogInfoA("\n");
                        }
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Exit")) { ::PostMessageW(window.NativeHwnd(), WM_CLOSE, 0, 0); }
                    ImGui::EndMenu();
                }
                ImGui::EndMainMenuBar();
            }

            // 패널 3개 — M2 에서 본격 편집 가능 패널로 채움.
            if (ImGui::Begin("Hierarchy"))
            {
                ImGui::TextDisabled("(M2 에서 씬 트리)");
            }
            ImGui::End();

            if (ImGui::Begin("Inspector"))
            {
                ImGui::TextWrapped("M1: File > Save Scene 으로 하드코딩 씬을 JSON 저장.");
                ImGui::Separator();
                ImGui::TextWrapped("Last save:");
                ImGui::TextWrapped("%s", lastSaveStatus.c_str());
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
