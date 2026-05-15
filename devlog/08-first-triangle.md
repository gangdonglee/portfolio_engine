# 08. Phase 1E — 첫 삼각형 (1E-1 ~ 1E-3) 🎉

- **날짜**: 2026-05-15
- **관련 커밋**: `059500b` (1E-1 ShaderCompiler), `8b36c33` (1E-2 RootSig+PSO), `3afede7` (1E-3 VertexBuffer+Draw)
- **소요 시간**: 약 2시간
- **단계**: Phase 1E — First Triangle (Phase 1 종결)

---

## 1. 목표

**자기 셰이더로 그린 첫 결과**. NDC 좌표 (0, 0.5), (0.5, -0.5), (-0.5, -0.5) 의 정점 3개에 빨강·초록·파랑 색을 부여하고 픽셀 보간된 그라데이션 삼각형을 dark slate 배경 위에 그린다.

Phase 1 (Foundation + Render Bootstrap) 의 종결점이자, 이후 모든 그래픽스 작업의 토대.

## 2. 사전 컨텍스트

- 직전 단계: [Phase 1D 첫 클리어](07-swapchain-and-first-clear.md). 매 프레임 dark slate 클리어된 윈도우.
- 본 단계로 도달하면 **Phase 2 (애니메이션/컴뱃 등 게임플레이) 진입 가능**.

## 3. 결정과 트레이드오프

### 결정 1 — D3DCompileFromFile (fxc) 런타임 컴파일, SM 5.0
- 후보: ① fxc 런타임 ② dxc 런타임 (SM 6.0+) ③ 오프라인 .cso 빌드
- 선택: **fxc 런타임 SM 5.0**
- 이유: 코드 짧음, 학습 자료와 일치, SM 6.0 의 새 기능(웨이브 등) 현 단계 비필요.
- 향후: 셰이더 기능 부족 또는 빌드 시간 단축 요구 시 dxc 또는 .cso 빌드 도입.

### 결정 2 — Upload heap VertexBuffer
- 후보: ① Upload heap 직접 (CPU writable) ② Default heap + staging copy
- 선택: **Upload heap 직접**
- 이유: HelloTriangle 정점 72바이트. CPU 캐시 영향 무시. 단순.
- 향후: 정적 메시 수가 늘면 Default heap 으로 마이그레이션 (성능 차).

### 결정 3 — 비어있는 RootSignature
- HelloTriangle 은 정점 입력만 받음. CBV/SRV/UAV/Sampler 모두 없음.
- `ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT` 플래그만 켜고 파라미터 0.

### 결정 4 — 입력 레이아웃 PSO 안에 하드코딩
- 첫 단계는 단일 메시 형식 → PSO 안에 `kHelloTriangleInputLayout` 정적 const.
- 향후 매개변수화 (PSO::Desc 에 InputLayout 필드 추가) — 메시 타입 다양해질 때.

