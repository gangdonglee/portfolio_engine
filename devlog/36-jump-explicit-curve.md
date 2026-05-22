# 36. 점프 Y를 시간 기반 명시적 곡선으로 교체 (Mixamo 자산 한계 우회 — 2차 시도)

- **날짜**: 2026-05-22
- **관련 커밋**: (이 작업 후 커밋)
- **소요 시간**: ~1시간
- **단계**: Phase 5 — Animator 점프 정합 (재시도)

---

## 1. 목표

devlog 35의 *자동 발-바닥 정렬 (footY 추적)* 이 시각적으로 *공중에 뜬 것처럼* 보이는 문제를 해결. 점프 Y 계산을 *불안정한 bone palette translation 추적* 에서 *Jump state time 기반 명시적 단계 곡선* 으로 교체.

## 2. 사전 컨텍스트

[devlog 35](35-imgui-animator-debug-and-foot-align.md) 에서 footY (LeftFoot 의 mesh-local Y) 와 bindY (Idle 안정화 시점의 footY) 차이로 매 frame `xform.position.y` 자동 보정. devlog 35의 자동 진단 로그상으로는 수치가 맞아 보였으나 (footY -19.86 → instY -15.92), 실제 시각 확인 결과 *도약 (도움닫기 phase) 에서 캐릭터가 공중에 뜬 것처럼* 보임. 사용자 보고.

## 3. 결정과 트레이드오프

### 3-1. footY 추적 폐기 vs 정밀 보정

- **결정**: footY 추적을 점프 식에서 *완전히 제거*. 대신 *Jump state time* 만 입력으로 받아 *코드에서 직접 단계별 Y 곡선* 계산.
- **후보**: footY 추적의 좌표계/스케일 보정으로 시각 일치시키기. Foot IK 도입.
- **선택 이유**: bone palette translation 의 mesh-local 좌표계와 world floor 의 단위 매핑이 자산별로 다름 (Mixamo 좌표계, importTransform 회전, 스케일). 정밀한 매핑은 자산별 튜닝이 필요해 *일반화 어려움*. 반면 명시적 곡선은 *완전히 예측 가능* 하고 *ImGui slider 로 즉시 튜닝* 가능.
- **포기한 것**: 다양한 점프 자산에 자동 적응하는 메커니즘. 자산별 단계 비율 (kTakeoffNorm/kLandingNorm) 은 *수동 측정* 후 코드에 박아야 함.

### 3-2. Locomotion 에서 y 제어 여부

- **결정**: Locomotion(=Jump 아닌 state) 일 때 `xform.position.y = 0` 으로 고정 (절대 안 건드림).
- **후보**: Idle 호흡 / Walking 골반 swing 을 살리려고 Locomotion 에서도 *완화된* footY 보정 적용.
- **선택 이유**: devlog 35 코드가 *항상* footY 보정을 깔아두는 바람에 Walking 한 발 들 때마다 *몸 전체* 가 같이 출렁이는 부작용. Walking 의 골반 swing 은 *bone rotation* 만으로 충분히 표현되므로 instance.position 까지 건드릴 필요 없음.
- **포기한 것**: Idle 호흡의 *몸 전체* 미세 bob (자산 자체에 없으니 어차피 표현 안 됨).

### 3-3. 단계별 Y 곡선의 형태

- **결정**:
  - 도움닫기 `0 ~ takeoffT`: `y = -crouchDepth · u²` (u=t/takeoffT) — 제곱 ease-in 으로 *가속하며 내려감*.
  - 활공 `takeoffT ~ landingT`: `y = -crouchDepth + (peak+crouchDepth) · 4t(1-t)` — crouch 바닥에서 시작해 peak 까지 포물선.
  - 착지 `landingT ~ end`: `y = -landingSquat · (1-t)` — squat 마무리 후 선형으로 0.
- **후보**: 도움닫기 선형 / 활공 cosine / 착지 ease-out 등.
- **선택 이유**: 도움닫기는 다리를 *천천히 굽히기 시작 → 가속* 이 자연스러움 (u²). 활공은 중력 포물선 (4t(1-t)). 착지는 *바닥 닿은 직후 squat → 선형 복귀* 가 시각적으로 충돌 안 됨.
- **포기한 것**: 더 정교한 곡선 (cubic spline 등). 현 단계는 *3 slider 로 튜닝* 가능한 단순한 식이 우선.

## 4. 작업 내용

### 4-1. Application.h — 멤버 정리

[Client/Application.h](../Client/Application.h):
- `m_jumpPeakHeight` 기본값 50 → 80.
- `m_crouchDepth = 25.0f` 추가 (도움닫기 깊이).
- `m_landingSquat = 15.0f` 추가 (착지 squat).
- `m_footBindY / m_footBindCaptured / m_bindCaptureTimer` 는 *디버그 표시용* 으로만 유지 (점프 식에서 분리).

### 4-2. Application.cpp — 점프 Y 단계 곡선

