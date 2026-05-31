#include "anim/AnimatorRuntime.h"

#include "core/Logger.h"
#include "render/AnimClip.h"
#include "render/Skeleton.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

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

        // 두 transform 의 element-wise lerp — crossfade 와 동일 정책.
        XMMATRIX LerpTransforms(const XMMATRIX& a, const XMMATRIX& b, float t)
        {
            XMMATRIX r;
            r.r[0] = XMVectorLerp(a.r[0], b.r[0], t);
            r.r[1] = XMVectorLerp(a.r[1], b.r[1], t);
            r.r[2] = XMVectorLerp(a.r[2], b.r[2], t);
            r.r[3] = XMVectorLerp(a.r[3], b.r[3], t);
            return r;
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
        m_boneMeshLocalY.assign(skeleton.BoneCount(), 0.0f);
        m_boneMeshLocalX.assign(skeleton.BoneCount(), 0.0f);
        const XMFLOAT4X4 identity = []{
            XMFLOAT4X4 m;
            XMStoreFloat4x4(&m, XMMatrixIdentity());
            return m;
        }();
        std::fill(m_palette.begin(), m_palette.end(), identity);
    }

    AnimatorRuntime::~AnimatorRuntime() = default;

    namespace {
        // state 의 [startNormTime, endNormTime] frame range 안에서 stateTime 을 실제 clip
        // 시간으로 매핑. range default [0, 1] 이면 stateTime 그대로.
        double MapToClipTime(const AnimatorState& state, double stateTime, double clipDuration) noexcept {
            const double startNorm = static_cast<double>(state.startNormTime);
            return clipDuration * startNorm + stateTime;
        }
    }

    XMMATRIX AnimatorRuntime::EvaluateStateBoneTransform(const AnimatorState& state,
                                                         size_t               boneIdx,
                                                         double               stateTime) const
    {
        // 단일 clip mode — blendTree 가 비어있음.
        if (state.blendTree.empty())
        {
            const engine::render::AnimClip* clip = nullptr;
            if (auto it = m_clipMap.find(state.motionClipPath); it != m_clipMap.end())
            {
                clip = it->second;
            }
            const double clipTime = clip ? MapToClipTime(state, stateTime, clip->DurationSec()) : stateTime;
            return EvaluateBoneTransform(clip, boneIdx, clipTime);
        }

        // 1D Blend Tree mode — blendParameter 값 → 인접 두 entry 의 clip 평가 후 lerp.
        //   threshold 정렬 가정. 파라미터 값이 범위 밖이면 양 끝으로 clamp.
        const engine::int32 pIdx = FindParamIndex(state.blendParameter);
        const float paramVal = (pIdx >= 0) ? m_paramValues[static_cast<size_t>(pIdx)] : 0.0f;

        const auto& entries = state.blendTree;
        // 첫 entry 보다 작거나 같으면 그 clip 만.
        if (paramVal <= entries.front().threshold)
        {
            const engine::render::AnimClip* clip = nullptr;
            if (auto it = m_clipMap.find(entries.front().motionClipPath); it != m_clipMap.end()) { clip = it->second; }
            const double clipTime = clip ? MapToClipTime(state, stateTime, clip->DurationSec()) : stateTime;
            return EvaluateBoneTransform(clip, boneIdx, clipTime);
        }
        // 마지막 entry 보다 크거나 같으면 그 clip 만.
        if (paramVal >= entries.back().threshold)
        {
            const engine::render::AnimClip* clip = nullptr;
            if (auto it = m_clipMap.find(entries.back().motionClipPath); it != m_clipMap.end()) { clip = it->second; }
            const double clipTime = clip ? MapToClipTime(state, stateTime, clip->DurationSec()) : stateTime;
            return EvaluateBoneTransform(clip, boneIdx, clipTime);
        }

        // 인접 두 entry 찾기 — paramVal 이 [thresholds[i], thresholds[i+1]] 사이.
        for (size_t i = 0; i + 1 < entries.size(); ++i)
        {
            const float a = entries[i].threshold;
            const float b = entries[i + 1].threshold;
            if (paramVal >= a && paramVal <= b)
            {
                const float t = (b - a > 0.0001f) ? (paramVal - a) / (b - a) : 0.0f;
                const engine::render::AnimClip* clipA = nullptr;
                const engine::render::AnimClip* clipB = nullptr;
                if (auto it = m_clipMap.find(entries[i    ].motionClipPath); it != m_clipMap.end()) { clipA = it->second; }
                if (auto it = m_clipMap.find(entries[i + 1].motionClipPath); it != m_clipMap.end()) { clipB = it->second; }
                const double clipTimeA = clipA ? MapToClipTime(state, stateTime, clipA->DurationSec()) : stateTime;
                const double clipTimeB = clipB ? MapToClipTime(state, stateTime, clipB->DurationSec()) : stateTime;
                const XMMATRIX A = EvaluateBoneTransform(clipA, boneIdx, clipTimeA);
                const XMMATRIX B = EvaluateBoneTransform(clipB, boneIdx, clipTimeB);
                return LerpTransforms(A, B, t);
            }
        }
        return XMMatrixIdentity();
    }

    double AnimatorRuntime::StateRepresentativeDuration(const AnimatorState& state) const
    {
        const std::string& path = state.blendTree.empty()
            ? state.motionClipPath
            : state.blendTree.front().motionClipPath;
        if (auto it = m_clipMap.find(path); it != m_clipMap.end() && it->second != nullptr)
        {
            const double full = it->second->DurationSec();
            // Frame range 적용 — windowed duration. state.startNorm = 0, endNorm = 1 면 동일.
            const double startNorm = static_cast<double>(state.startNormTime);
            const double endNorm   = static_cast<double>(state.endNormTime);
            if (endNorm > startNorm && endNorm <= 1.0 + 0.0001)
            {
                return full * (endNorm - startNorm);
            }
            return full;
        }
        return 0.0;
    }

    double AnimatorRuntime::StateDuration(std::string_view stateName) const
    {
        const engine::int32 idx = FindStateIndex(stateName);
        if (idx < 0) { return 0.0; }
        return StateRepresentativeDuration(m_controller.states[static_cast<size_t>(idx)]);
    }

    float AnimatorRuntime::RootMotionY() const noexcept
    {
        if (m_currentStateIndex >= m_controller.states.size()) { return 0.0f; }
        const auto& state = m_controller.states[m_currentStateIndex];
        if (!state.hasRootMotion) { return 0.0f; }

        const auto& rm = state.rootMotion;
        // Extract 모드 활성 시 — palette 는 baseline 으로 in-place 고정 (BuildPalette 처리).
        //   - peakHeight > 0: airborne 윈도우에서 parabola lift 적용 (Mixamo 처럼 in-place
        //     animation 이라 자체 lift 가 없는 클립용).
        //   - peakHeight == 0: extractedY 그대로 사용 (root motion 이 baked 된 클립용).
        //   anticipation/recovery 에선 0 (mesh 가 이미 in-place 라 feet 안착).
        if (!rm.extractRootMotionFromBone.empty())
        {
            const double duration = StateRepresentativeDuration(state);
            if (duration <= 0.05) { return 0.0f; }
            if (rm.takeoffNormTime >= rm.landingNormTime) { return 0.0f; }
            const float n = static_cast<float>(m_currentStateTime / duration);
            const auto sstep = [](float a, float b, float x) noexcept {
                const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
                return t * t * (3.0f - 2.0f * t);
            };
            const float fadeIn  = sstep(rm.takeoffNormTime - rm.fadeWindow,
                                        rm.takeoffNormTime + rm.fadeWindow, n);
            const float fadeOut = 1.0f - sstep(rm.landingNormTime - rm.fadeWindow,
                                               rm.landingNormTime + rm.fadeWindow, n);
            const float airborne = fadeIn * fadeOut;

            if (rm.peakHeight != 0.0f)
            {
                const float local = std::clamp((n - rm.takeoffNormTime) /
                                               (rm.landingNormTime - rm.takeoffNormTime),
                                               0.0f, 1.0f);
                const float s = std::sin(local * 3.14159265358979323846f);
                return rm.peakHeight * s * s * airborne;
            }
            // peakHeight=0 일 때는 inst.position 가산 안 함 — 외부 (Application + 캐릭터
            // 물리) 가 Y 를 처리하는 모드. palette 차감은 이미 BuildPalette 에서 끝나
            // mesh 는 in-place 라 Animator 측 추가 lift 불필요.
            return 0.0f;
        }
        const bool hasParabola    = (rm.peakHeight != 0.0f) && (rm.takeoffNormTime < rm.landingNormTime);
        const bool hasCrouch      = (rm.crouchOffsetY != 0.0f);
        const bool hasCrouchDip   = (rm.crouchDepth != 0.0f) && (rm.takeoffNormTime > 0.0f);
        const bool hasRecoveryDip = (rm.recoveryDepth != 0.0f) && (rm.landingNormTime < 1.0f);
        const bool hasBoneAlign   = !rm.groundAlignBone.empty();
        if (!hasParabola && !hasCrouch && !hasCrouchDip && !hasRecoveryDip && !hasBoneAlign) { return 0.0f; }

        const double duration = StateRepresentativeDuration(state);
        if (duration <= 0.05) { return 0.0f; }

        const float n = static_cast<float>(m_currentStateTime / duration);

        // smoothstep 페이드 — takeoff/landing 경계 hard-cut 방지.
        const auto smoothstep = [](float a, float b, float x) noexcept {
            const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        };
        const float fadeIn  = smoothstep(rm.takeoffNormTime - rm.fadeWindow,
                                         rm.takeoffNormTime + rm.fadeWindow, n);
        const float fadeOut = 1.0f - smoothstep(rm.landingNormTime - rm.fadeWindow,
                                                rm.landingNormTime + rm.fadeWindow, n);
        const float airborne = fadeIn * fadeOut;

        // groundAlign: 동적 — 매 프레임 본 mesh-local Y 와 baseline 차이 만큼 보정.
        //   Skeleton bone 이름 비교는 wstring (FBX 컨벤션). UTF-8 → wstring 단순 변환
        //   (Mixamo 본 이름은 ASCII). namespace prefix (mixamorig:) 대응 substring 매칭.
        float boneAlign = 0.0f;
        if (hasBoneAlign)
        {
            std::wstring wname;
            wname.reserve(rm.groundAlignBone.size());
            for (char c : rm.groundAlignBone) { wname.push_back(static_cast<wchar_t>(c)); }
            engine::int32 boneIdx = m_skeleton.FindIndex(wname);
            if (boneIdx < 0)
            {
                for (size_t i = 0; i < m_skeleton.BoneCount(); ++i)
                {
                    if (m_skeleton.Bones()[i].name.find(wname) != std::wstring::npos)
                    {
                        boneIdx = static_cast<engine::int32>(i);
                        break;
                    }
                }
            }
            if (boneIdx >= 0 && static_cast<size_t>(boneIdx) < m_boneMeshLocalY.size())
            {
                // align = baseline - currentBoneY: 본이 baseline 보다 위로 가면 음수 push down.
                boneAlign = rm.groundAlignBaseline - m_boneMeshLocalY[static_cast<size_t>(boneIdx)];
            }
        }

        // Phase weighting:
        //   crouchWeight   = 1 - fadeIn   (1 pre-takeoff, 0 elsewhere)
        //   airborneWeight = fadeIn * fadeOut
        //   recoveryWeight = 1 - fadeOut  (1 post-landing, 0 elsewhere)
        //   합 = 1 항상 (cross-blend 보장).
        //
        // crouchOffsetY: pre-takeoff / post-landing 양쪽에 (1 - airborne) 으로 적용.
        // boneAlign: pre-takeoff 만 적용 (crouchWeight). post-landing 은 FBX 가 자체적으로
        //   "feet on ground" 자세를 가지므로 동적 align 이 over-push 유발 — Jumping.fbx
        //   에서 recovery 시 footY 가 baseline 보다 매우 높아 character 가 floor 아래로
        //   박히는 현상.
        const float crouchWeight   = 1.0f - fadeIn;
        const float nonAirborne    = 1.0f - airborne;
        constexpr float kPi        = 3.14159265358979323846f;

        float result = rm.crouchOffsetY * nonAirborne + boneAlign * crouchWeight;

        // 비대칭 dip 곡선 — peakNorm 에서 1, 양 끝 (0, 1) 에서 0. smoothstep 두 개로 구성.
        //   crouchPeakNorm = 0.5 면 대칭, > 0.5 면 dip 이 더 늦게 들어감 (Mixamo 의 deep crouch
        //   anticipation 처럼 phase 후반에 발이 많이 떠 있는 케이스 대응).
        const auto asymDip = [](float local, float peakNorm) noexcept {
            const auto sstep = [](float a, float b, float x) {
                const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
                return t * t * (3.0f - 2.0f * t);
            };
            peakNorm = std::clamp(peakNorm, 0.01f, 0.99f);
            if (local < peakNorm)
            {
                return sstep(0.0f, peakNorm, local);
            }
            return 1.0f - sstep(peakNorm, 1.0f, local);
        };

        // crouchDepth: pre-takeoff dip — n=0/takeoff 에서 0, peakNorm 위치에서 깊이 최대.
        if (hasCrouchDip && n < rm.takeoffNormTime)
        {
            const float local = n / rm.takeoffNormTime;
            result += rm.crouchDepth * asymDip(local, rm.crouchPeakNorm);
        }
        // recoveryDepth: post-landing dip — landing/end 에서 0, peakNorm 위치에서 깊이 최대.
        if (hasRecoveryDip && n > rm.landingNormTime)
        {
            const float local = (n - rm.landingNormTime) / (1.0f - rm.landingNormTime);
            result += rm.recoveryDepth * asymDip(local, rm.recoveryPeakNorm);
        }
        if (hasParabola && airborne > 0.0f)
        {
            const float local = std::clamp((n - rm.takeoffNormTime) /
                                           (rm.landingNormTime - rm.takeoffNormTime),
                                           0.0f, 1.0f);
            const float s = std::sin(local * kPi);
            result += rm.peakHeight * s * s * airborne;
        }
        return result;
    }

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
        // 디버그 일시정지 — 시간 진행만 멈추고 BuildPalette 는 그대로 (slider 조작 반영).
        if (m_paused) { BuildPalette(); return; }

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

        // ② 현재 state 시간 진행 + wrap/clamp. 대표 duration 은 blend tree 의 첫 entry.
        if (m_currentStateIndex < m_controller.states.size())
        {
            const auto& curState = m_controller.states[m_currentStateIndex];
            m_currentStateTime += static_cast<double>(dt) * curState.speed;
            const double duration = StateRepresentativeDuration(curState);
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
            const double duration = StateRepresentativeDuration(curState);

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

                // 진단 — state 전환 로그.
                {
                    char buf[200];
                    std::snprintf(buf, sizeof(buf),
                                  "[anim] transition: '%s' -> '%s' (curTime=%.2fs)\n",
                                  m_controller.states[m_currentStateIndex].name.c_str(),
                                  m_controller.states[m_targetStateIndex].name.c_str(),
                                  m_currentStateTime);
                    engine::core::LogInfoA(buf);
                }

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
        const AnimatorState* curState = nullptr;
        const AnimatorState* tgtState = nullptr;
        if (m_currentStateIndex < m_controller.states.size())
        {
            curState = &m_controller.states[m_currentStateIndex];
        }
        if (m_transitioning && m_targetStateIndex < m_controller.states.size())
        {
            tgtState = &m_controller.states[m_targetStateIndex];
        }

        const float weight = CrossfadeProgress01();

        for (size_t b = 0; b < m_palette.size(); ++b)
        {
            const XMMATRIX curMat = curState
                ? EvaluateStateBoneTransform(*curState, b, m_currentStateTime)
                : XMMatrixIdentity();
            XMMATRIX boneGlobal;
            if (m_transitioning && tgtState != nullptr)
            {
                const XMMATRIX tgtMat = EvaluateStateBoneTransform(*tgtState, b, m_targetStateTime);
                boneGlobal = LerpTransforms(curMat, tgtMat, weight);
            }
            else
            {
                boneGlobal = curMat;
            }

            const XMMATRIX offsetMat = XMLoadFloat4x4(&m_skeleton.Bones()[b].offsetMatrix);
            const XMMATRIX combined  = XMMatrixMultiply(boneGlobal, offsetMat);
            XMStoreFloat4x4(&m_palette[b], combined);

            // currentBone (= boneGlobal) 의 translation vertical — bone joint 의 mesh-local 높이.
            //   ConvertMatrix transpose 정책 으로 translation 은 m[0..2][3] (col 3, rows 0-2).
            //   matReflect Y/Z swap 후 vertical = m[2][3] (FbxLoader 진단과 동일 convention).
            XMFLOAT4X4 stored;
            XMStoreFloat4x4(&stored, boneGlobal);
            m_boneMeshLocalY[b] = stored.m[2][3];
            m_boneMeshLocalX[b] = stored.m[0][3];
        }

        // === Root motion 추출 (Unreal/Unity 스타일) ===
        // current state 가 extractRootMotionFromBone 지정 시:
        //   1) 해당 본의 mesh-local Y 를 baseline 과 비교 → delta 계산
        //   2) 모든 본 팔레트의 Y translation 에서 delta 차감 (visual mesh in-place)
        //   3) m_rootMotionExtractedY 에 delta 저장 — 외부 (Application) 가 inst.position
        //      에 가산해서 캐릭터 world Y 가 애니메이션 hip curve 를 따라가게 함.
        // state 가 바뀌면 baseline 재캡처 (delta=0 부터 시작 → pop 방지).
        m_rootMotionExtractedY = 0.0f;
        if (curState && !curState->rootMotion.extractRootMotionFromBone.empty())
        {
            const auto& boneName = curState->rootMotion.extractRootMotionFromBone;
            std::wstring wname;
            wname.reserve(boneName.size());
            for (char c : boneName) { wname.push_back(static_cast<wchar_t>(c)); }
            engine::int32 boneIdx = m_skeleton.FindIndex(wname);
            if (boneIdx < 0)
            {
                for (size_t i = 0; i < m_skeleton.BoneCount(); ++i)
                {
                    if (m_skeleton.Bones()[i].name.find(wname) != std::wstring::npos)
                    {
                        boneIdx = static_cast<engine::int32>(i);
                        break;
                    }
                }
            }
            if (boneIdx >= 0 && static_cast<size_t>(boneIdx) < m_boneMeshLocalY.size())
            {
                const float currentBoneY = m_boneMeshLocalY[static_cast<size_t>(boneIdx)];
                // baseline 유지 정책 — 같은 본 이름으로 extract 모드 유지 시 baseline 그대로.
                //   Jump_Start → Apex → Fall_Loop → Land 처럼 같은 Hips bone 으로 추출하면
                //   첫 진입 (Locomotion → Jump_Start) 때만 캡처, phase 간 transition 시
                //   baseline 안 바뀌어서 mesh 튐 없음.
                if (m_rootMotionBaselineBone != boneName)
                {
                    m_rootMotionBaselineY    = currentBoneY;
                    m_rootMotionBaselineBone = boneName;
                }
                const float delta = currentBoneY - m_rootMotionBaselineY;
                m_rootMotionExtractedY = delta;

                // 본 팔레트 *상시* 차감 (in-place 강제). Mixamo Jumping 처럼 foot 가
                //   mesh-local 에 rigged 되어 있는 클립은 자체적으로 visual lift 가 없음.
                //   palette 를 baseline 으로 고정 후, RootMotionY() 가 airborne 때만 delta 를
                //   inst.position 에 가산 → 캐릭터가 capsule 처럼 hip curve 따라 lift.
                if (delta != 0.0f)
                {
                    for (size_t b = 0; b < m_palette.size(); ++b)
                    {
                        m_palette[b].m[2][3] -= delta;
                    }
                }
            }
        }
        else
        {
            // state 가 root motion 미지정 → baseline reset (다음 jump 진입 시 새로 캡처).
            m_rootMotionBaselineBone.clear();
        }
    }
}
