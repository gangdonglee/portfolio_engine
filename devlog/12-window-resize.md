# 12. Phase 3+ — 윈도우 리사이즈 (WM_SIZE → SwapChain/Depth/Viewport/Camera) 🪟

- **날짜**: 2026-05-15
- **관련 커밋**: (이 단계 커밋)
- **소요 시간**: 약 30분
- **단계**: Phase 3 누적 TODO 처리 (1/5)

---

## 1. 목표

마우스로 윈도우 모서리를 끌어 크기를 바꿔도 큐브가 늘어나거나 잘리지 않도록, 백버퍼·뎁스버퍼·뷰포트·카메라 종횡비를 모두 새 크기에 맞춰 일관되게 갱신.

## 2. 사전 컨텍스트

직전 단계까지: 1280×720 으로 시작된 윈도우는 사용자가 크기를 바꿔도 백버퍼는 그대로. 결과적으로 큐브가 비율 깨진 형태로 보임. 최소화 시엔 빈 화면.

WM_SIZE 메시지는 Window WndProc 가 이미 받아서 `m_width/m_height` 만 갱신하고 있었음. SwapChain·DepthStencilBuffer·viewport·camera 가 그 변화를 *모름*.

## 3. 결정과 트레이드오프

### 12-1. dirty 플래그 + 폴링 — 콜백 등록 회피
- **결정**: Window 에 `m_resizeDirty` bool + `ConsumeResize()` 메서드. main 이 매 프레임 폴링.
- **후보**:
  - A) `std::function<void(uint32, uint32)>` 콜백을 Window 에 등록.
  - B) Window → SwapChain/Camera 로 직접 참조 보유 (Window 가 알아서 호출).
- **선택 이유**:
  - 폴링은 의존 방향이 main → engine 한 방향 — engine 의 라이프타임/순서를 main 이 통제.
  - 콜백 패턴은 Window 가 렌더 자원을 알게 되어 platform 레이어가 render 레이어를 역참조하는 의존 역전.
  - 매 프레임 1회 bool 체크 비용 ≈ 0.
- **포기한 것**: 리사이즈 *시점*에 정확히 1회 트리거되는 게 아니라, 그 다음 PumpMessages 이후의 프레임 시작 시점에 처리됨. 1프레임 지연. 사람 눈엔 인지 불가.

### 12-2. RTV 슬롯 재사용 — 디스크립터 위치 고정
- **결정**: SwapChain::Resize 가 새 RTV 슬롯을 Allocate 하지 않고, 기존 `m_rtvHandles[i]` 위치에 `CreateRenderTargetView` 로 덮어쓰기.
- **후보**: RtvDescriptorHeap 의 `Free(idx)` API 신설 + 매 Resize 마다 새로 Allocate.
- **선택 이유**:
  - D3D12 의 `CreateRenderTargetView` 는 destination handle 에 그냥 덮어씀 — free/realloc 사이클 불필요.
  - RtvDescriptorHeap 은 현재 free 리스트 미구현 — Resize 마다 Allocate 하면 슬롯이 무한 증가.
  - 디스크립터 핸들은 *위치 슬롯*이지 *리소스 자체*가 아니므로 안전.
- **포기한 것**: 향후 동적 RTV 할당/해제가 필요한 시점(렌더 타깃 풀 등) 엔 별도 Free 메커니즘 필요.

### 12-3. DepthStencilBuffer — `CreateBufferAndView` private helper 추출
- **결정**: 생성자와 Resize 가 같은 텍스처 + DSV 생성 코드를 공유하도록 helper 도입. DSV 힙 자체는 ctor 에서 한 번만.
- **후보**: Resize 가 ctor 코드를 복붙.
- **선택 이유**: 같은 로직 2번 작성은 향후 한쪽만 수정될 위험. DSV 힙은 capacity 1 고정이라 재생성 불필요 — 슬롯 위치 보존 + 메모리 절약.
- **포기한 것**: 없음.

### 12-4. SIZE_MINIMIZED + 0x0 보호
- **결정**: WM_SIZE 핸들러에서 `wParam != SIZE_MINIMIZED && width > 0 && height > 0` 일 때만 dirty 설정.
- **이유**: `IDXGISwapChain::ResizeBuffers(N, 0, 0, ...)` 는 D3D12 가 거부 (E_INVALIDARG). 최소화 시엔 dirty 안 켜고 복귀 (SIZE_RESTORED) 시 다시 정상 크기로 켬.
- **포기한 것**: 최소화 중에는 카메라 종횡비 0 으로 분할 발생 가능 — 어차피 렌더 안 되니 무관.

