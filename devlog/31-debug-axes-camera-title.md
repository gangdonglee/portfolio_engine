# 31. 디버그 가시화 — 카메라 좌표 타이틀바 + 원점 좌표축 라인 🧭

- **날짜**: 2026-05-19
- **관련 커밋**: (이 단계 커밋)
- **소요 시간**: 약 1.5시간
- **단계**: Phase 5 Animator 작업 사이 — 디버깅 보조

---

## 1. 목표

사용자 요청: *"카메라가 보는 위치를 알기 위해 좌상단에 x,y,z 만들고 X-Bot 띄울 때 x,y,z 디버깅용으로 볼 수 있게 그려줘"*.

두 기능:
1. **카메라 좌표 표시** — Application::Tick 이 매 100ms 마다 타이틀바에 `cam=(x,y,z) look=(tx,ty,tz)` 갱신.
2. **원점 3D 좌표축** — `engine::render::DebugRenderer` 신규 클래스가 매 프레임 X/Y/Z 라인 (빨강/초록/파랑) 을 LineList topology + depth-test OFF 로 그림.

X-Bot 메시가 화면에 있을 때 원점 좌표축이 *항상 가시* — depth-test OFF 로 메시 뒤에 있어도 보임.

## 2. 사전 컨텍스트

- 5-M1 (devlog 30) 완료 — X-Bot 의 AnimatorRuntime 가 키 입력으로 state 전환.
- 사용자가 *카메라 시점/원점 방향* 을 시각 확인 못하고 있다고 보고 (X-Bot 이 어디 보고 있는지, 카메라가 어디 있는지).
- 5-M2 (Blend Tree) 가기 전에 디버깅 보조 인프라.

## 3. 결정과 트레이드오프

### 31-1. 좌표 표시 — ImGui Client 통합 vs 타이틀바

- **결정**: 타이틀바 매 100ms 갱신.
- **후보**:
  - A) ImGui 를 Client.exe 에도 통합 — Editor 와 동일 패턴. 분량 ~1h. 화면 내 좌상단 텍스트 가능 + 향후 디버그 UI 토대.
  - B) 타이틀바 — 분량 5분. 화면 외부지만 즉시 확인 가능.
- **선택 이유**: B. 사용자 의도 *"카메라 위치 알기 위해"* — 가장 빠른 길. ImGui 도입은 향후 다른 디버그 UI (FPS 카운터, 본 visualizer 등) 와 함께 정식 단계.
- **포기한 것**: 화면 내 위치. 사용자 부담 약간 — 윈도우 프레임 봐야.

### 31-2. 100ms throttle

- **결정**: `m_titleUpdateAccum += dt; if (>= 0.1f) update`.
- **이유**: 매 프레임 `SetWindowTextW` 호출은 (a) OS API 비용 (b) Windows 타이틀바 깜빡임 가능성. 100ms (10 Hz) 면 시각적 충분 + 비용 최소.

### 31-3. DebugRenderer — 자체 RootSig + 자체 PSO

- **결정**: 기존 `engine::render::PipelineState` 가 *HelloTriangle 입력 레이아웃 고정 + Triangle topology*. DebugRenderer 는 *POSITION+COLOR 단순 + LineList* 필요 → 자체 `ID3D12PipelineState` 직접 생성.
- **후보**:
  - A) `PipelineState::Desc` 일반화 (input layout / topology 옵션 추가).
  - B) DebugRenderer 자체 PSO 직접.
- **선택 이유**: B. 디버그용 1회성 PSO 가 `PipelineState` 의 *재사용성 추상화* 안에 들어가면 그쪽이 부풀음. 정식 일반화는 *다른 PSO 가 필요한 시점* (포스트프로세스 등) 에.
- **포기한 것**: `PipelineState` 의 통일성. 추후 일반화 시점에 DebugRenderer 도 그것 사용으로 마이그레이션.

### 31-4. Depth-test OFF — 항상 가시

- **결정**: PSO 의 `DepthStencilState.DepthEnable = FALSE`.
- **이유**: 디버그 좌표축은 *메시 안에 있어도* 보여야 함. 사용자가 X-Bot 메시 뒤의 +Z 축 못 보면 디버깅 의미 손실.
- **포기한 것**: 정확한 occlusion. 디버그용이라 OK.

### 31-5. 매 프레임 *항상* 그리기 (토글 없음)