### 결정 5 — PostBuildEvent 로 shaders 폴더 복사
- 런타임 셰이더 경로 결정: `GetModuleFileNameW(exe)` + `shaders/`.
- Client.vcxproj 의 PostBuildEvent 가 `$(SolutionDir)shaders\*.hlsl → $(OutDir)shaders\` 복사.
- 디버거/직접 실행 어느 경로에서든 exe 옆 shaders/ 가 일관됨.

## 4. 작업 내용

### 4-1. ShaderCompiler (1E-1, 커밋 `059500b`)
- `shaders/HelloTriangle.hlsl` — VS + PS 한 파일. 정점 입력 POSITION + COLOR.
- `Engine/render/ShaderCompiler.{h,cpp}` — D3DCompileFromFile 헬퍼.
  - Stage enum (Vertex/Pixel) → target string (vs_5_0/ps_5_0).
  - DefaultShaderDir() — exe 폴더 + `shaders/`.
  - 컴파일 플래그: Debug 시 DEBUG+SKIP_OPTIMIZATION, 공통 STRICTNESS+WARNINGS_AS_ERRORS.
  - 실패 시 error blob 메시지 포함 throw.
- Client.vcxproj PostBuildEvent — xcopy shaders/.

### 4-2. RootSignature + PSO (1E-2, 커밋 `8b36c33`)
- `Engine/render/RootSignature.{h,cpp}` — 비어있는 RS + `D3D12SerializeRootSignature` + `CreateRootSignature`.
- `Engine/render/PipelineState.{h,cpp}`:
  - `Desc` 구조체: vertexShader/pixelShader (`ID3DBlob*`), rootSignature 포인터, rtvFormat.
  - 디폴트 Rasterizer (Solid+CullBack), Blend (no blending), DepthStencil (Disabled, DSV UNKNOWN).
  - 입력 레이아웃 정적 (POSITION float3 + COLOR float3, 12바이트 오프셋).
  - 헤더 의존 d3dcommon.h + dxgiformat.h — d3d12.h 풀 노출 회피.

### 4-3. VertexBuffer + Draw (1E-3, 커밋 `3afede7`)
- `Engine/render/VertexBuffer.{h,cpp}`:
  - Upload heap 직접 + Map/memcpy/Unmap.
  - 헤더 위생: `D3D12_VERTEX_BUFFER_VIEW` 멤버 X, GPU 주소 + size + stride 만.
  - `Bind(cmdList, slot)` — IASetVertexBuffers.
  - `VertexCount() = byteSize / stride`.
- main.cpp 갱신:
  - `HelloVertex` struct, `kTriangleVertices[3]` 정점 데이터.
  - 매 프레임: `OMSetRenderTargets` → `RSSetViewports/ScissorRects` → `SetGraphicsRootSignature/PipelineState` → `IASetPrimitiveTopology` → `triangleVB.Bind` → `DrawInstanced(3, 1, 0, 0)`.

## 5. 마주친 문제와 해결 ⚠ 필수 — 빠짐없이 기록

### 문제 1 — ID3DBlob forward decl 충돌 (Phase 1E-1)
- **문제**: `ShaderCompiler.h` 에 `struct ID3DBlob;` forward decl. cpp 빌드 시 C2027/C2039 "정의되지 않은 형식 'ID3DBlob'".
- **원인**: `ID3DBlob` 는 `d3dcommon.h` 의 `typedef ID3D10Blob ID3DBlob`. 단순 `struct ID3DBlob;` 가 typedef 와 다른 entity 로 처리됨. ComPtr<ID3DBlob>::-> 호출 시 incomplete type.
- **해결**: 헤더에 가벼운 `<d3dcommon.h>` include. `d3d12.h` / dxgi 전체는 여전히 .cpp 한정.
- **교훈**: typedef 베이스 타입은 forward decl 불가. 정의 헤더가 가볍다면 inline include 가 정답. `Engine/core/Types.h` 같은 alias 파일 도입 시 같은 함정 주의.

### 문제 2 — 셰이더 컴파일 swprintf 경고 (downstream of 문제 1)
- **문제**: `swprintf` 가 `%zu` 에 대해 "variadic 인수 3개 필요하지만 2개" 경고.
- **원인**: 문제 1로 ID3DBlob 가 incomplete → `code->GetBufferSize()` 가 컴파일 단계에서 평가 실패 → variadic 호출이 인수 1개 줄어든 것처럼 보임.
- **해결**: 문제 1 해결과 동시에 자동 해소.
- **교훈**: 빌드 에러 메시지의 후행 경고는 선행 에러의 부수 효과일 때가 많다. 첫 에러부터 해결.

### 문제 3 — d3dx12.h 미포함 (이전 단계 재확인)
- **문제**: 이미 [Phase 1D 문제 5](07-swapchain-and-first-clear.md#문제-5-d3dx12h-가-windows-sdk-100261000-에-미포함) 에서 발견. ResourceBarrier 도 수기 작성 정책.
- **본 단계 영향**: PSO Desc 의 RasterizerState/BlendState/DepthStencilState 도 수기 채움 (CD3DX12_*_DESC 헬퍼 미사용). 약 50줄 추가되지만 의도 명시적.
- **교훈**: 풀스크래치 원칙 일관 적용 가치 — 모든 D3D12 구조체를 수기로 다룬다는 것이 면접/포트폴리오에서 "API 를 깊이 안다"는 시그널.

### 문제 4 — `D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST` vs `D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE`
- **문제**: 같은 이름의 다른 enum 두 개 헷갈리기 쉬움.
- **원인**: PSO 의 `PrimitiveTopologyType` 은 `D3D12_PRIMITIVE_TOPOLOGY_TYPE` (TRIANGLE/POINT/LINE/PATCH 등 카테고리). `IASetPrimitiveTopology` 는 `D3D_PRIMITIVE_TOPOLOGY` (TRIANGLELIST/STRIP 등 구체 토폴로지). 두 단계가 다른 enum 사용.
- **해결**: PSO 에 `D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE`, IA 에 `D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST` 명확 구분.
- **교훈**: D3D12 의 두 단계 enum 분리는 의도된 설계 (정적 PSO 카테고리 + 동적 IA 토폴로지). 처음 보면 헷갈리지만 한 번 익히면 명확.

### (실수 없음 — 빌드/실행 1E-2, 1E-3)
- 1E-1 의 forward decl 이슈 외엔 빌드/실행 1회 통과.
- 학습 자료 + Microsoft DX12 샘플 패턴 그대로 적용 → 회귀 위험 낮음.

## 6. 결과 / 검증

| 항목 | 결과 |
|---|---|
| Debug/Release\|x64 빌드 | ✅ 0 경고, 0 에러 |
| PostBuildEvent: shaders 복사 | ✅ 578 bytes hlsl 복사 |
| 셰이더 런타임 컴파일 (VS+PS) | ✅ SM 5.0 통과 |
| RootSignature + Graphics PSO 생성 | ✅ |
| VertexBuffer Upload heap + Map/memcpy | ✅ 72 bytes / stride 24 / count 3 |
| **DrawInstanced 첫 가시 결과** | ✅ **RGB 그라데이션 삼각형** |
| CloseMainWindow → ExitCode 0 | ✅ FlushGpu 명시 호출 |

### 가시 결과
- 1280×720 윈도우 클라이언트 영역 = dark slate (RGB 0.05, 0.07, 0.10) 배경
- 화면 중앙 NDC 삼각형:
  - 위 정점 (0, 0.5): **빨강** (1, 0, 0)
  - 우하 (0.5, -0.5): **초록** (0, 1, 0)
  - 좌하 (-0.5, -0.5): **파랑** (0, 0, 1)
- 픽셀 셰이더가 정점 색을 보간 → 부드러운 RGB 그라데이션

스크린샷 자리표시자 (포트폴리오 PPT 용):
- (TODO) **첫 삼각형 스크린샷** — 가장 임팩트 있는 Phase 1 결과
- (TODO) VS Output 창 로그 — [render] 라인 시퀀스 (Device → SwapChain → Shader → PSO → VertexBuffer)
- (TODO) PIX/Nsight 캡처 — 매 프레임 명령 시퀀스 시각화

## 7. AI 협업 메모

본 단계는 학습 자료 + Microsoft DX12 Hello Triangle 샘플 패턴을 직접 따라 작성. 패턴이 명확해 서브에이전트 리뷰 생략 (1E-1/1E-2/1E-3 모두).

향후 비표준·창의적 부분(예: 컴뱃 시스템, 애니메이션 스테이트 머신) 에서는 패턴 B 적극 적용 예정.

## 8. 다음 단계

### Phase 1 종결 — Phase 2 진입 준비
Phase 1 (Foundation + Render Bootstrap) 모든 단계 완료. 누적 commits 26+개. devlog 1~8 (8 엔트리).

다음 직후 후보:
- **Phase 2 도입 전 인프라 보강** (TODO 누적 정리):
  - `Engine/core/HrCheck.h` — ThrowIfFailed 공용 헤더 (현재 6 .cpp 중복 정의)
  - `Engine/core/Logger.h` — OutputDebugStringW 직접 호출 마이그레이션
  - `Engine/core/Types.h` — int8/uint32 alias
  - WM_SIZE → SwapChain Resize 처리
  - N프레임 in-flight 개선 (현재 매 프레임 FlushGpu)
- **Phase 2 직진**:
  - 깊이 버퍼 (DSV + Depth State)
  - 상수 버퍼 (MVP 변환)
  - 메시 로더 (간단 OBJ 또는 자체 포맷)
  - 카메라 클래스

추천: **인프라 보강 1~2개** (HrCheck, Logger) **후 Phase 2 직진**. 인프라가 커지기 전에 정리.

## 9. PPT 재료로 쓸 만한 포인트

- **"첫 삼각형 — Phase 1 종결"** 슬라이드 (스크린샷 + Phase 1 8단계 타임라인)
- **"DX12 첫 삼각형 파이프라인 다이어그램"** — Shader→PSO→RootSig→VertexBuffer→Draw 의 객체 그래프
- **"왜 풀스크래치인가 (재확인)"** — d3dx12.h 회피, 모든 D3D12 구조체 수기 채움이 만드는 깊이
- **"PSO 의 두 토폴로지 enum"** — `D3D12_PRIMITIVE_TOPOLOGY_TYPE` vs `D3D_PRIMITIVE_TOPOLOGY` 차이 슬라이드 (이해의 시그널)
- **"입력 레이아웃 + 정점 데이터 매칭"** — POSITION/COLOR HLSL semantic ↔ D3D12_INPUT_ELEMENT_DESC ↔ HelloVertex C++ struct 의 3 단 매칭 도식
- **"Phase 1 종결 — 8 단계 회고"** — 1A Foundation → 1B Window → 1C Device → 1D-1 CommandQueue → 1D-2 RtvHeap → 1D-3 SwapChain → 1D-4 Clear → 1E-1 ShaderCompiler → 1E-2 RootSig+PSO → 1E-3 첫 삼각형 (스택 다이어그램)
