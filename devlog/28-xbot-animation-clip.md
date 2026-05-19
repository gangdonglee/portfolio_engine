# 28. Mixamo X Bot — 메시/클립 분리 로드 (animationClipPath) 🏃

- **날짜**: 2026-05-19
- **관련 커밋**: (이 단계 커밋)
- **소요 시간**: 약 2.5시간
- **단계**: Phase 4 — 캐릭터 애니메이션 데모

---

## 1. 목표

사용자가 `Resources/FBX/` 에 가져다 둔 **Mixamo X Bot** 자산 (베이스 메시 + 6 클립 FBX) 을 엔진이 로드해서 *애니메이션 작업을 에디터에서* 가능하게.

핵심 기능:
- Editor 의 Inspector 에서 `MeshInstance.animationClipPath` 를 명시하면 *별도 클립 FBX* 의 첫 클립이 자동 재생.
- 베이스 메시 (`X Bot.fbx`) + 클립 (`Running.fbx`) 분리 구조를 엔진이 인식.
- Scene 으로 저장 → Client 재생.

## 2. 사전 컨텍스트

- 25/26/27 단계: Client/Editor 분할 + M2 패널 + M3a 런타임 씬 전환.
- 본 단계 직전 사용자가 Mixamo X Bot 자산 7개 다운로드. *Without Skin* 옵션 (베이스 메시는 X Bot.fbx, 클립 FBX 는 메시 없이 스켈레톤+클립만).
- 기존 `FbxLoader::LoadFbx` 는 *메시+스켈레톤+클립 통합 로드* 만 지원 — 메시 없는 클립 FBX 는 *"FBX: 유효한 메시/머티리얼 없음"* throw.

## 3. 결정과 트레이드오프

### 28-1. Mixamo Without Skin 그대로 + FbxLoader 분리 모드 (사용자 결정)

- **결정**: Mixamo 의 *Without Skin* 다운로드를 그대로 사용. FbxLoader 에 *클립만 추출* API 추가.
- **후보**:
  - A) Mixamo "Skin O" 재다운로드 — 각 클립 FBX 가 자체 메시 포함. 엔진 코드 변경 거의 없음. 단 자산 중복 (메시가 6번 복사) + 사용자 수작업.
  - B) Without Skin + FbxLoader 분리 모드 — 본 결정. 정석 엔진 워크플로우.
- **선택 이유**: B. 풀스크래치 엔진의 정직한 자산 파이프라인. 베이스 메시 1개 + 클립 N개 = 메모리 1배. AAA 패턴.

### 28-2. 클립 FBX 의 본 → 베이스 스켈레톤 인덱스 매핑 — 이름 기반 + UTF-8 정규화

- **결정**: 클립 FBX 의 모든 `eSkeleton` 노드를 *이름 → FbxNode\** 맵으로 수집. 베이스 스켈레톤의 본 이름 (wstring) → UTF-8 (WideCharToMultiByte) 변환 후 매칭.
- **이유**: Mixamo X Bot 의 본 이름은 `mixamorig:Hips` 등 ASCII — 단순 비트마스크 `wc & 0xFF` 변환으로도 동작. 그러나 *비-ASCII 본 이름 자산* (예: 한글 캐릭터) 호환 위해 UTF-8 변환.
- **포기한 것**: 본 *인덱스* 매칭 (이름 무관 + 트리 구조 동일 가정). 더 빠르지만 변동 자산에 약함. 이름 매칭이 정석.

### 28-3. 메시 노드 변환 생략 — Mixamo Without Skin 가정

- **결정**: 기존 `LoadAnimationData` 의 `matFromNode.Inverse() * linkNode->EvaluateGlobalTransform(t)` 에서 *mesh-node 변환 부분* 생략. 클립 FBX 에 메시 없음 — `linkNode->EvaluateGlobalTransform(t)` 자체가 mesh-local 변환 역할.
- **위험**: 메시 노드 변환이 비-identity 인 *다른* 클립 FBX 자산에선 transform 손상. 헤더 주석에 가정 명시.
- **이월**: 일반 케이스 처리 (mesh-node 발견 시 자동 결합) 는 후속 단계.

### 28-4. animationClipPath = 빈 문자열 = 미사용 (Scene 모델)

- **결정**: `MeshInstance.animationClipPath` 가 빈 문자열이면 기존 동작 (메시 FBX 내장 클립 또는 T-pose). 비어있지 않으면 별도 클립 FBX 로드.
- **후보**: `std::optional<std::string>` — 의도 더 명확. 단 SceneSerializer 의 JSON 직렬화에서 키 생략으로 같은 효과.
- **선택 이유**: POD 정책 일관성 (Scene.h 의 모든 필드가 기본 타입). 빈 문자열로 unset 표현.

## 4. 작업 내용

### 4-1. `Engine/render/FbxLoader::LoadFbxAnimationOnly` 신규

[Engine/render/FbxLoader.h](../Engine/render/FbxLoader.h):
```cpp
struct LoadedFbxAnimation { std::vector<std::unique_ptr<AnimClip>> clips; };

LoadedFbxAnimation LoadFbxAnimationOnly(
    const wchar_t*  absolutePath,
    const Skeleton& baseSkeleton);
```

