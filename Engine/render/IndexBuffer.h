#pragma once

#include "core/Types.h"

#include <wrl/client.h>

#include <dxgiformat.h>  // DXGI_FORMAT

struct ID3D12Resource;
struct ID3D12GraphicsCommandList;

namespace engine::render
{
    class Device;

    // 정적 인덱스 데이터를 GPU 메모리(Upload heap)에 업로드해 보관하는 RAII 버퍼.
    //
    // 헤더 위생: D3D12_INDEX_BUFFER_VIEW 멤버로 보관하지 않고 cpp 에서 매번 구성 →
    //   d3d12.h 풀 노출 회피 (VertexBuffer 와 동일 패턴).
    //
    // 지원 포맷: DXGI_FORMAT_R16_UINT (16비트) 또는 DXGI_FORMAT_R32_UINT (32비트).
    //
    // 단일 소유 (복사·이동 금지).
    class IndexBuffer final
    {
    public:
        // data: 인덱스 배열 시작 주소.
        // byteSize: 데이터 총 바이트 수.
        // format: DXGI_FORMAT_R16_UINT 또는 DXGI_FORMAT_R32_UINT.
        IndexBuffer(Device&     device,
                    const void* data,
                    uint32      byteSize,
                    DXGI_FORMAT format);
        ~IndexBuffer();

        IndexBuffer(const IndexBuffer&)            = delete;
        IndexBuffer& operator=(const IndexBuffer&) = delete;
        IndexBuffer(IndexBuffer&&)                 = delete;
        IndexBuffer& operator=(IndexBuffer&&)      = delete;

        // CommandList 의 IA 단계에 인덱스 버퍼 바인딩.
        void Bind(ID3D12GraphicsCommandList* list) const;

        uint32 IndexCount() const noexcept;

    private:
        Microsoft::WRL::ComPtr<ID3D12Resource> m_buffer;
        uint64      m_gpuAddress = 0;
        uint32      m_byteSize   = 0;
        DXGI_FORMAT m_format     = DXGI_FORMAT_R16_UINT;
    };
}
