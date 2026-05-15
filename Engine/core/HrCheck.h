#pragma once

#include <Windows.h>  // HRESULT

namespace engine::core
{
    // D3D12/DXGI/Win32 HRESULT 실패 시 std::runtime_error 던지는 공용 헬퍼.
    // what: 호출 식별자 — 정적 문자열 권장 (메시지에 그대로 포함).
    //
    // 메시지 형식: "<what> failed: HRESULT=0x<8 hex digits>"
    //
    // 본 함수는 모든 engine::render 의 .cpp 가 이전에 익명 네임스페이스로 중복 정의하던 것을
    // 단일 정의로 통합. 호출 측은 cpp 첫 부분에 다음 줄 추가만으로 기존 호출부 무변경:
    //   using engine::core::ThrowIfFailed;
    void ThrowIfFailed(HRESULT hr, const char* what);
}
