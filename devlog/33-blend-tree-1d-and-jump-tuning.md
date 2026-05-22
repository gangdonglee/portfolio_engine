# 33. 5-M2 Blend Tree 1D + 점프/입력 정비 + 바닥 grid

- **날짜**: 2026-05-22
- **관련 커밋**: (이 작업 후 커밋)
- **소요 시간**: ~4시간
- **단계**: Phase 5 — Animator M2 (Blend Tree)

---

## 1. 목표

- AnimatorController 의 *State 안 1D Blend Tree* 도입 — 단일 파라미터로 인접 두 motion 의 weighted blend.
- xbot.animator.json 의 Idle/Walk/Run 을 *Locomotion 단일 state* 로 통합.
- Speed 파라미터 입력 시 *Animator 측 부드러운 전환* 보장 (입력단 보간).
- Mixamo 자산 정상화 — Idle 정적 클립 + Walking In-Place + Jump 의 exitTime 단축.
- 바닥 grid 디버그 시각화 (점프 등 Y 변동 모션의 참조).

## 2. 사전 컨텍스트

[32-import-transform-and-walk-input.md](32-import-transform-and-walk-input.md) 완료 — importTransform 인프라로 자산별 좌표계 보정, X-Bot 정자세 확보. 단일 clip 기반 state machine (Idle ↔ Walk ↔ Run ↔ Jump) 가 동작 중. 이제 Unity Mecanim 의 Blend Tree 1D 패턴 도입.

## 3. 결정과 트레이드오프

### 3-1. Blend Tree 의 데이터 모델

- **결정**: `AnimatorState` 안에 `blendTree (BlendTreeEntry[])` + `blendParameter` 필드 평면 추가. 비어있으면 *단일 clip mode* (기존 호환).
- **후보**: Unity 의 Motion = Clip | BlendTree 재귀 트리.
- **선택 이유**: JSON 호환성 (기존 단일 clip state JSON 그대로 동작) + 1D blend tree 라는 한정된 시나리오. 재귀 트리는 *중첩 blend tree 시* 필요한데 현재 단계에서는 과도.
- **포기한 것**: 2D blend tree, blend tree 안 blend tree (중첩).

### 3-2. Blend Tree 평가 시 motion 별 시간 진행

- **결정**: state 의 `m_currentStateTime` 을 *모든 motion 에 공통* 적용. 각 motion 의 평가 시 자체 duration 으로 wrap (EvaluateBoneTransform 의 기존 wrap 로직).
- **후보**: motion 별 *normalized time* 동기화 (Unity 의 Compute Threshold).
- **선택 이유**: 구현 단순. Idle/Walk/Run 의 duration 이 비슷한 일반 케이스 OK.
- **포기한 것**: motion 간 phase 불일치 시 흔들. 자산 측 정렬 필요.

### 3-3. Hips lock 의 Y 제외

- **결정**: root bone (Hips) 의 translation lock 을 *XZ 만* 적용. Y 는 유지.
- **후보**: 전체 translation lock vs Y 도 lock.
- **선택 이유**: 점프 / Walking 의 자연스러운 Y bob 살아남도록. In Place 자산은 어차피 0 이라 영향 없음.
- **포기한 것**: 모든 자산에 정적 lock 적용. 일반 점프 자산 사용 시 안전망 약화.

### 3-4. Jump → Locomotion 의 exitTime

- **결정**: `exitTime=0.3` 으로 단축. 클립 78 frames 중 ~23 frames 시점에 Locomotion 으로 crossfade.
- **후보**: 1.0 (전체 재생) / 0.75 / 0.5 / 0.3.
- **선택 이유**: 받은 Jump.fbx 자산이 *내부에 2번 점프 + recovery* 동작 baked. 1회 점프만 보여주려면 클립의 1/3 시점 절단.
- **포기한 것**: 자산 의도 완전 표현. 자산이 *연속 점프 의도* 라면 잘림.

### 3-5. 점프 Y 코드 시도 + 철회

- **결정**: Application 의 *Animator state time 기반 포물선 Y 변환* 인프라 (`AnimatorInstanceTransform`, `AnimatorStateDuration`, `AnimatorCurrentStateName/Time` SceneRuntime passthrough) 를 추가했지만 *현 단계에서 활성 사용 X*. exitTime 단축만으로 자연스러운 점프가 확보됨.
- **선택 이유**: Mixamo Without-Skin 자산이 본 회전만으로 점프 형태 표현 가능. 코드 Y 보정은 *importTransform 의 회전 중심 효과 (Y=180) 과 결합 시 과장* — 자연스러움 손실.
- **포기한 것**: 자산 측 한계 (Hips translation 자체가 0) 의 강제 우회.
- 인프라는 유지 — 향후 *자산 변환 도구* 또는 *root motion 본격 통합* 시 재활용.

