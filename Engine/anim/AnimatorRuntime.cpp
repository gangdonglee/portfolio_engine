#include "anim/AnimatorRuntime.h"

#include "render/AnimClip.h"
#include "render/Skeleton.h"

#include <algorithm>
#include <cmath>

namespace engine::anim
{
    using DirectX::XMFLOAT4X4;
    using DirectX::XMMATRIX;
    using DirectX::XMMatrixIdentity;
    using DirectX::XMMatrixMultiply;
    using DirectX::XMLoadFloat4x4;
    using DirectX::XMStoreFloat4x4;
    using DirectX::XMVectorLerp;

    namespace
    {
        // 한 본의 시간 → transform — element-wise lerp (Animator 와 동일 정책).
        XMMATRIX EvaluateBoneTransform(const engine::render::AnimClip* clip,
                                       size_t                          boneIdx,
                                       double                          timeSec)
        {
            if (clip == nullptr) { return XMMatrixIdentity(); }
            if (boneIdx >= clip->bonesKeyFrames.size()) { return XMMatrixIdentity(); }
            const auto& kfs = clip->bonesKeyFrames[boneIdx];
            if (kfs.empty()) { return XMMatrixIdentity(); }

            const double duration = clip->DurationSec();
            if (duration <= 0.0) { return XMLoadFloat4x4(&kfs[0].transform); }

            double normalized = timeSec / duration;
            normalized -= std::floor(normalized);   // wrap 0..1

            const double frameF  = normalized * static_cast<double>(kfs.size());
            const size_t kfCount = kfs.size();
            size_t idxA = static_cast<size_t>(frameF);
            if (idxA >= kfCount) { idxA = kfCount - 1; }
            const size_t idxB    = (idxA + 1 < kfCount) ? idxA + 1 : idxA;
            const float  blend   = static_cast<float>(frameF - static_cast<double>(idxA));

            const XMMATRIX A = XMLoadFloat4x4(&kfs[idxA].transform);
            const XMMATRIX B = XMLoadFloat4x4(&kfs[idxB].transform);
            XMMATRIX result;
            result.r[0] = XMVectorLerp(A.r[0], B.r[0], blend);
            result.r[1] = XMVectorLerp(A.r[1], B.r[1], blend);
            result.r[2] = XMVectorLerp(A.r[2], B.r[2], blend);
            result.r[3] = XMVectorLerp(A.r[3], B.r[3], blend);
            return result;
        }
    }

    AnimatorRuntime::AnimatorRuntime(const AnimatorController&        controller,
                                     const engine::render::Skeleton&  skeleton,
                                     ClipMap                          clipMap)
        : m_controller(controller)
        , m_skeleton(skeleton)
        , m_clipMap(std::move(clipMap))
    {
        // Parameter 슬롯 초기화 — defaultValue 로.
        m_paramValues.reserve(controller.parameters.size());
        for (size_t i = 0; i < controller.parameters.size(); ++i)
        {
            m_paramValues.push_back(controller.parameters[i].defaultValue);
            m_paramNameToIndex.emplace(controller.parameters[i].name, i);
        }

        // 초기 state — defaultStateName 또는 states[0].
        if (!controller.states.empty())
        {
            size_t defaultIdx = 0;
            if (!controller.defaultStateName.empty())
            {
                const engine::int32 found = FindStateIndex(controller.defaultStateName);
                if (found >= 0) { defaultIdx = static_cast<size_t>(found); }
            }
            m_currentStateIndex = defaultIdx;
        }

        m_palette.resize(skeleton.BoneCount());
        const XMFLOAT4X4 identity = []{
            XMFLOAT4X4 m;
            XMStoreFloat4x4(&m, XMMatrixIdentity());
            return m;
        }();
        std::fill(m_palette.begin(), m_palette.end(), identity);
    }

    AnimatorRuntime::~AnimatorRuntime() = default;

    const std::string& AnimatorRuntime::CurrentStateName() const noexcept
    {
        static const std::string kEmpty;
        if (m_currentStateIndex >= m_controller.states.size()) { return kEmpty; }
        return m_controller.states[m_currentStateIndex].name;
    }

    float AnimatorRuntime::CrossfadeProgress01() const noexcept
    {
        if (!m_transitioning || m_crossfadeDuration <= 0.0f) { return 0.0f; }
        return std::clamp(m_crossfadeElapsed / m_crossfadeDuration, 0.0f, 1.0f);
    }

