# 22. M1 — Scene JSON 직렬화 + 다중 라이트 StructuredBuffer 🌟

- **날짜**: 2026-05-18
- **관련 커밋**: (이 단계 커밋)
- **소요 시간**: 약 2시간
- **단계**: Phase 4 — 맵 에디터 (M1/5)

---

## 1. 목표

맵툴(Editor) 이 만들어 `.scene.json` 으로 저장한 씬을 Client(런타임) 가 같은 포맷으로 로드해 렌더한다.
M1 은 그 데이터 파이프라인의 최소 증명:

- `engine::scene::Scene` 데이터 모델 + nlohmann/json 기반 Save/Load.
- 라이트는 가변 길이 (dir/point 둘 다 `std::vector`) — GPU 측 StructuredBuffer 로 캡 없이 업로드.
- Editor 의 File→Save 가 하드코딩 Scene 을 JSON 으로 저장 → Client 가 같은 트리에서 다른 .scene 을 로드 → 라운드트립 검증.

## 2. 사전 컨텍스트

- 17~19 단계에서 FBX/머티리얼 sub-draw/이미지 디코더 자산 파이프라인 완성.
- 20~21 단계에서 스키닝 + 애니메이션 (Dragon 본 팔레트 + 클립 재생) — RootSignature 에 b1 CBV(BonePalette) 추가됨.
- M0 단계에서 Editor.exe 골격 + ImGui 도킹 패널 3개 작성.

본 단계 시작 직전 RootSig 슬롯 구성: `[0]=b0 Frame` `[1]=b1 Bones(VS)` `[2]=t0 Material table(PS)`. 단일 라이트가 `b0` 안에 박혀 있었음.

## 3. 결정과 트레이드오프

### 22-1. 라이트 GPU 표현은 StructuredBuffer (캡 없음) — 사용자 결정

- **결정**: HLSL `StructuredBuffer<DirectionalLightGpu>` (t1) + `StructuredBuffer<PointLightGpu>` (t2). CPU 측 `engine::render::StructuredBuffer` 클래스가 Upload heap 위에서 capacity 만 사전 정의(dir 16 / point 64), 매 프레임 element 수와 데이터 업로드.
- **후보**:
  - A) cbuffer 에 고정 배열 (예: dir 4 + point 8) + `lightCount` — 단순하지만 캡 강제.
  - B) StructuredBuffer + root SRV — 캡 없음, descriptor 힙 미사용.
- **선택 이유**: B. AAA 패턴, "라이트 동적 추가/제거" 요구를 정직하게 만족. 비용은 StructuredBuffer 래퍼 클래스 1개 + RootSig 슬롯 2개. ConstantBuffer 와 동일 패턴이라 학습 비용 0.
- **포기한 것**: 단순성. 하지만 cbuffer 캡 방식은 라이트 수가 시나리오마다 다른 게임에서 결국 재방문하게 됨.

### 22-2. RootSig 슬롯 — Descriptor Table 대신 Root SRV

- **결정**: t1/t2 를 `D3D12_ROOT_PARAMETER_TYPE_SRV` (root descriptor) 로. SetGraphicsRootShaderResourceView(slot, GpuAddress) 로 직접 바인딩.
- **이유**: 디스크립터 힙을 거치지 않아 — 매 프레임 가변 길이 데이터(라이트)에 디스크립터 슬롯을 굳이 잡지 않음. heap 용량 절약. 머티리얼 텍스처는 다수 SRV 가 한 묶음으로 바인딩되어야 하니 t0 는 descriptor table 유지.
- **포기한 것**: tier 1 GPU 의 root signature 제한 (root param ≤ 64 DWORD). 현 RootSig 가 64 DWORD 안에 들어감 — Phase 4 안에서 우려 없음.

### 22-3. Scene 데이터 모델은 POD 컨테이너 + 별도 Serializer

- **결정**: `Scene { name, ambient, cameraStart, meshes, dirLights, pointLights }` — 메서드 없음. 직렬화/렌더/편집은 외부.
- **이유**: Scene 의 책임은 "런타임/도구가 공유하는 데이터 컨테이너" 하나. 라이프사이클·렌더링·편집 정책은 각자 다른 모듈이 결정. SRP.
- **포기한 것**: 메서드 체이닝 같은 편의 API. 호출 측 코드가 약간 풀어짐.

### 22-4. JSON 포맷 — nlohmann/json single header vendored

