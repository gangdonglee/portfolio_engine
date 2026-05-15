#include "render/Mesh.h"

#include "render/Device.h"

#include <Windows.h>
#include <d3d12.h>
#include <dxgiformat.h>

namespace engine::render
{
    Mesh::Mesh(Device&       device,
               const Vertex* vertices, uint32 vertexCount,
               const uint16* indices,  uint32 indexCount)
        : m_vb(std::make_unique<VertexBuffer>(
              device,
              vertices,
              static_cast<uint32>(vertexCount * sizeof(Vertex)),
              static_cast<uint32>(sizeof(Vertex))))
        , m_ib(std::make_unique<IndexBuffer>(
              device,
              indices,
              static_cast<uint32>(indexCount * sizeof(uint16)),
              DXGI_FORMAT_R16_UINT))
    {
    }

    Mesh::~Mesh() = default;

    void Mesh::Bind(ID3D12GraphicsCommandList* list) const
    {
        m_vb->Bind(list);
        m_ib->Bind(list);
    }

    void Mesh::Draw(ID3D12GraphicsCommandList* list) const
    {
        list->DrawIndexedInstanced(m_ib->IndexCount(), 1, 0, 0, 0);
    }

    uint32 Mesh::IndexCount() const noexcept
    {
        return m_ib->IndexCount();
    }
}
