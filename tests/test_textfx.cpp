// Тесты форматированного текста: стили, внутренние/внешние градиенты, обводки,
// тени, crossrender, скручивание, текст на пути и текст на дуге.
//
// CPU-тесты выполняются всегда; тестам рендера нужен GL-контекст, и они рисуют
// в offscreen-цель, поэтому корректно пропускаются, когда контекст недоступен.
#include "crossrender/gfx/GL.h"
#include "crossrender/core/File.h"
#include "crossrender/test/Test.h"
#include "crossrender/text/Font.h"
#include "crossrender/gfx/Renderer2D.h"
#include "crossrender/gfx/RenderTarget.h"
#include "crossrender/platform/Platform.h"

#include <cmath>
#include <vector>

using namespace crossrender;

namespace {

// Загружает шрифт уже после создания GL-контекста, чтобы его атлас был валиден.
struct TextFixture {
    Font font;
    bool loaded = false;

    bool Init(int px = 48) {
        FontDesc desc;
        desc.pixelHeight = static_cast<f32>(px);
        desc.hinting = true;
        // engine.ttf собирался удалённым генератором ассетов; пробуем его,
        // затем любой staged-шрифт примера.
        for (const char* name : {"fonts/engine.ttf", "fonts/ubuntu.ttf"}) {
            const std::string path = PathJoin(GetAssetRoot(), name);
            if (FileExists(path) && font.LoadFromFile(path, desc)) {
                loaded = true;
                return true;
            }
        }
        return false;
    }
};

// Рендерит по одному изображению за раз. Два живых холста разделяли бы глобальную
// привязку framebuffer (Renderer2D не привязывает цели сам), поэтому каждый
// тест рисования завершается и читает результат до создания следующего.
template <typename DrawFn>
bool RenderImage(int width, int height, DrawFn&& draw, std::vector<u8>* out) {
    RenderTarget rt;
    RenderTargetDesc d;
    d.width = width;
    d.height = height;
    d.depth = false;
    d.colorFormat = PixelFormat::RGBA8;
    if (!rt.Create(d)) return false;
    Renderer2D r2d;
    if (!r2d.Init()) return false;
    rt.Bind();
    rt.Clear(Color::Black, false, false);
    r2d.BeginFrame(width, height, 1.0f, &rt);
    draw(r2d);
    r2d.EndFrame();
    bool ok = rt.ReadPixels(out);
    rt.Unbind();
    return ok;
}


struct InkStats {
    int pixels = 0;
    f32 meanLuma = 0;
    int minX = 1 << 30, minY = 1 << 30, maxX = -1, maxY = -1;
    int distinctRowSpans = 0;
    std::vector<Color> colours;
};

// ReadPixels возвращает строки снизу вверх; интерпретируем как индексацию сверху слева.
InkStats Analyse(const std::vector<u8>& px, int w, int h, int sourceH) {
    InkStats s;
    f64 sum = 0;
    std::vector<int> rowCount(static_cast<usize>(h), 0);
    std::vector<u32> quantised;
    for (int y = 0; y < h; ++y) {
        int srcY = sourceH - 1 - y;
        if (srcY < 0 || srcY >= sourceH) continue;
        for (int x = 0; x < w; ++x) {
            usize i = (static_cast<usize>(srcY) * w + x) * 4;
            if (i + 3 >= px.size()) continue;
            int luma = px[i] + px[i + 1] + px[i + 2];
            if (luma <= 24) continue;
            ++s.pixels;
            sum += luma / 3.0;
            rowCount[static_cast<usize>(y)]++;
            s.minX = MinT(s.minX, x);
            s.maxX = MaxT(s.maxX, x);
            s.minY = MinT(s.minY, y);
            s.maxY = MaxT(s.maxY, y);
            // Квантование 4 бита на канал для подсчёта различных цветов.
            u32 q = (static_cast<u32>(px[i] >> 4) << 8) | (static_cast<u32>(px[i + 1] >> 4) << 4) |
                    static_cast<u32>(px[i + 2] >> 4);
            if (quantised.empty() || quantised.back() != q) quantised.push_back(q);
        }
    }
    if (s.pixels > 0) s.meanLuma = static_cast<f32>(sum / s.pixels);
    for (int c : rowCount)
        if (c > 0) ++s.distinctRowSpans;
    return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// CPU-only
// ---------------------------------------------------------------------------
ENG_TEST(TextStyle, PresetsAreSane) {
    TextStyle filled = TextStyle::Filled(Color::Red);
    ENG_CHECK(filled.color == Color::Red);
    ENG_CHECK(!filled.gradient);
    ENG_CHECK_NEAR(filled.outlineWidth, 0.0f, 1e-6f);

    TextStyle outlined = TextStyle::Outlined(Color::White, Color::Black, 3.0f);
    ENG_CHECK(outlined.color == Color::White);
    ENG_CHECK(outlined.outlineColor == Color::Black);
    ENG_CHECK_NEAR(outlined.outlineWidth, 3.0f, 1e-6f);

    TextStyle shadowed = TextStyle::Shadowed(Color::White, Vec2{4, 5});
    ENG_CHECK(shadowed.shadowColor.a > 0.0f);
    ENG_CHECK_NEAR(shadowed.shadowOffset.x, 4.0f, 1e-6f);
    ENG_CHECK_NEAR(shadowed.shadowOffset.y, 5.0f, 1e-6f);

    TextStyle grad = TextStyle::GradientText(Color::Yellow, Color::Blue);
    ENG_CHECK(grad.gradient);
    ENG_CHECK(grad.innerColor == Color::Yellow);
    ENG_CHECK(grad.outerColor == Color::Blue);
}

ENG_TEST(TextStyle, MeasureStyledTextWithoutGl) {
    Font* font = FontManager::Get().DefaultFont();
    if (!font || !font->Valid()) ENG_SKIP("no default font");
    Renderer2D r2d;
    r2d.Init();  // для измерений достаточно инертного режима

    TextStyle st;
    st.color = Color::White;
    auto m1 = r2d.MeasureStyledText(*font, "Hello", 24.0f, st);
    ENG_CHECK_GT(m1.width, 0.0f);
    ENG_CHECK_GT(m1.height, 0.0f);
    ENG_CHECK_GT(m1.glyphCount, 0);
    ENG_CHECK_EQ(m1.lineCount, 1);

    auto m2 = r2d.MeasureStyledText(*font, "HelloHello", 24.0f, st);
    ENG_CHECK_GT(m2.width, m1.width);
    ENG_CHECK_GT(m2.glyphCount, m1.glyphCount);

    // Масштабирование стиля масштабирует измерение.
    TextStyle scaled = st;
    scaled.scale = Vec2{2.0f, 2.0f};
    auto m3 = r2d.MeasureStyledText(*font, "Hello", 24.0f, scaled);
    ENG_CHECK_NEAR(m3.width, m1.width * 2.0f, m1.width * 0.05f + 0.5f);

    // Межбуквенный интервал расширяет строку.
    TextStyle spaced = st;
    spaced.letterSpacing = 4.0f;
    auto m4 = r2d.MeasureStyledText(*font, "Hello", 24.0f, spaced);
    ENG_CHECK_GT(m4.width, m1.width + 8.0f);

    // Переводы строк дают больше строк и более высокий бокс.
    auto m5 = r2d.MeasureStyledText(*font, "a\nb\nc", 24.0f, st);
    ENG_CHECK_EQ(m5.lineCount, 3);
    ENG_CHECK_GT(m5.height, m1.height);

    // Пустой текст измеряется нулём и не приводит к падению.
    auto m6 = r2d.MeasureStyledText(*font, "", 24.0f, st);
    ENG_CHECK_NEAR(m6.width, 0.0f, 1e-5f);
    ENG_CHECK_EQ(m6.glyphCount, 0);
}

// ---------------------------------------------------------------------------
// GPU
// ---------------------------------------------------------------------------
ENG_TEST(TextStyle, SolidFillRendersInk) {
    ENG_REQUIRE_GL();
    TextFixture fx;
    if (!fx.Init()) ENG_SKIP("no font file staged");
    std::vector<u8> px;
    if (!RenderImage(256, 96,
                     [&](Renderer2D& r) {
                         TextStyle st = TextStyle::Filled(Color::White);
                         r.DrawTextStyled(fx.font, "Engine", Vec2{8, 10}, 48.0f, st);
                     },
                     &px))
        ENG_SKIP("no render target");
    InkStats ink = Analyse(px, 256, 96, 96);
    ENG_CHECK_GT(ink.pixels, 200);
    ENG_CHECK_GT(ink.meanLuma, 80.0f);
    ENG_CHECK_GT(ink.distinctRowSpans, 5);   // настоящие формы глифов, а не блок
    ENG_CHECK_LT(ink.maxX - ink.minX, 256);
}

ENG_TEST(TextStyle, GradientFillVariesVertically) {
    ENG_REQUIRE_GL();
    TextFixture fx;
    if (!fx.Init(64)) ENG_SKIP("no font file staged");
    const int W = 256, H = 96;
    std::vector<u8> px;
    if (!RenderImage(W, H,
                     [&](Renderer2D& r) {
                         TextStyle st = TextStyle::GradientText(Color::Red, Color::Blue);
                         r.DrawTextStyled(fx.font, "GRAD", Vec2{4, 4}, 64.0f, st);
                     },
                     &px))
        ENG_SKIP("no render target");

    f64 topR = 0, topB = 0, botR = 0, botB = 0;
    int topN = 0, botN = 0;
    for (int y = 0; y < H; ++y) {
        int srcY = H - 1 - y;
        for (int x = 0; x < W; ++x) {
            usize i = (static_cast<usize>(srcY) * W + x) * 4;
            if (px[i] + px[i + 1] + px[i + 2] <= 60) continue;
            if (y < H / 2) {
                topR += px[i];
                topB += px[i + 2];
                ++topN;
            } else {
                botR += px[i];
                botB += px[i + 2];
                ++botN;
            }
        }
    }
    if (topN < 20 || botN < 20) ENG_SKIP("not enough ink to compare halves");
    f64 topRatio = topR / (topR + topB + 1e-6);
    f64 botRatio = botR / (botR + botB + 1e-6);
    ENG_CHECK_MSG(topRatio > botRatio + 0.10, "gradient should go from red to blue");
}

ENG_TEST(TextStyle, OutlineAddsInk) {
    ENG_REQUIRE_GL();
    TextFixture fx;
    if (!fx.Init(48)) ENG_SKIP("no font file staged");
    const int W = 240, H = 120;
    std::vector<u8> plain, outlined;
    if (!RenderImage(W, H,
                     [&](Renderer2D& r) {
                         r.DrawTextStyled(fx.font, "Outline", Vec2{16, 20}, 48.0f,
                                          TextStyle::Filled(Color::White));
                     },
                     &plain))
        ENG_SKIP("no render target");
    if (!RenderImage(W, H,
                     [&](Renderer2D& r) {
                         r.DrawTextStyled(fx.font, "Outline", Vec2{16, 20}, 48.0f,
                                          TextStyle::Outlined(Color::White, Color::FromRGB(0x3366FF),
                                                              4.0f));
                     },
                     &outlined))
        ENG_SKIP("no render target");

    InkStats sa = Analyse(plain, W, H, H);
    InkStats sb = Analyse(outlined, W, H, H);
    ENG_CHECK_GT(sa.pixels, 100);
    ENG_CHECK_MSG(sb.pixels > sa.pixels, "an outline must add ink around the glyphs");
    ENG_CHECK_MSG(sb.maxX > sa.maxX && sb.minX < sa.minX,
                  "the outline must grow the ink bounds on both sides");
    int blueish = 0;
    for (usize i = 0; i + 3 < outlined.size(); i += 4)
        if (outlined[i + 2] > 120 && outlined[i] < 120) ++blueish;
    ENG_CHECK_GT(blueish, 50);
}

ENG_TEST(TextStyle, ShadowOffsetsInk) {
    ENG_REQUIRE_GL();
    TextFixture fx;
    if (!fx.Init(48)) ENG_SKIP("no font file staged");
    const int W = 260, H = 150;
    std::vector<u8> plain, shadowed;
    if (!RenderImage(W, H,
                     [&](Renderer2D& r) {
                         r.DrawTextStyled(fx.font, "Shadow", Vec2{20, 20}, 40.0f,
                                          TextStyle::Filled(Color::White));
                     },
                     &plain))
        ENG_SKIP("no render target");
    if (!RenderImage(W, H,
                     [&](Renderer2D& r) {
                         TextStyle st = TextStyle::Filled(Color::White);
                         st.shadowColor = Color{1, 1, 1, 0.9f};
                         st.shadowOffset = Vec2{18, 16};
                         r.DrawTextStyled(fx.font, "Shadow", Vec2{20, 20}, 40.0f, st);
                     },
                     &shadowed))
        ENG_SKIP("no render target");

    InkStats sa = Analyse(plain, W, H, H);
    InkStats sb = Analyse(shadowed, W, H, H);
    ENG_CHECK_GT(sa.pixels, 50);
    ENG_CHECK_MSG(sb.maxX > sa.maxX + 8, "the shadow must extend the ink to the right");
    ENG_CHECK_MSG(sb.maxY > sa.maxY + 8, "the shadow must extend the ink downwards");
    ENG_CHECK_MSG(sb.pixels > sa.pixels, "the shadow must add ink");
}

ENG_TEST(TextStyle, GlowAddsInkAroundGlyphs) {
    ENG_REQUIRE_GL();
    TextFixture fx;
    if (!fx.Init(48)) ENG_SKIP("no font file staged");
    const int W = 260, H = 140;
    std::vector<u8> plain, crossrender;
    if (!RenderImage(W, H,
                     [&](Renderer2D& r) {
                         r.DrawTextStyled(fx.font, "Glow", Vec2{50, 50}, 40.0f,
                                          TextStyle::Filled(Color::White));
                     },
                     &plain))
        ENG_SKIP("no render target");
    if (!RenderImage(W, H,
                     [&](Renderer2D& r) {
                         TextStyle st = TextStyle::Filled(Color::White);
                         st.glowColor = Color{1.0f, 0.6f, 0.1f, 0.9f};
                         st.glowRadius = 14.0f;
                         st.glowIntensity = 1.5f;
                         r.DrawTextStyled(fx.font, "Glow", Vec2{50, 50}, 40.0f, st);
                     },
                     &crossrender))
        ENG_SKIP("no render target");

    InkStats sa = Analyse(plain, W, H, H);
    InkStats sb = Analyse(crossrender, W, H, H);
    ENG_CHECK_MSG(sb.pixels > sa.pixels * 2, "the crossrender should roughly double the lit area");
    int warm = 0;
    for (usize i = 0; i + 3 < crossrender.size(); i += 4)
        if (crossrender[i] > 100 && crossrender[i + 2] < 80) ++warm;
    ENG_CHECK_GT(warm, 100);
}

ENG_TEST(TextStyle, TwistBendsTheRun) {
    ENG_REQUIRE_GL();
    TextFixture fx;
    if (!fx.Init(48)) ENG_SKIP("no font file staged");
    // Скручивание вращает каждый глиф вокруг начала строки, поэтому длинная строка
    // уходит далеко вниз; даём ей место.
    const int W = 560, H = 420;
    std::vector<u8> plain, twisted;
    if (!RenderImage(W, H,
                     [&](Renderer2D& r) {
                         r.DrawTextStyled(fx.font, "Twisting text", Vec2{30, 60}, 40.0f,
                                          TextStyle::Filled(Color::White));
                     },
                     &plain))
        ENG_SKIP("no render target");
    if (!RenderImage(W, H,
                     [&](Renderer2D& r) {
                         TextStyle st = TextStyle::Filled(Color::White);
                         st.twist = 0.004f;   // радианы на логический пиксель
                         r.DrawTextStyled(fx.font, "Twisting text", Vec2{30, 60}, 40.0f, st);
                     },
                     &twisted))
        ENG_SKIP("no render target");

    InkStats sa = Analyse(plain, W, H, H);
    InkStats sb = Analyse(twisted, W, H, H);
    ENG_CHECK_GT(sa.pixels, 100);
    ENG_CHECK_MSG(sb.maxY - sb.minY > sa.maxY - sa.minY + 10,
                  "twisting must spread the ink vertically");
    ENG_CHECK_MSG(sb.maxY > sa.maxY, "the tail of a positive twist drops below the baseline");
}

ENG_TEST(TextStyle, ArcTextWrapsInkAroundCircle) {
    ENG_REQUIRE_GL();
    TextFixture fx;
    if (!fx.Init(48)) ENG_SKIP("no font file staged");
    const int W = 360, H = 360;
    std::vector<u8> px;
    if (!RenderImage(W, H,
                     [&](Renderer2D& r) {
                         TextStyle st = TextStyle::Filled(Color::White);
                         st.align = TextAlign::Center;
                         r.DrawTextOnArc(fx.font, "AROUND THE CIRCLE", Vec2{180, 180}, 120.0f,
                                         -kPi * 0.5f, 34.0f, st, true, false);
                     },
                     &px))
        ENG_SKIP("no render target");

    InkStats ink = Analyse(px, W, H, H);
    ENG_CHECK_GT(ink.pixels, 300);
    ENG_CHECK_GT(ink.maxX - ink.minX, 150);
    const int cx = W / 2, cy = H / 2;
    int centreInk = 0;
    for (int y = cy - 20; y <= cy + 20; ++y) {
        int srcY = H - 1 - y;
        for (int x = cx - 20; x <= cx + 20; ++x) {
            usize i = (static_cast<usize>(srcY) * W + x) * 4;
            if (px[i] + px[i + 1] + px[i + 2] > 40) ++centreInk;
        }
    }
    ENG_CHECK_MSG(centreInk < ink.pixels / 4, "the arc's centre should stay empty");
}

ENG_TEST(TextStyle, PathTextFollowsThePolyline) {
    ENG_REQUIRE_GL();
    TextFixture fx;
    if (!fx.Init(48)) ENG_SKIP("no font file staged");
    // Зигзаг, чья суммарная длина близка к длине строки, поэтому текст действительно
    // проходит от верха пути до низа.
    Vec2 path[8] = {{40, 30},  {160, 55}, {40, 80},  {160, 105},
                    {40, 130}, {160, 155}, {40, 180}, {160, 205}};
    const int W = 260, H = 250;
    std::vector<u8> px;
    if (!RenderImage(W, H,
                     [&](Renderer2D& r) {
                         TextStyle st = TextStyle::Filled(Color::White);
                         // Достаточно длинный, чтобы пройти несколько звеньев зигзага.
                         r.DrawTextOnPath(fx.font, "path text follows the polyline curve", path, 8,
                                          22.0f, st);
                     },
                     &px))
        ENG_SKIP("no render target");

    InkStats ink = Analyse(px, W, H, H);
    ENG_CHECK_GT(ink.pixels, 200);
    ENG_CHECK_MSG(ink.maxY - ink.minY > 60, "text must follow the path across several rows");
}

ENG_TEST(TextStyle, TextBoxStyledWraps) {
    ENG_REQUIRE_GL();
    TextFixture fx;
    if (!fx.Init(32)) ENG_SKIP("no font file staged");
    const int W = 280, H = 200;
    std::vector<u8> px;
    int lines = 0;
    if (!RenderImage(W, H,
                     [&](Renderer2D& r) {
                         TextStyle st = TextStyle::Filled(Color::White);
                         st.wrap = TextBreak::Word;
                         lines = r.DrawTextBoxStyled(
                             fx.font, "The quick brown fox jumps over the lazy dog and keeps running",
                             Rect{8, 8, 260, 180}, 20.0f, st);
                     },
                     &px))
        ENG_SKIP("no render target");
    ENG_CHECK_GT(lines, 2);
    InkStats ink = Analyse(px, W, H, H);
    ENG_CHECK_GT(ink.pixels, 200);
    ENG_CHECK_MSG(ink.maxY - ink.minY > 40, "wrapped text must occupy several lines");
    ENG_CHECK_MSG(ink.maxX <= 270, "wrapped text must stay inside the box");
}

ENG_TEST(TextStyle, StyledTextDoesNotFloodTheScreen) {
    ENG_REQUIRE_GL();
    TextFixture fx;
    if (!fx.Init(48)) ENG_SKIP("no font file staged");
    const int W = 320, H = 160;
    std::vector<u8> px;
    int glyphs = 0;
    if (!RenderImage(W, H,
                     [&](Renderer2D& r) {
                         TextStyle st = TextStyle::GradientText(Color::White, Color::FromRGB(0x2266FF));
                         st.outlineColor = Color::Black;
                         st.outlineWidth = 2.0f;
                         st.shadowColor = Color{0, 0, 0, 0.5f};
                         st.shadowOffset = Vec2{3, 3};
                         st.shadowSoftness = 2.0f;
                         r.DrawTextStyled(fx.font, "All effects", Vec2{16, 16}, 44.0f, st);
                         glyphs = r.GetStats().textGlyphs;
                     },
                     &px))
        ENG_SKIP("no render target");

    InkStats ink = Analyse(px, W, H, H);
    const f32 coverage = static_cast<f32>(ink.pixels) / static_cast<f32>(W * H);
    ENG_CHECK_GT(glyphs, 0);   // стилизованный путь должен сообщать свои глифы
    ENG_CHECK_GT(coverage, 0.02f);
    ENG_CHECK_MSG(coverage < 0.60f, "styled text must not cover most of the surface");
    ENG_CHECK_GT(ink.distinctRowSpans, 8);
}
