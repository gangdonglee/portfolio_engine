# 22. Phase 4 마무리 — Animator matrix element-wise lerp 보간

- **날짜**: 2026-05-18
- **관련 커밋**: (이 단계 커밋)
- **소요 시간**: 약 1시간 (SQT 실패 시도 포함)
- **단계**: Phase 4 자산 파이프라인 (포스트 fix)

---

## 1. 목표

21단계 매트릭스 layout 정합성 정립 후, Animator 의 nearest-frame 점프를 부드러운 보간으로 전환. Phase 4 마무리.

## 2. 사전 컨텍스트

- 21단계 fix 로 T-pose / 4 clip 모두 자연스러운 자세 확인.
- Animator::Update 가 `idx = floor(t01 × kfs.size())` 의 nearest-frame matrix 사용 → frame 전환점에서 자세 점핑.

## 3. 결정과 트레이드오프

### 22-1. matrix element-wise lerp 채택 (SQT Slerp 보류)
- **결정**: `XMVectorLerp` 로 frame N/N+1 의 matrix row 4개를 element-wise 보간.
- **후보**:
  - A) SQT 분해 + Lerp/Slerp + AffineTransformation 재조립 (정밀).
  - B) matrix element-wise lerp (단순, rotation 부분 약간 non-orthogonal).
  - C) Dual quaternion skinning (별도 큰 작업).
- **선택 이유**: B — SQT 시도 시 reflect (det=-1) 적용된 matrix 에서 `XMMatrixDecompose` 의 quaternion 부호가 모호해 인접 frame 끼리 *반대 방향 slerp* 가 발생, Dragon 이 *왜소화 + spike* 됨. element-wise lerp 는 24fps frame 간격(약 41ms)에서 회전 비직교성이 시각적으로 무시 가능.
- **포기한 것**: scale/translation 의 정확한 보간 (lerp 자체는 정확). rotation 의 *수학적으로 깔끔한* slerp.

### 22-2. SQT 정확한 접근 — 다음 라운드로 보류
- **결정**: SQT Slerp 정상 도입은 *reflect 적용 전* 의 SQT 추출 + 재조립 후 reflect 적용 방식이 필요. 이번 라운드 보류.
- **이유**: ① raw matrix (det>0) 에서 decompose 가 정확. ② AffineTransformation 결과(row-major)와 matReflect(FbxAMatrix, column-major) 의 곱셈 변환 검증 필요. ③ 매 본 매 frame XMMatrixDecompose + matmul 두 번 — CPU 비용도 검증 필요.

## 4. 작업 내용

### 4-1. Engine/render/Animator.cpp
```cpp
const double frameF = t01 * static_cast<double>(kfs.size());
size_t idxA = static_cast<size_t>(frameF);
if (idxA >= kfCount) { idxA = kfCount - 1; }
const size_t idxB = (idxA + 1 < kfCount) ? idxA + 1 : idxA;
const float blend = static_cast<float>(frameF - static_cast<double>(idxA));

const XMMATRIX A = XMLoadFloat4x4(&kfs[idxA].transform);
const XMMATRIX B = XMLoadFloat4x4(&kfs[idxB].transform);
XMMATRIX kfMat;
kfMat.r[0] = XMVectorLerp(A.r[0], B.r[0], blend);
kfMat.r[1] = XMVectorLerp(A.r[1], B.r[1], blend);
kfMat.r[2] = XMVectorLerp(A.r[2], B.r[2], blend);
kfMat.r[3] = XMVectorLerp(A.r[3], B.r[3], blend);
```

`kfMat.r[i]` 는 4 row 각각의 XMVECTOR. 4번의 SIMD vector lerp — 본 182개 × 60fps ≈ 11k op/sec. 무시할 비용.

### 4-2. Engine/render/AnimClip.h
- KeyFrame.transform 유지 (SQT 필드 시도 후 원상복구).
- 주석에 보간 정책 + SQT 접근의 잠재적 함정 명시.

## 5. 마주친 문제와 해결 ⚠ 필수

### 문제 1 — SQT Slerp 보간 시 Dragon 왜소화 + spike
- **문제**: KeyFrame 에 scale/rotation/translation 추가, FbxLoader 에서 `XMMatrixDecompose` 로 SQT 사전 추출, Animator 에서 `XMQuaternionSlerp` + `XMVectorLerp` + `XMMatrixAffineTransformation` 적용. 결과: Dragon 이 비현실적으로 작아지고 등에 spike.
- **원인**: matReflect (Y/Z 스왑, det = -1) 적용된 matrix 에서 SQT 분해 시:
  ① reflection 효과로 scale 의 부호가 음수로 추출될 수 있음.
  ② quaternion 의 부호가 인접 frame 간 *반대 부호* 로 추출되어 Slerp 가 *반대 방향 회전* 으로 보간.
  AffineTransformation 가 음수 scale + 반대 quaternion 으로 *기형적 변환 matrix* 생성.
- **해결**: SQT 시도 되돌리기 + matrix element-wise lerp 채택 (24fps frame 간격에서 rotation 비직교성 무시 가능).
- **교훈**: ① `XMMatrixDecompose` 는 *정상 affine matrix* (det > 0) 에서만 안전. mirror 가 포함된 matrix 는 SQT 분해 부정확. ② 인접 quaternion 의 부호 일관성 (`dot(qA, qB) < 0` 시 `-qB` 보정) 은 `XMQuaternionSlerp` 가 *내부적으로* 처리하지만, *추출 단계의 부호 모호* 는 별개 문제. 추출 직후 부호 일관성 보정 필요.

## 6. 결과 / 검증

- 빌드: Release x64 정상.
- 캡쳐: T-pose / Clip 0~3 모두 자연스러운 자세, 보간으로 vertex 부드럽게 이동.
- 4 row × XMVectorLerp 비용: 본 182개 매 프레임 — 무시할 수준.

## 7. AI 협업 메모

SQT 보간 실패 후 matrix element-wise lerp 로 *우아하지 않지만 동작 보장* 하는 차선책 선택. 정확한 SQT 접근은 *reflect 적용 시점* 의 의존성을 풀어내는 별도 설계 단계 (Phase 5 또는 Animator 고도화 시) 로 미룸.

## 8. 다음 단계

- **Phase 5 시작 — 캐릭터 컨트롤러**: WASD 이동, 점프, animation 상태 머신 (idle/walk/attack).
- **Editor M1**: Scene Hierarchy/Inspector 패널 + Save 버튼.
- **SQT 정밀 보간**: reflect 적용 전 SQT 추출 + quaternion 부호 일관성 보정.
- **Animator 의 BlendTree**: 두 clip 사이 cross-fade.

## 9. PPT 재료로 쓸 만한 포인트

- "XMMatrixDecompose 의 *mirror matrix 함정* — SQT 보간 시 quaternion 부호 일관성"
- "24fps animation 에서 matrix element-wise lerp 의 *시각적 충분 정확성* — 정밀 Slerp 와 trade-off"
