# 01. 프로젝트 셋업 — 결정과 출발

- **날짜**: 2026-05-13
- **관련 커밋**: `63c4577` (초기 셋업), `abffb6c` (커스텀 서브에이전트 추가)
- **소요 시간**: 약 1시간
- **단계**: Phase 0 — Pre-Foundation

---

## 1. 목표

펄어비스 게임플레이 프로그래머 직군 포트폴리오로 사용할 **풀스크래치 C++/DirectX 12 액션 컴뱃 데모**의 프로젝트 골격을 만든다. 코딩 시작 전 1) 큰 방향 결정 2) AI 협업 워크플로 셋업 3) 기록 체계 구축까지 정리한다.

## 2. 사전 컨텍스트

- 이 작업 이전엔 빈 디렉토리 외에 아무것도 없음.
- 사용자는 DX9/DX11/Unity 경험 있고 DX12는 학습 중.
- 환경: VS2022 Community, Windows SDK 10.0.26100.0, Windows 10 Pro.
- 사용자가 명시한 제약: ① 옵션 A 풀스크래치 ② 객체지향 준수 ③ 빌드 시스템은 순수 .sln/.vcxproj.

## 3. 결정과 트레이드오프

### 결정 1 — 옵션 A (풀스크래치)
- **후보**:
  - **A**: 모든 레이어 자작 (DX12 + 애니메이션 + 충돌 + 컴뱃 + AI)
  - **B**: DX12 자작 + 검증된 라이브러리 (ozz-animation, Jolt, ImGui, assimp 등)
  - **C**: DX12 비주얼 미니멀 + 게임플레이 풀스크래치
- **선택**: **A**
- **선택 이유**: 펄어비스가 자체 엔진(블랙스피릿) 회사이므로 풀스크래치 시그널이 강하게 작용. C++ 깊이를 직접 증명할 수 있음.
- **포기한 것**: 4개월 마일스톤 — A는 현실적으로 6~10개월. 스코프 컷 우선순위를 사전 합의해 리스크 완화.

### 결정 2 — 빌드 시스템: 순수 .sln/.vcxproj
- **후보**: CMake / Premake / 순수 VS
- **선택**: **순수 .sln/.vcxproj**
- **선택 이유**: VS2022 단일 IDE 전용. CMake/Premake의 학습/보일러플레이트 회피, 즉시 출발.
- **포기한 것**: 다른 IDE/플랫폼 이식성. 현 단계 비관심.

### 결정 3 — 객체지향 엄격 준수
- 모든 서브시스템을 클래스로 분리, SRP·캡슐화·RAII 기본.
- 게임 핫 패스에서도 가상 함수 비용이 OOP 위배의 면죄부가 되지 않음(필요 시 final/CRTP 등 비-위반 우회 사용).
- **자동 보증 장치**: `.claude/agents/oop-reviewer.md` — 새 클래스 추가 시 자동 호출.

## 4. 작업 내용

### 4-1. 디렉토리 / 파일
- `d:\Things\portfolio_engine\` 신규 생성.
- 다음 파일 작성:
  - `README.md` — 프로젝트 개요 및 목표
  - `ORCHESTRATION.md` — Claude Code 서브에이전트 운영 지침 (워크플로 패턴 5종 + 안티 패턴 + 마일스톤별 적용 + 일일 사이클)
  - `.gitignore` — VS / MSVC / CMake / 셰이더 캐시 / GPU 캡처 / Claude 로컬 설정 등 제외
  - `devlog/` — 본 디렉토리. 단계별 기록 누적용.

### 4-2. Git 셋업
- `git init -b main` (기본 브랜치 main)
- 로컬 user.name/email 설정 (전역 미설정 상태 존중, 로컬 한정)
- 원격 추가: `https://github.com/gangdonglee/portfolio_engine`
- 첫 커밋(63c4577): 초기 셋업
- 두 번째 커밋(abffb6c): 커스텀 서브에이전트 + ORCHESTRATION.md §1.1

### 4-3. AI 협업 워크플로
**ORCHESTRATION.md** 에 다음을 명문화:
1. 사용 가능한 서브에이전트 인벤토리(내장 5 + 커스텀 2)
2. 핵심 원칙 4가지 (도구로서의 위치, 병렬화, 이해 책임, 결과 길이 제한)
3. 워크플로 패턴 5종:
   - 패턴 A: Research Fan-out (병렬 조사)
   - 패턴 B: Independent Review (커밋 전 독립 검토)
   - 패턴 C: Plan then Validate (설계 + 비평)
   - 패턴 D: Lookup (빠른 위치 탐색)
   - 패턴 E: Background (디브로그·조사 병렬 실행)
