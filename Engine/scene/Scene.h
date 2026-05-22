#pragma once

#include "core/Types.h"

#include <DirectXMath.h>

#include <string>
#include <vector>

namespace engine::scene
{
    // 맵 에디터가 만들고 게임 런타임이 로드하는 씬 데이터 컨테이너.
    //
    // 디자인 원칙:
    //   - 순수 POD 컨테이너 (메서드 없음). 직렬화/렌더/편집은 외부 시스템 책임.
    //   - 라이트 개수는 가변 — std::vector 기반. GPU 측은 StructuredBuffer 로 매 프레임 재업로드.
    //   - 모든 트랜스폼은 world space (씬은 단일 평면 — 부모/자식 계층 도입 시 본 구조 확장).

    struct Transform
    {
        DirectX::XMFLOAT3 position{ 0.0f, 0.0f, 0.0f };
        DirectX::XMFLOAT4 rotation{ 0.0f, 0.0f, 0.0f, 1.0f }; // 쿼터니언 (xyzw, w=1 이면 회전 없음)
        DirectX::XMFLOAT3 scale   { 1.0f, 1.0f, 1.0f };
    };

    // 한 메시 자산(.obj/.fbx)이 씬에 배치된 한 인스턴스.
    // 같은 meshAssetPath 가 N번 등장 가능 — 런타임 측 Mesh 캐시가 중복 로드를 방지.
    //
    // animationClipPath: 선택적 — 메시와 별개의 FBX 에서 *애니메이션 클립만* 로드.
    //   비어있으면 메시 FBX 의 내장 클립 사용 (또는 T-pose).
    //   설정 시 메시의 스켈레톤 본 이름과 *문자열 매칭* 으로 키프레임 매핑.
    //   사용 예 (Mixamo without-skin): meshAssetPath="X Bot.fbx" + animationClipPath="Running.fbx".
    struct MeshInstance
    {
        std::string name;                    // 에디터 표시용 (Hierarchy/Inspector).
        std::string meshAssetPath;           // assets/ 또는 Resources/ 기준 상대 경로.
        std::string animationClipPath;       // 선택 — 별도 클립 FBX (M0 호환). 빈 문자열 = 미사용.
        std::string animatorControllerPath;  // 선택 — .animator.json 경로. 비어있지 않으면
                                             //   animationClipPath 보다 우선 (스테이트 머신 평가).
                                             //   M0: 데이터만 로드 + 로그. M1: 런타임 평가.
        // importTransform: 자산별 로드 시점 *baked-in* 보정 (FbxLoader 에서 mesh CP +
        //   inverse bind pose + animation keyframe 에 직접 곱셈). identity 면 무영향.
        //   Unity 의 model importer rotation/scale 옵션과 동일 패턴 — Mixamo 등 자산이
        //   다른 좌표계로 export 된 경우 여기에서 보정해야 인스턴스 transform 이 *게임
        //   런타임 자유도* 로 보존된다.
        Transform   importTransform;
        // 게임 런타임 인스턴스 transform — 캐릭터 위치/회전/스케일.
        Transform   transform;
    };

    // 무한 거리에서 평행하게 입사하는 라이트 (태양). 방향만 의미 — 위치 없음.
    struct DirectionalLight
    {
        std::string       name;
        DirectX::XMFLOAT3 directionWS{ 0.0f, -1.0f, 0.0f }; // 빛이 향하는 방향 (정규화 권장)
        DirectX::XMFLOAT3 color      { 1.0f,  1.0f, 1.0f };
        float             intensity   = 1.0f;
    };

    // 점 광원. range 외부는 falloff 0.
    struct PointLight
    {
        std::string       name;
        DirectX::XMFLOAT3 positionWS{ 0.0f, 0.0f, 0.0f };
        DirectX::XMFLOAT3 color     { 1.0f, 1.0f, 1.0f };
        float             intensity  = 1.0f;
        float             range      = 10.0f;
    };

    // 게임 런타임의 카메라 초기 상태. FreeCamera 가 이 값으로 SetPosition/SetTarget 후 시작.
    struct CameraStart
    {
        DirectX::XMFLOAT3 position{ 0.0f, 1.0f, -5.0f };
        DirectX::XMFLOAT3 target  { 0.0f, 0.0f,  0.0f };
        float             fovYRad  = 0.785398f; // π/4
    };

    struct Scene
    {
        std::string                   name;
        std::vector<MeshInstance>     meshes;
        std::vector<DirectionalLight> dirLights;
        std::vector<PointLight>       pointLights;
        DirectX::XMFLOAT3             ambient{ 0.15f, 0.15f, 0.18f };
        CameraStart                   cameraStart;
    };
}
