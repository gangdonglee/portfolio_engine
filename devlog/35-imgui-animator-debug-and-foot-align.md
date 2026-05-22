# 35. ImGui Animator Debug 패널 + 자동 발-바닥 정렬

- **날짜**: 2026-05-22
- **관련 커밋**: (이 작업 후 커밋)
- **소요 시간**: ~5시간 (시각 검증 도구 부재로 진단 사이클 반복)
- **단계**: Phase 5 — Animator 디버그 + 점프 정합

---

## 1. 목표

- Client.exe 에 ImGui 통합 + Animator Debug 패널 (현재 state, 프로그레스 바, frame slider, Play/Pause).
- Mixamo Without/With-Skin 자산이 *Hips Y translation = 0* 인 한계로 *Jump 시 다리만 굽고 몸 정지 → 공중부양* 문제 해결 — Mixamo viewer 와 동일한 *자동 발-바닥 정렬*.

## 2. 사전 컨텍스트

[devlog 33](33-blend-tree-1d-and-jump-tuning.md) 의 Blend Tree 1D 완료 후 점프 시각 문제 발견. Jump.fbx 의 자산 측 *bone translation track 자체가 없음* (Without/With-Skin 공통 — Mixamo export 메커니즘). 자산 교체로 해결 불가.

## 3. 결정과 트레이드오프

### 3-1. ImGui 통합 위치

- **결정**: Client.exe 에 직접 통합 (Editor 와 동일 패턴). Application 의 init/shutdown + FrameRenderer 의 RenderDrawData.
- **후보**: ImGui 없는 자체 디버그 UI 만들기.
- **선택 이유**: 이미 Editor 에서 검증된 ImGui DX12 backend 패턴 재사용. SliderInt/SliderFloat/ProgressBar 같은 위젯 즉시 사용.
- **포기한 것**: 의존성 ↑ (ImGui 6 cpp + Win32/DX12 backend 추가).

### 3-2. 자동 발-바닥 정렬의 측정 식

- **결정**: AnimatorRuntime 의 BuildPalette 안에서 *currentBone 의 translation Y* (= bone joint 의 mesh-local 위치) 를 별도 vector 에 보존. Application 이 *발 본 (LeftFoot) 의 mesh-local Y* 와 *bind 캡처 값* 차이로 `inst.transform.y` 자동 조정.
- **후보**: bone palette 의 translation 직접 사용.
- **선택 이유**: bone palette = currentBone × offsetMatrix 의 translation 은 *mesh origin 의 변환 결과* 이지 bone joint 위치가 아님. 식의 의미가 다름.
- **포기한 것**: Foot IK 같은 정밀한 위치 보정 (별도 분석 필요).

### 3-3. bind 캡처 시점

- **결정**: 부팅 후 1초 지난 *Idle 안정화 footY* 캡처.
- **후보**: 첫 frame 캡처.
- **선택 이유**: 첫 frame 은 AnimatorRuntime 의 BuildPalette 가 아직 안 돌아서 *footY=0* (잘못된 값) 캡처. 1초 후면 Idle 클립 진행 안정.
- **포기한 것**: 캐시 값 정확도 (Idle 호흡 bob 의 *순간 값* 캡처 — 평균 아님).

## 4. 작업 내용

### 4-1. Client.vcxproj — ImGui 통합

[Client/Client.vcxproj](../Client/Client.vcxproj):
- `<ImGuiRoot>` 경로 추가, AdditionalIncludeDirectories 에 ImGui + backends.
- ClCompile: imgui.cpp / imgui_draw.cpp / imgui_tables.cpp / imgui_widgets.cpp / imgui_impl_win32.cpp / imgui_impl_dx12.cpp.

### 4-2. AnimatorRuntime — Pause/SetStateTime/BoneMeshLocalY

[Engine/anim/AnimatorRuntime.{h,cpp}](../Engine/anim/AnimatorRuntime.cpp):
- `IsPaused() / SetPaused()` — 시간 진행 일시정지 (BuildPalette 는 진행 — slider 조작 반영).
- `SetCurrentStateTime(t)` — frame slider 용.
- `BoneMeshLocalY(boneIdx)` — `currentBone × (0,0,0,1)` 의 translation Y 보존 (BuildPalette 안에서 `m_boneMeshLocalY` 갱신).

### 4-3. SceneRuntime passthrough

[Client/SceneRuntime.{h,cpp}](../Client/SceneRuntime.cpp):
- `AnimatorIsPaused / SetPaused / SetCurrentStateTime / BoneMeshLocalY(name)` 추가.
- `AnimatorInstanceTransform()` — game 코드가 인스턴스 transform 에 write 권한.

### 4-4. Application — ImGui init + Animator Debug 패널

[Client/Application.{h,cpp}](../Client/Application.cpp):
- `InitImGui / ShutdownImGui` — ctor/dtor 에서 호출. WndProcHook 등록.
- `ImGuiSrvAllocator` — Application.cpp anonymous namespace 내 클래스, srvHeap bump-allocator.
- `DrawAnimatorPanel`:
  - state name + stateTime/duration 표시
  - 프로그레스바 (`frame X / Y (Z%)`)
  - Frame slider — Pause 후 드래그로 임의 frame 이동
  - Play/Pause 버튼
  - Jump Peak slider
  - jumpY / footY / bindY 실시간 표시
  - flight phase 명시 (takeoff/landing 비율)

### 4-5. FrameRenderer — RenderDrawData