- **결정**: FrameRenderer 의 Render 안에서 *조건 없이* DrawAxes 호출.
- **이유**: 현재 단계는 *디버그 시각화* 목적. 정식 ship 모드 토글은 M3+ ImGui 디버그 메뉴 단계에서.
- **포기한 것**: 정식 ship 빌드. 본 프로젝트는 *데모/포트폴리오* 라 디버그 보조가 ship 코드와 같이 있어도 무방.

## 4. 작업 내용

### 4-1. `Client/Application::Tick` 타이틀바

```cpp
m_titleUpdateAccum += dt;
if (m_titleUpdateAccum >= 0.1f) {
    m_titleUpdateAccum = 0.0f;
    const auto camPos = m_camera->Position();
    const auto camTgt = m_camera->Target();
    const std::string& state = m_sceneRuntime->HasAnimatorRuntime()
        ? m_sceneRuntime->CurrentAnimatorStateName() : "";
    wchar_t buf[256];
    std::swprintf(buf, ..., L"portfolio_engine | cam=(%.0f, %.0f, %.0f) look=(...) | state=%hs",
                  camPos.x, camPos.y, camPos.z, ...);
    m_window->SetTitle(buf);
}
```

씬 전환 시의 타이틀 (이전 단계의 `Loading: ...`) 은 그대로 — 단지 다음 100ms 후 카메라 좌표로 덮어쓰기.

### 4-2. `shaders/DebugLines.hlsl`

```hlsl
cbuffer DebugFrame : register(b0) {
    row_major float4x4 viewProj;
};
struct VSInput  { float3 position : POSITION; float3 color : COLOR; };
struct VSOutput { float4 position : SV_Position; float3 color : COLOR; };

VSOutput VSMain(VSInput input) {
    VSOutput o;
    o.position = mul(float4(input.position, 1.0), viewProj);
    o.color = input.color;
    return o;
}
float4 PSMain(VSOutput input) : SV_Target { return float4(input.color, 1.0); }
```

### 4-3. `Engine/render/DebugRenderer`

[Engine/render/DebugRenderer.h](../Engine/render/DebugRenderer.h):
```cpp
class DebugRenderer final {
public:
    DebugRenderer(Device&, DXGI_FORMAT rtvFormat, DXGI_FORMAT dsvFormat);
    void DrawAxes(ID3D12GraphicsCommandList*, uint32 frameIndex,
                  const XMMATRIX& viewProj, float axisLength = 100.0f);
private:
    ComPtr<ID3D12RootSignature> m_rootSig;
    ComPtr<ID3D12PipelineState> m_pso;
    unique_ptr<VertexBuffer>    m_axesVB;
    array<unique_ptr<ConstantBuffer>, kFrameCount> m_cbs;
};
```

[Engine/render/DebugRenderer.cpp](../Engine/render/DebugRenderer.cpp) ctor:
1. RootSig — `D3D12_ROOT_PARAMETER_TYPE_CBV` (b0, VS 가시) 1개.
2. 셰이더 컴파일 — `ShaderCompiler::CompileFromFile`.
3. PSO 직접 생성:
   - Input Layout: POSITION+COLOR (24 byte stride).
   - Rasterizer: CullMode NONE, FillMode SOLID.
   - DepthStencilState: DepthEnable FALSE.
   - PrimitiveTopologyType: LINE.
4. VertexBuffer 6 정점 (3 라인): X 빨강 / Y 초록 / Z 파랑.
5. ConstantBuffer × kFrameCount (XMFLOAT4X4 viewProj).

`DrawAxes`:
- CB[frameIndex] 에 viewProj 업데이트.
- SetGraphicsRootSignature + SetPipelineState + IASetPrimitiveTopology(LINELIST).
- VertexBuffer Bind + SetGraphicsRootConstantBufferView(0, ...).
- `DrawInstanced(6, 1, 0, 0)`.

### 4-4. `Client/FrameRenderer` 통합

ctor 에서 `m_debugRenderer = make_unique<DebugRenderer>(m_device, R8G8B8A8, depth.Format())`.

`Render()` 의 ⑤ SceneRuntime.RecordDraw 직후:
```cpp
if (m_debugRenderer) {
    m_debugRenderer->DrawAxes(list, fi, camera.ViewProjection(), 100.0f);
}
```

RTV/DSV/Viewport/Scissor 는 메인 패스에서 이미 셋업 — DebugRenderer 는 *RootSig + PSO + VB + topology* 만 자체 교체.

