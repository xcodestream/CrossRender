// Renderer3D: прямой PBR-lite рендерер с каскадными теневыми картами, небом,
// сеткой, отладочными линиями, GPU-инстансингом и CPU-пикингом по треугольникам.
//
// ---------------------------------------------------------------------------
// Модель кадра (важно)
// ---------------------------------------------------------------------------
// Отправка отложена (DEFERRED). `Draw()`, `DrawInstanced()`, `DrawGrid()`,
// `DrawSky()`, `DrawLine()` … пишут в покадровые CPU-списки; в момент вызова
// ничего не растеризуется. Затем `EndFrame()` прогоняет весь пайплайн в
// единственно возможном корректном порядке:
//
//   1. отсечение + классификация + сортировка записанных элементов
//   2. теневые проходы направленного света по непрозрачным кастерам
//   3. опциональный depth prepass
//   4. небо
//   5. непрозрачные / alpha-masked элементы, спереди назад
//   6. сетка
//   7. элементы с альфа-смешением, сзади наперёд (запись глубины выключена)
//   8. отладочные линии (батч с depth-тестом, затем батч поверх всего)
//
// Камера, источники света и окружение читаются в момент EndFrame, поэтому их
// можно менять после BeginFrame(). `Material`, переданный в Draw(), копируется
// в список кадра (Draw(mesh, xform) принимает временный объект), но текстуры,
// на которые он указывает, должны жить до EndFrame().
//
// `BeginFrame()` привязывает render target и очищает его; `EndFrame()` оставляет
// цель привязанной, чтобы вызывающий мог сделать resolve
// (RenderTarget::Unbind() / деструктор ScopedRenderTarget).
// ---------------------------------------------------------------------------
#include "crossrender/gfx/Renderer3D.h"

#include "crossrender/gfx/GL.h"
#include "crossrender/core/Log.h"
#include "crossrender/gfx/Shader.h"
#include "crossrender/gfx/Texture.h"

#include "gfx/Renderer3DInternal.h"

#include <cmath>
#include <chrono>
#include <vector>
#include <cstddef>
#include <algorithm>

namespace crossrender {
namespace r3d_internal {

// ---------------------------------------------------------------------------
// Отсечение по фрустуму
// ---------------------------------------------------------------------------
void ExtractFrustumPlanes(const Mat4& viewProj, Plane out[6]) {
    // Строки (column-major) матрицы: row(i) = (m[0*4+i], m[1*4+i], m[2*4+i], m[3*4+i]).
    auto row = [&viewProj](int i, int c) { return viewProj.at(c, i); };
    auto set = [&](int index, int a, int b, f32 sign) {
        Vec3 n{row(a, 0) + sign * row(b, 0), row(a, 1) + sign * row(b, 1),
               row(a, 2) + sign * row(b, 2)};
        f32 d = row(a, 3) + sign * row(b, 3);
        const f32 len = Length(n);
        if (len > 1e-8f) {
            n = n / len;
            d /= len;
        } else {
            n = Vec3{0, 1, 0};
            d = 1e30f;  // вырожденная плоскость: ничего не отсекает
        }
        out[index].normal = n;
        out[index].d = d;
    };
    // Внутри: dot(n, p) + d >= 0.
    set(0, 3, 0, 1.0f);   // левая:   w + x >= 0
    set(1, 3, 0, -1.0f);  // правая:  w - x >= 0
    set(2, 3, 1, 1.0f);   // нижняя:  w + y >= 0
    set(3, 3, 1, -1.0f);  // верхняя: w - y >= 0
    set(4, 3, 2, 1.0f);   // ближняя: w + z >= 0
    set(5, 3, 2, -1.0f);  // дальняя: w - z >= 0
}

bool AabbInFrustum(const Plane planes[6], const Vec3& lo, const Vec3& hi) {
    for (int i = 0; i < 6; ++i) {
        const Plane& p = planes[i];
        // Положительная вершина: угол бокса, наиболее удалённый вдоль нормали плоскости.
        const Vec3 positive{p.normal.x >= 0 ? hi.x : lo.x, p.normal.y >= 0 ? hi.y : lo.y,
                            p.normal.z >= 0 ? hi.z : lo.z};
        if (Dot(p.normal, positive) + p.d < 0.0f) return false;
    }
    return true;
}

bool AabbInFrustum(const Mat4& viewProj, const Vec3& lo, const Vec3& hi) {
    Plane planes[6];
    ExtractFrustumPlanes(viewProj, planes);
    return AabbInFrustum(planes, lo, hi);
}

void TransformAabb(const Mat4& m, const Vec3& lo, const Vec3& hi, Vec3* outLo, Vec3* outHi) {
    Vec3 mn{1e30f, 1e30f, 1e30f};
    Vec3 mx{-1e30f, -1e30f, -1e30f};
    for (int i = 0; i < 8; ++i) {
        const Vec3 corner{(i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z};
        const Vec3 p = m.TransformPoint(corner);
        mn = Min(mn, p);
        mx = Max(mx, p);
    }
    if (outLo) *outLo = mn;
    if (outHi) *outHi = mx;
}

// ---------------------------------------------------------------------------
// Каскады теней
// ---------------------------------------------------------------------------
void ComputeCascadeSplits(f32 nearZ, f32 farZ, f32 lambda, int count, f32* out) {
    if (!out || count <= 0) return;
    count = Clamp(count, 1, kMaxCascades);
    if (nearZ < 1e-4f) nearZ = 1e-4f;
    if (farZ <= nearZ) farZ = nearZ + 1e-3f;
    lambda = Clamp(lambda, 0.0f, 1.0f);
    for (int i = 0; i < count; ++i) {
        const f32 p = static_cast<f32>(i + 1) / static_cast<f32>(count);
        const f32 logSplit = nearZ * std::pow(farZ / nearZ, p);
        const f32 uniformSplit = nearZ + (farZ - nearZ) * p;
        out[i] = lambda * logSplit + (1.0f - lambda) * uniformSplit;
    }
    // Смешанная схема уже монотонна; страхуемся от дрейфа float и фиксируем
    // последний сплит точно: "дальше этого = без тени".
    for (int i = 1; i < count; ++i) {
        if (out[i] <= out[i - 1]) out[i] = out[i - 1] + 1e-4f;
    }
    out[count - 1] = farZ;
}

// ---------------------------------------------------------------------------
// Математика пикинга
// ---------------------------------------------------------------------------
bool RayAabbTest(const Vec3& origin, const Vec3& dir, const Vec3& lo, const Vec3& hi, f32* tMin) {
    f32 tNear = -1e30f;
    f32 tFar = 1e30f;
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(dir[i]) < 1e-9f) {
            if (origin[i] < lo[i] || origin[i] > hi[i]) return false;
            continue;
        }
        const f32 inv = 1.0f / dir[i];
        f32 t1 = (lo[i] - origin[i]) * inv;
        f32 t2 = (hi[i] - origin[i]) * inv;
        if (t1 > t2) std::swap(t1, t2);
        if (t1 > tNear) tNear = t1;
        if (t2 < tFar) tFar = t2;
        if (tNear > tFar) return false;
    }
    if (tFar < 0.0f) return false;
    if (tMin) *tMin = tNear >= 0.0f ? tNear : tFar;
    return true;
}

bool RayTriangle(const Vec3& origin, const Vec3& dir, const Vec3& v0, const Vec3& v1, const Vec3& v2,
                 f32* tOut, f32* uOut, f32* vOut) {
    const Vec3 e1 = v1 - v0;
    const Vec3 e2 = v2 - v0;
    const Vec3 p = Cross(dir, e2);
    const f32 det = Dot(e1, p);
    if (std::fabs(det) < 1e-9f) return false;  // луч параллелен треугольнику
    const f32 invDet = 1.0f / det;
    const Vec3 tv = origin - v0;
    const f32 u = Dot(tv, p) * invDet;
    if (u < -1e-5f || u > 1.0f + 1e-5f) return false;
    const Vec3 q = Cross(tv, e1);
    const f32 v = Dot(dir, q) * invDet;
    if (v < -1e-5f || u + v > 1.0f + 1e-5f) return false;
    const f32 t = Dot(e2, q) * invDet;
    if (t < 0.0f) return false;
    if (tOut) *tOut = t;
    if (uOut) *uOut = u;
    if (vOut) *vOut = v;
    return true;
}

bool RayMeshTriangles(const Vec3& origin, const Vec3& dir, const MeshData& data, f32 maxDistance,
                      f32* tOut, int* triOut, Vec3* normalOut, Vec2* uvOut) {
    const usize triCount = data.indices.size() / 3;
    f32 best = maxDistance;
    int bestTri = -1;
    f32 bestU = 0.0f, bestV = 0.0f;
    for (usize t = 0; t < triCount; ++t) {
        const u32 i0 = data.indices[t * 3 + 0];
        const u32 i1 = data.indices[t * 3 + 1];
        const u32 i2 = data.indices[t * 3 + 2];
        if (i0 >= data.vertices.size() || i1 >= data.vertices.size() || i2 >= data.vertices.size())
            continue;
        const Vec3& p0 = data.vertices[i0].position;
        const Vec3& p1 = data.vertices[i1].position;
        const Vec3& p2 = data.vertices[i2].position;
        f32 hitT = 0.0f, u = 0.0f, v = 0.0f;
        if (!RayTriangle(origin, dir, p0, p1, p2, &hitT, &u, &v)) continue;
        if (hitT >= best) continue;
        best = hitT;
        bestTri = static_cast<int>(t);
        bestU = u;
        bestV = v;
    }
    if (bestTri < 0) return false;
    if (tOut) *tOut = best;
    if (triOut) *triOut = bestTri;
    const usize t = static_cast<usize>(bestTri);
    const Vertex& a = data.vertices[data.indices[t * 3 + 0]];
    const Vertex& b = data.vertices[data.indices[t * 3 + 1]];
    const Vertex& c = data.vertices[data.indices[t * 3 + 2]];
    const f32 w = 1.0f - bestU - bestV;
    if (normalOut) {
        const Vec3 smooth = a.normal * w + b.normal * bestU + c.normal * bestV;
        if (LengthSq(smooth) > 1e-12f)
            *normalOut = Normalize(smooth);
        else
            *normalOut = Normalize(Cross(b.position - a.position, c.position - a.position));
    }
    if (uvOut) *uvOut = a.uv * w + b.uv * bestU + c.uv * bestV;
    return true;
}

// ---------------------------------------------------------------------------
// Привязка мешей на CPU
// ---------------------------------------------------------------------------
namespace {
struct MeshDataEntry {
    const Mesh* mesh;
    const MeshData* data;
};
std::vector<MeshDataEntry>& MeshDataRegistry() {
    static std::vector<MeshDataEntry> registry;
    return registry;
}
}  // namespace

void RegisterMeshData(const Mesh* mesh, const MeshData* data) {
    if (!mesh || !data) return;
    auto& reg = MeshDataRegistry();
    for (MeshDataEntry& e : reg) {
        if (e.mesh == mesh) {
            e.data = data;
            return;
        }
    }
    reg.push_back({mesh, data});
}

void UnregisterMeshData(const Mesh* mesh) {
    auto& reg = MeshDataRegistry();
    for (usize i = 0; i < reg.size(); ++i) {
        if (reg[i].mesh == mesh) {
            reg[i] = reg.back();
            reg.pop_back();
            return;
        }
    }
}

const MeshData* FindMeshData(const Mesh* mesh) {
    if (!mesh) return nullptr;
    for (const MeshDataEntry& e : MeshDataRegistry()) {
        if (e.mesh == mesh) return e.data;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Упаковка источников света
// ---------------------------------------------------------------------------
i32 LightTypeToUniform(LightType type) {
    switch (type) {
        case LightType::Directional: return 0;
        case LightType::Point: return 1;
        case LightType::Spot: return 2;
        case LightType::Area: return 3;
    }
    return 0;
}

Vec2 SpotConeCosines(f32 innerRad, f32 outerRad) {
    // Держим outer <= pi/2 - eps и inner <= outer: шейдер делит на
    // (cos(inner) - cos(outer)), что должно оставаться строго положительным.
    outerRad = Clamp(outerRad, 1e-3f, kPi * 0.5f - 1e-3f);
    innerRad = Clamp(innerRad, 1e-3f, outerRad);
    return Vec2{std::cos(innerRad), std::cos(outerRad)};
}

int PackLights(const Light* lights, int lightCount, int maxLights, PackedLights* out) {
    if (!out) return 0;
    *out = PackedLights{};
    if (!lights || lightCount <= 0 || maxLights <= 0) return 0;
    const int n = Clamp(lightCount, 0, MinT(maxLights, kMaxLights));
    for (int i = 0; i < n; ++i) {
        const Light& l = lights[i];
        const Vec3 dir = LengthSq(l.direction) > 1e-12f ? Normalize(l.direction) : Vec3{0, -1, 0};
        out->type[i] = LightTypeToUniform(l.type);
        out->position[i] = l.position;
        out->direction[i] = dir;
        out->color[i] = l.color.rgb();
        out->intensity[i] = l.intensity;
        out->range[i] = l.range > 1e-4f ? l.range : 1e-4f;
        out->cone[i] = SpotConeCosines(l.innerCone, l.outerCone);
        out->area[i] = l.areaSize;
    }
    out->count = n;
    return n;
}

}  // namespace r3d_internal

namespace {
// ---------------------------------------------------------------------------
// Настраиваемые константы
// ---------------------------------------------------------------------------
// Текстурные блоки 0..4 заняты пятью картами материала; теневые карты начинаются выше.
constexpr int kShadowMapUnitBase = 5;
constexpr usize kMaxLineVertices = 262144;
const Color kFrameClearColor{0.055f, 0.063f, 0.078f, 1.0f};
const Vec3 kCascadeDebugTint[3] = {{1.0f, 0.55f, 0.55f}, {0.55f, 1.0f, 0.55f}, {0.6f, 0.6f, 1.0f}};

// ---------------------------------------------------------------------------
// Локальные шейдеры
//
// Встроенный прямой вершинный шейдер (builtin::kForwardVert) не читает
// поэкземплярные атрибуты, поэтому DrawInstanced() нужна своя вершинная
// стадия. Она держится байт-совместимой со встроенным фрагментным шейдером
// (те же varying, те же имена uniform) и дословно переиспользует builtin::kForwardFrag.
//
// Матрицы экземпляров приходят как 4 столбца vec4 на атрибутах 6..9
// (локации 0..5 — вершинный расклад Mesh). Преобразования экземпляров должны
// быть жёсткими или с равномерным масштабом: нормаль корректируется
// normal-матрицей базовой модели, а неравномерный масштаб экземпляра потребовал
// бы индивидуальной обратной транспонированной матрицы.
// ---------------------------------------------------------------------------
const char* kInstancedForwardVert = R"GLSL(
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aTangent;
layout(location = 4) in vec4 aColor;
layout(location = 5) in vec2 aUV2;
layout(location = 6) in vec4 aInstance0;
layout(location = 7) in vec4 aInstance1;
layout(location = 8) in vec4 aInstance2;
layout(location = 9) in vec4 aInstance3;

uniform mat4 uModel;
uniform mat4 uViewProj;
uniform mat4 uNormalMatrix;

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUV;
out vec2 vUV2;
out vec4 vTangent;
out vec4 vColor;

void main() {
    mat4 instance = mat4(aInstance0, aInstance1, aInstance2, aInstance3);
    mat4 model = uModel * instance;
    vec4 wp = model * vec4(aPos, 1.0);
    vWorldPos = wp.xyz;
    vNormal = normalize(mat3(uNormalMatrix) * mat3(instance) * aNormal);
    vTangent = vec4(normalize(mat3(model) * aTangent.xyz), aTangent.w);
    vUV = aUV;
    vUV2 = aUV2;
    vColor = aColor;
    gl_Position = uViewProj * wp;
}
)GLSL";

// Отладочные линии и сетка делят одну программу: мировая позиция плюс цвет
// на вершину, смешение straight alpha.
const char* kLineVert = R"GLSL(
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;
uniform mat4 uViewProj;
out vec4 vColor;
void main() {
    vColor = aColor;
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
)GLSL";

const char* kLineFrag = R"GLSL(
in vec4 vColor;
out vec4 fragColor;
void main() {
    fragColor = vColor;
}
)GLSL";
}  // namespace

