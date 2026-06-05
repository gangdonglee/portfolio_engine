#include "physics/PhysicsWorld.h"

#include <PxPhysicsAPI.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace physx;

namespace engine::physics
{
    namespace
    {
        PxDefaultAllocator     g_allocator;
        PxDefaultErrorCallback g_errorCallback;
    }

    struct PhysicsWorld::Impl
    {
        PxFoundation*           foundation = nullptr;
        PxPhysics*              physics    = nullptr;
        PxDefaultCpuDispatcher* dispatcher = nullptr;
        PxScene*                scene      = nullptr;
        PxMaterial*             material   = nullptr;
        PxControllerManager*    ctlMgr     = nullptr;
        PxController*           controller = nullptr;
        float                   footToCenterY = 0.0f;   // 발 → 캡슐 중심 Y (= radius + halfHeight)
        float                   accumulator   = 0.0f;
    };

    PhysicsWorld::PhysicsWorld() : m_impl(std::make_unique<Impl>()) {}

    PhysicsWorld::~PhysicsWorld()
    {
        if (!m_impl) { return; }
        Impl& I = *m_impl;
        if (I.controller) { I.controller->release(); }
        if (I.ctlMgr)     { I.ctlMgr->release(); }
        if (I.scene)      { I.scene->release(); }
        if (I.dispatcher) { I.dispatcher->release(); }
        if (I.material)   { I.material->release(); }
        if (I.physics)    { I.physics->release(); }
        if (I.foundation) { I.foundation->release(); }
    }

    bool PhysicsWorld::Init()
    {
        Impl& I = *m_impl;

        I.foundation = PxCreateFoundation(PX_PHYSICS_VERSION, g_allocator, g_errorCallback);
        if (!I.foundation) { return false; }

        PxTolerancesScale scale;
        scale.length = 100.0f;   // cm 단위 — 일반 물체 길이 ~100cm
        scale.speed  = 980.0f;   // gravity 스케일
        I.physics = PxCreatePhysics(PX_PHYSICS_VERSION, *I.foundation, scale, false, nullptr);
        if (!I.physics) { return false; }

        PxSceneDesc sceneDesc(I.physics->getTolerancesScale());
        sceneDesc.gravity      = PxVec3(0.0f, -980.0f, 0.0f);
        I.dispatcher           = PxDefaultCpuDispatcherCreate(2);
        sceneDesc.cpuDispatcher = I.dispatcher;
        sceneDesc.filterShader  = PxDefaultSimulationFilterShader;
        I.scene = I.physics->createScene(sceneDesc);
        if (!I.scene) { return false; }

        I.material = I.physics->createMaterial(0.5f, 0.5f, 0.05f);   // staticFric, dynFric, restitution
        I.ctlMgr   = PxCreateControllerManager(*I.scene);
        return I.ctlMgr != nullptr;
    }

    void PhysicsWorld::Step(float dt)
    {
        Impl& I = *m_impl;
        if (!I.scene) { return; }
        // 고정 substep 60Hz 누적 분할 (가변 dt 안정화).
        I.accumulator += std::clamp(dt, 0.0f, 0.25f);
        constexpr float h = 1.0f / 60.0f;
        int steps = 0;
        while (I.accumulator >= h && steps < 5)
        {
            I.scene->simulate(h);
            I.scene->fetchResults(true);
            I.accumulator -= h;
            ++steps;
        }
    }

