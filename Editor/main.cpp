// portfolio_engine Editor — M2 본 작업
//   M0: ImGui Win32+DX12 부트 + 도킹 활성화 + 빈 패널 3개.
//   M1: File→Save 로 하드코딩 Scene → JSON 라운드트립 검증.
//   M2: 활성 Scene 보유 + Hierarchy/Inspector 패널 실제 편집 + File New/Open/Save 다이얼로그.

#include <Windows.h>
#include <ShObjIdl.h>      // IFileDialog
#include <d3d12.h>
#include <dxgiformat.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "Panels.h"

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
    // SRV 디스크립터 힙 bump-allocator — ImGui 1.92+ 의 동적 텍스처 API 대응.
    // 비소유 참조 (m_heap) — heap 은 외부 lifetime, 본 객체는 콜백 어댑터 역할만.
    class ImGuiSrvAllocator final
    {
    public:
        explicit ImGuiSrvAllocator(engine::render::SrvDescriptorHeap& heap) : m_heap(heap) {}

        ImGuiSrvAllocator(const ImGuiSrvAllocator&)            = delete;
        ImGuiSrvAllocator& operator=(const ImGuiSrvAllocator&) = delete;
        ImGuiSrvAllocator(ImGuiSrvAllocator&&)                 = delete;
        ImGuiSrvAllocator& operator=(ImGuiSrvAllocator&&)      = delete;

        static void Alloc(ImGui_ImplDX12_InitInfo* info,
                          D3D12_CPU_DESCRIPTOR_HANDLE* outCpu,
                          D3D12_GPU_DESCRIPTOR_HANDLE* outGpu)
        {
            auto* self = static_cast<ImGuiSrvAllocator*>(info->UserData);
            const auto handle = self->m_heap.Allocate();
            *outCpu = handle.cpu;
            *outGpu = handle.gpu;
        }

        static void Free(ImGui_ImplDX12_InitInfo* /*info*/,
                         D3D12_CPU_DESCRIPTOR_HANDLE /*cpu*/,
                         D3D12_GPU_DESCRIPTOR_HANDLE /*gpu*/)
        {
            // no-op.
        }

    private:
        engine::render::SrvDescriptorHeap& m_heap;
    };

    // CoInitializeEx 라이프타임 RAII — IFileDialog 사용 전 필요.
    class ComScope final
    {
    public:
        ComScope()
        {
            const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            m_initialized = SUCCEEDED(hr);
            if (!m_initialized && hr != RPC_E_CHANGED_MODE)
            {
                engine::core::LogInfoA("[editor] CoInitializeEx 실패 — IFileDialog 비활성\n");
            }
        }
        ~ComScope()
        {
            if (m_initialized) { ::CoUninitialize(); }
        }
        ComScope(const ComScope&)            = delete;
        ComScope& operator=(const ComScope&) = delete;
        ComScope(ComScope&&)                 = delete;
        ComScope& operator=(ComScope&&)      = delete;

        bool IsInitialized() const noexcept { return m_initialized; }

    private:
        bool m_initialized = false;
    };

    // 부팅 폴백 Scene — sample.scene.json 미존재 시. Dragon 1 + dir 1.
    engine::scene::Scene BuildDefaultScene()
    {
        engine::scene::Scene s;
        s.name = "untitled";
        s.ambient = { 0.12f, 0.13f, 0.16f };
        s.cameraStart.position = { 0.0f, 100.0f, -300.0f };
        s.cameraStart.target   = { 0.0f,  50.0f,    0.0f };
        s.cameraStart.fovYRad  = DirectX::XM_PIDIV4;

        engine::scene::MeshInstance dragon;
        dragon.name          = "Dragon";
        dragon.meshAssetPath = "Resources/FBX/Dragon.fbx";
        s.meshes.push_back(std::move(dragon));

        engine::scene::DirectionalLight sun;
        sun.name        = "Sun";
        sun.directionWS = { -0.5f, -1.0f, 0.4f };
        sun.color       = {  1.0f,  0.97f, 0.92f };
        sun.intensity   = 1.0f;
        s.dirLights.push_back(std::move(sun));

        return s;
    }

    // 디폴트 씬 디렉토리 ($(OutDir)assets/Scenes).
    std::filesystem::path DefaultScenesDir()
    {
        std::filesystem::path dir = std::filesystem::current_path() / "assets" / "Scenes";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir;
    }

    // IFileDialog 한 번 띄움. save=true=SaveDialog, false=OpenDialog. 사용자 취소면 false.
    bool ShowSceneFileDialog(HWND parent, bool save, std::wstring& outPath)
    {
        Microsoft::WRL::ComPtr<IFileDialog> dlg;
        const CLSID clsid = save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog;
        if (FAILED(::CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        {
            return false;
        }

        const COMDLG_FILTERSPEC filter[] = {
            { L"Scene JSON (*.scene.json)", L"*.scene.json" },
            { L"All files (*.*)",            L"*.*" },
        };
        dlg->SetFileTypes(2, filter);
        dlg->SetDefaultExtension(L"scene.json");
        dlg->SetTitle(save ? L"Save Scene" : L"Open Scene");

        // 기본 폴더 = $(OutDir)assets/Scenes.
        Microsoft::WRL::ComPtr<IShellItem> defaultFolder;
        const std::wstring defaultDir = DefaultScenesDir().wstring();
        if (SUCCEEDED(::SHCreateItemFromParsingName(defaultDir.c_str(), nullptr, IID_PPV_ARGS(&defaultFolder))))
        {
            dlg->SetFolder(defaultFolder.Get());
        }

        // Show 가 사용자 취소 시 HRESULT_FROM_WIN32(ERROR_CANCELLED) 반환 — false 처리.
        if (FAILED(dlg->Show(parent))) { return false; }

        Microsoft::WRL::ComPtr<IShellItem> item;
        if (FAILED(dlg->GetResult(&item))) { return false; }

        PWSTR pszPath = nullptr;
        if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &pszPath))) { return false; }
        outPath.assign(pszPath);
        ::CoTaskMemFree(pszPath);
        return true;
    }

    // wstring → utf-8 string (SceneSerializer 가 std::string_view 받음).
    std::string ToUtf8(const std::wstring& w)
    {
        if (w.empty()) { return {}; }
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<std::size_t>(n > 0 ? n - 1 : 0), '\0');
        if (n > 1)
        {
            ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
        }
        return out;
    }
}

