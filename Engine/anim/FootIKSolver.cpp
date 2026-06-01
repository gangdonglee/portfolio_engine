#include "anim/FootIKSolver.h"

#include "anim/AnimatorRuntime.h"
#include "render/Skeleton.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <queue>
#include <unordered_map>

namespace engine::anim
{
    namespace
    {
        using DirectX::XMFLOAT3;
        using DirectX::XMFLOAT4X4;
        using DirectX::XMMATRIX;
        using DirectX::XMVECTOR;
        using DirectX::XMVectorSet;
        using DirectX::XMVectorGetX;
        using DirectX::XMVectorGetY;
        using DirectX::XMVectorGetZ;
        using DirectX::XMVector3Length;
        using DirectX::XMVector3Normalize;
        using DirectX::XMVector3Cross;
        using DirectX::XMMatrixRotationAxis;
        using DirectX::XMVector3TransformCoord;
        using DirectX::XMLoadFloat4x4;
        using DirectX::XMStoreFloat4x4;
        using DirectX::XMMatrixInverse;
        using DirectX::XMMatrixMultiply;
        using DirectX::XMMatrixIdentity;

        std::wstring AsciiToLowerW(const std::string& s)
        {
            std::wstring w;
            w.reserve(s.size());
            for (char c : s)
            {
                w.push_back(static_cast<wchar_t>(std::towlower(static_cast<wchar_t>(c))));
            }
            return w;
        }
        std::wstring ToLowerW(const std::wstring& s)
        {
            std::wstring r = s;
            for (auto& c : r) { c = std::towlower(c); }
            return r;
        }

        engine::int32 FindBoneCaseInsensitive(const engine::render::Skeleton& skel,
                                              const std::string&              name)
        {
            const std::wstring needle = AsciiToLowerW(name);
            if (needle.empty()) { return -1; }
            for (size_t i = 0; i < skel.BoneCount(); ++i)
            {
                const std::wstring hay = ToLowerW(skel.Bones()[i].name);
                if (hay.find(needle) != std::wstring::npos)
                {
                    return static_cast<engine::int32>(i);
                }
            }
            return -1;
        }

        // 매트릭스의 translation column 추출 (model-space 본 위치).
        XMVECTOR GetTranslation(const XMFLOAT4X4& m)
        {
            return XMVectorSet(m.m[0][3], m.m[1][3], m.m[2][3], 1.0f);
        }

        // 본 위치 mesh-local → world.
        XMVECTOR MeshLocalToWorld(XMVECTOR meshLocalPos, const XMMATRIX& meshWorld)
        {
            return XMVector3TransformCoord(meshLocalPos, meshWorld);
        }

        // 본 global transform 의 translation 부분만 교체. rotation/scale 유지.
        void SetTranslation(XMFLOAT4X4& m, const XMVECTOR& newPos)
        {
            m.m[0][3] = XMVectorGetX(newPos);
            m.m[1][3] = XMVectorGetY(newPos);
            m.m[2][3] = XMVectorGetZ(newPos);
        }

        // 두 방향 vec a → b 사이 최단 회전 quaternion. 둘 다 non-zero 라고 가정.
        XMVECTOR QuaternionFromTo(XMVECTOR a, XMVECTOR b)
        {
            using namespace DirectX;
            a = XMVector3Normalize(a);
            b = XMVector3Normalize(b);
            float dot = XMVectorGetX(XMVector3Dot(a, b));
            if (dot > 0.99999f)  { return XMQuaternionIdentity(); }
            if (dot < -0.99999f)
            {
                // 정반대 — 임의 perpendicular axis 로 180°.
                XMVECTOR axis = XMVector3Cross(a, XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f));
                if (XMVectorGetX(XMVector3LengthSq(axis)) < 1e-6f)
                {
                    axis = XMVector3Cross(a, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
                }
                axis = XMVector3Normalize(axis);
                return XMQuaternionRotationAxis(axis, 3.14159265358979f);
            }
            const XMVECTOR axis = XMVector3Normalize(XMVector3Cross(a, b));
            const float    ang  = std::acos(dot);
            return XMQuaternionRotationAxis(axis, ang);
        }

