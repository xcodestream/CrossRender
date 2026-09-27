// Загрузка шрифтов TrueType / OpenType, растеризация и атласы глифов
// Signed Distance Field, реализованные с нуля.
//
// Структура файла:
//   1.  Проверяющий границы big-endian читатель + малые помощники   [fontimpl]
//   2.  Контейнер sfnt / TTC и каталог таблиц                        [fontimpl]
//   3.  head, hhea, maxp, hmtx, loca, post, OS/2, name, kern         [fontimpl]
//   4.  cmap (форматы 0/4/6/12/13/14) и GPOS PairPos                 [fontimpl]
//   5.  Контуры glyf (простые и составные)                           [fontimpl]
//   6.  CFF / Type2 charstring (CID FDArray/FDSelect + seac)         [fontimpl]
//   7.  Спрямление контуров, хинтинг, синтетический bold / oblique   [fontimpl]
//   8.  Растеризатор покрытия и генератор SDF                        [fontimpl]
//   9.  Метрики, отрисовка глифов, атласный shelf-упаковщик         [fontimpl]
//   10. Методы Font, FontManager, процедурный встроенный шрифт
//   11. Помощники UTF-8
//
// Все внутренние помощники живут в `crossrender::fontimpl`; одна using-директива
// в `crossrender` оставляет остаток файла читаемым без игр с вложенными
// анонимными пространствами имён.
//
// stb_truetype намеренно НЕ используется: конструкторы контуров glyf и CFF и
// растеризатор здесь оригинальные, чтобы оба варианта шли по одному пути кода.
#include "crossrender/text/Font.h"

#include "text/FontInternal.h"
#include "crossrender/core/Log.h"
#include "crossrender/core/File.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <utility>
#include <algorithm>
#include <unordered_map>

namespace crossrender {
namespace fontimpl {

// ===========================================================================
// 1. Малые помощники
// ===========================================================================
u8 CoverageByte(f32 v, f32 gamma) {
    f32 c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    if (gamma > 0.0f && std::fabs(gamma - 1.0f) > 1e-4f) c = std::pow(c, gamma);
    return static_cast<u8>(c * 255.0f + 0.5f);
}

f32 SegDistSq(f32 px, f32 py, f32 ax, f32 ay, f32 bx, f32 by) {
    f32 dx = bx - ax, dy = by - ay;
    f32 l2 = dx * dx + dy * dy;
    f32 t = 0.0f;
    if (l2 > 1e-12f) {
        t = ((px - ax) * dx + (py - ay) * dy) / l2;
        if (t < 0.0f) t = 0.0f;
        else if (t > 1.0f) t = 1.0f;
    }
    f32 cx = ax + dx * t - px;
    f32 cy = ay + dy * t - py;
    return cx * cx + cy * cy;
}

// ===========================================================================
// 2. Big-endian читатель с проверкой границ
// ===========================================================================
class Reader {
public:
    Reader(const u8* base, usize size, usize offset = 0) : base_(base), size_(size), pos_(offset) {
        if (offset > size) pos_ = size;
    }

    void Seek(usize p) { pos_ = p > size_ ? size_ : p; }
    void Skip(i64 n) {
        if (n >= 0) {
            usize d = static_cast<usize>(n);
            pos_ = (d > size_ - pos_) ? size_ : pos_ + d;
        } else {
            usize d = static_cast<usize>(-n);
            pos_ = (d > pos_) ? 0 : pos_ - d;
        }
    }
    [[nodiscard]] usize Pos() const { return pos_; }
    [[nodiscard]] usize Size() const { return size_; }
    [[nodiscard]] usize Remaining() const { return size_ - pos_; }
    [[nodiscard]] bool Eof() const { return pos_ >= size_; }
    [[nodiscard]] bool InRange(usize off, usize len) const { return off <= size_ && len <= size_ - off; }

    bool U8(u8* out) {
        if (pos_ + 1 > size_) return Fail();
        *out = base_[pos_++];
        return true;
    }
    bool I8(i8* out) {
        u8 v = 0;
        if (!U8(&v)) return false;
        *out = static_cast<i8>(v);
        return true;
    }
    bool U16(u16* out) {
        if (pos_ + 2 > size_) return Fail();
        *out = static_cast<u16>((base_[pos_] << 8) | base_[pos_ + 1]);
        pos_ += 2;
        return true;
    }
    bool I16(i16* out) {
        u16 v = 0;
        if (!U16(&v)) return false;
        *out = static_cast<i16>(v);
        return true;
    }
    bool U24(u32* out) {
        if (pos_ + 3 > size_) return Fail();
        *out = (static_cast<u32>(base_[pos_]) << 16) | (static_cast<u32>(base_[pos_ + 1]) << 8) |
               static_cast<u32>(base_[pos_ + 2]);
        pos_ += 3;
        return true;
    }
    bool U32(u32* out) {
        if (pos_ + 4 > size_) return Fail();
        *out = (static_cast<u32>(base_[pos_]) << 24) | (static_cast<u32>(base_[pos_ + 1]) << 16) |
               (static_cast<u32>(base_[pos_ + 2]) << 8) | static_cast<u32>(base_[pos_ + 3]);
        pos_ += 4;
        return true;
    }
    bool U64(u64* out) {
        u32 hi = 0, lo = 0;
        if (!U32(&hi) || !U32(&lo)) return false;
        *out = (static_cast<u64>(hi) << 32) | static_cast<u64>(lo);
        return true;
    }
    bool I64(i64* out) {
        u64 v = 0;
        if (!U64(&v)) return false;
        *out = static_cast<i64>(v);
        return true;
    }
    bool U8At(usize off, u8* out) const {
        if (!InRange(off, 1)) return false;
        *out = base_[off];
        return true;
    }
    bool I8At(usize off, i8* out) const {
        u8 v = 0;
        if (!U8At(off, &v)) return false;
        *out = static_cast<i8>(v);
        return true;
    }
    bool U16At(usize off, u16* out) const {
        if (!InRange(off, 2)) return false;
        *out = static_cast<u16>((base_[off] << 8) | base_[off + 1]);
        return true;
    }
    bool I16At(usize off, i16* out) const {
        u16 v = 0;
        if (!U16At(off, &v)) return false;
        *out = static_cast<i16>(v);
        return true;
    }
    bool U32At(usize off, u32* out) const {
        if (!InRange(off, 4)) return false;
        *out = (static_cast<u32>(base_[off]) << 24) | (static_cast<u32>(base_[off + 1]) << 16) |
               (static_cast<u32>(base_[off + 2]) << 8) | static_cast<u32>(base_[off + 3]);
        return true;
    }

private:
    bool Fail() {
        fail_ = true;
        pos_ = size_;
        return false;
    }

    const u8* base_ = nullptr;
    usize size_ = 0;
    usize pos_ = 0;
    bool fail_ = false;
};

struct TableRec {
    u32 tag = 0;
    u32 offset = 0;
    u32 length = 0;
    bool present = false;
};

constexpr u32 kTagHead = 0x68656164;  // 'head'
constexpr u32 kTagHhea = 0x68686561;  // 'hhea'
constexpr u32 kTagMaxp = 0x6D617870;  // 'maxp'
constexpr u32 kTagHmtx = 0x686D7478;  // 'hmtx'
constexpr u32 kTagLoca = 0x6C6F6361;  // 'loca'
constexpr u32 kTagGlyf = 0x676C7966;  // 'glyf'
constexpr u32 kTagCmap = 0x636D6170;  // 'cmap'
constexpr u32 kTagKern = 0x6B65726E;  // 'kern'
constexpr u32 kTagOs2 = 0x4F532F32;   // 'OS/2'
constexpr u32 kTagName = 0x6E616D65;  // 'name'
constexpr u32 kTagPost = 0x706F7374;  // 'post'
constexpr u32 kTagCff = 0x43464620;   // 'CFF '
constexpr u32 kTagCff2 = 0x43464632;  // 'CFF2'
constexpr u32 kTagGpos = 0x47504F53;  // 'GPOS'
constexpr u32 kTagTtcf = 0x74746366;  // 'ttcf'

// ===========================================================================
// 3. Простые парсеры таблиц
// ===========================================================================
struct HeadTable {
    f32 unitsPerEm = 1000.0f;
    int indexToLocFormat = 0;
    f32 xMin = 0, yMin = 0, xMax = 0, yMax = 0;
};

struct HheaTable {
    f32 ascender = 0, descender = 0, lineGap = 0;
    u32 numberOfHMetrics = 0;
};

struct Os2Table {
    u16 version = 0;
    f32 typoAscender = 0, typoDescender = 0, typoLineGap = 0;
    f32 winAscent = 0, winDescent = 0;
    f32 capHeight = 0, xHeight = 0;
    bool hasCap = false, hasX = false, hasTypo = false, hasWin = false;
    bool useTypoMetrics = false;
};

struct PostTable {
    f32 underlinePosition = 0, underlineThickness = 0, italicAngle = 0;
};

// Вершина контура: `on` отличает точки на кривой от опорных точек
// квадратичных/кубических кривых.  Единицы — шрифтовые единицы до масштабирования.
struct Vec2f {
    f32 x = 0, y = 0;
    bool on = true;
};

enum class CmdType : u8 { MoveTo, LineTo, QuadTo, CubicTo };

// Спрямлённая команда рисования.  QuadTo/CubicTo несут свои опорные точки;
// конечная точка — первая точка *следующей* команды (или начало контура).
struct Cmd {
    CmdType type = CmdType::MoveTo;
    Vec2f p[3]{};
    bool close = false;
};

// Число опорных/конечных точек, реально используемых командой (остаток `Cmd::p`
// — заполненные нулями слоты, которые не должны влиять на границы).
[[nodiscard]] inline usize CmdPointCount(CmdType t) {
    switch (t) {
        case CmdType::QuadTo: return 2;
        case CmdType::CubicTo: return 3;
        default: return 1;
    }
}

// Спрямлённый (хордовый) сегмент плюс максимальное отклонение истинной кривой от
// этой хорды, используемое для безопасного расширения корзин растеризатора.
struct Seg {
    f32 ax = 0, ay = 0, bx = 0, by = 0;
    f32 dev = 0;
};

struct RenderedGlyph {
    std::vector<u8> bitmap;
    int width = 0, height = 0;
    f32 bearingX = 0, bearingY = 0;
    f32 advance = 0;
    bool blank = false;
};

// Определено в разделе CFF ниже.
struct CffFont;

// Зазор по умолчанию между shelf-упакованными глифами на странице атласа;
// `atlasPadding` шрифта может его увеличить (0 отключает зазор).
constexpr int kAtlasGap = 1;

[[nodiscard]] inline int AtlasGapFor(const FontDesc& desc) {
    if (desc.atlasPadding > 0) return desc.atlasPadding;
    return desc.atlasPadding < 0 ? kAtlasGap : 0;
}

// --- помощники mtx / loca / glyf -------------------------------------------
// Запись `loca` одного глифа, проверенная по границам таблицы.
bool LocateGlyph(const TableRec* loca, const TableRec* glyf, int indexToLocFormat, u32 numGlyphs, u32 glyphIndex,
                 const u8* data, usize size, usize* off, usize* len) {
    if (!loca || !glyf) return false;
    if (glyphIndex + 1 > numGlyphs) return false;
    usize entrySize = indexToLocFormat == 0 ? 2 : 4;
    usize base = loca->offset;
    usize need = (static_cast<usize>(glyphIndex) + 2) * entrySize;
    if (need > loca->length) return false;
    Reader r(data, size);
    u32 start = 0, end = 0;
    if (entrySize == 2) {
        u16 s = 0, e = 0;
        if (!r.U16At(base + static_cast<usize>(glyphIndex) * 2, &s)) return false;
        if (!r.U16At(base + (static_cast<usize>(glyphIndex) + 1) * 2, &e)) return false;
        start = static_cast<u32>(s) * 2;
        end = static_cast<u32>(e) * 2;
    } else {
        if (!r.U32At(base + static_cast<usize>(glyphIndex) * 4, &start)) return false;
        if (!r.U32At(base + (static_cast<usize>(glyphIndex) + 1) * 4, &end)) return false;
    }
    if (end < start) return false;
    usize gOff = static_cast<usize>(glyf->offset) + start;
    usize gLen = static_cast<usize>(end - start);
    if (gOff > size) return false;
    if (gOff + gLen > size) gLen = size - gOff;
    usize tableEnd = static_cast<usize>(glyf->offset) + glyf->length;
    if (gOff > tableEnd) return false;
    if (gOff + gLen > tableEnd) gLen = tableEnd - gOff;
    *off = gOff;
    *len = gLen;
    return true;
}

struct CmapSubtable {
    u16 format = 0;
    u16 platform = 0;
    u16 encoding = 0;
    u32 offset = 0;
    u32 length = 0;
    int score = 0;
};

bool CmapLookupFormat0(const u8* base, usize size, u32 off, u32 len, u32 cp, u32* out) {
    if (cp > 255 || len < 262) return false;
    Reader r(base, size);
    u8 gid = 0;
    if (!r.U8At(off + 6 + cp, &gid)) return false;
    if (gid == 0) return false;
    *out = gid;
    return true;
}

bool CmapLookupFormat4(const u8* base, usize size, u32 off, u32 len, u32 cp, u32* out) {
    Reader r(base, size, off);
    u16 format = 0, length = 0, segCountX2 = 0;
    if (!r.U16(&format) || !r.U16(&length) || format != 4) return false;
    r.Skip(2);  // language
    if (!r.U16(&segCountX2)) return false;
    u32 segCount = segCountX2 / 2;
    if (segCount == 0 || segCount > 32768) return false;
    r.Skip(6);  // searchRange, entrySelector, rangeShift

    usize endCodes = r.Pos();
    if (!r.InRange(endCodes, static_cast<usize>(segCount) * 2)) return false;
    usize startCodes = endCodes + static_cast<usize>(segCount) * 2 + 2;  // reservedPad
    usize idDeltas = startCodes + static_cast<usize>(segCount) * 2;
    usize idRangeOffsets = idDeltas + static_cast<usize>(segCount) * 2;
    if (!r.InRange(startCodes, static_cast<usize>(segCount) * 2)) return false;
    if (!r.InRange(idRangeOffsets, static_cast<usize>(segCount) * 2)) return false;
    // Ограничиваем обращения меньшим из двух: объявленной длиной подтаблицы
    // или охватывающей таблицей.
    u32 avail = static_cast<u32>(size - idRangeOffsets);
    if (len >= idRangeOffsets + 2 && len < size) {
        u32 declared = len - static_cast<u32>(idRangeOffsets);
        if (declared < avail) avail = declared;
    }

    if (cp > 0xFFFF) return false;
    u16 code = static_cast<u16>(cp);
    for (u32 i = 0; i < segCount; ++i) {
        u16 end = 0, start = 0;
        if (!r.U16At(endCodes + i * 2, &end) || !r.U16At(startCodes + i * 2, &start)) return false;
        if (code > end || code < start) continue;
        i16 delta = 0;
        u16 rangeOffset = 0;
        if (!r.I16At(idDeltas + i * 2, &delta) || !r.U16At(idRangeOffsets + i * 2, &rangeOffset)) return false;
        if (rangeOffset == 0) {
            *out = (static_cast<u32>(code) + static_cast<u32>(static_cast<u16>(delta))) & 0xFFFFu;
            return *out != 0;
        }
        usize glyphOff = idRangeOffsets + i * 2 + rangeOffset + static_cast<usize>(code - start) * 2;
        if (glyphOff + 2 > idRangeOffsets + avail) return false;
        u16 gid = 0;
        if (!r.U16At(glyphOff, &gid)) return false;
        if (gid == 0) return false;
        *out = (static_cast<u32>(gid) + static_cast<u32>(static_cast<u16>(delta))) & 0xFFFFu;
        return *out != 0;
    }
    return false;
}

bool CmapLookupFormat6(const u8* base, usize size, u32 off, u32 len, u32 cp, u32* out) {
    Reader r(base, size, off);
    u16 format = 0, length = 0, language = 0, first = 0, count = 0;
    if (!r.U16(&format) || !r.U16(&length) || !r.U16(&language) || !r.U16(&first) || !r.U16(&count))
        return false;
    if (format != 6) return false;
    if (cp < first || cp - first >= count) return false;
    usize entry = off + 10 + static_cast<usize>(cp - first) * 2;
    if (len != 0 && entry + 2 > static_cast<usize>(off) + len) return false;
    u16 gid = 0;
    if (!r.U16At(entry, &gid)) return false;
    if (gid == 0) return false;
    *out = gid;
    return true;
}

bool CmapLookupFormat12(const u8* base, usize size, u32 off, u32 len, u32 cp, u32* out) {
    Reader r(base, size, off);
    u16 format = 0, reserved = 0;
    u32 length = 0, nGroups = 0;
    if (!r.U16(&format) || !r.U16(&reserved) || !r.U32(&length) || format != 12) return false;
    r.Skip(4);  // language
    if (!r.U32(&nGroups)) return false;
    if (nGroups > 0x100000) return false;
    usize groups = off + 16;
    if (!r.InRange(groups, static_cast<usize>(nGroups) * 12)) return false;
    if (len != 0 && groups + static_cast<usize>(nGroups) * 12 > static_cast<usize>(off) + len) return false;
    u32 lo = 0, hi = nGroups;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        u32 sc = 0, ec = 0, sg = 0;
        usize e = groups + static_cast<usize>(mid) * 12;
        if (!r.U32At(e, &sc) || !r.U32At(e + 4, &ec) || !r.U32At(e + 8, &sg)) return false;
        if (cp < sc) {
            hi = mid;
        } else if (cp > ec) {
            lo = mid + 1;
        } else {
            *out = sg + (cp - sc);
            return true;
        }
    }
    return false;
}

bool CmapLookupFormat13(const u8* base, usize size, u32 off, u32 len, u32 cp, u32* out) {
    Reader r(base, size, off);
    u16 format = 0, reserved = 0;
    u32 length = 0, nGroups = 0;
    if (!r.U16(&format) || !r.U16(&reserved) || !r.U32(&length) || format != 13) return false;
    r.Skip(4);  // language
    if (!r.U32(&nGroups)) return false;
    if (nGroups > 0x100000) return false;
    usize groups = off + 16;
    if (!r.InRange(groups, static_cast<usize>(nGroups) * 12)) return false;
    if (len != 0 && groups + static_cast<usize>(nGroups) * 12 > static_cast<usize>(off) + len) return false;
    for (u32 i = 0; i < nGroups; ++i) {
        u32 sc = 0, ec = 0, sg = 0;
        usize e = groups + static_cast<usize>(i) * 12;
        if (!r.U32At(e, &sc) || !r.U32At(e + 4, &ec) || !r.U32At(e + 8, &sg)) return false;
        if (cp >= sc && cp <= ec) {
            *out = sg;
            return true;
        }
    }
    return false;
}

struct CmapTable {
    std::vector<CmapSubtable> subs;

    bool Lookup(const u8* base, usize size, u32 cp, u32* out) const {
        for (const CmapSubtable& s : subs) {
            switch (s.format) {
                case 0:
                    if (CmapLookupFormat0(base, size, s.offset, s.length, cp, out)) return true;
                    break;
                case 4:
                    if (CmapLookupFormat4(base, size, s.offset, s.length, cp, out)) return true;
                    break;
                case 6:
                    if (CmapLookupFormat6(base, size, s.offset, s.length, cp, out)) return true;
                    break;
                case 12:
                    if (CmapLookupFormat12(base, size, s.offset, s.length, cp, out)) return true;
                    break;
                case 13:
                    if (CmapLookupFormat13(base, size, s.offset, s.length, cp, out)) return true;
                    break;
                default:
                    break;
            }
        }
        return false;
    }

    // Формат 14: Unicode-вариационные последовательности.  Запись по умолчанию
    // сводится к базовому отображению (возвращает true с глифом 0).
    bool LookupVariation(const u8* base, usize size, u32 cp, u32 selector, u32* out) const {
        Reader r(base, size);
        for (const CmapSubtable& s : subs) {
            if (s.format != 14) continue;
            u16 format = 0;
            u32 length = 0, numRecords = 0;
            if (!r.U16At(s.offset, &format) || format != 14) continue;
            if (!r.U32At(s.offset + 2, &length) || !r.U32At(s.offset + 6, &numRecords)) continue;
            if (numRecords > 0x10000) continue;
            usize recs = s.offset + 10;
            if (!r.InRange(recs, static_cast<usize>(numRecords) * 11)) continue;
            for (u32 i = 0; i < numRecords; ++i) {
                usize p = recs + static_cast<usize>(i) * 11;
                u32 var = 0;
                u8 defOff = 0, nonDefOff = 0;
                if (!r.U32At(p, &var) || !r.U8At(p + 4, &defOff) || !r.U8At(p + 5, &nonDefOff)) break;
                if (var != selector) continue;
                if (nonDefOff != 0) {
                    u32 n = 0;
                    if (!r.U32At(s.offset + nonDefOff, &n)) continue;
                    if (n > 0x10000) continue;
                    for (u32 k = 0; k < n; ++k) {
                        usize e = s.offset + nonDefOff + 4 + static_cast<usize>(k) * 5;
                        u32 u = 0;
                        u16 g = 0;
                        if (!r.U32At(e, &u) || !r.U16At(e + 4, &g)) break;
                        if (u == cp) {
                            *out = g;
                            return true;
                        }
                    }
                }
                if (defOff != 0) {
                    u32 n = 0;
                    if (!r.U32At(s.offset + defOff, &n)) continue;
                    if (n > 0x10000) continue;
                    for (u32 k = 0; k < n; ++k) {
                        usize e = s.offset + defOff + 4 + static_cast<usize>(k) * 4;
                        u32 u = 0, extra = 0;
                        if (!r.U32At(e, &u) || !r.U32At(e + 4, &extra)) break;
                        if (u <= cp && cp <= u + extra) {
                            *out = 0;
                            return true;
                        }
                    }
                }
                return false;
            }
        }
        return false;
    }
};

int CmapSubtableScore(u16 platform, u16 encoding, u16 format) {
    if (format == 14) return -1000;  // вариационные селекторы базовые символы не отображают
    int score = 10;
    switch (platform) {
        case 0: score = 100; break;  // Unicode
        case 3: score = 90; break;   // Windows
        case 1: score = 40; break;   // Macintosh
        default: break;
    }
    if (platform == 3 && encoding == 10) score += 20;  // Windows UCS-4
    if (platform == 3 && encoding == 1) score += 15;   // Windows BMP
    if (platform == 0 && encoding >= 4) score += 15;
    if (format == 12 || format == 13) score += 25;
    else if (format == 4) score += 12;
    else if (format == 6) score += 6;
    return score;
}

bool ParseCmap(const u8* base, usize size, const TableRec& t, CmapTable* out) {
    if (!t.present || t.length < 4) return false;
    Reader r(base, size, t.offset);
    u16 version = 0, numTables = 0;
    if (!r.U16(&version) || !r.U16(&numTables)) return false;
    (void)version;
    if (numTables == 0 || numTables > 1024) return false;
    for (u32 i = 0; i < numTables; ++i) {
        usize rec = t.offset + 4 + static_cast<usize>(i) * 8;
        u16 platform = 0, encoding = 0;
        u32 off = 0;
        if (!r.U16At(rec, &platform) || !r.U16At(rec + 2, &encoding) || !r.U32At(rec + 4, &off)) return false;
        if (off == 0 || off >= t.length) continue;
        usize abs = t.offset + off;
        u16 format = 0;
        if (!r.U16At(abs, &format)) continue;
        if (format != 0 && format != 4 && format != 6 && format != 12 && format != 13 && format != 14) {
            ENG_LOGW("font", "cmap: skipping unsupported subtable format %u", static_cast<unsigned>(format));
            continue;
        }
        u32 length = t.length - off;
        if (format == 0 || format == 4 || format == 6) {
            u16 len16 = 0;
            if (r.U16At(abs + 2, &len16) && len16 >= 4) length = len16 < length ? len16 : length;
        } else {
            u32 len32 = 0;
            if (r.U32At(abs + 4, &len32) && len32 >= 4) length = len32 < length ? len32 : length;
        }
        CmapSubtable s;
        s.format = format;
        s.platform = platform;
        s.encoding = encoding;
        s.offset = static_cast<u32>(abs);
        s.length = length;
        s.score = CmapSubtableScore(platform, encoding, format);
        out->subs.push_back(s);
    }
    std::stable_sort(out->subs.begin(), out->subs.end(),
                     [](const CmapSubtable& a, const CmapSubtable& b) { return a.score > b.score; });
    return !out->subs.empty();
}

// --- kern (подтаблицы Windows формата 0) ------------------------------------
struct KernTable {
    std::unordered_map<u32, f32> pairs;  // (left << 16) | right

