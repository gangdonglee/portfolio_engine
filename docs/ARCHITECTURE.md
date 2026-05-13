# ARCHITECTURE — 모듈 구조와 의존성

> 본 프로젝트의 vcxproj 구성, 폴더/모듈 경계, 의존성 방향. 신규 모듈 추가 시 본 문서가 결정 기준.

## 1. 큰 그림

본 프로젝트는 **Engine 정적 라이브러리(.lib) + Client 실행파일(.exe)** 두 프로젝트로 구성된다.

```
┌────────────────────────────────────────┐
│            Client.exe                   │
│            (Application)                │
│                                         │
│   ┌──────────────────────────────┐     │
│   │   main.cpp                    │     │
│   │   (게임 코드 / 부트스트랩)        │     │
│   └──────────────────────────────┘     │
└─────────────────┬──────────────────────┘
                  │ links
                  ▼
┌────────────────────────────────────────┐
│           Engine.lib                    │
│           (StaticLibrary)                │
│                                         │
│   platform/  ── Window, Input (예정)     │
│   render/    ── Device, CommandQueue,   │
│                  SwapChain (예정) 등      │
│   anim/      ── (예정)                   │
│   gameplay/  ── (예정)                   │
└────────────────────────────────────────┘
                  │ links (시스템)
                  ▼
        d3d12.lib, dxgi.lib,
        d3dcompiler.lib, dxguid.lib
```

## 2. 분리 이유

- **재사용성**: Engine.lib 를 향후 별도 도구(Editor / AssetCooker / Headless Runner) 가 링크해 재사용. 게임 코드만 바꿔 끼우면 됨.
- **경계 강제**: 게임 코드(Client)가 엔진 내부 구현에 절대 직접 의존하지 못하도록 컴파일러가 보장. Engine 헤더만 인클루드 경로에 노출.
- **빌드 시간**: 게임 코드 변경 시 Engine.lib 가 캐시되어 incremental build.
- **포트폴리오**: AAA 회사들이 채택하는 모듈 분리 패턴을 그대로 시연.

## 3. 폴더/프로젝트 구조

```
portfolio_engine/
├── .claude/                     # AI 협업 설정
├── devlog/                      # 단계별 작업 기록
├── docs/                        # ARCHITECTURE.md, CODE_STYLE.md
├── shaders/                     # HLSL (Engine/Client 양쪽이 참조)
├── assets/                      # 메시·텍스처·애니메이션 (Client 가 참조)
├── Engine/                      # ⭐ Engine.lib 프로젝트
│   ├── Engine.vcxproj
│   ├── Engine.vcxproj.filters
│   ├── platform/                # Win32 윈도우, 입력
│   │   ├── Window.h
│   │   └── Window.cpp
│   ├── render/                  # DX12 렌더링 코어
│   │   ├── Device.h/cpp
│   │   ├── CommandQueue.h/cpp
│   │   ├── SwapChain.h/cpp      (예정)
│   │   └── ...
│   ├── anim/                    # 애니메이션 시스템 (예정)
│   ├── core/                    # 공용 타입/유틸 (예정)
│   └── gameplay/                # 공통 게임플레이 인프라 (예정)
├── Client/                      # ⭐ Client.exe 프로젝트
│   ├── Client.vcxproj
│   ├── Client.vcxproj.filters
│   └── main.cpp
├── build/                       # gitignored 빌드 산출물
│   ├── x64/
│   │   ├── Debug/{Engine.lib, Client.exe, *.pdb}
│   │   └── Release/{Engine.lib, Client.exe, *.pdb}
│   └── intermediate/
└── portfolio_engine.sln
```

## 4. 빌드 / 링크

### 4-1. Engine.vcxproj 핵심
- **ConfigurationType**: `StaticLibrary`
- **PreprocessorDefinitions**: `_LIB; WIN32_LEAN_AND_MEAN; NOMINMAX`
- **AdditionalIncludeDirectories**: `$(SolutionDir)Engine` (자기 자신의 헤더 루트)
- 외부 라이브러리 명시적 링크 없음 — 정적 라이브러리는 심볼 미해결을 허용하고, 최종 링크 시점에 Client 가 해결한다.

