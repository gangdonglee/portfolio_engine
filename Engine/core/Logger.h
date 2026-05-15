#pragma once

namespace engine::core
{
    // 진단 로그 단순 wrapper. 현재 구현은 OutputDebugStringW / OutputDebugStringA 패스스루.
    //
    // 향후 확장 후보 (실제 필요해질 때 단계적 도입):
    //   - 카테고리/레벨 분리 (Info/Warn/Error)
    //   - sink 교체 (파일 / 콘솔 / ImGui 출력 창 등)
    //   - 가변 인자 포맷 헬퍼
    //
    // 호출 측 패턴:
    //   engine::core::LogInfo(L"[render] Device created\n");
    //   engine::core::LogInfoA(buf);   // 좁은 문자 버퍼 (snprintf 결과 등)
    //
    // 메시지에 줄바꿈은 호출자가 직접 포함 (디버거 출력 창의 줄 단위 그룹화 유지).
    void LogInfo(const wchar_t* message);
    void LogInfoA(const char*   message);
}
