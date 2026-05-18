#include "render/Mesh.h"

#include "render/Device.h"

#include <Windows.h>
#include <d3d12.h>
#include <dxgiformat.h>

#include <stdexcept>

namespace engine::render
{
    namespace
    {
        // 단일 머티리얼 ctor 들이 공유하는 폴백 머티리얼 — 흰색.
        std::shared_ptr<Material> MakeFallbackMaterial()
        {
            auto m = std::make_shared<Material>();
            m->name = L"<fallback>";
            m->diffuseColor = { 1.0f, 1.0f, 1.0f };
            return m;
        }
    }

    Mesh::Mesh(Device&       device,
               const Vertex* vertices, uint32 vertexCount,
               const uint16* indices,  uint32 indexCount)
        : m_vb(std::make_unique<VertexBuffer>(
              device,
              vertices,
              static_cast<uint32>(vertexCount * sizeof(Vertex)),
              static_cast<uint32>(sizeof(Vertex))))
    {
        SubMesh sm;
        sm.ib = std::make_unique<IndexBuffer>(
            device, indices,
            static_cast<uint32>(indexCount * sizeof(uint16)),
            DXGI_FORMAT_R16_UINT);
        sm.material = MakeFallbackMaterial();
        m_subs.push_back(std::move(sm));
    }

    Mesh::Mesh(Device&       device,
               const Vertex* vertices, uint32 vertexCount,
               const uint32* indices,  uint32 indexCount)
        : m_vb(std::make_unique<VertexBuffer>(
              device,
              vertices,
              static_cast<uint32>(vertexCount * sizeof(Vertex)),
              static_cast<uint32>(sizeof(Vertex))))
    {
        SubMesh sm;
        sm.ib = std::make_unique<IndexBuffer>(
            device, indices,
            static_cast<uint32>(indexCount * sizeof(uint32)),
            DXGI_FORMAT_R32_UINT);
        sm.material = MakeFallbackMaterial();
        m_subs.push_back(std::move(sm));
    }

    Mesh::Mesh(Device&                                       device,
               const Vertex*                                 vertices,
               uint32                                        vertexCount,
               const std::vector<std::vector<uint32>>&       subIndices,
               const std::vector<std::shared_ptr<Material>>& subMaterials)
        : m_vb(std::make_unique<VertexBuffer>(
              device,
              vertices,
              static_cast<uint32>(vertexCount * sizeof(Vertex)),
              static_cast<uint32>(sizeof(Vertex))))
    {
        if (subIndices.size() != subMaterials.size())
        {
            throw std::runtime_error("Mesh: subIndices.size() != subMaterials.size()");
        }
        m_subs.reserve(subIndices.size());
        for (size_t i = 0; i < subIndices.size(); ++i)
        {
            if (subIndices[i].empty()) { continue; }  // 빈 sub 는 폐기

            SubMesh sm;
            sm.ib = std::make_unique<IndexBuffer>(
                device,
                subIndices[i].data(),
                static_cast<uint32>(subIndices[i].size() * sizeof(uint32)),
                DXGI_FORMAT_R32_UINT);
            sm.material = subMaterials[i] ? subMaterials[i] : MakeFallbackMaterial();
            m_subs.push_back(std::move(sm));
        }
        if (m_subs.empty())
        {
            throw std::runtime_error("Mesh: subIndices 가 모두 비어있음");
        }
    }

    Mesh::~Mesh() = default;

    void Mesh::BindVertexBuffer(ID3D12GraphicsCommandList* list) const
    {
        m_vb->Bind(list);
    }

    void Mesh::DrawAll(ID3D12GraphicsCommandList* list,
                       uint32                     rootParamMaterialTable,
                       D3D12_GPU_DESCRIPTOR_HANDLE defaultSrvGpu) const
    {
        for (const SubMesh& sm : m_subs)
        {
            // 머티리얼 SRV 바인딩 — 텍스처 있으면 그것, 없으면 폴백.
            const D3D12_GPU_DESCRIPTOR_HANDLE srv =
                (sm.material && sm.material->albedoTexture)
                    ? sm.material->albedoSrvGpu
                    : defaultSrvGpu;
            list->SetGraphicsRootDescriptorTable(rootParamMaterialTable, srv);

            sm.ib->Bind(list);
            list->DrawIndexedInstanced(sm.ib->IndexCount(), 1, 0, 0, 0);
        }
    }

    const Material* Mesh::GetMaterial(size_t i) const noexcept
    {
        return (i < m_subs.size()) ? m_subs[i].material.get() : nullptr;
    }

    uint32 Mesh::TotalIndexCount() const noexcept
    {
        uint32 sum = 0;
        for (const SubMesh& sm : m_subs) { sum += sm.ib->IndexCount(); }
        return sum;
    }
}
