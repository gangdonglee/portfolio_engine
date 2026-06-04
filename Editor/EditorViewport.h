#pragma once

#include <d3d12.h>
#include <d3dcommon.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>

namespace engine::render
{
    class Camera;
    class CommandList;
    class CommandQueue;
    class DebugRenderer;
    class DepthStencilBuffer;
    class Device;
    class FreeCamera;
    class PipelineState;
    class RootSignature;
    class SrvDescriptorHeap;
    class Texture;
}

namespace client
{
    class SceneRuntime;
}

namespace editor
{
    // Editor 의 Viewport 패널 안에 3D 씬을 렌더링하기 위한 RTT + 카메라 + 파이프라인 묶음.
    //
    // 책임:
    //   - Render-to-Texture (자체 RTV 힙 1슬롯 + SrvHeap 의 SRV 슬롯 하나).
    //   - 자체 Depth buffer (viewport 크기와 일치).
    //   - Shader (HelloTriangle.hlsl) + RootSignature + PipelineState.
    //   - Fallback albedo texture (mesh 가 텍스처 없을 때).
    //   - Orbit camera (RMB drag + 마우스 휠 zoom).
    //   - 자체 boot CommandList (fallback texture / FBX upload용).
    //
    // 비책임:
    //   - SceneRuntime 라이프타임 (호출자가 소유, render() 에 인자로 전달).
    //   - ImGui 패널 표시 자체 (호출자가 ImGui::Image 호출 — GpuSrvHandle() 사용).
    //
    // 사용 흐름 (매 프레임):
    //   1) viewport 패널 크기 확인 → Resize(w, h) 호출 (변경 시).
    //   2) UpdateInput(...) — 마우스 드래그/휠 입력.
    //   3) Render(cmdList, sceneRuntime) — RTT 에 씬 렌더 + 종단 barrier 까지.
    //   4) ImGui::Image((ImTextureID)GpuSrvHandle().ptr, size).
    class EditorViewport final
    {
    public:
        EditorViewport(engine::render::Device&            device,
                       engine::render::CommandQueue&      queue,
                       engine::render::SrvDescriptorHeap& srvHeap);
        ~EditorViewport();

        EditorViewport(const EditorViewport&)            = delete;
        EditorViewport& operator=(const EditorViewport&) = delete;
        EditorViewport(EditorViewport&&)                 = delete;
        EditorViewport& operator=(EditorViewport&&)      = delete;

        // viewport 패널 크기 변경 시 호출. width/height 가 동일하면 no-op.
        // GPU 가 RTT 참조 중이 아니어야 안전 — 호출자 사전 FlushGpu.
        void Resize(std::uint32_t width, std::uint32_t height);

        // 매 프레임 — 마우스 드래그(RMB)/휠 입력으로 orbit 카메라 업데이트.
        //   mouseDeltaX/Y: 이번 프레임 ImGui IO mouse delta (px).
        //   wheelDelta: 이번 프레임 휠 노치.
        //   rmbHeld: 우 마우스 버튼 눌림 (드래그 회전).
        //   hovered: Viewport 패널 위 호버 (입력 무시 게이트).
        void UpdateInput(float mouseDeltaX, float mouseDeltaY, float wheelDelta, bool rmbHeld, bool hovered);

        // RTT 에 씬 렌더링 — barrier (RT) + clear + draw + barrier (SRV).
        // 호출 시점: ImGui::Render() 직후, ImGui_ImplDX12_RenderDrawData() 직전.
        // sceneRuntime: 호출자 소유. 매 프레임 PrepareGpuResources/RecordDraw 호출됨.
        // frameIndex: SceneRuntime 의 N프레임 in-flight 인덱스 (0..N-1 cycle).
        void Render(ID3D12GraphicsCommandList* cmdList,
                    client::SceneRuntime&      sceneRuntime,
                    std::uint32_t              frameIndex);

        // ImGui::Image 에 전달할 GPU SRV 핸들.
        D3D12_GPU_DESCRIPTOR_HANDLE GpuSrvHandle() const noexcept { return m_srvGpu; }

        // boot CommandList — SceneRuntime 생성 시 fallback texture/FBX upload 용.
        engine::render::CommandList& BootCommandList() noexcept { return *m_bootCmdList; }

        // fallback albedo (SceneRuntime.RecordDraw 인자).
        engine::render::Texture& FallbackAlbedo() noexcept { return *m_fallback; }

        // orbit camera — 외부에서 Scene 의 cameraStart 로 초기화 가능.
        engine::render::Camera&     Camera()     noexcept { return *m_camera; }
        std::uint32_t Width()  const noexcept { return m_width; }
        std::uint32_t Height() const noexcept { return m_height; }

