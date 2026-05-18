#pragma once

#include <DirectXMath.h>
#include <memory>
#include <string>

namespace engine::render
{
    class Device;
    class Mesh;

    // Autodesk FBX SDK 기반 .fbx 로더 (Phase A — 메시 + 머티리얼 색만).
    //
    // 책임:
    //   - FbxManager/FbxScene/FbxImporter 라이프타임 RAII (LoadFbx 호출 1회 안에 모두 처리).
    //   - 모든 eMesh attribute 노드의 polygon 을 단일 Mesh::Vertex 컬렉션에 concat.
    //   - 좌표계 변환: FBX 기본 Y-up RH → D3D LH (Y/Z swap + face winding 반전).
    //   - 머티리얼 Kd (diffuse 색) 를 정점 color 슬롯에 굽기 (OBJ MTL 과 동일 패턴).
    //
    // 본 단계 제외:
    //   - 스키닝 (bone weight / cluster) — 셰이더에 본 팔레트 cbuffer 도입 후 별도 단계.
    //   - 애니메이션 (FbxAnimStack / 키프레임) — 스키닝 도입 후.
    //   - 텍스처 자산 로드 — diffuseTexName 경로만 추출, 8x8 체커보드 알베도 유지.
    //   - 탄젠트 — Vertex 에 슬롯 없음.
    //
    // 함수 형식 (free function in namespace), ObjLoader 와 동일 패턴.
    namespace fbx_loader
    {
        // FBX 파일을 로드해 단일 Mesh 로 변환. 다중 메시 노드는 모두 concat.
        // defaultColor: 머티리얼이 없거나 Kd 가 0,0,0 인 면의 폴백 색.
        std::unique_ptr<Mesh> LoadFbx(
            Device&                  device,
            const wchar_t*           absolutePath,
            const DirectX::XMFLOAT3& defaultColor);

        // exe 옆 Resources\FBX\ 절대 경로 — 학습자료 폴더 구조 차용.
        std::wstring DefaultFbxDir();
    }
}
