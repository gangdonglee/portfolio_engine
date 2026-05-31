#include "SceneRuntime.h"

#include "anim/AnimatorController.h"
#include "anim/AnimatorRuntime.h"
#include "anim/AnimatorSerializer.h"
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
#include "render/Skeleton.h"
#include "render/SrvDescriptorHeap.h"
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
            DirectX::XMFLOAT3   cameraPosWS;  float _pad0;
            DirectX::XMFLOAT3   ambient;      float _pad1;
            std::uint32_t       dirLightCount;
            std::uint32_t       pointLightCount;
            std::uint32_t       _pad2[2];
        };
        static_assert(sizeof(FrameConstants) % 16 == 0, "FrameConstants 16바이트 정렬 깨짐");

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

        // 자산 캐시 — 확장자 분기.
        for (const auto& inst : m_scene.meshes)
        {
            if (m_assetCache.contains(inst.meshAssetPath)) { continue; }

            LoadedAsset asset;
            const std::filesystem::path p{ inst.meshAssetPath };
            const auto ext = p.extension().string();
            const std::wstring full = std::filesystem::absolute(p).wstring();

            if (ext == ".fbx" || ext == ".FBX")
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
        }

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
        m_instFrameCBs.resize(instCount);
        m_instBoneCBs .resize(instCount);
        for (size_t i = 0; i < instCount; ++i)
        {
            for (engine::uint32 f = 0; f < kFrameCount; ++f)
            {
                m_instFrameCBs[i][f] = std::make_unique<engine::render::ConstantBuffer>(
                    device, static_cast<engine::uint32>(sizeof(FrameConstants)));
                m_instBoneCBs [i][f] = std::make_unique<engine::render::ConstantBuffer>(
                    device, static_cast<engine::uint32>(sizeof(BonePalette)));
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
    }

    SceneRuntime::~SceneRuntime() = default;

    void SceneRuntime::Tick(float dt)
    {
        // AnimatorRuntime (M1+) 가 활성이면 그것 우선. 폴백은 단일 클립 Animator (M0 호환).
        if (m_animatorRuntime) { m_animatorRuntime->Update(dt); }
        else if (m_animator)   { m_animator->Update(dt); }
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
    }

    void SceneRuntime::RecordDraw(ID3D12GraphicsCommandList*    list,
                                  engine::uint32                frameIndex,
                                  const engine::render::Texture& fallbackAlbedo)
    {
        // frame-shared 라이트 SRV — RootSig [3]=t1, [4]=t2.
        list->SetGraphicsRootShaderResourceView(3, m_dirLightSBs  [frameIndex]->GpuAddress());
        list->SetGraphicsRootShaderResourceView(4, m_pointLightSBs[frameIndex]->GpuAddress());

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
            cb.cameraPosWS     = m_cachedCameraPos;
            cb.ambient         = m_scene.ambient;
            cb.dirLightCount   = static_cast<std::uint32_t>(m_scene.dirLights.size());
            cb.pointLightCount = static_cast<std::uint32_t>(m_scene.pointLights.size());

            m_instFrameCBs[i][frameIndex]->Update(&cb,     sizeof(cb));
            m_instBoneCBs [i][frameIndex]->Update(&palette, sizeof(palette));

            list->SetGraphicsRootConstantBufferView(0, m_instFrameCBs[i][frameIndex]->GpuAddress());
            list->SetGraphicsRootConstantBufferView(1, m_instBoneCBs [i][frameIndex]->GpuAddress());

            asset.mesh->BindVertexBuffer(list);
            asset.mesh->DrawAll(list, /*materialRootParam*/2, fallbackSrv);
        }
    }
}
