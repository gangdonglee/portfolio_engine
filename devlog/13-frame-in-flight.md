# 13. Phase 3+ — N 프레임 in-flight (CPU/GPU 병렬화) ⏩

- **날짜**: 2026-05-15
- **관련 커밋**: (이 단계 커밋)
- **소요 시간**: 약 1시간
- **단계**: Phase 3 누적 TODO 처리 (2/5)

---

## 1. 목표

매 프레임 `commandQueue.FlushGpu()` 로 CPU/GPU 를 직렬화하던 구조를 해체하여, CPU 가 다음 프레임 cmd 를 기록하는 동안 GPU 가 이전 프레임을 실행하는 일반적인 DX12 가속 패턴으로 전환.

성능 자체보다는 *DX12 의 프레임 동기화 모델을 직접 구현*하는 것이 본 단계의 목적 (펄어비스 면접 시그널).

## 2. 사전 컨텍스트

직전 단계까지의 매 프레임 흐름:

```
PumpMessages → BeginFrame → game update
→ commandQueue.FlushGpu()   ← CPU/GPU 직렬화
→ cmdList.Reset()
→ record (barrier/clear/draw/barrier)
→ cmdList.Close()
→ commandQueue.Execute(cmdList)
→ swapChain.Present()
```

문제:
- `FlushGpu` 는 *모든* 이전 GPU 작업 완료 대기. GPU 가 한가해질 때까지 CPU 가 놀고, CPU 가 record 하는 동안 GPU 도 놀음.
- DX12 의 in-flight 모델: CPU 가 N 프레임 앞서서 record, 같은 슬롯이 돌아왔을 때만 그 슬롯의 fence value 를 대기.

## 3. 결정과 트레이드오프

### 13-1. kFrameCount = SwapChain::kBackBufferCount = 2
- **결정**: 프레임 슬롯 수를 백버퍼 수와 동일하게.
- **후보**:
  - A) FrameCount = BackBufferCount (= 2) — 단순. SwapChain 의 백버퍼 인덱스 회전 cadence 와 프레임 슬롯 cadence 일치.
  - B) FrameCount = 3, BackBufferCount = 2 — 더 깊은 in-flight. CPU 가 GPU 보다 2 프레임 앞설 수 있음.
- **선택 이유**: A — 초기 도입은 단순함 우선. 백버퍼 인덱스가 자연 회전하므로 frameIndex 추가 회전이 SwapChain 의 currentIndex 와 동기. 입력 지연도 1프레임으로 제한.
- **포기한 것**: throughput 약간. N=3 가 더 안정적 60fps 유지에 유리하지만 입력 지연 1프레임 더 추가.

### 13-2. 슬롯당 자원 — CommandList + ConstantBuffer
- **결정**: 슬롯 N개의 (CommandList, ConstantBuffer). 나머지는 단일 인스턴스 공유.
  - 슬롯별: `cmdLists[i]`, `frameCBs[i]`, `frameFenceValues[i]`
  - 공유: Device, CommandQueue, Mesh(VB/IB), Texture, RootSig, PSO, RTV/DSV/SRV 힙, DepthStencilBuffer
- **이유**: GPU 큐는 명령을 *순차* 실행. frame N 의 모든 명령이 끝나야 frame N+1 시작이므로 GPU 측 자원은 동시 접근 X. CPU 가 *기록 단계*에서만 슬롯별로 자기 메모리에 쓰면 됨.
  - CommandList = CommandAllocator + GraphicsCommandList — CPU 가 Reset 후 기록. GPU 가 실행 중이면 Reset 불가 (이 단계의 핵심 동기화 포인트).
  - ConstantBuffer = Upload heap + persistent map. CPU 가 매 프레임 memcpy. GPU 가 이전 프레임 데이터 읽는 동안 같은 메모리 덮어쓰면 race.
- **포기한 것**: ConstantBuffer 가 1개에서 2개로 — 메모리 약간 (각 256B 정렬). 향후 `ConstantBuffer` 가 내부적으로 N 슬롯 링 버퍼로 발전 가능.

### 13-3. CommandQueue API 변경 없음 — main 이 frameIndex 보유
- **결정**: 기존 `Signal()`/`WaitForFenceValue()` 가 이미 노출되어 있음. CommandQueue 가 프레임 슬롯을 *모름*. main 이 frameFenceValues[N] 배열 보관.
- **후보**: CommandQueue 에 frame slot 개념 흡수 — `SubmitAndSignal(slot)`, `WaitForSlot(slot)` 등.
- **선택 이유**: CommandQueue 의 역할은 *큐 + 펜스 동기화* 단위. 프레임 슬롯은 *애플리케이션* 의 cadence. 분리가 단일 책임에 부합.
- **포기한 것**: main 이 frameIndex 관리 코드 약간 더 보유. 향후 Renderer 클래스로 추출 가능 시점.

### 13-4. frameFenceValues 초기값 0 = "미사용"
- **결정**: `std::array<uint64, kFrameCount> frameFenceValues{}` 모두 0 초기화. WaitForFenceValue(0) 호출 가드.
  ```cpp
  if (frameFenceValues[frameIndex] != 0) {
      commandQueue.WaitForFenceValue(frameFenceValues[frameIndex]);
  }
  ```
