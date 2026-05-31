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
    //   - Y 점프/중력 물리 (UE CharacterMovementComponent 패턴):
    //       Jump() 호출 시 m_velocityY = m_jumpZVelocity (UE 기본 420 cm/s).
    //       매 tick: m_velocityY -= m_gravity * dt; m_position.y += m_velocityY * dt.
    //       floor 충돌 (y <= 0): snap + velocityY=0 + IsGrounded=true.
    //
    // 비책임:
    //   - 카메라 제어 (ThirdPersonCamera 별도).
    //   - MeshInstance write-back (Player 의 책임).
    //   - 정확한 충돌 (capsule sweep 등 — 평면 floor 만 처리).
    //
    // 좌표계: LH (Engine 디폴트). yaw=0 → 캐릭터 forward = +Z. yaw 증가 = 시계 반대방향 (top view).
    class CharacterController final
    {
    public:
        CharacterController() = default;

        // input: WASD (또는 Shift 부스트), dt: 초, cameraYaw: 카메라의 현재 yaw (rad).
        // WASD 가 cameraYaw 기준 forward/right 로 분해되어 "카메라 뒤에 있는 캐릭터가
        // 카메라 방향대로 이동" 하는 3인칭 표준 입력 매핑.
        // 내부에서 UpdatePhysics(dt) 도 호출 — Y 물리 매 tick 적분.
        void Update(const engine::platform::Input& input, float dt, float cameraYaw);

        // *물리만* 적분 — 입력 처리 없이 Y 중력 + floor snap 만.
        //   free-cam 모드처럼 input 분리해야 하는 케이스에서 사용. Update() 가 끝에서
        //   호출하므로 Update() 와 동시 호출은 중복 적분 유발 — 둘 중 하나만 쓰기.
        void UpdatePhysics(float dt) noexcept;

        // 점프 임펄스 — IsGrounded 일 때만 m_velocityY = jumpZVelocity, MOVE_Falling 시작.
        //   UE ACharacter::Jump() + UCharacterMovementComponent::DoJump() 패턴.
        // 호출자 (Application) 는 Space down-edge 에서 호출. hold 무관, 단일 호출.
        void Jump() noexcept;

        // 외부 강제 위치 설정 (스폰, 디버그 텔레포트 등).
        void SetPosition(const DirectX::XMFLOAT3& position) noexcept { m_position = position; }
        void SetYaw     (float yaw) noexcept                          { m_yaw = yaw; }

        const DirectX::XMFLOAT3& Position() const noexcept { return m_position; }
        float                    Yaw()      const noexcept { return m_yaw; }
        // 이번 프레임 이동 속도 (units/sec) — Step 2 AnimSM 의 Speed 파라미터 입력에 쓰임.
        float                    Speed()    const noexcept { return m_lastSpeed; }
        // UE 의 IsFalling()/IsMovingOnGround() 등가. Animator state 결정에 사용.
        float                    VelocityY()  const noexcept { return m_velocityY; }
        bool                     IsGrounded() const noexcept { return m_isGrounded; }
        bool                     IsFalling()  const noexcept { return !m_isGrounded; }
        // UE 의 NotifyJumpApex() / Landed() 등가 — 이번 frame 에 해당 이벤트 발생했는지.
        //   Vy 가 + → ≤ 0 으로 떨어진 frame: ApexReachedThisFrame() == true.
        //   !grounded → grounded 전이 frame: LandedThisFrame() == true.
        //   UpdatePhysics() 호출 시 reset 후 갱신.
        bool                     ApexReachedThisFrame() const noexcept { return m_jumpApexThisFrame; }
        bool                     LandedThisFrame()      const noexcept { return m_landedThisFrame; }
        // 평면 floor (y=0) 가정 시 ground 까지의 거리. airborne 일 때만 양수.
        //   Lyra 의 GroundDistance (LineTrace 결과) 등가 — 우리는 단순화.
        float                    GroundDistance()       const noexcept { return m_isGrounded ? 0.0f : m_position.y; }

        void  SetMoveSpeed    (float unitsPerSec) noexcept { m_moveSpeed = unitsPerSec; }
        void  SetBoostFactor  (float factor)      noexcept { m_boostFactor = factor; }
        void  SetYawTurnRate  (float radPerSec)   noexcept { m_yawTurnRate = radPerSec; }
        void  SetJumpZVelocity(float unitsPerSec) noexcept { m_jumpZVelocity = unitsPerSec; }
        void  SetGravity      (float unitsPerSec2) noexcept { m_gravity = unitsPerSec2; }

    private:
        DirectX::XMFLOAT3 m_position { 0.0f, 0.0f, 0.0f };
        float             m_yaw       = 0.0f;   // rad — Y axis 회전.
        float             m_lastSpeed = 0.0f;   // units/sec — XZ 이동 속도 (Y 제외).

        // 단위 — Mixamo/UE 자산 (1 unit ≈ 1 cm). 사람 보통 걷기 ≈ 150 cm/s.
        float             m_moveSpeed   = 150.0f;
        float             m_boostFactor = 3.0f;   // Shift 시 ≈ 450 units/sec (달리기).
        // Yaw 회전 속도 (rad/sec). 입력 방향 변경 시 target yaw 로 *시간 기반 보간* — 즉시 흡수
        // 시 W→W+D→W 같은 미세 키 변화 / 대각→단일 복귀에서 캐릭터가 깜빡 회전하는 "뒤뚱댐"
        // 회피. 12 rad/sec ≈ 1 회전 (2π) 에 약 0.5 sec — 빠르게 반응하면서도 자연스러움.
        float             m_yawTurnRate = 12.0f;

        // Y 물리 — UE CharacterMovementComponent 기본값 (cm 스케일 가정).
        //   peak height = vy²/(2g) = 420²/1960 ≈ 90 cm.
        //   total air time = 2*vy/g ≈ 0.857 sec.
        float             m_velocityY         = 0.0f;
        float             m_gravity           = 980.0f;   // |GravityZ| (UE DefaultGravityZ = -980).
        float             m_jumpZVelocity     = 420.0f;   // UE 기본 JumpZVelocity.
        bool              m_isGrounded        = true;
        bool              m_jumpApexThisFrame = false;    // Vy 부호 전환 frame 만 true.
        bool              m_landedThisFrame   = false;    // airborne → grounded 전환 frame 만 true.
    };
}
