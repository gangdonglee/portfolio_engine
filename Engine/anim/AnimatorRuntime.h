#pragma once

#include "anim/AnimatorController.h"
#include "core/Types.h"

#include <DirectXMath.h>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace engine::render
{
    class Skeleton;
    class AnimClip;
}

namespace engine::anim
{
    // Animator Controller 런타임 평가 — Unity Mecanim 의 *Animator* 컴포넌트 등가.
    //
    // 입력:
    //   - controller: 데이터 (states / transitions / parameters).
    //   - skeleton:   본 트리 + offsetMatrix (베이스 메시 자산).
    //   - clipMap:    motionClipPath → AnimClip* (호출자가 사전 로드 + 라이프타임 보장).
    //                 controller 의 모든 state.motionClipPath 가 키로 등록되어야 함.
    //                 없는 키는 *T-pose* (해당 state 활성 시 본 transform = identity).
    //
    // 매 프레임 흐름 (Update(dt)):
    //   ① transitioning 이면 crossfade 진행 — 끝나면 currentState ← targetState.
    //   ② 현재 state 의 시간 진행 + (loop ? wrap : clamp).
    //   ③ transition 평가 — fromState == current OR ""(Any), conditions AND, hasExitTime.
    //   ④ Trigger 자동 reset (평가 후).
    //   ⑤ 본 팔레트 계산 — currentState 클립 + (transitioning 시) targetState 클립 weight lerp.
    //
    // API:
    //   - SetBool/Int/Float(name, val), SetTrigger(name): parameter 슬롯 설정.
    //   - Update(dt): 위 흐름.
    //   - Palette(): 매 Update 후 갱신된 본 팔레트 (HLSL b1 cbuffer).
    //
    // 단일 layer 가정 (Phase 5-M4 에서 다중 Layer 도입). Blend Tree 없음 (5-M2).
    //
    // 단일 소유 (복사·이동 금지).
    class AnimatorRuntime final
    {
    public:
        using ClipMap = std::unordered_map<std::string, const engine::render::AnimClip*>;

        AnimatorRuntime(const AnimatorController&        controller,
                        const engine::render::Skeleton&  skeleton,
                        ClipMap                          clipMap);
        ~AnimatorRuntime();

        AnimatorRuntime(const AnimatorRuntime&)            = delete;
        AnimatorRuntime& operator=(const AnimatorRuntime&) = delete;
        AnimatorRuntime(AnimatorRuntime&&)                 = delete;
        AnimatorRuntime& operator=(AnimatorRuntime&&)      = delete;

        void Update(float dt);

        // Parameter 설정 — name 미존재 시 silent skip (Editor 에서 typo 흔함).
        void SetBool   (std::string_view name, bool  value);
        void SetInt    (std::string_view name, engine::int32 value);
        void SetFloat  (std::string_view name, float value);
        void SetTrigger(std::string_view name);

        // 현재 state 의 인덱스 / 이름 (디버그 / Editor 표시용).
        size_t             CurrentStateIndex() const noexcept { return m_currentStateIndex; }
        const std::string& CurrentStateName() const noexcept;

        // transitioning 진행도 (0..1). 비활성 시 0.
        float CrossfadeProgress01() const noexcept;

        // 본 팔레트 — 매 Update 후 갱신.
        const std::vector<DirectX::XMFLOAT4X4>& Palette() const noexcept { return m_palette; }
        size_t                                  PaletteSize() const noexcept { return m_palette.size(); }

        // *bone joint 의 mesh-local 위치* (currentBone × (0,0,0,1) 의 translation Y).
        //   자동 floor 정렬 등 — bone palette 의 translation 과 다름 (mesh origin 변환과 혼동 X).
        //   BuildPalette 내부에서 m_boneMeshLocalY 갱신.
        float BoneMeshLocalY(size_t boneIdx) const noexcept
        {
            return (boneIdx < m_boneMeshLocalY.size()) ? m_boneMeshLocalY[boneIdx] : 0.0f;
        }
        // 진단용 — 본 joint 의 mesh-local X (수평 1축).
        float BoneMeshLocalX(size_t boneIdx) const noexcept
        {
            return (boneIdx < m_boneMeshLocalX.size()) ? m_boneMeshLocalX[boneIdx] : 0.0f;
        }

    private:
        // parameter 이름 → m_paramValues 인덱스 (없으면 -1).
        engine::int32 FindParamIndex(std::string_view name) const noexcept;

        // state 이름 → m_controller.states 인덱스 (없으면 -1).
        engine::int32 FindStateIndex(std::string_view name) const noexcept;

