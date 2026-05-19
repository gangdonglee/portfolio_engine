#pragma once

#include "anim/AnimatorController.h"

#include <string>
#include <string_view>

namespace engine::anim
{
    // AnimatorController ↔ JSON 직렬화.
    //
    // 포맷 (사람 가독 + git diff 가능):
    //   {
    //     "name": "...",
    //     "defaultStateName": "Idle",
    //     "states":     [ { name, motionClipPath, loop, speed }, ... ],
    //     "parameters": [ { name, type: "Bool"|"Int"|"Float"|"Trigger", defaultValue }, ... ],
    //     "transitions": [
    //       { fromStateName, toStateName, crossfadeDuration, hasExitTime, exitTime,
    //         conditions: [ { parameterName, op: "IfTrue"|... , value }, ... ] }, ...
    //     ]
    //   }
    //
    // 에러 정책: 파싱 실패 / 파일 미존재는 std::runtime_error throw.
    void                SaveJson(const AnimatorController& controller, std::string_view path);
    AnimatorController  LoadJson(std::string_view path);
}
