//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: 2D-векторный рендерер немедленного режима: пути, сплайны, фигуры и текст.
//
#pragma once

#include "crossrender/core/Base.h"
#include "crossrender/core/Math.h"
#include "crossrender/gfx/Texture.h"

#include <string>
#include <vector>

namespace crossrender {

class Font;
class RenderTarget;
class Renderer3D;

// ---------------------------------------------------------------------------
// Paint (кисть)
// ---------------------------------------------------------------------------
struct Paint {
    enum class Type : u8 { Solid, LinearGradient, RadialGradient, BoxGradient, ImagePattern };

    Type type = Type::Solid;
    Color color = Color::White;
    Color innerColor = Color::White;
    Color outerColor = Color{0, 0, 0, 0};
    Vec2 p0, p1;            // ось градиента или начало паттерна
    f32 r0 = 0, r1 = 1;     // радиусы радиального градиента / радиус угла box-градиента
    f32 feather = 0;        // растушёвка box-градиента
    f32 angle = 0;          // поворот паттерна изображения
    const Texture* image = nullptr;
    TextureFilter filter = TextureFilter::Linear;

    static Paint Solid(const Color& c) {
        Paint p;
        p.type = Type::Solid;
        p.color = c;
        return p;
    }
    static Paint Linear(const Vec2& a, const Vec2& b, const Color& inner, const Color& outer) {
        Paint p;
        p.type = Type::LinearGradient;
        p.p0 = a;
        p.p1 = b;
        p.innerColor = inner;
        p.outerColor = outer;
        return p;
    }
    static Paint Radial(const Vec2& center, f32 inner, f32 outer, const Color& cin, const Color& cout) {
        Paint p;
        p.type = Type::RadialGradient;
        p.p0 = center;
        p.p1 = center;
        p.r0 = inner;
        p.r1 = outer;
        p.innerColor = cin;
        p.outerColor = cout;
        return p;
    }
    static Paint Box(const Vec2& center, f32 w, f32 h, f32 radius, f32 feather, const Color& cin,
                     const Color& cout) {
        Paint p;
        p.type = Type::BoxGradient;
        p.p0 = center;
        p.p1 = {w, h};
        p.r0 = radius;
        p.feather = feather;
        p.innerColor = cin;
        p.outerColor = cout;
        return p;
    }
    static Paint Image(const Texture& tex, const Color& tint, f32 angleRad = 0) {
        Paint p;
        p.type = Type::ImagePattern;
        p.image = &tex;
        p.color = tint;
        p.angle = angleRad;
        return p;
    }
};

enum class LineCap : u8 { Butt, Round, Square };
enum class LineJoin : u8 { Miter, Round, Bevel };
enum class Winding : i8 { CCW = -1, CW = 1 };
enum class TextAlign : u8 { Left, Center, Right, Justify };
enum class TextBaseline : u8 { Top, Middle, Bottom, Alphabetic };
enum class TextBreak : u8 { None, Char, Word };

// Описание nine-patch для масштабируемой UI-графики (фоны кнопок и т.п.).
struct NinePatch {
    // Отступы в пикселях в пространстве текстуры.
    f32 left = 0, top = 0, right = 0, bottom = 0;
    static NinePatch Uniform(f32 v) {
        NinePatch n;
        n.left = n.top = n.right = n.bottom = v;
        return n;
    }
};

struct FontStyle;

// ---------------------------------------------------------------------------
// Стилизация rich text
// ---------------------------------------------------------------------------
// Всё, что умеет текстовый рендерер, в одном месте: двухцветные заливки, контуры,
// мягкие тени, свечение, скручивание, трансформации фрагментов и раскладка по пути/дуге.
struct TextStyle {
// Заливка: один цвет либо двухцветный градиент внутренний/внешний, когда
// задан `gradient` (inner у верха глифа, outer у низа, либо радиально от
// `gradientCenter` при `gradientRadial`).
    Color color = Color::White;
    Color innerColor = Color::White;
    Color outerColor = Color{0.35f, 0.55f, 1.0f, 1.0f};
    bool gradient = false;
    bool gradientRadial = false;
    Vec2 gradientCenter{0.5f, 0.5f};

    // Контур (обводка вокруг глифа).
    Color outlineColor{0, 0, 0, 1};
    f32 outlineWidth = 0.0f;     // логические пиксели

    // Внешнее свечение (размытая увеличенная копия позади глифа).
    Color glowColor{1.0f, 0.85f, 0.4f, 0.5f};
    f32 glowRadius = 0.0f;
    f32 glowIntensity = 1.0f;

