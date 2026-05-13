# 06. 코드 스타일 학습 + 프로젝트 구조 재편 (Engine.lib + Client.exe)

- **날짜**: 2026-05-13
- **관련 커밋**: `60c5b5b`, `d5ac6a2`, `a273542`, `070e9ad`, `888d226`, `d40a92b`
- **소요 시간**: 약 1시간 30분
- **단계**: 인프라 — 코드 스타일 영구화 + 프로젝트 분리

---

## 1. 목표

세 가지 인프라 작업을 묶음:
1. 외부 학습 자료(`D:\Things\Animation_소스코드`, 한국 게임 강의 스타일 DX12 풀스크래치 엔진) 의 코드 패턴 추출 + 본 프로젝트에 영구 보존 (`docs/CODE_STYLE.md`). 학습 자료 폴더는 학습 후 사용자가 삭제 예정.
2. 멤버 변수 prefix 결정 — 최종적으로 `m_camelCase` (Microsoft/Unreal 관행) 채택.
3. 단일 vcxproj → **Engine.lib (StaticLibrary) + Client.exe (Application)** 분리. 도구 재사용성 확장 대비.

## 2. 사전 컨텍스트

- 직전 단계: [Phase 1D-1 CommandQueue](05-command-queue.md). 코드 베이스에 6개 .cpp/.h 파일 + 단일 vcxproj.
- 본 작업 후 향후 코드는 새 구조·새 스타일로 작성.

## 3. 결정과 트레이드오프

### 결정 1 — 학습 자료에서 채택할 항목
- 멤버 prefix(처음엔 `_`, 이후 `m_` 으로 번복 — §5 문제 1 참조)
- 타입 alias `int8/uint32/...` (`std::*_t` 베이스)
- PascalCase 클래스/메서드, enum class UPPER_SNAKE + END 마커
- 클래스명 = 파일명
- DX12 패턴 (Allocator+List 페어, RenderBegin/End, CD3DX12_RESOURCE_BARRIER, SWAP_CHAIN_BUFFER_COUNT)

### 결정 2 — 학습 자료에서 거부할 항목
- PCH 거대화 (`using namespace std/WRL/DirectX` 전역)
- 매크로 단축 (`DEVICE`, `GRAPHICS_CMD_LIST`, `INPUT`)
- 전역 `extern unique_ptr<Engine> GEngine`
- 싱글톤 매크로 (`DECLARE_SINGLE`, `GET_SINGLE`)
- HRESULT 무시
- 네임스페이스 없음 (글로벌)
- `Init()` 분리 (우리는 RAII 생성자)

### 결정 3 — 멤버 prefix 최종 `m_camelCase`
- 초안에서 학습 자료 그대로 `_camelCase` 채택 → 1차 리팩토링 적용.
- 이후 사용자 재검토 → Microsoft/Unreal 광역 관행인 `m_camelCase` 가 외부 코드 친화성·일관성 면에서 유리하다고 결정 → 2차 리팩토링 (되돌림).

### 결정 4 — Engine 라이브러리 + Client 실행파일 분리
- 학습 자료의 Engine/Client 구조 채택.
- Engine.vcxproj = StaticLibrary. Client.vcxproj = Application + ProjectReference Engine.
- 향후 Editor/AssetCooker 같은 추가 도구는 Client.vcxproj 패턴 따라 별도 Application 으로.
- 의존성 방향: **Client → Engine 일방**.
- 새 include 루트: `$(SolutionDir)Engine`. 헤더 인클루드는 `platform/Window.h` 같이 단순.

## 4. 작업 내용

### 4-1. CODE_STYLE.md 작성 (커밋 `60c5b5b`)
- 9섹션 (출처, 결정 표, 명명 규칙, 헤더 위생, 클래스 패턴, DX12 코딩 패턴, 학습 자료 거부 항목, 적용 정책, 빠른 체크리스트).
- 학습 자료 폴더 삭제 후에도 본 문서가 영구 참조.
- 메모리에 `feedback-code-style` 저장.

