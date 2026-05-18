# 19. Phase 4 — 머티리얼 sub-draw 시스템 (FBX 머티리얼별 텍스처) 🎭

- **날짜**: 2026-05-18
- **관련 커밋**: (이 단계 커밋)
- **소요 시간**: 약 1.5시간
- **단계**: Phase 4 자산 파이프라인 (2/3)

---

## 1. 목표

Dragon.fbx 의 다중 머티리얼을 각자 자기 텍스처/색으로 렌더. 한 메시 = 단일 DrawIndexed → 한 메시 = N sub-draw (머티리얼별 SRV 교체 + DrawIndexed).

## 2. 사전 컨텍스트

- 17단계까지 모든 polygon 을 단일 sub-mesh 로 concat. 머티리얼 색은 정점 color 슬롯에 굽기 (controlPoint 공유로 일부 손실).
- 18단계에서 WIC 디코더 도입 — 단일 알베도 (Leather.jpg) 적용. Dragon 머티리얼의 자체 텍스처는 미사용.
- 본 단계: 머티리얼 sub-draw + 머티리얼별 텍스처 자동 매핑.

## 3. 결정과 트레이드오프

### 19-1. Mesh = 단일 VB + 다중 SubMesh
- **결정**: `Mesh` 가 `vector<SubMesh>` 보유. `SubMesh = { IndexBuffer, Material }`. 정점은 공유.
- **후보**:
  - A) Mesh N개 (머티리얼당 한 Mesh) — 정점 컬렉션 중복 → GPU 메모리 낭비.
  - B) 단일 VB + 다중 IB + Material — 우리 선택.
  - C) Mesh + 외부 Material 리스트 분리 — 호출자가 IB-Material 매핑 관리.
- **선택 이유**: B — Dragon 같은 메시는 정점 공유가 자연 (한 정점이 머티리얼 경계 무관). 머티리얼 분리는 *인덱스만*. 호출 측 단순.
- **포기한 것**: SubMesh 마다 별도 정점 변형 (예: 부분 변환) 불가. 본격 LOD/인스턴싱 도입 시 별도.

### 19-2. Material 은 plain struct (POD-ish)
- **결정**: `Material { name, diffuseColor, shared_ptr<Texture>, SRV GPU handle }`. 메서드 없음.
- **이유**: 머티리얼은 데이터 컨테이너. 셰이딩 모델은 셰이더 + RootSig 가 결정. PSO 선택 같은 정책은 *Material 외부 시스템*.
- **포기한 것**: 머티리얼 instancing (cbuffer 공유) 같은 최적화. 단순함 우선.

### 19-3. FbxLoader 시그니처 변경 — queue/list/srvHeap 인자 추가
- **결정**: `LoadFbx(device, queue, list, srvHeap, path, defaultColor)` — 텍스처 로드 + Texture 생성 + SRV 등록까지 함수 내부에서 완료.
- **후보**: FbxLoader 가 머티리얼 메타데이터만 반환, 호출자가 텍스처 로드.
- **선택 이유**: 텍스처 경로는 *FBX 의 책임* — 호출자가 알 필요 없음. 한 함수 호출로 "FBX 파일 → 즉시 렌더 가능 Mesh" 의 자연스러운 추상화.
- **포기한 것**: 텍스처 로드 정책 (캐시/비동기/품질) 외부화. 향후 ResourceManager 도입 시 분리.

### 19-4. TextureCache — 같은 텍스처 중복 디코드 회피
- **결정**: 함수 내부 `unordered_map<string, shared_ptr<Texture>>` 캐시. 같은 상대 경로면 재사용.
- **이유**: 한 FBX 안에 같은 텍스처를 여러 머티리얼이 공유하는 경우 흔함. JPG 디코드 + GPU 업로드는 비싸므로 1회만.
- **포기한 것**: 함수 호출 간 캐시 (LoadFbx 마다 새 캐시). 향후 전역 ResourceManager 도입 시 통합.

### 19-5. 폴백 SRV — main 의 fallbackAlbedo
- **결정**: FbxLoader 에서 텍스처 로드 실패 시 Material.albedoTexture = nullptr → Mesh::DrawAll 이 defaultSrvGpu 사용. main 이 Leather.jpg 를 폴백으로 등록.
- **후보**: FbxLoader 가 자체 1×1 흰색 텍스처 생성.
- **선택 이유**: 폴백 *선택* 은 애플리케이션 정책. 디버깅 시 회색/체커보드/Leather 등 자유 교체.
- **포기한 것**: 자체 완결성. 호출자가 폴백 SRV 를 반드시 준비해야 함.

