#pragma once

#include "core/Types.h"

#include <string>
#include <vector>

namespace engine::anim
{
    // 1D Blend Tree 항목 — 단일 파라미터 값(threshold)에 대응되는 motion 클립.
    //   AnimatorRuntime 이 *현재 blendParameter 값* 의 인접 두 항목 (threshold 정렬 기준)
    //   사이를 linear interp 으로 weighted blend.
    struct BlendTreeEntry
    {
        std::string  motionClipPath;
        float        threshold = 0.0f;
    };

    // State 의 ballistic root motion 메타데이터 — Jump 같은 *시간 기반 Y 곡선* 의
    //   takeoff/landing 정규화 시간 + peak height. Mixamo 의 jump 클립이 자체 root motion
    //   을 baked 하지 않거나 mesh-local 패턴이 우리 mesh 와 안 맞을 때, *코드 측에서* 부드러운
    //   parabola Y 를 덧씌워 시각 점프를 표현.
    //
    //   takeoffNormTime / landingNormTime: 0..1 정규화 시간. clip 의 takeoff (땅 떠나는 순간) 와
    //                                       landing (땅 닿는 순간) 의 normalized stateTime.
    //   peakHeight: airborne 의 sin² 곡선 최대값 (units).
    //   fadeWindow: takeoff/landing 경계의 smoothstep blend 폭 (정규화 시간 ±). 끊김 방지.
    //   crouchOffsetY: pre-takeoff / post-landing 단계에 *상시* 적용되는 Y 오프셋 (units, 보통 음수).
    //                  Mixamo 클립의 anticipation 자세에서 mesh-local foot 이 살짝 떠 있을 때 보정.
    //                  airborne 윈도우에서는 fadeOut 되어 parabola 만 남음 — 즉 두 곡선이
    //                  smoothstep 으로 cross-blend.
    //   groundAlignBone: 비어있지 않으면 *동적* foot-align 활성 — 매 프레임 해당 본의
    //                    mesh-local Y 를 읽어 (baseline - currentBoneY) 만큼 가산. FBX 가 프레임
    //                    마다 foot bone Y 가 바뀌는 (root motion baked) 경우 일정 오프셋만으론
    //                    부족 — 동적 추적 필요.
    //   groundAlignBaseline: bone 이 "feet on ground" 자세에 있을 때의 mesh-local Y 기준값.
    //                        대개 클립의 첫 프레임 또는 Idle bind pose 의 본 Y. 시각 검증으로 튜닝.
    //
    // 호출자 (Application) 는 AnimatorRuntime::RootMotionY() 의 결과를 transform.position.y 에
    // additive 적용. peakHeight 0 또는 takeoff>=landing 이면 parabola 비활성 (다른 항만 작용).
    struct RootMotionBallistic
    {
        float       takeoffNormTime     = 0.0f;
        float       landingNormTime     = 1.0f;
        float       peakHeight          = 0.0f;
        float       fadeWindow          = 0.04f;
        float       crouchOffsetY       = 0.0f;
        std::string groundAlignBone;
        float       groundAlignBaseline = 0.0f;
    };

    // 한 State = 하나의 motion. 두 모드:
    //   - 단일 clip: motionClipPath 만 사용. blendTree 비어있음.
    //   - 1D Blend Tree: blendTree 에 entries[] + blendParameter 지정.
    //     motionClipPath 는 *무시*. AnimatorRuntime 이 blendParameter 값으로 평가.
    //
    // motionClipPath: assets/ 또는 Resources/ 기준 상대 경로. 클립 FBX (Mixamo without-skin 등).
    //   비어있으면 T-pose (state 가 *유효한 motion 없음*).
    // loop: 클립 끝에서 wrap 할지. false 면 끝에 도달 후 정지 (또는 transition 의 hasExitTime 트리거).
    // speed: 시간 진행 배율. 1.0 = 정상, 2.0 = 2배 빠르게.
    // blendTree: 비어있지 않으면 blend tree mode 활성. threshold 오름차순 가정.
    // blendParameter: blend tree mode 의 평가 파라미터 이름 (보통 Float 타입 — Speed 등).
    // hasRootMotion / rootMotion: 점프 같은 ballistic Y 곡선 — JSON 의 "rootMotion" 객체로 지정.
    //                              없으면 코드 측 Y 보정 없음 (animation 만 재생).
    struct AnimatorState
    {
        std::string                  name;
        std::string                  motionClipPath;
        bool                         loop  = true;
        float                        speed = 1.0f;
        std::vector<BlendTreeEntry>  blendTree;          // empty = 단일 clip mode
        std::string                  blendParameter;     // blend tree mode 에서만 의미
        bool                         hasRootMotion = false;
        RootMotionBallistic          rootMotion{};
    };

