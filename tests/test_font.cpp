// Тесты текстового модуля: помощники UTF-8, процедурный встроенный шрифт и
// полностью синтетический TrueType-шрифт в памяти, нагружающий парсер `glyf`,
// растеризатор покрытия и генератор SDF без каких-либо ассетов на диске
// (и без GL-контекста).
#include "text/FontInternal.h"
#include "crossrender/core/File.h"
#include "crossrender/test/Test.h"
#include "crossrender/text/Font.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include <cstring>
#include <fstream>
#include <algorithm>

using namespace crossrender;

namespace {

// ---------------------------------------------------------------------------
// Минимальный big-endian-райтер для сборщика синтетического шрифта.
// ---------------------------------------------------------------------------
struct Writer {
    std::vector<u8> b;

    usize Size() const { return b.size(); }
    void U8(u8 v) { b.push_back(v); }
    void U16(u16 v) {
        b.push_back(static_cast<u8>(v >> 8));
        b.push_back(static_cast<u8>(v & 0xFF));
    }
    void I16(i16 v) { U16(static_cast<u16>(v)); }
    void U32(u32 v) {
        b.push_back(static_cast<u8>(v >> 24));
        b.push_back(static_cast<u8>((v >> 16) & 0xFF));
        b.push_back(static_cast<u8>((v >> 8) & 0xFF));
        b.push_back(static_cast<u8>(v & 0xFF));
    }
    void Bytes(const void* p, usize n) {
        const u8* q = static_cast<const u8*>(p);
        b.insert(b.end(), q, q + n);
    }
    void Pad4() {
        while (b.size() % 4 != 0) b.push_back(0);
    }
};

void OverwriteU16(std::vector<u8>* v, usize off, u16 x) {
    (*v)[off] = static_cast<u8>(x >> 8);
    (*v)[off + 1] = static_cast<u8>(x & 0xFF);
}
void OverwriteU32(std::vector<u8>* v, usize off, u32 x) {
    (*v)[off] = static_cast<u8>(x >> 24);
    (*v)[off + 1] = static_cast<u8>((x >> 16) & 0xFF);
    (*v)[off + 2] = static_cast<u8>((x >> 8) & 0xFF);
    (*v)[off + 3] = static_cast<u8>(x & 0xFF);
}

// --- энкодеры глифов glyf ---------------------------------------------------
// Простой глиф: флаги/координаты с короткими дельтами, где возможно.
void EncodeSimpleGlyph(Writer* w, const std::vector<std::vector<std::pair<i32, i32>>>& contours,
                       const std::vector<std::vector<bool>>& onCurve) {
    i16 nContours = static_cast<i16>(contours.size());
    i32 xMin = 1 << 20, yMin = 1 << 20, xMax = -(1 << 20), yMax = -(1 << 20);
    u16 nPoints = 0;
    for (const auto& c : contours) {
        for (const auto& p : c) {
            xMin = std::min(xMin, p.first);
            xMax = std::max(xMax, p.first);
            yMin = std::min(yMin, p.second);
            yMax = std::max(yMax, p.second);
        }
        nPoints = static_cast<u16>(nPoints + c.size());
    }
    w->I16(nContours);
    w->I16(static_cast<i16>(xMin));
    w->I16(static_cast<i16>(yMin));
    w->I16(static_cast<i16>(xMax));
    w->I16(static_cast<i16>(yMax));
    u16 acc = 0;
    for (const auto& c : contours) {
        acc = static_cast<u16>(acc + c.size());
        w->U16(static_cast<u16>(acc - 1));
    }
    w->U16(0);  // без инструкций

    // Флаги: бит on-curve + биты short/same дельт.
    std::vector<i32> xs, ys;
    std::vector<bool> ons;
    for (usize ci = 0; ci < contours.size(); ++ci) {
        for (usize pi = 0; pi < contours[ci].size(); ++pi) {
            xs.push_back(contours[ci][pi].first);
            ys.push_back(contours[ci][pi].second);
            ons.push_back(onCurve[ci][pi]);
        }
    }
    std::vector<u8> flags(xs.size(), 0);
    for (usize i = 0; i < xs.size(); ++i) {
        i32 dx = i == 0 ? xs[i] : xs[i] - xs[i - 1];
        i32 dy = i == 0 ? ys[i] : ys[i] - ys[i - 1];
        u8 f = ons[i] ? 0x01 : 0x00;
        if (dx == 0) {
            f |= 0x10;
        } else if (dx >= -255 && dx <= 255) {
            f |= 0x02;
            if (dx > 0) f |= 0x10;
        }
        if (dy == 0) {
            f |= 0x20;
        } else if (dy >= -255 && dy <= 255) {
            f |= 0x04;
            if (dy > 0) f |= 0x20;
        }
        flags[i] = f;
    }
    // Выпускаем серии с битом повтора.
    for (usize i = 0; i < flags.size();) {
        usize j = i;
        while (j + 1 < flags.size() && flags[j + 1] == flags[i] && (j - i) < 250) ++j;
        u8 f = flags[i];
        if (j > i) {
            w->U8(static_cast<u8>(f | 0x08));
            w->U8(static_cast<u8>(j - i));
        } else {
            w->U8(f);
        }
        i = j + 1;
    }
    for (usize i = 0; i < xs.size(); ++i) {
        i32 dx = i == 0 ? xs[i] : xs[i] - xs[i - 1];
        u8 f = flags[i];
        if (f & 0x02) {
            w->U8(static_cast<u8>(dx < 0 ? -dx : dx));
        } else if (!(f & 0x10)) {
            w->I16(static_cast<i16>(dx));
        }
    }
    for (usize i = 0; i < ys.size(); ++i) {
        i32 dy = i == 0 ? ys[i] : ys[i] - ys[i - 1];
        u8 f = flags[i];
        if (f & 0x04) {
            w->U8(static_cast<u8>(dy < 0 ? -dy : dy));
        } else if (!(f & 0x20)) {
            w->I16(static_cast<i16>(dy));
        }
    }
}

std::vector<u8> EncodeCompositeGlyph(u16 component, i16 dx, i16 dy) {
    Writer w;
    w.I16(-1);  // numberOfContours < 0 => composite
    w.I16(static_cast<i16>(std::min<i32>(0, dx)));  // xMin (информационно)
    w.I16(static_cast<i16>(std::min<i32>(0, dy)));
    w.I16(static_cast<i16>(std::max<i32>(600, dx)));
    w.I16(static_cast<i16>(std::max<i32>(700, dy)));
    // ARG_1_AND_2_ARE_WORDS | ARGS_ARE_XY_VALUES
    w.U16(0x0003);
    w.U16(component);
    w.I16(dx);
    w.I16(dy);
    return w.b;
}

// ---------------------------------------------------------------------------
// Синтетический TrueType-шрифт (таблицы: head hhea maxp hmtx loca glyf cmap kern
// name post OS/2) с корректными sfnt-контрольными суммами.
// ---------------------------------------------------------------------------
std::vector<u8> BuildSyntheticTtf() {
    const u16 kUpem = 1024;

    // --- glyf -----------------------------------------------------------------
    Writer glyf;
    std::vector<u32> glyfOffsets;
    glyfOffsets.push_back(0);                              // начало глифа 0
    glyfOffsets.push_back(static_cast<u32>(glyf.Size()));  // .notdef пуст

    // глиф 1: 'A' - квадратичная арка (внешняя) плюс треугольный счётчик.
    {
        std::vector<std::vector<std::pair<i32, i32>>> contours = {
            {{100, 0}, {300, 720}, {500, 0}},          // арка (внешняя)
            {{240, 90}, {360, 90}, {300, 300}},        // счётчик, обратное направление обхода
        };
        std::vector<std::vector<bool>> on = {
            {true, false, true},
            {true, true, true},
        };
        EncodeSimpleGlyph(&glyf, contours, on);
        glyf.Pad4();
        glyfOffsets.push_back(static_cast<u32>(glyf.Size()));
    }
    // глиф 2: 'B' - простой прямоугольник.
    {
        std::vector<std::vector<std::pair<i32, i32>>> contours = {
            {{50, 0}, {450, 0}, {450, 700}, {50, 700}},
        };
        std::vector<std::vector<bool>> on = {{true, true, true, true}};
        EncodeSimpleGlyph(&glyf, contours, on);
        glyf.Pad4();
        glyfOffsets.push_back(static_cast<u32>(glyf.Size()));
    }
    // глиф 3: 'C' - композит из 'B', сдвинутого вверх на 100 единиц.
    {
        std::vector<u8> c = EncodeCompositeGlyph(2, 0, 100);
        glyf.Bytes(c.data(), c.size());
        glyf.Pad4();
        glyfOffsets.push_back(static_cast<u32>(glyf.Size()));
    }
    // глиф 4: ' ' - пустой глиф.
    glyfOffsets.push_back(static_cast<u32>(glyf.Size()));

    const u16 kNumGlyphs = 5;

    // --- head -----------------------------------------------------------------
    Writer head;
    head.U32(0x00010000);      // version
    head.U32(0x00010000);      // fontRevision
    head.U32(0);               // checkSumAdjustment (patched later)
    head.U32(0x5F0F3CF5);      // magicNumber
    head.U16(0x0003);          // flags
    head.U16(kUpem);           // unitsPerEm
    head.U32(0);               // created (LONGDATETIME high)
    head.U32(0);               // created (low)
    head.U32(0);               // modified (high)
    head.U32(0);               // modified (low)
    head.I16(50);              // xMin
    head.I16(-10);             // yMin
    head.I16(600);             // xMax
    head.I16(750);             // yMax
    head.U16(0);               // macStyle
    head.U16(8);               // lowestRecPPEM
    head.I16(2);               // fontDirectionHint
    head.I16(1);               // indexToLocFormat (long)
    head.I16(0);               // glyphDataFormat

    // --- hhea -----------------------------------------------------------------
    Writer hhea;
    hhea.U32(0x00010000);
    hhea.I16(800);
    hhea.I16(-200);
    hhea.I16(90);
    hhea.U16(700);  // advanceWidthMax
    hhea.I16(0);    // minLeftSideBearing
    hhea.I16(0);    // minRightSideBearing
    hhea.I16(600);  // xMaxExtent
    hhea.I16(0);  // caretSlopeRise
    hhea.I16(1);  // caretSlopeRun
    hhea.I16(0);
    for (int i = 0; i < 4; ++i) hhea.I16(0);  // reserved
    hhea.I16(0);                              // metricDataFormat
    hhea.U16(kNumGlyphs);                     // numberOfHMetrics

    // --- maxp -----------------------------------------------------------------
    Writer maxp;
    maxp.U32(0x00010000);
    maxp.U16(kNumGlyphs);
    maxp.U16(8);   // maxPoints
    maxp.U16(2);   // maxContours
    maxp.U16(8);   // maxCompositePoints
    maxp.U16(1);   // maxCompositeContours
    maxp.U16(2);   // maxZones
    maxp.U16(0);   // maxTwilightPoints
    maxp.U16(0);   // maxStorage
    maxp.U16(0);   // maxFunctionDefs
    maxp.U16(0);   // maxInstructionDefs
    maxp.U16(0);   // maxStackElements
    maxp.U16(0);   // maxSizeOfInstructions
    maxp.U16(1);   // maxComponentElements
    maxp.U16(1);   // maxComponentDepth

    // --- hmtx -----------------------------------------------------------------
    Writer hmtx;
    const u16 advances[kNumGlyphs] = {500, 600, 520, 540, 260};
    for (u16 i = 0; i < kNumGlyphs; ++i) {
        hmtx.U16(advances[i]);
        hmtx.I16(0);
    }

    // --- loca (long format) ---------------------------------------------------
    Writer loca;
    for (u32 o : glyfOffsets) loca.U32(o);

    // --- cmap (format 4) ------------------------------------------------------
    // 0x20 -> glyph 4, 0x21..0x40 -> glyph 2, 0x41..0x43 -> glyph 1..3,
    // 0x44..0x7E -> glyph 2, 0xFFFF -> 0.
    struct Seg {
        u16 start, end;
        i16 delta;
    };
    const Seg segs[] = {
        {0x0020, 0x0020, static_cast<i16>(4 - 0x20)},
        {0x0021, 0x0040, static_cast<i16>(2 - 0x21)},
        {0x0041, 0x0043, static_cast<i16>(1 - 0x41)},
        {0x0044, 0x007E, static_cast<i16>(2 - 0x44)},
        {0xFFFF, 0xFFFF, 1},
    };
    const u16 segCount = sizeof(segs) / sizeof(segs[0]);
    Writer sub;
    sub.U16(4);                              // format
    sub.U16(0);                              // length (patched)
    sub.U16(0);                              // language
    sub.U16(static_cast<u16>(segCount * 2)); // segCountX2
    sub.U16(4);                              // searchRange
    sub.U16(1);                              // entrySelector
    sub.U16(static_cast<u16>(segCount * 2 - 4));  // rangeShift
    for (const Seg& s : segs) sub.U16(s.end);
    sub.U16(0);  // reservedPad
    for (const Seg& s : segs) sub.U16(s.start);
    for (const Seg& s : segs) sub.I16(s.delta);
    for (u16 i = 0; i < segCount; ++i) sub.U16(0);  // idRangeOffset
    OverwriteU16(&sub.b, 2, static_cast<u16>(sub.Size()));

    Writer cmap;
    cmap.U16(0);  // version
    cmap.U16(1);  // numTables
    cmap.U16(3);  // platform Windows
    cmap.U16(1);  // encoding BMP
    cmap.U32(12); // subtable offset
    cmap.Bytes(sub.b.data(), sub.b.size());

    // --- kern (Windows format 0) ---------------------------------------------
    Writer kern;
    kern.U16(0);  // version
    kern.U16(1);  // nTables
    kern.U16(0);  // subtable version
    kern.U16(20); // subtable length (6 + 8 + 6)
    kern.U16(0x0001);  // coverage: horizontal, format 0
    kern.U16(1);       // nPairs
    kern.U16(6);       // searchRange
    kern.U16(0);       // entrySelector
    kern.U16(0);       // rangeShift
    kern.U16(1);       // left = 'A'
    kern.U16(2);       // right = 'B'
    kern.I16(-40);     // value

    // --- name -----------------------------------------------------------------
    const char* family = "Synthetic Sans";
    const char* style = "Regular";
    Writer strings;
    // UTF-16BE-кодировки
    std::vector<u8> famBe, styBe;
    for (const char* p = family; *p; ++p) {
        famBe.push_back(0);
        famBe.push_back(static_cast<u8>(*p));
    }
    for (const char* p = style; *p; ++p) {
        styBe.push_back(0);
        styBe.push_back(static_cast<u8>(*p));
    }
    u16 famOff = 0;
    u16 styOff = static_cast<u16>(famBe.size());
    strings.Bytes(famBe.data(), famBe.size());
    strings.Bytes(styBe.data(), styBe.size());

    Writer name;
    name.U16(0);   // format
    name.U16(2);   // count
    name.U16(6 + 2 * 12);  // stringOffset
    name.U16(3);   // platform: Windows
    name.U16(1);   // encoding: BMP
    name.U16(0x0409);  // language: en-US
    name.U16(1);   // nameID: family (1)
    name.U16(static_cast<u16>(famBe.size()));
    name.U16(famOff);
    name.U16(3);
    name.U16(1);
    name.U16(0x0409);
    name.U16(2);   // nameID: subfamily
    name.U16(static_cast<u16>(styBe.size()));
    name.U16(styOff);
    name.Bytes(strings.b.data(), strings.b.size());

    // --- post -----------------------------------------------------------------
    Writer post;
    post.U32(0x00030000);  // version 3.0
    post.U32(0);           // italicAngle
    post.I16(-100);        // underlinePosition
    post.I16(50);          // underlineThickness
    post.U32(0);           // isFixedPitch
    post.U32(0);
    post.U32(0);
    post.U32(0);
    post.U32(0);

    // --- OS/2 (version 4) -----------------------------------------------------
    Writer os2;
    os2.U16(4);      // version
    os2.I16(500);    // xAvgCharWidth
    os2.U16(400);    // usWeightClass
    os2.U16(5);      // usWidthClass
    os2.U16(0);      // fsType
    for (int i = 0; i < 10; ++i) os2.I16(0);  // subscript/superscript/strikeout
    os2.I16(0);      // sFamilyClass
    for (int i = 0; i < 10; ++i) os2.U8(0);   // panose
    os2.U32(0);      // ulUnicodeRange1
    os2.U32(0);
    os2.U32(0);
    os2.U32(0);
    os2.Bytes("TEST", 4);  // achVendID
    os2.U16(0x0040);  // fsSelection: REGULAR
    os2.U16(0x20);    // usFirstCharIndex
    os2.U16(0x7E);    // usLastCharIndex
    os2.I16(800);     // sTypoAscender
    os2.I16(-200);    // sTypoDescender
    os2.I16(90);      // sTypoLineGap
    os2.U16(900);     // usWinAscent
    os2.U16(250);     // usWinDescent
    os2.U32(1);       // ulCodePageRange1
    os2.U32(0);       // ulCodePageRange2
    os2.I16(500);     // sxHeight
    os2.I16(700);     // sCapHeight
    os2.U16(0);       // usDefaultChar
    os2.U16(0x20);    // usBreakChar
    os2.U16(1);       // usMaxContext

    struct Tbl {
        const char* tag;
        std::vector<u8> data;
    };
    std::vector<Tbl> tables = {
        {"cmap", cmap.b}, {"glyf", glyf.b}, {"head", head.b}, {"hhea", hhea.b}, {"hmtx", hmtx.b},
        {"kern", kern.b}, {"loca", loca.b}, {"maxp", maxp.b}, {"name", name.b}, {"OS/2", os2.b},
        {"post", post.b},
    };
    std::sort(tables.begin(), tables.end(),
              [](const Tbl& a, const Tbl& b) { return std::strcmp(a.tag, b.tag) < 0; });

    // --- сборка ---------------------------------------------------------------
    const u16 numTables = static_cast<u16>(tables.size());
    u16 entrySelector = 0;
    while ((1u << (entrySelector + 1)) <= numTables) ++entrySelector;
    u16 searchRange = static_cast<u16>(16 * (1 << entrySelector));
    u16 rangeShift = static_cast<u16>(numTables * 16 - searchRange);

    Writer font;
    font.U32(0x00010000);  // sfnt version
    font.U16(numTables);
    font.U16(searchRange);
    font.U16(entrySelector);
    font.U16(rangeShift);

    const usize dirSize = 12 + static_cast<usize>(numTables) * 16;
    usize offset = (dirSize + 3) & ~static_cast<usize>(3);
    std::vector<u32> offsets;
    std::vector<u32> lengths;
    for (const Tbl& t : tables) {
        offsets.push_back(static_cast<u32>(offset));
        lengths.push_back(static_cast<u32>(t.data.size()));
        offset += (t.data.size() + 3) & ~static_cast<usize>(3);
    }
    // Каталог
    for (usize i = 0; i < tables.size(); ++i) {
        const u8* tag = reinterpret_cast<const u8*>(tables[i].tag);
        for (int k = 0; k < 4; ++k) font.U8(tag[k]);
        font.U32(0);  // контрольная сумма (патчится ниже)
        font.U32(offsets[i]);
        font.U32(lengths[i]);
    }
    // Данные таблиц
    for (usize i = 0; i < tables.size(); ++i) {
        while (font.Size() < offsets[i]) font.U8(0);
        font.Bytes(tables[i].data.data(), tables[i].data.size());
        while (font.Size() % 4 != 0) font.U8(0);
    }

    // Контрольные суммы: каждая таблица плюс весь файл (head.checkSumAdjustment).
    for (usize i = 0; i < tables.size(); ++i) {
        u32 sum = 0;
        usize end = offsets[i] + lengths[i];
        for (usize p = offsets[i]; p < end; p += 4) {
            u32 word = 0;
            for (int k = 0; k < 4; ++k) {
                word = (word << 8) | (p + static_cast<usize>(k) < font.b.size() ? font.b[p + k] : 0);
            }
            sum += word;
        }
        OverwriteU32(&font.b, 12 + i * 16 + 4, sum);
    }
    // head.checkSumAdjustment = 0xB1B0AFBA - fileChecksum
    usize headIndex = tables.size();
    for (usize i = 0; i < tables.size(); ++i)
        if (std::strcmp(tables[i].tag, "head") == 0) headIndex = i;
    u32 fileSum = 0;
    for (usize p = 0; p < font.b.size(); p += 4) {
        u32 word = 0;
        for (int k = 0; k < 4; ++k) word = (word << 8) | (p + static_cast<usize>(k) < font.b.size() ? font.b[p + k] : 0);
        fileSum += word;
    }
    u32 adjustment = 0xB1B0AFBAu - fileSum;
    OverwriteU32(&font.b, offsets[headIndex] + 8, adjustment);
    return font.b;
}

bool HasInk(const std::vector<u8>& bitmap) {
    for (u8 v : bitmap)
        if (v > 8) return true;
    return false;
}

std::string TempTtfPath() {
    const char* tmp = std::getenv("TMPDIR");
    std::string dir = tmp && *tmp ? tmp : "/tmp";
    if (!dir.empty() && dir.back() == '/') dir.pop_back();
    return dir + "/eng_test_font_synthetic.ttf";
}

bool WriteFileBytes(const std::string& path, const std::vector<u8>& data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return f.good();
}

}  // namespace

