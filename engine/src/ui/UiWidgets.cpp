// Immediate-mode UI - набор виджетов.
//
// Каждый виджет следует одному и тому же immediate-mode контракту:
//   * регистрирует себя (id + rect) для навигации наведения/фокуса,
//   * `interact()` крутит автомат наведение/нажатие/отпускание, поэтому виджет
//     срабатывает, только если отпускание случилось внутри него (увод в сторону отменяет),
//   * визуал интерполируется со скоростью анимации темы (экспоненциальное
//     сглаживание), поэтому движение наведения/нажатия/выбора не зависит от частоты кадров,
//   * отрисовка обрезается по rect виджета (и его контейнера).
//
// Виджеты без 9-patch графики откатываются к процедурному виду: скруглённый
// прямоугольник, вертикальный градиент, рамка 1px и мягкая тень.
#include "crossrender/ui/Ui.h"

#include "crossrender/core/Log.h"
#include "crossrender/text/Font.h"
#include "crossrender/platform/Window.h"

#include "UiInternal.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include <cstring>
#include <algorithm>

namespace crossrender {

using ui_internal::LayoutContainer;
using ui_internal::PopupState;
using ui_internal::UiImpl;
using ui_internal::WidgetStateEx;

namespace {

// ---------------------------------------------------------------------------
// Малые помощники
// ---------------------------------------------------------------------------
f32 ExpSmooth(f32 current, f32 target, f32 speed, f32 dt) {
    if (speed <= 0.0f) return target;
    return current + (target - current) * (1.0f - std::exp(-speed * dt));
}

Color WithA(const Color& c, f32 a) { return {c.r, c.g, c.b, a}; }

Rect Clipped(Renderer2D& r, const Rect& rect) {
    const Rect clip = r.CurrentClip();
    return clip.w <= 0.0f && clip.h <= 0.0f ? rect : rect.Intersect(clip);
}

void FillRound(Renderer2D& r, const Rect& rect, f32 radius, const Color& c) {
    if (c.a <= 0.0f || rect.w <= 0.0f || rect.h <= 0.0f) return;
    const Rect v = Clipped(r, rect);
    if (v.w <= 0.0f || v.h <= 0.0f) return;
    r.FillRoundedRect(v, radius, c);
}

void StrokeRound(Renderer2D& r, const Rect& rect, f32 radius, const Color& c, f32 width) {
    if (c.a <= 0.0f || rect.w <= 0.0f || rect.h <= 0.0f) return;
    const Rect v = Clipped(r, rect);
    if (v.w <= 0.0f || v.h <= 0.0f) return;
    r.StrokeRoundedRect(v, radius, c, width);
}

void FillRectC(Renderer2D& r, const Rect& rect, const Color& c) {
    if (c.a <= 0.0f || rect.w <= 0.0f || rect.h <= 0.0f) return;
    const Rect v = Clipped(r, rect);
    if (v.w <= 0.0f || v.h <= 0.0f) return;
    r.FillRect(v, c);
}

void FillGrad(Renderer2D& r, const Rect& rect, const Color& top, const Color& bottom) {
    if (rect.w <= 0.0f || rect.h <= 0.0f) return;
    const Rect v = Clipped(r, rect);
    if (v.w <= 0.0f || v.h <= 0.0f) return;
    r.FillRectGradient(v, top, bottom, true);
}

void TextClipped(Renderer2D& r, const Font& font, const std::string& s, f32 x, f32 y, const Color& c,
                 f32 size, TextAlign align, TextBaseline baseline) {
    if (c.a <= 0.0f || s.empty()) return;
    r.DrawText(font, s, x, y, c, size, align, baseline, 0.0f);
}


// Разделение метки/id в стиле ImGui: "Text##id" отображает "Text", а хеширует
// всю строку, поэтому вызывающий может передать "##id" для виджета, который рисует
// целиком вручную (метка тогда рендерится пустой — на это опираются карточки
// и табы SceneMenu). MakeId() намеренно сохраняет всю строку, чтобы id оставались уникальными.
std::string DisplayLabel(const char* label) {
    if (!label) return std::string();
    const char* hash = std::strstr(label, "##");
    if (!hash) return std::string(label);
    return std::string(label, static_cast<usize>(hash - label));
}

// ---------------------------------------------------------------------------
// Визуал кнопок (процедурный и 9-patch)
// ---------------------------------------------------------------------------
struct ButtonLook {
    Color fillTop;
    Color fillBottom;
    Color border;
    Color text;
};

void DrawButtonArt(Renderer2D& r, const UiTheme& th, const Rect& rect, f32 hover, f32 press,
                   bool enabled, const UiTheme::NinePatchStyle* art) {
    if (art && art->texture && art->texture->Valid()) {
        Color tint = Color::White;
        if (!enabled) tint = Color{0.55f, 0.55f, 0.55f, 0.65f};
        r.Image9(*art->texture, rect, art->patch, tint, art->scale);
        return;
    }

    const f32 radius = th.rounding;
    const f32 lift = 1.0f - press;
    const Color base = enabled ? th.panelAlt : Color{th.panelAlt.r, th.panelAlt.g, th.panelAlt.b, th.panelAlt.a};
    const Color accent = Lerp(th.accent, th.accentHover, hover);

    // Тень: растёт при наведении, схлопывается при нажатии.
    const f32 shadow = th.shadowSize * (0.5f + hover * 0.5f) * (1.0f - press * 0.7f);
    if (enabled && shadow > 0.5f) {
        const Color s = th.shadow;
        const f32 spread = shadow * 0.5f;
        FillRound(r, {rect.x - spread, rect.y + 1.0f + spread * 0.5f, rect.w + spread * 2.0f,
                      rect.h + spread},
                  radius + spread, Color{s.r, s.g, s.b, s.a * 0.55f * (0.4f + 0.6f * hover)});
    }

    // Тело: вертикальный градиент, светлее сверху, подкрашенный к акценту при
    // наведении; нажатие затемняет и слегка опускает градиент.
    Color top = Lerp(base, accent, 0.10f + hover * 0.22f);
    Color bottom = Lerp(base, accent, 0.02f + hover * 0.07f);
    top = Lerp(top, th.accentActive, press * 0.45f);
    bottom = Lerp(bottom, th.accentActive, press * 0.30f);
    if (!enabled) {
        top = Color{base.r, base.g, base.b, 0.55f};
        bottom = top;
    }
    const Rect body{rect.x, rect.y + (1.0f - lift) * 1.0f, rect.w, rect.h - (1.0f - lift)};
    FillGrad(r, body, top, bottom);

    // Рамка 1px, ярче при наведении / фокусе.
    Color border = Lerp(th.border, accent, hover * 0.9f);
    border = Lerp(border, th.accentHover, press * 0.4f);
    if (!enabled) border = Color{th.border.r, th.border.g, th.border.b, 0.35f};
    StrokeRound(r, body, radius, border, th.borderWidth);

    // Верхняя линия подсветки для «стеклянного» вида.
    if (enabled && rect.h > 6.0f) {
        const Color hi{1.0f, 1.0f, 1.0f, 0.06f + hover * 0.06f};
        FillRound(r, {body.x + radius * 0.5f, body.y + 1.0f, body.w - radius, 1.0f}, 0.5f, hi);
    }
}

// ---------------------------------------------------------------------------
// Отметки checkbox / radio
// ---------------------------------------------------------------------------
void DrawCheckMark(Renderer2D& r, const Rect& box, f32 t, const Color& color) {
    if (t <= 0.01f) return;
    r.Save();
    r.BeginPath();
    // Галочка из двух сегментов, рисуется постепенно по t.
    const Vec2 a{box.x + box.w * 0.24f, box.y + box.h * 0.52f};
    const Vec2 b{box.x + box.w * 0.43f, box.y + box.h * 0.71f};
    const Vec2 c{box.x + box.w * 0.77f, box.y + box.h * 0.30f};
    const f32 seg1 = Clamp(t * 2.0f, 0.0f, 1.0f);
    const f32 seg2 = Clamp(t * 2.0f - 1.0f, 0.0f, 1.0f);
    r.MoveTo(a.x, a.y);
    r.LineTo(Lerp(a.x, b.x, seg1), Lerp(a.y, b.y, seg1));
    if (seg2 > 0.0f) r.LineTo(Lerp(b.x, c.x, seg2), Lerp(b.y, c.y, seg2));
    r.LineCap(LineCap::Round);
    r.LineJoin(LineJoin::Round);
    r.StrokeWidth(2.0f);
    r.StrokeColor(color);
    r.Stroke();
    r.Restore();
}

// ---------------------------------------------------------------------------
// Ядро редактирования текста, общее для TextField и TextArea
// ---------------------------------------------------------------------------
struct EditFont {
    const Font* font = nullptr;
    f32 size = 16.0f;
};

f32 Measure(const EditFont& f, const std::string& s) {
    if (!f.font) return 0.0f;
    return MeasureText(*f.font, s, f.size).width;
}

u32 DecodeOne(const std::string& s, usize at, usize* len) {
    if (at >= s.size()) {
        *len = 0;
        return 0;
    }
    const u8 c = static_cast<u8>(s[at]);
    usize n = 1;
    if ((c & 0x80u) == 0x00u) n = 1;
    else if ((c & 0xE0u) == 0xC0u) n = 2;
    else if ((c & 0xF0u) == 0xE0u) n = 3;
    else if ((c & 0xF8u) == 0xF0u) n = 4;
    if (at + n > s.size()) n = 1;
    *len = n;
    if (n == 1) return c;
    u32 cp = 0;
    switch (n) {
        case 2: cp = c & 0x1Fu; break;
        case 3: cp = c & 0x0Fu; break;
        default: cp = c & 0x07u; break;
    }
    for (usize i = 1; i < n; ++i) cp = (cp << 6) | (static_cast<u8>(s[at + i]) & 0x3Fu);
    return cp;
}

std::string Masked(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    usize i = 0;
    while (i < s.size()) {
        usize n = 1;
        const u32 cp = DecodeOne(s, i, &n);
        if (cp == '\n')
            out.push_back('\n');
        else
            ui_internal::AppendCodepointUtf8(&out, 0x2022u);  // U+2022 bullet
        i += n;
    }
    return out;
}

usize CountLines(const std::string& s) {
    usize lines = 1;
    for (char c : s)
        if (c == '\n') ++lines;
    return lines;
}

void ResetBlink(WidgetState& ws) { ws.caretBlink = 0.0f; }

// Применяет покадровое мигание каретки. Возвращает true, когда каретка видна.
bool CaretVisible(UiContext& ctx, WidgetState& ws) {
    ws.caretBlink += ctx.DeltaTime();
    return std::fmod(ws.caretBlink, 1.06f) < 0.53f;
}

// Общий буфер обмена (без Windows/GLFW, работает headless).
std::string g_clipboard;

// Диапазон выделения состояния виджета (первый,последний) в смещениях байтов.
void SelRange(const WidgetState& ws, usize* first, usize* last) {
    ui_internal::NormalizeSelection(ws.selectionStart, ws.selectionEnd, first, last);
}

void DeleteSelection(std::string* text, WidgetState& ws) {
    usize a = 0, b = 0;
    SelRange(ws, &a, &b);
    if (a == b || b > text->size()) {
        ws.selectionStart = ws.selectionEnd = -1;
        return;
    }
    text->erase(a, b - a);
    ws.caretIndex = static_cast<int>(a);
    ws.selectionStart = ws.selectionEnd = -1;
}

// Прогоняет всё взаимодействие редактирования текста за один кадр. Возвращает
// true, когда текст изменился, и выставляет *submitted при нажатии Enter.
bool EditText(UiContext& ctx, UiId id, const Rect& rect, std::string* text, bool multiline,
              bool password, const char* placeholder, bool* submitted) {
    UiImpl& st = ui_internal::CtxState(ctx);
    UiTheme& th = ctx.Theme();
    const Input& in = *st.input;
    WidgetState& ws = ctx.State(id);
    const EditFont f{th.font, th.textSize};
    const f32 lineH = th.textSize * 1.3f;
    const f32 padX = 8.0f;
    const f32 innerW = rect.w - padX * 2.0f;
    const f32 innerH = rect.h - 6.0f;

    if (submitted) *submitted = false;
    bool changed = false;
    bool focused = (st.focusedId == id);

    // ---- фокус / позиция каретки -----------------------------------------
    if (st.input && !st.inputBlocked && in.MousePressed() && rect.Contains(in.MousePos())) {
        st.focusedId = id;
        st.activeId = id;
        focused = true;
        ctx.SetFocus(id);
    }

    if (focused) {
        st.editingId = id;
        // Редактор владеет стрелками / Tab, пока у него фокус.
        st.keyboardNavCaptured = true;
        ws.focused = true;
    }

    const bool canEdit = focused && !st.inputBlocked;

    // ---- мышь: каретка + выделение ---------------------------------------
    if (canEdit) {
        const bool shift = in.ShiftDown();
        if (in.MousePressed() && rect.Contains(in.MousePos())) {
            const f32 localX = in.MousePos().x - rect.x - padX + ws.scrollX;
            const f32 localY = in.MousePos().y - rect.y - 3.0f + ws.scrollY;
            usize caret = 0;
            if (multiline) {
                const usize line = static_cast<usize>(Clamp(std::floor(localY / lineH), 0.0f,
                                                            static_cast<f32>(CountLines(*text) - 1)));
                usize start = 0, idx = 0;
                for (usize l = 0; l < line && idx < text->size(); ++l) {
                    while (idx < text->size() && (*text)[idx] != '\n') ++idx;
                    if (idx < text->size()) ++idx;
                    start = idx;
                }
                const usize end = ui_internal::LineEnd(*text, start);
                const std::string row = text->substr(start, end - start);
                caret = start + ui_internal::CaretIndexFromClick(row, localX, [&](const std::string& s) {
                            return Measure(f, password ? Masked(s) : s);
                        });
            } else {
                caret = ui_internal::CaretIndexFromClick(*text, localX, [&](const std::string& s) {
                    return Measure(f, password ? Masked(s) : s);
                });
            }
            if (shift) {
                if (ws.selectionStart < 0) ws.selectionStart = ws.caretIndex;
                ws.selectionEnd = static_cast<int>(caret);
            } else {
                ws.selectionStart = ws.selectionEnd = -1;
            }
            ws.caretIndex = static_cast<int>(caret);
            ResetBlink(ws);
        }
        if (in.MouseDown() && rect.Contains(in.MousePos()) && st.activeId == id) {
            // Выделение перетаскиванием.
            const f32 localX = in.MousePos().x - rect.x - padX + ws.scrollX;
            const usize caret = ui_internal::CaretIndexFromClick(*text, localX, [&](const std::string& s) {
                return Measure(f, password ? Masked(s) : s);
            });
            if (static_cast<int>(caret) != ws.caretIndex) {
                if (ws.selectionStart < 0) ws.selectionStart = ws.caretIndex;
                ws.selectionEnd = static_cast<int>(caret);
                ws.caretIndex = static_cast<int>(caret);
                ResetBlink(ws);
            }
        }
    }

    // ---- клавиатура -------------------------------------------------------
    if (canEdit) {
        const bool shift = in.ShiftDown();
        const bool ctrl = in.CtrlDown() || in.SuperDown();
        usize caret = ui_internal::ClampCaret(*text, static_cast<usize>(ws.caretIndex < 0 ? 0 : ws.caretIndex));
        ws.caretIndex = static_cast<int>(caret);

        auto moveCaret = [&](usize to) {
            if (shift) {
                if (ws.selectionStart < 0) ws.selectionStart = static_cast<int>(caret);
                ws.selectionEnd = static_cast<int>(to);
            } else {
                ws.selectionStart = ws.selectionEnd = -1;
            }
            ws.caretIndex = static_cast<int>(to);
            ResetBlink(ws);
        };

        if (ctrl && in.KeyPressed(Key::A)) {
            ws.selectionStart = 0;
            ws.selectionEnd = static_cast<int>(text->size());
            ws.caretIndex = static_cast<int>(text->size());
        } else if (ctrl && in.KeyPressed(Key::C)) {
            usize a = 0, b = 0;
            SelRange(ws, &a, &b);
            if (b > a && b <= text->size()) g_clipboard = text->substr(a, b - a);
        } else if (ctrl && in.KeyPressed(Key::X)) {
            usize a = 0, b = 0;
            SelRange(ws, &a, &b);
            if (b > a && b <= text->size()) {
                g_clipboard = text->substr(a, b - a);
                DeleteSelection(text, ws);
                changed = true;
            }
        } else if (ctrl && in.KeyPressed(Key::V)) {
            if (!g_clipboard.empty()) {
                DeleteSelection(text, ws);
                const usize at = ui_internal::ClampCaret(*text, static_cast<usize>(ws.caretIndex));
                // Однострочные поля сплющивают вставленные переводы строк.
                std::string paste = g_clipboard;
                if (!multiline) {
                    for (char& c : paste)
                        if (c == '\n' || c == '\r') c = ' ';
                }
                ui_internal::InsertUtf8(text, at, paste);
                ws.caretIndex = static_cast<int>(at + paste.size());
                ws.selectionStart = ws.selectionEnd = -1;
                ResetBlink(ws);
                changed = true;
            }
        } else {
            // Навигация.
            if (in.KeyPressed(Key::Left)) {
                if (ctrl) moveCaret(ui_internal::WordLeft(*text, caret));
                else if (shift) moveCaret(ui_internal::CaretPrev(*text, caret));
                else {
                    usize a = 0, b = 0;
                    SelRange(ws, &a, &b);
                    const bool hadSel = b > a;
                    moveCaret(hadSel ? a : ui_internal::CaretPrev(*text, caret));
                }
            }
            if (in.KeyPressed(Key::Right)) {
                if (ctrl) moveCaret(ui_internal::WordRight(*text, caret));
                else if (shift) moveCaret(ui_internal::CaretNext(*text, caret));
                else {
                    usize a = 0, b = 0;
                    SelRange(ws, &a, &b);
                    const bool hadSel = b > a;
                    moveCaret(hadSel ? b : ui_internal::CaretNext(*text, caret));
                }
            }
            if (in.KeyPressed(Key::Up) && multiline) moveCaret(ui_internal::LineUp(*text, caret, [&](f32 c) {
                return c;
            }));
            if (in.KeyPressed(Key::Down) && multiline) moveCaret(ui_internal::LineDown(*text, caret, [&](f32 c) {
                return c;
            }));
            if (in.KeyPressed(Key::Home)) moveCaret(ui_internal::LineStart(*text, caret));
            if (in.KeyPressed(Key::End)) moveCaret(ui_internal::LineEnd(*text, caret));
            if (in.KeyPressed(Key::Backspace)) {
                usize a = 0, b = 0;
                SelRange(ws, &a, &b);
                if (b > a) {
                    DeleteSelection(text, ws);
                } else {
                    ws.caretIndex = static_cast<int>(ui_internal::BackspaceAt(text, caret));
                }
                ResetBlink(ws);
                changed = true;
            }
            if (in.KeyPressed(Key::Delete)) {
                usize a = 0, b = 0;
                SelRange(ws, &a, &b);
                if (b > a)
                    DeleteSelection(text, ws);
                else
                    ui_internal::DeleteAt(text, caret);
                changed = true;
            }
            if (in.KeyPressed(Key::Enter)) {
                if (multiline) {
                    DeleteSelection(text, ws);
                    const usize at = ui_internal::ClampCaret(*text, static_cast<usize>(ws.caretIndex));
                    ui_internal::InsertUtf8(text, at, "\n");
                    ws.caretIndex = static_cast<int>(at + 1);
                    ws.selectionStart = ws.selectionEnd = -1;
                    changed = true;
                } else {
                    if (submitted) *submitted = true;
                    st.focusedId = kUiIdNone;
                    ctx.ClearFocus();
                    st.pendingFocus = kUiIdNone;
                    st.requestFocusChange = true;
                }
                ResetBlink(ws);
            }
        }

        // IME / платформенный текстовый ввод (кодовые точки UTF-32 -> UTF-8).
        if (changed == false) {
            for (u32 cp : in.TextInput()) {
                if (cp < 0x20u && cp != '\t') continue;
                if (cp == '\t' && multiline) continue;
                if (cp == '\t') continue;
                DeleteSelection(text, ws);
                const usize at = ui_internal::ClampCaret(*text, static_cast<usize>(ws.caretIndex));
                std::string tmp;
                ui_internal::AppendCodepointUtf8(&tmp, cp);
                ui_internal::InsertUtf8(text, at, tmp);
                ws.caretIndex = static_cast<int>(at + tmp.size());
                ws.selectionStart = ws.selectionEnd = -1;
                ResetBlink(ws);
                changed = true;
            }
        }

        // После правок держим каретку на границе кодового символа.
        ws.caretIndex = static_cast<int>(
            ui_internal::ClampCaret(*text, static_cast<usize>(ws.caretIndex < 0 ? 0 : ws.caretIndex)));
    }

    // ---- прокрутка --------------------------------------------------------
    const usize caret = ui_internal::ClampCaret(*text, static_cast<usize>(ws.caretIndex < 0 ? 0 : ws.caretIndex));
    const std::string drawText = password ? Masked(*text) : *text;
    const f32 caretX = Measure(f, drawText.substr(0, caret));
    if (!multiline) {
        if (caretX - ws.scrollX > innerW) ws.scrollX = caretX - innerW;
        if (caretX - ws.scrollX < 0.0f) ws.scrollX = caretX;
        const f32 maxScroll = Measure(f, drawText) - innerW;
        ws.scrollX = ui_internal::ClampScroll(ws.scrollX, maxScroll > 0 ? maxScroll + innerW : 0.0f, innerW);
    } else {
        // Вертикально: считаем строки до каретки.
        f32 caretY = 0.0f;
        for (usize i = 0; i < caret; ++i)
            if ((*text)[i] == '\n') caretY += lineH;
        if (caretY + lineH - ws.scrollY > innerH) ws.scrollY = caretY + lineH - innerH;
        if (caretY - ws.scrollY < 0.0f) ws.scrollY = caretY;
        const f32 maxScroll = static_cast<f32>(CountLines(*text)) * lineH - innerH;
        ws.scrollY = ui_internal::ClampScroll(ws.scrollY, maxScroll > 0 ? maxScroll + innerH : 0.0f, innerH);
    }

    // ---- отрисовка --------------------------------------------------------
    st.r2d->Save();
    st.r2d->ClipRect(rect.x, rect.y, rect.w, rect.h);
    Renderer2D& r = *st.r2d;

    const bool hovered = rect.Contains(in.MousePos()) && !st.inputBlocked;
    const Color border = focused ? th.accent : (hovered ? Lerp(th.border, th.accent, 0.5f) : th.border);
    FillRound(r, rect, th.rounding, th.bg);
    StrokeRound(r, rect, th.rounding, border, focused ? th.borderWidth + 1.0f : th.borderWidth);

    const f32 textY = multiline ? rect.y + 3.0f - ws.scrollY : rect.Center().y;
    const f32 textX = rect.x + padX - (multiline ? 0.0f : ws.scrollX);

    // Сначала подсветка выделения (за глифами).
    usize sa = 0, sb = 0;
    SelRange(ws, &sa, &sb);
    const bool hasSel = sb > sa && sb <= text->size() && focused;
    if (hasSel) {
        if (multiline) {
            f32 y = rect.y + 3.0f - ws.scrollY;
            usize i = 0;
            while (i < text->size()) {
                const usize lineEnd = ui_internal::LineEnd(*text, i);
                const usize from = std::max(i, sa);
                const usize to = std::min(lineEnd, sb);
                if (to > from) {
                    const f32 x0 = Measure(f, drawText.substr(i, from - i));
                    const f32 x1 = Measure(f, drawText.substr(i, to - i));
                    FillRectC(r, {textX + x0, y, x1 - x0, lineH}, th.selection);
                }
                if (sb <= lineEnd) break;
                i = lineEnd + 1;
                y += lineH;
            }
        } else {
            const f32 x0 = Measure(f, drawText.substr(0, sa));
            const f32 x1 = Measure(f, drawText.substr(0, sb));
            FillRectC(r, {textX + x0, rect.y + 3.0f, x1 - x0, rect.h - 6.0f}, th.selection);
        }
    }

    if (!drawText.empty()) {
        if (multiline) {
            // Рисуем построчно, чтобы работала вертикальная прокрутка.
            f32 y = textY;
            usize i = 0;
            int lines = 0;
            while (i <= drawText.size() && lines < 512) {
                const usize end = ui_internal::LineEnd(drawText, i);
                if (end > i && y + lineH > rect.y && y < rect.Bottom())
                    TextClipped(r, *f.font, drawText.substr(i, end - i), textX, y, th.text, th.textSize,
                                TextAlign::Left, TextBaseline::Top);
                if (end >= drawText.size()) break;
                i = end + 1;
                y += lineH;
                ++lines;
            }
        } else {
            TextClipped(r, *f.font, drawText, textX, textY, th.text, th.textSize, TextAlign::Left,
                        TextBaseline::Middle);
        }
    } else if (placeholder && focused == false && placeholder[0]) {
        TextClipped(r, *f.font, placeholder, rect.x + padX, textY, th.textDisabled, th.textSize,
                    TextAlign::Left, multiline ? TextBaseline::Top : TextBaseline::Middle);
    }

    // Каретка.
    if (focused && CaretVisible(ctx, ws) && st.editingId == id) {
        if (multiline) {
            f32 y = rect.y + 3.0f - ws.scrollY;
            usize i = 0;
            f32 x = 0.0f;
            usize upto = 0;
            while (upto < caret) {
                const usize end = ui_internal::LineEnd(drawText, i);
                if (caret <= end) break;
                i = end + 1;
                upto = i;
                y += lineH;
            }
            x = Measure(f, drawText.substr(i, caret - i));
            FillRectC(r, {textX + x, y + 1.0f, 1.5f, lineH - 2.0f}, th.accent);
        } else {
            FillRectC(r, {textX + caretX, rect.y + 5.0f, 1.5f, rect.h - 10.0f}, th.accent);
        }
    }

    st.r2d->Restore();
    ws.hovered = hovered;
    return changed;
}

}  // namespace

// ---------------------------------------------------------------------------
// Кнопки
// ---------------------------------------------------------------------------
bool UiContext::Button(const char* label, const Rect& rect, bool enabled, const char* tooltip) {
    const bool clicked = ButtonStyled(label, rect, theme_.buttonNormal, enabled);
    if (tooltip && *tooltip) {
        // Показываем, только пока указатель над кнопкой.
        UiImpl& st = ui_internal::CtxState(*this);
        if (st.input && rect.Contains(st.input->MousePos())) SetTooltip(tooltip);
    }
    return clicked;
}

bool UiContext::InvisibleButton(const char* id, const Rect& rect, bool enabled) {
    UiImpl& st = ui_internal::CtxState(*this);
    const UiId wid = MakeId(id && *id ? id : "invisible", 10);
    RegisterWidget(wid, rect, true);
    WidgetStateEx& ws = static_cast<WidgetStateEx&>(State(wid));

    const bool clicked = st.interact(wid, rect, enabled);
    if (clicked) PlayClickSound();
    if (ws.hovered && ws.hoverAnim < 0.05f) PlayHoverSound();

    const bool forced = (st.activateId == wid);
    if (forced && enabled) {
        st.activateId = kUiIdNone;
        return true;
    }
    return clicked;
}

bool UiContext::ButtonStyled(const char* label, const Rect& rect, const UiTheme::NinePatchStyle& style,
                             bool enabled) {
    UiImpl& st = ui_internal::CtxState(*this);
    Renderer2D& r = *st.r2d;
    const UiId id = MakeId(label ? label : "button", 10);
    RegisterWidget(id, rect, true);
    WidgetStateEx& ws = static_cast<WidgetStateEx&>(State(id));

    const bool clicked = st.interact(id, rect, enabled);
    if (clicked) PlayClickSound();
    if (ws.hovered && ws.hoverAnim < 0.05f) PlayHoverSound();

    const bool forced = (st.activateId == id);
    if (forced && enabled) {
        st.activateId = kUiIdNone;
        return true;
    }

    const UiTheme::NinePatchStyle* art = nullptr;
    if (style.texture) art = (ws.active && enabled)    ? &theme_.buttonPressed
                             : (ws.hovered && enabled) ? &theme_.buttonHover
                                                       : &style;
    DrawButtonArt(r, theme_, rect, ws.hoverAnim, ws.pressAnim, enabled, art);

    if (theme_.font && label) {
        const Color c = enabled ? theme_.text : theme_.textDisabled;
        TextClipped(r, *theme_.font, DisplayLabel(label), rect.Center().x, rect.Center().y, c, theme_.textSize,
                    TextAlign::Center, TextBaseline::Middle);
    }
    if (ws.focused && enabled) {
        const Rect focus = rect.Inset(3.0f);
        StrokeRound(r, focus, std::max(theme_.rounding - 2.0f, 1.0f), WithA(theme_.accentHover, 0.6f), 1.0f);
    }
    return clicked;
}

bool UiContext::IconButton(const char* icon, const Rect& rect, bool enabled, const char* tooltip) {
    UiImpl& st = ui_internal::CtxState(*this);
    Renderer2D& r = *st.r2d;
    const UiId id = MakeId(icon ? icon : "icon", 11);
    RegisterWidget(id, rect, true);
    WidgetStateEx& ws = static_cast<WidgetStateEx&>(State(id));
    const bool clicked = st.interact(id, rect, enabled);
    if (clicked) PlayClickSound();

    const f32 hover = ws.hoverAnim, press = ws.pressAnim;
    Color bg = Lerp(Color{theme_.panelAlt.r, theme_.panelAlt.g, theme_.panelAlt.b, 0.0f},
                    theme_.accent, hover * 0.25f);
    bg = Lerp(bg, theme_.accentActive, press * 0.4f);
    if (hover > 0.01f || press > 0.01f) FillRound(r, rect, theme_.rounding, bg);
    StrokeRound(r, rect, theme_.rounding,
                WithA(enabled ? Lerp(theme_.border, theme_.accent, hover * 0.7f) : theme_.border, 0.8f),
                theme_.borderWidth);

    if (theme_.font && icon) {
        TextClipped(r, *theme_.font, icon, rect.Center().x, rect.Center().y,
                    enabled ? theme_.text : theme_.textDisabled, theme_.textSize, TextAlign::Center,
                    TextBaseline::Middle);
    }
    if (tooltip && *tooltip && ws.hovered) SetTooltip(tooltip);
    return clicked;
}

bool UiContext::ToggleButton(const char* label, const Rect& rect, bool* value, bool enabled) {
    if (!value) return false;
    UiImpl& st = ui_internal::CtxState(*this);
    Renderer2D& r = *st.r2d;
    const UiId id = MakeId(label ? label : "toggle", 12);
    RegisterWidget(id, rect, true);
    WidgetStateEx& ws = static_cast<WidgetStateEx&>(State(id));
    const bool clicked = st.interact(id, rect, enabled);
    if (clicked) {
        *value = !*value;
        PlayClickSound();
    }
    const bool on = *value;
    const f32 pillW = std::min(rect.h * 1.4f, rect.w * 0.5f);
    const Rect pill{rect.Right() - pillW - 3.0f, rect.y + 3.0f, pillW, rect.h - 6.0f};
    ws.animTarget = on ? 1.0f : 0.0f;
    if (!ws.animInit) {
        ws.animValue = ws.animTarget;  // без анимации в первом кадре
        ws.animInit = true;
    }
    ws.animValue = ExpSmooth(ws.animValue, ws.animTarget, theme_.animationSpeed, st.dt);

    DrawButtonArt(r, theme_, rect, ws.hoverAnim, ws.pressAnim, enabled, nullptr);
    // Дорожка переключателя.
    const Color trackOff = Color{theme_.border.r, theme_.border.g, theme_.border.b, 0.9f};
    Color track = Lerp(trackOff, theme_.accent, ws.animValue);
    if (!enabled) track = Color{track.r, track.g, track.b, 0.4f};
    FillRound(r, pill, pill.h * 0.5f, track);
    const f32 knobD = pill.h - 4.0f;
    const f32 knobX = Lerp(pill.x + 2.0f, pill.Right() - knobD - 2.0f, ws.animValue);
    r.FillCircle(knobX + knobD * 0.5f, pill.Center().y, knobD * 0.5f, Color::White);

    if (theme_.font && label) {
        const Rect textRect{rect.x + 10.0f, rect.y, pill.x - rect.x - 16.0f, rect.h};
        TextClipped(r, *theme_.font, DisplayLabel(label), textRect.x, textRect.Center().y,
                    enabled ? (on ? theme_.text : theme_.textDim) : theme_.textDisabled, theme_.textSize,
                    TextAlign::Left, TextBaseline::Middle);
    }
    return clicked;
}

// ---------------------------------------------------------------------------
// Checkbox / radio
// ---------------------------------------------------------------------------
bool UiContext::Checkbox(const char* label, const Rect& rect, bool* value, bool enabled) {
    if (!value) return false;
    UiImpl& st = ui_internal::CtxState(*this);
    Renderer2D& r = *st.r2d;
    const UiId id = MakeId(label ? label : "check", 13);
    RegisterWidget(id, rect, true);
    WidgetStateEx& ws = static_cast<WidgetStateEx&>(State(id));
    const bool clicked = st.interact(id, rect, enabled);
    if (clicked) {
        *value = !*value;
        PlayClickSound();
    }
    ws.animTarget = *value ? 1.0f : 0.0f;
    ws.animValue = ExpSmooth(ws.animValue, ws.animTarget, theme_.animationSpeed, st.dt);

    const f32 boxSize = std::min(rect.h, 20.0f);
    const Rect box{rect.x, rect.Center().y - boxSize * 0.5f, boxSize, boxSize};
    const Color fill = enabled ? (ws.hovered ? Lerp(theme_.bg, theme_.accent, 0.15f) : theme_.bg)
                              : Color{theme_.bg.r, theme_.bg.g, theme_.bg.b, 0.6f};
    FillRound(r, box, theme_.rounding * 0.6f, fill);
    StrokeRound(r, box, theme_.rounding * 0.6f,
                enabled ? Lerp(theme_.border, theme_.accent, ws.animValue * 0.9f + ws.hoverAnim * 0.2f)
                        : theme_.border,
                theme_.borderWidth);
    DrawCheckMark(r, box, ws.animValue,
                  enabled ? Lerp(theme_.textDim, theme_.accentHover, ws.animValue) : theme_.textDisabled);

    if (theme_.font && label) {
        TextClipped(r, *theme_.font, DisplayLabel(label), box.Right() + 8.0f, rect.Center().y,
                    enabled ? theme_.text : theme_.textDisabled, theme_.textSize, TextAlign::Left,
                    TextBaseline::Middle);
    }
    return clicked;
}

bool UiContext::RadioButton(const char* label, const Rect& rect, int* value, int optionValue,
                            bool enabled) {
    if (!value) return false;
    UiImpl& st = ui_internal::CtxState(*this);
    Renderer2D& r = *st.r2d;
    const UiId id = MakeId(label ? label : "radio", 14 + optionValue);
    RegisterWidget(id, rect, true);
    WidgetStateEx& ws = static_cast<WidgetStateEx&>(State(id));
    const bool clicked = st.interact(id, rect, enabled);
    if (clicked && *value != optionValue) {
        *value = optionValue;
        PlayClickSound();
    }
    const bool on = (*value == optionValue);
    ws.animTarget = on ? 1.0f : 0.0f;
    ws.animValue = ExpSmooth(ws.animValue, ws.animTarget, theme_.animationSpeed, st.dt);

    const f32 d = std::min(rect.h, 20.0f);
    const Vec2 c{rect.x + d * 0.5f, rect.Center().y};
    r.FillCircle(c.x, c.y, d * 0.5f, enabled ? theme_.bg : Color{theme_.bg.r, theme_.bg.g, theme_.bg.b, 0.6f});
    r.StrokeWidth(theme_.borderWidth);
    r.StrokeColor(enabled ? Lerp(theme_.border, theme_.accent, ws.animValue) : theme_.border);
    r.BeginPath();
    r.Circle(c.x, c.y, d * 0.5f);
    r.Stroke();
    if (ws.animValue > 0.01f)
        r.FillCircle(c.x, c.y, d * 0.5f * 0.55f * ws.animValue,
                     enabled ? theme_.accentHover : theme_.textDisabled);

    if (theme_.font && label) {
        TextClipped(r, *theme_.font, DisplayLabel(label), rect.x + d + 8.0f, rect.Center().y,
                    enabled ? theme_.text : theme_.textDisabled, theme_.textSize, TextAlign::Left,
                    TextBaseline::Middle);
    }
    return clicked;
}

// ---------------------------------------------------------------------------
// Слайдер / перетаскивание
// ---------------------------------------------------------------------------
namespace {

// Форматирует значение printf-форматом вызывающего, откатываясь к значению
// по умолчанию, если строка формата непригодна.
std::string FormatValue(const char* fmt, double v) {
    char buf[64];
    if (fmt && std::strchr(fmt, '%')) {
        std::snprintf(buf, sizeof(buf), fmt, v);
    } else {
        std::snprintf(buf, sizeof(buf), "%.2f", v);
    }
    return std::string(buf);
}

// Общее тело слайдера: возвращает true, если `value` изменилось.
bool SliderBody(UiContext& ctx, UiId id, const Rect& rect, f32* value, f32 lo, f32 hi, bool enabled,
                bool fine, const char* label, const char* valueText) {
    UiImpl& st = ui_internal::CtxState(ctx);
    Renderer2D& r = *st.r2d;
    UiTheme& th = ctx.Theme();
    WidgetState& ws = ctx.State(id);

    const f32 trackH = std::min(6.0f, rect.h * 0.28f);
    const Rect track{rect.x, rect.y, rect.w, trackH};
    const f32 knobR = std::min(rect.h * 0.42f, 10.0f);
    const f32 range = hi - lo;
    bool changed = false;

    if (enabled && st.input && !st.inputBlocked) {
        const Input& in = *st.input;
        const Rect hit = rect;
        if (hit.Contains(in.MousePos()) && in.MousePressed()) {
            st.activeId = id;
            ctx.SetFocus(id);
        }
        if (st.activeId == id) {
            if (in.MouseDown()) {
                f32 t = range != 0.0f ? (in.MousePos().x - track.x) / (track.w != 0.0f ? track.w : 1.0f) : 0.0f;
                t = Clamp(t, 0.0f, 1.0f);
                const f32 nv = lo + t * range;
                if (nv != *value) {
                    *value = nv;
                    changed = true;
                }
                st.wantsMouse = true;
            } else {
                st.activeId = kUiIdNone;
            }
        }
        if (st.focusedId == id) {
            const f32 step = range * (fine ? 0.001f : 0.01f);
            if (in.KeyPressed(Key::Left)) {
                *value = Clamp(*value - step, lo, hi);
                changed = true;
            }
            if (in.KeyPressed(Key::Right)) {
                *value = Clamp(*value + step, lo, hi);
                changed = true;
            }
            if (in.KeyPressed(Key::Home)) {
                *value = lo;
                changed = true;
            }
            if (in.KeyPressed(Key::End)) {
                *value = hi;
                changed = true;
            }
        }
    }
    *value = Clamp(*value, lo, hi);

    const f32 t = range != 0.0f ? Clamp((*value - lo) / range, 0.0f, 1.0f) : 0.0f;
    const f32 knobX = track.x + t * track.w;
    const Color trackBg = enabled ? th.panelAlt : Color{th.panelAlt.r, th.panelAlt.g, th.panelAlt.b, 0.5f};

    FillRound(r, track, trackH * 0.5f, trackBg);
    if (t > 0.0f) FillRound(r, {track.x, track.y, track.w * t, track.h}, trackH * 0.5f,
                            enabled ? th.accent : th.textDisabled);
    const bool hot = ws.hovered || st.activeId == id;
    r.FillCircle(knobX, track.Center().y, knobR, enabled ? (hot ? th.accentHover : th.text) : th.textDisabled);
    if (st.focusedId == id) {
        r.StrokeWidth(2.0f);
        r.StrokeColor(WithA(th.accentHover, 0.7f));
        r.BeginPath();
        r.Circle(knobX, track.Center().y, knobR + 3.0f);
        r.Stroke();
    }
    if (valueText && th.font && rect.h > 30.0f) {
        r.DrawText(*th.font, valueText, rect.x, rect.y - 4.0f, th.textDim, th.smallSize, TextAlign::Left,
                   TextBaseline::Bottom, 0.0f);
    }
    if (label && th.font && rect.h > 30.0f) {
        r.DrawText(*th.font, DisplayLabel(label), rect.Right(), rect.y - 4.0f, th.textDim, th.smallSize,
                   TextAlign::Right, TextBaseline::Bottom, 0.0f);
    }
    return changed;
}

}  // namespace

bool UiContext::Slider(const char* label, const Rect& rect, f32* value, f32 min, f32 max,
                       const char* format, bool enabled) {
    if (!value) return false;
    UiImpl& st = ui_internal::CtxState(*this);
    const UiId id = MakeId(label ? label : "slider", 20);
    RegisterWidget(id, rect, true);
    WidgetState& ws = st.widget(id);
    ws.hovered = enabled && rect.Contains(MousePos());
    const std::string text = FormatValue(format, *value);
    (void)ws;
    const bool shift = st.input ? st.input->ShiftDown() : false;
    const bool changed = SliderBody(*this, id, rect, value, min, max, enabled, shift, label,
                                    text.c_str());
    if (changed) *value = Clamp(*value, min, max);
    return changed;
}

bool UiContext::DragFloat(const char* label, const Rect& rect, f32* value, f32 speed, f32 min, f32 max,
                          const char* format, bool enabled) {
    if (!value) return false;
    UiImpl& st = ui_internal::CtxState(*this);
    const UiId id = MakeId(label ? label : "dragf", 21);
    RegisterWidget(id, rect, true);
    WidgetState& ws = st.widget(id);
    const Input& in = *st.input;
    const bool hover = enabled && !st.inputBlocked && rect.Contains(in.MousePos());
    ws.hovered = hover;
    if (hover && in.MousePressed()) {
        st.activeId = id;
        SetFocus(id);
        ws.dragging = true;
    }
    bool changed = false;
    if (st.activeId == id && in.MouseDown()) {
        const f32 dx = in.MouseDelta().x;
        if (st.dt > 0.0f && dx != 0.0f) {
            const f32 mult = in.ShiftDown() ? 0.1f : (in.CtrlDown() || in.SuperDown() ? 10.0f : 1.0f);
            *value += dx * speed * mult;
            changed = true;
            st.wantsMouse = true;
        }
    } else if (ws.dragging && !in.MouseDown()) {
        ws.dragging = false;
        if (st.activeId == id) st.activeId = kUiIdNone;
    }
    if (st.focusedId == id) {
        const f32 step = speed * 10.0f;
        if (in.KeyPressed(Key::Left)) {
            *value -= step;
            changed = true;
        }
        if (in.KeyPressed(Key::Right)) {
            *value += step;
            changed = true;
        }
    }
    if (min != max) {
        const f32 lo = std::min(min, max), hi = std::max(min, max);
        *value = Clamp(*value, lo, hi);
    }
    const std::string text = FormatValue(format, *value);
    Renderer2D& r = *st.r2d;
    FillRound(r, rect, theme_.rounding, hover || st.activeId == id ? theme_.panelAlt : theme_.bg);
    StrokeRound(r, rect, theme_.rounding, st.focusedId == id ? theme_.accent : theme_.border,
                theme_.borderWidth);
    if (theme_.font) {
        if (label && *label) {
            std::string full = std::string(label) + "  " + text;
            r.DrawText(*theme_.font, full, rect.Center().x, rect.Center().y, theme_.text, theme_.textSize,
                       TextAlign::Center, TextBaseline::Middle, 0.0f);
        } else {
            r.DrawText(*theme_.font, text, rect.Center().x, rect.Center().y, theme_.text, theme_.textSize,
                       TextAlign::Center, TextBaseline::Middle, 0.0f);
        }
    }
    if (hover || st.activeId == id) SetCursor(st.activeId == id ? 3 : 1);
    return changed;
}

bool UiContext::DragInt(const char* label, const Rect& rect, int* value, f32 speed, int min, int max,
                        bool enabled) {
    if (!value) return false;
    f32 f = static_cast<f32>(*value);
    const bool changed = DragFloat(label, rect, &f, speed, static_cast<f32>(min), static_cast<f32>(max),
                                   "%.0f", enabled);
    if (changed) *value = static_cast<int>(std::lround(f));
    return changed;
}

// ---------------------------------------------------------------------------
// Полоса прогресса
// ---------------------------------------------------------------------------
bool UiContext::ProgressBar(const Rect& rect, f32 fraction, const char* label, const Color* fillColor) {
    UiImpl& st = ui_internal::CtxState(*this);
    Renderer2D& r = *st.r2d;
    const UiId id = MakeId(label ? label : "progress", 22);
    RegisterWidget(id, rect, false);
    WidgetStateEx& ws = static_cast<WidgetStateEx&>(State(id));
    ws.animTarget = Clamp(fraction, 0.0f, 1.0f);
    ws.animValue = ExpSmooth(ws.animValue, ws.animTarget, 6.0f, st.dt);

    const f32 radius = std::min(theme_.rounding, rect.h * 0.5f);
    FillRound(r, rect, radius, theme_.scrollTrack);
    StrokeRound(r, rect, radius, theme_.border, theme_.borderWidth);
    const f32 w = rect.w * ws.animValue;
    if (w > 0.5f) {
        const Color c = fillColor ? *fillColor : theme_.accent;
        FillRound(r, {rect.x, rect.y, w, rect.h}, radius, c);
        // Лёгкая подсветка вдоль верха придаёт заливке глубину.
        FillRound(r, {rect.x + 2.0f, rect.y + 1.0f, std::max(w - 4.0f, 0.0f), rect.h * 0.35f}, radius * 0.5f,
                  Color{1, 1, 1, 0.10f});
    }
    if (label && theme_.font)
        r.DrawText(*theme_.font, DisplayLabel(label), rect.Center().x, rect.Center().y, theme_.text, theme_.smallSize,
                   TextAlign::Center, TextBaseline::Middle, 0.0f);
    return false;
}

// ---------------------------------------------------------------------------
// Текстовое поле / область
// ---------------------------------------------------------------------------
bool UiContext::TextField(const char* id, const Rect& rect, std::string* text, const char* placeholder,
                          bool password, bool* submitted, bool enabled) {
    if (!text) return false;
    UiImpl& st = ui_internal::CtxState(*this);
    const UiId wid = MakeId(id ? id : "text", 30);
    RegisterWidget(wid, rect, enabled);
    st.widget(wid);
    if (enabled) SetCursor(2);
    return EditText(*this, wid, rect, text, false, password, placeholder, enabled ? submitted : nullptr);
}

bool UiContext::TextArea(const char* id, const Rect& rect, std::string* text, int maxLines,
                         bool enabled) {
    if (!text) return false;
    UiImpl& st = ui_internal::CtxState(*this);
    const UiId wid = MakeId(id ? id : "textarea", 31 + maxLines);
    RegisterWidget(wid, rect, enabled);
    st.widget(wid);
    if (enabled) SetCursor(2);
    return EditText(*this, wid, rect, text, true, false, nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// Полоса прокрутки (отдельная)
// ---------------------------------------------------------------------------
bool UiContext::Scrollbar(const char* id, const Rect& rect, f32* value, f32 pageFraction,
                          bool horizontal, bool enabled) {
    if (!value) return false;
    UiImpl& st = ui_internal::CtxState(*this);
    Renderer2D& r = *st.r2d;
    const UiId wid = MakeId(id ? id : "scrollbar", 32);
    RegisterWidget(wid, rect, enabled);
    WidgetState& ws = st.widget(wid);
    const Input& in = *st.input;

    f32 page = Clamp(pageFraction, 0.0f, 1.0f);
    f32 v = Clamp(*value, 0.0f, 1.0f - page);
    bool changed = false;
    const f32 axis = horizontal ? rect.w : rect.h;
    const f32 thickness = horizontal ? rect.h : rect.w;
    f32 thumbLen = axis * page;
    const f32 minThumb = std::min(thickness * 1.5f, axis);
    if (thumbLen < minThumb) thumbLen = minThumb;
    const f32 travel = axis - thumbLen;
    const f32 maxValue = 1.0f - page;
    const f32 frac = maxValue > 0.0f ? v / maxValue : 0.0f;
    Rect thumb = horizontal ? Rect{rect.x + frac * travel, rect.y, thumbLen, rect.h}
                            : Rect{rect.x, rect.y + frac * travel, rect.w, thumbLen};

    const bool hot = enabled && !st.inputBlocked && rect.Contains(in.MousePos());
    ws.hovered = hot;
    if (hot && in.MousePressed()) {
        st.activeId = wid;
        if (!thumb.Contains(in.MousePos())) {
            const bool after = horizontal ? in.MousePos().x > thumb.Center().x : in.MousePos().y > thumb.Center().y;
            v = Clamp(v + (after ? page : -page), 0.0f, maxValue);
            *value = v;
            changed = true;
        }
    }
    if (st.activeId == wid && in.MouseDown()) {
        const f32 local = horizontal ? (in.MousePos().x - rect.x) : (in.MousePos().y - rect.y);
        const f32 t = travel > 0.0f ? Clamp((local - thumbLen * 0.5f) / travel, 0.0f, 1.0f) : 0.0f;
        const f32 nv = t * maxValue;
        if (nv != v) {
            *value = nv;
            changed = true;
        }
        st.wantsMouse = true;
    }
    if (st.activeId == wid && !in.MouseDown()) st.activeId = kUiIdNone;

    if (hot && in.ScrollDelta().y != 0.0f) {
        const f32 step = horizontal ? in.ScrollDelta().x : in.ScrollDelta().y;
        if (step != 0.0f) {
            *value = Clamp(*value - step * (page > 0 ? page * 0.5f : 0.1f), 0.0f, maxValue);
            changed = true;
            st.wantsMouse = true;
        }
    }

    const f32 radius = std::min(thickness * 0.5f, theme_.rounding + 2.0f);
    r.FillRoundedRect(rect, radius, theme_.scrollTrack);
    r.FillRoundedRect(thumb, radius, (hot || st.activeId == wid) ? theme_.scrollThumbHover : theme_.scrollThumb);
    return changed;
}

// ---------------------------------------------------------------------------
// Списки
// ---------------------------------------------------------------------------
namespace {

f32 ListItemHeight(const UiTheme& th, f32 requested) { return requested > 0.0f ? requested : th.itemHeight; }

bool ListViewImpl(UiContext& ctx, const char* id, const Rect& rect, int itemCount,
                  const std::function<void(int, const Rect&, bool)>& drawItem, int* selected,
                  f32 itemHeight, bool enabled) {
    UiImpl& st = ui_internal::CtxState(ctx);
    UiTheme& th = ctx.Theme();
    Renderer2D& r = *st.r2d;
    const UiId wid = ctx.MakeId(id ? id : "list", 40);
    ctx.RegisterWidget(wid, rect, enabled);
    WidgetStateEx& ws = static_cast<WidgetStateEx&>(ctx.State(wid));
    const f32 ih = ListItemHeight(th, itemHeight);
    const f32 contentH = static_cast<f32>(itemCount) * ih;
    const f32 viewH = rect.h;
    const f32 maxScroll = std::max(0.0f, contentH - viewH);
    const Input& in = *st.input;
    const bool hot = enabled && !st.inputBlocked && rect.Contains(in.MousePos());
    ws.hovered = hot;
    bool changed = false;

    if (hot && in.MousePressed()) {
        st.activeId = wid;
        ctx.SetFocus(wid);
        st.wantsMouse = true;
    }
    // Выделение следует за нажатием, поэтому клик-перетаскивание скользит по строкам.
    if (hot && in.MouseDown() && st.activeId == wid && selected) {
        const f32 local = in.MousePos().y - rect.y + ws.scrollY;
        const int idx = static_cast<int>(std::floor(local / ih));
        if (idx >= 0 && idx < itemCount && *selected != idx) {
            *selected = idx;
            changed = true;
        }
        st.wantsMouse = true;
    }
    if (st.activeId == wid && !in.MouseDown()) st.activeId = kUiIdNone;

    // Колесо.
    if (hot && in.ScrollDelta().y != 0.0f) {
        ws.scrollY = ui_internal::ClampScroll(ws.scrollY - in.ScrollDelta().y * ih * 0.6f, contentH, viewH);
        st.wantsMouse = true;
    }
    ws.scrollY = ui_internal::ClampScroll(ws.scrollY, contentH, viewH);

    // Клавиатурная навигация (только пока список владеет фокусом).
    if (enabled && st.focusedId == wid && selected && itemCount > 0) {
        int sel = Clamp(*selected, 0, itemCount - 1);
        const int page = std::max(1, static_cast<int>(viewH / ih));
        bool moved = false;
        if (in.KeyPressed(Key::Down)) {
            sel = std::min(sel + 1, itemCount - 1);
            moved = true;
            st.keyboardNavCaptured = true;
        }
        if (in.KeyPressed(Key::Up)) {
            sel = std::max(sel - 1, 0);
            moved = true;
            st.keyboardNavCaptured = true;
        }
        if (in.KeyPressed(Key::PageDown)) {
            sel = std::min(sel + page, itemCount - 1);
            moved = true;
            st.keyboardNavCaptured = true;
        }
        if (in.KeyPressed(Key::PageUp)) {
            sel = std::max(sel - page, 0);
            moved = true;
            st.keyboardNavCaptured = true;
        }
        if (in.KeyPressed(Key::Home)) {
            sel = 0;
            moved = true;
            st.keyboardNavCaptured = true;
        }
        if (in.KeyPressed(Key::End)) {
            sel = itemCount - 1;
            moved = true;
            st.keyboardNavCaptured = true;
        }
        if (moved && sel != *selected) {
            *selected = sel;
            changed = true;
        }
        // Автопрокрутка, чтобы выделение оставалось видимым.
        const f32 top = static_cast<f32>(*selected) * ih;
        if (top < ws.scrollY) ws.scrollY = top;
        if (top + ih > ws.scrollY + viewH) ws.scrollY = top + ih - viewH;
        ws.scrollY = ui_internal::ClampScroll(ws.scrollY, contentH, viewH);
    }

    // Анимированная полоса выделения.
    if (selected) {
        const f32 target = static_cast<f32>(*selected) * ih;
        if (!ws.animInit) {
            ws.animValue = target;
            ws.animInit = true;
        }
        ws.animTarget = target;
        ws.animValue = ExpSmooth(ws.animValue, ws.animTarget, 14.0f, st.dt);
    }

    r.Save();
    r.ClipRect(rect.x, rect.y, rect.w, rect.h);
    FillRound(r, rect, th.rounding, th.bg);
    StrokeRound(r, rect, th.rounding, th.border, th.borderWidth);

    // Полоса выделения за элементами.
    if (selected && *selected >= 0 && *selected < itemCount) {
        const Rect bar{rect.x + 2.0f, rect.y + ws.animValue - ws.scrollY + 1.0f, rect.w - 4.0f, ih - 2.0f};
        if (bar.Bottom() > rect.y && bar.y < rect.Bottom())
            FillRound(r, bar, th.rounding * 0.7f, WithA(th.accent, 0.28f));
    }

    // Рисуется / раскладывается только видимая часть (виртуализация).
    int first = static_cast<int>(std::floor(ws.scrollY / ih));
    int last = static_cast<int>(std::ceil((ws.scrollY + viewH) / ih));
    first = std::max(0, first);
    last = std::min(itemCount, last + 1);
    int hoverIdx = -1;
    if (hot) {
        const int idx = static_cast<int>(std::floor((in.MousePos().y - rect.y + ws.scrollY) / ih));
        if (idx >= 0 && idx < itemCount) hoverIdx = idx;
    }
    for (int i = first; i < last; ++i) {
        Rect ir{rect.x + 2.0f, rect.y + static_cast<f32>(i) * ih - ws.scrollY, rect.w - 4.0f, ih};
        const bool isSel = selected && *selected == i;
        if (!isSel && i == hoverIdx) FillRound(r, ir, th.rounding * 0.7f, WithA(th.panelAlt, 0.9f));
        if (drawItem) {
            drawItem(i, ir, isSel);
        } else if (th.font) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "Item %d", i);
            r.DrawText(*th.font, buf, ir.x + 8.0f, ir.Center().y, isSel ? th.text : th.textDim, th.textSize,
                       TextAlign::Left, TextBaseline::Middle, 0.0f);
        }
        if (i == hoverIdx && !isSel) StrokeRound(r, ir, th.rounding * 0.7f, WithA(th.border, 0.7f), 1.0f);
    }

    // Тонкая полоса прокрутки, когда содержимое переполняет.
    if (maxScroll > 0.5f) {
        const f32 bw = 6.0f;
        const Rect track{rect.Right() - bw - 2.0f, rect.y + 2.0f, bw, rect.h - 4.0f};
        f32 thumbH = std::max(20.0f, track.h * (viewH / contentH));
        const f32 t = ws.scrollY / maxScroll;
        const Rect thumb{track.x, track.y + t * (track.h - thumbH), track.w, thumbH};
        r.FillRoundedRect(track, bw * 0.5f, WithA(th.scrollTrack, 0.8f));
        r.FillRoundedRect(thumb, bw * 0.5f, WithA(th.scrollThumb, 0.95f));
    }
    r.Restore();

    if (hot) ctx.SetCursor(1);
    return changed;
}

}  // namespace

bool UiContext::ListView(const char* id, const Rect& rect, const std::vector<std::string>& items,
                         int* selected, f32 itemHeight, bool enabled) {
    const UiTheme& th = theme_;
    Renderer2D* r2d = ui_internal::CtxState(*this).r2d;
    return ListViewImpl(*this, id, rect, static_cast<int>(items.size()),
                        [&items, &th, r2d](int i, const Rect& ir, bool sel) {
                            if (i < 0 || i >= static_cast<int>(items.size()) || !th.font || !r2d) return;
                            r2d->DrawText(*th.font, items[static_cast<usize>(i)], ir.x + 8.0f, ir.Center().y,
                                          sel ? th.text : th.textDim, th.textSize, TextAlign::Left,
                                          TextBaseline::Middle, 0.0f);
                        },
                        selected, itemHeight, enabled);
}

bool UiContext::ListViewCustom(const char* id, const Rect& rect, int itemCount, int* selected,
                               const std::function<void(int, const Rect&, bool)>& drawItem,
                               f32 itemHeight) {
    return ListViewImpl(*this, id, rect, itemCount, drawItem, selected, itemHeight, true);
}

// ---------------------------------------------------------------------------
// Dropdown / combo box
// ---------------------------------------------------------------------------
bool UiContext::Dropdown(const char* id, const Rect& rect, const std::vector<std::string>& items,
                         int* selected, bool enabled) {
    UiImpl& st = ui_internal::CtxState(*this);
    Renderer2D& r = *st.r2d;
    const UiId wid = MakeId(id ? id : "dropdown", 50);
    const UiId pid = wid ^ 0xD1U;
    RegisterWidget(wid, rect, enabled);
    WidgetStateEx& ws = static_cast<WidgetStateEx&>(State(wid));
    const Input& in = *st.input;
    const bool open = (st.activeDropdown == pid) && st.popups.count(pid) > 0;
    const bool hot = enabled && !st.inputBlocked && rect.Contains(in.MousePos());
    ws.hovered = hot;
    bool changed = false;

    // Переключение по клику.
    if (hot && in.MousePressed()) {
        if (open) {
            st.closePopupById(pid);
            st.activeDropdown = kUiIdNone;
        } else {
            PopupState p;
            p.open = true;
            p.openedFrame = st.frameGen;
            p.rect = Rect{rect.x, rect.Bottom() + 2.0f, rect.w, std::min(static_cast<f32>(items.size()) * theme_.itemHeight + 8.0f, 240.0f)};
            p.position = {rect.x, rect.Bottom() + 2.0f};
            p.size = {rect.w, p.rect.h};
            p.owner = wid;
            st.popups[pid] = p;
            st.activeDropdown = pid;
            st.wantsMouse = true;
        }
    }

    // Тело.
    const bool hasArt = theme_.framePatch.valid();
    if (hasArt) {
        r.Image9(*theme_.framePatch.texture, rect, theme_.framePatch.patch, Color::White,
                 theme_.framePatch.scale);
    } else {
        FillRound(r, rect, theme_.rounding, hot || open ? theme_.panelAlt : theme_.bg);
        StrokeRound(r, rect, theme_.rounding, open ? theme_.accent : theme_.border, theme_.borderWidth);
    }
    if (theme_.font) {
        const char* label = (selected && *selected >= 0 && *selected < static_cast<int>(items.size()))
                                ? items[static_cast<usize>(*selected)].c_str()
                                : "";
        r.DrawText(*theme_.font, DisplayLabel(label), rect.x + 10.0f, rect.Center().y, enabled ? theme_.text : theme_.textDisabled,
                   theme_.textSize, TextAlign::Left, TextBaseline::Middle, 0.0f);
    }
    // Chevron.
    {
        r.Save();
        const Vec2 c{rect.Right() - 14.0f, rect.Center().y};
        r.BeginPath();
        r.MoveTo(c.x - 5.0f, c.y - 2.5f);
        r.LineTo(c.x, c.y + 2.5f);
        r.LineTo(c.x + 5.0f, c.y - 2.5f);
        r.LineCap(LineCap::Round);
        r.LineJoin(LineJoin::Round);
        r.StrokeWidth(1.6f);
        r.StrokeColor(enabled ? (open ? theme_.accent : theme_.textDim) : theme_.textDisabled);
        r.Stroke();
        r.Restore();
    }

    if (open) {
        auto it = st.popups.find(pid);
        if (it != st.popups.end()) {
            PopupState& p = it->second;
            p.open = true;
            p.generation = st.frameGen;
            const f32 ih = theme_.itemHeight;
            const Rect list = p.rect;
            const f32 contentH = static_cast<f32>(items.size()) * ih + 8.0f;
            const f32 maxScroll = std::max(0.0f, contentH - list.h);

            // Ввод: подсветка наведения, клик для выбора, колесо для прокрутки.
            const bool listHot = !st.inputBlocked && list.Contains(in.MousePos());
            if (listHot) {
                st.wantsMouse = true;
                if (in.ScrollDelta().y != 0.0f && maxScroll > 0.0f) {
                    ws.scrollY = ui_internal::ClampScroll(ws.scrollY - in.ScrollDelta().y * ih * 0.6f, contentH,
                                                          list.h);
                }
                const int idx = static_cast<int>(std::floor((in.MousePos().y - list.y + ws.scrollY - 4.0f) / ih));
                if (idx >= 0 && idx < static_cast<int>(items.size())) {
                    ws.hoveredItem = idx;
                    if (in.MouseReleased() && selected) {
                        if (*selected != idx) {
                            *selected = idx;
                            changed = true;
                        }
                        st.closePopupById(pid);
                        st.activeDropdown = kUiIdNone;
                        PlayClickSound();
                    }
                } else {
                    ws.hoveredItem = -1;
                }
            } else {
                ws.hoveredItem = -1;
                if (in.MousePressed() || in.KeyPressed(Key::Escape)) {
                    st.closePopupById(pid);
                    st.activeDropdown = kUiIdNone;
                }
            }
            ws.scrollY = ui_internal::ClampScroll(ws.scrollY, contentH, list.h);

            // Откладываем реальную отрисовку списка до прохода оверлеев, чтобы он
            // лёг поверх любого виджета, нарисованного позже в этом кадре.
            const std::vector<std::string>* itemsPtr = &items;
            const int sel = selected ? *selected : -1;
            const int hoverItem = ws.hoveredItem;
            const f32 scroll = ws.scrollY;
            const UiTheme* th = &theme_;
            st.pushOverlay(
                [list, itemsPtr, ih, contentH, scroll, sel, hoverItem, th, maxScroll](Renderer2D& rr) {
                    rr.FillRoundedRect({list.x + 2.0f, list.y + 3.0f, list.w, list.h}, th->rounding, th->shadow);
                    rr.FillRoundedRect(list, th->rounding, th->panel);
                    rr.StrokeRoundedRect(list, th->rounding, th->accent, th->borderWidth);
                    rr.Save();
                    rr.ClipRect(list.x, list.y, list.w, list.h);
                    const int n = static_cast<int>(itemsPtr->size());
                    const int first = std::max(0, static_cast<int>(std::floor(scroll / ih)));
                    const int last = std::min(n, static_cast<int>(std::ceil((scroll + list.h) / ih)) + 1);
                    for (int i = first; i < last; ++i) {
                        const Rect ir{list.x + 4.0f, list.y + 4.0f + static_cast<f32>(i) * ih - scroll,
                                      list.w - 8.0f, ih};
                        if (i == sel)
                            rr.FillRoundedRect(ir, th->rounding * 0.7f, WithA(th->accent, 0.35f));
                        else if (i == hoverItem)
                            rr.FillRoundedRect(ir, th->rounding * 0.7f, WithA(th->panelAlt, 0.95f));
                        if (th->font)
                            rr.DrawText(*th->font, (*itemsPtr)[static_cast<usize>(i)], ir.x + 6.0f, ir.Center().y,
                                        i == sel ? th->text : th->textDim, th->textSize, TextAlign::Left,
                                        TextBaseline::Middle, 0.0f);
                    }
                    rr.Restore();
                    if (maxScroll > 0.01f) {
                        const f32 bw = 6.0f;
                        const Rect track{list.Right() - bw - 2.0f, list.y + 2.0f, bw, list.h - 4.0f};
                        f32 thumbH = std::max(20.0f, track.h * (list.h / contentH));
                        const f32 maxS = std::max(0.001f, contentH - list.h);
                        const f32 t = Clamp(scroll / maxS, 0.0f, 1.0f);
                        const Rect thumb{track.x, track.y + t * (track.h - thumbH), track.w, thumbH};
                        rr.FillRoundedRect(track, bw * 0.5f, WithA(th->scrollTrack, 0.8f));
                        rr.FillRoundedRect(thumb, bw * 0.5f, WithA(th->scrollThumb, 0.95f));
                    }
                },
                1);
        }
    }

    if (hot) SetCursor(1);
    return changed;
}

bool UiContext::ComboBox(const char* id, const Rect& rect, const std::vector<std::string>& items,
                         int* selected, const char* label, bool enabled) {
    UiImpl& st = ui_internal::CtxState(*this);
    Renderer2D& r = *st.r2d;
    Rect body = rect;
    if (label && *label && theme_.font) {
        r.DrawText(*theme_.font, DisplayLabel(label), rect.x, rect.y - 2.0f, theme_.textDim, theme_.smallSize,
                   TextAlign::Left, TextBaseline::Bottom, 0.0f);
        const f32 labelH = theme_.smallSize * 1.6f;
        body = Rect{rect.x, rect.y + labelH, rect.w, std::max(rect.h - labelH, 8.0f)};
    }
    return Dropdown(id ? id : "combo", body, items, selected, enabled);
}

// ---------------------------------------------------------------------------
// Collapsing header
// ---------------------------------------------------------------------------
bool UiContext::CollapsingHeader(const char* label, const Rect& rect, bool* open, bool enabled) {
    if (!open) return false;
    UiImpl& st = ui_internal::CtxState(*this);
    Renderer2D& r = *st.r2d;
    const UiId wid = MakeId(label ? label : "header", 60);
    RegisterWidget(wid, rect, enabled);
    WidgetStateEx& ws = static_cast<WidgetStateEx&>(State(wid));
    const bool clicked = st.interact(wid, rect, enabled);
    if (clicked) {
        *open = !*open;
        PlayClickSound();
    }
    ws.animTarget = *open ? 1.0f : 0.0f;
    ws.animValue = ExpSmooth(ws.animValue, ws.animTarget, theme_.animationSpeed, st.dt);

    const f32 hover = ws.hoverAnim;
    FillRound(r, rect, theme_.rounding, Lerp(theme_.panel, theme_.panelAlt, hover * 0.8f));
    StrokeRound(r, rect, theme_.rounding, Lerp(theme_.border, theme_.accent, hover * 0.5f), theme_.borderWidth);

    // Вращающийся треугольник.
    const Vec2 c{rect.x + 12.0f, rect.Center().y};
    const f32 a = ws.animValue * kPi * 0.5f;
    const Vec2 p0{0.0f, -5.0f}, p1{0.0f, 5.0f}, p2{6.0f, 0.0f};
    auto rot = [&](const Vec2& v) {
        const f32 ca = std::cos(a), sa = std::sin(a);
        return Vec2{c.x + v.x * ca - v.y * sa, c.y + v.x * sa + v.y * ca};
    };
    const Vec2 q0 = rot(p0), q1 = rot(p1), q2 = rot(p2);
    r.BeginPath();
    r.MoveTo(q0.x, q0.y);
    r.LineTo(q1.x, q1.y);
    r.LineTo(q2.x, q2.y);
    r.ClosePath();
    r.FillColor(enabled ? theme_.accent : theme_.textDisabled);
    r.Fill();

    if (theme_.font && label) {
        r.DrawText(*theme_.font, DisplayLabel(label), c.x + 14.0f, rect.Center().y, enabled ? theme_.text : theme_.textDisabled,
                   theme_.textSize, TextAlign::Left, TextBaseline::Middle, 0.0f);
    }
    return clicked;
}

// ---------------------------------------------------------------------------
// Tab bar
// ---------------------------------------------------------------------------
bool UiContext::TabBar(const char* id, const Rect& rect, const std::vector<std::string>& tabs,
                       int* selected) {
    if (tabs.empty()) return false;
    UiImpl& st = ui_internal::CtxState(*this);
    Renderer2D& r = *st.r2d;
    const UiId wid = MakeId(id ? id : "tabs", 61);
    RegisterWidget(wid, rect, false);
    WidgetStateEx& ws = static_cast<WidgetStateEx&>(State(wid));
    const Input& in = *st.input;
    const bool hot = !st.inputBlocked && rect.Contains(in.MousePos());
    ws.hovered = hot;
    const int n = static_cast<int>(tabs.size());
    const f32 tabW = rect.w / static_cast<f32>(n);
    // Помним входящее выделение, чтобы возвращаемое значение сообщало об изменении
    // *этого кадра*, а не сравнивало уже обновлённое значение с самим собой.
    const bool hadSelection = selected != nullptr;
    const int previous = hadSelection ? *selected : 0;
    int hoverTab = -1;
    if (hot) {
        hoverTab = Clamp(static_cast<int>((in.MousePos().x - rect.x) / (tabW != 0.0f ? tabW : 1.0f)), 0, n - 1);
        if (in.MousePressed() && hadSelection && previous != hoverTab) {
            *selected = hoverTab;
            PlayClickSound();
        }
    }
    const int sel = Clamp(selected ? *selected : 0, 0, n - 1);

    // Анимированное подчёркивание скользит между табами.
    const f32 target = static_cast<f32>(sel) * tabW;
    if (!ws.animInit) {
        ws.animValue = target;
        ws.animInit = true;
    }
    ws.animTarget = target;
    ws.animValue = ExpSmooth(ws.animValue, ws.animTarget, 16.0f, st.dt);

    FillRound(r, rect, theme_.rounding, theme_.bg);
    StrokeRound(r, rect, theme_.rounding, theme_.border, theme_.borderWidth);
    for (int i = 0; i < n; ++i) {
        const Rect tr{rect.x + static_cast<f32>(i) * tabW, rect.y, tabW, rect.h};
        if (i == hoverTab && i != sel) FillRound(r, tr.Inset(2.0f), theme_.rounding * 0.8f, WithA(theme_.panelAlt, 0.85f));
        if (theme_.font) {
            r.DrawText(*theme_.font, tabs[static_cast<usize>(i)], tr.Center().x, tr.Center().y,
                       i == sel ? theme_.text : theme_.textDim, theme_.textSize, TextAlign::Center,
                       TextBaseline::Middle, 0.0f);
        }
    }
    const Rect underline{rect.x + ws.animValue + 4.0f, rect.Bottom() - 3.0f, tabW - 8.0f, 2.5f};
    FillRound(r, underline, 1.25f, theme_.accent);
    if (hot) SetCursor(1);
    // True ровно в кадре, когда пользователь выбрал другой таб.
    return hadSelection && in.MousePressed() && hot && sel != previous;
}

// ---------------------------------------------------------------------------
// Выбор цвета
// ---------------------------------------------------------------------------
namespace {

void RgbToHsv(const Color& c, f32* h, f32* s, f32* v) {
    const f32 mx = std::max(c.r, std::max(c.g, c.b));
    const f32 mn = std::min(c.r, std::min(c.g, c.b));
    const f32 d = mx - mn;
    f32 hh = 0.0f;
    if (d > 1e-6f) {
        if (mx == c.r) hh = std::fmod((c.g - c.b) / d, 6.0f);
        else if (mx == c.g) hh = (c.b - c.r) / d + 2.0f;
        else hh = (c.r - c.g) / d + 4.0f;
        hh /= 6.0f;
        if (hh < 0.0f) hh += 1.0f;
    }
    *h = hh;
    *s = mx <= 1e-6f ? 0.0f : d / mx;
    *v = mx;
}

Color HsvToRgb(f32 h, f32 s, f32 v, f32 a) {
    const f32 hh = (h - std::floor(h)) * 6.0f;
    const int i = static_cast<int>(hh);
    const f32 f = hh - static_cast<f32>(i);
    const f32 p = v * (1.0f - s);
    const f32 q = v * (1.0f - s * f);
    const f32 t = v * (1.0f - s * (1.0f - f));
    switch (i % 6) {
        case 0: return {v, t, p, a};
        case 1: return {q, v, p, a};
        case 2: return {p, v, t, a};
        case 3: return {p, q, v, a};
        case 4: return {t, p, v, a};
        default: return {v, p, q, a};
    }
}

std::string HexOf(const Color& c) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02X%02X%02X", static_cast<int>(Clamp(c.r, 0.0f, 1.0f) * 255.0f + 0.5f),
                  static_cast<int>(Clamp(c.g, 0.0f, 1.0f) * 255.0f + 0.5f),
                  static_cast<int>(Clamp(c.b, 0.0f, 1.0f) * 255.0f + 0.5f));
    return std::string(buf);
}

