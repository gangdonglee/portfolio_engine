# 29. Phase 5-M0 — Animator Controller 데이터 모델 + JSON I/O 🎛

- **날짜**: 2026-05-19
- **관련 커밋**: (이 단계 커밋)
- **소요 시간**: 약 2시간
- **단계**: Phase 5 — Animator System (M0/4)

---

## 1. 목표

사용자 의도 *"유니티처럼 사용자 편의에 맞게 animator 작업"*. Unity Mecanim 의 핵심 — *데이터 주도 스테이트 머신* — 을 풀스크래치로. 5-M0 는 **데이터 모델 + JSON I/O + Editor 데이터를 Client 가 읽는 흐름** 검증.

기능:
- `engine::anim::AnimatorController` POD (State / Transition / Parameter).
- JSON 직렬화 — `.animator.json`.
- Scene 의 `MeshInstance.animatorControllerPath` 필드 — controller 자산 참조.
- Editor Inspector 에 controller path 위젯.
- Client 가 부팅 시 controller 로드 + 카운트 로그 (런타임 평가는 M1).

## 2. 사전 컨텍스트

- 27/28 단계: Mixamo X Bot 캐릭터 + Without Skin 클립 6개 (`Breathing Idle` / `Walking` / `Running` / `Jump` / `Smash` / `Dying Backwards`).
- 28 단계 마지막에 `MeshInstance.animationClipPath` 추가 — 한 인스턴스 = 한 클립 자동 재생. *스테이트 머신 없음*.
- 5-M0 는 그 위에 *Animator Controller* 자산 도입 — 한 인스턴스 = 한 controller (다중 클립 + 전이 조건).

## 3. 결정과 트레이드오프

### 29-1. 단계 분할 (M0 → M1 → M2 → M3)

- **결정**: 사용자 동의로 5-M0 (데이터+로드) → 5-M1 (런타임 평가) → 5-M2 (Blend Tree) → 5-M3 (그래프 UI). 각 단계 매 마일스톤 *Editor → Client* 통합 검증.
- **포기한 것**: 한 번에 그래프 UI 까지 (~10h). 메모리 룰의 "작은 커밋 자주" 와 충돌.

### 29-2. 데이터 모델 — Unity Mecanim 1:1 매핑

- **결정**: `AnimatorState`(motion clip path + loop + speed) / `AnimatorParameter`(Bool/Int/Float/Trigger, 단일 float 슬롯) / `AnimatorTransition`(from/to + crossfade + hasExitTime + conditions AND) / `AnimatorController` (states + transitions + parameters + defaultStateName).
- **이유**: 사용자 의도 *"유니티처럼"*. 도메인 용어 그대로 매핑하면 학습 곡선 0 — 게임 디자이너가 Unity 경험 있으면 즉시 이해.
- **포기한 것**: 다중 Layer / Avatar Mask / Sub-State Machine / Blend Tree 2D — M4+ 이월.

### 29-3. Parameter 슬롯 — 단일 float (타입은 자기검사 메타)

- **결정**: Bool/Int/Float/Trigger 모두 `float defaultValue` 1 슬롯에 저장. `type` 은 직렬화/Editor UX (위젯 선택, condition op 후보) 용 메타.
- **이유**: 런타임 평가 (M1) 가 *조건 비교* 시 모든 타입을 float 비교로 통일. Bool=0/1, Trigger=0/1+자동 reset, Int=정수값 그대로, Float=그대로. 메모리 효율 + 코드 단순.
- **포기한 것**: 타입 안전성. `SetFloat(triggerName, ...)` 오용 가능 — Editor UI 에서 타입 위젯 분리로 우회.

### 29-4. enum class PascalCase — Mecanim 도메인 매핑 우선

- **결정**: `ParameterType::Bool/Int/Float/Trigger` + `ConditionOp::IfTrue/IfFalse/Greater/Less/Equals/NotEquals` 모두 PascalCase.
- **이유**: CODE_STYLE §3-1 이 "UPPER_SNAKE 또는 PascalCase (시맨틱에 따라)". Unity Mecanim 의 enum 값과 동일 표기 — 사용자/외부 자료 참조 용이.
- **현 엔진 컨벤션**: 기존 `CONSTANT_BUFFER_TYPE::TRANSFORM` 같은 UPPER_SNAKE 와 *섞임*. 후속 docs 갱신 필요 — 의식적 예외 명시.

### 29-5. SceneRuntime 의 controller 로드 — M0 는 *로그만*

