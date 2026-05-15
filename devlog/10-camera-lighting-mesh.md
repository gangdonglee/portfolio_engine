# 10. Phase 3 (전반) — 카메라 입력 + 조명 + 메시 로더 (C/F/D) 🎮

- **날짜**: 2026-05-15
- **관련 커밋**: `4281d78` (C 입력+FreeCamera), `81a2fcf` (F Phong 조명), `b633a59` (D Mesh+OBJ 로더)
- **소요 시간**: 약 3시간
- **단계**: Phase 3 — Interactive 3D (E 텍스처 미완)

---

## 1. 목표

회전하는 큐브의 정적 시점에서 벗어나, 사용자가 능동적으로 시점을 바꾸고 + 조명에 의한 입체감을 보고 + 자산을 외부 파일에서 로드하는 인터랙티브 3D 환경 도달.

세부 4개 중 3개 완료:
- **C 카메라 입력** — Input + FreeCamera (WASD + 우클릭 마우스)
- **F 조명** — Phong 셰이딩 (앰비언트 + 디퓨즈 + Blinn-Phong 스페큘러)
- **D 메시 로더** — Mesh 클래스 + OBJ 파서
- E 텍스처 — 다음 단계로 분리

## 2. 사전 컨텍스트

- 직전 단계: [Phase 2 회전 큐브](09-spinning-cube.md). 정적 시점 + 정점 색 보간만.
- 본 단계의 누적 효과: 사용자 시점 자유 + 면별 정확한 음영 + 자산 파이프라인 시작.

## 3. 결정과 트레이드오프

### C — 입력 시스템

#### C-1. Window 가 Input 멤버 보유
- 옵션 A: Window 가 Input 소유 + GetInput() 접근자.
- 옵션 B: Input 별도 인스턴스 + Window 에 콜백 등록.
- **선택 A** — Window 의 WndProc 가 Input 메시지를 직접 forward, 라이프타임 1:1 매칭.

#### C-2. FreeCamera 가 Camera 를 wrap
- FreeCamera 가 Camera& 참조 보유. position/target 갱신을 Camera 에 위임.
- 향후 다른 컨트롤러(OrbitCamera 등) 도 같은 패턴.

#### C-3. yaw/pitch 보유 + 매 프레임 forward 재계산
- yaw/pitch 로 회전 상태 유지 (쿼터니언 X — 짐벌락 위험 있지만 FPS 카메라엔 의도된 동작: pitch 클램프 ±89°).
- forward = (sin(yaw)cos(pitch), sin(pitch), cos(yaw)cos(pitch)) — LH.

### F — Phong 조명

#### F-1. 단일 cbuffer + Visibility ALL
- 옵션 A: b0(VS용 변환 행렬) + b1(PS용 라이트) 분리.
- 옵션 B: b0 단일 cbuffer 에 모두 모음 + Visibility ALL.
- **선택 B** — RootSig 단순. 향후 cbuffer 가 너무 커지면 분리.
- 트레이드오프: VS 가 라이트 데이터를 못 보는 게 더 명확하나, root descriptor 1개 절약이 더 가치.

#### F-2. 24 정점 큐브 (면별 normal)
- 옵션 A: 8 정점 + 공유 (각 꼭지점 1개) — normal 평균. 부드러운 음영.
- 옵션 B: 24 정점 (면당 4) — 각 면 평면 normal. 날카로운 면 경계.
- **선택 B** — 큐브는 직각 면이라 면별 normal 이 시각적으로 맞음. 부드러운 음영은 둥근 메시 (구 등) 시점에 도입.

#### F-3. cbuffer 패딩 (HLSL float3 = 16바이트)
- HLSL 의 float3 는 16바이트로 정렬됨. C++ 측 XMFLOAT3 + float pad0 패턴으로 일치.
- static_assert(sizeof(FrameConstants) == 192) 로 컴파일 시 검증.