4. 안티 패턴 6종
5. 마일스톤별 권장 패턴 표
6. 일일 작업 사이클

### 4-4. 프로젝트 전용 서브에이전트
`.claude/agents/` 에 정의:

| 에이전트 | 역할 |
|---|---|
| **dx12-reviewer** | DX12 코드 리뷰 — 리소스 상태 전이, 펜스/동기화, 디스크립터 힙, 커맨드 리스트 소유권, COM 라이프타임, HLSL↔C++ 인터페이스. 250단어 이내 punch list 반환. |
| **oop-reviewer** | C++ OOP 원칙 검사 — SRP, 캡슐화, RAII, 의존성 방향, 다형성 적절성, 인터페이스 분리. 200단어 이내 위반/의심/잘된점 보고. |

### 4-5. 권한 사전 허용 (마찰 제거)
`.claude/settings.local.json` (gitignored):
- `Read`, `Write`, `Edit`, `Glob`, `Grep` — 파일 작업 전반
- `Bash(git *)`, `Bash(msbuild *)`, `Bash(cl *)`, `Bash(devenv *)`, `Bash(fxc *)`, `Bash(dxc *)` — 빌드/Git/셰이더 컴파일
- `Bash(dir|cd|ls|pwd|echo|where *)` — 디렉토리 유틸

### 4-6. 한글 인코딩 처리
모든 한글 마크다운 파일에 **UTF-8 BOM** 적용. 한국어 Windows 환경 일부 에디터가 BOM 없는 UTF-8을 CP949로 오인식해 깨지는 문제 사전 방지.

## 5. 마주친 문제와 해결 ⚠ 필수 — 빠짐없이 기록

### 문제 1 — Git 글로벌 user.name/email 미설정
- **문제**: `git commit` 직전 검사 시 글로벌 git 사용자 정보가 비어 있어 커밋 실패 위험.
- **원인**: 사용자 PC에 글로벌 git config 미설정.
- **해결**: 사용자에게 이름·이메일 확인 받은 뒤 본 레포에만 로컬 config로 설정 (`git config --local user.name "Kangdong Lee"`, `git config --local user.email "kd2zzang@gmail.com"`). 다른 레포에 영향 없음.
- **교훈**: 새 PC/새 레포에서는 첫 커밋 전 `git config user.name`/`user.email` 사전 확인. 글로벌 미설정 발견 시 로컬로 한정 설정 (사용자 의도 보호).

### 문제 2 — Write 도구가 BOM 없이 UTF-8 저장
- **문제**: 한글 .md 파일이 일부 에디터에서 깨짐.
- **원인**: Claude Code의 Write 도구는 UTF-8(BOM 없음)이 기본. 한국어 Windows의 일부 에디터는 BOM이 없으면 CP949로 자동 추정.
- **해결**: Write 직후 PowerShell로 BOM 추가: `$utf8Bom = New-Object System.Text.UTF8Encoding($true); [System.IO.File]::WriteAllText($path, $content, $utf8Bom)`. 메모리에 규칙 저장(`feedback-korean-bom`)해 모든 후속 한글 문서에 자동 적용.
- **교훈**: 한글 콘텐츠 .md/.txt 작성 후 즉시 BOM 적용. 코드 파일(.cpp/.h/.hlsl)에는 적용 금지(컴파일러 호환성).

### 문제 3 — Bash 도구로 한글 경로 + 따옴표 명령 시 파싱 에러
- **문제**: 메모리 디렉토리 존재 확인을 위해 Bash로 `ls "C:\Users\이강동_2\.claude\projects\d--Things\memory\" 2>/dev/null || echo "EMPTY_OR_NEW"` 실행 → `bash: eval: line 1: unexpected EOF while looking for matching '` 에러 후 exit 2.
- **원인**: Git Bash가 Windows 백슬래시 경로 + 한글 + 따옴표의 조합을 처리하며 따옴표 매칭 파서가 오작동. 또는 trailing `\"` 가 escape로 해석돼 다음 따옴표 매칭이 깨짐.
- **해결**: PowerShell로 전환: `if (Test-Path $mem) { Get-ChildItem $mem -Force | ... } else { Write-Output "MEMORY_DIR_NOT_EXIST" }`. PowerShell은 백슬래시·한글·따옴표 처리가 안정적.
- **교훈**: 한글 경로 또는 백슬래시 포함 Windows 경로 다룰 때는 **Bash 대신 PowerShell 우선**. Bash는 UNIX 경로(`/d/Things/...`)에서만 안전.