    void PhysicsWorld::AddTerrain(const std::function<float(float, float)>& heightAt,
                                  float worldWidth, float worldDepth, int cols, int rows)
    {
        Impl& I = *m_impl;
        if (!I.physics || !I.scene || cols < 2 || rows < 2 || !heightAt) { return; }

        const auto vertX = [&](int c) { return -worldWidth * 0.5f + worldWidth * static_cast<float>(c) / static_cast<float>(cols - 1); };
        const auto vertZ = [&](int r) { return -worldDepth * 0.5f + worldDepth * static_cast<float>(r) / static_cast<float>(rows - 1); };

        // 높이 → int16 양자화 scale (데이터 범위 기준).
        float maxAbs = 1.0f;
        for (int r = 0; r < rows; ++r)
            for (int c = 0; c < cols; ++c)
                maxAbs = std::max(maxAbs, std::abs(heightAt(vertX(c), vertZ(r))));
        const float heightScale = maxAbs / 30000.0f;

        // PhysX heightfield: nbRows×nbColumns. row 축=local X, column 축=local Z, height=local Y.
        //   row=내 col(x), column=내 row(z) 로 매핑 → world (vertX(c), h, vertZ(r)) 일치.
        const int nbRows = cols, nbCols = rows;
        std::vector<PxHeightFieldSample> samples(static_cast<size_t>(nbRows) * nbCols);
        for (int c = 0; c < cols; ++c)
            for (int r = 0; r < rows; ++r)
            {
                PxHeightFieldSample& s = samples[static_cast<size_t>(c) * nbCols + r];
                s.height = static_cast<PxI16>(std::clamp(heightAt(vertX(c), vertZ(r)) / heightScale, -32000.0f, 32000.0f));
                s.materialIndex0 = 0;
                s.materialIndex1 = 0;
                s.clearTessFlag();
            }

        PxHeightFieldDesc hfDesc;
        hfDesc.format        = PxHeightFieldFormat::eS16_TM;
        hfDesc.nbRows        = static_cast<PxU32>(nbRows);
        hfDesc.nbColumns     = static_cast<PxU32>(nbCols);
        hfDesc.samples.data   = samples.data();
        hfDesc.samples.stride = sizeof(PxHeightFieldSample);

        PxHeightField* hf = PxCreateHeightField(hfDesc, I.physics->getPhysicsInsertionCallback());
        if (!hf) { return; }

        const float rowScale    = worldWidth / static_cast<float>(cols - 1);   // row(X) spacing
        const float columnScale = worldDepth / static_cast<float>(rows - 1);   // column(Z) spacing
        PxHeightFieldGeometry geom(hf, PxMeshGeometryFlags(), heightScale, rowScale, columnScale);

        // heightfield local 원점 = sample[0][0] 코너 → world 중심 정렬 위해 -worldW/2,-worldD/2 평행이동.
        PxRigidStatic* actor = I.physics->createRigidStatic(PxTransform(PxVec3(-worldWidth * 0.5f, 0.0f, -worldDepth * 0.5f)));
        if (!actor) { hf->release(); return; }
        PxRigidActorExt::createExclusiveShape(*actor, geom, *I.material);
        I.scene->addActor(*actor);
    }

    void PhysicsWorld::AddStaticBox(const DirectX::XMFLOAT3& center, const DirectX::XMFLOAT3& halfExtents)
    {
        Impl& I = *m_impl;
        if (!I.physics || !I.scene) { return; }
        PxRigidStatic* actor = I.physics->createRigidStatic(PxTransform(PxVec3(center.x, center.y, center.z)));
        if (!actor) { return; }
        PxRigidActorExt::createExclusiveShape(
            *actor, PxBoxGeometry(halfExtents.x, halfExtents.y, halfExtents.z), *I.material);
        I.scene->addActor(*actor);
    }

    void PhysicsWorld::CreateCharacter(const DirectX::XMFLOAT3& footPos, float radius, float halfHeight)
    {
        Impl& I = *m_impl;
        if (!I.ctlMgr || I.controller) { return; }

        I.footToCenterY = radius + halfHeight;
        PxCapsuleControllerDesc desc;
        desc.radius        = radius;
        desc.height        = 2.0f * halfHeight;   // 직선부 높이 (총 높이 = height + 2*radius)
        desc.position      = PxExtendedVec3(footPos.x, footPos.y + I.footToCenterY, footPos.z);
        desc.upDirection   = PxVec3(0.0f, 1.0f, 0.0f);
        desc.material      = I.material;
        desc.slopeLimit    = std::cos(0.785398f);   // 45° — 이보다 가파르면 못 올라감
        desc.stepOffset    = 30.0f;                 // 30cm 계단 자동 오름
        desc.contactOffset = 2.0f;                  // 2cm
        desc.climbingMode  = PxCapsuleClimbingMode::eCONSTRAINED;
        I.controller = I.ctlMgr->createController(desc);
    }

    bool PhysicsWorld::HasCharacter() const noexcept
    {
        return m_impl && m_impl->controller != nullptr;
    }

    DirectX::XMFLOAT3 PhysicsWorld::MoveCharacter(const DirectX::XMFLOAT3& disp, float dt, bool& outGrounded)
    {
        Impl& I = *m_impl;
        outGrounded = false;
        if (!I.controller) { return CharacterFootPos(); }
        const PxControllerCollisionFlags flags = I.controller->move(
            PxVec3(disp.x, disp.y, disp.z), 0.001f, std::clamp(dt, 0.0f, 0.1f), PxControllerFilters());
        outGrounded = flags.isSet(PxControllerCollisionFlag::eCOLLISION_DOWN);
        return CharacterFootPos();
    }

    DirectX::XMFLOAT3 PhysicsWorld::CharacterFootPos() const
    {
        const Impl& I = *m_impl;
        if (!I.controller) { return { 0.0f, 0.0f, 0.0f }; }
        const PxExtendedVec3 p = I.controller->getPosition();
        return { static_cast<float>(p.x),
                 static_cast<float>(p.y) - I.footToCenterY,
                 static_cast<float>(p.z) };
    }

    void PhysicsWorld::SetCharacterFootPos(const DirectX::XMFLOAT3& footPos)
    {
        Impl& I = *m_impl;
        if (!I.controller) { return; }
        I.controller->setPosition(PxExtendedVec3(footPos.x, footPos.y + I.footToCenterY, footPos.z));
    }
}