- **결정**: `external/nlohmann_json/json.hpp` (3.11.3). Engine.lib 의 SceneSerializer.cpp 만 인클루드.
- **이유**: 직렬화 코드 50줄 vs 자작 mini-JSON 파서 수백 줄. 단일 헤더라 빌드 부담 없음 (Engine.lib 1 TU 만).
- **포기한 것**: 풀스크래치 100%. ImGui 와 마찬가지로 합리적 양보 (사용자 동의함).

### 22-5. 인스턴스마다 별도 ConstantBuffer (in-flight 슬롯 × 인스턴스)

- **결정**: `instanceCount × kFrameCount` 개의 FrameConstants ConstantBuffer + 같은 수의 BonePalette ConstantBuffer.
- **이유**: 한 cbuffer 슬롯에 인스턴스 N개 데이터를 같은 commandlist 안에서 N번 덮어쓰면 GPU 가 마지막 값만 봄 → 인스턴스마다 별도 GPU 메모리 필요. 단순 구현은 "인스턴스 × 프레임" 만큼 alloc.
- **포기한 것**: 디스크립터·메모리 효율. M2~M3 에서 ring buffer (단일 큰 cbuffer 의 256B aligned offset) 로 전환 예정.

## 4. 작업 내용

### 4-1. `Engine/scene/` 신규 모듈

[Engine/scene/Scene.h](../Engine/scene/Scene.h):
```cpp
struct Transform { XMFLOAT3 position; XMFLOAT4 rotation; XMFLOAT3 scale; };
struct MeshInstance { std::string name; std::string meshAssetPath; Transform transform; };
struct DirectionalLight { std::string name; XMFLOAT3 directionWS; XMFLOAT3 color; float intensity; };
struct PointLight       { std::string name; XMFLOAT3 positionWS;  XMFLOAT3 color; float intensity; float range; };
struct CameraStart      { XMFLOAT3 position; XMFLOAT3 target; float fovYRad; };
struct Scene {
    std::string                   name;
    std::vector<MeshInstance>     meshes;
    std::vector<DirectionalLight> dirLights;
    std::vector<PointLight>       pointLights;
    XMFLOAT3                      ambient;
    CameraStart                   cameraStart;
};
```

[Engine/scene/SceneSerializer.cpp](../Engine/scene/SceneSerializer.cpp): `SaveJson` / `LoadJson`. nlohmann ADL `to_json/from_json` 대신 본 파일 안 헬퍼(`ToArray`, `ParseFloat3/4`, `Get<T>`) 로 명시 직렬화 — DirectXMath POD 가 ADL 친화적이지 않음.

### 4-2. `Engine/render/StructuredBuffer`

[Engine/render/StructuredBuffer.h](../Engine/render/StructuredBuffer.h)/[.cpp](../Engine/render/StructuredBuffer.cpp):
- Upload heap, persistent Map.
- 생성 시 `(elementCapacity, elementStride)` — total `capacity*stride` 바이트.
- `UpdateRange(elements, count)` — count <= capacity 보장.
- `GpuAddress()` — `SetGraphicsRootShaderResourceView` 인자.
- ConstantBuffer 와 1:1 대응 디자인 (cbuffer 정렬 256 / structured 는 stride 만).

### 4-3. RootSignature 확장

[Engine/render/RootSignature.h](../Engine/render/RootSignature.h) `Desc`:
```cpp
bool srvT1Pixel = false;  // StructuredBuffer<DirectionalLight>
bool srvT2Pixel = false;  // StructuredBuffer<PointLight>
```
[Engine/render/RootSignature.cpp](../Engine/render/RootSignature.cpp): `D3D12_ROOT_PARAMETER_TYPE_SRV` 로 root descriptor 슬롯 추가. 슬롯 순서:
```
[0] b0 CBV (frame)         (cbvAtB0)
[1] b1 CBV VS (bones)       (cbvB1Vertex)
[2] t0 SRV table PS (mat)   (srvT0Pixel)
[3] t1 SRV root PS (dir)    (srvT1Pixel)
[4] t2 SRV root PS (point)  (srvT2Pixel)
```

### 4-4. shaders/HelloTriangle.hlsl — 다중 라이트 루프

- `FrameConstants` 에서 단일 `lightDirWS/lightColor` 제거, `dirLightCount`/`pointLightCount` 추가.
- `StructuredBuffer<DirectionalLightGpu>` (t1), `StructuredBuffer<PointLightGpu>` (t2) 선언.
- PS 에서 dir 루프 + point 루프 (거리/range 기반 smooth attenuation `k = saturate(1 - d/range); atten = k*k`).
- 스키닝 VS 는 그대로 보존 (사용자 20~21 단계 결과).

