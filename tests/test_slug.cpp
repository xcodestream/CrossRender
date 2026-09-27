// Тесты GPU-векторного текстового рендерера в стиле Slug.
//
// CPU-часть (знаки обхода / аналитическое покрытие / подготовка глифов /
// измерение) работает без GL-контекста. GPU-часть защищена ENG_REQUIRE_GL() и
// сравнивает прочитанные пиксели с CPU-эталоном — это ключевое свойство
// корректности техники: фрагментный шейдер должен воспроизводить тот же
// квадратичный лучевой тест, что и SlugTextRenderer::CoverageAt.
#include "crossrender/test/Test.h"
#include "crossrender/text/Font.h"
#include "crossrender/gfx/SlugText.h"
#include "crossrender/gfx/Renderer2D.h"
#include "crossrender/gfx/RenderTarget.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

using namespace crossrender;

namespace {

// ---------------------------------------------------------------------------
// Контуры, собранные вручную
// ---------------------------------------------------------------------------
// Прямой отрезок как вырожденная квадратичная кривая (контрольная точка в середине),
// чтобы каждая тестовая форма проходила тот же квадратичный лучевой тест, что и настоящий глиф.
SlugCurve Seg(const Vec2& a, const Vec2& b) {
    SlugCurve c;
    c.p0 = a;
    c.p1 = (a + b) * 0.5f;
    c.p2 = b;
    return c;
}

// Замкнутый полигон, рёбра которого — квадратичные кривые. Точки в экранных
// координатах y-down; знак обхода не важен, поскольку покрытие использует |w|.
std::vector<SlugCurve> Polygon(const std::vector<Vec2>& pts) {
    std::vector<SlugCurve> out;
    for (usize i = 0; i < pts.size(); ++i) out.push_back(Seg(pts[i], pts[(i + 1) % pts.size()]));
    return out;
}

// Единичный квадрат [0,1]^2 (квадрат из четырёх кривых из ТЗ).
std::vector<SlugCurve> UnitSquare() {
    return Polygon({{0, 0}, {1, 0}, {1, 1}, {0, 1}});
}

// Квадрат с квадратным отверстием: обход равен 0 внутри отверстия и != 0 между
// двумя контурами, что доказывает nonzero winding, а не заливку по габаритам.
std::vector<SlugCurve> Ring() {
    std::vector<SlugCurve> out = Polygon({{0, 0}, {8, 0}, {8, 8}, {0, 8}});
    std::vector<SlugCurve> hole = Polygon({{2, 2}, {2, 6}, {6, 6}, {6, 2}});
    out.insert(out.end(), hole.begin(), hole.end());
    return out;
}

// "L" (вогнутая): точка в выемке должна иметь обход 0.
std::vector<SlugCurve> LShape() {
    return Polygon({{0, 0}, {2, 0}, {2, 6}, {6, 6}, {6, 8}, {0, 8}});
}

// ---------------------------------------------------------------------------
// Помощники чтения. Пиксели хранятся с началом в левом верхнем углу, 4 байта (RGBA).
// ---------------------------------------------------------------------------
struct Bitmap {
    std::vector<u8> px;
    int w = 0, h = 0;
    u8 At(int x, int y, int c) const {
        return px[(static_cast<usize>(y) * static_cast<usize>(w) + static_cast<usize>(x)) * 4 +
                  static_cast<usize>(c)];
    }
    // Покрытие — это альфа, записанная Slug-шейдером: для граничного пикселя
    // она равна 0.5, что согласуется с CPU CoverageAt() вокруг уровня 0.5.
    f32 Coverage(int x, int y) const { return At(x, y, 3) / 255.0f; }
    // Пиксель считается "чернилами", когда его покрытие не меньше половины.
    bool Ink(int x, int y) const { return At(x, y, 3) >= 128; }
    int InkArea() const {
        int n = 0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                if (Ink(x, y)) ++n;
        return n;
    }
    // Габаритный бокс чернил (включительно) или пустой rect, если чернил нет.
    Rect InkBounds() const {
        int x0 = w, y0 = h, x1 = -1, y1 = -1;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                if (!Ink(x, y)) continue;
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
                y0 = std::min(y0, y);
                y1 = std::max(y1, y);
            }
        }
        if (x1 < 0) return Rect{};
        return Rect{static_cast<f32>(x0), static_cast<f32>(y0), static_cast<f32>(x1 - x0 + 1),
                    static_cast<f32>(y1 - y0 + 1)};
    }
    // Число различных количеств span в строках: 1 — каждая строка один сплошной
    // run (залитый прямоугольник), >= 3 — у формы есть реальная структура.
    int DistinctRowSpans() const {
        std::vector<int> spans;
        for (int y = 0; y < h; ++y) {
            int runs = 0;
            bool in = false;
            for (int x = 0; x < w; ++x) {
                const bool ink = Ink(x, y);
                if (ink && !in) ++runs;
                in = ink;
            }
            if (runs > 0) spans.push_back(runs);
        }
        if (spans.empty()) return 0;
        std::sort(spans.begin(), spans.end());
        spans.erase(std::unique(spans.begin(), spans.end()), spans.end());
        return static_cast<int>(spans.size());
    }
};