### 12-5. 리사이즈 분기 위치 — Input::BeginFrame 직전
- **결정**: `PumpMessages → ConsumeResize → BeginFrame → 게임 루프` 순서.
- **이유**: 리사이즈는 *해당 프레임의 상태*에 영향 (viewport·camera). 입력 처리 전에 처리해야 그 프레임의 카메라 갱신이 새 종횡비 기반으로 일어남.

## 4. 작업 내용

### 4-1. Window 확장
- 위치: [Engine/platform/Window.h](../Engine/platform/Window.h), [.cpp](../Engine/platform/Window.cpp)
- `m_resizeDirty` 필드 추가. WM_SIZE 핸들러에서 정상 크기 변경 시만 set.
- `bool ConsumeResize() noexcept` — dirty 면 true + 클리어. width/height 는 `Width()/Height()` 로 별도 조회.

```cpp
case WM_SIZE: {
    const int newWidth  = static_cast<int>(LOWORD(lParam));
    const int newHeight = static_cast<int>(HIWORD(lParam));
    if (wParam != SIZE_MINIMIZED && newWidth > 0 && newHeight > 0) {
        if (newWidth != m_width || newHeight != m_height) {
            m_width        = newWidth;
            m_height       = newHeight;
            m_resizeDirty  = true;
        }
    }
    return 0;
}
```

### 4-2. SwapChain::Resize
- 위치: [Engine/render/SwapChain.cpp](../Engine/render/SwapChain.cpp)
- 시퀀스:
  1. 모든 `m_backBuffers[i].Reset()` — ResizeBuffers 가 외부 참조 없음을 요구.
  2. `m_swapChain->GetDesc1(&desc)` — 기존 desc 보존 (Format·Flags·SwapEffect).
  3. `m_swapChain->ResizeBuffers(kBackBufferCount, w, h, desc.Format, desc.Flags)`.
  4. `m_swapChain->GetBuffer(i, ...)` + `CreateRenderTargetView(..., m_rtvHandles[i])` — 슬롯 재사용.
  5. `m_currentIndex = GetCurrentBackBufferIndex()`.
- 호출자 책임: `commandQueue.FlushGpu()` 선행 (GPU 가 백버퍼 참조 중이면 ResizeBuffers 실패).

### 4-3. DepthStencilBuffer::Resize
- 위치: [Engine/render/DepthStencilBuffer.cpp](../Engine/render/DepthStencilBuffer.cpp)
- `CreateBufferAndView(device, w, h)` private helper 신설 — 생성자와 Resize 가 공유.
- DSV 힙은 ctor 에서 한 번만 생성, Resize 는 텍스처 + DSV 만 갱신 (같은 `m_dsvHandle` 에 덮어쓰기).

### 4-4. main.cpp 리사이즈 분기
- 위치: [Client/main.cpp](../Client/main.cpp)
- `PumpMessages` 직후 `ConsumeResize` 폴링 → 전체 자원 갱신:

```cpp
if (window.ConsumeResize()) {
    commandQueue.FlushGpu();
    const auto w = static_cast<std::uint32_t>(window.Width());
    const auto h = static_cast<std::uint32_t>(window.Height());

    swapChain.Resize(device, w, h);
    depthBuffer.Resize(device, w, h);

    viewport.Width  = static_cast<float>(w);
    viewport.Height = static_cast<float>(h);
    scissor.right   = static_cast<LONG>(w);
    scissor.bottom  = static_cast<LONG>(h);

    camera.SetPerspective(kFovY, static_cast<float>(w) / h, kNearPlane, kFarPlane);
}
```

- FoV / near / far 는 `constexpr float kFovY/kNearPlane/kFarPlane` 로 상수화 — 초기 호출과 리사이즈 호출이 같은 값 보장.

## 5. 마주친 문제와 해결 ⚠ 필수 — 빠짐없이 기록

