#include "game/CharacterController.h"

#include "physics/PhysicsWorld.h"
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

    void CharacterController::UpdatePhysics(float dt) noexcept
    {
        // 매 tick 이벤트 플래그 reset — 이번 frame 에 일어난 일만 표시.
        m_jumpApexThisFrame = false;
        m_landedThisFrame   = false;

        // === PhysX 위임 경로 — PxCapsuleController 가 collide-and-slide (지형/벽/경사/계단) ===
        if (m_physics != nullptr && m_physics->HasCharacter())
        {
            const float oldVy = m_velocityY;
            m_velocityY -= m_gravity * dt;                     // 항상 중력 적분(grounded 면 아래 충돌로 0)
            if (oldVy > 0.0f && m_velocityY <= 0.0f) { m_jumpApexThisFrame = true; }

            bool grounded = false;
            const DirectX::XMFLOAT3 disp{ m_pendingMoveXZ.x, m_velocityY * dt, m_pendingMoveXZ.z };
            const DirectX::XMFLOAT3 foot = m_physics->MoveCharacter(disp, dt, grounded);
            m_position = foot;

            if (grounded)
            {
                if (!m_isGrounded) { m_landedThisFrame = true; }   // UE Landed() 등가
                m_isGrounded = true;
                if (m_velocityY < 0.0f) { m_velocityY = 0.0f; }    // 낙하 멈춤(다음 프레임 새 중력)
            }
            else { m_isGrounded = false; }

            m_pendingMoveXZ = { 0.0f, 0.0f, 0.0f };   // 소비 — free-cam UpdatePhysics 에서 stale 방지
            return;
        }

        // === 폴백 — ground sampler snap (PhysX 미연결) ===
        // 현재 (x, z) 의 ground Y — sampler 있으면 호출, 없으면 평지 (0).
        const float groundY = m_groundSampler
            ? m_groundSampler(m_position.x, m_position.z)
            : 0.0f;

        // UE NewFallVelocity + PhysFalling 패턴 — grounded 아닐 때 중력 적분 + floor snap.
        if (!m_isGrounded)
        {
            const float oldVy = m_velocityY;
            m_velocityY -= m_gravity * dt;
            // Apex 감지 — UE NotifyJumpApex 등가. Vy 가 양수에서 0 이하로 떨어진 frame.
            if (oldVy > 0.0f && m_velocityY <= 0.0f)
            {
                m_jumpApexThisFrame = true;
            }
            m_position.y += m_velocityY * dt;
            if (m_position.y <= groundY)
            {
                m_position.y      = groundY;
                m_velocityY       = 0.0f;
                m_isGrounded      = true;
                m_landedThisFrame = true;   // UE Landed() 등가.
            }
        }
        else
        {
            // Grounded 시 새 위치의 terrain Y 를 *부드럽게* 따라간다(critically-damped lerp).
            //   매 frame groundY 로 *즉시 snap* 하면, 빠르게 달릴 때 굴곡 지면에서 몸이 위아래로
            //   딱딱 끊겨 보임(사용자 "뛸때 끊김"). 지수 보간으로 고주파 지터를 제거하고 저주파
            //   지형 형상만 따라가게 한다. 큰 단차(>일정값)는 즉시 따라가 발이 지면을 뚫지 않게.
            const float diff = groundY - m_position.y;
            if (std::abs(diff) > 40.0f)
            {
                m_position.y = groundY;                              // 큰 단차/순간이동 — 즉시
            }
            else
            {
                const float rate  = 12.0f;                          // 시상수 ≈ 1/12 ≈ 0.083 s
                const float alpha = std::min(1.0f, dt * rate);
                m_position.y += diff * alpha;
            }
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

            // 이번 프레임 XZ 변위. PhysX 연결 시엔 *적용하지 않고* m_pendingMoveXZ 에 저장 →
            //   UpdatePhysics 가 Y(중력)와 합쳐 MoveCharacter 로 위임(collide-and-slide). 폴백(미연결)
            //   땐 여기서 직접 적용(기존 동작).
            XMFLOAT3 mv; XMStoreFloat3(&mv, XMVectorScale(move, speed * dt));
            m_pendingMoveXZ = { mv.x, 0.0f, mv.z };
            if (m_physics == nullptr)
            {
                m_position.x += mv.x;
                m_position.z += mv.z;
            }

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

        // Y 물리 — 추출된 UpdatePhysics 호출. Update() 와 UpdatePhysics() 둘 다 사용 시
        //   중복 적분 유발하므로 호출자는 둘 중 하나만 쓰기.
        UpdatePhysics(dt);
    }
}