bool ParseHex(const std::string& s, Color* out) {
    u32 v = 0;
    int digits = 0;
    for (char ch : s) {
        int d = -1;
        if (ch >= '0' && ch <= '9') d = ch - '0';
        else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
        else if (ch == '#' || ch == ' ') continue;
        else return false;
        v = (v << 4) | static_cast<u32>(d);
        if (++digits == 6) break;
    }
    if (digits != 6) return false;
    out->r = ((v >> 16) & 0xFF) / 255.0f;
    out->g = ((v >> 8) & 0xFF) / 255.0f;
    out->b = (v & 0xFF) / 255.0f;
    return true;
}

}  // namespace

bool UiContext::ColorPicker(const char* id, const Rect& rect, Color* color) {
    if (!color) return false;
    UiImpl& st = ui_internal::CtxState(*this);
    Renderer2D& r = *st.r2d;
    const UiId wid = MakeId(id ? id : "color", 70);
    RegisterWidget(wid, rect, true);
    st.widget(wid);
    const Input& in = *st.input;
    bool changed = false;

    const f32 hueW = 20.0f;
    const f32 hexH = 24.0f;
    const f32 gap = 6.0f;
    const Rect sv{rect.x, rect.y, std::max(rect.w - hueW - gap, 20.0f), std::max(rect.h - hexH - gap, 20.0f)};
    const Rect hue{sv.Right() + gap, rect.y, hueW, sv.h};
    const Rect hexRect{rect.x, sv.Bottom() + gap, rect.w, hexH};

    f32 h = 0, s = 0, v = 0;
    RgbToHsv(*color, &h, &s, &v);

    // Квадрат SV.
    const bool svHot = !st.inputBlocked && sv.Contains(in.MousePos());
    if (svHot && in.MousePressed()) {
        st.activeId = wid;
        SetFocus(wid);
    }
    if (st.activeId == wid && in.MouseDown() && svHot) {
        s = Clamp((in.MousePos().x - sv.x) / (sv.w != 0 ? sv.w : 1.0f), 0.0f, 1.0f);
        v = 1.0f - Clamp((in.MousePos().y - sv.y) / (sv.h != 0 ? sv.h : 1.0f), 0.0f, 1.0f);
        *color = HsvToRgb(h, s, v, color->a);
        changed = true;
        st.wantsMouse = true;
    }
    const bool hueHot = !st.inputBlocked && hue.Contains(in.MousePos());
    if (hueHot && in.MousePressed()) {
        st.activeId = wid;
        SetFocus(wid);
    }
    if (st.activeId == wid && in.MouseDown() && hueHot) {
        h = Clamp((in.MousePos().y - hue.y) / (hue.h != 0 ? hue.h : 1.0f), 0.0f, 1.0f);
        *color = HsvToRgb(h, s, v, color->a);
        changed = true;
        st.wantsMouse = true;
    }
    if (st.activeId == wid && !in.MouseDown()) st.activeId = kUiIdNone;

    // SV: база оттенка + бело-чёрные градиенты.
    const Color hueCol = HsvToRgb(h, 1.0f, 1.0f, 1.0f);
    r.FillRect(sv, hueCol);
    r.FillRectGradient(sv, Color{1, 1, 1, 1}, Color{1, 1, 1, 0}, false);
    r.FillRectGradient(sv, Color{0, 0, 0, 0}, Color{0, 0, 0, 1}, true);
    r.StrokeRect(sv, theme_.border, theme_.borderWidth);
    const Vec2 svPos{sv.x + s * sv.w, sv.y + (1.0f - v) * sv.h};
    r.StrokeWidth(2.0f);
    r.StrokeColor(Color::White);
    r.BeginPath();
    r.Circle(svPos.x, svPos.y, 6.0f);
    r.Stroke();
    r.StrokeWidth(1.0f);
    r.StrokeColor(Color{0, 0, 0, 0.6f});
    r.BeginPath();
    r.Circle(svPos.x, svPos.y, 7.5f);
    r.Stroke();

    // Полоса оттенка: 6 вертикальных полос.
    for (int i = 0; i < 6; ++i) {
        const Color a = HsvToRgb(static_cast<f32>(i) / 6.0f, 1.0f, 1.0f, 1.0f);
        const Color b = HsvToRgb(static_cast<f32>(i + 1) / 6.0f, 1.0f, 1.0f, 1.0f);
        const Rect band{hue.x, hue.y + hue.h * static_cast<f32>(i) / 6.0f, hue.w, hue.h / 6.0f + 1.0f};
        r.FillRectGradient(band, a, b, true);
    }
    r.StrokeRect(hue, theme_.border, theme_.borderWidth);
    const f32 hueY = hue.y + h * hue.h;
    r.FillRect({hue.x - 2.0f, hueY - 2.0f, hue.w + 4.0f, 4.0f}, Color::White);
    r.FillRect({hue.x - 2.0f, hueY - 2.0f, hue.w + 4.0f, 1.0f}, Color{0, 0, 0, 0.6f});

    // Поле HEX.
    {
        const UiId hid = wid ^ 0x4E1U;
        RegisterWidget(hid, hexRect, true);
        WidgetState& hws = st.widget(hid);
        std::string hex = HexOf(*color);
        if (st.focusedId != hid) {
            // Держим буфер синхронным, пока поле не редактируется.
            hws.editBuffer = hex;
        } else if (hws.editBuffer.empty()) {
            hws.editBuffer = hex;
        }
        bool submitted = false;
        const bool edited = EditText(*this, hid, hexRect, &hws.editBuffer, false, false, "RRGGBB",
                                     &submitted);
        if (edited) {
            Color parsed;
            if (ParseHex(hws.editBuffer, &parsed)) {
                parsed.a = color->a;
                *color = parsed;
                changed = true;
            }
        }
        if (submitted) {
            Color parsed;
            if (ParseHex(hws.editBuffer, &parsed)) {
                parsed.a = color->a;
                *color = parsed;
                changed = true;
            }
            hws.editBuffer = HexOf(*color);
            st.focusedId = kUiIdNone;
        }
        // Живой образец цвета справа от поля.
        r.FillRect({hexRect.Right() - 20.0f, hexRect.y + 4.0f, 16.0f, hexRect.h - 8.0f}, *color);
    }
    if (svHot || hueHot) SetCursor(4);
    return changed;
}

