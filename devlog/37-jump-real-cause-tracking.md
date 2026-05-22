# 37. 점프 진짜 원인 추적 — 측정 식 버그 + skin matrix 부호 반전 발견

- **날짜**: 2026-05-22
- **관련 커밋**: (이 작업 후 커밋)
- **소요 시간**: ~3시간
- **단계**: Phase 5 — Animator 점프 정합 (재추적, 3번째 시도)

---

## 1. 목표

devlog 33/35/36 의 점프 보정 시도들이 모두 *우회 코드 (시간 기반 곡선, footY 자동 정렬)* 였다. 진짜 원인 파악:
- 자산에 진짜로 Hips translation 키프레임이 없는가?
- 있다면 왜 시각으로 점프 효과가 안 나오는가?

## 2. 사전 컨텍스트

[devlog 33](33-blend-tree-1d-and-jump-tuning.md), [35](35-imgui-animator-debug-and-foot-align.md), [36](36-jump-explicit-curve.md) 의 공통 가정: *Mixamo Without-Skin 자산은 bone translation track = 0*. 그래서 코드 측에서 *시각 점프 효과를 합성*. devlog 36 의 명시적 단계 곡선까지 적용했으나 사용자 시각으로 *도움닫기에서 공중에 뜬 듯*. 사용자가 *"리소스 문제냐 너가 못 맞추는 거냐"* 정확히 짚어줘서 *자산 측 검증* 으로 전환.

## 3. 결정과 트레이드오프

### 3-1. 자산 검증을 위한 측정 위치

- **결정**: FbxLoader 의 `LoadFbxAnimationOnly` 마지막에 *Hips bone 의 frame 별 raw translation* 진단 로그 추가.
- **후보**: FBX SDK 의 `EvaluateGlobalTransform` 직후 vs `ConvertMatrix` 적용 후. 본격 측정은 *코드가 실제 사용하는* `ConvertMatrix` 결과에서.
- **선택 이유**: *코드 측 데이터 흐름의 어느 단계에서 0 이 되는지* 추적하려면 *실사용 데이터* 가 우선.
- **포기한 것**: FBX SDK 의 *원본 데이터* 직접 검사 — 좀 더 깊은 추적은 다음 세션.

### 3-2. 측정 식 위치 — `m[3][0..2]` vs `m[0..2][3]`

- **결정**: 측정 식을 `m[0..2][3]` (col-major translation 위치) 으로 수정. *영구 변경*.
- **후보**: ConvertMatrix 의 transpose 자체를 제거 + 셰이더 컨벤션 통일.
- **선택 이유**: ConvertMatrix transpose 는 *셰이더/AnimatorRuntime 전체 체인과 정합* (셰이더의 `column_major float4x4` + `mul(row_vec, M)` 조합이 *마지막 열에 translation* 인 행렬을 정상 처리). transpose 자체를 건드리면 *시각 멀쩡한 부분까지 회귀*. 측정 식만 위치 정정.
- **포기한 것**: row-major / col-major 컨벤션 통일. 코드 전체의 일관성. *다음 세션 검토*.

### 3-3. 점프 곡선 코드 운명

- **결정**: Application 의 시간 기반 명시적 곡선 + footY 추적 + 관련 멤버 *전면 제거*.
- **후보**: 다음 세션 결과 의존으로 잠시 유지.
- **선택 이유**: 곡선 코드가 *잘못된 진단 위의 우회*. 진짜 원인 (좌표계 정합) 이 별도 작업이고, 그 사이에 *잘못된 우회가 계속 깔려있으면* 디버깅 노이즈. 진단 결과로 *자산 살아있음* 확정되었으니 *코드 측 합성을 즉시 제거* 가 정합.
- **포기한 것**: *다음 세션까지 시각적으로 점프가 나오는 상태* 유지. 현 상태는 *crouch 가 위로 떠 보이는 어색한 시각* 이지만 *진짜 원인 알고 있음*.

## 4. 작업 내용

### 4-1. FbxLoader 진단 — Hips raw translation range

[Engine/render/FbxLoader.cpp](../Engine/render/FbxLoader.cpp):
- `LoadFbxAnimationOnly` 의 XZ lock 안전망 *직전* 에 Hips bone 의 X/Y/Z span 진단.
- 좌표계 측정 식: `m[0..2][3]` (col 3 위치 = transpose 후 translation 저장 위치).

