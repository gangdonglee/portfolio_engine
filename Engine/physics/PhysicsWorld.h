#pragma once

#include <DirectXMath.h>

#include <functional>
#include <memory>

namespace engine::physics
{
    // NVIDIA PhysX 5 래퍼 — PxScene + 정적 지형/장애물 콜라이더 + 캡슐 캐릭터 컨트롤러.
    //   PhysX 헤더는 무겁고 매크로가 많아 *pimpl* 로 격리 (이 헤더엔 PhysX 타입 노출 안 함).
    //
    //   좌표: 엔진 LH(DX) world 를 PhysX 에 1:1 매핑 (x,y,z 그대로). 단위 cm (1 unit ≈ 1 cm),
    //   gravity -980. 캐릭터 컨트롤러가 지형/벽/경사/계단을 collide-and-slide 로 처리.
    class PhysicsWorld
    {
    public:
        PhysicsWorld();
        ~PhysicsWorld();

        PhysicsWorld(const PhysicsWorld&)            = delete;
        PhysicsWorld& operator=(const PhysicsWorld&) = delete;

        // PxFoundation/Physics/Scene/ControllerManager 생성. 실패 시 false.
        bool Init();

        // 씬 시뮬레이션 한 스텝. dt 는 내부에서 고정 substep 으로 누적/분할.
        void Step(float dt);

        // === 정적 콜라이더 ===
        // 지형 heightfield — heightAt(x,z) 를 cols×rows 그리드에 샘플. world 중심이 원점,
        //   X∈[-worldW/2,+worldW/2], Z∈[-worldD/2,+worldD/2]. (지형 메시와 동일 범위/해상도 권장.)
        void AddTerrain(const std::function<float(float, float)>& heightAt,
                        float worldWidth, float worldDepth, int cols, int rows);
        // 정적 박스 (벽/장애물) — world center + halfExtents.
        void AddStaticBox(const DirectX::XMFLOAT3& center, const DirectX::XMFLOAT3& halfExtents);

        // === 캐릭터 컨트롤러 (캡슐) ===
        // footPos: 캡슐 바닥(발) world 위치. radius/halfHeight: 캡슐 반경/직선부 절반높이.
        void CreateCharacter(const DirectX::XMFLOAT3& footPos, float radius, float halfHeight);
        bool HasCharacter() const noexcept;
        // disp(world) 만큼 이동 시도 → 충돌 후 실제 새 발 위치 반환. outGrounded: 이번 이동에서
        //   아래쪽(바닥) 충돌 여부. dt 는 컨트롤러 내부 보정용.
        DirectX::XMFLOAT3 MoveCharacter(const DirectX::XMFLOAT3& disp, float dt, bool& outGrounded);
        DirectX::XMFLOAT3 CharacterFootPos() const;
        void              SetCharacterFootPos(const DirectX::XMFLOAT3& footPos);   // 텔레포트

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