내부 흐름:
1. FbxManager/Scene/Importer 부팅 (LoadFbx 와 동일 패턴, 공용 `FbxManagerGuard` 사용).
2. `CollectBoneNodesRec` — 클립 FBX 의 `eSkeleton` 노드를 *이름 → FbxNode\** 매핑.
3. 베이스 본 이름 → UTF-8 → `baseIdxToClipNode[boneIdx]` 매핑 테이블.
4. `LoadAnimationInfo` (기존 헬퍼) 로 AnimStack 메타.
5. 각 AnimStack 활성화 후 본 노드의 `EvaluateGlobalTransform(t)` 평가 → `matReflect` 좌표계 변환 → `AnimClip.bonesKeyFrames[baseIdx]` 에 push.

### 4-2. `Engine/scene/Scene::MeshInstance::animationClipPath`

```cpp
struct MeshInstance {
    std::string name;
    std::string meshAssetPath;
    std::string animationClipPath;   // 선택 — 빈 문자열 = 미사용
    Transform   transform;
};
```

SceneSerializer 가 비어있을 때 JSON 키 *생략* — 디폴트와 명시 unset 일관 표현.

### 4-3. `Client/SceneRuntime` — m_clipOnlyCache + 자동 활성화

- 신규 멤버: `unordered_map<string, vector<unique_ptr<AnimClip>>> m_clipOnlyCache` — 클립 FBX 캐시.
- ctor 안의 *Animator 선택 루프* 확장:
  - animationClipPath 있는 인스턴스 → 베이스 메시의 skeleton + `LoadFbxAnimationOnly` 결과의 clips → Animator 자동 활성화 (첫 클립).
  - animationClipPath 없으면 기존 동작 (메시 FBX 내장 클립 또는 T-pose).
  - 첫 매칭에서 break (멀티 캐릭터 씬은 첫 캐릭터 기준).

### 4-4. `Editor/Panels::DrawInspector` — animationClipPath 위젯

MeshInstance Inspector 에 한 줄 추가:
```
[meshAssetPath:      Resources/FBX/X Bot.fbx     ]
[animationClipPath:  Resources/FBX/Running.fbx   ] [clear##anim]
(Client F0 = T-pose, 1..4 = clip select)
```

### 4-5. `assets/Scenes/xbot_running.scene.json`

```json
{
  "meshes": [{
    "meshAssetPath":     "Resources/FBX/X Bot.fbx",
    "animationClipPath": "Resources/FBX/Running.fbx",
    ...
  }],
  ...
}
```

### 4-6. FbxLoader 진행 로그

`LoadFbx` 와 `LoadFbxAnimationOnly` 둘 다 단계별 로그 추가:
- `[fbx] LoadFbx begin: ...`
- `[fbx]   imported, triangulating...`
- `[fbx]   triangulated, loading bones...`
- `[fbx]   bones loaded: N`
- `[fbx]   anim stacks: M, parsing nodes...`
- `[render] FBX loaded: ...` (기존)
- `[render] FBX animation-only loaded: clips=N, bones-matched=A/B (path: ...)` (신규)

X Bot.fbx 로드가 Debug 에서 *수십초* 소요 — 디버깅 시 *어디서 멈췄는지* 파악 가능.

## 5. 마주친 문제와 해결 ⚠ 필수

### 문제 1 — Mixamo X Bot 다운로드 옵션 미상 → 검증으로 판정
- **문제**: 사용자가 X Bot.fbx + 6 클립 FBX 다운로드. 다운로드 옵션 ("Skin O" vs "Without Skin") 미명시. *작업 분량* 이 옵션에 따라 천지차.
- **해결**: 임시 xbot_running.scene.json (meshAssetPath="Running.fbx") 작성 → Client 실행 → error.txt 로그 확인. *"FBX: 유효한 메시/머티리얼 없음"* throw 로 *Without Skin* 판정.
- **교훈**: 사용자가 아는 정보 (다운로드 옵션) 라도 *직접 검증* 으로 빠르게 확인 가능. 묻기 전에 한 사이클 실행.

### 문제 2 — Debug 빌드에서 X Bot.fbx 로드가 수십초 (정상)
- **문제**: 첫 검증에서 6초/12초/30초 후에도 로그 멈춤 — *hang* 으로 오판.
- **원인**: Debug 빌드의 SDK 코드가 *매우 느림*. X Bot.fbx (24K 정점, 65 본, 2 클립) Triangulate + Cluster keyframe 평가가 ~60초.
- **해결**: 60초 대기 후 정상 통과 확인. Release 빌드 ~10초 (정상).
- **교훈**: FBX SDK 의 Triangulate + EvaluateGlobalTransform 은 Debug 에서 *수십배 느림*. 빠른 진단 위해 진행 로그 박아두기 — 4-6 의 패턴.

