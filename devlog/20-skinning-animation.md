# 20. Phase 4 — 스키닝 + 애니메이션 (Dragon이 살아 움직임) 🐉🦴

- **날짜**: 2026-05-18
- **관련 커밋**: (이 단계 커밋)
- **소요 시간**: 약 3시간
- **단계**: Phase 4 자산 파이프라인 (3/3)

---

## 1. 목표

Dragon.fbx 의 스키레톤 + 애니메이션 클립을 직접 추출해 Animator 로 본 팔레트를 매 프레임 계산, HLSL VS 에서 4-weight 스키닝 적용. 정적 T-pose → 움직이는 캐릭터.

펄어비스 포트폴리오에서 가장 강력한 시그널 — *셰이더-CPU-자산 파이프라인 전체*가 한 단계에 묶임.

## 2. 사전 컨텍스트

직전까지:
- 17단계: FBX 메시 + 머티리얼 색 추출 (정점 color 슬롯에 굽기).
- 19단계: 머티리얼 sub-draw + 머티리얼별 텍스처.
- Dragon 은 *정적 T-pose* 로 회전만. 스키레톤/클립 미사용.

본 단계: 셰이더부터 자산까지 전체 풀스택 변경.

## 3. 결정과 트레이드오프

### 20-1. Vertex 에 boneIndices(uint4) + boneWeights(float4) 직접 추가
- **결정**: `Mesh::Vertex` 가 모든 정점마다 4개 본 인덱스 + 4개 가중치 보유. 정점 44B → 76B.
- **후보**:
  - A) 스키닝 정점과 정적 정점 분리 (별도 PSO).
  - B) 모든 정점에 동일 슬롯 + 정적 정점은 weight 합 0.
- **선택 이유**: B — VS 에 weight 합 검사 분기 한 줄로 정적/스키닝 모두 단일 PSO 에서 처리. PSO 분기 회피 + 자산 단순화.
- **포기한 것**: 정점 32B 추가. 64B 페이지 경계는 보존 (76B). 정적 메시 (Cube/OBJ) 도 같은 정점 포맷 — 약간 낭비지만 단일 PSO 이점이 큼.

### 20-2. HLSL `bones[128]` cbuffer + b1 root descriptor
- **결정**: `cbuffer BonePalette : register(b1) { row_major float4x4 bones[128]; };` 고정 크기 + b1 CBV root descriptor.
- **후보**:
  - A) StructuredBuffer<float4x4> (가변 크기, SRV table).
  - B) 고정 크기 cbuffer (root descriptor).
- **선택 이유**: B — 본 수가 캐릭터별로 다르긴 하지만 128 이면 일반 캐릭터 다 수용. CBV root descriptor 는 디스크립터 테이블 비용 없이 직접 GPU address.
- **포기한 것**: 128 본 초과 메시 (대형 군중/대형 캐릭터). 그때는 StructuredBuffer 마이그레이션.

### 20-3. Animator 는 nearest-frame 샘플 (보간 X)
- **결정**: 현재 시간 → `frameIdx = floor(t01 * keyFrameCount)` → 그 키프레임 transform 직접 사용.
- **후보**: Lerp(position) + Slerp(rotation) 보간.
- **선택 이유**: 본 단계는 *파이프라인 검증* 우선. FBX SDK 가 1초당 N프레임 균등 추출 → 시간 해상도 충분. 보간은 다음 단계.
- **포기한 것**: 60FPS 미만에서 약간 떨림. 화면 변화 큰 게임 카메라/모션 블러로 가려짐.

### 20-4. FBX 키프레임 추출 — 전체 프레임 미리 계산
- **결정**: LoadKeyframe 가 클립의 모든 프레임을 EvaluateGlobalTransform → memory 에 보관. 런타임은 lookup 만.
- **후보**: 런타임에 EvaluateGlobalTransform 호출 (필요 시점만).
- **선택 이유**: EvaluateGlobalTransform 은 FBX SDK 내부 변환 비싸 (~수십 μs). 로딩 시간 ↑ 허용 + 런타임 0 비용.
- **포기한 것**: 메모리 — 60FPS × 5초 × 50본 × 64B = 960KB. 캐릭터당. 본격 게임 자산은 별도 압축 포맷 (SQT + delta 인코딩) 도입 시점.

### 20-5. FbxAMatrix → XMFLOAT4X4 transpose 정책
- **결정**: 변환 시 1회 transpose. 결과는 모두 row-major. HLSL `mul(rowVec, mat)`.
- **이유**: FBX SDK 는 column-major. DirectXMath / HLSL row_major 와 충돌. transpose 를 *경계 한 곳* (FbxLoader 의 `ConvertMatrix`) 으로 모음.
- **포기한 것**: 없음.

