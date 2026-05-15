#include "render/Camera.h"

namespace engine::render
{
    Camera::Camera() = default;

    void Camera::SetPosition(const DirectX::XMFLOAT3& position)
    {
        m_position = position;
    }

    void Camera::SetTarget(const DirectX::XMFLOAT3& target)
    {
        m_target = target;
    }

    void Camera::SetUp(const DirectX::XMFLOAT3& up)
    {
        m_up = up;
    }

    void Camera::SetPerspective(float fovYRadians, float aspectRatio, float nearZ, float farZ)
    {
        m_fovY   = fovYRadians;
        m_aspect = aspectRatio;
        m_nearZ  = nearZ;
        m_farZ   = farZ;
    }

    DirectX::XMMATRIX Camera::View() const noexcept
    {
        using namespace DirectX;
        const XMVECTOR pos    = XMLoadFloat3(&m_position);
        const XMVECTOR target = XMLoadFloat3(&m_target);
        const XMVECTOR up     = XMLoadFloat3(&m_up);
        return XMMatrixLookAtLH(pos, target, up);
    }

    DirectX::XMMATRIX Camera::Projection() const noexcept
    {
        return DirectX::XMMatrixPerspectiveFovLH(m_fovY, m_aspect, m_nearZ, m_farZ);
    }

    DirectX::XMMATRIX Camera::ViewProjection() const noexcept
    {
        return DirectX::XMMatrixMultiply(View(), Projection());
    }
}
