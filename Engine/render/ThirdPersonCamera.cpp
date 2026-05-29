#include "render/ThirdPersonCamera.h"

#include "platform/Input.h"
#include "render/Camera.h"

#include <algorithm>
#include <cmath>

namespace engine::render
{
    namespace
    {
        constexpr float kPitchMax = 1.4f;   // ≈ 80도. 너무 위로 가면 카메라가 캐릭터 *위* 에서 내려다보게 됨.
        constexpr float kPitchMin = -0.4f;  // ≈ -23도. 너무 밑으로 가면 ground 통과.
    }

    ThirdPersonCamera::ThirdPersonCamera(Camera& camera)
        : m_camera(camera)
    {
    }

    void ThirdPersonCamera::Update(const engine::platform::Input& input,
                                   const DirectX::XMFLOAT3&       targetPos,
                                   float                          /*dt*/)
    {
        // 회전 — RMB hold + 마우스 드래그. FreeCamera 와 동일 UX (1 키 RMB).
        if (input.IsMouseButtonDown(1))
        {
            m_yaw   += static_cast<float>(input.MouseDeltaX()) * m_rotSpeed;
            m_pitch += static_cast<float>(input.MouseDeltaY()) * m_rotSpeed;
            m_pitch = std::clamp(m_pitch, kPitchMin, kPitchMax);
        }

        UpdateCameraFromState(targetPos);
    }

    void ThirdPersonCamera::UpdateCameraFromState(const DirectX::XMFLOAT3& targetPos)
    {
        using namespace DirectX;

        // yaw=0  → 카메라가 (0, +height, -distance) 에 위치 (캐릭터 뒤).
        // yaw 증가 → 시계 반대방향 (top view) 으로 카메라가 캐릭터 주변 도는 효과.
        const float cy = std::cos(m_yaw);
        const float sy = std::sin(m_yaw);
        const float cp = std::cos(m_pitch);
        const float sp = std::sin(m_pitch);

        // 캐릭터 → 카메라 방향의 단위 벡터 (back direction). pitch 가 양수면 위에서 내려보기.
        const float bx = -sy * cp;
        const float by =  sp;
        const float bz = -cy * cp;

        // camera position = target + lookHeight*up + back * distance
        const XMFLOAT3 camPos {
            targetPos.x + bx * m_distance,
            targetPos.y + m_lookHeight + by * m_distance,
            targetPos.z + bz * m_distance
        };
        const XMFLOAT3 camTgt {
            targetPos.x,
            targetPos.y + m_lookHeight,
            targetPos.z
        };

        m_camera.SetPosition(camPos);
        m_camera.SetTarget  (camTgt);
    }
}
