//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: внутренние помощники растеризатора шрифтов: контуры, развёртка и SDF для тестов.
//
#pragma once

#include "crossrender/core/Base.h"
#include "crossrender/text/Font.h"

#include <vector>

namespace crossrender {

// ---------------------------------------------------------------------------
// Хуки для тестов и утилит
// ---------------------------------------------------------------------------
// Растеризует один кодовый символ из образа шрифта в памяти без создания
// каких-либо GPU-ресурсов (текстура атласа не затрагивается).  `out` получает
// полную битовую карту чернил (width*height байтов, построчно, начало слева
// сверху, 0..255). Возвращает false для некорректных данных или отсутствующего
// глифа; при успехе `*w`/`*h` содержат размеры карты (любая может быть нулём
// для пустого глифа вроде пробела).
bool RasteriseGlyphForTest(const void* fontData, usize size, u32 codepoint, std::vector<u8>* out,
                           int* w, int* h, bool sdf);

// Измеряет UTF-8 строку шрифтом (ширины шага + кернинг, в пикселях).
f32 MeasureTextWidth(Font& font, const std::string& text);

// Хук дружбы: позволяет внутренним (и тестовым) помощникам добраться до
// Font::Impl и приватного кэша, не расширяя публичный API.
struct FontTestAccess;

namespace textdetail {

// Вершина контура: `on` отличает точки на кривой от опорных точек
// квадратичных/кубических кривых.  Единицы — какие использовал вызывающий
// (единицы шрифта внутри загрузчиков, пиксели после масштабирования).
struct Vec2f {
    f32 x = 0, y = 0;
    bool on = true;
};

enum class CmdType : u8 { MoveTo, LineTo, QuadTo, CubicTo };

// Плоская команда рисования.  QuadTo/CubicTo несут свои опорные точки; конечная
// точка — всегда первая точка *следующей* команды (или начало Close).
struct Cmd {
    CmdType type = CmdType::MoveTo;
    Vec2f p[3]{};
    bool close = false;
};

// Собирает синтетический («процедурный») шрифт без файла шрифта на диске.
std::unique_ptr<Font> CreateProceduralFont(const FontDesc& desc);

// --- Встроенный ~8x8 ASCII растровый шрифт (см. FontShapes.cpp) ------------
// `row` — это 0 (верх) .. 7 (низ); младшие 5 битов хранят пиксели, бит 4 —
// крайний слева.  Возвращает false для непредставимых таблицей кодовых символов.
bool BuiltinGlyphRows(u32 codepoint, u8 rows[7]);
constexpr int kBuiltinGlyphWidth = 5;
constexpr int kBuiltinGlyphHeight = 7;

}  // namespace textdetail

// Хук дружбы, реализованный в Font.cpp: даёт внутренним помощникам и тестам
// модуля доступ к Font::Impl, ленивому кэшу глифов и страницам атласа.

}  // namespace crossrender
