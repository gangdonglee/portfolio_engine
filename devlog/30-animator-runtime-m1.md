# 30. Phase 5-M1 — AnimatorRuntime (State Machine 런타임 + Crossfade) 🎬

- **날짜**: 2026-05-19
- **관련 커밋**: (이 단계 커밋)
- **소요 시간**: 약 3시간
- **단계**: Phase 5 — Animator System (M1/4)

---

## 1. 목표

5-M0 의 *데이터 모델* 위에 **런타임 평가** 추가. Unity Mecanim 의 `Animator` 컴포넌트 등가:

- `engine::anim::AnimatorRuntime` — controller 받아 매 프레임 Tick + parameter 평가 + state 전환 + crossfade.
- SceneRuntime 이 controller 의 모든 state.motionClipPath 사전 로드 + ClipMap 구축.
- Client 키 입력 (W / Shift+W / Space) → Animator parameter 설정 → state 전환 → 본 팔레트 변화.

검증 흐름: 부팅 시 X Bot 이 Idle (Breathing Idle) → W 누르면 Walk → Shift+W 누르면 Run → Space 누르면 Jump → 끝나면 자동 Idle 복귀.

## 2. 사전 컨텍스트

- 5-M0 (devlog 29): `AnimatorController` POD + JSON I/O + Scene.MeshInstance.animatorControllerPath. Client 가 부팅 시 controller 로드 + *로그만*.
- M1 의 목표 = *로그* 를 *실제 본 팔레트 변환* 으로 격상.
- 기존 `engine::render::Animator` (단일 클립) 의 element-wise lerp 패턴을 *두 클립 weight 보간* 으로 확장.

## 3. 결정과 트레이드오프

### 30-1. AnimatorRuntime 책임 — Tick + parameter + palette 한 클래스

- **결정**: `AnimatorRuntime` 단일 클래스가 (a) parameter 슬롯 (b) state machine 평가 (c) crossfade 진행 (d) 본 palette 계산을 모두 담당.
- **후보**:
  - A) 4개 별도 클래스 (ParameterBlock / StateMachine / CrossfadeTracker / PoseSampler).
  - B) 한 클래스 — 본 결정.
- **선택 이유**: B. Unity 의 `Animator` 도 같은 분할 — 4 개 책임이 *항상 함께 동작*. 분할은 응집도 훼손.
- **포기한 것**: 단위 테스트 격리. M2 (Blend Tree) 또는 M4 (Layer) 에서 분할 검토.

### 30-2. ClipMap — string → const AnimClip* (비소유)

- **결정**: `AnimatorRuntime::ClipMap = unordered_map<string, const AnimClip*>`. AnimatorRuntime 은 클립 *비소유* — SceneRuntime 의 `m_controllerClipCache` 가 owner.
- **이유**: 같은 클립이 *여러 state 의 motion* 또는 *여러 인스턴스의 controller* 에서 참조 가능. 소유는 한 곳, 참조는 여러 곳. AnimatorRuntime 의 ctor 가 ClipMap by value → 내부 멤버로 이동.
- **위험**: SceneRuntime 의 캐시 erase 도입 시 AnimatorRuntime 의 raw 포인터 dangling. *append-only* 가정 — 헤더 주석 명시.

### 30-3. Element-wise XMVectorLerp 행렬 보간 — 짧은 crossfade 에서 묵인

- **결정**: 기존 `Animator::Update` 의 element-wise lerp 패턴을 *crossfade* 에도 그대로 사용. 두 클립의 본 transform 매트릭스를 row 단위 `XMVectorLerp(curRow, tgtRow, weight)`.
- **위험 (리뷰 지적)**: 회전 부분 (3x3) 의 element-wise lerp 는 직교성/스케일 손실 — 본이 *shrink/skew*. 단일 클립 24fps 짧은 frame 간격에서는 시각 차이 거의 없음 (Animator.cpp 의 결정). Crossfade 0.2s 도 비슷 — 두 포즈가 *극단적으로 다르지 않으면* 시각 OK.
- **이월**: 정밀 quat slerp + position lerp 분해. M2 (Blend Tree) 또는 별도 정밀도 단계에서 일괄 개선.