namespace r3d_detail {

// Одна отправленная отрисовка. Material копируется, потому что Draw() принимает
// временные объекты (двухаргументная перегрузка передаёт Material::Default());
// текстурами по-прежнему владеет вызывающий.
struct DrawItem {
    const Mesh* mesh = nullptr;
    Material material;
    Mat4 transform;
    Mat4 normalMatrix;
    Vec3 worldMin{0, 0, 0};
    Vec3 worldMax{0, 0, 0};
    f32 depth = 0.0f;      // квадрат расстояния до камеры (ключ сортировки)
    bool transparent = false;
    bool culled = false;
    int instanceCount = 0;   // > 0 для инстансированных отрисовок
    int instanceOffset = 0;  // индекс в Impl::instancePool
    u32 externalBuffer = 0;  // не ноль для DrawInstancedBuffer
};

struct LineVertex {
    Vec3 position;
    Color color;
    f32 width = 1.0f;
    u8 depthTest = 1;
};

void PushLine(std::vector<LineVertex>& out, const Vec3& a, const Vec3& b, const Color& c, bool depth,
              f32 width) {
    const u8 d = depth ? 1u : 0u;
    out.push_back({a, c, width, d});
    out.push_back({b, c, width, d});
}

// Окружность через `center`, натянутая на (уже масштабированные) базисные векторы.
void PushCircle(std::vector<LineVertex>& out, const Vec3& center, const Vec3& axisU, const Vec3& axisV,
                const Color& c, int segments, bool depth, f32 width) {
    segments = MaxT(3, segments);
    Vec3 prev = center + axisU;
    for (int i = 1; i <= segments; ++i) {
        const f32 a = static_cast<f32>(i) / static_cast<f32>(segments) * kTau;
        const Vec3 p = center + axisU * std::cos(a) + axisV * std::sin(a);
        PushLine(out, prev, p, c, depth, width);
        prev = p;
    }
}

void PushArrow(std::vector<LineVertex>& out, const Vec3& from, const Vec3& to, const Color& c,
               f32 headSize, bool depth, f32 width) {
    const Vec3 delta = to - from;
    const f32 len = Length(delta);
    if (len < 1e-6f) return;
    const Vec3 dir = delta / len;
    PushLine(out, from, to, c, depth, width);
    Vec3 side = std::fabs(dir.y) > 0.95f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    const Vec3 u = Normalize(Cross(dir, side));
    const Vec3 v = Cross(dir, u);
    const f32 head = Clamp(headSize, len * 0.02f, len * 0.9f);
    const Vec3 base = to - dir * head;
    const int kSegments = 6;
    for (int i = 0; i < kSegments; ++i) {
        const f32 a = static_cast<f32>(i) / kSegments * kTau;
        const Vec3 p = base + (u * std::cos(a) + v * std::sin(a)) * (head * 0.45f);
        PushLine(out, to, p, c, depth, width);
    }
}

}  // namespace r3d_detail

// ---------------------------------------------------------------------------
// Renderer3D::Impl
// ---------------------------------------------------------------------------
struct Renderer3D::Impl {
    bool initialized = false;
    bool warnedNoContext = false;

    Shader forward;
    Shader forwardInstanced;
    Shader shadow;
    Shader sky;
    Shader line;

    // Отладочная геометрия
    u32 lineVao = 0, lineVbo = 0;
    u32 gridVao = 0, gridVbo = 0;
    int gridVertexCount = 0;
    f32 gridSize = -1.0f;
    int gridDivisions = -1;
    u32 gridMajorKey = 0, gridMinorKey = 0;
    u32 skyVao = 0, skyVbo = 0;
    u32 instanceVbo = 0;

    // Каскады теней: три отдельные depth-текстуры (соответствуют трём
    // сэмплерам `sampler2D uShadowMap0/1/2` в прямом фрагментном шейдере).
    Texture shadowMaps[r3d_internal::kMaxCascades];
    u32 shadowFbos[r3d_internal::kMaxCascades] = {0, 0, 0};
    int shadowMapSize = 0;
    bool shadowValid = false;
    // Привязывается к каждому материалному текстурному блоку без текстуры,
    // чтобы каждый объявленный `sampler2D` всегда видел совместимую 2D цветовую
    // текстуру (часть драйверов предупреждает о depth-текстуре на обычном сэмплере).
    Texture fallbackTexture;

    // Покадровое CPU-состояние (переиспользуется между кадрами: без аллокаций в установившемся режиме).
    std::vector<r3d_detail::DrawItem> items;
    std::vector<Mat4> instancePool;
    std::vector<std::function<void()>> postDraws;
    std::vector<r3d_detail::LineVertex> lineVerts;
    r3d_internal::PackedLights packed;

    Mat4 cascadeMatrices[r3d_internal::kMaxCascades];
    f32 cascadeSplits[r3d_internal::kMaxCascades] = {0, 0, 0};
    int activeCascades = 0;

    // Состояние кадра
    Camera camera;
    Environment env;
    const ShadowSettings* shadowsRef = nullptr;  // владеет Renderer3D, валидно в течение кадра
    Mat4 viewProj;
    f32 aspect = 1.0f;
    u32 mainFbo = 0;
    int mainWidth = 1, mainHeight = 1;
    bool customViewport = false;
    f32 vpX = 0, vpY = 0, vpW = 0, vpH = 0;
    bool skyRequested = false;
    bool gridRequested = false;
    int nextLightId = 1;

    // Параметры DrawGrid() (загрузка происходит в EndFrame).
    f32 reqGridSize = 20.0f;
    int reqGridDivisions = 20;
    Color reqGridMajor{Color::FromRGB(0x3A3A44)};
    Color reqGridMinor{Color::FromRGB(0x26262E)};

    // Покадровый снимок настроек.
    bool wireframe = false;
    bool backfaceCulling = true;
    bool cascadeDebug = false;

    [[nodiscard]] f32 EffectiveAspect() const {
        if (customViewport && vpW > 0.0f && vpH > 0.0f) return vpW / vpH;
        return mainHeight > 0 ? static_cast<f32>(mainWidth) / static_cast<f32>(mainHeight) : 1.0f;
    }