### 20-6. Animator palette 공식 = offsetMatrix × animatedGlobal
- **결정**: `palette[i] = offsetMatrix[i] × animatedGlobal[i]` (row-major mul). HLSL `mul(localPos, palette[b])`.
- **이유**: 학습 자료 패턴 차용 — `g_final[i] = mul(g_offset[i], matBone)`. offsetMatrix 는 inverse bind pose (정점을 본 로컬로 가져감), animatedGlobal 는 본의 현재 글로벌 변환 → 합쳐서 "정점이 어디로 움직이는가" 의 직접 표현.

### 20-7. LH 변환 reflect 행렬
- **결정**: Y-Z swap reflect 행렬을 offsetMatrix + keyFrame 양쪽에 적용. `matOffset = matReflect × matOffset × matReflect`.
- **이유**: FBX 의 좌표계 (RH/Maya) 와 D3D LH 가 다름. ConvertScene 으로 자동 변환되지 않는 cluster transform 은 수동 reflect.
- **포기한 것**: 다른 좌표계 자산은 reflect 매트릭스 다른 형태 — 우리는 학습 자료의 Maya→D3D 케이스만.

### 20-8. 정적 모델도 같은 셰이더 — weight 합 분기로 통과
- **결정**: 셰이더에 `if (weightSum > 0.0001)` 분기 — 합이 0 이면 스키닝 skip, 정점 그대로.
- **이유**: OBJ/Cube/정적 FBX 도 같은 PSO 로 그릴 수 있음. main 이 `loaded.skeleton == nullptr` 케이스에 identity palette 한 번만 채워 cbuffer 업로드.
- **포기한 것**: 정적 메시도 cbuffer 8KB 한 번 업로드 — 비용 미미.

## 4. 작업 내용

### 4-1. Vertex 확장
- [Engine/render/Mesh.h](../Engine/render/Mesh.h): `Vertex` 에 `XMUINT4 boneIndices` + `XMFLOAT4 boneWeights` 추가. 76B.

### 4-2. PSO 입력 레이아웃
- [Engine/render/PipelineState.cpp](../Engine/render/PipelineState.cpp): `BLENDINDICES (R32G32B32A32_UINT, offset 44)` + `BLENDWEIGHT (R32G32B32A32_FLOAT, offset 60)` 추가.

### 4-3. HLSL — 스키닝 VS + 본 팔레트 cbuffer
- [shaders/HelloTriangle.hlsl](../shaders/HelloTriangle.hlsl):
  - `cbuffer BonePalette : register(b1) { row_major float4x4 bones[128]; }`
  - `VSInput` 에 `uint4 boneIndices : BLENDINDICES; float4 boneWeights : BLENDWEIGHT`.
  - VS 스키닝: weight 합 검사 분기 → `skinnedPos = Σ w[i] * mul(float4(pos, 1.0), bones[bi[i]])`. unroll 4.

### 4-4. RootSig 확장
- [Engine/render/RootSignature.{h,cpp}](../Engine/render/RootSignature.cpp): `Desc::cbvB1Vertex` 추가. ctor 가 [0]=b0, [1]=b1, [2]=t0 table 순서로 파라미터 구성.

### 4-5. Skeleton / AnimClip / Animator
- [Engine/render/Skeleton.{h,cpp}](../Engine/render/Skeleton.cpp): bones 컬렉션 + `FindIndex(name)`.
- [Engine/render/AnimClip.h](../Engine/render/AnimClip.h): `bonesKeyFrames[boneIdx][frameIdx]` 구조 + duration.
- [Engine/render/Animator.{h,cpp}](../Engine/render/Animator.cpp):
  - ctor: skeleton + clip 참조 + palette identity 초기화.
  - Update(dt): 시간 진행 (루프) + 본별 nearest 키프레임 → `palette[b] = offset × kf.transform`.

### 4-6. FbxLoader 스키닝 추출
- [Engine/render/FbxLoader.cpp](../Engine/render/FbxLoader.cpp) 재작성:
  - `LoadBonesRec` — eSkeleton attribute 노드를 트리 순서로 push (parentIdx 추적).
  - `LoadAnimationInfo` — 모든 AnimStack → ClipMeta (name/start/end/timeMode + 빈 keyFrames).
  - `LoadAnimationData` (per mesh):
    1. 메시의 eSkin deformer 처리.
    2. cluster 별: LoadBoneWeight (controlPoint 단위 누적) + LoadOffsetMatrix (reflect + Inverse + transpose) + LoadKeyframe (모든 clip, 전체 frame, EvaluateGlobalTransform + reflect + transpose).
  - `AppendMesh` 가 스키닝 결과 (`accumWeights` 정규화) → `vertex.boneIndices/Weights` 채움.
