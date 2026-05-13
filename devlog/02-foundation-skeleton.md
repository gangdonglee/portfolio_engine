# 02. Foundation Skeleton — 빌드 시스템 부트스트랩

- **날짜**: 2026-05-13
- **관련 커밋**: `2e5c21d` (orchestration 루틴 갱신), `98c54d9` (빌드 스켈레톤)
- **소요 시간**: 약 30분
- **단계**: Phase 1A — Foundation Skeleton

---

## 1. 목표

프로젝트 디렉토리 구조 + VS2022 .sln/.vcxproj 셋업 + 빈 `wWinMain` stub 가 **Debug/Release 양쪽 x64 빌드** 에서 통과하고, 실행 시 ExitCode 0 으로 깨끗이 종료되는 상태에 도달.

## 2. 사전 컨텍스트

- 직전 단계: 프로젝트 셋업 ([devlog 01](01-project-setup.md)) — 빈 디렉토리 + 문서·orchestration·custom 서브에이전트·권한 설정만 있음.
- 직전 결정 사항: 옵션 A 풀스크래치, 순수 .sln/.vcxproj, OOP 준수, x64 only, VS2022 단일 IDE.
- 시작 직전 추가: ORCHESTRATION.md §6 에 **작업 후 커밋 루틴** 명시 (작은 단위로 자주 커밋, Conventional Commits 한국어). 본 단계부터 적용.

## 3. 결정과 트레이드오프

### 결정 1 — 단일 .vcxproj (현 단계)
- **후보**: 단일 .vcxproj / 다중 .vcxproj (engine.lib + app.exe)
- **선택**: **단일 .vcxproj**, 단 폴더 구조는 분리 친화적으로 (src/ 아래에 향후 engine/, app/ 서브폴더 둘 예정).
- **선택 이유**: 첫 빌드는 단순성 우선. 분리는 vcxproj 참조 설정 + 의존성 관리 추가. 엔진 규모가 작을 때 분리는 오히려 마찰.
- **포기한 것**: 엔진 코드와 게임 코드의 강제적 인터페이스 분리. 의식적 디시플린으로 보강(oop-reviewer 자동 검수).

