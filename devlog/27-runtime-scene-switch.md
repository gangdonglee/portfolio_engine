# 27. M3a — Client 런타임 씬 전환 (F1..F9 슬롯) 🔀

- **날짜**: 2026-05-18
- **관련 커밋**: (이 단계 커밋)
- **소요 시간**: 약 1.5시간
- **단계**: Phase 4 — 멀티 씬 지원 (M3a/2)

---

## 1. 목표

사용자 지적 *"씬이 하나가 아닐텐데 여러 개일 경우도 생각해서 만들었어?"* 의 첫 시나리오 — **Client 런타임 씬 전환**. 액션 게임의 챔버 전환(로비→챔버1→챔버2)에 필수.

기능:
- 부팅 시 `assets/Scenes/*.scene.json` 알파벳 순 스캔 → 최대 9 슬롯.
- 슬롯 0 의 씬으로 자동 부팅 (이전엔 `sample.scene.json` 하드코딩).
- F1..F9 키 → 슬롯 0..8 의 씬으로 런타임 전환. SceneRuntime 폐기 + 재생성 + GPU 자산 재할당.

## 2. 사전 컨텍스트

- 25 단계: Client/main.cpp 4 클래스 분할 (Application/SceneRuntime/FrameRenderer/InputController).
- 26 단계 (M2): Editor 가 여러 .scene.json 을 다룰 수 있게 (Open/Save 다이얼로그).
- 본 단계 직전: Client 가 부팅 시 `assets/Scenes/sample.scene.json` 하드코딩 1개만 로드. 런타임 전환 메커니즘 없음.
- 사용자 결정: **Client 런타임 씬 전환 + Editor 멀티 씬 탭 (M3b)** 두 시나리오. 본 단계는 (a) Client.

## 3. 결정과 트레이드오프

### 27-1. 키 매핑 — F1..F9 (assets/Scenes/*.scene.json 알파벳 순)

- **결정**: 부팅 시 디렉토리 스캔 → 최대 9 슬롯. F1=슬롯0, F2=슬롯1, ... F9=슬롯8.
- **후보**:
  - A) 콘솔 인자 / 환경 변수 — 부팅 시 1개만 결정, 런타임 전환 없음. 거부 (요구 미달).
  - B) F1..F9 ad-hoc 매핑 — 본 결정. 디버그/데모용 직관적.
  - C) 메뉴 UI — Client 는 ImGui 없음 (Editor 와 달리). M4+ 의 Client-side debug overlay 도입 시.
- **선택 이유**: B. 분량 작음, 직관적. 슬롯 9 한계는 데모 규모에서 충분.

### 27-2. SceneRuntime 폐기 + 재생성 — *Build-Then-Swap* 패턴

- **결정** (리뷰 시정 반영): `ChangeScene` 안에서
  1. **Build phase** (실패 무손실): `LoadJson(path)` 먼저 시도. 실패 시 throw → 기존 SceneRuntime 보존.
  2. **Teardown phase** (성공 보장 후): `FlushGpu` → `SceneRuntime.reset()` → `SrvHeap::Reset()` → fallback SRV 재등록.
  3. **Build SceneRuntime**: 카메라 reset → 새 SceneRuntime 생성 → `FrameRenderer::OnResize()`.
- **이유**: 강한 예외 안전성. 초안 (Teardown → Build) 은 파싱 실패 시 *기존 자산 폐기 + 새 자산 없음* = 무효 상태 → WM_CLOSE 강제 종료. Build-Then-Swap 은 LoadJson 실패 시 기존 씬 그대로.
- **포기한 것**: SceneRuntime ctor 실패 (GPU OOM 등 드문) 시 여전히 무효 상태. Tick 시작에 nullptr 가드로 한 프레임 skip + WM_CLOSE.

### 27-3. SrvDescriptorHeap 슬롯 reset

- **결정**: `SrvDescriptorHeap::Reset()` 메서드 추가 — `m_count = 0` 만. 디스크립터 힙 자체는 그대로 (덮어쓰기 안전).
- **이유**: 씬 전환마다 FBX 머티리얼 텍스처들이 슬롯 N개 소비. capacity 64 면 전환 ~몇 번 후 OOB. Reset 이 가장 단순.
- **호출자 책임**: GPU 가 기존 디스크립터 미참조 보장 (FlushGpu 선행) — 헤더 주석 명시.

### 27-4. m_bootCmdList 라이프타임 — Application 끝까지 보존

- **결정**: 25 단계 (devlog) 의 `LoadSceneAndRuntime` 끝 `m_bootCmdList.reset()` 을 *되돌림*. Application 라이프타임 동안 유지.
- **이유**: ChangeScene 가 SceneRuntime ctor 의 uploadList 로 재사용. 매번 새 CommandList 생성·폐기는 비용 증가.
- **사용자 사전 시정 인지**: 26 단계 작업 도중 사용자가 `m_queue->FlushGpu()` + reset() 패턴을 *FlushGpu 만* 으로 변경 + 멤버 보존 — 본 단계와 정합.

