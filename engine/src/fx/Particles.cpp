// 3D-система частиц: CPU-симуляция с кривыми по времени жизни, дружественный
// к GPU инстансированный рендеринг billboard/mesh, режимы смешивания,
// суб-эмиттеры, аттракторы, коллизии и следы.
//
// Замечания по дизайну / задокументированные решения:
//
//  * Рендеринг использует встроенный инстансированный шейдер частиц движка
//    (`builtin::kParticleVert` / `kParticleFrag`). Он потребляет один
//    инстанс-буфер с раскладкой
//        location 3 : vec4 iPosSize    (xyz позиция, w размер)
//        location 4 : vec4 iColor      (rgba, альфа уже ослаблена)
//        location 5 : vec4 iRotStretch (x поворот, y растяжение, zw резерв)
//        location 6 : vec4 iVelLife    (xyz скорость, w нормированный возраст)
//    (`ParticleInstance` ниже — ровно 64 байта и совпадает с этой раскладкой).
//    `Renderer3D::DrawInstancedBuffer` передаётся id VBO; мы также сами настраиваем
//    per-instance вершинные атрибуты на VAO меша (шаг 64, divisor 1), поэтому
//    отрисовка работает независимо от того, делает ли это рендерер.
//
//  * VAO меша движка привязывает normal -> location 1 и uv -> location 2
//    (см. Mesh.cpp), а вершинный шейдер частиц объявляет `aUV` в
//    location 1 и (неиспользуемый) `aNormal` в location 2. Поэтому единичный
//    квад, собранный здесь, хранит UV в поле `normal`, чтобы шейдер
//    получил их в location 1. Частицы в режиме Mesh наследуют это
//    несоответствие уровня движка (их UV/наложение текстуры могут отличаться);
//    код частиц не может это исправить, не меняя замороженные модули Mesh/Shader.
//
//  * Формы эмиттеров: Cone разбрасывает вокруг оси +Y трансформа, причём
//    `coneAngle` — *половинный* угол (равномерная выборка по телесному углу); Sphere
//    выбирается равномерно по объёму (rejection-выборка внутри единичного шара,
//    масштабированная выбранной радиусом); Box/Edge `shapeBox` — полные протяжённости;
//    Circle лежит в плоскости XZ; Hemisphere — +Y. Форма `Mesh` выбирает
//    случайную точку внутри AABB меша, потому что `Mesh` не экспонирует CPU
//    данные вершин/треугольников в замороженном публичном API
//    (поэтому взвешенная по площади треугольников выборка невозможна).
//
//  * Следы реализованы цепочкой растянутых квадов: каждая частица хранит
//    кольцевой буфер последних 8 позиций и при рендере выпускает по одному
//    инстансу stretched-billboard на сегмент (задокументированная альтернатива
//    отдельному ribbon-шейдеру, которого встроенный шейдер не даёт).
//
//  * Мягкие частицы используют аналитический дистанционный фейд, считаемый на CPU
//    (дистанция до камеры и, для наземных коллайдеров, близость к `groundY`).
//    `Renderer3D::SceneColor()` экспонирует HDR-*цветовую* цель, а не depth
//    буфер, поэтому выборка глубины сцены недоступна; uniform `uSoftFade`
//    шейдера остаётся 1, а альфа ослабляется в `BuildInstances`.
//
//  * Встроенный фрагментный шейдер использует premultiplied alpha (`rgb *= a`),
//    поэтому GL-факторы смешивания основаны на ONE для режимов
//    alpha/premultiplied/additive (см. `ApplyBlendState`). Depth test остаётся
//    включённым, depth write выключен, если эмиттер не `lit` и не `castShadows`.
//
//  * `speedOverLifetime` модулирует интегрируемое движение (позиция продвигается
//    на velocity * curve(t) * dt), а не пересчитывает накопленные силы, поэтому
//    постоянная кривая (по умолчанию) — строгий no-op.
//
//  * Встроенный шейдер частиц неосвещённый и без члена тени, поэтому `lit` /
//    `castShadows` влияют на флаги материала и состояние depth write
//    (depth write выключен, пока не задан ни один), а не на затенение;
//    `alignToVelocity` присущ StretchedBillboard/Trail (во встроенных
//    ветках billboard/mesh нет uniform выравнивания по скорости).
//
//  * Частицы `SimulationSpace::Local` симулируются в локальном пространстве;
//    трансформ эмиттера применяется на CPU при заполнении инстанс-буфера
//    (встроенный вершинный шейдер применяет `uModel` только в режиме mesh),
//    а `uModel` всегда единичная матрица.
//
//  * Эмиссия использует фиксированный подшаг не более 1/60 с; dt ограничивается
//    4 с, чтобы ограничить худший случай работы. Пулы выделяются один раз в Init
//    и переиспользуются; симуляция не делает heap-выделений на частицу.
//
//  * `Init` сразу выделяет CPU-пулы и засеивает RNG, но GPU-ресурсы
//    (инстанс VBO, единичный квад, программа шейдера частиц) создаются
//    лениво при первом вызове `Render`: у `Init` нет контекста рендерера/GL,
//    и он должен оставаться пригодным headless (тестовый набор зовёт Init/Simulate
//    без контекста). Неудачное ленивое создание оставляет систему симулирующей,
//    но пропускает отрисовку.

#include "crossrender/fx/Particles.h"

#include "crossrender/gfx/GL.h"
#include "crossrender/core/Log.h"
#include "crossrender/gfx/Shader.h"
#include "crossrender/gfx/Texture.h"
#include "crossrender/gfx/Renderer3D.h"

#include <cmath>
#include <string>
#include <vector>
#include <cstddef>
#include <algorithm>
#include <unordered_map>

