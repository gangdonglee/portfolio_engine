# 32. importTransform 인프라 + Idle/Walking 자산 정상화 + 1키 Walk

- **날짜**: 2026-05-22
- **관련 커밋**: (이 작업 후 커밋)
- **소요 시간**: ~6시간 (좌표계 진단 + 시행착오 포함)
- **단계**: Phase 5 — Animator 사용성 정비

---

## 1. 목표

- Mixamo X-Bot 메시가 baseline 에서 X축 -90° 회전된 채 누워있는 문제를 자산별 보정으로 해결.
- 보정을 `MeshInstance.transform` (게임 런타임 자유도) 와 분리된 `MeshInstance.importTransform` 필드로 처리.
- X-Bot 의 Walking 모션 진입을 단축키 `1` 로 명시화. `W` 매핑 제거.
- Mixamo 자산 (Walking, Idle) 의 root motion + 호흡 흔들 정상화.

## 2. 사전 컨텍스트

[31-debug-axes-camera-title.md](31-debug-axes-camera-title.md) 후 X-Bot 의 자세 점검 시:
- baseline (ConvertScene + Y/Z swap + matReflect 3중 조합) 상태에서 X-Bot 의 머리가 -Z, 발이 +Z, 얼굴이 -Y 인 *누운 자세*.
- Dragon 은 자산의 `LclR.z=-180` 이 3중 조합과 우연히 상쇄되어 정상 표시.
- X-Bot 의 X축 회전 baked-in (Mixamo Hips `GlobalR=(-179.3°, 0, 0)`) 이 우리 좌표계 변환과 결합되어 시각적으로 X-90 회전.

## 3. 결정과 트레이드오프

### 3-1. 자산별 보정의 적용 계층

- **결정**: `MeshInstance.importTransform` 필드를 추가하고, `SceneRuntime` 의 **world matrix 단계**에서 `inst.transform` 과 합성. FbxLoader 단의 vertex/bone palette baked-in 후처리는 두지 않는다.
- **후보**:
  - (A) FbxLoader 단에서 mesh CP + offsetMatrix + keyframe transform 에 conjugation 으로 baked-in.
  - (B) SceneRuntime 의 ComposeWorld 단계에서 `world = importAdjust * inst.transform` 합성.
  - (C) 인스턴스 `transform` 에 직접 baked-in.
- **선택 이유**: (A) 는 conjugation 식의 행렬 컨벤션 매칭이 미묘해 시행 시 mesh skinning 이 깨졌다. (C) 는 게임 런타임의 캐릭터 위치/회전 자유도를 막는다. (B) 는 vertex/bone palette 가 동일 world matrix 곱셈을 받으므로 깨질 위험 없이 자산만 정상화하고, inst.transform 은 자유.
- **포기한 것**: FbxLoader 단의 정밀한 baked-in 데이터 정규화. 대신 *런타임 합성* 의 단순성.

### 3-2. Walking root motion 처리

- **결정**: Mixamo 의 **In Place** 옵션 자산을 다운로드하여 교체. 코드 측에는 안전망으로 *root bone (Hips) translation-only lock* 후처리 유지.
- **후보**:
  - (A) Hips 의 4x4 transform 전체 lock (Unity Apply Root Motion=OFF 동등).
  - (B) Hips 의 translation 만 lock + 자산 자체를 In Place 로 받음.
- **선택 이유**: (A) 는 Hips 회전까지 lock 해 골반 swing 이 사라져 부자연. (B) 는 In Place 자산이면 translation 자체가 0 이라 lock 무영향 + 다른 root-motion 있는 자산이 들어와도 자동 처리.
- **포기한 것**: 자산 측 의존 — 진행 모션이 필요한 경우 별도 클립 받아야 함.

### 3-3. Idle 호흡 흔들

- **결정**: `Breathing Idle.fbx` 의 호흡 모션이 importTransform 의 X+90 회전 + Y=180 평행이동 효과와 결합해 시각적으로 Z 흔들로 증폭됨. 정적 `Idle.fbx` 클립으로 교체.
- **후보**: Spine/Chest 본까지 lock vs 정적 클립 교체.
- **선택 이유**: 정적 클립 교체가 *자산 자체의 의도된 모션* 을 존중. 본 단위 lock 은 Walking 의 자연스러움까지 손상.

## 4. 작업 내용

### 4-1. Scene 구조체 — importTransform 필드

- [Engine/scene/Scene.h](../Engine/scene/Scene.h): `MeshInstance.importTransform` 추가 (default identity). Unity 의 model importer rotation/scale 옵션과 동일 패턴.
- [Engine/scene/SceneSerializer.cpp](../Engine/scene/SceneSerializer.cpp): JSON 직/역직렬화 — identity 시 생략, 비-identity 시 저장.

### 4-2. SceneRuntime — world matrix 단계 합성

- [Client/SceneRuntime.cpp](../Client/SceneRuntime.cpp) `RecordDraw`:

```cpp
const XMMATRIX importAdjust = ComposeWorld(inst.importTransform);
const XMMATRIX instWorld    = ComposeWorld(inst.transform);
const XMMATRIX world        = importAdjust * instWorld;
```