    // 파라미터 타입 — Unity Mecanim 과 동일.
    //   Bool/Int/Float — 명시 값 (Editor 에서 설정 + 코드에서 SetBool/SetInt/SetFloat).
    //   Trigger — 펄스 (Set 후 transition 평가 시 한 번만 true, 자동 reset).
    enum class ParameterType : engine::uint8
    {
        Bool,
        Int,
        Float,
        Trigger,
    };

    // 모든 파라미터는 *단일 float 슬롯* 으로 표현.
    //   Bool/Trigger: 0=false, 1=true.
    //   Int: 정수값 (그대로 float 캐스트).
    //   Float: 그대로.
    // type 정보는 Editor UX (위젯 종류 + condition op 후보) 와 직렬화 자기검사용.
    struct AnimatorParameter
    {
        std::string    name;
        ParameterType  type         = ParameterType::Bool;
        float          defaultValue = 0.0f;
    };

    // Transition 의 조건 1개. AND 로 모든 조건이 만족되면 transition 발동.
    enum class ConditionOp : engine::uint8
    {
        IfTrue,      // Bool/Trigger 가 참
        IfFalse,     // Bool 이 거짓 (Trigger 비대상)
        Greater,     // Int/Float 가 value 보다 큼
        Less,        // Int/Float 가 value 보다 작음
        Equals,      // 정확 일치 (Int 권장, Float 는 epsilon 비교)
        NotEquals,   //
    };

    struct TransitionCondition
    {
        std::string  parameterName;
        ConditionOp  op    = ConditionOp::IfTrue;
        float        value = 0.0f;
    };

    // State A → State B 전이.
    //   conditions: 모두 만족(AND) 되어야 transition 발동.
    //   crossfadeDuration: A 의 weight 가 1→0, B 의 weight 가 0→1 로 보간되는 시간(초).
    //                      0 이면 즉시 hard cut.
    //   hasExitTime: true 면 fromState 의 normalized time(0..1) 이 exitTime 이상이어야 transition.
    //                false 면 conditions 만족 즉시 transition.
    struct AnimatorTransition
    {
        std::string                       fromStateName;   // 비어있으면 Any State (모든 state 에서 트리거 가능)
        std::string                       toStateName;
        std::vector<TransitionCondition>  conditions;
        float                             crossfadeDuration = 0.2f;
        bool                              hasExitTime       = false;
        float                             exitTime          = 1.0f;   // 0..1 normalized
    };

    // Animator 의 단일 컨트롤러. 한 .animator.json 파일 = 한 AnimatorController.
    //
    // 직렬화 / 로드 / 평가 책임:
    //   - 직렬화: engine::anim::AnimatorSerializer (Scene 과 동일 패턴).
    //   - 평가:   engine::anim::AnimatorRuntime (M1 단계에서 추가).
    //
    // Layer / Avatar Mask 는 M4+ 의 확장. 현재 단일 layer 가정.
    struct AnimatorController
    {
        std::string                       name;
        std::vector<AnimatorState>        states;
        std::vector<AnimatorTransition>   transitions;
        std::vector<AnimatorParameter>    parameters;
        std::string                       defaultStateName;   // 초기 state. 빈 문자열이면 states[0].
    };
}