namespace crossrender {
namespace {

// ---------------------------------------------------------------------------
// Кубическая bezier-интерполяция (локальная; намеренно независима от отдельно
// реализованного crossrender::CubicBezierEase, чтобы модуль линковался сам).
// ---------------------------------------------------------------------------
inline f32 BezierAxis(f32 p1, f32 p2, f32 u) {
    const f32 v = 1.0f - u;
    return 3.0f * v * v * u * p1 + 3.0f * v * u * u * p2 + u * u * u;
}

inline f32 BezierSlope(f32 p1, f32 p2, f32 u) {
    const f32 v = 1.0f - u;
    return 3.0f * v * v * p1 + 6.0f * v * u * (p2 - p1) + 3.0f * u * u * (1.0f - p2);
}

// Newton-Raphson с откатом к бисекции (решатель в стиле CSS/Lottie).
f32 SolveBezierU(f32 x1, f32 x2, f32 x) {
    x1 = Clamp(x1, 0.0f, 1.0f);
    x2 = Clamp(x2, 0.0f, 1.0f);
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    f32 u = x;
    bool converged = false;
    for (int i = 0; i < 8; ++i) {
        const f32 err = BezierAxis(x1, x2, u) - x;
        if (std::fabs(err) < 1e-6f) {
            converged = true;
            break;
        }
        const f32 d = BezierSlope(x1, x2, u);
        if (std::fabs(d) < 1e-5f) break;
        u -= err / d;
        if (u < 0.0f || u > 1.0f) {
            u = 0.5f;
            break;
        }
    }
    if (!converged) {
        f32 lo = 0.0f, hi = 1.0f;
        for (int i = 0; i < 32; ++i) {
            const f32 mid = 0.5f * (lo + hi);
            if (BezierAxis(x1, x2, mid) < x)
                lo = mid;
            else
                hi = mid;
        }
        u = 0.5f * (lo + hi);
    }
    return u;
}

// ---------------------------------------------------------------------------
// Детерминированный 3D value-шум (+ curl через конечные разности) для турбулентности.
// ---------------------------------------------------------------------------
inline u32 HashLattice(i32 x, i32 y, i32 z, u32 seed) {
    u32 h = seed + 0x9E3779B9u;
    h ^= static_cast<u32>(x) * 0x85EBCA6Bu;
    h = (h << 13) | (h >> 19);
    h ^= static_cast<u32>(y) * 0xC2B2AE35u;
    h = (h << 17) | (h >> 15);
    h ^= static_cast<u32>(z) * 0x27D4EB2Fu;
    h ^= h >> 15;
    h *= 0x2545F491u;
    h ^= h >> 13;
    return h;
}

inline f32 LatticeValue(i32 x, i32 y, i32 z, u32 seed) {
    return static_cast<f32>(HashLattice(x, y, z, seed) >> 8) * (1.0f / 16777216.0f);
}

// Трилинейно интерполированный решёточный шум в [-1, 1].
f32 ValueNoise3(const Vec3& p, u32 seed) {
    const f32 fx = std::floor(p.x), fy = std::floor(p.y), fz = std::floor(p.z);
    const i32 ix = static_cast<i32>(fx), iy = static_cast<i32>(fy), iz = static_cast<i32>(fz);
    f32 tx = p.x - fx, ty = p.y - fy, tz = p.z - fz;
    tx = tx * tx * (3.0f - 2.0f * tx);
    ty = ty * ty * (3.0f - 2.0f * ty);
    tz = tz * tz * (3.0f - 2.0f * tz);
    const f32 c000 = LatticeValue(ix, iy, iz, seed);
    const f32 c100 = LatticeValue(ix + 1, iy, iz, seed);
    const f32 c010 = LatticeValue(ix, iy + 1, iz, seed);
    const f32 c110 = LatticeValue(ix + 1, iy + 1, iz, seed);
    const f32 c001 = LatticeValue(ix, iy, iz + 1, seed);
    const f32 c101 = LatticeValue(ix + 1, iy, iz + 1, seed);
    const f32 c011 = LatticeValue(ix, iy + 1, iz + 1, seed);
    const f32 c111 = LatticeValue(ix + 1, iy + 1, iz + 1, seed);
    const f32 x00 = Lerp(c000, c100, tx), x10 = Lerp(c010, c110, tx);
    const f32 x01 = Lerp(c001, c101, tx), x11 = Lerp(c011, c111, tx);
    const f32 y0 = Lerp(x00, x10, ty), y1 = Lerp(x01, x11, ty);
    return Lerp(y0, y1, tz) * 2.0f - 1.0f;
}

Vec3 PotentialField(const Vec3& p, u32 seed) {
    return {ValueNoise3(p, seed),
            ValueNoise3(p + Vec3(31.416f, 47.853f, 12.793f), seed ^ 0x51ED270Bu),
            ValueNoise3(p + Vec3(-17.24f, 83.15f, 5.71f), seed ^ 0xA3C59AC3u)};
}

// Почти бездивергентное («curl-шум») направление ускорения.
Vec3 CurlNoise(const Vec3& p, u32 seed, f32 eps) {
    const Vec3 dx{eps, 0, 0}, dy{0, eps, 0}, dz{0, 0, eps};
    const Vec3 px1 = PotentialField(p + dx, seed), px0 = PotentialField(p - dx, seed);
    const Vec3 py1 = PotentialField(p + dy, seed), py0 = PotentialField(p - dy, seed);
    const Vec3 pz1 = PotentialField(p + dz, seed), pz0 = PotentialField(p - dz, seed);
    const f32 inv = 0.5f / eps;
    const Vec3 dPx = (px1 - px0) * inv;
    const Vec3 dPy = (py1 - py0) * inv;
    const Vec3 dPz = (pz1 - pz0) * inv;
    return {dPz.y - dPy.z, dPx.z - dPz.x, dPy.x - dPx.y};
}

// ---------------------------------------------------------------------------
// Помощники кривых (константные быстрые пути удешевляют каждый подшаг).
// ---------------------------------------------------------------------------
bool CurveIsConstant(const Curve& c, f32* out) {
    if (c.keys.empty()) {
        if (out) *out = c.minValue;
        return true;
    }
    const f32 v = c.keys[0].v;
    for (const Curve::Key& k : c.keys) {
        if (k.v != v) return false;
    }
    if (out) *out = v;
    return true;
}

bool ColorCurveIsConstant(const ColorCurve& c, Color* out) {
    if (c.keys.empty()) {
        if (out) *out = Color(1, 1, 1, 1);
        return true;
    }
    const Color v = c.keys[0].second;
    for (const auto& kv : c.keys) {
        if (!(kv.second == v)) return false;
    }
    if (out) *out = v;
    return true;
}

Color EvalColorCurve(const ColorCurve& c, f32 t) {
    if (c.keys.empty()) return Color(1, 1, 1, 1);
    if (c.keys.size() == 1) return c.keys[0].second;
    if (t <= c.keys.front().first) return c.keys.front().second;
    if (t >= c.keys.back().first) return c.keys.back().second;
    usize lo = 0, hi = c.keys.size() - 1;
    while (hi - lo > 1) {
        const usize mid = (lo + hi) / 2;
        if (c.keys[mid].first <= t)
            lo = mid;
        else
            hi = mid;
    }
    const auto& a = c.keys[lo];
    const auto& b = c.keys[lo + 1];
    const f32 dt = b.first - a.first;
    const f32 u = dt > kEpsilon ? (t - a.first) / dt : 0.0f;
    return Lerp(a.second, b.second, u);
}

// Аналитический фейд мягких частиц (см. комментарий в шапке: Renderer3D не
// экспонирует depth-цель, поэтому это заменяет фейд пересечения по глубине).
f32 SoftFadeFactor(const ParticleEmitterDesc& d, const Particle& p, const Vec3& cameraPos) {
    const f32 dist = MaxT(d.softFadeDistance, 1e-3f);
    f32 fade = Clamp(Distance(p.position, cameraPos) / dist, 0.0f, 1.0f);
    if (d.collideGround) fade *= Clamp((p.position.y - d.groundY) / dist, 0.0f, 1.0f);
    return fade;
}

// ParticleBlendMode -> BlendMode движка.
BlendMode EngineBlendMode(ParticleBlendMode mode) {
    switch (mode) {
        case ParticleBlendMode::Alpha: return BlendMode::Alpha;
        case ParticleBlendMode::Additive: return BlendMode::Additive;
        case ParticleBlendMode::Multiply: return BlendMode::Multiply;
        case ParticleBlendMode::Premultiplied: return BlendMode::Premultiplied;
        case ParticleBlendMode::Screen: return BlendMode::Screen;
    }
    return BlendMode::Alpha;
}

// Состояние смешивания GL для BlendMode движка. Встроенный фрагментный
// шейдер частиц делает premultiplied alpha (rgb *= a), отсюда источники на базе ONE.
void ApplyBlendState(BlendMode mode) {
    if (!gl::glEnable || !gl::glBlendFunc) return;
    gl::glEnable(gl::GL_BLEND);
    switch (mode) {
        case BlendMode::Alpha:
        case BlendMode::Premultiplied:
            gl::glBlendFunc(gl::GL_ONE, gl::GL_ONE_MINUS_SRC_ALPHA);
            break;
        case BlendMode::Additive:
            gl::glBlendFunc(gl::GL_ONE, gl::GL_ONE);
            break;
        case BlendMode::Multiply:
            gl::glBlendFunc(gl::GL_DST_COLOR, gl::GL_ZERO);
            break;
        case BlendMode::Screen:
            gl::glBlendFunc(gl::GL_ONE, gl::GL_ONE_MINUS_SRC_COLOR);
            break;
        default:
            gl::glBlendFunc(gl::GL_ONE, gl::GL_ONE_MINUS_SRC_ALPHA);
            break;
    }
}

i32 RenderModeIndex(ParticleRenderMode mode) {
    switch (mode) {
        case ParticleRenderMode::Billboard: return 0;
        case ParticleRenderMode::StretchedBillboard: return 1;
        case ParticleRenderMode::HorizontalBillboard: return 2;
        case ParticleRenderMode::VerticalBillboard: return 3;
        case ParticleRenderMode::Mesh: return 4;
        // У встроенного шейдера нет отдельного ribbon-режима; следы рисуются
        // цепочкой растянутых billboard (см. BuildInstances).
        case ParticleRenderMode::Trail: return 1;
    }
    return 0;
}

// Переводит позицию из пространства симуляции `from` в пространство `to`.
Vec3 ConvertSpace(const Vec3& p, SimulationSpace from, SimulationSpace to, const Mat4& xf) {
    if (from == to) return p;
    if (from == SimulationSpace::Local) return xf.TransformPoint(p);
    return xf.Inverse().TransformPoint(p);
}

constexpr int kTrailSamples = 8;
constexpr gl::GLuint kInstanceAttribLocations[4] = {3u, 4u, 5u, 6u};

}  // namespace

// ---------------------------------------------------------------------------
// Кривая
// ---------------------------------------------------------------------------
Curve Curve::Constant(f32 v) {
    Curve c;
    c.minValue = v;
    c.maxValue = v;
    Key k;
    k.t = 0.0f;
    k.v = v;
    c.keys.push_back(k);
    k.t = 1.0f;
    c.keys.push_back(k);
    return c;
}

Curve Curve::FromPoints(const std::vector<Vec2>& points, f32 smoothness) {
    Curve c;
    if (points.empty()) return c;
    if (points.size() == 1) return Constant(points[0].y);

    std::vector<Vec2> pts = points;
    std::stable_sort(pts.begin(), pts.end(),
                     [](const Vec2& a, const Vec2& b) { return a.x < b.x; });
    // Отбрасываем дублирующиеся моменты времени (побеждает последнее значение).
    std::vector<Vec2> uniq;
    uniq.reserve(pts.size());
    for (const Vec2& p : pts) {
        if (!uniq.empty() && std::fabs(uniq.back().x - p.x) < 1e-5f)
            uniq.back() = p;
        else
            uniq.push_back(p);
    }
    if (uniq.size() == 1) return Constant(uniq[0].y);

    const bool smooth = smoothness > kEpsilon;
    c.keys.reserve(uniq.size());
    for (const Vec2& p : uniq) {
        Key k;
        k.t = p.x;
        k.v = p.y;
        k.easing = smooth ? 1 : 0;
        c.keys.push_back(k);
        if (c.keys.size() == 1) {
            c.minValue = c.maxValue = p.y;
        } else {
            c.minValue = MinT(c.minValue, p.y);
            c.maxValue = MaxT(c.maxValue, p.y);
        }
    }

    if (!smooth) return c;

    // Касательные Catmull-Rom (конечные разности), преобразованные в handle-и bezier.
    const usize n = c.keys.size();
    auto tangentAt = [&](usize i) -> f32 {
        if (i == 0) {
            const f32 dt = c.keys[1].t - c.keys[0].t;
            return dt > kEpsilon ? smoothness * (c.keys[1].v - c.keys[0].v) / dt : 0.0f;
        }
        if (i + 1 >= n) {
            const f32 dt = c.keys[n - 1].t - c.keys[n - 2].t;
            return dt > kEpsilon ? smoothness * (c.keys[n - 1].v - c.keys[n - 2].v) / dt : 0.0f;
        }
        const f32 dt = c.keys[i + 1].t - c.keys[i - 1].t;
        return dt > kEpsilon ? smoothness * (c.keys[i + 1].v - c.keys[i - 1].v) / dt : 0.0f;
    };
    for (usize i = 0; i + 1 < n; ++i) {
        const f32 t0 = c.keys[i].t, t1 = c.keys[i + 1].t;
        const f32 dt = t1 - t0;
        if (dt <= kEpsilon) continue;
        const f32 third = dt / 3.0f;
        c.keys[i].outX = t0 + third;
        c.keys[i].outY = c.keys[i].v + tangentAt(i) * third;
        c.keys[i + 1].inX = t1 - third;
        c.keys[i + 1].inY = c.keys[i + 1].v - tangentAt(i + 1) * third;
    }
    return c;
}

f32 Curve::Evaluate(f32 t) const {
    if (keys.empty()) return minValue;
    if (keys.size() == 1) return keys[0].v;
    if (t <= keys.front().t) return keys.front().v;
    if (t >= keys.back().t) return keys.back().v;

    usize lo = 0, hi = keys.size() - 1;
    while (hi - lo > 1) {
        const usize mid = (lo + hi) / 2;
        if (keys[mid].t <= t)
            lo = mid;
        else
            hi = mid;
    }
    const Key& a = keys[lo];
    const Key& b = keys[lo + 1];
    if (a.easing == 2) return a.v;  // удержание
    const f32 span = b.t - a.t;
    const f32 u = span > kEpsilon ? (t - a.t) / span : 0.0f;
    if (a.easing == 1) {
        const f32 ux = SolveBezierU(a.outX, a.inX, u);
        const f32 uy = BezierAxis(a.outY, a.inY, ux);
        return a.v + (b.v - a.v) * uy;
    }
    return a.v + (b.v - a.v) * u;
}

void Curve::AddKey(f32 t, f32 v, int easing) {
    t = Clamp(t, 0.0f, 1.0f);
    Key k;
    k.t = t;
    k.v = v;
    k.easing = easing;
    if (keys.empty()) {
        minValue = maxValue = v;
    } else {
        minValue = MinT(minValue, v);
        maxValue = MaxT(maxValue, v);
    }
    usize i = 0;
    while (i < keys.size() && keys[i].t < t - 1e-6f) ++i;
    if (i < keys.size() && std::fabs(keys[i].t - t) < 1e-6f) {
        keys[i] = k;
        // Пересчитываем диапазон значений после замены.
        f32 lo = keys[0].v, hi = keys[0].v;
        for (const Key& key : keys) {
            lo = MinT(lo, key.v);
            hi = MaxT(hi, key.v);
        }
        minValue = lo;
        maxValue = hi;
        return;
    }
    keys.insert(keys.begin() + static_cast<std::ptrdiff_t>(i), k);
}

f32 Curve::RandomValue(Random& rng) const { return rng.Range(minValue, maxValue); }

// ---------------------------------------------------------------------------
// Цветовая кривая
// ---------------------------------------------------------------------------
ColorCurve ColorCurve::Constant(const Color& c) {
    ColorCurve cc;
    cc.keys.push_back({0.0f, c});
    cc.keys.push_back({1.0f, c});
    return cc;
}

ColorCurve ColorCurve::Gradient(const std::vector<std::pair<f32, Color>>& keys) {
    ColorCurve cc;
    cc.keys = keys;
    std::stable_sort(cc.keys.begin(), cc.keys.end(),
                     [](const std::pair<f32, Color>& a, const std::pair<f32, Color>& b) {
                         return a.first < b.first;
                     });
    return cc;
}

Color ColorCurve::Evaluate(f32 t) const { return EvalColorCurve(*this, t); }

// ---------------------------------------------------------------------------
// ParticleSystem::Impl
// ---------------------------------------------------------------------------
struct ParticleSystem::Impl {
    static constexpr int kMaxTotalParticles = 200000;
    static constexpr int kMaxSubEmitterDepth = 3;
    static constexpr f32 kMaxSubstep = 1.0f / 60.0f;

