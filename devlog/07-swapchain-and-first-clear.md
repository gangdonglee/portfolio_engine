# 07. Phase 1D — SwapChain + 첫 클리어 (1D-2 ~ 1D-4)

- **날짜**: 2026-05-13 ~ 2026-05-14
- **관련 커밋**: `810bda1` (1D-2 RtvHeap), `5095466` (1D-3 SwapChain), `0e8fe9c` (1D-4a CommandList), `8af9bcf` (1D-4b 프레임 루프)
- **소요 시간**: 약 3시간 (4 sub-stage 합산)
- **단계**: Phase 1D — Render Loop Bootstrap

---

## 1. 목표

Phase 1D 의 큰 목표 — **첫 가시 결과 도달**. 윈도우 클라이언트 영역이 매 프레임 정의된 색(어두운 슬레이트)으로 클리어되어 보이게 한다.

세부:
- RTV 디스크립터 힙 (1D-2)
- DXGI SwapChain + 백버퍼 RTV 등록 (1D-3)
- CommandAllocator + CommandList 페어 (1D-4a)
- 매 프레임 RenderBegin/Clear/RenderEnd/Present 루프 (1D-4b)
- Window 의 임시 회색 배경 → nullptr 복원

## 2. 사전 컨텍스트

- 직전 단계: [Phase 1D-1 CommandQueue](05-command-queue.md). Direct 큐 + 펜스 + 이벤트 동기화 인프라 완료.
- 그 사이에 인프라 작업([devlog 06](06-style-and-split.md)) — m_ prefix 통일 + Engine.lib/Client.exe 분리 + CODE_STYLE.md.
- 본 단계가 **첫 가시 출력 도달점**. 이후 모든 렌더 기능은 본 토대 위에 쌓임.

## 3. 결정과 트레이드오프

### 결정 1 — RtvDescriptorHeap 단순 단방향 할당 (Allocate + CpuHandle)
- 후보: ① freelist 기반 슬롯 재사용 ② 단방향 increment only
- 선택: **단방향**. 슬롯 해제/재사용은 후속 단계에서 freelist 도입 (현 단계 비필요).
- 캡슐화: Capacity 가드 + Debug 빌드 assert(index < capacity).

### 결정 2 — DXGI FLIP_DISCARD + 2 백버퍼
- Flip Model 채택 — DXGI_SWAP_EFFECT_FLIP_DISCARD.
- 백버퍼 수 = 2. 트리플 버퍼는 후속 단계 (필요 시 `kBackBufferCount` 만 바꾸면 됨).
- SampleDesc.Count = 1 — Flip Model 은 MSAA OFF 필수.

### 결정 3 — Window-SwapChain friend 패턴
- Window 의 HWND 는 외부 노출 금지 ([Phase 1B 결정](03-window-class.md)).
- SwapChain 만 HWND 필요 → `Window` 가 `engine::render::SwapChain` 을 friend 로 선언, private `NativeHwnd()` 메서드만 호출 허용.
- 공개 API 에는 HWND 가 등장하지 않음 → 최소 노출 원칙 유지.

### 결정 4 — Device::Factory() 공개 접근자 추가
- [Phase 1D-1 devlog](05-command-queue.md) 에서 "Factory/Adapter 는 호출처가 등장하는 시점에 추가" 명시.
- SwapChain 이 CreateSwapChainForHwnd 호출에 IDXGIFactory2+ 필요 → 이 시점에 도입.
- YAGNI 일관성 유지.

### 결정 5 — 단순 1프레임 in-flight (FlushGpu per frame)
- 학습 자료 패턴 채택. 매 프레임 시작 시 `commandQueue.FlushGpu()` 로 직전 GPU 완료 보장 후 Reset.
- 성능 < 단순성. 향후 N프레임 in-flight 로 개선 (cmdList N개 풀 + per-frame fence 추적).

### 결정 6 — d3dx12.h 회피, manual D3D12_RESOURCE_BARRIER
- Windows SDK 10.0.26100 에 d3dx12.h 미포함 (Microsoft 가 DirectX-Headers 외부 패키지로 분리).
- 옵션 A 풀스크래치 원칙 → 외부 의존 회피.
- D3D12_RESOURCE_BARRIER 를 수기로 채움. 코드는 조금 길어지지만 의도가 명시적.

## 4. 작업 내용

