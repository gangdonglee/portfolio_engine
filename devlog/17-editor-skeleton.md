# 17. M0 — Editor 골격: ImGui DX12 + 도킹 빈 패널 🪟

- **날짜**: 2026-05-18
- **관련 커밋**: (이 단계 커밋)
- **소요 시간**: 약 60분
- **단계**: Phase 4 — 맵 에디터 (M0/5)

---

## 1. 목표

엔진 자산을 시각적으로 배치하고 `.scene.json` 으로 저장 → Client 가 같은 포맷으로 로드하는 맵툴을 단계별로 짓는다. M0 는 그 첫 단계:

- `Editor.exe` 프로젝트를 솔루션에 추가.
- Engine.lib + ImGui (DX12+Win32 backend) 가 부팅되고 도킹 가능한 빈 패널 3개(Hierarchy / Inspector / Viewport) 가 뜬다.
- 씬 데이터/직렬화/뷰포트 렌더는 M1 이후 단계로 미룸.

## 2. 사전 컨텍스트

직전 16번까지의 결과:
- Engine.lib: Device, CommandQueue, CommandList, SwapChain, RTV/DSV/SRV 힙, Camera, FreeCamera, Mesh, ObjLoader, Texture, RootSignature, PipelineState 까지 갖춤.
- Client.exe: 위 인프라로 OBJ 큐브 + 체커보드 텍스처 + 자유 카메라까지 1프레임 in-flight 렌더.
- 도구 프로젝트(Editor / AssetCooker / Headless Runner) 는 [docs/ARCHITECTURE.md](../docs/ARCHITECTURE.md) §7-2 에서 추가 절차가 이미 정의됨 — 그 절차를 그대로 따른다.

사용자 결정(이 단계 시작 직전):
1. UI: **Dear ImGui (docking 브랜치)** vendored. Win32 native 자작 거부.
2. 씬 포맷: **JSON** (nlohmann/json single header vendored).
3. 다중 라이트 셰이더 작업은 **M1 에 묶음**.

## 3. 결정과 트레이드오프

### 17-1. ImGui 를 Editor.exe 안에서 직접 컴파일 (Engine.lib 미오염)

- **결정**: ImGui .cpp 7개(core 5 + Win32/DX12 backend 2) 를 **Editor.vcxproj 의 ClCompile 에 포함**. Engine.vcxproj 에는 포함하지 않음.
- **후보**:
  - A) Engine.lib 에 ImGui 포함 — Client 도 같은 ImGui 를 쓸 수 있음.
  - B) `EngineImGui.lib` 별도 정적 라이브러리 — 도구 전용 분리 명시.
  - C) Editor 가 직접 — 최소 변경.
- **선택 이유**: C. Engine.lib 의 의존성 방향(Engine 은 외부 GUI 라이브러리에 의존하지 않음) 을 보존. Client(런타임) 가 ImGui 를 쓸 일이 생기면 그때 B 로 승격.
- **포기한 것**: 향후 Client 가 게임 내 디버그 UI 로 ImGui 가 필요할 때 같은 빌드를 두 번 컴파일하게 됨 — 그 시점에 B 로 리팩토링.

### 17-2. Window 에 외부 WndProc 훅 콜백 추가

- **결정**: `Window::SetWndProcHook(std::function<LRESULT(...)>)` 추가. HandleMessage 가 시스템 메시지(WM_CLOSE/DESTROY/SIZE/NCCREATE) 이외에 대해 훅을 먼저 호출하고, 훅이 0 이외를 반환하면 Window 의 핸들러를 스킵.
- **후보**:
  - A) Window 가 ImGui 를 직접 알게 한다 — 학습 자료의 패턴이지만 Engine 이 UI 라이브러리에 의존하게 됨. 거부.
  - B) Editor 에서 Window 와 별개로 자체 WndProc 를 SetWindowLongPtrW 로 서브클래싱 — 작동하지만 우회.
  - C) Window 에 일반화된 훅 콜백. Engine 은 ImGui 를 모름. ← 채택.
- **선택 이유**: 의존성 방향 보존(Engine → ImGui 없음) + 훅이 ImGui 이외에 다른 도구에도 재사용 가능.
- **포기한 것**: 다중 훅 체인 없음. 단일 훅만 — 한 번에 하나의 도구 백엔드만 등록 가능. 현재 그 이상 필요 없음.

