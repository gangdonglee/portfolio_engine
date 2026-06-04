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
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "AnimatorGraph.h"
#include "AnimatorPanel.h"
#include "AssetBrowser.h"
#include "EditorViewport.h"
#include "Panels.h"
#include "SceneRuntime.h"

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
#include "imgui_internal.h"
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

    // 부팅 디폴트 Scene — 빈 씬 + 라이트만. 사용자가 Asset Browser 로 브러시 배치.
    engine::scene::Scene BuildDefaultScene()
    {
        engine::scene::Scene s;
        s.name = "untitled";
        s.ambient = { 0.18f, 0.18f, 0.22f };
        s.cameraStart.position = { 0.0f, 200.0f, -500.0f };
        s.cameraStart.target   = { 0.0f,  90.0f,    0.0f };
        s.cameraStart.fovYRad  = DirectX::XM_PIDIV4;

        engine::scene::DirectionalLight sun;
        sun.name        = "Sun";
        sun.directionWS = { -0.5f, -1.0f, 0.4f };
        sun.color       = {  1.0f,  0.97f, 0.92f };
        sun.intensity   = 1.5f;
        s.dirLights.push_back(std::move(sun));

        engine::scene::DirectionalLight fill;
        fill.name        = "Fill";
        fill.directionWS = { 0.7f, -0.3f, -0.6f };
        fill.color       = { 0.4f, 0.55f, 0.85f };
        fill.intensity   = 0.6f;
        s.dirLights.push_back(std::move(fill));

        return s;
    }

    // 메쉬 배치 시 자동 적용할 디폴트 — importTransform 보정 + 권장 animator.
    //   defaultAnimatorPath 가 비어있지 않으면 placement 시 자동 바인딩.
    //   Mixamo / UE 캐릭터는 bind pose 가 잘못된 자세이므로 (FbxAxisSystem baking 안 됨)
    //   animator 가 첫 프레임에서 본을 재배치해야 정상 자세가 됨.
    struct AssetPlacementDefaults
    {
        engine::scene::Transform importTransform;
        std::string              defaultAnimatorPath;
    };

    AssetPlacementDefaults GuessAssetDefaults(const std::string& meshAssetPath)
    {
        AssetPlacementDefaults d{};
        d.importTransform.position = { 0.0f, 0.0f, 0.0f };
        d.importTransform.rotation = { 0.0f, 0.0f, 0.0f, 1.0f };
        d.importTransform.scale    = { 1.0f, 1.0f, 1.0f };

        const std::filesystem::path p{ meshAssetPath };
        const std::string fname = p.filename().string();

        static constexpr std::array<const char*, 9> kMixamoNames = {
            "X Bot.fbx", "Y Bot.fbx", "Mannequin.fbx",
            "Idle.fbx",  "Walking.fbx", "Running.fbx",
            "Jumping.fbx", "Jump.fbx", "Standing Jump.fbx",
        };
        // animator-친화 import — 01_xbot.scene.json 의 검증된 값.
        // FbxLoader 가 FbxAxisSystem::DirectX 변환은 하지만 vertex/bone baking 은 안 하므로
        //   bind pose 와 animation pose 의 좌표계가 어긋남. 두 케이스에 *다른* 보정이 필요한데
        //   이 엔진의 주 사용처는 animated character → animator 친화 값을 디폴트로.
        //   side-effect: animator 가 없는 raw FBX 만 보면 거꾸로 표시됨 — animator 바인딩되면 정상.
        for (const char* name : kMixamoNames)
        {
            if (fname == name)
            {
                d.importTransform.position = { 0.0f, 180.0f, 0.0f };
                d.importTransform.rotation = { 0.0f, 0.7071068f, -0.7071068f, 0.0f };
                d.defaultAnimatorPath = "assets/Animators/xbot.animator.json";
                return d;
            }
        }

        const std::string lower = [&]{
            std::string s = meshAssetPath;
            for (auto& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
            return s;
        }();
        if (lower.find("/ue/")  != std::string::npos ||
            lower.find("quinn") != std::string::npos)
        {
            d.importTransform.rotation = { 0.7071068f, 0.0f, 0.0f, 0.7071068f };
            d.defaultAnimatorPath = "assets/Animators/quinn.animator.json";
            return d;
        }

        return d;   // identity + no animator
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

        // 한글 폰트 — Windows 기본 Malgun Gothic + Korean glyph range. 없으면 default font fallback.
        {
            const char* fontPath = "C:/Windows/Fonts/malgun.ttf";
            std::error_code ec;
            if (std::filesystem::exists(fontPath, ec))
            {
                ImFontConfig cfg;
                cfg.OversampleH = 2;
                cfg.OversampleV = 1;
                io.Fonts->AddFontFromFileTTF(fontPath, 16.0f, &cfg, io.Fonts->GetGlyphRangesKorean());
            }
            else
            {
                engine::core::LogInfoA("[editor] malgun.ttf 미존재 — 한글이 ??? 로 표시될 수 있음\n");
            }
        }

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

        // === Editor Viewport — RTT + 오빗 카메라 + Boot CmdList + Fallback Tex ===
        // world 뷰 (배치/조작) 와 IK 뷰 (본 편집) 를 분리 — 같은 center dock 의 탭으로 두어
        //   한 프레임에 활성 탭 하나만 렌더 (SceneRuntime cbuffer 공유 충돌 회피).
        editor::EditorViewport viewport(device, commandQueue, srvHeap);
        editor::EditorViewport ikViewport(device, commandQueue, srvHeap);
        viewport.SetShowSkeleton(false);    // world 뷰: 본 오버레이 숨김 (배치 전용)
        ikViewport.SetShowSkeleton(true);   // IK 뷰: 본 오버레이 표시

        // === 활성 Scene 상태 ===
        // 부팅은 항상 빈 default scene — sample.scene.json 자동 로드 안 함.
        // (Dragon 강제 표시 방지) 사용자가 File→Open 또는 Asset Browser 브러시로 자유 배치.
        engine::scene::Scene activeScene = BuildDefaultScene();
        std::string          activeScenePath;
        bool                 modified = false;

        // === SceneRuntime — activeScene 을 GPU 자원 (mesh/tex/anim) 으로 실체화 ===
        // 비어 있는 Scene 도 허용 — 단 try/catch 로 로드 실패 시 sceneRuntime=nullptr.
        std::unique_ptr<client::SceneRuntime> sceneRuntime;
        bool ikFocusDone = false;   // IK 뷰 카메라가 캐릭터에 자동 프레이밍됐는지 (씬 (재)생성 시 reset)
        auto rebuildSceneRuntime = [&]()
        {
            commandQueue.FlushGpu();
            sceneRuntime.reset();
            try
            {
                sceneRuntime = std::make_unique<client::SceneRuntime>(
                    device, commandQueue, viewport.BootCommandList(), srvHeap,
                    engine::scene::Scene(activeScene));
            }
            catch (const std::exception& e)
            {
                engine::core::LogInfoA("[editor] SceneRuntime 생성 실패: ");
                engine::core::LogInfoA(e.what());
                engine::core::LogInfoA("\n");
                sceneRuntime.reset();
            }
            ikFocusDone = false;   // 새 캐릭터 → IK 카메라 재프레이밍 필요
        };
        rebuildSceneRuntime();

        editor::panels::Selection selection{};
        editor::AssetBrowserState assetBrowserState{};
        editor::AnimatorPanelState animatorPanelState{};
        editor::AnimatorGraphState animatorGraphState{};

        // 본 조작 모드 — Phase 3. ikMode=true 면 선택 본을 끝 관절로 보고 드래그 시 부모 체인 CCD.
        bool ikMode      = false;
        int  ikChainLen  = 2;     // 굽힐 부모 관절 수 (2 = two-bone IK, 손→팔꿈치+어깨).

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

            const ImGuiID dockspaceId = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

            // 부팅 후 첫 프레임 1회: Unity/Unreal 스타일 기본 도킹 레이아웃.
            //   center  = Viewport (큰 영역)
            //   left    = Hierarchy
            //   right   = Inspector
            //   bottom  = Asset Browser
            // 사용자가 이후 패널 이동/리사이즈하면 imgui.ini 에 저장되어 다음 부팅에 복원.
            static bool sDockInit = false;
            if (!sDockInit)
            {
                sDockInit = true;
                ImGuiDockNode* existing = ImGui::DockBuilderGetNode(dockspaceId);
                // 이미 사용자가 커스터마이즈한 도킹이면 (분할 노드가 있으면) 그대로 둠.
                if (!existing || existing->IsLeafNode())
                {
                    ImGui::DockBuilderRemoveNode(dockspaceId);
                    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
                    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

                    ImGuiID dockMain  = dockspaceId;
                    ImGuiID dockLeft  = ImGui::DockBuilderSplitNode(dockMain,  ImGuiDir_Left,  0.18f, nullptr, &dockMain);
                    ImGuiID dockRight = ImGui::DockBuilderSplitNode(dockMain,  ImGuiDir_Right, 0.22f, nullptr, &dockMain);
                    ImGuiID dockBottom= ImGui::DockBuilderSplitNode(dockMain,  ImGuiDir_Down,  0.28f, nullptr, &dockMain);

                    ImGui::DockBuilderDockWindow("Hierarchy",      dockLeft);
                    ImGui::DockBuilderDockWindow("Inspector",      dockRight);
                    ImGui::DockBuilderDockWindow("Animator",       dockRight);
                    // Animator Graph 는 노드 그래프이므로 가로폭 충분한 하단 dock 에 탭으로 — Asset Browser 와 동일.
                    ImGui::DockBuilderDockWindow("Asset Browser",  dockBottom);
                    ImGui::DockBuilderDockWindow("Animator Graph", dockBottom);
                    ImGui::DockBuilderDockWindow("Viewport",       dockMain);
                    ImGui::DockBuilderDockWindow("IK",             dockMain);   // Viewport 와 같은 탭 그룹
                    ImGui::DockBuilderFinish(dockspaceId);
                }
            }

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

            // Hierarchy / AssetBrowser 의 *구조 변경* (add/remove/path bind) 은 SceneRuntime 재빌드 필요.
            // Inspector 의 *필드 변경* (transform, light 색 등) 은 modified 만 토글 — 재빌드 불필요.
            bool needSceneRebuild = false;

            // Animator + Animator Graph 패널이 동일 panelState 의 controller 공유 — Animator 탭이
            // 숨겨져 있어도 Graph 가 정상 표시되도록 매 프레임 reload 체크.
            editor::EnsureAnimatorLoaded(activeScene, selection, animatorPanelState);

            // === Hierarchy 패널 ===
            if (ImGui::Begin("Hierarchy"))
            {
                if (editor::panels::DrawHierarchy(activeScene, selection))
                {
                    modified = true;
                    needSceneRebuild = true;
                }
            }
            ImGui::End();

            // === Inspector 패널 ===
            // Inspector 의 transform/importTransform/light 편집은 자산 재로드 없이
            // SceneRuntime 의 m_scene 으로 cheap-copy 동기화 → 다음 프레임에 즉시 반영.
            // Drag-drop 또는 path 텍스트 직접 편집은 SceneRuntime 자산 reload 필요 → needRebuild.
            if (ImGui::Begin("Inspector"))
            {
                const editor::panels::InspectorResult ir =
                    editor::panels::DrawInspector(activeScene, selection);
                if (ir.changed)
                {
                    modified = true;
                    if (sceneRuntime) { sceneRuntime->SyncEditableFieldsFrom(activeScene); }
                }
                if (ir.needRebuild) { needSceneRebuild = true; }
            }
            ImGui::End();

            // === Animator 패널 ===
            // 선택된 MeshInstance 의 animatorControllerPath 가 가리키는 .animator.json 을
            // 로드해 파라미터 / state / transition 표시 + 편집. Float/Bool/Trigger 위젯의
            // 값은 즉시 SceneRuntime.SetAnimator* 로 전달. 구조 편집 + Save 클릭 시
            // JSON 저장 후 SceneRuntime 재빌드 필요 — DrawAnimatorPanel 가 true 반환.
            if (ImGui::Begin("Animator"))
            {
                if (editor::DrawAnimatorPanel(activeScene, selection,
                                              sceneRuntime.get(),
                                              animatorPanelState))
                {
                    needSceneRebuild = true;
                    // Animator JSON 저장 시 layout JSON 도 같이 저장 — 노드 위치 영속화.
                    if (animatorGraphState.layoutDirty)
                    {
                        editor::SaveAnimatorGraphLayout(animatorPanelState.loadedPath,
                                                       animatorGraphState);
                        animatorGraphState.layoutDirty = false;
                    }
                }
            }
            ImGui::End();

            // === Animator Graph 패널 ===
            // Animator 패널이 로드한 controller 를 시각적 노드+화살표 로 표시.
            // 화살표 클릭 → transition 선택 + inline editor — 편집 시 panelState.dirty 로 OR-in.
            // animatorJsonPath 는 layout JSON 파생용.
            if (ImGui::Begin("Animator Graph"))
            {
                bool graphDirty = false;
                editor::DrawAnimatorGraph(animatorPanelState.controller.get(),
                                          animatorPanelState.loadedPath,
                                          sceneRuntime.get(),
                                          animatorGraphState,
                                          graphDirty);
                if (graphDirty) { animatorPanelState.dirty = true; }
            }
            ImGui::End();

            // === Asset Browser 패널 ===
            if (ImGui::Begin("Asset Browser"))
            {
                if (editor::DrawAssetBrowser(activeScene, selection, assetBrowserState))
                {
                    modified = true;
                    needSceneRebuild = true;
                }
            }
            ImGui::End();

            // === Viewport 패널 (world 뷰) — 3D RTT + 오빗 카메라 + 브러시 배치. 본 편집은 IK 탭 ===
            bool viewportTabActive = false;
            if (ImGui::Begin("Viewport"))
            {
                viewportTabActive = true;
                const ImVec2 region = ImGui::GetContentRegionAvail();
                const auto rw = static_cast<std::uint32_t>(region.x > 1.0f ? region.x : 1.0f);
                const auto rh = static_cast<std::uint32_t>(region.y > 1.0f ? region.y : 1.0f);
                viewport.Resize(rw, rh);

                if (sceneRuntime)
                {
                    const ImVec2 imageOrigin = ImGui::GetCursorScreenPos();
                    ImGui::Image(static_cast<ImTextureID>(viewport.GpuSrvHandle().ptr), region);
                    const bool hovered = ImGui::IsItemHovered();
                    const ImGuiIO& io2 = ImGui::GetIO();

                    // RMB drag → orbit / 휠 → zoom (brush mode 와 독립).
                    viewport.UpdateInput(
                        io2.MouseDelta.x, io2.MouseDelta.y, io2.MouseWheel,
                        ImGui::IsMouseDown(ImGuiMouseButton_Right), hovered);

                    // 공통 배치 헬퍼 — meshPath 와 화면 좌표를 받아 raycast 후 인스턴스 push.
                    auto placeAt = [&](const std::string& meshPath, float screenX, float screenY)
                    {
                        DirectX::XMFLOAT3 hit{};
                        if (!viewport.RaycastToGround(screenX, screenY, hit)) { return; }
                        const AssetPlacementDefaults defaults = GuessAssetDefaults(meshPath);
                        engine::scene::MeshInstance m;
                        const auto stem = std::filesystem::path(meshPath).stem().string();
                        m.name                  = stem + "_" + std::to_string(activeScene.meshes.size());
                        m.meshAssetPath         = meshPath;
                        m.importTransform       = defaults.importTransform;
                        m.animatorControllerPath= defaults.defaultAnimatorPath;
                        m.transform.position    = hit;
                        activeScene.meshes.push_back(std::move(m));
                        selection.kind  = editor::panels::NodeKind::MeshInstance;
                        selection.index = activeScene.meshes.size() - 1;
                        modified         = true;
                        needSceneRebuild = true;
                    };

                    // Drag-and-drop: AssetBrowser 에서 메쉬 자산을 직접 Viewport 로 드롭.
                    if (ImGui::BeginDragDropTarget())
                    {
                        const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_MESH_PATH");
                        if (payload != nullptr && payload->Data != nullptr && payload->DataSize > 0)
                        {
                            const ImVec2 mp = ImGui::GetMousePos();
                            placeAt(std::string{ static_cast<const char*>(payload->Data) },
                                    mp.x - imageOrigin.x, mp.y - imageOrigin.y);
                        }
                        ImGui::EndDragDropTarget();
                    }

                    // LMB 클릭 — 브러시 모드면 배치 (본 picking 은 IK 탭 전용).
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) &&
                        !assetBrowserState.brushMeshPath.empty())
                    {
                        const ImVec2 mp = ImGui::GetMousePos();
                        placeAt(assetBrowserState.brushMeshPath, mp.x - imageOrigin.x, mp.y - imageOrigin.y);
                    }

                    // ESC → 브러시 해제 (Viewport 호버 시).
                    if (hovered && ImGui::IsKeyPressed(ImGuiKey_Escape) &&
                        !assetBrowserState.brushMeshPath.empty())
                    {
                        assetBrowserState.brushMeshPath.clear();
                    }

                    // 브러시 오버레이 — Viewport 좌상단에 표시.
                    if (!assetBrowserState.brushMeshPath.empty())
                    {
                        const auto fname = std::filesystem::path(
                            assetBrowserState.brushMeshPath).filename().string();
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        char buf[256];
                        std::snprintf(buf, sizeof(buf), "Brush: %s  (LMB place / ESC clear)", fname.c_str());
                        dl->AddText(ImVec2{ imageOrigin.x + 10.0f, imageOrigin.y + 10.0f },
                                    IM_COL32(255, 220, 80, 255), buf);
                    }
                }
                else
                {
                    ImGui::TextDisabled("(SceneRuntime 없음 — Scene 로드 실패?)");
                    ImGui::TextWrapped("Status: %s", lastStatus.c_str());
                }
            }
            ImGui::End();

            // === IK 패널 (IK 뷰) — 전용 3D 뷰포트에서 본 선택 + IK/FK 드래그 ===
            //   world Viewport 와 분리: 여기서만 본을 클릭/드래그하므로 배치/조작과 충돌 없음.
            //   같은 center dock 의 탭이라 활성 탭만 렌더됨 (cbuffer 공유 충돌 회피).
            bool ikTabActive = false;
            if (ImGui::Begin("IK"))
            {
                ikTabActive = true;
                if (sceneRuntime && sceneRuntime->HasAnimatorRuntime())
                {
                    // --- 상단 컨트롤 바 ---
                    ImGui::Checkbox("IK 모드", &ikMode);
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip("켜기: 끝 관절(손/발) 드래그 -> 부모 체인 굽힘 (IK).\n"
                                          "끄기: 선택 본만 제자리 회전 (FK).");
                    }
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(120.0f);
                    ImGui::SliderInt("체인", &ikChainLen, 1, 4);
                    ImGui::SameLine();
                    if (ImGui::Button("포즈 리셋"))
                    {
                        sceneRuntime->ClearBoneManualPosing();
                        modified = true;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("포커스")) { ikFocusDone = false; }   // 캐릭터에 카메라 재프레이밍
                    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("카메라를 캐릭터에 맞춰 가운데로."); }
                    // 주의: SameLine() 은 *실제로 버튼이 그려질 때만* 호출 — 조건이 false 면 dangling
                    //   SameLine 이 다음 항목(3D Image)에 적용돼 이미지가 툴바 옆으로 밀리고 좌측 공백 발생.
                    if (ikViewport.SelectedBone() >= 0)
                    {
                        ImGui::SameLine();
                        if (ImGui::Button("선택 해제")) { ikViewport.SetSelectedBone(-1); }
                    }

                    // 캐릭터 자동 프레이밍 — IK 뷰 진입/씬 변경 시 1회. 본 joints 의 world bbox 중심·반경
                    //   으로 카메라를 맞춰 캐릭터가 한쪽으로 치우치거나 공백이 남지 않게.
                    if (!ikFocusDone)
                    {
                        std::vector<DirectX::XMFLOAT3> jp;
                        std::vector<int>              jpar;
                        std::vector<std::string>      jn;
                        if (sceneRuntime->GetSkeletonWorldJoints(jp, jpar, jn) && !jp.empty())
                        {
                            DirectX::XMFLOAT3 mn = jp[0], mx = jp[0];
                            for (const auto& p : jp)
                            {
                                mn.x = std::min(mn.x, p.x); mn.y = std::min(mn.y, p.y); mn.z = std::min(mn.z, p.z);
                                mx.x = std::max(mx.x, p.x); mx.y = std::max(mx.y, p.y); mx.z = std::max(mx.z, p.z);
                            }
                            const DirectX::XMFLOAT3 c{ (mn.x+mx.x)*0.5f, (mn.y+mx.y)*0.5f, (mn.z+mx.z)*0.5f };
                            const float dx = mx.x-mn.x, dy = mx.y-mn.y, dz = mx.z-mn.z;
                            const float radius = 0.5f * std::sqrt(dx*dx + dy*dy + dz*dz);
                            // 첫 프레임엔 animator 가 BoneGlobal 미계산 → 전 관절이 한 점(=bbox 0)에
                            //   뭉쳐 잘못 프레이밍됨. 유효한 spread 가 생긴 뒤에만 확정.
                            if (radius > 10.0f)
                            {
                                ikViewport.FocusOn(c, radius);
                                ikFocusDone = true;
                            }
                        }
                    }

                    // --- IK 전용 3D 뷰포트 ---
                    const ImVec2 region = ImGui::GetContentRegionAvail();
                    const auto rw = static_cast<std::uint32_t>(region.x > 1.0f ? region.x : 1.0f);
                    const auto rh = static_cast<std::uint32_t>(region.y > 1.0f ? region.y : 1.0f);
                    ikViewport.Resize(rw, rh);

                    const ImVec2 imageOrigin = ImGui::GetCursorScreenPos();
                    ImGui::Image(static_cast<ImTextureID>(ikViewport.GpuSrvHandle().ptr), region);
                    const bool hovered = ImGui::IsItemHovered();
                    const ImGuiIO& io2 = ImGui::GetIO();

                    // RMB drag → orbit / 휠 → zoom.
                    ikViewport.UpdateInput(io2.MouseDelta.x, io2.MouseDelta.y, io2.MouseWheel,
                                           ImGui::IsMouseDown(ImGuiMouseButton_Right), hovered);

                    // LMB 클릭 → 가장 가까운 본 선택 (없으면 해제).
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
                    {
                        const ImVec2 mp = ImGui::GetMousePos();
                        ikViewport.PickBone(*sceneRuntime, mp.x - imageOrigin.x, mp.y - imageOrigin.y);
                    }

                    // 본 선택 + LMB 드래그 → IK(부모 체인 굽힘) 또는 FK(선택 본 회전).
                    if (ikViewport.SelectedBone() >= 0 && hovered &&
                        ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f))
                    {
                        if (ikMode)
                        {
                            std::vector<DirectX::XMFLOAT3> jp;
                            std::vector<int>              jpar;
                            std::vector<std::string>      jn;
                            if (sceneRuntime->GetSkeletonWorldJoints(jp, jpar, jn) &&
                                static_cast<size_t>(ikViewport.SelectedBone()) < jp.size())
                            {
                                const ImVec2 mp = ImGui::GetMousePos();
                                const DirectX::XMFLOAT3 endWorld =
                                    jp[static_cast<size_t>(ikViewport.SelectedBone())];
                                DirectX::XMFLOAT3 target;
                                if (ikViewport.ScreenToWorldAtDepth(mp.x - imageOrigin.x,
                                                                    mp.y - imageOrigin.y,
                                                                    endWorld, target))
                                {
                                    sceneRuntime->SolveBoneIK(ikViewport.SelectedBone(), target,
                                                              ikChainLen, /*iterations*/10);
                                    modified = true;
                                }
                            }
                        }
                        else
                        {
                            const ImVec2 d = io2.MouseDelta;
                            if (d.x != 0.0f || d.y != 0.0f)
                            {
                                constexpr float kRotSpeed = 0.01f;   // rad / px
                                DirectX::XMFLOAT3 camRight, camUp;
                                ikViewport.CameraRightUp(camRight, camUp);
                                sceneRuntime->AddBoneManualRotation(ikViewport.SelectedBone(), camUp,    -d.x * kRotSpeed);
                                sceneRuntime->AddBoneManualRotation(ikViewport.SelectedBone(), camRight, -d.y * kRotSpeed);
                                modified = true;
                            }
                        }
                    }

                    // ESC → 본 선택 해제 (호버 시).
                    if (hovered && ImGui::IsKeyPressed(ImGuiKey_Escape))
                    {
                        ikViewport.SetSelectedBone(-1);
                    }

                    // 오버레이 — 선택 본 이름 + 현재 모드.
                    {
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        char buf[256];
                        std::snprintf(buf, sizeof(buf), "%s  |  %s",
                                      ikMode ? "IK (끝 관절 끌기)" : "FK (선택 본 회전)",
                                      sceneRuntime->HasBoneManualPosing() ? "수정됨" : "원본");
                        dl->AddText(ImVec2{ imageOrigin.x + 10.0f, imageOrigin.y + 10.0f },
                                    IM_COL32(255, 220, 80, 255), buf);
                        if (ikViewport.SelectedBone() >= 0)
                        {
                            std::vector<DirectX::XMFLOAT3> jp;
                            std::vector<int>              jpar;
                            std::vector<std::string>      jn;
                            if (sceneRuntime->GetSkeletonWorldJoints(jp, jpar, jn) &&
                                static_cast<size_t>(ikViewport.SelectedBone()) < jn.size())
                            {
                                char b2[256];
                                std::snprintf(b2, sizeof(b2), "Bone [%d]: %s",
                                              ikViewport.SelectedBone(),
                                              jn[static_cast<size_t>(ikViewport.SelectedBone())].c_str());
                                dl->AddText(ImVec2{ imageOrigin.x + 10.0f, imageOrigin.y + 28.0f },
                                            IM_COL32(80, 255, 255, 255), b2);
                            }
                        }
                        else
                        {
                            dl->AddText(ImVec2{ imageOrigin.x + 10.0f, imageOrigin.y + 28.0f },
                                        IM_COL32(160, 160, 160, 255), "본을 클릭해 선택");
                        }
                    }
                }
                else
                {
                    ImGui::TextDisabled("(스켈레톤이 있는 캐릭터를 로드하세요)");
                }
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
                rebuildSceneRuntime();
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
                        rebuildSceneRuntime();
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

            // === Hierarchy/AssetBrowser 의 구조 변경 → SceneRuntime 재빌드 ===
            // 메뉴 (wantNew/wantOpen) 이 별도 rebuildSceneRuntime() 을 호출했더라도 1회만 실행되도록 가드.
            if (needSceneRebuild && !wantNew && !wantOpen)
            {
                // 이번 프레임 in-flight 자원 안전 해제 — rebuildSceneRuntime 내부에서 FlushGpu.
                for (auto& v : frameFenceValues) { v = 0; }
                rebuildSceneRuntime();
            }

            // === 이 슬롯 fence 대기 → 명령 기록 ===
            if (frameFenceValues[frameIndex] != 0)
            {
                commandQueue.WaitForFenceValue(frameFenceValues[frameIndex]);
            }

            engine::render::CommandList& cmdList = *cmdLists[frameIndex];
            cmdList.Reset();
            ID3D12GraphicsCommandList* list = cmdList.Native();

            // === SceneRuntime Tick + 3D Viewport 렌더 (RTT) — ImGui draw 이전에 ===
            // viewport.Render() 의 종단에서 RTT 는 SHADER_RESOURCE 상태로 전이 — ImGui::Image 가 sample 가능.
            if (sceneRuntime)
            {
                sceneRuntime->Tick(io.DeltaTime);
                // 활성 탭의 뷰포트만 RTT 렌더 — Viewport/IK 는 같은 dock 의 탭이라 한쪽만 active.
                if (viewportTabActive) { viewport.Render(list, *sceneRuntime, frameIndex); }
                if (ikTabActive)       { ikViewport.Render(list, *sceneRuntime, frameIndex); }
            }

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