    f32 Lookup(u32 left, u32 right) const {
        auto it = pairs.find((left << 16) | (right & 0xFFFF));
        return it == pairs.end() ? 0.0f : it->second;
    }
};

void ParseKern(const u8* base, usize size, const TableRec& t, KernTable* out) {
    if (!t.present || t.length < 4) return;
    Reader r(base, size, t.offset);
    u16 version = 0, nTables = 0;
    if (!r.U16(&version) || !r.U16(&nTables)) return;
    if (nTables > 4096) return;
    if (version != 0) {
        // `kern` от Apple начинается с 0x00010000 и 32-битного числа подтаблиц.
        ENG_LOGW("font", "kern: unsupported version %u (Apple), kerning disabled", static_cast<unsigned>(version));
        return;
    }
    usize p = t.offset + 4;
    for (u32 i = 0; i < nTables; ++i) {
        u16 subVersion = 0, subLength = 0, coverage = 0;
        if (!r.U16At(p, &subVersion) || !r.U16At(p + 2, &subLength) || !r.U16At(p + 4, &coverage)) return;
        (void)subVersion;
        if (subLength < 6) return;
        usize next = p + subLength;
        if (next > t.offset + t.length || next > size) return;
        u16 format = static_cast<u16>(coverage >> 8);
        bool horizontal = (coverage & 0x1) != 0;
        bool crossStream = (coverage & 0x4) != 0;
        if (format == 0 && horizontal && !crossStream) {
            u16 nPairs = 0;
            if (r.U16At(p + 6, &nPairs)) {
                usize entries = p + 14;
                if (entries + static_cast<usize>(nPairs) * 6 <= next) {
                    for (u32 k = 0; k < nPairs; ++k) {
                        usize e = entries + static_cast<usize>(k) * 6;
                        u16 l = 0, rr = 0;
                        i16 v = 0;
                        if (!r.U16At(e, &l) || !r.U16At(e + 2, &rr) || !r.I16At(e + 4, &v)) break;
                        if (v == 0) continue;
                        u32 key = (static_cast<u32>(l) << 16) | rr;
                        if (out->pairs.find(key) == out->pairs.end()) out->pairs.emplace(key, static_cast<f32>(v));
                    }
                }
            }
        } else if (format == 2 || format == 3) {
            ENG_LOGW("font", "kern: ignoring Apple format %u subtable", static_cast<unsigned>(format));
        } else if (format != 0) {
            ENG_LOGW("font", "kern: ignoring unsupported format %u subtable", static_cast<unsigned>(format));
        }
        p = next;
    }
    if (!out->pairs.empty()) ENG_LOGI("font", "kern: %zu pairs parsed", out->pairs.size());
}

// --- GPOS PairPos формат 1 --------------------------------------------------
// Намеренно минималистично: используется первая подтаблица PairPos первого
// lookup-а PairPos, без учёта выбора ScriptList/FeatureList (полноценный шейпер
// выбрал бы фичу `kern` для активного письма).  Это покрывает частый случай
// латинского шрифта, у которого единственный lookup PairPos — кернинг.
struct GposKern {
    struct PairSet {
        std::vector<std::pair<u16, f32>> values;  // второй глиф -> дельта шага по x
    };
    std::vector<u16> coverage;      // первые глифы, по возрастанию
    std::vector<PairSet> pairSets;  // parallel to coverage

    f32 Lookup(u32 left, u32 right) const {
        if (left > 0xFFFF || right > 0xFFFF) return 0.0f;
        u16 l = static_cast<u16>(left), rr = static_cast<u16>(right);
        usize lo = 0, hi = coverage.size();
        while (lo < hi) {
            usize mid = lo + (hi - lo) / 2;
            if (coverage[mid] < l) lo = mid + 1;
            else if (coverage[mid] > l) hi = mid;
            else {
                for (const auto& kv : pairSets[mid].values)
                    if (kv.first == rr) return kv.second;
                return 0.0f;
            }
        }
        return 0.0f;
    }
};

bool ParseGposCoverage(const u8* base, usize size, usize off, std::vector<u16>* out) {
    Reader r(base, size);
    u16 format = 0, count = 0;
    if (!r.U16At(off, &format) || !r.U16At(off + 2, &count)) return false;
    if (format == 1) {
        if (!r.InRange(off + 4, static_cast<usize>(count) * 2)) return false;
        for (u32 i = 0; i < count; ++i) {
            u16 g = 0;
            if (!r.U16At(off + 4 + static_cast<usize>(i) * 2, &g)) return false;
            out->push_back(g);
        }
    } else if (format == 2) {
        usize p = off + 4;
        if (!r.InRange(p, static_cast<usize>(count) * 6)) return false;
        for (u32 i = 0; i < count; ++i) {
            u16 start = 0, end = 0, startCoverage = 0;
            if (!r.U16At(p + static_cast<usize>(i) * 6, &start)) return false;
            if (!r.U16At(p + static_cast<usize>(i) * 6 + 2, &end)) return false;
            if (!r.U16At(p + static_cast<usize>(i) * 6 + 4, &startCoverage)) return false;
            (void)startCoverage;
            if (end < start) continue;
            u32 n = static_cast<u32>(end - start) + 1;
            if (n > 65536) return false;
            for (u32 k = 0; k < n; ++k) out->push_back(static_cast<u16>(start + k));
        }
    } else {
        return false;
    }
    return true;
}

void ParseGpos(const u8* base, usize size, const TableRec& t, GposKern* out) {
    if (!t.present || t.length < 10) return;
    Reader r(base, size);
    u16 major = 0, minor = 0, scriptList = 0, featureList = 0, lookupList = 0;
    if (!r.U16At(t.offset, &major) || !r.U16At(t.offset + 2, &minor) || !r.U16At(t.offset + 4, &scriptList) ||
        !r.U16At(t.offset + 6, &featureList) || !r.U16At(t.offset + 8, &lookupList))
        return;
    (void)minor;
    (void)scriptList;
    (void)featureList;
    if (major != 1 || lookupList == 0 || lookupList >= t.length) return;
    usize tableEnd = t.offset + t.length;
    usize lookupOff = t.offset + lookupList;
    u16 lookupCount = 0;
    if (!r.U16At(lookupOff, &lookupCount) || lookupCount > 4096) return;

    auto valueSize = [](u16 fmt) {
        u32 n = 0;
        for (int b = 0; b < 8; ++b)
            if (fmt & (1u << b)) ++n;
        return n * 2;
    };

    for (u32 i = 0; i < lookupCount; ++i) {
        u16 subOff = 0;
        if (!r.U16At(lookupOff + 2 + static_cast<usize>(i) * 2, &subOff)) return;
        usize lookup = lookupOff + subOff;
        u16 lookupType = 0, lookupFlag = 0, subCount = 0;
        if (!r.U16At(lookup, &lookupType) || !r.U16At(lookup + 2, &lookupFlag) || !r.U16At(lookup + 4, &subCount))
            return;
        (void)lookupFlag;
        if (lookupType != 2) continue;  // 2 == PairPos
        for (u32 s = 0; s < subCount; ++s) {
            u16 so = 0;
            if (!r.U16At(lookup + 6 + static_cast<usize>(s) * 2, &so)) return;
            usize sub = lookup + so;
            u16 format = 0, coverageOff = 0, valueFormat1 = 0, valueFormat2 = 0, valueCount = 0;
            if (!r.U16At(sub, &format) || !r.U16At(sub + 2, &coverageOff) || !r.U16At(sub + 4, &valueFormat1) ||
                !r.U16At(sub + 6, &valueFormat2) || !r.U16At(sub + 8, &valueCount))
                return;
            if (format != 1 || valueCount > 32768) continue;
            std::vector<u16> cov;
            if (!ParseGposCoverage(base, size, sub + coverageOff, &cov)) continue;
            if (cov.size() != valueCount) continue;
            u32 v1 = valueSize(valueFormat1);
            u32 v2 = valueSize(valueFormat2);
            bool hasXAdvance = (valueFormat1 & 0x0004) != 0;
            usize stride = 2 + v1 + v2;
            if (stride < 2) continue;
            std::vector<GposKern::PairSet> sets;
            sets.reserve(valueCount);
            bool any = false;
            for (u32 pi = 0; pi < valueCount; ++pi) {
                u16 pairSetOff = 0;
                if (!r.U16At(sub + 10 + static_cast<usize>(pi) * 2, &pairSetOff)) break;
                usize ps = sub + pairSetOff;
                u16 pairValueCount = 0;
                if (!r.U16At(ps, &pairValueCount)) break;
                usize entry = ps + 2;
                if (entry + static_cast<usize>(pairValueCount) * stride > tableEnd) break;
                GposKern::PairSet set;
                set.values.reserve(pairValueCount);
                for (u32 k = 0; k < pairValueCount; ++k) {
                    usize e = entry + static_cast<usize>(k) * stride;
                    u16 second = 0;
                    if (!r.U16At(e, &second)) break;
                    f32 adv = 0.0f;
                    if (hasXAdvance && v1 >= 6) {
                        i16 xa = 0;
                        if (r.I16At(e + 2 + 4, &xa)) adv = static_cast<f32>(xa);
                    }
                    if (adv != 0.0f) {
                        set.values.emplace_back(second, adv);
                        any = true;
                    }
                }
                sets.push_back(std::move(set));
            }
            if (!any) continue;
            for (usize k = 0; k < sets.size(); ++k) {
                u16 first = cov[k];
                usize lo = 0, hi = out->coverage.size();
                while (lo < hi) {
                    usize mid = lo + (hi - lo) / 2;
                    if (out->coverage[mid] < first) lo = mid + 1;
                    else hi = mid;
                }
                out->coverage.insert(out->coverage.begin() + static_cast<std::ptrdiff_t>(lo), first);
                out->pairSets.insert(out->pairSets.begin() + static_cast<std::ptrdiff_t>(lo), std::move(sets[k]));
            }
            ENG_LOGI("font", "GPOS: PairPos format 1, %zu covered first glyphs", out->coverage.size());
            return;
        }
    }
}

// --- name ------------------------------------------------------------------
std::string Utf16BeToUtf8(const u8* p, usize bytes) {
    std::string out;
    out.reserve(bytes);
    usize i = 0;
    while (i + 1 < bytes) {
        u32 u = (static_cast<u32>(p[i]) << 8) | p[i + 1];
        i += 2;
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < bytes) {
            u32 lo = (static_cast<u32>(p[i]) << 8) | p[i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            } else {
                u = 0xFFFD;
            }
        } else if (u >= 0xD800 && u <= 0xDFFF) {
            u = 0xFFFD;
        }
        if (u < 0x80) {
            out.push_back(static_cast<char>(u));
        } else if (u < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (u >> 6)));
            out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
        } else if (u < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (u >> 12)));
            out.push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (u >> 18)));
            out.push_back(static_cast<char>(0x80 | ((u >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
        }
    }
    return out;
}

std::string Latin1ToUtf8(const u8* p, usize bytes) {
    std::string out;
    out.reserve(bytes);
    for (usize i = 0; i < bytes; ++i) {
        u32 u = p[i];
        if (u < 0x80) {
            out.push_back(static_cast<char>(u));
        } else {
            out.push_back(static_cast<char>(0xC0 | (u >> 6)));
            out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
        }
    }
    return out;
}

struct NameEntry {
    int priority = 0;
    std::string value;
};

void ConsiderName(NameEntry* e, int priority, const std::string& v) {
    if (v.empty()) return;
    if (priority > e->priority) {
        e->priority = priority;
        e->value = v;
    }
}

void ParseName(const u8* base, usize size, const TableRec& t, std::string* family, std::string* style) {
    if (!t.present || t.length < 6) return;
    Reader r(base, size);
    u16 format = 0, count = 0, stringOffset = 0;
    if (!r.U16At(t.offset, &format) || !r.U16At(t.offset + 2, &count) || !r.U16At(t.offset + 4, &stringOffset))
        return;
    (void)format;
    if (count > 4096) return;
    usize stringBase = t.offset + stringOffset;
    NameEntry fam, sty;
    for (u32 i = 0; i < count; ++i) {
        usize rec = t.offset + 6 + static_cast<usize>(i) * 12;
        u16 platform = 0, encoding = 0, language = 0, nameId = 0, length = 0, offset = 0;
        if (!r.U16At(rec, &platform) || !r.U16At(rec + 2, &encoding) || !r.U16At(rec + 4, &language) ||
            !r.U16At(rec + 6, &nameId) || !r.U16At(rec + 8, &length) || !r.U16At(rec + 10, &offset))
            return;
        (void)language;
        if (nameId != 1 && nameId != 2) continue;
        usize abs = stringBase + offset;
        if (!r.InRange(abs, length)) continue;
        std::string v;
        int priority = 0;
        if (platform == 3 && encoding == 1) {
            v = Utf16BeToUtf8(base + abs, length);
            priority = 40;
        } else if (platform == 3 && encoding == 10) {
            v = Utf16BeToUtf8(base + abs, length);
            priority = 35;
        } else if (platform == 0) {
            v = Utf16BeToUtf8(base + abs, length);
            priority = 30;
        } else if (platform == 1 && encoding == 0) {
            v = Latin1ToUtf8(base + abs, length);
            priority = 20;
        } else {
            continue;
        }
        if (nameId == 1) ConsiderName(&fam, priority, v);
        else ConsiderName(&sty, priority, v);
    }
    *family = fam.value;
    *style = sty.value;
}

}  // namespace fontimpl
}  // namespace crossrender

namespace crossrender {

// Внутренние помощники используются без квалификации в остальной части файла.
using namespace fontimpl;

// ===========================================================================
// Font::Impl
// ===========================================================================
struct Font::Impl {
    std::vector<u8> owned;
    const u8* data = nullptr;
    usize size = 0;

    std::unordered_map<u32, TableRec> tables;
    HeadTable head;
    HheaTable hhea;
    Os2Table os2;
    PostTable post;
    CmapTable cmap;
    KernTable kern;
    GposKern gpos;
    u32 numGlyphs = 0;
    bool isCff = false;
    bool isCid = false;

    u64 atlasUsedPixels = 0;
    u64 atlasCapacityPixels = 0;

    f32 procUnitsPerEm = 1024.0f;
    f32 procAdvance = 1024.0f * 0.62f;
    bool procedural = false;
    std::vector<u8> procOutline;

    std::shared_ptr<CffFont> cff;
    std::unordered_map<u32, f32> cffAdvances;

    // Пиксели глифов, которые не удалось выгрузить, потому что текстура страницы
    // ещё не существовала (нет GL-контекста).  Проигрываются, как только контекст появится.
    struct PendingUpload {
        int x = 0, y = 0, w = 0, h = 0;
        std::vector<u8> rgba;
    };
    std::array<std::vector<PendingUpload>, 8> pending;
    bool hasPending = false;

    [[nodiscard]] bool InRange(usize off, usize len) const { return off <= size && len <= size - off; }
    [[nodiscard]] const TableRec* Table(u32 tag) const {
        auto it = tables.find(tag);
        return it == tables.end() ? nullptr : &it->second;
    }
};

}  // namespace crossrender

namespace crossrender {
namespace fontimpl {

// Помощники, определённые ниже, но используемые методами Font и загрузчиком
// glyf: ширины шага и точка входа растеризатора глифов.
f32 AdvanceFor(Font::Impl* impl, u32 gid);
bool RenderGlyphToBitmap(Font::Impl* impl, const CffFont* cff, u32 gid, const FontDesc& desc, f32 scale,
                         RenderedGlyph* get);
bool RenderAnyGlyph(Font* font, Font::Impl* impl, u32 codepoint, u32 gid, RenderedGlyph* rg);
bool PlaceGlyph(Font* font, Font::Impl* impl, u32 codepoint, const RenderedGlyph& rg, f32 advance, Glyph* out);
bool ProcRender(Font::Impl* impl, u32 codepoint, const FontDesc& desc, RenderedGlyph* rg);
void FlushPendingUploads(Font* font, Font::Impl* impl);
bool BuildGlyphOutline(Font::Impl* impl, const CffFont* cff, u32 gid, f32 scale, f32 slant, bool applySlant,
                       GlyphOutline* out);
bool ProceduralGlyphOutline(Font::Impl* impl, u32 codepoint, f32 scale, GlyphOutline* out);

}  // namespace fontimpl
}  // namespace crossrender

namespace crossrender {

// ---------------------------------------------------------------------------
// Хук дружбы (объявлен в engine/src/text/FontInternal.h).  Статические члены
// дружественного класса могут добираться до приватных членов Font — так
// внутренние помощники и модульные тесты трогают ленивый кэш и страницы атласа,
// не расширяя публичный API.
// ---------------------------------------------------------------------------
struct FontTestAccess {
    using Impl = Font::Impl;

    static bool NewFont(std::unique_ptr<Font>* out, Impl** implOut) {
        auto f = std::unique_ptr<Font>(new Font());
        if (implOut) *implOut = f->impl_.get();
        *out = std::move(f);
        return true;
    }
    static Impl* Of(Font* font) { return font ? font->impl_.get() : nullptr; }
    static void SetProcedural(Font* font, bool on) {
        if (font && font->impl_) font->impl_->procedural = on;
    }
    static bool IsProcedural(Font* font) { return font && font->impl_ && font->impl_->procedural; }
    static void Publish(Font* font, Impl*, bool valid, FontFormat format) {
        if (!font) return;
        font->valid_ = valid;
        font->format_ = format;
    }
    static const Glyph* FindCached(Font* font, u32 codepoint) {
        if (!font) return nullptr;
        auto it = font->glyphCache_.find(codepoint);
        return it == font->glyphCache_.end() ? nullptr : &it->second;
    }
    static std::vector<FontAtlasPage>& Pages(Font* font) { return font->pages_; }
    static FontAtlasPage* AddPage(Font* font) {
        font->pages_.emplace_back();
        return &font->pages_.back();
    }
    static const FontDesc& DescOf(const Font* font) { return font->desc_; }
    static void SetDesc(Font* font, const FontDesc& desc) { font->desc_ = desc; }
    static f32 ScaleOf(const Font* font) {
        f32 upem = (font->impl_ && font->impl_->head.unitsPerEm > 0.0f) ? font->impl_->head.unitsPerEm : 1000.0f;
        f32 ph = font->desc_.pixelHeight > 0.0f ? font->desc_.pixelHeight : 1.0f;
        return ph / upem;
    }
    static void SetMetrics(Font* font, f32 asc, f32 desc, f32 gap, f32 cap, f32 xh, f32 ulPos, f32 ulThick,
                           f32 upem) {
        font->ascender_ = asc;
        font->descender_ = desc;
        font->lineGap_ = gap;
        font->lineHeight_ = asc - desc + gap;
        if (font->lineHeight_ <= 0.0f) font->lineHeight_ = font->desc_.pixelHeight;
        font->capHeight_ = cap;
        font->xHeight_ = xh;
        font->underlinePos_ = ulPos;
        font->underlineThick_ = ulThick;
        font->unitsPerEm_ = upem;
    }
    static void SetNames(Font* font, const std::string& family, const std::string& style, FontFormat fmt,
                         const std::string& source) {
        font->family_ = family;
        font->style_ = style;
        font->format_ = fmt;
        if (!source.empty()) font->source_ = source;
    }
    static void SetLineHeight(Font* font, f32 lh) { font->lineHeight_ = lh; }
    static bool Render(Font* font, u32 gid, std::vector<u8>* bitmap, int* w, int* h) {
        if (!font || !font->impl_) return false;
        RenderedGlyph rg;
        if (!RenderGlyphToBitmap(font->impl_.get(), font->impl_->cff.get(), gid, font->desc_, ScaleOf(font), &rg))
            return false;
        *bitmap = std::move(rg.bitmap);
        if (w) *w = rg.width;
        if (h) *h = rg.height;
        return true;
    }
    static f32 Advance(Font* font, u32 gid) { return font ? AdvanceFor(font->impl_.get(), gid) : 0.0f; }
    static std::vector<u8> ProceduralOutline(Font* font) {
        return font && font->impl_ ? font->impl_->procOutline : std::vector<u8>();
    }
};

}  // namespace crossrender

namespace crossrender {
namespace fontimpl {

// Точка входа растеризации глифов, общая для процедурного и файлового путей.
bool RenderAnyGlyph(Font* font, Font::Impl* impl, u32 codepoint, u32 gid, RenderedGlyph* rg) {
    if (impl->procedural) return ProcRender(impl, codepoint, FontTestAccess::DescOf(font), rg);
    f32 scale = FontTestAccess::ScaleOf(font);
    return RenderGlyphToBitmap(impl, impl->cff.get(), gid, FontTestAccess::DescOf(font), scale, rg);
}

struct Matrix2 {
    f32 a = 1, b = 0, c = 0, d = 1;  // по строкам: [a b ; c d]

