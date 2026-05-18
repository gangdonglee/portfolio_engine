// WIN32_LEAN_AND_MEAN / NOMINMAX 는 vcxproj 전역 PreprocessorDefinitions 에서 정의.
#include <Windows.h>
#include <d3d12.h>
#include <DirectXMath.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/Logger.h"
#include "platform/Input.h"
#include "platform/Window.h"
#include "render/AnimClip.h"
#include "render/Animator.h"
#include "render/Camera.h"
#include "render/CommandList.h"
#include "render/FbxLoader.h"
#include "render/FreeCamera.h"
#include "render/CommandQueue.h"
#include "render/ConstantBuffer.h"
#include "render/DepthStencilBuffer.h"
#include "render/Device.h"
#include "render/ImageLoader.h"
#include "render/IndexBuffer.h"
#include "render/Mesh.h"
#include "render/ObjLoader.h"
#include "render/Skeleton.h"
#include "render/PipelineState.h"
#include "render/RootSignature.h"
#include "render/RtvDescriptorHeap.h"
#include "render/ShaderCompiler.h"
#include "render/SrvDescriptorHeap.h"
#include "render/StructuredBuffer.h"
#include "render/SwapChain.h"
#include "render/Texture.h"
#include "render/VertexBuffer.h"
#include "scene/Scene.h"
#include "scene/SceneSerializer.h"

namespace
{
    // HLSL StructuredBuffer 의 element 와 1:1 — stride 정확히 일치해야 셰이더가 올바르게 인덱싱.
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

    // 본 팔레트 cbuffer — HLSL `bones[MAX_BONES=256]`.
    constexpr std::uint32_t kMaxBones = 256;
    struct BonePalette
    {
        DirectX::XMFLOAT4X4 bones[kMaxBones];
    };
    static_assert(sizeof(BonePalette) == kMaxBones * 64, "BonePalette 크기 깨짐");

    // 라이트 capacity — Scene 마다 라이트 수가 다르지만 StructuredBuffer 는 사전 할당 필요.
    // 한 씬에서 dir 16 + point 64 면 일반적인 인디 액션게임 수준 충분. 초과 시 throw.
    constexpr std::uint32_t kDirLightCapacity   = 16;
    constexpr std::uint32_t kPointLightCapacity = 64;

    // Scene 의 Transform → world XMMATRIX. rotation 은 쿼터니언(xyzw).
    DirectX::XMMATRIX ComposeWorld(const engine::scene::Transform& xform)
    {
        using namespace DirectX;
        const XMVECTOR s = XMVectorSet(xform.scale.x,    xform.scale.y,    xform.scale.z,    0.0f);
        const XMVECTOR r = XMVectorSet(xform.rotation.x, xform.rotation.y, xform.rotation.z, xform.rotation.w);
        const XMVECTOR t = XMVectorSet(xform.position.x, xform.position.y, xform.position.z, 1.0f);
        const XMVECTOR zero = XMVectorZero();
        return XMMatrixAffineTransformation(s, zero, r, t);
    }

    // 디폴트 씬 — assets/Scenes/sample.scene.json 이 없을 때 폴백.
    // Dragon 1개 + dir 1개. 라이트가 0개일 때도 셰이더가 ambient 만으로 렌더하는지 검증 목적의
    // 변형 씬은 sample.scene.json 으로 작성.
    engine::scene::Scene BuildDefaultScene()
    {
        engine::scene::Scene s;
        s.name = "default-fallback";
        s.ambient = { 0.15f, 0.15f, 0.18f };
        s.cameraStart.position = { 0.0f, 100.0f, -300.0f };
        s.cameraStart.target   = { 0.0f,  50.0f,    0.0f };
        s.cameraStart.fovYRad  = DirectX::XM_PIDIV4;

        engine::scene::MeshInstance dragon;
        dragon.name          = "Dragon";
        dragon.meshAssetPath = "Resources/FBX/Dragon.fbx";
        s.meshes.push_back(std::move(dragon));

        engine::scene::DirectionalLight sun;
        sun.name        = "Sun";
        sun.directionWS = { -0.5f, -1.0f, 0.4f };
        sun.color       = {  1.0f,  0.97f, 0.92f };
        sun.intensity   = 1.0f;
        s.dirLights.push_back(std::move(sun));

        return s;
    }