### 30-4. Trigger 의 *소비된 것만* reset — Unity 사양

- **결정 (리뷰 시정)**: 발동된 transition 의 *conditions 에 등장한 Trigger 만* reset. 발동 실패한 transition 의 Trigger 는 다음 프레임에도 활성 유지.
- **초안의 버그**: ④ 단계에서 *모든 Trigger* reset → 한 프레임에 *발동 실패한 Trigger* 도 사라짐 → 사용자가 *Space* 눌렀는데 매칭 transition 없으면 그 Trigger 가 손실.
- **시정 패턴**: ③ 단계에서 매칭된 transition 의 condition 의 Trigger 들을 `consumedTriggers` vector 에 모음. ④ 에서 그것만 reset.

### 30-5. 키 입력 → Animator parameter — Application 측 직접

- **결정**: `Application::Tick` 안에서 `m_window->GetInput()` 의 키 상태를 직접 조회 → `m_sceneRuntime->SetAnimatorFloat/Trigger`. InputController 에 별도 매핑 추가 X.
- **이유**:
  - 지속 키 상태 (W 누르는 동안 Speed=0.6) 는 *down-edge 추적 불필요* — 매 프레임 키 상태 → float 매핑.
  - Trigger (Space) 는 *down-edge* 필요 — Application 의 `m_prevJumpDown` 멤버.
  - InputController 에 controller-specific 매핑 추가는 *게임 코드 결합*. Application 이 직접이 더 깔끔.

## 4. 작업 내용

### 4-1. `Engine/anim/AnimatorRuntime` 신규

[Engine/anim/AnimatorRuntime.h](../Engine/anim/AnimatorRuntime.h):
```cpp
class AnimatorRuntime final {
public:
    using ClipMap = unordered_map<string, const AnimClip*>;
    AnimatorRuntime(const AnimatorController&, const Skeleton&, ClipMap);
    void Update(float dt);
    void SetBool/Int/Float(string_view, value);
    void SetTrigger(string_view);
    const vector<XMFLOAT4X4>& Palette() const;
};
```

[Engine/anim/AnimatorRuntime.cpp](../Engine/anim/AnimatorRuntime.cpp) `Update(dt)` 5단계:
1. **transitioning 진행** — `crossfadeElapsed += dt`, `targetStateTime += dt * targetState.speed`. 끝나면 `current ← target`, transitioning 해제.
2. **current state 시간 진행** — `currentStateTime += dt * curState.speed`. loop ? wrap : clamp (클립 duration 기준).
3. **transition 평가** (transitioning 중이면 skip) — `fromState == current OR ""(Any)`, `hasExitTime ? normalizedTime >= exitTime : true`, `AllConditionsMet(conditions)`. 첫 매칭 transition 발동.
4. **Trigger reset** — `consumedTriggers` 만.
5. **BuildPalette** — current state 의 본 transform + (transitioning 시) target 의 transform `XMVectorLerp(weight)` + `offsetMatrix` 곱하기.

### 4-2. `Client/SceneRuntime` 확장

ctor 가 *animatorControllerPath 가 있는 첫 인스턴스* 처리:
1. controller `LoadJson` → `m_loadedController`.
2. 모든 `state.motionClipPath` 의 클립 FBX 를 `LoadFbxAnimationOnly` 로 `m_controllerClipCache` 에 사전 로드. 베이스 스켈레톤 = 그 인스턴스의 meshAsset.
3. ClipMap 구축 (path → unique_ptr.get()).
4. `AnimatorRuntime` 인스턴스 생성.

