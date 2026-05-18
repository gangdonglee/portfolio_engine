# 25. Client/main.cpp SRP 분할 — Application / SceneRuntime / FrameRenderer / InputController 🧱

- **날짜**: 2026-05-18
- **관련 커밋**: (이 단계 커밋)
- **소요 시간**: 약 2시간
- **단계**: Phase 4 — 맵 에디터 보조 (M1 리뷰 이월 + 코드 위생)

---

## 1. 목표

M1 리뷰의 OOP 이월 항목 *"wWinMain 거대화 (SRP)"* 해결. Client/main.cpp 의 552줄 단일 함수를 4 클래스로 분할 — 같은 동작, 각 책임 분리. M2 (Editor 편집 패널) 진입 전 위생 단계.

## 2. 사전 컨텍스트

- 22 단계까지 Client/main.cpp = 552줄. 부트, 자원 라이프타임, 매 프레임 입력/카메라/씬/렌더가 한 함수.
- 본 단계 시작 직전 사용자가 같은 분할을 Client 측에 시도했다 *vcxproj 등록·컴파일 전에* 폐기. 그 잔재가 워킹트리에 있었으나 명시 이유 없이 폐기.
- 사용자 결정 후 "main 분할한거 다시 진행시켜" 지시 — 그 시점부터 본 작업 재개.

## 3. 결정과 트레이드오프

### 25-1. 분할 위치 — Client 측 (engine 측 X)

- **결정**: 4 클래스 모두 `client::*` 네임스페이스, Client.exe 안.
- **후보**:
  - A) **Engine 측 모듈**: `engine::scene::AssetCache` + `engine::render::SceneRenderer` 등 — Editor 가 M4 뷰포트에서 재사용 가능. 작업 도중 이 방향으로 잠시 코드 추출했음.
  - B) **Client 측 클래스**: 한 게임 클라이언트의 부트/루프 분할. 사용자가 원안에서 시도한 방향.
- **선택 이유**: B. 사용자가 "다시 진행시켜" 지시한 게 자기 원안. Engine 재사용 vs Client 응집 트레이드오프에서, 본 단계는 **Client 응집** 우선. Editor M4 시점에 같은 책임의 Engine 모듈이 필요해지면 그때 *별도로* 추출 (지금 만들면 사용처 1 + dead-code 위험).
- **포기한 것**: Engine.lib 재사용 즉시 가치. M4 진입 시점 결정.

### 25-2. 클래스 4개 분할 — Application / SceneRuntime / FrameRenderer / InputController

- **결정**: 책임 4분할:
  - **Application** — 자원 라이프타임 소유, 부트 4단계 (InitGraphicsCore/InitGraphicsPipeline/LoadSceneAndRuntime/InitRendererAndInput), Run() 메인 루프, Tick(dt).
  - **SceneRuntime** — Scene + 자산 캐시 + Animator + 인스턴스 CB + 라이트 SB. `Tick`/`SetActiveClip`/`PrepareGpuResources`/`RecordDraw`.
  - **FrameRenderer** — kFrameCount CommandList + fence 관리. `Render()` 한 메서드 + `OnResize()`.
  - **InputController** — 키 0..4 다운 엣지 → 클립 변경 요청. `Tick(input)` + `ConsumeClipChange()` 1회 소비.
- **이유**: 변화 이유가 각각 다름 — 부트 자원/Scene 데이터/프레임 명령/입력 의미. SRP 정확 4분할.
- **포기한 것**: 더 잘게 (LightUploader / InstanceConstants / SceneAnimator) — 클래스 수 vs 응집 균형. 4개가 적당.

### 25-3. kFrameCount 1소스 통일 — SwapChain::kBackBufferCount

- **결정**: SceneRuntime + FrameRenderer 둘 다 `static constexpr engine::uint32 kFrameCount = engine::render::SwapChain::kBackBufferCount;`.
- **이유**: 리뷰의 **DX12 Critical** — 두 클래스가 같은 진실을 따로 보유하면 OOB 위험. SwapChain 의 백버퍼 수와 정확히 같아야 frameIndex 정합. 1 헤더 include 추가가 작은 비용.
- **포기한 것**: 런타임 frameCount 가변. `std::array<T, kFrameCount>` 가 컴파일 타임 상수 요구라 인자 주입 안 됨 — 1소스 통일이 더 안전.

### 25-4. FrameRenderer::InitInfo nullptr 검증을 멤버 초기화 *전* 으로

- **결정**: cpp 익명 namespace 의 `Require<T>(T*, name)` 헬퍼로 멤버 초기화 식 안에서 검증.
- **이유**: 리뷰의 **OOP 위반** — ctor 본문에서 nullptr 검증 시 *멤버 reference 초기화가 이미 역참조*하므로 UB. 멤버 초기화 식 자체에서 검증해야.
- **포기한 것**: InitInfo 의 raw 포인터 패턴 자체 (reference ctor 인자 8개로 대체 가능). 호출부 변경 최소화 위해 헬퍼만 추가.