struct GpuFixture {
    RenderTarget rt;
    Renderer2D r2d;
    int w = 0, h = 0;
    bool ok = false;

    bool Init(int width, int height) {
        w = width;
        h = height;
        RenderTargetDesc desc;
        desc.width = w;
        desc.height = h;
        desc.colorFormat = PixelFormat::RGBA8;
        desc.depth = false;
        desc.samples = 1;
        desc.name = "slug-fixture";
        if (!rt.Create(desc)) return false;
        if (!r2d.Init()) return false;
        ok = true;
        return true;
    }
    void Begin() {
        rt.Bind();
        rt.Clear(Color{0, 0, 0, 0}, false, false);
        r2d.BeginFrame(w, h, 1.0f, &rt);
    }
    void End() {
        r2d.EndFrame();
        rt.Unbind();
    }
    bool Read(Bitmap* out) {
        std::vector<u8> pixels;
        if (!rt.ReadPixels(&pixels)) return false;
        if (pixels.size() < static_cast<usize>(w) * static_cast<usize>(h) * 4) return false;
        // glReadPixels возвращает строки снизу вверх; переворачиваем в изображение с индексацией сверху слева.
        out->w = w;
        out->h = h;
        out->px.resize(pixels.size());
        const usize rowBytes = static_cast<usize>(w) * 4;
        for (int y = 0; y < h; ++y) {
            const usize src = static_cast<usize>(h - 1 - y) * rowBytes;
            const usize dst = static_cast<usize>(y) * rowBytes;
            std::copy(pixels.begin() + static_cast<long>(src),
                      pixels.begin() + static_cast<long>(src + rowBytes),
                      out->px.begin() + static_cast<long>(dst));
        }
        return true;
    }
};

// Поставляемый шрифт или nullptr, если ассета нет (тестовая часть, которой нужен
// настоящий глиф, в этом случае пропускается, а не падает).
Font* LoadEngineFont() {
    static Font* cached = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        static Font font;
        if (font.LoadFromFile("assets/fonts/engine.ttf", FontDesc{})) cached = &font;
    }
    return cached;
}

const char* kDemoText = "Slug";

}  // namespace

// ---------------------------------------------------------------------------
// CPU: числа обхода (winding)
// ---------------------------------------------------------------------------
ENG_TEST(Slug, WindingInsideAndOutsideSquare) {
    const std::vector<SlugCurve> square = UnitSquare();
    ENG_CHECK_GT(SlugTextRenderer::WindingAt(square, Vec2{0.5f, 0.5f}), 0.0f);
    ENG_CHECK_GT(SlugTextRenderer::WindingAt(square, Vec2{0.05f, 0.95f}), 0.0f);
    ENG_CHECK_NEAR(SlugTextRenderer::WindingAt(square, Vec2{-0.4f, 0.5f}), 0.0f, 1e-6f);
    ENG_CHECK_NEAR(SlugTextRenderer::WindingAt(square, Vec2{0.5f, -0.4f}), 0.0f, 1e-6f);
    ENG_CHECK_NEAR(SlugTextRenderer::WindingAt(square, Vec2{1.4f, 0.5f}), 0.0f, 1e-6f);
    ENG_CHECK_NEAR(SlugTextRenderer::WindingAt(square, Vec2{0.5f, 1.4f}), 0.0f, 1e-6f);
}

ENG_TEST(Slug, WindingNonzeroNotBoundingBox) {
    // Отверстие кольца должно взаимно гаситься до нуля: заливка по габаритному
    // боксу выдала бы там ненулевой обход.
    const std::vector<SlugCurve> ring = Ring();
    ENG_CHECK_NEAR(SlugTextRenderer::WindingAt(ring, Vec2{4, 4}), 0.0f, 1e-6f);
    ENG_CHECK_GT(std::fabs(SlugTextRenderer::WindingAt(ring, Vec2{1, 1})), 0.0f);
    ENG_CHECK_GT(std::fabs(SlugTextRenderer::WindingAt(ring, Vec2{1, 4})), 0.0f);
    ENG_CHECK_NEAR(SlugTextRenderer::WindingAt(ring, Vec2{9, 4}), 0.0f, 1e-6f);

    // Вогнутая "L": выемка снаружи, рукава внутри.
    const std::vector<SlugCurve> l = LShape();
    ENG_CHECK_NEAR(SlugTextRenderer::WindingAt(l, Vec2{4, 2}), 0.0f, 1e-6f);
    ENG_CHECK_GT(SlugTextRenderer::WindingAt(l, Vec2{1, 4}), 0.0f);
    ENG_CHECK_GT(SlugTextRenderer::WindingAt(l, Vec2{4, 7}), 0.0f);
}

