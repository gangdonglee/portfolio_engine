# PHASE5_PLAN — 캐릭터 컨트롤러 / 액션 컴뱃 로드맵

> Plan 에이전트 (Sonnet 4.6) 가 설계, 사용자가 채택 + 구현 진행.
> 운영 패턴: [EDITOR_ROADMAP.md](EDITOR_ROADMAP.md) 와 동일. 결정 변경 시 본 표 갱신.

---

## 1. 큰 그림

Dragon (또는 후속 캐릭터) 이 *정적 메시* 에서 *사용자 입력에 반응하는 플레이어* 로 전환되는 단계. 펄어비스 포트폴리오에서 가장 큰 임팩트 — Phase 4 까지의 자산 파이프라인을 *실제 게임* 으로 묶음.

---

## 2. 핵심 결정 (사용자 결정 — 변경 시 본 표 갱신)

| # | 결정 | 선택 | 이유 |
|---|---|---|---|
| 1 | Entity 추상화 | `client::Player` 1개를 Application 직접 멤버 | 단일 캐릭터뿐이라 GameObject 추상은 과잉. Phase 6 적이 생기면 마이그. |
| 2 | 책임 분리 | `engine::game::CharacterController` (입력→Transform) + `engine::render::ThirdPersonCamera` + `client::Player` (둘 보유 + MeshInstance write-back) | 입력 처리는 엔진(재사용), 게임 entity 는 클라이언트. |
| 3 | AnimSM | `engine::render::AnimStateMachine` (Animator 옆에). enum class STATE { IDLE, WALK, ATTACK }. 전이는 코드 hardcode | JSON 정의는 Phase 6+. |
| 4 | 카메라 | FreeCamera 유지 + ThirdPersonCamera 추가. 토글 키는 *F 외 다른 키* (F 는 씬 슬롯 점유). 후보: V/C/Tab. | 시점 전환 디버깅용. |
| 5 | 이동/점프 | XZ 자유 이동 + Y 고정 (Dragon 비행 컨셉). 점프/중력/콜리전은 Phase 6 로 미룸. | 지면 콜리전 미구현이라 회피. |

---

## 3. 단계별 로드맵

| 단계 | 목표 | 산출 / 검증 기준 | 상태 |
|---|---|---|---|
| **Step 1** | CharacterController + WASD 이동 + 3인칭 카메라 follow | Dragon 이 WASD 로 XZ 평면 이동, 마우스로 yaw 회전, 카메라가 뒤에서 따라옴. 1..4 클립 키 계속 동작. | ⏳ |
| **Step 2** | AnimStateMachine 자동 전환 (speed > 0 → WALK, 아니면 IDLE) | 이동하면 walk 클립, 멈추면 idle 클립으로 자동 전환. 1..4 수동 키 제거 또는 디버그 모드. | ⏳ |
| **Step 3** | LMB → ATTACK 상태 (non-looping, 1회 재생 후 IDLE 복귀) | LMB 클릭 시 공격 클립 1회 재생, 끝나면 자동 IDLE. 연타 시 큐잉 또는 무시 정책. | ⏳ |
| **Step 4** | Player yaw ↔ Camera yaw 부드러운 분리 | 이동 시 player yaw 가 camera yaw 로 보간 회귀. 정지 시 마우스로 둘러볼 수 있음. | ⏳ |
| **Step 5** | Scene JSON `playerSpawn` 필드 + SceneRuntime 의 player 인스턴스 식별 | sample.scene.json 의 playerSpawn 으로 시작 위치/회전 지정. SceneRuntime 이 그 메쉬를 Player 에 바인딩. | ⏳ |

---

## 4. Step 1 상세 (이번 라운드 작업)

### 신규 파일
- `Engine/game/CharacterController.{h,cpp}` — `Update(input, dt)` → `m_position`/`m_yaw` 갱신. `MoveSpeed()`, `Position()`, `Yaw()` getter.
- `Engine/render/ThirdPersonCamera.{h,cpp}` — `Player*` 또는 (pos, yaw) 참조 → `engine::render::Camera` 의 position/target 갱신. 거리/높이/마우스 yaw 보유.
- `Client/Player.{h,cpp}` — `engine::scene::MeshInstance*` (위치 write-back) + `CharacterController` 보유. `Update(input, dt)` 후 instance.transform 동기화.

### 변경 파일
- `Client/Application.{h,cpp}` — `m_player`, `m_thirdCamera` 추가. 토글 키 (V/C/Tab 중 선택) 로 FreeCamera↔ThirdPersonCamera. Tick 에서 `m_player->Update` → SceneRuntime 의 transform 자동 갱신.
- `Client/InputController.{h,cpp}` — 카메라 토글 키 다운 엣지 + `ConsumeCameraToggle()`.

### 의존성 방향
```
Application → Player → CharacterController (engine) → Input
Application → ThirdPersonCamera (engine) → Camera + Player(pos/yaw)
Application → SceneRuntime → MeshInstance ← Player (write-back)
```

### 검증
- Dragon.fbx 로드 후 WASD 로 XZ 이동, 마우스로 yaw 회전, 카메라가 뒤에서 따라옴.
- 카메라 토글 키 누르면 FreeCamera 로 복귀, 다시 누르면 ThirdPersonCamera.
- 1..4 클립 키는 계속 수동 전환 (Step 2 에서 자동화).

---

## 5. 의도적으로 미루는 항목 → Phase 6+

- 점프/중력/지면 콜리전
- 적 entity
- 공격 판정 / 히트박스
- IK (foot placement 등)
- 블렌딩 트리 (cross-fade)
- 데이터 기반 state machine (JSON)
- GameObject 일반화 (다중 entity)

---

## 6. 본 문서의 운영

- 결정 변경 시 §2 표 갱신.
- 각 Step 완료 시 §3 의 *상태* 컬럼 ⏳ → ✅, 관련 devlog 링크 추가.
- 단계가 너무 커지면 *서브 단계로 쪼개기*. 한 단계 = 1~수 라운드.
