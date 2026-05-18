#include "render/FbxLoader.h"

#include "core/Logger.h"
#include "core/Types.h"
#include "render/AnimClip.h"
#include "render/CommandList.h"
#include "render/CommandQueue.h"
#include "render/Device.h"
#include "render/ImageLoader.h"
#include "render/Material.h"
#include "render/Mesh.h"
#include "render/Skeleton.h"
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
#include <utility>
#include <vector>

namespace engine::render::fbx_loader
{
    namespace
    {
        using DirectX::XMFLOAT2;
        using DirectX::XMFLOAT3;
        using DirectX::XMFLOAT4;
        using DirectX::XMUINT4;
        using DirectX::XMFLOAT4X4;

        // FbxAMatrix (column-major) → XMFLOAT4X4 (row-major) — transpose.
        XMFLOAT4X4 ConvertMatrix(const FbxAMatrix& m)
        {
            XMFLOAT4X4 r;
            for (int row = 0; row < 4; ++row)
            {
                for (int col = 0; col < 4; ++col)
                {
                    r.m[row][col] = static_cast<float>(m.Get(col, row)); // transpose
                }
            }
            return r;
        }

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

        std::wstring AsciiToWide(const std::string& s)
        {
            return std::wstring(s.begin(), s.end());
        }

        std::wstring ComputeFbmDir(const std::wstring& fbxPath)
        {
            const size_t lastSep = fbxPath.find_last_of(L"\\/");
            const std::wstring parent = (lastSep == std::wstring::npos) ? std::wstring{} : fbxPath.substr(0, lastSep);
            std::wstring filename = (lastSep == std::wstring::npos) ? fbxPath : fbxPath.substr(lastSep + 1);
            const size_t dot = filename.find_last_of(L'.');
            const std::wstring stem = (dot == std::wstring::npos) ? filename : filename.substr(0, dot);
            return parent + L"\\" + stem + L".fbm";
        }

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

        // ───────────────────────────────────────────────────────────────────────
        // 스키닝 보조 — 학습 자료 BoneWeight 패턴 차용.
        // controlPoint 별로 최대 4개 (bone idx, weight) 보관, 큰 순으로 정렬, 합 1 정규화.
        // ───────────────────────────────────────────────────────────────────────
        struct BoneWeightAccum
        {
            std::vector<std::pair<int32, double>> entries;

            void Add(int32 boneIdx, double weight)
            {
                if (weight <= 0.0) { return; }
                // 큰 weight 순 정렬 위치에 삽입.
                auto it = std::find_if(entries.begin(), entries.end(),
                                       [=](const auto& p) { return p.second < weight; });
                entries.emplace(it, boneIdx, weight);
                if (entries.size() > 4) { entries.pop_back(); }
            }
            void Normalize()
            {
                double sum = 0.0;
                for (const auto& p : entries) { sum += p.second; }
                if (sum <= 0.0) { return; }
                for (auto& p : entries) { p.second /= sum; }
            }
        };

        // ───────────────────────────────────────────────────────────────────────
        // LoadBones — eSkeleton attribute 노드를 트리 순서로 push.
        // bones 의 인덱스 = 추가 순서. parentIdx 는 부모 본의 인덱스 (-1 = 루트).
        // ───────────────────────────────────────────────────────────────────────
        void LoadBonesRec(FbxNode* node, int32 currentIdx, int32 parentIdx, Skeleton& outSkeleton)
        {
            if (node == nullptr) { return; }
            FbxNodeAttribute* attr = node->GetNodeAttribute();
            int32 myIdx = currentIdx;
            if (attr != nullptr && attr->GetAttributeType() == FbxNodeAttribute::eSkeleton)
            {
                Skeleton::Bone bone;
                bone.name = AsciiToWide(node->GetName());
                bone.parentIndex = parentIdx;
                // offsetMatrix 는 LoadOffsetMatrix 에서 cluster 처리 시 채움. 임시 identity.
                DirectX::XMStoreFloat4x4(&bone.offsetMatrix, DirectX::XMMatrixIdentity());
                outSkeleton.Bones().push_back(std::move(bone));
                myIdx = static_cast<int32>(outSkeleton.Bones().size()) - 1;
            }
            const int32 childCount = node->GetChildCount();
            for (int32 i = 0; i < childCount; ++i)
            {
                LoadBonesRec(node->GetChild(i),
                             static_cast<int32>(outSkeleton.Bones().size()),  // next idx (재귀 시 부모 인덱스 인자로 안 쓰임)
                             myIdx,
                             outSkeleton);
            }
        }