    // Должно совпадать с location 3..6 атрибутов builtin::kParticleVert.
    struct ParticleInstance {
        Vec4 posSize;     // xyz позиция (пространство симуляции/мира), w размер
        Vec4 color;       // rgba, альфа уже ослаблена
        Vec4 rotStretch;  // x поворот, y растяжение, zw резерв
        Vec4 velLife;     // xyz скорость, w нормированный возраст
    };
    static_assert(sizeof(ParticleInstance) == 64, "particle instance must be 4 x vec4");

    struct EmitterState {
        std::vector<Particle> pool;
        std::vector<int> freeList;
        std::vector<f32> baseSpeed;
        std::vector<f32> baseSize;
        std::vector<Vec3> trail;       // kTrailSamples записей на частицу
        std::vector<u32> trailCount;
        int liveCount = 0;
        f32 accumulator = 0.0f;
        f32 time = 0.0f;
        bool burstPending = false;
        bool emitting = false;
        bool trailMode = false;
        bool sizeConst = true, alphaConst = true, speedConst = true, colorConst = true;
        f32 sizeK = 1.0f, alphaK = 1.0f, speedK = 1.0f;
        Color colorK{1, 1, 1, 1};
    };

    struct FrameUniforms {
        Mat4 viewProj;
        Vec3 camPos, camRight, camUp, camForward;
        Vec3 lightDir, lightColor, ambient;
        bool fogEnabled = false;
        Color fogColor{0.6f, 0.68f, 0.8f, 1.0f};
        f32 fogDensity = 0.0f;
    };

    std::vector<EmitterState> states;
    std::unordered_map<std::string, int> nameIndex;
    Random rng;
    u64 seed = 12345;
    u32 noiseSeed = 0x51ED270Bu;

    Shader shader;
    Mesh quad;
    Texture fallbackTexture;
    u32 instanceVbo = 0;
    bool gpuTried = false;
    bool gpuReady = false;
    bool quadReady = false;
    bool shaderReady = false;

    std::vector<ParticleInstance> instances;  // неотсортированный черновик
    std::vector<ParticleInstance> sorted;     // буфер выгрузки
    std::vector<f32> depths;
    std::vector<u32> order;

