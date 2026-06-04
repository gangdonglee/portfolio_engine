#include "render/HeightMap.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace engine::render
{
    namespace
    {
        constexpr char    kMagic[4] = { 'H', 'M', 'P', '1' };
        constexpr float   kSmoothStep(float t) noexcept { return t * t * (3.0f - 2.0f * t); }
    }

    HeightMap HeightMap::CreateFlat(int cols, int rows,
                                    float worldWidth, float worldDepth, float height)
    {
        HeightMap hm;
        hm.m_cols = std::max(2, cols);
        hm.m_rows = std::max(2, rows);
        hm.m_worldWidth = worldWidth;
        hm.m_worldDepth = worldDepth;
        hm.m_heights.assign(static_cast<size_t>(hm.m_cols) * hm.m_rows, height);
        return hm;
    }

    HeightMap HeightMap::Bake(int cols, int rows,
                              float worldWidth, float worldDepth,
                              const std::function<float(float, float)>& heightFunc)
    {
        HeightMap hm = CreateFlat(cols, rows, worldWidth, worldDepth, 0.0f);
        for (int r = 0; r < hm.m_rows; ++r)
        {
            const float z = hm.VertexZ(r);
            for (int c = 0; c < hm.m_cols; ++c)
            {
                hm.m_heights[static_cast<size_t>(r) * hm.m_cols + c] = heightFunc(hm.VertexX(c), z);
            }
        }
        return hm;
    }

    float HeightMap::VertexX(int col) const noexcept
    {
        const float t = (m_cols > 1) ? static_cast<float>(col) / static_cast<float>(m_cols - 1) : 0.0f;
        return -m_worldWidth * 0.5f + t * m_worldWidth;
    }

    float HeightMap::VertexZ(int row) const noexcept
    {
        const float t = (m_rows > 1) ? static_cast<float>(row) / static_cast<float>(m_rows - 1) : 0.0f;
        return -m_worldDepth * 0.5f + t * m_worldDepth;
    }

    float HeightMap::Sample(float x, float z) const noexcept
    {
        if (m_heights.empty()) { return 0.0f; }

        // world → 그리드 연속 좌표 (정점 인덱스 공간).
        const float gx = (x + m_worldWidth * 0.5f) / m_worldWidth * static_cast<float>(m_cols - 1);
        const float gz = (z + m_worldDepth * 0.5f) / m_worldDepth * static_cast<float>(m_rows - 1);

        const float cxf = std::clamp(gx, 0.0f, static_cast<float>(m_cols - 1));
        const float czf = std::clamp(gz, 0.0f, static_cast<float>(m_rows - 1));
        const int c0 = static_cast<int>(std::floor(cxf));
        const int r0 = static_cast<int>(std::floor(czf));
        const int c1 = std::min(c0 + 1, m_cols - 1);
        const int r1 = std::min(r0 + 1, m_rows - 1);
        const float fx = cxf - static_cast<float>(c0);
        const float fz = czf - static_cast<float>(r0);

        const auto at = [&](int c, int r) -> float {
            return m_heights[static_cast<size_t>(r) * m_cols + c];
        };
        const float h0 = at(c0, r0) * (1.0f - fx) + at(c1, r0) * fx;
        const float h1 = at(c0, r1) * (1.0f - fx) + at(c1, r1) * fx;
        return h0 * (1.0f - fz) + h1 * fz;
    }

    bool HeightMap::Sculpt(float wx, float wz, float radius, float strength, Brush brush)
    {
        if (m_heights.empty() || radius <= 0.0f) { return false; }

        // 영향 받는 그리드 인덱스 범위 (world radius → 셀 수).
        const float cellW = (m_cols > 1) ? m_worldWidth / static_cast<float>(m_cols - 1) : m_worldWidth;
        const float cellD = (m_rows > 1) ? m_worldDepth / static_cast<float>(m_rows - 1) : m_worldDepth;

        const float gcx = (wx + m_worldWidth * 0.5f) / m_worldWidth * static_cast<float>(m_cols - 1);
        const float gcz = (wz + m_worldDepth * 0.5f) / m_worldDepth * static_cast<float>(m_rows - 1);
        const int radC = static_cast<int>(std::ceil(radius / std::max(cellW, 1e-3f)));
        const int radR = static_cast<int>(std::ceil(radius / std::max(cellD, 1e-3f)));
        const int cMin = std::max(0, static_cast<int>(std::floor(gcx)) - radC);
        const int cMax = std::min(m_cols - 1, static_cast<int>(std::ceil(gcx)) + radC);
        const int rMin = std::max(0, static_cast<int>(std::floor(gcz)) - radR);
        const int rMax = std::min(m_rows - 1, static_cast<int>(std::ceil(gcz)) + radR);
        if (cMin > cMax || rMin > rMax) { return false; }

        const float invR = 1.0f / radius;
        bool changed = false;

        // Smooth 는 원본 복사본에서 이웃 평균을 읽어야 일관(in-place 누적 방지).
        std::vector<float> src;
        if (brush == Brush::Smooth) { src = m_heights; }
        const auto srcAt = [&](int c, int r) -> float {
            return (brush == Brush::Smooth ? src : m_heights)[static_cast<size_t>(r) * m_cols + c];
        };

        for (int r = rMin; r <= rMax; ++r)
        {
            const float vz = VertexZ(r);
            for (int c = cMin; c <= cMax; ++c)
            {
                const float vx = VertexX(c);
                const float dist = std::sqrt((vx - wx) * (vx - wx) + (vz - wz) * (vz - wz));
                if (dist >= radius) { continue; }
                const float falloff = kSmoothStep(1.0f - dist * invR);   // 중심 1 → 가장자리 0
                float& h = m_heights[static_cast<size_t>(r) * m_cols + c];

                switch (brush)
                {
                case Brush::Raise: h += strength * falloff; break;
                case Brush::Lower: h -= strength * falloff; break;
                case Brush::Smooth:
                {
                    // 3x3 이웃 평균 쪽으로 lerp.
                    float sum = 0.0f; int n = 0;
                    for (int dr = -1; dr <= 1; ++dr)
                        for (int dc = -1; dc <= 1; ++dc)
                        {
                            const int nc = c + dc, nr = r + dr;
                            if (nc < 0 || nc >= m_cols || nr < 0 || nr >= m_rows) { continue; }
                            sum += srcAt(nc, nr); ++n;
                        }
                    const float avg = (n > 0) ? sum / static_cast<float>(n) : h;
                    h += (avg - h) * std::clamp(strength * falloff, 0.0f, 1.0f);
                    break;
                }
                }
                changed = true;
            }
        }
        return changed;
    }

    bool HeightMap::Save(std::string_view path) const
    {
        if (m_heights.empty()) { return false; }
        std::FILE* f = nullptr;
        if (fopen_s(&f, std::string(path).c_str(), "wb") != 0 || f == nullptr) { return false; }

        bool ok = true;
        ok = ok && std::fwrite(kMagic, 1, 4, f) == 4;
        const uint32 cols = static_cast<uint32>(m_cols);
        const uint32 rows = static_cast<uint32>(m_rows);
        ok = ok && std::fwrite(&cols, sizeof(uint32), 1, f) == 1;
        ok = ok && std::fwrite(&rows, sizeof(uint32), 1, f) == 1;
        ok = ok && std::fwrite(&m_worldWidth, sizeof(float), 1, f) == 1;
        ok = ok && std::fwrite(&m_worldDepth, sizeof(float), 1, f) == 1;
        ok = ok && std::fwrite(m_heights.data(), sizeof(float), m_heights.size(), f) == m_heights.size();
        std::fclose(f);
        return ok;
    }

    bool HeightMap::Load(std::string_view path, HeightMap& out)
    {
        std::FILE* f = nullptr;
        if (fopen_s(&f, std::string(path).c_str(), "rb") != 0 || f == nullptr) { return false; }

        char magic[4] = {};
        uint32 cols = 0, rows = 0;
        float ww = 0.0f, wd = 0.0f;
        bool ok = true;
        ok = ok && std::fread(magic, 1, 4, f) == 4 && std::memcmp(magic, kMagic, 4) == 0;
        ok = ok && std::fread(&cols, sizeof(uint32), 1, f) == 1;
        ok = ok && std::fread(&rows, sizeof(uint32), 1, f) == 1;
        ok = ok && std::fread(&ww, sizeof(float), 1, f) == 1;
        ok = ok && std::fread(&wd, sizeof(float), 1, f) == 1;
        if (ok && (cols < 2 || rows < 2 || cols > 8192 || rows > 8192)) { ok = false; }
        if (ok)
        {
            out.m_cols = static_cast<int>(cols);
            out.m_rows = static_cast<int>(rows);
            out.m_worldWidth = ww;
            out.m_worldDepth = wd;
            out.m_heights.assign(static_cast<size_t>(cols) * rows, 0.0f);
            ok = std::fread(out.m_heights.data(), sizeof(float), out.m_heights.size(), f)
                 == out.m_heights.size();
        }
        std::fclose(f);
        if (!ok) { out = HeightMap{}; }
        return ok;
    }
}
