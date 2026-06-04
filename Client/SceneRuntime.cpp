#include "SceneRuntime.h"

#include "anim/AnimatorController.h"
#include "anim/AnimatorRuntime.h"
#include "anim/AnimatorSerializer.h"
#include "anim/FootIKSolver.h"
#include "core/Logger.h"
#include "render/AnimClip.h"
#include "render/Animator.h"
#include "render/Camera.h"
#include "render/CommandList.h"
#include "render/CommandQueue.h"
#include "render/ConstantBuffer.h"
#include "render/Device.h"
#include "render/FbxLoader.h"
#include "render/Mesh.h"
#include "render/ObjLoader.h"
#include "render/ProceduralTerrain.h"
#include "render/Skeleton.h"
#include "render/SrvDescriptorHeap.h"
#include "render/ShadowMap.h"
#include "render/StructuredBuffer.h"
#include "render/Texture.h"

#include <d3d12.h>

#include <filesystem>
#include <stdexcept>

namespace client
{
    namespace
    {
        // HLSL StructuredBuffer 의 element 와 stride 1:1.
        struct DirectionalLightGpu
        {
            DirectX::XMFLOAT3 directionWS; float _pad0;
            DirectX::XMFLOAT3 color;       float intensity;
        };
        static_assert(sizeof(DirectionalLightGpu) == 32, "DirectionalLightGpu stride 깨짐");

        struct PointLightGpu
        {
            DirectX::XMFLOAT3 positionWS; float _pad0;
            DirectX::XMFLOAT3 color;      float intensity;
            float             range;
            float             _pad1[3];
        };
        static_assert(sizeof(PointLightGpu) == 48, "PointLightGpu stride 깨짐");

        struct FrameConstants
        {
            DirectX::XMFLOAT4X4 mvp;
            DirectX::XMFLOAT4X4 world;
            DirectX::XMFLOAT4X4 lightViewProj;                   // 그림자 — 방향광 view-proj
            DirectX::XMFLOAT3   cameraPosWS;  float roughness;   // PBR
            DirectX::XMFLOAT3   ambient;      float metallic;    // PBR
            std::uint32_t       dirLightCount;
            std::uint32_t       pointLightCount;
            std::uint32_t       shadowEnabled;                   // 1=그림자 샘플
            std::uint32_t       normalFlipY;                     // 1=normal map Y 반전
            float               normalStrength;                  // normal map 섭동 세기
            std::uint32_t       applyTonemap;                    // 1=셰이더 톤맵(에디터), 0=선형HDR(게임)
            std::uint32_t       _pad[2];
        };
        static_assert(sizeof(FrameConstants) % 16 == 0, "FrameConstants 16바이트 정렬 깨짐");

        // 그림자 깊이 패스 cbuffer — worldLightMVP 한 행렬 (b0).
        struct ShadowConstants
        {
            DirectX::XMFLOAT4X4 worldLightMVP;
        };

        // 본 팔레트 cbuffer — HLSL bones[256].
        constexpr std::uint32_t kMaxBones = 256;
        struct BonePalette
        {
            DirectX::XMFLOAT4X4 bones[kMaxBones];
        };
        static_assert(sizeof(BonePalette) == kMaxBones * 64, "BonePalette 크기 깨짐");

        DirectX::XMMATRIX ComposeWorld(const engine::scene::Transform& xform)
        {
            using namespace DirectX;
            const XMVECTOR s = XMVectorSet(xform.scale.x,    xform.scale.y,    xform.scale.z,    0.0f);
            const XMVECTOR r = XMVectorSet(xform.rotation.x, xform.rotation.y, xform.rotation.z, xform.rotation.w);
            const XMVECTOR t = XMVectorSet(xform.position.x, xform.position.y, xform.position.z, 1.0f);
            return XMMatrixAffineTransformation(s, XMVectorZero(), r, t);
        }

        // identity 팔레트는 cpp-local static — Animator 없을 때 cbuffer 에 채워짐.
        const BonePalette& IdentityPalette()
        {
            static const BonePalette kIdentity = []
            {
                BonePalette p{};
                for (std::uint32_t i = 0; i < kMaxBones; ++i)
                {
                    DirectX::XMStoreFloat4x4(&p.bones[i], DirectX::XMMatrixIdentity());
                }
                return p;
            }();
            return kIdentity;
        }
    }

    SceneRuntime::SceneRuntime(engine::render::Device&            device,
                               engine::render::CommandQueue&      queue,
                               engine::render::CommandList&       uploadList,
                               engine::render::SrvDescriptorHeap& srvHeap,
                               engine::scene::Scene               scene)
        : m_scene(std::move(scene))
        , m_footIKConfig(std::make_unique<engine::anim::FootIKConfig>())
        , m_footIKBones(std::make_unique<engine::anim::FootIKBoneIndices>())
        , m_footIKDebug(std::make_unique<engine::anim::FootIKDebug>())
    {
        // capacity 검증 — 초과 시 부팅 단계에서 throw.
        if (m_scene.dirLights.size() > kDirLightCapacity)
        {
            throw std::runtime_error("SceneRuntime: dirLights 가 capacity " +
                                     std::to_string(kDirLightCapacity) + " 초과");
        }
        if (m_scene.pointLights.size() > kPointLightCapacity)
        {
            throw std::runtime_error("SceneRuntime: pointLights 가 capacity " +
                                     std::to_string(kPointLightCapacity) + " 초과");
        }

        // 지형 높이맵 로드 — scene.terrainHeightmapPath 가 있으면. 실패해도 절차적 폴백(throw 안 함).
        if (!m_scene.terrainHeightmapPath.empty())
        {
            const std::string hmPath =
                std::filesystem::absolute(std::filesystem::path{ m_scene.terrainHeightmapPath }).string();
            engine::render::HeightMap hm;
            if (engine::render::HeightMap::Load(hmPath, hm)) { m_heightMap = std::move(hm); }
            else { engine::core::LogInfoA("[SceneRuntime] heightmap 로드 실패 (절차적 폴백)"); }
        }

        // 자산 캐시 — 확장자 분기.
        for (const auto& inst : m_scene.meshes)
        {
            if (m_assetCache.contains(inst.meshAssetPath)) { continue; }

            LoadedAsset asset;
            const std::filesystem::path p{ inst.meshAssetPath };
            const auto ext = p.extension().string();
            const std::wstring full = std::filesystem::absolute(p).wstring();

            if (inst.meshAssetPath == "__Terrain__")
            {
                // 지형 메시 — 높이맵 있으면 그 데이터로, 없으면 절차적 sin/cos. 둘 다 동일 해상도
                //   규약(kTerrainSeg)이라 EnsureTerrainHeightMap 베이크 후 UpdateVertices 정점수 일치.
                float w = 5000.0f, d = 5000.0f;
                int segX = 192, segZ = 192;
                std::function<float(float, float)> hf =
                    engine::render::procedural_terrain::DefaultHeightFunc;
                if (!m_heightMap.Empty())
                {
                    w = m_heightMap.WorldWidth(); d = m_heightMap.WorldDepth();
                    segX = m_heightMap.Cols() - 1; segZ = m_heightMap.Rows() - 1;
                    hf = [this](float x, float z) { return m_heightMap.Sample(x, z); };
                }
                asset.mesh = engine::render::procedural_terrain::Generate(device, w, d, segX, segZ, hf);
            }
            else if (ext == ".fbx" || ext == ".FBX")
            {
                engine::render::fbx_loader::LoadedFbxModel loaded =
                    engine::render::fbx_loader::LoadFbx(
                        device, queue, uploadList, srvHeap,
                        full.c_str(),
                        { 0.85f, 0.85f, 0.92f });
                asset.mesh     = std::move(loaded.mesh);
                asset.skeleton = std::move(loaded.skeleton);
                asset.clips    = std::move(loaded.clips);
            }
            else if (ext == ".obj" || ext == ".OBJ")
            {
                asset.mesh = engine::render::obj_loader::LoadObj(
                    device, full.c_str(), { 1.0f, 1.0f, 1.0f });
            }
            else
            {
                throw std::runtime_error("SceneRuntime: unsupported mesh extension: " + inst.meshAssetPath);
            }

            m_assetCache.emplace(inst.meshAssetPath, std::move(asset));
            if (inst.meshAssetPath == "__Terrain__")
            {
                m_terrainMesh = m_assetCache.at(inst.meshAssetPath).mesh.get();
            }
        }

        // 통합 ground 샘플러 — 높이맵/절차적 자동 분기. 발 IK 가 이걸 쓰게 기본 연결
        //   (Application 이 컨트롤러 샘플러도 SampleGround 로 연결). 외부 SetGroundSampler 로 덮어쓰기 가능.
        m_groundSampler = [this](float x, float z) { return SampleGround(x, z); };

        // M1: animatorControllerPath 가 있는 *첫 번째* 인스턴스의 controller 활성화.
        //   - 베이스 스켈레톤 = 그 인스턴스의 meshAssetPath 의 skeleton.
        //   - controller 의 모든 state.motionClipPath 를 m_controllerClipCache 에 사전 로드.
        //   - AnimatorRuntime 인스턴스 생성.
        for (const auto& inst : m_scene.meshes)
        {
            if (inst.animatorControllerPath.empty()) { continue; }
            const auto& asset = m_assetCache.at(inst.meshAssetPath);
            if (!asset.skeleton)
            {
                engine::core::LogInfoA("[anim] controller skip — base mesh has no skeleton: ");
                engine::core::LogInfoA(inst.meshAssetPath.c_str());
                engine::core::LogInfoA("\n");
                continue;
            }

            try
            {
                const std::string controllerPath =
                    std::filesystem::absolute(inst.animatorControllerPath).string();
                auto controller = std::make_unique<engine::anim::AnimatorController>(
                    engine::anim::LoadJson(controllerPath));

                // 모든 state.motionClipPath 사전 로드 + clipMap 구축.
                engine::anim::AnimatorRuntime::ClipMap clipMap;
                // 여러 clip 중 *Mixamo 표준 이름 'mixamo.com'* 우선 — With-Skin 자산이
                // *Take 001* (T-pose padding, 100 frame) 같은 짜잘한 clip 도 포함하므로.
                //   1순위: 이름이 "mixamo" 포함된 clip
                //   2순위: 키프레임 가장 많은 clip
                auto pickMotion = [](const std::vector<std::unique_ptr<engine::render::AnimClip>>& clips)
                    -> const engine::render::AnimClip*
                {
                    const engine::render::AnimClip* mixamoClip = nullptr;
                    const engine::render::AnimClip* longest    = nullptr;
                    size_t longestKfs = 0;
                    for (const auto& c : clips)
                    {
                        if (!c || c->bonesKeyFrames.empty()) { continue; }
                        if (c->name.find(L"mixamo") != std::wstring::npos)
                        {
                            mixamoClip = c.get();
                        }
                        const size_t kfs = c->bonesKeyFrames[0].size();
                        if (kfs > longestKfs) { longestKfs = kfs; longest = c.get(); }
                    }
                    return mixamoClip ? mixamoClip : longest;
                };
                auto loadClipIntoMap = [&](const std::string& path)
                {
                    if (path.empty()) { return; }
                    if (m_controllerClipCache.contains(path))
                    {
                        const auto* picked = pickMotion(m_controllerClipCache.at(path));
                        if (picked) { clipMap.emplace(path, picked); }
                        return;
                    }
                    const std::wstring clipWpath = std::filesystem::absolute(path).wstring();
                    engine::render::fbx_loader::LoadedFbxAnimation loaded =
                        engine::render::fbx_loader::LoadFbxAnimationOnly(
                            clipWpath.c_str(), *asset.skeleton);
                    // 진단 — 각 clip 의 이름 + 키프레임 수 + pickMotion 선택 결과.
                    for (const auto& c : loaded.clips)
                    {
                        if (!c) { continue; }
                        const size_t kfs = c->bonesKeyFrames.empty() ? 0 : c->bonesKeyFrames[0].size();
                        char buf[256];
                        std::snprintf(buf, sizeof(buf),
                                      "[clip] %s: '%ls' keyframes=%zu\n",
                                      path.c_str(), c->name.c_str(), kfs);
                        engine::core::LogInfoA(buf);
                    }
                    const auto* picked = pickMotion(loaded.clips);
                    if (picked)
                    {
                        char buf[256];
                        std::snprintf(buf, sizeof(buf),
                                      "[clip] %s -> picked '%ls'\n",
                                      path.c_str(), picked->name.c_str());
                        engine::core::LogInfoA(buf);
                        clipMap.emplace(path, picked);
                    }
                    m_controllerClipCache.emplace(path, std::move(loaded.clips));
                };
                for (const auto& state : controller->states)
                {
                    loadClipIntoMap(state.motionClipPath);
                    for (const auto& entry : state.blendTree)
                    {
                        loadClipIntoMap(entry.motionClipPath);
                    }
                }

                m_loadedController = std::move(controller);
                m_animatorRuntime = std::make_unique<engine::anim::AnimatorRuntime>(
                    *m_loadedController, *asset.skeleton, std::move(clipMap));

                m_animSkeleton = asset.skeleton.get();   // RecordDraw 가 skeleton 본 수 참조에 사용.

                // Foot IK 본 캐시 — animator 활성 instance 의 스켈레톤에서 추출.
                if (m_footIKBones && m_footIKConfig)
                {
                    *m_footIKBones = engine::anim::FindFootIKBones(*asset.skeleton, *m_footIKConfig);
                }
                // animator 가 활성인 *첫 번째 instance* 의 index 캐시 — Tick 의 world matrix 계산용.
                {
                    for (size_t mi = 0; mi < m_scene.meshes.size(); ++mi)
                    {
                        if (!m_scene.meshes[mi].animatorControllerPath.empty())
                        {
                            m_animatorInstanceIdx = mi;
                            break;
                        }
                    }
                }

                engine::core::LogInfoA("[anim] AnimatorRuntime active: states=");
                {
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "%zu, transitions=%zu, params=%zu, default=",
                                  m_loadedController->states.size(),
                                  m_loadedController->transitions.size(),
                                  m_loadedController->parameters.size());
                    engine::core::LogInfoA(buf);
                }
                engine::core::LogInfoA(m_animatorRuntime->CurrentStateName().c_str());
                engine::core::LogInfoA("\n");
                break;   // 첫 controller-using 인스턴스만
            }
            catch (const std::exception& e)
            {
                engine::core::LogInfoA("[anim] controller load FAILED (");
                engine::core::LogInfoA(inst.animatorControllerPath.c_str());
                engine::core::LogInfoA("): ");
                engine::core::LogInfoA(e.what());
                engine::core::LogInfoA("\n");
            }
        }