// ---------------------------------------------------------------------------
// Spinner
// ---------------------------------------------------------------------------
bool UiContext::Spinner(const Rect& rect, f32 phase, const Color* color) {
    UiImpl& st = ui_internal::CtxState(*this);
    Renderer2D& r = *st.r2d;
    const UiId wid = MakeId("spinner", static_cast<int>(rect.x));
    RegisterWidget(wid, rect, false);
    (void)wid;
    (void)st;
    const f32 radius = std::min(rect.w, rect.h) * 0.5f - 1.5f;
    if (radius <= 1.0f) return false;
    const Vec2 c = rect.Center();
    const Color col = color ? *color : theme_.accent;
    r.Save();
    r.LineCap(LineCap::Round);
    r.StrokeWidth(std::max(2.0f, radius * 0.22f));
    // Затухающий хвост.
    const int segments = 12;
    for (int i = 0; i < segments; ++i) {
        const f32 t0 = phase + static_cast<f32>(i) / static_cast<f32>(segments) * kTau * 0.75f;
        const f32 t1 = t0 + kTau * 0.75f / static_cast<f32>(segments) * 1.2f;
        const f32 a = (1.0f - static_cast<f32>(i) / static_cast<f32>(segments)) * 0.9f + 0.05f;
        r.StrokeColor(WithA(col, a));
        r.BeginPath();
        r.Arc(c.x, c.y, radius, t0, t1, Winding::CW);
        r.Stroke();
    }
    r.Restore();
    return false;
}