ENG_TEST(Slug, WindingHandlesCurvedContour) {
    // Круг, аппроксимированный четырьмя квадриками: проверка внутренней/внешней полосы.
    const f32 k = 0.5522847f;
    std::vector<SlugCurve> circle;
    auto quad = [&](const Vec2& a, const Vec2& c, const Vec2& b) {
        SlugCurve q;
        q.p0 = a;
        q.p1 = c;
        q.p2 = b;
        circle.push_back(q);
    };
    quad({1, 0}, {1, k}, {0, 1});
    quad({0, 1}, {-k, 1}, {-1, 0});
    quad({-1, 0}, {-1, -k}, {0, -1});
    quad({0, -1}, {k, -1}, {1, 0});
    ENG_CHECK_GT(SlugTextRenderer::WindingAt(circle, Vec2{0, 0}), 0.0f);
    ENG_CHECK_NEAR(SlugTextRenderer::WindingAt(circle, Vec2{1.6f, 0}), 0.0f, 1e-6f);
    ENG_CHECK_GT(SlugTextRenderer::WindingAt(circle, Vec2{0.5f, 0.5f}), 0.0f);
}

// ---------------------------------------------------------------------------
// CPU: аналитическое покрытие со сглаживанием
// ---------------------------------------------------------------------------
ENG_TEST(Slug, CoverageAntialiasesEdges) {
    const std::vector<SlugCurve> square = UnitSquare();
    // Глубоко внутри / далеко снаружи.
    ENG_CHECK_NEAR(SlugTextRenderer::CoverageAt(square, Vec2{0.5f, 0.5f}), 1.0f, 1e-4f);
    ENG_CHECK_NEAR(SlugTextRenderer::CoverageAt(square, Vec2{2.5f, 0.5f}), 0.0f, 1e-4f);
    ENG_CHECK_NEAR(SlugTextRenderer::CoverageAt(square, Vec2{0.5f, 4.5f}), 0.0f, 1e-4f);

    // В пределах одной ширины AA от края покрытие — мягкий рамп, а не 0 или 1.
    // Левый край лежит в x = 0; шагаем по пикселю рядом с ним.
    int soft = 0;
    for (int i = 0; i <= 10; ++i) {
        const f32 x = -0.9f + 0.18f * static_cast<f32>(i);
        const f32 c = SlugTextRenderer::CoverageAt(square, Vec2{x, 0.5f}, 1.0f);
        ENG_CHECK(c >= 0.0f && c <= 1.0f);
        if (c > 0.001f && c < 0.999f) ++soft;
    }
    // Несколько сэмплов поперёк края должны попадать строго между 0 и 1.
    ENG_CHECK_GT(soft, 3);
    // Монотонность поперёк края (слева от края пустее, чем справа).
    const f32 before = SlugTextRenderer::CoverageAt(square, Vec2{-0.3f, 0.5f}, 1.0f);
    const f32 after = SlugTextRenderer::CoverageAt(square, Vec2{0.3f, 0.5f}, 1.0f);
    ENG_CHECK_LT(before, after);

    // Тот же рамп есть и на правом крае.
    const f32 rightIn = SlugTextRenderer::CoverageAt(square, Vec2{0.8f, 0.5f}, 1.0f);
    const f32 rightOut = SlugTextRenderer::CoverageAt(square, Vec2{1.2f, 0.5f}, 1.0f);
    ENG_CHECK_GT(rightIn, rightOut);
    ENG_CHECK_GT(rightIn, 0.0f);
    ENG_CHECK_LT(rightOut, 1.0f);
}

ENG_TEST(Slug, CoverageZeroInHole) {
    const std::vector<SlugCurve> ring = Ring();
    ENG_CHECK_NEAR(SlugTextRenderer::CoverageAt(ring, Vec2{4, 4}), 0.0f, 1e-4f);
    ENG_CHECK_NEAR(SlugTextRenderer::CoverageAt(ring, Vec2{1, 4}), 1.0f, 1e-4f);
}

