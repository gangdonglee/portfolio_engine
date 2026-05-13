# 03. Phase 1B — `engine::platform::Window` 클래스

- **날짜**: 2026-05-13
- **관련 커밋**: `3bf2bed` (Window 클래스), `51f38d7` (임시 회색 배경)
- **소요 시간**: 약 1시간
- **단계**: Phase 1B — Win32 Window

---

## 1. 목표

Win32 윈도우를 OOP 원칙(SRP/캡슐화/RAII)에 맞춰 클래스로 캡슐화. `main`에서 1280×720 윈도우를 띄우고, OS의 X 버튼 또는 외부 `CloseMainWindow` 신호에 정상 종료(ExitCode 0)되는 상태에 도달.

## 2. 사전 컨텍스트

- 직전 단계: [Foundation Skeleton](02-foundation-skeleton.md) — 빈 `wWinMain` 만 있음.
- 빌드 시스템 + OOP 디시플린 + 서브에이전트 셋업 완료.
- 본 단계가 **첫 실제 클래스** 등장 → `oop-reviewer` 호출 워크플로 첫 적용 예정.

## 3. 결정과 트레이드오프

### 결정 1 — RAII 생성자 (vs `Init()/Shutdown()` 분리)
- **후보**: 생성자에서 일괄 / `Init()` 분리
- **선택**: **생성자 일괄 처리**
- **이유**: Window는 외부 시스템 의존(GPU 디바이스 등)이 없음. 생성자 실패는 예외로 충분히 표현.
- **트레이드오프**: 멤버 디폴트(`m_hwnd = nullptr`)가 생성자 중간 실패 시 안전을 책임진다.

### 결정 2 — 복사·이동 모두 금지 (Rule of 5: all deleted)
- **이유**: HWND는 OS 단일 소유 리소스. 이동을 허용하면 src→dst 이전 후 src를 nullptr 로 비우는 책임이 복잡. 현 단계 단순성 우선.
- 향후 진정 필요해지면(예: 멀티 윈도우 컨테이너) `unique_ptr<Window>` 로 우회.

### 결정 3 — Static 트램폴린 + `GWLP_USERDATA`
- WindowProc 은 C 콜백 → 인스턴스 메서드 디스패치를 위해 `StaticWndProc` 트램폴린.
- `WM_NCCREATE` 시점에 `CreateWindowExW` 의 `lpParam(this)` 을 `GWLP_USERDATA` 로 저장.
- 핫 패스 비용: 간접 호출 1회 (가상 함수 없음).

### 결정 4 — `Handle()` 미노출 (리뷰 반영)
- 초기 작성에서 `HWND Handle() const` public 메서드 포함.
- `oop-reviewer` 지적: 외부 코드가 `DestroyWindow`/`SetWindowLongPtr` 등 임의 호출 가능 → RAII 소유권 무너짐.
- **반영**: `Handle()` 제거. Phase 1C(SwapChain)에서 `friend class` 또는 좁힌 어댑터 패턴으로 필요한 만큼만 재노출.

### 결정 5 — `Width()/Height()` = 클라이언트 영역
- 생성자에서 `AdjustWindowRectEx` 로 클라이언트 크기 ↔ 전체 윈도우 크기 분리.
- 게터의 의미를 헤더 주석에 명시 (외부 혼동 방지).

## 4. 작업 내용

### 4-1. `Window.h` 인터페이스 (요지)
```cpp
namespace engine::platform {
class Window {
public:
    Window(int width, int height, std::wstring_view title);
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&&) = delete;
    Window& operator=(Window&&) = delete;

    void PumpMessages();
    bool IsOpen() const noexcept;
    int  Width()  const noexcept;  // 클라이언트 영역
    int  Height() const noexcept;

private:
    static void EnsureClassRegistered();
    static LRESULT CALLBACK StaticWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(HWND, UINT, WPARAM, LPARAM);
    HWND m_hwnd = nullptr;
    int  m_width = 0, m_height = 0;
    bool m_isOpen = false;
};
}
```

### 4-2. 핵심 흐름
- 생성자: `EnsureClassRegistered()` → `AdjustWindowRectEx` → `CreateWindowExW(this 전달)` → `ShowWindow + UpdateWindow` → `m_isOpen = true`
- 소멸자: `m_hwnd != nullptr && IsWindow(m_hwnd)` 가드 후 `DestroyWindow`
- `PumpMessages`: `PeekMessageW(PM_REMOVE)` 루프, `WM_QUIT` 수신 시 `m_isOpen = false`
- `HandleMessage`: `WM_SIZE`(클라 크기 갱신), `WM_CLOSE`(`DestroyWindow`), `WM_DESTROY`(`m_hwnd=nullptr`, `PostQuitMessage(0)`), 그 외 `DefWindowProcW`

