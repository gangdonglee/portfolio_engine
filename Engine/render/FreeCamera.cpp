#include "render/FreeCamera.h"

#include "platform/Input.h"
#include "render/Camera.h"

#include <Windows.h>  // VK_W 등
#include <algorithm>  // std::clamp
#include <cmath>

namespace engine::render
{
    namespace
    {
        constexpr float kPitchLimit = 1.55334f;  // ≈ 89도 (라디안)

        // yaw/pitch → forward 벡터 (LH 좌표계).
        DirectX::XMVECTOR ForwardFromYawPitch(float yaw, float pitch)
        {
            const float cosPitch = std::cos(pitch);
            const float sinPitch = std::sin(pitch);
            const float cosYaw   = std::cos(yaw);
            const float sinYaw   = std::sin(yaw);
            // LH: 카메라 forward = (+sin(yaw)cos(pitch), sin(pitch), +cos(yaw)cos(pitch))
            return DirectX::XMVectorSet(sinYaw * cosPitch, sinPitch, cosYaw * cosPitch, 0.0f);
        }
    }

    FreeCamera::FreeCamera(Camera& camera)
        : m_camera(camera)
    {
        // 카메라의 현재 위치를 시작점으로 흡수. yaw/pitch 는 0 으로 시작 — 사용자가
        // 처음 우클릭 + 드래그 하기 전까지는 원래 카메라가 바라보던 방향과 일치 안 할 수 있다.
        // (위치만 이어받고, 회전은 yaw/pitch=0 부터 — Z축 +방향을 바라봄.)
        m_position = camera.Position();
        UpdateCameraFromState();
    }

    void FreeCamera::Update(const engine::platform::Input& input, float dt)
    {
        using namespace DirectX;

        // 회전 (우클릭 hold 시).
        if (input.IsMouseButtonDown(1))
        {
            m_yaw   += static_cast<float>(input.MouseDeltaX()) * m_rotSpeed;
            m_pitch += static_cast<float>(input.MouseDeltaY()) * m_rotSpeed;
            m_pitch = std::clamp(m_pitch, -kPitchLimit, kPitchLimit);
        }

        // 이동.
        const XMVECTOR forward = ForwardFromYawPitch(m_yaw, m_pitch);
        const XMVECTOR worldUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        // right = up × forward (LH, 정규화 필요 안 함 if forward·up 직교 — 일반 케이스는 cross+normalize)
        const XMVECTOR right   = XMVector3Normalize(XMVector3Cross(worldUp, forward));

        XMVECTOR move = XMVectorZero();
        if (input.IsKeyDown('W')) move = XMVectorAdd(move, forward);
        if (input.IsKeyDown('S')) move = XMVectorSubtract(move, forward);
        if (input.IsKeyDown('D')) move = XMVectorAdd(move, right);
        if (input.IsKeyDown('A')) move = XMVectorSubtract(move, right);
        if (input.IsKeyDown('E')) move = XMVectorAdd(move, worldUp);
        if (input.IsKeyDown('Q')) move = XMVectorSubtract(move, worldUp);

        if (!XMVector3Equal(move, XMVectorZero()))
        {
            move = XMVector3Normalize(move);
            const float speed = input.IsKeyDown(VK_SHIFT)
                ? m_moveSpeed * m_boostFactor
                : m_moveSpeed;
            move = XMVectorScale(move, speed * dt);

            XMVECTOR pos = XMLoadFloat3(&m_position);
            pos = XMVectorAdd(pos, move);
            XMStoreFloat3(&m_position, pos);
        }

        UpdateCameraFromState();
    }

    void FreeCamera::UpdateCameraFromState()
    {
        using namespace DirectX;
        const XMVECTOR pos     = XMLoadFloat3(&m_position);
        const XMVECTOR forward = ForwardFromYawPitch(m_yaw, m_pitch);
        const XMVECTOR target  = XMVectorAdd(pos, forward);

        XMFLOAT3 posF, targetF;
        XMStoreFloat3(&posF,    pos);
        XMStoreFloat3(&targetF, target);
        m_camera.SetPosition(posF);
        m_camera.SetTarget  (targetF);
    }
}