    void Allocate(const std::vector<ParticleEmitterDesc>& descs);
    void RefreshCache(const std::vector<ParticleEmitterDesc>& descs);
    void Release();
    [[nodiscard]] int FindEmitter(const std::string& name) const;
    int Spawn(int ei, const std::vector<ParticleEmitterDesc>& descs, const Mat4& xf, int depth,
              const Vec3* overridePos, bool triggerSubs);
    void Step(int ei, const std::vector<ParticleEmitterDesc>& descs, const Mat4& xf, bool playing,
              f32 h);
    void PushTrailSamples();
    void UpdateBounds(::crossrender::Bounds* out) const;
    void SampleShape(const ParticleEmitterDesc& d, Vec3* pos, Vec3* dir);
    bool EnsureGPU();
    void DestroyGPU();
    int BuildInstances(int ei, const ParticleEmitterDesc& d, const Vec3& camPos, const Vec3& camFwd,
                       const Mat4& xf);
    void DrawEmitter(const ParticleEmitterDesc& d, const FrameUniforms& u, int count);
};

void ParticleSystem::Impl::Allocate(const std::vector<ParticleEmitterDesc>& descs) {
    states.clear();
    states.resize(descs.size());

    i64 requested = 0;
    for (const ParticleEmitterDesc& d : descs) requested += MaxT(1, d.maxParticles);
    f32 scale = 1.0f;
    if (requested > static_cast<i64>(kMaxTotalParticles)) {
        scale = static_cast<f32>(kMaxTotalParticles) / static_cast<f32>(requested);
        ENG_LOGW("fx",
                 "particle budget exceeded (%lld requested, cap %d): scaling every emitter pool by "
                 "%.4f",
                 static_cast<long long>(requested), kMaxTotalParticles, scale);
    }

    for (usize i = 0; i < descs.size(); ++i) {
        const ParticleEmitterDesc& d = descs[i];
        EmitterState& st = states[i];
        const int cap = MaxT(1, static_cast<int>(static_cast<f32>(MaxT(1, d.maxParticles)) * scale));
        st.pool.assign(static_cast<usize>(cap), Particle{});
        for (Particle& p : st.pool) {
            p.alive = false;
            p.age = 0.0f;
            p.lifetime = 1.0f;
        }
        st.freeList.resize(static_cast<usize>(cap));
        for (int k = 0; k < cap; ++k) st.freeList[static_cast<usize>(cap - 1 - k)] = k;
        st.baseSpeed.assign(static_cast<usize>(cap), 0.0f);
        st.baseSize.assign(static_cast<usize>(cap), 0.0f);
        st.liveCount = 0;
        st.accumulator = 0.0f;
        st.time = 0.0f;
        st.burstPending = d.burst > 0;
        st.emitting = false;
        st.trailMode = d.renderMode == ParticleRenderMode::Trail;
        if (st.trailMode) {
            st.trail.assign(static_cast<usize>(cap) * kTrailSamples, Vec3{});
            st.trailCount.assign(static_cast<usize>(cap), 0u);
        } else {
            st.trail.clear();
            st.trailCount.clear();
        }
        RefreshCache(descs);
    }
    int allocated = 0;
    for (const EmitterState& st : states) allocated += static_cast<int>(st.pool.size());
    ENG_LOGI("fx", "ParticleSystem: %d emitter(s), %d particle slot(s) allocated",
             static_cast<int>(descs.size()), allocated);
}

void ParticleSystem::Impl::RefreshCache(const std::vector<ParticleEmitterDesc>& descs) {
    for (usize i = 0; i < states.size() && i < descs.size(); ++i) {
        const ParticleEmitterDesc& d = descs[i];
        EmitterState& st = states[i];
        st.sizeConst = CurveIsConstant(d.sizeOverLifetime, &st.sizeK);
        st.alphaConst = CurveIsConstant(d.alphaOverLifetime, &st.alphaK);
        st.speedConst = CurveIsConstant(d.speedOverLifetime, &st.speedK);
        Color a(1, 1, 1, 1), b(1, 1, 1, 1);
        const bool startConst = ColorCurveIsConstant(d.startColor, &a);
        const bool overConst = ColorCurveIsConstant(d.colorOverLifetime, &b);
        st.colorConst = startConst && overConst;
        st.colorK = a * b;
        // Синхронизируем поиск по имени с правками пользователя Emitter(i).
        if (d.name != std::string()) nameIndex[d.name] = static_cast<int>(i);
        // История следа выделяется один раз; перевыделяем, только если режим
        // рендера переключали на/с Trail после Init (не выделение на каждый кадр).
        const bool wantTrail = d.renderMode == ParticleRenderMode::Trail;
        if (wantTrail != st.trailMode) {
            st.trailMode = wantTrail;
            if (wantTrail) {
                st.trail.assign(st.pool.size() * kTrailSamples, Vec3{});
                st.trailCount.assign(st.pool.size(), 0u);
            } else {
                st.trail.clear();
                st.trailCount.clear();
            }
        }
    }
}

void ParticleSystem::Impl::Release() {
    DestroyGPU();
    states.clear();
    nameIndex.clear();
    instances.clear();
    sorted.clear();
    depths.clear();
    order.clear();
}

int ParticleSystem::Impl::FindEmitter(const std::string& name) const {
    const auto it = nameIndex.find(name);
    return it == nameIndex.end() ? -1 : it->second;
}

void ParticleSystem::Impl::SampleShape(const ParticleEmitterDesc& d, Vec3* pos, Vec3* dir) {
    Vec3 p{0, 0, 0};
    Vec3 v{0, 1, 0};
    switch (d.shape) {
        case EmitterShape::Point:
            break;
        case EmitterShape::Sphere: {
            const Vec3 r = rng.InUnitSphere();
            const f32 radius = rng.Range(d.shapeRadius.min, d.shapeRadius.max);
            p = r * radius;
            v = LengthSq(r) > 1e-8f ? Normalize(r) : Vec3{0, 1, 0};
            break;
        }
        case EmitterShape::Hemisphere: {
            Vec3 r = rng.InUnitSphere();
            r.y = std::fabs(r.y);
            const f32 radius = rng.Range(d.shapeRadius.min, d.shapeRadius.max);
            p = r * radius;
            v = LengthSq(r) > 1e-8f ? Normalize(r) : Vec3{0, 1, 0};
            break;
        }
        case EmitterShape::Box: {
            const Vec3 h = d.shapeBox * 0.5f;
            p = {rng.Range(-h.x, h.x), rng.Range(-h.y, h.y), rng.Range(-h.z, h.z)};
            break;
        }
        case EmitterShape::Cone: {
            // Ось +Y, `coneAngle` — половинный угол, равномерно по телесному углу
            // конуса; позиция лежит внутри объёма конуса.
            const f32 half = Clamp(d.coneAngle, 0.0f, kPi * 0.999f);
            const f32 ct = 1.0f - rng.NextFloat() * (1.0f - std::cos(half));
            const f32 st = std::sqrt(MaxT(0.0f, 1.0f - ct * ct));
            const f32 phi = rng.Range(0.0f, kTau);
            v = {st * std::cos(phi), ct, st * std::sin(phi)};
            const f32 radius = MaxT(rng.Range(d.shapeRadius.min, d.shapeRadius.max), 1e-4f);
            p = v * radius;
            break;
        }
        case EmitterShape::Circle: {
            const f32 radius = rng.Range(d.shapeRadius.min, d.shapeRadius.max);
            const Vec2 c = rng.OnUnitCircle();
            p = {c.x * radius, 0.0f, c.y * radius};
            break;
        }
        case EmitterShape::Edge: {
            const f32 h = d.shapeBox.x * 0.5f;
            p = {rng.Range(-h, h), 0.0f, 0.0f};
            break;
        }
        case EmitterShape::Mesh: {
            // `Mesh` не экспонирует данные вершин на CPU, поэтому выбираем точку в AABB меша.
            ::crossrender::Bounds b;
            if (d.mesh) b = d.mesh->Bounds();
            if (!b.Valid()) {
                const Vec3 h = d.shapeBox * 0.5f;
                b.min = -h;
                b.max = h;
            }
            p = {rng.Range(b.min.x, b.max.x), rng.Range(b.min.y, b.max.y),
                 rng.Range(b.min.z, b.max.z)};
            break;
        }
    }
    *pos = p;
    *dir = v;
}

int ParticleSystem::Impl::Spawn(int ei, const std::vector<ParticleEmitterDesc>& descs,
                                const Mat4& xf, int depth, const Vec3* overridePos,
                                bool triggerSubs) {
    if (ei < 0 || ei >= static_cast<int>(states.size())) return -1;
    EmitterState& st = states[static_cast<usize>(ei)];
    int slot = -1;
    while (!st.freeList.empty()) {
        const int idx = st.freeList.back();
        st.freeList.pop_back();
        if (!st.pool[static_cast<usize>(idx)].alive) {
            slot = idx;
            break;
        }
    }
    if (slot < 0) return -1;  // пул исчерпан

    const ParticleEmitterDesc& d = descs[static_cast<usize>(ei)];
    Particle& p = st.pool[static_cast<usize>(slot)];

    Vec3 localPos, localDir;
    SampleShape(d, &localPos, &localDir);
    const Vec3 simPos = overridePos ? *overridePos : localPos;
    Vec3 worldPos = simPos;
    Vec3 worldDir = localDir;
    if (d.space == SimulationSpace::World) {
        worldPos = xf.TransformPoint(simPos);
        worldDir = Normalize(xf.TransformDir(localDir));
        if (LengthSq(worldDir) < 1e-12f) worldDir = Vec3{0, 1, 0};
    }

    const f32 speed = d.startSpeed.Sample(rng);
    const f32 size = MaxT(d.startSize.Sample(rng) * d.sizeVariance.Sample(rng), 0.0f);

    p.position = worldPos;
    p.spawnPosition = worldPos;
    p.velocity = worldDir * speed;
    p.lifetime = MaxT(d.lifetime.Sample(rng), 1e-3f);
    p.age = 0.0f;
    p.rotation = d.startRotation.Sample(rng);
    p.angularVelocity = d.startAngularVelocity.Sample(rng);
    p.seed = rng.NextFloat();
    p.alive = true;
    p.size = size;
    p.color = EvalColorCurve(d.colorOverLifetime, 0.0f) * EvalColorCurve(d.startColor, 0.0f);
    p.color.a *= d.alphaOverLifetime.Evaluate(0.0f);

    st.baseSpeed[static_cast<usize>(slot)] = speed;
    st.baseSize[static_cast<usize>(slot)] = size;
    st.liveCount++;
    if (st.trailMode && !st.trail.empty()) {
        st.trail[static_cast<usize>(slot) * kTrailSamples] = worldPos;
        st.trailCount[static_cast<usize>(slot)] = 1u;
    }

    if (triggerSubs && depth < kMaxSubEmitterDepth) {
        for (const ParticleEmitterDesc::SubEmitter& sub : d.subEmitters) {
            if (sub.trigger != 0) continue;
            const int target = FindEmitter(sub.emitterName);
            if (target < 0) continue;
            for (int k = 0; k < sub.count; ++k)
                Spawn(target, descs, xf, depth + 1, nullptr, true);
        }
    }
    return slot;
}

void ParticleSystem::Impl::Step(int ei, const std::vector<ParticleEmitterDesc>& descs,
                                const Mat4& xf, bool playing, f32 h) {
    if (ei < 0 || ei >= static_cast<int>(states.size())) return;
    EmitterState& st = states[static_cast<usize>(ei)];
    const ParticleEmitterDesc& d = descs[static_cast<usize>(ei)];

    // ---- эмиссия ---------------------------------------------------------
    const bool withinDuration = d.duration <= 0.0f || st.time < d.duration;
    const bool canEmit = playing && withinDuration;
    st.emitting = canEmit;
    if (canEmit) {
        if (st.burstPending) {
            st.burstPending = false;
            for (int k = 0; k < d.burst; ++k) Spawn(ei, descs, xf, 0, nullptr, true);
        }
        st.accumulator += d.rate * h;
        int budget = static_cast<int>(st.pool.size()) - st.liveCount;
        while (st.accumulator >= 1.0f && budget > 0) {
            if (Spawn(ei, descs, xf, 0, nullptr, true) < 0) break;
            st.accumulator -= 1.0f;
            --budget;
        }
        if (st.accumulator > 1.0f) st.accumulator = 1.0f;  // пул полон: ограничиваем накопленное
        st.time += h;
        if (d.duration > 0.0f && st.time >= d.duration) {
            if (d.looping) {
                st.time = std::fmod(st.time, d.duration);
                if (d.burst > 0) st.burstPending = true;
            }
        }
    }

    // ---- интегрирование по частицам --------------------------------------
    const u32 emitterNoise = noiseSeed ^ (static_cast<u32>(ei + 1) * 0x9E3779B9u);
    const f32 dragFactor = d.drag > 0.0f ? std::exp(-d.drag * h) : 1.0f;
    const f32 turbFreq = MaxT(d.turbulenceFrequency, 1e-4f);
    const bool hasTurb = d.turbulenceStrength != 0.0f;
    const bool hasAttractors = !d.attractors.empty();
    const bool hasVortex = d.vortexStrength != 0.0f && LengthSq(d.vortexAxis) > 1e-12f;
    Quat vortexRot = Quat::Identity();
    if (hasVortex)
        vortexRot = Quat::FromAxisAngle(Normalize(d.vortexAxis), d.vortexStrength * h);

    const usize cap = st.pool.size();
    for (usize i = 0; i < cap; ++i) {
        Particle& p = st.pool[i];
        if (!p.alive) continue;

        p.age += h;
        if (p.age >= p.lifetime) {
            const Vec3 deathPos = p.position;  // копия: Spawn может переиспользовать слот
            p.alive = false;
            --st.liveCount;
            st.freeList.push_back(static_cast<int>(i));
            if (i < st.trailCount.size()) st.trailCount[i] = 0u;
            for (const ParticleEmitterDesc::SubEmitter& sub : d.subEmitters) {
                if (sub.trigger != 1) continue;
                const int target = FindEmitter(sub.emitterName);
                if (target < 0) continue;
                const Vec3 targetPos =
                    ConvertSpace(deathPos, d.space, descs[static_cast<usize>(target)].space, xf);
                for (int k = 0; k < sub.count; ++k)
                    Spawn(target, descs, xf, 1, &targetPos, true);
            }
            continue;
        }

        const f32 t = p.NormalizedAge();

        // Силы.
        Vec3 accel = d.gravity + d.wind;
        if (hasTurb) {
            const Vec3 q = p.position * turbFreq + Vec3(0.0f, st.time * 1.7f, st.time * 0.9f);
            accel += CurlNoise(q, emitterNoise, 0.35f) * d.turbulenceStrength;
        }
        if (hasAttractors) {
            for (const ParticleEmitterDesc::Attractor& a : d.attractors) {
                const Vec3 delta = a.position - p.position;
                const f32 dist2 = LengthSq(delta);
                const f32 radius = MaxT(a.radius, 1e-4f);
                if (dist2 > radius * radius || dist2 < 1e-8f) continue;
                const f32 dist = std::sqrt(dist2);
                const f32 falloff = 1.0f - dist / radius;
                const f32 mag = a.strength * falloff / MaxT(dist2, 0.01f);
                accel += (delta / dist) * mag;
            }
        }
        p.velocity += accel * h;
        if (dragFactor != 1.0f) p.velocity *= dragFactor;
        if (hasVortex) p.velocity = vortexRot * p.velocity;

        // Интегрируем (speedOverLifetime модулирует интегрируемое движение).
        const f32 speedScale = st.speedConst ? st.speedK : d.speedOverLifetime.Evaluate(t);
        p.position += p.velocity * (speedScale * h);
        p.rotation += p.angularVelocity * h;

        // Столкновение с землёй.
        if (d.collideGround && p.position.y < d.groundY) {
            const bool impact = p.velocity.y < 0.0f;
            p.position.y = d.groundY;
            if (impact) {
                const f32 bounce = Clamp(d.bounce, 0.0f, 1.0f);
                const f32 friction = Clamp(d.friction, 0.0f, 1.0f);
                const f32 vy = -p.velocity.y * bounce;
                p.velocity.y = vy <= 0.02f ? 0.0f : vy;
                if (bounce <= 0.0f) {
                    p.velocity.x = 0.0f;
                    p.velocity.z = 0.0f;
                } else {
                    p.velocity.x *= (1.0f - friction);
                    p.velocity.z *= (1.0f - friction);
                }
                for (const ParticleEmitterDesc::SubEmitter& sub : d.subEmitters) {
                    if (sub.trigger != 2) continue;
                    const int target = FindEmitter(sub.emitterName);
                    if (target < 0) continue;
                    const Vec3 targetPos =
                        ConvertSpace(p.position, d.space, descs[static_cast<usize>(target)].space, xf);
                    for (int k = 0; k < sub.count; ++k)
                        Spawn(target, descs, xf, 1, &targetPos, true);
                }
            }
        }

        // Обновляем производимое, видимое извне состояние.
        p.size = st.baseSize[i] *
                 MaxT(st.sizeConst ? st.sizeK : d.sizeOverLifetime.Evaluate(t), 0.0f);
        Color c;
        if (st.colorConst)
            c = st.colorK;
        else
            c = EvalColorCurve(d.startColor, t) * EvalColorCurve(d.colorOverLifetime, t);
        c.a *= st.alphaConst ? st.alphaK : d.alphaOverLifetime.Evaluate(t);
        p.color = c;
    }
}

void ParticleSystem::Impl::PushTrailSamples() {
    for (EmitterState& st : states) {
        if (!st.trailMode || st.trail.empty()) continue;
        for (usize i = 0; i < st.pool.size(); ++i) {
            if (!st.pool[i].alive) {
                st.trailCount[i] = 0u;
                continue;
            }
            Vec3* base = &st.trail[i * kTrailSamples];
            const u32 n = st.trailCount[i];
            if (n > 0 && Distance(base[0], st.pool[i].position) < 1e-5f) continue;
            const u32 shift = n < static_cast<u32>(kTrailSamples) ? n
                                                                  : static_cast<u32>(kTrailSamples) - 1u;
            for (u32 k = shift; k > 0; --k) base[k] = base[k - 1];
            base[0] = st.pool[i].position;
            st.trailCount[i] = MinT<u32>(n + 1u, static_cast<u32>(kTrailSamples));
        }
    }
}

void ParticleSystem::Impl::UpdateBounds(::crossrender::Bounds* out) const {
    ::crossrender::Bounds b;
    for (const EmitterState& st : states) {
        for (const Particle& p : st.pool) {
            if (!p.alive) continue;
            const f32 s = MaxT(p.size, 0.0f);
            b.Expand(p.position - Vec3(s));
            b.Expand(p.position + Vec3(s));
        }
    }
    *out = b;
}

// ---------------------------------------------------------------------------
// GPU-ресурсы
// ---------------------------------------------------------------------------
bool ParticleSystem::Impl::EnsureGPU() {
    if (gpuReady) return true;
    if (gpuTried) return false;
    gpuTried = true;
    if (!gl::glGenBuffers || !gl::glGenVertexArrays || !gl::glCreateShader) return false;

    // Единичный квад в плоскости XY (углы +/-0.5, нормаль +Z).
    // NOTE: вершинный шейдер частиц читает UV из location 1 атрибута,
    // который VAO меша подключает к Vertex::normal, поэтому UV хранятся там
    // (шейдер никогда не читает aNormal).
    MeshData md;
    md.name = "particle_quad";
    md.vertices.resize(4);
    const f32 cu[4] = {0.0f, 1.0f, 1.0f, 0.0f};
    const f32 cv[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    const f32 cp[4][2] = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}};
    for (int i = 0; i < 4; ++i) {
        Vertex& v = md.vertices[static_cast<usize>(i)];
        v.position = {cp[i][0], cp[i][1], 0.0f};
        v.normal = {0.0f, 0.0f, 1.0f};
        v.uv = {cu[i], cv[i]};
        v.tangent = {1, 0, 0, 1};
        v.color = {1, 1, 1, 1};
        v.uv2 = {0, 0};
        md.bounds.Expand(v.position);
    }
    md.indices = {0, 1, 2, 0, 2, 3};
    md.subMeshes.push_back({0, 4, 0, "quad"});
    quadReady = quad.Create(md);
    if (!quadReady) ENG_LOGW("fx", "particle system: unit quad mesh creation failed");

