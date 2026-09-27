// Реализация Renderer2D.
//
// Замечания по устройству
// -----------------------
// * Вся геометрия путей уплощается и тесселлируется на CPU, затем преобразуется
//   сразу в логическое экранное пространство и пишется в один динамический
//   вершинный буфер. Вершинный шейдер применяет единственную ортопроекцию.
// * Геометрия делится на "вызовы" с ключом (текстура, тип краски, режим
//   смешения, scissor, программа). Подряд идущие примитивы с одним ключом
//   живут в одном draw call, поэтому типичный UI-кадр — несколько вызовов.
// * Заливки используют триангуляцию ear-clipping с мостами для дыр (правило
//   nonzero) плюс однопиксельную аналитическую кайму сглаживания. Обводки
//   разворачиваются в triangle strips со стыками miter/round/bevel и торцами
//   butt/round/square и для краёв полагаются на MSAA (конфигурация по умолчанию).
// * Текст рисуется из атласа шрифта: либо альфа-битовые карты через
//   универсальный спрайтовый шейдер, либо знаковые поля расстояний через
//   отдельный SDF-шейдер. Вершинный расклад общий — они делят батч-буфер.
#include "crossrender/gfx/Renderer2D.h"

#include "crossrender/gfx/GL.h"
#include "crossrender/core/Log.h"
#include "crossrender/core/File.h"
#include "crossrender/text/Font.h"
#include "crossrender/gfx/Shader.h"
#include "crossrender/gfx/RenderTarget.h"
#include "crossrender/platform/Platform.h"

#include <cmath>
#include <algorithm>