    // Падающая тень.
    Color shadowColor{0, 0, 0, 0.6f};
    Vec2 shadowOffset{2.0f, 3.0f};
    f32 shadowSoftness = 0.0f;   // >0 = несколько смещённых копий (дешёвое размытие)
    int shadowSamples = 5;       // используется при shadowSoftness > 0

    // Раскладка / трансформация.
    f32 letterSpacing = 0.0f;
    f32 lineHeightMul = 1.2f;
    f32 rotation = 0.0f;         // радианы, весь фрагмент
    Vec2 scale{1, 1};
    Vec2 skew{0, 0};
    f32 twist = 0.0f;            // дополнительный поворот на каждый логический пиксель продвижения
    f32 waveAmplitude = 0.0f;    // вертикальное смещение глифов вдоль фрагмента
    f32 waveFrequency = 0.05f;
    bool bold = false;
    f32 boldAmount = 0.6f;

    // Многострочное поведение (DrawTextBox со стилем).
    TextAlign align = TextAlign::Left;
    TextBaseline baseline = TextBaseline::Top;
    TextBreak wrap = TextBreak::None;
    int maxLines = 0;

    static TextStyle Filled(const Color& c);
    static TextStyle Outlined(const Color& fill, const Color& outline, f32 width);
    static TextStyle Shadowed(const Color& fill, const Vec2& offset = {2, 3});
    static TextStyle GradientText(const Color& inner, const Color& outer);
};

// ---------------------------------------------------------------------------
// Renderer2D
// ---------------------------------------------------------------------------
class Renderer2D {
public:
    Renderer2D();
    ~Renderer2D();
    Renderer2D(const Renderer2D&) = delete;
    Renderer2D& operator=(const Renderer2D&) = delete;

    bool Init();
    void Shutdown();

// `fbWidth`/`fbHeight` заданы в пикселях; `dpiScale` переводит логические
// единицы в физические. Ненулевой `target` — рендер в оффскрин-фреймбуфер.
    void BeginFrame(int fbWidth, int fbHeight, f32 dpiScale = 1.0f, RenderTarget* target = nullptr);
    void EndFrame();
    // Немедленно отправляет всю накопленную геометрию на GPU.
    void Flush();

    // ---- состояние ---------------------------------------------------------
    void Save();
    void Restore();
    void Reset();
    void ResetTransform();
    void ResetState();
    void GlobalAlpha(f32 alpha);
    [[nodiscard]] f32 GetGlobalAlpha() const;
    void Composite(BlendMode mode);
    void AntiAlias(bool enabled);
    void LineCap(LineCap cap);
    void LineJoin(LineJoin join);
    void MiterLimit(f32 limit);
    void StrokeWidth(f32 width);
    void FillPaint(const Paint& paint);
    void StrokePaint(const Paint& paint);
    void FillColor(const Color& c);
    void StrokeColor(const Color& c);
    [[nodiscard]] const Paint& CurrentFill() const;
    [[nodiscard]] const Paint& CurrentStroke() const;

    // ---- трансформация -----------------------------------------------------
    void Translate(f32 x, f32 y);
    void Rotate(f32 radians);
    void Scale(f32 x, f32 y);
    void SkewX(f32 radians);
    void SkewY(f32 radians);
    void Transform(f32 a, f32 b, f32 c, f32 d, f32 e, f32 f);
    [[nodiscard]] Vec2 TransformPoint(const Vec2& p) const;

    // ---- пути ---------------------------------------------------------
    void BeginPath();
    void MoveTo(f32 x, f32 y);
    void LineTo(f32 x, f32 y);
    void BezierTo(f32 c1x, f32 c1y, f32 c2x, f32 c2y, f32 x, f32 y);
    void QuadTo(f32 cx, f32 cy, f32 x, f32 y);
    void ArcTo(f32 x1, f32 y1, f32 x2, f32 y2, f32 radius);
    void Arc(f32 cx, f32 cy, f32 r, f32 a0, f32 a1, Winding dir);
    void ClosePath();
    void PathWinding(Winding dir);
    void Rect(f32 x, f32 y, f32 w, f32 h);
    void RoundedRect(f32 x, f32 y, f32 w, f32 h, f32 r);
    void RoundedRectVarying(f32 x, f32 y, f32 w, f32 h, f32 tl, f32 tr, f32 br, f32 bl);
    void Ellipse(f32 cx, f32 cy, f32 rx, f32 ry);
    void Circle(f32 cx, f32 cy, f32 r);
    void Polyline(const Vec2* pts, int count);
    void Polygon(const Vec2* pts, int count);
    // Открытый путь через точки, сглаженный сплайном Catmull-Rom.
    void Spline(const Vec2* pts, int count, f32 tension = 0.5f);
    void Fill();
    void Stroke();