- LoadFbx 결과를 `LoadedFbxModel { mesh, skeleton, clips }` struct 로 변경.

### 4-7. main.cpp 통합
- RootSig.Desc.cbvB1Vertex = true.
- `LoadedFbxModel loaded = LoadFbx(...)` + `animator = make_unique<Animator>(*loaded.skeleton, *loaded.clips[0])` (둘 다 있으면).
- BonePalette struct + per-frame ConstantBuffer N개.
- Identity palette pre-compute — Animator 없을 때 cbuffer 한 번 채움.
- 매 프레임:
  ```cpp
  if (animator) animator->Update(dt);
  palette = identityPalette;
  for (i in 0..min(animator.palette.size(), 128)) palette.bones[i] = animator.palette[i];
  bonePaletteCB.Update(&palette, sizeof(palette));

  list->SetGraphicsRootConstantBufferView(0, frameCB.GpuAddress());      // b0
  list->SetGraphicsRootConstantBufferView(1, bonePaletteCB.GpuAddress()); // b1
  mainMesh->DrawAll(list, /*rootParam*/2, fallbackAlbedo.SrvGpuHandle());  // t0 sub-draw
  ```

## 5. 마주친 문제와 해결 ⚠ 필수 — 빠짐없이 기록

### 문제 1 — FbxAMatrix transpose 정책 일관성
- **문제**: FBX SDK 는 column-major, DirectXMath 는 row-major (HLSL row_major 매핑). 어느 단계에서 transpose 하는지 일관 안 되면 메시가 뒤집힌 채 움직임.
- **해결**: 변환 시점 1회 (`ConvertMatrix(FbxAMatrix) → XMFLOAT4X4` row-major). 이후 모든 곱셈은 row-major 컨벤션. HLSL `mul(localPos, palette[b])` (rowVec × mat).
- **교훈**: 행렬 컨벤션 차이는 단일 경계 함수에 격리.

### 문제 2 — 본 인덱스 매핑 — bones push 순서와 cluster->GetLink() 이름 매칭
- **문제**: LoadBones 가 트리 순서로 push. cluster->GetLink() 는 임의의 노드 — 이름으로 찾아야 함.
- **해결**: Skeleton::FindIndex(name) 추가. cluster 처리 시 cluster->GetLink()->GetName() → FindIndex → bone idx.
- **교훈**: 자산 그래프의 두 표현 (트리 ↔ 인덱스) 매핑은 이름 lookup 이 안전. 향후 hash map 최적화 시점.

### 문제 3 — controlPoint 공유 본 가중치 누적
- **문제**: 한 controlPoint 가 여러 cluster 에 가중치 가짐 (서로 다른 본). 정점 1개당 최대 4개만 보관 → 큰 weight 만 남기기.
- **해결**: 학습 자료 BoneWeight::AddWeights 패턴 차용 — 큰 weight 순 정렬 + size 4 초과 시 pop_back. Normalize 로 합 1.
- **교훈**: 표준 4-weight 스키닝의 가중치 선별 + 정규화는 정해진 알고리즘.

### 문제 4 — RootSig 파라미터 인덱스 — sub-draw rootParam=2
- **문제**: b1 추가로 rootParam 순서가 [0]=b0, [1]=t0 → [0]=b0, [1]=b1, [2]=t0 로 변경. Mesh::DrawAll 에 전달하는 rootParam 도 1 → 2 갱신 필요.
- **해결**: main 에서 DrawAll 호출 시 hardcoded 2 전달. RootSig 에서 파라미터 순서를 명세에 명시.
- **교훈**: RootSig 의 파라미터 인덱스는 *순서 의존* — 변경 시 모든 SetGraphicsRoot* 호출 인덱스 갱신.

### 문제 5 — Skeleton/Animator/AnimClip 의 라이프타임
- **문제**: Animator 가 Skeleton/AnimClip 참조 — LoadedFbxModel 의 unique_ptr 가 살아있어야.
- **해결**: main 이 loaded 객체를 함수 끝까지 보관. animator 는 loaded 의 멤버에 참조만.
- **교훈**: non-owning 참조는 owning 객체의 라이프타임 보장이 prerequisite. 호출자가 책임.