        // matrix → rotation-only matrix (translation 제거).
        XMMATRIX RotationOnly(const XMFLOAT4X4& m)
        {
            XMFLOAT4X4 r = m;
            r.m[0][3] = 0.0f; r.m[1][3] = 0.0f; r.m[2][3] = 0.0f;
            r.m[3][0] = 0.0f; r.m[3][1] = 0.0f; r.m[3][2] = 0.0f; r.m[3][3] = 1.0f;
            return XMLoadFloat4x4(&r);
        }

        // bone matrix 의 rotation 을 quaternion R 만큼 추가 회전 — pivot 위치는 그대로.
        XMFLOAT4X4 RotateRotationBy(const XMFLOAT4X4& bone, const XMVECTOR& addRotQuat)
        {
            using namespace DirectX;
            const XMVECTOR oldPos = XMVectorSet(bone.m[0][3], bone.m[1][3], bone.m[2][3], 1.0f);
            const XMMATRIX oldRotM = RotationOnly(bone);
            const XMMATRIX addRotM = XMMatrixRotationQuaternion(addRotQuat);
            const XMMATRIX newRotM = XMMatrixMultiply(oldRotM, addRotM);   // row-vector convention.
            XMFLOAT4X4 result;
            XMStoreFloat4x4(&result, newRotM);
            result.m[0][3] = XMVectorGetX(oldPos);
            result.m[1][3] = XMVectorGetY(oldPos);
            result.m[2][3] = XMVectorGetZ(oldPos);
            return result;
        }

        // child 의 *local transform* 보존하면서 새 parent 의 global 기준으로 child global 재계산.
        //   oldChild = oldParent * local  →  local = inverse(oldParent) * oldChild
        //   newChild = newParent * local
        XMFLOAT4X4 RecomputeChildGlobal(const XMFLOAT4X4& oldParent,
                                        const XMFLOAT4X4& newParent,
                                        const XMFLOAT4X4& oldChild)
        {
            using namespace DirectX;
            const XMMATRIX oldP   = XMLoadFloat4x4(&oldParent);
            const XMMATRIX newP   = XMLoadFloat4x4(&newParent);
            const XMMATRIX oldC   = XMLoadFloat4x4(&oldChild);
            XMVECTOR det;
            const XMMATRIX invOld = XMMatrixInverse(&det, oldP);
            const XMMATRIX local  = XMMatrixMultiply(oldC, invOld);   // row-vec: local = oldC * inv(oldP)
            const XMMATRIX newC   = XMMatrixMultiply(local, newP);
            XMFLOAT4X4 r;
            XMStoreFloat4x4(&r, newC);
            return r;
        }

        // Hip → Knee → Ankle 의 새 위치 계산 (Two-bone IK).
        //   bendHint: 무릎이 굽혀지는 방향 힌트 (보통 character forward). 평면 결정.
        //   결과: newKnee, newAnkle 의 model-space 위치.
        struct TwoBoneSolution
        {
            XMVECTOR newKnee;
            XMVECTOR newAnkle;
        };
        TwoBoneSolution SolveTwoBoneIK(
            const XMVECTOR& hip,
            const XMVECTOR& knee,
            const XMVECTOR& ankle,
            const XMVECTOR& targetAnkle,
            const XMVECTOR& bendHint,
            float           maxExtensionRatio)
        {
            using DirectX::XMVectorAdd;
            using DirectX::XMVectorSubtract;
            using DirectX::XMVectorScale;
            using DirectX::XMVector3Dot;

            const float L1 = XMVectorGetX(XMVector3Length(XMVectorSubtract(knee, hip)));
            const float L2 = XMVectorGetX(XMVector3Length(XMVectorSubtract(ankle, knee)));
            const float maxL = (L1 + L2) * maxExtensionRatio;

            // hip → target 방향과 거리. 너무 멀면 clamp (다리 늘어남 방지).
            XMVECTOR dHipTarget = XMVectorSubtract(targetAnkle, hip);
            float    L          = XMVectorGetX(XMVector3Length(dHipTarget));
            if (L < 1e-4f)
            {
                return { knee, ankle };
            }
            if (L > maxL)
            {
                XMVECTOR dir = XMVector3Normalize(dHipTarget);
                dHipTarget = XMVectorScale(dir, maxL);
                L          = maxL;
            }

            // 새 ankle 위치.
            const XMVECTOR newAnkle = XMVectorAdd(hip, dHipTarget);

            // 코사인 법칙 — Hip 각도 (Hip 정점, knee 와 ankle 사이 각).
            //   L1² + L² - L2² = 2 * L1 * L * cos(hipAngle) → cos = (L1² + L² - L2²) / (2 L1 L)
            float cosHip = (L1 * L1 + L * L - L2 * L2) / (2.0f * L1 * L);
            cosHip       = std::clamp(cosHip, -1.0f, 1.0f);
            const float hipAngle = std::acos(cosHip);

            // bend plane normal — (hip→target) × bendHint. 양 발 동일 평면 가정.
            XMVECTOR dirToTarget = XMVector3Normalize(dHipTarget);
            XMVECTOR bend        = XMVector3Normalize(bendHint);
            float dot = XMVectorGetX(XMVector3Dot(dirToTarget, bend));
            if (std::abs(dot) > 0.999f)
            {
                bend = XMVector3Normalize(XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f));
                dot  = XMVectorGetX(XMVector3Dot(dirToTarget, bend));
                if (std::abs(dot) > 0.999f)
                {
                    bend = XMVector3Normalize(XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f));
                }
            }
            XMVECTOR planeN = XMVector3Normalize(XMVector3Cross(dirToTarget, bend));