    // ---- быстрые помощники --------------------------------------------
    void FillRect(f32 x, f32 y, f32 w, f32 h, const Color& c);
    void FillRect(const crossrender::Rect& r, const Color& c) { FillRect(r.x, r.y, r.w, r.h, c); }
    void FillRoundedRect(const crossrender::Rect& r, f32 radius, const Color& c);
    void StrokeRect(const crossrender::Rect& r, const Color& c, f32 width = 1.0f);
    void StrokeRoundedRect(const crossrender::Rect& r, f32 radius, const Color& c, f32 width = 1.0f);
    void FillCircle(f32 cx, f32 cy, f32 r, const Color& c);
    void FillEllipse(f32 cx, f32 cy, f32 rx, f32 ry, const Color& c);
    void DrawLine(f32 x0, f32 y0, f32 x1, f32 y1, const Color& c, f32 width = 1.0f);
    // Вертикальные / горизонтальные / диагональные градиенты.
    void FillRectGradient(const crossrender::Rect& r, const Color& a, const Color& b, bool vertical = true);
    void FillRectGradient4(const crossrender::Rect& r, const Color& tl, const Color& tr, const Color& br,
                           const Color& bl);

    // ---- изображения --------------------------------------------------------
    void Image(const Texture& tex, const crossrender::Rect& dst, const crossrender::Rect& srcUV = crossrender::Rect{0, 0, 1, 1},
               const Color& tint = Color::White, f32 cornerRadius = 0.0f);
    void ImageQuad(const Texture& tex, const Vec2 quad[4], const crossrender::Rect& srcUV, const Color& tint);
    // Масштабирование nine-patch; `scale` умножает отступы (для HiDPI-графики).
    void Image9(const Texture& tex, const crossrender::Rect& dst, const NinePatch& patch,
                const Color& tint = Color::White, f32 scale = 1.0f);
    // Рисует текстуру с тонированием по каждому углу.
    void ImageTinted4(const Texture& tex, const crossrender::Rect& dst, const crossrender::Rect& srcUV, const Color& tl,
                      const Color& tr, const Color& br, const Color& bl);

    // ---- отсечение ------------------------------------------------------
    void ClipRect(f32 x, f32 y, f32 w, f32 h);
    void ClipRoundedRect(f32 x, f32 y, f32 w, f32 h, f32 r);
    void ClipPath();  // отсекает по текущему пути
    void ResetClip();
    [[nodiscard]] crossrender::Rect CurrentClip() const;

    // ---- текст ----------------------------------------------------------
    // Рисует текст UTF-8. `size` — размер em в логических единицах.
    void DrawText(const Font& font, const std::string& utf8, f32 x, f32 y, const Color& color,
                  f32 size = 0.0f, TextAlign align = TextAlign::Left,
                  TextBaseline baseline = TextBaseline::Top, f32 letterSpacing = 0.0f);
    void DrawTextRotated(const Font& font, const std::string& utf8, Vec2 pos, const Color& color,
                         f32 size, f32 rotation, TextAlign align = TextAlign::Center,
                         TextBaseline baseline = TextBaseline::Middle);
    // Текст с переносом внутри `box`. Возвращает число нарисованных строк.
    int DrawTextBox(const Font& font, const std::string& utf8, const crossrender::Rect& box, const Color& color,
                    f32 size = 0.0f, TextBreak brk = TextBreak::Word, f32 lineHeightMul = 1.2f,
                    TextAlign align = TextAlign::Left, int maxLines = 0);
    // Текст с контуром (обводка + заливка).
    void DrawTextOutline(const Font& font, const std::string& utf8, f32 x, f32 y, const Color& fill,
                         const Color& outline, f32 outlineWidth, f32 size = 0.0f,
                         TextAlign align = TextAlign::Left, TextBaseline baseline = TextBaseline::Top);

