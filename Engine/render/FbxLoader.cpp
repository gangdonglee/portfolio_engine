#include "render/FbxLoader.h"

#include "core/Logger.h"
#include "core/Types.h"
#include "render/Mesh.h"

#include <Windows.h>

#include <fbxsdk.h>
// fbxsdk/core/arch/fbxarch.h 가 `#define snprintf _snprintf` 매크로를 추가 — std::snprintf 호출을 깬다.
// fbxsdk 헤더가 자기 내부 코드 정상 동작을 위해 정의한 것 — 외부에서는 즉시 undef 로 안전 복원.
#ifdef snprintf
#undef snprintf
#endif

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace engine::render::fbx_loader
{
    namespace
    {
        using DirectX::XMFLOAT2;
        using DirectX::XMFLOAT3;

        struct MaterialInfo
        {
            XMFLOAT3 diffuseColor{ 1.0f, 1.0f, 1.0f };
            bool     hasDiffuse = false;
        };

        // FBX 의 좌표계는 본 코드에서 Y/Z swap 으로 LH 변환 (학습자료 패턴과 동일).
        XMFLOAT3 ConvertVec3(const FbxVector4& v) noexcept
        {
            return XMFLOAT3{
                static_cast<float>(v.mData[0]),
                static_cast<float>(v.mData[2]),
                static_cast<float>(v.mData[1])
            };
        }

        XMFLOAT2 ConvertUv(const FbxVector2& uv) noexcept
        {
            // OBJ 와 동일 — V축 뒤집기 (FBX bottom-up → D3D top-down).
            return XMFLOAT2{
                static_cast<float>(uv.mData[0]),
                1.0f - static_cast<float>(uv.mData[1])
            };
        }

        XMFLOAT3 ExtractKd(FbxSurfaceMaterial* mat)
        {
            if (mat == nullptr) { return { 1.0f, 1.0f, 1.0f }; }

            FbxProperty colorProp  = mat->FindProperty(FbxSurfaceMaterial::sDiffuse);
            FbxProperty factorProp = mat->FindProperty(FbxSurfaceMaterial::sDiffuseFactor);

            FbxDouble3 color{ 1.0, 1.0, 1.0 };
            FbxDouble  factor = 1.0;
            if (colorProp.IsValid())  { color  = colorProp.Get<FbxDouble3>(); }
            if (factorProp.IsValid()) { factor = factorProp.Get<FbxDouble>(); }

            return XMFLOAT3{
                static_cast<float>(color.mData[0] * factor),
                static_cast<float>(color.mData[1] * factor),
                static_cast<float>(color.mData[2] * factor)
            };
        }

        // GeometryElement mapping mode + reference mode 에 따라 (idx, vertexCounter) 로 실제 데이터 인덱스 산출.
        // FBX SDK 의 mapping/reference 조합은 4가지: ByControlPoint/ByPolygonVertex × Direct/IndexToDirect.
        int32 ResolveElementIndex(const FbxLayerElementTemplate<FbxVector4>* elem,
                                  int32 controlPointIdx,
                                  int32 vertexCounter) noexcept
        {
            const auto mapMode = elem->GetMappingMode();
            const auto refMode = elem->GetReferenceMode();
            if (mapMode == FbxGeometryElement::eByPolygonVertex)
            {
                return (refMode == FbxGeometryElement::eDirect)
                    ? vertexCounter
                    : elem->GetIndexArray().GetAt(vertexCounter);
            }
            if (mapMode == FbxGeometryElement::eByControlPoint)
            {
                return (refMode == FbxGeometryElement::eDirect)
                    ? controlPointIdx
                    : elem->GetIndexArray().GetAt(controlPointIdx);
            }
            return controlPointIdx;
        }

        int32 ResolveUvIndex(const FbxGeometryElementUV* elem,
                             int32 controlPointIdx,
                             int32 polyVertexUvIdx) noexcept
        {
            const auto mapMode = elem->GetMappingMode();
            const auto refMode = elem->GetReferenceMode();
            if (mapMode == FbxGeometryElement::eByPolygonVertex)
            {
                return (refMode == FbxGeometryElement::eDirect)
                    ? polyVertexUvIdx
                    : elem->GetIndexArray().GetAt(polyVertexUvIdx);
            }
            if (mapMode == FbxGeometryElement::eByControlPoint)
            {
                return (refMode == FbxGeometryElement::eDirect)
                    ? controlPointIdx
                    : elem->GetIndexArray().GetAt(controlPointIdx);
            }
            return controlPointIdx;
        }

        // 한 메시의 모든 polygon 을 전역 vertex/index 컬렉션에 append.
        // controlPoint 단위 dedup (학습자료 패턴). 즉 같은 controlPoint 의 normal/uv 는 마지막 polygon vertex 값 채택.
        void AppendMesh(FbxMesh*                   mesh,
                        const XMFLOAT3&            defaultColor,
                        std::vector<Mesh::Vertex>& outVertices,
                        std::vector<uint32>&       outIndices)
        {
            const int32 cpCount = mesh->GetControlPointsCount();
            if (cpCount <= 0) { return; }

            // 머티리얼별 색 추출 (다중 머티리얼 가능 — 각 polygon 이 자기 머티리얼 인덱스 보유).
            FbxNode* node = mesh->GetNode();
            std::vector<MaterialInfo> materials;
            if (node != nullptr)
            {
                const int32 matCount = node->GetMaterialCount();
                materials.reserve(matCount);
                for (int32 i = 0; i < matCount; ++i)
                {
                    MaterialInfo mi;
                    mi.diffuseColor = ExtractKd(node->GetMaterial(i));
                    mi.hasDiffuse   = true;
                    materials.push_back(mi);
                }
            }

            const FbxGeometryElementMaterial* matElem  = mesh->GetElementMaterial();
            const FbxGeometryElementNormal*   normElem = mesh->GetElementNormal();
            const FbxGeometryElementUV*       uvElem   = mesh->GetElementUV();

            // 글로벌 vertex 컬렉션에 본 메시의 controlPoints 를 베이스로 push.
            const uint32 vertexBase = static_cast<uint32>(outVertices.size());
            const FbxVector4* controlPoints = mesh->GetControlPoints();
            outVertices.resize(vertexBase + cpCount);
            for (int32 i = 0; i < cpCount; ++i)
            {
                Mesh::Vertex& v = outVertices[vertexBase + i];
                v.position = ConvertVec3(controlPoints[i]);
                v.normal   = { 0.0f, 1.0f, 0.0f };
                v.uv       = { 0.0f, 0.0f };
                v.color    = defaultColor;
            }

            // polygon 별 인덱스 + 정점 attribute 채우기.
            const int32 polyCount = mesh->GetPolygonCount();
            int32       vertexCounter = 0;
            for (int32 p = 0; p < polyCount; ++p)
            {
                const int32 polySize = mesh->GetPolygonSize(p);
                if (polySize != 3)
                {
                    // GeometryConverter::Triangulate 후엔 항상 3 — 그래도 방어.
                    vertexCounter += polySize;
                    continue;
                }

                // 머티리얼 색 — polygon 의 머티리얼 subset 인덱스 → materials[i].diffuseColor.
                XMFLOAT3 polyColor = defaultColor;
                if (matElem != nullptr && !materials.empty())
                {
                    const auto matMap = matElem->GetMappingMode();
                    int32 subsetIdx = 0;
                    if (matMap == FbxGeometryElement::eByPolygon)
                    {
                        subsetIdx = matElem->GetIndexArray().GetAt(p);
                    }
                    else if (matMap == FbxGeometryElement::eAllSame)
                    {
                        subsetIdx = matElem->GetIndexArray().GetAt(0);
                    }
                    if (subsetIdx >= 0 && subsetIdx < static_cast<int32>(materials.size()))
                    {
                        polyColor = materials[subsetIdx].diffuseColor;
                    }
                }

                uint32 cpIdxs[3];
                for (int32 j = 0; j < 3; ++j)
                {
                    const int32 cp = mesh->GetPolygonVertex(p, j);
                    cpIdxs[j] = static_cast<uint32>(cp);

                    Mesh::Vertex& v = outVertices[vertexBase + cp];

                    // Normal — controlPoint 또는 polygonVertex 단위. 후자면 마지막에 처리된 polygon vertex 값 채택.
                    if (normElem != nullptr)
                    {
                        const int32 ni = ResolveElementIndex(normElem, cp, vertexCounter);
                        v.normal = ConvertVec3(normElem->GetDirectArray().GetAt(ni));
                    }

                    // UV — GetTextureUVIndex 활용 (polygon-vertex UV index 직접 노출).
                    if (uvElem != nullptr)
                    {
                        const int32 polyUvIdx = mesh->GetTextureUVIndex(p, j);
                        const int32 ui = ResolveUvIndex(uvElem, cp, polyUvIdx);
                        v.uv = ConvertUv(uvElem->GetDirectArray().GetAt(ui));
                    }

                    // 머티리얼 색 굽기 — controlPoint 공유 시 마지막 색이 남음.
                    v.color = polyColor;

                    ++vertexCounter;
                }

                // Y/Z swap 으로 RH↔LH 변환 — face winding 도 반전 (0, 2, 1).
                outIndices.push_back(vertexBase + cpIdxs[0]);
                outIndices.push_back(vertexBase + cpIdxs[2]);
                outIndices.push_back(vertexBase + cpIdxs[1]);
            }
        }

        // FBX 트리 재귀 — eMesh attribute 노드의 메시를 AppendMesh.
        void ParseNode(FbxNode*                   node,
                       const XMFLOAT3&            defaultColor,
                       std::vector<Mesh::Vertex>& outVertices,
                       std::vector<uint32>&       outIndices)
        {
            if (node == nullptr) { return; }
            FbxNodeAttribute* attr = node->GetNodeAttribute();
            if (attr != nullptr && attr->GetAttributeType() == FbxNodeAttribute::eMesh)
            {
                AppendMesh(node->GetMesh(), defaultColor, outVertices, outIndices);
            }
            const int32 childCount = node->GetChildCount();
            for (int32 i = 0; i < childCount; ++i)
            {
                ParseNode(node->GetChild(i), defaultColor, outVertices, outIndices);
            }
        }
    } // anonymous

    std::unique_ptr<Mesh> LoadFbx(
        Device&                  device,
        const wchar_t*           absolutePath,
        const DirectX::XMFLOAT3& defaultColor)
    {
        // RAII — 함수 종료 시 manager 만 Destroy 하면 scene/importer 모두 자동 정리.
        FbxManager* manager = FbxManager::Create();
        if (manager == nullptr)
        {
            throw std::runtime_error("FbxManager::Create 실패");
        }
        struct ManagerGuard {
            FbxManager* m;
            ~ManagerGuard() { if (m) m->Destroy(); }
        } guard{ manager };

        FbxIOSettings* ios = FbxIOSettings::Create(manager, IOSROOT);
        manager->SetIOSettings(ios);

        FbxScene*    scene    = FbxScene::Create(manager, "");
        FbxImporter* importer = FbxImporter::Create(manager, "");

        // FBX SDK 는 char* (UTF-8) 경로를 받음 — wchar_t → UTF-8 변환.
        const int utf8Len = ::WideCharToMultiByte(CP_UTF8, 0, absolutePath, -1, nullptr, 0, nullptr, nullptr);
        std::string utf8Path(utf8Len > 0 ? utf8Len - 1 : 0, '\0');
        if (utf8Len > 0)
        {
            ::WideCharToMultiByte(CP_UTF8, 0, absolutePath, -1, utf8Path.data(), utf8Len, nullptr, nullptr);
        }

        if (!importer->Initialize(utf8Path.c_str(), -1, manager->GetIOSettings()))
        {
            char buf[512];
            std::snprintf(buf, sizeof(buf),
                          "FbxImporter::Initialize 실패: %s",
                          importer->GetStatus().GetErrorString());
            throw std::runtime_error(buf);
        }
        if (!importer->Import(scene))
        {
            throw std::runtime_error("FbxImporter::Import 실패");
        }

        // 축 시스템 변환 (DirectX = Y-up, Z-forward, LH).
        FbxAxisSystem::DirectX.ConvertScene(scene);

        // 다각형이 섞여 있을 수 있어 삼각형화 강제.
        FbxGeometryConverter geomConv(manager);
        geomConv.Triangulate(scene, /*replace*/true);

        std::vector<Mesh::Vertex> vertices;
        std::vector<uint32>       indices;
        ParseNode(scene->GetRootNode(), defaultColor, vertices, indices);

        if (vertices.empty() || indices.empty())
        {
            throw std::runtime_error("FBX: 유효한 메시 노드 없음");
        }

        wchar_t logLine[256];
        std::swprintf(logLine, std::size(logLine),
                      L"[render] FBX loaded: vertices=%zu, indices=%zu (path: %ls)\n",
                      vertices.size(), indices.size(), absolutePath);
        engine::core::LogInfo(logLine);

        // Mesh R32 인덱스 오버로드 사용 — Dragon 같은 65535 초과 메시 대응.
        return std::make_unique<Mesh>(
            device,
            vertices.data(),
            static_cast<uint32>(vertices.size()),
            indices.data(),
            static_cast<uint32>(indices.size()));
    }

    std::wstring DefaultFbxDir()
    {
        wchar_t buf[MAX_PATH];
        const DWORD len = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
        if (len == 0 || len == MAX_PATH)
        {
            throw std::runtime_error("GetModuleFileNameW failed");
        }
        std::wstring path(buf, len);
        const auto sep = path.find_last_of(L"\\/");
        if (sep == std::wstring::npos)
        {
            throw std::runtime_error("Executable path has no separator");
        }
        return path.substr(0, sep + 1) + L"Resources\\FBX\\";
    }
}