namespace crossrender {
namespace {

constexpr f32 kAaWidth = 1.0f;
constexpr usize kMaxVertices = 512 * 1024;

struct R2DVertex {
    Vec2 pos;
    Vec2 uv;
    Color color;
};

struct ScissorRect {
    f32 x = 0, y = 0, w = 0, h = 0;
    bool enabled = false;
    bool operator==(const ScissorRect& o) const {
        return enabled == o.enabled && (!enabled || (x == o.x && y == o.y && w == o.w && h == o.h));
    }
};

enum class Program : u8 { Sprite, SdfText };

struct CallKey {
    unsigned int texture = 0;
    int paintType = 0;
    BlendMode blend = BlendMode::Alpha;
    ScissorRect scissor;
    Program program = Program::Sprite;
    bool operator==(const CallKey& o) const {
        return texture == o.texture && paintType == o.paintType && blend == o.blend &&
               scissor == o.scissor && program == o.program;
    }
};

struct DrawCall {
    CallKey key;
    u32 indexOffset = 0;
    u32 indexCount = 0;
    Paint paint;  // снимок для uniform'ов градиента
    f32 alpha = 1;
    // Параметры SDF-текста (имеют смысл только для Program::SdfText).
    f32 sdfSpread = 4.0f;
    f32 sdfPixelRange = 1.0f;
    f32 sdfOutlineWidth = 0.0f;
    Color sdfOutlineColor{0, 0, 0, 0};
};

struct State {
    Mat4 transform;
    f32 alpha = 1;
    Paint fill = Paint::Solid(Color::White);
    Paint stroke = Paint::Solid(Color::Black);
    f32 strokeWidth = 1;
    f32 miterLimit = 4;
    LineCap cap = LineCap::Butt;
    LineJoin join = LineJoin::Miter;
    bool antialias = true;
    ScissorRect scissor;
    BlendMode blend = BlendMode::Alpha;
};

// Контракт `uType` спрайтового шейдера:
//   0 сплошной цвет, 1 линейный градиент, 2 радиальный градиент, 3 картинка, 4 box-градиент.
// Порядок объявления в `Paint::Type` иной (BoxGradient раньше ImagePattern),
// поэтому отображаем явно, а не кастуем значение перечисления.
int ShaderPaintType(Paint::Type t) {
    switch (t) {
        case Paint::Type::Solid: return 0;
        case Paint::Type::LinearGradient: return 1;
        case Paint::Type::RadialGradient: return 2;
        case Paint::Type::ImagePattern: return 3;
        case Paint::Type::BoxGradient: return 4;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Геометрические помощники
// ---------------------------------------------------------------------------
f32 PolygonArea(const std::vector<Vec2>& p) {
    f32 a = 0;
    for (usize i = 0, n = p.size(); i < n; ++i) {
        const Vec2& p0 = p[i];
        const Vec2& p1 = p[(i + 1) % n];
        a += p0.x * p1.y - p1.x * p0.y;
    }
    return a * 0.5f;
}

bool PointInPolygon(const std::vector<Vec2>& poly, const Vec2& pt) {
    bool inside = false;
    for (usize i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const Vec2& a = poly[i];
        const Vec2& b = poly[j];
        if (((a.y > pt.y) != (b.y > pt.y)) &&
            (pt.x < (b.x - a.x) * (pt.y - a.y) / (b.y - a.y + 1e-20f) + a.x))
            inside = !inside;
    }
    return inside;
}

f32 TriangleArea2(const Vec2& a, const Vec2& b, const Vec2& c) {
    return (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
}

bool PointInTriangle(const Vec2& p, const Vec2& a, const Vec2& b, const Vec2& c) {
    f32 d1 = TriangleArea2(p, a, b);
    f32 d2 = TriangleArea2(p, b, c);
    f32 d3 = TriangleArea2(p, c, a);
    bool neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
    bool pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
    return !(neg && pos);
}

bool SegmentsIntersect(const Vec2& p1, const Vec2& p2, const Vec2& p3, const Vec2& p4) {
    auto ccw = [](const Vec2& a, const Vec2& b, const Vec2& c) {
        return (c.y - a.y) * (b.x - a.x) > (b.y - a.y) * (c.x - a.x);
    };
    return ccw(p1, p3, p4) != ccw(p2, p3, p4) && ccw(p1, p2, p3) != ccw(p1, p2, p4);
}

// Триангуляция ear-clipping простого (возможно, с мостами) полигона.
bool EarClip(const std::vector<Vec2>& poly, std::vector<u32>* out) {
    usize n = poly.size();
    if (n < 3) return false;
    std::vector<u32> idx(n);
    for (usize i = 0; i < n; ++i) idx[i] = static_cast<u32>(i);

    // Порядок против часовой в экранных координатах (y вниз => CCW = отрицательная площадь).
    if (PolygonArea(poly) > 0) std::reverse(idx.begin(), idx.end());

    int guard = 0;
    while (idx.size() > 3 && guard++ < 10000) {
        bool clipped = false;
        usize count = idx.size();
        for (usize i = 0; i < count; ++i) {
            u32 i0 = idx[(i + count - 1) % count];
            u32 i1 = idx[i];
            u32 i2 = idx[(i + 1) % count];
            const Vec2& a = poly[i0];
            const Vec2& b = poly[i1];
            const Vec2& c = poly[i2];
            if (TriangleArea2(a, b, c) >= -1e-9f) continue;  // рефлексный или вырожденный
            bool contains = false;
            for (usize j = 0; j < count; ++j) {
                u32 k = idx[j];
                if (k == i0 || k == i1 || k == i2) continue;
                if (PointInTriangle(poly[k], a, b, c)) {
                    contains = true;
                    break;
                }
            }
            if (contains) continue;
            out->push_back(i0);
            out->push_back(i1);
            out->push_back(i2);
            idx.erase(idx.begin() + static_cast<long>(i));
            clipped = true;
            break;
        }
        if (!clipped) {
            // Вырожденный/самопересекающийся: остаток достраиваем веером, чтобы что-то нарисовалось.
            break;
        }
    }
    for (usize i = 1; i + 1 < idx.size(); ++i) {
        out->push_back(idx[0]);
        out->push_back(idx[i]);
        out->push_back(idx[i + 1]);
    }
    return true;
}

// Встраивает `hole` в `outer` через мост между взаимно видимыми вершинами.
void BridgeHole(std::vector<Vec2>* outer, const std::vector<Vec2>& hole) {
    if (hole.empty() || outer->empty()) return;
    // Самая правая вершина дыры.
    usize holeIdx = 0;
    for (usize i = 1; i < hole.size(); ++i)
        if (hole[i].x > hole[holeIdx].x) holeIdx = i;

    // Кандидаты среди внешних вершин, упорядоченные по расстоянию.
    std::vector<std::pair<f32, usize>> candidates;
    candidates.reserve(outer->size());
    for (usize i = 0; i < outer->size(); ++i) {
        f32 d = LengthSq((*outer)[i] - hole[holeIdx]);
        candidates.emplace_back(d, i);
    }
    std::sort(candidates.begin(), candidates.end());

    usize best = candidates[0].second;
    for (const auto& [dist, oi] : candidates) {
        (void)dist;
        bool ok = true;
        Vec2 a = hole[holeIdx];
        Vec2 b = (*outer)[oi];
        for (usize i = 0; i < outer->size() && ok; ++i) {
            usize j = (i + 1) % outer->size();
            if (i == oi || j == oi) continue;
            if (SegmentsIntersect(a, b, (*outer)[i], (*outer)[j])) ok = false;
        }
        for (usize i = 0; i < hole.size() && ok; ++i) {
            usize j = (i + 1) % hole.size();
            if (i == holeIdx || j == holeIdx) continue;
            Vec2 ha = hole[i], hb = hole[j];
            if (SegmentsIntersect(a, b, ha, hb)) ok = false;
        }
        if (ok) {
            best = oi;
            break;
        }
    }

    std::vector<Vec2> merged;
    merged.reserve(outer->size() + hole.size() + 2);
    for (usize i = 0; i <= best; ++i) merged.push_back((*outer)[i]);
    for (usize i = 0; i < hole.size(); ++i) merged.push_back(hole[(holeIdx + i) % hole.size()]);
    merged.push_back(hole[holeIdx]);
    for (usize i = best; i < outer->size(); ++i) merged.push_back((*outer)[i]);
    *outer = std::move(merged);
}

// Адаптивно уплощаем кубическую кривую Безье.
void FlattenCubic(std::vector<Vec2>* out, Vec2 p0, Vec2 c1, Vec2 c2, Vec2 p1, int depth = 0) {
    if (depth > 12) {
        out->push_back(p1);
        return;
    }
    // Тест плоскостности: удаление контрольных точек от хорды.
    Vec2 d = p1 - p0;
    f32 d1 = std::fabs((c1.x - p1.x) * d.y - (c1.y - p1.y) * d.x);
    f32 d2 = std::fabs((c2.x - p1.x) * d.y - (c2.y - p1.y) * d.x);
    f32 dd = d1 + d2;
    if (dd * dd <= 0.0625f * (d.x * d.x + d.y * d.y)) {
        out->push_back(p1);
        return;
    }
    Vec2 p01 = (p0 + c1) * 0.5f;
    Vec2 p12 = (c1 + c2) * 0.5f;
    Vec2 p23 = (c2 + p1) * 0.5f;
    Vec2 p012 = (p01 + p12) * 0.5f;
    Vec2 p123 = (p12 + p23) * 0.5f;
    Vec2 mid = (p012 + p123) * 0.5f;
    FlattenCubic(out, p0, p01, p012, mid, depth + 1);
    FlattenCubic(out, mid, p123, p23, p1, depth + 1);
}

void FlattenQuad(std::vector<Vec2>* out, Vec2 p0, Vec2 c, Vec2 p1) {
    Vec2 c1 = p0 + (c - p0) * (2.0f / 3.0f);
    Vec2 c2 = p1 + (c - p1) * (2.0f / 3.0f);
    FlattenCubic(out, p0, c1, c2, p1);
}

}  // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------
struct Renderer2D::Impl {
    Shader sprite;
    Shader sdf;
    unsigned int vao = 0, vbo = 0, ibo = 0;
    usize vboCapacity = 0, iboCapacity = 0;

    std::vector<R2DVertex> vertices;
    std::vector<u32> indices;
    std::vector<DrawCall> calls;

    std::vector<State> stack;
    State state;

    // Текущий путь
    std::vector<std::vector<Vec2>> contours;
    std::vector<Vec2> current;
    Vec2 cursor;
    Vec2 pathStart;
    Winding winding = Winding::CCW;

    crossrender::Rect screen{0, 0, 1, 1};
    f32 dpi = 1;
    RenderTarget* target = nullptr;
    bool inFrame = false;
    bool ready = false;
    bool initialised = false;
    bool clipStackPushed = false;
    std::vector<ScissorRect> scissors;
    const Texture* viewportBlitTex = nullptr;
    crossrender::Rect viewportBlitDst;
    // Белая текстура 1x1 для draw без текстуры: сэмплирование неполной/нулевой
    // текстуры заставляет часть драйверов подставлять белый и предупреждать.
    unsigned int whiteTexture = 0;

    // ---------------------------------------------------------------
    void ResetPath() {
        contours.clear();
        current.clear();
    }

    void FinishContour(bool close) {
        if (current.size() >= 2) {
            if (close && LengthSq(current.front() - current.back()) > 1e-8f)
                current.push_back(current.front());
            contours.push_back(current);
        }
        current.clear();
    }

    Vec2 Xf(const Vec2& p) const { return state.transform.TransformPoint(Vec3{p.x, p.y, 0}).xy(); }

    f32 TransformScale() const {
        Vec3 a = state.transform.TransformDir(Vec3{1, 0, 0});
        Vec3 b = state.transform.TransformDir(Vec3{0, 1, 0});
        return (Length(a) + Length(b)) * 0.5f;
    }

    void Reserve(usize vertexCount, usize indexCount) {
        vertices.reserve(vertices.size() + vertexCount);
        indices.reserve(indices.size() + indexCount);
    }

    void PushVertex(const Vec2& pos, const Vec2& uv, const Color& c) {
        if (vertices.size() >= kMaxVertices) return;
        vertices.push_back({pos, uv, c});
    }

    void PushTriangle(u32 a, u32 b, u32 c) {
        indices.push_back(a);
        indices.push_back(b);
        indices.push_back(c);
    }

    u32 VertexBase() const { return static_cast<u32>(vertices.size()); }

    void EnsureCall(const CallKey& key, const Paint& paint) {
        if (!calls.empty() && calls.back().key == key) return;
        DrawCall call;
        call.key = key;
        call.indexOffset = static_cast<u32>(indices.size());
        call.indexCount = 0;
        call.paint = paint;
        call.alpha = state.alpha;
        calls.push_back(call);
    }

    void EndPrimitive() {
        if (!calls.empty()) {
            calls.back().indexCount =
                static_cast<u32>(indices.size()) - calls.back().indexOffset;
            if (calls.back().indexCount == 0) calls.pop_back();
        }
    }

    CallKey KeyFor(const Paint& paint, Program prog) const {
        CallKey k;
        k.paintType = ShaderPaintType(paint.type);
        k.blend = state.blend;
        k.scissor = state.scissor;
        k.program = prog;
        switch (paint.type) {
            case Paint::Type::Solid:
                k.texture = 0;
                break;
            case Paint::Type::ImagePattern:
                k.texture = paint.image ? paint.image->Id() : 0;
                break;
            default:
                k.texture = 0;
                break;
        }
        return k;
    }

    // Применяет текущую альфу к цвету.
    Color WithAlpha(const Color& c) const { return {c.r, c.g, c.b, c.a * state.alpha}; }

    // ---------------- обводки ----------------
    void EmitStrokeQuad(const std::vector<Vec2>& pts, f32 halfWidth, const Color& c, const Paint& paint,
                        bool closed, enum LineCap cap, enum LineJoin join, f32 miterLimit) {
        if (pts.size() < 2) return;
        CallKey key = KeyFor(paint, Program::Sprite);
        EnsureCall(key, paint);

        std::vector<Vec2> nrm(pts.size());
        for (usize i = 0; i < pts.size(); ++i) {
            Vec2 prev = pts[i > 0 ? i - 1 : (closed ? pts.size() - 1 : 0)];
            Vec2 next = pts[i + 1 < pts.size() ? i + 1 : (closed ? 0 : pts.size() - 1)];
            Vec2 d = next - prev;
            if (LengthSq(d) < 1e-10f) d = Vec2{1, 0};
            nrm[i] = Normalize(Perp(d));
        }

        u32 base = VertexBase();
        for (usize i = 0; i < pts.size(); ++i) {
            PushVertex(pts[i] + nrm[i] * halfWidth, pts[i], c);
            PushVertex(pts[i] - nrm[i] * halfWidth, pts[i], c);
        }
        for (usize i = 0; i + 1 < pts.size(); ++i) {
            u32 a = base + static_cast<u32>(i * 2);
            PushTriangle(a, a + 1, a + 2);
            PushTriangle(a + 1, a + 3, a + 2);
        }
        if (closed) {
            u32 a = base + static_cast<u32>((pts.size() - 1) * 2);
            PushTriangle(a, a + 1, base);
            PushTriangle(a + 1, base + 1, base);
        }

        // Стыки
        auto emitJoin = [&](usize i) {
            const Vec2& p = pts[i];
            Vec2 n0 = nrm[i > 0 ? i - 1 : (closed ? pts.size() - 1 : 0)];
            Vec2 n1 = nrm[i];
            if (join == LineJoin::Bevel) {
                u32 v = VertexBase();
                PushVertex(p + n0 * halfWidth, p, c);
                PushVertex(p - n0 * halfWidth, p, c);
                PushVertex(p + n1 * halfWidth, p, c);
                PushVertex(p - n1 * halfWidth, p, c);
                PushTriangle(v, v + 1, v + 2);
                PushTriangle(v + 1, v + 3, v + 2);
                return;
            }
            if (join == LineJoin::Round) {
                f32 a0 = std::atan2(n0.y, n0.x);
                f32 a1 = std::atan2(n1.y, n1.x);
                f32 diff = a1 - a0;
                while (diff > kPi) diff -= kTau;
                while (diff < -kPi) diff += kTau;
                int steps = MaxT(2, static_cast<int>(std::fabs(diff) / 0.4f) + 1);
                u32 center = VertexBase();
                PushVertex(p, p, c);
                for (int s = 0; s <= steps; ++s) {
                    f32 a = a0 + diff * (static_cast<f32>(s) / steps);
                    PushVertex(p + Vec2{std::cos(a), std::sin(a)} * halfWidth, p, c);
                }
                for (int s = 0; s < steps; ++s) PushTriangle(center, center + 1 + s, center + 2 + s);
                return;
            }
            // Miter-стык
            Vec2 miter = n0 + n1;
            f32 len2 = LengthSq(miter);
            if (len2 < 1e-8f) return;
            miter = miter / len2;
            if (Length(miter) * 2.0f > miterLimit) {
                miter = Normalize(miter) * (halfWidth * 0.5f);
            } else {
                miter = miter * (halfWidth * 2.0f) * 0.5f;
            }
            // Сторона выбирается по направлению поворота.
            f32 cross = Cross(n0, n1);
            Vec2 outer = cross > 0 ? p + miter : p - miter;
            u32 v = VertexBase();
            PushVertex(p + n0 * halfWidth, p, c);
            PushVertex(p - n0 * halfWidth, p, c);
            PushVertex(outer, p, c);
            if (cross > 0) {
                PushTriangle(v, v + 1, v + 2);
            } else {
                PushTriangle(v, v + 2, v + 1);
            }
        };
        for (usize i = 0; i < pts.size(); ++i) {
            if (!closed && (i == 0 || i + 1 == pts.size())) continue;
            emitJoin(i);
        }

        // Торцы
        if (!closed && cap != LineCap::Butt) {
            auto emitCap = [&](usize i, f32 dir) {
                const Vec2& p = pts[i];
                Vec2 n = nrm[i];
                if (cap == LineCap::Square) {
                    Vec2 ext = p + Perp(n) * (halfWidth * dir);
                    u32 v = VertexBase();
                    PushVertex(p + n * halfWidth, p, c);
                    PushVertex(p - n * halfWidth, p, c);
                    PushVertex(ext + n * halfWidth, p, c);
                    PushVertex(ext - n * halfWidth, p, c);
                    if (dir > 0) {
                        PushTriangle(v, v + 1, v + 2);
                        PushTriangle(v + 1, v + 3, v + 2);
                    } else {
                        PushTriangle(v, v + 2, v + 1);
                        PushTriangle(v + 1, v + 2, v + 3);
                    }
                } else {
                    f32 a0 = std::atan2(n.y, n.x);
                    int steps = 8;
                    u32 center = VertexBase();
                    PushVertex(p, p, c);
                    for (int s = 0; s <= steps; ++s) {
                        f32 a = a0 - kPi * 0.5f + (kPi * static_cast<f32>(s) / steps);
                        PushVertex(p + Vec2{std::cos(a), std::sin(a)} * halfWidth, p, c);
                    }
                    for (int s = 0; s < steps; ++s) {
                        if (dir > 0)
                            PushTriangle(center, center + 1 + s, center + 2 + s);
                        else
                            PushTriangle(center, center + 2 + s, center + 1 + s);
                    }
                }
            };
            emitCap(0, -1.0f);
            emitCap(pts.size() - 1, 1.0f);
        }
    }
};

// ---------------------------------------------------------------------------
// Renderer2D
// ---------------------------------------------------------------------------
Renderer2D::Renderer2D() : impl_(new Impl()) {}

Renderer2D::~Renderer2D() {
    Shutdown();
    delete impl_;
}

bool Renderer2D::Init() {
    if (impl_->ready) return true;
    impl_->ResetPath();
    impl_->state = State{};
    // Инертный режим: пути, раскладка текста и hit testing работают, отправка
    // на GPU пропускается. Это сохраняет работоспособность UI-кода (и тестов)
    // без контекста. Одного `glCreateShader` недостаточно для проверки:
    // указатели остаются валидными и после уничтожения контекста, а
    // `glGetString(GL_VERSION)` возвращает null ровно когда контекста нет.
    const bool haveContext = gl::glCreateShader && gl::glGetString &&
                             gl::glGetString(gl::GL_VERSION) != nullptr;
    if (!haveContext) {
        ENG_LOGD("r2d", "no current GL context; Renderer2D initialised in inert mode");
        impl_->ready = false;
        impl_->initialised = true;
        return true;
    }
    if (!impl_->sprite.Build(builtin::kSpriteVert, builtin::kSpriteFrag, "r2d-sprite") ||
        !impl_->sdf.Build(builtin::kSdfTextVert, builtin::kSdfTextFrag, "r2d-sdf")) {
        // Откатываемся в инертный режим, а не падаем: драйвер, отвергший
        // шейдеры, не должен ронять весь UI.
        ENG_LOGW("r2d", "shader build failed; Renderer2D initialised in inert mode");
        impl_->ready = false;
        impl_->initialised = true;
        return true;
    }

    gl::glGenVertexArrays(1, &impl_->vao);
    gl::glGenBuffers(1, &impl_->vbo);
    gl::glGenBuffers(1, &impl_->ibo);
    gl::glBindVertexArray(impl_->vao);
    gl::glBindBuffer(gl::GL_ARRAY_BUFFER, impl_->vbo);
    gl::glBindBuffer(gl::GL_ELEMENT_ARRAY_BUFFER, impl_->ibo);
    gl::glEnableVertexAttribArray(0);
    gl::glVertexAttribPointer(0, 2, gl::GL_FLOAT, gl::GL_FALSE, sizeof(R2DVertex),
                              reinterpret_cast<const void*>(offsetof(R2DVertex, pos)));
    gl::glEnableVertexAttribArray(1);
    gl::glVertexAttribPointer(1, 2, gl::GL_FLOAT, gl::GL_FALSE, sizeof(R2DVertex),
                              reinterpret_cast<const void*>(offsetof(R2DVertex, uv)));
    gl::glEnableVertexAttribArray(2);
    gl::glVertexAttribPointer(2, 4, gl::GL_FLOAT, gl::GL_FALSE, sizeof(R2DVertex),
                              reinterpret_cast<const void*>(offsetof(R2DVertex, color)));
    gl::glBindVertexArray(0);
    impl_->ready = true;
    impl_->initialised = true;
    impl_->ResetPath();
    impl_->state = State{};
    return true;
}

void Renderer2D::Shutdown() {
    if (!impl_) return;
    if (impl_->vao && gl::glDeleteVertexArrays) {
        gl::glDeleteVertexArrays(1, &impl_->vao);
        gl::glDeleteBuffers(1, &impl_->vbo);
        gl::glDeleteBuffers(1, &impl_->ibo);
        if (impl_->whiteTexture) gl::glDeleteTextures(1, &impl_->whiteTexture);
    }
    impl_->vao = impl_->vbo = impl_->ibo = 0;
    impl_->whiteTexture = 0;
    impl_->sprite.Destroy();
    impl_->sdf.Destroy();
    impl_->ready = false;
}

void Renderer2D::BeginFrame(int fbWidth, int fbHeight, f32 dpiScale, RenderTarget* target) {
    impl_->dpi = dpiScale > 0 ? dpiScale : 1.0f;
    if (fbWidth <= 0) fbWidth = 1;
    if (fbHeight <= 0) fbHeight = 1;
    f32 logicalW = static_cast<f32>(fbWidth) / impl_->dpi;
    f32 logicalH = static_cast<f32>(fbHeight) / impl_->dpi;
    impl_->screen = crossrender::Rect{0, 0, logicalW, logicalH};
    impl_->target = target;
    projection_ = Mat4::Ortho2D(logicalW, logicalH);
    dpiScale_ = impl_->dpi;

    impl_->vertices.clear();
    impl_->indices.clear();
    impl_->calls.clear();
    impl_->scissors.clear();
    impl_->state = State{};
    impl_->state.transform = Mat4::Identity();
    impl_->stack.clear();
    impl_->ResetPath();
    impl_->inFrame = true;
    stats_ = Stats{};
    impl_->viewportBlitTex = nullptr;

    if (impl_->ready) {
        if (impl_->whiteTexture == 0) {
            gl::glGenTextures(1, &impl_->whiteTexture);
            gl::glBindTexture(gl::GL_TEXTURE_2D, impl_->whiteTexture);
            const unsigned char white[4] = {255, 255, 255, 255};
            gl::glPixelStorei(gl::GL_UNPACK_ALIGNMENT, 1);
            gl::glTexImage2D(gl::GL_TEXTURE_2D, 0, static_cast<gl::GLint>(gl::GL_RGBA8), 1, 1, 0,
                             gl::GL_RGBA, gl::GL_UNSIGNED_BYTE, white);
            gl::glTexParameteri(gl::GL_TEXTURE_2D, gl::GL_TEXTURE_MIN_FILTER, gl::GL_NEAREST);
            gl::glTexParameteri(gl::GL_TEXTURE_2D, gl::GL_TEXTURE_MAG_FILTER, gl::GL_NEAREST);
            gl::glBindTexture(gl::GL_TEXTURE_2D, 0);
        }
        gl::glEnable(gl::GL_BLEND);
        gl::glBlendFunc(gl::GL_ONE, gl::GL_ONE_MINUS_SRC_ALPHA);
        gl::glDisable(gl::GL_DEPTH_TEST);
        gl::glDepthMask(0);
        gl::glDisable(gl::GL_CULL_FACE);
        gl::glDisable(gl::GL_SCISSOR_TEST);
    }
}

void Renderer2D::EndFrame() {
    if (!impl_->inFrame) return;
    Flush();
    impl_->inFrame = false;
    impl_->viewportBlitTex = nullptr;
}

void Renderer2D::Flush() {
    if (impl_->vertices.empty() || impl_->indices.empty() || !impl_->ready) {
        impl_->vertices.clear();
        impl_->indices.clear();
        impl_->calls.clear();
        return;
    }
    auto& im = *impl_;
    gl::glBindVertexArray(im.vao);

    usize vBytes = im.vertices.size() * sizeof(R2DVertex);
    if (vBytes > im.vboCapacity) {
        im.vboCapacity = vBytes * 2;
        gl::glBindBuffer(gl::GL_ARRAY_BUFFER, im.vbo);
        gl::glBufferData(gl::GL_ARRAY_BUFFER, static_cast<gl::GLsizeiptr>(im.vboCapacity), nullptr,
                         gl::GL_DYNAMIC_DRAW);
        stats_.vertexBufferBytes = im.vboCapacity;
    }
    usize iBytes = im.indices.size() * sizeof(u32);
    if (iBytes > im.iboCapacity) {
        im.iboCapacity = iBytes * 2;
        gl::glBindBuffer(gl::GL_ELEMENT_ARRAY_BUFFER, im.ibo);
        gl::glBufferData(gl::GL_ELEMENT_ARRAY_BUFFER, static_cast<gl::GLsizeiptr>(im.iboCapacity),
                         nullptr, gl::GL_DYNAMIC_DRAW);
    }
    gl::glBindBuffer(gl::GL_ARRAY_BUFFER, im.vbo);
    gl::glBufferSubData(gl::GL_ARRAY_BUFFER, 0, static_cast<gl::GLsizeiptr>(vBytes),
                        im.vertices.data());
    gl::glBindBuffer(gl::GL_ELEMENT_ARRAY_BUFFER, im.ibo);
    gl::glBufferSubData(gl::GL_ELEMENT_ARRAY_BUFFER, 0, static_cast<gl::GLsizeiptr>(iBytes),
                        im.indices.data());

    bool scissorEnabled = false;
    BlendMode currentBlend = BlendMode::None;
    unsigned int currentProgram = 0;

    auto applyBlend = [&](BlendMode mode) {
        if (mode == currentBlend) return;
        currentBlend = mode;
        switch (mode) {
            case BlendMode::None:
            case BlendMode::Opaque:
                gl::glDisable(gl::GL_BLEND);
                gl::glBlendEquation(gl::GL_FUNC_ADD);
                break;
            case BlendMode::Alpha:
            case BlendMode::Premultiplied:
                gl::glEnable(gl::GL_BLEND);
                gl::glBlendEquation(gl::GL_FUNC_ADD);
                gl::glBlendFunc(gl::GL_ONE, gl::GL_ONE_MINUS_SRC_ALPHA);
                break;
            case BlendMode::Additive:
                gl::glEnable(gl::GL_BLEND);
                gl::glBlendEquation(gl::GL_FUNC_ADD);
                gl::glBlendFunc(gl::GL_ONE, gl::GL_ONE);
                break;
            case BlendMode::Multiply:
                gl::glEnable(gl::GL_BLEND);
                gl::glBlendEquation(gl::GL_FUNC_ADD);
                gl::glBlendFunc(gl::GL_DST_COLOR, gl::GL_ZERO);
                break;
            case BlendMode::Screen:
                gl::glEnable(gl::GL_BLEND);
                gl::glBlendEquation(gl::GL_FUNC_ADD);
                gl::glBlendFunc(gl::GL_ONE, gl::GL_ONE_MINUS_SRC_COLOR);
                break;
            case BlendMode::Min:
                gl::glEnable(gl::GL_BLEND);
                gl::glBlendEquation(gl::GL_MIN);
                break;
            case BlendMode::Max:
                gl::glEnable(gl::GL_BLEND);
                gl::glBlendEquation(gl::GL_MAX);
                break;
        }
    };

    for (const DrawCall& call : im.calls) {
        if (call.indexCount == 0) continue;
        const ScissorRect& sc = call.key.scissor;
        if (sc.enabled) {
            f32 scale = im.dpi;
            gl::glEnable(gl::GL_SCISSOR_TEST);
            scissorEnabled = true;
            int x = static_cast<int>(sc.x * scale);
            int y = static_cast<int>(im.screen.h * scale - (sc.y + sc.h) * scale);
            int w = static_cast<int>(sc.w * scale);
            int h = static_cast<int>(sc.h * scale);
            if (w < 0) w = 0;
            if (h < 0) h = 0;
            gl::glScissor(x, y, w, h);
        } else if (scissorEnabled) {
            gl::glDisable(gl::GL_SCISSOR_TEST);
            scissorEnabled = false;
        }
        applyBlend(call.key.blend);

        unsigned int prog = call.key.program == Program::Sprite ? im.sprite.Id() : im.sdf.Id();
        if (prog != currentProgram) {
            gl::glUseProgram(prog);
            currentProgram = prog;
        }

        if (call.key.program == Program::Sprite) {
            im.sprite.Set("uViewProj", projection_);
            im.sprite.Set("uType", call.key.paintType);
            gl::glActiveTexture(gl::GL_TEXTURE0);
            gl::glBindTexture(gl::GL_TEXTURE_2D,
                              call.key.texture ? call.key.texture : im.whiteTexture);
            im.sprite.Set("uTexture", 0);
            const Paint& p = call.paint;
            f32 s = im.TransformScale();
            Vec2 a = p.p0;
            Vec2 b = p.p1;
            switch (p.type) {
                case Paint::Type::LinearGradient:
                    im.sprite.Set("uGradA", a);
                    im.sprite.Set("uGradB", b);
                    im.sprite.Set("uInnerColor", p.innerColor);
                    im.sprite.Set("uOuterColor", p.outerColor);
                    break;
                case Paint::Type::RadialGradient:
                    im.sprite.Set("uGradA", a);
                    im.sprite.Set("uGradB", b);
                    im.sprite.Set("uRadiusA", p.r0 * s);
                    im.sprite.Set("uRadiusB", p.r1 * s);
                    im.sprite.Set("uInnerColor", p.innerColor);
                    im.sprite.Set("uOuterColor", p.outerColor);
                    break;
                case Paint::Type::BoxGradient:
                    im.sprite.Set("uGradA", a);
                    im.sprite.Set("uRadiusA", p.r0 * s);
                    im.sprite.Set("uFeather", p.feather * s);
                    im.sprite.Set("uBoxSize", p.p1);
                    im.sprite.Set("uInnerColor", p.innerColor);
                    im.sprite.Set("uOuterColor", p.outerColor);
                    break;
                default:
                    im.sprite.Set("uGradA", Vec2{0, 0});
                    im.sprite.Set("uGradB", Vec2{1, 1});
                    im.sprite.Set("uInnerColor", Color::White);
                    im.sprite.Set("uOuterColor", Color::White);
                    break;
            }
        } else {
            gl::glActiveTexture(gl::GL_TEXTURE0);
            gl::glBindTexture(gl::GL_TEXTURE_2D,
                              call.key.texture ? call.key.texture : im.whiteTexture);
            im.sdf.Set("uViewProj", projection_);
            im.sdf.Set("uTexture", 0);
            im.sdf.Set("uSdfSpread", call.sdfSpread);
            im.sdf.Set("uPixelRange", call.sdfPixelRange);
            im.sdf.Set("uOutlineWidth", call.sdfOutlineWidth);
            im.sdf.Set("uOutlineColor", call.sdfOutlineColor);
            im.sdf.Set("uSoftness", 1.0f);
        }

        gl::glDrawElements(gl::GL_TRIANGLES, static_cast<gl::GLsizei>(call.indexCount),
                           gl::GL_UNSIGNED_INT,
                           reinterpret_cast<const void*>(static_cast<usize>(call.indexOffset) * sizeof(u32)));
        ++stats_.drawCalls;
        stats_.vertices += static_cast<int>(call.indexCount);
    }

    if (scissorEnabled) gl::glDisable(gl::GL_SCISSOR_TEST);
    gl::glBindVertexArray(0);
    gl::glUseProgram(0);
    stats_.vertices /= 3;
    im.vertices.clear();
    im.indices.clear();
    im.calls.clear();
}

// ---------------------------------------------------------------------------
// Состояние
// ---------------------------------------------------------------------------
void Renderer2D::Save() {
    impl_->stack.push_back(impl_->state);
    impl_->scissors.push_back(impl_->state.scissor);
}

void Renderer2D::Restore() {
    if (impl_->stack.empty()) return;
    impl_->state = impl_->stack.back();
    impl_->stack.pop_back();
    if (!impl_->scissors.empty()) impl_->scissors.pop_back();
}

void Renderer2D::Reset() {
    Flush();
    impl_->stack.clear();
    impl_->scissors.clear();
    impl_->state = State{};
    impl_->state.transform = Mat4::Identity();
    impl_->ResetPath();
}

void Renderer2D::ResetTransform() { impl_->state.transform = Mat4::Identity(); }
void Renderer2D::ResetState() { Reset(); }

void Renderer2D::GlobalAlpha(f32 alpha) { impl_->state.alpha = Clamp(alpha, 0.0f, 1.0f); }
f32 Renderer2D::GetGlobalAlpha() const { return impl_->state.alpha; }
void Renderer2D::Composite(BlendMode mode) { impl_->state.blend = mode; }
void Renderer2D::AntiAlias(bool enabled) { impl_->state.antialias = enabled; }
void Renderer2D::LineCap(enum LineCap cap) { impl_->state.cap = cap; }
void Renderer2D::LineJoin(enum LineJoin join) { impl_->state.join = join; }
void Renderer2D::MiterLimit(f32 limit) { impl_->state.miterLimit = limit; }
void Renderer2D::StrokeWidth(f32 width) { impl_->state.strokeWidth = width; }
void Renderer2D::FillPaint(const Paint& paint) { impl_->state.fill = paint; }
void Renderer2D::StrokePaint(const Paint& paint) { impl_->state.stroke = paint; }
void Renderer2D::FillColor(const Color& c) { impl_->state.fill = Paint::Solid(c); }
void Renderer2D::StrokeColor(const Color& c) { impl_->state.stroke = Paint::Solid(c); }
const Paint& Renderer2D::CurrentFill() const { return impl_->state.fill; }
const Paint& Renderer2D::CurrentStroke() const { return impl_->state.stroke; }

// ---------------------------------------------------------------------------
// Трансформация
// ---------------------------------------------------------------------------
void Renderer2D::Translate(f32 x, f32 y) {
    impl_->state.transform = impl_->state.transform * Mat4::Translate(Vec3{x, y, 0});
}
void Renderer2D::Rotate(f32 radians) {
    impl_->state.transform = impl_->state.transform * Mat4::RotateZ(radians);
}
void Renderer2D::Scale(f32 x, f32 y) {
    impl_->state.transform = impl_->state.transform * Mat4::Scale(Vec3{x, y, 1});
}
void Renderer2D::SkewX(f32 radians) {
    Mat4 m;
    m.at(1, 0) = std::tan(radians);
    impl_->state.transform = impl_->state.transform * m;
}
void Renderer2D::SkewY(f32 radians) {
    Mat4 m;
    m.at(0, 1) = std::tan(radians);
    impl_->state.transform = impl_->state.transform * m;
}
void Renderer2D::Transform(f32 a, f32 b, f32 c, f32 d, f32 e, f32 f) {
    Mat4 m;
    m.at(0, 0) = a; m.at(0, 1) = b;
    m.at(1, 0) = c; m.at(1, 1) = d;
    m.at(3, 0) = e; m.at(3, 1) = f;
    impl_->state.transform = impl_->state.transform * m;
}
Vec2 Renderer2D::TransformPoint(const Vec2& p) const { return impl_->Xf(p); }

// ---------------------------------------------------------------------------
// Пути
// ---------------------------------------------------------------------------
void Renderer2D::BeginPath() { impl_->ResetPath(); }

void Renderer2D::MoveTo(f32 x, f32 y) {
    impl_->FinishContour(false);
    impl_->cursor = {x, y};
    impl_->pathStart = {x, y};
    impl_->current.push_back(impl_->cursor);
}

void Renderer2D::LineTo(f32 x, f32 y) {
    if (impl_->current.empty()) {
        MoveTo(x, y);
        return;
    }
    impl_->current.push_back({x, y});
    impl_->cursor = {x, y};
}

void Renderer2D::BezierTo(f32 c1x, f32 c1y, f32 c2x, f32 c2y, f32 x, f32 y) {
    if (impl_->current.empty()) MoveTo(c1x, c1y);
    FlattenCubic(&impl_->current, impl_->cursor, {c1x, c1y}, {c2x, c2y}, {x, y});
    impl_->cursor = {x, y};
}

void Renderer2D::QuadTo(f32 cx, f32 cy, f32 x, f32 y) {
    if (impl_->current.empty()) MoveTo(cx, cy);
    FlattenQuad(&impl_->current, impl_->cursor, {cx, cy}, {x, y});
    impl_->cursor = {x, y};
}

void Renderer2D::ArcTo(f32 x1, f32 y1, f32 x2, f32 y2, f32 radius) {
    Vec2 p0 = impl_->cursor;
    Vec2 p1{x1, y1};
    Vec2 p2{x2, y2};
    Vec2 d0 = Normalize(p0 - p1);
    Vec2 d1 = Normalize(p2 - p1);
    f32 a = std::acos(Clamp(Dot(d0, d1), -1.0f, 1.0f));
    if (a < 1e-4f || radius <= 0) {
        LineTo(x2, y2);
        return;
    }
    f32 tanHalf = std::tan(a * 0.5f);
    if (tanHalf < 1e-6f) {
        LineTo(x2, y2);
        return;
    }
    f32 dist = radius / tanHalf;
    Vec2 t0 = p1 + d0 * dist;
    Vec2 t1 = p1 + d1 * dist;
    // Аппроксимируем дугу квадратичной кривой через точки касания.
    QuadTo(x1, y1, t1.x, t1.y);
    LineTo(x2, y2);
    (void)t0;
}

void Renderer2D::Arc(f32 cx, f32 cy, f32 r, f32 a0, f32 a1, Winding dir) {
    f32 da = a1 - a0;
    if (dir == Winding::CW && da < 0) da += kTau;
    if (dir == Winding::CCW && da > 0) da -= kTau;
    int steps = MaxT(2, static_cast<int>(std::fabs(da) / (kPi / 16.0f)) + 1);
    Vec2 start{cx + std::cos(a0) * r, cy + std::sin(a0) * r};
    if (impl_->current.empty())
        MoveTo(start.x, start.y);
    else
        LineTo(start.x, start.y);
    for (int i = 1; i <= steps; ++i) {
        f32 a = a0 + da * (static_cast<f32>(i) / steps);
        LineTo(cx + std::cos(a) * r, cy + std::sin(a) * r);
    }
}

void Renderer2D::ClosePath() {
    impl_->FinishContour(true);
    impl_->cursor = impl_->pathStart;
}

void Renderer2D::PathWinding(Winding dir) { impl_->winding = dir; }

void Renderer2D::Rect(f32 x, f32 y, f32 w, f32 h) {
    BeginPath();
    MoveTo(x, y);
    LineTo(x + w, y);
    LineTo(x + w, y + h);
    LineTo(x, y + h);
    ClosePath();
}

void Renderer2D::RoundedRect(f32 x, f32 y, f32 w, f32 h, f32 r) {
    RoundedRectVarying(x, y, w, h, r, r, r, r);
}

void Renderer2D::RoundedRectVarying(f32 x, f32 y, f32 w, f32 h, f32 tl, f32 tr, f32 br, f32 bl) {
    f32 maxR = MinT(MinT(w, h) * 0.5f, 1e6f);
    tl = Clamp(tl, 0.0f, maxR);
    tr = Clamp(tr, 0.0f, maxR);
    br = Clamp(br, 0.0f, maxR);
    bl = Clamp(bl, 0.0f, maxR);
    BeginPath();
    MoveTo(x + tl, y);
    LineTo(x + w - tr, y);
    if (tr > 0) QuadTo(x + w, y, x + w, y + tr);
    LineTo(x + w, y + h - br);
    if (br > 0) QuadTo(x + w, y + h, x + w - br, y + h);
    LineTo(x + bl, y + h);
    if (bl > 0) QuadTo(x, y + h, x, y + h - bl);
    LineTo(x, y + tl);
    if (tl > 0) QuadTo(x, y, x + tl, y);
    ClosePath();
}

void Renderer2D::Ellipse(f32 cx, f32 cy, f32 rx, f32 ry) {
    const int steps = 48;
    BeginPath();
    for (int i = 0; i < steps; ++i) {
        f32 a = static_cast<f32>(i) / steps * kTau;
        f32 px = cx + std::cos(a) * rx;
        f32 py = cy + std::sin(a) * ry;
        if (i == 0)
            MoveTo(px, py);
        else
            LineTo(px, py);
    }
    ClosePath();
}

void Renderer2D::Circle(f32 cx, f32 cy, f32 r) { Ellipse(cx, cy, r, r); }

void Renderer2D::Polyline(const Vec2* pts, int count) {
    if (!pts || count < 2) return;
    BeginPath();
    MoveTo(pts[0].x, pts[0].y);
    for (int i = 1; i < count; ++i) LineTo(pts[i].x, pts[i].y);
}

void Renderer2D::Polygon(const Vec2* pts, int count) {
    if (!pts || count < 3) return;
    Polyline(pts, count);
    ClosePath();
}

void Renderer2D::Spline(const Vec2* pts, int count, f32 tension) {
    if (!pts || count < 3) {
        Polyline(pts, count);
        return;
    }
    BeginPath();
    MoveTo(pts[0].x, pts[0].y);
    for (int i = 0; i < count - 1; ++i) {
        Vec2 p0 = pts[i > 0 ? i - 1 : 0];
        Vec2 p1 = pts[i];
        Vec2 p2 = pts[i + 1];
        Vec2 p3 = pts[i + 2 < count ? i + 2 : count - 1];
        Vec2 c1 = p1 + (p2 - p0) * (tension / 3.0f);
        Vec2 c2 = p2 - (p3 - p1) * (tension / 3.0f);
        BezierTo(c1.x, c1.y, c2.x, c2.y, p2.x, p2.y);
    }
}

void Renderer2D::Fill() {
    impl_->FinishContour(false);
    if (impl_->contours.empty()) return;
    ++stats_.paths;

    // Строим геометрию заливки (с мостами для дыр + AA-каймой).
    auto& im = *impl_;
    std::vector<std::vector<Vec2>> polys = im.contours;
    for (auto& p : polys) {
        for (Vec2& v : p) v = im.Xf(v);
        std::vector<Vec2> cleaned;
        for (const Vec2& v : p)
            if (cleaned.empty() || LengthSq(cleaned.back() - v) > 1e-8f) cleaned.push_back(v);
        if (cleaned.size() >= 2 && LengthSq(cleaned.front() - cleaned.back()) < 1e-8f)
            cleaned.pop_back();
        p = std::move(cleaned);
    }
    polys.erase(std::remove_if(polys.begin(), polys.end(),
                               [](const std::vector<Vec2>& p) { return p.size() < 3; }),
                polys.end());
    if (polys.empty()) {
        im.contours.clear();
        return;
    }

    usize n = polys.size();
    std::vector<int> depth(n, 0);
    std::vector<f32> area(n, 0);
    for (usize i = 0; i < n; ++i) area[i] = std::fabs(PolygonArea(polys[i]));
    for (usize i = 0; i < n; ++i)
        for (usize j = 0; j < n; ++j)
            if (i != j && area[j] > area[i] && PointInPolygon(polys[j], polys[i][0])) ++depth[i];

    std::vector<std::vector<Vec2>> outers, holes;
    std::vector<int> outerSrc;
    std::vector<int> holeOwner;
    for (usize i = 0; i < n; ++i) {
        if (depth[i] % 2 == 0) {
            outers.push_back(polys[i]);
            outerSrc.push_back(static_cast<int>(i));
        } else {
            holes.push_back(polys[i]);
            int owner = -1;
            f32 bestArea = 1e30f;
            for (usize o = 0; o < outers.size(); ++o) {
                if (PointInPolygon(outers[o], polys[i][0]) &&
                    area[static_cast<usize>(outerSrc[o])] < bestArea) {
                    bestArea = area[static_cast<usize>(outerSrc[o])];
                    owner = static_cast<int>(o);
                }
            }
            holeOwner.push_back(owner);
        }
    }
    for (usize h = 0; h < holes.size(); ++h) {
        int owner = holeOwner[h];
        if (owner < 0) continue;
        std::vector<Vec2>& outer = outers[static_cast<usize>(owner)];
        // Дыра должна обходиться противоположно внешнему контуру, иначе мост не сработает.
        if ((PolygonArea(outer) < 0) == (PolygonArea(holes[h]) < 0)) {
            std::reverse(holes[h].begin(), holes[h].end());
        }
        BridgeHole(&outer, holes[h]);
    }

    const Paint& paint = im.state.fill;
    Color baseColor = (paint.type == Paint::Type::Solid || paint.type == Paint::Type::ImagePattern)
                          ? im.WithAlpha(paint.color)
                          : Color{1, 1, 1, im.state.alpha};
    CallKey key = im.KeyFor(paint, Program::Sprite);

    for (const auto& poly : outers) {
        if (poly.size() < 3) continue;
        std::vector<u32> tris;
        EarClip(poly, &tris);
        if (tris.empty()) continue;
        im.EnsureCall(key, paint);
        u32 base = im.VertexBase();
        for (const Vec2& p : poly) im.PushVertex(p, p, baseColor);
        for (usize t = 0; t + 2 < tris.size(); t += 3)
            im.PushTriangle(base + tris[t], base + tris[t + 1], base + tris[t + 2]);
    }

    // Аналитическая AA-кайма вокруг каждого исходного контура.
    if (im.state.antialias) {
        Color fringeColor = baseColor;
        if (paint.type != Paint::Type::Solid && paint.type != Paint::Type::ImagePattern)
            fringeColor = Color{1, 1, 1, im.state.alpha};
        for (const auto& c : im.contours) {
            std::vector<Vec2> pts;
            for (const Vec2& v : c) pts.push_back(im.Xf(v));
            if (pts.size() < 3) continue;
            bool ccw = PolygonArea(pts) < 0;
            im.EnsureCall(key, paint);
            for (usize i = 0; i < pts.size(); ++i) {
                const Vec2& a = pts[i];
                const Vec2& b = pts[(i + 1) % pts.size()];
                Vec2 d = b - a;
                if (LengthSq(d) < 1e-10f) continue;
                d = Normalize(d);
                Vec2 nrm = ccw ? Vec2{d.y, -d.x} : Vec2{-d.y, d.x};
                // Отбрасываем внутреннюю нормаль.
                Vec2 mid = (a + b) * 0.5f;
                if (PointInPolygon(pts, mid + nrm * 0.75f)) nrm = -nrm;
                Color inner = fringeColor;
                Color outer = fringeColor.WithAlpha(0.0f);
                u32 vbase = im.VertexBase();
                im.PushVertex(a, a, inner);
                im.PushVertex(b, b, inner);
                im.PushVertex(b + nrm * kAaWidth, b, outer);
                im.PushVertex(a + nrm * kAaWidth, a, outer);
                im.PushTriangle(vbase, vbase + 1, vbase + 2);
                im.PushTriangle(vbase, vbase + 2, vbase + 3);
            }
        }
    }

    im.EndPrimitive();
    im.contours.clear();
}

void Renderer2D::Stroke() {
    impl_->FinishContour(false);
    if (impl_->contours.empty()) return;
    ++stats_.paths;
    auto& im = *impl_;
    const Paint& paint = im.state.stroke;
    Color baseColor = paint.type == Paint::Type::Solid ? im.WithAlpha(paint.color)
                                                       : Color{1, 1, 1, im.state.alpha};
    f32 xfScale = im.TransformScale();
    f32 halfWidth = MaxT(0.35f, im.state.strokeWidth * 0.5f * (xfScale > 0 ? xfScale : 1.0f));
    for (const auto& c : im.contours) {
        std::vector<Vec2> pts;
        pts.reserve(c.size());
        for (const Vec2& v : c) {
            Vec2 t = im.Xf(v);
            if (pts.empty() || LengthSq(pts.back() - t) > 1e-8f) pts.push_back(t);
        }
        if (pts.size() < 2) continue;
        bool closed = LengthSq(pts.front() - pts.back()) < 1e-6f;
        if (closed && pts.size() > 2) pts.pop_back();
        im.EmitStrokeQuad(pts, halfWidth, baseColor, paint, closed, im.state.cap, im.state.join,
                          im.state.miterLimit);
    }
    im.EndPrimitive();
    im.contours.clear();
}

// ---------------------------------------------------------------------------
// Помощники мгновенной отрисовки
// ---------------------------------------------------------------------------
void Renderer2D::FillRect(f32 x, f32 y, f32 w, f32 h, const Color& c) {
    if (w <= 0 || h <= 0) return;
    ++stats_.paths;
    auto& im = *impl_;
    Vec2 p00 = im.Xf({x, y});
    Vec2 p10 = im.Xf({x + w, y});
    Vec2 p11 = im.Xf({x + w, y + h});
    Vec2 p01 = im.Xf({x, y + h});
    Color cc = im.WithAlpha(c);
    CallKey key = im.KeyFor(Paint::Solid(c), Program::Sprite);
    im.EnsureCall(key, Paint::Solid(c));
    u32 base = im.VertexBase();
    if (im.state.antialias) {
        Color zero = cc.WithAlpha(0.0f);
        // AA-квад из 4 вершин (внутренний непрозрачный, внешний прозрачный), растянутый в экранных координатах.
        Vec2 e{0.5f, 0.5f};
        im.PushVertex(p00 - e, p00, zero);
        im.PushVertex(p10 + Vec2{e.x, -e.y}, p10, zero);
        im.PushVertex(p11 + e, p11, zero);
        im.PushVertex(p01 + Vec2{-e.x, e.y}, p01, zero);
        im.PushVertex(p00, p00, cc);
        im.PushVertex(p10, p10, cc);
        im.PushVertex(p11, p11, cc);
        im.PushVertex(p01, p01, cc);
        // Кайма (внешнее кольцо)
        for (int i = 0; i < 4; ++i) {
            int j = (i + 1) % 4;
            im.PushTriangle(base + static_cast<u32>(i), base + static_cast<u32>(j),
                            base + static_cast<u32>(4 + j));
            im.PushTriangle(base + static_cast<u32>(i), base + static_cast<u32>(4 + j),
                            base + static_cast<u32>(4 + i));
        }
        // Внутренний квад
        im.PushTriangle(base + 4, base + 5, base + 6);
        im.PushTriangle(base + 4, base + 6, base + 7);
    } else {
        im.PushVertex(p00, p00, cc);
        im.PushVertex(p10, p10, cc);
        im.PushVertex(p11, p11, cc);
        im.PushVertex(p01, p01, cc);
        im.PushTriangle(base, base + 1, base + 2);
        im.PushTriangle(base, base + 2, base + 3);
    }
    im.EndPrimitive();
}

void Renderer2D::FillRoundedRect(const crossrender::Rect& r, f32 radius, const Color& c) {
    RoundedRect(r.x, r.y, r.w, r.h, radius);
    FillColor(c);
    Fill();
}

void Renderer2D::StrokeRect(const crossrender::Rect& r, const Color& c, f32 width) {
    Rect(r.x, r.y, r.w, r.h);
    StrokeColor(c);
    StrokeWidth(width);
    Stroke();
}

void Renderer2D::StrokeRoundedRect(const crossrender::Rect& r, f32 radius, const Color& c, f32 width) {
    RoundedRect(r.x, r.y, r.w, r.h, radius);
    StrokeColor(c);
    StrokeWidth(width);
    Stroke();
}

void Renderer2D::FillCircle(f32 cx, f32 cy, f32 r, const Color& c) {
    Circle(cx, cy, r);
    FillColor(c);
    Fill();
}

void Renderer2D::FillEllipse(f32 cx, f32 cy, f32 rx, f32 ry, const Color& c) {
    Ellipse(cx, cy, rx, ry);
    FillColor(c);
    Fill();
}

void Renderer2D::DrawLine(f32 x0, f32 y0, f32 x1, f32 y1, const Color& c, f32 width) {
    auto& im = *impl_;
    Vec2 a = im.Xf({x0, y0});
    Vec2 b = im.Xf({x1, y1});
    Vec2 d = Normalize(b - a);
    Vec2 nrm{-d.y, d.x};
    f32 hw = MaxT(0.5f, width * 0.5f);
    Color cc = im.WithAlpha(c);
    CallKey key = im.KeyFor(Paint::Solid(c), Program::Sprite);
    im.EnsureCall(key, Paint::Solid(c));
    u32 base = im.VertexBase();
    im.PushVertex(a + nrm * hw, a, cc);
    im.PushVertex(b + nrm * hw, b, cc);
    im.PushVertex(b - nrm * hw, b, cc);
    im.PushVertex(a - nrm * hw, a, cc);
    im.PushTriangle(base, base + 1, base + 2);
    im.PushTriangle(base, base + 2, base + 3);
    im.EndPrimitive();
}

void Renderer2D::FillRectGradient(const crossrender::Rect& r, const Color& a, const Color& b, bool vertical) {
    auto& im = *impl_;
    Paint p = vertical ? Paint::Linear(Vec2{r.x, r.y}, Vec2{r.x, r.y + r.h}, a, b)
                       : Paint::Linear(Vec2{r.x, r.y}, Vec2{r.x + r.w, r.y}, a, b);
    Vec2 p00 = im.Xf({r.x, r.y});
    Vec2 p10 = im.Xf({r.x + r.w, r.y});
    Vec2 p11 = im.Xf({r.x + r.w, r.y + r.h});
    Vec2 p01 = im.Xf({r.x, r.y + r.h});
    Color tint{1, 1, 1, im.state.alpha};
    CallKey key = im.KeyFor(p, Program::Sprite);
    im.EnsureCall(key, p);
    u32 base = im.VertexBase();
    im.PushVertex(p00, p00, tint);
    im.PushVertex(p10, p10, tint);
    im.PushVertex(p11, p11, tint);
    im.PushVertex(p01, p01, tint);
    im.PushTriangle(base, base + 1, base + 2);
    im.PushTriangle(base, base + 2, base + 3);
    im.EndPrimitive();
    ++stats_.paths;
}

void Renderer2D::FillRectGradient4(const crossrender::Rect& r, const Color& tl, const Color& tr, const Color& br,
                                   const Color& bl) {
    auto& im = *impl_;
    Paint p = Paint::Solid(Color::White);
    p.type = Paint::Type::ImagePattern;  // используем цвета вершин через сплошной путь
    p.image = nullptr;
    (void)p;
    Paint solid = Paint::Solid(Color::White);
    CallKey key = im.KeyFor(solid, Program::Sprite);
    key.paintType = 0;
    im.EnsureCall(key, solid);
    u32 base = im.VertexBase();
    im.PushVertex(im.Xf({r.x, r.y}), {0, 0}, tl);
    im.PushVertex(im.Xf({r.x + r.w, r.y}), {1, 0}, tr);
    im.PushVertex(im.Xf({r.x + r.w, r.y + r.h}), {1, 1}, br);
    im.PushVertex(im.Xf({r.x, r.y + r.h}), {0, 1}, bl);
    im.PushTriangle(base, base + 1, base + 2);
    im.PushTriangle(base, base + 2, base + 3);
    im.EndPrimitive();
    ++stats_.paths;
}

// ---------------------------------------------------------------------------
// Изображения
// ---------------------------------------------------------------------------
void Renderer2D::Image(const Texture& tex, const crossrender::Rect& dst, const crossrender::Rect& srcUV, const Color& tint,
                       f32 cornerRadius) {
    if (!tex.Valid()) return;
    if (cornerRadius > 0) {
        Save();
        RoundedRect(dst.x, dst.y, dst.w, dst.h, cornerRadius);
        ClipPath();
        Image(tex, dst, srcUV, tint, 0.0f);
        Restore();
        return;
    }
    auto& im = *impl_;
    Paint paint = Paint::Image(tex, tint);
    CallKey key = im.KeyFor(paint, Program::Sprite);
    Color c = im.WithAlpha(tint);
    Vec2 uv0{srcUV.x, srcUV.y};
    Vec2 uv1{srcUV.x + srcUV.w, srcUV.y + srcUV.h};
    Vec2 p00 = im.Xf({dst.x, dst.y});
    Vec2 p10 = im.Xf({dst.x + dst.w, dst.y});
    Vec2 p11 = im.Xf({dst.x + dst.w, dst.y + dst.h});
    Vec2 p01 = im.Xf({dst.x, dst.y + dst.h});
    im.EnsureCall(key, paint);
    u32 base = im.VertexBase();
    im.PushVertex(p00, uv0, c);
    im.PushVertex(p10, {uv1.x, uv0.y}, c);
    im.PushVertex(p11, uv1, c);
    im.PushVertex(p01, {uv0.x, uv1.y}, c);
    im.PushTriangle(base, base + 1, base + 2);
    im.PushTriangle(base, base + 2, base + 3);
    im.EndPrimitive();
    ++stats_.paths;
}

void Renderer2D::ImageQuad(const Texture& tex, const Vec2 quad[4], const crossrender::Rect& srcUV,
                           const Color& tint) {
    auto& im = *impl_;
    Paint paint = Paint::Image(tex, tint);
    CallKey key = im.KeyFor(paint, Program::Sprite);
    Color c = im.WithAlpha(tint);
    Vec2 uv0{srcUV.x, srcUV.y};
    Vec2 uv1{srcUV.x + srcUV.w, srcUV.y + srcUV.h};
    im.EnsureCall(key, paint);
    u32 base = im.VertexBase();
    im.PushVertex(im.Xf(quad[0]), uv0, c);
    im.PushVertex(im.Xf(quad[1]), {uv1.x, uv0.y}, c);
    im.PushVertex(im.Xf(quad[2]), uv1, c);
    im.PushVertex(im.Xf(quad[3]), {uv0.x, uv1.y}, c);
    im.PushTriangle(base, base + 1, base + 2);
    im.PushTriangle(base, base + 2, base + 3);
    im.EndPrimitive();
}

void Renderer2D::ImageTinted4(const Texture& tex, const crossrender::Rect& dst, const crossrender::Rect& srcUV,
                              const Color& tl, const Color& tr, const Color& br, const Color& bl) {
    auto& im = *impl_;
    Paint paint = Paint::Image(tex, Color::White);
    CallKey key = im.KeyFor(paint, Program::Sprite);
    Vec2 uv0{srcUV.x, srcUV.y};
    Vec2 uv1{srcUV.x + srcUV.w, srcUV.y + srcUV.h};
    im.EnsureCall(key, paint);
    u32 base = im.VertexBase();
    im.PushVertex(im.Xf({dst.x, dst.y}), uv0, im.WithAlpha(tl));
    im.PushVertex(im.Xf({dst.x + dst.w, dst.y}), {uv1.x, uv0.y}, im.WithAlpha(tr));
    im.PushVertex(im.Xf({dst.x + dst.w, dst.y + dst.h}), uv1, im.WithAlpha(br));
    im.PushVertex(im.Xf({dst.x, dst.y + dst.h}), {uv0.x, uv1.y}, im.WithAlpha(bl));
    im.PushTriangle(base, base + 1, base + 2);
    im.PushTriangle(base, base + 2, base + 3);
    im.EndPrimitive();
}

void Renderer2D::Image9(const Texture& tex, const crossrender::Rect& dst, const NinePatch& patch,
                        const Color& tint, f32 scale) {
    if (!tex.Valid()) return;
    f32 tw = static_cast<f32>(tex.Width());
    f32 th = static_cast<f32>(tex.Height());
    if (tw <= 0 || th <= 0) return;
    f32 l = patch.left * scale, t = patch.top * scale;
    f32 r = patch.right * scale, b = patch.bottom * scale;
    // Ограничиваем масштаб, чтобы патчи никогда не перекрывались.
    f32 hScale = MinT(1.0f, dst.w / MaxT(l + r, 1e-3f));
    f32 vScale = MinT(1.0f, dst.h / MaxT(t + b, 1e-3f));
    l *= hScale;
    r *= hScale;
    t *= vScale;
    b *= vScale;

    f32 u0 = 0, u1 = patch.left / tw, u2 = 1.0f - patch.right / tw, u3 = 1;
    f32 v0 = 0, v1 = patch.top / th, v2 = 1.0f - patch.bottom / th, v3 = 1;
    f32 x0 = dst.x, x1 = dst.x + l, x2 = dst.x + dst.w - r, x3 = dst.x + dst.w;
    f32 y0 = dst.y, y1 = dst.y + t, y2 = dst.y + dst.h - b, y3 = dst.y + dst.h;

    struct Piece {
        f32 x0, y0, x1, y1, u0, v0, u1, v1;
    };
    const Piece pieces[9] = {
        {x0, y0, x1, y1, u0, v0, u1, v1}, {x1, y0, x2, y1, u1, v0, u2, v1},
        {x2, y0, x3, y1, u2, v0, u3, v1}, {x0, y1, x1, y2, u0, v1, u1, v2},
        {x1, y1, x2, y2, u1, v1, u2, v2}, {x2, y1, x3, y2, u2, v1, u3, v2},
        {x0, y2, x1, y3, u0, v2, u1, v3}, {x1, y2, x2, y3, u1, v2, u2, v3},
        {x2, y2, x3, y3, u2, v2, u3, v3},
    };
    Save();
    for (const Piece& p : pieces) {
        if (p.x1 <= p.x0 || p.y1 <= p.y0) continue;
        Image(tex, crossrender::Rect{p.x0, p.y0, p.x1 - p.x0, p.y1 - p.y0},
              crossrender::Rect{p.u0, p.v0, p.u1 - p.u0, p.v1 - p.v0}, tint);
    }
    Restore();
}

// ---------------------------------------------------------------------------
// Отсечение
// ---------------------------------------------------------------------------
void Renderer2D::ClipRect(f32 x, f32 y, f32 w, f32 h) {
    auto& im = *impl_;
    crossrender::Rect world = crossrender::Rect{x, y, w, h};
    Vec2 corners[4] = {{world.x, world.y},
                       {world.x + world.w, world.y},
                       {world.x + world.w, world.y + world.h},
                       {world.x, world.y + world.h}};
    Vec2 lo{1e30f, 1e30f}, hi{-1e30f, -1e30f};
    for (Vec2 c : corners) {
        Vec2 p = im.Xf(c);
        lo = Min(lo, p);
        hi = Max(hi, p);
    }
    ScissorRect sc;
    sc.x = lo.x;
    sc.y = lo.y;
    sc.w = MaxT(0.0f, hi.x - lo.x);
    sc.h = MaxT(0.0f, hi.y - lo.y);
    sc.enabled = true;
    if (im.state.scissor.enabled) {
        ScissorRect cur = im.state.scissor;
        sc.x = MaxT(sc.x, cur.x);
        sc.y = MaxT(sc.y, cur.y);
        sc.w = MinT(sc.x + sc.w, cur.x + cur.w) - sc.x;
        sc.h = MinT(sc.y + sc.h, cur.y + cur.h) - sc.y;
        if (sc.w < 0) sc.w = 0;
        if (sc.h < 0) sc.h = 0;
    }
    im.state.scissor = sc;
    ++stats_.clipPushes;
}

void Renderer2D::ClipRoundedRect(f32 x, f32 y, f32 w, f32 h, f32 r) {
    (void)r;
    ClipRect(x, y, w, h);
}

void Renderer2D::ClipPath() {
    // AABB текущего пути (задокументированное приближение).
    if (impl_->contours.empty() && impl_->current.empty()) return;
    Vec2 lo{1e30f, 1e30f}, hi{-1e30f, -1e30f};
    auto expand = [&](const std::vector<Vec2>& c) {
        for (const Vec2& v : c) {
            Vec2 p = impl_->Xf(v);
            lo = Min(lo, p);
            hi = Max(hi, p);
        }
    };
    for (const auto& c : impl_->contours) expand(c);
    expand(impl_->current);
    if (lo.x > hi.x) return;
    ClipRect(lo.x, lo.y, hi.x - lo.x, hi.y - lo.y);
}

void Renderer2D::ResetClip() {
    impl_->state.scissor = ScissorRect{};
}

Rect Renderer2D::CurrentClip() const {
    const ScissorRect& sc = impl_->state.scissor;
    if (!sc.enabled) return impl_->screen;
    return crossrender::Rect{sc.x, sc.y, sc.w, sc.h};
}

// ---------------------------------------------------------------------------
// Текст
// ---------------------------------------------------------------------------
namespace {
// Font::GetKerning() возвращает шрифтовые единицы; переводим в логические пиксели для `size`.
inline f32 KernPx(const Font& font, u32 left, u32 right, f32 size) {
    if (left == 0 || right == 0) return 0.0f;
    const f32 upem = font.UnitsPerEm();
    if (upem <= 0.0f) return 0.0f;
    return font.GetKerning(left, right) * (size / upem);
}

struct GlyphPlacement {
    Rect dst;
    Rect uv;
    int page;
};

// Раскладывает UTF-8 строку в размещения глифов (в логических единицах).
void LayoutLine(const Font& font, const std::string& text, f32 size, f32 letterSpacing,
                f32 penX, f32 baselineY, std::vector<GlyphPlacement>* out, f32* advanceOut) {
    const f32 emSize = size > 0 ? size : font.Desc().pixelHeight;
    f32 scale = font.ScaleForSize(size);
    f32 pen = penX;
    u32 prev = 0;
    usize i = 0;
    const usize len = text.size();
    while (i < len) {
        u32 cp = Utf8Decode(text.c_str(), len, &i);
        if (cp == '\n' || cp == '\r') break;
        const Glyph* g = nullptr;
        Font* owner = font.Resolve(cp, &g);
        if (!g || !owner) {
            prev = cp;
            continue;
        }
        f32 kern = KernPx(font, prev, cp, emSize);
        pen += kern;
        if (!g->isEmpty() && g->u1 > g->u0 && g->page >= 0 &&
            g->page < owner->AtlasPageCount()) {
            GlyphPlacement gp;
            gp.dst = Rect{pen + g->bearingX * scale, baselineY - g->bearingY * scale,
                          g->width * scale, g->height * scale};
            gp.uv = Rect{g->u0, g->v0, g->u1 - g->u0, g->v1 - g->v0};
            gp.page = g->page;
            out->push_back(gp);
        }
        pen += g->advance * scale + letterSpacing;
        prev = cp;
    }
    if (advanceOut) *advanceOut = pen - penX;
}
}  // namespace

void Renderer2D::DrawText(const Font& font, const std::string& utf8, f32 x, f32 y,
                          const Color& color, f32 size, TextAlign align, TextBaseline baseline,
                          f32 letterSpacing) {
    if (!font.Valid() || utf8.empty()) return;
    f32 s = size > 0 ? size : font.Desc().pixelHeight;
    f32 scale = font.ScaleForSize(s);
    std::vector<GlyphPlacement> glyphs;
    f32 width = 0;
    f32 baselineY = y;
    switch (baseline) {
        case TextBaseline::Top: baselineY = y + font.Ascender() * scale; break;
        case TextBaseline::Middle: baselineY = y + (font.Ascender() + font.Descender()) * scale * 0.5f; break;
        case TextBaseline::Bottom: baselineY = y - font.Descender() * scale; break;
        case TextBaseline::Alphabetic: baselineY = y; break;
    }
    f32 startX = x;
    LayoutLine(font, utf8, s, letterSpacing, 0.0f, baselineY, &glyphs, &width);
    switch (align) {
        case TextAlign::Center: startX = x - width * 0.5f; break;
        case TextAlign::Right: startX = x - width; break;
        default: break;
    }
    if (startX != 0.0f) {
        for (GlyphPlacement& g : glyphs) g.dst.x += startX;
    }

    auto& im = *impl_;
    for (const GlyphPlacement& g : glyphs) {
        const FontAtlasPage& page = font.AtlasPage(g.page);
        const Texture& tex = page.texture;
        if (!tex.Valid()) continue;
        Vec2 p00 = im.Xf({g.dst.x, g.dst.y});
        Vec2 p10 = im.Xf({g.dst.x + g.dst.w, g.dst.y});
        Vec2 p11 = im.Xf({g.dst.x + g.dst.w, g.dst.y + g.dst.h});
        Vec2 p01 = im.Xf({g.dst.x, g.dst.y + g.dst.h});
        Vec2 uv0{g.uv.x, g.uv.y};
        Vec2 uv1{g.uv.x + g.uv.w, g.uv.y + g.uv.h};
        Color c = im.WithAlpha(color);

        if (font.IsSdf()) {
            CallKey key;
            key.texture = tex.Id();
            key.paintType = 3;
            key.blend = im.state.blend;
            key.scissor = im.state.scissor;
            key.program = Program::SdfText;
            im.EnsureCall(key, Paint::Solid(color));
            if (!im.calls.empty()) {
                DrawCall& dc = im.calls.back();
                dc.sdfSpread = tex.SdfSpread() > 0 ? tex.SdfSpread() : font.Desc().sdfSpread;
                dc.sdfPixelRange = scale;
                dc.sdfOutlineWidth = 0.0f;
            }
            u32 base = im.VertexBase();
            im.PushVertex(p00, uv0, c);
            im.PushVertex(p10, {uv1.x, uv0.y}, c);
            im.PushVertex(p11, uv1, c);
            im.PushVertex(p01, {uv0.x, uv1.y}, c);
            im.PushTriangle(base, base + 1, base + 2);
            im.PushTriangle(base, base + 2, base + 3);
        } else {
            Paint paint = Paint::Image(tex, color);
            CallKey key = im.KeyFor(paint, Program::Sprite);
            im.EnsureCall(key, paint);
            u32 base = im.VertexBase();
            im.PushVertex(p00, uv0, c);
            im.PushVertex(p10, {uv1.x, uv0.y}, c);
            im.PushVertex(p11, uv1, c);
            im.PushVertex(p01, {uv0.x, uv1.y}, c);
            im.PushTriangle(base, base + 1, base + 2);
            im.PushTriangle(base, base + 2, base + 3);
        }
        ++stats_.textGlyphs;
    }
    im.EndPrimitive();
}

void Renderer2D::DrawTextRotated(const Font& font, const std::string& utf8, Vec2 pos,
                                 const Color& color, f32 size, f32 rotation, TextAlign align,
                                 TextBaseline baseline) {
    Save();
    Translate(pos.x, pos.y);
    Rotate(rotation);
    DrawText(font, utf8, 0, 0, color, size, align, baseline);
    Restore();
}

int Renderer2D::DrawTextBox(const Font& font, const std::string& utf8, const crossrender::Rect& box,
                            const Color& color, f32 size, TextBreak brk, f32 lineHeightMul,
                            TextAlign align, int maxLines) {
    if (!font.Valid() || utf8.empty()) return 0;
    f32 s = size > 0 ? size : font.Desc().pixelHeight;
    f32 scale = font.ScaleForSize(s);
    std::vector<std::string> lines = WrapText(font, utf8, box.w, s, brk);
    f32 lineHeight = font.LineHeight() * scale * lineHeightMul;
    f32 y = box.y;
    int drawn = 0;
    for (const std::string& line : lines) {
        if (maxLines > 0 && drawn >= maxLines) break;
        if (y > box.y + box.h) break;
        f32 lx = box.x;
        if (align == TextAlign::Center)
            lx = box.x + box.w * 0.5f;
        else if (align == TextAlign::Right)
            lx = box.x + box.w;
        DrawText(font, line, lx, y, color, s, align, TextBaseline::Top);
        y += lineHeight;
        ++drawn;
    }
    return drawn;
}

void Renderer2D::DrawTextOutline(const Font& font, const std::string& utf8, f32 x, f32 y,
                                 const Color& fill, const Color& outline, f32 outlineWidth, f32 size,
                                 TextAlign align, TextBaseline baseline) {
    if (!font.Valid()) return;
    if (font.IsSdf()) {
        f32 s = size > 0 ? size : font.Desc().pixelHeight;
        f32 scale = font.ScaleForSize(s);
        std::vector<GlyphPlacement> glyphs;
        f32 width = 0;
        f32 baselineY = y;
        switch (baseline) {
            case TextBaseline::Top: baselineY = y + font.Ascender() * scale; break;
            case TextBaseline::Middle:
                baselineY = y + (font.Ascender() + font.Descender()) * scale * 0.5f;
                break;
            case TextBaseline::Bottom: baselineY = y - font.Descender() * scale; break;
            default: break;
        }
        LayoutLine(font, utf8, s, 0.0f, 0.0f, baselineY, &glyphs, &width);
        f32 startX = x;
        if (align == TextAlign::Center) startX = x - width * 0.5f;
        else if (align == TextAlign::Right) startX = x - width;
        auto& im = *impl_;
        for (const GlyphPlacement& g : glyphs) {
            const Texture& tex = font.AtlasPage(g.page).texture;
            if (!tex.Valid()) continue;
            f32 gx = g.dst.x + startX;
            Vec2 p00 = im.Xf({gx, g.dst.y});
            Vec2 p10 = im.Xf({gx + g.dst.w, g.dst.y});
            Vec2 p11 = im.Xf({gx + g.dst.w, g.dst.y + g.dst.h});
            Vec2 p01 = im.Xf({gx, g.dst.y + g.dst.h});
            CallKey key;
            key.texture = tex.Id();
            key.paintType = 3;
            key.blend = im.state.blend;
            key.scissor = im.state.scissor;
            key.program = Program::SdfText;
            im.EnsureCall(key, Paint::Solid(fill));
            if (!im.calls.empty()) {
                DrawCall& dc = im.calls.back();
                dc.sdfSpread = tex.SdfSpread() > 0 ? tex.SdfSpread() : font.Desc().sdfSpread;
                dc.sdfPixelRange = scale;
                dc.sdfOutlineWidth = outlineWidth / MaxT(scale, 1e-3f);
                dc.sdfOutlineColor = outline;
            }
            u32 base = im.VertexBase();
            Color cf = im.WithAlpha(fill);
            im.PushVertex(p00, {g.uv.x, g.uv.y}, cf);
            im.PushVertex(p10, {g.uv.x + g.uv.w, g.uv.y}, cf);
            im.PushVertex(p11, {g.uv.x + g.uv.w, g.uv.y + g.uv.h}, cf);
            im.PushVertex(p01, {g.uv.x, g.uv.y + g.uv.h}, cf);
            im.PushTriangle(base, base + 1, base + 2);
            im.PushTriangle(base, base + 2, base + 3);
        }
        im.EndPrimitive();
    } else {
        // Битовый шрифт: имитируем обводку восемью смещёнными копиями.
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) continue;
                DrawText(font, utf8, x + dx * outlineWidth, y + dy * outlineWidth, outline, size,
                         align, baseline);
            }
        }
        DrawText(font, utf8, x, y, fill, size, align, baseline);
    }
}

// ---------------------------------------------------------------------------
// Статистика / разное
// ---------------------------------------------------------------------------
int Renderer2D::PendingVertices() const { return static_cast<int>(impl_->vertices.size()); }
const Mat4& Renderer2D::Projection() const { return projection_; }
f32 Renderer2D::DpiScale() const { return impl_->dpi; }
Vec2 Renderer2D::ScreenSize() const { return impl_->screen.Size(); }

void Renderer2D::SetViewportBlit(const Texture& colorTex, const crossrender::Rect& dst) {
    impl_->viewportBlitTex = &colorTex;
    impl_->viewportBlitDst = dst;
    Image(colorTex, dst, crossrender::Rect{0, 0, 1, 1}, Color::White);
}

// ---------------------------------------------------------------------------
// Богато стилизованный текст
// ---------------------------------------------------------------------------
namespace {

// Разложенный глиф в локальных координатах строки: базовая линия y = 0, а x
// растёт от начала пера строки.
struct StyledGlyph {
    Rect dst;
    Rect uv;
    int page = 0;
};

void LayoutStyledLine(const Font& font, const std::string& text, f32 emSize, f32 letterSpacing,
                      f32 penX, std::vector<StyledGlyph>* out, f32* widthOut) {
    const f32 scale = font.ScaleForSize(emSize);
    f32 pen = penX;
    u32 prev = 0;
    usize i = 0;
    while (i < text.size()) {
        u32 cp = Utf8Decode(text.c_str(), text.size(), &i);
        if (cp == '\n' || cp == '\r') break;
        const Glyph* g = nullptr;
        Font* owner = font.Resolve(cp, &g);
        if (!g || !owner) {
            prev = cp;
            continue;
        }
        pen += KernPx(font, prev, cp, emSize);
        if (!g->isEmpty() && g->u1 > g->u0 && g->page >= 0 && g->page < owner->AtlasPageCount()) {
            StyledGlyph sg;
            sg.dst = Rect{pen + g->bearingX * scale, -g->bearingY * scale, g->width * scale,
                          g->height * scale};
            sg.uv = Rect{g->u0, g->v0, g->u1 - g->u0, g->v1 - g->v0};
            sg.page = g->page;
            out->push_back(sg);
        }
        pen += g->advance * scale + letterSpacing;
        prev = cp;
    }
    if (widthOut) *widthOut = pen - penX;
}

std::vector<std::string> SplitLines(const std::string& utf8) {
    std::vector<std::string> lines;
    std::string current;
    for (char ch : utf8) {
        if (ch == '\n') {
            lines.push_back(current);
            current.clear();
        } else if (ch != '\r') {
            current.push_back(ch);
        }
    }
    lines.push_back(current);
    return lines;
}

// Цвет заливки в нормированной позиции глифа (0,0 = левый верх знакового бокса).
Color GradientColorAt(const TextStyle& st, Vec2 t) {
    if (!st.gradient) return st.color;
    if (st.gradientRadial) {
        Vec2 d = t - st.gradientCenter;
        f32 r = Clamp(std::sqrt(d.x * d.x + d.y * d.y) / 0.7071f, 0.0f, 1.0f);
        return Lerp(st.innerColor, st.outerColor, r);
    }
    return Lerp(st.innerColor, st.outerColor, Clamp(t.y, 0.0f, 1.0f));
}

inline Color AlphaScaled(Color c, f32 a) {
    c.a *= a;
    return c;
}

}  // namespace

TextStyle TextStyle::Filled(const Color& c) {
    TextStyle s;
    s.color = c;
    return s;
}

TextStyle TextStyle::Outlined(const Color& fill, const Color& outline, f32 width) {
    TextStyle s;
    s.color = fill;
    s.outlineColor = outline;
    s.outlineWidth = width;
    return s;
}

TextStyle TextStyle::Shadowed(const Color& fill, const Vec2& offset) {
    TextStyle s;
    s.color = fill;
    s.shadowColor = Color{0, 0, 0, 0.65f};
    s.shadowOffset = offset;
    return s;
}

TextStyle TextStyle::GradientText(const Color& inner, const Color& outer) {
    TextStyle s;
    s.gradient = true;
    s.innerColor = inner;
    s.outerColor = outer;
    s.color = inner;
    return s;
}

// ---------------------------------------------------------------------------
// Один глиф, полный стек стилей
// ---------------------------------------------------------------------------
void Renderer2D::DrawStyledGlyph(const Font& font, int page, const crossrender::Rect& localDst,
                                 const crossrender::Rect& uv, const TextStyle& style, crossrender::Vec2 runOrigin,
                                 f32 rotation, f32 emScale) {
    if (page < 0 || page >= font.AtlasPageCount()) return;
    const Texture& tex = font.AtlasPage(page).texture;
    if (!tex.Valid()) return;
    const bool sdf = font.IsSdf();

    const bool hasOutline = style.outlineWidth > 0.01f && style.outlineColor.a > 0.003f;
    const bool hasShadow = style.shadowColor.a > 0.003f &&
                           (style.shadowOffset.x != 0.0f || style.shadowOffset.y != 0.0f ||
                            style.shadowSoftness > 0.0f);
    const bool hasGlow = style.glowRadius > 0.5f && style.glowColor.a > 0.003f;

    // Цвета заливки по углам (провершинный градиент не стоит ничего лишнего).
    const Color f00 = GradientColorAt(style, {0.0f, 0.0f});
    const Color f10 = GradientColorAt(style, {1.0f, 0.0f});
    const Color f11 = GradientColorAt(style, {1.0f, 1.0f});
    const Color f01 = GradientColorAt(style, {0.0f, 1.0f});

    // `outline` = ширина обводки SDF-шейдера для этого прохода (0 для копий
    // тени/свечения, чтобы они не наследовали обводку).
    auto emit = [&](const crossrender::Rect& local, Color c0, Color c1, Color c2, Color c3, f32 alpha,
                    f32 sdfOutline) {
        c0 = AlphaScaled(c0, alpha);
        c1 = AlphaScaled(c1, alpha);
        c2 = AlphaScaled(c2, alpha);
        c3 = AlphaScaled(c3, alpha);
        Save();
        Translate(runOrigin.x, runOrigin.y);
        Rotate(rotation);
        Scale(style.scale.x, style.scale.y);
        if (style.skew.x != 0.0f || style.skew.y != 0.0f)
            Transform(1.0f, style.skew.y, style.skew.x, 1.0f, 0.0f, 0.0f);
        if (sdf) {
            auto& im = *impl_;
            CallKey key;
            key.texture = tex.Id();
            key.paintType = 3;
            key.blend = im.state.blend;
            key.scissor = im.state.scissor;
            key.program = Program::SdfText;
            im.EnsureCall(key, Paint::Solid(c0));
            if (!im.calls.empty()) {
                DrawCall& dc = im.calls.back();
                dc.sdfSpread = tex.SdfSpread() > 0 ? tex.SdfSpread() : font.Desc().sdfSpread;
                const f32 px = MaxT(emScale * std::fabs(style.scale.x), 1e-3f);
                dc.sdfPixelRange = px;
                dc.sdfOutlineWidth = sdfOutline / px;
                dc.sdfOutlineColor = AlphaScaled(style.outlineColor, alpha);
            }
            Vec2 p00 = im.Xf({local.x, local.y});
            Vec2 p10 = im.Xf({local.Right(), local.y});
            Vec2 p11 = im.Xf({local.Right(), local.Bottom()});
            Vec2 p01 = im.Xf({local.x, local.Bottom()});
            u32 base = im.VertexBase();
            im.PushVertex(p00, {uv.x, uv.y}, c0);
            im.PushVertex(p10, {uv.Right(), uv.y}, c1);
            im.PushVertex(p11, {uv.Right(), uv.Bottom()}, c2);
            im.PushVertex(p01, {uv.x, uv.Bottom()}, c3);
            im.PushTriangle(base, base + 1, base + 2);
            im.PushTriangle(base, base + 2, base + 3);
            im.EndPrimitive();
        } else if (style.gradient) {
            ImageTinted4(tex, local, uv, c0, c1, c2, c3);
        } else {
            Image(tex, local, uv, c0);
        }
        Restore();
    };

    auto expanded = [&](f32 spread, Vec2 offset) {
        return crossrender::Rect{localDst.x - spread + offset.x, localDst.y - spread + offset.y,
                         localDst.w + spread * 2, localDst.h + spread * 2};
    };

    // 1. внешнее crossrender-свечение (несколько увеличенных, более тусклых копий позади всего).
    if (hasGlow) {
        const int rings = 4;
        for (int r = rings; r >= 1; --r) {
            const f32 spread = style.glowRadius * (static_cast<f32>(r) / rings);
            const Color gc = style.glowColor;
            const f32 a = style.glowIntensity / static_cast<f32>(r + 1);
            emit(expanded(spread, {0, 0}), gc, gc, gc, gc, a, 0.0f);
        }
    }
    // 2. отбрасываемая тень (одна жёсткая копия или кольцо мягких копий).
    if (hasShadow) {
        const int samples = style.shadowSoftness > 0.0f ? Clamp(style.shadowSamples, 1, 12) : 1;
        for (int s = 0; s < samples; ++s) {
            Vec2 off = style.shadowOffset;
            if (samples > 1) {
                const f32 a = kTau * static_cast<f32>(s) / static_cast<f32>(samples);
                off.x += std::cos(a) * style.shadowSoftness;
                off.y += std::sin(a) * style.shadowSoftness;
            }
            const Color sc = style.shadowColor;
            emit(expanded(0.0f, off), sc, sc, sc, sc, samples > 1 ? 0.55f : 1.0f, 0.0f);
        }
    }
    // 3. обводка. SDF делает её аналитически в проходе заливки; битовые шрифты
    //    получают кольцо смещённых копий.
    if (hasOutline && !sdf) {
        const int samples = static_cast<int>(Clamp(8.0f + style.outlineWidth * 2.0f, 8.0f, 20.0f));
        for (int s = 0; s < samples; ++s) {
            const f32 a = kTau * static_cast<f32>(s) / static_cast<f32>(samples);
            const Vec2 off{std::cos(a) * style.outlineWidth, std::sin(a) * style.outlineWidth};
            const Color oc = style.outlineColor;
            emit(expanded(0.0f, off), oc, oc, oc, oc, 1.0f, 0.0f);
        }
    }
    // 4. заливка.
    emit(localDst, f00, f10, f11, f01, 1.0f, hasOutline && sdf ? style.outlineWidth : 0.0f);
    ++stats_.textGlyphs;
}

Renderer2D::StyledMetrics Renderer2D::MeasureStyledText(const Font& font, const std::string& utf8,
                                                       f32 size, const TextStyle& style) {
    StyledMetrics m;
    if (!font.Valid() || utf8.empty()) return m;
    const f32 emSize = size > 0 ? size : font.Desc().pixelHeight;
    const f32 scale = font.ScaleForSize(emSize);
    std::vector<std::string> lines = SplitLines(utf8);
    m.lineCount = static_cast<int>(lines.size());
    f32 maxW = 0, maxTop = 0, maxBottom = 0;
    for (const std::string& line : lines) {
        std::vector<StyledGlyph> glyphs;
        f32 w = 0;
        LayoutStyledLine(font, line, emSize, style.letterSpacing, 0.0f, &glyphs, &w);
        maxW = MaxT(maxW, w);
        for (const StyledGlyph& g : glyphs) {
            maxTop = MaxT(maxTop, -g.dst.y);
            maxBottom = MaxT(maxBottom, g.dst.Bottom());
            ++m.glyphCount;
        }
    }
    const f32 lineH = font.LineHeight() * scale * style.lineHeightMul;
    const f32 sx = std::fabs(style.scale.x) > 0 ? std::fabs(style.scale.x) : 1.0f;
    const f32 sy = std::fabs(style.scale.y) > 0 ? std::fabs(style.scale.y) : 1.0f;
    m.advance = maxW;
    m.width = maxW * sx;
    m.height = (lineH * static_cast<f32>(MaxT(1, m.lineCount - 1)) + maxTop + maxBottom) * sy;
    m.bounds = crossrender::Rect{0, -maxTop * sy, m.width, m.height};
    return m;
}

void Renderer2D::DrawTextStyled(const Font& font, const std::string& utf8, Vec2 pos, f32 size,
                                const TextStyle& style) {
    if (!font.Valid() || utf8.empty()) return;
    const f32 emSize = size > 0 ? size : font.Desc().pixelHeight;
    const f32 scale = font.ScaleForSize(emSize);

    f32 baseline = 0.0f;
    switch (style.baseline) {
        case TextBaseline::Top: baseline = font.Ascender() * scale; break;
        case TextBaseline::Middle:
            baseline = (font.Ascender() + font.Descender()) * scale * 0.5f;
            break;
        case TextBaseline::Bottom: baseline = -font.Descender() * scale; break;
        case TextBaseline::Alphabetic: baseline = 0.0f; break;
    }
    const f32 lineH = font.LineHeight() * scale * style.lineHeightMul;

    std::vector<std::string> lines = SplitLines(utf8);
    if (style.maxLines > 0 && static_cast<int>(lines.size()) > style.maxLines)
        lines.resize(static_cast<usize>(style.maxLines));

    for (usize li = 0; li < lines.size(); ++li) {
        const std::string& line = lines[li];
        if (line.empty()) continue;
        std::vector<StyledGlyph> glyphs;
        f32 runWidth = 0;
        LayoutStyledLine(font, line, emSize, style.letterSpacing, 0.0f, &glyphs, &runWidth);
        if (glyphs.empty()) continue;

        f32 alignOffset = 0.0f;
        if (style.align == TextAlign::Center) alignOffset = -runWidth * 0.5f;
        else if (style.align == TextAlign::Right) alignOffset = -runWidth;
        const f32 lineY = baseline + static_cast<f32>(li) * lineH;

        for (const StyledGlyph& g : glyphs) {
            const f32 penX = g.dst.x + alignOffset;
            const f32 rotation = style.rotation + style.twist * penX;
            const f32 waveY = style.waveAmplitude != 0.0f
                                  ? std::sin(penX * style.waveFrequency) * style.waveAmplitude
                                  : 0.0f;
            const crossrender::Rect local{g.dst.x + alignOffset, g.dst.y + lineY + waveY, g.dst.w,
                                  g.dst.h};
            DrawStyledGlyph(font, g.page, local, g.uv, style, pos, rotation, scale);
        }
    }
}

int Renderer2D::DrawTextBoxStyled(const Font& font, const std::string& utf8, const crossrender::Rect& box,
                                  f32 size, const TextStyle& style) {
    if (!font.Valid() || utf8.empty()) return 0;
    const f32 emSize = size > 0 ? size : font.Desc().pixelHeight;
    const f32 scale = font.ScaleForSize(emSize);
    const TextBreak brk = style.wrap == TextBreak::None ? TextBreak::Word : style.wrap;
    std::vector<std::string> lines = WrapText(font, utf8, box.w, emSize, brk);
    const f32 lineH = font.LineHeight() * scale * style.lineHeightMul;
    int drawn = 0;
    const int limit = style.maxLines > 0 ? style.maxLines : static_cast<int>(lines.size());
    f32 y = box.y;
    for (const std::string& line : lines) {
        if (drawn >= limit) break;
        if (y > box.Bottom() + lineH) break;
        TextStyle lineStyle = style;
        lineStyle.wrap = TextBreak::None;
        lineStyle.maxLines = 1;
        f32 x = box.x;
        if (style.align == TextAlign::Center) x = box.x + box.w * 0.5f;
        else if (style.align == TextAlign::Right) x = box.Right();
        DrawTextStyled(font, line, Vec2{x, y}, emSize, lineStyle);
        y += lineH;
        ++drawn;
    }
    return drawn;
}

namespace {

// Идёт по UTF-8 строке и вызывает `place(glyph, penX)` для каждого видимого
// глифа. Перо сдвигается для *каждой* кодовой точки, включая пробелы, так что
// пробелы на пути или дуге сохраняют ширину.
template <typename F>
void ForEachStyledGlyph(const Font& font, const std::string& utf8, f32 emSize, f32 letterSpacing,
                        F&& place) {
    const f32 scale = font.ScaleForSize(emSize);
    std::vector<StyledGlyph> glyphs;
    LayoutStyledLine(font, utf8, emSize, letterSpacing, 0.0f, &glyphs, nullptr);
    f32 pen = 0;
    u32 prev = 0;
    usize i = 0;
    size_t gi = 0;
    while (i < utf8.size()) {
        u32 cp = Utf8Decode(utf8.c_str(), utf8.size(), &i);
        const Glyph* g = nullptr;
        Font* owner = font.Resolve(cp, &g);
        if (!g || !owner) {
            prev = cp;
            continue;
        }
        pen += KernPx(font, prev, cp, emSize);
        if (!g->isEmpty() && g->u1 > g->u0 && gi < glyphs.size()) {
            place(glyphs[gi], pen);
            ++gi;
        }
        pen += g->advance * scale + letterSpacing;
        prev = cp;
    }
}

}  // namespace

void Renderer2D::DrawTextOnPath(const Font& font, const std::string& utf8, const Vec2* path,
                                int pathCount, f32 size, const TextStyle& style, f32 offset,
                                bool closed) {
    if (!font.Valid() || utf8.empty() || !path || pathCount < 2) return;
    const f32 emSize = size > 0 ? size : font.Desc().pixelHeight;
    const f32 scale = font.ScaleForSize(emSize);

    std::vector<f32> segLen(static_cast<usize>(pathCount), 0.0f);
    f32 total = 0;
    for (int i = 1; i < pathCount; ++i) {
        segLen[static_cast<usize>(i)] = Length(path[i] - path[i - 1]);
        total += segLen[static_cast<usize>(i)];
    }
    if (closed && pathCount > 2) {
        segLen[0] = Length(path[0] - path[pathCount - 1]);
        total += segLen[0];
    }
    if (total <= 1e-4f) return;

    auto sampleAt = [&](f32 distance, Vec2* point, Vec2* tangent) {
        f32 d = std::fmod(distance, total);
        if (d < 0) d += total;
        f32 acc = 0;
        for (int i = 1; i < pathCount; ++i) {
            const f32 len = segLen[static_cast<usize>(i)];
            if (len <= 1e-6f) continue;
            if (acc + len >= d) {
                const f32 t = (d - acc) / len;
                *point = Lerp(path[i - 1], path[i], t);
                *tangent = Normalize(path[i] - path[i - 1]);
                return;
            }
            acc += len;
        }
        if (closed && segLen[0] > 1e-6f && acc + segLen[0] >= d) {
            const f32 t = (d - acc) / segLen[0];
            *point = Lerp(path[pathCount - 1], path[0], t);
            *tangent = Normalize(path[0] - path[pathCount - 1]);
            return;
        }
        *point = path[pathCount - 1];
        *tangent = Normalize(path[pathCount - 1] - path[pathCount - 2]);
    };

    TextStyle local = style;
    local.align = TextAlign::Left;
    local.baseline = TextBaseline::Alphabetic;
    const f32 savedRotation = style.rotation;
    ForEachStyledGlyph(font, utf8, emSize, style.letterSpacing,
                       [&](const StyledGlyph& g, f32 penX) {
                           Vec2 p, tangent;
                           sampleAt(penX + offset, &p, &tangent);
                           const f32 ang = std::atan2(tangent.y, tangent.x);
                           TextStyle placed = local;
                           placed.rotation = savedRotation + ang;
                           placed.twist = 0.0f;
                           placed.waveAmplitude = 0.0f;
                           const crossrender::Rect glyphRect{0.0f, g.dst.y, g.dst.w, g.dst.h};
                           DrawStyledGlyph(font, g.page, glyphRect, g.uv, placed, p, 0.0f, scale);
                       });
}

void Renderer2D::DrawTextOnArc(const Font& font, const std::string& utf8, Vec2 center, f32 radius,
                               f32 startAngle, f32 size, const TextStyle& style, bool outside,
                               bool clockwise) {
    if (!font.Valid() || utf8.empty() || radius <= 0.5f) return;
    const f32 emSize = size > 0 ? size : font.Desc().pixelHeight;
    const f32 scale = font.ScaleForSize(emSize);
    const f32 dir = clockwise ? -1.0f : 1.0f;

    // Центрируем строку на дуге (сначала измеряем).
    std::vector<StyledGlyph> measure;
    f32 runWidth = 0;
    LayoutStyledLine(font, utf8, emSize, style.letterSpacing, 0.0f, &measure, &runWidth);

    const f32 centering = -runWidth * 0.5f;
    ForEachStyledGlyph(font, utf8, emSize, style.letterSpacing,
                       [&](const StyledGlyph& g, f32 penX) {
                           const f32 angle = startAngle + dir * ((penX + centering) / radius);
                           const Vec2 p{center.x + std::cos(angle) * radius,
                                        center.y + std::sin(angle) * radius};
                           // Касательная вдоль дуги; по умолчанию глифы сидят
                           // снаружи и переворачиваются, когда оказываются внутри.
                           const f32 tangent = angle + (clockwise ? -kPi * 0.5f : kPi * 0.5f);
                           const f32 rot = outside ? tangent : tangent + kPi;
                           const crossrender::Rect glyphRect{0.0f, g.dst.y, g.dst.w, g.dst.h};
                           DrawStyledGlyph(font, g.page, glyphRect, g.uv, style, p, rot, scale);
                       });
}

void Renderer2D::DrawTextShadow(const Font& font, const std::string& utf8, Vec2 pos, f32 size,
                                const Color& fill, const Color& shadow, Vec2 offset, TextAlign align,
                                TextBaseline baseline) {
    TextStyle st;
    st.color = fill;
    st.shadowColor = shadow;
    st.shadowOffset = offset;
    st.align = align;
    st.baseline = baseline;
    DrawTextStyled(font, utf8, pos, size, st);
}

void Renderer2D::DrawTextTwisted(const Font& font, const std::string& utf8, Vec2 pos, f32 size,
                                 const Color& fill, f32 twist, f32 rotation, f32 waveAmplitude) {
    TextStyle st;
    st.color = fill;
    st.twist = twist;
    st.rotation = rotation;
    st.waveAmplitude = waveAmplitude;
    st.waveFrequency = 0.04f;
    DrawTextStyled(font, utf8, pos, size, st);
}

void Renderer2D::DrawTextGradient(const Font& font, const std::string& utf8, Vec2 pos, f32 size,
                                  const Color& inner, const Color& outer, TextAlign align,
                                  TextBaseline baseline) {
    TextStyle st = TextStyle::GradientText(inner, outer);
    st.align = align;
    st.baseline = baseline;
    DrawTextStyled(font, utf8, pos, size, st);
}

// ---------------------------------------------------------------------------
// Утилиты измерения текста
// ---------------------------------------------------------------------------
TextMetrics MeasureText(const Font& font, const std::string& utf8, f32 size, f32 letterSpacing) {
    TextMetrics m;
    if (!font.Valid()) return m;
    f32 s = size > 0 ? size : font.Desc().pixelHeight;
    f32 scale = font.ScaleForSize(s);
    m.ascender = font.Ascender() * scale;
    m.descender = font.Descender() * scale;
    m.height = font.LineHeight() * scale;
    f32 pen = 0;
    f32 maxWidth = 0;
    u32 prev = 0;
    usize i = 0;
    usize lines = 1;
    while (i < utf8.size()) {
        u32 cp = Utf8Decode(utf8.c_str(), utf8.size(), &i);
        if (cp == '\n') {
            maxWidth = MaxT(maxWidth, pen);
            pen = 0;
            prev = 0;
            ++lines;
            continue;
        }
        const Glyph* g = nullptr;
        Font* owner = font.Resolve(cp, &g);
        if (!g || !owner) continue;
        pen += KernPx(font, prev, cp, s);
        pen += g->advance * scale + letterSpacing;
        ++m.glyphCount;
        prev = cp;
    }
    maxWidth = MaxT(maxWidth, pen);
    m.width = maxWidth;
    m.lineCount = static_cast<int>(lines);
    return m;
}

std::vector<std::string> WrapText(const Font& font, const std::string& utf8, f32 maxWidth, f32 size,
                                  TextBreak brk) {
    std::vector<std::string> lines;
    if (utf8.empty()) return lines;
    f32 s = size > 0 ? size : font.Desc().pixelHeight;
    auto widthOf = [&](const std::string& t) { return MeasureText(font, t, s).width; };

    // Сначала делим на абзацы.
    std::vector<std::string> paragraphs;
    std::string current;
    for (char ch : utf8) {
        if (ch == '\n') {
            paragraphs.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    paragraphs.push_back(current);

    for (const std::string& para : paragraphs) {
        if (para.empty()) {
            lines.push_back("");
            continue;
        }
        if (brk == TextBreak::None) {
            lines.push_back(para);
            continue;
        }
        if (brk == TextBreak::Char || widthOf(para) <= maxWidth) {
            std::string line;
            usize i = 0;
            while (i < para.size()) {
                usize start = i;
                u32 cp = Utf8Decode(para.c_str(), para.size(), &i);
                (void)cp;
                std::string candidate = line + para.substr(start, i - start);
                if (widthOf(candidate) > maxWidth && !line.empty()) {
                    lines.push_back(line);
                    line = para.substr(start, i - start);
                } else {
                    line = candidate;
                }
            }
            if (!line.empty()) lines.push_back(line);
            continue;
        }
        // Перенос по словам.
        std::string line;
        std::string word;
        auto flushWord = [&]() {
            if (word.empty()) return;
            std::string candidate = line.empty() ? word : line + word;
            if (widthOf(candidate) > maxWidth && !line.empty()) {
                lines.push_back(line);
                line = word;
            } else {
                line = candidate;
            }
            word.clear();
        };
        usize i = 0;
        while (i < para.size()) {
            usize start = i;
            u32 cp = Utf8Decode(para.c_str(), para.size(), &i);
            std::string chunk = para.substr(start, i - start);
            if (cp == ' ' || cp == '\t') {
                flushWord();
                // Пробел остаётся на текущей строке.
                line += chunk;
                if (widthOf(line) > maxWidth && line.size() > 1) {
                    line.pop_back();
                    lines.push_back(line);
                    line.clear();
                }
            } else {
                word += chunk;
            }
        }
        flushWord();
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

std::string EllipsizeText(const Font& font, const std::string& utf8, f32 maxWidth, f32 size) {
    if (MeasureText(font, utf8, size).width <= maxWidth) return utf8;
    const std::string ellipsis = "...";
    std::string out;
    usize i = 0;
    while (i < utf8.size()) {
        usize start = i;
        Utf8Decode(utf8.c_str(), utf8.size(), &i);
        std::string candidate = utf8.substr(0, i) + ellipsis;
        if (MeasureText(font, candidate, size).width > maxWidth) {
            return utf8.substr(0, start) + ellipsis;
        }
        out = candidate;
    }
    return out;
}

usize TextIndexAt(const Font& font, const std::string& utf8, f32 x, f32 size) {
    f32 s = size > 0 ? size : font.Desc().pixelHeight;
    f32 scale = font.ScaleForSize(s);
    f32 pen = 0;
    usize i = 0;
    u32 prev = 0;
    while (i < utf8.size()) {
        usize start = i;
        u32 cp = Utf8Decode(utf8.c_str(), utf8.size(), &i);
        const Glyph* g = nullptr;
        Font* owner = font.Resolve(cp, &g);
        if (!g || !owner) continue;
        f32 kern = KernPx(font, prev, cp, s);
        f32 adv = kern + g->advance * scale;
        if (x < pen + adv * 0.5f) return start;
        pen += adv;
        prev = cp;
    }
    return utf8.size();
}

}  // namespace crossrender