    shaderReady = shader.Build(builtin::kParticleVert, builtin::kParticleFrag, "particles");
    if (!shaderReady) ENG_LOGE("fx", "particle system: shader build failed: %s", shader.Log().c_str());

    // Белый запасной 1x1 для частиц в режиме Mesh без текстуры (их UV не имеют
    // смысла, поэтому процедурную круглую маску нужно пропустить).
    const Color white(1, 1, 1, 1);
    if (!fallbackTexture.CreateSolid(white))
        ENG_LOGW("fx", "particle system: fallback texture creation failed");

    gl::glGenBuffers(1, &instanceVbo);
    gpuReady = instanceVbo != 0 && quadReady && shaderReady;
    if (!gpuReady) ENG_LOGW("fx", "particle system: GPU resources unavailable, rendering disabled");
    return gpuReady;
}

void ParticleSystem::Impl::DestroyGPU() {
    if (instanceVbo != 0 && gl::glDeleteBuffers) gl::glDeleteBuffers(1, &instanceVbo);
    instanceVbo = 0;
    quad.Destroy();
    fallbackTexture.Destroy();
    shader.Destroy();
    gpuReady = false;
    gpuTried = false;
    quadReady = false;
    shaderReady = false;
}

// ---------------------------------------------------------------------------
// Сборка инстансов
// ---------------------------------------------------------------------------
int ParticleSystem::Impl::BuildInstances(int ei, const ParticleEmitterDesc& d, const Vec3& camPos,
                                         const Vec3& camFwd, const Mat4& xf) {
    if (ei < 0 || ei >= static_cast<int>(states.size())) return 0;
    EmitterState& st = states[static_cast<usize>(ei)];
    const usize cap = st.pool.size();
    // `st.trailMode` отражает фактически выделенные буферы.
    const bool trail = st.trailMode;
    const bool local = d.space == SimulationSpace::Local;

    instances.clear();
    depths.clear();
    instances.reserve(trail ? cap * (kTrailSamples - 1) : cap);
    depths.reserve(trail ? cap * (kTrailSamples - 1) : cap);

    for (usize i = 0; i < cap; ++i) {
        const Particle& p = st.pool[i];
        if (!p.alive) continue;
        const f32 t = p.NormalizedAge();

        Color c = p.color;
        if (d.softParticles) c.a *= SoftFadeFactor(d, p, camPos);
        if (c.a <= 0.0005f || p.size <= 0.0f) continue;

        const f32 rot =
            p.rotation + (d.rotationOverLifetime.keys.empty() ? 0.0f : d.rotationOverLifetime.Evaluate(t));

        if (trail) {
            const u32 n = st.trailCount[i];
            if (n < 2) continue;
            const Vec3* base = &st.trail[i * kTrailSamples];
            for (u32 k = n - 1; k >= 1; --k) {
                // base[0] — новейший; идём от старейшей пробы к новейшей.
                const Vec3 a = local ? xf.TransformPoint(base[k]) : base[k];
                const Vec3 b = local ? xf.TransformPoint(base[k - 1]) : base[k - 1];
                const f32 segT = static_cast<f32>(n - 1 - k) / static_cast<f32>(n - 1);
                const f32 w = p.size * Lerp(0.2f, 1.0f, segT);
                Color sc = c;
                sc.a *= Lerp(0.2f, 1.0f, segT);
                ParticleInstance inst;
                inst.posSize = Vec4((a + b) * 0.5f, w);
                inst.color = sc.ToVec4();
                inst.rotStretch = Vec4(rot, 0.0f, 0.0f, 0.0f);
                inst.velLife = Vec4(b - a, t);
                instances.push_back(inst);
                depths.push_back(Dot((a + b) * 0.5f - camPos, camFwd));
            }
            continue;
        }

        const Vec3 pos = local ? xf.TransformPoint(p.position) : p.position;
        const Vec3 vel = local ? xf.TransformDir(p.velocity) : p.velocity;
        ParticleInstance inst;
        inst.posSize = Vec4(pos, p.size);
        inst.color = c.ToVec4();
        inst.rotStretch = Vec4(rot, d.stretchScale, 0.0f, 0.0f);
        inst.velLife = Vec4(vel, t);
        instances.push_back(inst);
        depths.push_back(Dot(pos - camPos, camFwd));
    }

    const int count = static_cast<int>(instances.size());
    if (count == 0) return 0;

    order.resize(static_cast<usize>(count));
    for (int k = 0; k < count; ++k) order[static_cast<usize>(k)] = static_cast<u32>(k);
    if (d.sortByDepth) {
        // От дальних к ближним (сначала наибольшая глубина вида); векторы переиспользуем, без выделений.
        std::sort(order.begin(), order.end(),
                  [this](u32 a, u32 b) { return depths[a] > depths[b]; });
    }
    sorted.resize(static_cast<usize>(count));
    for (int k = 0; k < count; ++k) sorted[static_cast<usize>(k)] = instances[order[static_cast<usize>(k)]];
    return count;
}

