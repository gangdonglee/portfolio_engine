#include "SceneRuntime.h"

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

        // 첫 번째 FBX 자산의 skeleton/clips — Animator 데모용.
        for (const auto& inst : m_scene.meshes)
        {
            const auto& asset = m_assetCache.at(inst.meshAssetPath);
            if (asset.skeleton && !asset.clips.empty())
            {
                m_animSkeleton = asset.skeleton.get();
                m_animClips    = &asset.clips;
                break;
            }
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
        if (m_animator) { m_animator->Update(dt); }
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

        // 본 팔레트 — Animator 있으면 갱신값, 없으면 identity.
        BonePalette palette = IdentityPalette();
        if (m_animator)
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

            const XMMATRIX world = ComposeWorld(inst.transform);
            const XMMATRIX mvp   = world * m_cachedViewProj;

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
