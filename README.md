# portfolio_engine

> 펄어비스 게임플레이 프로그래머 포트폴리오용 **C++ / DirectX 12 풀스크래치 엔진 + 액션 컴뱃 수직 슬라이스** 프로젝트.

## 목표

라이브러리 의존 없이 직접 짠 C++/DX12 코어 위에서 동작하는 짧은 액션 컴뱃 데모를 제작한다.
한 캐릭터 + 한 보스 + 한 챔버 + 60~90초 데모 영상을 최종 산출물로 한다.

## 핵심 시스템 (예정)

- DirectX 12 렌더러 (커맨드/디스크립터/리소스 관리, PBR, 그림자, 포스트프로세스)
- 메시/텍스처/애니메이션 에셋 파이프라인
- 스켈레탈 애니메이션 (스테이트 머신 + 블렌드 트리 + 루트 모션)
- 충돌/물리 (캐릭터 캡슐, 스윕, 히트박스)
- 캐릭터 컨트롤러 / 카메라 / 입력 (Win32 + XInput)
- 액션 컴뱃 (콤보 / 캔슬 / 입력 버퍼링 / 히트스톱)
- 어빌리티 / 스킬 시스템 (데이터 주도)
- AI 보스 (Behavior Tree + 페이즈)
- ImGui 디버그 오버레이 (개발용)

## 기술 스택

- C++20, MSVC (Visual Studio 2022)
- DirectX 12 (Windows SDK 10.0.26100.0)
- HLSL Shader Model 6
- Win32 API

## 프로젝트 구조

```
portfolio_engine/
├── Engine/                 # Engine.lib (StaticLibrary)
│   ├── platform/  (Window, Input 등 OS 계층)
│   └── render/    (Device, CommandQueue, SwapChain 등 DX12 코어)
├── Client/                 # Client.exe (Application)
│   └── main.cpp
├── shaders/                # HLSL (예정)
├── assets/                 # 메시·텍스처·애니메이션 (예정)
├── docs/                   # ARCHITECTURE.md, CODE_STYLE.md
├── devlog/                 # 단계별 작업 기록
└── portfolio_engine.sln    # 2개 프로젝트 (Engine + Client)
```

- 의존성 방향: **Client → Engine** 일방. 자세한 내용은 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) 참조.
- 향후 도구(Editor / AssetCooker 등) 는 Client.vcxproj 패턴으로 추가, Engine.lib 재사용.

## 빌드

### 요구사항
- Windows 10/11
- Visual Studio 2022 (v143 toolset)
- Windows SDK 10.0.26100.0 이상

### MSBuild (CLI)
```
"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" ^
    portfolio_engine.sln /p:Configuration=Debug /p:Platform=x64 /m
```

### VS2022 IDE
1. `portfolio_engine.sln` 열기
2. 솔루션 구성: Debug 또는 Release, 플랫폼: x64
3. Client 프로젝트 우클릭 → "시작 프로젝트로 설정"
4. F5 (디버깅 시작) 또는 Ctrl+F5

### 출력
- `build/x64/{Debug,Release}/Engine.lib`
- `build/x64/{Debug,Release}/Client.exe`

## 문서

- [ORCHESTRATION.md](ORCHESTRATION.md) — Claude Code 서브에이전트 활용 운영 지침
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — 모듈 구조와 의존성
- [docs/CODE_STYLE.md](docs/CODE_STYLE.md) — 코드 스타일 가이드 (명명·헤더·클래스 패턴)
- [devlog/](devlog/) — 단계별 상세 작업 기록 (포트폴리오 PPT 원천 자료)

## 진행 상황

📍 **현재**: **Phase 2 종결 — 회전하는 컬러 큐브 도달 🎲**

Phase 1 (Foundation + 첫 삼각형):
- 1A Foundation Skeleton / 1B Window / 1C Device / 1D-1 CommandQueue
- 1D-2 RtvHeap / 1D-3 SwapChain / 1D-4 Clear+Present
- 1E-1 Shader / 1E-2 RootSig+PSO / 1E-3 첫 삼각형 ✅

Phase 2 (인프라 + 3D 회전):
- A-1 HrCheck (ThrowIfFailed 공용) ✅
- A-2 Logger (OutputDebugString wrapper) ✅
- A-3 Types (int8/uint32 alias) ✅
- B-1 DepthStencilBuffer ✅
- B-2 ConstantBuffer + RootSig 매개변수화 ✅
- B-3 Camera (LookAt + Perspective) ✅
- B-4 IndexBuffer + 큐브 + 매 프레임 회전 ✅

**다음 후보**: 카메라 입력 (WASD + 마우스) → 메시 로더 → 텍스처 → 조명.