// ===========================================================================
// Помощники UTF-8
// ===========================================================================
ENG_TEST(FontUtf8, AsciiRoundTrip) {
    const std::string s = "Hello, engine! 0123";
    std::vector<u32> cps = Utf8ToCodepoints(s);
    ENG_CHECK_EQ(cps.size(), s.size());
    ENG_CHECK_STR_EQ(CodepointsToUtf8(cps), s);
    ENG_CHECK_EQ(Utf8Length(s), s.size());
    for (usize i = 0; i < s.size(); ++i) ENG_CHECK_EQ(Utf8Offset(s, i), i);
}

ENG_TEST(FontUtf8, CyrillicTwoByte) {
    // "Дурак" - 5 двухбайтовых кодпоинтов; это нужно игре-примеру.
    const std::string s = "\u0414\u0443\u0440\u0430\u043a";
    ENG_CHECK_EQ(s.size(), static_cast<usize>(10));
    std::vector<u32> cps = Utf8ToCodepoints(s);
    ENG_CHECK_EQ(cps.size(), static_cast<usize>(5));
    ENG_CHECK_EQ(cps[0], 0x0414u);
    ENG_CHECK_EQ(cps[1], 0x0443u);
    ENG_CHECK_EQ(cps[2], 0x0440u);
    ENG_CHECK_EQ(cps[3], 0x0430u);
    ENG_CHECK_EQ(cps[4], 0x043Au);
    ENG_CHECK_STR_EQ(CodepointsToUtf8(cps), s);
    ENG_CHECK_EQ(Utf8Length(s), static_cast<usize>(5));
    ENG_CHECK_EQ(Utf8Offset(s, 1), static_cast<usize>(2));
    ENG_CHECK_EQ(Utf8Offset(s, 4), static_cast<usize>(8));
    ENG_CHECK_EQ(Utf8Offset(s, 5), s.size());
}