    engine::int32 AnimatorRuntime::FindParamIndex(std::string_view name) const noexcept
    {
        auto it = m_paramNameToIndex.find(std::string{ name });
        if (it == m_paramNameToIndex.end()) { return -1; }
        return static_cast<engine::int32>(it->second);
    }

    engine::int32 AnimatorRuntime::FindStateIndex(std::string_view name) const noexcept
    {
        for (size_t i = 0; i < m_controller.states.size(); ++i)
        {
            if (m_controller.states[i].name == name)
            {
                return static_cast<engine::int32>(i);
            }
        }
        return -1;
    }

    void AnimatorRuntime::SetBool(std::string_view name, bool value)
    {
        const auto idx = FindParamIndex(name);
        if (idx >= 0) { m_paramValues[static_cast<size_t>(idx)] = value ? 1.0f : 0.0f; }
    }

    void AnimatorRuntime::SetInt(std::string_view name, engine::int32 value)
    {
        const auto idx = FindParamIndex(name);
        if (idx >= 0) { m_paramValues[static_cast<size_t>(idx)] = static_cast<float>(value); }
    }

    void AnimatorRuntime::SetFloat(std::string_view name, float value)
    {
        const auto idx = FindParamIndex(name);
        if (idx >= 0) { m_paramValues[static_cast<size_t>(idx)] = value; }
    }

    void AnimatorRuntime::SetTrigger(std::string_view name)
    {
        const auto idx = FindParamIndex(name);
        if (idx >= 0) { m_paramValues[static_cast<size_t>(idx)] = 1.0f; }
    }

    bool AnimatorRuntime::AllConditionsMet(const std::vector<TransitionCondition>& conds) const noexcept
    {
        for (const auto& c : conds)
        {
            const auto idx = FindParamIndex(c.parameterName);
            if (idx < 0) { return false; }   // 없는 parameter → 조건 false
            const float val = m_paramValues[static_cast<size_t>(idx)];
            bool ok = false;
            switch (c.op)
            {
                case ConditionOp::IfTrue:    ok = (val > 0.5f); break;
                case ConditionOp::IfFalse:   ok = (val < 0.5f); break;
                case ConditionOp::Greater:   ok = (val > c.value); break;
                case ConditionOp::Less:      ok = (val < c.value); break;
                case ConditionOp::Equals:    ok = (std::fabs(val - c.value) < 0.001f); break;
                case ConditionOp::NotEquals: ok = (std::fabs(val - c.value) >= 0.001f); break;
            }
            if (!ok) { return false; }
        }
        return true;
    }