### 문제 1 — `swprintf` + `std::size` 의 헤더 의존
- **문제**: SwapChain.cpp 에 새로 추가한 `std::swprintf(line, std::size(line), ...)` 로그가 컴파일 통과 여부 불확실 — 기존 include 에 `<cwchar>` 와 `<iterator>` 없음.
- **원인**: SwapChain.cpp 의 원래 코드는 wide string 포맷 로그 미사용. 이 단계에서 처음 도입.
- **해결**: `<cwchar>` (swprintf), `<iterator>` (std::size) 추가. 다른 cpp(Texture/DepthStencilBuffer)와 동일 패턴.
- **교훈**: 헤더 의존을 호출하는 함수 기준으로 명시. cstdio 는 char 계열, cwchar 는 wchar_t 계열 — 구분.

### 문제 2 — 매 픽셀 드래그마다 WM_SIZE 폭주
- **문제**: 사용자가 모서리를 천천히 드래그하면 1픽셀마다 WM_SIZE → 매번 FlushGpu + Resize. 드래그 중 GPU 가 거의 매 픽셀 stall.
- **원인**: WM_SIZE 는 OS 가 픽셀 단위로 발생. throttle 없음.
- **해결 (이번 단계)**: 같은 크기로의 중복 WM_SIZE 는 `newW != m_width || newH != m_height` 가드로 무시. dirty 플래그 자체가 멱등 (소비 시점에 한 번만 처리). 픽셀당 호출이 여전하지만 작업이 idempotent 라 시각적으로 부드러움.
- **향후 개선**: WM_ENTERSIZEMOVE / WM_EXITSIZEMOVE 페어로 드래그 중엔 dirty 만 누적, 종료 시 1회만 Resize 트리거. 마우스 드래그 인터랙션 개선 단계에서 도입 예정.
- **교훈**: WM_SIZE 같은 OS 이벤트는 빈도 가정을 검증하고, 무거운 작업을 매 이벤트마다 하지 말 것.

### 문제 3 — RtvDescriptorHeap 슬롯 누수 우려
- **문제**: 처음엔 Resize 마다 `rtvHeap.Allocate()` 를 새로 호출하는 안 고려 — capacity = `kBackBufferCount` = 2 인 힙이라 두 번째 Resize 에서 즉시 exception.
- **원인**: 디스크립터 핸들 ≠ GPU 리소스. 디스크립터 슬롯은 영구 할당이고 view 만 덮어쓰면 됨을 처음엔 직관적이지 않게 받아들임.
- **해결**: 기존 `m_rtvHandles[i]` 슬롯 재사용. `CreateRenderTargetView` 는 destination handle 에 단순 덮어쓰기.
- **교훈**: D3D12 디스크립터 모델 — 슬롯은 "어디에" 정보, 리소스는 "무엇이". 둘은 독립.

## 6. 결과 / 검증

- **빌드**: Debug + Release 둘 다 0 warning / 0 error.
- **실행 확인 포인트** (사용자 직접):
  - 윈도우를 좌우로 늘려도 큐브 종횡비 유지.
  - 위로 늘려도 동일.
  - 최소화 후 복귀해도 정상.
  - 드래그 중 일시적 stall 있지만 잔상/깨짐 없음.

## 7. AI 협업 메모

- Window·SwapChain·DepthStencilBuffer·main 4개 파일에 일관된 변경. 의존 방향(main → engine) 보존을 위해 dirty 플래그 + 폴링 패턴 선택.
- DepthStencilBuffer 의 ctor/Resize 코드 중복은 private helper 로 한 번에 정리 — D3D12 코드는 인자 많은 desc 구조체가 많아 helper 추출 가치가 큼.

## 8. 다음 단계

누적 TODO 순서:
- **N 프레임 in-flight** — 현재 매 프레임 FlushGpu 로 CPU/GPU 직렬화. 2~3 프레임 in-flight 로 throughput 개선.
- DXGI_PRESENT_ALLOW_TEARING — VRR 환경 V-Sync OFF 일관성.
- MTL 머티리얼 — 면별 색·텍스처.
- OBJ n-gon 자동 삼각형화.

## 9. PPT 재료로 쓸 만한 포인트

- "WM_SIZE → 4종 자원 일괄 갱신 흐름도 (SwapChain/Depth/Viewport/Camera)"
- "D3D12 디스크립터 슬롯 재사용 — 위치 vs 리소스 분리 모델의 이점"
- "polling vs callback — platform 레이어가 render 를 모르게 하는 의존 방향 선택"
