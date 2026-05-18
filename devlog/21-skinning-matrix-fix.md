# 21. Phase 4 — 스키닝 매트릭스 layout 정합성 + Device FL fallback

- **날짜**: 2026-05-18
- **관련 커밋**: (이 단계 커밋)
- **소요 시간**: 약 6시간 (긴 진단)
- **단계**: Phase 4 자산 파이프라인 (후속 fix)

---

## 1. 목표

20단계 스키닝/애니메이션 도입 후 *실행 시 흰 창 + 응답없음* + *애니메이션 시 메쉬 spike* 증상이 동시 발생. 코드 점검 → 매트릭스 layout 정합성 정립 + Device Feature Level 호환성 확보.

## 2. 사전 컨텍스트

- 20단계 직후. Animator 구조는 완성, palette = offset × keyFrame 식 적용.
- 실행 환경: NVIDIA GTX 750 Ti (Maxwell 1세대, D3D12 Feature Level 11_0 까지만 지원).
- Dragon.fbx: 182 본, 4 클립 (frames 181/545/39/78), 머티리얼 5개.

## 3. 결정과 트레이드오프

### 21-1. Device Feature Level fallback chain
- **결정**: `kFeatureLevelChain = { 12_0, 11_1, 11_0 }` 으로 어댑터별 첫 통과 레벨 채택.
- **후보**:
  - A) 12_0 고정 + GPU 업그레이드 권유.
  - B) 11_0 고정.
- **선택 이유**: C — fallback chain. 의도(12_0 우선)와 호환성(11_0 폴백) 양립. 펄어비스 포트폴리오 환경 변동성 대비.
- **포기한 것**: 12_0+ 의 Mesh Shader/VRS/Raytracing 활용은 GPU 한계로 보류 (후속 단계 결정).

### 21-2. HLSL bones cbuffer `column_major` 키워드
- **결정**: `row_major float4x4 bones[256]` → `column_major float4x4 bones[256]`.
- **후보**: row_major 유지 + ConvertMatrix transpose 정책 변경.
- **선택 이유**: FbxAMatrix 의 column-major 메모리 layout + C++ XMFLOAT4X4 row-major slot 의 element-direct 저장과 일관. row_major HLSL 일 때는 translation 이 row 0 의 [3] 에 들어가 m[3][0..2] = 0 으로 누락 → mesh 가 원점에 압축 (검은 화면).
- **포기한 것**: row_major 가 D3D 표준 권장 — 단 SkinSDK 데이터 흐름과 충돌하면 column_major 가 더 자연.

### 21-3. Animator combined matmul 순서 swap
- **결정**: `XMMatrixMultiply(offsetMat, kfMat)` → `XMMatrixMultiply(kfMat, offsetMat)`.
- **이유**: column-major mathematical 식 `vWorld = M_animated × M_inverseBind × v_col` 을 element-direct + column_major HLSL view 조합에서 정확히 표현하려면 *kf 가 좌측*. 직전 순서는 결과적으로 `M_inverseBind × M_animated × v` 가 되어 자세 점프 → spike.
- **검증**: T-pose 정상 → frame 0 고정 정상 → 4 clip 모두 자연스러운 dragon animation 확인.

### 21-4. FBX UV 인덱싱 단순화
- **결정**: `ResolveUvIndex(uvElem, cp, polyVertexUvIdx)` 제거, `mesh->GetTextureUVIndex(p, j)` 결과를 그대로 `GetDirectArray().GetAt` 에 전달.
- **이유**: `GetTextureUVIndex` 가 매핑/레퍼런스 모드를 내부 처리해 *resolved DirectArray index* 를 직접 반환. 추가 IndexArray lookup 은 이중 indirection 으로 UV 가 어긋남.

### 21-5. MAX_BONES 128 → 256
- **결정**: HLSL + main.cpp 둘 다 256 으로 확장.
- **이유**: Dragon 182 본 → 128 cbuffer 범위 밖 본 인덱스 OOB → 가비지 매트릭스. cbuffer 256*64 = 16KB (D3D12 64KB 한계 내).

