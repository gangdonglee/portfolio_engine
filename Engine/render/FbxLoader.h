#pragma once

#include <DirectXMath.h>
#include <memory>
#include <string>
#include <vector>

namespace engine::render
{
    class Device;
    class CommandQueue;
    class CommandList;
    class SrvDescriptorHeap;
    class Mesh;
    class Skeleton;
    class AnimClip;

    // Autodesk FBX SDK 기반 .fbx 로더 (Phase B — 메시 + 머티리얼 + 텍스처 + 스키닝 + 애니메이션).
    //
    // 책임:
    //   - FbxManager/FbxScene/FbxImporter 라이프타임 RAII.
    //   - LoadBones: 본 트리 + 부모 인덱스 추출.
    //   - LoadAnimationInfo: 모든 FbxAnimStack → AnimClip 생성.
    //   - ParseNode 재귀: 메시 노드의 polygon + 머티리얼 sub-indices + diffuse 텍스처 자동 로드 + 스키닝 정점 채움.
    //   - 좌표계 변환: FBX 기본 Y-up RH → D3D LH (Y/Z swap + face winding 반전).
    //
    // 본 단계 제외:
    //   - normal/specular map (FBX 슬롯만 정의되어 있고 미적용)
    //   - Lerp/Slerp 보간 (Animator 가 nearest-frame)
    //   - 본 단계는 LineSkin/RigidSkin 만 — Cluster 기반.
    namespace fbx_loader
    {
        // FBX 로드 결과 — 메시 + (있으면) 스키레톤 + 애니메이션 클립들.
        // skeleton 이 nullptr 이면 정적 메시 (스키닝 없음).
        struct LoadedFbxModel
        {
            std::unique_ptr<Mesh>                       mesh;
            std::unique_ptr<Skeleton>                   skeleton;          // nullable
            std::vector<std::unique_ptr<AnimClip>>      clips;             // empty if no animation
        };

        // queue/list 는 텍스처 업로드 1회용.
        // srvHeap 은 머티리얼 텍스처 SRV 등록처 — 호출자가 capacity 보장.
        // defaultColor: 머티리얼이 없거나 Kd 가 0,0,0 인 폴백.
        LoadedFbxModel LoadFbx(
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
