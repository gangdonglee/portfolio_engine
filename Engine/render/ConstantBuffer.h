#pragma once

#include "core/Types.h"

#include <wrl/client.h>

struct ID3D12Resource;

namespace engine::render
{
    class Device;

    // Upload heap 기반 상수 버퍼. CPU 가 매 프레임 쓰고 GPU 가 읽음.
    //
    // 동작:
    //   - 생성 시 byteSize 를 256바이트 정렬로 올린 크기로 Upload heap 리소스 생성.
    //   - Map 을 유지 (Unmap 안 함) — 매 프레임 memcpy 빈번하므로.
    //   - Update(data, size) 로 사용자 데이터 복사.
    //   - GpuAddress() 로 root descriptor (SetGraphicsRootConstantBufferView) 에 전달.
    //
    // 향후 확장:
    //   - 다중 슬롯(링 버퍼) — 한 프레임에 여러 번 갱신 + N프레임 in-flight 안전.
    //   - 디스크립터 테이블 기반 CBV 도 별도 ConstantBufferView 클래스.
    //
    // 단일 소유 (복사·이동 금지).
    class ConstantBuffer final
    {
    public:
        ConstantBuffer(Device& device, uint32 byteSize);
        ~ConstantBuffer();

        ConstantBuffer(const ConstantBuffer&)            = delete;
        ConstantBuffer& operator=(const ConstantBuffer&) = delete;
        ConstantBuffer(ConstantBuffer&&)                 = delete;
        ConstantBuffer& operator=(ConstantBuffer&&)      = delete;

        // 사용자 데이터를 매핑된 GPU 메모리에 복사. byteSize 는 align 된 m_byteSize 이내여야 한다.
        void Update(const void* data, uint32 byteSize);

        // GPU 가상 주소 — SetGraphicsRootConstantBufferView 인자.
        uint64 GpuAddress() const noexcept;

        // 정렬된 총 크기.
        uint32 AlignedByteSize() const noexcept;

    private:
        Microsoft::WRL::ComPtr<ID3D12Resource> m_buffer;
        void*  m_mapped     = nullptr;
        uint64 m_gpuAddress = 0;
        uint32 m_byteSize   = 0;  // 사용자 요청 (정렬 전)
        uint32 m_aligned    = 0;  // 256 정렬 후
    };
}
