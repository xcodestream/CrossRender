//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: геометрия 3D-сцены: формат вершины, меши, материалы, камера и источники света.
//
#pragma once

#include "crossrender/core/Base.h"
#include "crossrender/core/Math.h"
#include "crossrender/gfx/Texture.h"

#include <memory>
#include <string>
#include <vector>

namespace crossrender {

class Input;
class Renderer3D;

// ---------------------------------------------------------------------------
// Вершина / меш
// ---------------------------------------------------------------------------
struct Vertex {
    Vec3 position;
    Vec3 normal{0, 1, 0};
    Vec2 uv;
    Vec4 tangent{1, 0, 0, 1};
    Vec4 color{1, 1, 1, 1};
    // Специфика вокселей: индекс палитры / ambient occlusion упакованы в uv2.
    Vec2 uv2;
};

enum class VertexAttribute : u32 {
    Position = 1u << 0,
    Normal = 1u << 1,
    UV = 1u << 2,
    Tangent = 1u << 3,
    Color = 1u << 4,
    UV2 = 1u << 5,
};

struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<u32> indices;
    Bounds bounds;
    std::string name;
    // Суб-меши / группы материалов (OBJ `usemtl`, примитивы glTF).
    struct SubMesh {
        u32 indexOffset = 0;
        u32 indexCount = 0;
        int materialIndex = 0;
        std::string name;
    };
    std::vector<SubMesh> subMeshes;

    void ComputeBounds();
    void ComputeNormals(bool smooth = true);
    void ComputeTangents();
    // Конструкторы базовых примитивов (в духе LowPoly, переключение flat/smooth).
    static MeshData Cube(f32 size = 1.0f, bool smoothNormals = false);
    static MeshData Sphere(f32 radius = 1.0f, int segments = 24, int rings = 16);
    static MeshData IcoSphere(f32 radius = 1.0f, int subdivisions = 2);
    static MeshData Plane(f32 width = 1.0f, f32 depth = 1.0f, int subdiv = 1,
                          Vec2 uvScale = {1, 1});
    static MeshData Cylinder(f32 radius = 0.5f, f32 height = 1.0f, int segments = 24, bool capped = true);
    static MeshData Cone(f32 radius = 0.5f, f32 height = 1.0f, int segments = 24);
    static MeshData Torus(f32 major = 1.0f, f32 minor = 0.25f, int majorSeg = 32, int minorSeg = 16);
    static MeshData Capsule(f32 radius = 0.5f, f32 height = 1.0f, int segments = 16, int rings = 8);
    static MeshData Quad(f32 w = 1.0f, f32 h = 1.0f);
    // Сливает несколько мешей (используется процедурным контентом примеров).
    static MeshData Merge(const std::vector<MeshData>& parts);
};

class Mesh {
public:
    Mesh() = default;
    ~Mesh();
    Mesh(Mesh&&) noexcept;
    Mesh& operator=(Mesh&&) noexcept;
    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;

    bool Create(const MeshData& data);
    bool Create(const void* vertices, usize vertexBytes, const std::vector<u32>& indices,
                const std::vector<u32>& layout);
    void Destroy();
    void Bind() const;
    void Draw() const;
    void DrawSub(u32 indexOffset, u32 indexCount) const;
    void DrawInstanced(int count) const;
    void DrawSubInstanced(u32 indexOffset, u32 indexCount, int count) const;

    [[nodiscard]] bool Valid() const { return vao_ != 0; }
    [[nodiscard]] u32 IndexCount() const { return indexCount_; }
    [[nodiscard]] u32 VertexCount() const { return vertexCount_; }
    [[nodiscard]] const Bounds& Bounds() const { return bounds_; }
    [[nodiscard]] const MeshData::SubMesh* SubMeshes() const { return subMeshes_.data(); }
    [[nodiscard]] int SubMeshCount() const { return static_cast<int>(subMeshes_.size()); }
    [[nodiscard]] const std::string& Name() const { return name_; }
    void SetName(std::string n) { name_ = n; }