    void Apply(f32 x, f32 y, f32* ox, f32* oy) const {
        *ox = a * x + c * y;
        *oy = b * x + d * y;
    }
    void Concat(const Matrix2& o) {  // this := this * o
        f32 na = a * o.a + c * o.b;
        f32 nb = b * o.a + d * o.b;
        f32 nc = a * o.c + c * o.d;
        f32 nd = b * o.c + d * o.d;
        a = na;
        b = nb;
        c = nc;
        d = nd;
    }
};

constexpr u32 kMaxCompositeDepth = 8;

// Находит запись `glyf` глифа через `loca`.
bool GlyfRecord(Font::Impl* impl, u32 glyphIndex, usize* off, usize* len) {
    const TableRec* loca = impl->Table(kTagLoca);
    const TableRec* glyf = impl->Table(kTagGlyf);
    if (!loca || !glyf) return false;
    if (glyphIndex + 1 > impl->numGlyphs) return false;
    usize entrySize = impl->head.indexToLocFormat == 0 ? 2 : 4;
    usize base = loca->offset;
    usize n = static_cast<usize>(glyphIndex);
    usize need = (n + 2) * entrySize;
    if (need > loca->length || !impl->InRange(base, need)) return false;
    u32 start = 0, end = 0;
    if (entrySize == 2) {
        u16 s = 0, e = 0;
        Reader r(impl->data, impl->size);
        if (!r.U16At(base + n * 2, &s) || !r.U16At(base + (n + 1) * 2, &e)) return false;
        start = static_cast<u32>(s) * 2;
        end = static_cast<u32>(e) * 2;
    } else {
        Reader r(impl->data, impl->size);
        if (!r.U32At(base + n * 4, &start) || !r.U32At(base + (n + 1) * 4, &end)) return false;
    }
    if (end < start) return false;
    usize gOff = static_cast<usize>(glyf->offset) + start;
    usize gLen = static_cast<usize>(end - start);
    if (!impl->InRange(gOff, gLen)) return false;
    if (gOff + gLen > static_cast<usize>(glyf->offset) + glyf->length) {
        if (gOff > static_cast<usize>(glyf->offset) + glyf->length) return false;
        gLen = static_cast<usize>(glyf->offset) + glyf->length - gOff;
    }
    *off = gOff;
    *len = gLen;
    return true;
}

bool ReadGlyfHeader(Font::Impl* impl, u32 glyphIndex, i16* numberOfContours, usize* bodyOff, usize* bodyLen) {
    usize off = 0, len = 0;
    if (!GlyfRecord(impl, glyphIndex, &off, &len)) return false;
    if (len == 0) {
        *numberOfContours = 0;
        *bodyOff = off;
        *bodyLen = 0;
        return false;
    }
    if (len < 10) return false;
    Reader r(impl->data, impl->size);
    if (!r.I16At(off, numberOfContours)) return false;
    *bodyOff = off + 10;
    *bodyLen = len - 10;
    return true;
}

bool LoadGlyfOutlineRec(Font::Impl* impl, u32 glyphIndex, u32 depth, std::vector<std::vector<Vec2f>>* out);

bool LoadSimpleGlyph(Font::Impl* impl, usize bodyOff, usize bodyLen, i16 numberOfContours,
                     std::vector<std::vector<Vec2f>>* out) {
    if (numberOfContours <= 0) return false;
    Reader r(impl->data, impl->size);
    usize p = bodyOff;
    usize endOfBody = bodyOff + bodyLen;
    if (!impl->InRange(bodyOff, bodyLen)) return false;

    std::vector<u16> ends(static_cast<usize>(numberOfContours));
    for (u32 i = 0; i < static_cast<u32>(numberOfContours); ++i) {
        if (p + 2 > endOfBody) return false;
        u16 e = 0;
        if (!r.U16At(p, &e)) return false;
        ends[i] = e;
        p += 2;
    }
    u16 numPoints = static_cast<u16>(ends.back() + 1);
    if (numPoints == 0 || numPoints > 8192) return false;
    // Длина инструкций (пропускается).
    if (p + 2 > endOfBody) return false;
    u16 instrLen = 0;
    if (!r.U16At(p, &instrLen)) return false;
    p += 2;
    if (p + instrLen > endOfBody) return false;
    p += instrLen;

    // Flags.
    std::vector<u8> flags(numPoints);
    for (u32 i = 0; i < numPoints;) {
        if (p >= endOfBody) return false;
        u8 f = 0;
        if (!r.U8At(p++, &f)) return false;
        flags[i++] = f;
        if (f & 0x08) {  // REPEAT
            if (p >= endOfBody) return false;
            u8 rep = 0;
            if (!r.U8At(p++, &rep)) return false;
            for (u32 k = 0; k < rep && i < numPoints; ++k) flags[i++] = f;
        }
    }
    // X deltas.
    std::vector<i32> xs(numPoints, 0), ys(numPoints, 0);
    i32 acc = 0;
    for (u32 i = 0; i < numPoints; ++i) {
        u8 f = flags[i];
        i32 d = 0;
        if (f & 0x02) {  // X_SHORT_VECTOR
            if (p + 1 > endOfBody) return false;
            u8 v = 0;
            if (!r.U8At(p++, &v)) return false;
            d = (f & 0x10) ? static_cast<i32>(v) : -static_cast<i32>(v);
        } else if (!(f & 0x10)) {  // X_IS_SAME
            if (p + 2 > endOfBody) return false;
            i16 v = 0;
            if (!r.I16At(p, &v)) return false;
            p += 2;
            d = v;
        }
        acc += d;
        xs[i] = acc;
    }
    acc = 0;
    for (u32 i = 0; i < numPoints; ++i) {
        u8 f = flags[i];
        i32 d = 0;
        if (f & 0x04) {  // Y_SHORT_VECTOR
            if (p + 1 > endOfBody) return false;
            u8 v = 0;
            if (!r.U8At(p++, &v)) return false;
            d = (f & 0x20) ? static_cast<i32>(v) : -static_cast<i32>(v);
        } else if (!(f & 0x20)) {  // Y_IS_SAME
            if (p + 2 > endOfBody) return false;
            i16 v = 0;
            if (!r.I16At(p, &v)) return false;
            p += 2;
            d = v;
        }
        acc += d;
        ys[i] = acc;
    }

    out->clear();
    out->reserve(static_cast<usize>(numberOfContours));
    u32 start = 0;
    for (u32 c = 0; c < static_cast<u32>(numberOfContours); ++c) {
        u32 end = ends[c];
        if (end < start || end >= numPoints) return false;
        std::vector<Vec2f> contour;
        contour.reserve(static_cast<usize>(end - start) + 1);
        for (u32 i = start; i <= end; ++i) {
            Vec2f v;
            v.x = static_cast<f32>(xs[i]);
            v.y = static_cast<f32>(ys[i]);
            v.on = (flags[i] & 0x01) != 0;
            contour.push_back(v);
        }
        // Отбрасываем повторяющиеся соседние точки (в дикой природе встречаются).
        std::vector<Vec2f> clean;
        clean.reserve(contour.size());
        for (const Vec2f& v : contour) {
            if (!clean.empty() && clean.back().x == v.x && clean.back().y == v.y) continue;
            clean.push_back(v);
        }
        if (clean.size() >= 2) out->push_back(std::move(clean));
        start = end + 1;
    }
    return !out->empty();
}

bool LoadCompositeGlyph(Font::Impl* impl, usize bodyOff, usize bodyLen, u32 depth,
                        std::vector<std::vector<Vec2f>>* out) {
    Reader r(impl->data, impl->size);
    usize p = bodyOff;
    usize end = bodyOff + bodyLen;
    if (!impl->InRange(bodyOff, bodyLen)) return false;
    bool any = false;
    for (int guard = 0; guard < 512; ++guard) {
        if (p + 4 > end) break;
        u16 flags = 0, glyphIndex = 0;
        if (!r.U16At(p, &flags) || !r.U16At(p + 2, &glyphIndex)) break;
        p += 4;
        i32 arg1 = 0, arg2 = 0;
        if (flags & 0x0001) {  // ARG_1_AND_2_ARE_WORDS
            i16 a = 0, b = 0;
            if (p + 4 > end || !r.I16At(p, &a) || !r.I16At(p + 2, &b)) break;
            arg1 = a;
            arg2 = b;
            p += 4;
        } else {
            i8 a = 0, b = 0;
            if (p + 2 > end || !r.I8At(p, &a) || !r.I8At(p + 1, &b)) break;
            arg1 = a;
            arg2 = b;
            p += 2;
        }
        Matrix2 m;
        if (flags & 0x0008) {  // WE_HAVE_A_SCALE
            i16 s = 0;
            if (p + 2 > end || !r.I16At(p, &s)) break;
            f32 sc = static_cast<f32>(s) / 16384.0f;
            m.a = sc;
            m.d = sc;
            p += 2;
        } else if (flags & 0x0040) {  // X_AND_Y_SCALE
            i16 sx = 0, sy = 0;
            if (p + 4 > end || !r.I16At(p, &sx) || !r.I16At(p + 2, &sy)) break;
            m.a = static_cast<f32>(sx) / 16384.0f;
            m.d = static_cast<f32>(sy) / 16384.0f;
            p += 4;
        } else if (flags & 0x0080) {  // TWO_BY_TWO
            i16 a = 0, b = 0, c = 0, d = 0;
            if (p + 8 > end || !r.I16At(p, &a) || !r.I16At(p + 2, &b) || !r.I16At(p + 4, &c) ||
                !r.I16At(p + 6, &d))
                break;
            m.a = static_cast<f32>(a) / 16384.0f;
            m.b = static_cast<f32>(b) / 16384.0f;
            m.c = static_cast<f32>(c) / 16384.0f;
            m.d = static_cast<f32>(d) / 16384.0f;
            p += 8;
        }
        std::vector<std::vector<Vec2f>> comp;
        if (!LoadGlyfOutlineRec(impl, glyphIndex, depth + 1, &comp)) {
            // Отсутствующий компонент: пропускаем его, но продолжаем.
        } else if (flags & 0x0002) {
            f32 dx = static_cast<f32>(arg1);
            f32 dy = static_cast<f32>(arg2);
            if (flags & 0x0800) {  // SCALED_COMPONENT_OFFSET
                f32 ox = 0, oy = 0;
                m.Apply(dx, dy, &ox, &oy);
                dx = ox;
                dy = oy;
            }
            for (auto& contour : comp) {
                for (Vec2f& v : contour) {
                    f32 ox = 0, oy = 0;
                    m.Apply(v.x, v.y, &ox, &oy);
                    v.x = ox + dx;
                    v.y = oy + dy;
                }
                out->push_back(std::move(contour));
            }
            any = true;
        } else {
            // Сопоставление точек (редко): аппроксимируем, считая аргументы
            // нулевыми смещениями и оставляя компонент на месте.
            for (auto& contour : comp) {
                for (Vec2f& v : contour) {
                    f32 ox = 0, oy = 0;
                    m.Apply(v.x, v.y, &ox, &oy);
                    v.x = ox;
                    v.y = oy;
                }
                out->push_back(std::move(contour));
            }
            any = true;
        }
        if (!(flags & 0x0020)) break;  // MORE_COMPONENTS
    }
    return any;
}

bool LoadGlyfOutlineRec(Font::Impl* impl, u32 glyphIndex, u32 depth, std::vector<std::vector<Vec2f>>* out) {
    out->clear();
    if (!impl || depth > kMaxCompositeDepth) return false;
    if (glyphIndex >= impl->numGlyphs) return false;
    i16 numberOfContours = 0;
    usize bodyOff = 0, bodyLen = 0;
    if (!ReadGlyfHeader(impl, glyphIndex, &numberOfContours, &bodyOff, &bodyLen)) return false;
    if (numberOfContours > 0) return LoadSimpleGlyph(impl, bodyOff, bodyLen, numberOfContours, out);
    if (numberOfContours < 0) return LoadCompositeGlyph(impl, bodyOff, bodyLen, depth, out);
    return false;
}

bool LoadGlyfOutline(Font::Impl* impl, u32 glyphIndex, std::vector<std::vector<Vec2f>>* out) {
    return LoadGlyfOutlineRec(impl, glyphIndex, 0, out);
}

u32 GlyfContourCount(Font::Impl* impl, u32 glyphIndex) {
    i16 n = 0;
    usize off = 0, len = 0;
    if (!ReadGlyfHeader(impl, glyphIndex, &n, &off, &len)) return 0;
    if (n > 0) return static_cast<u32>(n);
    if (n < 0) return 1;  // composite: treat as non-empty
    return 0;
}


// 6. CFF / Type2 charstrings
// ===========================================================================
const char* const kCffStandardStrings[] = {
    ".notdef", "space", "exclam", "quotedbl", "numbersign", "dollar", "percent", "ampersand", "quoteright",
    "parenleft", "parenright", "asterisk", "plus", "comma", "hyphen", "period", "slash", "zero", "one", "two",
    "three", "four", "five", "six", "seven", "eight", "nine", "colon", "semicolon", "less", "equal", "greater",
    "question", "at", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R",
    "S", "T", "U", "V", "W", "X", "Y", "Z", "bracketleft", "backslash", "bracketright", "asciicircum",
    "underscore", "quoteleft", "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o", "p",
    "q", "r", "s", "t", "u", "v", "w", "x", "y", "z", "braceleft", "bar", "braceright", "asciitilde",
    "exclamdown", "cent", "sterling", "fraction", "yen", "florin", "section", "currency", "quotesingle",
    "quotedblleft", "guillemotleft", "guilsinglleft", "guilsinglright", "fi", "fl", "endash", "dagger",
    "daggerdbl", "periodcentered", "paragraph", "bullet", "quotesinglbase", "quotedblbase", "quotedblright",
    "guillemotright", "ellipsis", "perthousand", "questiondown", "grave", "acute", "circumflex", "tilde",
    "macron", "breve", "dotaccent", "dieresis", "ring", "cedilla", "hungarumlaut", "ogonek", "caron", "emdash",
    "AE", "ordfeminine", "Lslash", "Oslash", "OE", "ordmasculine", "ae", "dotlessi", "lslash", "oslash", "oe",
    "germandbls", "onesuperior", "logicalnot", "mu", "trademark", "Eth", "onehalf", "plusminus", "Thorn",
    "onequarter", "divide", "brokenbar", "degree", "thorn", "threequarters", "twosuperior", "registered",
    "minus", "eth", "multiply", "threesuperior", "copyright", "Aacute", "Acircumflex", "Adieresis", "Agrave",
    "Aring", "Atilde", "Ccedilla", "Eacute", "Ecircumflex", "Edieresis", "Egrave", "Iacute", "Icircumflex",
    "Idieresis", "Igrave", "Ntilde", "Oacute", "Ocircumflex", "Odieresis", "Ograve", "Otilde", "Scaron",
    "Uacute", "Ucircumflex", "Udieresis", "Ugrave", "Yacute", "Ydieresis", "Zcaron", "aacute", "acircumflex",
    "adieresis", "agrave", "aring", "atilde", "ccedilla", "eacute", "ecircumflex", "edieresis", "egrave",
    "iacute", "icircumflex", "idieresis", "igrave", "ntilde", "oacute", "ocircumflex", "odieresis", "ograve",
    "otilde", "scaron", "uacute", "ucircumflex", "udieresis", "ugrave", "yacute", "ydieresis", "zcaron",
    "exclamsmall", "Hungarumlautsmall", "dollaroldstyle", "dollarsuperior", "ampersandsmall", "Acutesmall",
    "parenleftsuperior", "parenrightsuperior", "twodotenleader", "onedotenleader", "zerooldstyle",
    "oneoldstyle", "twooldstyle", "threeoldstyle", "fouroldstyle", "fiveoldstyle", "sixoldstyle",
    "sevenoldstyle", "eightoldstyle", "nineoldstyle", "commasuperior", "threequartersemdash",
    "periodsuperior", "questionsmall", "asuperior", "bsuperior", "centsuperior", "dsuperior", "esuperior",
    "isuperior", "lsuperior", "msuperior", "nsuperior", "osuperior", "rsuperior", "ssuperior", "tsuperior",
    "ff", "ffi", "ffl", "parenleftinferior", "parenrightinferior", "Circumflexsmall", "hyphensuperior",
    "Gravesmall", "Asmall", "Bsmall", "Csmall", "Dsmall", "Esmall", "Fsmall", "Gsmall", "Hsmall", "Ismall",
    "Jsmall", "Ksmall", "Lsmall", "Msmall", "Nsmall", "Osmall", "Psmall", "Qsmall", "Rsmall", "Ssmall",
    "Tsmall", "Usmall", "Vsmall", "Wsmall", "Xsmall", "Ysmall", "Zsmall", "colonmonetary", "onefitted",
    "rupiah", "Tildesmall", "exclamdownsmall", "centoldstyle", "Lslashsmall", "Scaronsmall", "Zcaronsmall",
    "Dieresissmall", "Brevesmall", "Caronsmall", "Dotaccentsmall", "Macronsmall", "figuredash",
    "hypheninferior", "Ogoneksmall", "Ringsmall", "Cedillasmall", "questiondownsmall", "oneeighth",
    "threeeighths", "fiveeighths", "seveneighths", "onethird", "twothirds", "zerosuperior", "foursuperior",
    "fivesuperior", "sixsuperior", "sevensuperior", "eightsuperior", "ninesuperior", "zeroinferior",
    "oneinferior", "twoinferior", "threeinferior", "fourinferior", "fiveinferior", "sixinferior",
    "seveninferior", "eightinferior", "nineinferior", "centinferior", "dollarinferior", "periodinferior",
    "commainferior", "Agravesmall", "Aacutesmall", "Acircumflexsmall", "Atildesmall", "Adieresissmall",
    "Aringsmall", "AEsmall", "Ccedillasmall", "Egravesmall", "Eacutesmall", "Ecircumflexsmall",
    "Edieresissmall", "Igravesmall", "Iacutesmall", "Icircumflexsmall", "Idieresissmall", "Ethsmall",
    "Ntildesmall", "Ogravesmall", "Oacutesmall", "Ocircumflexsmall", "Otildesmall", "Odieresissmall",
    "OEsmall", "Oslashsmall", "Ugravesmall", "Uacutesmall", "Ucircumflexsmall", "Udieresissmall",
    "Yacutesmall", "Thornsmall", "Ydieresissmall", "001.000", "001.001", "Black", "Bold", "Book", "Light",
    "Medium", "Regular", "Roman", "Semibold"};
constexpr int kNumStandardStrings = static_cast<int>(sizeof(kCffStandardStrings) / sizeof(kCffStandardStrings[0]));

f64 ParseRealString(const std::string& s) {
    // strtod вместо std::stod: при некорректном вводе он возвращает 0,
    // а не бросает исключение, поэтому код шрифта собирается с -fno-exceptions
    // (сборка WASM отключает исключения ради размера).
    return std::strtod(s.c_str(), nullptr);
}

struct DictEntry {
    std::vector<f64> operands;
    u32 op = 0;  // один байт, либо 0x0C00 | экранированный байт
};

bool ParseCffNumbers(Reader& r, usize end, std::vector<DictEntry>* out) {
    constexpr int kMaxOperands = 48;
    std::vector<DictEntry> entries;
    std::vector<f64> operands;
    operands.reserve(8);
    while (r.Pos() < end) {
        u8 b = 0;
        if (!r.U8(&b)) break;
        if (b <= 21) {
            u32 op = b;
            if (b == 12) {
                u8 b1 = 0;
                if (!r.U8(&b1)) break;
                op = 0x0C00u | b1;
            }
            DictEntry e;
            e.op = op;
            e.operands = operands;
            entries.push_back(std::move(e));
            operands.clear();
        } else if (b == 28) {
            i16 v = 0;
            if (!r.I16(&v)) break;
            if (static_cast<int>(operands.size()) < kMaxOperands) operands.push_back(static_cast<f64>(v));
        } else if (b == 29) {
            u32 v = 0;
            if (!r.U32(&v)) break;
            if (static_cast<int>(operands.size()) < kMaxOperands)
                operands.push_back(static_cast<f64>(static_cast<i32>(v)));
        } else if (b == 30) {
            std::string s;
            bool done = false;
            while (!done && s.size() < 64) {
                u8 byte = 0;
                if (!r.U8(&byte)) break;
                for (int half = 0; half < 2 && !done; ++half) {
                    u8 nib = half == 0 ? static_cast<u8>(byte >> 4) : static_cast<u8>(byte & 0x0F);
                    switch (nib) {
                        case 0x0: s.push_back('0'); break;
                        case 0x1: s.push_back('1'); break;
                        case 0x2: s.push_back('2'); break;
                        case 0x3: s.push_back('3'); break;
                        case 0x4: s.push_back('4'); break;
                        case 0x5: s.push_back('5'); break;
                        case 0x6: s.push_back('6'); break;
                        case 0x7: s.push_back('7'); break;
                        case 0x8: s.push_back('8'); break;
                        case 0x9: s.push_back('9'); break;
                        case 0xA: s.push_back('.'); break;
                        case 0xB: s.push_back('E'); break;
                        case 0xC: s += "E-"; break;
                        case 0xE: s.push_back('-'); break;
                        case 0xF: done = true; break;
                        default: break;  // 0xD reserved
                    }
                }
            }
            if (static_cast<int>(operands.size()) < kMaxOperands) operands.push_back(ParseRealString(s));
        } else if (b == 31) {
            if (static_cast<int>(operands.size()) < kMaxOperands) operands.push_back(0.0);
        } else if (b >= 32 && b <= 246) {
            if (static_cast<int>(operands.size()) < kMaxOperands) operands.push_back(static_cast<f64>(b) - 139.0);
        } else if (b >= 247 && b <= 250) {
            u8 b1 = 0;
            if (!r.U8(&b1)) break;
            if (static_cast<int>(operands.size()) < kMaxOperands)
                operands.push_back((static_cast<f64>(b) - 247.0) * 256.0 + static_cast<f64>(b1) + 108.0);
        } else if (b >= 251 && b <= 254) {
            u8 b1 = 0;
            if (!r.U8(&b1)) break;
            if (static_cast<int>(operands.size()) < kMaxOperands)
                operands.push_back(-((static_cast<f64>(b) - 251.0) * 256.0) - static_cast<f64>(b1) - 108.0);
        }
    }
    *out = std::move(entries);
    return !out->empty();
}

// CFF INDEX с материализованными абсолютными смещениями.
struct CffIndex {
    std::vector<usize> offsets;  // count + 1 абсолютных смещений
    usize end = 0;

    bool Parse(const u8* base, usize size, usize off) {
        Reader r(base, size);
        u16 count = 0;
        offsets.clear();
        if (!r.U16At(off, &count)) return false;
        if (count == 0) {
            offsets.push_back(off + 2);
            end = off + 2;
            return true;
        }
        u8 offSize = 0;
        if (!r.U8At(off + 2, &offSize)) return false;
        if (offSize < 1 || offSize > 4) return false;
        usize offsetsBase = off + 3;
        usize dataBase = offsetsBase + (static_cast<usize>(count) + 1) * offSize;
        if (!r.InRange(offsetsBase, (static_cast<usize>(count) + 1) * offSize)) return false;
        if (dataBase > size) return false;
        for (u32 i = 0; i <= static_cast<u32>(count); ++i) {
            usize p = offsetsBase + static_cast<usize>(i) * offSize;
            u32 val = 0;
            for (u32 k = 0; k < offSize; ++k) {
                u8 byte = 0;
                if (!r.U8At(p + k, &byte)) return false;
                val = (val << 8) | byte;
            }
            // Смещения CFF INDEX 1-базные: смещение 1 указывает на первый байт
            // области данных.
            usize abs = dataBase + (val > 0 ? static_cast<usize>(val) - 1 : 0);
            if (abs > size) return false;
            if (!offsets.empty() && abs < offsets.back()) return false;
            offsets.push_back(abs);
        }
        end = offsets.back();
        return true;
    }

    [[nodiscard]] u32 Count() const { return offsets.empty() ? 0u : static_cast<u32>(offsets.size() - 1); }
    bool Object(usize size, u32 index, usize* off, usize* len) const {
        if (index + 1 >= offsets.size()) return false;
        usize s = offsets[index], e = offsets[index + 1];
        if (e < s || e > size) return false;
        *off = s;
        *len = e - s;
        return true;
    }
};

std::string CffSidToString(const u8* base, usize size, const CffIndex& strings, u32 sid) {
    if (sid < static_cast<u32>(kNumStandardStrings)) return kCffStandardStrings[sid];
    u32 idx = sid - static_cast<u32>(kNumStandardStrings);
    usize off = 0, len = 0;
    if (!strings.Object(size, idx, &off, &len)) return std::string();
    return std::string(reinterpret_cast<const char*>(base + off), len);
}

// Разворачивает charset в список SID по глифам (0 == .notdef).
bool ParseCffCharset(const u8* base, usize size, usize off, u32 nGlyphs, std::vector<u16>* sids) {
    sids->assign(nGlyphs, 0);
    if (off == 0) {
        // ISOAdobe: id глифа == SID для первых 229 глифов.
        for (u32 g = 1; g < nGlyphs && g <= 228; ++g) (*sids)[g] = static_cast<u16>(g);
        return true;
    }
    if (nGlyphs <= 1) return true;
    Reader r(base, size);
    u8 format = 0;
    if (!r.U8At(off, &format)) return false;
    u32 gid = 1;
    if (format == 0) {
        while (gid < nGlyphs) {
            u16 sid = 0;
            if (!r.U16At(off + 1 + static_cast<usize>(gid - 1) * 2, &sid)) return false;
            (*sids)[gid++] = sid;
        }
    } else if (format == 1 || format == 2) {
        usize p = off + 1;
        while (gid < nGlyphs) {
            u16 first = 0, nLeft = 0;
            if (format == 1) {
                u8 f = 0, n = 0;
                if (!r.U8At(p, &f) || !r.U8At(p + 1, &n)) return false;
                first = f;
                nLeft = n;
                p += 2;
            } else {
                if (!r.U16At(p, &first) || !r.U16At(p + 2, &nLeft)) return false;
                p += 4;
            }
            u32 n = static_cast<u32>(nLeft) + 1;
            if (n > nGlyphs - gid) n = nGlyphs - gid;
            for (u32 k = 0; k < n; ++k) {
                if (format == 1) {
                    (*sids)[gid++] = static_cast<u16>(first + k);
                } else {
                    u16 sid = 0;
                    if (!r.U16At(p + static_cast<usize>(k) * 2, &sid)) return false;
                    (*sids)[gid++] = sid;
                }
            }
        }
    } else {
        // Формат 3 (Expert) и всё прочее: имена оставляем пустыми.
        return format == 3;
    }
    return true;
}

struct CffProgram {
    CffIndex charStrings;
    CffIndex globalSubrs;
    CffIndex localSubrs;
    std::vector<std::pair<f64, f32>> privateDict;
    f32 defaultWidthX = 0.0f;
    f32 nominalWidthX = 0.0f;
    bool valid = false;