ENG_TEST(FontUtf8, ThreeByte) {
    // U+20AC EURO SIGN, U+4E2D CJK.
    const std::string s = "\u20AC\u4E2D";
    std::vector<u32> cps = Utf8ToCodepoints(s);
    ENG_CHECK_EQ(cps.size(), static_cast<usize>(2));
    ENG_CHECK_EQ(cps[0], 0x20ACu);
    ENG_CHECK_EQ(cps[1], 0x4E2Du);
    ENG_CHECK_STR_EQ(CodepointsToUtf8(cps), s);
    ENG_CHECK_EQ(Utf8Length(s), static_cast<usize>(2));
}

ENG_TEST(FontUtf8, FourByteEmoji) {
    const std::string s = "\U0001F600";  // улыбающееся лицо
    ENG_CHECK_EQ(s.size(), static_cast<usize>(4));
    std::vector<u32> cps = Utf8ToCodepoints(s);
    ENG_CHECK_EQ(cps.size(), static_cast<usize>(1));
    ENG_CHECK_EQ(cps[0], 0x1F600u);
    ENG_CHECK_STR_EQ(CodepointsToUtf8(cps), s);
    ENG_CHECK_EQ(Utf8Length(s), static_cast<usize>(1));
}

ENG_TEST(FontUtf8, InvalidBytesBecomeReplacement) {
    // 0xFF никогда не валиден; одиночный байт продолжения и обрезанная
    // 3-байтовая последовательность оба должны давать U+FFFD без чтения за границей.
    const std::string bad = std::string("a") + '\xFF' + "b" + '\x80' + "\xE2\x82";
    std::vector<u32> cps = Utf8ToCodepoints(bad);
    ENG_CHECK_EQ(cps.size(), static_cast<usize>(6));
    ENG_CHECK_EQ(cps[0], static_cast<u32>('a'));
    ENG_CHECK_EQ(cps[1], 0xFFFDu);
    ENG_CHECK_EQ(cps[2], static_cast<u32>('b'));
    ENG_CHECK_EQ(cps[3], 0xFFFDu);
    ENG_CHECK_EQ(cps[4], 0xFFFDu);
    ENG_CHECK_EQ(cps[5], 0xFFFDu);
    // Overlong-кодирование '/' двумя байтами тоже должно отвергаться.
    const std::string overlong = "\xC0\xAF";
    std::vector<u32> oc = Utf8ToCodepoints(overlong);
    ENG_CHECK_EQ(oc.size(), static_cast<usize>(2));
    ENG_CHECK_EQ(oc[0], 0xFFFDu);
}

ENG_TEST(FontUtf8, DecodeAdvancesAndClamps) {
    const std::string s = "A\u0414\U0001F600";
    usize i = 0;
    ENG_CHECK_EQ(Utf8Decode(s.data(), s.size(), &i), static_cast<u32>('A'));
    ENG_CHECK_EQ(i, static_cast<usize>(1));
    ENG_CHECK_EQ(Utf8Decode(s.data(), s.size(), &i), 0x0414u);
    ENG_CHECK_EQ(i, static_cast<usize>(3));
    ENG_CHECK_EQ(Utf8Decode(s.data(), s.size(), &i), 0x1F600u);
    ENG_CHECK_EQ(i, s.size());
    // Чтение за концом даёт 0 и не продвигает индекс за длину.
    ENG_CHECK_EQ(Utf8Decode(s.data(), s.size(), &i), 0u);
    ENG_CHECK_EQ(i, s.size());
}

// ===========================================================================
// Процедурный встроенный шрифт
// ===========================================================================
ENG_TEST(FontProcedural, DefaultFontWorksWithoutAssets) {
    Font* f = FontManager::Get().DefaultFont();
    ENG_CHECK(f != nullptr);
    if (!f) return;
    ENG_CHECK(f->Valid());
    ENG_CHECK(f->Format() == FontFormat::Bitmap);
    ENG_CHECK(!f->IsSdf());
    ENG_CHECK(f->UnitsPerEm() > 0.0f);
    ENG_CHECK(f->Ascender() > 0.0f);
    ENG_CHECK(f->Descender() < 0.0f);
    ENG_CHECK(f->LineHeight() > 0.0f);

    const u32 wanted[] = {'A', 'a', '0', ' ', 0x0414 /* Д */};
    for (u32 cp : wanted) {
        const Glyph* g = f->GetGlyph(cp);
        ENG_CHECK_MSG(g != nullptr, "default font is missing a glyph");
        if (!g) continue;
        ENG_CHECK_MSG(g->advance > 0.0f, "procedural glyph must advance");
        ENG_CHECK_MSG(f->Resolve(cp, nullptr) == f, "procedural resolve");
    }
    // Видимые глифы должны иметь чернила; пробел пуст, но advance остаётся.
    const Glyph* a = f->GetGlyph('A');
    ENG_CHECK(a && a->width > 0.0f && a->height > 0.0f);
    ENG_CHECK(a && a->u1 > a->u0 && a->v1 > a->v0);
    ENG_CHECK(a && !a->isEmpty());
    const Glyph* space = f->GetGlyph(' ');
    ENG_CHECK(space != nullptr);
    ENG_CHECK(space && space->isEmpty());
    ENG_CHECK(space && space->advance > 0.0f);

    const Glyph* cyr = f->GetGlyph(0x0414);
    ENG_CHECK(cyr != nullptr && !cyr->isEmpty());

    // MeasureText растёт вместе со строкой.
    f32 w1 = MeasureTextWidth(*f, "A");
    f32 w2 = MeasureTextWidth(*f, "AAA");
    f32 w3 = MeasureTextWidth(*f, "Hello, world");
    ENG_CHECK(w1 > 0.0f);
    ENG_CHECK(w2 > w1);
    ENG_CHECK(w3 > w2);

    ENG_CHECK(f->AtlasOccupancy() > 0.0f);
    ENG_CHECK(f->AtlasOccupancy() <= 1.0f);
    ENG_CHECK(f->AtlasPageCount() >= 1);
}

ENG_TEST(FontProcedural, DefaultSdfFont) {
    Font* f = FontManager::Get().DefaultSdfFont();
    ENG_CHECK(f != nullptr);
    if (!f) return;
    ENG_CHECK(f->Valid());
    ENG_CHECK(f->IsSdf());
    const Glyph* g = f->GetGlyph('B');
    ENG_CHECK(g != nullptr);
    ENG_CHECK(g && !g->isEmpty());
    ENG_CHECK(f->AtlasPageCount() >= 1);
    const FontAtlasPage& page = f->AtlasPage(0);
    ENG_CHECK(page.size > 0);
    ENG_CHECK_NEAR(page.texture.SdfSpread(), f->Desc().sdfSpread, 1e-4f);
    ENG_CHECK_NEAR(page.texture.SdfSize(), f->Desc().pixelHeight, 1e-4f);
}

ENG_TEST(FontProcedural, ProceduralGlyphHasInk) {
    std::unique_ptr<Font> f = textdetail::CreateProceduralFont(FontDesc{});
    ENG_CHECK(f != nullptr);
    if (!f) return;
    const Glyph* g = f->GetGlyph('W');
    ENG_CHECK(g != nullptr && !g->isEmpty());
    if (!g) return;
    ENG_CHECK(g->width > 0.0f && g->height > 0.0f);
    ENG_CHECK(g->u1 > g->u0 && g->v1 > g->v0);
    // Каждый процедурный глиф - полная ячейка 5x7, поэтому боксы одного
    // размера; различные буквы всё же должны попадать в разные области атласа
    // (полностью пустая таблица разместила бы их одинаково).
    const Glyph* i = f->GetGlyph('I');
    const Glyph* m = f->GetGlyph('M');
    ENG_CHECK(i != nullptr && m != nullptr);
    if (i && m) ENG_CHECK(i->u0 != m->u0 || i->v0 != m->v0);
}

ENG_TEST(FontProcedural, BoldAndItalicAreSupported) {
    FontDesc d;
    d.pixelHeight = 24.0f;
    d.bold = true;
    d.italic = true;
    std::unique_ptr<Font> f = textdetail::CreateProceduralFont(d);
    ENG_CHECK(f != nullptr);
    if (!f) return;
    const Glyph* g = f->GetGlyph('E');
    ENG_CHECK(g != nullptr && !g->isEmpty());
    ENG_CHECK(g && g->advance > 0.0f);
}

// ===========================================================================
// Синтетический TrueType-шрифт
// ===========================================================================
ENG_TEST(FontTtf, LoadsAndReportsMetrics) {
    std::vector<u8> ttf = BuildSyntheticTtf();
    ENG_CHECK(ttf.size() > 500);

    FontDesc d;
    d.pixelHeight = 32.0f;
    d.atlasSize = 256;
    d.hinting = true;
    Font font;
    ENG_CHECK(font.LoadFromMemory(ttf.data(), ttf.size(), d));
    ENG_CHECK(font.Valid());
    ENG_CHECK(font.Format() == FontFormat::TrueType);
    ENG_CHECK_NEAR(font.UnitsPerEm(), 1024.0f, 1e-3f);
    ENG_CHECK(font.Ascender() > 0.0f);
    ENG_CHECK(font.Descender() < 0.0f);
    ENG_CHECK(font.LineHeight() > 0.0f);
    ENG_CHECK(font.CapHeight() > 0.0f);
    ENG_CHECK(font.XHeight() > 0.0f);
    ENG_CHECK(font.UnderlineThickness() > 0.0f);
    ENG_CHECK_STR_EQ(font.FamilyName(), std::string("Synthetic Sans"));
    ENG_CHECK_STR_EQ(font.StyleName(), std::string("Regular"));

    // Индексы глифов берутся из cmap формата 4.
    ENG_CHECK_EQ(font.GlyphIndex('A'), 1u);
    ENG_CHECK_EQ(font.GlyphIndex('B'), 2u);
    ENG_CHECK_EQ(font.GlyphIndex('C'), 3u);
    ENG_CHECK_EQ(font.GlyphIndex(' '), 4u);
    ENG_CHECK_EQ(font.GlyphIndex(0x2F00 /* missing */), 0u);
    ENG_CHECK(font.HasGlyph('A'));
    ENG_CHECK(!font.HasGlyph(0x2F00));

    const Glyph* a = font.GetGlyph('A');
    ENG_CHECK(a != nullptr);
    if (a) {
        ENG_CHECK_MSG(a->advance > 0.0f, "advance from hmtx");
        ENG_CHECK_NEAR(a->advance, 600.0f * 32.0f / 1024.0f, 1e-3f);
        ENG_CHECK(a->width > 0.0f && a->height > 0.0f);
        ENG_CHECK(a->u1 > a->u0);
        ENG_CHECK(a->v1 > a->v0);
        ENG_CHECK(!a->isEmpty());
        ENG_CHECK(a->page == 0);
        ENG_CHECK(a->u1 <= 1.0f && a->v1 <= 1.0f);
    }

    // Кернинг берётся из таблицы `kern`, в единицах шрифта.
    ENG_CHECK_NEAR(font.GetKerning('A', 'B'), -40.0f, 1e-4f);
    ENG_CHECK_NEAR(font.GetKerning('B', 'A'), 0.0f, 1e-4f);
}