    // Простой кэш статической геометрии для отладочной отрисовки / примитивов.
    static Mesh* GetPrimitive(const std::string& key);

private:
    u32 vao_ = 0, vbo_ = 0, ebo_ = 0;
    u32 indexCount_ = 0, vertexCount_ = 0;
    bool index32_ = true;
    struct Bounds bounds_;
    std::vector<MeshData::SubMesh> subMeshes_;
    std::string name_;
};

// ---------------------------------------------------------------------------
// Материал (workflow metallic-roughness, forward-затенение PBR-lite)
// ---------------------------------------------------------------------------
enum class AlphaMode : u8 { Opaque, Mask, Blend };
enum class CullMode : u8 { None, Back, Front };

struct Material {
    std::string name;
    Color baseColor{0.8f, 0.8f, 0.8f, 1.0f};
    f32 metallic = 0.0f;
    f32 roughness = 0.7f;
    Color emissive{0, 0, 0, 1};
    f32 emissiveStrength = 1.0f;
    f32 normalStrength = 1.0f;
    f32 occlusionStrength = 1.0f;
    f32 alphaCutoff = 0.5f;
    AlphaMode alphaMode = AlphaMode::Opaque;
    CullMode cullMode = CullMode::Back;
    bool doubleSided = false;
    bool receiveShadows = true;
    bool unlit = false;
    f32 pointSize = 1.0f;
    // Текстуры (nullptr = отсутствуют).
    const Texture* baseColorTex = nullptr;
    const Texture* normalTex = nullptr;
    const Texture* metallicRoughnessTex = nullptr;
    const Texture* emissiveTex = nullptr;
    const Texture* occlusionTex = nullptr;
    Vec2 uvScale{1, 1};
    Vec2 uvOffset{0, 0};
    // Тонирование вершинными цветами / палитрой (воксели и low-poly).
    bool vertexColors = false;
    Color tint{1, 1, 1, 1};

    // Пресеты, используемые в примерах сцен.
    static Material Default();
    static Material Unlit(const Color& c);
    static Material Checker();
    static Material Metal(const Color& c, f32 roughness);
    static Material Emissive(const Color& c, f32 strength);
};

// ---------------------------------------------------------------------------
// Камера
// ---------------------------------------------------------------------------
enum class ProjectionType : u8 { Perspective, Orthographic };

class Camera {
public:
    Vec3 position{0, 2, 6};
    Vec3 target{0, 0, 0};
    Vec3 up{0, 1, 0};
    f32 fovY = 60.0f * kDeg2Rad;
    f32 nearZ = 0.05f;
    f32 farZ = 500.0f;
    f32 orthoHeight = 10.0f;
    ProjectionType projection = ProjectionType::Perspective;

    [[nodiscard]] Vec3 Forward() const { return Normalize(target - position); }
    [[nodiscard]] Vec3 Right() const { return Normalize(Cross(Forward(), up)); }
    [[nodiscard]] Vec3 Up() const { return Cross(Right(), Forward()); }

    [[nodiscard]] Mat4 View() const { return Mat4::LookAt(position, target, up); }
    [[nodiscard]] Mat4 Proj(f32 aspect) const {
        if (projection == ProjectionType::Orthographic) {
            f32 h = orthoHeight * 0.5f;
            f32 w = h * aspect;
            return Mat4::Ortho(-w, w, -h, h, nearZ, farZ);
        }
        return Mat4::Perspective(fovY, aspect, nearZ, farZ);
    }
    [[nodiscard]] Mat4 ViewProj(f32 aspect) const { return Proj(aspect) * View(); }
    // Луч в мировом пространстве через нормализованную координату устройства из [-1,1].
    void RayFromNdc(f32 ndcX, f32 ndcY, f32 aspect, Vec3* origin, Vec3* dir) const;
    // Обратная проекция из экранного пространства (0..1, y вниз), используется при пикинге.
    void RayFromScreen(Vec2 screenPos, Vec2 viewportSize, Vec3* origin, Vec3* dir) const;
    // Проецирует мировую точку в экранные координаты (0..1, y вниз); возвращает false, если она позади.
    bool WorldToScreen(const Vec3& world, Vec2 viewportSize, Vec2* screenOut) const;
};