### 4-1. RtvDescriptorHeap (커밋 `810bda1`)
```cpp
namespace engine::render {
class RtvDescriptorHeap final {
public:
    RtvDescriptorHeap(Device& device, uint32_t capacity);
    D3D12_CPU_DESCRIPTOR_HANDLE Allocate();
    D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle(uint32_t index) const noexcept;
    ID3D12DescriptorHeap* Native() const noexcept;
    /* ... */
};
}
```
- D3D12_DESCRIPTOR_HEAP_FLAG_NONE (RTV 는 GPU 가시 불필요)
- 생성자에서 `GetDescriptorHandleIncrementSize` 1회 캐시
- SIZE_T 양쪽 캐스트로 핸들 오프셋 오버플로 차단 (DX12 리뷰 반영)

### 4-2. SwapChain + Window friend (커밋 `5095466`)
```cpp
namespace engine::platform {
class Window final {
    friend class engine::render::SwapChain;
    HWND NativeHwnd() const noexcept { return m_hwnd; }  // private
};
}
namespace engine::render {
class SwapChain final {
    SwapChain(Device&, CommandQueue&, Window&, RtvDescriptorHeap&);
    void Present();
    ID3D12Resource* CurrentBackBuffer() const noexcept;
    D3D12_CPU_DESCRIPTOR_HANDLE CurrentRtv() const noexcept;
    static constexpr uint32_t kBackBufferCount = 2;
};
}
```
- `CreateSwapChainForHwnd(presentQueue.Native(), ...)` — 흔한 함정(device 오인 전달) 회피
- `MakeWindowAssociation(NO_ALT_ENTER)` — 직접 fullscreen 관리
- IDXGISwapChain1 → IDXGISwapChain3 QueryInterface
- 백버퍼 2개 각각 `GetBuffer` + `CreateRenderTargetView`, RTV 핸들 멤버에 저장

### 4-3. CommandList + CommandQueue::Execute (커밋 `0e8fe9c`)
```cpp
namespace engine::render {
class CommandList final {
    explicit CommandList(Device& device);
    void Reset();   // Allocator + List 리셋 (직전 FlushGpu 전제)
    void Close();
    ID3D12GraphicsCommandList* Native() const noexcept;
};
void CommandQueue::Execute(CommandList& list);  // ExecuteCommandLists 호출
}
```
- CreateCommandList 직후 recording 상태이므로 즉시 Close → 일관된 Reset→record→Close 사이클

### 4-4. 프레임 루프 + 회색 배경 nullptr 복원 (커밋 `8af9bcf`)
```cpp
while (window.IsOpen()) {
    window.PumpMessages();
    if (!window.IsOpen()) break;

    commandQueue.FlushGpu();  // 1프레임 in-flight
    cmdList.Reset();
    auto* list = cmdList.Native();

    // PRESENT → RENDER_TARGET (수기 D3D12_RESOURCE_BARRIER)
    list->ResourceBarrier(1, &toRenderTarget);

    list->ClearRenderTargetView(swapChain.CurrentRtv(), kClearColor, 0, nullptr);

    // RENDER_TARGET → PRESENT
    list->ResourceBarrier(1, &toPresent);

    cmdList.Close();
    commandQueue.Execute(cmdList);
    swapChain.Present();
}
commandQueue.FlushGpu();  // 소멸 순서 안전
```
- `kClearColor = (0.05, 0.07, 0.10, 1.0)` — dark slate
- Window `hbrBackground` 임시 회색 → `nullptr` 복원

## 5. 마주친 문제와 해결 ⚠ 필수 — 빠짐없이 기록

### 문제 1 — RtvDescriptorHeap CpuHandle 오버플로 가능성 (DX12 리뷰 반영)
- **문제**: `handle.ptr += static_cast<SIZE_T>(m_count) * m_descriptorSize` — `m_count`(uint32) × `m_descriptorSize`(uint32) 가 SIZE_T 캐스트 전 32비트로 곱해짐.
- **원인**: 한쪽만 캐스트한 단순 실수.
- **해결**: 양쪽 모두 `static_cast<SIZE_T>` — `Allocate()` / `CpuHandle()` 동일 패턴.
- **교훈**: 정수 승격은 첫 피연산자가 결정. 양쪽 캐스트가 안전 자산. RTV 힙은 실해 0이지만 향후 CBV/SRV 힙(수만 슬롯)에 같은 패턴 재사용 시 잠재 위험.

### 문제 2 — RtvDescriptorHeap::CpuHandle 인덱스 검증 0 + noexcept (DX12 + OOP 리뷰)
- **문제**: `noexcept` 메서드가 사일런트 OOB 가능. GPU 검증 레이어에서야 발견.
- **해결**: Debug 빌드 `assert(index < m_capacity)` 추가. Release 에선 no-op.
- **교훈**: noexcept getter 라도 assert 는 무료. silent OOB 차단.