    // Отслеживает, у какой программы сейчас загружены общие uniform'ы.
    Shader* boundProgram = nullptr;

    bool CreatePrograms();
    bool CreateDebugGeometry();
    bool CreateSkyGeometry();
    bool EnsureShadowMaps(int size, int count);
    void DestroyShadowMaps();

    void ApplyViewport();
    void ApplyCommonUniforms(Shader& s);
    void ApplyMaterial(Shader& s, const Material& m, int cascadeHint);
    void ApplyCullState(const Material& m, bool backfaceCulling, bool depthPass);
    void BindShared(Shader& s);

    void ComputeCascades(const ShadowSettings& sh, const std::vector<Light>& lights);
    void RenderShadowPass(Render3DStats& stats);
    void RenderSkyPass();
    void RenderGridPass(Render3DStats& stats);
    void FlushLines(Render3DStats& stats);
    void RenderItem(const r3d_detail::DrawItem& item, Render3DStats& stats);
    void BuildGrid(f32 size, int divisions, const Color& major, const Color& minor);
};

// ---------------------------------------------------------------------------
// Создание ресурсов
// ---------------------------------------------------------------------------
bool Renderer3D::Impl::CreatePrograms() {
    if (!forward.Build(builtin::kForwardVert, builtin::kForwardFrag, "forward3d")) return false;
    if (!forwardInstanced.Build(kInstancedForwardVert, builtin::kForwardFrag, "forward3d_instanced"))
        return false;
    if (!shadow.Build(builtin::kShadowVert, builtin::kShadowFrag, "shadow3d")) return false;
    if (!sky.Build(builtin::kSkyVert, builtin::kSkyFrag, "sky3d")) return false;
    if (!line.Build(kLineVert, kLineFrag, "line3d")) return false;
    return true;
}

bool Renderer3D::Impl::CreateDebugGeometry() {
    if (!gl::glGenVertexArrays || !gl::glGenBuffers) return false;
    gl::glGenVertexArrays(1, &lineVao);
    gl::glGenBuffers(1, &lineVbo);
    gl::glBindVertexArray(lineVao);
    gl::glBindBuffer(gl::GL_ARRAY_BUFFER, lineVbo);
    gl::glEnableVertexAttribArray(0);
    gl::glVertexAttribPointer(0, 3, gl::GL_FLOAT, gl::GL_FALSE, sizeof(r3d_detail::LineVertex),
                              reinterpret_cast<const void*>(offsetof(r3d_detail::LineVertex, position)));
    gl::glEnableVertexAttribArray(1);
    gl::glVertexAttribPointer(1, 4, gl::GL_FLOAT, gl::GL_FALSE, sizeof(r3d_detail::LineVertex),
                              reinterpret_cast<const void*>(offsetof(r3d_detail::LineVertex, color)));

    gl::glGenVertexArrays(1, &gridVao);
    gl::glGenBuffers(1, &gridVbo);
    gl::glBindVertexArray(gridVao);
    gl::glBindBuffer(gl::GL_ARRAY_BUFFER, gridVbo);
    gl::glEnableVertexAttribArray(0);
    gl::glVertexAttribPointer(0, 3, gl::GL_FLOAT, gl::GL_FALSE, sizeof(r3d_detail::LineVertex),
                              reinterpret_cast<const void*>(offsetof(r3d_detail::LineVertex, position)));
    gl::glEnableVertexAttribArray(1);
    gl::glVertexAttribPointer(1, 4, gl::GL_FLOAT, gl::GL_FALSE, sizeof(r3d_detail::LineVertex),
                              reinterpret_cast<const void*>(offsetof(r3d_detail::LineVertex, color)));
    gl::glBindVertexArray(0);

    gl::glGenBuffers(1, &instanceVbo);
    return lineVao != 0 && gridVao != 0 && instanceVbo != 0;
}

bool Renderer3D::Impl::CreateSkyGeometry() {
    if (!gl::glGenVertexArrays || !gl::glGenBuffers) return false;
    // Полноэкранный треугольник (покрывает [-1,1]^2 тремя вершинами).
    const f32 verts[6] = {-1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f};
    gl::glGenVertexArrays(1, &skyVao);
    gl::glGenBuffers(1, &skyVbo);
    gl::glBindVertexArray(skyVao);
    gl::glBindBuffer(gl::GL_ARRAY_BUFFER, skyVbo);
    gl::glBufferData(gl::GL_ARRAY_BUFFER, sizeof(verts), verts, gl::GL_STATIC_DRAW);
    gl::glEnableVertexAttribArray(0);
    gl::glVertexAttribPointer(0, 2, gl::GL_FLOAT, gl::GL_FALSE, 2 * sizeof(f32), nullptr);
    gl::glBindVertexArray(0);
    return skyVao != 0;
}

bool Renderer3D::Impl::EnsureShadowMaps(int size, int count) {
    size = Clamp(size, 64, 8192);
    count = Clamp(count, 1, r3d_internal::kMaxCascades);
    if (shadowValid && shadowMapSize == size && shadowFbos[0] != 0) return true;
    DestroyShadowMaps();
    if (!gl::glGenFramebuffers) return false;

    bool ok = true;
    // Создаём depth-текстуры на тех блоках, с которых их сэмплит шейдер,
    // чтобы depth-текстура не осталась привязанной к обычному материалному блоку.
    if (gl::glActiveTexture)
        gl::glActiveTexture(static_cast<gl::GLenum>(gl::GL_TEXTURE0 + kShadowMapUnitBase));
    for (int i = 0; i < count; ++i) {
        if (!shadowMaps[i].Create(size, size, PixelFormat::Depth24, nullptr, TextureFilter::Nearest,
                                  TextureWrap::ClampToEdge, false)) {
            ok = false;
            break;
        }
        gl::glGenFramebuffers(1, &shadowFbos[i]);
        gl::glBindFramebuffer(gl::GL_FRAMEBUFFER, shadowFbos[i]);
        gl::glFramebufferTexture2D(gl::GL_FRAMEBUFFER, gl::GL_DEPTH_ATTACHMENT, gl::GL_TEXTURE_2D,
                                   shadowMaps[i].Id(), 0);
        // FBO только с глубиной: цветовых подключений нет, явно рисуем «в никуда».
        const gl::GLenum none = gl::GL_NONE;
        gl::glDrawBuffers(1, &none);
        const gl::GLenum status = gl::glCheckFramebufferStatus(gl::GL_FRAMEBUFFER);
        if (status != gl::GL_FRAMEBUFFER_COMPLETE) {
            ENG_LOGE("r3d", "shadow framebuffer %d incomplete (0x%04X)", i,
                     static_cast<unsigned>(status));
            ok = false;
            break;
        }
    }
    gl::glBindFramebuffer(gl::GL_FRAMEBUFFER, mainFbo);
    if (gl::glActiveTexture) gl::glActiveTexture(gl::GL_TEXTURE0);
    if (!ok) {
        DestroyShadowMaps();
        return false;
    }
    shadowMapSize = size;
    shadowValid = true;
    ENG_LOGI("r3d", "shadow maps: %d cascade(s) at %dx%d", count, size, size);
    return true;
}

void Renderer3D::Impl::DestroyShadowMaps() {
    for (int i = 0; i < r3d_internal::kMaxCascades; ++i) {
        if (shadowFbos[i] && gl::glDeleteFramebuffers) gl::glDeleteFramebuffers(1, &shadowFbos[i]);
        shadowFbos[i] = 0;
        shadowMaps[i].Destroy();
    }
    shadowMapSize = 0;
    shadowValid = false;
}

// ---------------------------------------------------------------------------
// Привязка uniform'ов
// ---------------------------------------------------------------------------
void Renderer3D::Impl::ApplyViewport() {
    if (customViewport) {
        // Прямоугольники вьюпорта задаются с y вниз; в GL ось y — вверх.
        const gl::GLint y = static_cast<gl::GLint>(static_cast<f32>(mainHeight) - (vpY + vpH));
        gl::glViewport(static_cast<gl::GLint>(vpX), y, static_cast<gl::GLsizei>(vpW),
                       static_cast<gl::GLsizei>(vpH));
    } else {
        gl::glViewport(0, 0, mainWidth, mainHeight);
    }
}

void Renderer3D::Impl::ApplyCommonUniforms(Shader& s) {
    s.Set("uViewProj", viewProj);
    s.Set("uCameraPos", camera.position);

    s.Set("uAmbientSky", env.ambientSky.rgb());
    s.Set("uAmbientGround", env.ambientGround.rgb());
    s.Set("uAmbientIntensity", env.ambientIntensity);

    // Шейдер: 0 = выключен, 1 = линейный, 2 = экспоненциальный.
    const i32 fogMode = env.fogEnabled ? (env.fogMode == 0 ? 1 : 2) : 0;
    s.Set("uFogMode", fogMode);
    s.Set("uFogColor", env.fogColor.rgb());
    s.Set("uFogDensity", env.fogDensity);
    s.Set("uFogStart", env.fogStart);
    s.Set("uFogEnd", env.fogEnd);

    // ---- источники света ----
    const int n = packed.count;
    s.Set("uLightCount", n);
    auto uni1iv = [&](const char* name, const i32* v) {
        const int l = s.UniformLocation(name);
        if (l >= 0 && n > 0) gl::glUniform1iv(l, n, v);
    };
    auto uni1fv = [&](const char* name, const f32* v) {
        const int l = s.UniformLocation(name);
        if (l >= 0 && n > 0) gl::glUniform1fv(l, n, v);
    };
    auto uni2fv = [&](const char* name, const Vec2* v) {
        const int l = s.UniformLocation(name);
        if (l >= 0 && n > 0) gl::glUniform2fv(l, n, reinterpret_cast<const f32*>(v));
    };
    auto uni3fv = [&](const char* name, const Vec3* v) {
        const int l = s.UniformLocation(name);
        if (l >= 0 && n > 0) gl::glUniform3fv(l, n, reinterpret_cast<const f32*>(v));
    };
    uni1iv("uLightType", packed.type);
    uni3fv("uLightPos", packed.position);
    uni3fv("uLightDir", packed.direction);
    uni3fv("uLightColor", packed.color);
    uni1fv("uLightIntensity", packed.intensity);
    uni1fv("uLightRange", packed.range);
    uni2fv("uLightCone", packed.cone);
    uni2fv("uLightArea", packed.area);

    // ---- тени ----
    const bool shadowOn = activeCascades > 0 && shadowValid;
    s.Set("uShadowEnabled", shadowOn ? 1 : 0);
    s.Set("uShadowCascadeCount", shadowOn ? activeCascades : 0);
    s.Set("uShadowBias", shadowsRef ? shadowsRef->depthBias : 0.0025f);
    s.Set("uShadowNormalBias", shadowsRef ? shadowsRef->normalBias : 0.02f);
    s.Set("uShadowPcfRadius", shadowsRef ? shadowsRef->pcfRadius : 1.5f);
    if (shadowOn) {
        const int lm = s.UniformLocation("uShadowMatrices");
        if (lm >= 0)
            gl::glUniformMatrix4fv(lm, activeCascades, 0,
                                   reinterpret_cast<const f32*>(cascadeMatrices));
        const int ls = s.UniformLocation("uCascadeSplit");
        if (ls >= 0) gl::glUniform1fv(ls, activeCascades, cascadeSplits);
        const char* names[3] = {"uShadowMap0", "uShadowMap1", "uShadowMap2"};
        for (int i = 0; i < activeCascades; ++i) {
            if (shadowMaps[i].Valid())
                s.SetTexture(names[i], shadowMaps[i], kShadowMapUnitBase + i);
        }
    }
}

