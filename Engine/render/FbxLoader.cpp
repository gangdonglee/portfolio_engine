#include "render/FbxLoader.h"

#include "core/Logger.h"
#include "core/Types.h"
#include "render/CommandList.h"
#include "render/CommandQueue.h"
#include "render/Device.h"
#include "render/ImageLoader.h"
#include "render/Material.h"
#include "render/Mesh.h"
#include "render/SrvDescriptorHeap.h"
#include "render/Texture.h"

#include <Windows.h>

#include <fbxsdk.h>
// fbxsdk/core/arch/fbxarch.h 가 `#define snprintf _snprintf` 매크로를 추가 — std::snprintf 호출을 깬다.
#ifdef snprintf
#undef snprintf
#endif

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <iterator>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::render::fbx_loader
{
    namespace
    {
        using DirectX::XMFLOAT2;
        using DirectX::XMFLOAT3;

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

        // 머티리얼의 diffuse 텍스처 상대 경로 (없으면 empty).
        std::string ExtractDiffuseTexRelative(FbxSurfaceMaterial* mat)
        {
            if (mat == nullptr) { return {}; }
            FbxProperty prop = mat->FindProperty(FbxSurfaceMaterial::sDiffuse);
            if (!prop.IsValid()) { return {}; }
            const int srcCount = prop.GetSrcObjectCount();
            if (srcCount <= 0) { return {}; }
            FbxFileTexture* tex = prop.GetSrcObject<FbxFileTexture>(0);
            if (tex == nullptr) { return {}; }
            const char* name = tex->GetRelativeFileName();
            return name ? std::string(name) : std::string{};
        }

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

        // ASCII relative path → wstring widen.
        std::wstring AsciiToWide(const std::string& s)
        {
            return std::wstring(s.begin(), s.end());
        }

        // .fbm 폴더는 FBX import 시 임베디드 텍스처를 푸는 형제 폴더.
        // 우리 패턴: <fbxPath 의 부모>\<fbxFilename stem>.fbm\<텍스처 파일명>.
        std::wstring ComputeFbmDir(const std::wstring& fbxPath)
        {
            const size_t lastSep = fbxPath.find_last_of(L"\\/");
            const std::wstring parent = (lastSep == std::wstring::npos) ? std::wstring{} : fbxPath.substr(0, lastSep);
            std::wstring filename = (lastSep == std::wstring::npos) ? fbxPath : fbxPath.substr(lastSep + 1);
            const size_t dot = filename.find_last_of(L'.');
            const std::wstring stem = (dot == std::wstring::npos) ? filename : filename.substr(0, dot);
            return parent + L"\\" + stem + L".fbm";
        }

        // 머티리얼별 texture 캐시 키 = 상대 경로 ASCII.
        // 한 머티리얼이 여러 메시에 공유될 수 있고 텍스처도 그렇기 때문에 중복 디코드 회피.
        using TextureCache = std::unordered_map<std::string, std::shared_ptr<Texture>>;

        std::shared_ptr<Texture> LoadDiffuseTexture(
            const std::string& relPath,
            const std::wstring& fbmDir,
            Device& device, CommandQueue& queue, CommandList& list,
            SrvDescriptorHeap& srvHeap,
            TextureCache& cache)
        {
            if (relPath.empty()) { return nullptr; }
            auto it = cache.find(relPath);
            if (it != cache.end()) { return it->second; }

            // 상대 경로에서 파일명만 추출 → .fbm 폴더의 같은 이름 파일.
            const std::wstring relWide = AsciiToWide(relPath);
            const size_t lastSep = relWide.find_last_of(L"\\/");
            const std::wstring filename = (lastSep == std::wstring::npos) ? relWide : relWide.substr(lastSep + 1);
            const std::wstring fullPath = fbmDir + L"\\" + filename;

            try
            {
                ImageData img = image_loader::LoadImage(fullPath.c_str());
                auto tex = std::make_shared<Texture>(
                    device, queue, list,
                    img.pixels.data(), img.width, img.height);
                tex->CreateSrv(device, srvHeap);
                cache.emplace(relPath, tex);
                return tex;
            }
            catch (const std::exception& e)
            {
                wchar_t buf[256];
                std::swprintf(buf, std::size(buf),
                              L"[render] diffuse 텍스처 로드 실패 (%hs): %ls\n",
                              e.what(), fullPath.c_str());
                engine::core::LogInfo(buf);
                return nullptr;
            }
        }

        // 한 메시의 polygon 들을 *머티리얼 인덱스별* sub-indices 컬렉션에 분배.
        // controlPoint 단위 vertex dedup — 같은 controlPoint 가 여러 polygon vertex 에 매핑되면 마지막 attribute 값 채택.
        void AppendMesh(FbxMesh*                                  mesh,
                        const XMFLOAT3&                           defaultColor,
                        std::vector<Mesh::Vertex>&                outVertices,
                        std::vector<std::vector<uint32>>&         outSubIndices,
                        size_t                                    globalMatBase)
        {
            const int32 cpCount = mesh->GetControlPointsCount();
            if (cpCount <= 0) { return; }

            FbxNode* node = mesh->GetNode();
            const int32 nodeMatCount = (node ? node->GetMaterialCount() : 0);

            const FbxGeometryElementMaterial* matElem  = mesh->GetElementMaterial();
            const FbxGeometryElementNormal*   normElem = mesh->GetElementNormal();
            const FbxGeometryElementUV*       uvElem   = mesh->GetElementUV();

            // 글로벌 vertex 컬렉션에 controlPoints 베이스로 push.
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

            // polygon 별 인덱스 + attribute 채우기.
            const int32 polyCount = mesh->GetPolygonCount();
            int32       vertexCounter = 0;
            for (int32 p = 0; p < polyCount; ++p)
            {
                const int32 polySize = mesh->GetPolygonSize(p);
                if (polySize != 3)
                {
                    vertexCounter += polySize;
                    continue;
                }

                // 머티리얼 subset 인덱스 (글로벌 머티리얼 인덱스 = globalMatBase + 로컬).
                int32 localMatIdx = 0;
                if (matElem != nullptr && nodeMatCount > 0)
                {
                    const auto matMap = matElem->GetMappingMode();
                    if (matMap == FbxGeometryElement::eByPolygon)
                    {
                        localMatIdx = matElem->GetIndexArray().GetAt(p);
                    }
                    else if (matMap == FbxGeometryElement::eAllSame)
                    {
                        localMatIdx = matElem->GetIndexArray().GetAt(0);
                    }
                    if (localMatIdx < 0 || localMatIdx >= nodeMatCount) { localMatIdx = 0; }
                }
                const size_t globalMatIdx = globalMatBase + static_cast<size_t>(localMatIdx);
                if (globalMatIdx >= outSubIndices.size())
                {
                    outSubIndices.resize(globalMatIdx + 1);
                }

                uint32 cpIdxs[3];
                for (int32 j = 0; j < 3; ++j)
                {
                    const int32 cp = mesh->GetPolygonVertex(p, j);
                    cpIdxs[j] = static_cast<uint32>(cp);
                    Mesh::Vertex& v = outVertices[vertexBase + cp];

                    if (normElem != nullptr)
                    {
                        const int32 ni = ResolveElementIndex(normElem, cp, vertexCounter);
                        v.normal = ConvertVec3(normElem->GetDirectArray().GetAt(ni));
                    }
                    if (uvElem != nullptr)
                    {
                        const int32 polyUvIdx = mesh->GetTextureUVIndex(p, j);
                        const int32 ui = ResolveUvIndex(uvElem, cp, polyUvIdx);
                        v.uv = ConvertUv(uvElem->GetDirectArray().GetAt(ui));
                    }
                    ++vertexCounter;
                }

                // RH↔LH 변환: face winding 반전 (0, 2, 1).
                std::vector<uint32>& sub = outSubIndices[globalMatIdx];
                sub.push_back(vertexBase + cpIdxs[0]);
                sub.push_back(vertexBase + cpIdxs[2]);
                sub.push_back(vertexBase + cpIdxs[1]);
            }
        }

        // FBX 트리 재귀 — eMesh 노드의 머티리얼을 누적 + sub-indices 분리.
        void ParseNode(FbxNode*                                  node,
                       const XMFLOAT3&                           defaultColor,
                       std::vector<Mesh::Vertex>&                outVertices,
                       std::vector<std::vector<uint32>>&         outSubIndices,
                       std::vector<std::shared_ptr<Material>>&   outMaterials,
                       const std::wstring&                       fbmDir,
                       Device& device, CommandQueue& queue, CommandList& list,
                       SrvDescriptorHeap& srvHeap,
                       TextureCache& texCache)
        {
            if (node == nullptr) { return; }
            FbxNodeAttribute* attr = node->GetNodeAttribute();
            if (attr != nullptr && attr->GetAttributeType() == FbxNodeAttribute::eMesh)
            {
                // 본 노드의 머티리얼을 outMaterials 에 append. globalMatBase = 현재 outMaterials.size().
                const size_t globalMatBase = outMaterials.size();
                const int32 matCount = node->GetMaterialCount();
                if (matCount == 0)
                {
                    // 머티리얼 없는 메시 — 폴백 머티리얼 1개 push.
                    auto fallback = std::make_shared<Material>();
                    fallback->name = L"<no-material>";
                    fallback->diffuseColor = { 1.0f, 1.0f, 1.0f };
                    outMaterials.push_back(fallback);
                }
                for (int32 i = 0; i < matCount; ++i)
                {
                    FbxSurfaceMaterial* surf = node->GetMaterial(i);
                    auto mat = std::make_shared<Material>();
                    if (surf != nullptr)
                    {
                        const std::string nameUtf8 = surf->GetName();
                        mat->name = AsciiToWide(nameUtf8);
                        mat->diffuseColor = ExtractKd(surf);
                        const std::string diffRel = ExtractDiffuseTexRelative(surf);
                        mat->albedoTexture = LoadDiffuseTexture(
                            diffRel, fbmDir,
                            device, queue, list, srvHeap, texCache);
                        if (mat->albedoTexture)
                        {
                            mat->albedoSrvGpu = mat->albedoTexture->SrvGpuHandle();
                        }
                    }
                    outMaterials.push_back(mat);
                }

                AppendMesh(node->GetMesh(), defaultColor, outVertices, outSubIndices, globalMatBase);
            }
            const int32 childCount = node->GetChildCount();
            for (int32 i = 0; i < childCount; ++i)
            {
                ParseNode(node->GetChild(i), defaultColor,
                          outVertices, outSubIndices, outMaterials,
                          fbmDir, device, queue, list, srvHeap, texCache);
            }
        }
    } // anonymous

    std::unique_ptr<Mesh> LoadFbx(
        Device&                  device,
        CommandQueue&            queue,
        CommandList&             list,
        SrvDescriptorHeap&       srvHeap,
        const wchar_t*           absolutePath,
        const DirectX::XMFLOAT3& defaultColor)
    {
        FbxManager* manager = FbxManager::Create();
        if (manager == nullptr)
        {
            throw std::runtime_error("FbxManager::Create 실패");
        }
        struct ManagerGuard { FbxManager* m; ~ManagerGuard() { if (m) m->Destroy(); } } guard{ manager };

        FbxIOSettings* ios = FbxIOSettings::Create(manager, IOSROOT);
        manager->SetIOSettings(ios);

        FbxScene*    scene    = FbxScene::Create(manager, "");
        FbxImporter* importer = FbxImporter::Create(manager, "");

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

        FbxAxisSystem::DirectX.ConvertScene(scene);
        FbxGeometryConverter geomConv(manager);
        geomConv.Triangulate(scene, /*replace*/true);

        const std::wstring fbmDir = ComputeFbmDir(absolutePath);

        std::vector<Mesh::Vertex>              vertices;
        std::vector<std::vector<uint32>>       subIndices;
        std::vector<std::shared_ptr<Material>> materials;
        TextureCache                           texCache;
        ParseNode(scene->GetRootNode(), defaultColor,
                  vertices, subIndices, materials,
                  fbmDir, device, queue, list, srvHeap, texCache);

        if (vertices.empty() || materials.empty())
        {
            throw std::runtime_error("FBX: 유효한 메시/머티리얼 없음");
        }
        // subIndices 가 materials 보다 짧게 끝났으면 빈 sub 로 패딩.
        if (subIndices.size() < materials.size())
        {
            subIndices.resize(materials.size());
        }

        // 통계 로그.
        uint32 totalIdx = 0;
        for (const auto& s : subIndices) { totalIdx += static_cast<uint32>(s.size()); }
        wchar_t logLine[256];
        std::swprintf(logLine, std::size(logLine),
                      L"[render] FBX loaded: vertices=%zu, materials=%zu, indices=%u (path: %ls)\n",
                      vertices.size(), materials.size(), totalIdx, absolutePath);
        engine::core::LogInfo(logLine);

        return std::make_unique<Mesh>(
            device,
            vertices.data(),
            static_cast<uint32>(vertices.size()),
            subIndices,
            materials);
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
