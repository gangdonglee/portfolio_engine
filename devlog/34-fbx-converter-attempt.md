# 34. FbxConverter CLI — 시도와 한계

- **날짜**: 2026-05-22
- **관련 커밋**: (이 작업 후 커밋)
- **소요 시간**: ~3시간
- **단계**: Phase 5 — Asset Pipeline (시도 단계)

---

## 1. 목표

importTransform 을 *런타임 합성* 이 아닌 *FBX 자산 자체에 baked-in* 으로 처리해서 우리 엔진이 깨끗하게 로드하도록 하는 *FbxConverter CLI* 도구 제작.

사용 의도:
```
FbxConverter.exe -i "Resources/FBX/X Bot.fbx" \
                 -o "Resources/FBX/X Bot.normalized.fbx" \
                 --scene "assets/Scenes/01_xbot.scene.json" \
                 --mesh-name XBot
```

결과 normalized FBX 는 scene JSON 에서 importTransform 없이 정상 표시되어야 한다.

## 2. 사전 컨텍스트

[devlog 32](32-import-transform-and-walk-input.md) 의 importTransform 인프라가 X-Bot 의 X-90° 누운 자세를 런타임 합성으로 정자세화. 사용자 요청 — *코드 변환 없이 자산 자체* 로 baked-in 하는 방향. 새 milestone.

## 3. 시도와 실패

### 3-1. intermediate node 삽입 (1차 시도)

- **접근**: root 와 기존 자식 사이에 *ImportTransform LclTransform* 보유 노드 삽입. FBX SDK 의 `EvaluateGlobalTransform` 이 자동으로 모든 mesh/bone/animation 에 반영.
- **결과**: 시각상 변화 없음. X-Bot 이 baseline 의 누운 자세 그대로.
- **원인**: 우리 FbxLoader 의 `matFromNode.Inverse() × linkNode.GlobalTransform` 패턴이 *mesh node 의 GlobalTransform 으로 normalize*. intermediate 의 효과가 *currentBone 계산에서 상쇄*.

### 3-2. mesh CP + cluster TM/TLM 둘 다 prepend (2차 시도)

- **접근**: mesh control point 에 `importMatrix.MultT(cp)`, cluster TransformMatrix/TransformLinkMatrix 양쪽에 `importMatrix * old`.
- **결과**: 마찬가지로 시각 변화 없음.
- **원인**: `offsetMatrix = TLM^-1 × TM` 의 *양쪽 prepend* 가 수학적으로 상쇄 (`(import × TLM)^-1 × (import × TM) = old offsetMatrix`).

### 3-3. mesh CP + cluster TM only prepend (3차 시도)

- **접근**: mesh CP 변환은 그대로 + cluster TM 만 import prepend, TLM 은 변경 X.
- **결과**: **mesh 가 깨짐**. (사용자 보고)
- **원인 (재분석)**: bind pose 에서 `bone_palette × vertex_new = (TM^-1 × import × TM) × (import × vertex_old) = TM^-1 × import × TM × import × vertex_old`. TM=identity 가정 시 *import² (이중 적용)*. 우리 FbxLoader 의 normalize 패턴과 baked-in vertex 가 *함께 동작하는 정확한 수식* 을 찾기 어려움.

## 4. 결론 — 구조적 한계

우리 FbxLoader 의 mesh-local normalize 패턴 (`matFromNode.Inverse() × linkNode.GlobalTransform` + `matReflect Y/Z swap` + `ConvertVec3` Y/Z swap) 은 *bind pose 의 mesh-world 좌표계가 identity 임을 가정* + *bone bind pose 를 mesh-local 으로 normalize*. 이 구조 하에서 *FBX 자산 자체의 import baked-in* 은:

- vertex 와 bone palette 의 *별도 변환 적용 시점* 차이로 정합 어려움
- mesh node 의 GlobalTransform 이 normalize 분모라 *어떤 root LclTransform 변경도 상쇄*
- cluster TM/TLM 양쪽 변환은 `offsetMatrix = TLM^-1 × TM` 에서 자동 상쇄

**근본 해결 옵션**:
1. **FbxLoader 의 normalize 패턴 수정** — `matFromNode.Inverse()` 적용 제거. 단, *기존 자산 호환성* 위험.
2. **DCC 도구 (Blender/Maya) 로 자산 자체 정규화** — 표준 자산 파이프라인. CLI 자동화 안 됨.
3. **mesh import 시점에 mesh node 의 GlobalTransform 도 vertex 에 적용** — FbxLoader 가 *mesh-world 좌표계* 사용 + cluster transform 도 호환되게.

## 5. 현 상태

- **Tools/FbxConverter/{FbxConverter.vcxproj, main.cpp}**: 신규 CLI 빌드 타겟 (sln 등록). 현재는 *진단 dump 모드* — FBX hierarchy 만 출력하고 변환은 No-op.
- **시도 코드 (BakeIntoMesh, BakeIntoClusters)**: 정확한 변환 식을 찾지 못해 제거 (commit 이력에 보존).
- **scene JSON / FbxLoader**: 5-M2 완료 baseline 으로 복원 (런타임 importTransform 합성).

CLI 인프라는 *향후 확장 가능한 발판* 으로 유지. 변환 로직은 후속 milestone 에서 *FbxLoader 측 코드 수정* 과 함께 재시도.

## 6. 마주친 문제와 해결 ⚠

### 6-1. 자동 시각 검증 도구 부재

- *증상*: 변환 결과의 시각 정합성 확인을 위해 *사용자 시각 보고* 의존. 자동화 사이클에서 *내가 확인 보고* 받을 수 없음.
- *영향*: 시도-실패-재시도 사이클이 길어지고 *수학적 분석* 만으로 변환 정합 검증 필요. 한계 봉착.
- *대응*: 진단 로그 (first bone GlobalT, mesh CP bake 전/후) 로 *수치 측정*. 다만 시각 정합성은 결국 사용자 확인 필요.

### 6-2. 수학 분석의 1차 오류

- *시도 3 (mesh CP + cluster TM only)* 의 *bind pose 정합* 분석에서 *bone palette = identity* 가정을 잘못. 실제로는 `bone_palette = TM^-1 × import × TM` 으로 *import 가 conjugation 형태로 남음*. *TM=identity 가정* 이 일반적이지 않음.
- *교훈*: FBX 의 cluster TM/TLM 의 *실제 값* (보통 mesh world bind pose 가 identity 아님) 을 측정한 후 변환 식 정합 검증 필요.

## 7. 다음 단계

- **FbxLoader 의 normalize 패턴 수정** 또는 **Blender 자산 정규화 검토** (둘 중 결정).
- 그 전까지 importTransform 은 *런타임 합성* 으로 충분 동작.
- 5-M3 (Editor 그래프 UI) 로 우선 진행 후 자산 파이프라인 재시도.

## 8. AI 협업 메모

- *시각 검증을 자동화하지 못함* 이 사이클의 결정적 한계. 진단 로그 기반 *수치 검증* 만으로는 *bone palette × vertex* 같은 복합 식의 정합 판단 어려움.
- 다음에는 *작은 단위 시도 + 사용자 시각 확인* 사이클로 다시 갈 것. 한 번에 큰 baked-in 변환 시도는 정합 검증 비용이 큼.