### 17-3. 시스템 메시지(WM_CLOSE/DESTROY/SIZE/NCCREATE) 는 훅 우회

- **결정**: 위 4개 메시지는 Window 가 직접 처리. 훅이 가로채지 못함.
- **이유**: Window 의 라이프사이클 추적(`m_isOpen`, `m_resizeDirty`) 이 깨지면 SwapChain Resize/메인 루프 종료가 깨짐. ImGui 백엔드는 이 4개를 어차피 안 가로채지만, 미래에 가로채는 변경이 들어와도 Window 가 안전하도록 가드.

### 17-4. ImGui SRV 디스크립터: bump-allocate (free no-op)

- **결정**: ImGui 1.92+ 가 요구하는 `SrvDescriptorAllocFn / FreeFn` 을 Editor 측에서 구현. Engine 의 `SrvDescriptorHeap::Allocate()` 를 그대로 사용. Free 는 no-op.
- **이유**: 툴 수명 동안 ImGui 가 만드는 텍스처 수가 유한(폰트 + 자산 썸네일 수십 개). 64 슬롯 힙으로 M0~M3 충분.
- **포기한 것**: 슬롯 재사용. 자산 브라우저가 본격적으로 자산 수 100+ 를 다루기 시작하면 free-list 로 전환.

## 4. 작업 내용

### 4-1. external/ 폴더 + 외부 의존 vendor

```
external/
├── imgui/                    # docking 브랜치 1.92.9 WIP
│   ├── imgui.{cpp,h}
│   ├── imgui_demo.cpp
│   ├── imgui_draw.cpp
│   ├── imgui_internal.h
│   ├── imgui_tables.cpp
│   ├── imgui_widgets.cpp
│   ├── imconfig.h
│   ├── imstb_*.h
│   ├── backends/
│   │   ├── imgui_impl_win32.{cpp,h}
│   │   └── imgui_impl_dx12.{cpp,h}
│   └── LICENSE.txt           # MIT
└── nlohmann_json/            # v3.11.3
    ├── json.hpp              # single header (~900KB)
    └── LICENSE.MIT
```

submodule 미사용 — 단순 vendored. 버전 업그레이드는 수동 교체.

### 4-2. Engine/platform/Window 에 WndProc 훅

[Engine/platform/Window.h](../Engine/platform/Window.h):

```cpp
using WndProcHook = std::function<LRESULT(HWND, UINT, WPARAM, LPARAM)>;
void SetWndProcHook(WndProcHook hook) { m_wndProcHook = std::move(hook); }

// HWND 노출 — ImGui Win32 backend 가 필요.
HWND NativeHwnd() const noexcept { return m_hwnd; }
```

[Engine/platform/Window.cpp](../Engine/platform/Window.cpp) `HandleMessage`:

```cpp
const bool isSystemMsg =
    (msg == WM_CLOSE) || (msg == WM_DESTROY) || (msg == WM_SIZE) || (msg == WM_NCCREATE);
if (!isSystemMsg && m_wndProcHook)
{
    const LRESULT hookResult = m_wndProcHook(hwnd, msg, wParam, lParam);
    if (hookResult != 0) { return hookResult; }
}
// fall through to switch (...)
```

### 4-3. Editor/Editor.vcxproj — Client.vcxproj 패턴 따라 작성

핵심 차이만:
- `AdditionalIncludeDirectories`: `$(SolutionDir)Engine; $(ImGuiRoot); $(ImGuiRoot)\backends; $(NlohmannJsonRoot)`.
- `AdditionalDependencies`: `d3d12.lib; dxgi.lib; d3dcompiler.lib; dxguid.lib` (FBX 미사용).
- ImGui .cpp 7개 각각에 `<WarningLevel>TurnOffAllWarnings</WarningLevel>` + `<ConformanceMode>false</ConformanceMode>` 박음. 외부 코드의 warning 이 우리 `/W4 + /permissive-` 정책을 흐트러뜨리지 않게.
- PostBuild: shaders + assets 복사 (M1 부터 필요).

### 4-4. Editor/main.cpp — ImGui DX12 부트 + 도킹 빈 패널

핵심 구조:

```cpp
engine::platform::Window           window(1600, 900, L"portfolio_engine Editor");
engine::render::Device             device;
engine::render::CommandQueue       commandQueue(device);
engine::render::RtvDescriptorHeap  rtvHeap(device, SwapChain::kBackBufferCount);
engine::render::SwapChain          swapChain(device, commandQueue, window, rtvHeap);
engine::render::SrvDescriptorHeap  srvHeap(device, 64);  // ImGui font + 동적 텍스처

ImGui::CreateContext();
ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
ImGui_ImplWin32_Init(window.NativeHwnd());

ImGui_ImplDX12_InitInfo dxInit{};
dxInit.Device               = device.Native();
dxInit.CommandQueue         = commandQueue.Native();
dxInit.NumFramesInFlight    = kFrameCount;
dxInit.RTVFormat            = DXGI_FORMAT_R8G8B8A8_UNORM;
dxInit.SrvDescriptorHeap    = srvHeap.Native();
dxInit.SrvDescriptorAllocFn = &ImGuiSrvAllocator::Alloc;
dxInit.SrvDescriptorFreeFn  = &ImGuiSrvAllocator::Free;
dxInit.UserData             = &srvAllocator;
ImGui_ImplDX12_Init(&dxInit);

window.SetWndProcHook([](HWND h, UINT m, WPARAM w, LPARAM l) {
    return ImGui_ImplWin32_WndProcHandler(h, m, w, l);
});
```

매 프레임:
```cpp
ImGui_ImplDX12_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
// MainMenuBar (File: New/Open/Save/Exit) + Begin("Hierarchy") / Begin("Inspector") / Begin("Viewport")
ImGui::Render();

// PRESENT → RT, Clear, SetDescriptorHeaps(srvHeap),
// ImGui_ImplDX12_RenderDrawData(...), RT → PRESENT, Execute, Present.
```

N프레임 in-flight + fence 추적은 Client 의 패턴 그대로 재사용 ([Client/main.cpp](../Client/main.cpp) 와 동일 구조).

### 4-5. portfolio_engine.sln 에 Editor 등록

Project 블록 + ProjectDependencies (Engine.vcxproj) + ProjectConfigurationPlatforms 4행 추가.

## 5. 마주친 문제와 해결 ⚠ 필수

### 문제 1 — `%(ClCompile.Filename)` 조건은 ItemDefinitionGroup 에서 사용 불가

- **문제**: ImGui 7개 .cpp 만 warning 끄려고 `<ClCompile Condition="'%(ClCompile.Filename)'=='imgui' Or ...">` 를 ItemDefinitionGroup 안에 박았더니 MSBuild 가 `error MSB4190: ... '기본 제공 메타데이터 "Filename"을(를) 참조할 수 없습니다'` 로 거부.
- **원인**: well-known item metadata (`Filename`, `Extension`, `FullPath`) 는 ItemDefinitionGroup 의 조건식에서 평가할 수 없음. 평가는 ItemGroup 의 개별 항목 스코프에서만 가능.
- **해결**: ItemDefinitionGroup 의 조건식 제거. 각 `<ClCompile Include="...imgui.cpp">` 항목에 자식 엘리먼트 `<WarningLevel>TurnOffAllWarnings</WarningLevel>` + `<ConformanceMode>false</ConformanceMode>` 직접 부착. 항목당 4줄씩 늘어나지만 동작 명확.
- **교훈**: vcxproj 의 per-file 컴파일 옵션은 ItemGroup 항목의 자식 엘리먼트로 표현. ItemDefinitionGroup 은 "기본값", 항목 자식은 "오버라이드".

### 문제 2 — Window 의 public `NativeHwnd` 와 private `NativeHwnd` 중복 선언

- **문제**: 훅 추가하면서 public `HWND NativeHwnd() const noexcept` 를 노출했는데, 원본 코드의 private `NativeHwnd` (SwapChain friend 용) 를 제거하지 않아 같은 클래스에 동일 시그니처 멤버가 두 번 선언됨.
- **원인**: Edit 1회로 public 추가만 했고, private 쪽을 같이 정리하지 않음.
- **해결**: private NativeHwnd + `friend class engine::render::SwapChain` 전방선언 + namespace 줄까지 제거. 이제 SwapChain 은 public 접근자를 일반 호출처럼 사용.
- **교훈**: 캡슐화 후 풀 때, "왜 friend 였나" 의 이유가 사라졌는지 명시 확인. 본 건은 "도구 빌드 요구사항(ImGui HWND 직접 사용)" 으로 라이프사이클이 친구 모델에서 공개 모델로 전환됐다는 점이 명확.