### 4-3. `main.cpp` 갱신
```cpp
try {
    engine::platform::Window window(1280, 720, L"portfolio_engine");
    while (window.IsOpen()) {
        window.PumpMessages();
        // TODO(phase1c): 렌더 / 게임 틱
    }
} catch (const std::exception& e) {
    ::OutputDebugStringA("[portfolio_engine] fatal: ");
    ::OutputDebugStringA(e.what());
    return 1;
}
```

### 4-4. `vcxproj` 갱신
- 새 `<ClCompile>` / `<ClInclude>` 등록
- `<AdditionalOptions>/utf-8</AdditionalOptions>` 추가 — 한국어 Windows MSVC 의 디폴트 CP949 해석 회피

### 4-5. `vcxproj.filters` 갱신
- `Source Files/engine/platform`, `Header Files/engine/platform` 필터 트리 추가 → 솔루션 익스플로러 정리

## 5. 마주친 문제와 해결 ⚠ 필수 — 빠짐없이 기록

### 문제 1 — MSVC 가 UTF-8 한글 주석을 CP949 로 오해석 → brace 미스매치
- **문제**: 첫 빌드 실패. `C4819` 경고 + `main.cpp` 에서 `C2317`(try 블록의 catch 없음), `C2318`(catch에 try 없음), `C1075`(`{` 짝 없음) 연쇄 에러.
- **원인**: 한국어 Windows MSVC 의 디폴트 소스 인코딩은 **CP949**. UTF-8 로 저장한 한글 주석 바이트 시퀀스가 CP949 의 다중 바이트 문자로 잘못 그룹핑되며 그 중 일부가 ASCII 토큰(예: `{`, `\`)으로 오해석돼 코드 구조가 깨짐.
- **해결**: `portfolio_engine.vcxproj` 의 `ClCompile` 두 구성(Debug/Release) 모두에 다음 줄 추가:
  ```xml
  <AdditionalOptions>/utf-8 %(AdditionalOptions)</AdditionalOptions>
  ```
  `/utf-8` = `/source-charset:utf-8 /execution-charset:utf-8`. 소스도 UTF-8, 런타임 문자열도 UTF-8.
- **교훈**:
  - 새 MSVC 프로젝트 셋업 시 `/utf-8` 을 **디폴트**로 포함.
  - BOM 도 대안이나 소스 파일엔 비권장(다른 도구·일부 컴파일러 호환성). `/utf-8` 이 표준 해법.
  - `C4819` 경고는 곧 컴파일 실패 전조. 절대 무시 금지.

### 문제 2 — 익명 네임스페이스 함수에서 클래스 `private` 멤버 접근 (C2248)
- **문제**: `Window.cpp` 익명 네임스페이스의 `EnsureClassRegistered()` 람다에서 `&Window::StaticWndProc` 접근 시 `C2248: 'engine::platform::Window::StaticWndProc': private 멤버 ... 액세스할 수 없습니다`.
- **원인**: 익명 네임스페이스 함수는 클래스 외부 자유 함수. `private` 멤버에 접근 권한 없음.
- **해결**: `EnsureClassRegistered` 를 `Window` 클래스의 **`private static` 멤버 함수**로 이동. 멤버 함수 본문 내 람다는 enclosing 스코프(클래스)의 private 접근권을 그대로 갖는다.
- **교훈**: 클래스 private 멤버를 사용하는 헬퍼는 ① 멤버 함수, ② `friend` 선언, ③ 멤버를 public 화 중에서 선택. 디폴트는 **멤버 함수**.

### 문제 3 — 커스텀 서브에이전트(`oop-reviewer`) 호출 실패
- **문제**: `Agent(subagent_type='oop-reviewer', ...)` 호출 시 `"Agent type 'oop-reviewer' not found. Available agents: claude, claude-code-guide, Explore, general-purpose, Plan, statusline-setup"`.
- **원인**: Claude Code 는 `.claude/agents/*.md` 를 **세션 시작 시에만 로드**. 본 세션 중 추가·푸시한 커스텀 에이전트(`dx12-reviewer`, `oop-reviewer`)는 차후 세션부터 사용 가능.
- **해결**: `general-purpose` 서브에이전트에 `oop-reviewer.md` 의 시스템 프롬프트 핵심 내용을 인라인으로 임베드해 동등한 효과 달성. 리뷰 결과는 의도대로 도출됨.
- **교훈**:
  - 새 커스텀 에이전트 추가 후 **같은 세션 내 호출 불가**. 다음 세션 시작 후 정상 작동 예상.
  - 임시 우회는 `general-purpose` + 인라인 시스템 프롬프트.
  - ORCHESTRATION.md 의 §1.1 표가 “이 세션에서 즉시 호출 가능” 보장은 아님. 차후 세션부터 유효.

### 문제 4 — 캡슐화 위반(`Handle()` 노출) — 리뷰어 지적 반영
- **문제**: 초기 작성 `Window.h` 에 `HWND Handle() const noexcept`. 외부 코드가 OS 핸들로 임의 작업 가능 → RAII 소유권 모델 무너짐.
- **원인**: “다음 단계 SwapChain 이 HWND 필요할 테니 미리 노출” 식의 안일한 사전 노출. 현 단계 사용처는 0.
- **해결**: `Handle()` 제거. Phase 1C 의 `SwapChain` 도입 시 `friend class engine::render::SwapChain` 또는 `NativeHandle` 어댑터 패턴으로 좁혀 재도입.
- **교훈**: “곧 필요할 테니까” 가 캡슐화 위반의 가장 흔한 출입구. **YAGNI** 와 **최소 노출 원칙**을 의식하고, 정말 호출처가 생기는 시점에 가장 좁은 형태로 도입.

### 문제 5 — 흰색 백버퍼 잔상 (사용자 관찰 → 임시 가시화 처리)
- **문제**: 빌드·실행 후 사용자가 "클라이언트 영역이 하얗게만 나오는데 정상인가" 질문. 시각적으로 버그처럼 보임.
- **원인**: `hbrBackground = nullptr` 로 설정 → Windows 가 `WM_ERASEBKGND` 시 배경 페인트 생략. 동시에 우리 렌더러도 아직 없음(Phase 1D 전). 결과적으로 GPU 백버퍼의 미정의 픽셀(주로 하양·잔상)이 그대로 노출. **의도된 무렌더 상태**.
- **해결**: 두 단계 대응. ① 즉시: `hbrBackground = (HBRUSH)::GetStockObject(DKGRAY_BRUSH)` 로 임시 어두운 회색 배경 적용해 시각 혼동 제거(별도 커밋 `51f38d7`). ② 본격: Phase 1D 의 SwapChain Clear 도입 시 다시 `nullptr` 로 복원하고 렌더러가 모든 픽셀을 채우게 한다.
- **교훈**: “의도된 무렌더 상태”는 사용자·리뷰어에게 버그처럼 보일 수 있음. 코드 코멘트만으론 부족. **가시화 단계의 일관성**도 디시플린의 일부. 단계 사이에 임시 placeholder(회색 배경, 디버그 텍스트 등)를 두는 게 무난.

## 6. 결과 / 검증

### 빌드
| 구성 | 결과 | 비고 |
|---|---|---|
| Debug\|x64 | ✅ | 경고 0, 에러 0 (재빌드 후) |
| Release\|x64 | ✅ | 경고 0, 에러 0 |

### 실행
| 검증 항목 | 결과 |
|---|---|
| Start-Process → 2초간 살아있음 | ✅ |
| `CloseMainWindow()` 호출 → 응답 | ✅ |
| 정상 종료 ExitCode | **0** |

스크린샷 자리표시자:
- (TODO) 1280×720 윈도우 캡처
- (TODO) 솔루션 익스플로러 새 트리 (`engine/platform/`)
- (TODO) MSBuild 출력 (`Window.cpp` 컴파일 + 0 경고)

## 7. AI 협업 메모

본 단계가 **`oop-reviewer` 첫 적용 시도**.

- 호출 결과: `general-purpose` 우회로 동등 리뷰 받음.
- 지적된 항목:
  - 위반 2건: `Handle()` 노출, 파괴 경로 이중성
  - 의심 2건: `HandleMessage` 의 `hwnd` 파라미터(유지가 안전 — `DefWindowProc` 호출에 필요), `WM_SIZE` 의미 문서화
  - 잘된 점: 복사/이동 4종 delete, 가상 함수 0개
- 위반 2건 모두 반영. 의심은 의도 명확화(코멘트 추가)로 처리.
- **워크플로 검증**: 패턴 B(독립 리뷰) 가 실제로 코드 품질 향상에 기여함을 확인. 다음 단계부터도 클래스 추가/리팩토링마다 패턴 B 적용.

## 8. 다음 단계

- **Phase 1C — `engine::render::Device` 초기화**
  - DXGI Factory + D3D12 Device 생성
  - 디버그 레이어 + Info Queue 활성화 (Debug 빌드)
  - 어댑터 열거 → 최적 GPU 선택
  - 일단 windowless / standalone 으로 만들고, 다음 단계에서 SwapChain 으로 Window 와 결합

미뤄둔 항목:
- 로깅 시스템 — Device 초기화 직후 (OutputDebugString 만으론 불충분)
- `oop-reviewer` / `dx12-reviewer` 정식 호출 — 다음 세션부터

## 9. PPT 재료로 쓸 만한 포인트

- **"MSVC + 한글 인코딩"** 슬라이드 (CP949 해석 → `/utf-8` 해결, BOM vs `/utf-8` 비교)
- **"Win32 윈도우 OOP 캡슐화"** 슬라이드 (RAII + Static thunk + 4종 delete 다이어그램)
- **"AI 리뷰 반영 사례 1"** 슬라이드 (Handle() 노출 → 제거 → friend 예고)
- **"커스텀 에이전트 운영의 함정"** 슬라이드 (세션 캐시 + 우회 패턴) — AI 협업 워크플로 학습 사례
