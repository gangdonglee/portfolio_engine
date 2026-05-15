#pragma once

#include "core/Types.h"
#include "render/VertexBuffer.h"
#include "render/IndexBuffer.h"

#include <DirectXMath.h>
#include <memory>

struct ID3D12GraphicsCommandList;

namespace engine::render
{
    class Device;

    // VertexBuffer + IndexBuffer 묶음. 셰이더의 입력 레이아웃과 1:1 대응되는 표준 정점 형식.
    //
    // 표준 정점 형식: POSITION(float3) + NORMAL(float3) + TEXCOORD(float2) + COLOR(float3). 44바이트.
    //   PipelineState 의 kHelloTriangleInputLayout 과 일치.
    //
    // 단일 소유 (복사·이동 금지). 사용자가 unique_ptr<Mesh> 로 래핑하면 이동 가능.
    class Mesh final
    {
    public:
        struct Vertex
        {
            DirectX::XMFLOAT3 position;
            DirectX::XMFLOAT3 normal;
            DirectX::XMFLOAT2 uv;
            DirectX::XMFLOAT3 color;
        };

        Mesh(Device&       device,
             const Vertex* vertices, uint32 vertexCount,
             const uint16* indices,  uint32 indexCount);
        ~Mesh();

        Mesh(const Mesh&)            = delete;
        Mesh& operator=(const Mesh&) = delete;
        Mesh(Mesh&&)                 = delete;
        Mesh& operator=(Mesh&&)      = delete;

        // VertexBuffer + IndexBuffer 둘 다 IA 단계에 바인딩.
        void Bind(ID3D12GraphicsCommandList* list) const;

        // DrawIndexedInstanced(IndexCount, 1, 0, 0, 0). 호출 전 Bind 와 PSO/RootSig 셋업 전제.
        void Draw(ID3D12GraphicsCommandList* list) const;

        uint32 IndexCount() const noexcept;

    private:
        std::unique_ptr<VertexBuffer> m_vb;
        std::unique_ptr<IndexBuffer>  m_ib;
    };
}
