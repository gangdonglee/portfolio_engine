# 15. Phase 3+ — MTL 머티리얼 (mtllib/usemtl/Kd) 🎨

- **날짜**: 2026-05-15
- **관련 커밋**: (이 단계 커밋)
- **소요 시간**: 약 40분
- **단계**: Phase 3 누적 TODO 처리 (4/5)

---

## 1. 목표

OBJ 가 표준으로 참조하는 .mtl 파일을 직접 파싱해서 면별 머티리얼 색을 정점 color 슬롯에 굽기. 단일 색 큐브 → 6면 6색 큐브로 시각 데모.

펄어비스 포트폴리오에서 "표준 자산 파이프라인의 한 축을 직접 구현" 시그널.

## 2. 사전 컨텍스트

직전까지: OBJ 로더는 v/vn/vt/f 만 처리. mtllib/usemtl 라인은 무시. 색은 `LoadObj` 의 `defaultColor` 인자로 통일 — 면별 차이 X.

또한 11단계의 알베도 텍스처는 모든 면 공통 8×8 체커보드 → 면 구분 불가.

## 3. 결정과 트레이드오프

### 15-1. 정점 color 슬롯에 머티리얼 색 굽기 (sub-draw 분리 회피)
- **결정**: face 단위 머티리얼 색을 *정점 color 슬롯에 굽혀* 저장. HLSL 은 기존 `input.color` 그대로 사용.
- **후보**:
  - A) 머티리얼 그룹별로 sub-mesh 분리 — N 머티리얼 = N DrawIndexed 호출.
  - B) 정점 색에 굽기 — 단일 DrawIndexed.
- **선택 이유**: B — 본 단계는 *MTL 표준 파서 + 데모* 가 목적. sub-draw 시스템은 머티리얼별로 *텍스처가 다른 경우* 에 진짜 필요해짐. 그 시점에 sub-mesh 분리를 도입 예정.
- **포기한 것**: 같은 정점이 두 머티리얼에 등장하면 첫 머티리얼 색이 채택되고 두 번째는 손실. 큐브는 면별 normal 분리라 자동으로 안전.

### 15-2. .mtl 파일 부재 시 조용한 폴백
- **결정**: `mtllib` 가 가리키는 파일이 없으면 빈 머티리얼 테이블 + 로그 + 진행 계속. `usemtl name` 의 이름이 테이블에 없으면 `defaultColor` 폴백.
- **이유**: OBJ 만 있고 MTL 없는 케이스 (인터넷 자산 대다수) 가 정상. fatal 처리 시 일반 OBJ 로더 무용지물.
- **포기한 것**: 머티리얼 누락이 시각적으로 "회색 큐브" 로만 보이고 에러로 안 보임. 디버그 시 헷갈릴 수 있어 로그 1줄로 명시.

### 15-3. ObjLoader 가 .mtl 도 책임 (별도 클래스 X)
- **결정**: `LoadMtl` 을 ObjLoader 내부 익명 네임스페이스의 함수로 추가. `MtlLoader` 클래스 미도입.
- **이유**: 본 단계의 .mtl 파싱은 ~40줄. 별도 클래스로 추출하면 ObjLoader 가 의존성 1개 추가. 응집도 우선.
- **분리 시점**: .mtl 의 `map_Kd` (텍스처 경로) 가 도입되면 머티리얼 + 텍스처 결합이 복잡해지므로 그때 분리.

### 15-4. Kd 만 지원 (Ka/Ks/Ns/map_Kd/illum 무시)
- **결정**: newmtl + Kd 만 처리. 나머지 라인은 silently skip.
- **이유**: 현 셰이딩은 Phong with hard-coded ambient/specular. Kd 만 정점 색에 굽고 ambient/specular 는 cbuffer 상수 그대로.
- **포기한 것**: 면별 specular/ambient 변화. 향후 PBR 머티리얼 단계에서 도입.

## 4. 작업 내용

### 4-1. LoadMtl 헬퍼 함수
- 위치: [Engine/render/ObjLoader.cpp](../Engine/render/ObjLoader.cpp)
- 익명 네임스페이스의 자유 함수. `newmtl name` / `Kd r g b` 만 파싱, 나머지 무시.
- 파일 부재 → 빈 맵 + 로그.

```cpp
std::map<std::string, DirectX::XMFLOAT3> LoadMtl(const std::wstring& mtlPath)
{
    std::map<std::string, DirectX::XMFLOAT3> materials;
    std::ifstream file(mtlPath);
    if (!file.is_open()) {
        // 폴백 로그 + 빈 맵
        return materials;
    }
    // newmtl / Kd 만 파싱, flush 람다로 그룹화...
    return materials;
}
```

### 4-2. ObjLoader 본체 — mtllib/usemtl 처리
- OBJ 의 디렉터리 추출 (`absolutePath` 의 last separator 분리).
- `mtllib name` → `objDir + name` 으로 .mtl 경로 합성 → `LoadMtl` 로 머티리얼 테이블에 머지.
- `usemtl name` → 테이블 lookup → `currentColor` 갱신 (없으면 `defaultColor`).
- 각 face 의 3 정점마다 `faceVertexColors` 에 현재 색 push (faceVertices 와 같은 길이).

### 4-3. dedup 단계에서 색 굽기
- 기존 `v.color = defaultColor;` 한 줄을 `v.color = color;` 로 교체. 여기서 `color = faceVertexColors[fvIdx]`.
- 같은 정점 트리플(pos/uv/normal)이 두 머티리얼에 걸치면 첫 등장만 채택. 큐브는 normal 분리로 안전.