    // 로드된 메시 자산 캐시 — 같은 meshAssetPath 의 FBX/OBJ 를 한 번만 로드.
    struct LoadedAsset
    {
        std::unique_ptr<engine::render::Mesh>                  mesh;
        std::unique_ptr<engine::render::Skeleton>              skeleton;     // FBX 만
        std::vector<std::unique_ptr<engine::render::AnimClip>> clips;        // FBX 만
    };
}

int APIENTRY wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int)
{
    engine::core::LogInfo(L"[portfolio_engine] boot running\n");

    try
    {
        constexpr std::uint32_t kFrameCount = engine::render::SwapChain::kBackBufferCount;

        engine::platform::Window           window(1280, 720, L"portfolio_engine");
        engine::render::Device             device;
        engine::render::CommandQueue       commandQueue(device);
        engine::render::RtvDescriptorHeap  rtvHeap(device, engine::render::SwapChain::kBackBufferCount);
        engine::render::SwapChain          swapChain(device, commandQueue, window, rtvHeap);

        std::array<std::unique_ptr<engine::render::CommandList>, kFrameCount> cmdLists;
        for (std::uint32_t i = 0; i < kFrameCount; ++i)
        {
            cmdLists[i] = std::make_unique<engine::render::CommandList>(device);
        }

        engine::render::DepthStencilBuffer depthBuffer(
            device,
            static_cast<std::uint32_t>(window.Width()),
            static_cast<std::uint32_t>(window.Height()),
            DXGI_FORMAT_D32_FLOAT);

        const std::wstring shaderDir  = engine::render::ShaderCompiler::DefaultShaderDir();
        const std::wstring shaderPath = shaderDir + L"HelloTriangle.hlsl";

        const auto vsBlob = engine::render::ShaderCompiler::CompileFromFile(
            shaderPath.c_str(), "VSMain", engine::render::ShaderCompiler::Stage::Vertex);
        const auto psBlob = engine::render::ShaderCompiler::CompileFromFile(
            shaderPath.c_str(), "PSMain", engine::render::ShaderCompiler::Stage::Pixel);

        // RootSig: b0 + b1 + t0 table + t1 + t2.
        // 슬롯 인덱스: [0]=b0 frame, [1]=b1 bones, [2]=t0 material srv, [3]=t1 dir lights, [4]=t2 point lights.
        engine::render::RootSignature::Desc rsDesc{};
        rsDesc.cbvAtB0     = engine::render::RootSignature::Desc::CbvB0::All;
        rsDesc.cbvB1Vertex = true;
        rsDesc.srvT0Pixel  = true;
        rsDesc.srvT1Pixel  = true;
        rsDesc.srvT2Pixel  = true;
        engine::render::RootSignature rootSig(device, rsDesc);

        engine::render::PipelineState::Desc psoDesc{};
        psoDesc.vertexShader  = vsBlob.Get();
        psoDesc.pixelShader   = psBlob.Get();
        psoDesc.rootSignature = &rootSig;
        psoDesc.rtvFormat     = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoDesc.dsvFormat     = depthBuffer.Format();
        engine::render::PipelineState pso(device, psoDesc);

        // === SRV 힙 + 폴백 알베도 ===
        engine::render::SrvDescriptorHeap srvHeap(device, 64);

        const std::wstring texDir  = engine::render::fbx_loader::DefaultFbxDir() + L"..\\Texture\\";
        const std::wstring texPath = texDir + L"Leather.jpg";
        const engine::render::ImageData albedoImg =
            engine::render::image_loader::LoadImage(texPath.c_str());
        engine::render::Texture fallbackAlbedo(
            device, commandQueue, *cmdLists[0],
            albedoImg.pixels.data(), albedoImg.width, albedoImg.height);
        fallbackAlbedo.CreateSrv(device, srvHeap);

        // === Scene 로드 — assets/Scenes/sample.scene.json 시도, 없으면 디폴트 ===
        engine::scene::Scene scene;
        const std::string sceneRel = "assets/Scenes/sample.scene.json";
        if (std::filesystem::exists(sceneRel))
        {
            try
            {
                scene = engine::scene::LoadJson(sceneRel);
                engine::core::LogInfoA("[scene] loaded: ");
                engine::core::LogInfoA(sceneRel.c_str());
                engine::core::LogInfoA("\n");
            }
            catch (const std::exception& e)
            {
                engine::core::LogInfoA("[scene] load failed, fallback to default: ");
                engine::core::LogInfoA(e.what());
                engine::core::LogInfoA("\n");
                scene = BuildDefaultScene();
            }
        }
        else
        {
            engine::core::LogInfo(L"[scene] sample.scene.json 없음 - default Scene 사용\n");
            scene = BuildDefaultScene();
        }

        if (scene.dirLights.size() > kDirLightCapacity || scene.pointLights.size() > kPointLightCapacity)
        {
            throw std::runtime_error("Scene 의 라이트 개수가 capacity 초과");
        }

        // === 메시 자산 캐시 ===
        // meshAssetPath 가 동일하면 한 번만 로드. 인스턴스 N개여도 GPU 메모리는 1배.
        std::unordered_map<std::string, LoadedAsset> assetCache;
        const std::wstring fbxDir = engine::render::fbx_loader::DefaultFbxDir();

        for (const auto& inst : scene.meshes)
        {
            if (assetCache.contains(inst.meshAssetPath)) { continue; }

            LoadedAsset asset;
            const std::filesystem::path p{ inst.meshAssetPath };
            const auto ext = p.extension().string();

            if (ext == ".fbx" || ext == ".FBX")
            {
                // FBX 경로는 Resources/FBX/ 기준 파일명. fbx_loader 가 같은 폴더의 텍스처 자동 로드.
                const std::wstring full = std::filesystem::absolute(p).wstring();
                engine::render::fbx_loader::LoadedFbxModel loaded =
                    engine::render::fbx_loader::LoadFbx(
                        device, commandQueue, *cmdLists[0], srvHeap,
                        full.c_str(),
                        { 0.85f, 0.85f, 0.92f });
                asset.mesh     = std::move(loaded.mesh);
                asset.skeleton = std::move(loaded.skeleton);
                asset.clips    = std::move(loaded.clips);
            }
            else if (ext == ".obj" || ext == ".OBJ")
            {
                const std::wstring full = std::filesystem::absolute(p).wstring();
                asset.mesh = engine::render::obj_loader::LoadObj(
                    device, full.c_str(), { 1.0f, 1.0f, 1.0f });
            }
            else
            {
                throw std::runtime_error("Unsupported mesh asset extension: " + inst.meshAssetPath);
            }

            assetCache.emplace(inst.meshAssetPath, std::move(asset));
        }

        // === Animator — 첫 번째 FBX 인스턴스의 클립을 1..4 키로 활성화 ===
        // M1 데모 단순화: 모든 인스턴스에 동일 본 팔레트 적용 (sample scene 이 Dragon 1개라 무관).
        // M2~ 에서 인스턴스별 Animator 로 확장.
        engine::render::Skeleton*                         animSkeleton = nullptr;
        const std::vector<std::unique_ptr<engine::render::AnimClip>>* animClips = nullptr;
        for (const auto& inst : scene.meshes)
        {
            const auto& asset = assetCache.at(inst.meshAssetPath);
            if (asset.skeleton && !asset.clips.empty())
            {
                animSkeleton = asset.skeleton.get();
                animClips    = &asset.clips;
                break;
            }
        }
        std::unique_ptr<engine::render::Animator> animator;
        int currentClipIdx = -1;
        if (animSkeleton) { engine::core::LogInfo(L"[input] 0=T-pose, 1..4=clip select.\n"); }

        // === 카메라 ===
        constexpr float kNearPlane = 1.0f;
        constexpr float kFarPlane  = 5000.0f;
        engine::render::Camera camera;
        camera.SetPosition(scene.cameraStart.position);
        camera.SetTarget  (scene.cameraStart.target);
        camera.SetUp      ({ 0.0f, 1.0f, 0.0f });
        camera.SetPerspective(
            scene.cameraStart.fovYRad,
            static_cast<float>(window.Width()) / static_cast<float>(window.Height()),
            kNearPlane, kFarPlane);
        engine::render::FreeCamera freeCamera(camera);
        freeCamera.SetMoveSpeed(100.0f);

        // === 인스턴스마다 ConstantBuffer (FrameConstants + BonePalette) — N프레임 in-flight × M인스턴스 ===
        // 인스턴스 i 의 ConstantBuffer 는 매 프레임 갱신되고, 같은 슬롯이 N프레임 뒤 재사용 — N개의
        // 슬롯이 있어야 in-flight 안전.
        const std::uint32_t instanceCount = static_cast<std::uint32_t>(scene.meshes.size());

        std::vector<std::array<std::unique_ptr<engine::render::ConstantBuffer>, kFrameCount>> instFrameCBs(instanceCount);
        std::vector<std::array<std::unique_ptr<engine::render::ConstantBuffer>, kFrameCount>> instBoneCBs (instanceCount);
        for (std::uint32_t i = 0; i < instanceCount; ++i)
        {
            for (std::uint32_t f = 0; f < kFrameCount; ++f)
            {
                instFrameCBs[i][f] = std::make_unique<engine::render::ConstantBuffer>(
                    device, static_cast<engine::uint32>(sizeof(FrameConstants)));
                instBoneCBs[i][f]  = std::make_unique<engine::render::ConstantBuffer>(
                    device, static_cast<engine::uint32>(sizeof(BonePalette)));
            }
        }

        // === 라이트 StructuredBuffer — 프레임 슬롯당 인스턴스 ===
        // frame-shared 데이터. 매 프레임 시작 시 갱신, draw call 마다 같은 GPU 주소로 바인딩.
        std::array<std::unique_ptr<engine::render::StructuredBuffer>, kFrameCount> dirLightSBs;
        std::array<std::unique_ptr<engine::render::StructuredBuffer>, kFrameCount> pointLightSBs;
        for (std::uint32_t f = 0; f < kFrameCount; ++f)
        {
            dirLightSBs[f]   = std::make_unique<engine::render::StructuredBuffer>(
                device, kDirLightCapacity,   static_cast<engine::uint32>(sizeof(DirectionalLightGpu)));
            pointLightSBs[f] = std::make_unique<engine::render::StructuredBuffer>(
                device, kPointLightCapacity, static_cast<engine::uint32>(sizeof(PointLightGpu)));
        }

        // identity 팔레트.
        BonePalette identityPalette{};
        for (std::uint32_t i = 0; i < kMaxBones; ++i)
        {
            DirectX::XMStoreFloat4x4(&identityPalette.bones[i], DirectX::XMMatrixIdentity());
        }

        std::array<std::uint64_t, kFrameCount> frameFenceValues{};
        std::uint32_t frameIndex = 0;

        D3D12_VIEWPORT viewport{};
        viewport.Width    = static_cast<float>(window.Width());
        viewport.Height   = static_cast<float>(window.Height());
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;

        D3D12_RECT scissor{};
        scissor.right  = static_cast<LONG>(window.Width());
        scissor.bottom = static_cast<LONG>(window.Height());

        constexpr float kClearColor[4] = { 0.05f, 0.07f, 0.10f, 1.0f };

        const auto startTime = std::chrono::steady_clock::now();
        auto       prevFrame = startTime;

        while (window.IsOpen())
        {
            window.PumpMessages();
            if (!window.IsOpen()) { break; }

            if (window.ConsumeResize())
            {
                commandQueue.FlushGpu();
                for (auto& v : frameFenceValues) { v = 0; }

                const auto w = static_cast<std::uint32_t>(window.Width());
                const auto h = static_cast<std::uint32_t>(window.Height());
                swapChain.Resize(device, w, h);
                depthBuffer.Resize(device, w, h);

                viewport.Width  = static_cast<float>(w);
                viewport.Height = static_cast<float>(h);
                scissor.right   = static_cast<LONG>(w);
                scissor.bottom  = static_cast<LONG>(h);

                camera.SetPerspective(
                    scene.cameraStart.fovYRad,
                    static_cast<float>(w) / static_cast<float>(h),
                    kNearPlane, kFarPlane);
            }

            window.GetInput().BeginFrame();

            const auto now = std::chrono::steady_clock::now();
            const float dt = std::chrono::duration<float>(now - prevFrame).count();
            prevFrame = now;

            // 클립 전환 — animSkeleton 가 있을 때만.
            if (animSkeleton && animClips)
            {
                static bool prevDown[5]{};
                constexpr std::pair<std::uint32_t, int> kClipKeys[5] = {
                    { static_cast<std::uint32_t>('0'), -1 },
                    { static_cast<std::uint32_t>('1'),  0 },
                    { static_cast<std::uint32_t>('2'),  1 },
                    { static_cast<std::uint32_t>('3'),  2 },
                    { static_cast<std::uint32_t>('4'),  3 },
                };
                for (size_t i = 0; i < std::size(kClipKeys); ++i)
                {
                    const bool cur = window.GetInput().IsKeyDown(kClipKeys[i].first);
                    if (cur && !prevDown[i])
                    {
                        const int target = kClipKeys[i].second;
                        if (target < 0 || static_cast<size_t>(target) >= animClips->size())
                        {
                            if (currentClipIdx != -1) { animator.reset(); currentClipIdx = -1; }
                        }
                        else if (currentClipIdx != target)
                        {
                            animator = std::make_unique<engine::render::Animator>(
                                *animSkeleton, *(*animClips)[target]);
                            currentClipIdx = target;
                        }
                    }
                    prevDown[i] = cur;
                }
            }

            freeCamera.Update(window.GetInput(), dt);

            // === 라이트 데이터 → StructuredBuffer 업로드 (frame-shared) ===
            std::vector<DirectionalLightGpu> dirGpu;
            dirGpu.reserve(scene.dirLights.size());
            for (const auto& d : scene.dirLights)
            {
                DirectionalLightGpu g{};
                g.directionWS = d.directionWS;
                g.color       = d.color;
                g.intensity   = d.intensity;
                dirGpu.push_back(g);
            }
            std::vector<PointLightGpu> pointGpu;
            pointGpu.reserve(scene.pointLights.size());
            for (const auto& p : scene.pointLights)
            {
                PointLightGpu g{};
                g.positionWS = p.positionWS;
                g.color      = p.color;
                g.intensity  = p.intensity;
                g.range      = p.range;
                pointGpu.push_back(g);
            }

            // === N프레임 in-flight 대기 ===
            if (frameFenceValues[frameIndex] != 0)
            {
                commandQueue.WaitForFenceValue(frameFenceValues[frameIndex]);
            }

            dirLightSBs[frameIndex]->UpdateRange(
                dirGpu.empty()  ? nullptr : dirGpu.data(),
                static_cast<engine::uint32>(dirGpu.size()));
            pointLightSBs[frameIndex]->UpdateRange(
                pointGpu.empty() ? nullptr : pointGpu.data(),
                static_cast<engine::uint32>(pointGpu.size()));

            // === Animator 본 팔레트 갱신 — 단일 공유 ===
            BonePalette palette = identityPalette;
            if (animator)
            {
                animator->Update(dt);
                const auto& src = animator->Palette();
                const size_t n = (src.size() < kMaxBones) ? src.size() : kMaxBones;
                for (size_t i = 0; i < n; ++i) { palette.bones[i] = src[i]; }
            }

            engine::render::CommandList& cmdList = *cmdLists[frameIndex];
            cmdList.Reset();
            ID3D12GraphicsCommandList* list = cmdList.Native();

            ID3D12Resource* const backBuffer = swapChain.CurrentBackBuffer();

            D3D12_RESOURCE_BARRIER toRenderTarget{};
            toRenderTarget.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toRenderTarget.Transition.pResource   = backBuffer;
            toRenderTarget.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            toRenderTarget.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            toRenderTarget.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
            list->ResourceBarrier(1, &toRenderTarget);

            const D3D12_CPU_DESCRIPTOR_HANDLE rtv = swapChain.CurrentRtv();
            const D3D12_CPU_DESCRIPTOR_HANDLE dsv = depthBuffer.DsvHandle();
            list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
            list->ClearRenderTargetView(rtv, kClearColor, 0, nullptr);
            list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            list->SetGraphicsRootSignature(rootSig.Native());
            list->SetPipelineState(pso.Native());

            ID3D12DescriptorHeap* heaps[] = { srvHeap.Native() };
            list->SetDescriptorHeaps(1, heaps);

            // frame-shared SRV (라이트) — 모든 인스턴스에 동일.
            list->SetGraphicsRootShaderResourceView(3, dirLightSBs  [frameIndex]->GpuAddress());
            list->SetGraphicsRootShaderResourceView(4, pointLightSBs[frameIndex]->GpuAddress());

            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            // === 인스턴스 루프 ===
            using namespace DirectX;
            const XMMATRIX vp = camera.ViewProjection();
            for (std::uint32_t i = 0; i < instanceCount; ++i)
            {
                const auto& inst   = scene.meshes[i];
                const auto& asset  = assetCache.at(inst.meshAssetPath);

                const XMMATRIX world = ComposeWorld(inst.transform);
                const XMMATRIX mvp   = world * vp;

                FrameConstants cb{};
                XMStoreFloat4x4(&cb.mvp,   mvp);
                XMStoreFloat4x4(&cb.world, world);
                cb.cameraPosWS     = camera.Position();
                cb.ambient         = scene.ambient;
                cb.dirLightCount   = static_cast<std::uint32_t>(scene.dirLights.size());
                cb.pointLightCount = static_cast<std::uint32_t>(scene.pointLights.size());

                instFrameCBs[i][frameIndex]->Update(&cb, sizeof(cb));
                instBoneCBs [i][frameIndex]->Update(&palette, sizeof(palette));

                list->SetGraphicsRootConstantBufferView(0, instFrameCBs[i][frameIndex]->GpuAddress());
                list->SetGraphicsRootConstantBufferView(1, instBoneCBs [i][frameIndex]->GpuAddress());

                asset.mesh->BindVertexBuffer(list);
                asset.mesh->DrawAll(list, /*rootParamMaterialTable*/2, fallbackAlbedo.SrvGpuHandle());
            }

            D3D12_RESOURCE_BARRIER toPresent{};
            toPresent.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toPresent.Transition.pResource   = backBuffer;
            toPresent.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            toPresent.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
            list->ResourceBarrier(1, &toPresent);

            cmdList.Close();
            commandQueue.Execute(cmdList);
            swapChain.Present();

            frameFenceValues[frameIndex] = commandQueue.Signal();
            frameIndex = (frameIndex + 1) % kFrameCount;
        }

        commandQueue.FlushGpu();
    }
    catch (const std::exception& e)
    {
        engine::core::LogInfoA("[portfolio_engine] fatal: ");
        engine::core::LogInfoA(e.what());
        engine::core::LogInfoA("\n");
        return 1;
    }

    engine::core::LogInfo(L"[portfolio_engine] exit clean\n");
    return 0;
}
