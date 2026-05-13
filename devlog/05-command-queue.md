# 05. Phase 1D-1 — `engine::render::CommandQueue`

- **날짜**: 2026-05-13
- **관련 커밋**: `337d880`
- **소요 시간**: 약 1시간
- **단계**: Phase 1D-1 — CommandQueue

---

## 1. 목표

D3D12 Direct 큐 + ID3D12Fence + Win32 이벤트 핸들을 묶은 **GPU 동기화 인프라** 구축. 이후 SwapChain Present, ExecuteCommandLists 의 기반.

## 2. 사전 컨텍스트

- 직전 단계: [Phase 1C Device](04-device.md). DX12 디바이스 + DXGI Factory6 생성됨.
- 본 단계는 큐·펜스·이벤트만. ExecuteCommandLists 와 CommandAllocator/List 페어는 후속.

## 3. 결정과 트레이드오프

### 결정 1 — Direct 큐만 (Compute/Copy 큐 보류)
- 본 단계 데모는 그래픽 렌더만 필요 → DIRECT 큐.
- Compute/Copy 큐는 별도 클래스 또는 ctor 오버로드로 추가 (YAGNI).

### 결정 2 — `Device::Native()` 공개 접근자 도입
- Window 의 `Handle()` 제거 사례와 달리, `ID3D12Device*` 는 렌더 서브시스템 거의 전부가 필요로 함.
- friend 폭증 회피 위해 공개 접근자 채택. 단 명명을 `Native()` 로 — "raw COM 노출" 의도 명시.
- 초안에선 `Factory()`/`Adapter()` 도 같이 노출했으나 OOP 리뷰가 YAGNI 위반 지적 → 제거 (재추가는 호출처가 생길 때).

### 결정 3 — Signal 롤백 안전 패턴
- 학습 자료 패턴: `_fenceValue++; Signal(...);` — Signal 실패 시 멤버 비대칭 증가.
- 우리: `const next = m_fenceValue + 1; Signal(next); m_fenceValue = next;` — 실패 시 멤버 미증가.

### 결정 4 — WaitForFenceValue 5초 타임아웃
- 학습 자료 패턴: `WaitForSingleObject(event, INFINITE)` — GPU TDR/디바이스 제거 시 영구 블록 위험.
- 우리: 5초 타임아웃 → 타임아웃 시 예외. 소멸자 경로에선 try/catch 가 swallow → 프로세스 종료 가능.

### 결정 5 — `FenceEventHandle = void*` typedef
- 헤더에서 `HANDLE` 직접 노출하지 않기 위해 `void*` alias.
- Windows.h 의존성 누수 차단.

## 4. 작업 내용

### 4-1. 클래스 인터페이스 (`CommandQueue.h`)
```cpp
struct ID3D12CommandQueue;
struct ID3D12Fence;

namespace engine::render {
    using FenceEventHandle = void*;

    class Device;

    class CommandQueue final {
    public:
        explicit CommandQueue(Device& device);
        ~CommandQueue();
        CommandQueue(const CommandQueue&) = delete; /* + 3 deletes */

        std::uint64_t Signal();
        void WaitForFenceValue(std::uint64_t value);
        void FlushGpu();
        ID3D12CommandQueue* Native() const noexcept;

    private:
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
        Microsoft::WRL::ComPtr<ID3D12Fence>        m_fence;
        std::uint64_t    m_fenceValue = 0;
        FenceEventHandle m_fenceEvent = nullptr;
    };
}
```

### 4-2. 핵심 흐름
- 생성자: `CreateCommandQueue(DIRECT)` → `CreateFence(0)` → `CreateEventW(auto-reset, non-signaled)`.
- 소멸자: 정상 생성된 경우 `FlushGpu()` (try/catch) → `CloseHandle` → ComPtr 역순 Release.
- `Signal`: `next = m_fenceValue + 1` → `m_queue->Signal(fence, next)` → 성공 시 `m_fenceValue = next`.
- `WaitForFenceValue`: GetCompletedValue 체크 → SetEventOnCompletion → 5초 WaitForSingleObject → 타임아웃/비정상 시 예외.

## 5. 마주친 문제와 해결 ⚠ 필수 — 빠짐없이 기록