void Renderer3D::Impl::ApplyMaterial(Shader& s, const Material& m, int cascadeHint) {
    Vec3 baseRgb = m.baseColor.rgb() * m.tint.rgb();
    if (cascadeHint >= 0) baseRgb = baseRgb * kCascadeDebugTint[Clamp(cascadeHint, 0, 2)];
    s.Set("uBaseColor", Color{baseRgb, m.baseColor.a * m.tint.a});
    s.Set("uEmissive", m.emissive.rgb() * m.emissiveStrength);
    s.Set("uMetallic", m.metallic);
    s.Set("uRoughness", m.roughness);
    s.Set("uNormalStrength", m.normalStrength);
    s.Set("uOcclusionStrength", m.occlusionStrength);
    s.Set("uAlphaCutoff", m.alphaCutoff);
    s.Set("uAlphaMode", static_cast<i32>(m.alphaMode == AlphaMode::Mask
                                             ? 1
                                             : (m.alphaMode == AlphaMode::Blend ? 2 : 0)));
    s.Set("uUnlit", m.unlit ? 1 : 0);
    s.Set("uVertexColors", m.vertexColors ? 1 : 0);
    s.Set("uUvScale", m.uvScale);
    s.Set("uUvOffset", m.uvOffset);

    const Texture* textures[5] = {m.baseColorTex, m.normalTex, m.metallicRoughnessTex, m.emissiveTex,
                                 m.occlusionTex};
    const char* samplerNames[5] = {"uBaseColorTex", "uNormalTex", "uMetallicRoughnessTex",
                                   "uEmissiveTex", "uOcclusionTex"};
    const char* hasNames[5] = {"uHasBaseColorTex", "uHasNormalTex", "uHasMetallicRoughnessTex",
                               "uHasEmissiveTex", "uHasOcclusionTex"};
    for (int i = 0; i < 5; ++i) {
        const bool bound = textures[i] && textures[i]->Valid();
        const Texture* tex = bound ? textures[i] : (fallbackTexture.Valid() ? &fallbackTexture : nullptr);
        if (tex) s.SetTexture(samplerNames[i], *tex, i);
        s.Set(hasNames[i], bound ? 1 : 0);
    }
}

void Renderer3D::Impl::ApplyCullState(const Material& m, bool backfaceCulling, bool depthPass) {
    // Теневой проход рисует двусторонне, чтобы тонкая геометрия (плоскости) тоже отбрасывала тень.
    const bool cull = !depthPass && backfaceCulling && !m.doubleSided && m.cullMode != CullMode::None;
    if (!cull) {
        gl::glDisable(gl::GL_CULL_FACE);
        return;
    }
    gl::glEnable(gl::GL_CULL_FACE);
    gl::glCullFace(m.cullMode == CullMode::Front ? gl::GL_FRONT : gl::GL_BACK);
    gl::glFrontFace(gl::GL_CCW);
}

void Renderer3D::Impl::BindShared(Shader& s) {
    if (boundProgram == &s) return;
    s.Bind();
    ApplyCommonUniforms(s);
    boundProgram = &s;
}

// ---------------------------------------------------------------------------
// Помощники списка кадра
//
// Список кадра сортируется так, что рисуемые элементы идут первыми,
// непрозрачные раньше прозрачных: оба прохода получаются непрерывными диапазонами.
// ---------------------------------------------------------------------------
namespace {
usize OpaqueEndImpl(const std::vector<r3d_detail::DrawItem>& items) {
    usize i = 0;
    while (i < items.size() && !items[i].culled && !items[i].transparent) ++i;
    return i;
}
usize DrawableEndImpl(const std::vector<r3d_detail::DrawItem>& items) {
    usize i = 0;
    while (i < items.size() && !items[i].culled) ++i;
    return i;
}
}  // namespace

