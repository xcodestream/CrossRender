#include "crossrender/gfx/Mesh.h"

#include "crossrender/core/Log.h"
#include "crossrender/platform/Window.h"

namespace crossrender {

void Camera::RayFromNdc(f32 ndcX, f32 ndcY, f32 aspect, Vec3* origin, Vec3* dir) const {
    Mat4 invVP = ViewProj(aspect).Inverse();
    Vec4 nearP = invVP * Vec4{ndcX, ndcY, -1.0f, 1.0f};
    Vec4 farP = invVP * Vec4{ndcX, ndcY, 1.0f, 1.0f};
    Vec3 n = nearP.w != 0 ? Vec3{nearP.x / nearP.w, nearP.y / nearP.w, nearP.z / nearP.w} : nearP.xyz();
    Vec3 f = farP.w != 0 ? Vec3{farP.x / farP.w, farP.y / farP.w, farP.z / farP.w} : farP.xyz();
    if (origin) *origin = n;
    if (dir) *dir = Normalize(f - n);
}

void Camera::RayFromScreen(Vec2 screenPos, Vec2 viewportSize, Vec3* origin, Vec3* dir) const {
    if (viewportSize.x <= 0 || viewportSize.y <= 0) {
        if (dir) *dir = Forward();
        if (origin) *origin = position;
        return;
    }
    f32 ndcX = (screenPos.x / viewportSize.x) * 2.0f - 1.0f;
    f32 ndcY = 1.0f - (screenPos.y / viewportSize.y) * 2.0f;
    RayFromNdc(ndcX, ndcY, viewportSize.x / viewportSize.y, origin, dir);
}

bool Camera::WorldToScreen(const Vec3& world, Vec2 viewportSize, Vec2* screenOut) const {
    if (viewportSize.x <= 0 || viewportSize.y <= 0) return false;
    Mat4 vp = ViewProj(viewportSize.x / viewportSize.y);
    Vec4 clip = vp * Vec4{world, 1.0f};
    if (clip.w <= 0.0001f) return false;
    Vec3 ndc{clip.x / clip.w, clip.y / clip.w, clip.z / clip.w};
    if (screenOut) {
        screenOut->x = (ndc.x * 0.5f + 0.5f) * viewportSize.x;
        screenOut->y = (1.0f - (ndc.y * 0.5f + 0.5f)) * viewportSize.y;
    }
    return true;
}

// ---------------------------------------------------------------------------
// CameraController (по умолчанию орбита, полёт на WASD при зажатой ЛКМ/ПКМ)
// ---------------------------------------------------------------------------
void CameraController::SetOrbit(f32 yaw, f32 pitch, f32 distance, Vec3 target) {
    yaw_ = yaw;
    pitch_ = Clamp(pitch, -1.55f, 1.55f);
    distance_ = Clamp(distance, 0.2f, 5000.0f);
    target_ = target;
}

void CameraController::Orbit(f32 dx, f32 dy) {
    yaw_ -= dx * 0.01f;
    pitch_ = Clamp(pitch_ + dy * 0.01f, -1.5533f, 1.5533f);
}

void CameraController::Zoom(f32 delta) {
    distance_ = Clamp(distance_ * (1.0f - delta * 0.1f), 0.3f, 2000.0f);
}

void CameraController::Pan(f32 dx, f32 dy) {
    // Панорамирование в экранной плоскости камеры, с масштабом от дистанции.
    Vec3 forward{std::cos(pitch_) * std::sin(yaw_), std::sin(pitch_), std::cos(pitch_) * std::cos(yaw_)};
    Vec3 right = Normalize(Cross(forward, Vec3{0, 1, 0}));
    Vec3 up = Normalize(Cross(right, forward));
    f32 scale = distance_ * 0.0015f;
    target_ -= right * (dx * scale);
    target_ += up * (dy * scale);
}

void CameraController::Update(Camera& cam, const Input& input, f32 dt, bool active) {
    if (active) {
        Vec2 delta = input.MouseDelta();
        Vec2 scroll = input.ScrollDelta();

        bool orbiting = input.MouseDown(MouseButton::Left) || input.MouseDown(MouseButton::Right);
        if (orbiting) {
            Orbit(delta.x, delta.y);
        }
        if (input.MouseDown(MouseButton::Middle)) {
            Pan(delta.x, delta.y);
        }
        if (std::fabs(scroll.y) > 0.0001f) Zoom(scroll.y);

        // Полёт на WASD/QE: перемещает цель орбиты.
        f32 speed = moveSpeed_ * (input.ShiftDown() ? 3.0f : 1.0f) * dt;
        if (input.KeyDown(Key::W) || input.KeyDown(Key::S) || input.KeyDown(Key::A) ||
            input.KeyDown(Key::D) || input.KeyDown(Key::E) || input.KeyDown(Key::Q)) {
            Vec3 forward{std::cos(pitch_) * std::sin(yaw_), std::sin(pitch_),
                         std::cos(pitch_) * std::cos(yaw_)};
            Vec3 right = Normalize(Cross(forward, Vec3{0, 1, 0}));
            if (input.KeyDown(Key::W)) target_ += forward * speed;
            if (input.KeyDown(Key::S)) target_ -= forward * speed;
            if (input.KeyDown(Key::D)) target_ += right * speed;
            if (input.KeyDown(Key::A)) target_ -= right * speed;
            if (input.KeyDown(Key::E)) target_.y += speed;
            if (input.KeyDown(Key::Q)) target_.y -= speed;
        }
    }

    // Построение трансформации камеры из параметров орбиты.
    f32 cp = std::cos(pitch_);
    Vec3 offset{cp * std::sin(yaw_) * distance_, std::sin(pitch_) * distance_,
                cp * std::cos(yaw_) * distance_};
    cam.position = target_ + offset;
    cam.target = target_;
    cam.up = {0, 1, 0};
}

}  // namespace crossrender