[Client/Application.cpp](../Client/Application.cpp):
```cpp
float jumpY = 0.0f;
if (state == "Jump")
{
    if (stateTime < takeoffT) {
        const float u = stateTime / takeoffT;
        jumpY = -m_crouchDepth * (u * u);
    } else if (stateTime < landingT) {
        const float t   = (stateTime - takeoffT) / (landingT - takeoffT);
        const float amp = m_jumpPeakHeight + m_crouchDepth;
        jumpY = -m_crouchDepth + amp * 4.0f * t * (1.0f - t);
    } else {
        const float t = (stateTime - landingT) / (duration - landingT);
        jumpY = -m_landingSquat * (1.0f - t);
    }
}
xform->position.y = jumpY;
```
- Locomotion 에서는 `jumpY = 0` 그대로 적용 → Walking 흔들림 원천 차단.
- footY 측정은 그대로 두되 *디버그 표시 외* 영향 없음.

### 4-3. ImGui Animator Debug 패널

[Application.cpp `DrawAnimatorPanel`](../Client/Application.cpp):
- Slider: `Crouch Depth (0~100)`, `Jump Peak (0~300)`, `Landing Squat (0~100)`.
- footY/bindY 값은 *디버그 라벨* 로 유지 (`debug:` prefix 로 의도 명확화).

## 5. 마주친 문제와 해결 ⚠

### 문제 1 — devlog 35 의 자동 floor align 이 *시각* 과 안 맞음

- **문제**: footY 추적 식 `jumpY = (footY - bindY)` 의 *수치 검증* 은 정합 — 도움닫기에서 footY 가 감소(-19), 몸도 같은 부호로 내려감(-15). 그러나 시각으로는 *공중부양*.
- **원인**: bone palette translation 은 *mesh-local 모델 공간* 좌표. world 의 floor (Y=0) 와의 *단위/scale 매핑이 불명확*. importTransform 의 회전/스케일이 들어간 후 *시각 단위* 와 *bone palette 단위* 가 1:1 아님. 수치는 같은 방향으로 움직이지만 *크기 비율* 이 시각과 어긋남.
- **해결**: footY 추적 *폐기*. 시간 기반 명시적 곡선으로 전환 — 좌표계 의존성 없음, slider 로 즉시 튜닝.
- **교훈**: bone palette translation 으로 *world 단위 보정* 시도 자체가 잘못된 접근. mesh-local 값은 *자산 내부 비교* (다른 본과의 상대 위치) 에는 쓸 수 있어도 *world floor 와의 절대 정렬* 용으로는 부적합.

### 문제 2 — devlog 35 코드가 Locomotion 에서도 매 frame y 덮어씀

- **문제**: footY 보정이 *모든 state* 에 적용. Walking 한 발 들 때마다 몸 전체 출렁 + Idle 호흡 미세 떨림.
- **원인**: `jumpY` 계산이 `if (Jump) { jumpY += peak·4t(1-t) }` 였고, 그 *밖에서* `jumpY = (footY - bindY)` 가 항상 깔림. Locomotion 에서도 적용됨.
- **해결**: 점프 식 전체를 `if (state == "Jump") { ... } else { jumpY = 0; }` 구조로 격리.
- **교훈**: state-specific 효과는 *state 안에서만* 적용되는지 코드 구조로 강제. *바깥 default 값* 이 의도치 않게 흘러나가는지 확인.

## 6. 결과 / 검증

- 빌드: Debug x64 성공 (Engine.lib + Client.exe).
- 빠른 검증 항목 (사용자 시각 확인 대기):
  - [ ] 평상시 (Idle, 1키 Walking) 에 *몸 흔들림 없음* (xform.position.y = 0 고정).
  - [ ] Space → Jump 시:
    - 도움닫기: 다리 굽힘과 *동시에* 몸이 점진적으로 내려감 (공중부양 해소).
    - 활공: 몸이 위로 솟구침 (peak height 만큼).
    - 착지: 살짝 squat 후 0 복귀.
  - [ ] Jump 끝나면 즉시 y=0 으로 복귀 (Locomotion 진입).
- ImGui slider 로 *시각 보며 즉시 튜닝* 가능 — Crouch/Peak/Squat.

## 7. AI 협업 메모

- 1차 시도 (devlog 35) 의 *수치 진단 vs 시각 검증 괴리* 가 다시 확인됨. *수치가 맞으면 시각도 맞다* 는 가정이 *좌표계가 동일* 일 때만 성립. mesh-local 좌표는 world 와 *동일 좌표계가 아님*.
- 이번 접근은 *좌표계 의존성 자체를 제거* 하고 *코드가 결정하는 명시적 값* 만 사용. ImGui slider 와 결합 시 *시각 검증 사이클이 빨라짐* (slider 드래그 → 즉시 결과).

## 8. 다음 단계

- 사용자 시각 검증 후 Crouch/Peak/Squat 기본값 조정 (현 25/80/15).
- (선택) 점프 trigger 직후 *수평 forward 약간 이동* — 제자리 점프 → 도약 같은 시각 효과.
- FbxConverter CLI (devlog 33의 이월) — 자산 측 bone translation 정상화. 성공 시 코드 측 명시 곡선 불필요.
- Phase 5-M3 Editor 그래프 UI (ImNodes).

## 9. PPT 재료

- *footY 추적 → 시간 기반 명시 곡선* 전환의 *원인 분석* — bone palette translation 의 좌표계 한계.
- ImGui Animator Debug 패널의 *실시간 곡선 튜닝* — Crouch/Peak/Squat slider.
- Mixamo Without-Skin 자산의 *bone translation = 0* 한계를 *코드 측 단계 곡선* 으로 보완하는 전략.