### 문제 1 — `WaitForSingleObject(INFINITE)` 의 영구 블록 위험 (DX12 리뷰)
- **문제**: 초안에서 INFINITE 대기. GPU 행/TDR/디바이스 제거 시 CPU 영구 정지. 소멸자 경로에선 프로세스 종료까지 막힘.
- **원인**: 학습 자료 패턴 답습.
- **해결**: 5초 타임아웃(`kWaitTimeoutMs = 5000`) + WAIT_TIMEOUT / 비정상 반환 시 예외. 소멸자의 try/catch 가 예외 swallow → 깔끔 종료.
- **교훈**: 무한 대기는 라이브러리 코드의 디폴트가 되어선 안 됨. 항상 타임아웃 + 에러 채널.

### 문제 2 — Signal 실패 시 `m_fenceValue` 비대칭 증가 (DX12 리뷰)
- **문제**: `++m_fenceValue; Signal();` 패턴은 Signal 실패 시 펜스는 안 시그널됐는데 멤버만 증가. 후속 호출 의미 비대칭.
- **원인**: 학습 자료 패턴 답습.
- **해결**: 로컬 `next` 변수 → Signal 성공 후 멤버 갱신.
- **교훈**: 상태 변경은 검증된 작업 직후에만. "선 증가 후 사용" 패턴은 실패 처리에 약함.

### 문제 3 — `Device::Factory()/Adapter()` 선노출 (OOP 리뷰 — YAGNI 자기모순)
- **문제**: 헤더 주석에 "정말 필요한 호출처가 생길 때 추가" 라고 써놓고 미사용 접근자 3개 모두 노출.
- **원인**: "곧 SwapChain 이 필요할 테니" 식 선노출 충동.
- **해결**: `Native()` 만 남기고 Factory/Adapter 제거. 다음 단계 SwapChain 도입 시 친구 또는 좁힌 접근자로 다시 도입.
- **교훈**: 자기 디시플린을 자기가 깰 수 있음. 헤더 주석을 적었다면 그대로 지킬 것.

### 문제 4 — `Get()` 명명이 의도 불명 (OOP 리뷰)
- **문제**: `CommandQueue::Get()` 과 `Device::Get()` 시그니처 같고 반환 타입 다름. raw COM 노출인지 다른 의도인지 호출 측에서 모름.
- **원인**: 무성의한 일반 이름.
- **해결**: 둘 다 `Native()` 로 통일. "raw COM 포인터 노출" 의도 명시.
- **교훈**: 같은 이름을 같은 의도에 쓸 것. 모호한 일반 이름은 호출 측의 인지 부담.

### 문제 5 — `m_fenceEvent: void*` 의도 불명 (OOP 의심)
- **문제**: 헤더에서 Windows.h 의존을 피하려고 `void*` 멤버 — 의도가 코드만 봐선 안 보임.
- **원인**: 헤더 위생 우선했으나 타입 의미 약화.
- **해결**: `using FenceEventHandle = void*;` typedef 추가. 이름이 "HANDLE 등가" 임을 명시.
- **교훈**: 의도 표현이 약한 타입엔 alias 로 의미 부여.

### (그 외 실수 없음)
- 빌드는 첫 시도 통과. 실행도 깔끔. 리뷰만 5건 — 모두 반영.

## 6. 결과 / 검증

- Debug/Release|x64 빌드 0 경고 0 에러.
- 윈도우 + Device + CommandQueue 생성 → 2초 살아있음 → CloseMainWindow → 소멸자 FlushGpu → ExitCode 0.
- 디버거 부착 시 `[render] CommandQueue (Direct) created` 로그 확인 가능.

## 7. AI 협업 메모

- 패턴 B (독립 리뷰) 두 번째 적용. oop-reviewer + dx12-reviewer 두 에이전트 병렬 호출 (general-purpose 임베드 우회).
- 비겹침 결과: DX12 리뷰가 Critical 2건 (INFINITE/Signal 롤백) 잡고, OOP 리뷰가 YAGNI/명명/alias 3건 잡음.
- 리뷰 시간: 각 ~22초, 병렬이라 합산 시간이 직렬 대비 절반.

## 8. 다음 단계

- Phase 1D-2 (보류 → 학습 자료 분석 + Engine/Client 분리가 끼어듦): RtvDescriptorHeap.

## 9. PPT 재료

- **"펜스 동기화의 함정"** — INFINITE 대기 + 선 증가 패턴 → 5초 타임아웃 + 후 증가 패턴 비교 슬라이드.
- **"Native() 명명 규약"** — raw COM 노출의 명시적 의도 표현.
