#pragma once

#include "core/Types.h"

#include <DirectXMath.h>
#include <d3d12.h>
#include <memory>
#include <string>

namespace engine::render
{
    class Texture;

    // 머티리얼 — 메시 sub-draw 단위. 머티리얼 1개당 PS 가 한 번 SetGraphicsRootDescriptorTable + DrawIndexed.
    //
    // 현 단계 멤버:
    //   - name (디버그)
    //   - diffuseColor (Kd) — 텍스처 없을 때 정점 color 슬롯의 폴백 색
    //   - albedoTexture (shared_ptr) — 머티리얼별 알베도. nullable.
    //   - albedoSrvGpu — 셰이더 t0 슬롯 바인딩용 GPU handle. albedoTexture 가 nullptr 이면 0.
    //
    // 향후 확장: normal/specular/PBR map, alpha mode, two-sided 등.
    //
    // 단일 책임 — 머티리얼 데이터 컨테이너. PSO 선택은 본 단계 단일 PSO 라 외부 처리.
    class Material final
    {
    public:
        Material() = default;

        std::wstring                 name;
        DirectX::XMFLOAT3            diffuseColor{ 1.0f, 1.0f, 1.0f };
        std::shared_ptr<Texture>     albedoTexture;     // nullable
        D3D12_GPU_DESCRIPTOR_HANDLE  albedoSrvGpu{};    // CreateSrv 후 채워짐, 없으면 0
    };
}
