#pragma once

#include "core/Types.h"
#include "render/IndexBuffer.h"
#include "render/Material.h"
#include "render/VertexBuffer.h"

#include <DirectXMath.h>
#include <memory>
#include <vector>

struct ID3D12GraphicsCommandList;

namespace engine::render
{
    class Device;

    // VertexBuffer + 다중 SubMesh (각 SubMesh = IndexBuffer + Material) 묶음.
    //
    // 단일 VB + 머티리얼별 sub-indices 패턴 — FBX 의 다중 머티리얼 메시 그대로 표현.
    // OBJ 처럼 단일 머티리얼은 SubMesh 하나로 폴백.
    //
    // 표준 정점 형식 (76 바이트):
    //   POSITION(float3) + NORMAL(float3) + TEXCOORD(float2) + COLOR(float3)
    //   + BLENDINDICES(uint4) + BLENDWEIGHT(float4)
    //   = 12 + 12 + 8 + 12 + 16 + 16 = 76 바이트.
    // PipelineState 의 kHelloTriangleInputLayout 과 일치.
    // 스키닝 미사용 메시는 boneIndices = {0,0,0,0} + boneWeights = {0,0,0,0} 으로 두면
    // HLSL VS 가 weight 합 0 분기에서 정점을 그대로 통과시킨다.
    //
    // 단일 소유 (복사·이동 금지). 사용자가 unique_ptr<Mesh> 로 래핑하면 이동 가능.
    class Mesh final
    {
    public:
        struct Vertex
        {
            DirectX::XMFLOAT3  position;
            DirectX::XMFLOAT3  normal;
            DirectX::XMFLOAT2  uv;
            DirectX::XMFLOAT3  color;
            DirectX::XMUINT4   boneIndices{ 0, 0, 0, 0 };
            DirectX::XMFLOAT4  boneWeights{ 0.0f, 0.0f, 0.0f, 0.0f };
        };

        // 단일 머티리얼 메시 (OBJ/Cube 경로). SubMesh 1개로 폴백.
        // R16 인덱스 — 65535 정점 한계.
        Mesh(Device&       device,
             const Vertex* vertices, uint32 vertexCount,
             const uint16* indices,  uint32 indexCount);

        // 단일 머티리얼 + R32 인덱스 (Dragon 같은 65535 초과 메시용 단순화 경로).
        Mesh(Device&       device,
             const Vertex* vertices, uint32 vertexCount,
             const uint32* indices,  uint32 indexCount);

        // 다중 머티리얼 메시 (FBX 경로). 정점은 공유, sub-indices 컬렉션 각각이 SubMesh.
        // subIndices.size() 와 subMaterials.size() 가 일치해야 함.
        // R32 인덱스 — 캐릭터 메시 대응.
        Mesh(Device&                                       device,
             const Vertex*                                 vertices,
             uint32                                        vertexCount,
             const std::vector<std::vector<uint32>>&       subIndices,
             const std::vector<std::shared_ptr<Material>>& subMaterials);

        ~Mesh();

        Mesh(const Mesh&)            = delete;
        Mesh& operator=(const Mesh&) = delete;
        Mesh(Mesh&&)                 = delete;
        Mesh& operator=(Mesh&&)      = delete;

        // VertexBuffer 만 IA 단계에 바인딩. SubMesh 의 IB 는 각 Draw 호출 시 자체적으로 바인딩.
        void BindVertexBuffer(ID3D12GraphicsCommandList* list) const;

        // 모든 SubMesh 를 순회하며 머티리얼 SRV 바인딩 + DrawIndexed.
        // rootParamMaterialTable: SetGraphicsRootDescriptorTable 의 root parameter 인덱스.
        // defaultSrvGpu: 머티리얼에 albedoTexture 가 없을 때 사용할 폴백 SRV (예: 1x1 흰색).
        void DrawAll(ID3D12GraphicsCommandList* list,
                     uint32                     rootParamMaterialTable,
                     D3D12_GPU_DESCRIPTOR_HANDLE defaultSrvGpu) const;

        size_t                       SubMeshCount() const noexcept { return m_subs.size(); }
        const Material*              GetMaterial(size_t i) const noexcept;
        // SubMesh 의 IndexCount 합계 (디버그/로그용).
        uint32                       TotalIndexCount() const noexcept;

    private:
        struct SubMesh
        {
            std::unique_ptr<IndexBuffer> ib;
            std::shared_ptr<Material>    material;
        };

        std::unique_ptr<VertexBuffer> m_vb;
        std::vector<SubMesh>          m_subs;
    };
}