## 4. 작업 내용

### 4-1. AnimatorController + Serializer — BlendTree

[Engine/anim/AnimatorController.h](../Engine/anim/AnimatorController.h):
```cpp
struct BlendTreeEntry { std::string motionClipPath; float threshold; };

struct AnimatorState {
    // ... 기존 필드 ...
    std::vector<BlendTreeEntry>  blendTree;          // empty = 단일 clip
    std::string                  blendParameter;     // blend tree mode 전용
};
```

[Engine/anim/AnimatorSerializer.cpp](../Engine/anim/AnimatorSerializer.cpp): JSON 직/역직렬화 (blendTree 가 비어있을 때 생략 — 기존 JSON 호환).

### 4-2. AnimatorRuntime — Blend Tree 평가

[Engine/anim/AnimatorRuntime.h/cpp](../Engine/anim/AnimatorRuntime.cpp):

```cpp
XMMATRIX AnimatorRuntime::EvaluateStateBoneTransform(
    const AnimatorState& state, size_t boneIdx, double stateTime) const
{
    if (state.blendTree.empty()) { /* 단일 clip */ }
    // 1D Blend Tree — blendParameter 값 → 인접 두 entry → 각 clip 평가 → LerpTransforms.
}
```

- `StateRepresentativeDuration` 도 blend tree 의 첫 entry 의 clip duration 사용 (state time wrap).
- `BuildPalette` 가 *현재/타깃 state* 기반으로 EvaluateStateBoneTransform 호출 (crossfade lerp 포함).
- 외부 노출: `CurrentStateTime()`, `StateDuration(name)` — 게임 코드의 동기화용.

### 4-3. SceneRuntime — Blend Tree clip 사전 로드

[Client/SceneRuntime.cpp](../Client/SceneRuntime.cpp):
- `for (state : states)` 안에서 *state.blendTree 의 모든 entry.motionClipPath* 도 `loadClipIntoMap` 으로 사전 로드.
- 람다로 묶어 단일 clip + blend tree 둘 다 동일 경로.
- 추가 API: `AnimatorInstanceTransform()`, `AnimatorStateDuration/CurrentStateName/Time`.

### 4-4. xbot.animator.json — Locomotion 통합

[assets/Animators/xbot.animator.json](../assets/Animators/xbot.animator.json):
- 기존 Idle/Walk/Run 3 state + 4 transition 을 **Locomotion 단일 state + Speed blend tree** 로 통합.
- 항목: Idle (threshold=0.0) → Walking (0.5) → Running (1.0).
- Jump 는 별도 state 유지. Any State → Jump (Jump trigger), Jump → Locomotion (exitTime=0.3).

### 4-5. Application — Speed damping + 1 키 매핑

[Client/Application.cpp](../Client/Application.cpp):
- 1 키 → target Speed = 0.3 (Walk 50% blend). 안 누르면 0.0.
- `m_currentSpeed` 를 target 으로 *지수 보간* (smoothing rate 8/s ≈ 0.25s 시상수).
  Blend Tree 의 paramVal 즉시 변경 → 클립 hard cut 방지.
- W 매핑 제거 (FreeCamera 의 카메라 이동 키로만).
- 점프 Y 보정 코드는 *시도 후 비활성* — Animator clip 만으로 충분.

### 4-6. DebugRenderer — XZ 바닥 grid

[Engine/render/DebugRenderer.h/cpp](../Engine/render/DebugRenderer.cpp):
- `DrawGrid(list, frameIndex, viewProj)` 추가. ctor 에서 ±500 범위, step=50 의 grid VB 사전 생성.
- 색상 회색 톤 (0.40, 0.40, 0.45). LineList 토폴로지, 기존 PSO (depth-OFF) 공유.
- FrameRenderer 의 DrawAxes 직전 호출 — 좌표축이 grid 위에 덧그려짐.

### 4-7. Mixamo 자산 교체

- `Resources/FBX/Idle.fbx` 신규 (호흡 없는 정적 idle).
- `Resources/FBX/Walking.fbx` In-Place 옵션으로 재다운로드.
- `Resources/FBX/Jump.fbx` 신규로 교체 (557 KB → 백업 `Jump.fbx.bak`).

## 5. 마주친 문제와 해결 ⚠

### 5-1. Speed 즉시 전환 시 Blend Tree 의 hard cut

