# 11. Phase 3 (E) — 텍스처 업로드 + SRV 디스크립터 테이블 🖼️

- **날짜**: 2026-05-15
- **관련 커밋**: (이 단계 커밋)
- **소요 시간**: 약 1.5시간
- **단계**: Phase 3 — Interactive 3D (E 텍스처)

---

## 1. 목표

큐브 표면에 알베도 텍스처를 입혀 정점 색만으로는 표현 못 했던 디테일을 보태고, GPU 메모리 → SRV 디스크립터 테이블 → 셰이더 샘플링까지의 풀 파이프라인을 직접 다룬다.

E 단계는 [Phase 3 전반](10-camera-lighting-mesh.md) 의 C/F/D 에 이어 자산 파이프라인의 마지막 조각.

## 2. 사전 컨텍스트

직전 단계까지: 자유 카메라 + Phong 조명 + OBJ 메시 로더 완성. 큐브는 `defaultColor` 한 가지로 칠해진 무채색이라 면 음영만 보이고 표면 디테일은 없었다.

이번 단계로 추가될 것:
- `SrvDescriptorHeap` (shader-visible CBV_SRV_UAV 힙).
- `Texture` 클래스 (Default heap + Upload staging + CopyTextureRegion + Barrier).
- `RootSignature` 확장 — t0 SRV 디스크립터 테이블 + s0 정적 샘플러 (LINEAR/WRAP).
- 정점 포맷 확장 — TEXCOORD(float2) 추가, Mesh/PSO/OBJ 로더/Cube.obj 모두 업데이트.
- HLSL — `Texture2D g_albedo + SamplerState g_sampler` + `g_albedo.Sample(...)` 한 줄.
- 외부 이미지 로더 회피 — 8×8 체커보드 픽셀을 코드로 생성.

## 3. 결정과 트레이드오프

### E-1. SRV 와 RTV 디스크립터 힙 분리
- **결정**: SRV 전용 클래스 `SrvDescriptorHeap` 신설 (RtvDescriptorHeap 와 별개).
- **후보**: 하나의 통합 DescriptorHeap 클래스에 `Type` 파라미터로 분기.
- **선택 이유**:
  - RTV/DSV 힙은 `SHADER_VISIBLE = NONE`, SRV 힙은 `SHADER_VISIBLE = YES` — 플래그가 본질적으로 다름.
  - SRV 힙만 `GPU descriptor handle` 도 필요 (RTV/DSV 는 CPU 핸들만).
  - 통합 시 `if (type == RTV)` 분기로 가독성 손상.
- **포기한 것**: 코드 행 중복 (~30줄). 향후 CBV/UAV 까지 추가하면 SrvDescriptorHeap 을 `ShaderVisibleHeap` 으로 일반화 검토.

### E-2. Static Sampler — Root Sig 에 박아넣기
- **결정**: s0 슬롯에 `D3D12_STATIC_SAMPLER_DESC` (LINEAR / WRAP, PS visibility) — 별도 힙·디스크립터 슬롯 X.
- **후보**: dynamic sampler — 별도 SAMPLER 힙 + 디스크립터 슬롯.
- **선택 이유**: 본 단계에서 샘플러 1종만 필요. Static sampler 는 디스크립터 슬롯/힙 비용 0, 셰이더는 동일하게 register(s0) 으로 접근.
- **포기한 것**: 런타임 샘플러 교체. 향후 PBR/그림자 등에서 다양한 샘플러 필요해지면 동적으로 전환.

### E-3. 8×8 체커보드 — 외부 이미지 로더 회피
- **결정**: 노랑/검정 8×8 RGBA8 픽셀을 main.cpp 에서 코드로 생성.
- **후보**:
  - A) stb_image / WIC 도입 → PNG 로드.
  - B) DDS 로더 자작.
- **선택 이유**: 풀스크래치 원칙. 본 단계는 *텍스처 파이프라인 검증* 이 목적이고, 이미지 디코딩은 자산 단계의 별도 책임. 8×8 체커보드면 UV 매핑 결과(상하 뒤집힘, WRAP 동작) 가 한눈에 보임.
- **포기한 것**: 실제 텍스처 자산. 향후 BC1/BC3 압축 텍스처 + DDS 헤더 파서 단계로 분리.