### 19-6. 단일 PSO 유지 — 머티리얼별 PSO 분기 X
- **결정**: 본 단계는 모든 머티리얼이 같은 PSO + 같은 RootSig 사용.
- **이유**: 현 셰이더 (HelloTriangle.hlsl) 은 1종 — Phong + diffuse. alpha mode / two-sided / PBR 같은 분기 미존재.
- **향후**: 셰이딩 모델 다양화 시 머티리얼 → PSO 매핑 시스템. 본 단계의 sub-draw 구조에 자연 확장.

## 4. 작업 내용

### 4-1. Material 클래스 신규
- 위치: [Engine/render/Material.h](../Engine/render/Material.h)
- POD-style. cpp 없음. 본 단계는 4 필드 (name/diffuseColor/albedoTexture/albedoSrvGpu).

### 4-2. Mesh 재설계
- 위치: [Engine/render/Mesh.{h,cpp}](../Engine/render/Mesh.cpp)
- 3개 ctor:
  1. `(device, vertices, count, uint16* indices, count)` — 단일 머티리얼 R16. OBJ/Cube 폴백.
  2. `(device, vertices, count, uint32* indices, count)` — 단일 머티리얼 R32.
  3. `(device, vertices, count, vector<vector<uint32>>& subIndices, vector<shared_ptr<Material>>& subMaterials)` — 다중 머티리얼 R32. FBX 경로.
- `BindVertexBuffer(list)` + `DrawAll(list, rootParam, defaultSrv)` 노출.
- DrawAll 내부:
  ```cpp
  for (SubMesh& sm : m_subs) {
      const auto srv = (sm.material && sm.material->albedoTexture) ? sm.material->albedoSrvGpu : defaultSrvGpu;
      list->SetGraphicsRootDescriptorTable(rootParam, srv);
      sm.ib->Bind(list);
      list->DrawIndexedInstanced(sm.ib->IndexCount(), 1, 0, 0, 0);
  }
  ```

### 4-3. FbxLoader 재설계
- 위치: [Engine/render/FbxLoader.{h,cpp}](../Engine/render/FbxLoader.cpp)
- 시그니처: `LoadFbx(device, queue, list, srvHeap, path, defaultColor)`.
- ParseNode 가 머티리얼을 outMaterials 에 누적 + sub-indices 를 머티리얼 인덱스별로 분리.
- 머티리얼별 `ExtractDiffuseTexRelative` → 상대 경로 추출 → .fbm 폴더에서 `LoadDiffuseTexture`:
  1. TextureCache lookup → 중복 회피.
  2. `image_loader::LoadImage` → RGBA8.
  3. `Texture` 생성 + `CreateSrv(device, srvHeap)`.
  4. Material 에 `albedoTexture` + `albedoSrvGpu` 저장.
- 텍스처 로드 실패 (.fbm 폴더 부재 / 파일 부재) 는 fatal 아님 — 로그 후 폴백.

### 4-4. main.cpp 통합
- SrvHeap capacity 4 → 32 (Dragon 머티리얼 + 폴백 여유).
- `fallbackAlbedo` (Leather.jpg) 를 SrvHeap 슬롯 0 에 등록.
- `mainMesh = LoadFbx(..., srvHeap, ...)` — 슬롯 1+ 자동 사용.
- 매 프레임:
  ```cpp
  mainMesh->BindVertexBuffer(list);
  mainMesh->DrawAll(list, 1, fallbackAlbedo.SrvGpuHandle());
  ```

## 5. 마주친 문제와 해결 ⚠ 필수 — 빠짐없이 기록

### 문제 1 — Mesh::SubMesh 컨테이너 안의 unique_ptr / shared_ptr 혼용
- **문제**: IndexBuffer 는 비이동 → SubMesh 안에 unique_ptr. Material 은 머티리얼 공유 가능 (캐시) → shared_ptr.
- **해결**: `struct SubMesh { unique_ptr<IndexBuffer>; shared_ptr<Material>; }`. SubMesh 자체는 move-only — vector reserve + emplace_back 으로 안전.
- **교훈**: 비이동 멤버 + 공유 멤버의 컨테이너는 vector + move semantics 로 자연 해결.