// ---------------------------------------------------------------------------
// Отрисовка
// ---------------------------------------------------------------------------
void ParticleSystem::Impl::DrawEmitter(const ParticleEmitterDesc& d, const FrameUniforms& u,
                                       int count) {
    Shader& sh = shader;
    sh.Bind();
    sh.Set("uViewProj", u.viewProj);
    sh.Set("uModel", Mat4::Identity());  // локальное пространство запекается на CPU
    sh.Set("uCameraRight", u.camRight);
    sh.Set("uCameraUp", u.camUp);
    sh.Set("uCameraForward", u.camForward);
    sh.Set("uRenderMode", RenderModeIndex(d.renderMode));
    sh.Set("uStretchScale", d.stretchScale);
    sh.Set("uSoftFade", 1.0f);  // встроенный шейдер не использует; фейд на CPU
    const bool fog = d.receiveFog && u.fogEnabled;
    sh.Set("uFogEnabled", fog ? 1 : 0);
    sh.Set("uFogColor", u.fogColor);
    sh.Set("uFogDensity", u.fogDensity);

    bool useTexture = d.texture != nullptr && d.texture->Valid();
    const Texture* tex = useTexture ? d.texture : nullptr;
    if (!useTexture && d.renderMode == ParticleRenderMode::Mesh && fallbackTexture.Valid()) {
        // Геометрия Mesh не имеет осмысленных UV для процедурной маски.
        tex = &fallbackTexture;
        useTexture = true;
    }
    sh.Set("uHasTexture", useTexture ? 1 : 0);
    if (tex) sh.SetTexture("uTexture", *tex, 0);

    ApplyBlendState(EngineBlendMode(d.blendMode));
    if (gl::glDepthMask)
        gl::glDepthMask((d.lit || d.castShadows) ? gl::GL_TRUE : gl::GL_FALSE);

    const Mesh* mesh = &quad;
    if (d.renderMode == ParticleRenderMode::Mesh && d.mesh != nullptr && d.mesh->Valid()) mesh = d.mesh;

    if (gl::glBindBuffer)
        gl::glBindBuffer(gl::GL_ARRAY_BUFFER, instanceVbo);
    if (gl::glBufferData)
        gl::glBufferData(gl::GL_ARRAY_BUFFER,
                         static_cast<gl::GLsizeiptr>(count * static_cast<int>(sizeof(ParticleInstance))),
                         sorted.data(), gl::GL_STREAM_DRAW);

    mesh->Bind();
    if (gl::glEnableVertexAttribArray && gl::glVertexAttribPointer) {
        const gl::GLsizei stride = static_cast<gl::GLsizei>(sizeof(ParticleInstance));
        const usize offsets[4] = {offsetof(ParticleInstance, posSize), offsetof(ParticleInstance, color),
                                  offsetof(ParticleInstance, rotStretch),
                                  offsetof(ParticleInstance, velLife)};
        for (int i = 0; i < 4; ++i) {
            const gl::GLuint loc = kInstanceAttribLocations[i];
            gl::glEnableVertexAttribArray(loc);
            gl::glVertexAttribPointer(loc, 4, gl::GL_FLOAT, gl::GL_FALSE, stride,
                                      reinterpret_cast<const void*>(offsets[i]));
            if (gl::glVertexAttribDivisor) gl::glVertexAttribDivisor(loc, 1);
        }
    }

    // Свои шейдер и раскладка инстансов: рисуем напрямую. Система откладывает
    // этот вызов до конца кадра через Renderer3D::AddPostDraw, чтобы частицы
    // корректно накладывались поверх непрозрачной/прозрачной геометрии.
    mesh->DrawInstanced(count);
    if (gl::glVertexAttribDivisor) {
        for (int i = 0; i < 4; ++i) gl::glVertexAttribDivisor(kInstanceAttribLocations[i], 0);
    }
    if (gl::glBindVertexArray) gl::glBindVertexArray(0);
    (void)0;
}

// ---------------------------------------------------------------------------
// ParticleSystem
// ---------------------------------------------------------------------------
ParticleSystem::ParticleSystem() : impl_(std::make_unique<Impl>()) {}

ParticleSystem::~ParticleSystem() { Shutdown(); }

void ParticleSystem::Init(const std::vector<ParticleEmitterDesc>& emitters, u64 seed) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    impl_->Release();
    emitters_ = emitters;
    impl_->seed = seed;
    impl_->noiseSeed = static_cast<u32>((seed * 0x9E3779B97F4A7C15ull) >> 32) | 1u;
    impl_->rng.Seed(seed);
    impl_->nameIndex.clear();
    for (usize i = 0; i < emitters_.size(); ++i) impl_->nameIndex[emitters_[i].name] = static_cast<int>(i);
    impl_->Allocate(emitters_);
    transform_ = Mat4::Identity();
    bounds_ = ::crossrender::Bounds{};
    playing_ = true;
    ENG_LOGI("fx", "ParticleSystem::Init: %d emitter(s), %d slot(s)", EmitterCount(), Capacity());
}

void ParticleSystem::Shutdown() {
    if (impl_) impl_->Release();
    // Освобождаем и описания, чтобы устаревший индекс эмиттера нельзя было
    // применить к (теперь пустым) пулам; Init() всё заполнит заново.
    emitters_.clear();
    transform_ = Mat4::Identity();
    bounds_ = ::crossrender::Bounds{};
    playing_ = false;
}

void ParticleSystem::Restart() {
    if (!impl_) return;
    for (usize i = 0; i < impl_->states.size() && i < emitters_.size(); ++i) {
        Impl::EmitterState& st = impl_->states[i];
        for (usize k = 0; k < st.pool.size(); ++k) {
            st.pool[k].alive = false;
            st.pool[k].age = 0.0f;
            if (k < st.trailCount.size()) st.trailCount[k] = 0u;
        }
        st.freeList.resize(st.pool.size());
        for (usize k = 0; k < st.pool.size(); ++k)
            st.freeList[st.pool.size() - 1 - k] = static_cast<int>(k);
        st.liveCount = 0;
        st.accumulator = 0.0f;
        st.time = 0.0f;
        st.burstPending = emitters_[i].burst > 0;
        st.emitting = false;
    }
    impl_->rng.Seed(impl_->seed);
    bounds_ = ::crossrender::Bounds{};
    playing_ = true;
}

void ParticleSystem::Clear() {
    if (!impl_) return;
    for (Impl::EmitterState& st : impl_->states) {
        for (usize k = 0; k < st.pool.size(); ++k) {
            st.pool[k].alive = false;
            st.pool[k].age = 0.0f;
            if (k < st.trailCount.size()) st.trailCount[k] = 0u;
        }
        st.freeList.resize(st.pool.size());
        for (usize k = 0; k < st.pool.size(); ++k)
            st.freeList[st.pool.size() - 1 - k] = static_cast<int>(k);
        st.liveCount = 0;
        st.accumulator = 0.0f;
    }
    bounds_ = ::crossrender::Bounds{};
}

void ParticleSystem::EmitBurst(int emitterIndex, int count) {
    if (!impl_ || count <= 0) return;
    if (emitterIndex < 0 || emitterIndex >= static_cast<int>(emitters_.size())) return;
    // Публичные всплески не каскадируются в суб-эмиттеры рождения, поэтому
    // число созданных частиц ровно `count`.
    for (int i = 0; i < count; ++i)
        if (impl_->Spawn(emitterIndex, emitters_, transform_, 0, nullptr, false) < 0) break;
    impl_->UpdateBounds(&bounds_);
    playing_ = true;
}

void ParticleSystem::Simulate(f32 dt) {
    if (!impl_ || emitters_.empty()) return;
    if (!std::isfinite(dt) || !(dt > 0.0f)) return;
    if (dt > 4.0f) {
        ENG_LOGW("fx", "ParticleSystem::Simulate: clamping dt %.3f to 4.0", dt);
        dt = 4.0f;
    }

    impl_->RefreshCache(emitters_);

    int steps = static_cast<int>(std::ceil(dt / Impl::kMaxSubstep));
    steps = Clamp(steps, 1, 480);
    const f32 h = dt / static_cast<f32>(steps);
    const int emitterCount = static_cast<int>(emitters_.size());

    for (int s = 0; s < steps; ++s) {
        for (int e = 0; e < emitterCount; ++e)
            impl_->Step(e, emitters_, transform_, playing_, h);
        if (playing_) {
            int alive = 0;
            bool emitting = false;
            for (const Impl::EmitterState& st : impl_->states) {
                alive += st.liveCount;
                if (st.emitting) emitting = true;
            }
            // Завершённый непетлевой эмиттер сообщает playing == false, когда
            // погибли все частицы.
            if (!emitting && alive == 0) playing_ = false;
        }
    }

    impl_->PushTrailSamples();
    impl_->UpdateBounds(&bounds_);
}

