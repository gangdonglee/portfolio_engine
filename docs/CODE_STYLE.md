# CODE_STYLE — 코드 스타일 가이드

> 본 프로젝트의 신규/리팩토링 코드가 따라야 하는 명명·헤더·클래스 패턴.
> 한국 게임 업계의 흔한 DX12 학습 코드베이스 스타일을 채택하되, 본 프로젝트의 OOP 디시플린과 절충한다.

## 1. 출처

- **학습 자료**: `D:\Things\Animation_소스코드` (Engine 폴더 분석 — Device, CommandQueue, SwapChain, RootSignature, ConstantBuffer, Object, Engine, EnginePch 등). 한국 게임 업계 강의 스타일의 DX12 풀스크래치 엔진.
- 본 문서는 학습 자료 폴더 삭제 후에도 코드 스타일이 유지되도록 핵심을 추출·정리한다.

## 2. 결정 표 — 학습 자료 vs 본 프로젝트

| 항목 | 학습 자료 | 본 프로젝트 | 비고 |
|---|---|---|---|
| 멤버 변수 prefix | `_camelCase` | **채택** | `_hwnd`, `_fenceValue` |
| 타입 alias (`int8`/`uint32` 등) | `__int8` 기반 | **채택** (단 `std::int*_t` 베이스) | `using int32 = std::int32_t;` |
| 클래스/메서드 명명 | `PascalCase` | **채택** | `Init()`, `Device`, `PumpMessages()` |
| enum class | UPPER_SNAKE 값 + `END` 마커 | **채택** | `CONSTANT_BUFFER_TYPE::TRANSFORM` |
| 클래스명 = 파일명 | ✓ | **채택** | `Window.h` → `class Window` |
| RAII 생성자 | `Init()` 분리 | **거부** — 생성자 일괄 | 외부 의존 없으면 ctor 안에서 throw |
| 네임스페이스 | 없음 (global) | **거부** — `engine::*` 사용 | 모듈 경계 명확 |
| `using namespace std/WRL/DirectX` 전역 | ✓ | **거부** | `Microsoft::WRL::ComPtr` 명시 |
| PCH | EnginePch.h 거대 | **거부** | 전방선언으로 헤더 위생 |
| 매크로 단축 (`DEVICE`, `CMD_LIST`) | 적극 | **거부** | 가독성·디버그 우선 |
| 전역 `extern GEngine` | ✓ | **거부** | 의존성 명시적 |
| `DECLARE_SINGLE` 매크로 | ✓ | **거부** | DI 또는 명시적 인스턴스 |
| HRESULT 처리 | 무시 다수 | **거부** | `ThrowIfFailed` 일관 |
| `WIN32_LEAN_AND_MEAN` 위치 | pch.h | **vcxproj 전역** | 모든 TU 일관 |

핵심 원칙: **명명/엔지니어링 디테일은 학습 자료에서 가져오고, 아키텍처/디시플린은 본 프로젝트 OOP 기준 유지**.

---

## 3. 명명 규칙

### 3-1. 식별자

| 범주 | 규칙 | 예 |
|---|---|---|
| 클래스 / struct | PascalCase | `Device`, `SwapChain`, `WindowInfo` |
| 메서드 / 함수 | PascalCase | `Init()`, `PumpMessages()`, `Signal()` |
| 인터페이스/추상 | PascalCase | (`I` 접두사 사용 안 함) |
| 전역 함수 | PascalCase | `ThrowIfFailed()` |
| **private/protected 멤버 변수** | **`_camelCase`** | `_hwnd`, `_fenceValue`, `_isOpen` |
| public struct field | `camelCase` (그대로) | `width`, `windowed` |
| 로컬 변수 | camelCase | `backIndex`, `valueToSignal` |
| 함수 파라미터 | camelCase | `width`, `title` |
| `constexpr` / `const` 정적 상수 | `kPascalCase` | `kMinFeatureLevel`, `kWaitTimeoutMs` |
| 매크로(만들 일 거의 없음) | `UPPER_SNAKE_CASE` | `UNREFERENCED_PARAMETER` |
| enum class 이름 | UPPER_SNAKE_CASE 또는 PascalCase | `CONSTANT_BUFFER_TYPE`, `Phase` |
| enum class 값 | 시맨틱에 따라 (UPPER_SNAKE 또는 PascalCase) | `TRANSFORM`, `Direct` |
| 네임스페이스 | `snake_case` 또는 단일 단어 | `engine::render`, `engine::platform` |

### 3-2. 파일