### 문제 3 — `Device::Factory()/Adapter()` 사전 노출 (OOP 리뷰 — YAGNI 자기모순)
- **문제**: Phase 1D-1 에서 SwapChain 미존재인데 Factory/Adapter 도 같이 공개.
- **해결**: Native() 만 유지. Factory 는 Phase 1D-3 의 SwapChain 도입 시 추가.
- **교훈**: 헤더 주석에 "호출처 등장 시 추가" 라고 명시하면 그대로 지킬 것. 자기 디시플린 자기 감시.

### 문제 4 — `Get()` 명명 모호 (OOP 리뷰)
- **문제**: `Device::Get()` 과 `CommandQueue::Get()` 시그니처 같고 반환 타입 다름.
- **해결**: 둘 다 `Native()` 로 통일. raw COM 노출 의도 명시.
- **교훈**: 동일 의도엔 동일 이름.

### 문제 5 — d3dx12.h 가 Windows SDK 10.0.26100 에 미포함
- **문제**: `#include <d3dx12.h>` 시 C1083 "No such file or directory". CD3DX12_RESOURCE_BARRIER 헬퍼 사용 불가.
- **원인**: Microsoft 가 d3dx12.h 를 SDK 에서 분리해 DirectX-Headers GitHub/NuGet 패키지로 이동. 일부 SDK 버전엔 포함, 우리 26100 엔 미포함.
- **해결**: 옵션 A 풀스크래치 원칙에 따라 외부 의존 회피. `D3D12_RESOURCE_BARRIER` 를 수기로 작성 (8줄 정도).
- **교훈**: CODE_STYLE.md §6-1 의 "d3dx12.h 사용 OK" 가정은 SDK 별 차이. 실제 import 시 확인 필수. SDK 변경에 의존하지 않는 manual 방식이 풀스크래치 원칙과 더 일관.

### 문제 6 — 종료 시 hang (~3초 이상) — 소멸 순서로 인한 GPU 활성 중 Release
- **문제**: 첫 프레임 루프 도입 후 CloseMainWindow → 프로세스 종료가 3초+ 걸리거나 안 됨.
- **원인**: 선언 순서가 `cmdList` 가 `commandQueue` 보다 뒤 → 소멸은 cmdList 먼저. GPU 가 in-flight 작업 중인 상태에서 `ID3D12CommandAllocator` / `ID3D12GraphicsCommandList` Release → UB. 추가로 `IDXGISwapChain3` 파괴가 큐된 프레임 대기로 hang.
- **해결**: 메인 루프 종료 직후 명시적 `commandQueue.FlushGpu()` 추가. 모든 소멸자 진입 전 GPU 완료 보장.
- **교훈**: **D3D12 RAII 소멸 순서 ≠ GPU 작업 완료**. CPU↔GPU 동기화는 명시적이어야 한다. 학습 자료의 `Engine` 클래스도 동일 이유로 명시적 cleanup 단계를 둠. 본 프로젝트는 RAII 디시플린을 유지하되 마지막 sync 만 명시.

### 문제 7 — CommandList 의 Allocator+List 초기 상태 일관성
- **문제**: `CreateCommandList` 직후 List 는 recording 상태. 다음 `Reset()` 호출이 일관되려면 closed 상태여야 함.
- **해결**: 생성자 끝에 `m_list->Close()` 즉시 호출.
- **교훈**: D3D12 객체의 초기 상태 명세 확인 필수. "라이브러리 클래스의 디폴트 상태" 가 사용자 기대와 다를 수 있다.