    [[nodiscard]] usize GlobalSubrCount() const { return globalSubrs.Count(); }
    [[nodiscard]] usize LocalSubrCount() const { return localSubrs.Count(); }
};

void ParseCffPrivateDict(const u8* base, usize size, usize off, usize len, CffProgram* prog, bool withSubrs) {
    if (len == 0 || off + len > size) return;
    Reader r(base, size, off);
    std::vector<DictEntry> entries;
    ParseCffNumbers(r, off + len, &entries);
    for (const DictEntry& e : entries) {
        if (e.operands.empty()) continue;
        if (e.op == 20) {
            prog->defaultWidthX = static_cast<f32>(e.operands[0]);
        } else if (e.op == 21) {
            prog->nominalWidthX = static_cast<f32>(e.operands[0]);
        } else if (e.op == 19 && withSubrs) {
            // `Subrs` — смещение *относительно Private DICT*, и локальный
            // INDEX подрограмм обычно следует за DICT, а не живёт внутри него,
            // поэтому он может законно находиться на (или за) конце DICT.  Здесь
            // важны только границы таблицы CFF.
            i64 rel = static_cast<i64>(e.operands[0]);
            if (rel > 0 && off + static_cast<usize>(rel) < size) {
                CffIndex sub;
                if (sub.Parse(base, size, off + static_cast<usize>(rel))) prog->localSubrs = std::move(sub);
            } else if (rel > 0) {
                ENG_LOGW("font", "CFF: local Subrs offset %lld is out of bounds", static_cast<long long>(rel));
            }
        }
    }
    prog->privateDict.clear();
    for (const DictEntry& e : entries) {
        prog->privateDict.emplace_back(e.operands.empty() ? 0.0 : e.operands[0], static_cast<f32>(e.op));
    }
}

// --- интерпретатор charstring Type2 -----------------------------------------
struct T2Interp {
    const u8* base = nullptr;
    usize size = 0;
    const CffProgram* prog = nullptr;
    f32 hintScale = 1.0f;

    f32 x = 0, y = 0;
    f32 minX = 0, maxX = 0;
    i32 hintCount = 0;
    int widthState = 0;  // 0 = не встречалась, 1 = встречалась (без ширины), 2 = ширина потреблена
    f32 width = 0;
    std::vector<Vec2f> current;
    bool haveCurrent = false;
    int depth = 0;
    bool aborted = false;

    std::vector<std::vector<Vec2f>>* out = nullptr;

    void NoteX(f32 v) {
        if (v < minX) minX = v;
        if (v > maxX) maxX = v;
    }
    void MoveTo(f32 nx, f32 ny) {
        Flush();
        x = nx;
        y = ny;
        Vec2f v;
        v.x = x;
        v.y = y;
        v.on = true;
        current.clear();
        current.push_back(v);
        haveCurrent = true;
        NoteX(x);
    }
    void LineTo(f32 nx, f32 ny) {
        if (!haveCurrent) {
            MoveTo(nx, ny);
            return;
        }
        x = nx;
        y = ny;
        Vec2f v;
        v.x = x;
        v.y = y;
        v.on = true;
        current.push_back(v);
        NoteX(x);
    }
    void CurveTo(f32 c1x, f32 c1y, f32 c2x, f32 c2y, f32 ex, f32 ey, bool cubic) {
        if (!haveCurrent) MoveTo(c1x, c1y);
        Vec2f c1;
        c1.x = c1x;
        c1.y = c1y;
        c1.on = false;
        current.push_back(c1);
        if (cubic) {
            Vec2f c2;
            c2.x = c2x;
            c2.y = c2y;
            c2.on = false;
            current.push_back(c2);
        }
        Vec2f e;
        e.x = ex;
        e.y = ey;
        e.on = true;
        current.push_back(e);
        x = ex;
        y = ey;
        NoteX(c1x);
        NoteX(c2x);
        NoteX(ex);
    }
    void Flush() {
        if (haveCurrent && current.size() >= 2) {
            if (out) out->push_back(current);
        }
        current.clear();
        haveCurrent = false;
    }

    bool Run(const u8* cs, usize len) {
        if (aborted) return false;
        if (++depth > 10) {
            --depth;
            aborted = true;
            return false;
        }
        Reader cr(cs, len);
        f64 st[48];
        int sp = 0;
        auto push = [&](f64 v) {
            if (sp < 48) st[sp++] = v;
        };
        auto arg = [&](int i) -> f32 {
            if (sp == 0) return 0.0f;
            int idx = i < 0 ? sp + i : i;
            if (idx < 0 || idx >= sp) return 0.0f;
            return static_cast<f32>(st[idx]);
        };
        while (cr.Pos() < len) {
            u8 b = 0;
            if (!cr.U8(&b)) break;
            if (b == 28 || b >= 32) {
                if (b == 28) {
                    i16 v = 0;
                    if (!cr.I16(&v)) break;
                    push(static_cast<f64>(v));
                } else if (b < 247) {
                    push(static_cast<f64>(b) - 139.0);
                } else if (b < 251) {
                    u8 b1 = 0;
                    if (!cr.U8(&b1)) break;
                    push((static_cast<f64>(b) - 247.0) * 256.0 + static_cast<f64>(b1) + 108.0);
                } else if (b < 255) {
                    u8 b1 = 0;
                    if (!cr.U8(&b1)) break;
                    push(-((static_cast<f64>(b) - 251.0) * 256.0) - static_cast<f64>(b1) - 108.0);
                } else {
                    u32 v = 0;
                    if (!cr.U32(&v)) break;
                    push(static_cast<f64>(static_cast<i32>(v)) / 65536.0);
                }
                continue;
            }
            // Опциональная ведущая ширина у первого оператора, очищающего стек.
            if (widthState == 0) {
                widthState = 1;
                bool took = false;
                if (b == 1 || b == 3 || b == 18 || b == 23 || b == 19 || b == 20) {
                    // Стем-хинты идут парами; нечётное количество означает ширину.
                    took = (sp % 2) == 1;
                } else if (b == 21) {
                    // rmoveto принимает (dx dy); три аргумента означают (width dx dy).
                    took = sp >= 3 && (sp % 2) == 1;
                } else if (b == 22 || b == 4) {
                    // hmoveto/vmoveto принимают одну дельту; два аргумента означают ширину.
                    took = sp >= 2;
                } else if (b == 14) {
                    // endchar бывает «голым», только с шириной, seac (4) или width+seac (5).
                    took = sp == 1 || sp >= 5;
                }
                if (took) {
                    width = prog ? prog->nominalWidthX + static_cast<f32>(st[0]) : static_cast<f32>(st[0]);
                    for (int i = 1; i < sp; ++i) st[i - 1] = st[i];
                    --sp;
                    widthState = 2;
                }
            }

            switch (b) {
                case 1:   // hstem
                case 3:   // vstem
                case 18:  // hstemhm
                case 23:  // vstemhm
                    hintCount += sp / 2;
                    sp = 0;
                    break;
                case 19:  // hintmask
                case 20: {  // cntrmask
                    hintCount += sp / 2;
                    sp = 0;
                    u32 nBytes = static_cast<u32>((hintCount + 7) / 8);
                    if (nBytes > 0) {
                        if (cr.Remaining() < nBytes) {
                            aborted = true;
                            break;
                        }
                        cr.Skip(static_cast<i64>(nBytes));
                    }
                    break;
                }
                case 21:  // rmoveto
                    if (sp >= 2) MoveTo(x + arg(0), y + arg(1));
                    else if (sp == 1) MoveTo(x + arg(0), y);
                    sp = 0;
                    break;
                case 22:  // hmoveto
                    if (sp >= 1) MoveTo(x + arg(0), y);
                    sp = 0;
                    break;
                case 4:  // vmoveto
                    if (sp >= 1) MoveTo(x, y + arg(0));
                    sp = 0;
                    break;
                case 5:  // rlineto
                    for (int i = 0; i + 1 < sp; i += 2) LineTo(x + arg(i), y + arg(i + 1));
                    sp = 0;
                    break;
                case 6:  // hlineto
                    for (int i = 0; i < sp; ++i) {
                        if ((i % 2) == 0) LineTo(x + arg(i), y);
                        else LineTo(x, y + arg(i));
                    }
                    sp = 0;
                    break;
                case 7:  // vlineto
                    for (int i = 0; i < sp; ++i) {
                        if ((i % 2) == 0) LineTo(x, y + arg(i));
                        else LineTo(x + arg(i), y);
                    }
                    sp = 0;
                    break;
                case 8:  // rrcurveto
                    for (int i = 0; i + 5 < sp; i += 6) {
                        f32 c1x = x + arg(i), c1y = y + arg(i + 1);
                        f32 c2x = c1x + arg(i + 2), c2y = c1y + arg(i + 3);
                        CurveTo(c1x, c1y, c2x, c2y, c2x + arg(i + 4), c2y + arg(i + 5), true);
                    }
                    sp = 0;
                    break;
                case 24: {  // rcurveline
                    int i = 0;
                    for (; i + 5 < sp - 2; i += 6) {
                        f32 c1x = x + arg(i), c1y = y + arg(i + 1);
                        f32 c2x = c1x + arg(i + 2), c2y = c1y + arg(i + 3);
                        CurveTo(c1x, c1y, c2x, c2y, c2x + arg(i + 4), c2y + arg(i + 5), true);
                    }
                    if (i + 1 < sp) LineTo(x + arg(i), y + arg(i + 1));
                    sp = 0;
                    break;
                }
                case 25: {  // rlinecurve
                    int i = 0;
                    for (; i + 1 < sp - 6; i += 2) LineTo(x + arg(i), y + arg(i + 1));
                    if (i + 5 < sp) {
                        f32 c1x = x + arg(i), c1y = y + arg(i + 1);
                        f32 c2x = c1x + arg(i + 2), c2y = c1y + arg(i + 3);
                        CurveTo(c1x, c1y, c2x, c2y, c2x + arg(i + 4), c2y + arg(i + 5), true);
                    }
                    sp = 0;
                    break;
                }
                case 26:  // vvcurveto
                case 27: {  // hhcurveto
                    bool horizontal = (b == 27);
                    int i = 0;
                    f32 d1 = 0;
                    if ((sp % 4) == 1) {
                        d1 = arg(0);
                        i = 1;
                    }
                    for (; i + 3 < sp; i += 4) {
                        f32 c1x, c1y, c2x, c2y, ex, ey;
                        if (horizontal) {
                            c1x = x + d1;
                            c1y = y + arg(i);
                            c2x = c1x + arg(i + 1);
                            c2y = c1y + arg(i + 2);
                            ex = c2x + arg(i + 3);
                            ey = c2y;
                        } else {
                            c1x = x + arg(i);
                            c1y = y + d1;
                            c2x = c1x + arg(i + 1);
                            c2y = c1y + arg(i + 2);
                            ex = c2x;
                            ey = c2y + arg(i + 3);
                        }
                        CurveTo(c1x, c1y, c2x, c2y, ex, ey, true);
                        d1 = 0;
                    }
                    sp = 0;
                    break;
                }
                case 30:  // vhcurveto
                case 31: {  // hvcurveto
                    bool horizontal = (b == 31);
                    int i = 0;
                    int remaining = sp;
                    while (remaining >= 4) {
                        f32 c1x, c1y, c2x, c2y, ex, ey;
                        if (horizontal) {
                            c1x = x + arg(i);
                            c1y = y;
                            c2x = c1x + arg(i + 1);
                            c2y = c1y + arg(i + 2);
                            if (remaining == 5) {
                                ex = c2x;
                                ey = c2y + arg(i + 4);
                            } else {
                                ex = c2x + arg(i + 3);
                                ey = c2y;
                            }
                        } else {
                            c1x = x;
                            c1y = y + arg(i);
                            c2x = c1x + arg(i + 1);
                            c2y = c1y + arg(i + 2);
                            if (remaining == 5) {
                                ex = c2x + arg(i + 4);
                                ey = c2y;
                            } else {
                                ex = c2x;
                                ey = c2y + arg(i + 3);
                            }
                        }
                        CurveTo(c1x, c1y, c2x, c2y, ex, ey, true);
                        i += 4;
                        remaining -= 4;
                        horizontal = !horizontal;
                    }
                    sp = 0;
                    break;
                }
                case 10: {  // callsubr
                    i32 idx = sp >= 1 ? static_cast<i32>(arg(sp - 1)) : 0;
                    if (sp >= 1) --sp;
                    if (!CallSubr(idx, false)) aborted = true;
                    break;
                }
                case 29: {  // callgsubr
                    i32 idx = sp >= 1 ? static_cast<i32>(arg(sp - 1)) : 0;
                    if (sp >= 1) --sp;
                    if (!CallSubr(idx, true)) aborted = true;
                    break;
                }
                case 11:  // return
                    --depth;
                    return true;
                case 14:  // endchar
                    if (sp >= 4) Seac(arg(sp - 4), arg(sp - 3), arg(sp - 2), arg(sp - 1));
                    sp = 0;
                    Flush();
                    --depth;
                    return true;
                case 12: {  // escaped
                    u8 b1 = 0;
                    if (!cr.U8(&b1)) {
                        aborted = true;
                        break;
                    }
                    switch (b1) {
                        case 35:  // flex
                            if (sp >= 13) {
                                CurveTo(x + arg(0), y + arg(1), x + arg(0) + arg(2), y + arg(1) + arg(3),
                                        x + arg(0) + arg(2) + arg(4), y + arg(1) + arg(3) + arg(5), true);
                                CurveTo(x + arg(6), y + arg(7), x + arg(6) + arg(8), y + arg(7) + arg(9),
                                        x + arg(6) + arg(8) + arg(10), y + arg(7) + arg(9) + arg(11), true);
                            }
                            sp = 0;
                            break;
                        case 34:  // hflex
                            if (sp >= 7) {
                                f32 y0 = y;
                                CurveTo(x + arg(0), y0, x + arg(0) + arg(1), y0 + arg(2),
                                        x + arg(0) + arg(1) + arg(3), y0 + arg(2), true);
                                f32 x5 = x + arg(4);
                                CurveTo(x5, y, x5 + arg(5), y, x5 + arg(5) + arg(6), y0, true);
                            }
                            sp = 0;
                            break;
                        case 36:  // hflex1: dx1 dy1 dx2 dy2 dx3 dx4 dx5 dy5 dx6
                            if (sp >= 9) {
                                const f32 y0 = y;  // вторая кривая возвращается к этому y
                                // Первая кривая: c1, c2, end1.
                                CurveTo(x + arg(0), y0 + arg(1), x + arg(0) + arg(2), y0 + arg(1) + arg(3),
                                        x + arg(0) + arg(2) + arg(4), y0 + arg(1) + arg(3), true);
                                // Вторая кривая: c3 = end1 + (dx4, 0), c4 = c3 + (dx5, dy5),
                                // end2 = (c4.x + dx6, y0).
                                const f32 c3x = x + arg(5);
                                const f32 c3y = y;
                                CurveTo(c3x, c3y, c3x + arg(6), c3y + arg(7), c3x + arg(6) + arg(8), y0, true);
                            }
                            sp = 0;
                            break;
                        case 37:  // flex1
                            if (sp >= 11) {
                                f32 dx = arg(0) + arg(2) + arg(4) + arg(6) + arg(8);
                                f32 dy = arg(1) + arg(3) + arg(5) + arg(7) + arg(9);
                                bool lastHorizontal = std::fabs(dx) > std::fabs(dy);
                                CurveTo(x + arg(0), y + arg(1), x + arg(0) + arg(2), y + arg(1) + arg(3),
                                        x + arg(0) + arg(2) + arg(4), y + arg(1) + arg(3) + arg(5), true);
                                f32 c1x = x + arg(6), c1y = y + arg(7);
                                f32 c2x = c1x + arg(8), c2y = c1y + arg(9);
                                f32 ex = lastHorizontal ? c2x : c2x + arg(10);
                                f32 ey = lastHorizontal ? c2y + arg(10) : c2y;
                                CurveTo(c1x, c1y, c2x, c2y, ex, ey, true);
                            }
                            sp = 0;
                            break;
                        default:
                            sp = 0;
                            break;
                    }
                    break;
                }
                default:
                    sp = 0;
                    break;
            }
            if (aborted) break;
        }
        --depth;
        Flush();
        return true;
    }

    bool CallSubr(i32 idx, bool global) {
        if (!prog) return false;
        const CffIndex& ix = global ? prog->globalSubrs : prog->localSubrs;
        u32 count = ix.Count();
        if (count == 0) return false;
        i32 bias = count < 1240 ? 107 : (count < 33900 ? 1131 : 32768);
        i64 real = static_cast<i64>(idx) + bias;
        if (real < 0 || real >= static_cast<i64>(count)) return false;
        usize s = 0, e = 0;
        if (!ix.Object(size, static_cast<u32>(real), &s, &e)) return false;
        return Run(base + s, e - s);
    }

    // Устаревшая композиция акцентов `seac`: `endchar` с четырьмя аргументами
    // (adx, ady, bchar, achar), где оба «символа» — коды *стандартной
    // кодировки*, разрешаемые через таблицу имён глифов.
    void Seac(f32 adx, f32 ady, f32 bchar, f32 achar) {
        if (!nameToGid || !glyphNames || !out || !prog) return;
        if (bchar < 0 || achar < 0) return;
        u32 b = static_cast<u32>(bchar), a = static_cast<u32>(achar);
        // Эти два кода — коды символов стандартной кодировки Adobe, каковыми
        // в CFF-практике являются SID глифов.  Сначала разрешаем SID -> id глифа
        // с откатом к трактовке значения как «голого» id глифа.
        auto bySid = [&](u32 sid) -> i64 {
            if (sids) {
                for (usize i = 1; i < sids->size(); ++i)
                    if ((*sids)[i] == sid) return static_cast<i64>(i);
            }
            return -1;
        };
        auto find = [&](const std::string& n) -> i64 {
            auto it = nameToGid->find(n);
            return it == nameToGid->end() ? -1 : static_cast<i64>(it->second);
        };
        i64 baseGid = bySid(b);
        i64 accentGid = bySid(a);
        if (baseGid < 0 && b < glyphNames->size()) baseGid = find((*glyphNames)[b]);
        if (accentGid < 0 && a < glyphNames->size()) accentGid = find((*glyphNames)[a]);
        if (baseGid < 0 || accentGid < 0) return;
        // Текущий charstring обычно ничего не добавил.
        Flush();
        std::vector<std::vector<Vec2f>> baseC, accentC;
        f32 baseMinX = RenderGidInto(static_cast<u32>(baseGid), &baseC);
        f32 accentMinX = RenderGidInto(static_cast<u32>(accentGid), &accentC);
        f32 dx = adx - (baseMinX - accentMinX);
        for (auto& c : accentC) {
            for (Vec2f& v : c) {
                v.x += dx;
                v.y += ady;
            }
        }
        for (auto& c : baseC) out->push_back(std::move(c));
        for (auto& c : accentC) out->push_back(std::move(c));
    }

    // Рендерит `gid` в `dest` со свежим состоянием; возвращает min x результата.
    f32 RenderGidInto(u32 gid, std::vector<std::vector<Vec2f>>* dest) {
        if (!prog || !base) return 0.0f;
        usize off = 0, len = 0;
        if (!prog->charStrings.Object(size, gid, &off, &len)) return 0.0f;
        T2Interp sub;
        sub.base = base;
        sub.size = size;
        sub.prog = prog;
        sub.out = dest;
        sub.nameToGid = nameToGid;
        sub.glyphNames = glyphNames;
        sub.sids = sids;
        sub.Run(base + off, len);
        f32 mn = 0.0f;
        bool first = true;
        for (const auto& c : *dest) {
            for (const Vec2f& v : c) {
                if (first || v.x < mn) {
                    mn = v.x;
                    first = false;
                }
            }
        }
        return mn;
    }

