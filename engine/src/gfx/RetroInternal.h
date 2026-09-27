//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: внутренние помощники ретро-режимов: палитры, пиксельный конвейер и ASCII-сетка.
//
#pragma once

#include "crossrender/core/Base.h"
#include "crossrender/gfx/Retro.h"

namespace crossrender {
namespace retro {

// ---------------------------------------------------------------------------
// Палитры (RetroPalette.cpp)
// ---------------------------------------------------------------------------
// Необработанные таблицы палитр в формате 0xRRGGBB, `*count` — число записей.
// Возвращает nullptr для RetroPalette::None / Count (count = 0).
const u32* PaletteTable(RetroPalette p, int* count);

// ---------------------------------------------------------------------------
// Общая математика на CPU
// ---------------------------------------------------------------------------
// Относительная яркость по Rec.709. Точную формулу см. в описании `AsciiGrid`.
inline f32 Luminance(f32 r, f32 g, f32 b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }

// Число ASCII-строк, используемое при RetroSettings::asciiRows == 0.
//   rows = round(cols * (imageH / imageW) / cellAspect)
int DeriveAsciiRows(int cols, int width, int height, f32 cellAspect);

// Один шаг пиксельного конвейера, в точности повторяющий итоговый
// фрагментный шейдер в Retro.cpp:
//   насыщенность -> контраст -> яркость -> упорядоченный дизеринг ->
//   глубина цвета -> ближайший элемент палитры.
// `palette`/`paletteSize` могут быть null/0 (RetroPalette::None).
void ProcessPixel(u8* rgba, int x, int y, const RetroSettings& s, const Color* palette,
                  int paletteSize);

}  // namespace retro
}  // namespace crossrender