### 문제 2 — SubMesh 폴백 머티리얼 — 어디서 채울 것인가
- **문제**: 단일 머티리얼 ctor (OBJ/Cube) 호출 시 Material 이 nullptr 이면 DrawAll 의 defaultSrvGpu 가 어떻게 동작하는지.
- **해결**: 단순 ctor 가 내부에서 폴백 Material 1개 생성. DrawAll 은 머티리얼이 nullptr 인 경우와 albedoTexture 가 nullptr 인 경우를 동일 분기로 처리 (defaultSrvGpu fallback).
- **교훈**: 호출 측이 "머티리얼 신경 안 써도 되는 경로" 와 "신경 쓰는 경로" 를 같은 인터페이스로 노출.

### 문제 3 — 머티리얼 글로벌 인덱스 매핑
- **문제**: 한 FBX 트리에 여러 mesh node + 각 node 마다 자체 머티리얼 리스트. polygon 의 머티리얼 subset 인덱스는 *그 mesh node 의 로컬* — 글로벌 머티리얼 컬렉션에서 어떻게 매핑.
- **해결**: `globalMatBase = outMaterials.size() (push 전)` 저장 후 `globalMatIdx = globalMatBase + localMatIdx`. mesh node 마다 누적.
- **교훈**: 트리 재귀 + 글로벌 인덱스 합치기 — base offset 패턴.

### 문제 4 — .fbm 폴더 경로 추정
- **문제**: FBX 의 임베디드 텍스처는 import 시 `<fbx_stem>.fbm` 폴더에 자동 추출. FBX SDK 가 어디에 두는지 명세 모호.
- **해결**: 학습자료 패턴 차용 — `<fbx 부모>\<stem>.fbm\<텍스처 파일명>`. 실패해도 fatal 아님 (로그 + 폴백).
- **교훈**: 외부 도구가 만드는 사이드 파일은 *경로 규약* 으로 처리 + fail-safe.

### 문제 5 — SRV 힙 capacity
- **문제**: Dragon 의 머티리얼 수가 미지수. 4 슬롯은 부족할 가능성.
- **해결**: capacity 32 — 사실상 모든 일반 캐릭터 메시 수용. 향후 동적 grow 도입 시 SrvHeap 자체 발전.
- **교훈**: 자산 크기 미지수일 땐 안전 마진 부여.

### 문제 6 — Material::albedoSrvGpu 가 0 일 수 있음
- **문제**: 텍스처 없는 머티리얼은 albedoSrvGpu = {0}. SetGraphicsRootDescriptorTable(rootParam, {0}) 호출은 정의되지 않은 동작.
- **해결**: DrawAll 이 `albedoTexture == nullptr` 분기로 defaultSrvGpu 사용. albedoSrvGpu 는 그 분기 안 들어가면 참조 X.
- **교훈**: 두 필드 (texture + srv) 의 *valid invariant* 를 한 곳 (DrawAll) 에서 일관 처리.

## 6. 결과 / 검증

- **빌드**: Debug + Release ExitCode 0.
- **런타임 기대** (사용자 확인):
  - 로그: `[render] FBX loaded: vertices=N, materials=M, indices=K`.
  - 머티리얼별 텍스처 로드 로그 (성공/실패).
  - Dragon 표면이 *머티리얼별 다른 텍스처* 또는 (텍스처 없으면) Leather 폴백.

## 7. AI 협업 메모

- Mesh 의 ctor 3종 — 호출 측 코드 변화 없이 폴리모피즘. ObjLoader 등 기존 경로 그대로 동작.
- FbxLoader 의 책임이 늘어남 (메시 → 메시 + 머티리얼 + 텍스처). 단일 함수 호출 = 즉시 렌더 가능 결과의 추상화 — Mesh::DrawAll 한 줄로 모든 sub-draw 완료.

## 8. 다음 단계

- **스키닝 + 애니메이션** — Vertex 에 bone idx/weight 추가 + HLSL 본 팔레트 cbuffer + Skeleton/AnimClip/Animator + FbxLoader 스키닝 확장 → Dragon 이 살아 움직임.

후속:
- 머티리얼별 PSO 분기 (alpha mode / two-sided / PBR).
- ResourceManager — 전역 텍스처/머티리얼 캐시.
- Normal/Specular/Roughness map.

## 9. PPT 재료로 쓸 만한 포인트

- "단일 VB + 다중 SubMesh — FBX 다중 머티리얼의 자연 표현"
- "FbxLoader 한 함수 호출 = 즉시 렌더 가능 — 텍스처 로드/SRV 등록까지 흡수한 추상화"
- "TextureCache — 머티리얼 공유 텍스처의 디코드/업로드 1회 보장"
