# 17. Phase 3+ — Autodesk FBX SDK 통합 + Dragon.fbx 로드 🐉

- **날짜**: 2026-05-18
- **관련 커밋**: (이 단계 커밋)
- **소요 시간**: 약 2시간
- **단계**: Phase 4 진입 — 실제 자산 파이프라인

---

## 1. 목표

학습 자료의 Dragon.fbx (6.3MB) 를 우리 엔진으로 직접 로드해 렌더. OBJ 자작 파서에 이어 *실제 자산 포맷*의 첫 도입. FBX 표준 폴리곤 + 머티리얼 색 (Kd) 까지 추출, 스키닝/애니메이션은 별도 단계로 분리.

## 2. 사전 컨텍스트

- 직전까지 OBJ + MTL 만 지원. 본 단계의 도전: 실제 자산 포맷 처리.
- FBX 는 Autodesk 비공식 바이너리 포맷 — 자작 파서 사실상 불가능. 옵션 A (풀스크래치) 원칙의 첫 깨짐.
- 학습 자료 `D:\Things\Animation_소스코드\Library` 에 Autodesk FBX SDK 가 이미 들어있음 (Include 3.7MB + Lib 800MB).
- 학습 자료 폴더는 향후 정리 예정 — third_party 이전은 후속 단계로 분리.

## 3. 결정과 트레이드오프

### 17-1. FBX SDK 도입 — 풀스크래치 원칙의 의도된 깨짐
- **결정**: Autodesk FBX SDK 사용.
- **후보**:
  - A) ufbx (single-header, MIT) — single-header 라 옵션 A 원칙 손상 최소.
  - B) Assimp — 광범위 포맷 + 큰 의존성.
  - C) FBX SDK — 공식, 학습 자료에 이미 동봉.
- **선택 이유**: C — 학습 자료의 패턴을 그대로 차용하고 *학습 자료에서 본 코드를 우리 OOP 스타일로 리팩토링* 하는 게 본 프로젝트 의도. 면접 시 "FBX SDK 의 mapping/reference mode 4가지 조합을 직접 처리" 시그널 더 강력.
- **포기한 것**: 자체 완결성. 라이브러리 800MB + 학습 자료 폴더 의존. 후속 단계로 third_party 이전.

### 17-2. Phase A 범위 — 메시 + 머티리얼 Kd 만, 스키닝/애니메이션 제외
- **결정**: FbxLoader 1차 구현은 메시(pos/normal/uv) + 머티리얼 diffuse 색만. 본/가중치/키프레임 추출 X.
- **이유**:
  - 셰이더 (HelloTriangle.hlsl) 에 본 팔레트 cbuffer 미도입. 스키닝하려면 VS 가 본 행렬 N개를 받아 가중치 합성해야.
  - 머티리얼별 텍스처도 별도 SRV 슬롯 다중화 필요 — sub-draw 시스템.
  - 한 단계에 다 넣으면 디버깅 표면 폭증.
- **포기한 것**: Dragon 의 애니메이션 시각 (T-pose 정적 렌더). 후속 단계 (셰이더 본 cbuffer + Skeleton/AnimClip + Animator) 에서 도입.

### 17-3. SDK 위치 — 학습 자료 절대 경로 직접 참조 (third_party 복사 회피)
- **결정**: `FbxSdkRoot=D:\Things\Animation_소스코드\Library` 매크로 + vcxproj 의 Include/Lib path 가 참조.
- **후보**:
  - A) third_party/FBX 에 800MB 복사 — 자체 완결성, 디스크 부담.
  - B) 학습 자료 절대 경로 직접 참조 — 디스크 절약, 학습 자료 의존.
  - C) 환경변수 FBX_SDK_ROOT.
- **선택 이유**: B — 본 PC 1대 빌드 가정. 학습 자료 정리 단계 도래 시 매크로 한 줄만 갱신.
- **포기한 것**: 다른 PC/CI 빌드 가능성. 현재 단계는 사용자 본인 PC 에서만 빌드.

### 17-4. Static MD 변종 (libfbxsdk-md.lib + libxml2-md.lib + zlib-md.lib)
- **결정**: DLL 미사용. static link.
- **이유**: Runtime Library = MultiThreadedDLL 이므로 -md 변종 일치. DLL 복사 단계 회피.
- **포기한 것**: 컴파일 시간 약간 증가. 별 의미 없음.

### 17-5. controlPoint 단위 dedup (학습 자료 패턴) — face vertex 단위 분리 X
- **결정**: 학습 자료의 LoadMesh 패턴 그대로 — `vertices.resize(controlPointCount)`. 같은 controlPoint 의 normal/uv 는 마지막 polygon vertex 값.
- **트레이드오프**: 정확하진 않지만 (face별 normal 분리 손실) 캐릭터/유기 메시처럼 매끄러운 표면엔 자연. Dragon 은 유기 메시라 적합.
- **포기한 것**: 큐브처럼 면별 normal 분리 자산은 부정확. 그러나 큐브 시절은 OBJ 로더 사용. FBX 는 캐릭터/대형 메시 가정.

