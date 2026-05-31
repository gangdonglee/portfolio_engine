#include "game/CharacterController.h"

#include "platform/Input.h"

#include <Windows.h>   // VK_SHIFT
#include <algorithm>
#include <cmath>

namespace engine::game
{
    namespace
    {
        constexpr float kTwoPi = 6.283185307179586f;
        constexpr float kPi    = 3.141592653589793f;

        // 두 yaw 사이 *최단 경로* delta (−π..π). 예: 170° → −170° 의 자연스러운 회전은 +20° (왼쪽).
        float ShortestYawDelta(float fromYaw, float toYaw) noexcept
        {
            float d = toYaw - fromYaw;
            while (d >  kPi) { d -= kTwoPi; }
            while (d < -kPi) { d += kTwoPi; }
            return d;
        }
    }

    void CharacterController::Jump() noexcept
    {
        // UE DoJump: 점프는 grounded 상태에서만 발동. 공중 점프 (double jump) 미지원.
        //   Velocity.Z = max(Velocity.Z, JumpZVelocity) — Z (=Y up) 속도 즉시 설정.
        //   MovementMode → MOVE_Falling (우리는 m_isGrounded=false 로 등가).
        if (m_isGrounded)
        {
            m_velocityY  = m_jumpZVelocity;
            m_isGrounded = false;
        }
    }

    void CharacterController::Update(const engine::platform::Input& input, float dt, float cameraYaw)
    {
        using namespace DirectX;

        // Camera yaw 기준 forward/right (LH, Y up).
        //   yaw=0 → forward = +Z, right = +X.
        const float cy = std::cos(cameraYaw);
        const float sy = std::sin(cameraYaw);
        const XMVECTOR forward = XMVectorSet( sy, 0.0f,  cy, 0.0f);
        const XMVECTOR right   = XMVectorSet( cy, 0.0f, -sy, 0.0f);

        XMVECTOR move = XMVectorZero();
        if (input.IsKeyDown('W')) move = XMVectorAdd     (move, forward);
        if (input.IsKeyDown('S')) move = XMVectorSubtract(move, forward);
        if (input.IsKeyDown('D')) move = XMVectorAdd     (move, right);
        if (input.IsKeyDown('A')) move = XMVectorSubtract(move, right);

        const bool hasMove = !XMVector3Equal(move, XMVectorZero());
        if (hasMove)
        {
            move = XMVector3Normalize(move);
            const float speed = input.IsKeyDown(VK_SHIFT)
                ? m_moveSpeed * m_boostFactor
                : m_moveSpeed;
            m_lastSpeed = speed;

            XMVECTOR pos = XMLoadFloat3(&m_position);
            pos = XMVectorAdd(pos, XMVectorScale(move, speed * dt));
            XMStoreFloat3(&m_position, pos);

            // 이동 방향 → target yaw. m_yaw 를 target 으로 *각도 보간*.
            XMFLOAT3 moveF;
            XMStoreFloat3(&moveF, move);
            const float targetYaw = std::atan2(moveF.x, moveF.z);
            const float delta     = ShortestYawDelta(m_yaw, targetYaw);
            const float maxStep   = m_yawTurnRate * dt;
            const float step      = std::clamp(delta, -maxStep, maxStep);
            m_yaw += step;
            if (m_yaw >  kPi) { m_yaw -= kTwoPi; }
            if (m_yaw < -kPi) { m_yaw += kTwoPi; }
        }
        else
        {
            m_lastSpeed = 0.0f;
        }

        // === Y 물리 (UE NewFallVelocity + PhysFalling 패턴) ===
        // grounded 가 아니면 매 tick 중력 적분 + floor 충돌 검사.
        if (!m_isGrounded)
        {
            m_velocityY -= m_gravity * dt;        // UE: Result += Gravity * dt (Gravity 음수)
            m_position.y += m_velocityY * dt;     // Euler 적분

            // 평면 floor (y=0) 충돌 — 실제 게임은 capsule sweep + GroundCheck.
            if (m_position.y <= 0.0f)
            {
                m_position.y = 0.0f;
                m_velocityY  = 0.0f;
                m_isGrounded = true;
            }
        }
    }
}