    const std::unordered_map<std::string, u32>* nameToGid = nullptr;
    const std::vector<std::string>* glyphNames = nullptr;
    const std::vector<u16>* sids = nullptr;
};


// --- CFF -------------------------------------------------------------------
struct CffFdSelect {
    std::vector<u8> perGlyph;
    u8 Default(u32 gid) const { return gid < perGlyph.size() ? perGlyph[gid] : 0; }
};

struct CffFont {
    CffProgram program;                       // не-CID: единственная программа
    std::vector<CffProgram> fdPrograms;       // CID: по одной на запись FDArray
    CffFdSelect fdSelect;
    std::vector<u16> sids;                    // SID на глиф
    std::unordered_map<std::string, u32> nameToGid;
    std::vector<std::string> glyphNames;
    bool cid = false;
    bool valid = false;
};

bool ParseCffTopDict(const u8* base, usize size, const CffIndex& topDict, const CffIndex& strings,
                     CffFont* font, u32* charStringsOffset, f32* fontMatrix, usize* charsetOff,
                     std::vector<std::pair<f64, u32>>* privateLoc, std::vector<std::pair<f64, u32>>* fdArrayLoc,
                     std::vector<std::pair<f64, u32>>* fdSelectLoc, bool* isCid) {
    if (topDict.Count() == 0) return false;
    usize off = 0, len = 0;
    if (!topDict.Object(size, 0, &off, &len)) return false;
    Reader r(base, size, off);
    std::vector<DictEntry> entries;
    if (!ParseCffNumbers(r, off + len, &entries)) return false;
    for (const DictEntry& e : entries) {
        switch (e.op) {
            case 15:  // charset
                if (!e.operands.empty()) *charsetOff = static_cast<usize>(e.operands[0]);
                break;
            case 17:  // CharStrings
                if (!e.operands.empty()) *charStringsOffset = static_cast<u32>(e.operands[0]);
                break;
            case 18:  // Private: операнды — [size, offset]
                if (e.operands.size() >= 2) {
                    privateLoc->clear();
                    privateLoc->emplace_back(e.operands[0], 18);  // size
                    privateLoc->emplace_back(e.operands[1], 0);   // offset (CFF-relative)
                }
                break;
            case 0x0C06:  // ROS => CID-keyed
                *isCid = true;
                break;
            case 0x0C1E:  // FDArray
                if (!e.operands.empty()) fdArrayLoc->emplace_back(e.operands[0], 0);
                break;
            case 0x0C1F:  // FDSelect
                if (!e.operands.empty()) fdSelectLoc->emplace_back(e.operands[0], 0);
                break;
            case 0x0C07:  // FontMatrix
                if (e.operands.size() >= 6) {
                    for (int i = 0; i < 6; ++i) fontMatrix[i] = static_cast<f32>(e.operands[static_cast<usize>(i)]);
                }
                break;
            default:
                break;
        }
    }
    (void)strings;
    (void)font;
    return true;
}

bool LoadCff(Font::Impl* impl, FontFormat* formatOut, CffFont* out) {
    const TableRec* t = impl->Table(kTagCff);
    if (!t) return false;
    const u8* base = impl->data;
    const usize size = impl->size;
    Reader r(base, size, t->offset);
    u8 major = 0, minor = 0, headerSize = 0, offSize = 0;
    if (!r.U8(&major) || !r.U8(&minor) || !r.U8(&headerSize) || !r.U8(&offSize)) return false;
    (void)minor;
    (void)offSize;
    if (major != 1 || headerSize < 4) {
        if (major == 2) ENG_LOGW("font", "CFF2 is not supported");
        return false;
    }
    CffIndex nameIndex, topDictIndex, stringIndex, globalSubrs;
    if (!nameIndex.Parse(base, size, t->offset + headerSize)) return false;
    if (!topDictIndex.Parse(base, size, nameIndex.end)) return false;
    if (!stringIndex.Parse(base, size, topDictIndex.end)) return false;
    if (!globalSubrs.Parse(base, size, stringIndex.end)) return false;

    u32 charStringsOffset = 0;
    usize charsetOff = 0;
    f32 fontMatrix[6] = {0.001f, 0, 0, 0.001f, 0, 0};
    std::vector<std::pair<f64, u32>> privateLoc, fdArrayLoc, fdSelectLoc;
    bool isCid = false;
    if (!ParseCffTopDict(base, size, topDictIndex, stringIndex, out, &charStringsOffset, fontMatrix, &charsetOff,
                         &privateLoc, &fdArrayLoc, &fdSelectLoc, &isCid))
        return false;
    if (charStringsOffset == 0) {
        ENG_LOGE("font", "CFF: no CharStrings offset");
        return false;
    }
    // Все смещения внутри таблицы CFF отсчитываются от таблицы, а не от файла.
    const usize cffBase = t->offset;
    CffIndex charStrings;
    if (!charStrings.Parse(base, size, cffBase + static_cast<usize>(charStringsOffset))) {
        ENG_LOGE("font", "CFF: bad CharStrings INDEX");
        return false;
    }
    u32 nGlyphs = charStrings.Count();
    if (nGlyphs == 0 || nGlyphs > 65535) return false;
    impl->numGlyphs = nGlyphs;
    impl->isCff = true;
    impl->isCid = isCid;
    out->cid = isCid;
    out->program.charStrings = charStrings;
    out->program.globalSubrs = globalSubrs;
    out->program.valid = true;
    out->valid = true;

    // Charset -> имена глифов.
    if (charsetOff != 0) charsetOff += cffBase;
    if (!ParseCffCharset(base, size, charsetOff, nGlyphs, &out->sids)) {
        ENG_LOGW("font", "CFF: unable to read charset, glyph names unavailable");
        out->sids.assign(nGlyphs, 0);
    }
    out->glyphNames.resize(nGlyphs);
    for (u32 g = 0; g < nGlyphs; ++g) {
        out->glyphNames[g] = CffSidToString(base, size, stringIndex, out->sids[g]);
        if (out->glyphNames[g].empty()) out->glyphNames[g] = "glyph" + std::to_string(g);
        if (g > 0) out->nameToGid.emplace(out->glyphNames[g], g);
    }
    if (!isCid) {
        // Пара Private DICT (size, offset).
        if (privateLoc.size() >= 2) {
            const usize privSize = static_cast<usize>(privateLoc[0].first);  // [size, offset]
            const usize privOff = cffBase + static_cast<usize>(privateLoc[1].first);
            ParseCffPrivateDict(base, size, privOff, privSize, &out->program, true);
            ENG_LOGD("font", "CFF: private DICT %zu bytes at %zu, %u local subrs", privSize, privOff,
                     out->program.localSubrs.Count());
        }
    } else {
        // CID: FDArray + FDSelect.
        if (fdArrayLoc.empty()) {
            ENG_LOGW("font", "CFF: CID font without FDArray");
        } else {
            usize fdOff = cffBase + static_cast<usize>(fdArrayLoc[0].first);
            CffIndex fdArray;
            if (fdArray.Parse(base, size, fdOff)) {
                out->fdPrograms.resize(fdArray.Count());
                for (u32 i = 0; i < fdArray.Count(); ++i) {
                    out->fdPrograms[i].charStrings = charStrings;
                    out->fdPrograms[i].globalSubrs = globalSubrs;
                    out->fdPrograms[i].valid = true;
                    usize fo = 0, fl = 0;
                    if (!fdArray.Object(size, i, &fo, &fl)) continue;
                    Reader fr(base, size, fo);
                    std::vector<DictEntry> fentries;
                    if (!ParseCffNumbers(fr, fo + fl, &fentries)) continue;
                    for (const DictEntry& e : fentries) {
                        if (e.op == 18 && e.operands.size() >= 2) {
                            usize privSize = static_cast<usize>(e.operands[0]);
                            usize privOff = cffBase + static_cast<usize>(e.operands[1]);
                            ParseCffPrivateDict(base, size, privOff, privSize, &out->fdPrograms[i], true);
                        }
                    }
                }
            }
        }
        // FDSelect.
        out->fdSelect.perGlyph.assign(nGlyphs, 0);
        if (!fdSelectLoc.empty()) {
            usize selOff = cffBase + static_cast<usize>(fdSelectLoc[0].first);
            u8 fmt = 0;
            if (r.U8At(selOff, &fmt)) {
                if (fmt == 0) {
                    for (u32 g = 0; g < nGlyphs; ++g) {
                        u8 fd = 0;
                        if (!r.U8At(selOff + 1 + g, &fd)) break;
                        out->fdSelect.perGlyph[g] = fd;
                    }
                } else if (fmt == 3) {
                    u16 nRanges = 0;
                    if (r.U16At(selOff + 1, &nRanges) && nRanges <= 4096) {
                        for (u32 i = 0; i < nRanges; ++i) {
                            u16 first = 0, fd = 0;
                            u32 next = 0;
                            usize p = selOff + 3 + static_cast<usize>(i) * 4;
                            if (!r.U16At(p, &first) || !r.U16At(p + 2, &fd)) break;
                            if (i + 1 < nRanges) {
                                if (!r.U32At(selOff + 3 + static_cast<usize>(i + 1) * 4, &next)) break;
                            } else {
                                u16 sentinel = 0;
                                if (!r.U16At(selOff + 3 + static_cast<usize>(nRanges) * 4, &sentinel)) break;
                                next = sentinel;
                            }
                            u32 last = next <= nGlyphs ? next : nGlyphs;
                            for (u32 g = first; g < last; ++g) out->fdSelect.perGlyph[g] = fd;
                        }
                    }
                } else {
                    ENG_LOGW("font", "CFF: unsupported FDSelect format %u", static_cast<unsigned>(fmt));
                }
            }
        }
    }

    // FontMatrix: нормируем так, чтобы контуры глифов выходили в шрифтовых единицах.
    f32 sx = fontMatrix[0] != 0.0f ? fontMatrix[0] : 0.001f;
    f32 sy = fontMatrix[3] != 0.0f ? fontMatrix[3] : 0.001f;
    impl->head.unitsPerEm = 1.0f / sx;
    (void)sy;
    *formatOut = FontFormat::OpenTypeCFF;
    (void)formatOut;
    ENG_LOGI("font", "CFF: %u glyphs%s", nGlyphs, isCid ? " (CID keyed)" : "");
    return true;
}

std::vector<std::vector<Vec2f>> BuildCffOutline(Font::Impl* impl, const CffFont* cff, u32 gid) {
    std::vector<std::vector<Vec2f>> contours;
    if (!cff || gid >= cff->program.charStrings.Count()) return contours;
    const CffProgram* prog = &cff->program;
    if (cff->cid && !cff->fdPrograms.empty()) {
        u8 fd = cff->fdSelect.Default(gid);
        if (fd < cff->fdPrograms.size()) prog = &cff->fdPrograms[fd];
    }
    usize off = 0, len = 0;
    if (!prog->charStrings.Object(impl->size, gid, &off, &len)) return contours;
    T2Interp interp;
    interp.base = impl->data;
    interp.size = impl->size;
    interp.prog = prog;
    interp.out = &contours;
    interp.nameToGid = &cff->nameToGid;
    interp.glyphNames = &cff->glyphNames;
    interp.sids = &cff->sids;
    interp.Run(impl->data + off, len);
    if (interp.widthState == 2) {
        impl->cffAdvances[gid] = interp.width;  // явная ширина в charstring
    } else if (prog->defaultWidthX != 0.0f) {
        impl->cffAdvances[gid] = prog->defaultWidthX;
    }
    return contours;
}

// --- таблицы, которым нужен весь каталог ------------------------------------
void ParseKernTable(Font::Impl* impl) {
    const TableRec* t = impl->Table(kTagKern);
    if (t) ParseKern(impl->data, impl->size, *t, &impl->kern);
    const TableRec* g = impl->Table(kTagGpos);
    if (g && impl->kern.pairs.empty()) ParseGpos(impl->data, impl->size, *g, &impl->gpos);
}

// --- парсеры каталога таблиц ------------------------------------------------
bool ParseSfnt(Font::Impl* impl, usize offset, FontFormat* formatOut) {
    Reader r(impl->data, impl->size, offset);
    u32 version = 0;
    if (!r.U32(&version)) return false;
    if (version == 0x74746366) {  // 'ttcf'
        ENG_LOGW("font", "TTC collection: only the first face is used");
        r.Seek(offset + 8);
        u32 first = 0;
        if (!r.U32(&first)) return false;
        return ParseSfnt(impl, offset + first, formatOut);
    }
    if (version != 0x00010000 && version != 0x4F54544F && version != 0x74727565 && version != 0x74797031) {
        ENG_LOGE("font", "unknown sfnt version 0x%08X", version);
        return false;
    }
    bool otto = (version == 0x4F54544F);
    u16 numTables = 0, searchRange = 0, entrySelector = 0, rangeShift = 0;
    if (!r.U16(&numTables) || !r.U16(&searchRange) || !r.U16(&entrySelector) || !r.U16(&rangeShift)) return false;
    (void)searchRange;
    (void)entrySelector;
    (void)rangeShift;
    if (numTables == 0 || numTables > 512) return false;
    usize recBase = offset + 12;
    if (!r.InRange(recBase, static_cast<usize>(numTables) * 16)) return false;
    for (u32 i = 0; i < numTables; ++i) {
        usize p = recBase + static_cast<usize>(i) * 16;
        u32 tag = 0, checksum = 0, off = 0, len = 0;
        if (!r.U32At(p, &tag) || !r.U32At(p + 4, &checksum) || !r.U32At(p + 8, &off) || !r.U32At(p + 12, &len))
            return false;
        (void)checksum;
        TableRec t;
        t.tag = tag;
        t.offset = off;
        t.length = len;
        t.present = impl->InRange(off, len);
        if (!t.present) {
            // Тег — big-endian fourcc; печатаем его в порядке файла, а не в
            // порядке памяти, иначе читается задом наперёд ("tsop" для "post").
            const char tagText[5] = {static_cast<char>((tag >> 24) & 0xFF),
                                     static_cast<char>((tag >> 16) & 0xFF),
                                     static_cast<char>((tag >> 8) & 0xFF),
                                     static_cast<char>(tag & 0xFF), 0};
            ENG_LOGW("font", "table %s is out of bounds (%u bytes at %u)", tagText, len, off);
            t.length = 0;
            continue;
        }
        if (tag == kTagHead && len < 54) {
            ENG_LOGE("font", "head table too small (%u)", len);
            return false;
        }
        if (tag == kTagMaxp && len < 6) {
            ENG_LOGE("font", "maxp table too small (%u)", len);
            return false;
        }
        impl->tables[tag] = t;
    }
    if (impl->tables.find(kTagHead) == impl->tables.end()) {
        ENG_LOGE("font", "missing head table");
        return false;
    }
    if (impl->tables.find(kTagMaxp) == impl->tables.end()) {
        ENG_LOGE("font", "missing maxp table");
        return false;
    }
    *formatOut = otto ? FontFormat::OpenTypeCFF : FontFormat::TrueType;
    return true;
}

void ParseHead(Font::Impl* impl) {
    const TableRec* t = impl->Table(kTagHead);
    if (!t || t->length < 54) return;
    Reader r(impl->data, impl->size);
    u16 unitsPerEm = 0, indexToLocFormat = 0;
    i16 xMin = 0, yMin = 0, xMax = 0, yMax = 0;
    if (!r.U16At(t->offset + 18, &unitsPerEm)) return;
    if (!r.I16At(t->offset + 36, &xMin) || !r.I16At(t->offset + 38, &yMin) || !r.I16At(t->offset + 40, &xMax) ||
        !r.I16At(t->offset + 42, &yMax))
        return;
    if (!r.U16At(t->offset + 50, &indexToLocFormat)) return;
    impl->head.unitsPerEm = unitsPerEm > 0 ? static_cast<f32>(unitsPerEm) : 1000.0f;
    impl->head.indexToLocFormat = indexToLocFormat;
    impl->head.xMin = xMin;
    impl->head.yMin = yMin;
    impl->head.xMax = xMax;
    impl->head.yMax = yMax;
}

void ParseHhea(Font::Impl* impl) {
    const TableRec* t = impl->Table(kTagHhea);
    if (!t || t->length < 36) return;
    Reader r(impl->data, impl->size);
    i16 asc = 0, desc = 0, gap = 0;
    u16 nMetrics = 0;
    if (!r.I16At(t->offset + 4, &asc) || !r.I16At(t->offset + 6, &desc) || !r.I16At(t->offset + 8, &gap)) return;
    if (!r.U16At(t->offset + 34, &nMetrics)) return;
    impl->hhea.ascender = asc;
    impl->hhea.descender = desc;
    impl->hhea.lineGap = gap;
    impl->hhea.numberOfHMetrics = nMetrics;
}

void ParseMaxp(Font::Impl* impl) {
    const TableRec* t = impl->Table(kTagMaxp);
    if (!t || t->length < 6) return;
    Reader r(impl->data, impl->size);
    u16 n = 0;
    if (!r.U16At(t->offset + 4, &n)) return;
    impl->numGlyphs = n;
}

void ParseOs2(Font::Impl* impl) {
    const TableRec* t = impl->Table(kTagOs2);
    if (!t || t->length < 68) return;
    Reader r(impl->data, impl->size);
    Os2Table o;
    u16 version = 0;
    if (!r.U16At(t->offset, &version)) return;
    o.version = version;
    i16 v = 0;
    if (r.I16At(t->offset + 68, &v)) {
        o.typoAscender = v;
        if (r.I16At(t->offset + 70, &v)) o.typoDescender = v;
        if (r.I16At(t->offset + 72, &v)) o.typoLineGap = v;
        o.hasTypo = true;
    }
    u16 u = 0;
    if (version >= 1 && r.U16At(t->offset + 74, &u)) {
        o.winAscent = u;
        if (r.U16At(t->offset + 76, &u)) o.winDescent = u;
        o.hasWin = true;
    }
    if (version >= 2 && t->length >= 90) {
        i16 xh = 0, ch = 0;
        if (r.I16At(t->offset + 86, &xh)) {
            o.xHeight = xh;
            o.hasX = true;
        }
        if (r.I16At(t->offset + 88, &ch)) {
            o.capHeight = ch;
            o.hasCap = true;
        }
    }
    if (version >= 4 && t->length >= 100) {
        u16 fsSelection = 0;
        if (r.U16At(t->offset + 62, &fsSelection)) o.useTypoMetrics = (fsSelection & 0x0080) != 0;
    }
    impl->os2 = o;
}

void ParsePost(Font::Impl* impl) {
    const TableRec* t = impl->Table(kTagPost);
    if (!t || t->length < 32) return;
    Reader r(impl->data, impl->size);
    u32 fixed = 0;
    i16 underlinePos = 0, underlineThick = 0;
    if (!r.U32At(t->offset, &fixed)) return;
    if (!r.I16At(t->offset + 8, &underlinePos) || !r.I16At(t->offset + 10, &underlineThick)) return;
    impl->post.italicAngle = static_cast<f32>(static_cast<i32>(fixed)) / 65536.0f;
    impl->post.underlinePosition = underlinePos;
    impl->post.underlineThickness = underlineThick;
}

}  // namespace fontimpl
}  // namespace crossrender