        // 화면 좌표 (RTT 내부 좌표, 좌상단 (0,0) ~ (Width, Height)) → Y=0 평면 hit 위치.
        // 반환 true: rayDir.y 가 평행 아니고 t>=0 (카메라 앞쪽).
        // 배치(brush) 워크플로우용 — 외부 좌표는 ImGui::GetMousePos - ItemRectMin 으로 계산.
        bool RaycastToGround(float screenX, float screenY, DirectX::XMFLOAT3& outWorld) const noexcept;

        // 화면 좌표 (RTT 내부, 좌상단 0,0) → refWorld 를 지나는 *카메라-수직 평면* 위 world 점.
        // IK 드래그 타겟 산출용 — 끌고 있는 끝 관절의 카메라 깊이를 유지하며 커서를 따라간다.
        // 반환 false: ray 가 평면과 평행하거나 카메라 뒤.
        bool ScreenToWorldAtDepth(float screenX, float screenY,
                                  const DirectX::XMFLOAT3& refWorld,
                                  DirectX::XMFLOAT3& outWorld) const noexcept;

        // 스켈레톤 디버그 draw 토글 (본 잡고 움직이기 기반).
        void SetShowSkeleton(bool show) noexcept { m_showSkeleton = show; }
        bool ShowSkeleton() const noexcept       { return m_showSkeleton; }

        // world 좌표 → RTT 화면 좌표 (좌상단 0,0). 본 picking 용 — 본 world 위치를
        // 화면에 투영해 마우스와 거리 비교. 반환 false: 카메라 뒤 (clip).
        bool WorldToScreen(const DirectX::XMFLOAT3& world, float& outX, float& outY) const noexcept;

        // 본 picking — RTT 화면 좌표 (screenX, screenY) 에 가장 가까운 본 선택.
        //   pixelRadius 안에 본이 없으면 선택 해제 (-1). sceneRuntime 의 joints 사용.
        void PickBone(client::SceneRuntime& sceneRuntime, float screenX, float screenY,
                      float pixelRadius = 14.0f);
        int  SelectedBone() const noexcept { return m_selectedBone; }
        void SetSelectedBone(int idx) noexcept { m_selectedBone = idx; }

        // 카메라의 world-space right / up 축 — 본 드래그 회전축 계산용.
        void CameraRightUp(DirectX::XMFLOAT3& outRight, DirectX::XMFLOAT3& outUp) const noexcept;

        // orbit 카메라를 world 구체(center, radius)에 맞춰 프레이밍 — IK 뷰가 캐릭터를 꽉 채우도록.
        //   target=center, distance 는 radius 가 세로 FOV 에 들어오게 자동 계산.
        void FocusOn(const DirectX::XMFLOAT3& center, float radius) noexcept;

    private:
        void CreateRtv();        // RTT 텍스처 + RTV (1슬롯 RtvHeap) 생성/재생성.
        void UpdateCameraFromOrbit();   // m_orbit{...} → m_camera 의 position/target.

        engine::render::Device&            m_device;
        engine::render::CommandQueue&      m_queue;
        engine::render::SrvDescriptorHeap& m_srvHeap;

        // RTT 자원 — 자체 RtvHeap (1슬롯) + 텍스처 + SRV (SrvHeap 의 슬롯).
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
        Microsoft::WRL::ComPtr<ID3D12Resource>       m_rttTexture;
        D3D12_CPU_DESCRIPTOR_HANDLE                  m_rtvCpu{};
        D3D12_CPU_DESCRIPTOR_HANDLE                  m_srvCpu{};
        D3D12_GPU_DESCRIPTOR_HANDLE                  m_srvGpu{};
        std::uint32_t                                m_width  = 0;
        std::uint32_t                                m_height = 0;

        // Depth buffer (viewport 크기).
        std::unique_ptr<engine::render::DepthStencilBuffer> m_depth;

        // 렌더 파이프라인.
        Microsoft::WRL::ComPtr<ID3DBlob>                m_vsBlob;
        Microsoft::WRL::ComPtr<ID3DBlob>                m_psBlob;
        std::unique_ptr<engine::render::RootSignature>  m_rootSig;
        std::unique_ptr<engine::render::PipelineState>  m_pso;

        // Boot CommandList + fallback texture.
        std::unique_ptr<engine::render::CommandList>    m_bootCmdList;
        std::unique_ptr<engine::render::Texture>        m_fallback;

        // 디버그 그리기 — Y=0 격자 + 좌표축 (배치 워크플로우 시각 보조).
        std::unique_ptr<engine::render::DebugRenderer>  m_debug;
        bool                                            m_showSkeleton = true;
        int                                             m_selectedBone = -1;  // 선택된 본 인덱스 (-1=없음)

        // Orbit camera 상태 + 실제 Camera 객체.
        std::unique_ptr<engine::render::Camera>         m_camera;
        struct Orbit {
            DirectX::XMFLOAT3 target { 0.0f, 100.0f, 0.0f };
            float distance = 350.0f;
            float yaw      = 0.0f;    // around Y (rad)
            float pitch    = 0.2f;    // ~11도 살짝 내려보기
        } m_orbit;
    };
}