// ---------------------------------------------------------------------------
// Настройка каскадов
//
// Честно о границах: `builtin::kForwardFrag` сэмплирует максимум три каскада
// одного набора теневых матриц, поэтому сэмплируемые тени за кадр может давать
// только ОДИН источник. Каскадами владеет главный направленный источник.
// Прожекторы с `castShadows` тоже рендерятся в карты (их геометрия бесплатно
// корректно затеняет направленный свет); если направленного источника нет
// вовсе, первым источником становится первый отбрасывающий тень
// прожектор/точечный и получает одну перспективную каскаду — точно для
// прожектора, а для точечного — задокументированное приближение одной гранью (90 градусов).
// ---------------------------------------------------------------------------
void Renderer3D::Impl::ComputeCascades(const ShadowSettings& sh, const std::vector<Light>& lights) {
    activeCascades = 0;
    if (!sh.enabled || !shadowValid || shadowMapSize <= 0) return;

    const int considered = MinT(static_cast<int>(lights.size()), packed.count);
    int primaryDir = -1;
    int primaryLocal = -1;
    for (int i = 0; i < considered; ++i) {
        if (!lights[i].castShadows) continue;
        if (lights[i].type == LightType::Directional) {
            primaryDir = i;
            break;
        }
        if (primaryLocal < 0 && (lights[i].type == LightType::Spot || lights[i].type == LightType::Point))
            primaryLocal = i;
    }

    if (primaryDir >= 0) {
        const int n = Clamp(sh.cascadeCount, 1, r3d_internal::kMaxCascades);
        f32 shadowFar = MinT(camera.farZ, sh.cascadeDistance);
        if (shadowFar <= camera.nearZ) shadowFar = camera.nearZ + 1.0f;
        r3d_internal::ComputeCascadeSplits(camera.nearZ, shadowFar, sh.cascadeSplitLambda, n,
                                           cascadeSplits);
        const Vec3 lightDir = packed.direction[primaryDir];
        const Mat4 view = camera.View();
        for (int c = 0; c < n; ++c) {
            const f32 sliceNear = c == 0 ? camera.nearZ : cascadeSplits[c - 1];
            const f32 sliceFar = cascadeSplits[c];
            Mat4 sliceProj;
            if (camera.projection == ProjectionType::Orthographic) {
                const f32 h = camera.orthoHeight * 0.5f;
                const f32 w = h * aspect;
                sliceProj = Mat4::Ortho(-w, w, -h, h, sliceNear, sliceFar);
            } else {
                sliceProj = Mat4::Perspective(camera.fovY, aspect, sliceNear, sliceFar);
            }
            // Восемь мировых углов среза фрустума камеры.
            const Mat4 invSlice = (sliceProj * view).Inverse();
            Vec3 corners[8];
            for (int i = 0; i < 8; ++i) {
                const Vec3 ndc{(i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f,
                               (i & 4) ? 1.0f : -1.0f};
                corners[i] = invSlice.TransformPoint(ndc);
            }
            Vec3 center{0, 0, 0};
            for (const Vec3& p : corners) center += p;
            center = center / 8.0f;
            f32 radius = 0.0f;
            for (const Vec3& p : corners) radius = MaxT(radius, Length(p - center));
            radius = MaxT(radius, 1e-3f);

            Vec3 up{0, 1, 0};
            if (std::fabs(Dot(lightDir, up)) > 0.95f) up = Vec3{0, 0, 1};
            const Vec3 eye = center - lightDir * (radius * 3.0f);
            const Mat4 lightView = Mat4::LookAt(eye, center, up);
            Vec3 lo{1e30f, 1e30f, 1e30f};
            Vec3 hi{-1e30f, -1e30f, -1e30f};
            for (const Vec3& p : corners) {
                const Vec3 lp = lightView.TransformPoint(p);
                lo = Min(lo, lp);
                hi = Max(hi, lp);
            }
            // Привязываем начало орто-бокса к целым текселям: без этого карта
            // мерцает при движении камеры.
            const f32 invSize = 1.0f / static_cast<f32>(shadowMapSize);
            const f32 texelX = (hi.x - lo.x) * invSize;
            const f32 texelY = (hi.y - lo.y) * invSize;
            f32 cx = (lo.x + hi.x) * 0.5f;
            f32 cy = (lo.y + hi.y) * 0.5f;
            if (texelX > 1e-6f) cx = std::floor(cx / texelX) * texelX;
            if (texelY > 1e-6f) cy = std::floor(cy / texelY) * texelY;
            const f32 hw = (hi.x - lo.x) * 0.5f + 1e-3f;
            const f32 hh = (hi.y - lo.y) * 0.5f + 1e-3f;
            // В light view координата z отрицательна перед глазом: -hi.z —
            // ближайший угол, -lo.z — самый дальний. Отодвигаем near-плоскость
            // назад, чтобы кастеры между источником и срезом попали в карту.
            const f32 zn = MaxT(0.01f, -hi.z - radius * 0.5f);
            const f32 zf = -lo.z + radius * 0.25f;
            cascadeMatrices[c] =
                Mat4::Ortho(cx - hw, cx + hw, cy - hh, cy + hh, zn, MaxT(zf, zn + 1e-3f)) * lightView;
        }
        activeCascades = n;
        return;
    }

    if (primaryLocal >= 0) {
        const Light& l = lights[primaryLocal];
        const Vec3 dir = LengthSq(l.direction) > 1e-12f ? Normalize(l.direction) : Vec3{0, -1, 0};
        Vec3 up{0, 1, 0};
        if (std::fabs(Dot(dir, up)) > 0.95f) up = Vec3{0, 0, 1};
        const f32 far = MaxT(l.range, 1.0f);
        const f32 fov = l.type == LightType::Spot ? Clamp(l.outerCone * 2.2f, 0.05f, kPi * 0.5f)
                                                  : (kPi * 0.5f);
        cascadeMatrices[0] =
            Mat4::Perspective(fov, 1.0f, MaxT(0.02f, far * 0.005f), far) *
            Mat4::LookAt(l.position, l.position + dir, up);
        cascadeSplits[0] = far;
        activeCascades = 1;
    }
}

// ---------------------------------------------------------------------------
// Теневой проход (только глубина)
// ---------------------------------------------------------------------------
void Renderer3D::Impl::RenderShadowPass(Render3DStats& stats) {
    if (!shadowValid || activeCascades <= 0) return;
    // Всегда очищаем каждую каскаду, даже без кастеров в этом кадре: прямой
    // проход сэмплирует карты безусловно при uShadowCascadeCount > 0, и
    // устаревшее содержимое затемнило бы сцену неправильно.
    const usize opaqueEnd = OpaqueEndImpl(items);

    gl::glDisable(gl::GL_BLEND);
    gl::glDisable(gl::GL_CULL_FACE);
    gl::glEnable(gl::GL_DEPTH_TEST);
    gl::glDepthMask(1);
    gl::glDepthFunc(gl::GL_LESS);
    shadow.Bind();
    boundProgram = nullptr;

    for (int c = 0; c < activeCascades; ++c) {
        if (shadowFbos[c] == 0) continue;
        r3d_internal::Plane planes[6];
        r3d_internal::ExtractFrustumPlanes(cascadeMatrices[c], planes);
        gl::glBindFramebuffer(gl::GL_FRAMEBUFFER, shadowFbos[c]);
        gl::glViewport(0, 0, shadowMapSize, shadowMapSize);
        gl::glClear(gl::GL_DEPTH_BUFFER_BIT);
        shadow.Set("uLightViewProj", cascadeMatrices[c]);
        for (usize i = 0; i < opaqueEnd; ++i) {
            const r3d_detail::DrawItem& it = items[i];
            if (!it.mesh || !it.mesh->Valid() || it.mesh->IndexCount() == 0) continue;
            if (!r3d_internal::AabbInFrustum(planes, it.worldMin, it.worldMax)) continue;
            if (it.instanceCount > 0 && it.externalBuffer == 0) {
                for (int k = 0; k < it.instanceCount; ++k) {
                    shadow.Set("uModel", it.transform * instancePool[it.instanceOffset + k]);
                    it.mesh->Draw();
                    stats.triangles += static_cast<int>(it.mesh->IndexCount() / 3);
                    stats.vertices += static_cast<int>(it.mesh->VertexCount());
                    ++stats.drawCalls;
                    ++stats.shadowCasters;
                }
            } else {
                // Внешние буферы экземпляров нельзя прочитать обратно, поэтому
                // такие элементы отбрасывают одну тень в базовой трансформации.
                shadow.Set("uModel", it.transform);
                it.mesh->Draw();
                stats.triangles += static_cast<int>(it.mesh->IndexCount() / 3);
                stats.vertices += static_cast<int>(it.mesh->VertexCount());
                ++stats.drawCalls;
                ++stats.shadowCasters;
            }
        }
    }
    gl::glBindFramebuffer(gl::GL_FRAMEBUFFER, mainFbo);
    ApplyViewport();
}

// ---------------------------------------------------------------------------
// Небо / сетка / линии
// ---------------------------------------------------------------------------
void Renderer3D::Impl::RenderSkyPass() {
    if (skyVao == 0 || !sky.Valid()) return;
    gl::glDisable(gl::GL_CULL_FACE);
    gl::glDisable(gl::GL_BLEND);
    gl::glEnable(gl::GL_DEPTH_TEST);
    gl::glDepthFunc(gl::GL_LEQUAL);
    gl::glDepthMask(0);
    sky.Bind();
    boundProgram = nullptr;
    sky.Set("uInvViewProj", viewProj.Inverse());
    sky.Set("uSkyTop", env.skyTop.rgb());
    sky.Set("uSkyHorizon", env.skyHorizon.rgb());
    sky.Set("uSkyBottom", env.skyBottom.rgb());
    Vec3 sunDir{0.0f, 1.0f, 0.0f};
    Vec3 sunColor{0.0f, 0.0f, 0.0f};
    for (int i = 0; i < packed.count; ++i) {
        if (packed.type[i] == 0) {
            sunDir = -packed.direction[i];  // от поверхности к солнцу
            sunColor = packed.color[i] * Clamp(packed.intensity[i], 0.0f, 4.0f) * 0.35f;
            break;
        }
    }
    sky.Set("uSunDirection", sunDir);
    sky.Set("uSunColor", sunColor);
    gl::glBindVertexArray(skyVao);
    gl::glDrawArrays(gl::GL_TRIANGLES, 0, 3);
    gl::glBindVertexArray(0);
    gl::glDepthMask(1);
    gl::glDepthFunc(gl::GL_LESS);
}

void Renderer3D::Impl::BuildGrid(f32 size, int divisions, const Color& major, const Color& minor) {
    const u32 majorKey = major.ToRGBA8();
    const u32 minorKey = minor.ToRGBA8();
    if (gridVbo != 0 && NearlyEqual(gridSize, size) && gridDivisions == divisions &&
        gridMajorKey == majorKey && gridMinorKey == minorKey)
        return;
    if (gridVbo == 0 || !gl::glBufferData) return;

    size = MaxT(size, 0.01f);
    divisions = MaxT(divisions, 2);
    gridSize = size;
    gridDivisions = divisions;
    gridMajorKey = majorKey;
    gridMinorKey = minorKey;

    std::vector<r3d_detail::LineVertex> verts;
    const f32 half = size * 0.5f;
    const f32 step = size / static_cast<f32>(divisions);
    const int majorEvery = MaxT(1, divisions / 10);
    const int kSegments = 4;  // сегментов на полулинию, для плавного затухания по расстоянию
    auto fade = [half](f32 x, f32 z, f32 alpha) {
        const f32 d = std::sqrt(x * x + z * z);
        f32 a = Clamp(1.0f - d / half, 0.0f, 1.0f);
        a = a * a;
        return a * alpha;
    };
    auto pushSegment = [&](const Vec3& a, const Vec3& b, const Color& c, f32 alpha) {
        const Color ca = c.WithAlpha(c.a * fade(a.x, a.z, alpha));
        const Color cb = c.WithAlpha(c.a * fade(b.x, b.z, alpha));
        verts.push_back({a, ca, 1.0f, 1u});
        verts.push_back({b, cb, 1.0f, 1u});
    };

    for (int i = 0; i <= divisions; ++i) {
        const f32 t = -half + step * static_cast<f32>(i);
        const bool isMajor = (i % majorEvery) == 0;
        const Color c = isMajor ? major : minor;
        const int centerLine = divisions / 2;
        if (i == centerLine) continue;  // оси рисуются отдельно и в цвете
        // Линия вдоль X при z = t и вдоль Z при x = t.
        for (int s = 0; s < kSegments; ++s) {
            const f32 a0 = -half + size * static_cast<f32>(s) / kSegments;
            const f32 a1 = -half + size * static_cast<f32>(s + 1) / kSegments;
            pushSegment({a0, 0.0f, t}, {a1, 0.0f, t}, c, isMajor ? 1.0f : 0.85f);
            pushSegment({t, 0.0f, a0}, {t, 0.0f, a1}, c, isMajor ? 1.0f : 0.85f);
        }
    }
    // Цветные оси: X (красная) и Z (синяя).
    const Color axisX{0.85f, 0.25f, 0.28f, 1.0f};
    const Color axisZ{0.30f, 0.48f, 0.92f, 1.0f};
    for (int s = 0; s < kSegments * 2; ++s) {
        const f32 a0 = -half + size * static_cast<f32>(s) / (kSegments * 2);
        const f32 a1 = -half + size * static_cast<f32>(s + 1) / (kSegments * 2);
        pushSegment({a0, 0.0f, 0.0f}, {a1, 0.0f, 0.0f}, axisX, 1.0f);
        pushSegment({0.0f, 0.0f, a0}, {0.0f, 0.0f, a1}, axisZ, 1.0f);
    }

    gl::glBindVertexArray(gridVao);
    gl::glBindBuffer(gl::GL_ARRAY_BUFFER, gridVbo);
    gl::glBufferData(gl::GL_ARRAY_BUFFER,
                     static_cast<gl::GLsizeiptr>(verts.size() * sizeof(r3d_detail::LineVertex)),
                     verts.data(), gl::GL_STATIC_DRAW);
    gl::glBindVertexArray(0);
    gridVertexCount = static_cast<int>(verts.size());
}

void Renderer3D::Impl::RenderGridPass(Render3DStats& stats) {
    BuildGrid(reqGridSize, reqGridDivisions, reqGridMajor, reqGridMinor);
    if (gridVertexCount <= 0 || !line.Valid()) return;
    gl::glDisable(gl::GL_CULL_FACE);
    gl::glEnable(gl::GL_DEPTH_TEST);
    gl::glDepthFunc(gl::GL_LEQUAL);
    gl::glDepthMask(0);
    gl::glEnable(gl::GL_BLEND);
    gl::glBlendFunc(gl::GL_SRC_ALPHA, gl::GL_ONE_MINUS_SRC_ALPHA);
    line.Bind();
    boundProgram = nullptr;
    line.Set("uViewProj", viewProj);
    gl::glBindVertexArray(gridVao);
    if (gl::glLineWidth) gl::glLineWidth(1.0f);
    gl::glDrawArrays(gl::GL_LINES, 0, gridVertexCount);
    gl::glBindVertexArray(0);
    gl::glDepthMask(1);
    gl::glDisable(gl::GL_BLEND);
    stats.lines += gridVertexCount / 2;
    ++stats.drawCalls;
}

void Renderer3D::Impl::FlushLines(Render3DStats& stats) {
    if (lineVerts.empty() || lineVao == 0 || !line.Valid()) {
        lineVerts.clear();
        return;
    }
    // Сначала линии с depth-тестом (нижний батч), затем батч поверх всего;
    // внутри батча группируем по ширине, чтобы каждая группа была одним draw call.
    //
    // Обязателен *стабильный* sort: массив — это плоский список вершин GL_LINES,
    // поэтому перестановка двух вершин одного сегмента разорвала бы сегмент и
    // нарисовала длинные линии через сцену. stable_sort сохраняет относительный
    // порядок элементов с равным ключом и потому держит каждую пару (a, b)
    // рядом. (Он может аллоцировать временный буфер; список линий настолько мал,
    // что это неважно.)
    std::stable_sort(lineVerts.begin(), lineVerts.end(),
              [](const r3d_detail::LineVertex& a, const r3d_detail::LineVertex& b) {
                  if (a.depthTest != b.depthTest) return a.depthTest > b.depthTest;
                  return a.width < b.width;
              });
    gl::glBindVertexArray(lineVao);
    gl::glBindBuffer(gl::GL_ARRAY_BUFFER, lineVbo);
    gl::glBufferData(gl::GL_ARRAY_BUFFER,
                     static_cast<gl::GLsizeiptr>(lineVerts.size() * sizeof(r3d_detail::LineVertex)),
                     lineVerts.data(), gl::GL_STREAM_DRAW);
    line.Bind();
    boundProgram = nullptr;
    line.Set("uViewProj", viewProj);
    gl::glDisable(gl::GL_CULL_FACE);
    gl::glEnable(gl::GL_BLEND);
    gl::glBlendFunc(gl::GL_SRC_ALPHA, gl::GL_ONE_MINUS_SRC_ALPHA);
    gl::glDepthMask(0);

    usize i = 0;
    while (i < lineVerts.size()) {
        const bool depthTest = lineVerts[i].depthTest != 0;
        const f32 width = lineVerts[i].width;
        usize j = i + 1;
        while (j < lineVerts.size() && (lineVerts[j].depthTest != 0) == depthTest &&
               NearlyEqual(lineVerts[j].width, width))
            ++j;
        if (depthTest) {
            gl::glEnable(gl::GL_DEPTH_TEST);
            gl::glDepthFunc(gl::GL_LEQUAL);
        } else {
            gl::glDisable(gl::GL_DEPTH_TEST);
        }
        // Core-профили ограничивают ширину линии значением 1.0; больше — ошибка GL.
        if (gl::glLineWidth) gl::glLineWidth(Clamp(width, 0.5f, 1.0f));
        gl::glDrawArrays(gl::GL_LINES, static_cast<gl::GLint>(i), static_cast<gl::GLsizei>(j - i));
        stats.lines += static_cast<int>((j - i) / 2);
        ++stats.drawCalls;
        i = j;
    }
    gl::glBindVertexArray(0);
    gl::glEnable(gl::GL_DEPTH_TEST);
    gl::glDepthFunc(gl::GL_LESS);
    gl::glDepthMask(1);
    gl::glDisable(gl::GL_BLEND);
    lineVerts.clear();
}

// ---------------------------------------------------------------------------
// Один затеняемый элемент
// ---------------------------------------------------------------------------
void Renderer3D::Impl::RenderItem(const r3d_detail::DrawItem& it, Render3DStats& stats) {
    if (!it.mesh || !it.mesh->Valid() || it.mesh->IndexCount() == 0) return;
    const bool instanced = it.instanceCount > 0;
    Shader& s = instanced ? forwardInstanced : forward;
    if (!s.Valid()) return;
    BindShared(s);

    int cascadeHint = -1;
    if (cascadeDebug && activeCascades > 0) {
        const f32 dist = std::sqrt(it.depth);
        cascadeHint = activeCascades - 1;
        for (int i = 0; i < activeCascades; ++i) {
            if (dist < cascadeSplits[i]) {
                cascadeHint = i;
                break;
            }
        }
    }
    ApplyMaterial(s, it.material, cascadeHint);
    ApplyCullState(it.material, backfaceCulling, false);

    if (instanced) {
        // Матрицы экземпляров лежат в `instanceVbo` (копирует DrawInstanced) либо
        // в буфере вызывающего. Настраиваем атрибуты 6..9 на собственном VAO меша
        // и затем восстанавливаем его.
        const u32 buffer = it.externalBuffer ? it.externalBuffer : instanceVbo;
        if (buffer == 0) return;
        if (it.externalBuffer == 0) {
            const usize first = static_cast<usize>(it.instanceOffset);
            gl::glBindBuffer(gl::GL_ARRAY_BUFFER, instanceVbo);
            gl::glBufferData(gl::GL_ARRAY_BUFFER,
                             static_cast<gl::GLsizeiptr>(static_cast<usize>(it.instanceCount) * sizeof(Mat4)),
                             instancePool.data() + first, gl::GL_STREAM_DRAW);
        }
        it.mesh->Bind();
        if (it.externalBuffer != 0) gl::glBindBuffer(gl::GL_ARRAY_BUFFER, buffer);
        for (u32 k = 0; k < 4; ++k) {
            const u32 location = 6 + k;
            gl::glEnableVertexAttribArray(location);
            gl::glVertexAttribPointer(location, 4, gl::GL_FLOAT, gl::GL_FALSE,
                                      static_cast<gl::GLsizei>(sizeof(Mat4)),
                                      reinterpret_cast<const void*>(static_cast<usize>(k) * 4 * sizeof(f32)));
            gl::glVertexAttribDivisor(location, 1);
        }
        s.Set("uModel", it.transform);
        s.Set("uNormalMatrix", it.normalMatrix);
        it.mesh->DrawInstanced(it.instanceCount);
        for (u32 k = 0; k < 4; ++k) {
            gl::glVertexAttribDivisor(6 + k, 0);
            gl::glDisableVertexAttribArray(6 + k);
        }
        gl::glBindVertexArray(0);
        ++stats.drawCalls;
        ++stats.instancedDraws;
        stats.triangles += static_cast<int>(it.mesh->IndexCount() / 3) * it.instanceCount;
        stats.vertices += static_cast<int>(it.mesh->VertexCount()) * it.instanceCount;
        return;
    }

    s.Set("uModel", it.transform);
    s.Set("uNormalMatrix", it.normalMatrix);
    if (wireframe) {
        // Отладочное приближение: пере-рисуем индексный буфер парами линий.
        const u32 count = it.mesh->IndexCount() & ~1u;
        if (count > 0) {
            it.mesh->Bind();
            gl::glDrawElements(gl::GL_LINES, static_cast<gl::GLsizei>(count), gl::GL_UNSIGNED_INT,
                               nullptr);
            gl::glBindVertexArray(0);
            ++stats.drawCalls;
            stats.lines += static_cast<int>(count / 2);
        }
    } else {
        it.mesh->Draw();
        ++stats.drawCalls;
    }
    stats.triangles += static_cast<int>(it.mesh->IndexCount() / 3);
    stats.vertices += static_cast<int>(it.mesh->VertexCount());
}

// ---------------------------------------------------------------------------
// Публичный API Renderer3D
// ---------------------------------------------------------------------------
Renderer3D::Renderer3D() : impl_(new Impl()) {}

Renderer3D::~Renderer3D() { Shutdown(); }

bool Renderer3D::Init() {
    if (!impl_) impl_.reset(new Impl());
    Impl& G = *impl_;
    if (G.initialized) return true;
    if (!gl::glGenVertexArrays || !gl::glGenBuffers || !gl::glCreateShader || !gl::glGenFramebuffers ||
        !gl::glBindVertexArray) {
        ENG_LOGE("r3d", "Renderer3D::Init: no GL context (entry points not loaded)");
        return false;
    }
    if (!G.CreatePrograms()) {
        ENG_LOGE("r3d", "Renderer3D::Init: shader program creation failed");
        return false;
    }
    if (!G.CreateDebugGeometry() || !G.CreateSkyGeometry()) {
        ENG_LOGE("r3d", "Renderer3D::Init: debug/sky geometry creation failed");
        return false;
    }
    if (!G.fallbackTexture.CreateSolid(Color::White))
        ENG_LOGW("r3d", "Renderer3D::Init: fallback texture unavailable");
    if (!G.EnsureShadowMaps(shadows_.directionalMapSize,
                            Clamp(shadows_.cascadeCount, 1, r3d_internal::kMaxCascades)))
        ENG_LOGW("r3d", "Renderer3D::Init: shadow maps unavailable; shadows disabled");
    G.initialized = true;
    ENG_LOGI("r3d", "Renderer3D initialised (maxLights=%d, requested cascades=%d)",
             settings_.maxLights, shadows_.cascadeCount);
    return true;
}

void Renderer3D::Shutdown() {
    if (!impl_) return;
    Impl& G = *impl_;
    if (G.lineVbo && gl::glDeleteBuffers) gl::glDeleteBuffers(1, &G.lineVbo);
    if (G.gridVbo && gl::glDeleteBuffers) gl::glDeleteBuffers(1, &G.gridVbo);
    if (G.skyVbo && gl::glDeleteBuffers) gl::glDeleteBuffers(1, &G.skyVbo);
    if (G.instanceVbo && gl::glDeleteBuffers) gl::glDeleteBuffers(1, &G.instanceVbo);
    if (G.lineVao && gl::glDeleteVertexArrays) gl::glDeleteVertexArrays(1, &G.lineVao);
    if (G.gridVao && gl::glDeleteVertexArrays) gl::glDeleteVertexArrays(1, &G.gridVao);
    if (G.skyVao && gl::glDeleteVertexArrays) gl::glDeleteVertexArrays(1, &G.skyVao);
    G.lineVbo = G.gridVbo = G.skyVbo = G.instanceVbo = 0;
    G.lineVao = G.gridVao = G.skyVao = 0;
    G.gridVertexCount = 0;
    G.gridSize = -1.0f;
    G.gridDivisions = -1;
    G.DestroyShadowMaps();
    G.fallbackTexture.Destroy();
    G.forward.Destroy();
    G.forwardInstanced.Destroy();
    G.shadow.Destroy();
    G.sky.Destroy();
    G.line.Destroy();
    G.items.clear();
    G.instancePool.clear();
    G.lineVerts.clear();
    G.initialized = false;
    G.boundProgram = nullptr;
}

void Renderer3D::BeginFrame(const Camera& camera, int fbWidth, int fbHeight, RenderTarget* target,
                            bool clearColor, bool clearDepth) {
    if (!impl_) impl_.reset(new Impl());
    Impl& G = *impl_;

    camera_ = camera;
    fbWidth_ = MaxT(fbWidth, 1);
    fbHeight_ = MaxT(fbHeight, 1);
    G.items.clear();
    G.instancePool.clear();
    G.lineVerts.clear();
    G.skyRequested = false;
    G.gridRequested = false;
    G.boundProgram = nullptr;
    stats_ = Render3DStats{};

    if (!G.initialized) return;

    if (target) {
        target->Bind();  // привязывает FBO и ставит вьюпорт по размеру цели
        G.mainFbo = target->Fbo();
        G.mainWidth = MaxT(target->Width(), 1);
        G.mainHeight = MaxT(target->Height(), 1);
    } else {
        gl::glBindFramebuffer(gl::GL_FRAMEBUFFER, 0);
        G.mainFbo = 0;
        G.mainWidth = fbWidth_;
        G.mainHeight = fbHeight_;
    }
    G.aspect = G.EffectiveAspect();
    G.ApplyViewport();

    viewProj_ = camera_.ViewProj(G.aspect);
    G.viewProj = viewProj_;

    if (clearColor || clearDepth) {
        const bool scissor = G.customViewport;
        if (scissor && gl::glScissor) {
            gl::glEnable(gl::GL_SCISSOR_TEST);
            gl::glScissor(static_cast<gl::GLint>(G.vpX),
                          static_cast<gl::GLint>(static_cast<f32>(G.mainHeight) - (G.vpY + G.vpH)),
                          static_cast<gl::GLsizei>(G.vpW), static_cast<gl::GLsizei>(G.vpH));
        }
        gl::glClearColor(kFrameClearColor.r, kFrameClearColor.g, kFrameClearColor.b,
                         kFrameClearColor.a);
        gl::glDepthMask(1);
        gl::GLbitfield mask = 0;
        if (clearColor) mask |= gl::GL_COLOR_BUFFER_BIT;
        if (clearDepth) mask |= gl::GL_DEPTH_BUFFER_BIT;
        if (mask) gl::glClear(mask);
        if (scissor && gl::glDisable) gl::glDisable(gl::GL_SCISSOR_TEST);
    }

    // Дистанции разбиений каскадов зависят только от камеры и настроек; матрицам
    // света дополнительно нужны источники кадра, поэтому они строятся в
    // EndFrame() (см. ComputeCascades).
    const int n = Clamp(shadows_.cascadeCount, 1, r3d_internal::kMaxCascades);
    f32 shadowFar = MinT(camera_.farZ, shadows_.cascadeDistance);
    if (shadowFar <= camera_.nearZ) shadowFar = camera_.nearZ + 1.0f;
    r3d_internal::ComputeCascadeSplits(camera_.nearZ, shadowFar, shadows_.cascadeSplitLambda, n,
                                       G.cascadeSplits);
}

void Renderer3D::EndFrame() {
    if (!impl_) return;
    Impl& G = *impl_;
    if (!G.initialized) {
        if (!G.warnedNoContext) {
            ENG_LOGW("r3d", "EndFrame() ignored: Renderer3D::Init() did not succeed");
            G.warnedNoContext = true;
        }
        G.items.clear();
        G.instancePool.clear();
        G.lineVerts.clear();
        G.skyRequested = false;
        G.gridRequested = false;
        return;
    }
    const auto startTime = std::chrono::steady_clock::now();

    G.camera = camera_;
    G.env = env_;
    G.shadowsRef = &shadows_;
    G.wireframe = settings_.wireframe;
    G.backfaceCulling = settings_.backfaceCulling;
    G.cascadeDebug = settings_.showShadowCascades;
    G.aspect = G.EffectiveAspect();
    G.viewProj = camera_.ViewProj(G.aspect);
    viewProj_ = G.viewProj;

    // ---- 1. отсечение, классификация, сортировка --------------------------
    stats_.culledObjects = 0;
    r3d_internal::Plane planes[6];
    const bool doCull = settings_.frustumCulling;
    if (doCull) r3d_internal::ExtractFrustumPlanes(viewProj_, planes);
    for (r3d_detail::DrawItem& it : G.items) {
        it.transparent = it.material.alphaMode == AlphaMode::Blend;
        it.culled = false;
        if (doCull && !r3d_internal::AabbInFrustum(planes, it.worldMin, it.worldMax)) {
            it.culled = true;
            ++stats_.culledObjects;
            continue;
        }
        const Vec3 center = (it.worldMin + it.worldMax) * 0.5f;
        it.depth = LengthSq(center - G.camera.position);
    }
    std::sort(G.items.begin(), G.items.end(),
              [](const r3d_detail::DrawItem& a, const r3d_detail::DrawItem& b) {
                  if (a.culled != b.culled) return !a.culled;
                  if (a.transparent != b.transparent) return !a.transparent;
                  if (a.transparent) return a.depth > b.depth;  // сзади наперёд
                  return a.depth < b.depth;                     // спереди назад
              });
    const usize opaqueEnd = OpaqueEndImpl(G.items);
    const usize drawableEnd = DrawableEndImpl(G.items);

    // ---- 2. источники света, каскады, теневые карты ------------------------
    r3d_internal::PackLights(lights_.data(), LightCount(), settings_.maxLights, &G.packed);
    stats_.lights = G.packed.count;
    if (shadows_.enabled)
        G.EnsureShadowMaps(shadows_.directionalMapSize,
                           Clamp(shadows_.cascadeCount, 1, r3d_internal::kMaxCascades));
    G.ComputeCascades(shadows_, lights_);
    G.RenderShadowPass(stats_);

    // ---- 3. цветовой проход -----------------------------------------------
    gl::glBindFramebuffer(gl::GL_FRAMEBUFFER, G.mainFbo);
    G.ApplyViewport();
    gl::glEnable(gl::GL_DEPTH_TEST);
    gl::glDepthMask(1);
    gl::glDepthFunc(gl::GL_LESS);
    gl::glDisable(gl::GL_BLEND);
    gl::glColorMask(1, 1, 1, 1);
    G.boundProgram = nullptr;

    if (settings_.depthPrepass && opaqueEnd > 0) {
        // Prepass только с позициями: теневая программа — это в точности
        // depth-only шейдер с парой model + view-projection.
        gl::glDisable(gl::GL_CULL_FACE);
        G.shadow.Bind();
        G.shadow.Set("uLightViewProj", viewProj_);
        gl::glColorMask(0, 0, 0, 0);
        for (usize i = 0; i < opaqueEnd; ++i) {
            const r3d_detail::DrawItem& it = G.items[i];
            if (!it.mesh || !it.mesh->Valid() || it.mesh->IndexCount() == 0) continue;
            if (it.instanceCount > 0) continue;  // инстансированные элементы шейдятся как обычно
            // У prepass-шейдера нет alpha-теста: masked-материалы записали бы
            // глубину для отброшенных текселей, поэтому оставляем их главному проходу.
            if (it.material.alphaMode == AlphaMode::Mask) continue;
            G.shadow.Set("uModel", it.transform);
            it.mesh->Draw();
            ++stats_.drawCalls;
            stats_.triangles += static_cast<int>(it.mesh->IndexCount() / 3);
            stats_.vertices += static_cast<int>(it.mesh->VertexCount());
        }
        gl::glColorMask(1, 1, 1, 1);
        gl::glDepthFunc(gl::GL_LEQUAL);
        G.boundProgram = nullptr;
    }

    if (G.skyRequested || G.env.drawSky) G.RenderSkyPass();

    // Непрозрачные + alpha-masked, спереди назад, с записью глубины.
    gl::glDisable(gl::GL_BLEND);
    gl::glDepthMask(1);
    gl::glDepthFunc(gl::GL_LEQUAL);
    for (usize i = 0; i < opaqueEnd; ++i) G.RenderItem(G.items[i], stats_);

    // Сетка: как и небо, включается покадрово через `Environment::drawGrid`;
    // явный вызов DrawGrid() дополнительно загружает запрошенные параметры.
    if (G.gridRequested || G.env.drawGrid) G.RenderGridPass(stats_);

    // С альфа-смешением, сзади наперёд, без записи глубины.
    if (opaqueEnd < drawableEnd) {
        gl::glEnable(gl::GL_BLEND);
        gl::glBlendFunc(gl::GL_SRC_ALPHA, gl::GL_ONE_MINUS_SRC_ALPHA);
        gl::glDepthMask(0);
        for (usize i = opaqueEnd; i < drawableEnd; ++i) G.RenderItem(G.items[i], stats_);
        gl::glDepthMask(1);
        gl::glDisable(gl::GL_BLEND);
    }

    // ---- 4. пользовательские пост-проходы (частицы и другие владельцы шейдеров) ---
    if (!G.postDraws.empty()) {
        for (auto& fn : G.postDraws) {
            if (fn) fn();
        }
        G.postDraws.clear();
        gl::glDepthMask(1);
        gl::glEnable(gl::GL_DEPTH_TEST);
    }

    // ---- 5. отладочные линии ---------------------------------------------------
    G.FlushLines(stats_);

    // ---- 6. восстановление GL-состояния -------------------------------------
    gl::glDepthMask(1);
    gl::glDepthFunc(gl::GL_LESS);
    gl::glEnable(gl::GL_DEPTH_TEST);
    gl::glDisable(gl::GL_BLEND);
    gl::glColorMask(1, 1, 1, 1);
    gl::glBindVertexArray(0);
    Shader::Unbind();
    G.boundProgram = nullptr;

    G.items.clear();
    G.instancePool.clear();
    G.postDraws.clear();
    G.skyRequested = false;
    G.gridRequested = false;

    const auto endTime = std::chrono::steady_clock::now();
    stats_.cpuFrameMs = std::chrono::duration<f32, std::milli>(endTime - startTime).count();
}

void Renderer3D::AddPostDraw(std::function<void()> fn) {
    if (!impl_ || !fn) return;
    impl_->postDraws.push_back(std::move(fn));
}

void Renderer3D::SetViewport(f32 x, f32 y, f32 w, f32 h) {
    if (!impl_) return;
    Impl& G = *impl_;
    G.customViewport = true;
    G.vpX = x;
    G.vpY = y;
    G.vpW = w;
    G.vpH = h;
    G.aspect = G.EffectiveAspect();
    viewProj_ = camera_.ViewProj(G.aspect);
    if (G.initialized) G.ApplyViewport();
}

void Renderer3D::ResetViewport() {
    if (!impl_) return;
    Impl& G = *impl_;
    G.customViewport = false;
    G.aspect = G.EffectiveAspect();
    viewProj_ = camera_.ViewProj(G.aspect);
    if (G.initialized) G.ApplyViewport();
}

// ---- источники света -------------------------------------------------------
int Renderer3D::AddLight(const Light& light) {
    if (settings_.maxLights <= 0) return -1;
    if (static_cast<int>(lights_.size()) >= MinT(settings_.maxLights, r3d_internal::kMaxLights)) {
        ENG_LOGW("r3d", "AddLight: light limit reached (%d)", settings_.maxLights);
        return -1;
    }
    Light l = light;
    l.id = impl_ ? impl_->nextLightId++ : static_cast<int>(lights_.size());
    lights_.push_back(l);
    return static_cast<int>(lights_.size()) - 1;
}

void Renderer3D::ClearLights() { lights_.clear(); }

Light* Renderer3D::GetLight(int index) {
    if (index < 0 || index >= static_cast<int>(lights_.size())) return nullptr;
    return &lights_[static_cast<usize>(index)];
}

// ---- drawing --------------------------------------------------------------
void Renderer3D::Draw(const Mesh& mesh, const Material& material, const Mat4& transform) {
    if (!impl_) return;
    if (!mesh.Valid() || mesh.IndexCount() == 0) return;  // никогда не отправляем пустую отрисовку
    Impl& G = *impl_;
    r3d_detail::DrawItem item;
    item.mesh = &mesh;
    item.material = material;  // копируем: вызывающий может передать временный объект
    item.transform = transform;
    item.normalMatrix = transform.NormalMatrix();
    Vec3 lo{-0.5f, -0.5f, -0.5f};
    Vec3 hi{0.5f, 0.5f, 0.5f};
    if (mesh.Bounds().Valid()) {
        lo = mesh.Bounds().min;
        hi = mesh.Bounds().max;
    }
    r3d_internal::TransformAabb(transform, lo, hi, &item.worldMin, &item.worldMax);
    G.items.push_back(std::move(item));
}

void Renderer3D::DrawInstanced(const Mesh& mesh, const Material& material, const Mat4* transforms,
                               int count) {
    if (!impl_ || !transforms || count <= 0) return;
    if (!mesh.Valid() || mesh.IndexCount() == 0) return;
    Impl& G = *impl_;
    r3d_detail::DrawItem item;
    item.mesh = &mesh;
    item.material = material;
    item.transform = Mat4::Identity();
    item.normalMatrix = Mat4::Identity();
    item.instanceCount = count;
    item.instanceOffset = static_cast<int>(G.instancePool.size());
    G.instancePool.insert(G.instancePool.end(), transforms, transforms + count);

    Vec3 lo{-0.5f, -0.5f, -0.5f};
    Vec3 hi{0.5f, 0.5f, 0.5f};
    if (mesh.Bounds().Valid()) {
        lo = mesh.Bounds().min;
        hi = mesh.Bounds().max;
    }
    // Объединение габаритов по экземплярам: один бокс отсечения на весь батч.
    Vec3 mn{1e30f, 1e30f, 1e30f};
    Vec3 mx{-1e30f, -1e30f, -1e30f};
    for (int i = 0; i < count; ++i) {
        Vec3 a, b;
        r3d_internal::TransformAabb(transforms[i], lo, hi, &a, &b);
        mn = Min(mn, a);
        mx = Max(mx, b);
    }
    item.worldMin = mn;
    item.worldMax = mx;
    G.items.push_back(std::move(item));
}

void Renderer3D::DrawInstancedBuffer(const Mesh& mesh, const Material& material, u32 instanceBuffer,
                                     int count) {
    if (!impl_ || instanceBuffer == 0 || count <= 0) return;
    if (!mesh.Valid() || mesh.IndexCount() == 0) return;
    Impl& G = *impl_;
    r3d_detail::DrawItem item;
    item.mesh = &mesh;
    item.material = material;
    item.transform = Mat4::Identity();
    item.normalMatrix = Mat4::Identity();
    item.instanceCount = count;
    item.externalBuffer = instanceBuffer;
    Vec3 lo{-0.5f, -0.5f, -0.5f};
    Vec3 hi{0.5f, 0.5f, 0.5f};
    if (mesh.Bounds().Valid()) {
        lo = mesh.Bounds().min;
        hi = mesh.Bounds().max;
    }
    // Данными матриц владеет вызывающий, поэтому для отсечения известны только
    // локальные габариты (единичная трансформация).
    item.worldMin = lo;
    item.worldMax = hi;
    G.items.push_back(std::move(item));
}

void Renderer3D::DrawSky() {
    if (impl_) impl_->skyRequested = true;
}

void Renderer3D::DrawGrid(f32 size, int divisions, const Color& major, const Color& minor) {
    if (!impl_) return;
    Impl& G = *impl_;
    G.gridRequested = true;
    G.reqGridSize = size;
    G.reqGridDivisions = divisions;
    G.reqGridMajor = major;
    G.reqGridMinor = minor;
}

// ---- отладочная отрисовка ---------------------------------------------------
namespace {
// Добавляет линию, пока не исчерпан отладочный бюджет кадра.
bool PushDebugLine(std::vector<r3d_detail::LineVertex>& out, const Vec3& a, const Vec3& b,
                   const Color& c, bool depth, f32 width) {
    if (out.size() + 2 > kMaxLineVertices) return false;
    r3d_detail::PushLine(out, a, b, c, depth, width);
    return true;
}
}  // namespace

void Renderer3D::DrawLine(const Vec3& a, const Vec3& b, const Color& color, bool depthTest, f32 width) {
    if (!impl_) return;
    PushDebugLine(impl_->lineVerts, a, b, color, depthTest, width);
}

void Renderer3D::DrawLines(const Vec3* points, int count, const Color& color, bool depthTest) {
    if (!impl_ || !points || count < 2) return;
    // Связанная ломаная: count - 1 сегментов.
    for (int i = 0; i + 1 < count; ++i)
        PushDebugLine(impl_->lineVerts, points[i], points[i + 1], color, depthTest, 1.0f);
}

void Renderer3D::DrawWireBox(const Vec3& center, const Vec3& extents, const Color& color,
                             const Mat4& transform) {
    if (!impl_) return;
    const Vec3 e = Max(extents, Vec3{0, 0, 0});
    Vec3 c[8];
    for (int i = 0; i < 8; ++i) {
        const Vec3 local{(i & 1) ? e.x : -e.x, (i & 2) ? e.y : -e.y, (i & 4) ? e.z : -e.z};
        c[i] = transform.TransformPoint(center + local);
    }
    // 12 рёбер куба, индексируемых битами угла.
    static const int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                      {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& e2 : kEdges) DrawLine(c[e2[0]], c[e2[1]], color, true, 1.0f);
}

void Renderer3D::DrawWireBox(const Bounds& b, const Color& color, const Mat4& transform) {
    if (!b.Valid()) return;
    DrawWireBox(b.Center(), b.Extents(), color, transform);
}

void Renderer3D::DrawAabb(const Bounds& b, const Color& color) {
    if (!b.Valid()) return;
    DrawWireBox(b.Center(), b.Extents(), color, Mat4::Identity());
}

void Renderer3D::DrawSphere(const Vec3& center, f32 radius, const Color& color, int segments) {
    if (!impl_ || radius <= 0.0f) return;
    auto& out = impl_->lineVerts;
    const Vec3 rx{radius, 0, 0};
    const Vec3 ry{0, radius, 0};
    const Vec3 rz{0, 0, radius};
    r3d_detail::PushCircle(out, center, rx, ry, color, segments, true, 1.0f);  // XY
    r3d_detail::PushCircle(out, center, rx, rz, color, segments, true, 1.0f);  // XZ
    r3d_detail::PushCircle(out, center, ry, rz, color, segments, true, 1.0f);  // YZ
}

void Renderer3D::DrawArrow(const Vec3& from, const Vec3& to, const Color& color, f32 headSize) {
    if (!impl_) return;
    r3d_detail::PushArrow(impl_->lineVerts, from, to, color, headSize, true, 1.0f);
}

void Renderer3D::DrawGizmo(const Mat4& transform, f32 scale, bool depthTest) {
    if (!impl_) return;
    const Vec3 origin = transform.TransformPoint(Vec3{0, 0, 0});
    const Vec3 axes[3] = {transform.TransformDir(Vec3{1, 0, 0}), transform.TransformDir(Vec3{0, 1, 0}),
                          transform.TransformDir(Vec3{0, 0, 1})};
    const Color colors[3] = {{0.92f, 0.26f, 0.30f, 1.0f}, {0.45f, 0.86f, 0.32f, 1.0f},
                             {0.30f, 0.52f, 0.95f, 1.0f}};
    for (int i = 0; i < 3; ++i) {
        const f32 len = Length(axes[i]);
        if (len < 1e-6f) continue;
        const Vec3 to = origin + axes[i] * scale;
        r3d_detail::PushArrow(impl_->lineVerts, origin, to, colors[i], MaxT(0.08f, scale * 0.15f),
                              depthTest, 1.0f);
    }
}

void Renderer3D::DrawCapsule(const Vec3& a, const Vec3& b, f32 radius, const Color& color) {
    if (!impl_ || radius <= 0.0f) return;
    const Vec3 delta = b - a;
    const f32 len = Length(delta);
    Vec3 axis = len > 1e-6f ? delta / len : Vec3{0, 1, 0};
    Vec3 u = Normalize(Cross(axis, std::fabs(axis.y) > 0.95f ? Vec3{1, 0, 0} : Vec3{0, 1, 0}));
    Vec3 v = Cross(axis, u);
    auto& out = impl_->lineVerts;
    // Кольца полусфер + четыре боковые линии: читаемая каркасная капсула.
    r3d_detail::PushCircle(out, a, u * radius, v * radius, color, 16, true, 1.0f);
    r3d_detail::PushCircle(out, b, u * radius, v * radius, color, 16, true, 1.0f);
    for (int i = 0; i < 4; ++i) {
        const Vec3 dir = u * std::cos(static_cast<f32>(i) * kPi * 0.5f) +
                         v * std::sin(static_cast<f32>(i) * kPi * 0.5f);
        DrawLine(a + dir * radius, b + dir * radius, color, true, 1.0f);
    }
    DrawSphere(a, radius, color, 16);
    DrawSphere(b, radius, color, 16);
}

void Renderer3D::DrawFrustum(const Mat4& viewProj, const Color& color) {
    if (!impl_) return;
    const Mat4 inv = viewProj.Inverse();
    Vec3 c[8];
    for (int i = 0; i < 8; ++i) {
        const Vec3 ndc{(i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f};
        c[i] = inv.TransformPoint(ndc);
    }
    // Ближний (бит 4 сброшен) и дальний (бит 4 установлен) прямоугольники плюс соединяющие рёбра.
    static const int kEdges[12][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7},
                                      {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& e : kEdges) DrawLine(c[e[0]], c[e[1]], color, true, 1.0f);
}

// ---- пикинг -----------------------------------------------------------------
void Renderer3D::SubmitPickable(const Mesh& mesh, const Mat4& transform, int id, void* userData) {
    if (pickables_.size() >= 4096) {
        ENG_LOGW("r3d", "SubmitPickable: too many pickables (%zu)", pickables_.size());
        return;
    }
    PickableObject p;
    p.mesh = &mesh;
    p.transform = transform;
    p.inverse = transform.Inverse();
    p.id = id;
    p.userData = userData;
    pickables_.push_back(p);
}

void Renderer3D::ClearPickables() { pickables_.clear(); }

bool Renderer3D::Raycast(const Ray& ray, RayHit* hit, f32 maxDistance) {
    if (hit) *hit = RayHit{};
    if (pickables_.empty()) return false;
    const Vec3 rd = LengthSq(ray.direction) > 1e-12f ? Normalize(ray.direction) : Vec3{0, 0, -1};

    bool any = false;
    RayHit best;
    best.distance = maxDistance;

    for (const PickableObject& p : pickables_) {
        if (!p.mesh || !p.mesh->Bounds().Valid()) continue;
        const Vec3 lo = p.mesh->Bounds().min;
        const Vec3 hi = p.mesh->Bounds().max;
        // Параметр луча сохраняется при обратном преобразовании, поэтому
        // дистанции остаются в мировых единицах даже при масштабировании моделью.
        const Vec3 localOrigin = p.inverse.TransformPoint(ray.origin);
        const Vec3 localDir = p.inverse.TransformDir(rd);

        RayHit candidate;
        candidate.objectId = p.id;
        candidate.userData = p.userData;
        candidate.meshName = p.mesh->Name();

        const MeshData* data = r3d_internal::FindMeshData(p.mesh);
        if (data && !data->indices.empty()) {
            f32 t = 0.0f;
            int tri = -1;
            Vec3 n{0, 1, 0};
            Vec2 uv;
            if (!r3d_internal::RayMeshTriangles(localOrigin, localDir, *data, best.distance, &t, &tri,
                                                &n, &uv))
                continue;
            candidate.distance = t;
            candidate.triangleIndex = tri;
            candidate.point = ray.origin + rd * t;
            candidate.normal = Normalize(p.transform.NormalMatrix().TransformDir(n));
            candidate.uv = uv;
        } else {
            // Откат: GPU-меш не отдаёт данные вершин, поэтому лучшее доступное
            // попадание — преобразованный габаритный бокс.
            f32 t = 0.0f;
            if (!r3d_internal::RayAabbTest(localOrigin, localDir, lo, hi, &t)) continue;
            if (t > best.distance) continue;
            const Vec3 localPoint = localOrigin + localDir * t;
            candidate.distance = t;
            candidate.triangleIndex = -1;
            candidate.point = ray.origin + rd * t;
            // Нормаль грани — по той границе бокса, на которой лежит точка попадания.
            Vec3 ln{0, 1, 0};
            const f32 eps = 1e-4f * MaxT(1.0f, Length(hi - lo));
            if (std::fabs(localPoint.x - lo.x) < eps) ln = {-1, 0, 0};
            else if (std::fabs(localPoint.x - hi.x) < eps) ln = {1, 0, 0};
            else if (std::fabs(localPoint.y - lo.y) < eps) ln = {0, -1, 0};
            else if (std::fabs(localPoint.y - hi.y) < eps) ln = {0, 1, 0};
            else if (std::fabs(localPoint.z - lo.z) < eps) ln = {0, 0, -1};
            else ln = {0, 0, 1};
            candidate.normal = Normalize(p.transform.NormalMatrix().TransformDir(ln));
            const Vec3 size = Max(hi - lo, Vec3{1e-6f, 1e-6f, 1e-6f});
            candidate.uv = Vec2{Clamp((localPoint.x - lo.x) / size.x, 0.0f, 1.0f),
                                Clamp((localPoint.z - lo.z) / size.z, 0.0f, 1.0f)};
        }
        if (candidate.distance < best.distance || !any) {
            best = candidate;
            any = true;
        }
    }
    if (!any) return false;
    best.hit = true;
    if (hit) *hit = best;
    return true;
}

bool Renderer3D::PickAtScreen(Vec2 screenPos, Vec2 viewportSize, RayHit* hit) {
    Vec3 origin, dir;
    camera_.RayFromScreen(screenPos, viewportSize, &origin, &dir);
    Ray ray{origin, dir};
    return Raycast(ray, hit);
}



}  // namespace crossrender