## 4. 작업 내용

### 4-1. Client 측 4 클래스 + 관계

```
        +--------------------+   1
        | client::Application|----o--- engine::platform::Window, ::render::Device/Queue/...
        +---------+----------+
                  |  1
                  | owns
                  v
+-----------------+-------------------------------+
|     client::SceneRuntime         client::FrameRenderer        client::InputController
|     - engine::scene::Scene       - kFrameCount CommandList   - 다운 엣지 + Consume*()
|     - assetCache (unordered_map) - fence value 슬롯
|     - Animator + skeleton/clips  - Render(scene, camera, ...) 한 메서드
|     - 인스턴스 CB / 라이트 SB
|     - PrepareGpuResources / RecordDraw
+-------------------------------------------------+
```

각 클래스는 `final` + 4종 delete + ctor RAII + 멤버 `m_camelCase` — CODE_STYLE 준수.

### 4-2. Client/main.cpp 감축 — 552줄 → 28줄

```cpp
int APIENTRY wWinMain(...)
{
    engine::core::LogInfo(L"[portfolio_engine] boot running\n");
    try
    {
        client::Application app(1280, 720, L"portfolio_engine");
        app.Run();
    }
    catch (const std::exception& e) { /* ... */ return 1; }
    return 0;
}
```

### 4-3. 의존성 방향

`client::*` → `engine::*` 일방. 역방향 0건 — 리뷰의 **OOP Good** 으로 평가.

### 4-4. 헤더 위생 — 전방선언 활용

`Application.h` / `SceneRuntime.h` / `FrameRenderer.h` 모두 `engine::render::*` 의 클래스를 *전방선언* 으로 노출. `unique_ptr<T>` 멤버는 `T` 가 incomplete 여도 OK — 단 dtor 가 .cpp 에. 그래서 모든 클래스가 ~~`= default`~~ 명시 dtor 를 .cpp 에 둠.

예외: `Application.h` 가 `InputController` 를 *value 멤버* 로 보유 → `InputController.h` 인클루드 필요. 인라인 헤더라 부담 미미.

## 5. 마주친 문제와 해결 ⚠ 필수

### 문제 1 — `kFrameCount` 두 곳에 분산 (DX12 Critical)
- **문제**: 초안에서 `SceneRuntime::kFrameCount = 2` (static constexpr) 와 `FrameRenderer::InitInfo::frameCount = 2` (runtime) 분리. SwapChain 백버퍼 수가 3 으로 바뀌면 FrameRenderer 는 frameIndex 0/1/2 인데 SceneRuntime 은 슬롯 2개라 OOB.
- **원인**: 클래스 분할 시 "각자 자기 진실 보유" 패턴 무의식 채택. SRP 추구하면서도 *같은 진실* 은 공유해야 한다는 점 누락.
- **해결**: 두 클래스 모두 `static constexpr kFrameCount = engine::render::SwapChain::kBackBufferCount;` — SwapChain.h 인클루드 1줄 추가. FrameRenderer::InitInfo 의 frameCount 필드 제거. `std::array<T, kFrameCount>` 로 컴파일 타임 정합 보장.
- **교훈**: 분할이 SRP 라고 *같은 사실의 중복 표현* 까지 분할하지 말 것. "변화 이유 1개" 가 SRP. 백버퍼 수가 바뀌면 두 클래스 모두 변해야 함 = 변화 이유 1개 = 1소스.

### 문제 2 — FrameRenderer ctor 의 nullptr 역참조 UB (OOP 위반)
- **문제**: 초안의 `: m_device(*info.device)` 가 ctor 본문의 nullptr 검사 *이전* 실행. info.device == nullptr 면 역참조 UB.
- **원인**: 멤버 reference 초기화 순서 = 멤버 선언 순서. ctor 본문은 모든 초기화 *후*. nullptr 검증이 본문에 있으면 이미 늦음.
- **해결**: cpp 익명 namespace 의 `template<T> T& Require(T*, name)` 헬퍼. 멤버 초기화 식에서 호출:
  ```cpp
  : m_device(Require(info.device, "device"))
  ```
  Require 가 nullptr 시 `runtime_error` throw — 멤버 초기화가 진행되지 않고 ctor exit.
- **교훈**: ctor 멤버 초기화 식의 *사이드 이펙트* (예외 throw) 는 가능. 검증 헬퍼는 멤버 초기화 *식 안* 에 둘 것.

