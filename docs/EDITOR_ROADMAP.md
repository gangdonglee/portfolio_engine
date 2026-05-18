# EDITOR_ROADMAP — 맵 에디터 단계별 로드맵 (Phase 4)

> 본 엔진의 자산을 시각적으로 배치하고 `.scene.json` 으로 저장 → 게임 런타임(Client) 이 같은 포맷으로 로드하는 **Unity/Unreal 스타일 맵 에디터**.
>
> 단계별 작은 증명 → 합의 → 확장. 본 문서는 한 페이지 로드맵 — 단계 세부는 각 단계의 devlog 참조.

---

## 1. 큰 그림

```
portfolio_engine.sln
├── Engine.lib       (StaticLibrary)   ← 둘 다 링크
├── Client.exe       (Application)     ← 런타임 — .scene.json 로드 + 게임 루프
└── Editor.exe       (Application)     ← 맵툴 — 시각 편집 + .scene.json 저장

데이터 흐름:
  [Editor.exe] ──── .scene.json ────► [Client.exe]
                   (JSON, git diff 가능)
```

- 같은 직렬화 코드 (`engine::scene::SaveJson`/`LoadJson`) 를 양쪽이 공유 → 포맷 깨질 때 한 곳만 수정.
- Engine.lib 은 **ImGui 를 모른다** — Editor.exe 만 ImGui 를 링크. 의존성 방향 보존.

자세한 모듈 경계: [ARCHITECTURE.md](ARCHITECTURE.md) §7-2.

---

## 2. 핵심 결정 (사용자 결정 — 변경 시 본 표 갱신)

| 항목 | 결정 | 결정 시점 |
|---|---|---|
| UI 라이브러리 | **Dear ImGui** (docking 브랜치, vendored `external/imgui/`) | M0 시작 직전 |
| 씬 직렬화 포맷 | **JSON** (nlohmann/json single-header, vendored) | M0 시작 직전 |
| 라이트 GPU 표현 | **StructuredBuffer** (캡 없음, t1/t2 root SRV) | M1 시작 직전 |

---

## 3. 단계별 로드맵

| 단계 | 목표 | 산출 / 검증 기준 | 상태 |
|---|---|---|---|
| **M0** | Editor.exe 골격 — ImGui DX12+Win32 부트 + 도킹 활성 + 빈 패널 3개 (Hierarchy/Inspector/Viewport) | Editor.exe 가 부팅되어 도킹 가능한 빈 패널 3개 + MainMenuBar 표시. 충돌 없이 종료. | ✅ |
| **M1** | Scene 데이터 모델 + JSON I/O + 다중 라이트 셰이더 + Client 가 `.scene.json` 로드 | Editor 가 만든 `.scene.json` 을 Client 가 로드해서 같은 화면. 라이트는 dir N + point N 동적. | ✅ |
| **M2** | Hierarchy 패널(트리) + Inspector 패널(Transform/color/intensity DragFloat3) + File→New/Open 다이얼로그 + 라이트 추가/제거 버튼 | Editor 안에서 메시 위치/회전 + 라이트 색/방향 편집 → Save → Client 가 변경 반영. | ⏳ |
| **M3** | Asset Browser 패널 (`assets/` 폴더 스캔 + 드래그앤드롭으로 씬에 메시 추가) + 메시 캐시 정식화 | 새 OBJ/FBX 를 assets 폴더에 떨구면 Editor 브라우저에 등장. 드래그로 씬 배치. | ⏳ |
| **M4** | 3D Gizmo (Translate/Rotate/Scale 직접 조작 핸들, ImGuizmo vendored) + 뷰포트에 실제 3D 렌더 | Editor 뷰포트가 빈 패널이 아니라 씬 미리보기. 마우스로 객체 잡고 끌어 이동. | ⏳ |
| **M5+** | 라이트 시각화 기즈모, 그리드/스내핑, Undo/Redo, 멀티 선택, 카메라 프리셋, 씬 저장 안 한 변경 경고 | UX 강화. 우선순위는 데모 영상 일정에 따라 사용자 결정. | ⏳ |

---

## 4. 의존성 방향 (불변)

```
Editor.exe ──► Engine.lib ◄── Client.exe
                  │
                  └── engine::scene (Scene / MeshInstance / Light / Serializer)
                       └── external/nlohmann_json (헤더만, Engine 내부)

ImGui / ImGuizmo (M4+) 는 Editor.exe 전용 — Engine.lib 오염 금지.
```

위반 발견 시 즉시 수정 + devlog §5 에 기록.

---

## 5. 단계별 devlog 링크

| 단계 | devlog | 핵심 산출 |
|---|---|---|
| M0 | [17-editor-skeleton.md](../devlog/17-editor-skeleton.md) | `external/imgui` vendored, Editor.vcxproj, WndProc 훅, 도킹 패널 3개 |
| M1 | [22-scene-json-multi-light.md](../devlog/22-scene-json-multi-light.md) | `engine::scene` 모듈, SceneSerializer, StructuredBuffer, RootSig 5슬롯, sample.scene.json |

---

## 6. 본 문서의 운영

- M2 이후 단계 시작 시점에 본 표의 **상태 마커** 갱신 + devlog 링크 추가.
- 결정이 바뀌면 §2 표 + 영향받는 단계의 행에 기록 (예: 라이트 GPU 표현을 cbuffer 캡으로 회귀하면 §2 갱신).
- 본 로드맵 자체가 길어지면 단계별 상세를 `docs/EDITOR_M*_DESIGN.md` 로 분리 — 본 문서는 한 페이지 인덱스 유지.