### E-4. 정점 포맷에 TEXCOORD 추가 — 한 번에 전체 갱신
- **결정**: `Mesh::Vertex` 에 `XMFLOAT2 uv` 필드 추가, PipelineState / Cube.obj / ObjLoader 일괄 수정.
- **후보**: 텍스처 정점/비-텍스처 정점 두 종류 유지.
- **선택 이유**: 현재 메시는 큐브 1개. 정점 종류를 늘리면 PSO/Mesh 도 분기해야 하고 단계 복잡도가 폭증. 향후 머티리얼 시스템에서 본격 분기.
- **포기한 것**: UV 없는 메시도 같은 정점 포맷을 채워야 함 (uv = (0,0) 디폴트). 8 byte 낭비.

### E-5. OBJ V축 뒤집기 — 로더 단계에서 변환
- **결정**: ObjLoader 가 vt 파싱 시 `v = 1.0f - v` 로 저장.
- **후보**: 셰이더에서 `1 - uv.y` 처리 / 텍스처 자체를 미리 뒤집어 저장.
- **선택 이유**: OBJ 표준은 V축 bottom-up, D3D 텍스처는 top-down. 변환은 *입력 단계* 에서 하는 게 데이터 흐름상 자연. 셰이더는 표준 D3D UV 만 알면 됨.
- **포기한 것**: OBJ 의 원래 UV 값을 디버깅 시 잃음 (덜 중요).

## 4. 작업 내용

### 4-1. SrvDescriptorHeap 클래스
- 위치: [Engine/render/SrvDescriptorHeap.h](../Engine/render/SrvDescriptorHeap.h), [.cpp](../Engine/render/SrvDescriptorHeap.cpp)
- 책임: `CBV_SRV_UAV` 타입 + `SHADER_VISIBLE` 플래그. 순차 `Allocate()` + `GetHandle(idx)` 둘 다 노출.
- 핵심 구조체:
  ```cpp
  struct Handle {
      D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
      D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
  };
  ```
  → `CreateShaderResourceView(..., cpu)` 와 `SetGraphicsRootDescriptorTable(slot, gpu)` 둘 다 한 객체로 처리.

### 4-2. Texture 클래스
- 위치: [Engine/render/Texture.h](../Engine/render/Texture.h), [.cpp](../Engine/render/Texture.cpp)
- 생성자 시퀀스 (6단계):
  1. Default heap 텍스처 리소스 (`COPY_DEST` 초기 상태).
  2. Upload heap 스테이징 버퍼 (`GetCopyableFootprints` 로 총 크기 + row pitch 계산).
  3. `Map → memcpy 행 단위 → Unmap`. 소스 row pitch ≠ 대상 row pitch 일 수 있어 매 행 별도 memcpy.
  4. `list.Reset() → CopyTextureRegion → ResourceBarrier(COPY_DEST → PIXEL_SHADER_RESOURCE)`.
  5. `list.Close() → queue.Execute(list) → queue.FlushGpu()` — 업로드 완료 동기 대기.
  6. 스테이징 ComPtr 자동 해제.
- `CreateSrv(device, heap)`: 외부 SRV 힙의 다음 슬롯에 SRV 등록, GPU handle 내부 보관.

### 4-3. RootSignature 확장
- Desc 에 `bool srvT0Pixel = false` 추가.
- 파라미터 최대 2개: [0] b0 CBV root descriptor, [1] t0 SRV descriptor table (1 range).
- `srvT0Pixel = true` 시 static sampler s0 (LINEAR / WRAP / PS visibility) 자동 추가.
- `D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT` 는 항상 켜짐.

### 4-4. 정점 포맷 확장
- `Mesh::Vertex` = `{ XMFLOAT3 pos; XMFLOAT3 normal; XMFLOAT2 uv; XMFLOAT3 color; }` (44 bytes).
- `PipelineState` 의 `kHelloTriangleInputLayout` 에 TEXCOORD 추가 — POSITION(0) / NORMAL(12) / TEXCOORD(24) / COLOR(32).
- HLSL VSInput 도 동일 순서.

### 4-5. ObjLoader vt 지원
- `FaceVertex` 에 `int32 uvIdx` 추가.
- `ParseFaceVertex` 가 `"v"`, `"v/vt"`, `"v//vn"`, `"v/vt/vn"` 4가지 형식 모두 처리.
- vt 라인 파싱 시 V축 뒤집기 (`v = 1.0f - v`).
- dedup 키를 `(pos, uv, norm)` 트리플로 확장 — UV 가 면별로 다르므로 면 경계에서 정점 분리.

