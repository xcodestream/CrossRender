//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: 3D-рендерер (forward PBR-lite): каскадные тени, небо и отладочная отрисовка.
//
#pragma once

#include "crossrender/gfx/Mesh.h"
#include "crossrender/core/Base.h"
#include "crossrender/core/Math.h"
#include "crossrender/gfx/RenderTarget.h"

#include <memory>
#include <vector>
#include <functional>

namespace crossrender {

struct ShadowSettings {
    bool enabled = true;
    int directionalMapSize = 2048;
    int cascadeCount = 3;
    f32 cascadeSplitLambda = 0.85f;
    f32 cascadeDistance = 60.0f;
    f32 depthBias = 0.0025f;
    f32 normalBias = 0.02f;
    f32 pcfRadius = 1.5f;
    int pointShadowMapSize = 512;
    int maxShadowCasters = 256;
    bool softShadows = true;
};

struct Renderer3DSettings {
    bool frustumCulling = true;
    bool sortTransparent = true;
    bool wireframe = false;          // отладка: рисовать меши линиями
    bool showShadowCascades = false; // отладка: тонировать по каскаду
    bool depthPrepass = false;
    bool backfaceCulling = true;
    int maxLights = 8;
    f32 lodBias = 1.0f;
};

struct Render3DStats {
    int drawCalls = 0;
    int triangles = 0;
    int vertices = 0;
    int shadowCasters = 0;
    int lights = 0;
    int instancedDraws = 0;
    int culledObjects = 0;
    int lines = 0;
    f32 cpuFrameMs = 0;
};

struct Ray {
    Vec3 origin;
    Vec3 direction;
};

struct RayHit {
    bool hit = false;
    f32 distance = 0;
    Vec3 point;
    Vec3 normal{0, 1, 0};
    Vec2 uv;
    int objectId = -1;
    int triangleIndex = -1;
    std::string meshName;
    const void* userData = nullptr;
};

// Регистрирует геометрию сцены для CPU-пикинга / физических запросов.
struct PickableObject {
    const Mesh* mesh = nullptr;
    Mat4 transform;
    Mat4 inverse;
    int id = -1;
    void* userData = nullptr;
};

class Renderer3D {
public:
    Renderer3D();
    ~Renderer3D();
    Renderer3D(const Renderer3D&) = delete;
    Renderer3D& operator=(const Renderer3D&) = delete;

    bool Init();
    void Shutdown();

    // Начинает 3D-кадр. При `target` == null используется фреймбуфер по умолчанию.
    void BeginFrame(const Camera& camera, int fbWidth, int fbHeight, RenderTarget* target = nullptr,
                    bool clearColor = true, bool clearDepth = true);
    void EndFrame();

// Задаёт вьюпорт внутри текущего фреймбуфера (прямоугольник в пикселях, y вниз).
// Позволяет отрисовать 3D-сцену в подобласть, чтобы смешать её с 2D UI.
    void SetViewport(f32 x, f32 y, f32 w, f32 h);
    void ResetViewport();

    [[nodiscard]] const Camera& GetCamera() const { return camera_; }
    void SetCamera(const Camera& c) { camera_ = c; }
    [[nodiscard]] Renderer3DSettings& Settings() { return settings_; }
    [[nodiscard]] ShadowSettings& Shadows() { return shadows_; }
    [[nodiscard]] Environment& GetEnvironment() { return env_; }
    void SetEnvironment(const Environment& e) { env_ = e; }

    // ---- источники света --------------------------------------------------------
    int AddLight(const Light& light);
    void ClearLights();
    [[nodiscard]] Light* GetLight(int index);
    [[nodiscard]] int LightCount() const { return static_cast<int>(lights_.size()); }
    [[nodiscard]] int MaxLights() const { return settings_.maxLights; }

    // ---- отрисовка -------------------------------------------------------
    void Draw(const Mesh& mesh, const Material& material, const Mat4& transform = Mat4::Identity());
    void Draw(const Mesh& mesh, const Mat4& transform) { Draw(mesh, Material::Default(), transform); }
    void DrawInstanced(const Mesh& mesh, const Material& material, const Mat4* transforms, int count);
    // Рисует меш, используя внешний буфер экземпляров Mat4.
    void DrawInstancedBuffer(const Mesh& mesh, const Material& material, u32 instanceBuffer,
                             int count);
// Ставит в очередь колбэк, выполняемый в самом конце кадра — после непрозрачной
// и прозрачной геометрии (чтобы порядок по глубине был верным) и перед проходом
// отладочных линий. Колбэк сам владеет своим GL-состоянием и шейдером. Так
// кастомные проходы (например, 3D-частицы) накладываются с корректной глубиной.
    void AddPostDraw(std::function<void()> fn);
    void DrawSky();
    void DrawGrid(f32 size = 20.0f, int divisions = 20, const Color& major = Color::FromRGB(0x3A3A44),
                  const Color& minor = Color::FromRGB(0x26262E));

    // ---- отладочная отрисовка -------------------------------------------------
    void DrawLine(const Vec3& a, const Vec3& b, const Color& color, bool depthTest = true,
                  f32 width = 1.0f);
    void DrawLines(const Vec3* points, int count, const Color& color, bool depthTest = true);
    void DrawWireBox(const Vec3& center, const Vec3& extents, const Color& color,
                     const Mat4& transform = Mat4::Identity());
    void DrawWireBox(const Bounds& b, const Color& color, const Mat4& transform = Mat4::Identity());
    void DrawAabb(const Bounds& b, const Color& color);
    void DrawSphere(const Vec3& center, f32 radius, const Color& color, int segments = 16);
    void DrawArrow(const Vec3& from, const Vec3& to, const Color& color, f32 headSize = 0.15f);
    void DrawGizmo(const Mat4& transform, f32 scale = 1.0f, bool depthTest = false);
    void DrawCapsule(const Vec3& a, const Vec3& b, f32 radius, const Color& color);
    void DrawFrustum(const Mat4& viewProj, const Color& color);

    // ---- пикинг -------------------------------------------------------
    void SubmitPickable(const Mesh& mesh, const Mat4& transform, int id, void* userData = nullptr);
    void ClearPickables();
    // Луч с точностью до треугольника по зарегистрированным объектам пикинга.
    bool Raycast(const Ray& ray, RayHit* hit, f32 maxDistance = 1e30f);
    // Пикинг по экранным координатам.
    bool PickAtScreen(Vec2 screenPos, Vec2 viewportSize, RayHit* hit);

    // ---- помощники -------------------------------------------------------
    [[nodiscard]] const Render3DStats& GetStats() const { return stats_; }
    void ResetStats() { stats_ = Render3DStats{}; }
    [[nodiscard]] const Mat4& ViewProj() const { return viewProj_; }
    // HDR-текстура сцены при рендеринге через PostProcessor (может быть null).
    [[nodiscard]] Texture* SceneColor() { return sceneColor_; }
    void SetSceneColor(Texture* t) { sceneColor_ = t; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    Camera camera_;
    Environment env_{};
    Renderer3DSettings settings_{};
    ShadowSettings shadows_{};
    std::vector<Light> lights_;
    std::vector<PickableObject> pickables_;
    Mat4 viewProj_;
    Render3DStats stats_{};
    Texture* sceneColor_ = nullptr;
    int fbWidth_ = 1, fbHeight_ = 1;
};

}  // namespace crossrender
