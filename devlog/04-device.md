# 04. Phase 1C — `engine::render::Device` 초기화

- **날짜**: 2026-05-13
- **관련 커밋**: `94adb96`
- **소요 시간**: 약 1시간 20분
- **단계**: Phase 1C — Render Device

---

## 1. 목표

D3D12 Device 와 그 필수 의존(DXGI Factory, 어댑터)을 OOP 캡슐화하여 RAII 로 초기화한다. 멀티 GPU 환경에서 고성능 GPU 결정적 선택, Debug 빌드에서 검증·진단 자동 활성화까지 포함.

본 단계의 산출물은 **렌더 호출 없이 디바이스 생성에만 집중**. SwapChain·CommandQueue·Allocator·Fence·DescriptorHeap 은 Phase 1D 이후 별도 단계.

## 2. 사전 컨텍스트

- 직전 단계: [Phase 1B Window 클래스](03-window-class.md). 1280×720 윈도우(임시 어두운 회색 배경) 정상 동작.
- 본 단계가 **첫 DX12 코드** 등장 → `dx12-reviewer` 호출 워크플로 첫 적용 예정.
- 동시에 OOP 측면 리뷰는 두 번째 적용 (Window 에 이어).

## 3. 결정과 트레이드오프

### 결정 1 — Device 가 Factory + Adapter 까지 소유 (umbrella SRP)
- **후보**: 별도 `Factory` / `Adapter` 클래스 / Device 가 umbrella 로 모두 소유
- **선택**: **umbrella**
- **이유**: 세 객체는 생성 순서가 엄격히 결정적이고 라이프타임이 동일 (Factory → Adapter → Device, 모두 디바이스 수명 동안 유지). 분리 시 클래스 3개 + 의존 주입 + 보일러플레이트 증가. SRP 약간 양보하되 응집도가 높음.
- **포기한 것**: 멀티 어댑터 시나리오에서 Factory 공유 — 본 프로젝트엔 비필요.

### 결정 2 — IDXGIFactory6 직접 요청 (vs Factory4 + QueryInterface 폴백)
- **후보**: ① Factory4 유지하고 어댑터 선택만 Factory6 로 query ② Factory6 멤버로 직접
- **선택**: **Factory6 멤버 직접**
- **이유**: SDK 10.0.26100 (Win11 24H2 헤더) + Windows 10 1803+ 가 사실상 보장. Factory6 의 `EnumAdapterByGpuPreference` 가 멀티 GPU 환경에서 결정적 선택을 제공 (노트북 iGPU/dGPU 자동 dGPU 선택). 폴백 코드의 복잡도 < 멀티 GPU 정확성의 이득.
- **포기한 것**: Windows 10 1709 이하 호환. 현 타겟엔 비고려.

### 결정 3 — 어댑터 호환성 드라이런 (`D3D12CreateDevice(adapter, FL, ..., nullptr)`)
- 실제 디바이스 생성 전, nullptr 디바이스 인수로 `D3D12CreateDevice` 호출해 FL 12.0 호환만 확인.
- 호환 실패 어댑터는 건너뛰고 다음 후보 시도. 모든 하드웨어 실패 시 WARP 폴백.
- **이유**: 정석 패턴. 빌트인 호환성 체크 API 가 따로 없어 드라이런이 표준.

### 결정 4 — 헤더 의존성 차단 (전방 선언)
- **이전**: `Device.h` 에 `Windows.h`, `d3d12.h`, `dxgi1_4.h` 모두 include → Device 를 include 한 TU 가 DX 심볼로 오염.
- **변경 (OOP 리뷰 반영)**: `Device.h` 에서 위 모든 헤더 제거. `<wrl/client.h>` 만 유지(ComPtr 멤버 위해). 인터페이스는 전역 네임스페이스 전방선언:
  ```cpp
  struct IDXGIFactory6;
  struct IDXGIAdapter1;
  struct ID3D12Device;
  struct ID3D12InfoQueue;
  ```
- **조건**: `ComPtr<incomplete type>` 가 작동하려면 ComPtr destructor 가 완전 타입에서 호출돼야 함 → `~Device()` 정의를 `.cpp` 에 둠 (이미 그러함).

### 결정 5 — WIN32_LEAN_AND_MEAN / NOMINMAX 전역 정의
- Phase 1B 에서 `Window.h` / `main.cpp` 가 개별로 `#define WIN32_LEAN_AND_MEAN` 사용 → 헤더 영향 범위가 TU 별로 다른 비대칭.
- vcxproj `PreprocessorDefinitions` 에 `WIN32_LEAN_AND_MEAN;NOMINMAX` 전역 추가 → 일관성 확보. 개별 `#define` 제거.

