# 26. M2 — Editor 의 Hierarchy / Inspector 패널 + IFileDialog 🪟

- **날짜**: 2026-05-18
- **관련 커밋**: (이 단계 커밋)
- **소요 시간**: 약 2.5시간
- **단계**: Phase 4 — 맵 에디터 (M2/5)

---

## 1. 목표

EDITOR_ROADMAP §3 의 M2 정의 완료:
> Hierarchy 패널(트리) + Inspector 패널(Transform/color/intensity DragFloat3) + File→New/Open 다이얼로그 + 라이트 추가/제거 버튼.
>
> 검증: Editor 안에서 메시 위치/회전 + 라이트 색/방향 편집 → Save → Client 가 변경 반영.

M1 의 *"라이트 동적 추가/제거"* 사용자 요구가 이번 단계에서 처음으로 *시각적으로* 가시화 됨 — Hierarchy 의 `+ Add DirLight` / `- Remove selected` 버튼.

## 2. 사전 컨텍스트

- M0: ImGui 도킹 + 빈 패널 3개 (Hierarchy/Inspector/Viewport).
- M1: SceneSerializer + StructuredBuffer + 다중 라이트 셰이더 + Client 가 .scene.json 로드.
- M1 의 Editor 는 File→Save 가 *하드코딩 Scene* 만 저장. 실제 편집은 없었음.
- 25 단계에서 Client/main.cpp 분할 완료 — Application/SceneRuntime/FrameRenderer/InputController.
- 본 단계 시작 직전 EDITOR_ROADMAP §3 의 M2 상태는 ⏳.

## 3. 결정과 트레이드오프

### 26-1. 패널 그리기 분리 — `Editor/Panels.h+cpp`

- **결정**: Hierarchy/Inspector 의 ImGui 위젯 코드를 `editor::panels` namespace 자유함수로 분리. `DrawHierarchy(Scene&, Selection&) -> bool` 와 `DrawInspector(Scene&, Selection&) -> bool`.
- **후보**:
  - A) Editor/main.cpp 안에 인라인 — wWinMain 부풀음 (M1 의 552줄 wWinMain 재현).
  - B) `editor::PanelRenderer` 같은 클래스 — 상태가 없으니 클래스 과잉.
  - C) 자유함수 namespace — 현재 채택.
- **선택 이유**: C. 패널 그리기는 *함수형* (입력 → ImGui 사이드 이펙트 + Scene 변경). 클래스가 보유할 *상태* 가 없음. SRP — 한 함수 한 패널.
- **포기한 것**: 향후 패널이 *내부 상태* (예: 검색 필터, 정렬 옵션) 를 가질 때 자유함수가 부족 — 그 시점에 클래스화.

### 26-2. 메뉴 클릭은 *플래그* 만 세팅, 액션은 `ImGui::Render` 후 처리

- **결정**: `wantNew`/`wantOpen`/`wantSave`/`wantSaveAs` 4 플래그. 메뉴 클릭에서 세팅 후 ImGui::Render() 후 분기 처리.
- **이유**: IFileDialog::Show 가 *modal* — 메뉴 클릭 즉시 호출하면 ImGui 의 frame draw data 미완성 상태에서 modal 진입 가능. 플래그 패턴이 안전.
- **부가 시정 (리뷰 반영)**: 다이얼로그 동반 액션 (Open / SaveAs) 직전에 `commandQueue.FlushGpu() + frameFenceValues[*] = 0` — modal 동안 in-flight slot 의 fence value 가 묶여 GPU idle 시간이 모달 길이만큼 늘어나는 것을 회피.

### 26-3. `Selection` 모델 — kind + index POD

- **결정**: `enum class NodeKind { None, SceneRoot, MeshInstance, DirLight, PointLight }` + `struct Selection { kind, index }`.
- **이유**: Hierarchy 의 트리 노드 5종을 단일 변수로 표현. Inspector 의 switch 한 번에 분기. 가장 단순한 PSD (Plain Selection Data).
- **포기한 것**: 멀티 셀렉션 (`std::vector<Selection>`) — M5+ 의 권장 항목으로 미룸.

### 26-4. 다이얼로그 — Win32 IFileDialog (Common Item Dialog API)

