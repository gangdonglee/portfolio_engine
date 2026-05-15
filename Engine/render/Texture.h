#pragma once

#include "core/Types.h"

#include <wrl/client.h>

#include <d3d12.h>
#include <dxgiformat.h>

struct ID3D12Resource;

namespace engine::render
{
    class Device;
    class CommandQueue;
    class CommandList;
    class SrvDescriptorHeap;

    // RGBA8 2D 텍스처 (Default heap 거주).
    //
    // 생성자 단계:
    //   ① Default heap 텍스처 리소스 (initial state = COPY_DEST).
    //   ② Upload heap 스테이징 버퍼.
    //   ③ 픽셀 데이터 → 스테이징 (Map/memcpy/Unmap).
    //   ④ list.Reset() → CopyTextureRegion → barrier(COPY_DEST → PIXEL_SHADER_RESOURCE).
    //   ⑤ queue.Execute(list) → queue.FlushGpu() — 동기 업로드 완료 대기.
    //   ⑥ 스테이징 ComPtr 자동 해제.
    //
    // CreateSrv() 로 외부 SrvDescriptorHeap 에 SRV 등록 + GPU handle 보유.
    //
    // 본 단계는 RGBA8 단일 mip 만. 향후 압축 포맷(BC1/BC3) / mipmap / cubemap 추가.
    //
    // 단일 소유 (복사·이동 금지).
    class Texture final
    {
    public:
        // RGBA8 픽셀 데이터(row-major, 4*width*height 바이트)를 GPU 에 업로드.
        // queue 와 list 는 업로드 1회용 — 호출 후 list 는 다음 record 를 위해 재사용 가능.
        Texture(Device&       device,
                CommandQueue& queue,
                CommandList&  list,
                const void*   rgba8Pixels,
                uint32        width,
                uint32        height);
        ~Texture();

        Texture(const Texture&)            = delete;
        Texture& operator=(const Texture&) = delete;
        Texture(Texture&&)                 = delete;
        Texture& operator=(Texture&&)      = delete;

        // SrvDescriptorHeap 의 다음 슬롯에 SRV 등록. 결과 GPU handle 을 내부 보관.
        void CreateSrv(Device& device, SrvDescriptorHeap& heap);

        // CreateSrv 호출 후의 GPU handle. SetGraphicsRootDescriptorTable 에 전달.
        D3D12_GPU_DESCRIPTOR_HANDLE SrvGpuHandle() const noexcept { return m_srvGpu; }

        ID3D12Resource* Native() const noexcept;
        uint32          Width()  const noexcept { return m_width; }
        uint32          Height() const noexcept { return m_height; }
        DXGI_FORMAT     Format() const noexcept { return DXGI_FORMAT_R8G8B8A8_UNORM; }

    private:
        Microsoft::WRL::ComPtr<ID3D12Resource> m_buffer;
        D3D12_GPU_DESCRIPTOR_HANDLE            m_srvGpu{};
        uint32 m_width  = 0;
        uint32 m_height = 0;
    };
}