        // Animator 데모용 — 첫 번째 *애니메이션 가능* 인스턴스 선택.
        // 우선순위:
        //   ① animationClipPath 지정된 인스턴스: 베이스 메시의 스켈레톤 + 별도 클립 FBX 의 클립.
        //      자동으로 첫 클립 활성화 (Editor 에서 미리보기 의도).
        //   ② 그게 없으면 메시 FBX 자체에 클립이 있는 인스턴스 (기존 동작, T-pose 시작).
        // 첫 매칭 인스턴스 발견 즉시 break — 멀티 캐릭터 씬에서 첫 캐릭터 기준.
        bool autoActivateClip = false;
        for (const auto& inst : m_scene.meshes)
        {
            const auto& asset = m_assetCache.at(inst.meshAssetPath);
            if (!asset.skeleton) { continue; }

            if (!inst.animationClipPath.empty())
            {
                // 캐시에 없으면 LoadFbxAnimationOnly — 메시 없이 클립만 추출.
                if (!m_clipOnlyCache.contains(inst.animationClipPath))
                {
                    const std::wstring wpath =
                        std::filesystem::absolute(inst.animationClipPath).wstring();
                    engine::render::fbx_loader::LoadedFbxAnimation loaded =
                        engine::render::fbx_loader::LoadFbxAnimationOnly(
                            wpath.c_str(), *asset.skeleton);
                    m_clipOnlyCache.emplace(inst.animationClipPath, std::move(loaded.clips));
                }
                const auto& clipVec = m_clipOnlyCache.at(inst.animationClipPath);
                if (!clipVec.empty())
                {
                    m_animSkeleton    = asset.skeleton.get();
                    m_animClips       = &clipVec;
                    autoActivateClip  = true;
                    break;
                }
            }
            else if (!asset.clips.empty())
            {
                m_animSkeleton = asset.skeleton.get();
                m_animClips    = &asset.clips;
                break;
            }
        }

        // animationClipPath 명시 → 첫 클립 자동 활성화 (Editor 미리보기 흐름).
        if (autoActivateClip && m_animSkeleton && m_animClips && !m_animClips->empty())
        {
            m_animator = std::make_unique<engine::render::Animator>(
                *m_animSkeleton, *(*m_animClips)[0]);
            m_currentClipIdx = 0;
        }

        // 인스턴스 × frame ConstantBuffer.
        const auto instCount = m_scene.meshes.size();
        m_instFrameCBs .resize(instCount);
        m_instBoneCBs  .resize(instCount);
        m_instShadowCBs.resize(instCount);
        for (size_t i = 0; i < instCount; ++i)
        {
            for (engine::uint32 f = 0; f < kFrameCount; ++f)
            {
                m_instFrameCBs[i][f] = std::make_unique<engine::render::ConstantBuffer>(
                    device, static_cast<engine::uint32>(sizeof(FrameConstants)));
                m_instBoneCBs [i][f] = std::make_unique<engine::render::ConstantBuffer>(
                    device, static_cast<engine::uint32>(sizeof(BonePalette)));
                m_instShadowCBs[i][f] = std::make_unique<engine::render::ConstantBuffer>(
                    device, static_cast<engine::uint32>(sizeof(ShadowConstants)));
            }
        }

        // 라이트 SB × frame.
        for (engine::uint32 f = 0; f < kFrameCount; ++f)
        {
            m_dirLightSBs  [f] = std::make_unique<engine::render::StructuredBuffer>(
                device, kDirLightCapacity,   static_cast<engine::uint32>(sizeof(DirectionalLightGpu)));
            m_pointLightSBs[f] = std::make_unique<engine::render::StructuredBuffer>(
                device, kPointLightCapacity, static_cast<engine::uint32>(sizeof(PointLightGpu)));
        }

