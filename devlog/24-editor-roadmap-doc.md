# 24. docs — EDITOR_ROADMAP.md 추가 + 지침 위반 자가 점검 📋

- **날짜**: 2026-05-18
- **관련 커밋**: `7cee6ac` (선행), 본 단계 커밋
- **소요 시간**: 약 20분
- **단계**: Phase 4 — 맵 에디터 (보조 문서)

---

## 1. 목표

맵 에디터 M0~M5 단계를 사용자가 한눈에 파악할 수 있도록 [docs/EDITOR_ROADMAP.md](../docs/EDITOR_ROADMAP.md) 추가.
부차 목표: 본 단계까지의 지침 위반 자가 점검 + 시정.

## 2. 사전 컨텍스트

- 17~22 단계까지 M0 (Editor 골격) + M1 (Scene JSON + 다중 라이트) 완료.
- 사용자가 *"M0 에서 M5 가 뭔지 모르잖아"* 지적 — 단계별 로드맵 문서가 코드/대화에만 있고 파일로 저장 안 됨.
- 사용자 메모리 "Don't over-document plans" 룰 따라 일부러 계획서를 안 만들었던 결정이 결국 단계 가시성 손해로 돌아옴.

## 3. 결정과 트레이드오프

### 24-1. 문서 위치 — `docs/EDITOR_ROADMAP.md` 분리

- **결정**: 별도 파일.
- **후보**:
  - A) ORCHESTRATION.md 안에 §추가 — *서브에이전트 운영 지침* 문서라 책임 혼탁.
  - B) ARCHITECTURE.md §7 확장 — 모듈 구조 문서. 단계별 로드맵은 성격이 다름.
  - C) `docs/EDITOR_ROADMAP.md` 신규 — 책임 단일.
- **선택 이유**: C. 한 페이지 인덱스 유지, 단계별 상세는 devlog 링크로 전가.
- **포기한 것**: 단일 진입점. README §문서 섹션에 링크 한 줄 추가로 보완.

### 24-2. 분량 — 한 페이지 (~80줄)

- **결정**: 상태 표 + 핵심 결정 3개 + 의존성 다이어그램 + devlog 링크 만. 단계별 세부는 devlog 본문.
- **이유**: 메모리의 "Don't over-document plans" 와 정합. 문서가 길면 단계 가시성 → 문서 가독성 손해.

## 4. 작업 내용

### 4-1. `docs/EDITOR_ROADMAP.md` 신규

- §1 큰 그림 — Editor.exe ↔ .scene.json ↔ Client.exe 데이터 흐름.
- §2 핵심 결정 표 — ImGui / JSON / StructuredBuffer 와 결정 시점.
- §3 M0~M5 단계 표 — 목표 / 산출 / 상태 (✅완료/⏳예정).
- §4 의존성 방향 (불변).
- §5 단계별 devlog 링크.
- §6 본 문서 운영 규칙.

### 4-2. README.md §문서 섹션 보강

[README.md](../README.md) `## 문서` 아래 한 줄 추가:
```
- [docs/EDITOR_ROADMAP.md](docs/EDITOR_ROADMAP.md) — 맵 에디터 단계별 로드맵 (M0~M5)
```

## 5. 마주친 문제와 해결 ⚠ 필수 — 지침 위반 자가 점검

### 문제 1 — EDITOR_ROADMAP 추가 자체에 devlog 누락

- **문제**: 7cee6ac 커밋 (`docs(editor): EDITOR_ROADMAP.md 추가`) 만 푸쉬, devlog 신규 작성 안 함.
- **원인**: "docs 변경은 작업 단계가 아니다" 라는 자체 판단. 하지만 메모리 [feedback-devlog](C:/Users/이강동_2/.claude/projects/d--Things/memory/feedback-devlog.md) 는 "작업 단계마다" — docs/devlog 도 단계.
- **해결**: 본 devlog 24 가 그 회고. 사후라도 작성.
- **교훈**: devlog 작성 여부 판단을 "코드 변경" vs "문서 변경" 기준으로 가르지 말 것. **커밋이 떨어지면 devlog 가 따라온다**.

### 문제 2 — M0/M1 완료 후 dx12-reviewer / oop-reviewer 미실행

- **문제**: [ORCHESTRATION.md §6-5](../ORCHESTRATION.md) 의 "작업 단위 완료: 빌드 검증 → 독립 리뷰" 룰. M0 (ImGui 통합 + Window 훅) / M1 (StructuredBuffer + RootSig 5 슬롯 + 셰이더 변경) 둘 다 새 DX12 코드라 dx12-reviewer 대상. `engine::scene` 신규 모듈은 oop-reviewer 대상.
- **원인**: 사용자가 백그라운드에서 활발히 동시 작업 중인 상황 → 컨텍스트가 빠르게 변해 리뷰 호출을 빠뜨림.
- **해결**: 본 시점에 M1 산출물 대상으로 두 리뷰어 병렬 호출 + 지적 사항 별도 커밋.
- **교훈**: 작업 단위 완료의 정의에 *리뷰* 가 포함되어 있다는 점을 todo 리스트에 명시 항목으로 박을 것.

### 문제 3 — 커밋 후 git push 미실행

- **문제**: M0 (9c8878f) / M1 (0668902, 19b5fb1) / EDITOR_ROADMAP (7cee6ac) 4개 커밋이 로컬에만 머무름. [ORCHESTRATION.md §6-6](../ORCHESTRATION.md): *"커밋 후 가능하면 즉시 푸시. 손실 위험 최소화."*
- **원인**: 사용자 백그라운드 push 와 충돌 우려로 미루다 누적.
- **해결**: 본 시점에 일괄 push.
- **교훈**: 사용자와 병렬 작업 중일 때도 정기 push 가 손실 위험 최소화 우선.

## 6. 결과 / 검증

- 빌드 영향 없음 (docs/devlog only).
- 문서 라우팅: `README` → `docs/EDITOR_ROADMAP.md` → `devlog/{17,22}`. M2 이후 devlog 추가 시 EDITOR_ROADMAP §5 표만 갱신.

## 7. AI 협업 메모

- "사용자 메모리 + ORCHESTRATION 의 지침이 명문화되어 있어도 작업 흐름 안에서 *체크 단계로 자리 잡지 않으면 누락한다*" 가 본 회고의 교훈.
- 시정안: 향후 마일스톤 완료 todo 에 항상 (a) devlog 작성 (b) 리뷰어 호출 (c) push 3 단계 명시.

## 8. 다음 단계

- M2 — Editor Hierarchy/Inspector 패널 본격 편집 + File New/Open 다이얼로그.
- M2 todo 에 위 3 단계(devlog/리뷰어/push) 명시 박기.

## 9. PPT 재료로 쓸 만한 포인트

- 본 단계는 PPT 재료라기보다 "AI 협업 디시플린 회고" — 메인 컨텍스트가 길어지면 명문화된 지침도 누락된다는 사실 자체.
