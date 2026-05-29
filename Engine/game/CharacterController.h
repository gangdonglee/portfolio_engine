#pragma once

#include <DirectXMath.h>

namespace engine::platform { class Input; }

namespace engine::game
{
    // 3인칭 캐릭터의 *입력 → 위치/yaw* 변환기.
    //
    // 책임:
    //   - WASD 입력 + *camera yaw 기준* forward/right 분해 → XZ 이동 누적.
    //   - 이동 벡터 방향으로 player yaw 즉시 흡수 (no smoothing in this step).
    //   - Y 는 고정 (Phase 6 점프/중력 도입 전까지).
    //
    // 비책임:
    //   - 카메라 제어 (ThirdPersonCamera 별도).
    //   - MeshInstance write-back (Player 의 책임).
    //   - 충돌/물리 (Phase 6+).
    //
    // 좌표계: LH (Engine 디폴트). yaw=0 → 캐릭터 forward = +Z. yaw 증가 = 시계 반대방향 (top view).
    class CharacterController final
    {
    public:
        CharacterController() = default;

        // input: WASD (또는 Shift 부스트), dt: 초, cameraYaw: 카메라의 현재 yaw (rad).
        // WASD 가 cameraYaw 기준 forward/right 로 분해되어 "카메라 뒤에 있는 캐릭터가
        // 카메라 방향대로 이동" 하는 3인칭 표준 입력 매핑.
        void Update(const engine::platform::Input& input, float dt, float cameraYaw);

        // 외부 강제 위치 설정 (스폰, 디버그 텔레포트 등).
        void SetPosition(const DirectX::XMFLOAT3& position) noexcept { m_position = position; }
        void SetYaw     (float yaw) noexcept                          { m_yaw = yaw; }

        const DirectX::XMFLOAT3& Position() const noexcept { return m_position; }
        float                    Yaw()      const noexcept { return m_yaw; }
        // 이번 프레임 이동 속도 (units/sec) — Step 2 AnimSM 의 Speed 파라미터 입력에 쓰임.
        float                    Speed()    const noexcept { return m_lastSpeed; }

        void  SetMoveSpeed  (float unitsPerSec) noexcept { m_moveSpeed = unitsPerSec; }
        void  SetBoostFactor(float factor)      noexcept { m_boostFactor = factor; }
        void  SetYawTurnRate(float radPerSec)   noexcept { m_yawTurnRate = radPerSec; }

    private:
        DirectX::XMFLOAT3 m_position { 0.0f, 0.0f, 0.0f };
        float             m_yaw       = 0.0f;   // rad — Y axis 회전.
        float             m_lastSpeed = 0.0f;   // units/sec — 이번 프레임 실제 이동 속도.

        // 단위 — Mixamo 자산 스케일 (1 unit ≈ 1 cm 가까움). 사람 보통 걷기 ≈ 150 cm/s = 150 units/sec.
        float             m_moveSpeed   = 150.0f;
        float             m_boostFactor = 3.0f;   // Shift 시 ≈ 450 units/sec (달리기).
        // Yaw 회전 속도 (rad/sec). 입력 방향 변경 시 target yaw 로 *시간 기반 보간* — 즉시 흡수
        // 시 W→W+D→W 같은 미세 키 변화 / 대각→단일 복귀에서 캐릭터가 깜빡 회전하는 "뒤뚱댐"
        // 회피. 12 rad/sec ≈ 1 회전 (2π) 에 약 0.5 sec — 빠르게 반응하면서도 자연스러움.
        float             m_yawTurnRate = 12.0f;
    };
}