### D — 메시 로더

#### D-1. Mesh 가 VB/IB 를 unique_ptr 로 보유
- VertexBuffer/IndexBuffer 가 4 deletes(비이동) → 직접 멤버 두면 Mesh 도 비이동.
- unique_ptr 로 우회하여 Mesh 자체는 비이동 유지 (단일 소유), 내부 자원은 동적 할당.

#### D-2. OBJ 파서 자체 작성 (외부 라이브러리 회피)
- tinyobj/assimp 등 라이브러리 회피 — 풀스크래치 원칙.
- 단순 파서 ~120줄: v/vn/vt/f 파싱 + (posIdx, normIdx) dedup.
- 삼각형 가정. n-gon 미지원 (향후 추가).

#### D-3. defaultColor 파라미터로 색상 대체
- OBJ 표준에 정점 색상 없음 (MTL 파일 별도).
- 단순화: LoadObj 호출 시 defaultColor 인자로 모든 정점에 일괄 적용.
- 면별 색은 후속 (MTL 파일 또는 비표준 v r g b 확장).

#### D-4. assets/ 폴더 자동 복사 (PostBuildEvent 확장)
- shaders 패턴 그대로: `xcopy /Y /D /Q $(SolutionDir)assets\* $(OutDir)assets\`.
- DefaultAssetsDir() 가 GetModuleFileNameW + "assets\\" 반환.

## 4. 작업 내용

### C-1 + C-2 — Input + FreeCamera (커밋 `4281d78`)
**신규**: `Engine/platform/Input.{h,cpp}` (키보드 256 + 마우스 위치/델타/버튼), `Engine/render/FreeCamera.{h,cpp}` (FPS 컨트롤러).

**연동**: Window 에 Input 멤버 + WndProc 의 키/마우스 메시지 forward.

**main 흐름**: PumpMessages → input.BeginFrame() → freeCamera.Update(input, dt) → 렌더.

### F — Phong 조명 (커밋 `81a2fcf`)
**셰이더**: cbuffer 확장 (mvp/world/camera/light/ambient), VS 가 normalWS + positionWS 출력, PS 가 Lambert + Blinn-Phong 결합.

**PSO**: 입력 레이아웃에 NORMAL 추가 (POSITION + NORMAL + COLOR).

**RootSig**: cbvAtB0Vertex → CbvB0 enum (None/Vertex/All).

**큐브**: 24 정점 면별 normal/색상 + 36 CW 인덱스.

### D — Mesh + OBJ 로더 (커밋 `b633a59`)
**신규**: `Engine/render/Mesh.{h,cpp}` (VB+IB 묶음), `Engine/render/ObjLoader.{h,cpp}` (네임스페이스 함수), `assets/Cube.obj`.

**손코딩 정점 60+ 줄 제거** → 3줄 (LoadObj 호출).

## 5. 마주친 문제와 해결 ⚠ 필수 — 빠짐없이 기록

### 문제 1 — Cube 와인딩 검증 (이전 단계 부채)
- **문제**: Phase 2 의 큐브 인덱스 와인딩이 LH + CullBack 정합 미검증 상태 (devlog 09 §5 문제 5). 이번 F 단계 24 정점 재작성 시 동시 해결 필요.
- **원인**: 면별 outward normal 방향에서 CW 가 정면이라는 룰 적용 필요. 손코딩 순서가 헷갈리기 쉬움.
- **해결**: 각 면을 "outward normal 방향에서 본 좌하→우하→우상→좌상" 4 정점으로 일관 배치. 인덱스 (0,1,2)+(0,2,3) 패턴이 모든 면에 통일됨. OBJ 파일도 동일 방식.
- **교훈**: 4 정점을 면별 (좌하/우하/우상/좌상) 표준 순서로 정렬하면 인덱스는 면별 동일 패턴 → 손코딩 오류 격감.

### 문제 2 — PostBuildEvent 가 Debug/Release 두 곳 동일 매치 → Edit 실패
- **문제**: assets 복사 명령 추가 시 Edit(replace_all=false) 가 "2 matches found" 에러로 거부.
- **원인**: vcxproj 에 Debug 와 Release ItemDefinitionGroup 두 곳에 동일한 PostBuildEvent 명령. 두 곳 모두 갱신 필요.
- **해결**: replace_all=true 로 재시도. 두 configuration 모두 갱신.
- **교훈**: vcxproj 의 Debug/Release 설정 변경은 거의 항상 두 곳 동시 — replace_all=true 디폴트로 가정.

### 문제 3 — HLSL cbuffer 의 float3 정렬 (Phong 조명 cbuffer)
- **문제**: HLSL cbuffer 안의 float3 가 16바이트 경계로 정렬 — C++ 측 XMFLOAT3 (12바이트) 뒤에 4바이트 패딩 명시 안 하면 GPU 가 잘못된 위치에서 읽음.
- **원인**: HLSL constant buffer packing 규칙. float3 + float 가 묶여 16바이트 슬롯에 들어가지만, float3 단독은 다음 float3 와 연속될 때 잘못 정렬됨.
- **해결**: C++ FrameConstants 에 각 XMFLOAT3 뒤에 `float _padN` 명시. static_assert(sizeof == 192) 로 검증.
- **교훈**: HLSL cbuffer ↔ C++ struct 일치는 packing 규칙 숙지 필수. static_assert 로 컴파일 시 잡기.

### 문제 4 — RootSignature Visibility 의미 분리
- **문제**: F 조명 도입 시 cbuffer 를 PS 도 접근해야 함. 기존 RootSig::Desc::cbvAtB0Vertex (bool) 는 Vertex 전용. ALL 옵션 필요.
- **원인**: bool 로 켜고 끄기만 했음 → 가시성 범위 표현력 부족.
- **해결**: enum class CbvB0 { None, Vertex, All } 로 매개변수화. Vertex 가시 vs ALL 가시 분리.
- **교훈**: bool 매개변수는 의미가 두 개일 때만 안전. 의미가 세 개 이상 또는 확장 가능성 있으면 처음부터 enum.

### 문제 5 — OBJ 의 face vertex 토큰 파싱
- **문제**: OBJ 의 f 라인 face vertex 가 `1//1` (v//vn), `1/2/1` (v/vt/vn), `1` (v 만) 3가지 형식. 일관 파서 필요.
- **원인**: 슬래시 개수와 위치로 분기 — `find('/')` + `rfind('/')` 비교로 처리.
- **해결**: ParseFaceVertex(token) 헬퍼 — 첫/마지막 슬래시 위치로 형식 식별 + posIdx/normIdx 추출.
- **교훈**: 텍스트 파싱은 형식 분기를 명확히 분리. 정규식보다 명시적 슬래시 위치 검사가 빠름.