        // ───────────────────────────────────────────────────────────────────────
        // LoadAnimationInfo — 모든 AnimStack → AnimClip 생성 (이름/duration). 키프레임은 LoadKeyframe 에서 채움.
        // ───────────────────────────────────────────────────────────────────────
        struct ClipMeta
        {
            std::unique_ptr<AnimClip>   clip;
            FbxAnimStack*               stack = nullptr;
            FbxTime                     startTime;
            FbxTime                     endTime;
            FbxTime::EMode              timeMode = FbxTime::eDefaultMode;
        };

        std::vector<ClipMeta> LoadAnimationInfo(FbxScene* scene, size_t boneCount)
        {
            std::vector<ClipMeta> result;
            FbxArray<FbxString*> names;
            scene->FillAnimStackNameArray(names);
            const int32 animCount = names.GetCount();

            for (int32 i = 0; i < animCount; ++i)
            {
                FbxAnimStack* stack = scene->FindMember<FbxAnimStack>(names[i]->Buffer());
                if (stack == nullptr) { continue; }

                ClipMeta cm;
                cm.clip = std::make_unique<AnimClip>();
                cm.clip->name = AsciiToWide(stack->GetName());

                FbxTakeInfo* takeInfo = scene->GetTakeInfo(stack->GetName());
                if (takeInfo == nullptr) { continue; }

                cm.startTime = takeInfo->mLocalTimeSpan.GetStart();
                cm.endTime   = takeInfo->mLocalTimeSpan.GetStop();
                cm.timeMode  = scene->GetGlobalSettings().GetTimeMode();
                cm.clip->startSec = cm.startTime.GetSecondDouble();
                cm.clip->endSec   = cm.endTime.GetSecondDouble();
                cm.clip->bonesKeyFrames.resize(boneCount);
                cm.stack = stack;

                result.push_back(std::move(cm));
            }

            // names 는 FbxScene 이 owning. 해제 안 함 (FBX SDK 라이프타임).
            return result;
        }

        // mesh node 의 geometric transform — pivot offset 같은 것.
        FbxAMatrix GetGeomTransform(FbxNode* node)
        {
            const FbxVector4 t = node->GetGeometricTranslation(FbxNode::eSourcePivot);
            const FbxVector4 r = node->GetGeometricRotation(FbxNode::eSourcePivot);
            const FbxVector4 s = node->GetGeometricScaling(FbxNode::eSourcePivot);
            return FbxAMatrix(t, r, s);
        }