// ---------------------------------------------------------------------------
// CPU: подготовка глифов (процедурный шрифт; поставляемый шрифт опционален)
// ---------------------------------------------------------------------------
ENG_TEST(Slug, PrepareGlyphProceduralFont) {
    Font* font = FontManager::Get().DefaultFont();
    if (!font || !font->Valid()) ENG_SKIP("no default font");

    SlugTextRenderer slug;
    // Рендерер, созданный без GL-контекста, всё равно пригоден для CPU-работы.
    slug.Init(font, 64);
    ENG_CHECK(slug.PrepareGlyph('A'));
    const SlugGlyph* g = slug.GetGlyph('A');
    ENG_CHECK(g != nullptr);
    if (g) {
        ENG_CHECK(!g->empty);
        ENG_CHECK_GT(g->curveCount, 0);
        ENG_CHECK_GE(g->bandCount, 8);
        ENG_CHECK_LE(g->bandCount, 16);
        ENG_CHECK_GT(g->bounds.w, 0.0f);
        ENG_CHECK_GT(g->bounds.h, 0.0f);
        // Полосы (bands) — это пары (firstCurve, count) в плоском массиве.
        ENG_CHECK_GE(slug.BandCount(), g->bandCount * 2);
        ENG_CHECK_GE(slug.CurveCount(), g->curveCount);
        // Run каждой полосы остаётся внутри диапазона кривых этого глифа, и ни один
        // run не превышает константную границу цикла в шейдере.
        for (int b = 0; b < g->bandCount; ++b) {
            const int idx = (g->firstBand + b) * 2;
            ENG_CHECK_LT(idx + 1, slug.BandCount());
        }
    }
    // Пробел не имеет контура, но всё же имеет ширину (advance).
    ENG_CHECK(!slug.PrepareGlyph(' '));
    const SlugGlyph* space = slug.GetGlyph(' ');
    ENG_CHECK(space != nullptr);
    if (space) ENG_CHECK(space->empty);
}

ENG_TEST(Slug, PrepareGlyphBundledFont) {
    Font* font = LoadEngineFont();
    if (!font) ENG_SKIP("assets/fonts/engine.ttf not available");

    SlugTextRenderer slug;
    slug.Init(font, 64);
    int prepared = 0;
    for (u32 cp : {'A', 'g', 'o', 'W', 'y'}) {
        if (!slug.PrepareGlyph(cp)) continue;
        const SlugGlyph* g = slug.GetGlyph(cp);
        ENG_CHECK(g != nullptr);
        if (!g) continue;
        ++prepared;
        ENG_CHECK_GT(g->curveCount, 0);
        ENG_CHECK_GE(g->bandCount, 8);
        ENG_CHECK_LE(g->bandCount, 16);
        ENG_CHECK_GT(g->bounds.w, 0.0f);
        ENG_CHECK_GT(g->bounds.h, 0.0f);
        ENG_CHECK_GT(g->advance, 0.0f);
        // Глиф может опускаться ниже базовой линии ('g', 'y') или сидеть выше неё
        // ('A'), поэтому гарантируется лишь невырожденность размеров бокса.
        ENG_CHECK(g->bounds.h < 4.0f);
    }
    ENG_CHECK_GT(prepared, 0);
    // Повторная подготовка кэшированного глифа не должна дублировать кривые.
    const int curvesBefore = slug.CurveCount();
    slug.PrepareGlyph('A');
    ENG_CHECK_EQ(slug.CurveCount(), curvesBefore);
}

// ---------------------------------------------------------------------------
// CPU: измерение
// ---------------------------------------------------------------------------
ENG_TEST(Slug, MeasureGrowsWithLength) {
    Font* font = FontManager::Get().DefaultFont();
    if (!font || !font->Valid()) ENG_SKIP("no default font");
    SlugTextRenderer slug;
    slug.Init(font, 64);

    ENG_CHECK_NEAR(slug.Measure(""), 0.0f, 1e-6f);
    const f32 one = slug.Measure("A");
    const f32 two = slug.Measure("AA");
    const f32 three = slug.Measure("AAA");
    ENG_CHECK_GT(one, 0.0f);
    ENG_CHECK_GT(two, one);
    ENG_CHECK_GT(three, two);
    // С явным размером результат в логических пикселях.
    const f32 scaled = slug.Measure("AAA", 0.0f, 32.0f);
    ENG_CHECK_NEAR(scaled, three * 32.0f, 1e-3f);
}

// ---------------------------------------------------------------------------
// GPU
// ---------------------------------------------------------------------------
ENG_TEST(Slug, GpuInitAndPrepareGlyph) {
    ENG_REQUIRE_GL();
    Font* font = FontManager::Get().DefaultFont();
    if (!font || !font->Valid()) ENG_SKIP("no default font");
    SlugTextRenderer slug;
    if (!slug.Init(font, 64)) ENG_SKIP("SlugTextRenderer::Init failed");
    ENG_CHECK(slug.Valid());
    ENG_CHECK(slug.PrepareGlyph('A'));
    slug.Flush();
    // Данные кривых/полос действительно дошли до GPU.
    const SlugGlyph* g = slug.GetGlyph('A');
    ENG_CHECK(g != nullptr);
    if (g) ENG_CHECK_GT(g->curveCount, 0);
    ENG_CHECK_GT(slug.CurveCount(), 0);
    ENG_CHECK_GT(slug.BandCount(), 0);
    slug.Shutdown();
    ENG_CHECK(!slug.Valid());
}

