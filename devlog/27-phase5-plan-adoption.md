# 27. Phase 5 진입 — 캐릭터 컨트롤러 로드맵 채택

- **날짜**: 2026-05-18
- **관련 커밋**: (이 단계 커밋)
- **소요 시간**: 약 30분 (Plan 에이전트 호출 + 결정 합의)
- **단계**: Phase 5 (액션 컴뱃) 진입 결정

---

## 1. 목표

Phase 4 (자산 파이프라인 + 스키닝) 완료 + Editor M1/M2 (Scene JSON + Hierarchy/Inspector) 동시 진행 상태에서 Phase 5 진입. Plan 에이전트로 *Phase 5 전체 단계 설계* 후 사용자 채택, 구현은 사용자가 직접 진행.

## 2. 사전 컨텍스트

- Phase 4: ✅ 매트릭스 layout 정합성 + Animator 보간.
- Editor M0/M1: ✅ Scene JSON + multi-light.
- Editor M2: ⚙️ 사용자 inflight (Panels.{h,cpp} 추가 + Application.cpp 의 ScanSceneSlots 으로 F 키 씬 슬롯 전환).
- 23단계 시점에는 *Phase 5 시작 후보* 만 거론, 진입 시점 미정.

## 3. 결정과 트레이드오프

### 27-1. Plan 에이전트로 전체 단계 설계 먼저
- **결정**: 옵션 A (작은 단위 즉시 구현) 대신 Plan 에이전트로 5 결정 + 5 단계 한 번에 설계.
- **후보**:
  - A) CharacterController + WASD 만 1 라운드 진행 (가장 작게 시작).
  - B) Plan 으로 전체 설계 후 단계별 구현.
- **선택 이유**: B — Phase 5 가 *여러 세션 분량* (Entity 추상화, 카메라, AnimSM, 입력 매핑 등). 설계 합의 없이 시작하면 *이전 라운드 4 클래스 중복 작업* 같은 충돌 재발 위험.
- **포기한 것**: 1 라운드 시작의 즉시성. *합의 비용* 1 라운드 추가.

### 27-2. Plan 채택 + 사용자 구현
- **결정**: Plan 결과를 [docs/PHASE5_PLAN.md](../docs/PHASE5_PLAN.md) 에 채택. 구현은 사용자가 진행. AI 는 추후 리뷰/진단만 지원.
- **이유**: 사용자가 동시에 Editor M2 진행 + Phase 5 시작 — 작업 갈래 명확히 분리. 23단계 (4 클래스 분할 시도) 처럼 *동시 작성 후 폐기* 위험 회피.
- **포기한 것**: AI 가 *직접 빌드/실행 검증 사이클* 까지 책임지지 못함. 사용자 구현 후 추후 라운드에 검증 라운드 별도.

### 27-3. F 키 = 씬 슬롯 점유 → 카메라 토글은 다른 키
- **결정**: 사용자가 F1~F? 를 *.scene.json 슬롯 전환* 에 점유 중이라 Plan 의 *F 키 카메라 토글* 변경 필요. 후보: V/C/Tab.
- **이유**: 키 충돌 회피. F 가 자연스러운 카메라 토글 (FreeLook) 이지만 점유됨.
- **포기한 것**: F 의 자연스러움. 구현 시 사용자가 최종 키 결정.

## 4. 작업 내용

### 4-1. docs/PHASE5_PLAN.md (신규)
EDITOR_ROADMAP.md 와 동일 패턴. 5 결정 표 + 5 단계 로드맵 + Step 1 상세 + 미루는 항목.

### 4-2. devlog 27 (본 문서)
Phase 5 진입 결정의 회의록. Plan 채택 + 사용자 구현 분기점.

### 4-3. Plan 에이전트 호출
- 입력: 현재 코드베이스 + Phase 5 목표 + 결정해야 할 5 사안 + 제약 (풀스크래치, 코드 스타일, 작게 시작).
- 출력 400단어 이내 강제. 5 결정 + Step 1 상세 + Step 2~5 한 줄 + 미루는 항목.
- 채택 결정 사항: 본 devlog §3.

## 5. 마주친 문제와 해결 ⚠

### 문제 1 — 사용자 동시 작업 (Editor M2 / Application.cpp 의 ScanSceneSlots) 진행 중
- **문제**: Plan 결과 발표 시점에 Application.cpp 가 *F 키 씬 전환* 으로 갱신됨. Plan 의 F 키 카메라 토글과 충돌.
- **원인**: AI 가 Plan 호출 *전* 에 사용자 현재 작업 영역 파악 누락. 23단계 (Engine 측 SceneRenderer 와 Client 4 클래스 중복) 와 동일 패턴 재발 가능성.
- **해결**: ① Plan 의 카메라 토글 키를 V/C/Tab 후보로 명시. ② 구현은 사용자가 진행 → AI 가 사용자 작업 영역 침범 회피.
- **교훈**: 큰 작업 시작 전 `git status` + 최근 commit + ORCHESTRATION 의 동시 진행 사항을 *Plan 호출 전* 점검. [[feedback-self-diagnose]] 의 자가 진단 사이클에 *동시 작업 점검* 항목 추가 검토.

## 6. 결과 / 검증

- docs/PHASE5_PLAN.md: 6 섹션 (큰그림 / 결정 / 로드맵 / Step 1 상세 / 미루는 항목 / 운영).
- 구현 코드 변경 X — 본 라운드는 *설계 채택* 단계.
- 다음 라운드: 사용자가 Step 1 구현 → AI 가 oop-reviewer + dx12-reviewer 호출 + 빌드 검증.

## 7. AI 협업 메모

- Plan 에이전트: 큰 단계 설계용. 400단어 이내로 강제하니 *과도하게 상세* 한 결과 회피 가능 ([[feedback-plan-scope]] 메모리 일치).
- 사용자 채택 → AI 가 구현 X 패턴: 사용자 코드 영역 침범 회피, AI 는 추후 리뷰 지원만.

## 8. 다음 단계

- 사용자가 Step 1 구현 (CharacterController + WASD + ThirdPersonCamera).
- 사용자 commit 후 AI 가 oop-reviewer + dx12-reviewer 호출 + 빌드/실행 검증.
- Step 1 ✅ 후 Step 2 (AnimStateMachine 자동 전환) 진입.

## 9. PPT 재료로 쓸 만한 포인트

- "Phase 4 자산 파이프라인 → Phase 5 인터랙티브 entity — Plan 에이전트로 전체 5 단계 설계 후 채택"
- "AI 협업 패턴: 큰 설계는 Plan 에이전트, 구현은 사용자, 리뷰는 oop/dx12-reviewer"