- **결정**: ctor 안에서 `animatorControllerPath` 있는 인스턴스마다 `LoadJson` + 카운트 로그. *런타임 평가 / Animator 활성화는 M1*.
- **이유**: M0 의 검증 가치 = *Editor 가 만든 데이터를 Client 가 정확히 읽는지*. 평가까지 한 번에 가면 디버깅 영역이 데이터/평가 양쪽으로 퍼짐.

## 4. 작업 내용

### 4-1. `Engine/anim/` 신규 모듈

[Engine/anim/AnimatorController.h](../Engine/anim/AnimatorController.h):
```cpp
struct AnimatorState {
    std::string name;
    std::string motionClipPath;
    bool        loop  = true;
    float       speed = 1.0f;
};

enum class ParameterType : uint8 { Bool, Int, Float, Trigger };
struct AnimatorParameter {
    std::string    name;
    ParameterType  type         = ParameterType::Bool;
    float          defaultValue = 0.0f;
};

enum class ConditionOp : uint8 {
    IfTrue, IfFalse, Greater, Less, Equals, NotEquals
};
struct TransitionCondition {
    std::string  parameterName;
    ConditionOp  op;
    float        value;
};

struct AnimatorTransition {
    std::string                       fromStateName;   // 빈 문자열 = Any State
    std::string                       toStateName;
    std::vector<TransitionCondition>  conditions;       // AND
    float                             crossfadeDuration = 0.2f;
    bool                              hasExitTime       = false;
    float                             exitTime          = 1.0f;
};

struct AnimatorController {
    std::string                       name;
    std::vector<AnimatorState>        states;
    std::vector<AnimatorTransition>   transitions;
    std::vector<AnimatorParameter>    parameters;
    std::string                       defaultStateName;
};
```

[Engine/anim/AnimatorSerializer.cpp](../Engine/anim/AnimatorSerializer.cpp): SceneSerializer 와 동형 패턴 (json + ifstream/ofstream + 키 누락 fallback). enum class ↔ string 매핑은 익명 namespace 의 `ToStr` 오버로드 + `Parse*` 함수. *알려지지 않은 enum string* 시 LogInfo WARN 출력 (silent default 보강).

### 4-2. Scene 확장

[Engine/scene/Scene.h](../Engine/scene/Scene.h):
```cpp
struct MeshInstance {
    ...
    std::string animatorControllerPath;  // 선택 — .animator.json 경로
};
```
SceneSerializer 가 빈 문자열일 때 키 생략 — animationClipPath 와 동일 패턴.

### 4-3. Client/SceneRuntime 통합

`SceneRuntime::SceneRuntime` ctor 안에서 controller 로드 시도:
```cpp
for (const auto& inst : m_scene.meshes) {
    if (inst.animatorControllerPath.empty()) continue;
    try {
        auto path = std::filesystem::absolute(inst.animatorControllerPath).string();
        auto c = engine::anim::LoadJson(path);
        LogInfo("[anim] controller loaded: ... (states=N, transitions=M, params=K, default=...)");
    } catch (const std::exception& e) {
        LogInfo("[anim] controller load FAILED: ...");
    }
}
```

### 4-4. Editor Inspector

[Editor/Panels.cpp](../Editor/Panels.cpp) 의 MeshInstance 분기에 `animatorControllerPath` InputText + `clear##animator` 버튼 추가. ID 충돌 회피 (`##anim` 과 별도).

### 4-5. `assets/Animators/xbot.animator.json` — 데모

X Bot 의 Mecanim 표준 패턴:
- **States**: Idle (Breathing Idle) / Walk (Walking) / Run (Running) / Jump (Jump).
- **Parameters**: `Speed: Float`, `Jump: Trigger`.
- **Transitions** (6 개):
  - Idle → Walk (Speed > 0.1)
  - Walk → Run (Speed > 0.5)
  - Run → Walk (Speed < 0.5)
  - Walk → Idle (Speed < 0.1)
  - Any State → Jump (Jump trigger)
  - Jump → Idle (hasExitTime, 클립 끝 자동)

`assets/Scenes/xbot_running.scene.json` 의 메시 인스턴스에 `animatorControllerPath: "assets/Animators/xbot.animator.json"` 추가.

## 5. 마주친 문제와 해결 ⚠ 필수

### 문제 1 — Parameter 타입 통일 vs 안전성 (디자인 결정)
- **문제**: Bool/Int/Float/Trigger 를 *별도 슬롯 (variant 또는 union)* vs *단일 float* 중 선택.
- **해결**: 단일 float — 런타임 조건 비교 통일 + 메모리 효율. 타입 안전성은 Editor UI 측에서 위젯 분리.
- **교훈**: 런타임 표현 (float 1슬롯) 과 의도 메타 (type) 를 분리하면 타입 안전성 *외부* 화 가능.