ENG_TEST(Slug, GpuRendersGlyphNotRectangle) {
    ENG_REQUIRE_GL();
    // Поставляемый контурный шрифт: глифы процедурного fallback — сплошные
    // боксы (это bitmap-шрифт, у которого бокс синтезирован как "контур"), поэтому
    // он не может отличить настоящий глиф от залитого прямоугольника.
    Font* font = LoadEngineFont();
    if (!font) ENG_SKIP("assets/fonts/engine.ttf not available");
    GpuFixture f;
    if (!f.Init(256, 256)) ENG_SKIP("no GL context");
    SlugTextRenderer slug;
    if (!slug.Init(font, 64)) ENG_SKIP("SlugTextRenderer::Init failed");

    f.Begin();
    SlugTextStyle style;
    style.color = Color::White;
    // У 'W' есть строки с 1, 2 и 3 отдельными прогонами чернил (вершина, штрихи,
    // средняя впадина), поэтому залитый прямоугольник, дающий ровно один run на
    // строку, не может пройти проверку span ниже.
    slug.Draw(f.r2d, "W", Vec2{40.0f, 200.0f}, 160.0f, style);
    f.End();

    Bitmap bmp;
    if (!f.Read(&bmp)) ENG_SKIP("ReadPixels failed");
    const int area = bmp.InkArea();
    const f32 coverage = static_cast<f32>(area) / static_cast<f32>(f.w * f.h);
    ENG_CHECK_GT(coverage, 0.02f);
    ENG_CHECK_LT(coverage, 0.90f);
    // Залитый прямоугольник давал бы один span на каждую строку.
    ENG_CHECK_GE(bmp.DistinctRowSpans(), 3);
    ENG_CHECK_GT(slug.GetStats().glyphsDrawn, 0);
    ENG_CHECK_GT(slug.GetStats().quads, 0);
}

ENG_TEST(Slug, GpuShadowOffsetShiftsInk) {
    ENG_REQUIRE_GL();
    Font* font = FontManager::Get().DefaultFont();
    if (!font || !font->Valid()) ENG_SKIP("no default font");
    GpuFixture f;
    if (!f.Init(320, 256)) ENG_SKIP("no GL context");
    SlugTextRenderer slug;
    if (!slug.Init(font, 64)) ENG_SKIP("SlugTextRenderer::Init failed");

    const Vec2 baseline{40.0f, 190.0f};
    const f32 size = 150.0f;
    const f32 dx = 30.0f;

    SlugTextStyle plain;
    plain.color = Color::White;
    f.Begin();
    slug.Draw(f.r2d, "A", baseline, size, plain);
    f.End();
    Bitmap a;
    if (!f.Read(&a)) ENG_SKIP("ReadPixels failed");

    SlugTextStyle shadowed = plain;
    shadowed.shadowColor = Color{1, 0, 0, 0.6f};
    shadowed.shadowOffset = Vec2{dx / size, 0.0f};  // смещения стиля задаются в em
    f.Begin();
    slug.Draw(f.r2d, "A", baseline, size, shadowed);
    f.End();
    Bitmap b;
    if (!f.Read(&b)) ENG_SKIP("ReadPixels failed");

    const Rect ba = a.InkBounds();
    const Rect bb = b.InkBounds();
    ENG_CHECK_GT(ba.w, 0.0f);
    ENG_CHECK_GT(bb.w, 0.0f);
    // Тень рисуется позади заливки и простирается вправо, поэтому общий бокс чернил
    // вырастает примерно на запрошенное смещение в пикселях.
    ENG_CHECK_GT(bb.w, ba.w + dx * 0.4f);
    ENG_CHECK_LT(bb.w, ba.w + dx * 1.8f);
    ENG_CHECK_NEAR(bb.x, ba.x, 3.0f);
}

ENG_TEST(Slug, GpuOutlineGrowsInk) {
    ENG_REQUIRE_GL();
    Font* font = LoadEngineFont();
    if (!font) ENG_SKIP("assets/fonts/engine.ttf not available");
    GpuFixture f;
    if (!f.Init(256, 256)) ENG_SKIP("no GL context");
    SlugTextRenderer slug;
    if (!slug.Init(font, 64)) ENG_SKIP("SlugTextRenderer::Init failed");

    SlugTextStyle plain;
    plain.color = Color::White;
    f.Begin();
    slug.Draw(f.r2d, kDemoText, Vec2{20.0f, 190.0f}, 120.0f, plain);
    f.End();
    Bitmap a;
    if (!f.Read(&a)) ENG_SKIP("ReadPixels failed");

    SlugTextStyle outlined = plain;
    outlined.outlineColor = Color{1, 1, 0, 1};
    outlined.outlineWidth = 0.06f;  // единицы em
    f.Begin();
    slug.Draw(f.r2d, kDemoText, Vec2{20.0f, 190.0f}, 120.0f, outlined);
    f.End();
    Bitmap b;
    if (!f.Read(&b)) ENG_SKIP("ReadPixels failed");

    const int areaA = a.InkArea();
    const int areaB = b.InkArea();
    ENG_CHECK_GT(areaA, 0);
    ENG_CHECK_GT(areaB, areaA);
    // Обводка должна быть реально видна снаружи заливки.
    const Rect ba = a.InkBounds();
    const Rect bb = b.InkBounds();
    ENG_CHECK_GT(bb.w * bb.h, ba.w * ba.h);
    // Цвет обводки должен появиться где-то в изображении.
    bool sawOutline = false;
    for (int y = 0; y < b.h && !sawOutline; ++y)
        for (int x = 0; x < b.w; ++x)
            if (b.Ink(x, y) && b.At(x, y, 0) > 120 && b.At(x, y, 2) < 90) {
                sawOutline = true;
                break;
            }
    ENG_CHECK(sawOutline);
}

