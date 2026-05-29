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

        if (XMVector3Equal(move, XMVectorZero()))
        {
            m_lastSpeed = 0.0f;
            // 정지 시 yaw 도 유지 (target 변화 없음 — 갱신 안 함).
            return;
        }

        move = XMVector3Normalize(move);
        const float speed = input.IsKeyDown(VK_SHIFT)
            ? m_moveSpeed * m_boostFactor
            : m_moveSpeed;
        m_lastSpeed = speed;

        XMVECTOR pos = XMLoadFloat3(&m_position);
        pos = XMVectorAdd(pos, XMVectorScale(move, speed * dt));
        XMStoreFloat3(&m_position, pos);

        // 이동 방향 → target yaw. m_yaw 를 target 으로 *각도 보간* (instantaneous 가 아닌
        // angular rate 기반) — W→W+D→W 같은 미세 키 변화 / 대각→단일 복귀 시 캐릭터가 깜빡
        // 회전하는 *뒤뚱댐* 회피. 12 rad/sec 면 빠르게 따라가면서도 visual smoothing.
        XMFLOAT3 moveF;
        XMStoreFloat3(&moveF, move);
        const float targetYaw = std::atan2(moveF.x, moveF.z);
        const float delta     = ShortestYawDelta(m_yaw, targetYaw);
        const float maxStep   = m_yawTurnRate * dt;
        const float step      = std::clamp(delta, -maxStep, maxStep);
        m_yaw += step;
        // 정규화 (−π..π) — 누적 drift 방지.
        if (m_yaw >  kPi) { m_yaw -= kTwoPi; }
        if (m_yaw < -kPi) { m_yaw += kTwoPi; }
    }
}
