#pragma once

#include <DirectXMath.h>

namespace engine::platform { class Input; }

namespace engine::render
{
    class Camera;

    // 3인칭 follow 카메라.
    //
    // 동작:
    //   - 매 프레임 Update(input, targetPos, dt):
    //     * RMB hold + 마우스 drag → camera yaw/pitch 회전 (둘러보기).
    //     * 카메라 위치 = targetPos + offset(yaw, pitch, distance) + height up
    //     * 카메라 target = targetPos + lookHeight up
    //   - Camera 의 position/target 갱신.
    //
    // yaw 컨벤션 — CharacterController 와 동일:
    //   yaw=0  → 카메라가 캐릭터 *뒤* (즉 캐릭터 forward +Z 방향) 에 위치, +Z 를 바라봄.
    //
    // pitch: ±~80° 클램프 (over-the-top / under-the-floor 회피).
    class ThirdPersonCamera final
    {
    public:
        explicit ThirdPersonCamera(Camera& camera);

        // input: RMB + 마우스 이동, targetPos: 따라갈 캐릭터의 world position, dt: 초.
        void Update(const engine::platform::Input& input, const DirectX::XMFLOAT3& targetPos, float dt);

        // CharacterController 가 입력 분해할 때 참조 (WASD 의 forward = camera forward 의 XZ).
        float Yaw() const noexcept { return m_yaw; }

        void SetDistance   (float units) noexcept { m_distance = units; }
        void SetLookHeight (float units) noexcept { m_lookHeight = units; }
        void SetRotationSpeed(float radPerPx) noexcept { m_rotSpeed = radPerPx; }

    private:
        void UpdateCameraFromState(const DirectX::XMFLOAT3& targetPos);

        Camera& m_camera;

        float m_yaw      = 0.0f;   // rad — 카메라 자체의 Y axis 회전.
        float m_pitch    = 0.2f;   // rad — 살짝 내려보기 (~11도) 가 기본.

        float m_distance   = 300.0f;  // 캐릭터로부터 카메라까지의 거리 (units).
        float m_lookHeight = 100.0f;  // 캐릭터 origin 위 lookHeight 지점을 카메라가 바라봄 (가슴/머리 높이).
        float m_rotSpeed   = 0.005f;  // rad / pixel.
    };
}