    // ---- форматированный текст ------------------------------------------------------
// Единая точка входа для всех вспомогательных методов стилизованного текста
// ниже. `pos` — якорь, определяемый style.align / style.baseline.
    void DrawTextStyled(const Font& font, const std::string& utf8, Vec2 pos, f32 size,
                        const TextStyle& style);
    // Стилизованный текст с переносом по словам внутри `box`; возвращает число строк.
    int DrawTextBoxStyled(const Font& font, const std::string& utf8, const crossrender::Rect& box, f32 size,
                          const TextStyle& style);
// Стилизованный по глифам текст вдоль ломаной (параметризация по длине дуги,
// глифы поворачиваются по касательной). `offset` — стартовое смещение по пути.
    void DrawTextOnPath(const Font& font, const std::string& utf8, const Vec2* path, int pathCount,
                        f32 size, const TextStyle& style, f32 offset = 0.0f, bool closed = false);
// Стилизованный текст по круговой дуге. `outside` направляет верха глифов
// от центра; `clockwise` меняет направление обхода.
    void DrawTextOnArc(const Font& font, const std::string& utf8, Vec2 center, f32 radius,
                       f32 startAngle, f32 size, const TextStyle& style, bool outside = true,
                       bool clockwise = false);
    // Удобные обёртки (все построены поверх DrawTextStyled).
    void DrawTextShadow(const Font& font, const std::string& utf8, Vec2 pos, f32 size,
                        const Color& fill, const Color& shadow = Color{0, 0, 0, 0.6f},
                        Vec2 offset = {2, 3}, TextAlign align = TextAlign::Left,
                        TextBaseline baseline = TextBaseline::Top);
    void DrawTextTwisted(const Font& font, const std::string& utf8, Vec2 pos, f32 size,
                         const Color& fill, f32 twist, f32 rotation = 0.0f, f32 waveAmplitude = 0.0f);
    void DrawTextGradient(const Font& font, const std::string& utf8, Vec2 pos, f32 size,
                          const Color& inner, const Color& outer, TextAlign align = TextAlign::Left,
                          TextBaseline baseline = TextBaseline::Top);

    // Измеряет стилизованный текст (учитывает масштаб, межбуквенный интервал и скручивание).
    struct StyledMetrics {
        f32 width = 0;
        f32 height = 0;
        f32 advance = 0;
        int glyphCount = 0;
        int lineCount = 1;
        crossrender::Rect bounds;   // плотные границы краски в локальном пространстве, начало — в якоре
    };
    StyledMetrics MeasureStyledText(const Font& font, const std::string& utf8, f32 size,
                                    const TextStyle& style);

    // ---- статистика ---------------------------------------------------------
    struct Stats {
        int drawCalls = 0;
        int vertices = 0;
        int paths = 0;
        int clipPushes = 0;
        int textGlyphs = 0;
        usize vertexBufferBytes = 0;
    };
    [[nodiscard]] const Stats& GetStats() const { return stats_; }
    void ResetStats() { stats_ = Stats{}; }
    // Рисует пакетную геометрию в виде квазикаркасного отладочного оверлея (счётчики).
    [[nodiscard]] int PendingVertices() const;

    // Матрица проекции текущего кадра (логическое пространство координат).
    [[nodiscard]] const Mat4& Projection() const;
    [[nodiscard]] f32 DpiScale() const;
    [[nodiscard]] Vec2 ScreenSize() const;

// Обходной путь: рисует 3D-содержимое в текущий 2D-кадр в заданный прямоугольник
// (для наложения 2D UI поверх 3D-сцены: см. Renderer3D::BeginViewport).
    void SetViewportBlit(const Texture& colorTex, const crossrender::Rect& dst);

private:
// Рисует один уже позиционированный глиф с полным стеком стилей rich text
// (свечение, тень, контур, градиентная заливка). `localDst` задан относительно
// начала пера, базовая линия при y = 0; `rotation` применяется вокруг начала
// фрагмента; `emScale` — масштаб шрифта px-per-em для текущего размера.
    void DrawStyledGlyph(const Font& font, int page, const crossrender::Rect& localDst,
                         const crossrender::Rect& uv, const TextStyle& style, crossrender::Vec2 runOrigin,
                         f32 rotation, f32 emScale);

    struct Impl;
    Impl* impl_ = nullptr;
    Stats stats_{};
    Mat4 projection_;
    f32 dpiScale_ = 1;
};

// ---------------------------------------------------------------------------
// Утилиты компоновки текста (независимые от состояния Renderer2D)
// ---------------------------------------------------------------------------
struct TextMetrics {
    f32 width = 0;
    f32 height = 0;
    f32 ascender = 0;
    f32 descender = 0;
    int lineCount = 1;
    int glyphCount = 0;
};

TextMetrics MeasureText(const Font& font, const std::string& utf8, f32 size = 0.0f,
                        f32 letterSpacing = 0.0f);
// Разбивает текст на строки с учётом `maxWidth` (логические единицы). Возвращает строки UTF-8.
std::vector<std::string> WrapText(const Font& font, const std::string& utf8, f32 maxWidth,
                                  f32 size = 0.0f, TextBreak brk = TextBreak::Word);
// Обрезает многоточием так, чтобы результат уместился в `maxWidth`.
std::string EllipsizeText(const Font& font, const std::string& utf8, f32 maxWidth, f32 size = 0.0f);
// Индекс глифа (смещение в байтах), ближайшего к локальной координате x.
usize TextIndexAt(const Font& font, const std::string& utf8, f32 x, f32 size = 0.0f);

}  // namespace crossrender
