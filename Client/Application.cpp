#include "Application.h"

#include "FrameRenderer.h"
#include "Player.h"
#include "SceneRuntime.h"

#include "core/Logger.h"
#include "platform/Input.h"
#include "platform/Window.h"
#include "render/Camera.h"
#include "render/CommandList.h"
#include "render/CommandQueue.h"
#include "render/DepthStencilBuffer.h"
#include "render/Device.h"
#include "render/FbxLoader.h"
#include "render/FreeCamera.h"
#include "render/ImageLoader.h"
#include "render/PipelineState.h"
#include "render/RootSignature.h"
#include "render/RtvDescriptorHeap.h"
#include "render/ShaderCompiler.h"
#include "render/SrvDescriptorHeap.h"
#include "render/SwapChain.h"
#include "render/Texture.h"
#include "render/ThirdPersonCamera.h"
#include "scene/Scene.h"
#include "scene/SceneSerializer.h"

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx12.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <stdexcept>

// ImGui 의 Win32 backend WndProc 핸들러 — Window 의 hook 으로 등록.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace client
{
    namespace
    {
        constexpr float kNearPlane = 1.0f;
        constexpr float kFarPlane  = 5000.0f;

        // ImGui SRV bump-allocator — Editor 의 동일 패턴. srvHeap 은 외부 소유 (비소유 참조).
        class ImGuiSrvAllocator final
        {
        public:
            explicit ImGuiSrvAllocator(engine::render::SrvDescriptorHeap& heap) : m_heap(heap) {}
            ImGuiSrvAllocator(const ImGuiSrvAllocator&)            = delete;
            ImGuiSrvAllocator& operator=(const ImGuiSrvAllocator&) = delete;

            static void Alloc(ImGui_ImplDX12_InitInfo* info,
                              D3D12_CPU_DESCRIPTOR_HANDLE* outCpu,
                              D3D12_GPU_DESCRIPTOR_HANDLE* outGpu)
            {
                auto* self = static_cast<ImGuiSrvAllocator*>(info->UserData);
                const auto h = self->m_heap.Allocate();
                *outCpu = h.cpu;
                *outGpu = h.gpu;
            }
            static void Free(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE) {}

        private:
            engine::render::SrvDescriptorHeap& m_heap;
        };

        engine::scene::Scene BuildDefaultScene()
        {
            engine::scene::Scene s;
            s.name = "default-fallback";
            s.ambient = { 0.15f, 0.15f, 0.18f };
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
    }

    Application::Application(int widthPx, int heightPx, const std::wstring& title)
        : m_widthPx(widthPx), m_heightPx(heightPx), m_title(title)
    {
        InitGraphicsCore();
        InitGraphicsPipeline();
        LoadSceneAndRuntime();
        InitRendererAndInput();
        InitImGui();
    }

    Application::~Application()
    {
        // 메인 루프 종료 후 호출됨 — GPU 작업 모두 완료된 상태.
        // 안전 차원에서 한 번 더 FlushGpu (소멸 순서 보장).
        if (m_queue)
        {
            m_queue->FlushGpu();
        }
        ShutdownImGui();
    }

    void Application::InitImGui()
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::StyleColorsDark();

        if (!ImGui_ImplWin32_Init(m_window->NativeHwnd()))
        {
            throw std::runtime_error("ImGui_ImplWin32_Init failed");
        }

        auto* alloc = new ImGuiSrvAllocator(*m_srvHeap);
        m_imguiSrvAllocator = alloc;

        ImGui_ImplDX12_InitInfo dxInit{};
        dxInit.Device               = m_device->Native();
        dxInit.CommandQueue         = m_queue->Native();
        dxInit.NumFramesInFlight    = static_cast<int>(engine::render::SwapChain::kBackBufferCount);
        dxInit.RTVFormat            = DXGI_FORMAT_R8G8B8A8_UNORM;
        dxInit.DSVFormat            = DXGI_FORMAT_UNKNOWN;
        dxInit.SrvDescriptorHeap    = m_srvHeap->Native();
        dxInit.SrvDescriptorAllocFn = &ImGuiSrvAllocator::Alloc;
        dxInit.SrvDescriptorFreeFn  = &ImGuiSrvAllocator::Free;
        dxInit.UserData             = alloc;
        if (!ImGui_ImplDX12_Init(&dxInit))
        {
            throw std::runtime_error("ImGui_ImplDX12_Init failed");
        }

        m_window->SetWndProcHook(
            [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT
            {
                return ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
            });

        m_imguiInitialized = true;
        engine::core::LogInfo(L"[imgui] ImGui Win32 + DX12 backend initialized\n");
    }

    void Application::ShutdownImGui()
    {
        if (!m_imguiInitialized) { return; }
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        delete static_cast<ImGuiSrvAllocator*>(m_imguiSrvAllocator);
        m_imguiSrvAllocator = nullptr;
        m_imguiInitialized = false;
    }

    void Application::DrawAnimatorPanel()
    {
        if (!m_sceneRuntime || !m_sceneRuntime->HasAnimatorRuntime()) { return; }

        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(420, 260), ImGuiCond_FirstUseEver);
        ImGui::Begin("Animator Debug");

        const std::string state = m_sceneRuntime->AnimatorCurrentStateName();
        const float duration    = m_sceneRuntime->AnimatorStateDuration(state);
        const float stateTime   = m_sceneRuntime->AnimatorCurrentStateTime();

        ImGui::Text("State: %s", state.empty() ? "(none)" : state.c_str());
        ImGui::Text("stateTime: %.3f s / duration: %.3f s", stateTime, duration);

        if (duration > 0.05f)
        {
            constexpr float kFps = 30.0f;
            const int curFrame   = static_cast<int>(stateTime * kFps + 0.5f);
            const int totalFrame = static_cast<int>(duration  * kFps + 0.5f);
            const float pct      = stateTime / duration;
            char overlay[64];
            std::snprintf(overlay, sizeof(overlay), "frame %d / %d  (%.1f%%)",
                          curFrame, totalFrame, pct * 100.0f);
            ImGui::ProgressBar(pct, ImVec2(-FLT_MIN, 0), overlay);

            int sliderFrame = curFrame;
            if (ImGui::SliderInt("Frame", &sliderFrame, 0, totalFrame))
            {
                const float newTime = static_cast<float>(sliderFrame) / kFps;
                m_sceneRuntime->AnimatorSetCurrentStateTime(newTime);
            }
        }
        else
        {
            ImGui::Text("duration unavailable");
        }

        bool paused = m_sceneRuntime->AnimatorIsPaused();
        if (ImGui::Button(paused ? "Play" : "Pause"))
        {
            m_sceneRuntime->AnimatorSetPaused(!paused);
        }
        ImGui::SameLine();
        ImGui::Text("(Space=Jump, 1=Walk)");

        ImGui::Separator();
        const auto* xform = m_sceneRuntime->AnimatorInstanceTransform();
        ImGui::Text("inst.transform.y = %.2f", xform ? xform->position.y : 0.0f);
        const float footY = m_sceneRuntime->AnimatorBoneMeshLocalY(L"LeftFoot");
        ImGui::Text("LeftFoot mesh-local Y = %.2f (rootMotion baseline 튜닝 참고)", footY);

        ImGui::End();
    }

    void Application::InitGraphicsCore()
    {
        m_window      = std::make_unique<engine::platform::Window>(m_widthPx, m_heightPx, m_title);
        m_device      = std::make_unique<engine::render::Device>();
        m_queue       = std::make_unique<engine::render::CommandQueue>(*m_device);
        m_rtvHeap     = std::make_unique<engine::render::RtvDescriptorHeap>(*m_device,
                          engine::render::SwapChain::kBackBufferCount);
        m_swap        = std::make_unique<engine::render::SwapChain>(*m_device, *m_queue, *m_window, *m_rtvHeap);
        m_depth       = std::make_unique<engine::render::DepthStencilBuffer>(
                          *m_device,
                          static_cast<engine::uint32>(m_window->Width()),
                          static_cast<engine::uint32>(m_window->Height()),
                          DXGI_FORMAT_D32_FLOAT);
        m_srvHeap     = std::make_unique<engine::render::SrvDescriptorHeap>(*m_device, 64);
        m_bootCmdList = std::make_unique<engine::render::CommandList>(*m_device);

        // Fallback albedo — neutral gray 1×1 (Mesh.cpp 가 텍스처 없는 SubMesh 에 입힘).
        //   과거에는 Leather.jpg 였는데 Mixamo without-skin X-Bot 같이 *body texture 없는*
        //   자산에 갈색 가죽 무늬가 어색하게 입혀졌다. plain gray 면 색상만 변경되어 자연스러움.
        const std::uint8_t grayRgba[4] = { 200, 200, 200, 255 };
        m_fallbackAlbedo = std::make_unique<engine::render::Texture>(
            *m_device, *m_queue, *m_bootCmdList,
            grayRgba, 1, 1);
        m_fallbackAlbedo->CreateSrv(*m_device, *m_srvHeap);
    }

    void Application::InitGraphicsPipeline()
    {
        const std::wstring shaderDir  = engine::render::ShaderCompiler::DefaultShaderDir();
        const std::wstring shaderPath = shaderDir + L"HelloTriangle.hlsl";

        m_vsBlob = engine::render::ShaderCompiler::CompileFromFile(
            shaderPath.c_str(), "VSMain", engine::render::ShaderCompiler::Stage::Vertex);
        m_psBlob = engine::render::ShaderCompiler::CompileFromFile(
            shaderPath.c_str(), "PSMain", engine::render::ShaderCompiler::Stage::Pixel);

        // 슬롯 순서: [0]b0 frame / [1]b1 bones VS / [2]t0 material table PS / [3]t1 dirLights / [4]t2 pointLights.
        engine::render::RootSignature::Desc rsDesc{};
        rsDesc.cbvAtB0     = engine::render::RootSignature::Desc::CbvB0::All;
        rsDesc.cbvB1Vertex = true;
        rsDesc.srvT0Pixel  = true;
        rsDesc.srvT1Pixel  = true;
        rsDesc.srvT2Pixel  = true;
        m_rootSig = std::make_unique<engine::render::RootSignature>(*m_device, rsDesc);

        engine::render::PipelineState::Desc psoDesc{};
        psoDesc.vertexShader  = m_vsBlob.Get();
        psoDesc.pixelShader   = m_psBlob.Get();
        psoDesc.rootSignature = m_rootSig.get();
        psoDesc.rtvFormat     = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoDesc.dsvFormat     = m_depth->Format();
        m_pso = std::make_unique<engine::render::PipelineState>(*m_device, psoDesc);
    }

    void Application::ScanSceneSlots()
    {
        m_sceneSlots.clear();
        std::filesystem::path dir = "assets/Scenes";
        std::error_code ec;
        if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec))
        {
            return;
        }
        std::vector<std::filesystem::path> found;
        for (const auto& entry : std::filesystem::directory_iterator{ dir, ec })
        {
            if (!entry.is_regular_file()) { continue; }
            const auto& p = entry.path();
            // .scene.json — 확장자 둘 합쳐서 매칭.
            const std::string name = p.filename().string();
            constexpr std::string_view suffix = ".scene.json";
            if (name.size() <= suffix.size()) { continue; }
            if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) { continue; }
            found.push_back(p);
        }
        std::sort(found.begin(), found.end());
        const size_t cap = (found.size() < static_cast<size_t>(InputController::kSceneSwitchSlotCount))
            ? found.size()
            : static_cast<size_t>(InputController::kSceneSwitchSlotCount);
        m_sceneSlots.reserve(cap);
        for (size_t i = 0; i < cap; ++i)
        {
            m_sceneSlots.push_back(found[i].string());
        }

        if (!m_sceneSlots.empty())
        {
            engine::core::LogInfoA("[scene] slots scanned:\n");
            for (size_t i = 0; i < m_sceneSlots.size(); ++i)
            {
                char buf[512];
                std::snprintf(buf, sizeof(buf), "  F%zu = %s\n", i + 1, m_sceneSlots[i].c_str());
                engine::core::LogInfoA(buf);
            }
        }
    }

    void Application::LoadSceneAndRuntime()
    {
        ScanSceneSlots();

        // 부팅 씬 — 슬롯 0 (알파벳 순 첫 .scene.json) 우선, 없으면 default.
        engine::scene::Scene scene;
        if (!m_sceneSlots.empty())
        {
            const std::string& path = m_sceneSlots.front();
            try
            {
                scene = engine::scene::LoadJson(path);
                m_currentScenePath = path;
                engine::core::LogInfoA("[scene] boot loaded: ");
                engine::core::LogInfoA(path.c_str());
                engine::core::LogInfoA("\n");
            }
            catch (const std::exception& e)
            {
                engine::core::LogInfoA("[scene] boot load failed, fallback: ");
                engine::core::LogInfoA(e.what());
                engine::core::LogInfoA("\n");
                scene = BuildDefaultScene();
            }
        }
        else
        {
            engine::core::LogInfo(L"[scene] assets/Scenes 비어있음 - default Scene 사용\n");
            scene = BuildDefaultScene();
        }

        // 카메라 — Scene 의 cameraStart 기준.
        m_camera = std::make_unique<engine::render::Camera>();
        m_camera->SetPosition(scene.cameraStart.position);
        m_camera->SetTarget  (scene.cameraStart.target);
        m_camera->SetUp      ({ 0.0f, 1.0f, 0.0f });
        m_camera->SetPerspective(
            scene.cameraStart.fovYRad,
            static_cast<float>(m_window->Width()) / static_cast<float>(m_window->Height()),
            kNearPlane, kFarPlane);

        m_freeCamera = std::make_unique<engine::render::FreeCamera>(*m_camera);
        m_freeCamera->SetMoveSpeed(100.0f);

        m_thirdPersonCamera = std::make_unique<engine::render::ThirdPersonCamera>(*m_camera);

        // Player — CharacterController + transform write-back. SceneRuntime 생성 후 Bind() 호출.
        m_player = std::make_unique<Player>();

        // SceneRuntime — Scene 의 owner 가 SceneRuntime 으로 이동.
        m_sceneRuntime = std::make_unique<SceneRuntime>(
            *m_device, *m_queue, *m_bootCmdList, *m_srvHeap, std::move(scene));

        // Player 의 transform 바인딩 — AnimatorInstanceTransform() 이 nullptr 면 silent unbound.
        m_player->Bind(m_sceneRuntime->AnimatorInstanceTransform());

        // 부팅 씬 타이틀 표시.
        if (!m_currentScenePath.empty())
        {
            const int wlen = ::MultiByteToWideChar(CP_UTF8, 0, m_currentScenePath.c_str(), -1, nullptr, 0);
            std::wstring wpath(static_cast<size_t>(wlen > 0 ? wlen - 1 : 0), L'\0');
            if (wlen > 0)
            {
                ::MultiByteToWideChar(CP_UTF8, 0, m_currentScenePath.c_str(), -1, wpath.data(), wlen);
            }
            m_window->SetTitle(L"portfolio_engine - " + wpath);
        }

        // 부트 전용 CommandList — Scene 로드 끝나도 Application 라이프타임 유지.
        // GPU 가 fallback texture / FBX 업로드 명령을 처리하는 동안 CommandList COM 객체가
        // 살아있어야 안전. 즉시 reset 시 GPU 가 invalid pointer 참조 위험.
        // (FrameRenderer 는 별도 in-flight CommandList 슬롯을 자체 보유 — 충돌 없음.)
    }

    void Application::InitRendererAndInput()
    {
        FrameRenderer::InitInfo info{};
        info.device         = m_device.get();
        info.queue          = m_queue.get();
        info.swapChain      = m_swap.get();
        info.depthBuffer    = m_depth.get();
        info.rootSig        = m_rootSig.get();
        info.pso            = m_pso.get();
        info.srvHeap        = m_srvHeap.get();
        info.fallbackAlbedo = m_fallbackAlbedo.get();
        m_frameRenderer = std::make_unique<FrameRenderer>(info);

        if (m_sceneRuntime->ClipCount() > 0)
        {
            engine::core::LogInfo(L"[input] 0=T-pose, 1..4=clip select.\n");
        }
        if (m_sceneSlots.size() > 1)
        {
            engine::core::LogInfo(L"[input] F1..F9 = scene slot select.\n");
        }
    }

    void Application::ChangeScene(const std::string& scenePath)
    {
        engine::core::LogInfoA("[scene] ChangeScene begin: ");
        engine::core::LogInfoA(scenePath.c_str());
        engine::core::LogInfoA("\n");

        // 로딩 진행 즉시 가시화 — 타이틀바에 "Loading: <path>".
        {
            const int wlen = ::MultiByteToWideChar(CP_UTF8, 0, scenePath.c_str(), -1, nullptr, 0);
            std::wstring wpath(static_cast<size_t>(wlen > 0 ? wlen - 1 : 0), L'\0');
            if (wlen > 0)
            {
                ::MultiByteToWideChar(CP_UTF8, 0, scenePath.c_str(), -1, wpath.data(), wlen);
            }
            m_window->SetTitle(L"portfolio_engine - Loading: " + wpath);
        }

        // === Build phase (실패 무손실) ===
        // LoadJson 이 가장 깨지기 쉬운 step (파일 부재 / 파싱 오류). 기존 SceneRuntime 폐기 *전*
        // 에 시도 — 실패 시 throw 가 기존 자산을 보존한 채 호출자로 전파됨 (강한 예외 보장).
        engine::scene::Scene newScene = engine::scene::LoadJson(scenePath);

        // === Teardown phase (이 시점부터 실패 시 기존 자산 회복 불가 — 약한 예외 보장) ===
        // ① GPU 작업 완료 보장 — 기존 자산이 GPU 에서 미사용 상태.
        m_queue->FlushGpu();
        // Player 의 transform 포인터 dangling 회피 — SceneRuntime reset 전 unbind.
        if (m_player) { m_player->Bind(nullptr); }
        // ② 기존 SceneRuntime 폐기 — 인스턴스 CB / 라이트 SB / Animator / 자산 캐시 release.
        m_sceneRuntime.reset();
        // ③ SrvHeap 슬롯 카운터 reset + fallback SRV 슬롯 0 재등록.
        //    bump-allocate 라 reset 직후 첫 Allocate 가 슬롯 0 보장.
        m_srvHeap->Reset();
        m_fallbackAlbedo->CreateSrv(*m_device, *m_srvHeap);

        // === Build SceneRuntime ===
        // 카메라 reset — 새 Scene 의 cameraStart.
        m_camera->SetPosition(newScene.cameraStart.position);
        m_camera->SetTarget  (newScene.cameraStart.target);
        m_camera->SetPerspective(
            newScene.cameraStart.fovYRad,
            static_cast<float>(m_window->Width()) / static_cast<float>(m_window->Height()),
            kNearPlane, kFarPlane);

        // 새 SceneRuntime — m_bootCmdList 재사용 (Application 라이프타임 보존).
        // 이 단계 throw 시 (GPU 자원 부족 등 드문 케이스) m_sceneRuntime == nullptr 로 남음.
        // Tick 시작의 nullptr 가드가 한 프레임 skip 으로 안전.
        m_sceneRuntime = std::make_unique<SceneRuntime>(
            *m_device, *m_queue, *m_bootCmdList, *m_srvHeap, std::move(newScene));

        // Player 의 transform 재바인딩 — 기존 instance 가 폐기되었으므로 새 ptr 로 갱신.
        if (m_player)
        {
            m_player->Bind(m_sceneRuntime->AnimatorInstanceTransform());
        }

        // Jump hipX bind 자료 reset — 씬 변경 시 새 자산의 baseline 다시 캡처 필요.
        m_hipBindXCaptured  = false;
        m_hipBindX          = 0.0f;

        // FrameRenderer 의 in-flight fence value reset — 새 슬롯들이 미사용 상태.
        // (Texture/FbxLoader 가 ctor 안에서 자체 FlushGpu 하므로 별도 FlushGpu 불필요.)
        m_frameRenderer->OnResize();

        m_currentScenePath = scenePath;

        // 타이틀바 갱신 — Loading 표시 제거.
        {
            const int wlen = ::MultiByteToWideChar(CP_UTF8, 0, scenePath.c_str(), -1, nullptr, 0);
            std::wstring wpath(static_cast<size_t>(wlen > 0 ? wlen - 1 : 0), L'\0');
            if (wlen > 0)
            {
                ::MultiByteToWideChar(CP_UTF8, 0, scenePath.c_str(), -1, wpath.data(), wlen);
            }
            m_window->SetTitle(L"portfolio_engine - " + wpath);
        }

        engine::core::LogInfoA("[scene] switched to: ");
        engine::core::LogInfoA(scenePath.c_str());
        engine::core::LogInfoA("\n");
    }

    void Application::Run()
    {
        const auto startTime = std::chrono::steady_clock::now();
        auto       prevFrame = startTime;

        while (m_window->IsOpen())
        {
            const auto now = std::chrono::steady_clock::now();
            const float dt = std::chrono::duration<float>(now - prevFrame).count();
            prevFrame = now;

            Tick(dt);
        }
    }

    void Application::Tick(float dt)
    {
        m_window->PumpMessages();
        if (!m_window->IsOpen()) { return; }

        // ImGui frame 시작 — Tick 본 작업 후 ImGui::Render 로 마무리.
        if (m_imguiInitialized)
        {
            ImGui_ImplDX12_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
        }

        // 비정상 상태 가드 — ChangeScene 의 Build SceneRuntime 단계 실패 시 nullptr.
        // WM_CLOSE 가 다음 PumpMessages 에서 처리되므로 한 프레임만 skip 후 메인 루프 종료.
        if (!m_sceneRuntime) { return; }

        // 리사이즈 처리.
        if (m_window->ConsumeResize())
        {
            m_queue->FlushGpu();
            m_frameRenderer->OnResize();

            const auto w = static_cast<engine::uint32>(m_window->Width());
            const auto h = static_cast<engine::uint32>(m_window->Height());
            m_swap->Resize(*m_device, w, h);
            m_depth->Resize(*m_device, w, h);

            m_camera->SetPerspective(
                m_sceneRuntime->InitialCameraStart().fovYRad,
                static_cast<float>(w) / static_cast<float>(h),
                kNearPlane, kFarPlane);
        }

        // 입력 갱신 + 클립/씬 전환 트리거.
        m_window->GetInput().BeginFrame();
        m_inputController.Tick(m_window->GetInput());

        // 씬 전환은 클립 전환보다 먼저 처리 — SceneRuntime 가 폐기·재생성되므로 그 후의 클립 전환은
        // 새 SceneRuntime 에 적용. 같은 프레임에 F1+1 동시 누름은 자연스럽게 새 씬 + clip 0 로 끝남.
        const int sceneSlot = m_inputController.ConsumeSceneSwitch();
        if (sceneSlot != InputController::kNoSceneSwitch)
        {
            char buf[80];
            std::snprintf(buf, sizeof(buf), "[input] scene switch requested: slot %d (F%d)\n",
                          sceneSlot, sceneSlot + 1);
            engine::core::LogInfoA(buf);
        }
        if (sceneSlot != InputController::kNoSceneSwitch
            && static_cast<size_t>(sceneSlot) >= m_sceneSlots.size())
        {
            // OOB — 슬롯 수보다 큰 F-키. 조용히 skip 하면 사용자 혼란.
            char buf[120];
            std::snprintf(buf, sizeof(buf),
                          "[input] slot %d (F%d) ignored — only %zu slot(s) available\n",
                          sceneSlot, sceneSlot + 1, m_sceneSlots.size());
            engine::core::LogInfoA(buf);
        }
        if (sceneSlot != InputController::kNoSceneSwitch
            && static_cast<size_t>(sceneSlot) < m_sceneSlots.size())
        {
            const std::string& path = m_sceneSlots[static_cast<size_t>(sceneSlot)];
            if (path != m_currentScenePath)
            {
                try
                {
                    ChangeScene(path);
                }
                catch (const std::exception& e)
                {
                    engine::core::LogInfoA("[scene] switch FAILED: ");
                    engine::core::LogInfoA(e.what());
                    engine::core::LogInfoA("\n");
                    // ChangeScene 이 SceneRuntime 폐기 후 throw 한 경우 — 다음 프레임 Render 가 nullptr
                    // 참조하므로 종료. 실 운용에선 sentinel scene 으로 폴백 권장.
                    ::PostMessageW(m_window->NativeHwnd(), WM_CLOSE, 0, 0);
                    return;
                }
            }
        }

        const int clipChange = m_inputController.ConsumeClipChange();
        if (clipChange != InputController::kNoClipChange)
        {
            m_sceneRuntime->SetActiveClip(clipChange);
        }

        // F 키 토글 — 3인칭 (Player + ThirdPersonCamera) 와 FreeCamera 전환.
        //   down edge 만 처리 (hold 시 매 프레임 toggle 방지). VK 'F' (0x46) 는 VK_F1 (0x70) 와
        //   다른 코드 — F1..F9 씬 슬롯과 충돌 없음.
        {
            const auto& input = m_window->GetInput();
            const bool curToggle = input.IsKeyDown(static_cast<std::uint32_t>('F'));
            if (curToggle && !m_prevToggleKeyDown)
            {
                m_thirdPersonActive = !m_thirdPersonActive;
                if (m_thirdPersonActive && m_player)
                {
                    // 토글 시 카메라 yaw 기준으로 캐릭터 위치를 카메라 앞에 맞추진 않음 — 현재 위치
                    // 유지 (transform.position 그대로). ThirdPersonCamera 가 다음 Update 에서
                    // 캐릭터 따라잡음.
                    engine::core::LogInfoA("[input] camera = ThirdPerson (Player active)\n");
                }
                else
                {
                    engine::core::LogInfoA("[input] camera = Free\n");
                }
            }
            m_prevToggleKeyDown = curToggle;
        }

        // AnimatorRuntime parameter 입력 (5-M2 Blend Tree 매핑):
        //   3인칭 active: Speed = Player.Speed() / maxRunSpeed (clamp 0..1)
        //   free-cam: 1 → 0.3 (Walk), 2 → 1.0 (Run), 안 누름 → 0 (Idle)
        //   Space (down edge) → Jump trigger (양쪽 모드 공통)
        //
        // Blend Tree 에서 Speed 즉시 변경은 paramVal 점프 → 클립 hard cut.
        // m_currentSpeed 를 target 으로 *지수 보간* 으로 부드럽게 전환.
        if (m_sceneRuntime->HasAnimatorRuntime())
        {
            const auto& input = m_window->GetInput();
            // 3인칭: Player 의 실측 속도 → 0..1. 최대 달리기 속도 450 u/s (Shift+WASD) 기준.
            //   free-cam: 1 hold → Walking(0.3), 2 hold → Running(1.0).
            float target = 0.0f;
            if (m_thirdPersonActive && m_player)
            {
                constexpr float kMaxRunSpeed = 450.0f;
                target = std::clamp(m_player->Controller().Speed() / kMaxRunSpeed, 0.0f, 1.0f);
            }
            else
            {
                target = input.IsKeyDown(static_cast<std::uint32_t>('2')) ? 1.0f
                       : input.IsKeyDown(static_cast<std::uint32_t>('1')) ? 0.3f
                       : 0.0f;
            }
            // smoothing rate 8/s — 0.125 s 시상수. 키 누름/뗌 ≈ 0.25 s 안에 완전 도달.
            const float smoothingRate = 8.0f;
            const float alpha = std::min(1.0f, dt * smoothingRate);
            m_currentSpeed += (target - m_currentSpeed) * alpha;
            m_sceneRuntime->SetAnimatorFloat("Speed", m_currentSpeed);

            const bool curJump = input.IsKeyDown(static_cast<std::uint32_t>(VK_SPACE));
            if (curJump && !m_prevJumpDown)
            {
                m_sceneRuntime->SetAnimatorTrigger("Jump");
                // UE 패턴 — 점프 임펄스는 CharacterController (capsule) 가 처리.
                //   애니메이션은 visual pose 만, 실제 world Y 는 controller 의 vy/gravity 적분.
                if (m_thirdPersonActive && m_player)
                {
                    m_player->Controller().Jump();
                }
            }
            m_prevJumpDown = curJump;

            // Ballistic root motion Y — 데이터 주도. animator.json 의 state.rootMotion 가
            //   takeoff/landing/peakHeight 를 제공. 없으면 0 반환. state 이름 분기 불필요 —
            //   AnimatorRuntime 이 현재 state 의 메타데이터를 자체 조회.
            const float jumpY = m_sceneRuntime->AnimatorRootMotionY();
            const std::string animStateName = m_sceneRuntime->CurrentAnimatorStateName();
            const bool inLocomotion = (animStateName == "Locomotion");
            // Hip X auto-align — Mixamo Jump 의 hip swing 보정.
            //   importTransform.rotation 이 mesh-X → world-(-X) 매핑이라, mesh-local hipX 가
            //   감소하면 world X 가 증가 (visible right drift). 같은 부호로 inst.x 적용 시
            //   캐릭터가 더 멀어지므로 *동일 부호* (jumpX = hipX - bindX) 로 cancel.
            const float hipX = m_sceneRuntime->AnimatorBoneMeshLocalX(L"Hips");
            if (!m_hipBindXCaptured && inLocomotion && std::fabs(hipX) > 0.0001f)
            {
                m_hipBindX = hipX;
                m_hipBindXCaptured = true;
            }
            float jumpX = 0.0f;
            if (animStateName == "Jump" && m_hipBindXCaptured)
            {
                jumpX = (hipX - m_hipBindX);
            }

            // Player.Update 가 transform.position 의 baseline (x, z) 을 먼저 쓰므로,
            //   jump 보정은 그 위에 *additive* 로 적용. 3인칭 비활성 시엔 baseline 이 0 이라
            //   absolute 와 동일 효과 — 양쪽 모드 코드 path 통합.
            if (m_thirdPersonActive && m_player)
            {
                m_player->Update(m_window->GetInput(), dt,
                                 m_thirdPersonCamera ? m_thirdPersonCamera->Yaw() : 0.0f);
            }

            if (engine::scene::Transform* xform = m_sceneRuntime->AnimatorInstanceTransform())
            {
                if (m_thirdPersonActive)
                {
                    // jumpX 는 mesh-local 보정값. Player yaw 가 캐릭터를 회전시키므로 그 방향에
                    //   맞춰 world (x, z) 로 분해해야 hip swing cancel 이 정확히 들어감.
                    //   yaw=0 (facing +Z): cosYaw=1, sinYaw=0 → x += jumpX, z 변화 없음
                    //                       → free-cam 시절 동작과 정확히 일치.
                    //   yaw=π/2 (facing +X): cosYaw=0, sinYaw=1 → x 불변, z -= jumpX
                    //                       → 캐릭터 right 방향으로 hip 보정 진행.
                    //   부호 (z -= jumpX) 는 CharacterController 의 right = (cosYaw, 0, -sinYaw)
                    //   convention 과 동일 — mesh-X 가 character right 으로 매핑.
                    const float yaw    = m_player ? m_player->Controller().Yaw() : 0.0f;
                    const float cosYaw = std::cos(yaw);
                    const float sinYaw = std::sin(yaw);
                    xform->position.y += jumpY;
                    xform->position.x += jumpX * cosYaw;
                    xform->position.z -= jumpX * sinYaw;
                }
                else
                {
                    xform->position.y = jumpY;
                    xform->position.x = jumpX;
                }
            }

        }

        // 카메라 갱신 — 3인칭 active 면 ThirdPersonCamera 가 Player 위치 추적,
        // 아니면 FreeCamera 가 WASD/마우스 입력 받음.
        if (m_thirdPersonActive && m_thirdPersonCamera && m_player)
        {
            m_thirdPersonCamera->Update(m_window->GetInput(),
                                        m_player->Controller().Position(), dt);
        }
        else
        {
            m_freeCamera->Update(m_window->GetInput(), dt);
        }

        // UE 패턴 — animator 가 캐릭터 물리 상태 읽음. Player.Update 후 controller 의
        //   최신 isGrounded 를 animator 에 push → Jump→Locomotion 전이 (IsGrounded
        //   조건) 가 실제 capsule 착지 시점에 발동. 애니메이션이 물리에 종속.
        // 3인칭 비활성 시엔 기본값 (parameter defaultValue = 1.0 = true) 유지.
        if (m_sceneRuntime->HasAnimatorRuntime() && m_thirdPersonActive && m_player)
        {
            m_sceneRuntime->SetAnimatorBool("IsGrounded", m_player->Controller().IsGrounded());
        }

        m_sceneRuntime->Tick(dt);

        D3D12_VIEWPORT viewport{};
        viewport.Width    = static_cast<float>(m_window->Width());
        viewport.Height   = static_cast<float>(m_window->Height());
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;

        D3D12_RECT scissor{};
        scissor.right  = static_cast<LONG>(m_window->Width());
        scissor.bottom = static_cast<LONG>(m_window->Height());

        // ImGui 패널 + Render — FrameRenderer 가 ImGui::GetDrawData() 결과를 마지막에 commit.
        if (m_imguiInitialized)
        {
            DrawAnimatorPanel();
            ImGui::Render();
        }

        m_frameRenderer->Render(*m_sceneRuntime, *m_camera, viewport, scissor);

        // 타이틀바 디버그 정보 — 100ms 주기로 갱신 (매 프레임은 SetWindowTextW 비용 + 깜빡임 우려).
        m_titleUpdateAccum += dt;
        if (m_titleUpdateAccum >= 0.1f)
        {
            m_titleUpdateAccum = 0.0f;
            const auto camPos = m_camera->Position();
            const auto camTgt = m_camera->Target();
            std::string  curState;
            std::wstring frameInfo;
            if (m_sceneRuntime->HasAnimatorRuntime())
            {
                curState = m_sceneRuntime->CurrentAnimatorStateName();
                const float stateTime = m_sceneRuntime->AnimatorCurrentStateTime();
                const float duration  = m_sceneRuntime->AnimatorStateDuration(curState);
                if (duration > 0.05f)
                {
                    // Mixamo 진행바 스타일 — frame / totalFrames + normalized %.
                    constexpr float kFps = 30.0f;
                    const int   curFrame   = static_cast<int>(stateTime * kFps + 0.5f);
                    const int   totalFrame = static_cast<int>(duration  * kFps + 0.5f);
                    const float pct        = stateTime / duration * 100.0f;
                    wchar_t fbuf[80];
                    std::swprintf(fbuf, std::size(fbuf),
                                  L" | frame %d/%d (%.1f%%)",
                                  curFrame, totalFrame, pct);
                    frameInfo = fbuf;
                }
            }
            wchar_t buf[320];
            std::swprintf(buf, std::size(buf),
                          L"portfolio_engine | cam=(%.0f, %.0f, %.0f) look=(%.0f, %.0f, %.0f)%hs%hs%ls",
                          camPos.x, camPos.y, camPos.z,
                          camTgt.x, camTgt.y, camTgt.z,
                          curState.empty() ? "" : " | state=",
                          curState.c_str(),
                          frameInfo.c_str());
            m_window->SetTitle(buf);
        }
    }
}