    void AnimatorRuntime::Update(float dt)
    {
        if (m_controller.states.empty() || m_palette.empty()) { return; }

        // ① transitioning 진행
        if (m_transitioning)
        {
            m_crossfadeElapsed += dt;
            if (m_targetStateIndex < m_controller.states.size())
            {
                m_targetStateTime += static_cast<double>(dt) * m_controller.states[m_targetStateIndex].speed;
            }
            if (m_crossfadeElapsed >= m_crossfadeDuration)
            {
                // 전환 완료 — target 이 current.
                m_currentStateIndex = m_targetStateIndex;
                m_currentStateTime  = m_targetStateTime;
                m_transitioning     = false;
                m_crossfadeElapsed  = 0.0f;
                m_crossfadeDuration = 0.0f;
            }
        }

        // ② 현재 state 시간 진행 + wrap/clamp.
        if (m_currentStateIndex < m_controller.states.size())
        {
            const auto& curState = m_controller.states[m_currentStateIndex];
            m_currentStateTime += static_cast<double>(dt) * curState.speed;
            const engine::render::AnimClip* curClip = nullptr;
            if (auto it = m_clipMap.find(curState.motionClipPath); it != m_clipMap.end())
            {
                curClip = it->second;
            }
            const double duration = (curClip != nullptr) ? curClip->DurationSec() : 0.0;
            if (duration > 0.0)
            {
                if (curState.loop)
                {
                    while (m_currentStateTime >= duration) { m_currentStateTime -= duration; }
                    while (m_currentStateTime < 0.0)        { m_currentStateTime += duration; }
                }
                else
                {
                    if (m_currentStateTime > duration) { m_currentStateTime = duration; }
                    if (m_currentStateTime < 0.0)       { m_currentStateTime = 0.0; }
                }
            }
        }

        // ③ transition 평가 — 진행 중이면 skip (다중 동시 전환 제한).
        // 발동된 transition 의 *조건에 쓰인 Trigger 만* reset (Unity 사양: 소비된 Trigger).
        // 발동 실패한 transition 의 Trigger 는 다음 프레임에도 활성 유지.
        std::vector<engine::int32> consumedTriggers;
        if (!m_transitioning && m_currentStateIndex < m_controller.states.size())
        {
            const auto& curState = m_controller.states[m_currentStateIndex];
            const engine::render::AnimClip* curClip = nullptr;
            if (auto it = m_clipMap.find(curState.motionClipPath); it != m_clipMap.end())
            {
                curClip = it->second;
            }
            const double duration = (curClip != nullptr) ? curClip->DurationSec() : 0.0;

            for (const auto& t : m_controller.transitions)
            {
                // fromStateName == "" (Any State) OR == 현재 state name
                const bool fromMatch = t.fromStateName.empty()
                                       || t.fromStateName == curState.name;
                if (!fromMatch) { continue; }

                // hasExitTime
                if (t.hasExitTime && duration > 0.0)
                {
                    const double normalizedTime = m_currentStateTime / duration;
                    if (normalizedTime < t.exitTime) { continue; }
                }

                // conditions AND
                if (!AllConditionsMet(t.conditions)) { continue; }

                // 새 transition 시작
                const auto toIdx = FindStateIndex(t.toStateName);
                if (toIdx < 0) { continue; }

                m_transitioning     = true;
                m_targetStateIndex  = static_cast<size_t>(toIdx);
                m_targetStateTime   = 0.0;
                m_crossfadeDuration = (t.crossfadeDuration > 0.0001f) ? t.crossfadeDuration : 0.0001f;
                m_crossfadeElapsed  = 0.0f;

                // 이 transition 의 *Trigger 조건* 만 consumed.
                for (const auto& cond : t.conditions)
                {
                    const auto pIdx = FindParamIndex(cond.parameterName);
                    if (pIdx < 0) { continue; }
                    if (m_controller.parameters[static_cast<size_t>(pIdx)].type == ParameterType::Trigger)
                    {
                        consumedTriggers.push_back(pIdx);
                    }
                }
                break;
            }
        }

        // ④ Trigger reset — 발동된 transition 의 trigger 만.
        for (engine::int32 idx : consumedTriggers)
        {
            m_paramValues[static_cast<size_t>(idx)] = 0.0f;
        }

        // ⑤ 본 팔레트 계산.
        BuildPalette();
    }

    void AnimatorRuntime::BuildPalette()
    {
        const engine::render::AnimClip* curClip = nullptr;
        const engine::render::AnimClip* tgtClip = nullptr;
        if (m_currentStateIndex < m_controller.states.size())
        {
            if (auto it = m_clipMap.find(m_controller.states[m_currentStateIndex].motionClipPath);
                it != m_clipMap.end())
            {
                curClip = it->second;
            }
        }
        if (m_transitioning && m_targetStateIndex < m_controller.states.size())
        {
            if (auto it = m_clipMap.find(m_controller.states[m_targetStateIndex].motionClipPath);
                it != m_clipMap.end())
            {
                tgtClip = it->second;
            }
        }

        const float weight = CrossfadeProgress01();

        for (size_t b = 0; b < m_palette.size(); ++b)
        {
            const XMMATRIX curMat = EvaluateBoneTransform(curClip, b, m_currentStateTime);
            XMMATRIX boneGlobal;
            if (m_transitioning && tgtClip != nullptr)
            {
                const XMMATRIX tgtMat = EvaluateBoneTransform(tgtClip, b, m_targetStateTime);
                boneGlobal.r[0] = XMVectorLerp(curMat.r[0], tgtMat.r[0], weight);
                boneGlobal.r[1] = XMVectorLerp(curMat.r[1], tgtMat.r[1], weight);
                boneGlobal.r[2] = XMVectorLerp(curMat.r[2], tgtMat.r[2], weight);
                boneGlobal.r[3] = XMVectorLerp(curMat.r[3], tgtMat.r[3], weight);
            }
            else
            {
                boneGlobal = curMat;
            }

            const XMMATRIX offsetMat = XMLoadFloat4x4(&m_skeleton.Bones()[b].offsetMatrix);
            const XMMATRIX combined  = XMMatrixMultiply(boneGlobal, offsetMat);
            XMStoreFloat4x4(&m_palette[b], combined);
        }
    }
}