### 4-5. Client/main.cpp — Scene 로드 + 메시 캐시 + 라이트 SB

[Client/main.cpp](../Client/main.cpp) 핵심 변화:
1. 부팅 시 `assets/Scenes/sample.scene.json` 로드. 없으면 `BuildDefaultScene()` (Dragon 1 + dir 1) 폴백.
2. `std::unordered_map<std::string, LoadedAsset>` 메시 캐시. 확장자로 FBX/OBJ 분기.
3. `kFrameCount × instanceCount` 개의 FrameConstants/BonePalette ConstantBuffer.
4. `kFrameCount` 개의 dir/point StructuredBuffer.
5. 매 프레임:
   - 라이트 데이터 → `StructuredBuffer::UpdateRange` (한 번).
   - 인스턴스 루프: 각 인스턴스의 `Transform` → world → `FrameConstants.mvp/world` → CB Update → SetRootCBV(0,1) → DrawAll.
   - 라이트 SRV(3,4) 는 인스턴스 무관 — frame 시작 시 한 번 SetRootSRV.
6. Scene 의 `cameraStart` 로 카메라 초기 위치/타겟/FoV.

### 4-6. assets/Scenes/sample.scene.json (손작성)

Dragon 1 메시 + dir 2개 (Sun + Rim) + point 2개 (FrontFill orange + BackHighlight cyan). Client 가 이걸로 부팅 시 두 방향 + 두 점광이 동시 반영된 화면.

### 4-7. Editor — File→Save 핸들러

`BuildHardcodedScene()` (Dragon 1 + dir 1 + point 1) → `engine::scene::SaveJson(scene, "assets/Scenes/from_editor.scene.json")`. Inspector 패널에 "Last save: ..." 표시. 실패 시 catch + 메시지 표시.

M2 에서 hardcoded 대신 Hierarchy/Inspector 가 만든 활성 Scene 을 저장.

### 4-8. PostBuild xcopy /E /I 추가

[Client/Client.vcxproj](../Client/Client.vcxproj) + [Editor/Editor.vcxproj](../Editor/Editor.vcxproj):
- `xcopy /Y /D /Q "$(SolutionDir)assets\*" "$(OutDir)assets\"` → `xcopy /Y /D /Q /E /I "$(SolutionDir)assets" "$(OutDir)assets"`
- 기존 명령은 하위 폴더 미복사 → `assets/Scenes/sample.scene.json` 이 OutDir 에 안 옴 → Client 가 디폴트 폴백.

## 5. 마주친 문제와 해결 ⚠ 필수

### 문제 1 — assets 하위 폴더 미복사
- **문제**: 첫 실행 시 `[scene] sample.scene.json 없음 - default Scene 사용` 로그. OutDir 의 assets/Scenes/ 가 비어 있음.
- **원인**: 기존 PostBuild xcopy 가 `/E` 플래그 없이 `$(SolutionDir)assets\*` 패턴 사용 — 와일드카드 `*` 가 *현재 폴더 파일만* 매칭, 하위 폴더는 미복사.
- **해결**: `/E /I` 추가 + 인자를 폴더 자체로 변경. `xcopy /Y /D /Q /E /I "$(SolutionDir)assets" "$(OutDir)assets"`. Resources/ 와 동일 패턴.
- **교훈**: xcopy 의 `*` 와 폴더-인자는 동작이 다름. `*` 는 파일만, 폴더 인자 + `/E` 는 트리 통째. assets 하위 폴더가 도입되는 시점부터 `/E` 필수.

### 문제 2 — nlohmann include 경로 결정
- **문제**: 표준 패턴 `<nlohmann/json.hpp>` 는 single-header vendored 위치 (`external/nlohmann_json/json.hpp`) 와 일치하지 않음 — `nlohmann/` 서브폴더 없음.
- **해결**: include path `$(SolutionDir)external\nlohmann_json` 추가 + 코드는 `<json.hpp>` 직접 인클루드. single-header 의 단순 경로 채택.
- **교훈**: vendor 시 폴더 구조를 표준 include 패턴에 맞출지(원본 그대로 두려면 `nlohmann/` 서브폴더 추가) vs include 경로를 vendored 폴더에 맞출지 결정 필요. 본 프로젝트는 후자.