        // ───────────────────────────────────────────────────────────────────────
        // LoadAnimationData — mesh 의 eSkin deformer 처리.
        //   cluster 마다: LoadBoneWeight + LoadOffsetMatrix + LoadKeyframe (모든 clip)
        // accumWeights 는 mesh 의 controlPoint 별 BoneWeightAccum (사이즈 = cpCount).
        // ───────────────────────────────────────────────────────────────────────
        void LoadAnimationData(FbxMesh* mesh,
                               Skeleton& skeleton,
                               std::vector<ClipMeta>& clipMetas,
                               std::vector<BoneWeightAccum>& accumWeights)
        {
            const int32 skinCount = mesh->GetDeformerCount(FbxDeformer::eSkin);
            if (skinCount <= 0) { return; }

            const FbxAMatrix matNodeTransform = GetGeomTransform(mesh->GetNode());

            for (int32 s = 0; s < skinCount; ++s)
            {
                FbxSkin* skin = static_cast<FbxSkin*>(mesh->GetDeformer(s, FbxDeformer::eSkin));
                if (skin == nullptr) { continue; }
                const FbxSkin::EType type = skin->GetSkinningType();
                if (type != FbxSkin::eRigid && type != FbxSkin::eLinear) { continue; }

                const int32 clusterCount = skin->GetClusterCount();
                for (int32 c = 0; c < clusterCount; ++c)
                {
                    FbxCluster* cluster = skin->GetCluster(c);
                    if (cluster == nullptr || cluster->GetLink() == nullptr) { continue; }

                    const int32 boneIdx = skeleton.FindIndex(AsciiToWide(cluster->GetLink()->GetName()));
                    if (boneIdx < 0) { continue; }

                    // 1) Bone weight — controlPoint 단위 (idx, weight) 누적.
                    const int32* cpIdxs   = cluster->GetControlPointIndices();
                    const double* weights = cluster->GetControlPointWeights();
                    const int32 idxCount  = cluster->GetControlPointIndicesCount();
                    for (int32 i = 0; i < idxCount; ++i)
                    {
                        const int32 vtxIdx = cpIdxs[i];
                        if (vtxIdx < 0 || vtxIdx >= static_cast<int32>(accumWeights.size())) { continue; }
                        accumWeights[vtxIdx].Add(boneIdx, weights[i]);
                    }

                    // 2) Offset matrix — inverse bind pose. RH↔LH reflect 학습 자료 패턴.
                    FbxAMatrix matClusterTrans, matClusterLinkTrans;
                    cluster->GetTransformMatrix(matClusterTrans);
                    cluster->GetTransformLinkMatrix(matClusterLinkTrans);

                    FbxAMatrix matReflect;
                    matReflect.SetRow(0, FbxVector4(1, 0, 0, 0));
                    matReflect.SetRow(1, FbxVector4(0, 0, 1, 0));
                    matReflect.SetRow(2, FbxVector4(0, 1, 0, 0));
                    matReflect.SetRow(3, FbxVector4(0, 0, 0, 1));

                    FbxAMatrix matOffset = matClusterLinkTrans.Inverse() * matClusterTrans;
                    matOffset = matReflect * matOffset * matReflect;

                    skeleton.Bones()[boneIdx].offsetMatrix = ConvertMatrix(matOffset);

                    // 3) Keyframe — 모든 clip 의 startFrame..endFrame.
                    for (ClipMeta& cm : clipMetas)
                    {
                        if (cm.stack == nullptr) { continue; }
                        mesh->GetScene()->SetCurrentAnimationStack(cm.stack);

                        const FbxLongLong startFrame = cm.startTime.GetFrameCount(cm.timeMode);
                        const FbxLongLong endFrame   = cm.endTime.GetFrameCount(cm.timeMode);
                        for (FbxLongLong f = startFrame; f < endFrame; ++f)
                        {
                            FbxTime fbxTime;
                            fbxTime.SetFrame(f, cm.timeMode);

                            FbxAMatrix matFromNode = mesh->GetNode()->EvaluateGlobalTransform(fbxTime);
                            FbxAMatrix matTransform = matFromNode.Inverse() * cluster->GetLink()->EvaluateGlobalTransform(fbxTime);
                            matTransform = matReflect * matTransform * matReflect;

                            KeyFrame kf;
                            kf.timeSec   = fbxTime.GetSecondDouble();
                            kf.transform = ConvertMatrix(matTransform);
                            cm.clip->bonesKeyFrames[boneIdx].push_back(kf);
                        }
                    }
                }
            }
        }