### 4-2. 1차 _ → m_? 아니, 사실 m_ → _ 리팩토링 (커밋 `d5ac6a2`)
- 학습 자료 스타일 `_camelCase` 채택 결정에 따라.
- PowerShell `m_` → `_` 전체 치환 (6 파일).

### 4-3. m_ 복귀 결정 (커밋 `a273542`)
- 사용자 재검토 후 `m_camelCase` 결정.
- CODE_STYLE.md §2 결정 표 + §3-1 + §5-3 + §8-2 + §9 갱신.
- 메모리 갱신.

### 4-4. 2차 _ → m_ 리팩토링 (커밋 `070e9ad`)
- PowerShell + 정규식 word boundary (`\b_xxx\b → m_xxx`) 정확 매칭.
- 6 파일, 12 멤버 변수.

### 4-5. Engine.lib + Client.exe 분리 (커밋 `888d226`)
- 폴더 이동 (`git mv`): `src/engine/*` → `Engine/`, `src/main.cpp` → `Client/main.cpp`.
- 새 vcxproj 4개 작성 (Engine.vcxproj + .filters, Client.vcxproj + .filters).
- portfolio_engine.sln 재작성 (2 프로젝트 + 의존성).
- 기존 portfolio_engine.vcxproj/.filters 삭제.
- include 경로 수정 (`engine/...` → `platform/...`, `render/...` 직접).
- Engine 인클루드 루트는 `$(SolutionDir)Engine`.

### 4-6. ARCHITECTURE.md + 문서 갱신 (커밋 `d40a92b`)
- `docs/ARCHITECTURE.md` 신규 (10섹션).
- CODE_STYLE.md §8-4 추가 (프로젝트 구조 규칙).
- README.md 프로젝트 구조 + 빌드 섹션 갱신.

## 5. 마주친 문제와 해결 ⚠ 필수 — 빠짐없이 기록

### 문제 1 — 결정 번복 (`_` → `m_`) — 두 번 작업한 비용
- **문제**: 학습 자료의 `_camelCase` 를 먼저 채택해 일괄 리팩토링 → 사용자가 Microsoft/Unreal 관행 `m_camelCase` 선호로 재결정 → 다시 일괄 리팩토링.
- **원인**: 큰 명명 결정은 외부 코드 친화성·일관성 같은 광역 기준이 따로 있는데, 학습 자료만 기준으로 결정해버림.
- **해결**: CODE_STYLE.md 결정 표에 학습 자료 vs 우리 결정 컬럼 명시. 멤버 prefix 행을 "학습 자료 `_` / 우리 `m_`" 로 갱신.
- **교훈**: **큰 명명 결정 (멤버 prefix, 폴더 명명 등)** 은 학습 자료뿐 아니라 외부 광역 관행도 같이 검토. "학습 자료 그대로" 가 디폴트가 되면 번복 비용. 사용자 컨펌을 한 번 더 받는 게 안전.

### 문제 2 — `git mv` 후 Edit 도구가 새 경로 미인식
- **문제**: `git mv src/engine/platform/Window.cpp Engine/platform/Window.cpp` 직후 `Edit` 으로 include 경로 변경 시 "File has not been read yet" 에러.
- **원인**: Edit 도구는 세션 내 Read 이력을 추적. git mv 는 파일 시스템 경로를 바꿈 → Edit 입장에선 "새 경로의 파일" 로 인식. 같은 파일이지만 Read 이력이 새 경로엔 없음.
- **해결**: PowerShell 로 직접 텍스트 치환 (System.IO.File.ReadAllText + String.Replace + WriteAllText). 한 번에 7개 include 수정.
- **교훈**: 파일 이동(`git mv` 또는 Move-Item) 후엔 Edit 도구는 Read 가 필요. 대량 치환은 PowerShell 이 더 빠르고 안전.

### 문제 3 — 헤더 의존성 누수 — `engine/...` 접두 제거 필요
- **문제**: 기존 include `#include "engine/platform/Window.h"` 형태. 새 구조에선 Engine 폴더 자체가 인클루드 루트가 되므로 `engine/` 접두는 중복.
- **원인**: 단일 vcxproj 시절 `$(SolutionDir)src` 인클루드 루트 → `src/engine/...` 경로 → include 에 `engine/` 명시.
- **해결**: 4 파일 7개 include 라인 수정. `#include "platform/Window.h"` 같이 단순.
- **교훈**: include 경로는 vcxproj 의 AdditionalIncludeDirectories 와 함께 설계. 변경 시 양쪽 동시 갱신.

