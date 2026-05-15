#pragma once

#include <DirectXMath.h>  // SDK 내장 — 외부 의존 아님

namespace engine::render
{
    // 단순 LookAt + Perspective 카메라 (LH 좌표계).
    //
    // 책임:
    //   - 위치 / 타겟 / 업 벡터 보유.
    //   - 시야각 / 종횡비 / near·far 보유.
    //   - View / Projection / ViewProjection 행렬 즉시 계산.
    //
    // 행렬 컨벤션: DirectXMath 의 LH (Left-Handed). row-major 저장 (XMMATRIX 기본).
    // HLSL 측에서 `row_major float4x4` 명시로 transpose 불필요.
    //
    // 향후 확장: FreeCamera (FPS 입력), OrbitCamera, 뷰포트별 다중 카메라 등.
    class Camera final
    {
    public:
        Camera();

        void SetPosition(const DirectX::XMFLOAT3& position);
        void SetTarget  (const DirectX::XMFLOAT3& target);
        void SetUp      (const DirectX::XMFLOAT3& up);

        void SetPerspective(float fovYRadians, float aspectRatio, float nearZ, float farZ);

        DirectX::XMMATRIX View()           const noexcept;
        DirectX::XMMATRIX Projection()     const noexcept;
        DirectX::XMMATRIX ViewProjection() const noexcept;

        const DirectX::XMFLOAT3& Position() const noexcept { return m_position; }
        const DirectX::XMFLOAT3& Target()   const noexcept { return m_target; }

    private:
        DirectX::XMFLOAT3 m_position{ 0.0f, 0.0f, -5.0f };
        DirectX::XMFLOAT3 m_target  { 0.0f, 0.0f,  0.0f };
        DirectX::XMFLOAT3 m_up      { 0.0f, 1.0f,  0.0f };
        float m_fovY    = DirectX::XM_PIDIV4;  // 45도
        float m_aspect  = 16.0f / 9.0f;
        float m_nearZ   = 0.1f;
        float m_farZ    = 100.0f;
    };
}
