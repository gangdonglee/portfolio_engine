#pragma once

#include "InputController.h"

#include "core/Types.h"

#include <Windows.h>
#include <DirectXMath.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <memory>
#include <string>
#include <vector>

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

        // 런타임 씬 전환. SceneRuntime 폐기 + 새 .scene.json 로드 + GPU 자원 재할당.
        // 호출 안전성:
        //   - 메인 루프 Tick 안에서 호출 — FlushGpu 가 GPU 작업 완료 보장.
        //   - SceneRuntime 의 기존 자산 (FBX 텍스처 SRV 등) 은 SrvHeap::Reset 으로 슬롯 재사용.
        //   - FrameRenderer::OnResize 호출로 in-flight fence value 초기화.
        // 실패 시 (LoadJson 예외) 기존 SceneRuntime 은 이미 폐기된 상태 — 호출자 예외 처리.
        void ChangeScene(const std::string& scenePath);

    private:
        // 부트 단계 — Run() 진입 전 ctor 에서 순서대로 호출.
        void InitGraphicsCore();           // Device/Queue/Swap/Depth/SrvHeap/fallback
        void InitGraphicsPipeline();       // Shader/RootSig/PSO
        void LoadSceneAndRuntime();        // Scene JSON + SceneRuntime + 카메라 초기화
        void InitRendererAndInput();       // FrameRenderer + InputController
        void ScanSceneSlots();             // assets/Scenes/*.scene.json 스캔 → F1..F9 매핑

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

        // F1..F9 슬롯 매핑 — assets/Scenes/*.scene.json. 최대 9 슬롯, 부팅 시 한 번 스캔.
        // 슬롯 0 = 첫 알파벳 순. ChangeScene 호출 시 인덱싱.
        std::vector<std::string>                            m_sceneSlots;
        std::string                                         m_currentScenePath;

        // AnimatorRuntime 입력용 키 다운 엣지 추적 (Space → Jump 트리거).
        bool                                                m_prevJumpDown = false;
        // Blend Tree 의 Speed 파라미터 보간용 누적 값.
        //   1 키를 떼면 target speed 가 즉시 0.0 이 되는데, 그대로 SetAnimatorFloat 하면
        //   blend tree 가 paramVal 따라 점프해 Idle 클립으로 시각적으로 끊긴다.
        //   m_currentSpeed 를 target 으로 *시간 보간* 해 부드럽게 전환.
        float                                               m_currentSpeed = 0.0f;

        // 점프 Y 는 Application::Tick 에서 *Animator state time 의 포물선 함수* 로 매 frame
        // 평가 (Mixamo Without-Skin 자산이 Hips Y translation 없는 한계 우회). 자체 timer 멤버
        // 불필요 — Animator state 가 진실의 단일 출처.

        // 타이틀바 디버그 정보 갱신 throttle — 매 프레임 SetWindowTextW 호출 시 CPU 비용 + 깜빡임 우려.
        float                                               m_titleUpdateAccum = 0.0f;

        // ctor 인자.
        int          m_widthPx;
        int          m_heightPx;
        std::wstring m_title;
    };
}
