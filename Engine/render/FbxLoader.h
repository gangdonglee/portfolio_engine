#pragma once

#include <DirectXMath.h>
#include <memory>
#include <string>

namespace engine::render
{
    class Device;
    class CommandQueue;
    class CommandList;
    class SrvDescriptorHeap;
    class Mesh;

    // Autodesk FBX SDK 기반 .fbx 로더 (Phase A — 메시 + 머티리얼 + 디퓨즈 텍스처).
    //
    // 책임:
    //   - FbxManager/FbxScene/FbxImporter 라이프타임 RAII.
    //   - 모든 eMesh attribute 노드의 polygon 을 단일 vertex 컬렉션 + 머티리얼별 sub-indices 로 분리.
    //   - 좌표계 변환: FBX 기본 Y-up RH → D3D LH (Y/Z swap + face winding 반전).
    //   - 머티리얼 Kd 색을 정점 color 슬롯에 굽기 + diffuse 텍스처 자동 로드 (WIC) + SRV 등록.
    //
    // 본 단계 제외:
    //   - 스키닝 / 애니메이션 — 셰이더 본 팔레트 cbuffer 도입 후 별도 단계.
    //   - normal/specular map — 슬롯만 정의되어 있고 본 단계 미적용.
    //   - 탄젠트 — Vertex 슬롯 없음.
    //
    // 결과: Mesh (다중 SubMesh, 각 SubMesh = IndexBuffer + Material).
    namespace fbx_loader
    {
        // FBX 파일 + 그 .fbm 폴더의 텍스처 자동 로드. 머티리얼별 sub-mesh 분리.
        // queue/list 는 텍스처 업로드 1회용 (Texture ctor 와 동일 패턴).
        // srvHeap 은 머티리얼별 텍스처 SRV 등록처 — 호출자는 충분한 capacity 보장.
        // defaultColor: 머티리얼이 없거나 Kd 가 0,0,0 인 폴백.
        std::unique_ptr<Mesh> LoadFbx(
            Device&                  device,
            CommandQueue&            queue,
            CommandList&             list,
            SrvDescriptorHeap&       srvHeap,
            const wchar_t*           absolutePath,
            const DirectX::XMFLOAT3& defaultColor);

        // exe 옆 Resources\FBX\ 절대 경로.
        std::wstring DefaultFbxDir();
    }
}