- **이유**: 첫 N 프레임은 슬롯이 아직 사용된 적 없음 → wait skip. CommandQueue::Signal 은 1부터 시작 (m_fenceValue+1) 이라 0 은 안 옴 — sentinel 안전.
- **대체**: `optional<uint64>` 도 가능하지만 비용↑ 가독성 변화 적음.

### 13-5. 리사이즈 분기 — FlushGpu 유지 + 값 reset
- **결정**: 리사이즈 시 `commandQueue.FlushGpu()` 후 `for (auto& v : frameFenceValues) v = 0;`
- **이유**: 리사이즈는 모든 in-flight 자원(백버퍼)을 무효화하므로 모든 슬롯 완료 보장 필요. 그 후 fenceValues 를 0 으로 reset 해서 다시 첫 N 프레임처럼 wait skip. (안 reset 해도 동작은 하지만 의미 명확화.)

### 13-6. Texture 업로드는 cmdLists[0] 빌려쓰기
- **결정**: 메인 루프 시작 *전* 의 1회성 업로드는 `*cmdLists[0]` 를 인자로 전달. Texture 내부에서 list.Reset/기록/Close/Execute/FlushGpu.
- **이유**: 별도 업로드 큐/리스트를 더 만들 필요 없음. 메인 루프 시작 시점엔 cmdLists[0] 가 Close 상태이고 frameFenceValues[0]=0 라 첫 반복에서 즉시 Reset 가능.
- **포기한 것**: 비동기 텍스처 업로드. 향후 Copy 큐 + 별도 list 풀로 발전 시 분리.

## 4. 작업 내용

### 4-1. main.cpp 슬롯 자원 선언
```cpp
constexpr std::uint32_t kFrameCount = engine::render::SwapChain::kBackBufferCount;

std::array<std::unique_ptr<engine::render::CommandList>, kFrameCount> cmdLists;
for (std::uint32_t i = 0; i < kFrameCount; ++i) {
    cmdLists[i] = std::make_unique<engine::render::CommandList>(device);
}

std::array<std::unique_ptr<engine::render::ConstantBuffer>, kFrameCount> frameCBs;
for (std::uint32_t i = 0; i < kFrameCount; ++i) {
    frameCBs[i] = std::make_unique<engine::render::ConstantBuffer>(
        device, sizeof(FrameConstants));
}

std::array<std::uint64_t, kFrameCount> frameFenceValues{};
std::uint32_t frameIndex = 0;
```

- `CommandList` 와 `ConstantBuffer` 가 모두 비이동 비복사 → `std::array<T,N>` 직접 사용 불가, `unique_ptr` 슬롯에 채움.

### 4-2. 매 프레임 동기화 시퀀스
```cpp
// 이 슬롯이 직전에 제출한 값이 GPU 에서 완료될 때까지만 대기.
if (frameFenceValues[frameIndex] != 0)
{
    commandQueue.WaitForFenceValue(frameFenceValues[frameIndex]);
}

auto& cmdList = *cmdLists[frameIndex];
auto& frameCB = *frameCBs[frameIndex];
frameCB.Update(&cb, sizeof(cb));

cmdList.Reset();
// record (barrier/clear/setup/draw/barrier)
cmdList.Close();
commandQueue.Execute(cmdList);
swapChain.Present();

// 이 슬롯의 직전 fence value 갱신 — 다음 회전 때 wait 기준.
frameFenceValues[frameIndex] = commandQueue.Signal();
frameIndex = (frameIndex + 1) % kFrameCount;
```

### 4-3. Texture 생성 호출 갱신
```cpp
engine::render::Texture albedoTex(
    device, commandQueue, *cmdLists[0],
    checker.data(), kTexW, kTexH);
```

### 4-4. 종료 정리
```cpp
// 메인 루프 종료 후 모든 in-flight 완료 대기 (소멸 순서 안전).
commandQueue.FlushGpu();
```

기존 그대로 유지. 단일 인스턴스 cmdList 가 없으므로 별도 작업 X.

## 5. 마주친 문제와 해결 ⚠ 필수 — 빠짐없이 기록

### 문제 1 — 비이동 클래스를 std::array 에 못 넣음
- **문제**: `std::array<CommandList, kFrameCount>` 컴파일 실패 — CommandList 가 `delete` 된 이동/복사 + default ctor 없음.
- **원인**: `std::array<T,N>` 은 T 의 default-init 또는 aggregate init 요구. 두 경로 모두 막힘 (CommandList 는 `explicit CommandList(Device&)` 만 보유).
- **해결**: `std::array<std::unique_ptr<CommandList>, kFrameCount>` 로 슬롯에 in-place 생성. ConstantBuffer 도 동일 패턴.
- **교훈**: "비이동 + ctor 가 비-default 인자" 클래스를 N개 컨테이너에 넣을 땐 unique_ptr 슬롯. vector::emplace_back(reserve 선행) 도 가능하지만 unique_ptr 가 더 명시적.