### 4-2. Client.vcxproj 핵심
- **ConfigurationType**: `Application`
- **SubSystem**: `Windows` (콘솔 창 미표시)
- **PreprocessorDefinitions**: `_WINDOWS; WIN32_LEAN_AND_MEAN; NOMINMAX`
- **AdditionalIncludeDirectories**: `$(SolutionDir)Engine`
- **AdditionalDependencies**: `d3d12.lib; dxgi.lib; d3dcompiler.lib; dxguid.lib`
- **ProjectReference**: `..\Engine\Engine.vcxproj` → MSBuild 가 의존 빌드 순서 강제 + Engine.lib 자동 링크.

### 4-3. 공통 컴파일러 옵션 (양 프로젝트 동일)
- C++ 표준: `stdcpp20`
- 경고 수준: `Level4` + `/permissive-` (ConformanceMode)
- `/utf-8` 명시 (한국어 Windows 의 CP949 오해석 차단)
- `MultiProcessorCompilation` true

### 4-4. 출력 경로
```
build/
├── x64/Debug/      ← Engine.lib, Client.exe, *.pdb
└── x64/Release/    ← Engine.lib, Client.exe, *.pdb
```
정적 라이브러리·실행파일·PDB 가 같은 폴더에 모여 디버거 부착 단순화.

## 5. include 경로 규약

- 양 프로젝트 모두 `$(SolutionDir)Engine` 을 인클루드 루트로.
- 헤더 인클루드는 `Engine/` 폴더 하위 경로 기준:
  - `#include "platform/Window.h"`
  - `#include "render/Device.h"`
  - `#include "render/CommandQueue.h"`
- `engine/` 접두는 사용하지 않음 (폴더 이름이 `Engine` 이며 인클루드 루트로 등록되어 있어 중복 표기 회피).

## 6. 네임스페이스 vs 폴더

- 폴더 이름은 **PascalCase** (`Engine/`, `Client/`).
- 네임스페이스는 **소문자** (`engine::platform::Window`, `engine::render::Device`).
- 두 표기가 다른 이유: 폴더는 프로젝트 단위 표시(빌드 시스템), 네임스페이스는 코드 단위 표시.

## 7. 신규 모듈 추가 절차

### 7-1. Engine 내부 새 모듈 (예: `engine::anim`)
1. `Engine/anim/` 폴더 생성.
2. 클래스 헤더/소스 추가 (CODE_STYLE.md 규칙 준수).
3. `Engine.vcxproj` 에 `<ClCompile>` / `<ClInclude>` 항목 추가.
4. `Engine.vcxproj.filters` 에 폴더 트리 반영.
5. 네임스페이스 `engine::anim::*` 사용.

### 7-2. 새 Client-스타일 도구 추가 (예: Editor)
1. `Editor/` 폴더 생성.
2. `Editor.vcxproj` 작성 (Client.vcxproj 를 템플릿으로):
   - `Application`
   - `ProjectReference Engine.vcxproj`
   - `AdditionalIncludeDirectories=$(SolutionDir)Engine`
3. `portfolio_engine.sln` 에 새 프로젝트 등록 + ProjectDependencies 명시.
4. Editor 전용 main.cpp + 추가 소스.

### 7-3. 외부 라이브러리 추가
- 단일 외부 lib 만 필요한 경우: 사용처(Engine 또는 Client)의 `AdditionalDependencies` 에 추가.
- 두 곳 모두 필요한 경우: Client 의 `AdditionalDependencies` 에만 추가 (Engine.lib 가 미해결 심볼로 두면 Client 가 해결).
- 헤더 검색 경로: 양쪽 `AdditionalIncludeDirectories` 에 추가.

## 8. 의존성 방향 (반드시 준수)

```
Client  ──►  Engine
```
- Client 는 Engine 을 의존. **반대 방향 금지**.
- Engine 의 어떤 모듈도 Client 의 헤더를 인클루드하지 않는다.
- Engine 내부에선 **상위 → 하위** (예: `render` → `core`)는 OK, **하위 → 상위**는 금지.

## 9. 현재 단계 (라이브 상태)

- **Engine**:
  - `platform/Window` — Win32 윈도우 RAII
  - `render/Device` — DXGI Factory6 + D3D12 Device
  - `render/CommandQueue` — Direct 큐 + 펜스 동기화
- **Client**:
  - `main.cpp` — 윈도우 + Device + CommandQueue 인스턴스 생성, 메시지 펌프 루프

## 10. 다음 단계 (예고)

- Engine 에 추가 예정: `render/SwapChain`, `render/RtvDescriptorHeap`, `core/Types`, `core/Logging`
- Client 에 추가 예정: 본격 렌더 루프 (Phase 1D 후반), 게임 로직 (Phase 2 이후)
