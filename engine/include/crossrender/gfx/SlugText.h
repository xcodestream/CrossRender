//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: векторный текст из контуров глифов: аналитический рендеринг в стиле Slug.
//
#pragma once

#include "crossrender/core/Base.h"
#include "crossrender/core/Math.h"
#include "crossrender/gfx/Texture.h"

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

namespace crossrender {

class Font;
class Renderer2D;
class Shader;

// Один сегмент квадратичной Безье в пространстве глифа (единицы шрифта,
// масштабированные в фиксированный em-квадрат, +y вниз).
struct SlugCurve {
    Vec2 p0, p1, p2;   // начало, контрольная точка, конец
};

// Глиф, подготовленный для Slug-рендеринга.
struct SlugGlyph {
    u32 codepoint = 0;
    f32 advance = 0;
    Rect bounds;         // плотные границы краски в em-единицах
    int firstCurve = 0;  // индекс в текстуре кривых
    int curveCount = 0;
    int firstBand = 0;
    int bandCount = 0;
    u32 bandOffsetX = 0, bandOffsetY = 0;   // левый верх блока полос глифа
    bool empty = true;
};

// Параметры раскладки фрагмента Slug-текста.
struct SlugTextStyle {
    Color color{1, 1, 1, 1};
    Color outlineColor{0, 0, 0, 1};
    f32 outlineWidth = 0.0f;      // в em-единицах
    Color shadowColor{0, 0, 0, 0.6f};
    Vec2 shadowOffset{0, 0};      // em-единицы
    f32 softness = 1.0f;          // ширина сглаживания (AA) в пикселях
    f32 dilation = 0.0f;          // дополнительная толщина в em-единицах
    f32 rotation = 0.0f;          // радианы, весь фрагмент
    Vec2 scale{1, 1};
    f32 twist = 0.0f;             // рампа поворота на em: rotation += twist * x
    bool billboard = false;
};

class SlugTextRenderer {
public:
    SlugTextRenderer();
    ~SlugTextRenderer();
    SlugTextRenderer(const SlugTextRenderer&) = delete;
    SlugTextRenderer& operator=(const SlugTextRenderer&) = delete;

    // `font` должен оставаться живым; контуры кэшируются по паре (шрифт, размер).
    bool Init(Font* font, int emResolution = 64);
    void Shutdown();
    [[nodiscard]] bool Valid() const;

// Ничего не растеризует: всё аналитически. Возвращает false, если у глифа
// нет контура (например, пробел).
    bool PrepareGlyph(u32 codepoint);
    [[nodiscard]] const SlugGlyph* GetGlyph(u32 codepoint) const;
    [[nodiscard]] int GlyphCount() const { return static_cast<int>(glyphs_.size()); }
    [[nodiscard]] int CurveCount() const { return static_cast<int>(curves_.size()); }
    [[nodiscard]] int BandCount() const { return static_cast<int>(bands_.size()); }

    // Измеряет строку UTF-8 в em-единицах при масштабе 1.
    f32 Measure(const std::string& utf8, f32 letterSpacing = 0.0f, f32 size = 0.0f);
// Рисует строку UTF-8 с базовой линией в `baseline` (логические пиксели).
// Возвращает фактически нарисованную ширину продвижения.
    f32 Draw(Renderer2D& r2d, const std::string& utf8, Vec2 baseline, f32 pixelSize,
             const SlugTextStyle& style);
// Рисует вдоль ломаной (параметризация по длине дуги, глифы поворачиваются
// по касательной). `path` задан в логических пикселях.
    f32 DrawOnPath(Renderer2D& r2d, const std::string& utf8, const Vec2* path, int pathCount,
                   f32 pixelSize, const SlugTextStyle& style);
    // Рисует по круговой дуге с центром в `center`.
    f32 DrawOnArc(Renderer2D& r2d, const std::string& utf8, Vec2 center, f32 radius,
                  f32 startAngle, f32 pixelSize, const SlugTextStyle& style,
                  bool outside = true);

    // Загружает ожидающие кривые/полосы; вызывается автоматически из Draw.
    void Flush();

    struct Stats {
        int glyphsDrawn = 0;
        int curvesEvaluated = 0;
        int quads = 0;
        f32 uploadMs = 0;
    };
    [[nodiscard]] const Stats& GetStats() const { return stats_; }
    void ResetStats() { stats_ = Stats{}; }

// CPU-фолбэк при отсутствии GL-контекста (тесты): вычисляет покрытие пикселя
// тем же тестом луча по квадратичным кривым, что и шейдер.
    static f32 CoverageAt(const std::vector<SlugCurve>& curves, Vec2 point, f32 aaWidth = 1.0f);
    // Число оборотов точки `point` относительно кривых (>0 внутри при nonzero-заливке).
    static f32 WindingAt(const std::vector<SlugCurve>& curves, Vec2 point);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    Font* font_ = nullptr;
    int emResolution_ = 64;
    std::vector<SlugCurve> curves_;
    std::vector<u32> bands_;   // пары (firstCurve, curveCount), развёрнутые в плоский список
    std::vector<SlugGlyph> glyphs_;
    std::unordered_map<u32, int> glyphIndex_;
    Texture curveTexture_;
    Texture bandTexture_;
    int curveCursor_ = 0;
    int bandCursor_ = 0;
    int bandOffsetX_ = 0, bandOffsetY_ = 0;
    bool dirty_ = false;
    Stats stats_{};
};

}  // namespace crossrender