### 21-6. Logger error.txt 파일 sink
- **결정**: `OutputDebugString` 외에 *exe 디렉토리 / error.txt* 에도 매 LogInfo 호출마다 fflush 동시 기록.
- **이유**: 사용자가 응답없음 상태에서 직접 디버거 출력을 캡쳐하기 어려움 → 자동 진단을 위해 파일 동기 기록. 한글 환경 호환을 위해 UTF-8 BOM.

## 4. 작업 내용

### 4-1. Device.cpp / Device.h
- `kFeatureLevelChain` 추가, `SelectAdapter` 가 어댑터별 chain 순회 + 첫 통과 레벨 채택.
- `m_selectedFeatureLevel` 멤버, `CreateDevice` 가 그 레벨로 생성.
- 로그: `[render] Selected feature level: 0x{X}`.

### 4-2. shaders/HelloTriangle.hlsl
- `column_major float4x4 bones[MAX_BONES]`, `#define MAX_BONES 256`.
- VS 스키닝 식 자체는 그대로 `mul(float4(input.position, 1.0), bones[b])`.

### 4-3. Engine/render/Animator.cpp
- `combined = XMMatrixMultiply(kfMat, offsetMat)` (column-major 식 일치).

### 4-4. Engine/render/FbxLoader.cpp
- `ConvertMatrix` 유지 (`m.Get(col, row)`) — 결과적으로 row-major slot 에 column-major math element 직접.
- `ResolveUvIndex` 제거, UV 인덱싱 단순화.
- 사전 계산(invMeshNode 캐시) 최적화 제거 — 참조 구조 그대로 cluster 내부 inline 평가. 디버그 빌드에서 다시 느려졌지만 *데이터 정합성 우선*.
- `LoadAnimationData` 의 SetCurrentAnimationStack 매 cluster × clip 호출.

### 4-5. Engine/core/Logger.cpp
- `OpenLogFileOnce` 가 exe 디렉토리에 error.txt 생성 (UTF-8 BOM, "wb" truncate).
- LogInfo/LogInfoA 가 OutputDebugString + fwrite + fflush.

### 4-6. Client/main.cpp
- Animator 시작 시 nullptr (T-pose). `1..4` 키 → loaded.clips[0..3] 활성, `0` 키 → T-pose. 다운 엣지 감지.
- world matrix Y축 자전 제거 (디버깅 편의).
- BonePalette 256 본 + cbuffer 64KB 한계 static_assert.

## 5. 마주친 문제와 해결 ⚠ 필수

### 문제 1 — 흰 창 + 응답없음
- **문제**: 실행 후 흰 창 + OS "응답없음" 상태 유지.
- **원인**: GTX 750 Ti 가 FL 12_0 미지원 → WARP(소프트웨어 래스터라이저) 폴백 → 4096×4096 텍스처 + LoadAnimationData (115k EvaluateGlobalTransform 호출) 가 CPU 시뮬레이션으로 1분+ 메인 스레드 점유 → 메시지 펌프 멈춤.
- **해결**: Device FL fallback chain 으로 11_0 채택 → 하드웨어 GPU 사용. Debug 빌드 80초, Release 5초 로딩.
- **교훈**: 어댑터 enumeration 시 항상 fallback chain. 흰 창 = 응답없음 ≠ deadlock — 매우 느린 동기 작업도 같은 증상.

### 문제 2 — 클립별 spike (목 위로 폭주)
- **문제**: 1/2 키 누르면 메쉬 부분이 위로 spike, 3/4 키는 *시각적으로 다른 자세* 였으나 spike 만 없음. palette 수치 dump 는 정상 (translation < 50, rotation 정상).
- **원인**: HLSL `column_major bones` + ConvertMatrix element-direct 조합에서 `XMMatrixMultiply(offset, kf)` 순서가 *column-major 식 의도와 반대*. T-pose 시 palette = identity 가까워 정상 보였으나 animation 진행 시 곱 순서 어긋남이 vertex 폭주로 발현.
- **해결**: `XMMatrixMultiply(kfMat, offsetMat)` 으로 swap.
- **교훈**: row-major XMMatrixMultiply 의 결과 element 가 column-major matmul element 와 동일 — *원하는 column-major 식 순서 그대로* 입력. T-pose 정상만으로 매트릭스 식 검증 X (palette = identity 인 자명 케이스).