## 4. 작업 내용

### 4-1. `Engine/render/SrvDescriptorHeap::Reset()`

[Engine/render/SrvDescriptorHeap.h](../Engine/render/SrvDescriptorHeap.h):
```cpp
void Reset() noexcept { m_count = 0; }
```
헤더 주석에 호출자 책임 (GPU 미참조 보장) 명시.

### 4-2. `Client/InputController::ConsumeSceneSwitch()`

[Client/InputController.h](../Client/InputController.h):
```cpp
static constexpr int kNoSceneSwitch = -1;
static constexpr int kSceneSwitchSlotCount = 9;
int ConsumeSceneSwitch();  // -1 또는 0..8
```
`Tick(input)` 안에서 `VK_F1..VK_F9` 다운 엣지 추적. `m_prevClipDown` 와 `m_prevSceneDown` 별도 배열.

### 4-3. `Client/Application::ChangeScene` + `ScanSceneSlots`

[Client/Application.cpp](../Client/Application.cpp):

```cpp
void Application::ChangeScene(const std::string& scenePath)
{
    // Build phase (실패 무손실)
    engine::scene::Scene newScene = engine::scene::LoadJson(scenePath);

    // Teardown phase
    m_queue->FlushGpu();
    m_sceneRuntime.reset();
    m_srvHeap->Reset();
    m_fallbackAlbedo->CreateSrv(*m_device, *m_srvHeap);

    // Build SceneRuntime
    m_camera->SetPosition(newScene.cameraStart.position);
    /* ... 카메라 reset ... */
    m_sceneRuntime = std::make_unique<SceneRuntime>(
        *m_device, *m_queue, *m_bootCmdList, *m_srvHeap, std::move(newScene));
    m_frameRenderer->OnResize();
    m_currentScenePath = scenePath;
}
```

`ScanSceneSlots()`: `assets/Scenes` 디렉토리에서 `.scene.json` suffix 매칭 + `std::sort` 알파벳 순 + 최대 `kSceneSwitchSlotCount` 슬롯. 부팅 시 로그 출력.

### 4-4. `Application::Tick` 의 입력 처리 순서

씬 전환 → 클립 전환 순. 씬 전환이 SceneRuntime 을 폐기·재생성하므로 *그 후의* 클립 전환은 *새* SceneRuntime 에 적용.

```cpp
if (!m_sceneRuntime) { return; }   // nullptr 가드 — ChangeScene 실패 후 한 프레임 skip

m_inputController.Tick(input);
const int sceneSlot = m_inputController.ConsumeSceneSwitch();
if (sceneSlot != kNoSceneSwitch && sceneSlot < m_sceneSlots.size()
    && m_sceneSlots[sceneSlot] != m_currentScenePath)
{
    try { ChangeScene(m_sceneSlots[sceneSlot]); }
    catch (const std::exception& e) {
        // LoadJson 실패 — 기존 SceneRuntime 유지 (Build-Then-Swap 덕분).
        // SceneRuntime ctor 실패는 nullptr — WM_CLOSE.
        if (!m_sceneRuntime) { PostMessageW(... WM_CLOSE ...); return; }
    }
}

const int clipChange = m_inputController.ConsumeClipChange();
if (clipChange != kNoClipChange) { m_sceneRuntime->SetActiveClip(clipChange); }
```

## 5. 마주친 문제와 해결 ⚠ 필수

### 문제 1 — SceneRuntime 폐기 후 LoadJson 실패 시 무효 상태 (OOP 위반, 리뷰)
- **문제**: 초안의 step 순서가 ① FlushGpu → ② reset → ③ SrvHeap reset → ④ LoadJson. ④ 가 파싱 오류로 throw 시 ②③ 이미 진행됨 → 기존 자산 폐기 + 새 자산 없음. 호출자 catch 가 `WM_CLOSE` 강제 종료 (생존 불가).
- **원인**: "tear down → build up" 직관적 순서. 강한 예외 안전성 미고려.
- **해결**: **Build-Then-Swap 패턴**. LoadJson 을 step ① 로 끌어올림 — 실패 시 throw 가 기존 자산을 손상 없이 전파.
- **교훈**: 자원 폐기와 신규 구성이 한 메서드에 섞일 때 *깨지기 쉬운 step* 을 가장 먼저. 실패 시 invariant 보존.

### 문제 2 — SceneRuntime ctor 실패 시 nullptr 상태 (DX12 Critical, 리뷰)
- **문제**: Build-Then-Swap 후에도 SceneRuntime ctor 자체가 throw 할 수 있음 (FBX 누락, GPU OOM 등). 이 경우 `m_sceneRuntime == nullptr` 로 남고 다음 Tick 의 `m_sceneRuntime->Tick` 가 nullptr 역참조.
- **원인**: ctor 실패 시점이 Build phase 끝 — 이미 SrvHeap reset 됨. 기존 자산 회복 불가.
- **해결**: `Tick` 시작에 `if (!m_sceneRuntime) return;` 가드. `WM_CLOSE` 가 다음 PumpMessages 에서 처리되므로 한 프레임만 skip 후 메인 루프 종료. 실 운용에선 sentinel scene 폴백 권장 (M5+).
- **교훈**: 약한 예외 보장 메서드의 호출자는 *비정상 상태 가드* 가 필요. ctor 가 throw 할 수 있는 모든 객체는 보유자 측에서 nullable 가정.