## 5. 마주친 문제와 해결 ⚠ 필수

### 문제 1 — PipelineState 의 입력 레이아웃 고정
- **문제**: 기존 `PipelineState::Desc` 가 *HelloTriangle 76 byte 정점 레이아웃 + Triangle topology* 고정. 디버그 라인은 24 byte + LineList.
- **해결**: DebugRenderer 가 *자체* `ID3D12PipelineState` 직접 생성. `PipelineState` 일반화는 다음 PSO 필요 시점에.
- **교훈**: 추상화는 *재사용 시점* 에 일반화. 디버그용 1회성 코드는 *직접* 이 정직.

### 문제 2 — DSVFormat 일관성 (depth-off 라도 필수)
- **문제**: DepthStencilState.DepthEnable=FALSE 라도 PSO 의 DSVFormat 은 *바인딩되는 DSV 의 포맷과 일치* 해야 함. FrameRenderer 가 `OMSetRenderTargets` 로 메인 패스의 DSV 를 그대로 두면 DSV 가 활성 — PSO 의 DSVFormat 도 일치 필요.
- **해결**: DebugRenderer ctor 가 `dsvFormat` 인자 받음. FrameRenderer 가 `m_depthBuffer.Format()` 전달.
- **교훈**: D3D12 의 PSO 일관성 검사는 매우 엄격. depth-off 의도라도 *모든 출력 자원의 포맷* 이 컴파일 시점 일관.

### 문제 3 — Debug 빌드의 X-Bot 부팅이 60~90초
- **상황**: 28단계 (Mixamo X Bot) 부터 확인된 현상. 5-M1 에 클립 4개 사전 로드 추가되어 더 느려짐. Release 는 10초 안.
- **해결 X**: Debug 의 LoadFbx EvaluateGlobalTransform 호출 비용. *정상 동작이지만 느림*. Release 검증으로 최종 확인.
- **교훈**: Debug 빌드의 *Sample/검증* 은 Release 우선. Debug 는 *디버거 부착* 시만.

## 6. 결과 / 검증

- **빌드 (Debug + Release)**: 0 warning / 0 error.
- **Client.exe Release 자동 실행 (15초)**:
  ```
  [debug] DebugRenderer ready (axes LineList, depth-off)
  [input] 0=T-pose, 1..4=clip select.
  [input] F1..F9 = scene slot select.
  ```
  DebugRenderer 정상 초기화 + 메인 루프 진입.
- **시각 검증 (사용자 부탁)**:
  1. 타이틀바: `portfolio_engine | cam=(0, 100, -300) look=(0, 50, 0) | state=Idle` 형태로 100ms 마다 갱신.
  2. 화면 원점에 좌표축 3개 라인 가시 — 빨강(X), 초록(Y), 파랑(Z), 각 100 unit 길이.
  3. X-Bot 메시 뒤로 가도 좌표축 보임 (depth-test OFF).
  4. 카메라 이동 (마우스 우클릭 + WASD) 시 타이틀바 좌표 변화 확인.

## 7. AI 협업 메모

- 사용자 요청 *짧은 한 줄* 이 두 기능 + 큰 새 모듈 (DebugRenderer) 작업으로 풀림. 분량 ~1.5h.
- ImGui Client 통합을 의식적으로 *보류* — 타이틀바 우선이 빠른 길. 디버그 UI 가 본격 필요해질 때 (FPS 카운터, 본 visualizer, gizmo) ImGui 도입.

## 8. 다음 단계

- **5-M2** (Blend Tree 1D) 재개.
- 디버그 시각화 후보 (필요 시):
  - Bone visualizer — Skeleton 의 본 트리를 흰색 라인 + 본 위치에 작은 박스.
  - Light 위치 — DirectionalLight 방향 / PointLight 위치+range 와이어프레임 구.
  - Frustum — 카메라 절두체 시각화.

## 9. PPT 재료로 쓸 만한 포인트

- "DebugRenderer — 디버깅 인프라의 첫 단계. LineList + depth-off + 자체 PSO. 게임 엔진의 *시각 디버깅* 핵심."
- "PSO 일관성 — DepthEnable=FALSE 라도 DSVFormat 은 바인딩 DSV 와 일치 필수. D3D12 의 엄격한 컴파일 시 검사."
- "타이틀바 디버그 정보 — ImGui 없이도 즉시 가능한 가장 가벼운 디버그 UI."