### 문제 6 — Mesh 의 비이동 + 자원 멤버
- **문제**: VertexBuffer/IndexBuffer 가 4 deletes (비이동). Mesh 가 이들을 직접 멤버로 두면 Mesh 도 비이동 → ObjLoader::LoadObj 가 Mesh 반환 못함.
- **해결**: Mesh 의 멤버를 unique_ptr<VertexBuffer> + unique_ptr<IndexBuffer> 로. ObjLoader::LoadObj 가 unique_ptr<Mesh> 반환.
- **교훈**: 비이동 자원을 한 단계 wrap 할 때 unique_ptr 멤버가 자연. 가독성 약간 손해(`->`) 보지만 반환 패턴이 깔끔.

## 6. 결과 / 검증

| 항목 | 결과 |
|---|---|
| Debug/Release\|x64 빌드 (3 commit 누적) | ✅ 0 경고, 0 에러 |
| WASD/QE + 우클릭 마우스 입력 처리 | ✅ |
| Phong 조명 (앰비언트/디퓨즈/스페큘러) | ✅ 시각적 음영 차이 명확 |
| OBJ 파일 로드 (Cube.obj, 1224 bytes) | ✅ 8 위치 + 6 normal → 24 unique 정점 + 36 인덱스 |
| 카메라 이동 시 스페큘러 추적 | ✅ |
| CloseMainWindow → ExitCode 0 | ✅ |