### 4-4. Cube.mtl 신규 + Cube.obj 갱신
- [assets/Cube.mtl](../assets/Cube.mtl): 6 머티리얼 (FaceFront/Back/Left/Right/Top/Bottom), 각 Kd 다름.
- [assets/Cube.obj](../assets/Cube.obj): `mtllib Cube.mtl` 라인 + 각 면 그룹 앞 `usemtl FaceXXX`.

PostBuildEvent xcopy 가 `assets\*` 전체 복사하므로 Cube.mtl 자동 포함 — 빌드 시스템 변경 불필요.

## 5. 마주친 문제와 해결 ⚠ 필수 — 빠짐없이 기록

### 문제 1 — 같은 정점이 두 머티리얼에 걸치는 경우
- **문제**: dedup 키는 (pos, uv, normal) 트리플. 두 머티리얼이 같은 트리플 정점을 공유하면 *첫 등장 색* 만 남고 두 번째 색이 무시됨.
- **원인**: 본 단계 설계는 색을 정점 color 슬롯에 굽기. 정점 단위로만 색을 보유.
- **현 상황**: Cube.obj 는 면별 normal 분리 → 각 면 정점은 트리플이 모두 다름 → 충돌 X.
- **일반 케이스**: 평면 메시(공유 normal)에서 머티리얼 경계가 정점 사이를 지나면 일부 색 손실 가능. 진정한 면별 머티리얼은 sub-mesh 분리 필요.
- **교훈**: "정점 색에 굽기" 는 빠르지만 한계. 본격 머티리얼 시스템은 sub-draw + 머티리얼별 텍스처/상수 분리.

### 문제 2 — .mtl 파일명 인코딩
- **문제**: OBJ 의 `mtllib` 라인은 ASCII 8비트. 우리 OBJ 절대 경로는 `wchar_t` UTF-16. 두 인코딩 매핑.
- **해결**: 파일명을 ASCII 가정하고 `std::wstring(mtlName.begin(), mtlName.end())` 로 단순 widen. 비-ASCII 파일명은 unsupported.
- **교훈**: 자산 파일명에 한글 등 multi-byte 가 들어가면 다국어 처리 필요. 본 단계는 ASCII 자산 컨벤션.

### 문제 3 — face vertex 색 추적 — faceVertices 동기 길이
- **문제**: face 단위가 아닌 *face vertex 단위* 로 색을 보유해야 dedup 루프와 동기. 단, OBJ 의 face 는 3 정점 묶음 → face 단위로 색이 같음.
- **해결**: 매 `f` 라인마다 currentColor 를 3번 push → `faceVertexColors.size() == faceVertices.size()` 보장. dedup 루프는 인덱스 동기.
- **교훈**: 두 vector 의 길이 동기는 작은 invariant. 한 곳에서만 push 하도록 묶으면 깨질 일 적음.

### 문제 4 — mtl 파싱의 `flush` 람다
- **문제**: MTL 은 `newmtl name` 으로 새 그룹 시작 + 후속 라인이 그 그룹의 속성. 마지막 그룹은 EOF 까지 미커밋 상태.
- **해결**: 람다 `flush()` — 다음 `newmtl` 시작 시 + EOF 도달 시 둘 다 호출. `hasCurrent` 플래그로 첫 newmtl 전의 더미 flush 회피.
- **교훈**: 스트림 파서의 마지막 그룹 처리 — `flush at next-start AND at eof` 패턴.

## 6. 결과 / 검증

- **빌드**: Debug + Release 둘 다 0 warning / 0 error.
- **자산 복사**: `build/x64/Debug/assets/Cube.mtl` 자동 생성 확인.
- **런타임 로그 기대**: `materials=6` 포함된 OBJ 로드 라인 (확인 시 사용자).
- **시각 기대** (사용자 확인): 회전 큐브의 각 면이 다른 색으로 보임 — 정면 빨강·뒷면 파랑·좌측 노랑·우측 초록·윗면 흰색·아랫면 보라. 알베도 텍스처 (체커보드) 와 곱해져 톤 변화.

## 7. AI 협업 메모

- MTL 파서가 가벼워서 단일 함수로 끝. OBJ 와 같은 익명 네임스페이스에 두고 외부 API 비공개.
- 머티리얼이 *정점 색 슬롯* 으로 흡수되는 단순화는 *현 단계 셰이딩 모델 가정* (Kd 외 무시) 위에 성립. 셰이딩 확장 시 (PBR/specular map 등) 자연 분리.

## 8. 다음 단계

누적 TODO 마지막:
- **OBJ n-gon 자동 삼각형화** — fan triangulation. 현재 quad(4) 이상 face 에 throw.

후속:
- 머티리얼 sub-draw 시스템 — 머티리얼별 sub-mesh + 텍스처/상수 분리.
- map_Kd (텍스처 경로) — 이미지 디코더 도입 후 결합.
- PBR 머티리얼 (Roughness/Metallic) — 셰이딩 모델 갱신.

## 9. PPT 재료로 쓸 만한 포인트

- "OBJ + MTL 표준 파이프라인 자체 구현 — 외부 라이브러리 0"
- "정점 색 굽기 vs sub-mesh 분리 — 단계별 머티리얼 시스템 진화 트레이드오프"
- "스트림 파서의 마지막 그룹 — flush-at-next-and-eof 패턴"