### 결정 6 — Info Queue: Corruption/Error break + INFO 필터링
- Corruption/Error severity 는 디버거에서 즉시 break (자동 회귀 캐치).
- Warning 은 break 안 함 (개발 중 알림용).
- INFO 메시지는 `DenyList` 로 저장 필터링 — 디버그 출력 노이즈 차단.

## 4. 작업 내용

### 4-1. 클래스 인터페이스 (`Device.h`)
```cpp
struct IDXGIFactory6;
struct IDXGIAdapter1;
struct ID3D12Device;
struct ID3D12InfoQueue;

namespace engine::render {
class Device final {
public:
    Device(); ~Device();
    Device(const Device&) = delete; Device& operator=(const Device&) = delete;
    Device(Device&&) = delete;       Device& operator=(Device&&) = delete;
private:
    static void EnableDebugLayer();
    void CreateFactory(); void SelectAdapter();
    void CreateDevice(); void ConfigureInfoQueue();
    Microsoft::WRL::ComPtr<IDXGIFactory6> m_factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> m_adapter;
    Microsoft::WRL::ComPtr<ID3D12Device>  m_device;
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> m_infoQueue;
};
}
```

### 4-2. 생성자 흐름
```
EnableDebugLayer (D3D12)         (Debug only, 실패 시 경고만)
   ↓
CreateFactory (DXGI Factory6)    (Debug 빌드 시 DXGI 디버그 플래그)
   ↓
SelectAdapter                    (EnumAdapterByGpuPreference + FL 12.0 드라이런, WARP 폴백)
   ↓
CreateDevice (D3D12Device)
   ↓
ConfigureInfoQueue               (Debug only, Corruption/Error break + INFO DenyList)
```

### 4-3. 어댑터 선택 (`SelectAdapter`)
```cpp
for (UINT i = 0;; ++i) {
    ComPtr<IDXGIAdapter1> candidate;
    if (m_factory->EnumAdapterByGpuPreference(
            i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
            IID_PPV_ARGS(&candidate)) == DXGI_ERROR_NOT_FOUND) break;
    if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
    if (SUCCEEDED(D3D12CreateDevice(candidate.Get(), FL_12_0, __uuidof(ID3D12Device), nullptr))) {
        m_adapter = candidate; return;
    }
}
// WARP 폴백 (소프트웨어 래스터라이저)
m_factory->EnumWarpAdapter(...);
```

### 4-4. Info Queue 필터링
```cpp
m_infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
m_infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR,      TRUE);
D3D12_MESSAGE_SEVERITY denied[] = { D3D12_MESSAGE_SEVERITY_INFO };
D3D12_INFO_QUEUE_FILTER filter{};
filter.DenyList.NumSeverities = 1;
filter.DenyList.pSeverityList = denied;
m_infoQueue->PushStorageFilter(&filter);
```

### 4-5. `main.cpp` 갱신
```cpp
engine::platform::Window window(1280, 720, L"portfolio_engine");
engine::render::Device   device;            // ← 추가
while (window.IsOpen()) { window.PumpMessages(); }
```

## 5. 마주친 문제와 해결 ⚠ 필수 — 빠짐없이 기록

### 문제 1 — 헤더에서 DX 의존성 누수 (잘못된 가정, 리뷰 발견)
- **문제**: 초안 `Device.h` 가 `Windows.h`, `d3d12.h`, `dxgi1_4.h` 를 모두 노출 → Device 를 include 하는 모든 TU(현재 `main.cpp` 포함)가 DX 심볼 전체로 오염.
- **원인**: "어차피 .cpp 에서도 쓰니까 헤더에 두자" 식의 안일한 가정.
- **해결**: `Device.h` 에서 위 헤더 제거. ComPtr 위해 `<wrl/client.h>` 만 유지, 인터페이스는 전역 네임스페이스 전방선언(`struct IDXGIFactory6;` 등). ComPtr<incomplete>은 destructor 가 .cpp 에 정의되어 있는 한 안전.
- **교훈**: 헤더는 클라이언트가 "꼭 알아야 하는 것"만. **YAGNI + 의존성 누수 차단**. 큰 OS/벤더 헤더는 절대 헤더에 두지 않는다.

