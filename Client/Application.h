#pragma once

#include "InputController.h"

#include "core/Types.h"

#include <Windows.h>
#include <DirectXMath.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <memory>
#include <string>

struct ID3D10Blob;
using ID3DBlob = ID3D10Blob;

namespace engine::platform
{
    class Window;
}

namespace engine::render
{
    class Device;
    class CommandQueue;
    class CommandList;
    class RtvDescriptorHeap;
    class SrvDescriptorHeap;
    class SwapChain;
    class DepthStencilBuffer;
    class RootSignature;
    class PipelineState;
    class Texture;
    class Camera;
    class FreeCamera;
}

namespace client
{
    class FrameRenderer;
    class SceneRuntime;

    // 클라이언트 애플리케이션의 루트.
    //   - 윈도우/Device/CommandQueue/SwapChain/DepthBuffer/RootSig/PSO/SrvHeap/fallback texture
    //     같은 부트 자원의 라이프타임 소유.
    //   - SceneRuntime/FrameRenderer/InputController/Camera/FreeCamera 의 생성·소유.
    //   - Run() 의 메인 루프 — 입력 폴링 + Tick + Frame.
    class Application final
    {
    public:
        Application(int widthPx, int heightPx, const std::wstring& title);
        ~Application();

        Application(const Application&)            = delete;
        Application& operator=(const Application&) = delete;
        Application(Application&&)                 = delete;
        Application& operator=(Application&&)      = delete;

        // 메인 루프. 윈도우가 닫힐 때까지 블록.
        void Run();

    private:
        // 부트 단계 — Run() 진입 전 ctor 에서 순서대로 호출.
        void InitGraphicsCore();           // Device/Queue/Swap/Depth/SrvHeap/fallback
        void InitGraphicsPipeline();       // Shader/RootSig/PSO
        void LoadSceneAndRuntime();        // Scene JSON + SceneRuntime + 카메라 초기화
        void InitRendererAndInput();       // FrameRenderer + InputController

        // 매 프레임 호출 — Run() 안에서.
        void Tick(float dt);

        // ----- 자원 (부트 순서대로 ctor 에서 초기화) -----
        std::unique_ptr<engine::platform::Window>           m_window;
        std::unique_ptr<engine::render::Device>             m_device;
        std::unique_ptr<engine::render::CommandQueue>       m_queue;
        std::unique_ptr<engine::render::RtvDescriptorHeap>  m_rtvHeap;
        std::unique_ptr<engine::render::SwapChain>          m_swap;
        std::unique_ptr<engine::render::DepthStencilBuffer> m_depth;
        std::unique_ptr<engine::render::SrvDescriptorHeap>  m_srvHeap;

        // 부트 시 fallback texture 업로드용 임시 CommandList.
        std::unique_ptr<engine::render::CommandList>        m_bootCmdList;
        std::unique_ptr<engine::render::Texture>            m_fallbackAlbedo;

        // 셰이더 + 파이프라인.
        Microsoft::WRL::ComPtr<ID3DBlob>                    m_vsBlob;
        Microsoft::WRL::ComPtr<ID3DBlob>                    m_psBlob;
        std::unique_ptr<engine::render::RootSignature>      m_rootSig;
        std::unique_ptr<engine::render::PipelineState>      m_pso;

        // 카메라 (FreeCamera 가 Camera 를 참조하므로 Camera 먼저).
        std::unique_ptr<engine::render::Camera>             m_camera;
        std::unique_ptr<engine::render::FreeCamera>         m_freeCamera;

        // Scene 런타임 / 매 프레임 렌더러 / 입력 컨트롤러.
        std::unique_ptr<SceneRuntime>                       m_sceneRuntime;
        std::unique_ptr<FrameRenderer>                      m_frameRenderer;
        InputController                                     m_inputController;

        // ctor 인자.
        int          m_widthPx;
        int          m_heightPx;
        std::wstring m_title;
    };
}