- 클래스 1개에 대응하는 파일 쌍: `<ClassName>.h` + `<ClassName>.cpp`.
- 폴더 구조가 네임스페이스를 미러: `engine::platform` → `src/engine/platform/`.

### 3-3. 타입 alias

`engine/core/types.h` (예정) 에 정의 후 본 프로젝트 전역 사용.

```cpp
#pragma once
#include <cstdint>

using int8  = std::int8_t;
using int16 = std::int16_t;
using int32 = std::int32_t;
using int64 = std::int64_t;
using uint8  = std::uint8_t;
using uint16 = std::uint16_t;
using uint32 = std::uint32_t;
using uint64 = std::uint64_t;
```

- 학습 자료는 `__int8` 등 Microsoft 전용 타입 사용 → 우리는 `<cstdint>` 표준 베이스.
- 도입 시점: 첫 사용처가 등장하는 단계(예: SwapChain 의 `BackBufferIndex` 카운터, 정점 인덱스 등).

---

## 4. 헤더 위생

### 4-1. 디폴트
- 모든 헤더는 `#pragma once`.
- 헤더에는 **클라이언트가 정말 필요로 하는 것만** 노출.
- 큰 SDK 헤더(`d3d12.h`, `dxgi*.h`, `Windows.h`, `fbxsdk.h`)는 **`.cpp` 에 한정**. 헤더에선 전방선언.
- `WIN32_LEAN_AND_MEAN` / `NOMINMAX` 는 `vcxproj` 전역 `PreprocessorDefinitions`.
- ComPtr 멤버는 헤더에 두지만 `ComPtr<incomplete-type>` 패턴이 작동하도록 **소멸자 정의를 `.cpp` 에** 둠 (mandatory).

### 4-2. 학습 자료와의 차이
- 학습 자료는 거대한 PCH 로 모든 헤더를 묶고 `using namespace` 까지 전역.
- 우리는 PCH 미사용. 빌드 속도는 SDK 가 충분히 빠르고, 향후 PCH 도입 결정점은 빌드 시간이 명확히 문제될 때.

### 4-3. 전방선언 패턴 (DX12)
```cpp
// Device.h
#include <wrl/client.h>

struct IDXGIFactory6;
struct IDXGIAdapter1;
struct ID3D12Device;
struct ID3D12InfoQueue;

namespace engine::render {
class Device final { /* ... */ };
}
```

---

## 5. 클래스 패턴

### 5-1. RAII 생성자
```cpp
class Window final {
public:
    Window(int width, int height, std::wstring_view title);  // 모든 초기화, 실패 시 throw
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&&) = delete;
    Window& operator=(Window&&) = delete;
    /* ... */
};
```

- 학습 자료는 `Init()` 메서드 분리 → 우리는 **생성자 일괄**.
- 부분 생성 상태 보호: 멤버 디폴트 초기화 + 소멸자 가드.

### 5-2. final + 4종 삭제
- 상속 의도 없으면 `final`.
- 단일 소유 의도면 복사·이동 4종 모두 `= delete` (Rule of Zero 변형).
- 진정 이동 가능해야 할 경우만 명시적 default.

### 5-3. 멤버 변수
- `_camelCase` prefix.
- 디폴트 초기화 권장: `int _width = 0;`, `bool _isOpen = false;`.
- ComPtr 멤버는 자동 nullptr.

### 5-4. raw COM 노출
```cpp
// Native() 로 통일 — "raw COM 포인터를 그대로 노출" 의도 명시
ID3D12Device* Native() const noexcept;
```
- 외부 노출은 **YAGNI** — 정말 호출처가 있을 때만 추가.

### 5-5. 에러 처리
```cpp
// 모든 HRESULT 는 ThrowIfFailed 로
ThrowIfFailed(d3dDevice->CreateCommandQueue(...), "CreateCommandQueue");
```
- 학습 자료의 "HRESULT 무시" 패턴은 채택하지 않음.

---

## 6. DX12 코딩 패턴 (학습 자료 참고, 적용 예정)

### 6-1. ResourceBarrier — `d3dx12.h` 헬퍼
```cpp
auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
    backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
cmdList->ResourceBarrier(1, &barrier);
```
- `d3dx12.h` (Microsoft 공식 helper 헤더) 사용 OK. Phase 1D 의 SwapChain Present 단계에서 도입.