- **결정**: `ShowSceneFileDialog(parent, save, outPath)` 자유함수. `CLSID_FileSaveDialog` / `CLSID_FileOpenDialog` 분기. 기본 폴더 = `$(OutDir)assets/Scenes`.
- **후보**:
  - A) **IFileDialog (Vista+ Common Item Dialog)** — Windows 정석. ImGui 의 [Dialog 라이브러리](https://github.com/aiekick/ImGuiFileDialog) 같은 외부 의존 회피.
  - B) `GetOpenFileNameW` (Win32 legacy) — 가능하나 Vista+ API 가 더 깔끔.
  - C) ImGui 자체 파일 다이얼로그 위젯 (외부 라이브러리) — 외부 의존.
- **선택 이유**: A. Windows 기본 + ComPtr 라이프타임 + COM 초기화 만 처리하면 됨. 풀스크래치 정신 보존.

### 26-5. `modified` 플래그 — Hierarchy 구조 변경 + Inspector 필드 변경 통합

- **결정**: `DrawHierarchy` (구조 변경 시 true) 와 `DrawInspector` (필드 변경 시 true) 의 반환값을 합집합 OR. modified 가 true 이면 메뉴바 우측에 "[modified]" 황색 표시 + Save 메뉴는 activeScenePath 가 있을 때만 활성.
- **이유**: 사용자가 *변경을 알아채는* 가장 작은 시그널. 미저장 변경 경고 (창 닫기 시 확인 다이얼로그) 는 M5+ 로 미룸.

## 4. 작업 내용

### 4-1. `Editor/Panels.h+cpp` 신규

[Editor/Panels.h](../Editor/Panels.h):
```cpp
namespace editor::panels
{
    enum class NodeKind : int { None, SceneRoot, MeshInstance, DirLight, PointLight };
    struct Selection { NodeKind kind = NodeKind::None; std::size_t index = 0; };

    bool DrawHierarchy(engine::scene::Scene& scene, Selection& sel);
    bool DrawInspector(engine::scene::Scene& scene, Selection& sel);
}
```

핵심 위젯:
- **Hierarchy**: `TreeNodeEx` 3 그룹 (Meshes / DirLights / PointLights), 각 그룹 안에 `Selectable` 노드 + `SmallButton(+ Add ...)` / `SmallButton(- Remove selected)`.
- **Inspector**: `switch(sel.kind)` 한 번. 각 분기:
  - `SceneRoot`: name(InputText), ambient(ColorEdit3), cameraStart(position/target DragFloat3 + fovYRad DragFloat).
  - `MeshInstance`: name, meshAssetPath(InputText), transform.position/scale(DragFloat3) + rotation(DragFloat4 quat + 자동 normalize).
  - `DirLight`: directionWS(DragFloat3 + normalize), color(ColorEdit3), intensity.
  - `PointLight`: positionWS, color, intensity, range.

### 4-2. `Editor/main.cpp` — 활성 Scene 상태 + 메뉴 처리

부팅 직후 `$(OutDir)assets/Scenes/sample.scene.json` 자동 로드 시도. 없으면 `BuildDefaultScene()` (Dragon 1 + dir 1).

메인 루프 안:
```cpp
bool wantNew = false, wantOpen = false, wantSave = false, wantSaveAs = false;
if (ImGui::BeginMainMenuBar()) { ... 메뉴 클릭 시 플래그만 ... }

if (ImGui::Begin("Hierarchy")) {
    if (DrawHierarchy(activeScene, selection)) modified = true;
}
if (ImGui::Begin("Inspector")) {
    if (DrawInspector(activeScene, selection)) modified = true;
}

ImGui::Render();

// modal 직전 FlushGpu (in-flight fence 묶임 방지)
if (wantOpen || wantSaveAs) {
    commandQueue.FlushGpu();
    for (auto& v : frameFenceValues) v = 0;
}
// wantNew/Open/Save/SaveAs 처리...
```

### 4-3. `ShowSceneFileDialog` — IFileDialog 래퍼