### 결정 2 — 빌드 출력 경로
- **선택**: `$(SolutionDir)build\$(Platform)\$(Configuration)\` (예: `build/x64/Debug/`)
- **선택 이유**: 루트의 단일 `build/` 폴더로 통일 → .gitignore 한 줄로 처리. 중간 산출물은 `build/intermediate/...`.

### 결정 3 — 엄격한 컴파일 옵션
- C++ 표준: **stdcpp20**
- WarningLevel: **Level4**
- ConformanceMode: **true** (`/permissive-`)
- SDLCheck: **true**
- 캐릭터 셋: **Unicode**
- MultiProcessorCompilation: **true**
- **선택 이유**: 처음부터 엄격하게. 나중에 완화는 쉽지만 강화는 어렵다. 풀스크래치 엔진이라 표준 준수와 워닝 청결 유지가 코드 품질 지표 그 자체.
- **포기한 것**: TreatWarningAsError 는 아직 false (초기 보일러플레이트 단계). 추후 코어 안정화 후 true 전환.

### 결정 4 — 사전 라이브러리 링크
- `d3d12.lib`, `dxgi.lib`, `d3dcompiler.lib`, `dxguid.lib` 를 시작부터 링크.
- **이유**: 다음 단계부터 DX12 코드 들어가는데 매번 추가하는 번거로움 회피. 미사용 심볼이라 링커가 dead-strip.

### 결정 5 — Subsystem: Windows (Console 아님)
- **이유**: 다음 단계에 윈도우 띄움. Console 창은 불필요. 디버그 출력은 `OutputDebugStringW` + VS 출력 창.
- **트레이드오프**: 콘솔 부재 시 `printf` 디버깅 불가. 초기엔 약간 불편하지만 후일 사용자 빌드에 콘솔이 안 뜨는 게 더 중요.

## 4. 작업 내용

### 4-1. 디렉토리 구조
```
portfolio_engine/
├── .claude/                 # AI 협업 설정 (agents/, settings.local.json)
├── .git/
├── devlog/                  # 단계별 기록
├── docs/                    # (예정) ARCHITECTURE.md
├── shaders/                 # (예정) HLSL
├── assets/                  # (예정) 메시·텍스처·애니메이션
├── src/
│   └── main.cpp             # 진입점
├── portfolio_engine.sln
├── portfolio_engine.vcxproj
├── portfolio_engine.vcxproj.filters
├── README.md
├── ORCHESTRATION.md
└── .gitignore
```

빈 디렉토리(shaders/, assets/, docs/)는 첫 파일이 들어올 때 git에 자연 추적. 현 단계엔 git에 안 보이는 게 정상.

### 4-2. `src/main.cpp` (stub)
```cpp
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                      _In_opt_ HINSTANCE hPrevInstance,
                      _In_ LPWSTR lpCmdLine,
                      _In_ int nCmdShow)
{
    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

    OutputDebugStringW(L"[portfolio_engine] boot stub running\n");
    return 0;
}
```

### 4-3. `portfolio_engine.vcxproj` 핵심
- ProjectGuid: `{B1B6F8A3-1C2D-4E5F-9A1B-2C3D4E5F6A7B}`
- VCProjectVersion: 17.0 (VS2022)
- PlatformToolset: v143
- WindowsTargetPlatformVersion: 10.0 (SDK 10.0.26100.0 자동 사용)
- 구성: Debug|x64, Release|x64 (x86 미지원)
- Debug/Release 각각 ItemDefinitionGroup 으로 옵션 분리
- AdditionalIncludeDirectories: `$(SolutionDir)src` (인클루드 경로 루트)

### 4-4. `portfolio_engine.sln`
- 단일 프로젝트 참조
- 솔루션 구성 ↔ 프로젝트 구성 매핑 (Build.0 활성)
- SolutionGuid 부여

## 5. 마주친 문제와 해결 ⚠ 필수 — 빠짐없이 기록

### 문제 1 — `New-Item -Force | Out-Null` 응답 없음으로 인한 일순 혼란
- **문제**: 디렉토리 생성용 PowerShell 명령 직후 도구 응답이 "(PowerShell completed with no output)" 으로 표시되어 잠시 실패 의심.
- **원인**: New-Item 의 출력을 의도적으로 `| Out-Null` 로 버렸기 때문에 정상. -Force 는 이미 존재해도 조용히 통과.
- **해결**: 별도 `Get-ChildItem ... | Select-Object Name` 호출로 디렉토리 생성 결과 검증.
- **교훈**: 부수 효과만 있는 명령(mkdir, 파일 삭제 등) 직후엔 항상 즉시 다음 명령으로 결과 검증. "응답 없음 ≠ 실패". 도구 호출 시 의도적으로 verbose 정도를 다음 명령에 부여하는 게 안전.

### (이번 세션 추가 실수 없음)
빌드는 한 번에 Debug/Release 모두 통과. .vcxproj/.sln 작성 보일러플레이트 영역이라 평소보다 사고 마진 큼. 다음 단계(Window 클래스부터) 사고 빈도 증가 예상.

## 6. 결과 / 검증

| 구성 | 출력 | 크기 | 결과 |
|---|---|---|---|
| Debug\|x64 | `build/x64/Debug/portfolio_engine.exe` | 60.5 KB | ✅ |
| Release\|x64 | `build/x64/Release/portfolio_engine.exe` | 11.0 KB | ✅ |

- MSBuild 17.14.23, Warning Level 4 + /permissive-, 경고/에러 **0**
- Debug exe 실행 → ExitCode **0**
- `OutputDebugStringW` 메시지는 디버거 부착 시 VS 출력 창에서 확인 가능

스크린샷 자리표시자:
- (TODO) VS2022 솔루션 익스플로러 트리
- (TODO) 빌드 출력 창 (경고 0, 에러 0)
- (TODO) 디렉토리 트리 (`tree /F`)

## 7. AI 협업 메모

- 이 단계는 표준 보일러플레이트라 서브에이전트 fan-out 없이 메인 컨텍스트에서 직접 작성.
- 다음 단계(Window 클래스)부터 `oop-reviewer` 호출 시작 예정 — 첫 실제 클래스 등장.
- 본 단계에서 ORCHESTRATION.md §6 갱신(작업 후 커밋 루틴) 도 같이 진행. 본 devlog 자체가 새 루틴의 첫 적용 사례.

## 8. 다음 단계

- **Phase 1B — Win32 윈도우 클래스 (`engine::Window`)**
  - 윈도우 등록·생성·메시지 펌프 캡슐화
  - WM_DESTROY → PostQuitMessage
  - WM_SIZE / WM_CLOSE 처리
  - 리사이즈 콜백 인터페이스 (다음 단계 SwapChain 과 연동)
  - `main.cpp` 에서 `Window` 인스턴스 생성 후 메인 루프
- 본 단계 종료 시 `oop-reviewer` 호출 워크플로 첫 적용

미뤄둔 항목:
- 셰이더 빌드 자동화 (fxc/dxc, 빌드 이벤트) — 첫 셰이더 추가 시 결정
- 로깅 시스템 — Window 또는 Device 초기화 직후

## 9. PPT 재료로 쓸 만한 포인트

- **"빌드 시스템 — 왜 순수 .sln/.vcxproj?"** 슬라이드 (CMake/Premake/Pure VS 비교)
- **"엄격한 컴파일 옵션"** 슬라이드 (Level4 + /permissive- + stdcpp20 — 처음부터 깐깐하게)
- **"디렉토리 구조"** 슬라이드 (트리 다이어그램, 의도된 분리)
- **"작업 후 커밋 루틴"** 슬라이드 (커밋 그래프 + Conventional Commits 한국어 정착)