### 문제 4 — `\b_(...|...)` 정규식 word boundary 매칭 — `_fence` 가 `_fenceValue` 의 부분문자열
- **문제**: 단순 문자열 치환으로 `_fence → m_fence` 하면 `_fenceValue` 가 `m_fenceValue` 로 변환되긴 하지만 의도가 우연. 안전성 약함.
- **원인**: 한 멤버 이름이 다른 멤버의 prefix.
- **해결**: 정규식 word boundary `\b_fence\b` 로 정확 매칭. `_fence`, `_fenceValue`, `_fenceEvent` 각각 별도 매칭.
- **교훈**: 멤버 이름이 prefix 관계일 때 일괄 치환은 word boundary 필수. PowerShell `[regex]::Replace` 사용.

### 문제 5 — Edit 도구 Read 캐시 다섯번 무효화
- **문제**: 작업 중 Edit 도구의 Read 캐시가 여러 차례 갱신되어 system-reminder 가 누적 (총 5번 가량).
- **원인**: 같은 파일을 여러 edit + PowerShell 외부 변경 + 사용자 IDE 갱신 의 혼재.
- **해결**: 정상 동작이라 무시. 단 system-reminder 가 누적되어 컨텍스트 약간 소모.
- **교훈**: PowerShell 직접 치환과 Edit 도구를 혼용할 땐 캐시 무효화가 자주 일어남. 한 파일은 한 도구로 일관 처리가 효율적.

### (실수 없음 — 빌드/실행)
- 모든 빌드 첫 시도 통과. 빌드 실패 0회.

## 6. 결과 / 검증

| 항목 | 결과 |
|---|---|
| CODE_STYLE.md 영구 보존 | ✅ 9섹션, 학습 자료 폴더 삭제 후에도 참조 가능 |
| 멤버 prefix `m_camelCase` 통일 | ✅ 6 파일 12 멤버, grep 검증 |
| Engine.lib 빌드 | ✅ Debug/Release |
| Client.exe 빌드 | ✅ Debug/Release |
| Client 실행 + 깔끔 종료 | ✅ ExitCode 0 |
| ARCHITECTURE.md 작성 | ✅ 10섹션 |

스크린샷 자리표시자:
- (TODO) VS2022 솔루션 익스플로러: Engine + Client 두 프로젝트 트리
- (TODO) 빌드 출력 창: Engine.lib → Client.exe 의존 순서

## 7. AI 협업 메모

- 학습 자료 분석은 메인 컨텍스트에서 직접 (Read 도구 9개 파일).
- 코드 작성은 모두 메인 직접.
- 서브에이전트 호출 없음 (인프라 작업은 리뷰 대상이 아니라 결정 작업).
- 향후: 새 코드는 신 스타일·신 구조 기준으로 oop-reviewer 호출.

## 8. 다음 단계

- Phase 1D-2 — `RtvDescriptorHeap` (RTV 디스크립터 힙 클래스).
- 신규 코드는 새 구조(`Engine/render/`) + 신 스타일(`m_camelCase`, 전방선언, RAII 등).
- 추가로 `Engine/core/Types.h` 도입 검토 (int32/uint32 등 alias 첫 사용처에서).

## 9. PPT 재료로 쓸 만한 포인트

- **"학습 자료에서 무엇을 가져오고 무엇을 거부하는가"** — 결정 표 슬라이드. 한국 게임 학습 코드 vs AAA 디시플린 절충.
- **"멤버 prefix 결정의 광역 컨텍스트"** — `_` (학습 자료) vs `m_` (Microsoft/Unreal). 외부 코드 친화성 고려.
- **"Engine.lib 분리의 시그널"** — 모놀리식 vs 도구 재사용 가능 구조. 펄어비스 자체 엔진 경험 어필.
- **"파일 이동 vs 도구 캐시"** — 결정 번복 비용 + git mv 워크플로 학습 사례.