        // ───────────────────────────────────────────────────────────────────────
        // AppendMesh — controlPoint 단위 정점 + polygon 단위 머티리얼 분기 + 스키닝 정점 채움.
        // ───────────────────────────────────────────────────────────────────────
        void AppendMesh(FbxMesh*                                  mesh,
                        const XMFLOAT3&                           defaultColor,
                        std::vector<Mesh::Vertex>&                outVertices,
                        std::vector<std::vector<uint32>>&         outSubIndices,
                        size_t                                    globalMatBase,
                        Skeleton*                                 skeleton,
                        std::vector<ClipMeta>*                    clipMetas)
        {
            const int32 cpCount = mesh->GetControlPointsCount();
            if (cpCount <= 0) { return; }

            FbxNode* node = mesh->GetNode();
            const int32 nodeMatCount = (node ? node->GetMaterialCount() : 0);

            const FbxGeometryElementMaterial* matElem  = mesh->GetElementMaterial();
            const FbxGeometryElementNormal*   normElem = mesh->GetElementNormal();
            const FbxGeometryElementUV*       uvElem   = mesh->GetElementUV();

            // 정점 push (controlPoint 베이스).
            const uint32 vertexBase = static_cast<uint32>(outVertices.size());
            const FbxVector4* controlPoints = mesh->GetControlPoints();
            outVertices.resize(vertexBase + cpCount);
            for (int32 i = 0; i < cpCount; ++i)
            {
                Mesh::Vertex& v = outVertices[vertexBase + i];
                v.position    = ConvertVec3(controlPoints[i]);
                v.normal      = { 0.0f, 1.0f, 0.0f };
                v.uv          = { 0.0f, 0.0f };
                v.color       = defaultColor;
                v.boneIndices = { 0, 0, 0, 0 };
                v.boneWeights = { 0.0f, 0.0f, 0.0f, 0.0f };
            }

            // 스키닝 데이터 수집 — accumWeights 사이즈 = cpCount.
            std::vector<BoneWeightAccum> accumWeights;
            if (skeleton != nullptr && clipMetas != nullptr)
            {
                accumWeights.resize(cpCount);
                LoadAnimationData(mesh, *skeleton, *clipMetas, accumWeights);

                // accumWeights → vertex.boneIndices/Weights 채움.
                for (int32 i = 0; i < cpCount; ++i)
                {
                    accumWeights[i].Normalize();
                    Mesh::Vertex& v = outVertices[vertexBase + i];
                    XMUINT4   bi{ 0, 0, 0, 0 };
                    XMFLOAT4  bw{ 0.0f, 0.0f, 0.0f, 0.0f };
                    for (size_t w = 0; w < accumWeights[i].entries.size() && w < 4; ++w)
                    {
                        const auto& p = accumWeights[i].entries[w];
                        switch (w)
                        {
                            case 0: bi.x = static_cast<uint32>(p.first); bw.x = static_cast<float>(p.second); break;
                            case 1: bi.y = static_cast<uint32>(p.first); bw.y = static_cast<float>(p.second); break;
                            case 2: bi.z = static_cast<uint32>(p.first); bw.z = static_cast<float>(p.second); break;
                            case 3: bi.w = static_cast<uint32>(p.first); bw.w = static_cast<float>(p.second); break;
                        }
                    }
                    v.boneIndices = bi;
                    v.boneWeights = bw;
                }
            }

            // polygon 별 인덱스 + attribute.
            const int32 polyCount = mesh->GetPolygonCount();
            int32       vertexCounter = 0;
            for (int32 p = 0; p < polyCount; ++p)
            {
                const int32 polySize = mesh->GetPolygonSize(p);
                if (polySize != 3) { vertexCounter += polySize; continue; }

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

        // FBX 트리 재귀 — 메시 처리 + 머티리얼 추출.
        void ParseNode(FbxNode*                                  node,
                       const XMFLOAT3&                           defaultColor,
                       std::vector<Mesh::Vertex>&                outVertices,
                       std::vector<std::vector<uint32>>&         outSubIndices,
                       std::vector<std::shared_ptr<Material>>&   outMaterials,
                       const std::wstring&                       fbmDir,
                       Device& device, CommandQueue& queue, CommandList& list,
                       SrvDescriptorHeap& srvHeap,
                       TextureCache& texCache,
                       Skeleton*                                 skeleton,
                       std::vector<ClipMeta>*                    clipMetas)
        {
            if (node == nullptr) { return; }
            FbxNodeAttribute* attr = node->GetNodeAttribute();
            if (attr != nullptr && attr->GetAttributeType() == FbxNodeAttribute::eMesh)
            {
                const size_t globalMatBase = outMaterials.size();
                const int32 matCount = node->GetMaterialCount();
                if (matCount == 0)
                {
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
                        mat->name = AsciiToWide(surf->GetName());
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

                AppendMesh(node->GetMesh(), defaultColor,
                           outVertices, outSubIndices, globalMatBase,
                           skeleton, clipMetas);
            }
            const int32 childCount = node->GetChildCount();
            for (int32 i = 0; i < childCount; ++i)
            {
                ParseNode(node->GetChild(i), defaultColor,
                          outVertices, outSubIndices, outMaterials,
                          fbmDir, device, queue, list, srvHeap, texCache,
                          skeleton, clipMetas);
            }
        }
    } // anonymous

    LoadedFbxModel LoadFbx(
        Device&                  device,
        CommandQueue&            queue,
        CommandList&             list,
        SrvDescriptorHeap&       srvHeap,
        const wchar_t*           absolutePath,
        const DirectX::XMFLOAT3& defaultColor)
    {
        FbxManager* manager = FbxManager::Create();
        if (manager == nullptr) { throw std::runtime_error("FbxManager::Create 실패"); }
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
            std::snprintf(buf, sizeof(buf), "FbxImporter::Initialize 실패: %s",
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

        // 1) 본 트리 추출.
        LoadedFbxModel result;
        result.skeleton = std::make_unique<Skeleton>();
        LoadBonesRec(scene->GetRootNode(),
                     /*currentIdx*/0,
                     /*parentIdx*/-1,
                     *result.skeleton);

        // 2) 애니메이션 메타 (clips skeleton 의 본 수에 맞춰 keyFrames 컨테이너 사전 할당).
        std::vector<ClipMeta> clipMetas;
        if (result.skeleton->BoneCount() > 0)
        {
            clipMetas = LoadAnimationInfo(scene, result.skeleton->BoneCount());
        }

        // 3) ParseNode — 메시 + 머티리얼 + 텍스처 + 스키닝 정점 + 키프레임.
        std::vector<Mesh::Vertex>              vertices;
        std::vector<std::vector<uint32>>       subIndices;
        std::vector<std::shared_ptr<Material>> materials;
        TextureCache                           texCache;
        ParseNode(scene->GetRootNode(), defaultColor,
                  vertices, subIndices, materials,
                  fbmDir, device, queue, list, srvHeap, texCache,
                  result.skeleton->BoneCount() > 0 ? result.skeleton.get() : nullptr,
                  result.skeleton->BoneCount() > 0 ? &clipMetas : nullptr);

        if (vertices.empty() || materials.empty())
        {
            throw std::runtime_error("FBX: 유효한 메시/머티리얼 없음");
        }
        if (subIndices.size() < materials.size())
        {
            subIndices.resize(materials.size());
        }

        // 통계.
        uint32 totalIdx = 0;
        for (const auto& s : subIndices) { totalIdx += static_cast<uint32>(s.size()); }
        wchar_t logLine[300];
        std::swprintf(logLine, std::size(logLine),
                      L"[render] FBX loaded: vertices=%zu, materials=%zu, indices=%u, bones=%zu, clips=%zu (path: %ls)\n",
                      vertices.size(), materials.size(), totalIdx,
                      result.skeleton->BoneCount(), clipMetas.size(), absolutePath);
        engine::core::LogInfo(logLine);

        // 결과 채우기.
        result.mesh = std::make_unique<Mesh>(
            device,
            vertices.data(),
            static_cast<uint32>(vertices.size()),
            subIndices,
            materials);

        for (ClipMeta& cm : clipMetas)
        {
            if (cm.clip) { result.clips.push_back(std::move(cm.clip)); }
        }
        if (result.skeleton->BoneCount() == 0) { result.skeleton.reset(); }

        return result;
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
