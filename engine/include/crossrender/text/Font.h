//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: шрифты: загрузка TrueType/OpenType с нуля, растеризация и SDF-атлас глифов.
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

class Renderer2D;

// Разновидность источника контуров шрифта (информационно).
enum class FontFormat : u8 { Unknown, TrueType, OpenTypeCFF, Collection, Bitmap };

struct FontDesc {
    f32 pixelHeight = 48.0f;   // размер растеризации в пикселях
    bool sdf = false;          // строить поля знакового расстояния вместо битмапов
    f32 sdfSpread = 6.0f;      // диапазон расстояний SDF в пикселях
    bool hinting = true;       // привязка штрихов/граней к сетке для более чёткого мелкого текста
    u32 atlasSize = 1024;      // сторона текстуры атласа (растёт за счёт новых страниц)
    int atlasPadding = 2;
    u32 firstCodepoint = 32;
    u32 lastCodepoint = 0x2FFF;   // диапазон предварительной запечки (остальное растеризуется по запросу)
    bool prebake = false;         // запечь весь диапазон заранее (медленнее старт)
    f32 gamma = 1.0f;
    bool bold = false;            // синтетический жирный (расширение штриха)
    f32 boldAmount = 0.6f;
    bool italic = false;          // синтетический курсив (сдвиг)
    f32 italicSlant = 0.25f;
    u32 oversample = 2;           // суперсэмплинг для битмапных глифов (1..4)
};

// Один растеризованный глиф внутри страницы атласа.
struct Glyph {
    u32 codepoint = 0;
    f32 advance = 0;       // в пикселях при `pixelHeight`
    f32 bearingX = 0;      // левое опорное расстояние
    f32 bearingY = 0;      // верхнее опорное расстояние
    f32 width = 0;         // ширина изображения глифа
    f32 height = 0;        // высота изображения глифа
    // Размещение в атласе (пиксели, начало в левом верхнем углу, y вниз).
    int page = 0;
    f32 u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    bool isEmpty() const { return width <= 0 || height <= 0; }
};

// ---------------------------------------------------------------------------
// Векторные контуры (используются аналитическим текстовым рендерером в стиле Slug)
// ---------------------------------------------------------------------------
struct GlyphPoint {
    Vec2 p;
    u8 onCurve = 1;   // 1 = точка на кривой, 0 = квадратичная контрольная точка
};

struct GlyphContour {
    std::vector<GlyphPoint> points;
    bool closed = true;
};

struct GlyphOutline {
    // Контуры в пикселях запрошенного размера, y вниз, (0,0) в позиции пера
    // на базовой линии. Точки чередуются на/вне кривой; замкнутый контур
    // всегда начинается и заканчивается на кривой.
    std::vector<GlyphContour> contours;
    Rect bounds{};      // границы изображения глифа в том же пространстве
    f32 advance = 0;
    bool empty = true;
};

// Страница атласа глифов.
struct FontAtlasPage {
    Texture texture;
    int usedWidth = 0, usedHeight = 0, rowHeight = 0;
    int size = 0;
};

struct GlyphQuad {
    Rect dst;      // в пикселях относительно позиции пера на базовой линии
    Rect uv;
    int page = 0;
};

// ---------------------------------------------------------------------------
// Шрифт
// ---------------------------------------------------------------------------
class Font {
public:
    // Даёт внутренним помощникам модуля (engine/src/text/FontInternal.h)
    // доступ к Font::Impl, ленивому кэшу глифов и страницам атласа, чтобы
    // растеризатор только на CPU можно было тестировать без GL-контекста.
    friend struct FontTestAccess;

    Font();
    ~Font();
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;

    bool LoadFromFile(const std::string& path, const FontDesc& desc = {});
    bool LoadFromMemory(const void* data, usize size, const FontDesc& desc = {});
    void Destroy();

    [[nodiscard]] bool Valid() const { return valid_; }
    [[nodiscard]] bool IsSdf() const { return desc_.sdf; }
    [[nodiscard]] FontFormat Format() const { return format_; }
    [[nodiscard]] const FontDesc& Desc() const { return desc_; }
    [[nodiscard]] const std::string& FamilyName() const { return family_; }
    [[nodiscard]] const std::string& StyleName() const { return style_; }
    [[nodiscard]] const std::string& SourcePath() const { return source_; }

    // Метрики при размере растеризации шрифта (в пикселях).
    [[nodiscard]] f32 Ascender() const { return ascender_; }
    [[nodiscard]] f32 Descender() const { return descender_; }
    [[nodiscard]] f32 LineGap() const { return lineGap_; }
    [[nodiscard]] f32 LineHeight() const { return lineHeight_; }
    [[nodiscard]] f32 UnitsPerEm() const { return unitsPerEm_; }
    [[nodiscard]] f32 CapHeight() const { return capHeight_; }
    [[nodiscard]] f32 XHeight() const { return xHeight_; }
    [[nodiscard]] f32 UnderlinePosition() const { return underlinePos_; }
    [[nodiscard]] f32 UnderlineThickness() const { return underlineThick_; }

