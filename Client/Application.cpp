#include "Application.h"

#include "FrameRenderer.h"
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
#include "scene/Scene.h"
#include "scene/SceneSerializer.h"

#include <chrono>
#include <filesystem>
#include <stdexcept>

namespace client
{
    namespace
    {
        constexpr float kNearPlane = 1.0f;
        constexpr float kFarPlane  = 5000.0f;

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
    }

    Application::~Application()
    {
        // 메인 루프 종료 후 호출됨 — GPU 작업 모두 완료된 상태.
        // 안전 차원에서 한 번 더 FlushGpu (소멸 순서 보장).
        if (m_queue)
        {
            m_queue->FlushGpu();
        }
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

        // Fallback albedo — Resources/Texture/Leather.jpg.
        const std::wstring texDir  = engine::render::fbx_loader::DefaultFbxDir() + L"..\\Texture\\";
        const std::wstring texPath = texDir + L"Leather.jpg";
        const engine::render::ImageData img =
            engine::render::image_loader::LoadImage(texPath.c_str());
        m_fallbackAlbedo = std::make_unique<engine::render::Texture>(
            *m_device, *m_queue, *m_bootCmdList,
            img.pixels.data(), img.width, img.height);
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

    void Application::LoadSceneAndRuntime()
    {
        // Scene 로드 — sample.scene.json 없으면 default.
        engine::scene::Scene scene;
        const std::string sceneRel = "assets/Scenes/sample.scene.json";
        if (std::filesystem::exists(sceneRel))
        {
            try
            {
                scene = engine::scene::LoadJson(sceneRel);
                engine::core::LogInfoA("[scene] loaded: ");
                engine::core::LogInfoA(sceneRel.c_str());
                engine::core::LogInfoA("\n");
            }
            catch (const std::exception& e)
            {
                engine::core::LogInfoA("[scene] load failed, fallback: ");
                engine::core::LogInfoA(e.what());
                engine::core::LogInfoA("\n");
                scene = BuildDefaultScene();
            }
        }
        else
        {
            engine::core::LogInfo(L"[scene] sample.scene.json 없음 - default Scene 사용\n");
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

        // SceneRuntime — Scene 의 owner 가 SceneRuntime 으로 이동.
        m_sceneRuntime = std::make_unique<SceneRuntime>(
            *m_device, *m_queue, *m_bootCmdList, *m_srvHeap, std::move(scene));

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

        // 입력 갱신 + 클립 전환 트리거.
        m_window->GetInput().BeginFrame();
        m_inputController.Tick(m_window->GetInput());
        const int clipChange = m_inputController.ConsumeClipChange();
        if (clipChange != InputController::kNoClipChange)
        {
            m_sceneRuntime->SetActiveClip(clipChange);
        }

        // 카메라 + Scene tick + 렌더.
        m_freeCamera->Update(m_window->GetInput(), dt);
        m_sceneRuntime->Tick(dt);

        D3D12_VIEWPORT viewport{};
        viewport.Width    = static_cast<float>(m_window->Width());
        viewport.Height   = static_cast<float>(m_window->Height());
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;

        D3D12_RECT scissor{};
        scissor.right  = static_cast<LONG>(m_window->Width());
        scissor.bottom = static_cast<LONG>(m_window->Height());

        m_frameRenderer->Render(*m_sceneRuntime, *m_camera, viewport, scissor);
    }
}