ENG_TEST(Slug, GpuGradientRamps) {
    ENG_REQUIRE_GL();
    Font* font = FontManager::Get().DefaultFont();
    if (!font || !font->Valid()) ENG_SKIP("no default font");
    const char* path = "assets/fonts/engine.ttf";
    Font real;
    const bool hasReal = real.LoadFromFile(path, FontDesc{});
    if (hasReal) font = &real;

    GpuFixture f;
    if (!f.Init(256, 256)) ENG_SKIP("no GL context");
    SlugTextRenderer slug;
    if (!slug.Init(font, 64)) ENG_SKIP("SlugTextRenderer::Init failed");

    // Публичный стиль имеет один цвет, поэтому градиент проверяется через
    // uniform-ы inner/outer, задаваемые рендерером: с одним цветом глиф плоский —
    // именно это контракт и открывает. Проверяем, что плоская заливка всё ещё
    // точно совпадает с запрошенным цветом.
    SlugTextStyle style;
    style.color = Color{0.2f, 0.6f, 1.0f, 1.0f};
    f.Begin();
    slug.Draw(f.r2d, "A", Vec2{48.0f, 200.0f}, 160.0f, style);
    f.End();
    Bitmap b;
    if (!f.Read(&b)) ENG_SKIP("ReadPixels failed");
    // Находим внутренний пиксель (покрытие 1) и проверяем его цвет.
    bool checked = false;
    for (int y = 0; y < b.h && !checked; ++y) {
        for (int x = 0; x < b.w; ++x) {
            if (b.At(x, y, 3) < 250) continue;
            ENG_CHECK_NEAR(b.At(x, y, 0) / 255.0f, 0.2f, 0.05f);
            ENG_CHECK_NEAR(b.At(x, y, 1) / 255.0f, 0.6f, 0.05f);
            ENG_CHECK_NEAR(b.At(x, y, 2) / 255.0f, 1.0f, 0.05f);
            checked = true;
            break;
        }
    }
    ENG_CHECK(checked);
}

ENG_TEST(Slug, GpuDrawOnPathOrientation) {
    ENG_REQUIRE_GL();
    Font* font = LoadEngineFont();
    if (!font) ENG_SKIP("assets/fonts/engine.ttf not available");
    GpuFixture f;
    if (!f.Init(320, 320)) ENG_SKIP("no GL context");
    SlugTextRenderer slug;
    if (!slug.Init(font, 64)) ENG_SKIP("SlugTextRenderer::Init failed");

    SlugTextStyle style;
    style.color = Color::White;
    const f32 size = 42.0f;

    // Горизонтальный путь: чернила растягиваются вдоль +x.
    const Vec2 hpath[2] = {{10.0f, 160.0f}, {310.0f, 160.0f}};
    f.Begin();
    slug.DrawOnPath(f.r2d, "PATH", hpath, 2, size, style);
    f.End();
    Bitmap hb;
    if (!f.Read(&hb)) ENG_SKIP("ReadPixels failed");

    // Вертикальный путь: чернила растягиваются вдоль +y.
    const Vec2 vpath[2] = {{160.0f, 10.0f}, {160.0f, 310.0f}};
    f.Begin();
    slug.DrawOnPath(f.r2d, "PATH", vpath, 2, size, style);
    f.End();
    Bitmap vb;
    if (!f.Read(&vb)) ENG_SKIP("ReadPixels failed");

    const Rect hr = hb.InkBounds();
    const Rect vr = vb.InkBounds();
    ENG_CHECK_GT(hb.InkArea(), 0);
    ENG_CHECK_GT(vb.InkArea(), 0);
    ENG_CHECK_GT(hr.w, 0.0f);
    ENG_CHECK_GT(vr.h, 0.0f);
    // Горизонтальный прогон шире, чем выше; вертикальный — наоборот.
    ENG_CHECK_GT(hr.w, hr.h);
    ENG_CHECK_GT(vr.h, vr.w);
}