`CoCreateInstance(CLSID_FileSaveDialog / CLSID_FileOpenDialog)` → `SetFileTypes` + `SetDefaultExtension("scene.json")` + `SetFolder($(OutDir)assets/Scenes)` + `Show(parent)` + `GetResult` + `IShellItem::GetDisplayName(SIGDN_FILESYSPATH)` + `CoTaskMemFree`. 사용자 취소 시 `Show` 가 HRESULT 실패 — `false` 반환.

### 4-4. `ComScope` RAII

`CoInitializeEx(COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)` 를 wWinMain 시작 시 한 번. `RPC_E_CHANGED_MODE` 가 아닌 실패는 로그 출력.

### 4-5. EDITOR_ROADMAP.md 갱신

§3 표의 M2 상태: ⏳ → ✅. §5 단계별 devlog 링크에 M2 행 추가.

## 5. 마주친 문제와 해결 ⚠ 필수

### 문제 1 — `ComScope` / `ImGuiSrvAllocator` 의 캡슐화 위반 (OOP)
- **문제**: M0/M1 부터 두 RAII 구조체가 `struct` + `public` 멤버 + 이동 2종 delete 누락. CODE_STYLE §5-2 의 *final + 4종 delete* 룰 위반.
- **원인**: ImGui DX12 backend 의 C-API 콜백 (`UserData` 포인터 전달) 패턴에 맞춰 빠르게 PoD-ish 로 작성. 의도된 단순함이었지만 캡슐화 정책 일관 미준수.
- **해결**: 둘 다 `class final` + 4종 delete (복사·이동) + `private` 멤버 (`m_heap` / `m_initialized`) + `explicit` ctor. `ImGuiSrvAllocator` 는 `SrvDescriptorHeap&` reference 멤버로 비소유 의미 명시.
- **교훈**: C-API 콜백을 위한 정적 함수가 `static_cast<T*>(info->UserData)` 로 멤버에 접근할 때, 멤버는 private 여도 같은 클래스의 정적 함수라 OK. *외부 노출* 만 막으면 됨.

### 문제 2 — Modal 다이얼로그 동안 in-flight fence 묶임 (DX12)
- **문제**: `IFileDialog::Show` 가 modal 로 블록되는 동안 `frameFenceValues[frameIndex]` 의 `WaitForFenceValue` 가 그 슬롯의 GPU 작업 완료를 기다림. 백버퍼 자체엔 충돌 없지만 GPU idle 시간이 모달 길이만큼 늘어남.
- **원인**: modal 호출이 frame 명령 기록 *이전* 위치 — fence wait 는 그 후라 modal 끝날 때까지 누적.
- **해결**: 다이얼로그 동반 액션 (Open / SaveAs) 직전에 `commandQueue.FlushGpu()` + `frameFenceValues[*] = 0`. 모달 진입 전 모든 슬롯 wait 분기 skip.
- **교훈**: ImGui+DX12+Win32 modal 다이얼로그의 *fence 상호작용* 은 자체 검토만으로 놓치기 쉬움. 리뷰어가 발견한 가치 있는 항목.

### 문제 3 — `m_bootCmdList` reset 시점에 GPU 가 명령 진행 중일 가능성 (DX12, 사용자 사전 시정)
- **문제**: 25 단계의 `m_bootCmdList.reset()` 가 `LoadSceneAndRuntime` 종료 직후 호출되는데, FBX/텍스처 업로드 명령을 GPU 가 아직 완료하지 않았을 수 있음. ComPtr 소멸 시점에 GPU 가 invalid pointer 참조 가능성.
- **원인**: CommandList 의 GPU lifetime 가정을 명시하지 않음.
- **해결**: `m_queue->FlushGpu()` 를 reset 직전에 호출 — GPU 가 부트 명령 모두 완료. *(이 시정은 사용자가 본 단계 작업 도중 별도로 적용)*.
- **교훈**: GPU 자원을 멤버에서 폐기할 때 항상 *GPU 가 더 이상 참조하지 않음* 을 명시 보장. 25번 devlog 의 교훈 1 (StructuredBuffer 소멸자 주석) 과 같은 맥락.