int APIENTRY wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int)
{
    ::OutputDebugStringW(L"[editor] boot running\n");

    ComScope com{};   // IFileDialog 사용 전 CoInitializeEx.

    try
    {
        constexpr std::uint32_t kFrameCount = engine::render::SwapChain::kBackBufferCount;
        constexpr DXGI_FORMAT   kRtvFormat  = DXGI_FORMAT_R8G8B8A8_UNORM;

        engine::platform::Window           window(1600, 900, L"portfolio_engine Editor");
        engine::render::Device             device;
        engine::render::CommandQueue       commandQueue(device);
        engine::render::RtvDescriptorHeap  rtvHeap(device, engine::render::SwapChain::kBackBufferCount);
        engine::render::SwapChain          swapChain(device, commandQueue, window, rtvHeap);
        engine::render::SrvDescriptorHeap  srvHeap(device, 64);

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

        ImGuiSrvAllocator srvAllocator{ srvHeap };
        ImGui_ImplDX12_InitInfo dxInit{};
        dxInit.Device               = device.Native();
        dxInit.CommandQueue         = commandQueue.Native();
        dxInit.NumFramesInFlight    = static_cast<int>(kFrameCount);
        dxInit.RTVFormat            = kRtvFormat;
        dxInit.DSVFormat            = DXGI_FORMAT_UNKNOWN;
        dxInit.SrvDescriptorHeap    = srvHeap.Native();
        dxInit.SrvDescriptorAllocFn = &ImGuiSrvAllocator::Alloc;
        dxInit.SrvDescriptorFreeFn  = &ImGuiSrvAllocator::Free;
        dxInit.UserData             = &srvAllocator;
        if (!ImGui_ImplDX12_Init(&dxInit))
        {
            throw std::runtime_error("ImGui_ImplDX12_Init failed");
        }

        window.SetWndProcHook(
            [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT
            {
                return ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
            });

        // === 활성 Scene 상태 ===
        // 부팅 시 OutDir/assets/Scenes/sample.scene.json 자동 로드 시도. 없으면 default.
        engine::scene::Scene activeScene;
        std::string          activeScenePath;
        bool                 modified = false;
        {
            const std::filesystem::path bootPath = DefaultScenesDir() / "sample.scene.json";
            if (std::filesystem::exists(bootPath))
            {
                try
                {
                    activeScene = engine::scene::LoadJson(bootPath.string());
                    activeScenePath = bootPath.string();
                }
                catch (const std::exception& e)
                {
                    engine::core::LogInfoA("[editor] sample 로드 실패, default 사용: ");
                    engine::core::LogInfoA(e.what());
                    engine::core::LogInfoA("\n");
                    activeScene = BuildDefaultScene();
                }
            }
            else
            {
                activeScene = BuildDefaultScene();
            }
        }
        editor::panels::Selection selection{};

        std::array<std::uint64_t, kFrameCount> frameFenceValues{};
        std::uint32_t frameIndex = 0;

        constexpr float kClearColor[4] = { 0.10f, 0.11f, 0.13f, 1.0f };
        std::string lastStatus = activeScenePath.empty()
            ? std::string{"(부팅: default scene)"}
            : ("Opened: " + activeScenePath);

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

            ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

            // 메뉴 클릭은 이번 프레임에 처리할 의도만 플래그로 기록 — 다이얼로그를 ImGui::Render
            // 이전에 띄우면 modal block 시 그 프레임 ImGui 출력이 멈출 수 있어 안전.
            bool wantNew = false, wantOpen = false, wantSave = false, wantSaveAs = false;

            if (ImGui::BeginMainMenuBar())
            {
                if (ImGui::BeginMenu("File"))
                {
                    if (ImGui::MenuItem("New Scene"))               { wantNew     = true; }
                    if (ImGui::MenuItem("Open Scene..."))           { wantOpen    = true; }
                    if (ImGui::MenuItem("Save",         "Ctrl+S",
                                        false,
                                        /*enabled*/ !activeScenePath.empty()))     { wantSave   = true; }
                    if (ImGui::MenuItem("Save As..."))              { wantSaveAs  = true; }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Exit")) { ::PostMessageW(window.NativeHwnd(), WM_CLOSE, 0, 0); }
                    ImGui::EndMenu();
                }
                // 우측에 modified 표시.
                if (modified)
                {
                    ImGui::SameLine(ImGui::GetWindowWidth() - 80.0f);
                    ImGui::TextColored(ImVec4{1.0f, 0.7f, 0.3f, 1.0f}, "[modified]");
                }
                ImGui::EndMainMenuBar();
            }

            // === Hierarchy 패널 ===
            if (ImGui::Begin("Hierarchy"))
            {
                if (editor::panels::DrawHierarchy(activeScene, selection))
                {
                    modified = true;
                }
            }
            ImGui::End();

            // === Inspector 패널 ===
            if (ImGui::Begin("Inspector"))
            {
                if (editor::panels::DrawInspector(activeScene, selection))
                {
                    modified = true;
                }
            }
            ImGui::End();

            // === Viewport 패널 (M4 에서 실제 3D 렌더) ===
            if (ImGui::Begin("Viewport"))
            {
                ImGui::TextDisabled("(M4 이후 3D 뷰포트 렌더)");
                ImGui::Separator();
                ImGui::TextWrapped("Status: %s", lastStatus.c_str());
            }
            ImGui::End();

            ImGui::Render();

            // === 메뉴 액션 처리 (ImGui::Render 후 — 다이얼로그 modal 안전) ===
            // 다이얼로그 동작이 있으면 modal 동안 in-flight fence 가 묶이지 않도록
            // 미리 GPU 작업 완료 + frame 슬롯 wait 분기 skip 표시.
            const bool willModal = wantOpen || wantSaveAs;
            if (willModal)
            {
                commandQueue.FlushGpu();
                for (auto& v : frameFenceValues) { v = 0; }
            }

            if (wantNew)
            {
                activeScene = engine::scene::Scene{};
                activeScene.name = "untitled";
                activeScenePath.clear();
                selection = {};
                modified = false;
                lastStatus = "New empty scene";
            }
            if (wantOpen)
            {
                std::wstring picked;
                if (ShowSceneFileDialog(window.NativeHwnd(), /*save*/false, picked))
                {
                    const std::string path = ToUtf8(picked);
                    try
                    {
                        activeScene = engine::scene::LoadJson(path);
                        activeScenePath = path;
                        selection = {};
                        modified = false;
                        lastStatus = "Opened: " + path;
                    }
                    catch (const std::exception& e)
                    {
                        lastStatus = std::string{"Open FAILED: "} + e.what();
                    }
                }
            }
            if (wantSave && !activeScenePath.empty())
            {
                try
                {
                    engine::scene::SaveJson(activeScene, activeScenePath);
                    modified = false;
                    lastStatus = "Saved: " + activeScenePath;
                }
                catch (const std::exception& e)
                {
                    lastStatus = std::string{"Save FAILED: "} + e.what();
                }
            }
            if (wantSaveAs)
            {
                std::wstring picked;
                if (ShowSceneFileDialog(window.NativeHwnd(), /*save*/true, picked))
                {
                    const std::string path = ToUtf8(picked);
                    try
                    {
                        engine::scene::SaveJson(activeScene, path);
                        activeScenePath = path;
                        modified = false;
                        lastStatus = "Saved As: " + path;
                    }
                    catch (const std::exception& e)
                    {
                        lastStatus = std::string{"Save As FAILED: "} + e.what();
                    }
                }
            }

            // === 이 슬롯 fence 대기 → 명령 기록 ===
            if (frameFenceValues[frameIndex] != 0)
            {
                commandQueue.WaitForFenceValue(frameFenceValues[frameIndex]);
            }

            engine::render::CommandList& cmdList = *cmdLists[frameIndex];
            cmdList.Reset();
            ID3D12GraphicsCommandList* list = cmdList.Native();

            ID3D12Resource* const backBuffer = swapChain.CurrentBackBuffer();

            D3D12_RESOURCE_BARRIER toRT{};
            toRT.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toRT.Transition.pResource   = backBuffer;
            toRT.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            toRT.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            toRT.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
            list->ResourceBarrier(1, &toRT);

            const D3D12_CPU_DESCRIPTOR_HANDLE rtv = swapChain.CurrentRtv();
            list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            list->ClearRenderTargetView(rtv, kClearColor, 0, nullptr);

            ID3D12DescriptorHeap* heaps[] = { srvHeap.Native() };
            list->SetDescriptorHeaps(1, heaps);

            ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), list);

            D3D12_RESOURCE_BARRIER toPresent{};
            toPresent.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
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
