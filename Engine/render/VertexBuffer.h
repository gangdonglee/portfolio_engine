#pragma once

#include <cstdint>
#include <wrl/client.h>

struct ID3D12Resource;
struct ID3D12GraphicsCommandList;

namespace engine::render
{
    class Device;

    // 정적 정점 데이터를 GPU 메모리에 업로드해 보관하는 RAII 버퍼.
    //
    // 단순화 전략 (Phase 1E-3):
    //   - D3D12_HEAP_TYPE_UPLOAD 직접 사용 (CPU writable + GPU readable).
    //   - 생성 시 Map → memcpy → Unmap 으로 즉시 데이터 채움.
    //   - 첫 삼각형 같은 정적 데이터엔 Default heap + staging copy 가 더 효율적이지만,
    //     본 단계엔 단순성 우선. 향후 정점 수가 많아지면 Default heap 으로 마이그레이션.
    //
    // 헤더 위생: D3D12_VERTEX_BUFFER_VIEW 를 멤버로 보유하지 않고 매번 cpp 에서 구성
    //   → d3d12.h 풀 노출 회피. 헤더는 GPU 주소(uint64) + size/stride 만 보관.
    //
    // 단일 소유 (복사·이동 금지).
    class VertexBuffer final
    {
    public:
        // data: 정점 배열 시작 주소 (생성 시점에만 읽음, 이후 미참조).
        // byteSize: 데이터 총 바이트 수.
        // stride: 한 정점의 바이트 크기 (D3D12_VERTEX_BUFFER_VIEW.StrideInBytes).
        VertexBuffer(Device& device,
                     const void*   data,
                     std::uint32_t byteSize,
                     std::uint32_t stride);
        ~VertexBuffer();

        VertexBuffer(const VertexBuffer&)            = delete;
        VertexBuffer& operator=(const VertexBuffer&) = delete;
        VertexBuffer(VertexBuffer&&)                 = delete;
        VertexBuffer& operator=(VertexBuffer&&)      = delete;

        // CommandList 의 IA 단계에 정점 버퍼 슬롯 바인딩. live 크기 (Update 로 갱신) 기준.
        void Bind(ID3D12GraphicsCommandList* list, std::uint32_t slot = 0) const;

        // 동적 갱신 — UPLOAD heap 이라 re-map 후 memcpy. newByteSize 는 생성 capacity 이하.
        //   debug line buffer 처럼 매 프레임 내용이 바뀌는 용도. stride 는 불변.
        //   newByteSize == 0 이면 빈 버퍼 (DrawInstanced 0 — no-op).
        void Update(const void* data, std::uint32_t newByteSize);

        std::uint32_t VertexCount() const noexcept;

    private:
        Microsoft::WRL::ComPtr<ID3D12Resource> m_buffer;
        std::uint64_t m_gpuAddress    = 0;  // D3D12_GPU_VIRTUAL_ADDRESS == UINT64
        std::uint32_t m_byteSize      = 0;  // capacity (생성 시 고정)
        std::uint32_t m_liveByteSize  = 0;  // 현재 유효 바이트 (Update 로 갱신, ≤ capacity)
        std::uint32_t m_stride        = 0;
    };
}
