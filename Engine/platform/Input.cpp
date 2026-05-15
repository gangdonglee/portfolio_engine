#include "platform/Input.h"

namespace engine::platform
{
    Input::Input()  = default;
    Input::~Input() = default;

    void Input::BeginFrame()
    {
        m_mouseDeltaX = m_mouseX - m_prevMouseX;
        m_mouseDeltaY = m_mouseY - m_prevMouseY;
        m_prevMouseX  = m_mouseX;
        m_prevMouseY  = m_mouseY;
    }

    void Input::OnKeyDown(uint32 vkey)
    {
        if (vkey < 256) m_keys[vkey] = true;
    }

    void Input::OnKeyUp(uint32 vkey)
    {
        if (vkey < 256) m_keys[vkey] = false;
    }

    void Input::OnMouseMove(int32 x, int32 y)
    {
        m_mouseX = x;
        m_mouseY = y;
    }

    void Input::OnMouseButton(int32 button, bool down)
    {
        if (button >= 0 && button < 3) m_buttons[button] = down;
    }

    bool Input::IsKeyDown(uint32 vkey) const noexcept
    {
        return vkey < 256 ? m_keys[vkey] : false;
    }

    bool Input::IsMouseButtonDown(int32 button) const noexcept
    {
        return (button >= 0 && button < 3) ? m_buttons[button] : false;
    }
}
