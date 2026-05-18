#pragma once

namespace engine::platform
{
    class Input;
}

namespace client
{
    // 키보드 입력 → 게임 의미 트리거. 본 단계는 "Animator 클립 전환" 1 종류.
    //
    // 다운 엣지 감지: 키가 *직전 프레임에는 안 눌렸고* 이번 프레임 눌린 순간만 트리거.
    // 매 프레임 Tick(input) 호출 후 ConsumeClipChange() 로 결과를 소비.
    class InputController final
    {
    public:
        // 변경 없음 — 키 입력 미발생.
        static constexpr int kNoClipChange = -2;
        // T-pose 로 복귀 (Animator nullptr).
        static constexpr int kClipTpose    = -1;

        InputController() = default;

        InputController(const InputController&)            = delete;
        InputController& operator=(const InputController&) = delete;
        InputController(InputController&&)                 = delete;
        InputController& operator=(InputController&&)      = delete;

        // 매 프레임 1회. window.GetInput().BeginFrame() 직후 호출.
        // 키 0 → T-pose, 키 1..4 → clip 0..3 다운 엣지 감지.
        void Tick(const engine::platform::Input& input);

        // 이번 프레임의 요청 1회 소비. 호출 후 kNoClipChange 로 리셋.
        // 반환값: kNoClipChange / kClipTpose / 0..3.
        int ConsumeClipChange();

    private:
        bool m_prevDown[5]{};
        int  m_pendingClip = kNoClipChange;
    };
}