### 문제 2 — `EnumAdapters1` 의 순서 가정 (잘못된 가정, 리뷰 발견)
- **문제**: 초안에서 `EnumAdapters1(i, ...)` 로 어댑터 열거 후 첫 호환 어댑터 선택.
- **원인**: "어차피 첫 번째가 GPU 1번이겠지" 식의 잘못된 가정. 실제로는 perf 순서 보장 없음. 특히 노트북 iGPU+dGPU 환경에서 iGPU 가 먼저 나올 위험.
- **해결**: `IDXGIFactory6::EnumAdapterByGpuPreference(HIGH_PERFORMANCE)` 로 교체. 멀티 GPU 환경에서 dGPU 결정적 선택. 헤더는 `dxgi1_6.h` 로 승격.
- **교훈**: 열거 API 는 항상 **순서 명세를 문서로 확인**. "그럴듯한 디폴트" 가정 금지. DX 의 perf-preferred 어댑터 API 가 따로 있음을 기억.

### 문제 3 — `WIN32_LEAN_AND_MEAN` 헤더 정의의 비대칭 (Phase 1B 잘못된 가정의 노출)
- **문제**: Phase 1B 에서 `Window.h` 가 `#define WIN32_LEAN_AND_MEAN` 후 `#include <Windows.h>` 패턴 사용. Phase 1C 시작하며 `Device.h` 도 같은 패턴으로 두려다 리뷰가 지적.
- **원인**: 매크로 정의가 TU 의 첫 `Windows.h` include 시점에 보여야 효과 발생. 헤더에 두면 이전 include 순서에 따라 효과 비결정적.
- **해결**: vcxproj `PreprocessorDefinitions` 에 `WIN32_LEAN_AND_MEAN;NOMINMAX` 추가 (Debug/Release 양쪽). 개별 파일의 `#define` 제거. 이로써 모든 TU 가 동일한 매크로 환경에서 컴파일.
- **교훈**: 사전 매크로는 **빌드 시스템 수준**에서 통일. 헤더에 두지 말 것. PCH 도 동일한 역할. NOMINMAX 도 함께 정의해 `std::min/max` 충돌 사전 차단.

### 문제 4 — `ComPtr::GetAddressOf` 의 누수 리스크 (잘못된 가정, 리뷰 발견)
- **문제**: `IID_PPV_ARGS(m_factory.GetAddressOf())` 식 호출. 멤버에 이미 객체가 있으면 그 객체가 누수됨.
- **원인**: 현재는 생성자에서 1회만 호출 → 멤버는 nullptr 상태라 실해는 없음. 하지만 방어적 코딩 부재.
- **해결**: 멤버 ComPtr 채우는 호출에 `ReleaseAndGetAddressOf()` 사용 — 기존 포인터 있으면 Release 후 주소 반환. 로컬 ComPtr(루프 변수)은 `GetAddressOf()` 유지(매번 새 인스턴스라 안전).
- **교훈**: ComPtr 멤버에 결과를 직접 받는 호출은 항상 `ReleaseAndGetAddressOf`. 미래에 호출이 한 번 더 생겨도 자동 안전.

### 문제 5 — Info Queue INFO severity 노이즈 (잘못된 가정, 리뷰 발견)
- **문제**: 초안에서 `SetBreakOnSeverity` 만 설정. `D3D12_MESSAGE_SEVERITY_INFO` 가 디버그 출력 창에 쏟아져 신호:노이즈 비율 악화 가능.
- **원인**: "INFO 는 어차피 가벼우니 두자" 가정. 실제 DX12 INFO 메시지는 매우 다수 — 셰이더 컴파일, 리소스 생성 등 모든 작업.
- **해결**: `D3D12_INFO_QUEUE_FILTER` 의 `DenyList.pSeverityList` 에 `INFO` 추가 후 `PushStorageFilter`. WARNING 이상만 출력.
- **교훈**: 디버그 출력은 **신호:노이즈**가 생명. 첫 셋업부터 필터링 정책 명시. 필요하면 PopStorageFilter 로 임시 해제.

### (이번 세션 추가 실수 없음)
- 빌드는 한 번에 통과 (review 반영 후 재빌드 1회).
- 한글 인코딩/타이포/명령 실수 없음.
- 커밋 직전 점검 체크리스트 통과 (무관 파일 X, 임시 디버그 X, BOM 적용 — 코드는 미적용, .md만).

## 6. 결과 / 검증