### 4-2. *측정 식 버그* 발견 — 1차 시도

처음 *m[3][0..2]* 위치에서 측정 → 모든 자산의 *X/Y/Z 모두 span=0.00*. *자산 한계 결론* 으로 다시 들어가는 듯. 그러나 *Walking 의 LeftFoot Y 도 0* 인 게 결정적 의심 (걷기는 발이 움직임).

`LeftFoot first kf 의 16 원소 4x4 dump` 추가:
```
[-0.971  0.151  0.187   16.984]
[-0.240 -0.615 -0.751   -4.509]
[ 0.002 -0.774  0.633  197.882]
[ 0.000  0.000  0.000    1.000]
```

→ **translation 이 *마지막 행* (`m[3][0..2]`) 이 아니라 *마지막 열* (`m[0..2][3]`)**. devlog 33/35 의 모든 측정이 *잘못된 위치* 에서 *0* 만 읽고 있었음.

### 4-3. 측정 위치 수정 후 재측정

`m[0..2][3]` 으로 측정 식 수정 후:

| 클립 | Hips Y span | 비고 |
|---|---|---|
| **Jump.fbx (mixamo.com, 71f)** | **14.26** (-9.40 → 4.86, peak @ f8) | 점프 Y 살아있음 |
| Walking.fbx | 3.06 | 정상 Y bob |
| Idle.fbx | 1.22 | 호흡 미세 |

**자산 한계 가설은 *완전히 틀렸음*.** devlog 33/35/36 의 모든 시도가 *없는 문제를 우회하기 위한 코드*. 자산은 처음부터 정상.

### 4-4. Hips Y 시계열 — 자산 모션 형태 확정

Jump.fbx (71 frame) 의 Hips Y per-frame:

| 단계 | frame 범위 | Y 변동 |
|---|---|---|
| 도움닫기 (crouch ↓) | 0 ~ 9 | -0.1 → -14.5 (최저) |
| 도약 + 활공 ↑ | 9 ~ 23 | -14.5 → +4.9 (peak) |
| 착지 squat | 23 ~ 30 | 4.9 → -7.8 |
| recovery | 30 ~ 70 | -7.8 → 0 |

**자산이 정확히 점프 4단계** 다 들어있음. devlog 33의 "78 frame, 자산이 2번 점프 baked" 도 *측정 식 버그* 위의 잘못된 해석 가능성. (현재는 71 frame 다른 자산이라 직접 비교 어려움.)

### 4-5. Application 점프 곡선 제거

[Client/Application.cpp](../Client/Application.cpp), [Client/Application.h](../Client/Application.h):
- devlog 36 의 시간 기반 명시적 곡선 (도움닫기 crouch / 활공 peak / 착지 squat) *전면 제거*.
- 관련 멤버 `m_jumpPeakHeight / m_crouchDepth / m_landingSquat / m_footBindY / m_footBindCaptured / m_bindCaptureTimer` 제거.
- ImGui 패널의 slider + footY/bindY 디버그 표시 제거.

### 4-6. AnimatorRuntime 추적 — bone palette translation 흐름

[Engine/anim/AnimatorRuntime.cpp](../Engine/anim/AnimatorRuntime.cpp):
- `EvaluateBoneTransform`: `result.r[1] = lerp(A.r[1], B.r[1], blend)` 로 *r[1].w (=Ty)* 까지 element-wise lerp. **translation 정상 통과**.
- `BuildPalette`: `combined = boneGlobal × offsetMatrix` (XMMatrixMultiply, col-vec convention). **표준 skin matrix 식**.

코드만 보면 *Hips Y 14 단위 변동* 이 *vertex 14 단위 이동* 으로 셰이더에 도달해야 정상.

### 4-7. 셰이더 컨벤션 검증

[shaders/HelloTriangle.hlsl](../shaders/HelloTriangle.hlsl):
```hlsl
column_major float4x4 bones[MAX_BONES];
skinnedPos += w * mul(float4(input.position, 1.0), bones[b]);
```
`column_major` 키워드 + `mul(row_vec, M)` 조합이 *C++ 측 col-major 메모리* (translation @ m[0..2][3]) 와 *정합*. 셰이더는 *translation 을 정상 적용*.