        // 모든 condition AND 만족?
        bool AllConditionsMet(const std::vector<TransitionCondition>& conds) const noexcept;

        // 본 팔레트 채움.
        void BuildPalette();

        // state 의 한 본 transform 평가 — 단일 clip 또는 1D blend tree.
        //   blend tree 면 blendParameter 값 → 인접 두 entry 의 clip 평가 후 weighted lerp.
        //   각 entry 는 clipMap 조회 + 자체 duration 으로 wrap.
        DirectX::XMMATRIX EvaluateStateBoneTransform(const AnimatorState& state,
                                                     size_t               boneIdx,
                                                     double               stateTime) const;

        // state 의 *대표 duration* — Update 의 m_currentStateTime wrap 용.
        //   단일 clip: 그 clip 의 DurationSec.
        //   blend tree: blendTree[0] 의 clip duration (첫 entry, 정렬 가정).
        //   클립 매핑 실패 / 비어있음 → 0.0 반환 (호출자가 wrap skip).
        double StateRepresentativeDuration(const AnimatorState& state) const;

    public:
        // 외부 게임 코드용 — state 이름으로 대표 duration 조회 (점프 물리 동기화 등).
        double StateDuration(std::string_view stateName) const;

        // 현재 state 의 ballistic root motion Y 오프셋 (units).
        //   - 현재 state 의 hasRootMotion==false: 0 반환.
        //   - peakHeight==0 또는 takeoffNormTime>=landingNormTime: 0 반환.
        //   - airborne 윈도우 [takeoff, landing] 안에서 sin² parabola 적용 후 smoothstep 페이드.
        // 호출자 (Application) 가 transform.position.y += RootMotionY() 로 가산.
        float RootMotionY() const noexcept;

        // 애니메이션의 root motion 추출값 (units, mesh-local Y delta from state-entry baseline).
        //   state.rootMotion.extractRootMotionFromBone 가 설정된 경우만 의미 있음 (else 0).
        //   BuildPalette 가 이 값만큼 모든 본 팔레트의 Y translation 을 차감 — visual mesh
        //   는 in-place. 동시에 외부에서 inst.position.y 에 가산 가능 (capsule-style).
        float RootMotionExtractedY() const noexcept { return m_rootMotionExtractedY; }

        // 현재 state 의 진행 시간 (sec). transition 중에는 *from state 의 시간*.
        double CurrentStateTime() const noexcept { return m_currentStateTime; }

        // 디버그 — 시간 진행 일시정지 (Update 의 dt 누적 중단).
        //   transition 도 진행 중단 → state 가 freeze.
        bool IsPaused() const noexcept     { return m_paused; }
        void SetPaused(bool paused) noexcept { m_paused = paused; }

        // 디버그 — 현재 state 의 stateTime 직접 설정 (frame slider 용).
        //   transition 중이 아니라고 가정 (slider 조작은 보통 paused 상태에서).
        void SetCurrentStateTime(double t) noexcept { m_currentStateTime = t; }

    private:
        const AnimatorController&        m_controller;
        const engine::render::Skeleton&  m_skeleton;
        ClipMap                          m_clipMap;

        std::vector<float>                          m_paramValues;
        std::unordered_map<std::string, std::size_t> m_paramNameToIndex;

        size_t  m_currentStateIndex = 0;
        double  m_currentStateTime  = 0.0;

        bool    m_transitioning     = false;
        size_t  m_targetStateIndex  = 0;
        double  m_targetStateTime   = 0.0;
        float   m_crossfadeDuration = 0.0f;
        float   m_crossfadeElapsed  = 0.0f;

        std::vector<DirectX::XMFLOAT4X4> m_palette;
        // *currentBone 의 translation Y* per bone — 자동 floor 정렬용.
        std::vector<float>               m_boneMeshLocalY;
        std::vector<float>               m_boneMeshLocalX;   // 진단용 — 수평축 변동 추적.

        bool    m_paused = false;

        // Root motion 추출 상태.
        //   m_rootMotionBaselineBone: 현재 baseline 의 본 이름 — 같으면 baseline 유지.
        //     Lyra 패턴 — Jump_Start → Apex → Fall_Loop → Land 모두 같은 본 이라 통과.
        //   m_rootMotionBaselineY: 첫 캡처한 bone Y. extract 상태 이어지는 동안 유지.
        //   m_rootMotionExtractedY: 이번 frame 의 delta (현재 boneY - baselineY).
        std::string m_rootMotionBaselineBone;
        float       m_rootMotionBaselineY    = 0.0f;
        float       m_rootMotionExtractedY   = 0.0f;
    };
}