            // bend hint 가 양의 회전 결과 쪽에 있는지 검증 — 아니면 axis 반전.
            XMVECTOR rotAxis    = planeN;
            XMMATRIX testRot    = XMMatrixRotationAxis(rotAxis, 0.1f);
            XMVECTOR testKneeDir= XMVector3TransformCoord(dirToTarget, testRot);
            // bend 의 dirToTarget 평행 성분 제거 → 평면 내 성분.
            XMVECTOR bendComp   = XMVectorSubtract(bend,
                                    XMVectorScale(dirToTarget, XMVectorGetX(XMVector3Dot(bend, dirToTarget))));
            const float sign    = (XMVectorGetX(XMVector3Dot(
                                    XMVectorSubtract(testKneeDir, dirToTarget), bendComp)) > 0.0f) ? 1.0f : -1.0f;
            const XMMATRIX rotKnee = XMMatrixRotationAxis(rotAxis, sign * hipAngle);
            const XMVECTOR kneeDir = XMVector3TransformCoord(dirToTarget, rotKnee);
            const XMVECTOR newKnee = XMVectorAdd(hip, XMVectorScale(kneeDir, L1));

            return { newKnee, newAnkle };
        }
    }

    FootIKBoneIndices FindFootIKBones(const engine::render::Skeleton& skel,
                                       const FootIKConfig&             cfg)
    {
        FootIKBoneIndices idx;
        idx.leftHip    = FindBoneCaseInsensitive(skel, cfg.leftHipBone);
        idx.leftKnee   = FindBoneCaseInsensitive(skel, cfg.leftKneeBone);
        idx.leftAnkle  = FindBoneCaseInsensitive(skel, cfg.leftAnkleBone);
        idx.rightHip   = FindBoneCaseInsensitive(skel, cfg.rightHipBone);
        idx.rightKnee  = FindBoneCaseInsensitive(skel, cfg.rightKneeBone);
        idx.rightAnkle = FindBoneCaseInsensitive(skel, cfg.rightAnkleBone);
        return idx;
    }

    std::vector<engine::int32> CollectDescendants(const engine::render::Skeleton& skel,
                                                    engine::int32                   boneIdx)
    {
        std::vector<engine::int32> result;
        if (boneIdx < 0) { return result; }

        // BFS — top-down order. parentIndex 만 있으므로 child list 매번 검색.
        std::queue<engine::int32> q;
        q.push(boneIdx);
        while (!q.empty())
        {
            const engine::int32 cur = q.front();
            q.pop();
            for (size_t i = 0; i < skel.BoneCount(); ++i)
            {
                if (skel.Bones()[i].parentIndex == cur)
                {
                    const auto childIdx = static_cast<engine::int32>(i);
                    result.push_back(childIdx);
                    q.push(childIdx);
                }
            }
        }
        return result;
    }

    // 한 발 chain 처리 — IK 솔브 후 boneGlobal 갱신 + 자손 본 FK 재계산.
    static void ProcessOneFootChain(
        AnimatorRuntime&                                anim,
        const engine::render::Skeleton&                 skel,
        engine::int32                                   hipIdx,
        engine::int32                                   kneeIdx,
        engine::int32                                   ankleIdx,
        const FootIKConfig&                             cfg,
        const XMMATRIX&                                 meshWorld,
        const XMMATRIX&                                 meshWorldInv,
        const std::function<float(float, float)>&       groundSample,
        bool&                                           outValid,
        XMFLOAT3&                                       outAnimWorld,
        XMFLOAT3&                                       outTargetWorld)
    {
        outValid = false;
        if (hipIdx < 0 || kneeIdx < 0 || ankleIdx < 0) { return; }
        const auto& boneGlobal = anim.BoneGlobal();
        if (static_cast<size_t>(ankleIdx) >= boneGlobal.size()) { return; }

        // 현재 본 위치 (model-space).
        const XMVECTOR hipMS   = GetTranslation(boneGlobal[hipIdx]);
        const XMVECTOR kneeMS  = GetTranslation(boneGlobal[kneeIdx]);
        const XMVECTOR ankleMS = GetTranslation(boneGlobal[ankleIdx]);

        // World 좌표로 변환 — ankle 위치 raycast 대상.
        const XMVECTOR ankleWS = MeshLocalToWorld(ankleMS, meshWorld);
        const float    ankleWX = XMVectorGetX(ankleWS);
        const float    ankleWY = XMVectorGetY(ankleWS);
        const float    ankleWZ = XMVectorGetZ(ankleWS);

        outAnimWorld = XMFLOAT3{ ankleWX, ankleWY, ankleWZ };

        // 지면 Y 샘플 — caller-supplied callback. 없으면 0.
        const float groundY = groundSample ? groundSample(ankleWX, ankleWZ) : 0.0f;
        const float targetWY = std::max(ankleWY, groundY + cfg.ankleHeightOffset);

        outTargetWorld = XMFLOAT3{ ankleWX, targetWY, ankleWZ };

        // 발이 이미 지면보다 위에 있으면 IK skip — animation 그대로.
        if (targetWY <= ankleWY + 1e-3f)
        {
            outValid = true;
            return;
        }

        // weight 보간 — 0 일 땐 변화 없음.
        const float wy_blend = ankleWY + (targetWY - ankleWY) * cfg.weight;
        const XMVECTOR targetWS = XMVectorSet(ankleWX, wy_blend, ankleWZ, 1.0f);
        const XMVECTOR targetMS = XMVector3TransformCoord(targetWS, meshWorldInv);

        // bend hint — 무릎의 현재 위치 방향으로 굽히기.
        const XMVECTOR hipToKneeOrig = DirectX::XMVectorSubtract(kneeMS, hipMS);
        const XMVECTOR bendHint      = hipToKneeOrig;

        const TwoBoneSolution sol = SolveTwoBoneIK(
            hipMS, kneeMS, ankleMS, targetMS, bendHint, cfg.maxLegExtension);

        // === Rotation 재구성 — hip, knee 의 global rotation 을 새 방향에 맞춤. ===
        using DirectX::XMVectorSubtract;
        using DirectX::XMQuaternionInverse;

        // Hip: oldHipToKnee → newHipToKnee 방향으로 align.
        const XMVECTOR oldHipToKnee = XMVectorSubtract(kneeMS, hipMS);
        const XMVECTOR newHipToKnee = XMVectorSubtract(sol.newKnee, hipMS);
        const XMVECTOR rHipAlign    = QuaternionFromTo(oldHipToKnee, newHipToKnee);

        // Knee: oldKneeToAnkle → newKneeToAnkle (new knee 기준!).
        const XMVECTOR oldKneeToAnkle = XMVectorSubtract(ankleMS,    kneeMS);
        const XMVECTOR newKneeToAnkle = XMVectorSubtract(sol.newAnkle, sol.newKnee);
        const XMVECTOR rKneeAlign     = QuaternionFromTo(oldKneeToAnkle, newKneeToAnkle);

        // 새 global matrices.
        const XMFLOAT4X4 oldHipM   = boneGlobal[hipIdx];
        const XMFLOAT4X4 oldKneeM  = boneGlobal[kneeIdx];
        const XMFLOAT4X4 oldAnkleM = boneGlobal[ankleIdx];

        // Hip: translation 유지 + rotation × rHipAlign.
        XMFLOAT4X4 newHipM = RotateRotationBy(oldHipM, rHipAlign);

        // Knee: translation = newKnee, rotation × rKneeAlign.
        XMFLOAT4X4 newKneeM = RotateRotationBy(oldKneeM, rKneeAlign);
        SetTranslation(newKneeM, sol.newKnee);

        // Ankle: translation = newAnkle, rotation 유지 (animation 그대로).
        XMFLOAT4X4 newAnkleM = oldAnkleM;
        SetTranslation(newAnkleM, sol.newAnkle);

        anim.SetBoneGlobal(static_cast<size_t>(hipIdx),   newHipM);
        anim.SetBoneGlobal(static_cast<size_t>(kneeIdx),  newKneeM);
        anim.SetBoneGlobal(static_cast<size_t>(ankleIdx), newAnkleM);

        // === 자손 본 (Toe 등) FK 재계산 — local transform 보존하면서 새 parent global 기준. ===
        // BFS 순서 (parent 가 child 보다 먼저). cache 에 새 global 보관.
        std::unordered_map<engine::int32, XMFLOAT4X4> newGlobals;
        newGlobals[hipIdx]   = newHipM;
        newGlobals[kneeIdx]  = newKneeM;
        newGlobals[ankleIdx] = newAnkleM;

        const std::vector<engine::int32> descs = CollectDescendants(skel, ankleIdx);
        for (engine::int32 d : descs)
        {
            if (static_cast<size_t>(d) >= boneGlobal.size()) { continue; }
            const engine::int32 parent = skel.Bones()[static_cast<size_t>(d)].parentIndex;
            if (parent < 0) { continue; }

            const XMFLOAT4X4& oldParent = boneGlobal[static_cast<size_t>(parent)];
            const auto itNewP = newGlobals.find(parent);
            const XMFLOAT4X4& newParent = (itNewP != newGlobals.end())
                ? itNewP->second
                : boneGlobal[static_cast<size_t>(parent)];

            const XMFLOAT4X4& oldChild = boneGlobal[static_cast<size_t>(d)];
            const XMFLOAT4X4  newChild = RecomputeChildGlobal(oldParent, newParent, oldChild);

            newGlobals[d] = newChild;
            anim.SetBoneGlobal(static_cast<size_t>(d), newChild);
        }

        outValid = true;
    }

    void ApplyFootIK(AnimatorRuntime&                                 anim,
                     const engine::render::Skeleton&                  skel,
                     const FootIKBoneIndices&                         bones,
                     const FootIKConfig&                              cfg,
                     const XMMATRIX&                                  meshWorld,
                     const XMMATRIX&                                  meshWorldInv,
                     const std::function<float(float, float)>&        groundSample,
                     FootIKDebug&                                     outDebug)
    {
        outDebug = FootIKDebug{};
        if (cfg.weight <= 0.001f) { return; }

        if (bones.LeftValid())
        {
            ProcessOneFootChain(
                anim, skel,
                bones.leftHip, bones.leftKnee, bones.leftAnkle,
                cfg, meshWorld, meshWorldInv, groundSample,
                outDebug.leftValid, outDebug.leftAnkleAnimWorld, outDebug.leftAnkleTargetWorld);
        }
        if (bones.RightValid())
        {
            ProcessOneFootChain(
                anim, skel,
                bones.rightHip, bones.rightKnee, bones.rightAnkle,
                cfg, meshWorld, meshWorldInv, groundSample,
                outDebug.rightValid, outDebug.rightAnkleAnimWorld, outDebug.rightAnkleTargetWorld);
        }
    }
}