### 17-6. 좌표계 변환 — Y/Z swap + face winding 반전
- **결정**: 학습 자료 패턴 차용 — `pos.{x, y, z} = controlPoints[{0, 2, 1}]`. face winding `(0, 2, 1)`.
- **이유**: FBX 기본은 Y-up RH (Maya 컨벤션). 우리는 D3D LH. swap + winding 반전이 정석.
- **추가 안전**: `FbxAxisSystem::DirectX.ConvertScene(scene)` 도 호출 — SDK 가 노드 transform 까지 자동 변환.

### 17-7. Mesh R32 인덱스 오버로드
- **결정**: `Mesh(device, vertices, count, uint32* indices, count)` 오버로드 추가.
- **이유**: Dragon 은 수만 정점 → R16 한계(65535) 초과. IndexBuffer 는 이미 R16/R32 둘 다 지원 — Mesh ctor 만 추가.
- **포기한 것**: 없음. OBJ 는 65535 미만이라 기존 R16 그대로.

## 4. 작업 내용

### 4-1. 빌드 시스템
- **Engine.vcxproj**: UserMacros 에 `FbxSdkRoot/FbxSdkInclude/FbxSdkLibDebug/FbxSdkLibRelease` 추가. AdditionalIncludeDirectories 에 `$(FbxSdkInclude)`.
- **Client.vcxproj**: 같은 UserMacros + Link 의 AdditionalDependencies 에 `libfbxsdk-md.lib;libxml2-md.lib;zlib-md.lib`, AdditionalLibraryDirectories 에 `$(FbxSdkLibDebug|Release)`. LNK4099 무시 (`/ignore:4099`).
- **PostBuildEvent**: `Resources\*` 디렉터리 트리를 통째 출력으로 복사 (xcopy /E /I).

### 4-2. FbxLoader 구현
- 위치: [Engine/render/FbxLoader.h](../Engine/render/FbxLoader.h), [.cpp](../Engine/render/FbxLoader.cpp)
- 자유 함수 `fbx_loader::LoadFbx(device, absolutePath, defaultColor) -> unique_ptr<Mesh>` — ObjLoader 와 같은 패턴.
- 흐름:
  1. `FbxManager::Create` + RAII 가드 (함수 종료 시 Destroy → scene/importer 모두 자동 정리).
  2. `wchar_t → UTF-8` 변환 후 `FbxImporter::Initialize/Import`.
  3. `FbxAxisSystem::DirectX.ConvertScene` + `FbxGeometryConverter::Triangulate`.
  4. ParseNode 재귀 — `eMesh attribute` 노드의 polygon 을 단일 vertex/index 컬렉션에 append.
  5. AppendMesh 가 controlPoint 단위로 정점 push + polygon 단위로 normal/uv/머티리얼색 채움.
  6. Mesh R32 오버로드로 생성.

### 4-3. fbxsdk 의 snprintf 매크로 잡기
fbxarch.h 가 `#define snprintf _snprintf` 정의 — `std::snprintf` 호출이 깨짐. fbxsdk include 직후 `#undef snprintf` 로 복원.

### 4-4. main.cpp 통합
- `cubeMesh` → `mainMesh`, `LoadObj` → `LoadFbx`.
- 카메라 (0, 100, -300) + far plane 5000 + FoV 그대로 — Dragon cm 단위 스케일 대응.
- `freeCamera.SetMoveSpeed(100.0f)` — 단위 스케일 보정.
- 회전 속도 1/4 로 감속.

### 4-5. Mesh R32 인덱스 오버로드
[Engine/render/Mesh.{h,cpp}](../Engine/render/Mesh.cpp) 의 ctor 한 개 추가. IndexBuffer 는 이미 `DXGI_FORMAT_R32_UINT` 지원하므로 ctor 만 위임.

## 5. 마주친 문제와 해결 ⚠ 필수 — 빠짐없이 기록

### 문제 1 — fbxsdk 의 `#define snprintf _snprintf` 가 `std::snprintf` 호출 깨뜨림
- **문제**: 컴파일 에러 `C2039: '_snprintf': 'std'의 멤버가 아닙니다`. 라인은 우리 `std::snprintf(buf, sizeof(buf), ...)` 호출.
- **원인**: `fbxsdk/core/arch/fbxarch.h:223` 의 `#define snprintf _snprintf` 매크로가 후속 모든 코드의 `snprintf` 토큰 (네임스페이스 무관) 을 치환.
- **해결**: fbxsdk include 직후 `#ifdef snprintf #undef snprintf #endif` 로 복원.
- **교훈**: 외부 라이브러리 매크로 오염은 항상 의심. 함수성 매크로는 `#undef` 안전.

### 문제 2 — LNK4099 PDB 경고 50+ 줄
- **문제**: 빌드 통과하지만 link 단계가 `libfbxsdk-md.pdb 못 찾음` 경고로 도배.
- **원인**: Autodesk 가 FBX SDK 배포 패키지에 PDB 미동봉. 정상 현상.
- **해결**: Client.vcxproj 의 Link 옵션에 `/ignore:4099` 추가.
- **교훈**: 외부 lib 가 PDB 없이 배포되는 건 흔함 — 4099 무시는 표준 대응.

