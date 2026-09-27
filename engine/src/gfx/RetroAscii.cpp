// Retro display — ASCII-градационные ряды и сетка яркости на стороне CPU.
//
// Математика в BuildAsciiGrid — точный CPU-двойник ASCII-фрагментного
// шейдера в Retro.cpp; общая формула задокументирована у функции.
#include "crossrender/gfx/Retro.h"

#include "RetroInternal.h"

#include "crossrender/text/Font.h"

#include <cmath>

namespace crossrender {
namespace retro {
namespace {

// Классический 70-уровневый ASCII-ряд (широко известный список "$@B%8&WM#... ."),
// развёрнутый так, что первым идёт самый тёмный символ. Порядок перцептивный,
// а не по измерению по битмапу — см. контракт CharsetRamp «сначала самые тёмные».
const char* kRamp70Utf8 =
    R"RAMP( .'`^",:;Il!i><~+_-?][}{1)(|\/tfjrxnuvczXYUJCLQ0OZmwqpdbkhao*#MW&8%B@$)RAMP";

const char* kRamp10Utf8 = " .:-=+*#%@";

// Элементы блоков Unicode: от пустого к полностью залитому.
const u32 kBlocks[] = {0x0020, 0x2591, 0x2592, 0x2593, 0x2588};

// Узоры Брайля, точки добавляются по одной (U+2800 + битовое поле 2x4 точек).
const u32 kBraille[] = {0x2800, 0x2801, 0x2803, 0x2807, 0x280F,
                        0x281F, 0x283F, 0x287F, 0x28FF};

}  // namespace

}  // namespace retro

std::vector<u32> RetroDisplay::CharsetRamp(AsciiCharset charset, const std::string& custom) {
    std::vector<u32> out;
    switch (charset) {
        case AsciiCharset::Ramp10:
            out = Utf8ToCodepoints(retro::kRamp10Utf8);
            break;
        case AsciiCharset::Ramp70:
            out = Utf8ToCodepoints(retro::kRamp70Utf8);
            break;
        case AsciiCharset::Blocks:
            out.assign(retro::kBlocks, retro::kBlocks + sizeof(retro::kBlocks) / sizeof(retro::kBlocks[0]));
            break;
        case AsciiCharset::Braille:
            out.assign(retro::kBraille, retro::kBraille + sizeof(retro::kBraille) / sizeof(retro::kBraille[0]));
            break;
        case AsciiCharset::Custom:
            out = Utf8ToCodepoints(custom);
            break;
        default:
            break;
    }
    // Ряду нужны как минимум два уровня; пустой/сломанный пользовательский ряд
    // откатывается к 10-уровневому, чтобы ASCII-режим всегда мог что-то нарисовать.
    if (out.size() < 2) out = Utf8ToCodepoints(retro::kRamp10Utf8);
    return out;
}

// ---------------------------------------------------------------------------
// BuildAsciiGrid
// ---------------------------------------------------------------------------
// Точная математика для каждой ячейки (идентична ASCII-фрагментному шейдеру):
//
//   cellW = width / cols, cellH = height / rows          (в пикселях источника)
//   centre = ((cx + 0.5) * cellW, (cy + 0.5) * cellH)
//   9 выборок в точках centre + (dx, dy) * 0.25 * (cellW, cellH), dx/dy из {-1, 0, 1}
//   texel index = clamp(floor(tap), 0, size - 1)         (GL_NEAREST)
//   avg     = среднее 9 выборок (RGB, нормировано в [0,1])
//   lum     = 0.2126 * avg.r + 0.7152 * avg.g + 0.0722 * avg.b      (Rec.709)
//   lum     = pow(clamp(lum, 0, 1), 1 / max(gamma, 0.01))
//   lum     = clamp((lum - 0.5) * contrast + 0.5, 0, 1)
//   lum     = clamp(lum + brightness, 0, 1)
//   level   = round(lum * 255)                            (u8, 0..255)
//
// Отображение уровня -> индекса ряда, используемое рендерером:
//   index = min(int(floor(lum * N)), N - 1)   (N = длина ряда)
//   index = invert ? N - 1 - index : index
// Именно это делает шейдер; BuildAsciiGrid намеренно возвращает сырой
// (не инвертированный) уровень, чтобы вызывающий применил свой ряд.
RetroDisplay::AsciiGrid RetroDisplay::BuildAsciiGrid(const u8* rgba, int width, int height,
                                                    const RetroSettings& s) {
    AsciiGrid grid;
    if (!rgba || width <= 0 || height <= 0) return grid;

    int cols = Clamp(s.asciiCols, 1, 512);
    int rows = s.asciiRows > 0 ? Clamp(s.asciiRows, 1, 512)
                               : retro::DeriveAsciiRows(cols, width, height, s.asciiCellAspect);
    grid.cols = cols;
    grid.rows = rows;
    grid.levels.assign(static_cast<usize>(cols) * static_cast<usize>(rows), 0);
    grid.colors.assign(static_cast<usize>(cols) * static_cast<usize>(rows), Color{});

    f32 cellW = static_cast<f32>(width) / static_cast<f32>(cols);
    f32 cellH = static_cast<f32>(height) / static_cast<f32>(rows);
    f32 gamma = s.asciiGamma > 0.01f ? s.asciiGamma : 0.01f;

    for (int cy = 0; cy < rows; ++cy) {
        for (int cx = 0; cx < cols; ++cx) {
            f32 centreX = (static_cast<f32>(cx) + 0.5f) * cellW;
            f32 centreY = (static_cast<f32>(cy) + 0.5f) * cellH;
            f32 sumR = 0, sumG = 0, sumB = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    int px = static_cast<int>(
                        std::floor(centreX + static_cast<f32>(dx) * cellW * 0.25f));
                    int py = static_cast<int>(
                        std::floor(centreY + static_cast<f32>(dy) * cellH * 0.25f));
                    px = Clamp(px, 0, width - 1);
                    py = Clamp(py, 0, height - 1);
                    usize idx = (static_cast<usize>(py) * static_cast<usize>(width) +
                                 static_cast<usize>(px)) * 4u;
                    sumR += static_cast<f32>(rgba[idx + 0]) / 255.0f;
                    sumG += static_cast<f32>(rgba[idx + 1]) / 255.0f;
                    sumB += static_cast<f32>(rgba[idx + 2]) / 255.0f;
                }
            }
            Color avg{sumR / 9.0f, sumG / 9.0f, sumB / 9.0f, 1.0f};
            f32 lum = retro::Luminance(avg.r, avg.g, avg.b);
            lum = std::pow(Clamp(lum, 0.0f, 1.0f), 1.0f / gamma);
            lum = Clamp((lum - 0.5f) * s.asciiContrast + 0.5f, 0.0f, 1.0f);
            lum = Clamp(lum + s.asciiBrightness, 0.0f, 1.0f);

            int i = grid.Index(cx, cy);
            grid.levels[static_cast<usize>(i)] =
                static_cast<u8>(Clamp(lum * 255.0f + 0.5f, 0.0f, 255.0f));
            grid.colors[static_cast<usize>(i)] = avg;
        }
    }
    return grid;
}

}  // namespace crossrender