### 문제 2 — Mecanim 도메인 enum 의 표기 — CODE_STYLE 충돌
- **문제**: 기존 엔진 enum 은 `CONSTANT_BUFFER_TYPE::TRANSFORM` UPPER_SNAKE. Mecanim 의 `ParameterType.Bool` 은 PascalCase.
- **해결**: 도메인 매핑 우선 — PascalCase 의식적 채택. Editor 의 학습 곡선 (Unity 와 정확히 같은 enum 값) 가치 > 일관성.
- **이월**: CODE_STYLE.md 에 *enum class 표기 정책의 도메인 예외* 명시 — 후속 docs 단계.

### 문제 3 — Reviewer 지적: `fromStateName == ""` Any State sentinel (매직 값)
- **상태**: 의식적 결정. Unity Mecanim 도 동일 패턴 (`Animator.SetTrigger` 가 Any State 트리거).
- **이월**: `std::optional<std::string>` 으로 명시화 — 분량 작지만 SceneRuntime 측 코드 변경. M1 의 런타임 평가 작성 시 같이.

### 문제 4 — Reviewer 시정 1건 적용: parse fallback 명시 로그
- **수정**: `ParseParamType` / `ParseConditionOp` 가 알려지지 않은 string 받을 때 LogInfo WARN + fallback. silent default 의 디버깅 어려움 회피.

## 6. 결과 / 검증

- **빌드 (Debug + Release)**: 0 warning / 0 error.
- **Client.exe 자동 실행 (xbot_running 슬롯 격리 부팅)**:
  ```
  [scene] boot loaded: assets/Scenes\xbot_running.scene.json
  [render] FBX loaded: ...X Bot.fbx (bones=65, clips=2)
  [anim] controller loaded: assets/Animators/xbot.animator.json
         (states=4, transitions=6, params=2, default=Idle)
  [render] FBX animation-only loaded: clips=1, bones-matched=65/65 (...Running.fbx)
  ```
- **Editor → Client 라운드트립 검증** (수동):
  - Editor.exe 실행 → xbot_running.scene.json Open → Inspector 가 `animatorControllerPath` 표시.
  - 다른 .animator.json 경로로 변경 → Save → Client 재실행 → 새 controller 카운트 로그.

## 7. AI 협업 메모

- 사용자 한 줄 *"유니티처럼 사용자 편의에 맞게 animator 작업"* + *"client 에 붙이는 작업도 추가"* 명령에서 **단계 분할 + 매 단계 Editor→Client 통합** 흐름 도출. 메모리 룰 (작은 커밋) 과 정합.
- Reviewer 가 *enum 표기 일관성* / *Any State sentinel* / *parse silent default* 셋 다 지적. silent default 만 즉시 시정, 나머지는 명시 이월 — 분량 비용 대비 가치 판단.

## 8. 다음 단계 — 5-M1 (State Machine 런타임)

- `engine::anim::AnimatorRuntime` 클래스 — controller 받아 평가:
  - 현재 state + active time + 다음 state crossfade 진행도.
  - `SetBool/Int/Float(name, value)` + `SetTrigger(name)` API.
  - Tick(dt) 가 transition 평가 + state 변경 + 클립 시간 진행.
- `client::SceneRuntime` 의 Animator 가 *AnimClip 직접* 대신 *AnimatorRuntime* 사용.
  - 현재 state 의 motionClipPath → 이미 캐시된 클립 자산 → 본 팔레트 계산.
  - Crossfade 중에는 두 state 의 본 팔레트 weight 보간.
- Client 키 입력 매핑:
  - 좌우 화살표 → Speed parameter 증감.
  - Space → Jump trigger.
- Editor Inspector 의 controller 편집 위젯 — InputText 단일 path → 추후 M3 그래프.

## 9. PPT 재료로 쓸 만한 포인트

- "Animator Controller = 데이터 + 런타임 + 시각 편집의 3 분리 — 같은 .animator.json 을 Editor 가 만들고 Client 가 읽는다. 라운드트립 검증으로 데이터 모델 무결성 보장."
- "Mecanim 도메인 1:1 매핑 — PascalCase enum, Any State sentinel, Trigger 자동 reset 등 Unity 사용자가 즉시 이해."
- "단계 분할 — M0 (데이터 모델만) → M1 (런타임) → M2 (Blend Tree) → M3 (그래프 UI). 매 단계 Editor → Client 라운드트립이 *진정한* 완료 기준."
