# 14. Phase 3+ — DXGI_PRESENT_ALLOW_TEARING (VRR V-Sync OFF) 📺

- **날짜**: 2026-05-15
- **관련 커밋**: (이 단계 커밋)
- **소요 시간**: 약 20분
- **단계**: Phase 3 누적 TODO 처리 (3/5)

---

## 1. 목표

VRR(가변 리프레시) 모니터에서 V-Sync OFF 가 의도대로 동작하도록 `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING` + `DXGI_PRESENT_ALLOW_TEARING` 일관 적용. 미지원 환경에서는 조용히 비활성으로 폴백.

## 2. 사전 컨텍스트

직전 단계까지: `swapChain.Present(0, 0)` — SyncInterval=0 으로 tearing 의도하지만 PresentFlags 0 이라 VRR 환경에서 OS 가 강제로 V-Sync 와 합성. FPS cap 발생.

또한 `DXGI_SWAP_CHAIN_DESC1::Flags = 0` 이라 ALLOW_TEARING 을 Present 에 넣어도 거부 (생성 시 flag 가 없으면 Present 도 불가).

## 3. 결정과 트레이드오프

### 14-1. 지원 쿼리는 Device 가, 적용은 SwapChain 이
- **결정**: `Device::SupportsTearing()` — 생성자에서 `IDXGIFactory6::CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING)` 한 번 조회 후 bool 저장. SwapChain 이 ctor 에서 Device 에 조회 → 자기 멤버에 캐시 → 생성 desc.Flags + Present flag 양쪽에 동일 적용.
- **후보**:
  - A) SwapChain 이 직접 CheckFeatureSupport. (역할 분산 ↓)
  - B) Device 가 결정.
- **선택 이유**: B — 기능 지원은 *하드웨어/드라이버 속성*. Device 가 자기 어댑터의 능력을 알게 두는 게 자연스러움. 향후 다른 기능 (MeshShader 등) 도 같은 패턴.
- **포기한 것**: 없음.

### 14-2. 생성 desc.Flags 와 Present flag 의 1대1 일관성
- **결정**: SwapChain 이 `m_allowTearing` 멤버 1개로 둘을 묶어 일치 보장.
- **이유**: DXGI 규약 — `SWAP_CHAIN_FLAG_ALLOW_TEARING` 없이 생성된 스왑체인에 `PRESENT_ALLOW_TEARING` 호출 시 `DXGI_ERROR_INVALID_CALL`. 따라서 둘은 항상 짝.
- **리사이즈 시 보존**: `ResizeBuffers` 가 `desc.Flags` 인자를 받음. 우리 코드는 `m_swapChain->GetDesc1(&desc); ... ResizeBuffers(..., desc.Flags)` 로 기존 flag 가 자동 보존. 추가 작업 없음.

### 14-3. 미지원 환경 — 조용한 폴백
- **결정**: CheckFeatureSupport 실패 / FALSE 반환 시 `m_supportsTearing = false`. SwapChain 도 그 값을 캐시 후 flag 모두 0.
- **이유**: 일부 WARP / 구형 GPU / Windows 7 에선 미지원. 사용자가 옵션 켰는데 갑자기 에러나는 것보다 자동 폴백이 안전.
- **사용자 옵션화**: 향후 "Force V-Sync" 같은 UI 옵션을 별도 멤버로 추가하면 됨. 현재 단계는 "지원하면 켠다" 정책.

### 14-4. SyncInterval 은 그대로 0
- **결정**: `Present(0, ...)` 유지.
- **이유**: ALLOW_TEARING 은 SyncInterval=0 일 때만 효과. SyncInterval ≥ 1 이면 V-Sync 가 무조건 적용되어 flag 무의미.

## 4. 작업 내용

### 4-1. Device 확장
- 위치: [Engine/render/Device.h](../Engine/render/Device.h), [.cpp](../Engine/render/Device.cpp)
- `bool SupportsTearing() const noexcept` 공개 메서드 + private `QueryTearingSupport()` 호출 추가.

```cpp
void Device::QueryTearingSupport()
{
    BOOL allowTearing = FALSE;
    const HRESULT hr = m_factory->CheckFeatureSupport(
        DXGI_FEATURE_PRESENT_ALLOW_TEARING,
        &allowTearing,
        sizeof(allowTearing));
    m_supportsTearing = SUCCEEDED(hr) && (allowTearing != FALSE);

    engine::core::LogInfo(m_supportsTearing
        ? L"[render] DXGI tearing support: yes\n"
        : L"[render] DXGI tearing support: no\n");
}
```