// ---------------------------------------------------------------------------
// Images
// ---------------------------------------------------------------------------
void UiContext::Image(const Texture& tex, const Rect& rect, const Color& tint) {
    UiImpl& st = ui_internal::CtxState(*this);
    if (!st.r2d) return;
    Renderer2D& r = *st.r2d;
    if (tex.Valid()) {
        r.Image(tex, rect, Rect{0, 0, 1, 1}, tint);
    } else {
        // Шахматная заглушка, чтобы работа раскладки была видна без ассетов.
        FillRound(r, rect, theme_.rounding, theme_.panelAlt);
        StrokeRound(r, rect, theme_.rounding, theme_.border, theme_.borderWidth);
        const f32 cell = 10.0f;
        for (f32 y = rect.y; y < rect.Bottom(); y += cell)
            for (f32 x = rect.x; x < rect.Right(); x += cell)
                if ((static_cast<int>((x - rect.x) / cell) + static_cast<int>((y - rect.y) / cell)) % 2 == 0)
                    FillRectC(r, {x, y, std::min(cell, rect.Right() - x), std::min(cell, rect.Bottom() - y)},
                              WithA(theme_.border, 0.25f));
    }
}

bool UiContext::ImageButton(const char* id, const Texture& tex, const Rect& rect, const Color& tint) {
    UiImpl& st = ui_internal::CtxState(*this);
    Renderer2D& r = *st.r2d;
    const UiId wid = MakeId(id ? id : "imgbtn", 80);
    RegisterWidget(wid, rect, true);
    WidgetStateEx& ws = static_cast<WidgetStateEx&>(State(wid));
    const bool clicked = st.interact(wid, rect, true);
    if (clicked) PlayClickSound();
    const Color t = ws.active ? tint * 0.85f : (ws.hovered ? tint : tint);
    if (tex.Valid()) {
        r.Image(tex, rect, Rect{0, 0, 1, 1}, t);
    } else {
        FillRound(r, rect, theme_.rounding, Lerp(theme_.panelAlt, theme_.accent, ws.hoverAnim * 0.2f));
    }
    StrokeRound(r, rect, theme_.rounding,
                Lerp(theme_.border, theme_.accentHover, ws.hoverAnim * 0.9f),
                theme_.borderWidth + (ws.active ? 0.0f : 0.0f));
    if (ws.hovered) SetCursor(1);
    return clicked;
}