### 문제 3 — wstring→string 변환 정책 불일치 (OOP 위반, 리뷰)
- **문제**: 초안에서 클립 본 매핑의 변환을 `static_cast<char>(wc & 0xFF)` 비트마스크 — 같은 cpp 의 LoadFbx 는 `WideCharToMultiByte(CP_UTF8)` 사용. 일관성 위반 + 비-ASCII 본 이름 매칭 실패.
- **해결**: `WideCharToMultiByte` 로 통일.
- **교훈**: 한 모듈 안의 *동일 작업* 은 단일 정책. 빠르게 적은 코드는 정합성 위반 신호.

### 문제 4 — ManagerGuard struct 복붙 (OOP DRY)
- **문제**: `LoadFbx` 와 `LoadFbxAnimationOnly` 안에 같은 익명 로컬 struct `ManagerGuard` 정의. SRP 도 모호 — Manager 만 destroy.
- **해결**: 익명 namespace 에 `FbxManagerGuard final` 1회 정의 + 양쪽 함수가 재사용. 4종 delete 명시.
- **교훈**: 같은 RAII 패턴 두 번이면 추출. 리뷰가 발견.

### 문제 5 — endFrame <= startFrame 시 unsigned cast 폭발 (DX12 Warning)
- **문제**: `static_cast<size_t>(endFrame - startFrame)` — takeInfo 비정상 (endTime < startTime) 시 underflow → 거대값 reserve.
- **해결**: `if (endFrame <= startFrame) continue;` 가드.
- **교훈**: signed→unsigned cast 전에 항상 비교 가드.

### 문제 6 — SceneRuntime ctor 의 두 번 순회 (OOP SRP)
- **문제**: 초안이 (1) Animator 후보 결정 루프 + (2) auto-activate 루프 두 번 순회.
- **해결**: 한 루프 안에서 `autoActivateClip` 플래그 보관 → 루프 후 한 번에 활성화.

## 6. 결과 / 검증

- **빌드 (Debug + Release)**: Engine + Client + Editor 모두 0 warning / 0 error.
- **Client.exe 자동 실행** (Release, xbot_running 슬롯 격리 부팅):
  ```
  [fbx] LoadFbx begin: ...X Bot.fbx
  [fbx]   bones loaded: 65
  [fbx]   anim stacks: 2, parsing nodes...
  [render] FBX loaded: vertices=24746, materials=2, indices=147336, bones=65, clips=2
  [render] FBX animation-only loaded: clips=1, bones-matched=65/65 (path: ...Running.fbx)
  ```
  **본 매칭 65/65** — Mixamo 표준 스켈레톤 완전 일치 확인.
- **시각 검증 (사용자 부탁)**:
  1. Client.exe 실행 → F3 (xbot_running 슬롯) 으로 전환.
  2. X Bot 캐릭터가 *Running 애니메이션* 으로 움직임 확인.
  3. Editor 실행 → Open `xbot_running.scene.json` → MeshInstance Inspector 에서 `animationClipPath` 를 `Resources/FBX/Walking.fbx` 로 변경 → Save → Client F3 다시 누름 → Walking 으로 전환.

## 7. AI 협업 메모

- 사용자가 *"x-bot 폴더에 만들어 놓은건데"* 한 줄로 컨텍스트 전달. 폴더 스캔 + 파일 크기 분석으로 자산 구조 추정. *Skin O vs Without Skin* 디자인 결정만 사용자 확인.
- 진행 로그 추가 → 60초 대기 패턴이 *디버그 시간* 절약. self-diagnose 메모리의 "직접 빌드/실행/로그읽기 한 사이클" 룰 정확 적용.
- 리뷰어가 *DRY (ManagerGuard 복붙)* 와 *변환 정책 불일치* 둘 다 발견 — 자체 점검에서 놓친 핵심.

## 8. 다음 단계

미뤄둔 항목:
- **클립 전환 UI** (M3+) — 현재 F1..F4 키는 *FBX 내장* 클립만 전환. animationClipPath 의 클립이 *여러 개* (이번 단계는 첫 클립만) 일 때 UI 도입.
- **메시 노드 변환 일반화** — 클립 FBX 에 메시가 있을 때 자동 결합. 또는 assert 로 거부.
- **FbxImporter::Destroy() 명시 호출** — Manager 가 회수하지만 SDK 권장 패턴.
- **LoadFbx / LoadFbxAnimationOnly 명명 비대칭** — `LoadFbxModel` / `LoadFbxClips` 같은 동등 명사 페어. 후속 정리 단계.
- **Editor 의 Asset Browser** — Resources/FBX/ 폴더 스캔 + 드래그앤드롭 (M3b 또는 M4).

## 9. PPT 재료로 쓸 만한 포인트

- "Mixamo Without Skin 자산 분리 로드 — 베이스 메시 X Bot.fbx + 클립 Running.fbx 결합. 본 매칭 65/65 = 표준 Mixamo 파이프라인 호환."
- "FbxLoader 의 두 API 페어 (`LoadFbx` / `LoadFbxAnimationOnly`) — 자산 책임 분리. 메시 로드는 무거운 ParseNode, 클립 로드는 본 transform 평가만."
- "Scene.animationClipPath 가 빈 문자열로 unset — POD 정책 유지 + JSON 직렬화에서 키 생략으로 일관."
