#pragma once

#include "game/CharacterController.h"

namespace engine::platform { class Input; }
namespace engine::scene    { struct Transform; }

namespace client
{
    // 3인칭 플레이어 — CharacterController + MeshInstance.transform write-back.
    //
    // 라이프타임 / ownership:
    //   - Application 이 보유. SceneRuntime 의 AnimatorInstanceTransform() 으로 *non-owning*
    //     포인터를 받아 매 프레임 transform 갱신.
    //   - 씬 전환 시 (ChangeScene) instance 가 재생성되므로 Player 도 재바인딩 필요 — Bind() 별도.
    class Player final
    {
    public:
        Player() = default;

        Player(const Player&)            = delete;
        Player& operator=(const Player&) = delete;
        Player(Player&&)                 = delete;
        Player& operator=(Player&&)      = delete;

        // SceneRuntime 의 animator 인스턴스 transform 을 바인딩. nullptr 도 허용 (애니메이터 없는
        // 씬 또는 ChangeScene 직후 임시 unbind 상태).
        void Bind(engine::scene::Transform* instanceTransform) noexcept;

        // 매 프레임 1회. cameraYaw 는 ThirdPersonCamera 에서 받아 WASD 분해 기준으로 사용.
        // 인스턴스 transform 이 unbound 면 silent no-op.
        void Update(const engine::platform::Input& input, float dt, float cameraYaw);

        const engine::game::CharacterController& Controller() const noexcept { return m_controller; }
        engine::game::CharacterController&       Controller()       noexcept { return m_controller; }

    private:
        engine::game::CharacterController m_controller;
        engine::scene::Transform*         m_instanceTransform = nullptr;
    };
}