### 문제 6 — 정적 모델도 본 팔레트 cbuffer 필요
- **문제**: 스키레톤/클립 없는 모델도 b1 root descriptor 가 set 되어야 함. 안 set 되면 D3D12 Debug Layer 에러.
- **해결**: identity palette 한 번 만들어 cbuffer 채움 → 매 프레임 같은 값으로 update. weight 합 0 정점은 셰이더가 통과 시킴.
- **교훈**: RootSig 슬롯은 "비어있을 수" 없음 — 사용 안 해도 dummy 데이터 필수.

### 문제 7 — BonePalette cbuffer 8KB — frame slot 마다 별도
- **문제**: per-frame cbuffer 두 개 (FrameConstants 192B + BonePalette 8KB). frame slot 2 → 16KB.
- **해결**: 그대로 진행. GPU 메모리 16KB 무시 가능.
- **교훈**: in-flight cbuffer 는 N배 메모리 — 단, GPU 메모리는 GB 단위라 KB 는 무관.

### 문제 8 — Animator.Palette 의 N 본 vs 셰이더 MAX_BONES=128
- **문제**: Skeleton 본 수 < 128 이면 cbuffer 의 나머지 슬롯은 garbage. 셰이더가 잘못된 인덱스 접근 시 garbage matrix 사용.
- **해결**: identityPalette 로 모든 128 슬롯 identity 초기화 → animator 채울 슬롯만 덮어쓰기. 잘못된 인덱스 접근해도 정점 그대로 통과.
- **교훈**: 고정 크기 배열 cbuffer 는 unused slot 의 *valid default* 보장 — 디버깅 표면 감소.

## 6. 결과 / 검증

- **빌드**: Debug + Release ExitCode 0.
- **런타임 기대** (사용자 확인):
  - 로그: `[render] FBX loaded: vertices=N, materials=M, indices=K, bones=B, clips=C`.
  - Dragon 이 *애니메이션 재생* (걷기/날기 등 클립 0 의 동작 자동 반복).
  - 머티리얼별 텍스처 + 스키닝 조합 — 본격 캐릭터 렌더.
  - 정적 메시 (없지만 OBJ 큐브로 테스트 가능) 도 같은 PSO 로 통과.

## 7. AI 협업 메모

- 본 단계는 *셰이더 + CPU 자료구조 + 자산 파이프라인* 전 레이어가 한 번에 변경. 의존 순서를 잘 잡지 못하면 디버깅 표면 폭증.
- 변경 순서: ① 정점/입력레이아웃/HLSL (셰이더 호환) → ② RootSig (b1 추가) → ③ Skeleton/AnimClip/Animator (자료구조) → ④ FbxLoader (자산 → 자료구조 변환) → ⑤ main (런타임 통합). 각 단계가 독립 빌드 가능.
- 학습 자료의 FBXLoader.cpp 569줄 → 우리 ~600줄 (텍스처/스키닝 모두 합침). 차용한 패턴: BoneWeight 4-cap, LoadBones 트리 재귀, cluster offset reflect, EvaluateGlobalTransform per-frame.

## 8. 다음 단계

본 단계로 Phase 4 자산 파이프라인 완결 (WIC + sub-draw + 스키닝/애니메이션).

후속 큰 단계 후보:
- **본 보간** (Lerp/Slerp) — Animator 의 nearest 샘플 → 매끄러운 재생.
- **머티리얼별 PSO** — alpha mode / two-sided / 다중 셰이딩 모델.
- **normal/specular map** — Material 슬롯 확장 + HLSL TBN.
- **그림자맵** — DSV + shadow VS/PS.
- **클립 블렌딩 / 전이** — Animator 가 두 클립 weighted blend.
- **ResourceManager** — 전역 텍스처/머티리얼/메시 캐시.
- **씬 / GameObject / Transform** — 다중 객체 + 게임 로직.

## 9. PPT 재료로 쓸 만한 포인트

- "Dragon 의 풀스택 — 셰이더 (b1 cbuffer + 4-weight VS) + CPU (Skeleton/AnimClip/Animator) + 자산 (FBX SDK cluster/keyframe) 한 번에"
- "FbxAMatrix 의 column-major → DirectXMath row-major — 단일 경계 함수 transpose 정책"
- "정적/스키닝 단일 PSO — weight 합 검사 한 줄로 분기"
- "RootSig 파라미터 순서의 명세 — b0/b1/t0/s0 매핑"
- "본 가중치 4-cap + 정렬 + 정규화 — 표준 스키닝 알고리즘 직접 구현"