### 문제 2 — 매 프레임 FlushGpu 제거 시 자원 race 분석
- **문제**: FlushGpu 제거 후 어떤 자원에 race 가 생길지 정리해야 안전 통과.
- **분석 결과**:
  - **race 발생**: CommandAllocator (Reset 시 GPU 사용 중이면 충돌), Persistent-mapped ConstantBuffer (CPU memcpy ↔ GPU read).
  - **race 없음**: DepthStencilBuffer, Mesh VB/IB, Texture, PSO/RootSig, RTV/DSV/SRV 힙 — GPU 큐가 순차 실행이라 frame N 종료 후 N+1 시작.
  - **SwapChain 백버퍼**: 자연 회전. frame N 이 backbuffer[N%2] 를 RT 로 쓰는 동안 GPU 는 frame N-1 의 backbuffer[(N-1)%2] 를 Present 중 — 다른 자원이라 안전.
- **해결**: race 대상 2종만 슬롯 N개로 분리.
- **교훈**: DX12 의 N-frame in-flight 핵심은 "GPU 가 같은 시점에 *여러* 프레임을 동시 실행하는 것이 아니라, CPU 가 다음 프레임 *기록*을 미리 시작하는 것". 따라서 race 는 *CPU 측 자원*에만 발생.

### 문제 3 — frameIndex 와 SwapChain 의 currentIndex 동기 확인
- **문제**: 우리 `frameIndex = (frameIndex + 1) % kFrameCount;` 와 `swapChain.CurrentBackBuffer()` 가 같은 자원 슬롯을 가리키는지.
- **분석**:
  - 시작 시점: `swapChain.m_currentIndex = GetCurrentBackBufferIndex() = 0`, 우리 `frameIndex = 0`. → 동일.
  - 첫 Present 후 swapChain.m_currentIndex 자동 회전 → 1. 우리도 `(0+1)%2 = 1`. → 동일.
- **결론**: cadence 일치. 별도 보정 불필요. 단 *기록 시점* 의 `swapChain.CurrentBackBuffer()` 는 *다음에 그릴* 백버퍼 (Present 직후 회전된 값) — 우리 frameIndex 와 일치.
- **교훈**: 두 인덱스가 같은 모듈로 회전이지만 의미가 다름 (frame 슬롯 vs 백버퍼 슬롯). 우연이 아니라 *백버퍼 인덱스가 frame 슬롯 인덱스의 자연 매핑*이라는 의도된 일치.

### 문제 4 — 리사이즈 후 fence 값 처리
- **문제**: 리사이즈는 FlushGpu 로 모든 in-flight 완료시킴. 그 후 `frameFenceValues` 에 들어있는 *과거* 값들이 의미상 stale.
- **해결**: WaitForFenceValue 가 GetCompletedValue >= value 면 즉시 통과하므로 동작은 OK. 명시적으로 모든 값을 0 으로 reset — 의미 표현 + 첫 N 프레임 wait skip 경로 재진입.
- **교훈**: 코드 동작과 코드 의미가 다를 때, *의미* 를 명시적으로 reset 해두면 향후 변경 시 안전 마진.

## 6. 결과 / 검증

- **빌드**: Debug + Release 둘 다 0 warning / 0 error.
- **기대 효과** (사용자 직접 확인):
  - 회전 부드러움 유지 (혹은 향상).
  - 큰 게임 루프 작업 추가 시 FPS drop 적어짐 (CPU 작업이 GPU 실행과 겹침).
  - 리사이즈 직후 첫 N 프레임은 wait skip — visual artefact 없음.
- **검증 한계**: 본 단계는 작업량이 가벼워 FPS 차이가 측정 가능 수준 X. 효과는 향후 복잡한 씬에서 두드러질 예정.

## 7. AI 협업 메모

- 자원 분류 (race 발생 vs 안전 공유) 가 본 단계의 핵심. Claude 가 각 자원별 GPU 접근 패턴을 분석하고, GPU 큐의 순차 실행 가정을 검증한 후 분리 대상 좁힘.
- CommandQueue 의 기존 Signal/WaitForFenceValue API 가 이미 충분히 일반적이라 변경 없이 재활용 — *기존 추상화의 좋은 단위가 보존됨*을 확인.

## 8. 다음 단계

누적 TODO 순서:
- DXGI_PRESENT_ALLOW_TEARING — VRR 환경 V-Sync OFF 일관성.
- MTL 머티리얼 — 면별 색·텍스처.
- OBJ n-gon 자동 삼각형화.

또한:
- ConstantBuffer 내부 N 슬롯 링 버퍼화 — 현재 인스턴스 N개 패턴을 클래스 내부로 흡수.
- N=3 in-flight 실험 (입력 지연 vs throughput 트레이드오프).

## 9. PPT 재료로 쓸 만한 포인트

- "DX12 N-frame in-flight 모델 — race 자원 분류표 (CPU측 N개 vs GPU 공유)"
- "FlushGpu 1줄 제거 → 동기화 모델 전환 (CPU/GPU 직렬화 → 파이프라이닝)"
- "기존 CommandQueue API 재활용 — 추상화 단위가 잘 설계되면 클라이언트 변경만으로 모델 교체 가능"