### 문제 3 — ImGui 1.92+ 의 SrvDescriptorAllocFn 누락 가능성

- **문제**: 헤더 주석 "from 1.92 the backend will need to allocate more" — Legacy 단일 디스크립터 경로를 쓰면 ImGui 가 폰트 외 텍스처를 만들 때 모두 같은 슬롯에 덮어쓰기.
- **원인**: 1.92 에서 동적 텍스처 API 가 들어가면서 폰트 외에도 SRV 슬롯이 더 필요해짐.
- **해결**: `ImGuiSrvAllocator` (UserData 로 전달되는 small struct) 가 Engine 의 `SrvDescriptorHeap::Allocate()` 를 호출하는 `Alloc`/`Free` 정적 함수를 제공. `dxInit.SrvDescriptorAllocFn / FreeFn / UserData` 세팅.
- **교훈**: 외부 라이브러리의 헤더 주석에 명시된 "버전 N+ 변경 사항" 은 vendor 시점 버전(1.92.9 WIP) 이 이미 영향권인지 즉시 확인.

## 6. 결과 / 검증

- **빌드 (Debug)**: Engine + Client + Editor 모두 0 warning / 0 error.
- **빌드 (Release)**: 동일 — 0 warning / 0 error.
- **실행 검증**: GUI 앱이라 자동 검증 어려움. 사용자가 `build/x64/Debug/Editor.exe` 직접 실행 → ImGui DockSpace + MainMenuBar + 빈 패널 3개(Hierarchy/Inspector/Viewport) 가 도킹 가능한 형태로 표시 + 패널을 드래그해서 분할/탭화 가능 + File→Exit 로 정상 종료까지 수동 확인 예정.
- **회귀**: Client.exe 도 같이 Release 빌드 통과 — Window 헤더 변경이 기존 게임 코드를 깨지 않음 확인.

## 7. AI 협업 메모

- M0 단계 시작 전, 사용자가 핵심 결정(UI 라이브러리 / 직렬화 포맷 / M1 범위)을 먼저 골라준 덕에 계획 분량 절제. 작은 페이지 1장으로 시작 → 합의 → 코드.
- ImGui 1.92+ 의 SrvDescriptorAllocFn 변경은 헤더 주석 한 줄에서 발견. 외부 라이브러리 vendor 시 핵심 헤더의 변경 노트는 빠르게 훑는 게 비용 대비 효과 큼.

## 8. 다음 단계 — M1

- `Engine/scene/` 모듈: `Transform`, `MeshInstance`, `DirectionalLight`, `PointLight`, `CameraStart`, `Scene` 데이터 구조.
- `engine::scene::SaveJson(scene, path)` / `LoadJson(path) -> Scene` — nlohmann/json ADL `to_json`/`from_json` 패턴.
- `assets/Scenes/sample.scene.json` (손작성) 을 Client 가 로드해서 그대로 렌더 — 하드코딩 cube/라이트 제거.
- Mesh 캐시 (`unordered_map<path, unique_ptr<Mesh>>`).
- **셰이더 작업 (M1 에 묶음)**: forward.fx 의 단일 directional light 가정을 다중 라이트 (예: dir 4 + point 8) 로 확장. cbuffer 에 `lightCount` 추가, HLSL 측 루프.
- Editor 는 일단 hardcode Scene → JSON 저장만. 패널 내용은 비어둠. M2 에서 채움.

## 9. PPT 재료로 쓸 만한 포인트

- "도구 분리: Engine.lib 은 ImGui 를 모름. Editor.exe 만 알게 함 — 의존성 방향 보존."
- "Window 의 일반화된 WndProc 훅: ImGui 백엔드 통합을 위해 Engine 코드가 ImGui 를 직접 참조하지 않게 하는 단방향 의존 디자인."
- "ImGui 1.92+ 의 SrvDescriptorAllocFn: 폰트 외 동적 텍스처 시대를 위한 알로케이터 콜백 — Engine 의 SrvDescriptorHeap 위에 bump-allocate 어댑터."