[Client/SceneRuntime.cpp](../Client/SceneRuntime.cpp) 핵심:
```cpp
for (const auto& state : controller->states) {
    if (m_controllerClipCache.contains(state.motionClipPath)) {
        const auto& cached = m_controllerClipCache.at(state.motionClipPath);
        clipMap.emplace(state.motionClipPath, cached[0].get());
        continue;
    }
    auto loaded = LoadFbxAnimationOnly(wpath.c_str(), *asset.skeleton);
    if (!loaded.clips.empty()) clipMap.emplace(state.motionClipPath, loaded.clips[0].get());
    m_controllerClipCache.emplace(state.motionClipPath, std::move(loaded.clips));
}
m_animatorRuntime = make_unique<AnimatorRuntime>(*m_loadedController, *asset.skeleton, std::move(clipMap));
```

`Tick(dt)` / `RecordDraw` 둘 다 `m_animatorRuntime` 우선 + `m_animator` 폴백 (M0 호환).

Passthrough API: `HasAnimatorRuntime()` / `SetAnimatorFloat` / `SetAnimatorBool` / `SetAnimatorTrigger` / `CurrentAnimatorStateName`.

### 4-3. `Client/Application::Tick` — 키 입력 매핑

```cpp
if (m_sceneRuntime->HasAnimatorRuntime()) {
    float speed = 0.0f;
    if (input.IsKeyDown('W')) {
        speed = input.IsKeyDown(VK_SHIFT) ? 1.0f : 0.6f;
    }
    m_sceneRuntime->SetAnimatorFloat("Speed", speed);

    const bool curJump = input.IsKeyDown(VK_SPACE);
    if (curJump && !m_prevJumpDown) m_sceneRuntime->SetAnimatorTrigger("Jump");
    m_prevJumpDown = curJump;
}
```

## 5. 마주친 문제와 해결 ⚠ 필수

### 문제 1 — Trigger 의 *모든 reset* 패턴 버그 (리뷰 지적)
- **문제**: 초안 ④ 단계가 `for each parameter if Trigger → 0`. 발동 실패한 transition 의 Trigger 도 reset → 사용자가 *Space* 눌렀는데 매칭 transition 없으면 손실.
- **해결**: ③ 에서 매칭 발동 시 그 transition 의 condition 의 Trigger parameter 인덱스를 `consumedTriggers` 에 push. ④ 에서 그것만 reset. Unity 사양 (소비된 Trigger).
- **교훈**: Trigger 의 *펄스 시맨틱* 은 "다음 평가까지 활성, 소비 시 사라짐". *시간 기반 reset* 이 아님.

### 문제 2 — Element-wise lerp 의 회전 정확성 (리뷰 의식만)
- **문제**: 두 클립의 본 transform 매트릭스를 row 단위 lerp → 회전 부분 직교성 손실 가능.
- **현 상황**: Mixamo X Bot 의 클립들이 *유사 포즈* (인간형 자연 자세) — 시각 차이 무시 가능. crossfade 0.2s 도 짧음.
- **이월**: 정밀 quat slerp — 클립 로드 시점에 본별 SRT 분해 캐시 + Tick 에서 slerp+lerp 합성. M2 (Blend Tree) 또는 별도 정밀도 단계.

### 문제 3 — ClipMap raw 포인터 lifetime (리뷰 의심)
- **문제**: `AnimatorRuntime::ClipMap` 이 `m_controllerClipCache` 의 `unique_ptr<AnimClip>` 의 `get()` 보유. 캐시 erase / 재할당 시 dangling.
- **현 상황**: `m_controllerClipCache` 가 *append-only* (SceneRuntime 의 생애 동안 erase 없음). unique_ptr 의 *get()* 은 heap 주소라 unordered_map rehash 도 안정.
- **해결**: 헤더 주석에 *"append-only 가정"* 명시. 동적 자산 언로드 도입 시 weak_ptr / 핸들 ID 격상.

### 문제 4 — 타입 alias 일관성 (리뷰 시정)
- **문제**: `FindStateIndex` 반환 `engine::int32` 인데 `int found` 로 받음. CODE_STYLE §3-3.
- **해결**: `engine::int32 found = ...`.

