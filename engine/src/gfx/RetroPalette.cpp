// Retro display — цветовые палитры и пиксельный конвейер на стороне CPU.
//
// Каждая таблица ниже — вручную перенесённая копия палитры реального железа
// (источники указаны у таблиц).  CPU-конвейер здесь повторяет GLSL в
// Retro.cpp один в один, чтобы headless-утилиты и GPU-расчёт совпадали.
#include "crossrender/gfx/Retro.h"

#include "RetroInternal.h"

#include <cmath>

namespace crossrender {
namespace retro {
namespace {

// ---------------------------------------------------------------------------
// NES / Famicom (Ricoh 2C02)
// ---------------------------------------------------------------------------
// Перенесено из канонической 64-записной NTSC-таблицы 2C02 (вики nesdev,
// «PPU palettes»; та же таблица поставляется с FCEUX/Nestopia).  Десять из
// 64 записей — чистый чёрный ($0D,$0E,$0F,$1D,$1E,$1F,$2E,$2F,$3E,$3F) и
// схлопываются в один цвет; остаётся 55 различных значений.  $2D (#787878)
// отличается от $00 (#7C7C7C) на 4/255 и объединяется с ним, что даёт
// традиционные 54 используемых цвета NES.
const u32 kNeS[] = {
    0x7C7C7C, 0x0000FC, 0x0000BC, 0x4428BC, 0x940084, 0xA80020, 0xA81000, 0x881400, 0x503000,
    0x007800, 0x006800, 0x005800, 0x004058, 0x000000, 0xBCBCBC, 0x0078F8, 0x0058F8, 0x6844FC,
    0xD800CC, 0xE40058, 0xF83800, 0xE45C10, 0xAC7C00, 0x00B800, 0x00A800, 0x00A844, 0x008888,
    0xF8F8F8, 0x3CBCFC, 0x6888FC, 0x9878F8, 0xF878F8, 0xF85898, 0xF87858, 0xFCA044, 0xF8B800,
    0xB8F818, 0x58D854, 0x58F898, 0x00E8D8, 0xFCFCFC, 0xA4E4FC, 0xB8B8F8, 0xD8B8F8, 0xF8B8F8,
    0xF8A4C0, 0xF0D0B0, 0xFCE0A8, 0xF8D878, 0xD8F878, 0xB8F8B8, 0xB8F8D8, 0x00FCFC, 0xF8D8F8,
};

// Nintendo DMG (оригинальный Game Boy) — четыре оттенка, опубликованные для
// LCD DMG-01 (оливково-зелёный).
const u32 kGameBoy[] = {0x0F380F, 0x306230, 0x8BAC0F, 0x9BBC0F};

// Game Boy Pocket / Light — LCD с оттенками серого.
const u32 kGameBoyPocket[] = {0x000000, 0x555555, 0xAAAAAA, 0xFFFFFF};

// 16-цветная текстовая палитра IBM CGA / EGA (IRGB, коричневый в индексе 6).
const u32 kCga16[] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

// 64-цветный режим EGA: полный RGB-куб 4x4x4 на уровнях EGA DAC
// 0x00/0x55/0xAA/0xFF (все 64 цвета доступны, без усечения).
const u32 kEga64[64] = {
    0x000000, 0x000055, 0x0000AA, 0x0000FF, 0x005500, 0x005555, 0x0055AA, 0x0055FF,
    0x00AA00, 0x00AA55, 0x00AAAA, 0x00AAFF, 0x00FF00, 0x00FF55, 0x00FFAA, 0x00FFFF,
    0x550000, 0x550055, 0x5500AA, 0x5500FF, 0x555500, 0x555555, 0x5555AA, 0x5555FF,
    0x55AA00, 0x55AA55, 0x55AAAA, 0x55AAFF, 0x55FF00, 0x55FF55, 0x55FFAA, 0x55FFFF,
    0xAA0000, 0xAA0055, 0xAA00AA, 0xAA00FF, 0xAA5500, 0xAA5555, 0xAA55AA, 0xAA55FF,
    0xAAAA00, 0xAAAA55, 0xAAAAAA, 0xAAAAFF, 0xAAFF00, 0xAAFF55, 0xAAFFAA, 0xAAFFFF,
    0xFF0000, 0xFF0055, 0xFF00AA, 0xFF00FF, 0xFF5500, 0xFF5555, 0xFF55AA, 0xFF55FF,
    0xFFAA00, 0xFFAA55, 0xFFAAAA, 0xFFAAFF, 0xFFFF00, 0xFFFF55, 0xFFFFAA, 0xFFFFFF,
};

// Commodore 64 — откалиброванная палитра VIC-II от Pepto (фактический эталон).
const u32 kC64[] = {
    0x000000, 0xFFFFFF, 0x68372B, 0x70A4B2, 0x6F3D86, 0x588D43, 0x352879, 0xB8C76F,
    0x6F4F25, 0x433900, 0x9A6759, 0x444444, 0x6C6C6C, 0x9AD284, 0x6C5EB5, 0x959595,
};

// Фэнтези-консоль PICO-8, официальная 16-цветная палитра.
const u32 kPico8[] = {
    0x000000, 0x1D2B53, 0x7E2553, 0x008751, 0xAB5236, 0x5F574F, 0xC2C3C7, 0xFFF1E8,
    0xFF004D, 0xFFA300, 0xFFEC27, 0x00E436, 0x29ADFF, 0x83769C, 0xFF77A8, 0xFFCCAA,
};

// Amstrad CPC (AY-3-8912 / Gate Array) — три уровня на канал, 27 цветов.
const u32 kAmstrad32[27] = {
    0x000000, 0x000080, 0x0000FF, 0x800000, 0x800080, 0x8000FF, 0xFF0000, 0xFF0080, 0xFF00FF,
    0x008000, 0x008080, 0x0080FF, 0x808000, 0x808080, 0x8080FF, 0xFF8000, 0xFF8080, 0xFF80FF,
    0x00FF00, 0x00FF80, 0x00FFFF, 0x80FF00, 0x80FF80, 0x80FFFF, 0xFFFF00, 0xFFFF80, 0xFFFFFF,
};

// ZX Spectrum ULA — 8 оттенков при двух уровнях яркости; яркий чёрный
// совпадает с обычным чёрным, поэтому 15 уникальных цветов.
const u32 kZxSpectrum[] = {
    0x000000, 0x0000D7, 0xD70000, 0xD700D7, 0x00D700, 0x00D7D7, 0xD7D700, 0xD7D7D7,
    0x0000FF, 0xFF0000, 0xFF00FF, 0x00FF00, 0x00FFFF, 0xFFFF00, 0xFFFFFF,
};

const u32 kMono[] = {0x000000, 0xFFFFFF};

// Nintendo Virtual Boy — четыре оттенка красного на LED-дисплее.
const u32 kVirtualBoy[] = {0x000000, 0x550000, 0xAA0000, 0xFF0000};

struct PaletteEntry {
    const u32* data;
    int count;
    const char* name;
};

const PaletteEntry kPalettes[static_cast<int>(RetroPalette::Count)] = {
    {nullptr, 0, "None"},
    {kNeS, 54, "NES"},
    {kGameBoy, 4, "Game Boy"},
    {kGameBoyPocket, 4, "Game Boy Pocket"},
    {kCga16, 16, "CGA 16"},
    {kEga64, 64, "EGA 64"},
    {kC64, 16, "Commodore 64"},
    {kPico8, 16, "PICO-8"},
    {kAmstrad32, 27, "Amstrad CPC"},
    {kZxSpectrum, 15, "ZX Spectrum"},
    {kMono, 2, "Monochrome"},
    {kVirtualBoy, 4, "Virtual Boy"},
};

}  // namespace

const u32* PaletteTable(RetroPalette p, int* count) {
    int idx = static_cast<int>(p);
    if (idx < 0 || idx >= static_cast<int>(RetroPalette::Count)) {
        if (count) *count = 0;
        return nullptr;
    }
    if (count) *count = kPalettes[idx].count;
    return kPalettes[idx].data;
}

namespace {

// Ближайший элемент палитры с перцептивными весами.  Веса из публичного контракта:
// R 0.30, G 0.59, B 0.11 (примерно люминансные веса Rec.601), поэтому ошибки оттенка
// в зелёном канале доминируют, а синий — самый дешёвый.
int NearestIndex(const u32* table, int count, f32 r, f32 g, f32 b) {
    int best = 0;
    f32 bestDist = 1e30f;
    for (int i = 0; i < count; ++i) {
        f32 pr = static_cast<f32>((table[i] >> 16) & 0xFF) / 255.0f;
        f32 pg = static_cast<f32>((table[i] >> 8) & 0xFF) / 255.0f;
        f32 pb = static_cast<f32>(table[i] & 0xFF) / 255.0f;
        f32 dr = (r - pr) * 0.30f;
        f32 dg = (g - pg) * 0.59f;
        f32 db = (b - pb) * 0.11f;
        f32 d = dr * dr + dg * dg + db * db;
        if (d < bestDist) {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

int NearestIndexColors(const Color* table, int count, f32 r, f32 g, f32 b) {
    int best = 0;
    f32 bestDist = 1e30f;
    for (int i = 0; i < count; ++i) {
        f32 dr = (r - table[i].r) * 0.30f;
        f32 dg = (g - table[i].g) * 0.59f;
        f32 db = (b - table[i].b) * 0.11f;
        f32 d = dr * dr + dg * dg + db * db;
        if (d < bestDist) {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

}  // namespace

int DeriveAsciiRows(int cols, int width, int height, f32 cellAspect) {
    if (cols <= 0 || width <= 0 || height <= 0) return 1;
    f32 aspect = Clamp(cellAspect, 0.2f, 2.0f);
    f32 rows = static_cast<f32>(cols) * (static_cast<f32>(height) / static_cast<f32>(width)) / aspect;
    int r = static_cast<int>(std::lround(rows));
    return Clamp(r, 1, 512);
}

void ProcessPixel(u8* rgba, int x, int y, const RetroSettings& s, const Color* palette,
                  int paletteSize) {
    f32 r = static_cast<f32>(rgba[0]) / 255.0f;
    f32 g = static_cast<f32>(rgba[1]) / 255.0f;
    f32 b = static_cast<f32>(rgba[2]) / 255.0f;

    // 1. насыщенность (в display-пространстве; см. заметку про «sRGB-ish» в Retro.cpp).
    f32 lum = Luminance(r, g, b);
    r = lum + (r - lum) * s.saturation;
    g = lum + (g - lum) * s.saturation;
    b = lum + (b - lum) * s.saturation;

    // 2. контраст, 3. яркость.
    r = (r - 0.5f) * s.contrast + 0.5f;
    g = (g - 0.5f) * s.contrast + 0.5f;
    b = (b - 0.5f) * s.contrast + 0.5f;
    r = Clamp(r * s.brightness, 0.0f, 1.0f);
    g = Clamp(g * s.brightness, 0.0f, 1.0f);
    b = Clamp(b * s.brightness, 0.0f, 1.0f);

    // 4. глубина цвета (бит на канал).
    int bits = Clamp(static_cast<int>(s.colorDepth + 0.5f), 2, 6);
    bool useDepth = s.colorDepth >= 0.5f;
    bool usePalette = palette != nullptr && paletteSize > 0;

    // 5. упорядоченный дизеринг со смещением на виртуальный пиксель, чтобы узор
    //    был привязан к низкоразрешённому изображению, а не к окну.  Имеет смысл
    //    только когда что-то квантует результат; амплитуда равна одному шагу
    //    палитры (или одному шагу глубины цвета, если палитры нет).
    if (s.dither && (usePalette || useDepth)) {
        f32 denom = usePalette ? static_cast<f32>(MaxT(paletteSize, 2))
                               : static_cast<f32>(1 << bits);
        f32 amp = s.ditherStrength / denom;
        f32 t = (RetroDisplay::BayerThreshold(x, y, s.ditherMatrix) - 0.5f) * amp;
        r = Clamp(r + t, 0.0f, 1.0f);
        g = Clamp(g + t, 0.0f, 1.0f);
        b = Clamp(b + t, 0.0f, 1.0f);
    }

    // 6. квантование глубины цвета.
    if (useDepth) {
        f32 levels = static_cast<f32>((1 << bits) - 1);
        r = std::floor(r * levels + 0.5f) / levels;
        g = std::floor(g * levels + 0.5f) / levels;
        b = std::floor(b * levels + 0.5f) / levels;
    }

    // 7. квантование по палитре.
    if (usePalette) {
        int best = NearestIndexColors(palette, paletteSize, r, g, b);
        r = palette[best].r;
        g = palette[best].g;
        b = palette[best].b;
    }

    auto toByte = [](f32 v) {
        return static_cast<u8>(Clamp(v * 255.0f + 0.5f, 0.0f, 255.0f));
    };
    rgba[0] = toByte(r);
    rgba[1] = toByte(g);
    rgba[2] = toByte(b);
}

}  // namespace retro

// ---------------------------------------------------------------------------
// Публичный API палитр
// ---------------------------------------------------------------------------
int RetroDisplay::PaletteSize(RetroPalette p) {
    int n = 0;
    retro::PaletteTable(p, &n);
    return n;
}

Color RetroDisplay::PaletteColor(RetroPalette p, int index) {
    int n = 0;
    const u32* table = retro::PaletteTable(p, &n);
    if (!table || n <= 0) return Color::White;
    int i = Clamp(index, 0, n - 1);
    return Color::FromRGB(table[i]);
}

const char* RetroDisplay::PaletteName(RetroPalette p) {
    int idx = static_cast<int>(p);
    if (idx < 0 || idx >= static_cast<int>(RetroPalette::Count)) return "Unknown";
    return retro::kPalettes[idx].name;
}

Color RetroDisplay::Quantize(RetroPalette p, const Color& c) {
    int n = 0;
    const u32* table = retro::PaletteTable(p, &n);
    if (!table || n <= 0) return c;
    int idx = retro::NearestIndex(table, n, Clamp(c.r, 0.0f, 1.0f), Clamp(c.g, 0.0f, 1.0f),
                           Clamp(c.b, 0.0f, 1.0f));
    return Color::FromRGB(table[idx]);
}

void RetroDisplay::ApplyPalette(u8* rgba, int width, int height, const RetroSettings& s) {
    if (!rgba || width <= 0 || height <= 0) return;
    int paletteSize = 0;
    const u32* table = retro::PaletteTable(s.palette, &paletteSize);
    std::vector<Color> palette;
    if (table && paletteSize > 0) {
        palette.reserve(static_cast<usize>(paletteSize));
        for (int i = 0; i < paletteSize; ++i) palette.push_back(Color::FromRGB(table[i]));
    }
    const Color* pal = palette.empty() ? nullptr : palette.data();
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            retro::ProcessPixel(rgba + (static_cast<usize>(y) * static_cast<usize>(width) +
                                 static_cast<usize>(x)) * 4u,
                         x, y, s, pal, static_cast<int>(palette.size()));
        }
    }
}

}  // namespace crossrender
