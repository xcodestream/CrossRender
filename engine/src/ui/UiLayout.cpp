// Чистая математика раскладки / прокрутки / редактирования текста для UI-модуля.
//
// Всё в этом файле намеренно свободно от GL и UiContext:
// юнит-тесты гоняют его напрямую. Слой виджетов (UiWidgets.cpp) и
// слой контекста (Ui.cpp) используют ровно те же функции, поэтому ошибка,
// исправленная здесь, исправлена и для реального UI.
#include "UiInternal.h"

#include <cmath>
#include <cstring>
#include <algorithm>

namespace crossrender {
namespace ui_internal {

// ---------------------------------------------------------------------------
// FNV-1a
// ---------------------------------------------------------------------------
UiId Fnv1a(const char* str, usize len, int index) {
    // 64-битный FNV-1a по байтам, затем (опциональный) индекс подмешивается
    // простым числом FNV, чтобы `UiHash("x", 0) != UiHash("x", 1)`.
    u64 hash = 1469598103934665603ULL;  // базис FNV
    for (usize i = 0; i < len; ++i) {
        hash ^= static_cast<u8>(str[i]);
        hash *= 1099511628211ULL;  // простое FNV
    }
    hash ^= static_cast<u64>(static_cast<u32>(index));
    hash *= 1099511628211ULL;
    // Никогда не возвращаем 0: это сентинел «нет id».
    return hash == 0 ? 1ULL : hash;
}

// ---------------------------------------------------------------------------
// Решатель flex
// ---------------------------------------------------------------------------
void SolveFlex(const std::vector<LayoutSize>& sizes, const std::vector<f32>& intrinsic, f32 available,
               f32 spacing, std::vector<f32>* out) {
    if (!out) return;
    const usize n = sizes.size();
    out->assign(n, 0.0f);
    if (n == 0) return;

    const f32 gap = spacing > 0.0f ? spacing : 0.0f;
    f32 totalSpacing = gap * static_cast<f32>(n - 1);
    if (totalSpacing < 0.0f) totalSpacing = 0.0f;
    f32 usable = available - totalSpacing;
    if (usable < 0.0f) usable = 0.0f;

    int growCount = 0;
    f32 consumed = 0.0f;
    for (usize i = 0; i < n; ++i) {
        const LayoutSize& s = sizes[i];
        f32 v = 0.0f;
        switch (s.mode) {
            case SizeMode::Fixed: v = s.value; break;
            case SizeMode::Percent: v = s.value * usable; break;
            case SizeMode::Content:
                v = i < intrinsic.size() ? intrinsic[i] : 0.0f;
                break;
            case SizeMode::Grow: ++growCount; continue;
        }
        if (v < 0.0f) v = 0.0f;
        (*out)[i] = v;
        consumed += v;
    }

    f32 leftover = usable - consumed;
    if (leftover < 0.0f) leftover = 0.0f;
    if (growCount > 0) {
        const f32 share = leftover / static_cast<f32>(growCount);
        for (usize i = 0; i < n; ++i)
            if (sizes[i].mode == SizeMode::Grow) (*out)[i] = share;
    }
}

f32 FlexTotal(const std::vector<f32>& solved, f32 spacing) {
    f32 total = 0.0f;
    for (usize i = 0; i < solved.size(); ++i) total += solved[i];
    if (solved.size() > 1 && spacing > 0.0f) total += spacing * static_cast<f32>(solved.size() - 1);
    return total;
}

f32 FlexLeftover(const std::vector<f32>& solved, f32 available, f32 spacing) {
    return available - FlexTotal(solved, spacing);
}

// ---------------------------------------------------------------------------
// Прокрутка
// ---------------------------------------------------------------------------
f32 ClampScroll(f32 offset, f32 content, f32 view) {
    f32 maxScroll = content - view;
    if (maxScroll < 0.0f) maxScroll = 0.0f;
    if (offset < 0.0f) return 0.0f;
    if (offset > maxScroll) return maxScroll;
    // Не возвращаем -0.0f, чтобы не провалить точное сравнение ==.
    return offset == 0.0f ? 0.0f : offset;
}

void StepScroll(f32* offset, f32* velocity, f32 content, f32 view, f32 dt, f32 friction) {
    if (!offset || !velocity) return;
    if (dt <= 0.0f) dt = 1.0f / 60.0f;
    if (*velocity == 0.0f) {
        *offset = ClampScroll(*offset, content, view);
        return;
    }
    *offset += *velocity * dt;
    // Экспоненциальное трение (независимое от частоты кадров).
    *velocity *= std::exp(-(friction > 0.0f ? friction : 4.5f) * dt);
    if (std::fabs(*velocity) < 8.0f) *velocity = 0.0f;

    const f32 clamped = ClampScroll(*offset, content, view);
    if (clamped != *offset) {
        *offset = clamped;
        *velocity = 0.0f;  // упёрлись в край: гасим инерцию
    }
}

// ---------------------------------------------------------------------------
// UTF-8 / редактирование текста
// ---------------------------------------------------------------------------
namespace {

usize SequenceLength(const char* s, usize remaining) {
    const u8 c = static_cast<u8>(s[0]);
    usize n = 1;
    if ((c & 0x80u) == 0x00u) n = 1;
    else if ((c & 0xE0u) == 0xC0u) n = 2;
    else if ((c & 0xF0u) == 0xE0u) n = 3;
    else if ((c & 0xF8u) == 0xF0u) n = 4;
    else n = 1;  // одиночный байт продолжения
    return n > remaining ? remaining : n;
}

u32 DecodeAt(const char* s, usize remaining, usize* consumed) {
    const u8 c = static_cast<u8>(s[0]);
    usize n = SequenceLength(s, remaining);
    *consumed = n;
    if (n == 1) return (c & 0x80u) ? 0xFFFDu : static_cast<u32>(c);
    u32 cp = 0;
    switch (n) {
        case 2: cp = c & 0x1Fu; break;
        case 3: cp = c & 0x0Fu; break;
        default: cp = c & 0x07u; break;
    }
    for (usize i = 1; i < n; ++i) {
        const u8 cc = static_cast<u8>(s[i]);
        if ((cc & 0xC0u) != 0x80u) {
            *consumed = i;
            return 0xFFFDu;
        }
        cp = (cp << 6) | (cc & 0x3Fu);
    }
    return cp;
}

bool IsBreakChar(u32 cp) {
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == ',' || cp == '.' ||
           cp == ';' || cp == ':' || cp == '/' || cp == '\\' || cp == '(' || cp == ')' ||
           cp == '[' || cp == ']' || cp == '{' || cp == '}' || cp == '-' || cp == '_';
}

}  // namespace

void AppendCodepointUtf8(std::string* out, u32 cp) {
    if (!out) return;
    if (cp < 0x80u) {
        out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800u) {
        out->push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out->push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
        out->push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out->push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out->push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out->push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out->push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out->push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out->push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

void InsertUtf8(std::string* out, usize index, const std::string& utf8) {
    if (!out) return;
    if (index > out->size()) index = out->size();
    out->insert(index, utf8);
}

void CaretPositions(const std::string& text, std::vector<usize>* out) {
    if (!out) return;
    out->clear();
    out->reserve(text.size() + 1);
    usize i = 0;
    while (i < text.size()) {
        out->push_back(i);
        usize used = 1;
        DecodeAt(text.data() + i, text.size() - i, &used);
        if (used == 0) used = 1;
        i += used;
    }
    out->push_back(text.size());
}

usize ClampCaret(const std::string& text, usize index) {
    if (index > text.size()) index = text.size();
    // Сдвигаем индекс внутри последовательности назад к её началу.
    while (index > 0 && index < text.size() &&
           (static_cast<u8>(text[index]) & 0xC0u) == 0x80u)
        --index;
    return index;
}

usize CaretIndexFromWidth(const std::string& text, f32 x,
                          const std::function<f32(const std::string&)>& widthFn) {
    if (!widthFn || text.empty() || x <= 0.0f) return 0;
    if (x >= widthFn(text)) return text.size();

    // Позиции каретки — смещения в байтах UTF-8 на каждой границе кодового
    // символа плюс конец строки. Их префиксные ширины монотонно не убывают,
    // поэтому ответ — upper_bound по этой последовательности. Префикс
    // измеряется один раз на позицию (O(n^2) байтов, но без повторных
    // проходов), а сам поиск — O(log n).
    const usize n = Utf8Length(text) + 1;
    usize lo = 0, hi = n;  // инвариант: префиксная ширина в lo <= x < префиксная ширина в hi
    while (hi - lo > 1) {
        const usize mid = lo + (hi - lo) / 2;
        const usize off = Utf8Offset(text, mid);
        if (widthFn(text.substr(0, off)) <= x)
            lo = mid;
        else
            hi = mid;
    }
    return Utf8Offset(text, lo);
}

usize CaretIndexFromClick(const std::string& text, f32 x,
                          const std::function<f32(const std::string&)>& widthFn) {
    const usize idx = CaretIndexFromWidth(text, x, widthFn);
    if (idx == 0 || idx >= text.size() || !widthFn) return idx;
    // Сдвигаем вперёд, если клик пришёлся на правую половину глифа,
    // начинающегося в `idx`.
    const f32 before = widthFn(text.substr(0, idx));
    const usize next = CaretNext(text, idx);
    const f32 after = widthFn(text.substr(0, next));
    const f32 mid = before + (after - before) * 0.5f;
    return x >= mid ? next : idx;
}

usize CaretPrev(const std::string& text, usize index) {
    index = ClampCaret(text, index);
    if (index == 0) return 0;
    usize i = index - 1;
    while (i > 0 && (static_cast<u8>(text[i]) & 0xC0u) == 0x80u) --i;
    return i;
}

usize CaretNext(const std::string& text, usize index) {
    index = ClampCaret(text, index);
    if (index >= text.size()) return text.size();
    usize used = 1;
    DecodeAt(text.data() + index, text.size() - index, &used);
    if (used == 0) used = 1;
    return index + used;
}

usize LineStart(const std::string& text, usize index) {
    index = ClampCaret(text, index);
    while (index > 0 && text[index - 1] != '\n') --index;
    return index;
}

usize LineEnd(const std::string& text, usize index) {
    index = ClampCaret(text, index);
    while (index < text.size() && text[index] != '\n') ++index;
    return index;
}

usize WordLeft(const std::string& text, usize index) {
    index = ClampCaret(text, index);
    // Пропускаем разделители, затем само слово. Переводы строк — жёсткая остановка.
    while (index > 0) {
        const usize prev = CaretPrev(text, index);
        usize used = 1;
        const u32 cp = DecodeAt(text.data() + prev, index - prev, &used);
        if (cp == '\n' || !IsBreakChar(cp)) break;
        index = prev;
    }
    while (index > 0) {
        const usize prev = CaretPrev(text, index);
        usize used = 1;
        const u32 cp = DecodeAt(text.data() + prev, index - prev, &used);
        if (cp == '\n' || IsBreakChar(cp)) break;
        index = prev;
    }
    return index;
}

usize WordRight(const std::string& text, usize index) {
    index = ClampCaret(text, index);
    while (index < text.size()) {
        usize used = 1;
        const u32 cp = DecodeAt(text.data() + index, text.size() - index, &used);
        if (cp == '\n' || IsBreakChar(cp)) break;
        index += used;
    }
    while (index < text.size()) {
        usize used = 1;
        const u32 cp = DecodeAt(text.data() + index, text.size() - index, &used);
        if (cp == '\n' || !IsBreakChar(cp)) break;
        index += used;
    }
    return index;
}

usize LineUp(const std::string& text, usize index, const std::function<f32(f32)>& xToCaret) {
    index = ClampCaret(text, index);
    const usize start = LineStart(text, index);
    if (start == 0) return index;
    const f32 column = xToCaret(static_cast<f32>(index - start));
    const usize prevLineEnd = start - 1;  // the '\n'
    const usize prevLineStart = LineStart(text, prevLineEnd);
    const f32 lineLen = static_cast<f32>(prevLineEnd - prevLineStart);
    const f32 clamped = column < 0 ? 0.0f : (column > lineLen ? lineLen : column);
    return LineStart(text, prevLineStart) + static_cast<usize>(clamped);
}

usize LineDown(const std::string& text, usize index, const std::function<f32(f32)>& xToCaret) {
    index = ClampCaret(text, index);
    const usize start = LineStart(text, index);
    const usize end = LineEnd(text, index);
    if (end >= text.size()) return index;
    const f32 column = xToCaret(static_cast<f32>(index - start));
    const usize nextLineStart = end + 1;
    const usize nextLineEnd = LineEnd(text, nextLineStart);
    const f32 lineLen = static_cast<f32>(nextLineEnd - nextLineStart);
    const f32 clamped = column < 0 ? 0.0f : (column > lineLen ? lineLen : column);
    return nextLineStart + static_cast<usize>(clamped);
}

void NormalizeSelection(int selectionStart, int selectionEnd, usize* first, usize* last) {
    if (selectionStart < 0 || selectionEnd < 0) {
        if (first) *first = 0;
        if (last) *last = 0;
        return;
    }
    usize a = static_cast<usize>(selectionStart);
    usize b = static_cast<usize>(selectionEnd);
    if (a > b) std::swap(a, b);
    if (first) *first = a;
    if (last) *last = b;
}

usize BackspaceAt(std::string* text, usize index) {
    if (!text) return 0;
    const usize at = ClampCaret(*text, index);
    if (at == 0) return 0;
    const usize prev = CaretPrev(*text, at);
    text->erase(prev, at - prev);
    return prev;
}

usize DeleteAt(std::string* text, usize index) {
    if (!text) return 0;
    const usize at = ClampCaret(*text, index);
    if (at >= text->size()) return at;
    const usize next = CaretNext(*text, at);
    text->erase(at, next - at);
    return at;
}

// ---------------------------------------------------------------------------
// Процедурная 9-patch графика
// ---------------------------------------------------------------------------
f32 RoundedRectCoverage(f32 px, f32 py, f32 halfW, f32 halfH, f32 radius) {
    const f32 hw = halfW - radius;
    const f32 hh = halfH - radius;
    const f32 qx = std::fabs(px) - hw;
    const f32 qy = std::fabs(py) - hh;
    const f32 mx = qx > 0.0f ? qx : 0.0f;
    const f32 my = qy > 0.0f ? qy : 0.0f;
    f32 d = std::sqrt(mx * mx + my * my) + std::min(std::max(qx, qy), 0.0f) - radius;
    if (d <= -0.5f) return 1.0f;
    if (d >= 0.5f) return 0.0f;
    return 0.5f - d;
}

void GenerateRoundedRectPixels(int size, f32 radius, const Color& fill, const Color& border,
                               f32 borderWidth, std::vector<u8>* outRgba) {
    if (!outRgba) return;
    outRgba->clear();
    if (size <= 0) return;
    if (borderWidth < 0) borderWidth = 0;
    const f32 half = static_cast<f32>(size) * 0.5f;
    const f32 maxRadius = half - 0.5f;
    if (radius > maxRadius) radius = maxRadius;
    if (radius < 0) radius = 0;

    outRgba->resize(static_cast<usize>(size) * static_cast<usize>(size) * 4u);
    auto* dst = outRgba->data();
    for (int y = 0; y < size; ++y) {
        const f32 py = static_cast<f32>(y) + 0.5f - half;
        for (int x = 0; x < size; ++x) {
            const f32 px = static_cast<f32>(x) + 0.5f - half;
            const f32 outer = RoundedRectCoverage(px, py, half, half, radius);
            f32 r = fill.r, g = fill.g, b = fill.b, a = fill.a;
            if (borderWidth > 0.0f && border.a > 0.0f) {
                const f32 innerHalf = half - borderWidth;
                const f32 innerR = radius - borderWidth > 0.0f ? radius - borderWidth : 0.0f;
                const f32 inner = innerHalf > 0.0f ? RoundedRectCoverage(px, py, innerHalf, innerHalf, innerR)
                                                   : 0.0f;
                r = border.r + (fill.r - border.r) * inner;
                g = border.g + (fill.g - border.g) * inner;
                b = border.b + (fill.b - border.b) * inner;
                a = border.a + (fill.a - border.a) * inner;
            }
            // Сглаживаем только внешний силуэт, чтобы прямые края и центр
            // оставались идеально непрозрачными (это делает текстуру корректным
            // 9-patch: центральный тексель — сплошная заливка).
            a *= outer;
            u8* p = dst + (static_cast<usize>(y) * static_cast<usize>(size) + static_cast<usize>(x)) * 4u;
            p[0] = static_cast<u8>(Clamp(r, 0.0f, 1.0f) * 255.0f + 0.5f);
            p[1] = static_cast<u8>(Clamp(g, 0.0f, 1.0f) * 255.0f + 0.5f);
            p[2] = static_cast<u8>(Clamp(b, 0.0f, 1.0f) * 255.0f + 0.5f);
            p[3] = static_cast<u8>(Clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }
}

}  // namespace ui_internal
}  // namespace crossrender