### 4-8. 런타임 진단 — Hips palette translation dump

코드만 보면 정합인데 시각이 어긋남 → 런타임 값 직접 측정.

`BuildPalette` 내 *Hips (b=0) 의 current / offset / skin* translation 매 frame dump. 자동 trigger 로 부팅 후 3초에 Space 누른 효과 자동 발사.

**관찰**:

| 시점 | state | current Y | skin Y |
|---|---|---|---|
| Idle | 0 | +1.42 | -3.22 |
| Jump 도움닫기 (t=0.30, f≈9) | 1 | **-14.48** | **+27.14** |
| Jump 활공 peak (t=0.76, f≈23) | 1 | +4.74 | +19.89 |

**`current Y` 가 -14.48 (아래) 인데 `skin Y` 가 +27.14 (위)**. **부호 반대 + 증폭**. 사용자가 본 *"발이 위로 올라감"* 의 정확한 시각 원인.

### 4-9. 부호 반전 가설

`combined = boneGlobal × offsetMat` 의 row-vec 컨벤션 translation:
```
combined.t = current.t · R_offset + offset.t
```
`R_offset` (= bind pose 의 inverse rotation) 이 *Y 반전 효과* 를 포함하면 *current.Y 부호가 반전되어* skin 에 적용. 원인 가설:
- `ConvertVec3` 의 *Y/Z swap* (`v[0], v[2], v[1]`) 가 *vertex position* 에만 적용되고 *bone transform 의 4x4* 에는 미적용.
- `matReflect` 의 적용이 `LoadFbx` (bind pose) 와 `LoadFbxAnimationOnly` (animation) 에 *일관되지 않음* 가능성.

좌표계 통일이 *진짜 해법* — 별도 큰 작업, 다음 세션.

## 5. 마주친 문제와 해결 ⚠

### 문제 1 — 측정 식 위치 버그 (3 세션 동안 안 보였음)

- **문제**: devlog 33/35/36 모두 `m[3][0..2]` 에서 translation 측정. 모든 클립이 *전부 0* 으로 측정되어 *"자산이 translation track 제거됨"* 잘못된 결론.
- **원인**: ConvertMatrix 의 주석 `FbxAMatrix (column-major) → XMFLOAT4X4 (row-major) — transpose` 가 *실제 동작* 과 어긋남. 실제로는 FbxAMatrix 가 row-major 이고 transpose 후 translation 이 *col 3 위치* (m[0..2][3]) 로 옮겨짐.
- **해결**: LeftFoot 의 *16 원소 4x4 dump* 로 *어디에 값이 있는지* 직접 확인. *col 3 위치 발견* → 모든 측정 식을 `m[0..2][3]` 으로 수정.
- **교훈**: *측정 식이 항상 0 을 반환할 때 자산 탓 하기 전에 *측정 식 자체* 의심*. *행렬의 한 위치만 들여다보고 결론 내지 말고 *전체 dump* 로 검증*.

### 문제 2 — 사용자가 직접 짚어주기 전까지 자산만 의심

- **문제**: 3 세션 동안 *Mixamo export 한계* 가정으로 *코드 우회만 작성*. 사용자가 "리소스 문제냐, 너가 못 맞추는 거냐" 라고 정확히 짚어줘서야 *자산 검증* 으로 전환.
- **원인**: devlog 33 의 첫 진단 결과 ("모든 본 Y span = 0") 를 *측정 신뢰* 했음. *측정 식 자체* 를 의심 안 함. 동일한 잘못된 측정 위에 devlog 35, 36 의 우회 코드 쌓아 올림.
- **해결**: 다음에는 *진단 도구 자체* 의 정확성 검증을 *결과의 정합성* (예: Walking 의 LeftFoot Y span = 0 은 시각과 모순) 으로 *snap test*.
- **교훈**: 자산을 탓하기 전에 *내가 자산을 제대로 보고 있는지* 부터 검증. *시각으로 분명한 사실 (걷기는 발이 움직임) 과 측정 결과 (Y span 0) 가 모순* 이면 측정이 잘못된 것.

### 문제 3 — 점프 곡선 코드의 누적