- *증상*: 1 키 떼는 순간 Idle 클립으로 시각적으로 뚝 끊김.
- *원인*: Animator 가 paramVal 을 즉시 따라가 *blend weight = 0:1 즉시 점프*.
- *해결*: 입력단에서 `m_currentSpeed` 를 *지수 보간* 으로 부드럽게 (Unity Animator.SetFloat 의 dampTime 인자와 같은 효과).

### 5-2. Hips 4x4 transform 전체 lock → 골반 swing 사라짐

- *증상*: Walking 의 자연스러운 골반 좌우 swing 이 사라지고 다리만 움직이는 어색한 걸음.
- *원인*: Hips matrix 전체를 lock 하면 *Walking 의 골반 회전 (rotation)* 까지 차단.
- *해결*: lock 을 *translation-only* + *XZ 만* 으로 축소. Y 와 rotation 모두 유지.

### 5-3. Jump 의 *2번 점프 + 허우적*

- *증상*: Space 한 번에 Jump 가 *2번 뛰는* 듯 + 끝에 padding 동작.
- *진단*: Animator transition 로그 확인 → *Locomotion → Jump → Locomotion 1회만* 발생. trigger 재발사 아님. 즉 *자산 자체* 가 2.6 s / 78 frames 안에 2번 점프 + recovery 동작 baked.
- *해결*: exitTime 을 1.0 → 0.75 → 0.4 → **0.3** 으로 단축. 1회 점프 끝 시점에서 Locomotion 으로 crossfade.

### 5-4. Mixamo Without-Skin 자산의 *모든 본 translation = 0*

- *증상*: Jump.fbx 의 Hips Y translation 도 0 — 실제 Y 점프 동작이 자산에 없음.
- *진단*: 모든 본의 Y translation 변동 측정 → 모두 0.00. 새 자산 (557 KB) 도 동일.
- *원인*: Mixamo Without-Skin export 옵션 자체가 *bone hierarchy 의 translation 키프레임을 제거* + 모든 동작을 *rotation 누적* 으로 표현하는 구조. *클립이 In-Place 든 아니든* 동일.
- *해결*: 자산 한계 수용. Animator 클립의 본 회전만으로 점프 형태 표현 + exitTime 단축으로 자연스러움 확보.

### 5-5. CurrentStateIndex 중복 선언

- *증상*: 빌드 실패 — `error C2535: 'engine::anim::AnimatorRuntime::CurrentStateIndex(void)' already declared`.
- *원인*: AnimatorRuntime.h 에 *public* 섹션 이미 `CurrentStateIndex` 존재, 외부 노출 추가 시 중복 선언.
- *해결*: 추가 선언 제거 + private 섹션 복원.

## 6. 결과 / 검증

- 1 키 누른 채 → Idle 50% + Walking 50% blend (Speed=0.3 → blend tree (0.0..0.5) 의 60% 진행).
- 1 키 떼면 ~0.25s 안에 Idle 로 부드럽게 복귀 (Speed damping).
- Space → Jump state 1회 진입 → 0.3 시점에 Locomotion 으로 crossfade. 1회 점프 모션만 노출.
- Dragon (F2) 정상 동작 — 본 변경의 *blend tree 가 없는 state* 경로 무영향.
- 바닥 grid 표시됨 (점프 시각 검증 가능).

## 7. AI 협업 메모

- Speed 즉시 변경 → blend tree hard cut 의 *원인* 을 추론 후 dampTime 패턴 제안. 사용자가 Unity 의 동일 명칭 호출 (`Animator.SetFloat(name, value, dampTime, dt)`) 을 알아 *Unity 와의 메커니즘 일치* 확인.
- Jump 의 *2번 점프* 원인 진단 — *trigger 재발사 vs 자산 baked* 가설 분리. 진단 로그 (Animator transition log) 추가로 *1회 transition* 확인 → 자산 측 원인 확정.

## 8. 다음 단계

- **FbxConverter CLI 자산 변환 도구** (새 milestone) — .fbx + importTransform 입력 → 정상화된 .fbx 출력. baked-in 으로 우리 코드 측 importTransform 합성 제거 가능.
- 5-M3 Editor 그래프 UI (ImNodes vendored) — Animator state machine 시각 편집.
- Layer/Mask (Phase 5-M4+).

## 9. PPT 재료

- Blend Tree 1D 평가 다이어그램 (Speed 값에 따른 인접 motion 의 weight).
- Animator state machine 의 Locomotion (blend tree) + Jump (단일 clip) 구조.
- Mixamo Without-Skin 자산의 *bone rotation only* 한계와 exitTime 단축 접근.