ENG_TEST(FontTtf, RasterisesInkWithoutGl) {
    std::vector<u8> ttf = BuildSyntheticTtf();
    std::vector<u8> bmp;
    int w = 0, h = 0;
    ENG_CHECK(RasteriseGlyphForTest(ttf.data(), ttf.size(), 'A', &bmp, &w, &h, false));
    ENG_CHECK(w > 0 && h > 0);
    ENG_CHECK_EQ(bmp.size(), static_cast<usize>(w) * static_cast<usize>(h));
    ENG_CHECK_MSG(HasInk(bmp), "glyph 'A' must rasterise to ink");

    // Счётчик (дырка) должен быть пуст: проба из середины глифа должна быть
    // заметно светлее левой стойки.
    usize mid = static_cast<usize>(h / 2) * static_cast<usize>(w) + static_cast<usize>(w / 2);
    ENG_CHECK(bmp[mid] < 250);

    // Пробел растеризуется в ничто, но advance остаётся.
    std::vector<u8> blank;
    int bw = 0, bh = 0;
    ENG_CHECK(RasteriseGlyphForTest(ttf.data(), ttf.size(), ' ', &blank, &bw, &bh, false));
    ENG_CHECK_EQ(bw, 0);
    ENG_CHECK_EQ(bh, 0);
}

ENG_TEST(FontTtf, PrebakeAsciiPopulatesManyGlyphs) {
    std::vector<u8> ttf = BuildSyntheticTtf();
    FontDesc d;
    d.pixelHeight = 16.0f;
    d.atlasSize = 256;
    Font font;
    ENG_CHECK(font.LoadFromMemory(ttf.data(), ttf.size(), d));
    font.PrebakeAscii();
    ENG_CHECK_MSG(font.GlyphCount() >= 90, "PrebakeAscii should cache the whole ASCII range");
    ENG_CHECK(font.AtlasPageCount() >= 1);
    ENG_CHECK(font.AtlasOccupancy() > 0.0f);
    ENG_CHECK(font.AtlasOccupancy() <= 1.0f);
}

ENG_TEST(FontTtf, PrebakeExplicitCodepoints) {
    std::vector<u8> ttf = BuildSyntheticTtf();
    Font font;
    FontDesc d;
    d.pixelHeight = 20.0f;
    d.atlasSize = 128;
    ENG_CHECK(font.LoadFromMemory(ttf.data(), ttf.size(), d));
    const u32 cps[] = {'A', 'B', 'C', ' '};
    font.Prebake(cps, 4);
    ENG_CHECK_EQ(font.GlyphCount(), 4);
}