        // normal map 폴백 — 1x1 평탄 노멀(탄젠트 +Z). 머티리얼에 normal map 없으면 t3 에 바인딩.
        {
            const std::uint8_t kFlatNormal[4] = { 128, 128, 255, 255 };
            m_flatNormalTex = std::make_unique<engine::render::Texture>(
                device, queue, uploadList, kFlatNormal, 1, 1);
            m_flatNormalTex->CreateSrv(device, srvHeap);
        }
    }

    SceneRuntime::~SceneRuntime() = default;

    // === 지형 높이맵 ===
    float SceneRuntime::SampleGround(float x, float z) const noexcept
    {
        if (!m_heightMap.Empty()) { return m_heightMap.Sample(x, z); }
        return engine::render::procedural_terrain::DefaultHeightFunc(x, z);
    }

    float SceneRuntime::TerrainHalfExtent() const noexcept
    {
        return (m_heightMap.Empty() ? 5000.0f : m_heightMap.WorldWidth()) * 0.5f;
    }

    void SceneRuntime::EnsureTerrainHeightMap()
    {
        if (!m_heightMap.Empty()) { return; }
        // 현재 절차적 지형을 그리드로 베이크 — 기존 메시(192 seg)와 동일 해상도라 UpdateVertices 일치.
        m_heightMap = engine::render::HeightMap::Bake(
            193, 193, 5000.0f, 5000.0f,
            engine::render::procedural_terrain::DefaultHeightFunc);
        RegenerateTerrainMesh();
    }

    void SceneRuntime::RegenerateTerrainMesh()
    {
        if (m_terrainMesh == nullptr || m_heightMap.Empty()) { return; }
        std::vector<engine::render::Mesh::Vertex> verts;
        engine::render::procedural_terrain::FillGridVertices(
            verts, m_heightMap.WorldWidth(), m_heightMap.WorldDepth(),
            m_heightMap.Cols() - 1, m_heightMap.Rows() - 1,
            [this](float x, float z) { return m_heightMap.Sample(x, z); });
        m_terrainMesh->UpdateVertices(verts.data(), static_cast<engine::uint32>(verts.size()));
    }

    void SceneRuntime::SculptTerrain(float wx, float wz, float radius, float strength, int brush)
    {
        EnsureTerrainHeightMap();
        const auto b = (brush == 1) ? engine::render::HeightMap::Brush::Lower
                     : (brush == 2) ? engine::render::HeightMap::Brush::Smooth
                                    : engine::render::HeightMap::Brush::Raise;
        if (m_heightMap.Sculpt(wx, wz, radius, strength, b)) { RegenerateTerrainMesh(); }
    }

    bool SceneRuntime::SaveTerrainHeightMap(std::string_view path) const
    {
        return m_heightMap.Save(path);
    }

    namespace
    {
        // column-vector convention 4x4 곱 — C = A·B (수학적 행렬곱, B 먼저 적용 후 A).
        DirectX::XMFLOAT4X4 MatMulCol(const DirectX::XMFLOAT4X4& A, const DirectX::XMFLOAT4X4& B)
        {
            DirectX::XMFLOAT4X4 C;
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                {
                    float s = 0.0f;
                    for (int k = 0; k < 4; ++k) { s += A.m[i][k] * B.m[k][j]; }
                    C.m[i][j] = s;
                }
            return C;
        }

        // column-convention rotate-about-pivot: T = Trans(pivot)·Rot·Trans(-pivot).
        //   axis 정규화 가정 X — 내부에서 정규화. angle rad.
        DirectX::XMFLOAT4X4 RotateAboutPivotCol(const DirectX::XMFLOAT3& axis, float angle,
                                                const DirectX::XMFLOAT3& pivot)
        {
            using namespace DirectX;
            const float len = std::sqrt(axis.x*axis.x + axis.y*axis.y + axis.z*axis.z);
            if (len < 1e-6f)
            {
                XMFLOAT4X4 I; XMStoreFloat4x4(&I, XMMatrixIdentity()); return I;
            }
            const float x = axis.x/len, y = axis.y/len, z = axis.z/len;
            const float c = std::cos(angle), s = std::sin(angle), t = 1.0f - c;
            // column-convention 3x3 rotation (R·v rotates v).
            XMFLOAT4X4 R{};
            R.m[0][0] = t*x*x + c;   R.m[0][1] = t*x*y - s*z; R.m[0][2] = t*x*z + s*y; R.m[0][3] = 0.0f;
            R.m[1][0] = t*x*y + s*z; R.m[1][1] = t*y*y + c;   R.m[1][2] = t*y*z - s*x; R.m[1][3] = 0.0f;
            R.m[2][0] = t*x*z - s*y; R.m[2][1] = t*y*z + s*x; R.m[2][2] = t*z*z + c;   R.m[2][3] = 0.0f;
            R.m[3][0] = 0.0f; R.m[3][1] = 0.0f; R.m[3][2] = 0.0f; R.m[3][3] = 1.0f;
            // Trans(pivot) (column): translation in right column.
            // T = Tp · R · T(-p). column convention: new = Tp·R·Tmp.
            XMFLOAT4X4 Tp{}, Tmp{};
            XMStoreFloat4x4(&Tp,  XMMatrixIdentity());
            XMStoreFloat4x4(&Tmp, XMMatrixIdentity());
            Tp.m[0][3]  =  pivot.x; Tp.m[1][3]  =  pivot.y; Tp.m[2][3]  =  pivot.z;
            Tmp.m[0][3] = -pivot.x; Tmp.m[1][3] = -pivot.y; Tmp.m[2][3] = -pivot.z;
            return MatMulCol(MatMulCol(Tp, R), Tmp);
        }
    }

    void SceneRuntime::Tick(float dt)
    {
        // AnimatorRuntime (M1+) 가 활성이면 그것 우선. 폴백은 단일 클립 Animator (M0 호환).
        if (m_animatorRuntime) { m_animatorRuntime->Update(dt); }
        else if (m_animator)   { m_animator->Update(dt); }

        // 수동 포징 — BuildPalette(Update 내부) 직후 manual 회전을 subtree 에 적용.
        if (m_animatorRuntime && !m_boneManualRot.empty()) { ApplyManualBonePosing(); }

        // Foot IK — Update(BuildPalette) + manual posing 직후. CCD rotate-about-pivot 로 두 발을
        //   각자 발밑 지면에 안착 (검증된 SolveBoneIK 기법, mesh 정상 변형).
        if (m_footIKEnabled) { ApplyFootIKRuntime(dt); }
    }

    void SceneRuntime::SetFootIKConfig(const engine::anim::FootIKConfig& cfg)
    {
        if (!m_footIKConfig) { m_footIKConfig = std::make_unique<engine::anim::FootIKConfig>(); }
        *m_footIKConfig = cfg;
        // bone 이름 바뀌었을 수도 — 재캐시.
        if (m_animSkeleton && m_footIKBones)
        {
            *m_footIKBones = engine::anim::FindFootIKBones(*m_animSkeleton, *m_footIKConfig);
        }
    }
    const engine::anim::FootIKConfig& SceneRuntime::FootIKConfigRef() const noexcept
    {
        return *m_footIKConfig;
    }
    engine::anim::FootIKConfig& SceneRuntime::FootIKConfigMutable() noexcept
    {
        return *m_footIKConfig;
    }
    const engine::anim::FootIKDebug& SceneRuntime::LastFootIKDebug() const noexcept
    {
        return *m_footIKDebug;
    }

    bool SceneRuntime::HasAnimatorRuntime() const noexcept
    {
        return m_animatorRuntime != nullptr;
    }

    engine::scene::Transform* SceneRuntime::AnimatorInstanceTransform() noexcept
    {
        for (auto& inst : m_scene.meshes)
        {
            if (!inst.animatorControllerPath.empty())
            {
                return &inst.transform;
            }
        }
        return nullptr;
    }

    float SceneRuntime::AnimatorStateDuration(std::string_view stateName) const noexcept
    {
        if (!m_animatorRuntime) { return 0.0f; }
        return static_cast<float>(m_animatorRuntime->StateDuration(stateName));
    }

    float SceneRuntime::AnimatorRootMotionY() const
    {
        if (!m_animatorRuntime) { return 0.0f; }
        return m_animatorRuntime->RootMotionY();
    }

    std::string SceneRuntime::AnimatorCurrentStateName() const
    {
        if (!m_animatorRuntime) { return {}; }
        return m_animatorRuntime->CurrentStateName();
    }

    float SceneRuntime::AnimatorCurrentStateTime() const noexcept
    {
        if (!m_animatorRuntime) { return 0.0f; }
        return static_cast<float>(m_animatorRuntime->CurrentStateTime());
    }

    bool SceneRuntime::AnimatorIsPaused() const noexcept
    {
        return m_animatorRuntime && m_animatorRuntime->IsPaused();
    }

    void SceneRuntime::AnimatorSetPaused(bool paused) noexcept
    {
        if (m_animatorRuntime) { m_animatorRuntime->SetPaused(paused); }
    }

    void SceneRuntime::AnimatorSetCurrentStateTime(float t) noexcept
    {
        if (m_animatorRuntime) { m_animatorRuntime->SetCurrentStateTime(static_cast<double>(t)); }
    }

    float SceneRuntime::AnimatorBoneMeshLocalY(std::wstring_view boneName) const
    {
        if (!m_animatorRuntime || !m_animSkeleton) { return 0.0f; }
        // 정확 매칭 우선, 실패 시 substring 매칭 (Mixamo namespace prefix 변동 흡수).
        engine::int32 idx = m_animSkeleton->FindIndex(std::wstring{ boneName });
        if (idx < 0)
        {
            const std::wstring needle{ boneName };
            for (size_t i = 0; i < m_animSkeleton->BoneCount(); ++i)
            {
                if (m_animSkeleton->Bones()[i].name.find(needle) != std::wstring::npos)
                {
                    idx = static_cast<engine::int32>(i);
                    break;
                }
            }
        }
        if (idx < 0) { return 0.0f; }
        return m_animatorRuntime->BoneMeshLocalY(static_cast<size_t>(idx));
    }

    float SceneRuntime::AnimatorBoneMeshLocalX(std::wstring_view boneName) const
    {
        if (!m_animatorRuntime || !m_animSkeleton) { return 0.0f; }
        engine::int32 idx = m_animSkeleton->FindIndex(std::wstring{ boneName });
        if (idx < 0)
        {
            const std::wstring needle{ boneName };
            for (size_t i = 0; i < m_animSkeleton->BoneCount(); ++i)
            {
                if (m_animSkeleton->Bones()[i].name.find(needle) != std::wstring::npos)
                {
                    idx = static_cast<engine::int32>(i);
                    break;
                }
            }
        }
        if (idx < 0) { return 0.0f; }
        return m_animatorRuntime->BoneMeshLocalX(static_cast<size_t>(idx));
    }

    void SceneRuntime::SetAnimatorFloat(std::string_view name, float value)
    {
        if (m_animatorRuntime) { m_animatorRuntime->SetFloat(name, value); }
    }

    void SceneRuntime::SetAnimatorBool(std::string_view name, bool value)
    {
        if (m_animatorRuntime) { m_animatorRuntime->SetBool(name, value); }
    }

    void SceneRuntime::SetAnimatorTrigger(std::string_view name)
    {
        if (m_animatorRuntime) { m_animatorRuntime->SetTrigger(name); }
    }

    std::string SceneRuntime::CurrentAnimatorStateName() const
    {
        if (m_animatorRuntime) { return m_animatorRuntime->CurrentStateName(); }
        return {};
    }

    bool SceneRuntime::GetSkeletonWorldSegments(
        std::vector<std::pair<DirectX::XMFLOAT3, DirectX::XMFLOAT3>>& outPairs) const
    {
        outPairs.clear();
        if (!m_animatorRuntime || !m_animSkeleton) { return false; }
        if (m_animatorInstanceIdx >= m_scene.meshes.size()) { return false; }

        using namespace DirectX;
        const auto& boneGlobal = m_animatorRuntime->BoneGlobal();
        const auto& bones      = m_animSkeleton->Bones();
        if (boneGlobal.size() != bones.size() || bones.empty()) { return false; }

        // mesh-local → world. 본 mesh-local pos = boneGlobal translation (m[0..2][3]),
        // 정점과 동일 swapped 공간 → 정점 렌더와 동일 world matrix 적용 시 정확히 overlay.
        const auto& inst       = m_scene.meshes[m_animatorInstanceIdx];
        const XMMATRIX importM = ComposeWorld(inst.importTransform);
        const XMMATRIX instM   = ComposeWorld(inst.transform);
        const XMMATRIX meshW   = importM * instM;

        auto bonePosWorld = [&](size_t b) -> XMFLOAT3 {
            const XMFLOAT4X4& m = boneGlobal[b];
            const XMVECTOR meshLocal = XMVectorSet(m.m[0][3], m.m[1][3], m.m[2][3], 1.0f);
            const XMVECTOR world     = XMVector3TransformCoord(meshLocal, meshW);
            XMFLOAT3 r; XMStoreFloat3(&r, world); return r;
        };

        outPairs.reserve(bones.size());
        for (size_t b = 0; b < bones.size(); ++b)
        {
            const engine::int32 parent = bones[b].parentIndex;
            if (parent < 0 || static_cast<size_t>(parent) >= bones.size()) { continue; }
            outPairs.emplace_back(bonePosWorld(static_cast<size_t>(parent)), bonePosWorld(b));
        }
        return !outPairs.empty();
    }

    bool SceneRuntime::GetSkeletonWorldJoints(
        std::vector<DirectX::XMFLOAT3>& outPositions,
        std::vector<int>&               outParent,
        std::vector<std::string>&       outNames) const
    {
        outPositions.clear();
        outParent.clear();
        outNames.clear();
        if (!m_animatorRuntime || !m_animSkeleton) { return false; }
        if (m_animatorInstanceIdx >= m_scene.meshes.size()) { return false; }

        using namespace DirectX;
        const auto& boneGlobal = m_animatorRuntime->BoneGlobal();
        const auto& bones      = m_animSkeleton->Bones();
        if (boneGlobal.size() != bones.size() || bones.empty()) { return false; }

        const auto& inst       = m_scene.meshes[m_animatorInstanceIdx];
        const XMMATRIX importM = ComposeWorld(inst.importTransform);
        const XMMATRIX instM   = ComposeWorld(inst.transform);
        const XMMATRIX meshW   = importM * instM;

        outPositions.reserve(bones.size());
        outParent.reserve(bones.size());
        outNames.reserve(bones.size());
        for (size_t b = 0; b < bones.size(); ++b)
        {
            const XMFLOAT4X4& m = boneGlobal[b];
            const XMVECTOR meshLocal = XMVectorSet(m.m[0][3], m.m[1][3], m.m[2][3], 1.0f);
            const XMVECTOR world     = XMVector3TransformCoord(meshLocal, meshW);
            XMFLOAT3 wp; XMStoreFloat3(&wp, world);
            outPositions.push_back(wp);
            outParent.push_back(static_cast<int>(bones[b].parentIndex));
            // wstring 이름 → ascii (디버그 표시용, 비-ascii 는 '?').
            const std::wstring& wn = bones[b].name;
            std::string nm; nm.reserve(wn.size());
            for (wchar_t c : wn) { nm.push_back(c < 128 ? static_cast<char>(c) : '?'); }
            outNames.push_back(std::move(nm));
        }
        return true;
    }

    void SceneRuntime::AddBoneManualRotation(int boneIdx,
                                             const DirectX::XMFLOAT3& axisWorld,
                                             float angleDelta)
    {
        using namespace DirectX;
        if (boneIdx < 0) { return; }
        if (std::abs(angleDelta) < 1e-7f) { return; }
        if (m_animatorInstanceIdx >= m_scene.meshes.size()) { return; }

        // world-space 축 → mesh-local(model) 축. model→world = importTransform*instTransform (row-vec).
        //   world→model 방향 변환 = inverse(meshWorld) 의 3x3 부분 적용.
        const auto& inst       = m_scene.meshes[m_animatorInstanceIdx];
        const XMMATRIX meshW   = ComposeWorld(inst.importTransform) * ComposeWorld(inst.transform);
        XMVECTOR det;
        const XMMATRIX meshWInv = XMMatrixInverse(&det, meshW);
        if (XMVectorGetX(det) == 0.0f) { return; }
        XMVECTOR axisModelV = XMVector3TransformNormal(XMLoadFloat3(&axisWorld), meshWInv);
        const float mlen = XMVectorGetX(XMVector3Length(axisModelV));
        if (mlen < 1e-6f) { return; }
        const XMVECTOR axis = XMVectorScale(axisModelV, 1.0f / mlen);
        const XMVECTOR dq   = XMQuaternionRotationAxis(axis, angleDelta);
        AccumulateBoneModelQuat(boneIdx, dq);
    }

    void SceneRuntime::AccumulateBoneModelQuat(int boneIdx, DirectX::FXMVECTOR dq)
    {
        using namespace DirectX;
        if (boneIdx < 0) { return; }
        auto it = m_boneManualRot.find(boneIdx);
        if (it == m_boneManualRot.end())
        {
            XMFLOAT4 q; XMStoreFloat4(&q, XMQuaternionNormalize(dq));
            m_boneManualRot.emplace(boneIdx, q);
        }
        else
        {
            const XMVECTOR cur = XMLoadFloat4(&it->second);
            // 모델공간 누적 — 새 회전을 기존 앞에 곱 (dq * cur).
            XMStoreFloat4(&it->second, XMQuaternionNormalize(XMQuaternionMultiply(cur, dq)));
        }
    }

    void SceneRuntime::ClearBoneManualPosing() noexcept { m_boneManualRot.clear(); }
    bool SceneRuntime::HasBoneManualPosing() const noexcept { return !m_boneManualRot.empty(); }

    void SceneRuntime::ApplyManualBonePosing()
    {
        using namespace DirectX;
        if (!m_animatorRuntime || !m_animSkeleton) { return; }
        const auto& bones = m_animSkeleton->Bones();
        const auto& bg    = m_animatorRuntime->BoneGlobal();
        if (bg.size() != bones.size()) { return; }

        // 각 manipulated 본을 인덱스 오름차순으로 적용 (parent-first ≈ Mixamo 계층 순).
        std::vector<int> order;
        order.reserve(m_boneManualRot.size());
        for (const auto& kv : m_boneManualRot) { order.push_back(kv.first); }
        std::sort(order.begin(), order.end());

        for (int boneIdx : order)
        {
            if (boneIdx < 0 || static_cast<size_t>(boneIdx) >= bones.size()) { continue; }

            // pivot = 본의 현재 모델공간 위치 (column-convention translation = 오른쪽 열).
            const XMFLOAT4X4& bm = bg[static_cast<size_t>(boneIdx)];
            const XMFLOAT3 pivot{ bm.m[0][3], bm.m[1][3], bm.m[2][3] };

            // 누적 quat → axis/angle → column-convention rotate-about-pivot.
            const XMVECTOR q = XMLoadFloat4(&m_boneManualRot[boneIdx]);
            XMVECTOR axisV; float angle;
            XMQuaternionToAxisAngle(&axisV, &angle, q);
            if (std::abs(angle) < 1e-6f) { continue; }
            XMFLOAT3 axis; XMStoreFloat3(&axis, axisV);
            const XMFLOAT4X4 T = RotateAboutPivotCol(axis, angle, pivot);

            // BFS subtree (본 + 모든 자손). new_col = T · old_col.
            std::vector<int> stack{ boneIdx };
            std::vector<char> visited(bones.size(), 0);
            while (!stack.empty())
            {
                const int cur = stack.back(); stack.pop_back();
                if (visited[static_cast<size_t>(cur)]) { continue; }
                visited[static_cast<size_t>(cur)] = 1;

                const XMFLOAT4X4 newM = MatMulCol(T, bg[static_cast<size_t>(cur)]);
                m_animatorRuntime->SetBoneGlobal(static_cast<size_t>(cur), newM);

                for (size_t b = 0; b < bones.size(); ++b)
                {
                    if (bones[b].parentIndex == cur) { stack.push_back(static_cast<int>(b)); }
                }
            }
        }
    }

    void SceneRuntime::ApplyFootIKRuntime(float dt)
    {
        using namespace DirectX;
        if (!m_animatorRuntime || !m_animSkeleton || !m_footIKBones || !m_footIKConfig) { return; }
        if (m_animatorInstanceIdx >= m_scene.meshes.size()) { return; }
        const auto& cfg = *m_footIKConfig;
        if (cfg.weight <= 0.001f || m_footIKWeight <= 0.01f) { return; }   // 속도 페이드로 꺼지면 skip

        const auto& bones = m_animSkeleton->Bones();
        if (m_animatorRuntime->BoneGlobal().size() != bones.size()) { return; }

        const auto& inst     = m_scene.meshes[m_animatorInstanceIdx];
        const XMMATRIX meshW = ComposeWorld(inst.importTransform) * ComposeWorld(inst.transform);
        XMVECTOR det;
        const XMMATRIX meshWInv = XMMatrixInverse(&det, meshW);
        if (XMVectorGetX(det) == 0.0f) { return; }

        // Root lift — 리그의 발이 메쉬 원점보다 *상수(≈toe depth)만큼 아래* 에 있는데 컨트롤러가 그 원점을
        //   지면에 핀(pos.y=지면) → 발이 그 상수만큼 묻힘. 진단(2026-06): 순수 애니 toe world Y ≈ −14.6
        //   (몸 원점=지면=0 기준) → 스켈레톤 전체를 +14.6 올려 평지서 발(sole)이 지표에 닿게. 몸·발이 함께
        //   올라가 다리는 자연 포즈 유지(크라우치/뜸 없음). 이게 정확해야 절대 접지 IK 가 보정 0(평지)이 됨.
        const float kRootLift = 14.6f;
        // 실제 적용 root lift = kRootLift − bodyLower. 낮은 발이 다리 길이로 못 닿으면 몸을 내려(bodyLower)
        //   닿게 함. solveLeg(목표 = ground+aboveToe−effectiveLift)·최종 translate 모두 이 값을 씀.
        float effectiveLift = kRootLift;

        auto bonePos = [&](int b) -> XMVECTOR {
            const XMFLOAT4X4& m = m_animatorRuntime->BoneGlobal()[static_cast<size_t>(b)];
            return XMVectorSet(m.m[0][3], m.m[1][3], m.m[2][3], 1.0f);
        };
        // joint subtree 를 live BoneGlobal 에 직접 rotate-about-pivot (column-convention) 적용.
        auto rotateSubtreeLive = [&](int joint, const XMFLOAT3& axis, float angle, const XMFLOAT3& pivot)
        {
            const XMFLOAT4X4 T = RotateAboutPivotCol(axis, angle, pivot);
            std::vector<int>  stack{ joint };
            std::vector<char> visited(bones.size(), 0);
            while (!stack.empty())
            {
                const int cur = stack.back(); stack.pop_back();
                if (visited[static_cast<size_t>(cur)]) { continue; }
                visited[static_cast<size_t>(cur)] = 1;
                const XMFLOAT4X4 newM =
                    MatMulCol(T, m_animatorRuntime->BoneGlobal()[static_cast<size_t>(cur)]);
                m_animatorRuntime->SetBoneGlobal(static_cast<size_t>(cur), newM);
                for (size_t b = 0; b < bones.size(); ++b)
                {
                    if (bones[b].parentIndex == cur) { stack.push_back(static_cast<int>(b)); }
                }
            }
        };
        // 모든 본을 model-space 벡터만큼 평행이동 (root lift — 스켈레톤 전체를 강체 이동).
        auto translateAllBones = [&](XMVECTOR modelDelta)
        {
            const float dx = XMVectorGetX(modelDelta);
            const float dy = XMVectorGetY(modelDelta);
            const float dz = XMVectorGetZ(modelDelta);
            for (size_t b = 0; b < bones.size(); ++b)
            {
                XMFLOAT4X4 m = m_animatorRuntime->BoneGlobal()[b];
                m.m[0][3] += dx; m.m[1][3] += dy; m.m[2][3] += dz;
                m_animatorRuntime->SetBoneGlobal(b, m);
            }
        };
        // === Foot phase — swing(든)/airborne 발 감지. 디딘 발만 IK.
        //   (a) 상대: 두 발 애니 world Y 중 낮은 발=디딘 발 (걷기 swing 구분).
        //   (b) 절대: 발이 *제 발밑 지면* 위로 떠 있는 높이 (달리기 flight 양발 공중 구분).
        //   둘 다 낮아야(min) 디딘 발 → flight 에선 양발 모두 IK 제외. 경사/단차도 안전.
        auto footYG = [&](int ankle) -> std::pair<float, float> {   // {animWorldY, groundAtFoot}
            if (ankle < 0 || static_cast<size_t>(ankle) >= bones.size()) { return { 1e9f, 0.0f }; }
            const XMVECTOR w = XMVector3TransformCoord(bonePos(ankle), meshW);
            const float g = m_groundSampler ? m_groundSampler(XMVectorGetX(w), XMVectorGetZ(w)) : 0.0f;
            return { XMVectorGetY(w), g };
        };
        const auto [leftAnimY,  leftG]  = footYG(m_footIKBones->leftAnkle);
        const auto [rightAnimY, rightG] = footYG(m_footIKBones->rightAnkle);
        const float minAnkleY = std::min(leftAnimY, rightAnimY);

        // Toe 본 — 절대 접지의 핀 지점(발바닥 앞). 이름으로 한 번 찾아 solveLeg 에 전달.
        auto findBone = [&](const wchar_t* sub) -> int {
            for (size_t b = 0; b < bones.size(); ++b)
                if (bones[b].name.find(sub) != std::wstring::npos) { return static_cast<int>(b); }
            return -1;
        };
        const int leftToe  = findBone(L"LeftToe");
        const int rightToe = findBone(L"RightToe");
        auto plantWeight = [&](float ay, float groundAtFoot) -> float {
            const float rel = std::clamp(1.0f - ((ay - minAnkleY)      - 2.0f)  / 10.0f, 0.0f, 1.0f);
            const float abs = std::clamp(1.0f - ((ay - groundAtFoot)   - 28.0f) / 22.0f, 0.0f, 1.0f);
            return std::min(rel, abs);   // 둘 다 디딘 상태여야 IK
        };

        // 골반 좌우축(두 고관절 사이) — 무릎 굽힘 평면 고정용. 보행/달리기 중에도 안정적이라
        //   무릎을 *sagittal(앞뒤) 평면* 에만 가두면 좌우 jitter 가 원천 제거됨. (hip 은 IK pivot 이라
        //   solveLeg 들이 회전해도 위치 불변 → 한 번만 계산.)
        XMVECTOR lrAxis = XMVectorZero();
        bool     lrValid = false;
        if (m_footIKBones->leftHip >= 0 && m_footIKBones->rightHip >= 0)
        {
            lrAxis = XMVectorSubtract(bonePos(m_footIKBones->rightHip), bonePos(m_footIKBones->leftHip));
            const float lrLen = XMVectorGetX(XMVector3Length(lrAxis));
            if (lrLen > 1e-3f) { lrAxis = XMVectorScale(lrAxis, 1.0f / lrLen); lrValid = true; }
        }

        // 한 다리 analytic two-bone IK — *절대 접지*: 발바닥(toe)을 발밑 지면에 핀. pw=plant weight.
        auto solveLeg = [&](int hip, int knee, int ankle, int toe, float pw, int footIdx)
        {
            if (hip < 0 || knee < 0 || ankle < 0) { return; }
            if (static_cast<size_t>(ankle) >= bones.size()) { return; }

            // ankle·toe 현재 world 위치(pre-IK, pre-lift).
            const XMVECTOR ankleWorld = XMVector3TransformCoord(bonePos(ankle), meshW);
            const float ax = XMVectorGetX(ankleWorld);
            const float ay = XMVectorGetY(ankleWorld);
            const float az = XMVectorGetZ(ankleWorld);
            const float groundAtAnkle = m_groundSampler ? m_groundSampler(ax, az) : 0.0f;

            // === 절대 접지 목표 ===
            //   sole(≈toe)을 *제 발밑 지면* 에 올림. ankle 은 sole 위로 ankleAboveToe(현 포즈 발목−toe)
            //   만큼 떠야 함 → desiredAnkleY = groundUnderToe + ankleAboveToe. 단 발끝/발목 지면 중 *높은*
            //   쪽 기준으로(max) 업/다운슬로프 모두 관통 방지(오르막=toe 지면↑ 가 발 들어올림, 내리막=ankle
            //   지면이 기준 → 발끝은 모서리 밖으로 자연히 넘어감). root lift(+kRootLift)가 뒤에 더해지므로
            //   여기선 그만큼 빼서(pre-lift 공간) 최종이 정확히 지표에 오게. 평지선 보정 0(크라우치/뜸 없음).
            float ankleAboveToe = 7.2f;        // fallback(진단 측정값) — toe 본 없으면 사용
            float groundUnderToe = groundAtAnkle;
            if (toe >= 0 && static_cast<size_t>(toe) < bones.size())
            {
                const XMVECTOR toeWorld = XMVector3TransformCoord(bonePos(toe), meshW);
                ankleAboveToe  = ay - XMVectorGetY(toeWorld);
                groundUnderToe = m_groundSampler
                    ? m_groundSampler(XMVectorGetX(toeWorld), XMVectorGetZ(toeWorld)) : groundAtAnkle;
            }
            const float plantGround   = std::max(groundUnderToe, groundAtAnkle);
            const float desiredAnkleY = plantGround + ankleAboveToe - effectiveLift;
            const float blend         = cfg.weight * pw * m_footIKWeight;
            const float rawCorr       = (desiredAnkleY - ay) * blend;
            // *temporal smoothing* — 보정량을 프레임간 lerp(달리기 plant↔swing 전환에서 발 *툭* 튐 방지).
            //   swing 땐 blend≈0 → rawCorr≈0 → 보정이 0 으로 부드럽게 감쇠. pw<0.05 라도 early-out 없이
            //   매 프레임 lerp 갱신해야 다음 plant 에서 stale 값으로 안 튄다.
            float& sc = m_footIKCorrSmooth[footIdx];
            sc += (rawCorr - sc) * 0.25f;
            const float targetY = ay + sc;
            if (std::abs(targetY - ay) < 0.5f) { return; }   // 보정 미미(swing/평지) — IK·정렬 미적용

            const XMVECTOR targetModel =
                XMVector3TransformCoord(XMVectorSet(ax, targetY, az, 1.0f), meshWInv);

            // === Analytic two-bone IK — 결정론적(지터 없음) + 무릎 굽힘 방향 고정(앞으로) ===
            const XMVECTOR hipP   = bonePos(hip);
            const XMVECTOR kneeP  = bonePos(knee);
            const XMVECTOR ankleP = bonePos(ankle);
            const float L1 = XMVectorGetX(XMVector3Length(XMVectorSubtract(kneeP,  hipP)));
            const float L2 = XMVectorGetX(XMVector3Length(XMVectorSubtract(ankleP, kneeP)));
            if (L1 < 1e-3f || L2 < 1e-3f) { return; }

            XMVECTOR toTarget = XMVectorSubtract(targetModel, hipP);
            float L = XMVectorGetX(XMVector3Length(toTarget));
            if (L < 1e-3f) { return; }
            const float maxL = (L1 + L2) * 0.999f;
            if (L > maxL) { toTarget = XMVectorScale(XMVector3Normalize(toTarget), maxL); L = maxL; }
            const XMVECTOR newAnkle    = XMVectorAdd(hipP, toTarget);
            const XMVECTOR dirToTarget = XMVectorScale(toTarget, 1.0f / L);

            // 무릎 굽힘 방향(pole) — *애니 무릎의 leg 수직 성분* 을 그대로 사용(연속적).
            //   예전엔 cross(leg,좌우축) 의 부호를 애니 무릎 쪽으로 *binary flip* 했는데, 달리기 중
            //   애니 무릎이 그 평면을 지나는 순간 부호가 휙 뒤집혀 무릎이 반대편으로 *툭* 튐. 대신
            //   애니 무릎 방향(hip→knee 의 leg 수직 성분)을 직접 쓰면 부호가 안 뒤집힌다(연속). 거기서
            //   *좌우(lrAxis) 성분만 제거* 해 sagittal 평면에 가두면 좌우 wobble 도 없앤다 → 둘 다 해결.
            XMVECTOR bend;
            {
                const XMVECTOR hk = XMVectorSubtract(kneeP, hipP);
                XMVECTOR fwd = XMVectorSubtract(hk,
                    XMVectorScale(dirToTarget, XMVectorGetX(XMVector3Dot(hk, dirToTarget))));
                if (lrValid)   // 좌우 성분 제거 → sagittal 평면 (wobble 방지)
                {
                    fwd = XMVectorSubtract(fwd,
                        XMVectorScale(lrAxis, XMVectorGetX(XMVector3Dot(fwd, lrAxis))));
                }
                float bl = XMVectorGetX(XMVector3Length(fwd));
                if (bl < 1e-3f)
                {   // 다리 거의 일직선 — 굽힘 방향 모호(이때 무릎 위치는 bend 에 거의 무관).
                    if (!lrValid) { return; }
                    fwd = XMVector3Cross(dirToTarget, lrAxis);   // 안전 폴백
                    bl  = XMVectorGetX(XMVector3Length(fwd));
                    if (bl < 1e-3f) { return; }
                }
                bend = XMVectorScale(fwd, 1.0f / bl);
            }

            // 코사인 법칙 — hip 정점 각. knee 를 dirToTarget 에서 bend 쪽으로 hipAngle 회전.
            const float cosH = std::clamp((L1*L1 + L*L - L2*L2) / (2.0f * L1 * L), -1.0f, 1.0f);
            const float hipAngle = std::acos(cosH);
            const XMVECTOR kneeDir = XMVectorAdd(
                XMVectorScale(dirToTarget, std::cos(hipAngle)),
                XMVectorScale(bend,        std::sin(hipAngle)));
            const XMVECTOR newKnee = XMVectorAdd(hipP, XMVectorScale(kneeDir, L1));

            // from→to 정렬 회전을 joint subtree 에 적용 (model 축).
            auto applyAlign = [&](int joint, XMVECTOR fromV, XMVECTOR toV, const XMVECTOR& pivotV)
            {
                const float lf = XMVectorGetX(XMVector3Length(fromV));
                const float lt = XMVectorGetX(XMVector3Length(toV));
                if (lf < 1e-4f || lt < 1e-4f) { return; }
                fromV = XMVectorScale(fromV, 1.0f / lf);
                toV   = XMVectorScale(toV,   1.0f / lt);
                XMVECTOR axisV = XMVector3Cross(fromV, toV);
                const float al = XMVectorGetX(XMVector3Length(axisV));
                if (al < 1e-5f) { return; }
                axisV = XMVectorScale(axisV, 1.0f / al);
                const float ang = std::acos(std::clamp(XMVectorGetX(XMVector3Dot(fromV, toV)), -1.0f, 1.0f));
                if (ang < 1e-5f) { return; }
                XMFLOAT3 axis;  XMStoreFloat3(&axis,  axisV);
                XMFLOAT3 pivot; XMStoreFloat3(&pivot, pivotV);
                rotateSubtreeLive(joint, axis, ang, pivot);
            };

            // 1) hip: hip→knee 를 hip→newKnee 방향으로 (leg subtree 전체 회전).
            applyAlign(hip, XMVectorSubtract(kneeP, hipP), XMVectorSubtract(newKnee, hipP), hipP);
            // 2) knee: (1 적용 후) knee→ankle 를 newKnee→newAnkle 방향으로.
            const XMVECTOR kneeP2  = bonePos(knee);
            const XMVECTOR ankleP2 = bonePos(ankle);
            applyAlign(knee, XMVectorSubtract(ankleP2, kneeP2),
                             XMVectorSubtract(newAnkle, newKnee), kneeP2);

            // === 발 방향 정렬 (#1) — 발바닥을 지면 *경사* 에 맞춰 기울임.
            //   유한차분으로 발밑 지면 normal 추정 → world up→normal 회전을 ankle subtree 에 적용.
            //   평지면 normal=up → 무회전(애니 발 포즈 유지 = toe-down 은 애니라 그대로). 경사만 기울임.
            //   model 공간에서 from/to 정렬(leg IK 와 동일 패턴) → 컨벤션 reflection 안전.
            //   cap ~22° + plant·전역 weight → 가파른 경사 over-roll(바깥날) 방지.
            if (m_groundSampler && pw > 0.1f)
            {
                const float e  = 6.0f;
                const float hL = m_groundSampler(ax - e, az), hR = m_groundSampler(ax + e, az);
                const float hB = m_groundSampler(ax, az - e), hF = m_groundSampler(ax, az + e);
                const XMVECTOR nWorld = XMVector3Normalize(XMVectorSet(hL - hR, 2.0f * e, hB - hF, 0.0f));
                const XMVECTOR nModel  = XMVector3Normalize(XMVector3TransformNormal(nWorld, meshWInv));
                const XMVECTOR upModel = XMVector3Normalize(
                    XMVector3TransformNormal(XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), meshWInv));
                XMVECTOR axisV = XMVector3Cross(upModel, nModel);
                const float al = XMVectorGetX(XMVector3Length(axisV));
                if (al > 1e-5f)
                {
                    axisV = XMVectorScale(axisV, 1.0f / al);
                    float ang = std::acos(std::clamp(
                        XMVectorGetX(XMVector3Dot(upModel, nModel)), -1.0f, 1.0f));
                    ang = std::min(ang, 0.39f) * pw * m_footIKWeight;   // ~22° cap
                    if (ang > 1e-4f)
                    {
                        XMFLOAT3 axis;  XMStoreFloat3(&axis,  axisV);
                        XMFLOAT3 pivot; XMStoreFloat3(&pivot, bonePos(ankle));
                        rotateSubtreeLive(ankle, axis, ang, pivot);
                    }
                }
            }
        };

        const float leftPlant  = plantWeight(leftAnimY,  leftG);
        const float rightPlant = plantWeight(rightAnimY, rightG);

        // === Body lower (pelvis IK) — 낮은 발이 다리 길이로 못 닿으면 몸을 내려 닿게 함.
        //   *terrain-driven*: 두 발밑 지면 중 *낮은 쪽* 이 몸 접지 기준(controller pos.y)보다
        //   kMaxFootDrop 이상 아래면 그 초과분만큼 몸을 내림. **어느 발이 디뎠는지(plant)·보행
        //   사이클에 무관** → step 주파수 진동(덜그럭) 원천 제거. (예전 per-foot reach-deficit 는
        //   디딘 발이 L↔R 번갈며 deficit 이 매 스텝 출렁여 몸이 위아래로 덜그럭거렸다.) 발 XZ 가
        //   stride 로 범프를 지나며 생기는 잔 ripple 은 느린 스무딩(lerp 0.03)으로 제거. cap=과스쿼트
        //   방지(깊은 구덩이 잔여 뜸은 허용 — 묻힘보다 덜 거슬림).
        // === Body lower (pelvis IK) — *leg-reach deficit* 기반.
        //   이 리그는 standing 다리가 거의 곧아(origin 아래 reach ≈ anim+1.4 뿐) 발을 제 지면에 디디려
        //   해도 *다리가 못 뻗어* 떠버림(공중부양). 그 부족분(desired ankle 가 다리 reach 보다 얼마나
        //   아래)만큼 몸을 내려 닿게 함. controller 가 몸을 지형에 접지해도 발밑 지면이 몸중심 지면과
        //   달라(rolling) bodyRefY 비교는 틀림 → 반드시 *다리 reach* 로 측정. 두 발 max(×plantWeight
        //   안 함 — 평지선 두 발 deficit 유사해 안정, ×pw 가 디딘 발 L↔R 교대로 출렁이게 했던 주범).
        //   effectiveLift 는 직전 프레임값으로 근사(음의 되먹임이라 수렴). dt 지수 스무딩으로 부드럽게.
        const float kMaxBodyLower = 12.0f;
        const float effLiftEst = kRootLift - m_footIKBodyLower;   // 직전값 근사
        auto footDeficit = [&](int hip, int knee, int ankle, int toe) -> float {
            if (hip < 0 || knee < 0 || ankle < 0) { return 0.0f; }
            const XMVECTOR hipW   = XMVector3TransformCoord(bonePos(hip),   meshW);
            const XMVECTOR kneeW  = XMVector3TransformCoord(bonePos(knee),  meshW);
            const XMVECTOR ankleW = XMVector3TransformCoord(bonePos(ankle), meshW);
            const float ax = XMVectorGetX(ankleW), ay = XMVectorGetY(ankleW), az = XMVectorGetZ(ankleW);
            const float gA = m_groundSampler ? m_groundSampler(ax, az) : 0.0f;
            float aboveToe = 7.2f, gT = gA;
            if (toe >= 0 && static_cast<size_t>(toe) < bones.size())
            {
                const XMVECTOR tW = XMVector3TransformCoord(bonePos(toe), meshW);
                aboveToe = ay - XMVectorGetY(tW);
                gT = m_groundSampler ? m_groundSampler(XMVectorGetX(tW), XMVectorGetZ(tW)) : gA;
            }
            const float desiredAnkleWorldY = std::max(gT, gA) + aboveToe;
            const float legLen =
                XMVectorGetX(XMVector3Length(XMVectorSubtract(kneeW,  hipW))) +
                XMVectorGetX(XMVector3Length(XMVectorSubtract(ankleW, kneeW)));
            const float finalHipY = XMVectorGetY(hipW) + effLiftEst;
            const float horiz = std::hypot(ax - XMVectorGetX(hipW), az - XMVectorGetZ(hipW));
            const float vReach = std::sqrt(std::max(legLen * legLen - horiz * horiz, 0.0f)) * 0.98f;
            const float lowestReachableY = finalHipY - vReach;   // 다리로 닿는 최저 ankle Y
            return lowestReachableY - desiredAnkleWorldY;        // signed: >0=뜸(더 내려야), <0=여유(올려도 됨)
        };
        // target = 현재 bodyLower + 남은 부족분(가장 제약 큰 발). deficit 만 쓰면 bodyLower 가 커질수록
        //   deficit 이 줄어 *절반에서 수렴*(half-correction) → 발이 여전히 뜸. 현재값에 더해야 deficit→0
        //   까지 누적해 완전 접지. signed 라 지형이 올라오면(여유) target<현재 → 몸도 다시 올라옴.
        const float defMax = std::max(
            footDeficit(m_footIKBones->leftHip,  m_footIKBones->leftKnee,  m_footIKBones->leftAnkle,  leftToe),
            footDeficit(m_footIKBones->rightHip, m_footIKBones->rightKnee, m_footIKBones->rightAnkle, rightToe));
        const float target = std::clamp(m_footIKBodyLower + defMax, 0.0f, kMaxBodyLower);
        const float tau = 0.15f;   // 시상수 — 지형엔 즉각, 스텝 노이즈 흡수
        const float a   = 1.0f - std::exp(-std::clamp(dt, 0.0f, 0.1f) / tau);
        m_footIKBodyLower += (target - m_footIKBodyLower) * a;
        effectiveLift = kRootLift - m_footIKBodyLower;

        // (골반 하강 #2 는 제거 — 디딘 발 animAnkleY 가 보행 사이클마다 변해 pelvisTarget 이 매 프레임
        //  요동 → 몸 전체가 위아래로 출렁임. 하이브리드 타깃이 standing 을 보존하므로 과신전도 드묾.
        //  단차용 골반 보정이 필요하면 *지면 높이차* 기반(보행 사이클 비의존)으로 재설계 필요.)

        // plant weight 는 height ramp 자체가 부드럽고 즉각적 → 발 IK 별도 temporal 스무딩 없음.
        solveLeg(m_footIKBones->leftHip,  m_footIKBones->leftKnee,  m_footIKBones->leftAnkle,  leftToe,  leftPlant,  0);
        solveLeg(m_footIKBones->rightHip, m_footIKBones->rightKnee, m_footIKBones->rightAnkle, rightToe, rightPlant, 1);

        // Root lift — 스켈레톤 전체를 world up 으로 effectiveLift(=kRootLift−bodyLower) 만큼 올려
        //   묻힘 보정(곧은 다리 유지) + 낮은 발 닿게 몸 하강 반영.
        if (effectiveLift != 0.0f)
        {
            const XMVECTOR modelLift =
                XMVector3TransformNormal(XMVectorSet(0.0f, effectiveLift, 0.0f, 0.0f), meshWInv);
            translateAllBones(modelLift);
        }

        // === calibration readout (디버그 오버레이) — 최종 ankle world Y vs 발밑 지면.
        auto ankleWorldY = [&](int a) -> float {
            if (a < 0 || static_cast<size_t>(a) >= bones.size()) { return 0.0f; }
            return XMVectorGetY(XMVector3TransformCoord(bonePos(a), meshW));
        };
        m_footIKReadout.leftAnkleY  = ankleWorldY(m_footIKBones->leftAnkle);
        m_footIKReadout.leftGroundY = leftG;   m_footIKReadout.leftPlant  = leftPlant;
        m_footIKReadout.rightAnkleY = ankleWorldY(m_footIKBones->rightAnkle);
        m_footIKReadout.rightGroundY= rightG;  m_footIKReadout.rightPlant = rightPlant;
        m_footIKReadout.bodyLower = m_footIKBodyLower;
        m_footIKReadout.valid = true;
    }

    void SceneRuntime::SolveBoneIK(int endBoneIdx,
                                   const DirectX::XMFLOAT3& targetWorld,
                                   int chainLength,
                                   int iterations)
    {
        using namespace DirectX;
        if (!m_animatorRuntime || !m_animSkeleton) { return; }
        const auto& bones = m_animSkeleton->Bones();
        if (endBoneIdx < 0 || static_cast<size_t>(endBoneIdx) >= bones.size()) { return; }
        if (m_animatorInstanceIdx >= m_scene.meshes.size()) { return; }
        if (chainLength < 1) { chainLength = 1; }
        if (iterations  < 1) { iterations  = 1; }

        // target(world) → mesh-local(model). model→world = importTransform*instTransform (row-vec).
        const auto& inst     = m_scene.meshes[m_animatorInstanceIdx];
        const XMMATRIX meshW = ComposeWorld(inst.importTransform) * ComposeWorld(inst.transform);
        XMVECTOR det;
        const XMMATRIX meshWInv = XMMatrixInverse(&det, meshW);
        if (XMVectorGetX(det) == 0.0f) { return; }
        const XMVECTOR targetModel = XMVector3TransformCoord(XMLoadFloat3(&targetWorld), meshWInv);

        // 회전 체인 — 끝 관절의 부모를 위로 chainLength 단계 (effector tip 인 끝 관절은 제외).
        //   예) 손을 잡으면 [팔꿈치(아래팔본), 어깨(위팔본)] → two-bone IK.
        std::vector<int> chain;
        chain.reserve(static_cast<size_t>(chainLength));
        for (int j = bones[static_cast<size_t>(endBoneIdx)].parentIndex, n = 0;
             n < chainLength && j >= 0;
             j = bones[static_cast<size_t>(j)].parentIndex, ++n)
        {
            chain.push_back(j);
        }
        if (chain.empty()) { return; }

        // 작업 복사본 — 현재 렌더 포즈 (애니메이션 + 기존 manual posing). CCD 가 여기서 관절
        //   위치를 계산/갱신하고, 산출된 증분만 m_boneManualRot 에 누적 (단일 source-of-truth).
        std::vector<XMFLOAT4X4> work = m_animatorRuntime->BoneGlobal();
        if (work.size() != bones.size()) { return; }

        auto colPos = [](const XMFLOAT4X4& m) -> XMVECTOR {
            return XMVectorSet(m.m[0][3], m.m[1][3], m.m[2][3], 1.0f);
        };
        // joint subtree 에 model-space rotate-about-pivot 적용 (work 갱신). new_col = T · old_col.
        auto rotateSubtree = [&](int joint, const XMFLOAT3& axis, float angle, const XMFLOAT3& pivot)
        {
            const XMFLOAT4X4 T = RotateAboutPivotCol(axis, angle, pivot);
            std::vector<int>  stack{ joint };
            std::vector<char> visited(bones.size(), 0);
            while (!stack.empty())
            {
                const int cur = stack.back(); stack.pop_back();
                if (visited[static_cast<size_t>(cur)]) { continue; }
                visited[static_cast<size_t>(cur)] = 1;
                work[static_cast<size_t>(cur)] = MatMulCol(T, work[static_cast<size_t>(cur)]);
                for (size_t b = 0; b < bones.size(); ++b)
                {
                    if (bones[b].parentIndex == cur) { stack.push_back(static_cast<int>(b)); }
                }
            }
        };

        // 누적 증분 quat (joint → model quat). solve 종료 후 m_boneManualRot 에 합산.
        std::unordered_map<int, XMVECTOR> deltaQ;

        constexpr float kMaxStep = 0.35f;   // iteration·joint 당 회전 상한 (rad) — 안정성/부드러움.
        for (int iter = 0; iter < iterations; ++iter)
        {
            for (int joint : chain)   // effector 가까운 쪽 (체인 앞)부터 — 표준 CCD 순.
            {
                const XMVECTOR jp  = colPos(work[static_cast<size_t>(joint)]);
                const XMVECTOR eff = colPos(work[static_cast<size_t>(endBoneIdx)]);
                XMVECTOR v1 = XMVectorSubtract(eff,         jp);   // joint → effector
                XMVECTOR v2 = XMVectorSubtract(targetModel, jp);   // joint → target
                const float l1 = XMVectorGetX(XMVector3Length(v1));
                const float l2 = XMVectorGetX(XMVector3Length(v2));
                if (l1 < 1e-4f || l2 < 1e-4f) { continue; }
                v1 = XMVectorScale(v1, 1.0f / l1);
                v2 = XMVectorScale(v2, 1.0f / l2);

                XMVECTOR axisV = XMVector3Cross(v1, v2);
                const float axisLen = XMVectorGetX(XMVector3Length(axisV));
                if (axisLen < 1e-5f) { continue; }   // 평행 (이미 정렬 / 정반대)
                axisV = XMVectorScale(axisV, 1.0f / axisLen);

                float dot = XMVectorGetX(XMVector3Dot(v1, v2));
                dot = std::clamp(dot, -1.0f, 1.0f);
                float angle = std::acos(dot);
                if (angle < 1e-5f) { continue; }
                if (angle > kMaxStep) { angle = kMaxStep; }

                XMFLOAT3 axis;  XMStoreFloat3(&axis,  axisV);
                XMFLOAT3 pivot; XMStoreFloat3(&pivot, jp);
                rotateSubtree(joint, axis, angle, pivot);

                const XMVECTOR dq = XMQuaternionRotationAxis(axisV, angle);
                auto it = deltaQ.find(joint);
                if (it == deltaQ.end()) { deltaQ.emplace(joint, dq); }
                else { it->second = XMQuaternionNormalize(XMQuaternionMultiply(it->second, dq)); }
            }
        }

        // 증분 결과를 manual posing 에 합산 → 다음 Tick 의 ApplyManualBonePosing 가 replay.
        for (const auto& kv : deltaQ)
        {
            AccumulateBoneModelQuat(kv.first, XMQuaternionNormalize(kv.second));
        }
    }

    void SceneRuntime::SetActiveClip(int clipIdx)
    {
        if (m_animSkeleton == nullptr || m_animClips == nullptr) { return; }

        if (clipIdx < 0 || static_cast<size_t>(clipIdx) >= m_animClips->size())
        {
            m_animator.reset();
            m_currentClipIdx = -1;
            return;
        }
        if (m_currentClipIdx != clipIdx)
        {
            m_animator = std::make_unique<engine::render::Animator>(
                *m_animSkeleton, *(*m_animClips)[static_cast<size_t>(clipIdx)]);
            m_currentClipIdx = clipIdx;
        }
    }

    size_t SceneRuntime::ClipCount() const noexcept
    {
        return m_animClips ? m_animClips->size() : 0;
    }

    void SceneRuntime::SyncEditableFieldsFrom(const engine::scene::Scene& source) noexcept
    {
        m_scene.ambient = source.ambient;

        const size_t nm = std::min(m_scene.meshes.size(), source.meshes.size());
        for (size_t i = 0; i < nm; ++i)
        {
            m_scene.meshes[i].transform       = source.meshes[i].transform;
            m_scene.meshes[i].importTransform = source.meshes[i].importTransform;
            m_scene.meshes[i].name            = source.meshes[i].name;
            m_scene.meshes[i].roughness       = source.meshes[i].roughness;   // PBR 라이브 편집 반영
            m_scene.meshes[i].metallic        = source.meshes[i].metallic;
            m_scene.meshes[i].normalStrength  = source.meshes[i].normalStrength;
            m_scene.meshes[i].normalFlipY     = source.meshes[i].normalFlipY;
        }
        const size_t nd = std::min(m_scene.dirLights.size(), source.dirLights.size());
        for (size_t i = 0; i < nd; ++i) { m_scene.dirLights[i] = source.dirLights[i]; }
        const size_t np = std::min(m_scene.pointLights.size(), source.pointLights.size());
        for (size_t i = 0; i < np; ++i) { m_scene.pointLights[i] = source.pointLights[i]; }
    }

    void SceneRuntime::PrepareGpuResources(engine::uint32 frameIndex, const engine::render::Camera& camera)
    {
        // view-proj / 카메라 위치 캐시.
        m_cachedViewProj  = camera.ViewProjection();
        m_cachedCameraPos = camera.Position();

        // 라이트 데이터 → SB 업로드 (frame-shared).
        std::vector<DirectionalLightGpu> dirGpu;
        dirGpu.reserve(m_scene.dirLights.size());
        for (const auto& d : m_scene.dirLights)
        {
            DirectionalLightGpu g{};
            g.directionWS = d.directionWS;
            g.color       = d.color;
            g.intensity   = d.intensity;
            dirGpu.push_back(g);
        }
        std::vector<PointLightGpu> pointGpu;
        pointGpu.reserve(m_scene.pointLights.size());
        for (const auto& p : m_scene.pointLights)
        {
            PointLightGpu g{};
            g.positionWS = p.positionWS;
            g.color      = p.color;
            g.intensity  = p.intensity;
            g.range      = p.range;
            pointGpu.push_back(g);
        }
        m_dirLightSBs  [frameIndex]->UpdateRange(
            dirGpu.empty()  ? nullptr : dirGpu.data(),  static_cast<engine::uint32>(dirGpu.size()));
        m_pointLightSBs[frameIndex]->UpdateRange(
            pointGpu.empty()? nullptr : pointGpu.data(), static_cast<engine::uint32>(pointGpu.size()));

        // 그림자 — 첫 방향광 + 캐릭터(없으면 카메라 타깃) 중심으로 ortho light view-proj 구성.
        using namespace DirectX;
        m_shadowActive = (m_shadowMap != nullptr) && !m_scene.dirLights.empty();
        if (m_shadowActive)
        {
            XMFLOAT3 centerF = camera.Target();
            if (m_animatorInstanceIdx < m_scene.meshes.size())
            {
                centerF = m_scene.meshes[m_animatorInstanceIdx].transform.position;
            }
            const XMVECTOR center   = XMLoadFloat3(&centerF);
            XMVECTOR       lightDir  = XMVector3Normalize(XMLoadFloat3(&m_scene.dirLights[0].directionWS));
            const float    dist      = 800.0f;
            const XMVECTOR eye       = XMVectorSubtract(center, XMVectorScale(lightDir, dist));
            const XMVECTOR up        = (std::abs(XMVectorGetY(lightDir)) > 0.95f)
                                         ? XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f)
                                         : XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
            const XMMATRIX view = XMMatrixLookAtLH(eye, center, up);
            const XMMATRIX proj = XMMatrixOrthographicLH(700.0f, 700.0f, 1.0f, 1600.0f);
            m_cachedLightViewProj = XMMatrixMultiply(view, proj);
        }
    }

    void SceneRuntime::RecordDraw(ID3D12GraphicsCommandList*    list,
                                  engine::uint32                frameIndex,
                                  const engine::render::Texture& fallbackAlbedo)
    {
        // frame-shared 라이트 SRV — RootSig [3]=t1, [4]=t2.
        list->SetGraphicsRootShaderResourceView(3, m_dirLightSBs  [frameIndex]->GpuAddress());
        list->SetGraphicsRootShaderResourceView(4, m_pointLightSBs[frameIndex]->GpuAddress());

        // frame-shared 그림자맵 SRV — RootSig [6]=t4. 없으면 평탄노멀을 더미로(샘플 안 함).
        list->SetGraphicsRootDescriptorTable(
            6, m_shadowMap ? m_shadowMap->SrvGpu() : m_flatNormalTex->SrvGpuHandle());

        // 본 팔레트 — AnimatorRuntime(M1+) 우선, 폴백 Animator(M0), 없으면 identity.
        BonePalette palette = IdentityPalette();
        if (m_animatorRuntime)
        {
            const auto& src = m_animatorRuntime->Palette();
            const size_t n = (src.size() < kMaxBones) ? src.size() : kMaxBones;
            for (size_t i = 0; i < n; ++i) { palette.bones[i] = src[i]; }
        }
        else if (m_animator)
        {
            const auto& src = m_animator->Palette();
            const size_t n = (src.size() < kMaxBones) ? src.size() : kMaxBones;
            for (size_t i = 0; i < n; ++i) { palette.bones[i] = src[i]; }
        }

        // 인스턴스 루프.
        using namespace DirectX;
        const D3D12_GPU_DESCRIPTOR_HANDLE fallbackSrv = fallbackAlbedo.SrvGpuHandle();
        for (size_t i = 0; i < m_scene.meshes.size(); ++i)
        {
            const auto& inst  = m_scene.meshes[i];
            const auto& asset = m_assetCache.at(inst.meshAssetPath);

            // 자산 좌표계 보정 (Mixamo X-Bot 등) 은 importTransform 으로 mesh-local 측 합성.
            //   final = inst.transform 의 변환 적용 *전* 에 importTransform 적용.
            //   row-vector convention (XMMATRIX × XMMATRIX = first A then B): import 먼저, inst 다음.
            //   identity importTransform 은 합성 비용만 미미.
            const XMMATRIX importAdjust = ComposeWorld(inst.importTransform);
            const XMMATRIX instWorld    = ComposeWorld(inst.transform);
            const XMMATRIX world        = importAdjust * instWorld;
            const XMMATRIX mvp          = world * m_cachedViewProj;

            FrameConstants cb{};
            XMStoreFloat4x4(&cb.mvp,   mvp);
            XMStoreFloat4x4(&cb.world, world);
            XMStoreFloat4x4(&cb.lightViewProj, m_cachedLightViewProj);
            cb.cameraPosWS     = m_cachedCameraPos;
            cb.ambient         = m_scene.ambient;
            cb.roughness       = inst.roughness;   // PBR — 오브젝트별 머티리얼
            cb.metallic        = inst.metallic;
            cb.dirLightCount   = static_cast<std::uint32_t>(m_scene.dirLights.size());
            cb.pointLightCount = static_cast<std::uint32_t>(m_scene.pointLights.size());
            cb.shadowEnabled   = m_shadowActive ? 1u : 0u;
            cb.normalFlipY     = inst.normalFlipY ? 1u : 0u;
            cb.normalStrength  = inst.normalStrength;
            cb.applyTonemap    = m_applyTonemap ? 1u : 0u;

            m_instFrameCBs[i][frameIndex]->Update(&cb,     sizeof(cb));
            m_instBoneCBs [i][frameIndex]->Update(&palette, sizeof(palette));

            list->SetGraphicsRootConstantBufferView(0, m_instFrameCBs[i][frameIndex]->GpuAddress());
            list->SetGraphicsRootConstantBufferView(1, m_instBoneCBs [i][frameIndex]->GpuAddress());

            asset.mesh->BindVertexBuffer(list);
            asset.mesh->DrawAll(list, /*materialRootParam*/2, fallbackSrv,
                                /*normalRootParam*/5, m_flatNormalTex->SrvGpuHandle());
        }
    }

    void SceneRuntime::RecordShadowDraw(ID3D12GraphicsCommandList* list, engine::uint32 frameIndex)
    {
        if (!m_shadowActive) { return; }
        using namespace DirectX;

        // 본 팔레트 — 메인 패스와 동일 (같은 값이라 m_instBoneCBs 재사용 안전).
        BonePalette palette = IdentityPalette();
        if (m_animatorRuntime)
        {
            const auto& src = m_animatorRuntime->Palette();
            const size_t n = (src.size() < kMaxBones) ? src.size() : kMaxBones;
            for (size_t i = 0; i < n; ++i) { palette.bones[i] = src[i]; }
        }
        else if (m_animator)
        {
            const auto& src = m_animator->Palette();
            const size_t n = (src.size() < kMaxBones) ? src.size() : kMaxBones;
            for (size_t i = 0; i < n; ++i) { palette.bones[i] = src[i]; }
        }

        // 그림자 rootsig: [0]b0 = worldLightMVP, [1]b1 = bone palette.
        for (size_t i = 0; i < m_scene.meshes.size(); ++i)
        {
            const auto& inst  = m_scene.meshes[i];
            const auto& asset = m_assetCache.at(inst.meshAssetPath);

            const XMMATRIX world = ComposeWorld(inst.importTransform) * ComposeWorld(inst.transform);
            ShadowConstants sc{};
            XMStoreFloat4x4(&sc.worldLightMVP, XMMatrixMultiply(world, m_cachedLightViewProj));

            m_instShadowCBs[i][frameIndex]->Update(&sc,      sizeof(sc));
            m_instBoneCBs  [i][frameIndex]->Update(&palette, sizeof(palette));

            list->SetGraphicsRootConstantBufferView(0, m_instShadowCBs[i][frameIndex]->GpuAddress());
            list->SetGraphicsRootConstantBufferView(1, m_instBoneCBs  [i][frameIndex]->GpuAddress());

            asset.mesh->BindVertexBuffer(list);
            asset.mesh->DrawAllDepthOnly(list);
        }
    }
}