    // Коэффициент масштабирования от запечённой высоты в пикселях к запрошенному размеру.
    [[nodiscard]] f32 ScaleForSize(f32 size) const {
        f32 base = desc_.pixelHeight > 0 ? desc_.pixelHeight : 1.0f;
        return (size > 0 ? size : base) / base;
    }

    // Поиск глифа (растеризует по запросу и меняет внутренний кэш атласа,
    // поэтому `const` + mutable-кэш). Возвращает nullptr, если глифа нет.
    const Glyph* GetGlyph(u32 codepoint) const;
    // Извлекает векторный контур `codepoint`, масштабированный до `size` пикселей
    // (y вниз, начало в позиции пера на базовой линии, только квадратичные сегменты:
    // кубические сегменты CFF конвертируются). Пустой глиф, например пробел,
    // возвращает true с `empty = true`; false значит, что шрифт вообще не даёт этот глиф.
    bool GetGlyphOutline(u32 codepoint, f32 size, GlyphOutline* out) const;
    // То же, но в единицах шрифта (без масштаба) - удобно, чтобы собрать Slug-атлас один раз.
    bool GetGlyphOutlineUnits(u32 codepoint, GlyphOutline* out) const;

    // Кернинг в единицах шрифта между двумя кодовыми точками.
    [[nodiscard]] f32 GetKerning(u32 left, u32 right) const;
    [[nodiscard]] bool HasGlyph(u32 codepoint) const;
    // Отображает кодовую точку в индекс глифа (0 = .notdef).
    [[nodiscard]] u32 GlyphIndex(u32 codepoint) const;

    [[nodiscard]] int AtlasPageCount() const { return static_cast<int>(pages_.size()); }
    [[nodiscard]] const FontAtlasPage& AtlasPage(int i) const { return pages_[static_cast<usize>(i)]; }
    [[nodiscard]] int GlyphCount() const { return static_cast<int>(glyphCache_.size()); }
    // Заполненность атласа в [0,1] (полезно для тестов).
    [[nodiscard]] f32 AtlasOccupancy() const;

    // Цепочка фолбэков, когда кодовая точка отсутствует (например, латинский шрифт + CJK-шрифт).
    void AddFallback(Font* font);
    // Разрешает `codepoint` через этот шрифт и его фолбэки.
    Font* Resolve(u32 codepoint, const Glyph** glyphOut) const;

    // Предварительно запекает набор кодовых точек (используется тестами / экранами прогрева).
    void Prebake(const u32* codepoints, int count);
    void PrebakeAscii();

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    FontDesc desc_{};
    FontFormat format_ = FontFormat::Unknown;
    bool valid_ = false;
    std::string family_, style_, source_;
    f32 ascender_ = 0, descender_ = 0, lineGap_ = 0, lineHeight_ = 0;
    f32 unitsPerEm_ = 1000, capHeight_ = 0, xHeight_ = 0;
    f32 underlinePos_ = 0, underlineThick_ = 0;
    std::vector<FontAtlasPage> pages_;
    std::unordered_map<u32, Glyph> glyphCache_;
    std::vector<Font*> fallbacks_;
};

// ---------------------------------------------------------------------------
// Менеджер / кэш шрифтов
// ---------------------------------------------------------------------------
class FontManager {
public:
    static FontManager& Get();
    // Загружает (и кэширует по path+desc). Возвращает nullptr при неудаче.
    Font* Load(const std::string& path, const FontDesc& desc = {});
    // Возвращает встроенный процедурный резервный шрифт (всегда доступен).
    Font* DefaultFont();
    Font* DefaultSdfFont();
    void SetDefaultFont(Font* f) { default_ = f; }
    void Clear();
    void Shutdown();

private:
    FontManager() = default;
    std::vector<std::unique_ptr<Font>> owned_;
    std::unordered_map<std::string, Font*> cache_;
    Font* default_ = nullptr;
    Font* defaultSdf_ = nullptr;
};

// ---------------------------------------------------------------------------
// Помощники UTF-8
// ---------------------------------------------------------------------------
// Декодирует одну кодовую точку, продвигая `i`. Невалидные байты дают U+FFFD.
u32 Utf8Decode(const char* s, usize len, usize* i);
std::vector<u32> Utf8ToCodepoints(const std::string& s);
std::string CodepointsToUtf8(const std::vector<u32>& cps);
// Число кодовых точек в строке UTF-8.
usize Utf8Length(const std::string& s);
// Смещение в байтах n-й кодовой точки.
usize Utf8Offset(const std::string& s, usize index);

}  // namespace crossrender
