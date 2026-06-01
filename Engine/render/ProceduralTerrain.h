#pragma once

#include <DirectXMath.h>

#include <functional>
#include <memory>

namespace engine::render
{
    class Device;
    class Mesh;

    // Procedural heightmap terrain — 그리드 mesh + 정점별 height/normal.
    //   Mesh 생성 시간 비용 적음 (CPU 측 정점 채움). 추후 GPU compute 로 옮길 수 있음.
    //   기본 색 = 풀밭 톤. 머티리얼은 albedo texture 없이 diffuseColor 만.
    namespace procedural_terrain
    {
        // heightFunc(x, z) → y. caller 가 단순 sin/cos 또는 Perlin noise 등 전달.
        std::unique_ptr<Mesh> Generate(
            Device&                                  device,
            float                                    widthUnits,
            float                                    depthUnits,
            int                                      segmentsX,
            int                                      segmentsZ,
            const std::function<float(float, float)>& heightFunc,
            const DirectX::XMFLOAT3&                 tintColor = { 0.45f, 0.55f, 0.35f });

        // 표준 패턴 — sin/cos 다중 주파수 합성. 시연 디폴트.
        //   x, z 가 큰 값일 때도 부드러운 언덕 + 작은 융기 패턴.
        float DefaultHeightFunc(float x, float z) noexcept;

        // Foot IK / character placement 등에서 ground Y 조회 — Generate 와 동일 height func.
        //   별도 함수 — Generate 의 콜백 캐시 없이도 sample 가능.
        inline float SampleDefaultHeight(float x, float z) noexcept { return DefaultHeightFunc(x, z); }
    }
}