### 문제 4 — PowerShell 병렬 호출 중 한 건 취소
- **문제**: 환경 점검 시 3개 PowerShell 명령을 병렬 호출했는데 git config 체크가 exit 1로 끝나며 vswhere 검사가 "Cancelled: parallel tool call PowerShell errored" 로 취소됨.
- **원인**: 병렬 도구 호출 그룹에서 한 건이 실패하면 같은 그룹의 나머지가 취소되는 도구 실행 메커니즘. git config --global의 미설정 값 조회는 git에서 exit 1을 반환하는데 이게 그룹 실패로 인식됨.
- **해결**: 실패한 명령을 별도 메시지로 분리해 재실행. 동시에 git config 결과를 사용자 질문으로 분기 처리.
- **교훈**: 병렬 호출 시 **각 명령이 독립적으로 exit 0 가능한지** 사전 확인. 결과가 없을 수 있는 조회(`git config --get`, `where.exe`)는 단독 호출 또는 `2>$null; if ($?)...` 가드로 감싸기.

## 6. 결과 / 검증

- ✅ `d:\Things\portfolio_engine\` 디렉토리 + 6개 파일 생성
- ✅ GitHub 원격 푸시 성공 (`origin/main`, 2 커밋)
- ✅ 한글 인코딩 정상 (GitHub 웹뷰 + 로컬 VSCode 확인)
- ✅ VS2022 Community + Windows SDK 10.0.26100.0 동작 확인

스크린샷 자리표시자:
- (TODO) GitHub 레포 메인 페이지 스크린샷
- (TODO) ORCHESTRATION.md 한 페이지 캡처 (워크플로 패턴 표)

## 7. AI 협업 메모

- 이 단계에서는 코드를 짜지 않았으므로 코드 리뷰 에이전트 호출은 없음.
- 사용한 도구는 Write/Edit/Bash/PowerShell 직접 호출. 서브에이전트 fan-out은 다음 단계부터.
- 사용자 피드백 누적 — 메모리에 저장된 항목:
  - `user-language`: 한국어 응답 기본
  - `user-role`: 게임플레이 프로그래머 / 펄어비스 목표
  - `user-tech-background`: DX9/DX11/Unity 경험, DX12 학습 중
  - `feedback-korean-bom`: 한글 파일에 UTF-8 BOM 적용
  - `feedback-plan-scope`: 큰 계획서는 폐기되는 경향, 작게 시작
  - `project-portfolio-engine`: 본 프로젝트 메타데이터

## 8. 다음 단계

- **Phase 1 Foundation 시작**:
  1. `src/`, `shaders/`, `assets/`, `external/` 디렉토리 구조
  2. .sln + .vcxproj 생성 (VS2022 Win32 Desktop App)
  3. 진입점, 메인 루프, Win32 윈도우 클래스 (`engine::Window`)
  4. DX12 디바이스 초기화 (`engine::render::Device`, `SwapChain`, `CommandQueue`)
  5. 백버퍼 클리어 → 첫 삼각형 (`HelloTriangle`)

미뤄둔 항목:
- 셰이더 빌드 자동화 (fxc vs dxc, 빌드 이벤트 vs 커스텀 빌드 단계) — 첫 셰이더 추가 시 결정
- Debug 출력 채널 / 로깅 시스템 — 첫 디바이스 초기화 직후

## 9. PPT 재료로 쓸 만한 포인트

- **"왜 풀스크래치인가" 슬라이드** — 옵션 A/B/C 비교 표 + 펄어비스 자체 엔진 컨텍스트
- **"AI-Assisted Engine Development" 슬라이드** — Claude Code 서브에이전트 오케스트레이션 도입한 워크플로 다이어그램
- **"객체지향 엄격 준수" 슬라이드** — `oop-reviewer` 자동 검증 흐름도
- **개발 진행 방식 슬라이드** — devlog 누적 + 커밋 그래프