### 문제 4 — `InputStdString` 256-char 사일런트 truncation (작은 Warning)
- **문제**: ImGui `InputText` 가 char 버퍼 기반. 256바이트 고정. UTF-8 한글 경로면 약 85자 한계.
- **현 상황**: M2 의 sample.scene.json 경로는 짧음 — 실용 문제 없음.
- **이월**: ImGui 의 `ImGuiInputTextFlags_CallbackResize` + 동적 std::string 버퍼 콜백은 M3 이상에서 ImGui-string 헬퍼로 통합.

## 6. 결과 / 검증

- **빌드 (Debug + Release)**: Engine + Client + Editor 모두 0 warning / 0 error.
- **Editor.exe 자동 실행 (3초)**: ImGui 도킹 + Hierarchy/Inspector/Viewport 3 패널 + MainMenuBar — 충돌 없음. GPU debug layer 에러 0.
- **수동 검증 흐름** (사용자 실행 부탁):
  1. Editor.exe 부팅 → Hierarchy 에 `sample.scene.json` 의 메시 1 + dir 2 + point 2 표시 + 우측 메뉴바에 modified 표시 없음.
  2. Hierarchy 의 `+ Add PointLight` 클릭 → 새 PointLight 자동 선택 + Inspector 가 그 노드 위젯 표시 + 우측 `[modified]` 등장.
  3. Inspector 에서 색/위치/intensity DragFloat 변경 → 즉시 modified.
  4. File→Save → 파일 갱신 + modified 사라짐. (또는 Save As → IFileDialog).
  5. Client.exe 실행 → 새 라이트 반영된 렌더 (사용자 시각 확인).
- **회귀**: Client/Engine 코드 변경 0건 — 다른 빌드/실행 영향 없음.

라인 카운트:
| 파일 | 라인 | 책임 |
|---|---|---|
| Editor/main.cpp | 약 350 | wWinMain + 부트 + 메뉴 + 다이얼로그 + ImGui+DX12 루프 |
| Editor/Panels.h | 36 | namespace API |
| Editor/Panels.cpp | 약 270 | DrawHierarchy + DrawInspector + 익명 헬퍼 |

## 7. AI 협업 메모

- 메모리 [feedback-milestone-checklist](C:/Users/이강동_2/.claude/projects/d--Things/memory/feedback-milestone-checklist.md) 룰을 본 단계 todo 의 마지막 3개로 박음 — devlog/reviewer/push. 누락 없음.
- 리뷰어가 발견한 DX12 Warning (modal during render frame) 은 자체 점검만으로는 놓쳤을 항목. 리뷰 루틴의 비용 대비 가치 재확인.
- 사용자가 본 단계 도중 Application.cpp 의 m_bootCmdList reset 직전에 FlushGpu 추가 — 25 단계 리뷰의 의식만 처리 항목을 *실제* GPU 안전 강화로 격상. 병렬 작업의 좋은 사례.

## 8. 다음 단계 — M3

- Asset Browser 패널 — `assets/` 폴더 스캔 + 드래그앤드롭으로 씬에 메시 인스턴스 추가.
- 메시 캐시 정식화 — Engine/scene/AssetCache 형태로 (25 단계에서 한 번 폐기했던 모듈을 *재사용 가능* 모듈로 부활).
- ImGui-string 헬퍼 — `ImGuiInputTextFlags_CallbackResize` + std::string 동적 버퍼.

미뤄둔 항목:
- Undo/Redo, 멀티 셀렉션, 미저장 변경 경고 — M5+.
- Ctrl+S 실제 단축키 디스패치 — M3 이상에서 keybinding 시스템과 함께.
- ImGuizmo (M4) — 3D 핸들.

## 9. PPT 재료로 쓸 만한 포인트

- "Editor 의 Hierarchy/Inspector — Unity/Unreal 의 5분 미만 클릭으로 라이트 추가/색 변경 → 저장 → 게임 런타임이 같은 .json 을 로드. 데이터 파이프라인의 *손에 잡히는* 가시화."
- "ImGui+DX12+Win32 modal — 메뉴 클릭 플래그 패턴 + ImGui::Render 후 다이얼로그 + modal 직전 FlushGpu 로 in-flight fence 묶임 회피."
- "패널 그리기 = 자유함수 namespace — 상태 없는 함수형 패턴이 OOP 디시플린 안에서 클래스 대안으로 정당."