[Client/FrameRenderer.cpp](../Client/FrameRenderer.cpp):
- Render() 의 Present 직전에 `ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), list)`.
- 동일 RTV/DSV. ImGui 가 자체 PSO 로 list 의 binding 변경 — 후속 코드 없음.

### 4-6. Application — 자동 발-바닥 정렬

```cpp
const float footY = m_sceneRuntime->AnimatorBoneMeshLocalY(L"mixamorig:LeftFoot");
if (!m_footBindCaptured && m_bindCaptureTimer >= 1.0f) {
    m_footBindY        = footY;
    m_footBindCaptured = true;
}
float jumpY = m_footBindCaptured ? (footY - m_footBindY) : 0.0f;

// 활공 phase (24~42 frame) 만 추가 +peak·4t(1-t).
if (state == "Jump" && stateTime ∈ [takeoffT, landingT]) {
    jumpY += m_jumpPeakHeight * 4·t · (1 - t);
}
xform->position.y = jumpY;
```

- 발 본 mesh-local Y 가 *bind 보다 위* (footY < bindY) → jumpY 음수 → 몸 내림 → *발이 floor 에 유지*.
- 활공 phase 만 추가 *양수 점프* — 캐릭터가 실제 위로 떴다 내려옴.

## 5. 마주친 문제와 해결 ⚠

### 5-1. 시각 캡처 도구 부재로 사이클 반복

- *증상*: 사용자가 *점프 이상함* 보고 → 식 수정 → 사용자 시각 확인 → 또 이상 → 반복.
- *원인*: 시각 검증을 자동화 못 함. 수학 분석에 1차 오류 (`bone palette translation` 의 의미 오해 — *mesh origin 변환* vs *bone joint 위치*).
- *해결*: 임시 진단 로그 (자동 Jump trigger + 매 frame footY/bindY/instY 출력) 로 *수치 검증* 가능. 시각 검증을 *수치 검증* 으로 우회.

### 5-2. 1차 식 — `bone palette × (0,0,0,1)` 잘못된 의미

- *증상*: footY 측정해서 보정 식 적용해도 mesh 깨짐.
- *원인*: `bone palette = currentBone × offsetMatrix`. `bone palette × (0,0,0,1) = mesh origin 의 변환 결과` 이지 *bone joint 위치* 가 아님. 진짜 식은 `currentBone × (0,0,0,1) = currentBone translation`.
- *해결*: AnimatorRuntime 의 BuildPalette 안에서 `boneGlobal` (= currentBone) 의 translation Y 를 별도 `m_boneMeshLocalY` 에 보존 + 외부 노출.

### 5-3. bindY 캡처 시점

- *증상*: footBindY 가 0 으로 캡처. jumpY 식이 모두 (footY - 0) = footY 가 되어 *몸이 *위로* 올라감* (공중부양 악화).
- *원인*: 부팅 첫 frame 에서 캡처했는데 *그 시점에 AnimatorRuntime BuildPalette 가 아직 실행 안 됨*. footY = 0.
- *해결*: `m_bindCaptureTimer ≥ 1.0s` 시점에 캡처. Idle 안정화 후 정확한 footY 캡처.

### 5-4. footY 의 *부호* 가 시각과 반대

- *증상*: *발이 위로 올라가면* footY 가 *감소* (-3 → -23). 식이 *증가* 가정 → 부호 반대.
- *원인*: Mixamo 좌표계의 mesh-local 기준. bone palette translation 의 *Y 부호 컨벤션*.
- *해결*: 식 `jumpY = (footY - bindY)` — footY 감소 시 jumpY 음수 → 몸 내림 (정확).

### 5-5. ImGui 통합 시 cpp ItemGroup 누락

- *증상*: ImGui 클래스 미정의 빌드 에러.
- *해결*: Client.vcxproj 에 imgui.cpp 외 imgui_draw/tables/widgets/backend 6 cpp 명시.

## 6. 결과 / 검증

자동 진단 로그 (부팅 후 3초 자동 Jump trigger + 매 frame footY/bindY/instY 출력):

| 단계 | footY | bindY | instY | 의미 |
|---|---|---|---|---|
| Idle 안정화 (1초 후 캡처) | -3.84 | -3.84 | (init 안 됨) | 기준 설정 |
| Idle 후속 (3.0s) | -2.87 | -3.84 | +0.99 | 거의 정지 (호흡 미세) |
| Jump 도움닫기 (3.17s) | -19.86 | -3.84 | -15.92 | *몸이 자동 내림* ✓ |

식 정합 확인 — *footY 감소 (발 올라감) → instY 동등 음수 (몸 내림)*. 공중부양 해소.

## 7. 다음 단계

- 임시 진단 코드 (자동 Jump test) 제거됨. 정식 ImGui Debug 패널만 유지.
- 사용자 시각 검증 후 *kJumpPeakHeight* 등 미세 튜닝 가능.
- 다른 캐릭터 자산 적용 시 발 본 이름 *mixamorig:LeftFoot* 가정 — 다른 자산은 *이름 매핑 설정* 필요.

## 8. AI 협업 메모

- 시각 검증 도구 부재 → 사용자 시각 보고에 의존 → 사이클 반복으로 사용자 좌절. 다음에는 *수치 진단 자동화* 를 빠른 단계에서 도입할 것.
- bone palette 의 translation 의미 오해 (1차 오류) — *행렬 곱셈의 의미를 깊이 분석 후 식 수정* 필요. 표면적 시도는 비효율.
