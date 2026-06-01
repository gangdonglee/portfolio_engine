#include "render/ProceduralTerrain.h"

#include "render/Material.h"
#include "render/Mesh.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace engine::render::procedural_terrain
{
    float DefaultHeightFunc(float x, float z) noexcept
    {
        // 다주파 합성 — 큰 언덕 + 중간 융기 + 작은 노이즈.
        float h = 0.0f;
        h += 50.0f * std::sin(x * 0.003f) * std::cos(z * 0.003f);
        h += 30.0f * std::sin(x * 0.008f + z * 0.005f);
        h += 15.0f * std::sin(x * 0.020f) * std::cos(z * 0.015f);
        return h;
    }

    std::unique_ptr<Mesh> Generate(
        Device&                                   device,
        float                                     widthUnits,
        float                                     depthUnits,
        int                                       segmentsX,
        int                                       segmentsZ,
        const std::function<float(float, float)>& heightFunc,
        const DirectX::XMFLOAT3&                  tintColor)
    {
        if (segmentsX < 1) { segmentsX = 1; }
        if (segmentsZ < 1) { segmentsZ = 1; }

        const int vx = segmentsX + 1;
        const int vz = segmentsZ + 1;

        std::vector<Mesh::Vertex> verts;
        verts.reserve(static_cast<std::size_t>(vx) * static_cast<std::size_t>(vz));

        const float halfW = widthUnits * 0.5f;
        const float halfD = depthUnits * 0.5f;
        const float dx    = widthUnits / static_cast<float>(segmentsX);
        const float dz    = depthUnits / static_cast<float>(segmentsZ);
        const float eps   = std::min(dx, dz);

        for (int iz = 0; iz < vz; ++iz)
        {
            const float z = -halfD + static_cast<float>(iz) * dz;
            for (int ix = 0; ix < vx; ++ix)
            {
                const float x = -halfW + static_cast<float>(ix) * dx;
                const float y = heightFunc(x, z);

                // Finite-difference normal — central difference.
                const float yL = heightFunc(x - eps, z);
                const float yR = heightFunc(x + eps, z);
                const float yD = heightFunc(x, z - eps);
                const float yU = heightFunc(x, z + eps);
                const float dydx = (yR - yL) / (2.0f * eps);
                const float dydz = (yU - yD) / (2.0f * eps);
                float nx = -dydx, ny = 1.0f, nz = -dydz;
                const float invLen = 1.0f / std::sqrt(nx * nx + ny * ny + nz * nz);

                Mesh::Vertex v{};
                v.position = { x, y, z };
                v.normal   = { nx * invLen, ny * invLen, nz * invLen };
                v.uv       = { static_cast<float>(ix) / static_cast<float>(segmentsX),
                               static_cast<float>(iz) / static_cast<float>(segmentsZ) };
                v.color    = tintColor;
                // boneIndices / boneWeights default zero — no skinning.
                verts.push_back(v);
            }
        }

        // Indices — 각 quad 를 2 triangle 로. Winding 은 엔진의 기존 mesh 패턴 (CCW 또는 CW) 일치 필요.
        // FbxLoader 가 생성한 mesh 가 정상 표시되는 winding 과 맞춰서 시도 → 안 보이면 swap.
        std::vector<uint32> indices;
        indices.reserve(static_cast<std::size_t>(segmentsX) * static_cast<std::size_t>(segmentsZ) * 6);
        for (int iz = 0; iz < segmentsZ; ++iz)
        {
            for (int ix = 0; ix < segmentsX; ++ix)
            {
                const uint32 i0 = static_cast<uint32>(iz * vx + ix);
                const uint32 i1 = i0 + 1;
                const uint32 i2 = i0 + static_cast<uint32>(vx);
                const uint32 i3 = i2 + 1;
                // Top-down view (camera 위에서 -Y 방향): CCW = front.
                indices.push_back(i0); indices.push_back(i2); indices.push_back(i1);
                indices.push_back(i1); indices.push_back(i2); indices.push_back(i3);
            }
        }

        auto mat = std::make_shared<Material>();
        mat->name          = L"Terrain";
        mat->diffuseColor  = tintColor;
        // albedoTexture / albedoSrvGpu 는 null — shader 에서 fallback SRV 사용.

        std::vector<std::vector<uint32>>             subIdx { std::move(indices) };
        std::vector<std::shared_ptr<Material>>       subMats{ mat };

        return std::make_unique<Mesh>(
            device,
            verts.data(),
            static_cast<uint32>(verts.size()),
            subIdx,
            subMats);
    }
}
