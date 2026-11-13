#pragma once
// Orbit camera around a target point. Left-handed, Y up (same as Arma model space).
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

// Views named as in Blender after an a3ob import (P3D x, y, z -> Blender x, z, y; a3ob io/data_p3d.py:430):
// Front (Numpad 1) = camera at -Z looking +Z, Left (Ctrl+Numpad 3) = camera at -X, Right (Numpad 3) = camera at +X,
// Top (Numpad 7) = camera above. With yaw: camera position = target + (sin yaw, -, cos yaw) * distance.
constexpr float kViewFrontYaw = 3.14159265f, kViewBackYaw = 0.0f;
constexpr float kViewLeftYaw = -1.57079633f, kViewRightYaw = 1.57079633f;

class OrbitCamera
{
public:
    glm::vec3 target{ 0.0f, 1.0f, 0.0f };
    // Radians around Y. Camera position = target + (sin yaw, -, cos yaw) * distance.
    // MLOD data has the model's front at -Z and its left at +X (checked on characters and on cars:
    // wheel_1_1 = left front at +X/-Z), so the Object Builder "Front" view is yaw = pi.
    float yaw = 3.14159265f - 0.5f;
    float pitch = 0.25f;            // radians, positive = camera above target
    float distance = 4.0f;
    float fovY = glm::radians(45.0f);
    float nearZ = 0.01f, farZ = 2000.0f;

    glm::vec3 Forward() const // from camera towards target
    {
        return -glm::vec3(std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw));
    }
    glm::vec3 Position() const { return target - Forward() * distance; }
    glm::vec3 Right() const { return glm::normalize(glm::cross(glm::vec3(0, 1, 0), Forward())); }
    glm::vec3 Up() const { return glm::cross(Forward(), Right()); }

    glm::mat4 View() const { return glm::lookAt(Position(), target, glm::vec3(0, 1, 0)); }
    glm::mat4 Projection(float aspect) const { return glm::perspective(fovY, aspect, nearZ, farZ); }

    void Rotate(float dx, float dy)
    {
        yaw += dx * 0.008f;
        pitch = std::clamp(pitch + dy * 0.008f, -1.55f, 1.55f);
    }
    void Pan(float dx, float dy)
    {
        float k = distance * 0.0015f;
        target += (-Right() * dx + Up() * dy) * k;
    }
    void Zoom(float wheel) { distance = std::clamp(distance * std::pow(0.88f, wheel), 0.02f, 1500.0f); }

    void Frame(const glm::vec3& bmin, const glm::vec3& bmax)
    {
        target = (bmin + bmax) * 0.5f;
        float radius = std::max(glm::length(bmax - bmin) * 0.5f, 0.05f);
        distance = radius / std::sin(fovY * 0.5f) * 1.05f;
        nearZ = std::max(distance * 0.002f, 0.001f);
        farZ = distance * 50.0f + 100.0f;
    }
};