namespace crossrender {



// ===========================================================================
// Font
// ===========================================================================
Font::Font() : impl_(std::make_unique<Impl>()) {}
Font::~Font() = default;

void Font::Destroy() {
    pages_.clear();
    glyphCache_.clear();
    fallbacks_.clear();
    if (impl_) {
        impl_->owned.clear();
        impl_->owned.shrink_to_fit();
        impl_->data = nullptr;
        impl_->size = 0;
        impl_->tables.clear();
        impl_->atlasUsedPixels = 0;
        impl_->atlasCapacityPixels = 0;
    }
    valid_ = false;
    format_ = FontFormat::Unknown;
}

// Заполняет метрики и прогрев кэша глифов; `impl` уже должен быть разобран.
bool FontLoadCommon(Font* font, Font::Impl* impl, FontFormat fmt) {
    ParseHead(impl);
    ParseHhea(impl);
    ParseMaxp(impl);
    ParseOs2(impl);
    ParsePost(impl);
    const TableRec* cmap = impl->Table(kTagCmap);
    if (cmap) {
        if (!ParseCmap(impl->data, impl->size, *cmap, &impl->cmap)) {
            ENG_LOGW("font", "cmap: no usable subtable");
        }
    }
    ParseKernTable(impl);

    const FontDesc& fd = FontTestAccess::DescOf(font);
    f32 upem = impl->head.unitsPerEm > 0.0f ? impl->head.unitsPerEm : 1000.0f;
    f32 scale = fd.pixelHeight / upem;
    if (scale <= 0.0f) scale = 1.0f;
    // Предпочитаем типографские метрики, когда шрифт сам их просит
    // (бит 7 fsSelection в OS/2), иначе пару из hhea.
    f32 asc = impl->hhea.ascender;
    f32 desc = impl->hhea.descender;
    f32 gap = impl->hhea.lineGap;
    if (impl->os2.hasTypo && (impl->os2.useTypoMetrics || asc == 0.0f)) {
        asc = impl->os2.typoAscender;
        desc = impl->os2.typoDescender;
        gap = impl->os2.typoLineGap;
    }
    if (asc == 0.0f && impl->os2.hasWin) asc = impl->os2.winAscent;
    if (desc == 0.0f && impl->os2.hasWin) desc = -impl->os2.winDescent;
    f32 ascPx = asc * scale;
    f32 descPx = desc * scale;
    if (descPx > 0.0f) descPx = -descPx;
    f32 gapPx = gap * scale;
    f32 lineHeight = ascPx - descPx + gapPx;
    if (lineHeight <= 0.0f) lineHeight = fd.pixelHeight;
    if (ascPx <= 0.0f) ascPx = lineHeight * 0.8f;
    if (descPx >= 0.0f) descPx = -lineHeight * 0.2f;
    f32 capPx = (impl->os2.hasCap && impl->os2.capHeight > 0.0f) ? impl->os2.capHeight * scale
                                                                 : 0.7f * upem * scale;
    f32 xPx = (impl->os2.hasX && impl->os2.xHeight > 0.0f) ? impl->os2.xHeight * scale : 0.5f * upem * scale;
    f32 ulPos = 0.0f, ulThick = 0.0f;
    if (impl->post.underlinePosition != 0.0f || impl->post.underlineThickness != 0.0f) {
        ulPos = impl->post.underlinePosition * scale;
        ulThick = impl->post.underlineThickness * scale;
    } else {
        ulPos = -0.1f * upem * scale;
        ulThick = 0.05f * upem * scale;
    }
    if (ulThick < 1.0f) ulThick = 1.0f;
    FontTestAccess::SetMetrics(font, ascPx, descPx, gapPx, capPx, xPx, ulPos, ulThick, upem);
    (void)lineHeight;
    FontTestAccess::Publish(font, impl, true, fmt);
    (void)scale;
    return true;
}

bool Font::LoadFromMemory(const void* data, usize size, const FontDesc& desc) {
    Destroy();
    desc_ = desc;
    if (data == nullptr || size < 12) {
        ENG_LOGE("font", "LoadFromMemory: buffer too small (%zu bytes)", size);
        return false;
    }
    const u8* bytes = static_cast<const u8*>(data);
    impl_->owned.assign(bytes, bytes + size);
    impl_->data = impl_->owned.data();
    impl_->size = size;

    // TTC: используем первый шрифт набора, о чём предупреждает лог ниже.
    usize faceOffset = 0;
    u32 tag = 0;
    {
        Reader r(impl_->data, impl_->size);
        if (r.U32At(0, &tag) && tag == kTagTtcf) {
            u32 first = 0;
            if (!r.U32At(12, &first) || first >= size) {
                ENG_LOGE("font", "TTC: bad first face offset");
                Destroy();
                return false;
            }
            ENG_LOGW("font", "TTC: using face 0 of %zu bytes only", size);
            faceOffset = first;
        }
    }
    FontFormat fmt = FontFormat::Unknown;
    if (!ParseSfnt(impl_.get(), faceOffset, &fmt)) {
        Destroy();
        return false;
    }
    CffFont cff;
    if (fmt == FontFormat::OpenTypeCFF) {
        if (!LoadCff(impl_.get(), &fmt, &cff)) {
            ENG_LOGE("font", "CFF: load failed");
            Destroy();
            return false;
        }
    } else if (impl_->Table(kTagCff) != nullptr) {
        // OpenType с таблицей CFF, но TrueType-версией sfnt: верим CFF.
        if (LoadCff(impl_.get(), &fmt, &cff)) fmt = FontFormat::OpenTypeCFF;
    } else if (impl_->Table(kTagCff2) != nullptr) {
        ENG_LOGW("font", "CFF2 variable fonts are not supported; falling back to glyf");
    }

    impl_->cff = std::make_shared<CffFont>(std::move(cff));
    std::string family, style;
    const TableRec* name = impl_->Table(kTagName);
    if (name) ParseName(impl_->data, impl_->size, *name, &family, &style);
    family_ = family.empty() ? "(unnamed)" : family;
    style_ = style;
    valid_ = false;  // set by FontLoadCommon
    FontLoadCommon(this, impl_.get(), fmt);
    if (format_ == FontFormat::OpenTypeCFF) ENG_LOGI("font", "loaded OTF/CFF '%s'", family_.c_str());
    else ENG_LOGI("font", "loaded TTF '%s' (%u glyphs)", family_.c_str(), impl_->numGlyphs);

    if (desc_.prebake) PrebakeAscii();
    return valid_;
}

bool Font::LoadFromFile(const std::string& path, const FontDesc& desc) {
    ByteBuffer buf = ReadBinaryFile(path);
    if (buf.empty()) {
        ENG_LOGE("font", "cannot read '%s'", path.c_str());
        return false;
    }
    if (!LoadFromMemory(buf.data(), buf.size(), desc)) return false;
    source_ = path;
    return true;
}

u32 Font::GlyphIndex(u32 codepoint) const {
    if (!valid_ || !impl_) return 0;
    if (impl_->cmap.subs.empty()) return codepoint < impl_->numGlyphs ? codepoint : 0;
    u32 gid = 0;
    if (impl_->cmap.Lookup(impl_->data, impl_->size, codepoint, &gid)) return gid;
    return 0;
}

bool Font::HasGlyph(u32 codepoint) const {
    if (!valid_) return false;
    return GlyphIndex(codepoint) != 0;
}

// `const`, хотя лениво растеризует и выгружает: трогаются только указуемый Impl
// и приватные кэши, но не собственное состояние Font.
const Glyph* Font::GetGlyph(u32 codepoint) const {
    if (!valid_ || !impl_) return nullptr;
    // Дешёвая (по одному флагу) повторная попытка для глифов, растеризованных до
    // появления GL-контекста: создаём текстуры атласа и проигрываем их пиксели.
    if (impl_->hasPending) FlushPendingUploads(const_cast<Font*>(this), impl_.get());
    auto it = glyphCache_.find(codepoint);
    if (it != glyphCache_.end()) return &it->second;

    u32 gid = GlyphIndex(codepoint);
    const bool procedural = impl_->procedural;
    if (!procedural && gid == 0 && codepoint != 0) return nullptr;  // глифа нет

    f32 scale = desc_.pixelHeight / (impl_->head.unitsPerEm > 0.0f ? impl_->head.unitsPerEm : 1000.0f);
    if (scale <= 0.0f) scale = 1.0f;

    RenderedGlyph rg;
    if (!RenderAnyGlyph(const_cast<Font*>(this), impl_.get(), codepoint, gid, &rg)) return nullptr;
    f32 advance = AdvanceFor(impl_.get(), gid) * scale;
    if (rg.blank && advance <= 0.0f) return nullptr;
    Glyph g;
    if (!PlaceGlyph(const_cast<Font*>(this), impl_.get(), codepoint, rg, advance, &g)) {
        // Растеризация или выгрузка в атлас не удались (например, GL-контекста
        // ещё нет): не кэшируем, чтобы следующий вызов повторил попытку.
        return nullptr;
    }
    auto& cache = const_cast<std::unordered_map<u32, Glyph>&>(glyphCache_);
    auto res = cache.emplace(codepoint, g);
    return &res.first->second;
}

// Векторные контуры для рендерера в стиле Slug: только квадратичные контуры, y вниз,
// (0,0) в позиции пера на базовой линии.  Пробел возвращает true с
// `empty = true`; false означает лишь «такого глифа нет» или «нет источника контура».
bool Font::GetGlyphOutlineUnits(u32 codepoint, GlyphOutline* out) const {
    if (!out) return false;
    *out = GlyphOutline{};
    if (!valid_ || !impl_) return false;
    if (impl_->procedural) return ProceduralGlyphOutline(impl_.get(), codepoint, 1.0f, out);
    if (!impl_->isCff && impl_->Table(kTagGlyf) == nullptr) return false;
    const u32 gid = GlyphIndex(codepoint);
    if (gid == 0 && codepoint != 0) return false;
    return BuildGlyphOutline(impl_.get(), impl_->cff.get(), gid, 1.0f, 0.0f, false, out);
}

bool Font::GetGlyphOutline(u32 codepoint, f32 size, GlyphOutline* out) const {
    if (!out) return false;
    *out = GlyphOutline{};
    if (!valid_ || !impl_) return false;
    const f32 upem = impl_->head.unitsPerEm > 0.0f ? impl_->head.unitsPerEm : 1000.0f;
    const f32 em = size > 0.0f ? size : (desc_.pixelHeight > 0.0f ? desc_.pixelHeight : upem);
    // `size` — размер em в пикселях, поэтому это совпадает с растеризатором при
    // size == desc_.pixelHeight (та же геометрия, минус чисто растеризационный
    // grid fit).
    const f32 scale = em / upem;
    if (impl_->procedural) return ProceduralGlyphOutline(impl_.get(), codepoint, scale, out);
    if (!impl_->isCff && impl_->Table(kTagGlyf) == nullptr) return false;
    const u32 gid = GlyphIndex(codepoint);
    if (gid == 0 && codepoint != 0) return false;
    const bool slant = desc_.italic && desc_.italicSlant != 0.0f;
    return BuildGlyphOutline(impl_.get(), impl_->cff.get(), gid, scale, desc_.italicSlant, slant, out);
}

f32 Font::GetKerning(u32 left, u32 right) const {
    if (!valid_ || !impl_) return 0.0f;
    u32 lg = GlyphIndex(left);
    u32 rg = GlyphIndex(right);
    if (lg == 0 || rg == 0) return 0.0f;
    f32 v = impl_->kern.Lookup(lg, rg);
    if (v == 0.0f) v = impl_->gpos.Lookup(lg, rg);
    return v;
}

f32 Font::AtlasOccupancy() const {
    if (!impl_ || impl_->atlasCapacityPixels == 0) return 0.0f;
    f32 occ = static_cast<f32>(static_cast<f64>(impl_->atlasUsedPixels) /
                               static_cast<f64>(impl_->atlasCapacityPixels));
    return Clamp(occ, 0.0f, 1.0f);
}

void Font::AddFallback(Font* font) {
    if (!font || font == this) return;
    for (Font* f : fallbacks_)
        if (f == font) return;
    fallbacks_.push_back(font);
}

Font* Font::Resolve(u32 codepoint, const Glyph** glyphOut) const {
    if (glyphOut) *glyphOut = nullptr;
    if (!valid_) return nullptr;
    if (HasGlyph(codepoint)) {
        const Glyph* g = GetGlyph(codepoint);
        if (g) {
            if (glyphOut) *glyphOut = g;
            return const_cast<Font*>(this);
        }
    }
    for (Font* f : fallbacks_) {
        if (!f) continue;
        const Glyph* g = f->GetGlyph(codepoint);
        if (g && !g->isEmpty()) {
            if (glyphOut) *glyphOut = g;
            return f;
        }
    }
    return nullptr;
}

void Font::Prebake(const u32* codepoints, int count) {
    if (!codepoints || count <= 0) return;
    for (int i = 0; i < count; ++i) GetGlyph(codepoints[i]);
}

void Font::PrebakeAscii() {
    for (u32 cp = 32; cp <= 126; ++cp) GetGlyph(cp);
}

namespace fontimpl {

// ===========================================================================
// 7. Спрямление контуров, хинтинг и синтетические стили
// ===========================================================================
void AddSeg(std::vector<Seg>* segs, const Vec2f& p0, const Vec2f& p1, f32 dev) {
    if (p0.x == p1.x && p0.y == p1.y) return;
    Seg s;
    s.ax = p0.x;
    s.ay = p0.y;
    s.bx = p1.x;
    s.by = p1.y;
    s.dev = dev;
    segs->push_back(s);
}

void FlattenQuad(std::vector<Seg>* segs, const Vec2f& p0, const Vec2f& p1, const Vec2f& p2, f32 tol) {
    f32 d = std::fabs(p1.x * 2.0f - p0.x - p2.x) + std::fabs(p1.y * 2.0f - p0.y - p2.y);
    f32 chord = std::fabs(p2.x - p0.x) + std::fabs(p2.y - p0.y);
    f32 dev = (d + chord) * 0.5f * 0.5f;
    int n = 1;
    if (tol > 1e-4f && dev > tol) {
        f32 ratio = dev / tol;
        if (ratio > 1e6f) ratio = 1e6f;
        n = static_cast<int>(std::sqrt(ratio)) + 1;
    }
    if (n > 48) n = 48;
    if (n < 1) n = 1;
    Vec2f prev = p0;
    for (int i = 1; i <= n; ++i) {
        f32 t = static_cast<f32>(i) / static_cast<f32>(n);
        f32 mt = 1.0f - t;
        f32 bx = mt * mt * p0.x + 2.0f * mt * t * p1.x + t * t * p2.x;
        f32 by = mt * mt * p0.y + 2.0f * mt * t * p1.y + t * t * p2.y;
        Vec2f q{bx, by, true};
        AddSeg(segs, prev, q, dev);
        prev = q;
    }
}

void FlattenCubic(std::vector<Seg>* segs, const Vec2f& p0, const Vec2f& p1, const Vec2f& p2, const Vec2f& p3,
                  f32 tol) {
    f32 dev = (std::fabs(p1.x * 3.0f - p0.x * 2.0f - p3.x) + std::fabs(p1.y * 3.0f - p0.y * 2.0f - p3.y) +
               std::fabs(p2.x * 3.0f - p0.x - p3.x * 2.0f) + std::fabs(p2.y * 3.0f - p0.y - p3.y * 2.0f)) *
              0.25f * 0.5f;
    int n = 1;
    if (tol > 1e-4f && dev > tol) {
        f32 ratio = dev / tol;
        if (ratio > 1e6f) ratio = 1e6f;
        n = static_cast<int>(std::sqrt(ratio)) + 1;
    }
    if (n > 48) n = 48;
    if (n < 1) n = 1;
    Vec2f prev = p0;
    for (int i = 1; i <= n; ++i) {
        f32 t = static_cast<f32>(i) / static_cast<f32>(n);
        f32 mt = 1.0f - t;
        f32 a = mt * mt * mt, b = 3.0f * mt * mt * t, c = 3.0f * mt * t * t, d = t * t * t;
        Vec2f q{a * p0.x + b * p1.x + c * p2.x + d * p3.x, a * p0.y + b * p1.y + c * p2.y + d * p3.y, true};
        AddSeg(segs, prev, q, dev);
        prev = q;
    }
}

void FlattenContours(const std::vector<std::vector<Vec2f>>& contours, f32 tol, std::vector<Seg>* segs) {
    segs->clear();
    for (const std::vector<Vec2f>& c : contours) {
        if (c.size() < 2) continue;
        // Конвенция TrueType: контур может начинаться с точки вне кривой; тогда
        // неявное начало — середина между последней и первой точками.
        bool startOn = c[0].on;
        Vec2f start = c[0];
        size_t begin = 1;
        if (!startOn) {
            const Vec2f& last = c.back();
            if (last.on) {
                start = c[0];
                startOn = true;
                begin = 1;
            } else {
                start.x = (c[0].x + last.x) * 0.5f;
                start.y = (c[0].y + last.y) * 0.5f;
                start.on = true;
                begin = 0;
            }
        }
        Vec2f cur = start;
        size_t n = c.size();
        size_t i = begin;
        size_t guard = 0;
        while (guard++ <= n + 2) {
            const Vec2f& p = c[i % n];
            if (p.on) {
                AddSeg(segs, cur, p, 0.0f);
                cur = p;
                ++i;
            } else {
                const Vec2f& next = c[(i + 1) % n];
                Vec2f end;
                if (next.on) {
                    end = next;
                } else {
                    end.x = (p.x + next.x) * 0.5f;
                    end.y = (p.y + next.y) * 0.5f;
                    end.on = true;
                }
                FlattenQuad(segs, cur, p, end, tol);
                cur = end;
                i += next.on ? 2 : 1;
            }
            if (i - begin >= n) break;
        }
    }
}

// --- лёгкое выравнивание по сетке -------------------------------------------
// Два независимых 1-D прохода.
//   * вертикальные стемы: x-кромки в пределах 0.35..1.2px друг от друга
//     спариваются и привязываются так, что левая кромка ложится на границу
//     пикселя, а стем сохраняет (как минимум) один пиксель ширины;
//   * горизонтальные кромки: базовая линия (0) и, если шрифт их даёт, «блюзы»
//     cap-height / x-height пришиваются к сетке; прочие y-кромки квантуются к
//     ближайшему центру пикселя со сдвигом к ближайшей зоне.  Это классическая
//     эвристика «blue zone» без полноценного интерпретатора хинтинга
//     (программы `cvt`/`fpgm` не исполняются).
void BuildHintGrids(std::vector<f32>* xs, std::vector<f32>* ys, const std::vector<Cmd>& cmds, f32 capHeightPx,
                    f32 xHeightPx) {
    std::vector<std::pair<f32, f32>> vert, horiz;  // (кромка, парная кромка)
    const usize n = cmds.size();
    for (usize i = 0; i < n; ++i) {
        const Cmd& a = cmds[i];
        const Cmd& b = cmds[(i + 1) % n];
        if (a.type == CmdType::MoveTo && b.type == CmdType::MoveTo) continue;
        f32 ax = a.p[0].x, ay = a.p[0].y;
        f32 bx = b.p[0].x, by = b.p[0].y;
        f32 w = bx - ax, h = by - ay;
        if (std::fabs(h) <= 1e-4f && std::fabs(w) >= 0.35f && std::fabs(w) <= 1.2f) {
            vert.emplace_back(ax, bx);
        } else if (std::fabs(w) <= 1e-4f && std::fabs(h) >= 0.35f && std::fabs(h) <= 1.2f) {
            horiz.emplace_back(ay, by);
        }
    }
    std::sort(vert.begin(), vert.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    xs->clear();
    f32 prevRight = -1e30f;
    for (const auto& s : vert) {
        f32 left = s.first, right = s.second;
        if (left < prevRight - 0.01f) continue;  // пропускаем перекрывающиеся / внутренние кромки
        f32 ql = std::floor(left + 0.5f);
        f32 w = std::fabs(right - left);
        if (w < 1.0f) w = 1.0f;
        f32 qr = std::floor(ql + w + 0.5f);
        xs->push_back(ql);
        xs->push_back(qr);
        prevRight = right < qr ? right : qr;
    }

    std::sort(horiz.begin(), horiz.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<f32> zones;
    zones.push_back(0.0f);
    if (capHeightPx > 1.0f) zones.push_back(std::floor(capHeightPx + 0.5f));
    if (xHeightPx > 1.0f) zones.push_back(std::floor(xHeightPx + 0.5f));
    ys->clear();
    f32 prevY = -1e30f;
    for (const auto& s : horiz) {
        f32 y0 = s.first, y1 = s.second;
        if (y0 < prevY - 0.01f) continue;
        f32 best = y0;
        if (y0 > -0.75f && y0 < 0.75f) best = 0.0f;  // базовая линия
        else best = std::floor(y0 + 0.5f);
        for (f32 z : zones) {
            if (std::fabs(y0 - z) < 0.6f) {
                best = z;
                break;
            }
        }
        f32 h = std::fabs(y1 - y0);
        if (h < 1.0f) h = 1.0f;
        f32 top = best + std::floor(h + 0.5f);
        ys->push_back(best);
        ys->push_back(top);
        prevY = y1 > top ? y1 : top;
    }
}

f32 SnapTo(const std::vector<f32>& grid, f32 v) {
    f32 best = v;
    f32 bestD = 0.51f;
    for (f32 g : grid) {
        f32 d = std::fabs(g - v);
        if (d < bestD) {
            bestD = d;
            best = g;
        }
    }
    return best;
}

void ApplyHinting(std::vector<Cmd>* cmds, f32 capHeightPx, f32 xHeightPx) {
    if (cmds->empty()) return;
    std::vector<f32> xs, ys;
    BuildHintGrids(&xs, &ys, *cmds, capHeightPx, xHeightPx);
    if (xs.empty() && ys.empty()) return;
    for (Cmd& c : *cmds) {
        const usize n = CmdPointCount(c.type);
        for (usize i = 0; i < n; ++i) {
            c.p[i].x = SnapTo(xs, c.p[i].x);
            c.p[i].y = SnapTo(ys, c.p[i].y);
        }
    }
}

// Добавляет контур ещё дважды, смещённым по горизонтали на `d` пикселей — это
// дешёвая аппроксимация синтетического bold.  (Вместе с исходным проходом все
// три растеризации ложатся на сетку 0.5 * d.)
void AddBoldOffsets(const std::vector<Cmd>& in, f32 d, std::vector<Cmd>* out) {
    if (d <= 0.0f || out == nullptr) return;
    for (f32 off : {d * 0.5f, d}) {
        for (const Cmd& c : in) {
            Cmd n = c;
            for (Vec2f& p : n.p) p.x += off;
            out->push_back(n);
        }
    }
}

void ApplyOblique(std::vector<Cmd>* cmds, f32 slant) {
    if (slant == 0.0f) return;
    for (Cmd& c : *cmds) {
        const usize n = CmdPointCount(c.type);
        for (usize i = 0; i < n; ++i) c.p[i].x += slant * (-c.p[i].y);
    }
}

void ComputeBox(const std::vector<Cmd>& cmds, f32* minX, f32* minY, f32* maxX, f32* maxY) {
    f32 x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (const Cmd& c : cmds) {
        const usize n = CmdPointCount(c.type);
        for (usize i = 0; i < n; ++i) {
            const Vec2f& p = c.p[i];
            if (p.x < x0) x0 = p.x;
            if (p.y < y0) y0 = p.y;
            if (p.x > x1) x1 = p.x;
            if (p.y > y1) y1 = p.y;
        }
    }
    *minX = x0;
    *minY = y0;
    *maxX = x1;
    *maxY = y1;
}

// ===========================================================================
// 8. Растеризатор
// ===========================================================================
struct Contour {
    u32 first = 0;  // индекс первой кромки
    u32 count = 0;  // число кромок (замыкание неявное)
    f32 y0 = 0, y1 = 0;
};

// Разбивает поток команд на контуры из прямых кромок (замкнутые).
void BuildEdges(const std::vector<Cmd>& cmds, std::vector<Seg>* edges, std::vector<Contour>* contours) {
    edges->clear();
    contours->clear();
    Vec2f start, cur;
    bool open = false, haveStart = false;
    u32 first = 0;
    auto finish = [&]() {
        if (open && haveStart) AddSeg(edges, cur, start, 0.0f);
        open = false;
        haveStart = false;
        u32 count = static_cast<u32>(edges->size()) - first;
        if (count > 0) {
            f32 lo = 1e30f, hi = -1e30f;
            for (u32 k = 0; k < count; ++k) {
                const Seg& e = (*edges)[first + k];
                lo = std::fmin(lo, std::fmin(e.ay, e.by));
                hi = std::fmax(hi, std::fmax(e.ay, e.by));
            }
            Contour c;
            c.first = first;
            c.count = count;
            c.y0 = lo;
            c.y1 = hi;
            contours->push_back(c);
        }
        first = static_cast<u32>(edges->size());
    };
    for (const Cmd& c : cmds) {
        Vec2f p = c.p[0];
        if (c.type == CmdType::MoveTo) {
            finish();
            start = p;
            cur = p;
            haveStart = true;
            open = true;
        } else {
            if (!haveStart) {
                start = p;
                cur = p;
                haveStart = true;
                open = true;
            }
            AddSeg(edges, cur, p, 0.0f);
            cur = p;
        }
    }
    finish();
}

// Сканлайн-отрезки по контурам (неявное правило заливки полигонов у контуров).
// Сканлайн-заливка с ненулевым числом обхода по *всем* контурам, поэтому
// счётчики (дырки) в глифе пробивают внешний контур, как того требует TrueType.
struct Crossing {
    f32 x = 0;
    f32 dir = 0;
};

void RowSpans(const std::vector<Seg>& edges, const std::vector<Contour>& contours, f32 sy,
              std::vector<std::pair<f32, f32>>* spans) {
    spans->clear();
    static thread_local std::vector<Crossing> cross;
    cross.clear();
    for (const Contour& c : contours) {
        if (sy < c.y0 || sy >= c.y1) continue;
        for (u32 k = 0; k < c.count; ++k) {
            const Seg& e = edges[c.first + k];
            f32 y0 = e.ay, y1 = e.by;
            if (y0 == y1) continue;
            f32 dir = 1.0f;
            if (y0 > y1) {
                std::swap(y0, y1);
                dir = -1.0f;
            }
            if (sy < y0 || sy >= y1) continue;
            f32 t = (sy - e.ay) / (e.by - e.ay);
            Crossing k2;
            k2.x = e.ax + (e.bx - e.ax) * t;
            k2.dir = dir;
            cross.push_back(k2);
        }
    }
    if (cross.empty()) return;
    std::sort(cross.begin(), cross.end(), [](const Crossing& a, const Crossing& b) { return a.x < b.x; });
    f32 wind = 0.0f;
    f32 spanStart = 0.0f;
    for (const Crossing& c : cross) {
        if (wind == 0.0f) spanStart = c.x;
        wind += c.dir;
        if (wind == 0.0f && c.x > spanStart) spans->emplace_back(spanStart, c.x);
    }
}

// Растеризатор покрытия: `sampleCount` вертикальных проб на строку (по умолчанию 4),
// каждая под-сканлиния добавляет дробное горизонтальное покрытие.
void RasteriseToBuffer(const std::vector<Cmd>& cmds, int width, int height, f32 originX, f32 originY,
                       int sampleCount, f32 gamma, std::vector<u8>* out) {
    out->assign(static_cast<usize>(width) * static_cast<usize>(height), 0);
    if (width <= 0 || height <= 0) return;
    std::vector<Seg> edges;
    std::vector<Contour> contours;
    BuildEdges(cmds, &edges, &contours);
    if (contours.empty()) return;
    if (sampleCount < 1) sampleCount = 1;
    if (sampleCount > 4) sampleCount = 4;

    std::vector<f32> cover(static_cast<usize>(width) + 1, 0.0f);
    std::vector<std::pair<f32, f32>> spans;
    static thread_local std::vector<f32> sampleY;
    sampleY.clear();
    sampleY.resize(static_cast<usize>(sampleCount));
    const f32 invSamples = 1.0f / static_cast<f32>(sampleCount);
    const f32 w = static_cast<f32>(width);

    // Координаты уже в пикселях, поэтому один пиксель — ровно одна единица,
    // а `originY` — верхняя кромка строки 0.
    for (int py = 0; py < height; ++py) {
        for (int s = 0; s < sampleCount; ++s) {
            sampleY[static_cast<usize>(s)] =
                originY + static_cast<f32>(py) + (static_cast<f32>(s) + 0.5f) / static_cast<f32>(sampleCount);
        }
        std::fill(cover.begin(), cover.end(), 0.0f);
        for (int s = 0; s < sampleCount; ++s) {
            RowSpans(edges, contours, sampleY[static_cast<usize>(s)], &spans);
            for (const auto& sp : spans) {
                f32 xa = sp.first - originX;
                f32 xb = sp.second - originX;
                if (xa > xb) std::swap(xa, xb);
                if (xb <= 0.0f || xa >= w) continue;
                if (xa < 0.0f) xa = 0.0f;
                if (xb > w) xb = w;
                int ia = static_cast<int>(std::floor(xa));
                int ib = static_cast<int>(std::floor(xb));
                if (ia >= width) continue;
                if (ia == ib) {
                    cover[static_cast<usize>(ia)] += xb - xa;
                } else {
                    cover[static_cast<usize>(ia)] += static_cast<f32>(ia + 1) - xa;
                    for (int p = ia + 1; p < ib && p < width; ++p) cover[static_cast<usize>(p)] += 1.0f;
                    if (ib < width) cover[static_cast<usize>(ib)] += xb - static_cast<f32>(ib);
                }
            }
        }
        for (int px = 0; px < width; ++px) {
            f32 cov = cover[static_cast<usize>(px)] * invSamples;
            (*out)[static_cast<usize>(py) * static_cast<usize>(width) + static_cast<usize>(px)] =
                CoverageByte(cov, gamma);
        }
    }
}

// Растеризует объединение нескольких контуров команд (объединение через max покрытия).
void RasteriseUnion(const std::vector<std::vector<Cmd>>& passes, int width, int height, f32 originX, f32 originY,
                    int sampleCount, f32 gamma, std::vector<u8>* out) {
    out->clear();
    if (width <= 0 || height <= 0 || passes.empty()) {
        out->assign(static_cast<usize>(std::max(width, 0)) * static_cast<usize>(std::max(height, 0)), 0);
        return;
    }
    std::vector<u8> tmp;
    bool first = true;
    for (const std::vector<Cmd>& pass : passes) {
        RasteriseToBuffer(pass, width, height, originX, originY, sampleCount, gamma, &tmp);
        if (first) {
            *out = tmp;
            first = false;
        } else {
            const usize n = std::min(out->size(), tmp.size());
            for (usize i = 0; i < n; ++i)
                if (tmp[i] > (*out)[i]) (*out)[i] = tmp[i];
        }
    }
}

// --- signed distance field --------------------------------------------------
// Сетка имеет одну ячейку на пиксель, `spread` выражен в пикселях.  Для каждой
// строки объединяются верхние и нижние x-позиции пересечений контура;
// `inside` — это затем пробный тест между соседними пересечениями, что точно
// для несамопересекающихся контуров (обычный случай контуров глифов).
void RasteriseSdf(const std::vector<Cmd>& cmds, int width, int height, f32 spread, std::vector<u8>* out) {
    out->assign(static_cast<usize>(width) * static_cast<usize>(height), 0);
    if (width <= 0 || height <= 0) return;
    if (spread < 0.5f) spread = 0.5f;
    f32 minX = 0, minY = 0, maxX = 0, maxY = 0;
    ComputeBox(cmds, &minX, &minY, &maxX, &maxY);
    (void)maxX;
    (void)maxY;

    std::vector<Seg> edges;
    std::vector<Contour> contours;
    BuildEdges(cmds, &edges, &contours);
    if (contours.empty()) {
        std::fill(out->begin(), out->end(), 255);
        return;
    }

    // Кандидатные корзины по строкам для прохода расстояний.
    std::vector<std::vector<u32>> rows(static_cast<usize>(height));
    for (u32 i = 0; i < edges.size(); ++i) {
        const Seg& e = edges[i];
        f32 lo = std::fmin(e.ay, e.by) - spread;
        f32 hi = std::fmax(e.ay, e.by) + spread;
        int r0 = static_cast<int>(std::floor(lo - minY - 0.5f));
        int r1 = static_cast<int>(std::ceil(hi - minY - 0.5f));
        if (r0 < 0) r0 = 0;
        if (r1 > height - 1) r1 = height - 1;
        for (int r = r0; r <= r1; ++r) rows[static_cast<usize>(r)].push_back(i);
    }

    struct XDir {
        f32 x = 0;
        f32 dir = 0;
    };
    std::vector<XDir> cross;
    std::vector<u8> inside(static_cast<usize>(width), 0);
    std::vector<u8> nearMask(static_cast<usize>(width), 0);
    std::vector<u32> nearGen(static_cast<usize>(width), 0);
    u32 gen = 0;
    const f32 scale = 127.0f / spread;

    for (int py = 0; py < height; ++py) {
        f32 yc = minY + static_cast<f32>(py) + 0.5f;
        // Внутри/снаружи определяется тестом ненулевого числа обхода в центре
        // пикселя — ровно как в растеризаторе покрытия, поэтому счётчики и
        // перекрывающиеся контуры разрешаются одинаково.
        cross.clear();
        for (const Seg& e : edges) {
            if (e.ay == e.by) continue;
            f32 y0 = e.ay, y1 = e.by;
            f32 dir = 1.0f;
            if (y0 > y1) {
                std::swap(y0, y1);
                dir = -1.0f;
            }
            if (yc < y0 || yc >= y1) continue;
            f32 t = (yc - e.ay) / (e.by - e.ay);
            cross.push_back({e.ax + (e.bx - e.ax) * t, dir});
        }
        std::sort(cross.begin(), cross.end(), [](const XDir& a2, const XDir& b2) { return a2.x < b2.x; });
        std::fill(inside.begin(), inside.end(), 0);
        {
            f32 wind = 0.0f;
            f32 runStart = 0.0f;
            for (const XDir& c : cross) {
                if (wind == 0.0f) runStart = c.x;
                wind += c.dir;
                if (wind == 0.0f && c.x > runStart) {
                    int p0 = static_cast<int>(std::floor(runStart - minX));
                    int p1 = static_cast<int>(std::ceil(c.x - minX));
                    if (p0 < 0) p0 = 0;
                    if (p1 > width) p1 = width;
                    for (int px = p0; px < p1; ++px) inside[static_cast<usize>(px)] = 1;
                }
            }
        }

        ++gen;
        if (gen == 0) {
            std::fill(nearGen.begin(), nearGen.end(), 0);
            gen = 1;
        }
        std::fill(nearMask.begin(), nearMask.end(), 0);
        const std::vector<u32>& rowEdges = rows[static_cast<usize>(py)];
        for (u32 ei : rowEdges) {
            const Seg& e = edges[ei];
            f32 x0 = std::fmin(e.ax, e.bx) - spread;
            f32 x1 = std::fmax(e.ax, e.bx) + spread;
            int p0 = static_cast<int>(std::floor(x0 - minX - 0.5f));
            int p1 = static_cast<int>(std::ceil(x1 - minX + 0.5f));
            if (p0 < 0) p0 = 0;
            if (p1 > width - 1) p1 = width - 1;
            for (int px = p0; px <= p1; ++px) {
                nearGen[static_cast<usize>(px)] = gen;
                nearMask[static_cast<usize>(px)] = 1;
            }
        }
        for (int px = 0; px < width; ++px) {
            usize idx = static_cast<usize>(py) * static_cast<usize>(width) + static_cast<usize>(px);
            if (nearMask[static_cast<usize>(px)] == 0) {
                (*out)[idx] = inside[static_cast<usize>(px)] ? 0 : 255;
                continue;
            }
            f32 xc = minX + static_cast<f32>(px) + 0.5f;
            f32 best = 1e30f;
            for (u32 ei : rowEdges) {
                const Seg& e = edges[ei];
                // Вертикальный отсев (корзина — надмножество).
                if (std::fmin(e.ay, e.by) - spread > yc || std::fmax(e.ay, e.by) + spread < yc) continue;
                f32 d = SegDistSq(xc, yc, e.ax, e.ay, e.bx, e.by);
                if (d < best) best = d;
            }
            f32 dist = best < 1e29f ? std::sqrt(best) : spread;
            f32 v = inside[static_cast<usize>(px)] ? 128.0f - dist * scale : 128.0f + dist * scale;
            if (v < 0.0f) v = 0.0f;
            if (v > 255.0f) v = 255.0f;
            (*out)[idx] = static_cast<u8>(v + 0.5f);
        }
    }
}

// ===========================================================================
// 9. Метрики, отрисовка глифов и shelf-упаковка атласа
// ===========================================================================
// Разбивает команды кривых на прямые LineTo, чтобы построители кромок
// (покрытие + SDF) видели только полигоны.
void FlattenCommands(const std::vector<Cmd>& in, f32 tol, std::vector<Cmd>* out) {
    out->clear();
    Vec2f cur;
    Vec2f start;
    bool haveStart = false;
    auto emitLine = [&](const Vec2f& p) {
        if (!haveStart) {
            start = p;
            cur = p;
            haveStart = true;
            Cmd m;
            m.type = CmdType::MoveTo;
            m.p[0] = p;
            out->push_back(m);
            return;
        }
        if (p.x == cur.x && p.y == cur.y) return;
        Cmd l;
        l.type = CmdType::LineTo;
        l.p[0] = p;
        out->push_back(l);
        cur = p;
    };
    for (const Cmd& c : in) {
        switch (c.type) {
            case CmdType::MoveTo:
                if (haveStart) emitLine(start);  // неявное замыкание предыдущего контура
                haveStart = false;
                emitLine(c.p[0]);
                start = cur;
                break;
            case CmdType::LineTo:
                emitLine(c.p[0]);
                break;
            case CmdType::QuadTo: {
                std::vector<Seg> segs;
                FlattenQuad(&segs, cur, c.p[0], c.p[1], tol);
                for (const Seg& sg : segs) {
                    Vec2f p;
                    p.x = sg.bx;
                    p.y = sg.by;
                    emitLine(p);
                }
                break;
            }
            case CmdType::CubicTo: {
                std::vector<Seg> segs;
                FlattenCubic(&segs, cur, c.p[0], c.p[1], c.p[2], tol);
                for (const Seg& sg : segs) {
                    Vec2f p;
                    p.x = sg.bx;
                    p.y = sg.by;
                    emitLine(p);
                }
                break;
            }
        }
    }
    if (haveStart) emitLine(start);  // замыкаем финальный контур
}

// Преобразует один замкнутый контур в шрифтовом пространстве в команды рисования.
// Контур обходится как замкнутая петля и всегда начинается с точки на кривой
// (неявная середина, когда и первая, и последняя точки вне кривой — это
// допускает конвенция TrueType).
void ConvertContour(const std::vector<Vec2f>& pts, bool cubic, std::vector<Cmd>* cmds) {
    if (pts.size() < 2) return;
    std::vector<Vec2f> c = pts;
    // Отбрасываем явную дублирующую замыкающую точку, чтобы логика петли была однородной.
    if (c.size() >= 2 && c.front().x == c.back().x && c.front().y == c.back().y && c.front().on == c.back().on)
        c.pop_back();
    const usize n = c.size();
    if (n < 2) return;

    auto clean = [](const Vec2f& v) {
        Vec2f o = v;
        if (!std::isfinite(o.x)) o.x = 0.0f;
        if (!std::isfinite(o.y)) o.y = 0.0f;
        return o;
    };
    auto mid = [](const Vec2f& a2, const Vec2f& b2) {
        Vec2f m;
        m.x = (a2.x + b2.x) * 0.5f;
        m.y = (a2.y + b2.y) * 0.5f;
        m.on = true;
        return m;
    };
    auto push = [&](CmdType t, const Vec2f& p0) {
        Cmd cmd;
        cmd.type = t;
        cmd.p[0] = clean(p0);
        cmds->push_back(cmd);
    };

    // Выбираем стартовую точку на кривой.
    Vec2f start;
    usize first = 0;
    if (c[0].on) {
        start = clean(c[0]);
        first = 1;
    } else if (c[n - 1].on) {
        start = clean(c[n - 1]);
        first = 0;
    } else {
        start = mid(clean(c[n - 1]), clean(c[0]));
        first = 0;
    }
    push(CmdType::MoveTo, start);

    Vec2f cur = start;
    usize i = first;
    usize remaining = c.size();
    int guard = 0;
    const int limit = static_cast<int>(n) * 3 + 8;
    while (remaining > 0 && guard++ < limit) {
        const Vec2f p = clean(c[i % n]);
        if (p.on) {
            if (!(p.x == cur.x && p.y == cur.y)) push(CmdType::LineTo, p);
            cur = p;
            ++i;
            --remaining;
            continue;
        }
        if (cubic) {
            if (remaining >= 3) {
                Cmd q;
                q.type = CmdType::CubicTo;
                q.p[0] = p;
                q.p[1] = clean(c[(i + 1) % n]);
                q.p[2] = clean(c[(i + 2) % n]);
                cmds->push_back(q);
                cur = q.p[2];
                i += 3;
                remaining -= 3;
            } else if (remaining == 2) {
                Cmd q;
                q.type = CmdType::CubicTo;
                q.p[0] = p;
                q.p[1] = clean(c[(i + 1) % n]);
                q.p[2] = start;
                cmds->push_back(q);
                cur = start;
                i += 2;
                remaining = 0;
            } else {
                Cmd q;
                q.type = CmdType::CubicTo;
                q.p[0] = p;
                q.p[1] = p;
                q.p[2] = start;
                cmds->push_back(q);
                cur = start;
                ++i;
                --remaining;
            }
        } else {
            // Квадратичные: две подряд идущие точки вне кривой подразумевают
            // точку на кривой между ними (конвенция TrueType).
            const Vec2f next = remaining >= 2 ? clean(c[(i + 1) % n]) : start;
            Vec2f end = next;
            if (!next.on) {
                end = mid(p, next);
                ++i;
                --remaining;
            } else {
                i += 2;
                remaining = remaining >= 2 ? remaining - 2 : 0;
            }
            Cmd q;
            q.type = CmdType::QuadTo;
            q.p[0] = p;
            q.p[1] = end;
            cmds->push_back(q);
            cur = end;
        }
    }
    // Явно замыкаем петлю, если обход не закончил на стартовой точке.
    if (!(cur.x == start.x && cur.y == start.y)) push(CmdType::LineTo, start);
}

void BuildCommands(const std::vector<std::vector<Vec2f>>& contours, bool cubic, std::vector<Cmd>* cmds) {
    cmds->clear();
    for (const std::vector<Vec2f>& c : contours) ConvertContour(c, cubic, cmds);
}

// ===========================================================================
// Экспорт векторных контуров (для аналитического текстового рендерера в стиле Slug)
// ===========================================================================
struct QuadSeg {
    Vec2f ctrl;
    Vec2f end;
};

Vec2f QuadAt(const Vec2f& a, const Vec2f& b, const Vec2f& c, f32 t) {
    const f32 mt = 1.0f - t;
    Vec2f r;
    r.x = mt * mt * a.x + 2.0f * mt * t * b.x + t * t * c.x;
    r.y = mt * mt * a.y + 2.0f * mt * t * b.y + t * t * c.y;
    r.on = true;
    return r;
}

Vec2f CubicAt(const Vec2f& a, const Vec2f& b, const Vec2f& c, const Vec2f& d, f32 t) {
    const f32 mt = 1.0f - t;
    const f32 w0 = mt * mt * mt, w1 = 3.0f * mt * mt * t, w2 = 3.0f * mt * t * t, w3 = t * t * t;
    Vec2f r;
    r.x = w0 * a.x + w1 * b.x + w2 * c.x + w3 * d.x;
    r.y = w0 * a.y + w1 * b.y + w2 * c.y + w3 * d.y;
    r.on = true;
    return r;
}

// Аппроксимирует кубическую кривую уровнями квадратичного разбиения вплоть до
// `kQuadMaxSplit` (то есть не более четырёх квадратичных на кубическую).
// Квадратичная проходит через середину кубической; отклонение меряется
// в t = 0.25, 0.5 и 0.75 и сравнивается с `tol` в шрифтовых единицах.
void CubicToQuadratics(const Vec2f& p0, const Vec2f& c1, const Vec2f& c2, const Vec2f& p3, f32 tol, int depth,
                       std::vector<QuadSeg>* out) {
    Vec2f q;
    q.x = (3.0f * (c1.x + c2.x) - (p0.x + p3.x)) * 0.25f;
    q.y = (3.0f * (c1.y + c2.y) - (p0.y + p3.y)) * 0.25f;
    q.on = false;

    f32 err = 0.0f;
    for (f32 t : {0.25f, 0.5f, 0.75f}) {
        const Vec2f cc = CubicAt(p0, c1, c2, p3, t);
        const Vec2f qq = QuadAt(p0, q, p3, t);
        const f32 dx = cc.x - qq.x, dy = cc.y - qq.y;
        const f32 d = std::sqrt(dx * dx + dy * dy);
        if (d > err) err = d;
    }
    if (err <= tol || depth >= 2) {
        QuadSeg seg;
        seg.ctrl = q;
        seg.end = p3;
        out->push_back(seg);
        return;
    }
    // Разбиение де Кастельжо при t = 0.5.
    auto mid = [](const Vec2f& a, const Vec2f& b) {
        Vec2f m;
        m.x = (a.x + b.x) * 0.5f;
        m.y = (a.y + b.y) * 0.5f;
        return m;
    };
    const Vec2f m01 = mid(p0, c1), m12 = mid(c1, c2), m23 = mid(c2, p3);
    const Vec2f m012 = mid(m01, m12), m123 = mid(m12, m23);
    const Vec2f centre = mid(m012, m123);
    CubicToQuadratics(p0, m01, m012, centre, tol, depth + 1, out);
    CubicToQuadratics(centre, m123, m23, p3, tol, depth + 1, out);
}

// Спрямляет один загруженный список контуров в квадратичные GlyphContours
// в шрифтовых единицах, y вверх.  Каждый контур начинается и заканчивается
// на кривой, а замыкающая точка повторяет первую (`points.front() == points.back()`).
void MakeQuadraticContours(const std::vector<std::vector<Vec2f>>& src, bool cubic, std::vector<GlyphContour>* out) {
    out->clear();
    std::vector<Cmd> cmds;
    BuildCommands(src, cubic, &cmds);
    GlyphContour current;
    bool open = false;
    Vec2f first{}, cur{};

    auto pushPoint = [&](const Vec2f& v, u8 on) {
        GlyphPoint gp;
        gp.p = Vec2(v.x, v.y);
        gp.onCurve = on;
        current.points.push_back(gp);
    };
    auto flush = [&]() {
        if (!open) return;
        open = false;
        if (current.points.size() >= 2) {
            // Явно замыкаем петлю, если обход этого уже не сделал.
            const GlyphPoint& a = current.points.front();
            const GlyphPoint& b = current.points.back();
            if (a.p.x != b.p.x || a.p.y != b.p.y) current.points.push_back(a);
            if (current.points.size() >= 3) {
                current.closed = true;
                out->push_back(current);
            }
        }
        current.points.clear();
    };

    for (const Cmd& cmd : cmds) {
        switch (cmd.type) {
            case CmdType::MoveTo:
                flush();
                current.points.clear();
                pushPoint(cmd.p[0], 1);
                first = cmd.p[0];
                cur = cmd.p[0];
                open = true;
                break;
            case CmdType::LineTo:
                pushPoint(cmd.p[0], 1);
                cur = cmd.p[0];
                break;
            case CmdType::QuadTo:
                pushPoint(cmd.p[0], 0);
                pushPoint(cmd.p[1], 1);
                cur = cmd.p[1];
                break;
            case CmdType::CubicTo: {
                if (!open) break;
                std::vector<QuadSeg> quads;
                CubicToQuadratics(cur, cmd.p[0], cmd.p[1], cmd.p[2], 0.2f, 0, &quads);
                for (const QuadSeg& qs : quads) {
                    pushPoint(qs.ctrl, 0);
                    pushPoint(qs.end, 1);
                    cur = qs.end;
                }
                break;
            }
        }
    }
    flush();
}

// --- потребители в стиле Slug ----------------------------------------------
// Применяет финальное преобразование (y вниз) и вычисляет плотные границы чернил,
// включая реальные экстремумы квадратичных, а не только опорные точки.
void FinalizeOutline(std::vector<GlyphContour>* contours, f32 scale, f32 slant, bool applySlant, Rect* bounds) {
    f32 minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
    bool any = false;
    auto expand = [&](f32 x, f32 y) {
        if (x < minX) minX = x;
        if (y < minY) minY = y;
        if (x > maxX) maxX = x;
        if (y > maxY) maxY = y;
        any = true;
    };
    for (GlyphContour& contour : *contours) {
        for (GlyphPoint& gp : contour.points) {
            gp.p.x *= scale;
            gp.p.y *= -scale;  // шрифтовое пространство — y вверх, API — y вниз
            if (applySlant && slant != 0.0f) gp.p.x += slant * (-gp.p.y);
        }
        const usize n = contour.points.size();
        // Полигон ограничивают точки на кривой; опорные точки квадратичных
        // достигаются только через их кривую, чьи экстремумы добавляются ниже,
        // поэтому итоговый бокс — плотный боксы чернил, а не бокс опорных точек.
        for (usize i = 0; i < n; ++i) {
            if (contour.points[i].onCurve) expand(contour.points[i].p.x, contour.points[i].p.y);
        }
        for (usize i = 1; i + 1 < n; ++i) {
            if (contour.points[i].onCurve) continue;
            const Vec2& a = contour.points[i - 1].p;
            const Vec2& b = contour.points[i].p;
            const Vec2& c = contour.points[i + 1].p;
            for (int axis = 0; axis < 2; ++axis) {
                const f32 a0 = a[axis], b0 = b[axis], c0 = c[axis];
                const f32 denom = a0 - 2.0f * b0 + c0;
                if (std::fabs(denom) < 1e-9f) continue;
                const f32 t = (a0 - b0) / denom;
                if (t <= 0.0f || t >= 1.0f) continue;
                Vec2f fa, fb, fc;
                fa.x = a.x; fa.y = a.y;
                fb.x = b.x; fb.y = b.y;
                fc.x = c.x; fc.y = c.y;
                const Vec2f p = QuadAt(fa, fb, fc, t);
                expand(p.x, p.y);
            }
        }
    }
    if (bounds) *bounds = any ? Rect{minX, minY, maxX - minX, maxY - minY} : Rect{};
}

// Замкнутый прямоугольник в шрифтовом пространстве (y вверх) на [0, w] x [0, h];
// вызывающий прогоняет его через FinalizeOutline — тот масштабирует и переворачивает в y-вниз.
void MakeBoxOutline(f32 w, f32 h, std::vector<GlyphContour>* out) {
    out->clear();
    GlyphContour c;
    c.closed = true;
    const f32 xs[4] = {0.0f, w, w, 0.0f};
    const f32 ys[4] = {h, h, 0.0f, 0.0f};
    for (int i = 0; i < 4; ++i) {
        GlyphPoint gp;
        gp.p = Vec2(xs[i], ys[i]);
        gp.onCurve = 1;
        c.points.push_back(gp);
    }
    c.points.push_back(c.points.front());
    out->push_back(std::move(c));
}

// У встроенного шрифта нет контуров; синтезируем его ячейку 5x7 как прямоугольник,
// чтобы векторному рендереру было что рисовать.
bool ProceduralGlyphOutline(Font::Impl* impl, u32 codepoint, f32 scale, GlyphOutline* out) {
    out->contours.clear();
    out->bounds = Rect{};
    out->empty = true;
    out->advance = impl->procAdvance * scale;
    if (codepoint == ' ' || codepoint == 0x00A0) return true;
    // Ячейка 5x7 в шрифтовых единицах, масштабируется и переворачивается FinalizeOutline.
    MakeBoxOutline(impl->procUnitsPerEm * 5.0f / 7.0f, impl->procUnitsPerEm, &out->contours);
    FinalizeOutline(&out->contours, scale, 0.0f, false, &out->bounds);
    out->empty = out->contours.empty();
    return true;
}

bool BuildGlyphOutline(Font::Impl* impl, const CffFont* cff, u32 gid, f32 scale, f32 slant, bool applySlant,
                       GlyphOutline* out) {
    std::vector<std::vector<Vec2f>> src;
    if (impl->isCff && cff) src = BuildCffOutline(impl, cff, gid);
    else LoadGlyfOutline(impl, gid, &src);
    MakeQuadraticContours(src, impl->isCff, &out->contours);
    FinalizeOutline(&out->contours, scale, slant, applySlant, &out->bounds);
    out->advance = AdvanceFor(impl, gid) * scale;
    out->empty = out->contours.empty();
    return true;
}

// Растеризует один глиф.  Здесь происходит только работа на CPU: выгрузка в
// атлас, если она нужна, — забота вызывающего.
bool RenderGlyphToBitmap(Font::Impl* impl, const CffFont* cff, u32 gid, const FontDesc& desc, f32 scale,
                         RenderedGlyph* get) {
    get->bitmap.clear();
    get->width = get->height = 0;
    get->bearingX = get->bearingY = 0;
    get->blank = false;

    std::vector<std::vector<Vec2f>> contours;
    bool cubic = impl->isCff;
    if (impl->isCff && cff) {
        contours = BuildCffOutline(impl, cff, gid);
    } else {
        LoadGlyfOutline(impl, gid, &contours);
    }

    std::vector<Cmd> cmds;
    BuildCommands(contours, cubic, &cmds);
    if (!cmds.empty()) {
        for (Cmd& c : cmds) {
            const usize n = CmdPointCount(c.type);
            for (usize i = 0; i < n; ++i) {
                c.p[i].x *= scale;
                c.p[i].y *= -scale;
            }
        }
        if (desc.italic && desc.italicSlant != 0.0f) ApplyOblique(&cmds, desc.italicSlant);
    }
    // Кривые спрямляются первыми: хинтинг и оба растеризатора работают с
    // полигонами, и бокс чернил тогда — плотный бокс реального контура,
    // а не опорных точек.
    std::vector<Cmd> flat;
    FlattenCommands(cmds, 0.2f, &flat);
    if (!flat.empty() && desc.hinting) ApplyHinting(&flat, 0.0f, 0.0f);
    cmds.swap(flat);

    f32 minX = 0, minY = 0, maxX = 0, maxY = 0;
    if (!cmds.empty()) ComputeBox(cmds, &minX, &minY, &maxX, &maxY);
    const f32 atlasLimit = static_cast<f32>(std::max<int>(desc.atlasSize, 8));

    // Заявленные в спеке 4x вертикальных суперсэмпла соответствуют дефолтному
    // `oversample` = 2; горизонтальное покрытие считается аналитически (точное
    // покрытие отрезком), что заменяет горизонтальную выборку.
    int sampleCount = static_cast<int>(desc.oversample);
    if (sampleCount < 1) sampleCount = 1;
    if (sampleCount > 4) sampleCount = 4;
    sampleCount *= 2;

    if (desc.sdf) {
        f32 spread = desc.sdfSpread > 1.0f ? desc.sdfSpread : 1.0f;
        f32 bx0 = std::floor(minX - spread);
        f32 by0 = std::floor(minY - spread);
        f32 bx1 = std::ceil(maxX + spread);
        f32 by1 = std::ceil(maxY + spread);
        f32 wF = bx1 - bx0;
        f32 hF = by1 - by0;
        if (wF < 2.0f || hF < 2.0f) {
            get->blank = true;
            return true;
        }
        bool scaled = false;
        f32 s = 1.0f;
        if (wF > atlasLimit - 2.0f || hF > atlasLimit - 2.0f) {
            f32 fit = (atlasLimit - 2.0f) / std::max(wF, hF);
            s = fit < 0.05f ? 0.05f : fit;
            scaled = true;
        }
        std::vector<Cmd> sdfCmds;
        const std::vector<Cmd>* use = &cmds;
        f32 oX = bx0, oY = by0;
        f32 sp = spread;
        if (scaled) {
            sdfCmds = cmds;
            for (Cmd& c : sdfCmds) {
                for (Vec2f& p : c.p) {
                    p.x *= s;
                    p.y *= s;
                }
            }
            use = &sdfCmds;
            oX = std::floor(minX * s);
            oY = std::floor(minY * s);
            sp = spread * s;
        }
        int w = static_cast<int>(std::ceil((scaled ? maxX * s : maxX) + sp - oX)) + 1;
        int h = static_cast<int>(std::ceil((scaled ? maxY * s : maxY) + sp - oY)) + 1;
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        std::vector<Cmd> pixelSpace;
        pixelSpace.reserve(use->size());
        for (const Cmd& c : *use) {
            Cmd n = c;
            for (Vec2f& p : n.p) {
                p.x -= oX;
                p.y -= oY;
            }
            pixelSpace.push_back(n);
        }
        std::vector<u8> sdf;
        RasteriseSdf(pixelSpace, w, h, sp, &sdf);
        get->bitmap = std::move(sdf);
        get->width = w;
        get->height = h;
        f32 inv = scaled ? 1.0f / s : 1.0f;
        get->bearingX = oX * inv;
        get->bearingY = -oY * inv;
        return true;
    }

    // Битмапный режим: целочисленный бокс, включающий протяжённости чернил.
    int w = static_cast<int>(std::ceil(maxX)) - static_cast<int>(std::floor(minX));
    int h = static_cast<int>(std::ceil(maxY)) - static_cast<int>(std::floor(minY));
    if (w <= 0 || h <= 0) {
        get->blank = true;
        return true;
    }
    if (w > static_cast<int>(atlasLimit) || h > static_cast<int>(atlasLimit)) {
        f32 fit = (atlasLimit - 2.0f) / static_cast<f32>(std::max(w, h));
        if (fit < 0.05f) fit = 0.05f;
        for (Cmd& c : cmds) {
            const usize n = CmdPointCount(c.type);
            for (usize i = 0; i < n; ++i) {
                c.p[i].x *= fit;
                c.p[i].y *= fit;
            }
        }
        ComputeBox(cmds, &minX, &minY, &maxX, &maxY);
        w = static_cast<int>(std::ceil(maxX)) - static_cast<int>(std::floor(minX));
        h = static_cast<int>(std::ceil(maxY)) - static_cast<int>(std::floor(minY));
        if (w <= 0 || h <= 0 || w > static_cast<int>(atlasLimit) || h > static_cast<int>(atlasLimit)) {
            get->blank = true;
            return true;
        }
    }
    f32 originX = std::floor(minX);
    f32 originY = std::floor(minY);

    std::vector<std::vector<Cmd>> passes;
    passes.push_back(cmds);
    if (desc.bold && desc.boldAmount > 0.0f) {
        f32 r = desc.boldAmount * 0.5f;
        for (f32 ox : {-r * 0.5f, r * 0.5f}) {
            for (f32 oy : {-r * 0.5f, r * 0.5f}) {
                std::vector<Cmd> off = cmds;
                for (Cmd& c : off) {
                    const usize n = CmdPointCount(c.type);
                    for (usize i = 0; i < n; ++i) {
                        c.p[i].x += ox;
                        c.p[i].y += oy;
                    }
                }
                passes.push_back(std::move(off));
            }
        }
    }
    std::vector<u8> bitmap;
    RasteriseUnion(passes, w, h, originX, originY, sampleCount, desc.gamma, &bitmap);
    get->bitmap = std::move(bitmap);
    get->width = w;
    get->height = h;
    get->bearingX = originX;
    get->bearingY = -originY;
    return true;
}

// --- атлас ------------------------------------------------------------------
// (Пере)создаёт текстуру страницы атласа.  Глифы можно запросить до появления
// OpenGL-контекста (headless-тесты, прогрев UI); тогда страница создаётся позже,
// при первом вызове GetGlyph(), которому реально нужно выгрузить пиксели.
bool EnsurePageTexture(FontAtlasPage* page, const FontDesc& desc) {
    if (page->texture.Valid()) return true;
    page->texture.Create(page->size, page->size, PixelFormat::RGBA8, nullptr, TextureFilter::Linear,
                         TextureWrap::ClampToEdge, false);
    if (!page->texture.Valid()) return false;
    page->texture.SetDebugName("font-atlas");
    // SDF-страницы помнят spread/размер, чтобы текстовый шейдер мог отобразить
    // 8-битное расстояние обратно в пиксельный диапазон.
    page->texture.SetSdfParams(desc.sdf ? desc.sdfSpread : 0.0f, desc.pixelHeight);
    ENG_LOGI("font", "atlas page texture created (%dx%d)", page->size, page->size);
    return true;
}

// Проигрывает отложенные выгрузки глифов, сделанные при отсутствии GL-контекста.
void FlushPendingUploads(Font* font, Font::Impl* impl) {
    if (!impl->hasPending) return;
    std::vector<FontAtlasPage>& pages = FontTestAccess::Pages(font);
    const FontDesc& desc = FontTestAccess::DescOf(font);
    bool any = false;
    for (usize i = 0; i < pages.size() && i < impl->pending.size(); ++i) {
        std::vector<Font::Impl::PendingUpload>& q = impl->pending[i];
        if (q.empty()) continue;
        if (!EnsurePageTexture(&pages[i], desc)) {
            any = true;  // контекста всё ещё нет; очередь сохраняем
            continue;
        }
        for (const Font::Impl::PendingUpload& p : q)
            pages[i].texture.Update(p.rgba.data(), p.x, p.y, p.w, p.h);
        q.clear();
    }
    impl->hasPending = any;
}

bool EnsurePage(Font* font, Font::Impl* impl, int neededW, int neededH, bool* textureReady) {
    std::vector<FontAtlasPage>& pages = FontTestAccess::Pages(font);
    const FontDesc& desc = FontTestAccess::DescOf(font);
    const int gap = AtlasGapFor(desc);
    *textureReady = false;
    if (!pages.empty()) {
        FontAtlasPage& p = pages.back();
        if (p.usedWidth + neededW + gap <= p.size &&
            p.usedHeight + p.rowHeight + neededH + gap <= p.size) {
            *textureReady = EnsurePageTexture(&p, desc);
            return true;
        }
        // Переходим на новую страницу, а не форсируем новую строку: так делаем,
        // только когда текущая страница вообще не может вместить глиф.
        if (p.usedHeight + p.rowHeight + neededH + gap <= p.size) {
            p.usedWidth = 0;
            p.usedHeight += p.rowHeight + gap;
            p.rowHeight = 0;
            if (p.usedWidth + neededW + gap <= p.size) {
                *textureReady = EnsurePageTexture(&p, desc);
                return true;
            }
        }
    }
    if (pages.size() >= 8) return false;
    FontAtlasPage page;
    page.size = static_cast<int>(std::max<u32>(desc.atlasSize, 16));
    if (page.size < neededW + gap) page.size = neededW + gap;
    if (page.size < neededH + gap) page.size = neededH + gap;
    page.texture.Create(page.size, page.size, PixelFormat::RGBA8, nullptr, TextureFilter::Linear,
                        TextureWrap::ClampToEdge, false);
    page.texture.SetDebugName("font-atlas");
    page.texture.SetSdfParams(desc.sdf ? desc.sdfSpread : 0.0f, desc.pixelHeight);
    impl->atlasCapacityPixels += static_cast<u64>(page.size) * static_cast<u64>(page.size);
    ENG_LOGI("font", "atlas page %d created (%dx%d)", static_cast<int>(pages.size()) + 1, page.size, page.size);
    pages.push_back(std::move(page));
    *textureReady = EnsurePageTexture(&pages.back(), desc);
    return true;
}

bool PlaceGlyph(Font* font, Font::Impl* impl, u32 codepoint, const RenderedGlyph& rg, f32 advance, Glyph* out) {
    Glyph g;
    g.codepoint = codepoint;
    g.advance = advance;
    g.bearingX = rg.bearingX;
    g.bearingY = rg.bearingY;
    g.width = static_cast<f32>(rg.width);
    g.height = static_cast<f32>(rg.height);
    if (rg.blank || rg.width <= 0 || rg.height <= 0) {
        // Пустой глиф (пробел) пикселей не имеет, поэтому кэшируем даже без
        // GL-контекста.
        g.width = g.height = 0;
        g.u0 = g.v0 = g.u1 = g.v1 = 0;
        g.page = 0;
        *out = g;
        return true;
    }
    bool textureReady = false;
    if (!EnsurePage(font, impl, rg.width, rg.height, &textureReady)) {
        ENG_LOGW("font", "atlas full (8 pages); glyph U+%04X dropped", codepoint);
        return false;
    }
    std::vector<FontAtlasPage>& pages = FontTestAccess::Pages(font);
    FontAtlasPage& page = pages.back();
    if (page.usedWidth + rg.width + kAtlasGap > page.size) {
        page.usedWidth = 0;
        page.usedHeight += page.rowHeight + kAtlasGap;
        page.rowHeight = 0;
    }
    if (page.usedHeight + rg.height + kAtlasGap > page.size) {
        // После EnsurePage такого быть не должно, но перестрахуемся.
        return false;
    }
    int x = page.usedWidth;
    int y = page.usedHeight;
    std::vector<u8> rgba(static_cast<usize>(rg.width) * static_cast<usize>(rg.height) * 4);
    for (usize i = 0; i < static_cast<usize>(rg.width) * static_cast<usize>(rg.height); ++i) {
        u8 c = rg.bitmap[i];
        rgba[i * 4 + 0] = c;
        rgba[i * 4 + 1] = c;
        rgba[i * 4 + 2] = c;
        rgba[i * 4 + 3] = c;
    }
    if (textureReady) {
        page.texture.Update(rgba.data(), x, y, rg.width, rg.height);
    } else {
        // GL-контекста пока нет: запоминаем пиксели и проиграем их при первом
        // запросе глифа после появления контекста.  UV остаются валидными, так
        // что глиф всё это время можно измерять и кэшировать.
        usize pageIndex = pages.size() - 1;
        if (pageIndex < impl->pending.size()) {
            Font::Impl::PendingUpload up;
            up.x = x;
            up.y = y;
            up.w = rg.width;
            up.h = rg.height;
            up.rgba = std::move(rgba);
            impl->pending[pageIndex].push_back(std::move(up));
            impl->hasPending = true;
        }
    }
    page.usedWidth += rg.width + kAtlasGap;
    if (rg.height > page.rowHeight) page.rowHeight = rg.height;
    impl->atlasUsedPixels += static_cast<u64>(rg.width + kAtlasGap) * static_cast<u64>(rg.height + kAtlasGap);

    g.page = static_cast<int>(pages.size()) - 1;
    f32 ps = static_cast<f32>(page.size);
    g.u0 = static_cast<f32>(x) / ps;
    g.v0 = static_cast<f32>(y) / ps;
    g.u1 = static_cast<f32>(x + rg.width) / ps;
    g.v1 = static_cast<f32>(y + rg.height) / ps;
    *out = g;
    return true;
}

f32 AdvanceFor(Font::Impl* impl, u32 gid) {
    if (!impl) return 0.0f;
    if (impl->procedural) {
        if (gid == 0) return impl->procUnitsPerEm * 0.55f;
        return impl->procAdvance;
    }
    if (impl->isCff) {
        auto it = impl->cffAdvances.find(gid);
        if (it != impl->cffAdvances.end()) return it->second;
        return impl->head.unitsPerEm * 0.5f;
    }
    const TableRec* hmtx = impl->Table(kTagHmtx);
    u32 nMetrics = impl->hhea.numberOfHMetrics;
    if (!hmtx || nMetrics == 0) return impl->head.unitsPerEm * 0.5f;
    u32 idx = gid < nMetrics ? gid : nMetrics - 1;
    usize off = hmtx->offset + static_cast<usize>(idx) * 4;
    Reader r(impl->data, impl->size);
    u16 adv = 0;
    if (!r.U16At(off, &adv)) return impl->head.unitsPerEm * 0.5f;
    return static_cast<f32>(adv);
}

// --- процедурный шрифт ------------------------------------------------------
f32 ProcRowHeight(Font::Impl* impl) { return impl->procUnitsPerEm / 8.0f; }

void ProcFillRect(std::vector<u8>* bmp, int w, int h, int x0, int y0, int x1, int y1, u8 value) {
    if (w <= 0 || h <= 0) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) (*bmp)[static_cast<usize>(y) * static_cast<usize>(w) + static_cast<usize>(x)] = value;
}

// Синтезирует один глиф встроенного шрифта.  Ячейка 5x7 масштабируется
// взвешиванием по ближайшему соседу до `pixelHeight`, затем при необходимости
// делается bold / наклон.  Координаты намеренно пиксельные: у процедурного
// шрифта нет шрифтовых единиц, и конвейер контуров он не трогает.
bool ProcRender(Font::Impl* impl, u32 codepoint, const FontDesc& desc, RenderedGlyph* rg) {
    rg->bitmap.clear();
    rg->width = rg->height = 0;
    rg->bearingX = rg->bearingY = 0;
    rg->blank = false;
    impl->procOutline.clear();
    rg->advance = impl->procAdvance;

    u8 rows[textdetail::kBuiltinGlyphHeight] = {0, 0, 0, 0, 0, 0, 0};
    bool blank = false;
    if (codepoint == ' ' || codepoint == 0x00A0) {
        blank = true;
    } else if (codepoint < 0x80 || (codepoint >= 0x0400 && codepoint <= 0x04FF)) {
        textdetail::BuiltinGlyphRows(codepoint, rows);
    } else {
        // Неизвестная письменность: полая рамка — лучший индикатор «нет глифа»,
        // чем пустота, и сохраняет разумные ширины шага.
        rows[0] = 0x1F;
        rows[6] = 0x1F;
        for (int r = 1; r < 6; ++r) rows[r] = 0x11;
    }
    if (blank) {
        rg->blank = true;
        return true;
    }

    f32 ph = desc.pixelHeight > 1.0f ? desc.pixelHeight : 1.0f;
    int w = static_cast<int>(std::lround(textdetail::kBuiltinGlyphWidth * ph / 7.0f));
    int h = static_cast<int>(std::lround(textdetail::kBuiltinGlyphHeight * ph / 7.0f));
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w > 512) w = 512;
    if (h > 512) h = 512;

    std::vector<u8> bmp(static_cast<usize>(w) * static_cast<usize>(h), 0);
    const int cw = textdetail::kBuiltinGlyphWidth;
    const int ch = textdetail::kBuiltinGlyphHeight;
    for (int gy = 0; gy < ch; ++gy) {
        int y0 = (gy * h) / ch;
        int y1 = ((gy + 1) * h) / ch;
        if (y1 <= y0) y1 = y0 + 1;
        for (int gx = 0; gx < cw; ++gx) {
            if (!(rows[gy] & (1u << (cw - 1 - gx)))) continue;
            int x0 = (gx * w) / cw;
            int x1 = ((gx + 1) * w) / cw;
            if (x1 <= x0) x1 = x0 + 1;
            ProcFillRect(&bmp, w, h, x0, y0, x1, y1, 255);
        }
    }
    if (desc.bold) {
        int off = static_cast<int>(std::lround(desc.boldAmount * ph / 48.0f * 2.0f));
        if (off < 1) off = 1;
        if (off > 4) off = 4;
        std::vector<u8> shifted(bmp.size(), 0);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x + off < w; ++x) {
                shifted[static_cast<usize>(y) * static_cast<usize>(w) + static_cast<usize>(x + off)] =
                    bmp[static_cast<usize>(y) * static_cast<usize>(w) + static_cast<usize>(x)];
            }
        }
        for (usize i = 0; i < bmp.size(); ++i)
            if (shifted[i] > bmp[i]) bmp[i] = shifted[i];
    }
    if (desc.italic && desc.italicSlant != 0.0f) {
        std::vector<u8> sheared(bmp.size(), 0);
        for (int y = 0; y < h; ++y) {
            int dx = static_cast<int>(std::lround(desc.italicSlant * static_cast<f32>(h - y) * 0.5f));
            for (int x = 0; x < w; ++x) {
                int nx = x + dx;
                if (nx < 0 || nx >= w) continue;
                sheared[static_cast<usize>(y) * static_cast<usize>(w) + static_cast<usize>(nx)] =
                    bmp[static_cast<usize>(y) * static_cast<usize>(w) + static_cast<usize>(x)];
            }
        }
        bmp.swap(sheared);
    }
    impl->procOutline = bmp;
    rg->bitmap = std::move(bmp);
    rg->width = w;
    rg->height = h;
    rg->bearingX = 0.0f;
    rg->bearingY = static_cast<f32>(h);
    return true;
}

}  // namespace fontimpl
}  // namespace crossrender