### 문제 3 — `m_bootCmdList` 가 부트 후 멤버로 끝까지 살아있음 (OOP 의심)
- **문제**: Application 멤버 `m_bootCmdList` 가 부트 시 fallback texture 업로드 + Scene 자산 로드에 사용된 후, 메인 루프 동안 미사용 상태로 메모리만 차지.
- **원인**: ctor 안에서만 쓰이는 객체를 무심코 멤버화. lifetime 이 과도 확장됨.
- **해결**: `LoadSceneAndRuntime` 끝에 `m_bootCmdList.reset();` 추가. 부트 전용임을 명시.
- **교훈**: 부트 전용 자원은 함수 로컬 스코프가 정석. 멤버화는 *메인 루프에서도 필요* 한 경우만.

### 문제 4 — Engine 측 분할 vs Client 측 분할 선택 오판
- **문제**: 작업 도중 Engine 측 (`engine::scene::AssetCache` + `engine::render::SceneRenderer` + `engine::scene::SceneValidation`) 으로 코드를 먼저 추출. 사용자가 같은 분할을 Client 측에 시도했다 폐기한 상태였음.
- **원인**: 사용자의 폐기 흔적 (.h/.cpp 4쌍 + Client.vcxproj 수정) 을 *늦게* 발견. 사용자 의도 확인 전에 작업 진행.
- **해결**: 사용자 지시 ("main 분할한거 다시 진행시켜") 후 Engine 측 신규 파일 폐기 (워킹트리만이라 손실 없음), Client 측으로 재진행.
- **교훈**: 워킹트리에 사용자의 *시도 흔적* 이 있으면 git 으로 추적 가능한지 먼저 확인 (`git fsck --lost-found`, dangling blob, build/intermediate 의 .obj 잔재). 의도 파악 후 작업 시작.

## 6. 결과 / 검증

- **빌드 (Debug + Release)**: Engine + Client + Editor 모두 0 warning / 0 error.
- **자동 실행 (Client.exe 3초)**: `[scene] loaded`, `RootSignature params=5`, GPU debug layer 에러 0.
- **리뷰어 호출 후 시정 4건 반영**:
  - kFrameCount 1소스 통일.
  - FrameRenderer ctor Require 헬퍼.
  - m_bootCmdList LoadSceneAndRuntime 끝 reset.
  - OnResize 주석 명확화.

라인 카운트:
| 파일 | 라인 | 책임 |
|---|---|---|
| Client/main.cpp | 28 | 부트 + 예외 catch |
| Client/Application.h+cpp | 약 230 | 자원 라이프타임 + Run + Tick |
| Client/SceneRuntime.h+cpp | 약 320 | Scene + asset + Animator + CB/SB |
| Client/FrameRenderer.h+cpp | 약 165 | 매 프레임 명령 기록 + Present |
| Client/InputController.h+cpp | 약 80 | 다운 엣지 + Consume |

원래 552줄 단일 함수 → 5 파일 분산. 한 파일이 한 책임만.

## 7. AI 협업 메모

- 메모리 [feedback-milestone-checklist](../../../../Users/이강동_2/.claude/projects/d--Things/memory/feedback-milestone-checklist.md) 의 *"마일스톤 완료 todo 의 마지막 3 항목은 devlog/reviewer/push"* 룰을 본 단계에서 즉시 적용. 작업 초반 TodoWrite 에 12 항목 명시, 마지막 3개가 devlog/reviewer/push.
- 리뷰어 호출이 Critical 3건 + 위반 3건 발견 — 자가 검토만으로는 놓쳤을 항목들 (UB 역참조, kFrameCount 분산). 리뷰 루틴의 가치 재확인.
- 사용자의 Client 측 분할 시도 폐기 흔적을 늦게 발견한 점은 *워킹트리 사전 점검 부족* — git fsck 또는 reflog 를 작업 시작 시점에 확인하는 게 안전.

## 8. 다음 단계

- **M2 본 작업** — Editor 의 Hierarchy / Inspector 패널 본격 편집 + 라이트 +/- 버튼 + File New/Open 다이얼로그.
- M2 의 todo 시작 시 *(a) devlog (b) reviewer (c) push* 3 항목 명시 박기 (메모리 룰).

미뤄둔 항목:
- `Scene::ValidateForRuntime` 자유함수 — 본 단계에서 폐기했으나 SceneRuntime ctor 의 capacity 검증이 그 책임을 흡수. 별도 자유함수는 *맵 에디터가 Save 직전에 호출* 하는 시점에 다시 부활 검토.
- ConstantBuffer ring buffer (256B aligned offset) — M3 이상.

## 9. PPT 재료로 쓸 만한 포인트

- "wWinMain 552줄 → 28줄 + 4 클래스 분할 — SRP 정확 적용, 의존성 방향 `client::*` → `engine::*` 일방 보존."
- "kFrameCount 1소스 통일 — SRP 가 '같은 사실의 중복 표현' 까지 분할하라는 게 아님. SwapChain.h 인클루드 1줄로 OOB 차단."
- "ctor 멤버 초기화 *식 안* 의 검증 헬퍼 — nullptr 역참조 UB 차단의 가장 가벼운 패턴."