void UiContext::Draw9(const Texture& tex, const Rect& rect, const NinePatch& patch, const Color& tint,
                      f32 scale) {
    UiImpl& st = ui_internal::CtxState(*this);
    if (!st.r2d || !tex.Valid() || rect.w <= 0.0f || rect.h <= 0.0f) return;
    st.r2d->Image9(tex, rect, patch, tint, scale);
}

// ---------------------------------------------------------------------------
// Tooltips / modals / popups
// ---------------------------------------------------------------------------
void UiContext::SetTooltip(const char* text) {
    UiImpl& st = ui_internal::CtxState(*this);
    if (text) st.tooltip = text;
}

void UiContext::Tooltip(const char* text) { SetTooltip(text); }

bool UiContext::BeginModal(const char* id, const Vec2& size, const char* title) {
    UiImpl& st = ui_internal::CtxState(*this);
    const UiId wid = MakeId(id ? id : "modal", 90);
    WidgetState& ws = st.widget(wid);
    st.modalOpen = true;
    st.setInputBlockedWindowed(*this, true);
    ++st.modalDepth;

    if (ws.expanded.empty()) ws.expanded.push_back(true);
    if (st.input && !st.inputBlocked && st.input->KeyPressed(Key::Escape)) ws.expanded[0] = false;
    const bool open = ws.expanded[0];
    if (!open) {
        --st.modalDepth;
        if (st.modalDepth <= 0) {
            st.modalDepth = 0;
            st.modalOpen = false;
            if (st.modalDrivenBlock) st.setInputBlockedWindowed(*this, false);
        }
        return false;
    }

    const f32 w = size.x, h = size.y;
    const Rect box{st.screen.Center().x - w * 0.5f, st.screen.Center().y - h * 0.5f, w, h};
    st.modalRect = box;

    if (st.r2d) {
        Renderer2D& r = *st.r2d;
        r.Save();
        r.ResetClip();
        r.ClipRect(st.screen.x, st.screen.y, st.screen.w, st.screen.h);
        r.FillRect(st.screen, theme_.overlay);
        r.FillRoundedRect({box.x + 3, box.y + 5, box.w, box.h}, theme_.rounding, theme_.shadow);
        r.FillRoundedRect(box, theme_.rounding, theme_.panel);
        r.StrokeRoundedRect(box, theme_.rounding, theme_.accent, theme_.borderWidth);
        if (title && theme_.font)
            r.DrawText(*theme_.font, title, box.Center().x, box.y + theme_.padding + 4.0f, theme_.text,
                       theme_.titleSize * 0.8f, TextAlign::Center, TextBaseline::Top, 0.0f);
        r.Restore();
    }

    const f32 titleH = title ? theme_.titleSize * 1.1f : 0.0f;
    BeginPanel(id ? id : "modalbody", Rect{box.x + theme_.padding, box.y + theme_.padding + titleH,
                                           box.w - theme_.padding * 2.0f,
                                           box.h - theme_.padding * 2.0f - titleH},
               false);
    return true;
}