### 문제 3 — Mesh R16 인덱스 한계 초과
- **문제**: Dragon 정점 수가 65535 초과 가능 — R16 인덱스로 narrowing 시 truncation.
- **해결**: Mesh 에 `uint32*` 오버로드 추가. IndexBuffer 의 R32_UINT 포맷 그대로 활용. ObjLoader 는 R16 유지 (큐브 정점 24개).
- **교훈**: 인덱스 폭은 정점 수의 함수. 자산 가정이 깨지면 ctor 분리.

### 문제 4 — Client.vcxproj 단독 빌드 시 SolutionDir 미해결
- **문제**: 솔루션 빌드는 통과하지만 `msbuild Client.vcxproj` 직접 실행 시 Engine 의 AdditionalIncludeDirectories `$(SolutionDir)Engine` 가 Client 폴더로 해석되어 include 실패.
- **원인**: ProjectReference 가 macro 컨텍스트를 전파 안 함. 직접 빌드 시 SolutionDir 가 호출 위치.
- **해결**: `/p:SolutionDir=d:\Things\portfolio_engine\` 명시.
- **교훈**: vcxproj 의 $(SolutionDir) 의존은 sln 빌드를 가정. 직접 빌드 또는 CI 환경에선 명시 전달.

### 문제 5 — Editor.vcxproj 의 `%(ClCompile.Filename)` 컨텍스트 오류
- **문제**: 솔루션 빌드 시 사용자가 작업 중인 Editor.vcxproj (ImGui 통합용) 에서 MSB4190 에러.
- **원인**: %(metadata) 함수는 ItemDefinitionGroup 안에서 사용 불가. 사용자가 작업중인 파일.
- **해결**: 본 단계는 Engine + Client 만 검증. Editor 는 사용자 자체 작업 영역 — 건드리지 않음.
- **교훈**: 다른 프로젝트가 작업 중일 땐 단일 vcxproj 직접 빌드로 검증 범위 한정.

### 문제 6 — FBX SDK 라이브러리 크기 800MB
- **문제**: third_party 복사 시 디스크 부담 + Git 추적 불가.
- **해결**: 학습 자료 절대 경로 직접 참조. third_party 이전은 학습 자료 정리 단계에 별도.
- **교훈**: 큰 외부 의존은 *복사* 가 항상 답이 아님. 환경 매크로/외부 경로 참조도 정석.

## 6. 결과 / 검증

- **빌드**: Debug + Release 둘 다 ExitCode 0. 산출물 `build/x64/{Debug,Release}/Client.exe`.
- **자산 복사**: `build/x64/Debug/Resources/FBX/Dragon.fbx` 자동 확인됨.
- **런타임 기대** (사용자 확인):
  - 런타임 로그 `[render] FBX loaded: vertices=N, indices=M (path: ...)` 출력.
  - Dragon 메시가 화면에 보임 (T-pose 정적, Y축 천천히 자전).
  - 면별 머티리얼 Kd 색 적용 (Dragon 머티리얼 분포에 따라 색조).
  - WASD/QE 로 카메라 이동, 우클릭 + 마우스로 시점 회전.

## 7. AI 협업 메모

- 학습 자료 (FBXLoader.cpp 569줄) 의 패턴을 우리 OOP 스타일로 압축 — 자유 함수 + 익명 ns 헬퍼. RAII 가드로 manager 라이프타임 안전.
- fbxsdk 의 매크로 충돌 (snprintf) 은 흔한 외부 라이브러리 이슈. 빠른 진단 — grep 으로 정의 위치 추적.
- 스키닝/애니메이션은 *셰이더 변경*이 선행돼야 의미 있음. 단계 분리 결정.

## 8. 다음 단계

본격 자산 파이프라인 확장:
- **셰이더 본 팔레트 cbuffer** + Skeleton/Animation 클래스 + FbxLoader 스키닝 확장 → Dragon 애니메이션 재생.
- **이미지 디코더** (stb_image / WIC) — diffuseTexName 으로부터 실제 텍스처 로드 → 8x8 체커보드 알베도 교체.
- **머티리얼 sub-draw 시스템** — 머티리얼별 PSO/SRV/cbuffer 분리.
- **third_party/FBX 이전** — 학습 자료 정리 시점에 SDK 위치 변경 + vcxproj 매크로 한 줄 갱신.

## 9. PPT 재료로 쓸 만한 포인트

- "Autodesk FBX SDK 직접 통합 — Manager/Scene/Importer 라이프타임 RAII"
- "controlPoint × polygonVertex × Material — FBX 의 4가지 mapping mode 대응 표"
- "Mesh R16 → R32 인덱스 오버로드 — 캐릭터 자산 도입과 인덱스 폭 진화"
- "fbxsdk 의 snprintf 매크로 — 외부 라이브러리 매크로 오염 진단/대응 패턴"