void ParticleSystem::Render(Renderer3D& renderer, const Camera& camera) {
    if (!impl_ || emitters_.empty()) return;
    if (impl_->states.size() != emitters_.size()) return;  // после Shutdown() / без Init()
    if (!impl_->EnsureGPU()) return;  // нет GL-контекста / сбой шейдера: рисовать нечего

    Impl::FrameUniforms u;
    u.viewProj = renderer.ViewProj();
    // Рендерер владеет view-projection, поэтому его камера авторитетна для
    // базиса billboard; камера от вызывающего — запас для вырожденной
    // (не заданной) камеры рендерера.
    const Camera& rcam = renderer.GetCamera();
    Vec3 camForward = Normalize(rcam.Forward());
    if (LengthSq(camForward) < 0.5f) {
        u.camPos = camera.position;
        u.camRight = Normalize(camera.Right());
        u.camUp = Normalize(camera.Up());
        u.camForward = Normalize(camera.Forward());
    } else {
        u.camPos = rcam.position;
        u.camRight = Normalize(rcam.Right());
        u.camUp = Normalize(rcam.Up());
        u.camForward = camForward;
    }

    u.lightDir = Normalize(Vec3(0.45f, 1.0f, 0.35f));
    u.lightColor = Vec3(1, 1, 1) * 0.8f;
    for (int i = 0; i < renderer.LightCount(); ++i) {
        Light* l = renderer.GetLight(i);
        if (l != nullptr && l->type == LightType::Directional) {
            u.lightDir = Normalize(-l->direction);
            u.lightColor = l->color.rgb() * l->intensity;
            break;
        }
    }
    const Environment& env = renderer.GetEnvironment();
    u.ambient = env.ambientSky.rgb() * env.ambientIntensity;
    u.fogEnabled = env.fogEnabled;
    u.fogColor = env.fogColor;
    // Встроенный шейдер ослабляет туман по нормированному возрасту; отображаем
    // линейный диапазон тумана окружения в плотность на возраст.
    if (env.fogEnabled) {
        u.fogDensity = env.fogMode == 0
                           ? Clamp(1.0f / MaxT(env.fogEnd - env.fogStart, 1e-3f), 0.0f, 4.0f)
                           : env.fogDensity;
    }

    // Откладываем реальные отрисовки до конца кадра рендерера: Renderer3D
    // батчит геометрию между BeginFrame и EndFrame, поэтому немедленная
    // отрисовка здесь была бы перекрыта непрозрачной геометрией, отправленной позже.
    renderer.AddPostDraw([this, u]() {
        Impl& im = *impl_;
        const bool blendWasOn = gl::glIsEnabled && gl::glIsEnabled(gl::GL_BLEND) != 0;
        const bool cullWasOn = gl::glIsEnabled && gl::glIsEnabled(gl::GL_CULL_FACE) != 0;
        const bool depthWasOn = gl::glIsEnabled && gl::glIsEnabled(gl::GL_DEPTH_TEST) != 0;
        if (cullWasOn && gl::glDisable) gl::glDisable(gl::GL_CULL_FACE);
        if (!depthWasOn && gl::glEnable) gl::glEnable(gl::GL_DEPTH_TEST);

        for (int e = 0; e < static_cast<int>(emitters_.size()); ++e) {
            const ParticleEmitterDesc& d = emitters_[static_cast<usize>(e)];
            const int count = im.BuildInstances(e, d, u.camPos, u.camForward, transform_);
            if (count <= 0) continue;
            im.DrawEmitter(d, u, count);
        }

        if (gl::glDepthMask) gl::glDepthMask(gl::GL_TRUE);
        if (!blendWasOn && gl::glDisable) gl::glDisable(gl::GL_BLEND);
        if (cullWasOn && gl::glEnable) gl::glEnable(gl::GL_CULL_FACE);
        if (!depthWasOn && gl::glDisable) gl::glDisable(gl::GL_DEPTH_TEST);
        if (gl::glDepthFunc) gl::glDepthFunc(gl::GL_LESS);
        if (gl::glBlendFunc) gl::glBlendFunc(gl::GL_ONE, gl::GL_ONE_MINUS_SRC_ALPHA);
    });
}

int ParticleSystem::AliveCount() const {
    if (!impl_) return 0;
    int n = 0;
    for (const Impl::EmitterState& st : impl_->states) n += st.liveCount;
    return n;
}

int ParticleSystem::Capacity() const {
    if (!impl_) return 0;
    int n = 0;
    for (const Impl::EmitterState& st : impl_->states) n += static_cast<int>(st.pool.size());
    return n;
}

const Particle* ParticleSystem::Particles(int emitterIndex) const {
    if (!impl_ || emitterIndex < 0 || emitterIndex >= static_cast<int>(impl_->states.size()))
        return nullptr;
    const std::vector<Particle>& pool = impl_->states[static_cast<usize>(emitterIndex)].pool;
    return pool.empty() ? nullptr : pool.data();
}

// ---------------------------------------------------------------------------
// Пресеты
// ---------------------------------------------------------------------------
std::vector<ParticleEmitterDesc> ParticleSystem::PresetFire() {
    ParticleEmitterDesc d;
    d.name = "Fire";
    d.rate = 140.0f;
    d.burst = 30;
    d.duration = 0.0f;
    d.looping = true;
    d.maxParticles = 900;
    d.lifetime = {0.9f, 1.7f};
    d.startSpeed = {0.5f, 1.4f};
    d.startSize = {0.22f, 0.5f};
    d.startRotation = {0.0f, kTau};
    d.startAngularVelocity = {-1.5f, 1.5f};
    d.shape = EmitterShape::Cone;
    d.coneAngle = 14.0f * kDeg2Rad;
    d.shapeRadius = {0.0f, 0.25f};
    d.gravity = {0.0f, 1.2f, 0.0f};  // плавучесть
    d.wind = {0.15f, 0.0f, 0.0f};
    d.drag = 1.4f;
    d.turbulenceStrength = 0.5f;
    d.turbulenceFrequency = 2.2f;
    d.sizeOverLifetime =
        Curve::FromPoints({{0.0f, 0.45f}, {0.25f, 1.0f}, {0.7f, 0.75f}, {1.0f, 0.05f}}, 0.6f);
    d.alphaOverLifetime =
        Curve::FromPoints({{0.0f, 0.0f}, {0.12f, 0.9f}, {0.6f, 0.55f}, {1.0f, 0.0f}}, 0.4f);
    d.colorOverLifetime =
        ColorCurve::Gradient({{0.0f, Color::FromRGB(0xFFF3B0)},
                              {0.35f, Color::FromRGB(0xFFA02A)},
                              {0.75f, Color::FromRGB(0xE0431A)},
                              {1.0f, Color::FromRGB(0x40100A)}});
    d.renderMode = ParticleRenderMode::Billboard;
    d.blendMode = ParticleBlendMode::Additive;
    d.softParticles = true;
    d.softFadeDistance = 0.35f;
    d.sortByDepth = true;
    d.receiveFog = true;
    return {d};
}

std::vector<ParticleEmitterDesc> ParticleSystem::PresetSmoke() {
    ParticleEmitterDesc d;
    d.name = "Smoke";
    d.rate = 35.0f;
    d.burst = 5;
    d.duration = 0.0f;
    d.looping = true;
    d.maxParticles = 700;
    d.lifetime = {2.5f, 4.5f};
    d.startSpeed = {0.25f, 0.7f};
    d.startSize = {0.5f, 1.1f};
    d.startRotation = {0.0f, kTau};
    d.startAngularVelocity = {-0.6f, 0.6f};
    d.shape = EmitterShape::Sphere;
    d.shapeRadius = {0.15f, 0.5f};
    d.gravity = {0.0f, 0.25f, 0.0f};
    d.wind = {0.55f, 0.05f, 0.15f};
    d.drag = 0.55f;
    d.turbulenceStrength = 0.1f;
    d.turbulenceFrequency = 0.7f;
    d.sizeOverLifetime =
        Curve::FromPoints({{0.0f, 0.4f}, {0.3f, 1.0f}, {0.7f, 1.5f}, {1.0f, 1.9f}}, 0.5f);
    d.alphaOverLifetime =
        Curve::FromPoints({{0.0f, 0.0f}, {0.15f, 0.5f}, {0.6f, 0.35f}, {1.0f, 0.0f}}, 0.5f);
    d.colorOverLifetime =
        ColorCurve::Gradient({{0.0f, Color::FromRGB(0x9AA0A6)},
                              {0.5f, Color::FromRGB(0x555A60)},
                              {1.0f, Color::FromRGB(0x2B2E33)}});
    d.renderMode = ParticleRenderMode::Billboard;
    d.blendMode = ParticleBlendMode::Alpha;
    d.softParticles = true;
    d.softFadeDistance = 1.0f;
    d.sortByDepth = true;
    d.receiveFog = true;
    return {d};
}

std::vector<ParticleEmitterDesc> ParticleSystem::PresetSparks() {
    ParticleEmitterDesc d;
    d.name = "Sparks";
    d.rate = 320.0f;
    d.burst = 50;
    d.duration = 0.0f;
    d.looping = true;
    d.maxParticles = 1500;
    d.lifetime = {0.5f, 1.3f};
    d.startSpeed = {4.0f, 9.0f};
    d.startSize = {0.03f, 0.075f};
    d.startRotation = {0.0f, kTau};
    d.startAngularVelocity = {-6.0f, 6.0f};
    d.shape = EmitterShape::Circle;
    d.shapeRadius = {0.0f, 0.12f};
    d.gravity = {0.0f, -14.0f, 0.0f};
    d.drag = 0.2f;
    d.turbulenceStrength = 0.15f;
    d.turbulenceFrequency = 3.0f;
    d.collideGround = true;
    d.groundY = 0.0f;
    d.bounce = 0.45f;
    d.friction = 0.6f;
    d.sizeOverLifetime = Curve::FromPoints({{0.0f, 1.0f}, {1.0f, 0.25f}});
    d.alphaOverLifetime = Curve::FromPoints({{0.0f, 1.0f}, {0.15f, 1.0f}, {1.0f, 0.0f}});
    d.colorOverLifetime =
        ColorCurve::Gradient({{0.0f, Color::FromRGB(0xFFFFFF)},
                              {0.3f, Color::FromRGB(0xFFD070)},
                              {0.7f, Color::FromRGB(0xFF6A20)},
                              {1.0f, Color::FromRGB(0x502008)}});
    d.renderMode = ParticleRenderMode::StretchedBillboard;
    d.alignToVelocity = true;
    d.stretchScale = 0.06f;
    d.blendMode = ParticleBlendMode::Additive;
    d.sortByDepth = true;
    return {d};
}

