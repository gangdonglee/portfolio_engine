#pragma once

#include <DirectXMath.h>

namespace engine::platform { class Input; }

namespace engine::render
{
    class Camera;

    // FPS 스타일 자유 카메라 컨트롤러.
    //
    // 입력 매핑:
    //   - W/S/A/D: forward/back/strafe (수평).
    //   - Q/E: down/up (수직).
    //   - Shift: 이동 속도 부스트.
    //   - 우클릭(hold) + 마우스 이동: yaw/pitch 회전. Pitch 는 ±89도 클램프.
    //
    // 매 프레임 Update(input, dt) 호출 → 내부 위치/회전 갱신 → 대상 Camera 의 position/target 갱신.
    //
    // 좌표계: LH (Engine 의 디폴트).
    class FreeCamera final
    {
    public:
        // camera: 컨트롤할 카메라 (non-owning, 라이프타임은 호출자가 보장).
        // 초기 위치/타겟은 호출자가 camera 에 미리 설정해두면, 본 ctor 가 그것을 시작점으로 흡수.
        explicit FreeCamera(Camera& camera);

        void Update(const engine::platform::Input& input, float dt);

        void SetMoveSpeed   (float metersPerSec) noexcept { m_moveSpeed = metersPerSec; }
        void SetBoostFactor (float factor)       noexcept { m_boostFactor = factor; }
        void SetRotationSpeed(float radiansPerPixel) noexcept { m_rotSpeed = radiansPerPixel; }

    private:
        void UpdateCameraFromState();

        Camera& m_camera;

        DirectX::XMFLOAT3 m_position{ 0.0f, 0.0f, -5.0f };
        float m_yaw   = 0.0f;  // 라디안 — Y축 회전 (좌우 둘러보기)
        float m_pitch = 0.0f;  // 라디안 — X축 회전 (위아래 둘러보기)

        float m_moveSpeed   = 5.0f;   // m/s
        float m_boostFactor = 3.0f;
        float m_rotSpeed    = 0.005f; // rad / pixel
    };
}