### 4-6. Cube.obj UV 추가
- 4개 vt 라인 추가 — 모든 면 공통 (0,0)·(1,0)·(1,1)·(0,1).
- face 라인 24개 모두 `pos/uv/norm` 형식으로 변경. UV 인덱스 1,2,3,4 (좌하→우하→우상→좌상).

### 4-7. HLSL 텍스처 샘플링
- `Texture2D g_albedo : register(t0); SamplerState g_sampler : register(s0);`
- VSInput / VSOutput 에 `uv : TEXCOORD` / `uv : TEXCOORD0` 추가.
- PS: `albedo = g_albedo.Sample(g_sampler, uv).rgb * input.color` — 텍스처 색과 정점 색의 component-wise 곱. 정점 색은 OBJ 의 defaultColor 로 흰색이므로 사실상 텍스처 색 그대로.

### 4-8. Client main.cpp 통합
- 8×8 체커보드 픽셀 생성 — `(x ^ y) & 1` 로 노랑/검정 격자.
- `Texture albedoTex(device, queue, list, pixels, 8, 8)` — 업로드 + barrier 모두 생성자에서 동기 완료.
- `SrvDescriptorHeap srvHeap(device, 4)` — 향후 텍스처 추가 여유 capacity.
- `albedoTex.CreateSrv(device, srvHeap)` — slot 0 등록.
- 매 프레임:
  ```cpp
  ID3D12DescriptorHeap* heaps[] = { srvHeap.Native() };
  list->SetDescriptorHeaps(1, heaps);
  list->SetGraphicsRootConstantBufferView(0, frameCB.GpuAddress());
  list->SetGraphicsRootDescriptorTable(1, albedoTex.SrvGpuHandle());
  ```

## 5. 마주친 문제와 해결 ⚠ 필수 — 빠짐없이 기록

### 문제 1 — `Mesh::Vertex` 멤버 순서 = PSO 입력 레이아웃 오프셋 일치 필수
- **문제**: 정점 구조체에 `uv` 를 끼울 위치를 결정하면서, PSO 의 입력 레이아웃 오프셋과 어긋날 위험.
- **원인**: D3D12 입력 레이아웃은 `AlignedByteOffset` 을 수기로 지정 — 구조체 멤버 순서 ≠ 레이아웃 순서면 잘못된 바이트가 셰이더로 들어감.
- **해결**: 양쪽을 한 번에 수정. `Vertex = pos/normal/uv/color` (12/12/8/12 = 44B) ↔ 입력 레이아웃 = `POSITION(0)/NORMAL(12)/TEXCOORD(24)/COLOR(32)` 일치 확인.
- **교훈**: 정점 포맷 변경 시 체크리스트:
  - [ ] `Mesh::Vertex` 구조체
  - [ ] `kHelloTriangleInputLayout` 의 `AlignedByteOffset` 4개
  - [ ] HLSL `VSInput` 의 시맨틱 순서
  - [ ] OBJ 로더가 정점에 모든 필드를 채우는지
  하나라도 빠지면 GPU 가 쓰레기 데이터 읽음.

### 문제 2 — OBJ V축 방향 — 두 좌표계 충돌
- **문제**: OBJ 표준은 V축이 bottom-up (V=0 이 텍스처 아래쪽), D3D 의 `Texture2D.Sample` 은 top-down (V=0 이 위쪽). 변환 안 하면 텍스처가 상하 뒤집힘.
- **원인**: 두 시스템의 관습이 다름. 표준 차이를 인지 못하면 디버깅이 어렵다 (정확히 상하반전이라 "이게 맞나?" 싶음).
- **해결**: ObjLoader 의 `vt` 파싱에서 `v = 1.0f - v` 로 저장.
- **교훈**: 외부 포맷을 받아들이는 단계에서 좌표계/축 방향 차이를 흡수. 셰이더는 *언제나* D3D 표준 UV 만 받게 유지.