void UiContext::EndModal() {
    UiImpl& st = ui_internal::CtxState(*this);
    EndPanel();
    if (st.modalDepth > 0) --st.modalDepth;
    if (st.modalDepth <= 0) {
        st.modalDepth = 0;
        st.modalOpen = false;
        if (st.modalDrivenBlock) st.setInputBlockedWindowed(*this, false);
    }
}

bool UiContext::BeginPopup(const char* id, const Vec2& position, const Vec2& size) {
    UiImpl& st = ui_internal::CtxState(*this);
    const UiId wid = MakeId(id ? id : "popup", 91);
    if (st.activeDropdown != kUiIdNone && st.activeDropdown != wid) return false;
    auto it = st.popups.find(wid);
    if (it == st.popups.end() || !it->second.open) return false;
    PopupState& p = it->second;
    p.generation = st.frameGen;
    p.position = position;
    p.size = size;
    p.rect = Rect{position.x, position.y, size.x, size.y};
    st.popupOpen = true;
    st.popupId = wid;
    st.activeDropdown = wid;
    st.inputBlocked = st.inputBlocked;  // попапы не блокируют ввод жёстко

    if (st.input && !st.inputBlocked) {
        const Input& in = *st.input;
        if (in.KeyPressed(Key::Escape)) {
            st.closePopupById(wid);
            return false;
        }
        if (in.MousePressed() && !p.rect.Contains(in.MousePos())) {
            st.closePopupById(wid);
            return false;
        }
    }

    if (st.r2d) {
        Renderer2D& r = *st.r2d;
        r.FillRoundedRect({p.rect.x + 2, p.rect.y + 4, p.rect.w, p.rect.h}, theme_.rounding, theme_.shadow);
        r.FillRoundedRect(p.rect, theme_.rounding, theme_.panel);
        r.StrokeRoundedRect(p.rect, theme_.rounding, theme_.accent, theme_.borderWidth);
    }
    BeginPanel(id ? id : "popupbody", p.rect.Inset(theme_.padding * 0.5f), false);
    return true;
}