### 문제 3 — RootSignature 슬롯 인덱스 정합성
- **문제**: 사용자가 20단계에서 b1(BonePalette) 을 슬롯 [1] 에 끼워넣어 머티리얼 SRV table 이 [2] 로 밀림. Client 의 `mesh->DrawAll(list, 2, ...)` 가 이미 그 사실 반영. 내가 추가한 t1/t2 root SRV 는 [3], [4] 위치 — `SetGraphicsRootShaderResourceView(3, ...)` / `(4, ...)`.
- **해결**: 슬롯 순서를 RootSignature.cpp 의 if 분기 순서로 강제. 호출 측은 같은 순서로 Desc 플래그를 켜야 — 본 프로젝트는 한 곳(Client) 에서만 RS 만들어 정합성 유지 쉬움.
- **교훈**: 슬롯 인덱스 매직 넘버는 코드 가까이에 주석으로 명시. 향후 RS Desc 가 더 복잡해지면 "슬롯 인덱스" 반환 API 필요할 듯.

## 6. 결과 / 검증

- **빌드 (Debug)**: Engine + Client + Editor 0 warning / 0 error.
- **빌드 (Release)**: 동일.
- **Client.exe 자동 실행 검증 (3초)**:
  - `[scene] loaded: assets/Scenes/sample.scene.json` 로그 확인.
  - `[render] RootSignature created (params=5, samplers=1)` — 5 슬롯 (b0+b1+t0+t1+t2) 확인.
  - 렌더 루프 3초 이상 충돌 없이 진행, GPU debug layer 에러 0.
- **Editor.exe 자동 실행 검증 (3초)**: 부팅 + 도킹 패널 + ImGui 렌더 충돌 없음.
- **수동 검증 필요**: 시각적으로 라이트가 의도대로 적용됐는지 (Sun 따뜻한 흰색 + Rim 푸른빛 + FrontFill 주황 점광 + BackHighlight 청록 점광 의 합성). 사용자가 `Client.exe` 직접 띄워 확인.

## 7. AI 협업 메모

- 사용자가 백그라운드에서 17→18→19→20→21 까지 활발히 작업 중인 상황과 동시 진행. M0 종료 시점 vs M1 시작 시점 사이에 사용자가 스키닝 단계(20~21) 를 추가 — RootSignature 의 b1 슬롯 + 셰이더 BonePalette + Client 의 본 팔레트 cbuffer 가 이미 들어가 있어 M1 가 그 위에 자연스럽게 빌드.
- "사용자 작업 끝날 때까지 대기" 결정 후 Scene/SceneSerializer/StructuredBuffer 세 신규 파일은 *기존 파일 0 수정* 으로 사용자와 충돌 0. 사용자가 끝낸 후 RootSig/shader/Client/vcxproj 차례로 변경.
- Self-diagnose 루틴 도입 — error.txt 파일 sink + 직접 Client/Editor 실행 + 3초 후 kill + 로그 검증 한 사이클로 빌드 결과 + 부팅 정합성 확인.

## 8. 다음 단계 — M2

- Editor 의 Hierarchy 패널: Scene 의 meshes/dirLights/pointLights 트리 리스트.
- Editor 의 Inspector 패널: 선택된 노드의 Transform/color/intensity 편집 (ImGui DragFloat3/ColorEdit3).
- File→New / Open Scene 다이얼로그 (IFileDialog).
- 라이트 추가/제거 버튼 (가변 길이 vector 의 진정한 동적 가시).
- Save 가 BuildHardcodedScene 대신 activeScene 직접 저장.

미뤄둔 항목:
- ConstantBuffer ring buffer (256B aligned offset) — M3 이상.
- 메시 인스턴스마다 별도 Animator — M3.
- Editor 뷰포트에 실제 3D 렌더 (현재는 빈 패널).

## 9. PPT 재료로 쓸 만한 포인트

- "라이트 캡 없음 — StructuredBuffer 로 Scene 의 `std::vector<Light>` 가 GPU 까지 그대로." (Unity/Unreal 도 비슷한 접근.)
- "RootSig 슬롯 순서 = 5 — b0 frame / b1 bones VS / t0 material table PS / t1 dir lights / t2 point lights. descriptor heap 없는 root SRV 로 frame-shared 동적 데이터 표현."
- "Editor.exe → JSON → Client.exe — 자산 파이프라인의 데이터 흐름이 한 .json 파일로 명확."