## 6. 결과 / 검증

- **빌드 (Debug + Release)**: 0 warning / 0 error.
- **Client.exe 자동 실행 (xbot_running 슬롯 격리 부팅)**:
  ```
  [scene] boot loaded: assets/Scenes\xbot_running.scene.json
  [render] FBX loaded: ...X Bot.fbx (bones=65, clips=2)
  [render] FBX animation-only loaded: clips=1, bones-matched=65/65 (Breathing Idle.fbx)
  [render] FBX animation-only loaded: clips=1, bones-matched=65/65 (Walking.fbx)
  [render] FBX animation-only loaded: clips=1, bones-matched=65/65 (Running.fbx)
  [render] FBX animation-only loaded: clips=1, bones-matched=65/65 (Jump.fbx)
  [anim] AnimatorRuntime active: states=4, transitions=6, params=2, default=Idle
  ```
  Controller 의 4 state 모두 클립 사전 로드. 본 매칭 65/65.

- **수동 시각 검증 (사용자 부탁)**:
  1. Client.exe 부팅 → X Bot 이 *Breathing Idle* (가벼운 호흡) 으로 시작.
  2. **W 누름** → 0.2s crossfade → *Walking*.
  3. **Shift+W 누름** → *Running* (Speed > 0.5 임계값).
  4. **W 떼기** → Speed=0 → *Idle* 복귀.
  5. **Space 누름** → *Jump* (Any State → Jump trigger). 클립 끝나면 자동 Idle.

## 7. AI 협업 메모

- 5-M0 의 데이터 모델이 *그대로 5-M1 의 입력* — 단계 분할의 가치 명확. M0 검증 단계 (controller 로드 + 로그) 가 *M1 의 토대* 가 되어 *M1 에서는 평가 로직만 추가*.
- 리뷰어가 **Trigger 의 시맨틱 버그** (소비된 것만 reset) 를 발견 — 자체 점검에선 *모든 Trigger reset* 이 자연 패턴으로 보였음. Unity 사양 숙지 부족이 *리뷰 가치* 로 보강.

## 8. 다음 단계 — 5-M2 (Blend Tree 1D)

- `Motion` 추상화 — 현재 `state.motionClipPath` 가 단일 클립. Blend Tree 도입 시 `Motion` 이 *(a) 단일 클립 / (b) BlendTree1D* 변형:
  ```cpp
  struct BlendTreeMotion {
      std::string parameterName;
      std::vector<BlendTreeChild> children;   // { motionClipPath, threshold }
  };
  ```
- `EvaluateBoneTransform` 가 motion 이 Blend Tree 면 *child 들의 가중 평균*. Speed 파라미터 → idle/walk/run 부드러운 보간.
- xbot.animator.json 의 *Locomotion state* 가 Blend Tree — Speed 0=Idle, 0.6=Walk, 1.0=Run.

미뤄둔 항목:
- Quat slerp 정밀 보간 — 정확성 강화 단계.
- AnimatorRuntime ctor 의 controller 로드 helper 추출 — SceneRuntime SRP 개선.

## 9. PPT 재료로 쓸 만한 포인트

- "AnimatorRuntime — Unity Mecanim 의 Animator 컴포넌트 등가. State / Transition / Parameter / Crossfade 한 클래스에 응집. 풀스크래치 C++, 외부 의존 0."
- "Trigger 의 *소비된 것만* reset — Unity 사양 정합. 발동 실패한 Trigger 가 다음 프레임까지 활성, 발동 시 사라짐."
- "ClipMap 패턴 — controller 의 모든 state.motionClipPath 가 한 번 사전 로드, AnimatorRuntime 은 *비소유* 참조. 메모리 1배."
- "Element-wise XMVectorLerp 행렬 보간 — 정밀도 약간 양보, 성능 우선. crossfade 0.2s 의 시각 차이 무시 가능. M2 의 quat slerp 로 격상 예정."