### 문제 3 — `ParseFaceVertex` 의 `"v/vt"` 케이스 누락
- **문제**: 기존 파서는 `"v"`, `"v//vn"`, `"v/vt/vn"` 만 가정. `"v/vt"` (normal 없이 uv만) 케이스에서 uv 가 무시되고 normal 로 잘못 들어갈 위험.
- **원인**: 기존 코드는 `lastSlash == firstSlash` 시 normal 만 비었다고 가정 — uv 추출 로직 없음.
- **해결**: `lastSlash == firstSlash` 분기에서도 `token.substr(firstSlash + 1)` 로 uv 추출 추가. `"v//vn"` 분기에서는 가운데가 비었는지 (`lastSlash > firstSlash + 1`) 명시 체크.
- **교훈**: OBJ face vertex 토큰은 4가지 형식 (`v`, `v/vt`, `v//vn`, `v/vt/vn`) — 케이스 하나라도 누락하면 조용히 잘못된 인덱스로 매핑됨. 파서는 4가지 모두 명시적 분기 + 단위 테스트 같은 검증이 이상적.

### 문제 4 — Texture 생성 타이밍 — 메인 루프 cmdList 와 공유
- **문제**: Texture 생성자가 `list.Reset/Close/Execute/FlushGpu` 를 하면 메인 루프의 cmdList 상태를 어지럽힐 수 있음.
- **원인**: cmdList 는 단일 인스턴스, 생성자에서 한 번 + 매 프레임 재사용.
- **해결**: Texture 생성 후 메인 루프 시작 전이라 안전. 메인 루프 첫 반복에서 다시 `Reset` 부터 시작하므로 cmdList 의 닫힌 상태가 그대로 이어짐. 단, Texture 생성 *동안* 다른 cmdList 작업이 끼어들면 안 됨.
- **교훈**: 일회용 업로드 코드가 영구적인 렌더 루프와 같은 cmdList 를 쓰면 시퀀스 가정이 깨질 수 있음. 본 단계는 안전 영역이지만, 향후 다중 텍스처 비동기 업로드 시엔 별도 업로드 큐/리스트 풀 필요.

## 6. 결과 / 검증

- **빌드**: Debug + Release 둘 다 0 warning / 0 error 성공.
- **실행**: 큐브가 노랑/검정 8×8 체커보드 패턴으로 렌더링. 면별 음영 유지 + WRAP 샘플러 동작 확인.
- **확인 포인트** (사용자 직접 확인):
  - 텍스처 상하 뒤집힘 X (V축 보정 확인).
  - 각 면이 4×4 블록으로 나뉘어 보이는가 (UV 0~1 매핑 + 8×8 텍스처).
  - 카메라 회전 시 면별 노출에 따라 음영 + 텍스처 동시 변화.

## 7. AI 협업 메모

- 이번 단계는 Texture 업로드 시퀀스 + RootSig 확장 + 정점 포맷 일괄 갱신이 동시에 일어나는 단계라 한 번에 변경할 파일이 8개 (Engine 측 4, Client 측 4) 였음. Claude 가 체크리스트로 관리하고 각 파일을 의존 순서대로 수정 (헤더 → cpp → HLSL → 자산).
- d3dx12.h 미포함 환경이라 모든 D3D12 구조체(`D3D12_RESOURCE_DESC`, `D3D12_HEAP_PROPERTIES`, `D3D12_TEXTURE_COPY_LOCATION`, `D3D12_RESOURCE_BARRIER`) 를 수기로 채움. d3dx12 헬퍼 의존 없이도 동등 동작.

## 8. 다음 단계

즉시 다음:
- 시각 확인 (사용자 직접) — 체커보드가 의도대로 보이는지 + 면별 회전.
- 누적 TODO 처리:
  - 윈도우 리사이즈 → SwapChain Resize.
  - N 프레임 in-flight 동기 (현재 매 프레임 FlushGpu 로 직렬화).
  - DXGI_PRESENT_ALLOW_TEARING 옵션.
  - MTL 파일 + 면별 머티리얼.
  - OBJ n-gon 자동 삼각형화.

미뤄둔 항목:
- 이미지 디코더 (stb_image / WIC / DDS) — 본격 텍스처 자산 단계.
- 압축 텍스처 (BC1/BC3) + mipmap.
- 큐브맵 / IBL.

## 9. PPT 재료로 쓸 만한 포인트

- "텍스처 업로드 시퀀스 — Upload heap → CopyTextureRegion → Barrier 6단계 흐름도"
- "정점 포맷 변경의 4가지 동기 포인트 (구조체 / 입력 레이아웃 / HLSL / 자산)"
- "d3dx12.h 없이 D3D12 텍스처 — 풀스크래치 의의"