namespace crossrender {

std::unique_ptr<Font> textdetail::CreateProceduralFont(const FontDesc& desc) {
    std::unique_ptr<Font> font;
    Font::Impl* impl = nullptr;
    if (!FontTestAccess::NewFont(&font, &impl)) return nullptr;
    FontTestAccess::SetDesc(font.get(), desc);
    impl->procedural = true;
    impl->procUnitsPerEm = 1024.0f;
    impl->procAdvance = impl->procUnitsPerEm * 0.62f;
    impl->isCff = false;
    impl->head.unitsPerEm = impl->procUnitsPerEm;
    impl->numGlyphs = 65535;

    f32 upem = impl->procUnitsPerEm;
    f32 scale = desc.pixelHeight / upem;
    f32 asc = 0.80f * upem * scale;
    f32 dsc = -0.20f * upem * scale;
    FontTestAccess::SetMetrics(font.get(), asc, dsc, 0.0f, 0.70f * upem * scale, 0.50f * upem * scale,
                               -0.10f * upem * scale, std::max(1.0f, 0.05f * upem * scale), upem);
    FontTestAccess::SetNames(font.get(), "Engine Built-in", "Regular", FontFormat::Bitmap, std::string());
    FontTestAccess::Publish(font.get(), impl, true, FontFormat::Bitmap);
    ENG_LOGI("font", "procedural built-in font created at %.0fpx", desc.pixelHeight);
    return font;
}


// ===========================================================================
bool RasteriseGlyphForTest(const void* fontData, usize size, u32 codepoint, std::vector<u8>* out, int* w,
                           int* h, bool sdf) {
    if (out) out->clear();
    if (w) *w = 0;
    if (h) *h = 0;
    if (!fontData || size < 12) return false;
    std::unique_ptr<Font> font;
    Font::Impl* impl = nullptr;
    if (!FontTestAccess::NewFont(&font, &impl)) return false;
    FontDesc desc;
    desc.pixelHeight = 32.0f;
    desc.atlasSize = 256;
    desc.sdf = sdf;
    desc.sdfSpread = 4.0f;
    desc.hinting = true;
    desc.oversample = 4;
    if (!font->LoadFromMemory(fontData, size, desc)) return false;
    u32 gid = font->GlyphIndex(codepoint);
    if (gid == 0 && codepoint != 0) return false;
    return FontTestAccess::Render(font.get(), gid, out, w, h);
}

f32 MeasureTextWidth(Font& font, const std::string& text) {
    f32 scale = font.Desc().pixelHeight > 0.0f ? 1.0f : 1.0f;  // шаги уже в пикселях
    (void)scale;
    std::vector<u32> cps = Utf8ToCodepoints(text);
    f32 total = 0.0f;
    u32 prev = 0;
    for (usize i = 0; i < cps.size(); ++i) {
        u32 cp = cps[i];
        if (i > 0 && prev != 0) total += font.GetKerning(prev, cp) * font.ScaleForSize(font.Desc().pixelHeight);
        const Glyph* g = font.GetGlyph(cp);
        if (g) {
            total += g->advance;
        } else {
            Font* owner = const_cast<Font&>(font).Resolve(cp, &g);
            if (owner && g) {
                total += g->advance * owner->ScaleForSize(font.Desc().pixelHeight);
            } else {
                total += font.Desc().pixelHeight * 0.5f;
            }
        }
        prev = cp;
    }
    return total;
}

// ===========================================================================
// 11. FontManager
// ===========================================================================
FontManager& FontManager::Get() {
    static FontManager mgr;
    return mgr;
}

namespace {

std::string CacheKey(const std::string& path, const FontDesc& d) {
    char buf[192];
    std::snprintf(buf, sizeof(buf), "|%.2f|%d|%.2f|%d|%u|%d|%u|%u|%d|%.2f|%.2f|%d|%d|%.3f|%u", d.pixelHeight,
                  d.sdf ? 1 : 0, d.sdfSpread, d.hinting ? 1 : 0, d.atlasSize, d.atlasPadding, d.firstCodepoint,
                  d.lastCodepoint, d.prebake ? 1 : 0, d.gamma, d.boldAmount, d.bold ? 1 : 0, d.italic ? 1 : 0,
                  d.italicSlant, d.oversample);
    return path + buf;
}

}  // namespace

Font* FontManager::Load(const std::string& path, const FontDesc& desc) {
    if (path.empty()) return nullptr;
    const std::string key = CacheKey(path, desc);
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;
    auto font = std::unique_ptr<Font>(new Font());
    if (!font->LoadFromFile(path, desc)) {
        ENG_LOGE("font", "FontManager: failed to load '%s'", path.c_str());
        return nullptr;
    }
    Font* raw = font.get();
    owned_.push_back(std::move(font));
    cache_.emplace(key, raw);
    return raw;
}

Font* FontManager::DefaultFont() {
    if (default_) return default_;
    FontDesc d;
    d.pixelHeight = 48.0f;
    d.sdf = false;
    // UI обязан рисовать текст вообще без ассетов на диске: синтезируем шрифт,
    // чьи глифы берутся из встроенной таблицы ячеек ASCII 5x7.
    auto font = textdetail::CreateProceduralFont(d);
    if (!font) {
        ENG_LOGE("font", "FontManager: procedural default font creation failed");
        return nullptr;
    }
    Font* raw = font.get();
    owned_.push_back(std::move(font));
    default_ = raw;
    return raw;
}

Font* FontManager::DefaultSdfFont() {
    if (defaultSdf_) return defaultSdf_;
    FontDesc d;
    d.pixelHeight = 48.0f;
    d.sdf = true;
    d.sdfSpread = 6.0f;
    auto font = textdetail::CreateProceduralFont(d);
    if (!font) return DefaultFont();
    Font* raw = font.get();
    owned_.push_back(std::move(font));
    defaultSdf_ = raw;
    return raw;
}

void FontManager::Clear() {
    default_ = nullptr;
    defaultSdf_ = nullptr;
    cache_.clear();
    owned_.clear();
}

void FontManager::Shutdown() { Clear(); }

namespace {

// ===========================================================================
// 12. Помощники UTF-8
// ===========================================================================

// Возвращает длину в байтах последовательности, начинающейся в `p` (>= 1), и её
// кодовую точку, заменяя некорректный ввод на U+FFFD.
void DecodeOne(const char* s, usize len, usize* i, u32* cpOut) {
    const u8* p = reinterpret_cast<const u8*>(s);
    usize i0 = *i;
    u8 b0 = p[i0];
    auto cont = [&](usize k) -> bool {
        if (i0 + k >= len) return false;
        return (p[i0 + k] & 0xC0) == 0x80;
    };
    if (b0 < 0x80) {
        *cpOut = b0;
        *i = i0 + 1;
        return;
    }
    if ((b0 & 0xE0) == 0xC0) {
        if (cont(1)) {
            u32 cp = (static_cast<u32>(b0 & 0x1F) << 6) | (p[i0 + 1] & 0x3F);
            if (cp >= 0x80) {
                *cpOut = cp;
                *i = i0 + 2;
                return;
            }
        }
        *cpOut = 0xFFFD;
        *i = i0 + 1;
        return;
    }
    if ((b0 & 0xF0) == 0xE0) {
        if (cont(1) && cont(2)) {
            u32 cp = (static_cast<u32>(b0 & 0x0F) << 12) | (static_cast<u32>(p[i0 + 1] & 0x3F) << 6) |
                     (p[i0 + 2] & 0x3F);
            if (cp >= 0x800 && !(cp >= 0xD800 && cp <= 0xDFFF)) {
                *cpOut = cp;
                *i = i0 + 3;
                return;
            }
        }
        *cpOut = 0xFFFD;
        *i = i0 + 1;
        return;
    }
    if ((b0 & 0xF8) == 0xF0) {
        if (cont(1) && cont(2) && cont(3)) {
            u32 cp = (static_cast<u32>(b0 & 0x07) << 18) | (static_cast<u32>(p[i0 + 1] & 0x3F) << 12) |
                     (static_cast<u32>(p[i0 + 2] & 0x3F) << 6) | (p[i0 + 3] & 0x3F);
            if (cp >= 0x10000 && cp <= 0x10FFFF) {
                *cpOut = cp;
                *i = i0 + 4;
                return;
            }
        }
        *cpOut = 0xFFFD;
        *i = i0 + 1;
        return;
    }
    *cpOut = 0xFFFD;
    *i = i0 + 1;
}

}  // namespace

u32 Utf8Decode(const char* s, usize len, usize* i) {
    if (!s || !i) return 0xFFFD;
    if (*i >= len) {
        *i = len;
        return 0;
    }
    u32 cp = 0xFFFD;
    DecodeOne(s, len, i, &cp);
    return cp;
}

std::vector<u32> Utf8ToCodepoints(const std::string& s) {
    std::vector<u32> out;
    out.reserve(s.size());
    usize i = 0;
    const usize n = s.size();
    while (i < n) {
        u32 cp = 0xFFFD;
        DecodeOne(s.data(), n, &i, &cp);
        out.push_back(cp);
    }
    return out;
}

std::string CodepointsToUtf8(const std::vector<u32>& cps) {
    std::string out;
    out.reserve(cps.size() * 2);
    for (u32 cp : cps) {
        u32 c = cp;
        if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) c = 0xFFFD;
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (c >> 18)));
            out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

usize Utf8Length(const std::string& s) {
    usize i = 0, count = 0;
    const usize n = s.size();
    while (i < n) {
        u32 cp = 0;
        DecodeOne(s.data(), n, &i, &cp);
        ++count;
    }
    return count;
}

usize Utf8Offset(const std::string& s, usize index) {
    usize i = 0, count = 0;
    const usize n = s.size();
    while (i < n && count < index) {
        u32 cp = 0;
        DecodeOne(s.data(), n, &i, &cp);
        ++count;
    }
    return i > n ? n : i;
}

}  // namespace crossrender