void UiContext::EndPopup() {
    UiImpl& st = ui_internal::CtxState(*this);
    EndPanel();
    st.popupOpen = false;
}

void UiContext::OpenPopup(const char* id) {
    UiImpl& st = ui_internal::CtxState(*this);
    const UiId wid = MakeId(id ? id : "popup", 91);
    PopupState p;
    p.open = true;
    p.openedFrame = st.frameGen;
    st.popups[wid] = p;
    st.popupOpen = true;
    st.popupId = wid;
    st.activeDropdown = wid;
}

void UiContext::ClosePopup() {
    UiImpl& st = ui_internal::CtxState(*this);
    if (st.popupId != kUiIdNone) st.closePopupById(st.popupId);
    st.activeDropdown = kUiIdNone;
    st.popupOpen = false;
}

bool UiContext::IsPopupOpen(const char* id) const {
    const UiImpl& st = ui_internal::CtxState(*this);
    const UiId wid = MakeId(id ? id : "popup", 91);
    auto it = st.popups.find(wid);
    return it != st.popups.end() && it->second.open;
}

// ---------------------------------------------------------------------------
// Тосты / отладочный оверлей
// ---------------------------------------------------------------------------
void UiContext::Toast(const std::string& message, f32 duration, const Color* color) {
    UiImpl& st = ui_internal::CtxState(*this);
    // Ограничиваем стопку, чтобы сбежавший цикл не растил её бесконечно.
    if (st.toasts.size() >= 6) st.toasts.erase(st.toasts.begin());
    ui_internal::ToastState t;
    t.message = message;
    t.remaining = duration;
    t.duration = duration;
    t.anim = 0.0f;
    if (color) {
        t.color = *color;
        t.hasColor = true;
    }
    st.toasts.push_back(std::move(t));
}

void UiContext::DrawDebugOverlay(const std::string& text) {
    UiImpl& st = ui_internal::CtxState(*this);
    char buf[512];
    const Input* in = st.input;
    std::snprintf(buf, sizeof(buf),
                  "widgets %d  active %u  hot %u  focus %u\n"
                  "dt %.2f ms (%.0f fps)  dpi %.2f  mouse %.0f,%.0f\n%s",
                  stats_.widgets, static_cast<unsigned>(st.activeId & 0xFFFFFFFFu),
                  static_cast<unsigned>(st.hoveredId & 0xFFFFFFFFu),
                  static_cast<unsigned>(st.focusedId & 0xFFFFFFFFu), st.dt * 1000.0f,
                  st.dt > 0.0f ? 1.0f / st.dt : 0.0f, dpiScale_,
                  in ? in->MousePos().x : 0.0f, in ? in->MousePos().y : 0.0f,
                  text.empty() ? "" : text.c_str());
    st.debugText = buf;
}

}  // namespace crossrender