- row-vector convention: import 먼저 (mesh-local 측), inst 다음 (게임 런타임 측).
- vertex 와 bone palette 가 같은 world matrix 를 받으므로 일관 변환. inst.transform 은 캐릭터 이동/회전 자유로 보존.

### 4-3. X-Bot scene 의 importTransform

[assets/Scenes/01_xbot.scene.json](../assets/Scenes/01_xbot.scene.json):
- `rotation = [0, 0.7071068, -0.7071068, 0]` — Hamilton product 로 X+90 (자세 정상화) ∘ Y+180 (정면을 +Z 향) 합성.
- `position = [0, 180, 0]` — 발이 floor (Y=0) 에 닿도록 들어올림.

### 4-4. FbxLoader — root motion lock 안전망

[Engine/render/FbxLoader.cpp](../Engine/render/FbxLoader.cpp) `LoadFbxAnimationOnly` 끝:
- base skeleton 의 "Hips" 부분 문자열 매칭 본 인덱스 검색 (mixamorig:Hips 등 prefix 흡수).
- 그 본의 모든 키프레임 translation 을 *첫 frame 값으로 lock*. rotation 은 유지 → 골반 swing 자연.

### 4-5. 1키 → Walk 입력 매핑

[Client/Application.cpp](../Client/Application.cpp) Tick:
- `1` → `Speed = 0.3` → Animator transition 임계 (>0.1) 통과 → Walk 상태로 전환.
- `W` 매핑 제거 (카메라 이동 키로만 유지).

### 4-6. 자산 교체

- `Resources/FBX/Idle.fbx` 추가 (Mixamo 정적 idle, Without Skin)
- `Resources/FBX/Walking.fbx` In Place 옵션으로 재다운로드
- [assets/Animators/xbot.animator.json](../assets/Animators/xbot.animator.json): Idle.motionClipPath 갱신

### 4-7. Fallback texture 변경

[Client/Application.cpp](../Client/Application.cpp):
- 기존: `Resources/Texture/Leather.jpg` (갈색 가죽 무늬가 X-Bot 같이 텍스처 없는 자산에 어색하게 입혀짐)
- 변경: 1×1 plain gray RGBA(200,200,200,255). 텍스처 있는 자산은 그대로 자기 텍스처 사용 — 영향 없음.

## 5. 마주친 문제와 해결 ⚠ 필수

### 5-1. FbxLoader 단 conjugation 후처리 시 mesh 깨짐

- *증상*: importMatrix 를 mesh CP, offsetMatrix, keyframe transform 에 `import * M * import^-1` conjugation 으로 baked-in 했더니 X-Bot 메시 자체가 깨졌다.
- *원인*: XMFLOAT4X4 (row-vector) ↔ FbxAMatrix (column-vector) 의 transpose 매핑이 직접 element 채우는 경로에서 FbxAMatrix 의 affine multiplication 컨벤션과 어긋났을 가능성. roundtrip self-check 는 OK 였지만 multiplication 결과는 mismatch.
- *해결*: 후처리 인프라 자체를 제거하고, SceneRuntime 의 world matrix 단계로 단순 합성 이동.

### 5-2. ConvertVec3 Y/Z swap 단독 제거 → Dragon 망가짐

- *증상*: 이중 변환 의심으로 swap 만 제거했더니 Dragon 까지 X-180 회전된 채 표시.
- *원인*: Dragon 의 자산별 `LclR.z=-180` 이 swap + ConvertScene 3중 조합과 우연히 상쇄되어 정상으로 보이던 것. swap 단독 제거 시 그 상쇄가 깨짐.
- *해결*: 좌표계 변환 3중 조합 baseline 복원 + 자산별 보정은 importTransform 으로 처리.

### 5-3. Hips matrix 전체 lock 시 골반 swing 사라짐

- *증상*: Walking 의 Z 진행이 본 rotation 누적으로 표현된다고 가정해 Hips 의 4x4 matrix 전체를 lock 했더니 골반이 정지된 어색한 걸음.
- *원인*: 진행 모션이 본 회전 누적이 아니라 자산 자체의 root motion (In Place 옵션 미적용) 이었음.
- *해결*: 자산을 Mixamo In Place 로 재다운로드 + Hips lock 을 translation-only 로 완화.

### 5-4. Breathing Idle 의 호흡 Z 흔들

- *증상*: Idle 자세에서 X-Bot 이 추 마냥 Z 축으로 흔들.
- *원인*: Mixamo Breathing Idle 의 정상 호흡 모션이 importTransform.X+90 회전 + Y=180 평행이동 효과로 회전 중심이 Y=180 으로 이동, 작은 본 회전이 큰 호로 증폭.
- *해결*: 호흡 없는 정적 Idle 클립으로 교체.

### 5-5. Fallback texture 가 갈색 가죽

- *증상*: Mixamo X Bot.fbm 폴더가 없는 Without-Skin 자산에 fallback 인 Leather.jpg 가 입혀져 어색.
- *해결*: 1×1 plain gray 텍스처로 fallback 교체.

## 6. 다음 단계 후보

- 5-M2 Blend Tree 1D
- 5-M3 Editor 그래프 UI (ImNodes vendored)
- 자산별 importTransform Editor UI (Inspector 패널)
- Walking 진행 시 root motion 으로 *실제 위치 이동* 처리 (게임플레이 단계)