std::vector<ParticleEmitterDesc> ParticleSystem::PresetMagic() {
    ParticleEmitterDesc d;
    d.name = "Magic";
    d.rate = 180.0f;
    d.burst = 20;
    d.duration = 0.0f;
    d.looping = true;
    d.maxParticles = 1200;
    d.lifetime = {1.4f, 2.4f};
    d.startSpeed = {0.6f, 1.6f};
    d.startSize = {0.06f, 0.16f};
    d.startRotation = {0.0f, kTau};
    d.startAngularVelocity = {-4.0f, 4.0f};
    d.shape = EmitterShape::Sphere;
    d.shapeRadius = {0.35f, 1.0f};
    d.gravity = {0.0f, 0.4f, 0.0f};
    d.drag = 0.35f;
    d.turbulenceStrength = 0.25f;
    d.turbulenceFrequency = 1.4f;
    d.attractors.push_back({{0.0f, 1.1f, 0.0f}, 3.0f, 3.5f});
    d.vortexAxis = {0.0f, 1.0f, 0.0f};
    d.vortexStrength = 3.2f;
    d.sizeOverLifetime =
        Curve::FromPoints({{0.0f, 1.0f}, {0.35f, 0.6f}, {0.75f, 1.1f}, {1.0f, 0.2f}}, 0.7f);
    d.alphaOverLifetime =
        Curve::FromPoints({{0.0f, 0.0f}, {0.15f, 0.9f}, {0.7f, 0.7f}, {1.0f, 0.0f}}, 0.5f);
    d.colorOverLifetime =
        ColorCurve::Gradient({{0.0f, Color::FromRGB(0xFF5AD0)},
                              {0.3f, Color::FromRGB(0x7A5CFF)},
                              {0.65f, Color::FromRGB(0x38E8FF)},
                              {1.0f, Color::FromRGB(0xB44CFF)}});
    d.renderMode = ParticleRenderMode::Billboard;
    d.blendMode = ParticleBlendMode::Additive;
    d.softParticles = true;
    d.softFadeDistance = 0.3f;
    d.sortByDepth = true;
    return {d};
}

std::vector<ParticleEmitterDesc> ParticleSystem::PresetFountain() {
    ParticleEmitterDesc d;
    d.name = "Fountain";
    d.rate = 420.0f;
    d.burst = 60;
    d.duration = 0.0f;
    d.looping = true;
    d.maxParticles = 2200;
    d.lifetime = {1.5f, 2.3f};
    d.startSpeed = {5.0f, 7.5f};
    d.startSize = {0.08f, 0.16f};
    d.startRotation = {0.0f, kTau};
    d.startAngularVelocity = {-2.0f, 2.0f};
    d.shape = EmitterShape::Cone;
    d.coneAngle = 20.0f * kDeg2Rad;
    d.shapeRadius = {0.0f, 0.15f};
    d.gravity = {0.0f, -9.81f, 0.0f};
    d.drag = 0.06f;
    d.collideGround = true;
    d.groundY = 0.0f;
    d.bounce = 0.22f;
    d.friction = 0.75f;
    d.sizeOverLifetime = Curve::FromPoints({{0.0f, 1.0f}, {1.0f, 0.5f}});
    d.alphaOverLifetime =
        Curve::FromPoints({{0.0f, 0.0f}, {0.1f, 1.0f}, {0.8f, 0.85f}, {1.0f, 0.1f}});
    d.colorOverLifetime =
        ColorCurve::Gradient({{0.0f, Color::FromRGB(0xEAF6FF)},
                              {0.5f, Color::FromRGB(0x7FC8FF)},
                              {1.0f, Color::FromRGB(0x2A6FBF)}});
    d.renderMode = ParticleRenderMode::Billboard;
    d.blendMode = ParticleBlendMode::Alpha;
    d.softParticles = true;
    d.softFadeDistance = 0.5f;
    d.sortByDepth = true;
    d.receiveFog = true;
    return {d};
}

std::vector<ParticleEmitterDesc> ParticleSystem::PresetExplosion() {
    ParticleEmitterDesc e;
    e.name = "Explosion";
    e.rate = 0.0f;
    e.burst = 300;
    e.duration = 0.12f;
    e.looping = false;
    e.maxParticles = 600;
    e.lifetime = {0.45f, 1.0f};
    e.startSpeed = {6.0f, 15.0f};
    e.startSize = {0.12f, 0.3f};
    e.startRotation = {0.0f, kTau};
    e.startAngularVelocity = {-8.0f, 8.0f};
    e.shape = EmitterShape::Sphere;
    e.shapeRadius = {0.0f, 0.35f};
    e.gravity = {0.0f, -6.0f, 0.0f};
    e.drag = 1.6f;
    e.turbulenceStrength = 0.6f;
    e.turbulenceFrequency = 2.0f;
    e.sizeOverLifetime = Curve::FromPoints({{0.0f, 1.4f}, {0.4f, 0.9f}, {1.0f, 0.15f}}, 0.4f);
    e.alphaOverLifetime =
        Curve::FromPoints({{0.0f, 0.0f}, {0.06f, 1.0f}, {0.5f, 0.8f}, {1.0f, 0.0f}}, 0.4f);
    e.colorOverLifetime =
        ColorCurve::Gradient({{0.0f, Color::FromRGB(0xFFFFFF)},
                              {0.25f, Color::FromRGB(0xFFC24A)},
                              {0.6f, Color::FromRGB(0xE0431A)},
                              {1.0f, Color::FromRGB(0x2A0A06)}});
    e.renderMode = ParticleRenderMode::Billboard;
    e.blendMode = ParticleBlendMode::Additive;
    e.softParticles = true;
    e.softFadeDistance = 0.3f;
    e.sortByDepth = true;
    // Суб-эмиттер смерти: каждый умирающий огненный шар выпускает дым.
    e.subEmitters.push_back({"ExplosionSmoke", 1, 6});

    ParticleEmitterDesc s;
    s.name = "ExplosionSmoke";
    s.rate = 0.0f;       // питается только суб-эмиттером смерти
    s.burst = 0;
    s.duration = 0.0f;
    s.looping = true;
    s.maxParticles = 2000;
    s.lifetime = {1.8f, 3.2f};
    s.startSpeed = {0.4f, 1.2f};
    s.startSize = {0.35f, 0.8f};
    s.startRotation = {0.0f, kTau};
    s.startAngularVelocity = {-1.2f, 1.2f};
    s.shape = EmitterShape::Sphere;
    s.shapeRadius = {0.0f, 0.5f};
    s.gravity = {0.0f, 0.5f, 0.0f};
    s.wind = {0.3f, 0.0f, 0.1f};
    s.drag = 0.8f;
    s.turbulenceStrength = 0.2f;
    s.turbulenceFrequency = 1.1f;
    s.sizeOverLifetime =
        Curve::FromPoints({{0.0f, 0.6f}, {0.5f, 1.6f}, {1.0f, 2.2f}}, 0.5f);
    s.alphaOverLifetime =
        Curve::FromPoints({{0.0f, 0.0f}, {0.15f, 0.6f}, {0.6f, 0.4f}, {1.0f, 0.0f}}, 0.5f);
    s.colorOverLifetime =
        ColorCurve::Gradient({{0.0f, Color::FromRGB(0x6E6E72)},
                              {0.5f, Color::FromRGB(0x44474C)},
                              {1.0f, Color::FromRGB(0x1E2024)}});
    s.renderMode = ParticleRenderMode::Billboard;
    s.blendMode = ParticleBlendMode::Alpha;
    s.softParticles = true;
    s.softFadeDistance = 1.0f;
    s.sortByDepth = true;
    s.receiveFog = true;
    return {e, s};
}

std::vector<ParticleEmitterDesc> ParticleSystem::PresetSnow() {
    ParticleEmitterDesc d;
    d.name = "Snow";
    d.rate = 260.0f;
    d.burst = 120;
    d.duration = 0.0f;
    d.looping = true;
    d.maxParticles = 3500;
    d.lifetime = {4.0f, 7.0f};
    d.startSpeed = {0.1f, 0.45f};
    d.startSize = {0.03f, 0.07f};
    d.startRotation = {0.0f, kTau};
    d.startAngularVelocity = {-0.8f, 0.8f};
    d.shape = EmitterShape::Box;
    d.shapeBox = {9.0f, 4.0f, 9.0f};
    d.gravity = {0.0f, -0.45f, 0.0f};
    d.wind = {0.55f, 0.0f, 0.2f};
    d.drag = 0.3f;
    d.turbulenceStrength = 0.16f;
    d.turbulenceFrequency = 0.55f;
    d.sizeOverLifetime = Curve::Constant(1.0f);
    d.alphaOverLifetime =
        Curve::FromPoints({{0.0f, 0.0f}, {0.1f, 0.9f}, {0.85f, 0.9f}, {1.0f, 0.0f}});
    d.colorOverLifetime =
        ColorCurve::Gradient({{0.0f, Color::FromRGB(0xFFFFFF)}, {1.0f, Color::FromRGB(0xBFD8FF)}});
    d.renderMode = ParticleRenderMode::Billboard;
    d.blendMode = ParticleBlendMode::Alpha;
    d.softParticles = true;
    d.softFadeDistance = 0.25f;
    d.sortByDepth = true;
    d.receiveFog = true;
    return {d};
}

std::vector<ParticleEmitterDesc> ParticleSystem::PresetTrail() {
    ParticleEmitterDesc d;
    d.name = "Trail";
    d.rate = 90.0f;
    d.burst = 0;
    d.duration = 0.0f;
    d.looping = true;
    d.maxParticles = 300;
    d.lifetime = {0.45f, 0.75f};
    d.startSpeed = {2.5f, 4.5f};
    d.startSize = {0.09f, 0.16f};
    d.startRotation = {0.0f, kTau};
    d.startAngularVelocity = {0.0f, 0.0f};
    d.shape = EmitterShape::Point;
    d.gravity = {0.0f, 0.0f, 0.0f};
    d.drag = 0.4f;
    d.sizeOverLifetime = Curve::FromPoints({{0.0f, 1.0f}, {1.0f, 0.2f}});
    d.alphaOverLifetime =
        Curve::FromPoints({{0.0f, 0.0f}, {0.1f, 1.0f}, {0.7f, 0.7f}, {1.0f, 0.0f}});
    d.colorOverLifetime =
        ColorCurve::Gradient({{0.0f, Color::FromRGB(0xFFFFFF)},
                              {0.4f, Color::FromRGB(0x56E8FF)},
                              {1.0f, Color::FromRGB(0x1840A0)}});
    d.renderMode = ParticleRenderMode::Trail;
    d.blendMode = ParticleBlendMode::Additive;
    d.alignToVelocity = true;
    d.stretchScale = 6.0f;  // квадам следа задано растяжение вдоль своего сегмента
    d.sortByDepth = true;
    return {d};
}

}  // namespace crossrender