생성자 시퀀스: `EnableDebugLayer → CreateFactory → SelectAdapter → CreateDevice → ConfigureInfoQueue → QueryTearingSupport`.

### 4-2. SwapChain 확장
- 위치: [Engine/render/SwapChain.h](../Engine/render/SwapChain.h), [.cpp](../Engine/render/SwapChain.cpp)
- ctor: 멤버 이니셜라이저로 `m_allowTearing(device.SupportsTearing())`.
- desc.Flags: `m_allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0`.
- Present:

```cpp
const UINT flags = m_allowTearing ? DXGI_PRESENT_ALLOW_TEARING : 0u;
ThrowIfFailed(m_swapChain->Present(0, flags), "IDXGISwapChain3::Present");
```

- Resize: 기존 코드가 `GetDesc1(&desc); ResizeBuffers(..., desc.Flags)` 라 별도 변경 불필요.

## 5. 마주친 문제와 해결 ⚠ 필수 — 빠짐없이 기록

### 문제 1 — 생성·Present 두 곳의 flag 일관성
- **문제**: 처음엔 Present 만 `DXGI_PRESENT_ALLOW_TEARING` 으로 켜는 게 떠올랐는데, DXGI 가 `DXGI_ERROR_INVALID_CALL` 반환.
- **원인**: 스왑체인 생성 desc.Flags 에 `SWAP_CHAIN_FLAG_ALLOW_TEARING` 없으면 Present flag 도 거부됨. DXGI 규약.
- **해결**: 한 멤버 `m_allowTearing` 으로 두 위치를 묶어 자동 일관성 보장.
- **교훈**: DXGI 의 *생성 옵션* 과 *호출 옵션* 이 짝을 이루는 패턴은 흔함 — 한 곳에서 진실의 원천(single source of truth) 유지.

### 문제 2 — 리사이즈 시 flag 보존 우려
- **문제**: ResizeBuffers 가 새 desc.Flags 를 받음. 매번 0 으로 호출하면 ALLOW_TEARING 상실?
- **확인**: 기존 SwapChain::Resize 코드가 이미 `GetDesc1(&desc)` 로 현재 desc 를 읽어 `desc.Flags` 를 그대로 전달. → ALLOW_TEARING flag 보존. 추가 작업 없음.
- **교훈**: 이전 단계(12)에서 *기존 desc 보존* 패턴을 골라둔 덕에 이번 단계가 무료. 작은 결정이 누적 이익.

### 문제 3 — VRR 모니터 부재 시 시각 효과 확인 불가
- **문제**: 본 개발 PC 가 일반 60Hz 모니터인 경우 ALLOW_TEARING 의 효과(고 FPS 시 화면 분할 tearing)가 직접 안 보임.
- **현 단계 검증**: 빌드 + Device 로그 `DXGI tearing support: yes/no` 출력으로 코드 경로만 확인. 시각 효과는 VRR 모니터 보유 시 확인.
- **교훈**: 일부 기능은 환경 의존. 로그 + 코드 경로 검증으로 분리.

## 6. 결과 / 검증

- **빌드**: Debug + Release 둘 다 0 warning / 0 error.
- **런타임 로그**: `[render] DXGI tearing support: yes` (또는 no) 로 지원 여부 확인.
- **추가 검증** (VRR 환경 보유 시): 게임 루프가 100+ FPS 도달할 때 화면 분할 tearing 관찰. VRR 모니터에선 동적 리프레시 따라감.

## 7. AI 협업 메모

- Device → SwapChain 으로 기능 지원 값 전달하는 *thin pull* 패턴. Device 가 자기 어댑터 능력을 모듈로 노출하고 클라이언트가 조회.
- 작은 단계라 단일 커밋. devlog 도 짧게.

## 8. 다음 단계

누적 TODO 남은 항목:
- **MTL 머티리얼** — 면별 색 + (선택) 면별 텍스처 / 알베도. OBJ 의 `usemtl` 라인 처리.
- **OBJ n-gon 자동 삼각형화** — 현재 삼각형 가정. 4 이상 fan triangulation.

## 9. PPT 재료로 쓸 만한 포인트

- "VRR 모니터 시대의 DXGI Present — 생성 옵션과 호출 옵션의 일관성 패턴"
- "Device 가 어댑터 능력의 단일 진실 원천 — 클라이언트는 조회만"