### 문제 3 — FlushGpu 2회 → 1회 (DX12 Warning, 리뷰)
- **문제**: 초안에 step ① 와 ⑦ 둘 다 FlushGpu. ⑦ 은 FBX/텍스처 업로드 후 다음 프레임 안전성. 그러나 `Texture` ctor + `FbxLoader` 가 *이미 내부 FlushGpu* 함.
- **해결**: ⑦ 제거. `FrameRenderer::OnResize()` 만 호출 — fence value reset.
- **교훈**: 의존성 내부 동기화를 모르고 *방어적* FlushGpu 추가는 비용만 늘림. 호출자 책임을 정확히 파악.

### 문제 4 — Editor 가 만든 from_editor.scene.json 이 알파벳 상 앞 → 부팅 씬이 sample 아닌 from_editor
- **상황**: 26 단계 (M2) 작업 중 Editor 가 `from_editor.scene.json` 저장. 이름이 'f' 로 시작 → 알파벳 순 sample('s') 앞.
- **결정**: 의도된 동작 — 사용자가 마지막 편집한 씬이 부팅 시 자동 로드되는 *데모용 편의*. 부담 없음. 문제 시 알파벳 순이 아닌 명시적 우선순위 (sample 또는 default name 지정) 도입.

## 6. 결과 / 검증

- **빌드 (Debug + Release)**: Engine + Client + Editor 모두 0 warning / 0 error.
- **Client.exe 자동 실행 (3초)**: 부팅 로그 확인
  - `[scene] slots scanned: F1 = .../from_editor.scene.json / F2 = .../sample.scene.json`
  - `[scene] boot loaded: ...from_editor.scene.json`
  - `[input] F1..F9 = scene slot select.`
- **수동 검증 (사용자 부탁)**:
  1. Client.exe 실행 → F2 누르면 sample.scene.json 으로 전환 (Dragon + 2 dir + 2 point 라이트 합성).
  2. F1 누르면 from_editor.scene.json 으로 복귀 (Dragon + Sun + RimLight).
  3. Editor 에서 라이트 추가 + Save → Client 의 F1/F2 로 즉시 반영 (재시작 불필요).
- **회귀**: SceneRuntime/FrameRenderer 의 기존 API 변경 없음 — M1/M2 동작 보존.

## 7. AI 협업 메모

- 사용자 지적이 *"이미 됐는지 확인" + 시나리오 선택* 패턴이라 작업 범위가 명확했음. 두 시나리오 (Client / Editor) 분할 결정도 빠르게.
- Build-Then-Swap 패턴은 자체 검토에서 놓친 항목 — 리뷰어 가치 재확인.
- 사용자가 25 단계의 `m_bootCmdList.reset()` 을 *FlushGpu 만* 으로 변경한 사전 시정이 27 단계의 m_bootCmdList 재사용을 정확히 뒷받침. 병렬 작업의 의도 정합.

## 8. 다음 단계 — M3b

- Editor 의 활성 Scene 을 *vector<{Scene, path, modified, selection}>* 으로 확장 + `ImGui::BeginTabBar/BeginTabItem` 으로 탭 UI.
- File→Open 이 *현재 탭 교체* vs *새 탭* 선택지.
- 탭 닫기 (X 버튼) — modified 상태면 경고 (M5+ 미저장 변경 경고와 통합 가능).
- EDITOR_ROADMAP §3 표 갱신: M3 = 멀티 씬 (a + b), Asset Browser 는 M4 로 이동.

미뤄둔 항목:
- ChangeScene SRP 분해 (TeardownSceneResources / BuildSceneRuntime) — Build-Then-Swap 적용 후엔 함수 안의 phase 주석이 그 역할. 정식 분해는 동적 자산 언로드 도입 시점.
- SceneSlotRegistry 별도 클래스 — 현재 Application 멤버 분량 작아 그대로 OK. 슬롯 개수가 vector 동적이 되면 분리.
- sentinel/default scene fallback — ChangeScene 실패 시 무효 상태 회피. M5+ "안전한 폴백 씬" 패턴.

## 9. PPT 재료로 쓸 만한 포인트

- "F1..F9 런타임 씬 전환 — 게임 챔버 전환의 기본 메커니즘. Build-Then-Swap 으로 파싱 실패 시 기존 씬 무손상."
- "SrvDescriptorHeap::Reset — 씬 전환마다 머티리얼 텍스처 슬롯 회수. 디스크립터 힙은 *덮어쓰기 안전* 한 자원이라는 D3D12 보장 활용."
- "강한 vs 약한 예외 보장 — 메서드 책임에 따라 분리. Build-Then-Swap = 강한 보장 단계 + 약한 보장 단계 분명히."
