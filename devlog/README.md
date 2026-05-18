# devlog

> 작업 단계별 상세 기록. 추후 포트폴리오 PPT/블로그 작성 시 슬라이드·문단의 원천 자료로 사용한다.

## 작성 원칙

1. **루틴 — 모든 작업 세션마다 작성**. "이번 변경은 작아서 패스" 금지. 작업 완료 직후 또는 커밋 직후, 다음 작업 시작 전에 작성.
2. **실수 무조건 기록 — 빠짐없이**. §5 "마주친 문제와 해결"은 절대 비워두지 않는다. 정말 실수가 0이면 "이번 세션 실수 없음"으로 명시. 사소한 따옴표 실수, 명령어 오타, 잘못된 가정, 도구 사용 실수까지 모두 기록한다. 면접·PPT에서 "어떤 문제를 풀었나"가 가장 강력한 시그널.
3. **각 단계마다 별도 파일**. `NN-topic-slug.md` 형식 (예: `01-project-setup.md`, `02-foundation.md`).
4. **PPT 슬라이드로 바로 옮길 수 있게 작성**:
   - 각 H2/H3 헤딩이 잠재 슬라이드 제목
   - 본문은 짧은 단락 또는 3~5개 불릿
   - 다이어그램·스크린샷·코드 발췌·수치 적극 포함
5. **"무엇을"보다 "왜"를 강조**. 결정과 트레이드오프 기록.
6. **검증/결과를 수치로**: 빌드 시간, 프레임 시간, 메모리 사용량 등.
7. **재현 가능한 정보**: 관련 커밋 해시, 핵심 파일 경로, 정확한 명령어.

## 표준 구조

각 엔트리는 [_TEMPLATE.md](_TEMPLATE.md) 기반. 누락 섹션은 비워두지 않고 “해당 없음”으로 명시.

## 색인

| # | 단계 | 날짜 | 상태 |
|---|---|---|---|
| 01 | [프로젝트 셋업](01-project-setup.md) | 2026-05-13 | ✅ |
| 02 | [Foundation Skeleton — 빌드 시스템 부트스트랩](02-foundation-skeleton.md) | 2026-05-13 | ✅ |
| 03 | [Phase 1B — engine::platform::Window 클래스](03-window-class.md) | 2026-05-13 | ✅ |
| 04 | [Phase 1C — engine::render::Device 초기화](04-device.md) | 2026-05-13 | ✅ |
| 05 | [Phase 1D-1 — engine::render::CommandQueue](05-command-queue.md) | 2026-05-13 | ✅ |
| 06 | [코드 스타일 학습 + Engine.lib/Client.exe 분리](06-style-and-split.md) | 2026-05-13 | ✅ |
| 07 | [Phase 1D — SwapChain + 첫 클리어 (1D-2~1D-4)](07-swapchain-and-first-clear.md) | 2026-05-14 | ✅ |
| 08 | [Phase 1E — 첫 삼각형 (1E-1~1E-3) 🎉](08-first-triangle.md) | 2026-05-15 | ✅ |
| 09 | [Phase 2 — 인프라 보강 + 회전 큐브 🎲](09-spinning-cube.md) | 2026-05-15 | ✅ |
| 10 | [Phase 3 전반 — 카메라 입력 + 조명 + 메시 로더 🎮](10-camera-lighting-mesh.md) | 2026-05-15 | ✅ |
| 11 | [Phase 3 (E) — 텍스처 업로드 + SRV 디스크립터 테이블 🖼️](11-texture.md) | 2026-05-15 | ✅ |
| 12 | [윈도우 리사이즈 (WM_SIZE → SwapChain/Depth/Viewport/Camera) 🪟](12-window-resize.md) | 2026-05-15 | ✅ |
| 13 | [N프레임 in-flight (CPU/GPU 병렬화) ⏩](13-frame-in-flight.md) | 2026-05-15 | ✅ |
| 14 | [DXGI_PRESENT_ALLOW_TEARING (VRR V-Sync OFF) 📺](14-allow-tearing.md) | 2026-05-15 | ✅ |
| 15 | [MTL 머티리얼 (mtllib/usemtl/Kd) 🎨](15-mtl-materials.md) | 2026-05-15 | ✅ |
| 16 | [OBJ n-gon 자동 삼각형화 (Fan Triangulation) 🔺](16-obj-ngon.md) | 2026-05-15 | ✅ |
| 17 | [Autodesk FBX SDK 통합 + Dragon.fbx 로드 🐉](17-fbx-loader.md) | 2026-05-18 | ✅ |
| 18 | [WIC 이미지 디코더 (체커보드 → 실제 텍스처) 🖼️](18-image-loader-wic.md) | 2026-05-18 | ✅ |
| 19 | [머티리얼 sub-draw 시스템 (FBX 머티리얼별 텍스처) 🎭](19-material-subdraw.md) | 2026-05-18 | ✅ |