### 빌드
| 구성 | 결과 | 비고 |
|---|---|---|
| Debug\|x64 | ✅ | 경고 0, 에러 0 |
| Release\|x64 | ✅ | 경고 0, 에러 0 |

### 실행
- 윈도우 표시 → Device 생성 완료 → 2초 정상 동작 → `CloseMainWindow` → ExitCode 0
- VS 디버거 부착 시 출력 창에 다음 라인이 보일 것 (디버그 빌드):
  - `[render] D3D12 debug layer enabled`
  - `[render] Adapter: <GPU 이름> (VRAM <X> MB, VendorId 0x..., DeviceId 0x...)`
  - `[render] D3D12 device created`
  - `[render] Info queue configured (break on Corruption/Error, INFO filtered)`

스크린샷 자리표시자:
- (TODO) VS 출력 창의 [render] 로그 캡처
- (TODO) PIX/Nsight 캡처로 D3D12Device 생성 확인
- (TODO) 솔루션 익스플로러 트리 (`engine/render/Device.{h,cpp}` 추가)

## 7. AI 협업 메모

본 단계가 **두 서브에이전트 병렬 호출** 첫 실행 — 패턴 A(병렬 조사)의 변형.

- 한 메시지에 `oop-reviewer` 와 `dx12-reviewer` 두 prompt 를 `general-purpose` 로 동시 호출.
- 응답 시간: 각 ~22~26초. 직렬 호출이면 합산 ~50초 — 병렬이라 약 절반 단축.
- **비겹침 결과**: 두 리뷰의 지적이 거의 겹치지 않음.
  - OOP 리뷰: 헤더 의존성, LogAdapter SRP, OutputDebugString 의존성 방향
  - DX12 리뷰: EnumAdapterByGpuPreference, ReleaseAndGetAddressOf, Info Queue 필터, WIN32_LEAN_AND_MEAN 헤더 위치
- 한 리뷰만으로는 발견 못했을 지점을 다른 리뷰가 잡음 → 병렬 멀티 시각이 단일 시각 합산보다 큼.

반영 정책:
- **즉시 반영**: 헤더 의존성 차단, Factory6, ReleaseAndGetAddressOf, Info Queue 필터, WIN32_LEAN_AND_MEAN 전역.
- **TODO 로 기록**: LogAdapter SRP, OutputDebugString 직접 호출 — 로깅 시스템 도입 시 일괄 마이그레이션 (현 단계 분리는 premature).
- **유지**: move 4종 delete (의도된 선택), EnableDebugLayer만 static (비대칭 자연스러움).

## 8. 다음 단계

- **Phase 1D — `engine::render::SwapChain` + 첫 클리어**
  - `IDXGISwapChain3` 생성 (Window HWND 와 결합 — Window 에 friend 또는 좁힌 어댑터 도입)
  - `ID3D12CommandQueue` 생성 (Direct queue)
  - Back buffer RTV 생성 (RTV 디스크립터 힙)
  - Frame allocator/list 셋업
  - 첫 클리어 색상 적용 (예: dark slate `(0.05, 0.07, 0.10, 1.0)`) → `Present`
  - 회색 임시 배경(`DKGRAY_BRUSH`) 을 `nullptr` 로 복원 — 렌더러가 모든 픽셀 책임

미뤄둔 항목:
- 로깅 시스템 (`LogAdapter`/`OutputDebugString` 마이그레이션 대상)
- Window→SwapChain HWND 접근 패턴 (friend vs NativeHandle 어댑터) — Phase 1D 첫 결정

## 9. PPT 재료로 쓸 만한 포인트

- **"DXGI Factory6 + EnumAdapterByGpuPreference"** 슬라이드 (멀티 GPU 환경 dGPU 선택 다이어그램, EnumAdapters1 의 함정 비교)
- **"D3D12 디버그 레이어 + Info Queue 셋업 순서"** 슬라이드 (정석 시퀀스 다이어그램)
- **"헤더 위생 — 전방선언 패턴"** 슬라이드 (Device.h Before/After, 컴파일 시간/심볼 누출 비교)
- **"병렬 AI 리뷰의 비겹침 효과"** 슬라이드 (OOP/DX12 두 시각에서 5건의 서로 다른 지적, 단일 시각 한계 시각화)
- **"잘못된 가정 5건과 교훈"** 슬라이드 (어댑터 순서, 헤더 의존성, 매크로 위치, ComPtr 누수, 디버그 노이즈)