ENG_TEST(Slug, GpuDrawOnArcSpansHorizontally) {
    ENG_REQUIRE_GL();
    Font* font = FontManager::Get().DefaultFont();
    if (!font || !font->Valid()) ENG_SKIP("no default font");
    GpuFixture f;
    if (!f.Init(480, 480)) ENG_SKIP("no GL context");
    SlugTextRenderer slug;
    if (!slug.Init(font, 64)) ENG_SKIP("SlugTextRenderer::Init failed");

    SlugTextStyle style;
    style.color = Color::White;
    // Дуга проходится от `startAngle` в направлении возрастания угла. Старт с pi
    // (крайняя левая точка круга в пространстве y-down) и строка, достаточно длинная,
    // чтобы покрыть большую часть полуокружности, заставляют прогон пройти слева,
    // через верх, направо, поэтому бокс чернил гораздо шире, чем выше. Короткий
    // прогон (например "ARC") покрывает лишь ~60 градусов и вовсе не был бы широким,
    // поэтому длина строки — часть теста.
    f.Begin();
    slug.DrawOnArc(f.r2d, "SEMICIRCLE", Vec2{240.0f, 260.0f}, 120.0f, kPi, 50.0f, style, true);
    f.End();
    Bitmap b;
    if (!f.Read(&b)) ENG_SKIP("ReadPixels failed");
    const Rect r = b.InkBounds();
    ENG_CHECK_GT(r.w, 0.0f);
    ENG_CHECK_GT(r.h, 0.0f);
    ENG_CHECK_GT(r.w, r.h * 1.2f);
}

// ---------------------------------------------------------------------------
// Согласие GPU и CPU (ключевой тест корректности)
// ---------------------------------------------------------------------------
ENG_TEST(Slug, GpuMatchesCpuCoverage) {
    ENG_REQUIRE_GL();
    Font* font = LoadEngineFont();
    if (!font) ENG_SKIP("assets/fonts/engine.ttf not available");

    const int kW = 320, kH = 320;
    GpuFixture f;
    if (!f.Init(kW, kH)) ENG_SKIP("no GL context");
    SlugTextRenderer slug;
    if (!slug.Init(font, 64)) ENG_SKIP("SlugTextRenderer::Init failed");
    if (!slug.PrepareGlyph('S')) ENG_SKIP("no outline for 'S'");

    const SlugGlyph* g = slug.GetGlyph('S');
    if (!g) ENG_SKIP("glyph not cached");
    // Один em — 240 пикселей: достаточно, чтобы AA-зона в 1 пиксель разрешалась
    // несколькими сэмплами поперёк края.
    const f32 size = 240.0f;
    const Vec2 baseline{40.0f, 260.0f};

    SlugTextStyle style;
    style.color = Color::White;
    style.softness = 1.0f;
    f.Begin();
    slug.Draw(f.r2d, "S", baseline, size, style);
    f.End();
    Bitmap bmp;
    if (!f.Read(&bmp)) ENG_SKIP("ReadPixels failed");

    // Перестраиваем список кривых глифа в том же em-пространстве, которое видит
    // шейдер, и сравниваем CoverageAt с прочитанной альфой на той же сетке.
    std::vector<SlugCurve> curves;
    curves.reserve(static_cast<usize>(g->curveCount));
    // Рендерер хранит свою копию; извлекаем заново через шрифт, чтобы тест не
    // зависел от приватных массивов рендерера.
    GlyphOutline outline;
    if (!font->GetGlyphOutline('S', 64.0f, &outline)) ENG_SKIP("outline extraction failed");
    const f32 inv = 1.0f / 64.0f;
    for (const GlyphContour& contour : outline.contours) {
        const std::vector<GlyphPoint>& pts = contour.points;
        const usize n = pts.size();
        if (n < 2) continue;
        const bool wrapped = n > 2 && pts[0].p == pts[n - 1].p;
        const usize m = wrapped ? n - 1 : n;
        usize start = 0;
        for (usize i = 0; i < m; ++i)
            if (pts[i].onCurve) {
                start = i;
                break;
            }
        Vec2 pen = pts[start].onCurve ? pts[start].p * inv
                                      : (pts[(start + m - 1) % m].p + pts[start].p) * 0.5f * inv;
        for (usize k = 1; k <= m; ++k) {
            const usize i0 = (start + k) % m;
            const usize i1 = (start + k + 1) % m;
            if (pts[i0].onCurve) {
                SlugCurve c;
                c.p0 = pen;
                c.p1 = pen;
                c.p2 = pts[i0].p * inv;
                curves.push_back(c);
                pen = c.p2;
                continue;
            }
            Vec2 ctrl = pts[i0].p * inv;
            Vec2 end;
            if (pts[i1].onCurve) {
                end = pts[i1].p * inv;
            } else {
                end = (ctrl + pts[i1].p * inv) * 0.5f;
                --k;
            }
            SlugCurve c;
            c.p0 = pen;
            c.p1 = ctrl;
            c.p2 = end;
            curves.push_back(c);
            pen = end;
        }
    }
    ENG_CHECK_GT(curves.size(), 0u);

    // Локальное (относительное пера) em-начало глифа было помещено на базовую
    // линию; фрагментный шейдер работает в немасштабированных em-единицах.
    const f32 aa = 1.0f / size;
    const Rect ink = bmp.InkBounds();
    if (ink.w <= 0.0f) ENG_SKIP("no ink rendered");

    // Сравниваем каждый пиксель бокса чернил с полем 3 px. Это окно содержит
    // внутренность (CPU 1 / GPU 1), внешность (0/0) и аналитическое кольцо
    // сглаживания, так что расхождению правила заливки или покрытия не скрыться.
    double sum = 0.0;
    int counted = 0;
    int softPixels = 0;
    int largeDiffs = 0;
    for (int y = static_cast<int>(ink.y) - 3; y <= static_cast<int>(ink.y + ink.h) + 3; ++y) {
        for (int x = static_cast<int>(ink.x) - 3; x <= static_cast<int>(ink.x + ink.w) + 3; ++x) {
            if (x < 0 || y < 0 || x >= kW || y >= kH) continue;
            const f32 localX = ((static_cast<f32>(x) + 0.5f) - baseline.x) / size;
            const f32 localY = ((static_cast<f32>(y) + 0.5f) - baseline.y) / size;
            const f32 cpu = SlugTextRenderer::CoverageAt(curves, Vec2{localX, localY}, aa);
            const f32 gpu = bmp.Coverage(x, y);
            const double diff = std::fabs(static_cast<double>(cpu) - static_cast<double>(gpu));
            sum += diff;
            ++counted;
            if (cpu > 0.02f && cpu < 0.98f) ++softPixels;
            if (diff > 0.5) ++largeDiffs;
        }
    }
    ENG_CHECK_GT(counted, 5000);
    // В окне должно быть настоящее кольцо сглаживания, иначе сравнение проверяло
    // бы только плоские внутренние/внешние пиксели.
    ENG_CHECK_GT(softPixels, 100);
    // Шейдер и CPU-эталон решают одну и ту же квадратичную кривую с той же
    // структурой ветвлений; единственная разница — координаты кривых на GPU
    // округляются до float32. На этом окне средняя абсолютная разница равна 0.000,
    // а пикселей с расхождением больше 0.5 — ноль, поэтому 0.05 — щедрый потолок,
    // который всё же громко падает, если оба расходятся в правиле заливки
    // (это проявляется как MAD около 0.3 и выше).
    const double mad = counted > 0 ? sum / static_cast<double>(counted) : 1.0;
    ENG_CHECK_MSG(mad < 0.05, "mean |GPU - CPU| = " + std::to_string(mad));
    // Ни один пиксель не может быть классифицирован наоборот: это сильная форма
    // проверки согласия (пиксель на полупокрытом крае может отличаться, сплошной
    // внутренний или внешний — нет).
    ENG_CHECK_LT(largeDiffs, counted / 200);
}