### 문제 3 — SQT 분해 시도 더 망가짐 + clean rebuild 누락
- **문제**: KeyFrame 을 SQT 로 분해 + Animator 에서 `XMMatrixAffineTransformation` 재조립 시도. 더 망가짐. 이후 진단 로그 추가했는데 `[ANIM] entry #N` dump 가 전혀 출력 안 됨.
- **원인**: ① reflect 변환된 matrix (det=-1) 에서 GetS/GetQ/GetT 분해 시 quaternion 부정확. ② `std::sqrt` 헤더 미포함으로 incremental build 가 *실패한 상태에서 stale .obj 가 link* → 새 dump 코드 자체가 컴파일 안 되고 이전 빌드 binary 가 실행.
- **해결**: SQT 시도 되돌림 + `#include <cmath>` 추가. clean rebuild.
- **교훈**: ① reflection matrix 의 SQT 분해는 부호 문제 발생 가능. ② "코드 변경했는데 동작 변화 없음" 시 컴파일 에러 + cached .obj 의심. Build 출력의 error/warning 행 빠짐없이 확인.

### 문제 4 — SendKeys 가 portfolio_engine 윈도우 활성화 못함
- **문제**: PowerShell `WScript.Shell.SendKeys` 가 Discord 등 다른 활성 윈도우로 키 전송. 자동 진단 불가.
- **원인**: Windows 11 의 *foreground 강제 제한* — 다른 앱이 활성일 때 SetForegroundWindow 실패.
- **해결**: ① `PostMessage` 로 윈도우 핸들에 직접 WM_KEYDOWN/WM_KEYUP 전송 (활성화 불필요). ② 화면 캡쳐는 `PrintWindow` API 의 `PW_RENDERFULLCONTENT` 플래그 (background 윈도우도 D3D 컨텐츠 캡쳐).
- **교훈**: 자동화 진단 시 윈도우 활성화 의존 회피. PostMessage + PrintWindow 가 background-safe.

## 6. 결과 / 검증

### 빌드
- Release x64: 정상 빌드. 약 5초 로딩 후 T-pose 표시.
- Debug x64: 약 80초 로딩 (LoadAnimationData 의 EvaluateGlobalTransform 누적). Release 권장.

### 시각 검증 (PrintWindow 자동 캡쳐)
- T-pose: Dragon 양 날개 펴고 정상.
- Clip 0 (181 frames): 날개 접힘 자세.
- Clip 1 (545 frames): 양 날개 펼친 비행 자세.
- Clip 2 (39 frames): 날개 들어올림.
- Clip 3 (78 frames): 날개 들어올림 + 다리 굽힘.

모든 자세 자연스러운 dragon animation, vertex spike 없음.

### error.txt 자동 진단 도구
- `OpenLogFileOnce` 가 exe 옆 error.txt 에 모든 LogInfo 즉시 flush.
- 사용자 또는 자동화에서 응답없음 시 즉시 확인 가능.

## 7. AI 협업 메모

진단 사이클:
- AI 가 매트릭스 layout 수학 분석으로 곱 순서 후보 좁힘.
- T-pose / frame 0 고정 / 4 clip 비교로 spike 원인 단계적 격리.
- PowerShell PostMessage + PrintWindow 자동화로 사용자 부담 없이 매 시도 검증.

가장 큰 학습: *수학적으로 동일* 한 두 식이 *layout 조합* 에 따라 실제 동작이 다름. row-major / column-major HLSL 키워드 + ConvertMatrix transpose 정책 + matmul 순서의 4 가지 조합 중 *유일하게 정확한 set* 을 dump 데이터로 검증.

## 8. 다음 단계

- 진단 인프라(error.txt) 유지 — 다음 hang/렌더링 이슈 즉시 분석.
- Animator 의 보간 (Lerp + Slerp) 도입으로 frame 점프 부드럽게.
- 22단계: Phase 5 시작 (캐릭터 컨트롤러 / 액션 컴뱃 기획).

## 9. PPT 재료로 쓸 만한 포인트

- "row-major DirectXMath ↔ column_major HLSL 의 element-direct 일관성 — 곱 순서가 결정하는 vertex spike"
- "fbx 풀스크래치 로더의 매트릭스 layout 디버깅 — T-pose 정상에 속지 말 것"
- "PostMessage + PrintWindow 기반 자동 진단 사이클로 매트릭스 layout 가설 N회 검증"
