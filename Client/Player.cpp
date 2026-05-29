#include "Player.h"

#include "platform/Input.h"
#include "scene/Scene.h"

#include <cmath>

namespace client
{
    void Player::Bind(engine::scene::Transform* instanceTransform) noexcept
    {
        m_instanceTransform = instanceTransform;
    }

    void Player::Update(const engine::platform::Input& input, float dt, float cameraYaw)
    {
        m_controller.Update(input, dt, cameraYaw);
        if (m_instanceTransform == nullptr) { return; }

        m_instanceTransform->position = m_controller.Position();

        // yaw → quaternion (Y axis 회전, LH).
        //   X-Bot scene 의 inst.transform.rotation 은 identity 라 충돌 없음 — retargeting 결과
        //   의 head-down 보정은 FbxLoader 의 코드 X 180° flip 으로 처리. 여기선 player yaw 만.
        const float half = m_controller.Yaw() * 0.5f;
        const float s    = std::sin(half);
        const float c    = std::cos(half);
        m_instanceTransform->rotation = { 0.0f, s, 0.0f, c };
    }
}