### 6-2. SwapChain 백버퍼 인덱스
```cpp
constexpr int kSwapChainBufferCount = 2;
_backBufferIndex = (_backBufferIndex + 1) % kSwapChainBufferCount;
```
- 학습 자료의 `SWAP_CHAIN_BUFFER_COUNT = 2` 상수 + 모듈러 증가 패턴.

### 6-3. CommandAllocator + CommandList 페어
- Allocator 1 + List 1 페어 (단일 프레임).
- 멀티 프레임 in-flight 는 추후 단계.
- 학습 자료의 `_cmdAlloc / _cmdList` 패턴 채택.

### 6-4. 펜스 동기화 — 우리 개선 패턴 유지
- 학습 자료: `_fenceValue++; Signal; if(GetCompletedValue<_fenceValue) Wait(INFINITE);`
- 우리: 로컬 `next` 변수로 롤백 안전 + 5초 타임아웃.
- **우리 패턴 유지** (학습 자료보다 견고).

### 6-5. 매 프레임 Render 흐름
```
RenderBegin:
  Allocator->Reset(); List->Reset();
  Barrier: Present → RenderTarget
  ClearRenderTargetView
RenderEnd:
  Barrier: RenderTarget → Present
  List->Close();
  Queue->ExecuteCommandLists();
  SwapChain->Present();
  WaitSync();
  SwapIndex();
```
- 학습 자료 패턴 그대로 채택.

---

## 7. 학습 자료에서 가져오지 않는 것

### 7-1. PCH 거대화
```cpp
// 학습 자료 EnginePch.h — 거대 통합. 우리는 안 함.
#include <windows.h>
#include <d3d12.h>
#include <wrl.h>
using namespace std;
using namespace Microsoft::WRL;
using namespace DirectX;
```
**거부 이유**: `using namespace` 충돌 위험, 헤더 의존성 누수, 컴파일 단위 위생 손상.

### 7-2. 매크로 단축
```cpp
// 학습 자료 — 우리는 안 함.
#define DEVICE GEngine->GetDevice()->GetDevice()
#define GRAPHICS_CMD_LIST GEngine->GetGraphicsCmdQueue()->GetGraphicsCmdList()
#define INPUT GET_SINGLE(Input)
```
**거부 이유**: 디버그 추적 어려움, 검색·리팩토링 마찰, IDE 지원 약화.

### 7-3. 전역 Engine 인스턴스
```cpp
// 학습 자료 — 우리는 안 함.
extern unique_ptr<class Engine> GEngine;
```
**거부 이유**: 전역 가변 상태, 테스트 어려움, 초기화 순서 fiasco 위험.

### 7-4. 싱글톤 매크로
```cpp
// 학습 자료 — 우리는 안 함.
#define DECLARE_SINGLE(type) \
private: type() {} ~type() {} \
public: static type* GetInstance() { static type instance; return &instance; }
```
**거부 이유**: DI 가 더 명확하고 테스트 친화적. 진정 한 인스턴스만 가능해야 하면 명시적 보장(예: factory 또는 owner 클래스).

---

## 8. 적용 정책

### 8-1. 신규 코드
- 본 문서의 결정을 그대로 따른다.
- 새로운 패턴이 필요해지면 **본 문서에 먼저 추가**한 뒤 코드에 적용.

### 8-2. 기존 코드
- 멤버 prefix `m_` → `_` 일괄 변경 (별도 commit, `refactor:` 타입).
- 다른 결정 사항은 이미 본 프로젝트가 따르고 있음.

### 8-3. 검토 루틴
- 코드 리뷰(`oop-reviewer`) 호출 시 본 문서의 규칙도 점검 항목.
- 위반 발견 시 즉시 수정 + devlog `§5 마주친 문제와 해결` 에 기록.

### 8-4. 본 문서의 운영
- 새 결정 사항은 본 문서에 추가하고 메모리에도 보강.
- 학습 자료 폴더가 삭제되어도 본 문서가 영구 참조 자료가 된다.

---

## 9. 빠른 체크리스트 (코드 작성 직전 확인)

- [ ] 클래스: `final` + 4종 delete 명시?
- [ ] 멤버: `_camelCase` prefix?
- [ ] 생성자: RAII 일괄 처리 (Init 분리 X)?
- [ ] 헤더: 큰 SDK 헤더는 .cpp 로? 전방선언 사용?
- [ ] HRESULT: `ThrowIfFailed` 로?
- [ ] 네임스페이스: `engine::*` 안에?
- [ ] 매크로 단축 / 전역 / 싱글톤 사용 안 함?
- [ ] enum class + UPPER_SNAKE 또는 PascalCase 값?