### 가시 결과
- 회색-푸른 단색 큐브가 dark slate 배경 위에서 회전.
- 면별 라이트 음영 — 라이트(사선 위, 정규화된 (-0.5, -1, 0.4)) 방향에 따라 면의 밝기 차이.
- WASD 로 시점 이동, 우클릭 hold + 드래그로 회전. Shift 부스트.
- 스페큘러 하이라이트가 카메라 위치 따라 면 위로 이동.

스크린샷 자리표시자:
- (TODO) Phong 음영 큐브 정면/회전 스크린샷
- (TODO) 짧은 GIF — WASD 이동 + 마우스 회전
- (TODO) 라이트 방향 벡터 시각화 다이어그램

## 7. AI 협업 메모

- 작업 흐름이 표준 패턴 (입력 → 카메라 → 조명 → 메시 로더) 으로 회귀 위험 낮아 서브에이전트 리뷰 생략.
- PowerShell 일괄 치환 활용 (Edit 실패 시 PowerShell ReadAllText + Replace 우회 패턴).
- Edit 도구의 Read 캐시 무효화 (vcxproj 등 자주 외부 갱신되는 파일) 빈발 — Read 재호출 또는 Write 전체 재작성.

## 8. 다음 단계

### E 텍스처 — 다음 turn 본격 진행
- 정점에 UV(float2) 추가
- Cube.obj 에 vt 라인 + ObjLoader 가 vt 읽기
- **Texture 클래스** — Default heap 텍스처 + Upload heap 스테이징 + CopyTextureRegion + barrier
- **SRV 디스크립터 힙** (shader-visible — RTV/DSV 와 다름)
- **RootSignature 확장** — descriptor table (SRV) + static sampler
- 셰이더 — Texture2D + SamplerState + 샘플링

**풀스크래치 원칙 영향**: PNG/JPG 라이브러리 회피 → 더미 텍스처(체커보드, 코드 생성) 또는 자체 DDS 로더로 시작. 시각 임팩트 vs 구현 복잡도 트레이드오프.

### TODO 누적
- [x] WM_SIZE → SwapChain Resize (여전히 미구현, 윈도우 크기 고정)
- [ ] N프레임 in-flight 개선
- [ ] DXGI_PRESENT_ALLOW_TEARING
- [ ] OBJ 의 vt(UV) 읽기 (E 단계에서)
- [ ] MTL 파일 지원 (재질/색상)
- [ ] N-gon 삼각형화

## 9. PPT 재료로 쓸 만한 포인트

- **"FPS 자유 카메라"** — 입력 매핑 표 (WASD/QE/Shift/우클릭) + LH 좌표계 forward 벡터 공식
- **"Phong 셰이딩 분해"** — 앰비언트 / 디퓨즈(Lambert) / 스페큘러(Blinn-Phong) 각 단계 시각화
- **"HLSL cbuffer 정렬의 함정"** — float3 패딩 + static_assert 패턴
- **"OBJ 파서 직접 구현"** — 외부 라이브러리(tinyobj/assimp) 회피 vs 자체 ~120줄. 풀스크래치 원칙의 일관 적용
- **"인덱스 와인딩 손코딩 룰"** — outward normal 시점 좌하/우하/우상/좌상 + (0,1,2)+(0,2,3) 통일 패턴
- **"인터랙티브 3D 마일스톤"** — Phase 3 (전반) 결과: 자유 시점 + 음영 + 자산 파이프라인 시작