- **문제**: devlog 33 의 *footY 추적*, 35 의 *자동 floor align*, 36 의 *명시적 곡선* 이 차례로 추가되며 *없는 문제를 해결하려는* 우회 코드만 늘어남.
- **원인**: 진단 결과가 잘못이라는 가능성을 *재검증* 안 함.
- **해결**: 이번 세션에서 *전면 제거*. devlog 36 의 결론 (자산 한계 수용) 은 *틀린 결론*. 36 자체는 *시행착오 기록* 으로 유지.
- **교훈**: *우회가 자꾸 어색하면* 원인 가정을 재의심.

### 문제 4 — XZ lock 안전망의 무효

- **문제**: FbxLoader.cpp:874 의 *Hips XZ root motion 제거 안전망* 이 `m[3][0/2]` 을 lock. 측정 식 버그와 동일한 위치 — 즉 *항상 0 인 위치를 0 으로 lock* — **효과 0**.
- **원인**: 측정 식 버그와 동일한 가정.
- **해결**: 이번 세션에서 *수정 안 함* (필요 시 다음 세션의 좌표계 통일 작업에서 같이). 자산의 *실제 Hips XZ root motion 은 lock 안 되고 그대로 통과* 중이었던 셈. 시각이 그래도 정상이었던 건 *다른 어딘가에서 *우연히* 무효화* 되었거나 *우리가 못 봤을* 가능성.
- **교훈**: *데이터를 건드린다고 주장하는 코드* 가 *정말로 그 위치를 만지는지* 검증.

## 6. 결과 / 검증

- 빌드: Debug + Release x64 성공.
- 시각: *점프 곡선 제거 상태* — Space 점프 시 *crouch 가 시각으로 위로 떠 보이는* 어색한 표현. *진짜 원인 (skin translation 부호 반전) 알고 있음* → 다음 세션에서 좌표계 통일로 해결.
- 진단 코드: *영구 유지* 항목 = FbxLoader 의 Hips X/Y/Z span (자산 검증용) + LeftFoot Y range (간소). *제거* 항목 = LeftFoot 4x4 dump, Hips Y 시계열, BuildPalette palette dump, Application 자동 trigger.

## 7. AI 협업 메모

- 사용자의 직설적 질문 *"리소스 문제냐 너가 못 맞추는 거냐"* 가 *3 세션 동안 의심 못 하던 가정* 을 즉시 무너뜨렸음. *자산 탓* 의 편안한 길에 안주하던 패턴이 명확히 보임.
- 진단 식 자체를 의심하는 *snap test* (예: Walking 의 발 Y span = 0 은 시각과 모순) 가 매우 효과적. 다음에 *모든 데이터가 같은 값* (특히 0) 으로 측정될 때 *식 자체 검증* 부터.

## 8. 다음 단계

- **좌표계 통일 (다음 세션 1순위)**: LoadFbx vs LoadFbxAnimationOnly 의 `matReflect` + `ConvertVec3` Y/Z swap 의 적용 정합 검증. bone transform 4x4 에도 *vertex 와 동일한 좌표계 변환* 적용. 또는 *모든 변환을 ConvertScene 1회로 통일*.
- **skin matrix 부호 반전 수정**: 좌표계 통일 후 *current vs skin* 의 부호 일치 확인.
- **XZ lock 안전망 위치 수정**: 좌표계 통일 후 *진짜 XZ 위치 (m[0/2][3])* 에서 lock.
- **AnimatorController.json 의 Jump exitTime**: 새 자산 (71f) 에 맞춰 *전체 재생* 또는 *적정 시점* 으로 조정 (현재 0.538 = devlog 33 의 78f 기준).
- **점프 시각 검증**: 좌표계 통일 + Animator 정합 후 *코드 측 합성 없이* 자연스러운 점프가 보이는지 시각 확인.

## 9. PPT 재료

- *측정 식 버그의 3 세션 누적 → 결정적 진단 → 자산 살아있음 발견* 의 *디버깅 스토리*. *진단 도구 자체의 정확성 검증* 이라는 일반 교훈.
- *current Y -14.48 → skin Y +27.14* 의 *부호 반전 표* — 좌표계 변환 불일치의 시각화.
- *3개의 devlog (33/35/36) 의 우회 코드를 1 세션에 전면 제거* 한 정리 경험.
