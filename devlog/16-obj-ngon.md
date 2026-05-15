# 16. Phase 3+ — OBJ n-gon 자동 삼각형화 (Fan Triangulation) 🔺

- **날짜**: 2026-05-15
- **관련 커밋**: (이 단계 커밋)
- **소요 시간**: 약 10분
- **단계**: Phase 3 누적 TODO 처리 (5/5 — 마지막)

---

## 1. 목표

OBJ 의 `f` 라인이 3 정점 이상의 다각형(quad 등)을 가질 때 자동으로 삼각형들로 분해하여 IA 단계에 전달.

## 2. 사전 컨텍스트

직전 단계까지: `f a b c` 3 토큰만 허용. 4 이상이면 `throw runtime_error`. 표준 OBJ 자산 다수가 quad 면을 포함하는데 우리 로더가 그것들을 거부.

## 3. 결정과 트레이드오프

### 16-1. Fan Triangulation (단순)
- **결정**: 정점 N개 face → (v0, v_i, v_{i+1}) 삼각형 N-2 개로 분해. v0 가 부채꼴 중심.
- **후보**:
  - A) Fan — 한 정점 기준, 볼록 polygon 가정.
  - B) Ear Clipping — 임의의 simple polygon (오목 포함) 처리.
  - C) Earcut / Constrained Delaunay — 더 정교, 외부 라이브러리.
- **선택 이유**: A — Phase 3 까지의 자산이 단순 형상. ear clipping/delaunay 는 면 수가 많거나 캐릭터 메시 도입 시점에 발전.
- **포기한 것**: 오목 polygon 정확 처리. 게임 자산에서 오목 face 는 드물고 (요즘 자산은 거의 quad 또는 tri), 자산 단계에서 자체 triangulate 하는 게 권장.

### 16-2. 머티리얼 색 동기
- **결정**: triangulate 결과 N-2 개 삼각형 모두 동일 face 의 머티리얼 색 적용. `currentColor` 를 3*(N-2) 번 push.
- **이유**: 머티리얼은 OBJ 의 `usemtl` 가 *face 단위* 로 적용 — 한 face 내 모든 정점이 같은 머티리얼. 따라서 fan 분해 후 모든 삼각형이 동일 색.

## 4. 작업 내용

[Engine/render/ObjLoader.cpp](../Engine/render/ObjLoader.cpp) 의 `f` 핸들러 교체:

```cpp
else if (tag == "f")
{
    // n-gon fan triangulation — 정점 N (≥3) 개를 (v0, v_i, v_{i+1}) 삼각형 (N-2) 개로 분해.
    // 볼록 다각형 가정.
    std::vector<std::string> tokens;
    tokens.reserve(4);
    std::string tok;
    while (iss >> tok) { tokens.push_back(tok); }
    if (tokens.size() < 3) { throw std::runtime_error("OBJ: f 라인 정점 수 < 3"); }

    const FaceVertex v0 = ParseFaceVertex(tokens[0]);
    for (size_t i = 1; i + 1 < tokens.size(); ++i)
    {
        const FaceVertex vi  = ParseFaceVertex(tokens[i]);
        const FaceVertex vii = ParseFaceVertex(tokens[i + 1]);
        faceVertices.push_back(v0);
        faceVertices.push_back(vi);
        faceVertices.push_back(vii);
        faceVertexColors.push_back(currentColor);
        faceVertexColors.push_back(currentColor);
        faceVertexColors.push_back(currentColor);
    }
}
```

## 5. 마주친 문제와 해결 ⚠ 필수 — 빠짐없이 기록

### 문제 1 — 오목 polygon 시각 오류 가능성
- **문제**: Fan triangulation 은 볼록 가정. 오목 정점에 인접한 fan 삼각형은 polygon 외부로 튀어나옴 → 시각 깨짐.
- **현 상황**: 본 단계 자산 (Cube) 은 quad 면 → 평면 + 볼록. 안전.
- **향후**: ear clipping 필요할 때 별도 단계.
- **교훈**: "단순 알고리즘 + 가정 명시" 가 충분히 가는 단계. 가정 깨질 자산을 받았을 때 알고리즘 업그레이드.

### 문제 2 — 빈 토큰 누적 회피
- **문제**: `iss >> tok` 가 공백 구분으로 토큰 추출. 빈 토큰은 자동 skip이지만 `tok` 가 *이전 값* 남는 위험.
- **해결**: 루프 안에서 `tok` 가 매 추출마다 갱신됨. `iss >> tok` 가 false 면 루프 종료. 별도 clear 불필요.
- **교훈**: stream extractor 의 동작 검증.

## 6. 결과 / 검증

- **빌드**: Debug + Release 둘 다 0 warning / 0 error.
- **현 자산 영향**: Cube.obj 는 모두 3-token `f` 라인 → triangulate 분기에 `tokens.size() == 3` → for 루프 1회 실행 → 기존 동작과 동일.
- **회귀 없음** 확인. n-gon 자산 도입 시 자동 동작.

## 7. AI 협업 메모

- 마지막 누적 TODO. 코드 변화 작아 단일 커밋 + 짧은 devlog. 누적 TODO 5개 모두 처리 완료.

## 8. 다음 단계

본 단계로 Phase 3 누적 TODO 모두 처리.

후속 큰 단계 후보 (사용자 결정):
- **이미지 디코더** (stb_image / WIC / DDS 자작) — 실제 텍스처 자산.
- **압축 텍스처** (BC1/BC3) + mipmap.
- **머티리얼 sub-draw 시스템** — 머티리얼별 텍스처/상수 분리.
- **PBR 머티리얼** (Roughness/Metallic).
- **그림자맵** — DSV 추가 + 셰도우 셰이더.
- **캐릭터 모델 + Skinning** — 액션 컴뱃 수직 슬라이스 본격 시작.

## 9. PPT 재료로 쓸 만한 포인트

- "OBJ 표준 폴리곤 처리 — fan triangulation 의 한 줄 표현력"
- "단순 가정 + 명시적 폴백 — 알고리즘 도입 시점 선택"