// Контроллер орбитальной/свободной камеры из примера приложения.
class CameraController {
public:
    void Update(Camera& cam, const Input& input, f32 dt, bool active);
    void Orbit(f32 dx, f32 dy);
    void Zoom(f32 delta);
    void Pan(f32 dx, f32 dy);
    void SetOrbit(f32 yaw, f32 pitch, f32 distance, Vec3 target = {0, 0, 0});
    [[nodiscard]] f32 Distance() const { return distance_; }
    [[nodiscard]] f32 Yaw() const { return yaw_; }
    [[nodiscard]] f32 Pitch() const { return pitch_; }
    [[nodiscard]] Vec3 Target() const { return target_; }
    void SetTarget(const Vec3& t) { target_ = t; }
    void SetDistance(f32 d) { distance_ = d; }
    [[nodiscard]] f32 MoveSpeed() const { return moveSpeed_; }
    void SetMoveSpeed(f32 s) { moveSpeed_ = s; }

private:
    f32 yaw_ = 0.6f, pitch_ = 0.4f, distance_ = 8.0f;
    f32 moveSpeed_ = 6.0f;
    Vec3 target_{0, 0, 0};
};

// ---------------------------------------------------------------------------
// Источники света
// ---------------------------------------------------------------------------
enum class LightType : u8 { Directional, Point, Spot, Area };

struct Light {
    LightType type = LightType::Directional;
    Vec3 position{0, 5, 0};
    Vec3 direction{0, -1, 0};
    Color color{1, 1, 1, 1};
    f32 intensity = 1.0f;
    f32 range = 10.0f;
    f32 innerCone = 20.0f * kDeg2Rad;
    f32 outerCone = 35.0f * kDeg2Rad;
    // Только для area light.
    Vec2 areaSize{1, 1};
    bool castShadows = false;
    f32 shadowBias = 0.0025f;
    f32 shadowNormalBias = 0.02f;
    int shadowMapSize = 1024;
    // Идентификатор, назначаемый Renderer3D во время выполнения.
    int id = -1;

    static Light Directional(const Vec3& dir, const Color& c, f32 intensity, bool shadows = true);
    static Light Point(const Vec3& pos, const Color& c, f32 intensity, f32 range, bool shadows = false);
    static Light Spot(const Vec3& pos, const Vec3& dir, const Color& c, f32 intensity, f32 range,
                      f32 inner, f32 outer, bool shadows = true);
};

struct Environment {
    Color ambientSky{0.35f, 0.45f, 0.6f, 1.0f};
    Color ambientGround{0.15f, 0.13f, 0.12f, 1.0f};
    f32 ambientIntensity = 0.35f;
    // Градиент неба (также используется для отрисовки фонового квада).
    Color skyTop{0.16f, 0.28f, 0.55f, 1.0f};
    Color skyHorizon{0.62f, 0.72f, 0.86f, 1.0f};
    Color skyBottom{0.25f, 0.24f, 0.26f, 1.0f};
    bool drawSky = true;
    bool drawGrid = true;
    // Туман
    bool fogEnabled = false;
    Color fogColor{0.6f, 0.68f, 0.8f, 1.0f};
    f32 fogDensity = 0.02f;
    f32 fogStart = 5.0f, fogEnd = 60.0f;
    int fogMode = 0;  // 0 — линейный, 1 — экспоненциальный
    // Аппроксимация image based lighting (аналитическая облучённость).
    bool useIbl = true;
};

}  // namespace crossrender