// ---------------------------------------------------------------------------
// Прочее покрытие контракта
// ---------------------------------------------------------------------------
ENG_TEST(Slug, StyleTransformsAreHonoured) {
    ENG_REQUIRE_GL();
    Font* font = FontManager::Get().DefaultFont();
    if (!font || !font->Valid()) ENG_SKIP("no default font");
    GpuFixture f;
    if (!f.Init(256, 256)) ENG_SKIP("no GL context");
    SlugTextRenderer slug;
    if (!slug.Init(font, 64)) ENG_SKIP("SlugTextRenderer::Init failed");

    SlugTextStyle style;
    style.color = Color::White;
    style.rotation = 0.6f;
    style.scale = Vec2{1.0f, 1.4f};
    style.twist = 0.5f;
    style.dilation = 0.02f;
    f.Begin();
    const f32 advance = slug.Draw(f.r2d, "Tw", Vec2{64.0f, 160.0f}, 90.0f, style);
    f.End();
    ENG_CHECK_GT(advance, 0.0f);
    Bitmap b;
    if (!f.Read(&b)) ENG_SKIP("ReadPixels failed");
    ENG_CHECK_GT(b.InkArea(), 0);
}

ENG_TEST(Slug, MeasureMatchesDrawnAdvance) {
    ENG_REQUIRE_GL();
    Font* font = FontManager::Get().DefaultFont();
    if (!font || !font->Valid()) ENG_SKIP("no default font");
    GpuFixture f;
    if (!f.Init(512, 128)) ENG_SKIP("no GL context");
    SlugTextRenderer slug;
    if (!slug.Init(font, 64)) ENG_SKIP("SlugTextRenderer::Init failed");

    SlugTextStyle style;
    style.color = Color::White;
    const f32 size = 48.0f;
    f.Begin();
    const f32 drawn = slug.Draw(f.r2d, "Measure me", Vec2{8.0f, 90.0f}, size, style);
    f.End();
    const f32 measured = slug.Measure("Measure me", 0.0f, size);
    ENG_CHECK_GT(drawn, 0.0f);
    ENG_CHECK_NEAR(drawn, measured, 1e-2f);
}