ENG_TEST(FontTtf, SdfMatchesBitmapMetrics) {
    std::vector<u8> ttf = BuildSyntheticTtf();

    FontDesc bd;
    bd.pixelHeight = 32.0f;
    bd.atlasSize = 256;
    bd.hinting = false;
    Font bitmapFont;
    ENG_CHECK(bitmapFont.LoadFromMemory(ttf.data(), ttf.size(), bd));
    const Glyph* bg = bitmapFont.GetGlyph('A');
    ENG_CHECK(bg != nullptr);
    if (!bg) return;

    FontDesc sd = bd;
    sd.sdf = true;
    sd.sdfSpread = 4.0f;
    Font sdfFont;
    ENG_CHECK(sdfFont.LoadFromMemory(ttf.data(), ttf.size(), sd));
    ENG_CHECK(sdfFont.IsSdf());

    const Glyph* sg = sdfFont.GetGlyph('A');
    ENG_CHECK(sg != nullptr);
    if (!sg) return;
    // Страница атласа (и её SDF-параметры) появляются только после растеризации
    // первого глифа.
    ENG_CHECK(sdfFont.AtlasPageCount() >= 1);
    ENG_CHECK(sdfFont.AtlasOccupancy() > 0.0f);
    ENG_CHECK(sdfFont.AtlasOccupancy() <= 1.0f);
    const FontAtlasPage& page = sdfFont.AtlasPage(0);
    ENG_CHECK_NEAR(page.texture.SdfSpread(), 4.0f, 1e-4f);
    ENG_CHECK_NEAR(page.texture.SdfSize(), 32.0f, 1e-4f);
    ENG_CHECK(!sg->isEmpty());
    // Advance идентичны; SDF-бокс - это бокс чернил, расширенный на spread с каждой
    // стороны, поэтому восстановленный бокс чернил должен попасть в пределах
    // одного пикселя от bitmap-варианта.
    ENG_CHECK_NEAR(sg->advance, bg->advance, 1e-4f);
    ENG_CHECK_NEAR(sg->bearingX + sd.sdfSpread, bg->bearingX, 1.0f);
    ENG_CHECK_NEAR(sg->bearingY - sd.sdfSpread, bg->bearingY, 1.0f);
    ENG_CHECK_NEAR(sg->width, bg->width + 2.0f * sd.sdfSpread, 3.0f);
    ENG_CHECK_NEAR(sg->height, bg->height + 2.0f * sd.sdfSpread, 3.0f);

    // Значения SDF: 128 на границе, мало глубоко внутри, много далеко снаружи.
    std::vector<u8> sdf;
    int w = 0, h = 0;
    ENG_CHECK(RasteriseGlyphForTest(ttf.data(), ttf.size(), 'A', &sdf, &w, &h, true));
    ENG_CHECK(w > 0 && h > 0);
    u8 lo = 255, hi = 0;
    for (u8 v : sdf) {
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    ENG_CHECK_MSG(lo < 100, "SDF must contain deep-interior values");
    ENG_CHECK_MSG(hi > 160, "SDF must contain far-outside values");
}

ENG_TEST(FontTtf, AtlasGrowthStaysWithinBounds) {
    std::vector<u8> ttf = BuildSyntheticTtf();
    FontDesc d;
    d.pixelHeight = 48.0f;
    d.atlasSize = 64;  // намеренно крошечный, чтобы понадобилось больше страниц
    d.atlasPadding = 1;
    Font font;
    ENG_CHECK(font.LoadFromMemory(ttf.data(), ttf.size(), d));
    u32 cps[512];
    int n = 0;
    for (u32 cp = 0x20; cp <= 0x7E; ++cp) cps[n++] = cp;
    for (u32 cp = 0x400; cp < 0x400 + 128; ++cp) cps[n++] = cp;
    font.Prebake(cps, n);
    ENG_CHECK(font.AtlasPageCount() >= 1);
    ENG_CHECK(font.AtlasPageCount() <= 8);
    ENG_CHECK(font.AtlasOccupancy() > 0.0f);
    ENG_CHECK(font.AtlasOccupancy() <= 1.0f);
    for (int i = 0; i < font.AtlasPageCount(); ++i) {
        const FontAtlasPage& p = font.AtlasPage(i);
        ENG_CHECK(p.usedWidth <= p.size);
        ENG_CHECK(p.usedHeight + p.rowHeight <= p.size);
    }
}

ENG_TEST(FontTtf, TruncatedDataIsRejectedWithoutCrashing) {
    std::vector<u8> ttf = BuildSyntheticTtf();
    // Загрузчик сообщает о каждой отклонённой таблице - это и есть проверяемое
    // поведение; приглушение убирает ожидаемые предупреждения из вывода прогона.
    const LogLevel previousLevel = LogGetLevel();
    LogSetLevel(LogLevel::Error);
    // Каждая длина обрезки должна завершаться чисто (или загружаться для коротких
    // префиксов, всё ещё содержащих валидный каталог, что здесь невозможно).
    for (usize len = 0; len < ttf.size(); len += 97) {
        Font font;
        FontDesc d;
        d.pixelHeight = 16.0f;
        d.atlasSize = 64;
        std::vector<u8> cut(ttf.begin(), ttf.begin() + static_cast<std::ptrdiff_t>(len));
        bool ok = font.LoadFromMemory(cut.data(), cut.size(), d);
        if (ok) {
            // Если загрузилось, растеризация должна оставаться в границах.
            font.PrebakeAscii();
            std::vector<u8> bmp;
            int w = 0, h = 0;
            RasteriseGlyphForTest(cut.data(), cut.size(), 'A', &bmp, &w, &h, false);
        }
    }
    LogSetLevel(previousLevel);
    // Портим sfnt-магию.
    std::vector<u8> bad = ttf;
    bad[0] = 0xDE;
    bad[1] = 0xAD;
    Font font;
    ENG_CHECK(!font.LoadFromMemory(bad.data(), bad.size(), FontDesc{}));
    ENG_CHECK(!font.Valid());
    // Null / крошечные буферы.
    ENG_CHECK(!font.LoadFromMemory(nullptr, 0, FontDesc{}));
    u8 tiny[4] = {0, 1, 0, 0};
    ENG_CHECK(!font.LoadFromMemory(tiny, sizeof(tiny), FontDesc{}));
}

ENG_TEST(FontTtf, GlyphCacheIsStable) {
    std::vector<u8> ttf = BuildSyntheticTtf();
    Font font;
    FontDesc d;
    d.pixelHeight = 24.0f;
    d.atlasSize = 128;
    ENG_CHECK(font.LoadFromMemory(ttf.data(), ttf.size(), d));
    const Glyph* first = font.GetGlyph('A');
    const Glyph* second = font.GetGlyph('A');
    ENG_CHECK(first != nullptr);
    ENG_CHECK(first == second);
    ENG_CHECK_EQ(font.GlyphCount(), 1);
    ENG_CHECK(font.GetGlyph(0x2F00) == nullptr);
}

// ---------------------------------------------------------------------------
// Синтетический OpenType/CFF-шрифт: нагружает парсер CFF INDEX/DICT, интерпретатор
// charstring Type2 (rmoveto/rlineto/rrcurveto/seac) и CFF-путь растеризатора
// без какого-либо ассета на диске.
// ---------------------------------------------------------------------------
void CffDictInt(Writer* w, i32 v) {
    if (v >= -107 && v <= 107) {
        w->U8(static_cast<u8>(v + 139));
    } else {
        w->U8(29);
        w->U32(static_cast<u32>(v));
    }
}

// INDEX с однобайтовым массивом смещений (все данные малы).
void CffIndex(Writer* w, const std::vector<std::vector<u8>>& objects) {
    w->U16(static_cast<u16>(objects.size()));
    if (objects.empty()) return;
    w->U8(1);  // offSize
    usize off = 1;
    w->U8(static_cast<u8>(off));
    for (const auto& o : objects) {
        off += o.size();
        w->U8(static_cast<u8>(off));
    }
    for (const auto& o : objects) w->Bytes(o.data(), o.size());
}

std::vector<u8> EncodeType2Int(i32 v) {
    Writer t;
    if (v >= -107 && v <= 107) {
        t.U8(static_cast<u8>(v + 139));
    } else if (v >= 108 && v <= 1131) {
        t.U8(static_cast<u8>(247 + (v - 108) / 256));
        t.U8(static_cast<u8>((v - 108) % 256));
    } else if (v <= -108 && v >= -1131) {
        t.U8(static_cast<u8>(251 + (-v - 108) / 256));
        t.U8(static_cast<u8>((-v - 108) % 256));
    } else {
        t.U8(28);
        t.I16(static_cast<i16>(v));
    }
    return t.b;
}

std::vector<u8> BuildSyntheticOtf() {
    const u16 kUpem = 1000;

    // Charstring'ы. Ширины берутся из Private DICT (явной ширины нет).
    // глиф 0: .notdef - пустой charstring.
    std::vector<u8> cs0 = {14};  // endchar
    // глиф 1: 'A' - растеризуется как прямоугольник через rmoveto/rlineto.
    std::vector<u8> cs1;
    auto add = [&](const std::vector<u8>& v) { cs1.insert(cs1.end(), v.begin(), v.end()); };
    add(EncodeType2Int(100));
    add(EncodeType2Int(0));
    cs1.push_back(21);  // rmoveto
    add(EncodeType2Int(200));
    add(EncodeType2Int(0));
    cs1.push_back(5);  // rlineto
    add(EncodeType2Int(0));
    add(EncodeType2Int(500));
    cs1.push_back(5);
    add(EncodeType2Int(-200));
    add(EncodeType2Int(0));
    cs1.push_back(5);
    cs1.push_back(14);  // endchar (неявное замыкание к началу)
    // глиф 2: 'B' - арка через rrcurveto.
    std::vector<u8> cs2;
    auto add2 = [&](const std::vector<u8>& v) { cs2.insert(cs2.end(), v.begin(), v.end()); };
    add2(EncodeType2Int(50));
    add2(EncodeType2Int(0));
    cs2.push_back(21);
    add2(EncodeType2Int(100));
    add2(EncodeType2Int(300));
    add2(EncodeType2Int(100));
    add2(EncodeType2Int(-300));
    add2(EncodeType2Int(100));
    add2(EncodeType2Int(0));
    cs2.push_back(8);  // rrcurveto
    cs2.push_back(14);
    // глиф 3: 'C' - seac-композит: база SID 34 ('A'), акцент SID 35 ('B').
    std::vector<u8> cs3;
    auto add3 = [&](const std::vector<u8>& v) { cs3.insert(cs3.end(), v.begin(), v.end()); };
    add3(EncodeType2Int(0));
    add3(EncodeType2Int(0));
    add3(EncodeType2Int(34));
    add3(EncodeType2Int(35));
    cs3.push_back(14);  // endchar с 4 аргументами == seac
    // глиф 4: ' ' - пустой.
    std::vector<u8> cs4 = {14};
    // глиф 5: 'D' - hstemhm + vstemhm (2 стема), затем hintmask, чей единственный
    // байт маски является *числовым* байтом, поэтому без его пропуска стек
    // испортится и команды рисования сдвинутся.
    std::vector<u8> cs5;
    auto add5 = [&](const std::vector<u8>& v) { cs5.insert(cs5.end(), v.begin(), v.end()); };
    add5(EncodeType2Int(0));
    add5(EncodeType2Int(20));
    cs5.push_back(18);  // hstemhm
    add5(EncodeType2Int(300));
    add5(EncodeType2Int(20));
    cs5.push_back(23);  // vstemhm -> 2 стема
    cs5.push_back(19);  // hintmask
    cs5.push_back(0x8B);  // 1 байт маски (число 0)
    add5(EncodeType2Int(100));
    add5(EncodeType2Int(0));
    cs5.push_back(21);  // rmoveto
    add5(EncodeType2Int(200));
    add5(EncodeType2Int(0));
    cs5.push_back(5);
    add5(EncodeType2Int(0));
    add5(EncodeType2Int(500));
    cs5.push_back(5);
    add5(EncodeType2Int(-200));
    add5(EncodeType2Int(0));
    cs5.push_back(5);
    cs5.push_back(14);
    // глиф 6: 'E' - две кубики, нарисованные оператором `flex` (12 35).
    std::vector<u8> cs6;
    auto add6 = [&](const std::vector<u8>& v) { cs6.insert(cs6.end(), v.begin(), v.end()); };
    add6(EncodeType2Int(0));
    add6(EncodeType2Int(0));
    cs6.push_back(21);  // rmoveto(0, 0)
    {
        const int deltas[12] = {0, 100, 100, 0, 100, -100, 0, 100, -100, 0, -100, -100};
        for (int v : deltas) add6(EncodeType2Int(v));
        add6(EncodeType2Int(50));  // глубина flex
        cs6.push_back(12);
        cs6.push_back(35);  // flex
    }
    cs6.push_back(14);
    // глиф 7: 'F' - `hflex1` (12 36): dx1 dy1 dx2 dy2 dx3 dx4 dx5 dy5 dx6.
    std::vector<u8> cs7;
    auto add7 = [&](const std::vector<u8>& v) { cs7.insert(cs7.end(), v.begin(), v.end()); };
    add7(EncodeType2Int(0));
    add7(EncodeType2Int(0));
    cs7.push_back(21);
    {
        const int hf[9] = {0, 100, 100, 0, 100, 0, -100, 0, -100};
        for (int v : hf) add7(EncodeType2Int(v));
        cs7.push_back(12);
        cs7.push_back(36);  // hflex1
    }
    cs7.push_back(14);

    // Private DICT: defaultWidthX 500, nominalWidthX 500 (однобайтовые кодировки).
    Writer priv;
    CffDictInt(&priv, 500);
    priv.U8(20);  // defaultWidthX
    CffDictInt(&priv, 500);
    priv.U8(21);  // nominalWidthX

    // Charset (формат 0), чтобы имена глифов брались из стандартных строк:
    // gid 1..4 -> SID 34 ('A'), 35 ('B'), 36 ('C'), 1 ('space').
    Writer charset;
    charset.U8(0);
    for (u16 sid : {34, 35, 36, 37, 38, 39, 1}) charset.U16(sid);

    // CharStrings INDEX, измеряется заранее, чтобы цикл поиска неподвижной точки
    // ниже смог разместить следующий за ним charset.
    Writer charStringsIndex;
    // порядок глифов: .notdef, A, B, C, D, E, F, space
    CffIndex(&charStringsIndex, {cs0, cs1, cs2, cs3, cs5, cs6, cs7, cs4});
    const usize charStringsIndexSize = charStringsIndex.Size();

    // --- таблица CFF ---------------------------------------------------------
    // Абсолютные смещения CharStrings/Private/charset живут внутри TopDICT,
    // размер которого зависит от них; итерируемся до неподвижной точки.
    const std::vector<u8> nameData = {'S', 'y', 'n', 'C'};
    std::vector<u8> topDict;
    i32 privateOff = 0, charStringsOff = 0, charsetOff = 0;
    const i32 privateSize = static_cast<i32>(priv.b.size());
    for (int iter = 0; iter < 8; ++iter) {
        Writer td;
        CffDictInt(&td, charsetOff);
        td.U8(15);  // charset
        CffDictInt(&td, charStringsOff);
        td.U8(17);  // CharStrings
        CffDictInt(&td, privateSize);
        CffDictInt(&td, privateOff);
        td.U8(18);  // Private [size offset]
        topDict = td.b;

        Writer measure;
        CffIndex(&measure, {nameData});
        CffIndex(&measure, {topDict});
        CffIndex(&measure, {});
        CffIndex(&measure, {});
        const i32 newPrivate = static_cast<i32>(measure.Size()) + 4;  // + CFF header
        const i32 newCharStrings = newPrivate + privateSize;
        const i32 newCharset = newCharStrings + static_cast<i32>(charStringsIndexSize);
        if (newPrivate == privateOff && newCharStrings == charStringsOff && newCharset == charsetOff) break;
        privateOff = newPrivate;
        charStringsOff = newCharStrings;
        charsetOff = newCharset;
    }

    Writer cff;
    cff.U8(1);  // major
    cff.U8(0);  // minor
    cff.U8(4);  // hdrSize
    cff.U8(4);  // offSize
    CffIndex(&cff, {nameData});
    CffIndex(&cff, {topDict});
    CffIndex(&cff, {});
    CffIndex(&cff, {});
    cff.Bytes(priv.b.data(), priv.b.size());
    cff.Bytes(charStringsIndex.b.data(), charStringsIndex.b.size());
    cff.Bytes(charset.b.data(), charset.b.size());

    // --- вспомогательные таблицы ---------------------------------------------
    Writer head;
    head.U32(0x00010000);
    head.U32(0x00010000);
    head.U32(0);            // checkSumAdjustment
    head.U32(0x5F0F3CF5);
    head.U16(0x0003);
    head.U16(kUpem);
    head.U32(0); head.U32(0); head.U32(0); head.U32(0);
    head.I16(30); head.I16(-20); head.I16(520); head.I16(560);
    head.U16(0);
    head.U16(8);
    head.I16(2);
    head.I16(0);  // indexToLocFormat (не используется для CFF)
    head.I16(0);

    Writer hhea;
    hhea.U32(0x00010000);
    hhea.I16(780);
    hhea.I16(-220);
    hhea.I16(80);
    hhea.U16(600);
    hhea.I16(0); hhea.I16(0); hhea.I16(520);
    hhea.I16(0); hhea.I16(1); hhea.I16(0);
    for (int i = 0; i < 4; ++i) hhea.I16(0);
    hhea.I16(0);
    hhea.U16(8);

    Writer maxp;
    maxp.U32(0x00005000);  // версия 0.5 для CFF
    maxp.U16(8);

    Writer hmtx;
    const u16 adv[8] = {500, 520, 560, 580, 520, 540, 500, 250};
    for (u16 a : adv) {
        hmtx.U16(a);
        hmtx.I16(0);
    }

    struct Seg { u16 start, end; i16 delta; };
    const Seg segs[] = {
        {0x0020, 0x0020, static_cast<i16>(7 - 0x20)},
        {0x0041, 0x0047, static_cast<i16>(1 - 0x41)},
        {0xFFFF, 0xFFFF, 1},
    };
    const u16 segCount = 3;
    Writer sub;
    sub.U16(4);
    sub.U16(0);
    sub.U16(0);
    sub.U16(static_cast<u16>(segCount * 2));
    sub.U16(4);
    sub.U16(1);
    sub.U16(static_cast<u16>(segCount * 2 - 4));
    for (const Seg& g : segs) sub.U16(g.end);
    sub.U16(0);
    for (const Seg& g : segs) sub.U16(g.start);
    for (const Seg& g : segs) sub.I16(g.delta);
    for (u16 i = 0; i < segCount; ++i) sub.U16(0);
    OverwriteU16(&sub.b, 2, static_cast<u16>(sub.Size()));

    Writer cmap;
    cmap.U16(0);
    cmap.U16(1);
    cmap.U16(3);
    cmap.U16(1);
    cmap.U32(12);
    cmap.Bytes(sub.b.data(), sub.b.size());

    Writer name;
    const char* family = "Synthetic CFF";
    std::vector<u8> famBe;
    for (const char* q = family; *q; ++q) {
        famBe.push_back(0);
        famBe.push_back(static_cast<u8>(*q));
    }
    name.U16(0);
    name.U16(1);
    name.U16(6 + 12);
    name.U16(3);
    name.U16(1);
    name.U16(0x0409);
    name.U16(1);
    name.U16(static_cast<u16>(famBe.size()));
    name.U16(0);
    name.Bytes(famBe.data(), famBe.size());

    Writer post;
    post.U32(0x00030000);
    post.U32(0);
    post.I16(-80);
    post.I16(40);
    post.U32(0); post.U32(0); post.U32(0); post.U32(0); post.U32(0);

    Writer os2;
    os2.U16(4);
    os2.I16(500);
    os2.U16(400);
    os2.U16(5);
    os2.U16(0);
    for (int i = 0; i < 10; ++i) os2.I16(0);
    os2.I16(0);
    for (int i = 0; i < 10; ++i) os2.U8(0);
    os2.U32(0); os2.U32(0); os2.U32(0); os2.U32(0);
    os2.Bytes("TEST", 4);
    os2.U16(0x0040);
    os2.U16(0x20);
    os2.U16(0x43);
    os2.I16(780);
    os2.I16(-220);
    os2.I16(80);
    os2.U16(900);
    os2.U16(250);
    os2.U32(1);
    os2.U32(0);
    os2.I16(500);
    os2.I16(700);
    os2.U16(0);
    os2.U16(0x20);
    os2.U16(1);

    struct Tbl { const char* tag; std::vector<u8> data; };
    std::vector<Tbl> tables = {
        {"CFF ", cff.b}, {"OS/2", os2.b}, {"cmap", cmap.b}, {"head", head.b}, {"hhea", hhea.b},
        {"hmtx", hmtx.b}, {"maxp", maxp.b}, {"name", name.b}, {"post", post.b},
    };
    std::sort(tables.begin(), tables.end(),
              [](const Tbl& a, const Tbl& b) { return std::strcmp(a.tag, b.tag) < 0; });

    const u16 numTables = static_cast<u16>(tables.size());
    u16 entrySelector = 0;
    while ((1u << (entrySelector + 1)) <= numTables) ++entrySelector;
    u16 searchRange = static_cast<u16>(16 * (1 << entrySelector));
    u16 rangeShift = static_cast<u16>(numTables * 16 - searchRange);

    Writer font;
    font.U32(0x4F54544F);  // 'OTTO'
    font.U16(numTables);
    font.U16(searchRange);
    font.U16(entrySelector);
    font.U16(rangeShift);
    const usize dirSize = 12 + static_cast<usize>(numTables) * 16;
    usize offset = (dirSize + 3) & ~static_cast<usize>(3);
    std::vector<u32> offsets, lengths;
    for (const Tbl& t : tables) {
        offsets.push_back(static_cast<u32>(offset));
        lengths.push_back(static_cast<u32>(t.data.size()));
        offset += (t.data.size() + 3) & ~static_cast<usize>(3);
    }
    for (usize i = 0; i < tables.size(); ++i) {
        const u8* tag = reinterpret_cast<const u8*>(tables[i].tag);
        for (int k = 0; k < 4; ++k) font.U8(tag[k]);
        font.U32(0);
        font.U32(offsets[i]);
        font.U32(lengths[i]);
    }
    for (usize i = 0; i < tables.size(); ++i) {
        while (font.Size() < offsets[i]) font.U8(0);
        font.Bytes(tables[i].data.data(), tables[i].data.size());
        while (font.Size() % 4 != 0) font.U8(0);
    }
    for (usize i = 0; i < tables.size(); ++i) {
        u32 sum = 0;
        usize end = offsets[i] + lengths[i];
        for (usize q = offsets[i]; q < end; q += 4) {
            u32 word = 0;
            for (int k = 0; k < 4; ++k)
                word = (word << 8) | (q + static_cast<usize>(k) < font.b.size() ? font.b[q + k] : 0);
            sum += word;
        }
        OverwriteU32(&font.b, 12 + i * 16 + 4, sum);
    }
    usize headIndex = tables.size();
    for (usize i = 0; i < tables.size(); ++i)
        if (std::strcmp(tables[i].tag, "head") == 0) headIndex = i;
    u32 fileSum = 0;
    for (usize q = 0; q < font.b.size(); q += 4) {
        u32 word = 0;
        for (int k = 0; k < 4; ++k)
            word = (word << 8) | (q + static_cast<usize>(k) < font.b.size() ? font.b[q + k] : 0);
        fileSum += word;
    }
    OverwriteU32(&font.b, offsets[headIndex] + 8, 0xB1B0AFBAu - fileSum);
    return font.b;
}

ENG_TEST(FontCff, LoadsAndRasterisesOpenTypeCff) {
    std::vector<u8> otf = BuildSyntheticOtf();
    ENG_CHECK(otf.size() > 500);
    FontDesc d;
    d.pixelHeight = 32.0f;
    d.atlasSize = 256;
    Font font;
    ENG_CHECK_MSG(font.LoadFromMemory(otf.data(), otf.size(), d), "OTF/CFF load");
    ENG_CHECK(font.Valid());
    ENG_CHECK(font.Format() == FontFormat::OpenTypeCFF);
    ENG_CHECK_NEAR(font.UnitsPerEm(), 1000.0f, 1.0f);
    ENG_CHECK(font.Ascender() > 0.0f);
    ENG_CHECK(font.Descender() < 0.0f);
    ENG_CHECK(font.LineHeight() > 0.0f);
    ENG_CHECK_STR_EQ(font.FamilyName(), std::string("Synthetic CFF"));
    ENG_CHECK(font.GlyphIndex('A') == 1u);
    ENG_CHECK(font.GlyphIndex('B') == 2u);
    ENG_CHECK(font.GlyphIndex('C') == 3u);
    ENG_CHECK(font.GlyphIndex(' ') == 7u);  // порядок глифов: A B C D E F space

    // 'A' - простой прямоугольник, нарисованный rmoveto/rlineto.
    const Glyph* a = font.GetGlyph('A');
    ENG_CHECK(a != nullptr);
    if (a) {
        ENG_CHECK(a->width > 0.0f && a->height > 0.0f);
        ENG_CHECK(!a->isEmpty());
        // 200 x 500 единиц при 32/1000 px на единицу.
        ENG_CHECK_NEAR(a->width, 200.0f * 32.0f / 1000.0f, 1.5f);
        ENG_CHECK_NEAR(a->height, 500.0f * 32.0f / 1000.0f, 1.5f);
        ENG_CHECK(a->advance > 0.0f);
    }
    // 'B' использует rrcurveto, значит тоже должен растеризоваться во что-то.
    const Glyph* b = font.GetGlyph('B');
    ENG_CHECK(b != nullptr && !b->isEmpty());
    // 'C' - seac-композит (база 'A' + акцент 'B').
    const Glyph* c = font.GetGlyph('C');
    ENG_CHECK(c != nullptr && !c->isEmpty());
    if (c && a) ENG_CHECK(c->height >= a->height - 1.0f);

    std::vector<u8> bmp;
    int w = 0, h = 0;
    ENG_CHECK(RasteriseGlyphForTest(otf.data(), otf.size(), 'A', &bmp, &w, &h, false));
    ENG_CHECK(w > 0 && h > 0);
    ENG_CHECK(HasInk(bmp));
    std::vector<u8> sdf;
    int sw = 0, sh = 0;
    ENG_CHECK(RasteriseGlyphForTest(otf.data(), otf.size(), 'B', &sdf, &sw, &sh, true));
    ENG_CHECK(sw > 0 && sh > 0);
}

// ===========================================================================
// Векторные контуры (аналитический рендерер в стиле Slug)
// ===========================================================================
namespace {

// Каждый контур должен быть замкнут, начинаться/заканчиваться на on-curve точках и
// использовать только квадратичные точки, причём за каждой off-curve следует on-curve.
void CheckQuadraticContours(const GlyphOutline& o) {
    for (const GlyphContour& c : o.contours) {
        ENG_CHECK(c.points.size() >= 3);
        if (c.points.size() < 3) continue;
        ENG_CHECK_EQ(static_cast<int>(c.points.front().onCurve), 1);
        ENG_CHECK_EQ(static_cast<int>(c.points.back().onCurve), 1);
        ENG_CHECK_NEAR(c.points.front().p.x, c.points.back().p.x, 1e-4f);
        ENG_CHECK_NEAR(c.points.front().p.y, c.points.back().p.y, 1e-4f);
        for (usize i = 0; i < c.points.size(); ++i) {
            ENG_CHECK(c.points[i].onCurve == 0 || c.points[i].onCurve == 1);
            if (c.points[i].onCurve == 0) {
                ENG_CHECK(i + 1 < c.points.size());
                if (i + 1 < c.points.size()) ENG_CHECK_EQ(static_cast<int>(c.points[i + 1].onCurve), 1);
            }
            ENG_CHECK(std::isfinite(c.points[i].p.x) && std::isfinite(c.points[i].p.y));
        }
    }
}

usize CountPoints(const GlyphOutline& o) {
    usize n = 0;
    for (const GlyphContour& c : o.contours) n += c.points.size();
    return n;
}

}  // namespace

ENG_TEST(FontCff, HintmaskFlexAndHflex1Charstrings) {
    std::vector<u8> otf = BuildSyntheticOtf();
    Font font;
    FontDesc d;
    d.pixelHeight = 32.0f;
    d.atlasSize = 256;
    ENG_CHECK(font.LoadFromMemory(otf.data(), otf.size(), d));
    ENG_CHECK(font.GlyphIndex('D') != 0u);
    ENG_CHECK(font.GlyphIndex('E') != 0u);
    ENG_CHECK(font.GlyphIndex('F') != 0u);

    // 'D' использует hstemhm + vstemhm + hintmask. Байт маски - числовой байт,
    // поэтому без пропуска останется лишний операнд и глиф сдвинется.
    GlyphOutline hm;
    ENG_CHECK(font.GetGlyphOutlineUnits('D', &hm));
    ENG_CHECK(!hm.empty);
    CheckQuadraticContours(hm);
    ENG_CHECK_NEAR(hm.bounds.x, 100.0f, 1.0f);
    ENG_CHECK_NEAR(hm.bounds.w, 200.0f, 1.0f);
    ENG_CHECK_NEAR(hm.bounds.y, -500.0f, 1.0f);
    ENG_CHECK_NEAR(hm.bounds.h, 500.0f, 1.0f);
    const Glyph* gd = font.GetGlyph('D');
    ENG_CHECK(gd != nullptr && gd->width > 0.0f && gd->height > 0.0f);

    // 'E' использует `flex`: две кубики, чьё квадратичное преобразование достигает пика при y = 75.
    GlyphOutline fl;
    ENG_CHECK(font.GetGlyphOutlineUnits('E', &fl));
    ENG_CHECK(!fl.empty);
    CheckQuadraticContours(fl);
    ENG_CHECK_NEAR(fl.bounds.x, 0.0f, 2.0f);
    ENG_CHECK_NEAR(fl.bounds.w, 200.0f, 2.0f);
    ENG_CHECK_NEAR(fl.bounds.y, -75.0f, 2.0f);
    ENG_CHECK_NEAR(fl.bounds.h, 75.0f, 2.0f);

    // 'F' использует `hflex1`: вторая кривая плоская, поэтому бокс чернил 100 в высоту
    // (сдвинутый dx5/dy5 примерно удвоил бы это).
    GlyphOutline hf;
    ENG_CHECK(font.GetGlyphOutlineUnits('F', &hf));
    ENG_CHECK(!hf.empty);
    CheckQuadraticContours(hf);
    ENG_CHECK_NEAR(hf.bounds.x, 0.0f, 2.0f);
    ENG_CHECK_NEAR(hf.bounds.w, 200.0f, 2.0f);
    ENG_CHECK_NEAR(hf.bounds.y, -100.0f, 2.0f);
    ENG_CHECK_NEAR(hf.bounds.h, 100.0f, 2.0f);
}

ENG_TEST(FontOutline, TrueTypeGlyphHasPredictablePointsAndBounds) {
    std::vector<u8> ttf = BuildSyntheticTtf();
    Font font;
    FontDesc d;
    d.pixelHeight = 32.0f;
    d.atlasSize = 256;
    ENG_CHECK(font.LoadFromMemory(ttf.data(), ttf.size(), d));

    GlyphOutline o;
    ENG_CHECK(font.GetGlyphOutlineUnits('A', &o));
    ENG_CHECK(!o.empty);
    ENG_CHECK_EQ(o.contours.size(), static_cast<usize>(2));
    ENG_CHECK_EQ(CountPoints(o), static_cast<usize>(8));  // 2 контура x (3 + замыкание)
    CheckQuadraticContours(o);

    // Контур 0 - квадратичная арка: on, control, on, (замыкающая) on.
    ENG_CHECK_EQ(o.contours[0].points.size(), static_cast<usize>(4));
    ENG_CHECK_EQ(static_cast<int>(o.contours[0].points[0].onCurve), 1);
    ENG_CHECK_EQ(static_cast<int>(o.contours[0].points[1].onCurve), 0);
    ENG_CHECK_EQ(static_cast<int>(o.contours[0].points[2].onCurve), 1);
    ENG_CHECK_NEAR(o.contours[0].points[1].p.x, 300.0f, 1e-3f);  // контрольная точка
    // Единицы с y вниз: 'A' сидит над базовой линией, поэтому y отрицателен.
    ENG_CHECK(o.bounds.y < 0.0f);
    ENG_CHECK_NEAR(o.bounds.x, 100.0f, 1.0f);
    ENG_CHECK_NEAR(o.bounds.w, 400.0f, 1.0f);
    // Вершина арки на половине высоты контрольной точки, поэтому плотный бокс
    // чернил должен использовать кривую, а не контрольную точку (720).
    ENG_CHECK_NEAR(o.bounds.y, -360.0f, 1.0f);
    ENG_CHECK_NEAR(o.bounds.h, 360.0f, 1.0f);
    ENG_CHECK_NEAR(o.advance, 600.0f, 1e-3f);  // advance из hmtx в единицах шрифта

    // 'C' - композит из 'B', сдвинутого вверх на 100 единиц, поэтому композит
    // должен разрешаться в один прямоугольник с y в [-800, -100].
    GlyphOutline comp;
    ENG_CHECK(font.GetGlyphOutlineUnits('C', &comp));
    ENG_CHECK(!comp.empty);
    ENG_CHECK_EQ(comp.contours.size(), static_cast<usize>(1));
    CheckQuadraticContours(comp);
    ENG_CHECK_NEAR(comp.bounds.x, 50.0f, 1.0f);
    ENG_CHECK_NEAR(comp.bounds.w, 400.0f, 1.0f);
    ENG_CHECK_NEAR(comp.bounds.y, -800.0f, 1.0f);
    ENG_CHECK_NEAR(comp.bounds.h, 700.0f, 1.0f);

}

ENG_TEST(FontOutline, SizeScalingAndOrientation) {
    std::vector<u8> ttf = BuildSyntheticTtf();
    Font font;
    FontDesc d;
    d.pixelHeight = 32.0f;
    d.atlasSize = 256;
    ENG_CHECK(font.LoadFromMemory(ttf.data(), ttf.size(), d));

    GlyphOutline at32, at64;
    ENG_CHECK(font.GetGlyphOutline('A', 32.0f, &at32));
    ENG_CHECK(font.GetGlyphOutline('A', 64.0f, &at64));
    ENG_CHECK(!at32.empty && !at64.empty);
    CheckQuadraticContours(at32);
    CheckQuadraticContours(at64);

    // scale = size / unitsPerEm, поэтому 32px при 1024 upem шрифта - это 1/32.
    ENG_CHECK_NEAR(at32.bounds.x, 100.0f * 32.0f / 1024.0f, 0.1f);
    ENG_CHECK_NEAR(at32.bounds.w, 400.0f * 32.0f / 1024.0f, 0.2f);
    ENG_CHECK_NEAR(at32.bounds.h, 360.0f * 32.0f / 1024.0f, 0.2f);
    ENG_CHECK(at32.bounds.y < 0.0f);  // y растёт вниз, глиф над базовой линией
    ENG_CHECK_NEAR(at32.advance, 600.0f * 32.0f / 1024.0f, 0.05f);
    // Удвоение запрошенного размера удваивает контур.
    ENG_CHECK_NEAR(at64.bounds.w, at32.bounds.w * 2.0f, 0.2f);
    ENG_CHECK_NEAR(at64.bounds.h, at32.bounds.h * 2.0f, 0.2f);
    ENG_CHECK_NEAR(at64.bounds.x, at32.bounds.x * 2.0f, 0.1f);
    ENG_CHECK_NEAR(at64.advance, at32.advance * 2.0f, 0.05f);

    // size <= 0 откатывается к размеру растеризации шрифта.
    GlyphOutline fallback;
    ENG_CHECK(font.GetGlyphOutline('A', 0.0f, &fallback));
    ENG_CHECK_NEAR(fallback.bounds.w, at32.bounds.w, 1e-3f);
    ENG_CHECK_NEAR(fallback.bounds.h, at32.bounds.h, 1e-3f);

    // Без хинтинга растровый бокс чернил - та же геометрия, поэтому границы
    // контура и бокса bitmap совпадают с точностью до пикселя.
    FontDesc raw;
    raw.pixelHeight = 32.0f;
    raw.atlasSize = 256;
    raw.hinting = false;
    Font plain;
    ENG_CHECK(plain.LoadFromMemory(ttf.data(), ttf.size(), raw));
    const Glyph* g = plain.GetGlyph('A');
    ENG_CHECK(g != nullptr);
    if (g) {
        ENG_CHECK_NEAR(at32.bounds.w, g->width, 1.5f);
        ENG_CHECK_NEAR(at32.bounds.h, g->height, 1.5f);
        ENG_CHECK_NEAR(at32.bounds.x, g->bearingX, 1.5f);
    }
}

ENG_TEST(FontOutline, SpaceIsEmptyButValid) {
    std::vector<u8> ttf = BuildSyntheticTtf();
    Font font;
    FontDesc d;
    d.pixelHeight = 32.0f;
    d.atlasSize = 256;
    ENG_CHECK(font.LoadFromMemory(ttf.data(), ttf.size(), d));

    GlyphOutline o;
    ENG_CHECK_MSG(font.GetGlyphOutline(' ', 32.0f, &o), "space must return true");
    ENG_CHECK(o.empty);
    ENG_CHECK(o.contours.empty());
    ENG_CHECK(o.advance > 0.0f);
    ENG_CHECK_NEAR(o.bounds.w, 0.0f, 1e-6f);
    ENG_CHECK_NEAR(o.bounds.h, 0.0f, 1e-6f);

    GlyphOutline units;
    ENG_CHECK(font.GetGlyphOutlineUnits(' ', &units));
    ENG_CHECK(units.empty);
    ENG_CHECK(units.advance > 0.0f);

    // Кодпоинт, который шрифт не отображает, - настоящая ошибка.
    ENG_CHECK(!font.GetGlyphOutline(0x2F00, 32.0f, &o));
    ENG_CHECK(!font.GetGlyphOutlineUnits(0x2F00, &o));
    // Null-указатель вывода отвергается, а не разыменовывается.
    ENG_CHECK(!font.GetGlyphOutline('A', 32.0f, nullptr));
}

ENG_TEST(FontOutline, CffCubicsBecomeQuadratics) {
    std::vector<u8> otf = BuildSyntheticOtf();
    Font font;
    FontDesc d;
    d.pixelHeight = 32.0f;
    d.atlasSize = 256;
    ENG_CHECK(font.LoadFromMemory(otf.data(), otf.size(), d));
    ENG_CHECK(font.Format() == FontFormat::OpenTypeCFF);

    // 'A' - прямоугольник, выровненный по осям: 4 угла плюс замыкающая точка.
    GlyphOutline rect;
    ENG_CHECK(font.GetGlyphOutlineUnits('A', &rect));
    ENG_CHECK(!rect.empty);
    ENG_CHECK_EQ(rect.contours.size(), static_cast<usize>(1));
    ENG_CHECK_EQ(rect.contours[0].points.size(), static_cast<usize>(5));
    CheckQuadraticContours(rect);
    ENG_CHECK_NEAR(rect.bounds.x, 100.0f, 1.0f);
    ENG_CHECK_NEAR(rect.bounds.w, 200.0f, 1.0f);
    ENG_CHECK_NEAR(rect.bounds.y, -500.0f, 1.0f);
    ENG_CHECK_NEAR(rect.bounds.h, 500.0f, 1.0f);

    // 'B' нарисован rrcurveto, поэтому кубике нужно преобразование. Аналитическая
    // кубика покрывает x [50,350] и y_up [0,133.33] -> y_down [-133.33,0].
    GlyphOutline arch;
    ENG_CHECK(font.GetGlyphOutlineUnits('B', &arch));
    ENG_CHECK(!arch.empty);
    CheckQuadraticContours(arch);
    usize offCurve = 0;
    for (const GlyphContour& c : arch.contours)
        for (const GlyphPoint& p : c.points)
            if (p.onCurve == 0) ++offCurve;
    ENG_CHECK_MSG(offCurve >= 1, "the cubic must contribute quadratic control points");
    ENG_CHECK_MSG(offCurve <= 4, "at most four quadratics per cubic");
    ENG_CHECK_NEAR(arch.bounds.x, 50.0f, 2.0f);
    ENG_CHECK_NEAR(arch.bounds.w, 300.0f, 2.0f);
    ENG_CHECK_NEAR(arch.bounds.y, -133.333f, 2.0f);
    ENG_CHECK_NEAR(arch.bounds.h, 133.333f, 2.0f);

    // 'C' - seac-композит (база 'A' + акцент 'B'), поэтому он должен давать
    // оба разрешённых контура.
    GlyphOutline seac;
    ENG_CHECK(font.GetGlyphOutlineUnits('C', &seac));
    ENG_CHECK(!seac.empty);
    ENG_CHECK(seac.contours.size() >= 2);
    CheckQuadraticContours(seac);
    ENG_CHECK(seac.bounds.w > 200.0f);

    // Вариант в пиксельном пространстве.
    GlyphOutline px;
    ENG_CHECK(font.GetGlyphOutline('B', 32.0f, &px));
    CheckQuadraticContours(px);
    ENG_CHECK_NEAR(px.bounds.y, -133.333f * 32.0f / 1000.0f, 0.2f);
}

ENG_TEST(FontOutline, ProceduralFontSynthesisesABox) {
    Font* f = FontManager::Get().DefaultFont();
    ENG_CHECK(f != nullptr);
    if (!f) return;
    ENG_CHECK(f->Format() == FontFormat::Bitmap);

    GlyphOutline o;
    ENG_CHECK(f->GetGlyphOutline('A', 48.0f, &o));
    ENG_CHECK(!o.empty);
    ENG_CHECK_EQ(o.contours.size(), static_cast<usize>(1));
    if (!o.contours.empty()) {
        ENG_CHECK_EQ(o.contours[0].points.size(), static_cast<usize>(5));  // бокс + замыкание
        CheckQuadraticContours(o);
    }
    ENG_CHECK_NEAR(o.bounds.w, 48.0f * 5.0f / 7.0f, 1e-3f);
    ENG_CHECK_NEAR(o.bounds.h, 48.0f, 1e-3f);
    ENG_CHECK_NEAR(o.bounds.y, -48.0f, 1e-3f);
    ENG_CHECK(o.advance > 0.0f);

    // Пробел остаётся пустым (но валидным); неизвестные знаки всё равно получают бокс.
    GlyphOutline space;
    ENG_CHECK(f->GetGlyphOutline(' ', 48.0f, &space));
    ENG_CHECK(space.empty);
    GlyphOutline emoji;
    ENG_CHECK(f->GetGlyphOutline(0x1F600, 48.0f, &emoji));
    ENG_CHECK(!emoji.empty);
    ENG_CHECK(emoji.bounds.w > 0.0f && emoji.bounds.h > 0.0f);

    // Вариант в единицах шрифта.
    GlyphOutline units;
    ENG_CHECK(f->GetGlyphOutlineUnits('A', &units));
    ENG_CHECK(!units.empty);
    ENG_CHECK_NEAR(units.bounds.h, 1024.0f, 1.0f);
    ENG_CHECK_NEAR(units.bounds.y, -1024.0f, 1.0f);
}

ENG_TEST(FontOutline, ItalicAppliesASlant) {
    std::vector<u8> ttf = BuildSyntheticTtf();
    FontDesc upright;
    upright.pixelHeight = 32.0f;
    upright.atlasSize = 256;
    FontDesc slanted = upright;
    slanted.italic = true;
    slanted.italicSlant = 0.25f;
    Font a, b;
    ENG_CHECK(a.LoadFromMemory(ttf.data(), ttf.size(), upright));
    ENG_CHECK(b.LoadFromMemory(ttf.data(), ttf.size(), slanted));
    GlyphOutline ua, ub;
    ENG_CHECK(a.GetGlyphOutline('A', 32.0f, &ua));
    ENG_CHECK(b.GetGlyphOutline('A', 32.0f, &ub));
    // Сдвиг оставляет точки на базовой линии на месте и двигает верх глифа вправо,
    // поэтому наивысшая точка смещается на slant * height-above-baseline.
    auto topX = [](const GlyphOutline& o, f32* y) {
        f32 best = 1e30f, x = 0.0f;
        for (const GlyphContour& c : o.contours) {
            for (const GlyphPoint& p : c.points) {
                if (p.p.y < best) {
                    best = p.p.y;
                    x = p.p.x;
                }
            }
        }
        if (y) *y = best;
        return x;
    };
    f32 uaTopY = 0.0f, ubTopY = 0.0f;
    const f32 uaTopX = topX(ua, &uaTopY);
    const f32 ubTopX = topX(ub, &ubTopY);
    ENG_CHECK_NEAR(ubTopY, uaTopY, 1e-3f);  // сдвиг не меняет y
    ENG_CHECK(uaTopY < 0.0f);
    ENG_CHECK_NEAR(ubTopX - uaTopX, 0.25f * (-uaTopY), 0.05f);
    // Точки базовой линии не двигаются, поэтому бокс чернил сохраняет размах слева/справа.
    ENG_CHECK_NEAR(ub.bounds.x, ua.bounds.x, 1e-3f);
    ENG_CHECK_NEAR(ub.bounds.w, ua.bounds.w, 1e-3f);
    // Контуры в единицах никогда не наклоняются (это собственная геометрия шрифта).
    GlyphOutline uu, su;
    ENG_CHECK(a.GetGlyphOutlineUnits('A', &uu));
    ENG_CHECK(b.GetGlyphOutlineUnits('A', &su));
    ENG_CHECK_NEAR(uu.bounds.x, su.bounds.x, 1e-3f);
    ENG_CHECK_NEAR(uu.bounds.w, su.bounds.w, 1e-3f);
}

// ===========================================================================
// FontManager / фолбэки
// ===========================================================================
ENG_TEST(FontManagerTest, LoadCachesByPathAndDesc) {
    std::vector<u8> ttf = BuildSyntheticTtf();
    const std::string path = TempTtfPath();
    ENG_CHECK(WriteFileBytes(path, ttf));

    FontDesc d;
    d.pixelHeight = 24.0f;
    d.atlasSize = 128;
    FontManager& mgr = FontManager::Get();
    Font* a = mgr.Load(path, d);
    if (!a) {
        ENG_SKIP("file system cannot read the temporary font path");
    }
    Font* b = mgr.Load(path, d);
    ENG_CHECK(a == b);
    ENG_CHECK(a->Valid());
    // Другой дескриптор должен давать другую запись.
    FontDesc d2 = d;
    d2.pixelHeight = 40.0f;
    Font* c = mgr.Load(path, d2);
    ENG_CHECK(c != nullptr);
    ENG_CHECK(c != a);
    ENG_CHECK_NEAR(c->Desc().pixelHeight, 40.0f, 1e-4f);
    mgr.Clear();
    std::remove(path.c_str());
}

ENG_TEST(FontManagerTest, SetDefaultFont) {
    FontManager& mgr = FontManager::Get();
    Font* d = mgr.DefaultFont();
    ENG_CHECK(d != nullptr);
    mgr.SetDefaultFont(d);
    ENG_CHECK(mgr.DefaultFont() == d);
}

ENG_TEST(FontFallback, ResolvesThroughChain) {
    std::vector<u8> ttf = BuildSyntheticTtf();
    Font primary;
    FontDesc d;
    d.pixelHeight = 24.0f;
    d.atlasSize = 128;
    ENG_CHECK(primary.LoadFromMemory(ttf.data(), ttf.size(), d));

    Font* fallback = FontManager::Get().DefaultFont();
    ENG_CHECK(fallback != nullptr);
    if (!fallback) return;
    primary.AddFallback(fallback);

    // Прямое попадание остаётся в основном шрифте.
    const Glyph* g = nullptr;
    Font* owner = primary.Resolve('A', &g);
    ENG_CHECK(owner == &primary);
    ENG_CHECK(g != nullptr);

    // Кодпоинт, которого нет в синтетическом шрифте, разрешается через
    // процедурный фолбэк.
    const u32 missing = 0x1F600;
    ENG_CHECK(!primary.HasGlyph(missing));
    g = nullptr;
    owner = primary.Resolve(missing, &g);
    ENG_CHECK(owner == fallback);
    ENG_CHECK(g != nullptr);
    if (g) ENG_CHECK(!g->isEmpty());

    // Без цепочки фолбэков отсутствующий кодпоинт не разрешается ни во что.
    Font lonely;
    ENG_CHECK(lonely.LoadFromMemory(ttf.data(), ttf.size(), d));
    g = nullptr;
    ENG_CHECK(lonely.Resolve(0x1F600, &g) == nullptr);
    ENG_CHECK(g == nullptr);
}

// ===========================================================================
// Реальные подготовленные шрифты (examples/assets/fonts)
// ===========================================================================
ENG_TEST(FontCffReal, PrintedAsciiAllRasteriseAndOutline) {
    const std::string path = PathJoin(GetAssetRoot(), "fonts/cff.otf");
    if (!FileExists(path)) ENG_SKIP("assets/fonts/cff.otf not staged");

    Font font;
    FontDesc d;
    d.pixelHeight = 48.0f;
    ENG_CHECK_MSG(font.LoadFromFile(path, d), "real CFF face must load");
    if (!font.Valid()) return;
    ENG_CHECK_MSG(font.Format() == FontFormat::OpenTypeCFF, "cff.otf must take the CFF path");

    int mapped = 0, bitmapOk = 0, outlineOk = 0;
    std::string noBitmap, noOutline;
    for (u32 cp = 32; cp < 127; ++cp) {
        const Glyph* g = nullptr;
        font.Resolve(cp, &g);
        if (g == nullptr) continue;
        ++mapped;
        GlyphOutline o;
        const bool haveOutline = font.GetGlyphOutline(cp, 48.0f, &o) && !o.empty;
        if (cp == ' ') {
            // Пробел законно пуст, но всё равно должен возвращать валидный контур.
            ENG_CHECK(!haveOutline);
            ENG_CHECK(o.empty);
            continue;
        }
        if (g->width > 0.0f && g->height > 0.0f) ++bitmapOk; else noBitmap += static_cast<char>(cp);
        if (haveOutline) ++outlineOk; else noOutline += static_cast<char>(cp);
    }
    ENG_CHECK_MSG(mapped >= 90, "the CFF face should map the printable ASCII range");
    ENG_CHECK_MSG(bitmapOk >= 90, ("glyphs with an empty bitmap: " + noBitmap).c_str());
    ENG_CHECK_MSG(outlineOk >= 90, ("glyphs with an empty outline: " + noOutline).c_str());

    // Знаки, заведомо нагружающие путь сабрутин (баг, делавший большинство букв
    // пустыми: перепутанный Private [size offset] прятал локальный INDEX сабрутин).
    for (u32 cp : {'A', 'O', 'S'}) {
        const Glyph* g = nullptr;
        font.Resolve(cp, &g);
        ENG_CHECK_MSG(g != nullptr && g->width > 0.0f && g->height > 0.0f, "expected a rasterised glyph");
        GlyphOutline o;
        ENG_CHECK(font.GetGlyphOutline(cp, 48.0f, &o));
        ENG_CHECK_MSG(!o.empty, "expected a non-empty outline");
        CheckQuadraticContours(o);
    }
}