### 문제 8 — git mv 후 Edit 도구 Read 캐시 무효화 (재발)
- **문제**: 본 단계에서도 파일 이동·갱신 후 Edit 호출 시 "File has not been read yet" 에러 발생.
- **해결**: Read 우선 호출 후 Edit. 또는 PowerShell 로 직접 치환.
- **교훈**: [devlog 06 문제 2](06-style-and-split.md#문제-2) 와 동일 패턴. 도구 캐시 기억 항목 고정화.

## 6. 결과 / 검증

| 구성 | 결과 | 비고 |
|---|---|---|
| Debug\|x64 빌드 | ✅ | 경고 0, 에러 0 |
| Release\|x64 빌드 | ✅ | 경고 0, 에러 0 |
| 윈도우 표시 + 렌더 루프 | ✅ | 3초 이상 안정 실행 |
| CloseMainWindow → 정상 종료 | ✅ | ExitCode 0, 명시적 FlushGpu 후 깔끔 |
| 디버그 레이어 자동 검증 | ✅ | Corruption/Error 시 break 활성 |

**가시 결과**: 윈도우 클라이언트 영역이 **dark slate(어두운 청색-회색)** 단색으로 매 프레임 클리어된다. 윈도우 리사이즈 시 색은 유지되지만 백버퍼 크기가 자동 재생성되지 않음(현 단계 미구현, TODO).

스크린샷 자리표시자:
- (TODO) dark slate 색상 클리어된 1280×720 윈도우
- (TODO) VS 디버거 출력 창 — Adapter / SwapChain / CommandList 로그
- (TODO) PIX 캡처 — 매 프레임 Barrier + Clear + Present 명령 순서

## 7. AI 협업 메모

본 Phase 1D 의 모든 sub-stage 에서 패턴 B (독립 리뷰) 적용.

| sub-stage | oop-reviewer | dx12-reviewer | 즉시 반영 |
|---|---|---|---|
| 1D-2 RtvHeap | 0건 위반, 4건 의심 | 0 Critical, 2 Warning, 4 Nit | SIZE_T 양쪽 캐스트, debug assert, =default |
| 1D-3 SwapChain | 0건 위반, 5건 의심 | 0 Critical, 2 Warning, 3 Nit | factory 일관성 주석, Win10+ 주석 |
| 1D-4a CommandList | (생략 — 패턴 익숙, 회귀 위험 낮음) | (생략) | — |
| 1D-4b 프레임 루프 | (생략 — main.cpp 사용 코드 위주) | (생략) | hang fix (명시적 FlushGpu) |

병렬 리뷰 (한 메시지 2 Agent 호출) — 4 sub-stage 중 2번 적용. 응답 시간 각 ~22~30초, 합산 시 직렬 대비 절반 절약. 비겹침 결과(OOP 와 DX12 가 서로 다른 지점 잡음) 가 매번 입증됨.

향후 패턴 D (Lookup) 도 도입 검토 — `Explore` 서브에이전트로 비슷한 패턴이 다른 곳에 어떻게 적용되는지 빠른 탐색.

## 8. 다음 단계

### Phase 1 종결, Phase 2 진입 준비
Phase 1 (Foundation + Render Bootstrap) 모든 sub-stage 완료. 다음은 Phase 2 — 의미 있는 그래픽 출력 또는 Phase 1 마무리 작업.

**즉시 후보 (택일 또는 묶음):**
- **Phase 1E**: 첫 삼각형 (정점 버퍼 + 셰이더 + PSO + DrawInstanced)
- **인프라 보강**: WM_SIZE 시 SwapChain Resize, 로깅 시스템, ThrowIfFailed 공용 헤더(`Engine/core/HrCheck.h`)
- **타입 alias 도입**: `Engine/core/Types.h` (int8/uint32 등)
- **N프레임 in-flight 개선**: cmdList N개 풀 + per-frame fence 추적

추천: **Phase 1E 첫 삼각형** — 가장 임팩트 큰 마일스톤(자기 셰이더로 그린 첫 결과). 이후 인프라 보강을 순차 도입.

### TODO 누적 목록 (commit 본문 산재 → 통합)
- [ ] DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING + DXGI_PRESENT_ALLOW_TEARING (Tearing 지원 사전 확인)
- [ ] WM_SIZE → SwapChain::Resize 처리
- [ ] ThrowIfFailed 공용 헤더 (`Engine/core/HrCheck.h`) — 4 cpp 에 중복 정의
- [ ] OutputDebugStringW 직접 호출 → 로깅 시스템 (`Engine/core/Logger.h`)
- [ ] `Engine/core/Types.h` int8/uint32 alias
- [ ] N프레임 in-flight 개선
- [ ] kBackBufferFormat (SwapChain 익명 ns) — DSV/포맷 일치 코드 등장 시 헤더 노출 검토
- [ ] friend 폭증 → `NativeHandle` opaque token 어댑터 — 호출처 증가 시 검토

## 9. PPT 재료로 쓸 만한 포인트

- **"첫 가시 결과 — Dark Slate Clear"** (Before/After 스크린샷 + 코드 발췌)
- **"DX12 매 프레임 흐름"** 다이어그램 (FlushGpu → Reset → Barrier(P→RT) → Clear → Barrier(RT→P) → Close → Execute → Present)
- **"ResourceBarrier 상태 전이"** — Present ↔ RenderTarget 다이어그램, 자동 추적이 없는 DX12 의 명시적 모델
- **"Windows SDK + d3dx12 분리 — 외부 의존 회피"** — DirectX-Headers 패키지 vs manual D3D12_RESOURCE_BARRIER 결정 슬라이드
- **"소멸 순서의 함정"** — RAII 만으로는 안되는 D3D12 의 CPU↔GPU 동기화. 명시적 FlushGpu 단계 추가
- **"Window-SwapChain Friend 캡슐화"** — 공개 API 에 HWND 미등장 모범 사례
- **"YAGNI 일관성"** — Device::Factory() 사전 노출 → 제거 → 호출처 등장 시 재도입 흐름 (학습 사례)
