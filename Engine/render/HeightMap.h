#pragma once

#include "core/Types.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::render
{
    // 데이터 주도 높이맵 — 에디터가 스컬프트(브러시)로 편집하고 .hmap 바이너리로 저장,
    //   클라이언트가 로드해 지형 메시 생성 + ground 샘플러로 사용. 절차적 sin/cos 지형을 대체.
    //
    //   좌표: world XZ ∈ [-worldWidth/2, +worldWidth/2] × [-worldDepth/2, +worldDepth/2] 를
    //   cols×rows 그리드에 매핑. heights[row*cols + col] = 정점 높이(y). row↔z, col↔x.
    //   Sample() 은 bilinear 보간 (정점 사이 부드러운 높이) — 메시·ground 샘플러 동일 사용.
    class HeightMap
    {
    public:
        enum class Brush { Raise, Lower, Smooth };

        HeightMap() = default;

        // 평탄(또는 상수 높이) 그리드 생성.
        static HeightMap CreateFlat(int cols, int rows,
                                    float worldWidth, float worldDepth,
                                    float height = 0.0f);

        // heightFunc(x,z) 를 각 정점에 샘플해 그리드 베이크 (절차적 지형을 시작점으로).
        static HeightMap Bake(int cols, int rows,
                              float worldWidth, float worldDepth,
                              const std::function<float(float, float)>& heightFunc);

        // .hmap 바이너리 로드/저장. 실패 시 false / throw 아님(호출자 폴백 가능).
        static bool Load(std::string_view path, HeightMap& out);
        bool        Save(std::string_view path) const;

        // world XZ → 높이 (bilinear, 경계 clamp). 빈 맵이면 0.
        float Sample(float x, float z) const noexcept;

        // 브러시 적용 — (wx,wz) 중심 radius(world units) 안의 정점을 falloff(smoothstep)로 편집.
        //   Raise/Lower: ±strength·falloff. Smooth: 이웃 평균 쪽으로 strength·falloff 만큼 lerp.
        //   편집된 정점이 있으면 true (메시 재생성 트리거용).
        bool Sculpt(float wx, float wz, float radius, float strength, Brush brush);

        bool  Empty()  const noexcept { return m_heights.empty(); }
        int   Cols()   const noexcept { return m_cols; }
        int   Rows()   const noexcept { return m_rows; }
        float WorldWidth() const noexcept { return m_worldWidth; }
        float WorldDepth() const noexcept { return m_worldDepth; }
        const std::vector<float>& Heights() const noexcept { return m_heights; }

        // 정점(col,row) 의 world XZ.
        float VertexX(int col) const noexcept;
        float VertexZ(int row) const noexcept;

    private:
        int                m_cols = 0;
        int                m_rows = 0;
        float              m_worldWidth = 0.0f;
        float              m_worldDepth = 0.0f;
        std::vector<float> m_heights;   // row-major: [row*cols + col]
    };
}
