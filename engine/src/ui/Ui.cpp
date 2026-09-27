// Immediate-mode UI - контекст, идентификаторы, якоря, темы, контейнеры и
// проход оверлеев.
//
// Контракт раскладки, реализованный здесь (также задокументирован в UiInternal.h):
//   Alloc() занимает следующий слот самого внутреннего контейнера раскладки. Fixed и
//   Percent точны; Content использует внутренний размер, предоставленный
//   вызывающим; Grow получают равную долю того протяжённого по главной оси
//   пространства, которое осталось свободным к моменту их выделения. Элементы
//   никогда не записываются за пределы контейнера: слот, который не помещается,
//   ограничивается оставшимся пространством, помощники рисования пересекают каждый
//   rect с текущим клипом, а переполнившиеся строки схлопывают следующие элементы в ноль.
#include "crossrender/ui/Ui.h"

#include "crossrender/core/Log.h"
#include "crossrender/text/Font.h"
#include "crossrender/audio/Audio.h"
#include "crossrender/platform/Window.h"

#include "UiInternal.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>

// Локальный помощник альфы (вынесен из анонимного пространства имён ниже, чтобы
// проход оверлеев тоже мог его использовать).
namespace {
inline crossrender::Color Alpha(const crossrender::Color& c, crossrender::f32 a) { return {c.r, c.g, c.b, a}; }
}  // namespace

namespace crossrender {
namespace ui_internal {

UiImpl* UiImplOf(UiContext& ctx) {
    // `impl_` приватен; UiContext объявляет эту функцию дружественной (см.
    // Ui.h). Смещение — константа времени компиляции, поэтому это одна загрузка указателя.
    static const std::size_t kOffset = offsetof(UiContext, impl_);
    void* base = static_cast<void*>(&ctx);
    auto* slot = reinterpret_cast<std::unique_ptr<UiContext::Impl>*>(static_cast<char*>(base) + kOffset);
    return static_cast<UiImpl*>(slot->get());
}

// ---------------------------------------------------------------------------
// Помощники состояния кадра
// ---------------------------------------------------------------------------
void UiImpl::beginInputBlock(bool appBlocked) {
    inputBlocked = appBlocked || modalDrivenBlock;
}

void UiImpl::setInputBlockedWindowed(UiContext& ctx, bool blocked) {
    // SetInputBlocked — inline-сеттер в замороженном публичном заголовке, поэтому
    // пишем через него *и* обновляем состояние кадра (которое помощники ввода
    // читают для текущего кадра).
    ctx.SetInputBlocked(blocked);
    inputBlocked = blocked;
    modalDrivenBlock = blocked;
}

void UiImpl::ensureFont() {
    // Тема владеет своими указателями на шрифты; нулевой шрифт просто превращает
    // текстовые вызовы в no-op — именно поэтому логика раскладки работает headless в тестах.
}

WidgetState& UiImpl::widget(UiId id) {
    WidgetStateEx& ws = states[id];
    ws.used = true;
    return ws;
}

WidgetState& UiImpl::touchState(UiId id) { return widget(id); }

bool UiImpl::interact(UiId id, const Rect& rect, bool enabled) {
    WidgetState& ws = widget(id);
    const bool canInteract = enabled && input != nullptr && !inputBlocked;
    const bool hover = canInteract && rect.Contains(input->MousePos());
    ws.hovered = hover;

    if (hover && hotId == kUiIdNone) hotId = id;
    if (!canInteract) {
        if (activeId == id) activeId = kUiIdNone;
        return false;
    }

    if (hover && input->MousePressed()) activeId = id;
    if (input->MouseReleased()) {
        if (activeId == id) {
            activeId = kUiIdNone;
            // Клик засчитывает только отпускание *внутри* виджета; отпускание
            // снаружи (или увод мыши в сторону) его отменяет.
            if (hover) {
                ws.clickCount = input->MouseDoubleClick() ? 2 : 1;
                return true;
            }
        }
    }
    return false;
}

void UiImpl::closePopupById(UiId id) {
    popups.erase(id);
    if (activeDropdown == id) activeDropdown = kUiIdNone;
    if (popupId == id) {
        popupOpen = false;
        popupId = kUiIdNone;
    }
}

UiId UiImpl::nextFocusable(int from, bool backwards) const {
    const int n = static_cast<int>(widgets.size());
    if (n == 0) return kUiIdNone;
    const int step = backwards ? -1 : 1;
    const int start = from < 0 ? (backwards ? 0 : -1) : from;
    for (int k = 1; k <= n; ++k) {
        int i = ((start + step * k) % n + n) % n;
        if (widgets[static_cast<usize>(i)].focusable) return widgets[static_cast<usize>(i)].id;
    }
    return kUiIdNone;
}

UiId UiImpl::spatialFocus(int from, Key dir) const {
    if (from < 0 || from >= static_cast<int>(widgets.size())) return kUiIdNone;
    const Rect src = widgets[static_cast<usize>(from)].rect;
    const Vec2 c = src.Center();
    UiId best = kUiIdNone;
    f32 bestScore = 1e30f;
    for (usize i = 0; i < widgets.size(); ++i) {
        if (static_cast<int>(i) == from || !widgets[i].focusable) continue;
        const Vec2 o = widgets[i].rect.Center();
        const f32 dx = o.x - c.x, dy = o.y - c.y;
        f32 primary = 0, secondary = 0;
        switch (dir) {
            case Key::Left:  primary = -dx; secondary = std::fabs(dy); break;
            case Key::Right: primary = dx;  secondary = std::fabs(dy); break;
            case Key::Up:    primary = -dy; secondary = std::fabs(dx); break;
            default:         primary = dy;  secondary = std::fabs(dx); break;
        }
        if (primary <= 1.0f) continue;  // не в этом направлении
        const f32 score = primary + secondary * 2.0f;
        if (score < bestScore) {
            bestScore = score;
            best = widgets[i].id;
        }
    }
    return best;
}

void UiImpl::renderOverlays() {
    Renderer2D& r = *r2d;
    const UiTheme& th = theme;

    // Отложенные списки dropdown / combo, рисуемые поверх всего остального.
    if (!overlayDraws.empty()) {
        r.Save();
        r.ResetClip();
        r.ClipRect(screen.x, screen.y, screen.w, screen.h);
        for (usize i = 0; i < overlayDraws.size(); ++i) {
            if (overlayDraws[i].draw) overlayDraws[i].draw(r);
        }
        r.ResetClip();
        r.Restore();
        overlayDraws.clear();
    }
    editingId = kUiIdNone;
    keyboardNavCaptured = false;

    // Подсказка возле курсора, ограниченная экраном.
    if (!tooltip.empty() && th.font) {
        const f32 pad = th.padding * 0.75f;
        f32 w = MeasureText(*th.font, tooltip, th.smallSize).width + pad * 2.0f;
        f32 h = th.smallSize * 1.6f + pad;
        const Vec2 m = input ? input->MousePos() : screen.Center();
        f32 x = m.x + 16.0f, y = m.y + 20.0f;
        if (x + w > screen.Right()) x = screen.Right() - w - 4.0f;
        if (y + h > screen.Bottom()) y = m.y - h - 8.0f;
        if (x < screen.x) x = screen.x + 4.0f;
        if (y < screen.y) y = screen.y + 4.0f;
        const Rect box{x, y, w, h};
        r.FillRoundedRect({box.x + 2, box.y + 3, box.w, box.h}, th.rounding, th.shadow);
        r.FillRoundedRect(box, th.rounding, th.panel);
        r.StrokeRoundedRect(box, th.rounding, th.border, th.borderWidth);
        r.DrawText(*th.font, tooltip, box.x + pad * 0.5f, box.Center().y, th.text, th.smallSize,
                   TextAlign::Left, TextBaseline::Middle, 0.0f);
    }

    // Тосты, стопкой по низу по центру.
    if (!toasts.empty()) {
        const f32 gap = 6.0f;
        const f32 h = 40.0f;
        f32 y = screen.Bottom() - 24.0f;
        for (usize i = toasts.size(); i > 0; --i) {
            ToastState& t = toasts[i - 1];
            const f32 a = Clamp(t.anim, 0.0f, 1.0f);
            if (a <= 0.001f) continue;
            f32 w = 280.0f;
            if (th.font) w = MeasureText(*th.font, t.message, th.textSize).width + 48.0f;
            w = Clamp(w, 180.0f, screen.w * 0.8f);
            const f32 x = screen.Center().x - w * 0.5f;
            const f32 oy = (1.0f - a) * 18.0f;
            const Rect box{x, y - h + oy, w, h};
            Color bg = t.hasColor ? t.color : th.panel;
            bg.a *= a;
            r.FillRoundedRect({box.x + 2, box.y + 3, box.w, box.h}, th.rounding,
                              Alpha(th.shadow, th.shadow.a * a));
            r.FillRoundedRect(box, th.rounding, bg);
            r.StrokeRoundedRect(box, th.rounding, Alpha(th.border, a), th.borderWidth);
            if (th.font)
                r.DrawText(*th.font, t.message, box.Center().x, box.Center().y, Alpha(th.text, a),
                           th.textSize, TextAlign::Center, TextBaseline::Middle, 0.0f);
            y -= h + gap;
        }
    }

    // Отладочный оверлей (F1): панель статистики в левом верхнем углу.
    if (!debugText.empty()) {
        if (th.font) {
            const f32 pad = th.padding;
            int lines = 1;
            for (char c : debugText)
                if (c == '\n') ++lines;
            f32 w = MeasureText(*th.font, debugText, th.smallSize).width + pad * 2.0f;
            f32 h = static_cast<f32>(lines) * th.smallSize * 1.35f + pad * 1.5f;
            w = Clamp(w, 220.0f, screen.w - 16.0f);
            const Rect box{screen.x + 8.0f, screen.y + 8.0f, w, h};
            r.FillRoundedRect({box.x + 2, box.y + 3, box.w, box.h}, th.rounding, th.shadow);
            r.FillRoundedRect(box, th.rounding, Alpha(th.bg, 0.88f));
            r.StrokeRoundedRect(box, th.rounding, Alpha(th.accent, 0.55f), th.borderWidth);
            r.DrawTextBox(*th.font, debugText, box.Inset(pad * 0.5f), th.text, th.smallSize,
                          TextBreak::Word, 1.35f, TextAlign::Left, 0);
        }
        debugText.clear();
    }
    tooltip.clear();
}

}  // namespace ui_internal

using ui_internal::LayoutContainer;
using ui_internal::UiImpl;

namespace {

f32 ExpSmooth(f32 current, f32 target, f32 speed, f32 dt) {
    if (speed <= 0.0f) return target;
    return current + (target - current) * (1.0f - std::exp(-speed * dt));
}

bool PatchUsable(const UiTheme::NinePatchStyle& style) {
    return style.texture != nullptr && style.texture->Valid();
}

// Правило разрешения якоря (точное):
//
//   Точечный якорь (min == max по обеим осям)
//       size' = size * scale
//       pivot = ( min.x == 0 ? 0 : 0.5 , min.y == 0 ? 0 : (min.y == 1 ? 1 : 0.5) )
//       pos   = parent.min + min * parent.size + offset - pivot * size'
//       rect  = (pos.x, pos.y, size'.x, size'.y)
//     `offset` — сдвиг от точки якоря к углу-опоре rect'а; именно поэтому
//     Anchor::TopLeft({200,40}) попадает в левый верхний угол родителя, а
//     Anchor::Bottom({300,60}) садится вплотную к нижней кромке, при этом
//     якоря остаются относительными к родителю.
//
//   Растянутый якорь (min != max хотя бы по одной оси)
//       size' = max(0, size * scale)
//       inset = offset, применяемый симметрично к обеим сторонам оси
//       rect  = ( parent.x + min.x * parent.w + offset.x,
//                 parent.y + min.y * parent.h + offset.y,
//                 max(size'.x, (max.x - min.x) * parent.w - 2 * offset.x),
//                 max(size'.y, (max.y - min.y) * parent.h - 2 * offset.y) )
//     т.е. якорный бокс сначала сжимается inset-ом, затем расширяется до `size`
//     (размер выступает минимальной протяжённостью).
//
// `scale` умножает только явный `size`; сами якоря остаются относительными
// к rect родителя, поэтому масштабированный родитель сохраняет раскладку.
Rect ResolveAnchor(const Rect& parent, const Anchor& a, f32 scale) {
    const bool point = (a.min.x == a.max.x) && (a.min.y == a.max.y);
    if (point) {
        const Vec2 size{a.size.x * scale, a.size.y * scale};
        const f32 pivotX = a.min.x == 0.0f ? 0.0f : 0.5f;
        const f32 pivotY = a.min.y == 0.0f ? 0.0f : (a.min.y == 1.0f ? 1.0f : 0.5f);
        const f32 px = parent.x + a.min.x * parent.w + a.offset.x - pivotX * size.x;
        const f32 py = parent.y + a.min.y * parent.h + a.offset.y - pivotY * size.y;
        return {px, py, size.x, size.y};
    }
    Vec2 minSize{a.size.x * scale, a.size.y * scale};
    if (minSize.x < 0) minSize.x = 0;
    if (minSize.y < 0) minSize.y = 0;
    f32 w = (a.max.x - a.min.x) * parent.w - 2.0f * a.offset.x;
    f32 h = (a.max.y - a.min.y) * parent.h - 2.0f * a.offset.y;
    if (w < minSize.x) w = minSize.x;
    if (h < minSize.y) h = minSize.y;
    return {parent.x + a.min.x * parent.w + a.offset.x, parent.y + a.min.y * parent.h + a.offset.y, w,
            h};
}

}  // namespace

// ---------------------------------------------------------------------------
// Идентификаторы
// ---------------------------------------------------------------------------
UiId UiHash(const char* str, int index) {
    if (!str) str = "";
    return ui_internal::Fnv1a(str, std::strlen(str), index);
}

UiId UiHash(const std::string& str, int index) {
    return ui_internal::Fnv1a(str.c_str(), str.size(), index);
}

// ---------------------------------------------------------------------------
// Якоря
// ---------------------------------------------------------------------------
Rect Anchor::Resolve(const Rect& parent) const { return ResolveAnchor(parent, *this, 1.0f); }

Rect Anchor::ResolveScaled(const Rect& parent, f32 scale) const {
    return ResolveAnchor(parent, *this, scale <= 0.0f ? 1.0f : scale);
}

// ---------------------------------------------------------------------------
// Safe area
// ---------------------------------------------------------------------------
#if !defined(ENG_PLATFORM_IOS)
// На iOS реальную реализацию даёт платформенный слой
// (engine/src/platform/ios/SafeAreaIOS.cpp, собранный с weak-атрибутом, так что
// эта версия там просто опускается). Везде остальном — десктоп, Android и web —
// у окна пока нет вырезов, поэтому инсеты намеренно нулевые. Инсеты Android
// заполнит платформенный слой, когда начнёт их сообщать.
SafeArea SafeArea::Query() {
    SafeArea sa;
    sa.left = sa.top = sa.right = sa.bottom = 0.0f;
    return sa;
}
#endif

// ---------------------------------------------------------------------------
// Темы
// ---------------------------------------------------------------------------
UiTheme UiTheme::Dark() {
    UiTheme t;
    t.bg = Color::FromARGB(0xFF14161C);
    t.panel = Color::FromARGB(0xFF1D2029);
    t.panelAlt = Color::FromARGB(0xFF252935);
    t.border = Color::FromARGB(0xFF333849);
    t.text = Color::FromARGB(0xFFE6E9F2);
    t.textDim = Color::FromARGB(0xFF9AA1B4);
    t.textDisabled = Color::FromARGB(0xFF5A6072);
    t.accent = Color::FromARGB(0xFF4C8DFF);
    t.accentHover = Color::FromARGB(0xFF6BA1FF);
    t.accentActive = Color::FromARGB(0xFF2F6FE0);
    t.success = Color::FromARGB(0xFF3FBF7F);
    t.warning = Color::FromARGB(0xFFF2B33D);
    t.danger = Color::FromARGB(0xFFE4574F);
    t.scrollTrack = Color::FromARGB(0xFF1A1D25);
    t.scrollThumb = Color::FromARGB(0xFF3C4254);
    t.scrollThumbHover = Color::FromARGB(0xFF525A70);
    t.selection = Color::FromARGB(0x664C8DFF);
    t.shadow = Color{0, 0, 0, 0.35f};
    t.overlay = Color{0, 0, 0, 0.55f};
    return t;
}

UiTheme UiTheme::Light() {
    UiTheme t;
    t.bg = Color::FromARGB(0xFFF2F3F7);
    t.panel = Color::FromARGB(0xFFFFFFFF);
    t.panelAlt = Color::FromARGB(0xFFE9EBF1);
    t.border = Color::FromARGB(0xFFC7CBD6);
    t.text = Color::FromARGB(0xFF1B1F29);
    t.textDim = Color::FromARGB(0xFF5B6272);
    t.textDisabled = Color::FromARGB(0xFFA0A5B2);
    t.accent = Color::FromARGB(0xFF2F6FE0);
    t.accentHover = Color::FromARGB(0xFF4C8DFF);
    t.accentActive = Color::FromARGB(0xFF1E56BE);
    t.success = Color::FromARGB(0xFF1F9D5C);
    t.warning = Color::FromARGB(0xFFC98A12);
    t.danger = Color::FromARGB(0xFFCC4038);
    t.shadow = Color{0.1f, 0.12f, 0.18f, 0.18f};
    t.overlay = Color{0.08f, 0.09f, 0.12f, 0.35f};
    t.selection = Color::FromARGB(0x552F6FE0);
    t.scrollTrack = Color::FromARGB(0xFFE4E6EC);
    t.scrollThumb = Color::FromARGB(0xFFBCC1CD);
    t.scrollThumbHover = Color::FromARGB(0xFF9AA1B4);
    return t;
}

UiTheme UiTheme::Neon() {
    UiTheme t;
    t.bg = Color::FromARGB(0xFF07080E);
    t.panel = Color::FromARGB(0xEE0D1020);
    t.panelAlt = Color::FromARGB(0xFF141A33);
    t.border = Color::FromARGB(0x4000E5FF);
    t.text = Color::FromARGB(0xFFE6FCFF);
    t.textDim = Color::FromARGB(0xFF8FB8C4);
    t.textDisabled = Color::FromARGB(0xFF4A5A6A);
    t.accent = Color::FromARGB(0xFF00E5FF);
    t.accentHover = Color::FromARGB(0xFF6BF3FF);
    t.accentActive = Color::FromARGB(0xFF00A8C8);
    t.success = Color::FromARGB(0xFF39FF9E);
    t.warning = Color::FromARGB(0xFFFFD24A);
    t.danger = Color::FromARGB(0xFFFF3D7F);
    t.shadow = Color{0.0f, 0.9f, 1.0f, 0.16f};
    t.overlay = Color{0.02f, 0.03f, 0.08f, 0.72f};
    t.selection = Color::FromARGB(0x66FF2FD9);
    t.scrollTrack = Color::FromARGB(0xFF0A0D18);
    t.scrollThumb = Color::FromARGB(0xAAFF2FD9);
    t.scrollThumbHover = Color::FromARGB(0xFFFF6BE4);
    t.rounding = 4.0f;
    t.animationSpeed = 16.0f;
    return t;
}

// ---------------------------------------------------------------------------
// UiContext - время жизни
// ---------------------------------------------------------------------------
UiContext::UiContext() : impl_(new Impl()) {}
UiContext::~UiContext() = default;

void UiContext::Init(Renderer2D* r2d) {
    UiImpl& st = ui_internal::CtxState(*this);
    st.r2d = r2d;
    theme_ = UiTheme::Dark();
    st.theme = theme_;
    st.states.clear();
    st.layouts.clear();
    st.popups.clear();
    st.toasts.clear();
    st.tooltip.clear();
    st.frame = 0;
    st.frameGen = 0;
    st.currentId = st.hotId = st.activeId = st.focusedId = kUiIdNone;
    idStack_.clear();
    focusedId_ = hotId_ = activeId_ = currentId_ = kUiIdNone;
}

void UiContext::Shutdown() {
    UiImpl& st = ui_internal::CtxState(*this);
    st.r2d = nullptr;
    st.input = nullptr;
    st.states.clear();
    st.layouts.clear();
    st.popups.clear();
    st.toasts.clear();
    st.widgets.clear();
    st.overlayDraws.clear();
    tooltip_.clear();
}

// ---------------------------------------------------------------------------
// UiContext - кадр
// ---------------------------------------------------------------------------
void UiContext::BeginFrame(Renderer2D& r2d, const Input& input, const Rect& screen, f32 dt) {
    BeginFrame(&r2d, input, screen, dt);
}

void UiContext::BeginFrame(Renderer2D* r2d, const Input& input, const Rect& screen, f32 dt) {
    UiImpl& st = ui_internal::CtxState(*this);
    r2d_ = r2d;
    // `InputRef()` выдаёт этот указатель, поэтому обновлять его нужно каждый кадр.
    input_ = &input;
    st.r2d = r2d;
    st.input = &input;
    st.screen = screen;
    st.dt = dt > 0.0f ? dt : 1.0f / 60.0f;
    // Renderer2D не может существовать без GL-контекста, поэтому headless-вызывающие
    // передают nullptr и выполняется только раскладка/проверка попаданий.
    if (r2d != nullptr && r2d->DpiScale() > 0.0f) dpiScale_ = r2d->DpiScale();
    st.dpi = dpiScale_;
    screen_ = screen;
    dt_ = st.dt;
    ++st.frame;
    st.frameGen = static_cast<int>(st.frame);

    // Theme() — единственный источник истины: публикуем её в состояние кадра
    // до того, как её прочитает любой виджет. Правки SetTheme()/Theme(), сделанные
    // между кадрами (или в течение предыдущего), поэтому учитываются, а не
    // затираются более ранней копией.
    st.theme = theme_;
    if (!theme_.font) theme_.font = FontManager::Get().DefaultFont();
    st.ensureFont();

    st.layouts.clear();
    st.clipStack.clear();
    st.widgets.clear();
    st.scopeStack.clear();
    st.hotId = kUiIdNone;
    st.hoveredId = kUiIdNone;
    // `activateId` здесь намеренно *не* очищается: это запрос, поднятый EndFrame
    // для виджета, владеющего фокусом, и потребляемый самим виджетом
    // в следующем кадре (см. ButtonStyled).
    st.editingId = kUiIdNone;
    st.inputBlocked = false;
    st.wantsMouse = false;
    st.modalDepth = 0;
    st.modalOpen = false;
    st.modalRect = Rect{};
    st.tooltip.clear();
    st.overlayDraws.clear();
    st.paddingStack.clear();
    st.spacingStack.clear();
    st.dragActiveScroll = false;
    st.requestFocusChange = false;
    st.pendingFocus = kUiIdNone;
    st.currentId = kUiIdNone;
    currentId_ = kUiIdNone;
    wantsMouse_ = false;
    st.keyboardNavCaptured = false;

    stats_.widgets = 0;
    stats_.drawCalls = 0;
    stats_.activeWidget = 0;
    stats_.hoveredWidget = 0;
    stats_.focusedWidget = 0;

    if (r2d != nullptr) {
        r2d->ResetClip();
        r2d->ClipRect(screen.x, screen.y, screen.w, screen.h);
    }
    st.clipStack.push_back(st.nextClipToken++);

    // Засеваем флаг блокировки кадра из публичного члена, чтобы вызов приложения
    // SetInputBlocked() учитывался хотя бы в кадре, где он сделан.
    st.beginInputBlock(UiImpl::AppBlockFlag(*this));
}

void UiContext::EndFrame() {
    UiImpl& st = ui_internal::CtxState(*this);
    const Input& in = *st.input;

    // Зарегистрированные позже виджеты рисуются позже, поэтому выигрывают проверку наведения.
    if (st.hotId == kUiIdNone) {
        for (auto it = st.widgets.rbegin(); it != st.widgets.rend(); ++it) {
            if (it->rect.Contains(in.MousePos())) {
                st.hoveredId = it->id;
                break;
            }
        }
    } else {
        st.hoveredId = st.hotId;
    }
    if (st.activeId != kUiIdNone) st.hoveredId = st.activeId;

    // Сбрасываем попапы, не начатые в этом кадре.
    if (st.activeDropdown != kUiIdNone) {
        auto it = st.popups.find(st.activeDropdown);
        if (it != st.popups.end() && it->second.generation != st.frameGen) {
            if (in.KeyPressed(Key::Escape) ||
                (in.MousePressed() && !it->second.rect.Contains(in.MousePos()))) {
                st.closePopupById(st.activeDropdown);
            }
        }
    }

    // Клавиатурная навигация по виджетам, зарегистрированным в этом кадре.
    if (st.keyboardNav && !st.inputBlocked) {
        st.focusedOrder = -1;
        for (usize i = 0; i < st.widgets.size(); ++i) {
            if (st.widgets[i].id == st.focusedId) {
                st.focusedOrder = static_cast<int>(i);
                break;
            }
        }
        if (in.KeyPressed(Key::Escape)) {
            st.pendingFocus = kUiIdNone;
            st.requestFocusChange = true;
        }
        // Tab двигает фокус, если текстовый редактор не поглотил его в этом кадре.
        if (in.KeyPressed(Key::Tab) && !st.keyboardNavCaptured) {
            st.requestFocusChange = true;
            st.pendingFocus = st.nextFocusable(st.focusedOrder, in.ShiftDown());
        }
        if (!st.keyboardNavCaptured) {
            static const Key arrows[4] = {Key::Left, Key::Right, Key::Up, Key::Down};
            for (int a = 0; a < 4; ++a) {
                if (in.KeyPressed(arrows[a]) && st.focusedOrder >= 0) {
                    const UiId next = st.spatialFocus(st.focusedOrder, arrows[a]);
                    if (next) {
                        st.pendingFocus = next;
                        st.requestFocusChange = true;
                    }
                }
            }
            // Enter / Space активируют виджет в фокусе; виджет потребляет
            // запрос (см. ButtonStyled).
            if (st.focusedId != kUiIdNone &&
                (in.KeyPressed(Key::Enter) || in.KeyPressed(Key::Space)))
                st.activateId = st.focusedId;
        }
    }
    if (st.requestFocusChange) {
        if (st.activateId != kUiIdNone && st.activateId != st.pendingFocus) st.activateId = kUiIdNone;
        st.focusedId = st.pendingFocus;
        focusedId_ = st.focusedId;
    }

    // Обновляем покадровые флаги виджетов; выбрасываем исчезнувшие записи.
    for (auto it = st.states.begin(); it != st.states.end();) {
        ui_internal::WidgetStateEx& ws = it->second;
        if (!ws.used) {
            it = st.states.erase(it);
            continue;
        }
        ws.used = false;
        ws.hovered = (st.hoveredId == it->first);
        ws.focused = (st.focusedId == it->first);
        ws.active = (st.activeId == it->first);
        ws.pressed = ws.active;
        ws.hoverAnim = ExpSmooth(ws.hoverAnim, ws.hovered ? 1.0f : 0.0f, theme_.animationSpeed, st.dt);
        ws.pressAnim = ExpSmooth(ws.pressAnim, ws.active ? 1.0f : 0.0f, theme_.animationSpeed, st.dt);
        ++it;
    }

    // Тосты растворяются.
    for (usize i = 0; i < st.toasts.size();) {
        auto& t = st.toasts[i];
        t.remaining -= st.dt;
        const f32 target = t.remaining > 0.4f ? 1.0f : 0.0f;
        t.anim = ExpSmooth(t.anim, target, 8.0f, st.dt);
        if (t.remaining <= 0.0f && t.anim < 0.02f) {
            st.toasts.erase(st.toasts.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }

    hotId_ = st.hotId;
    activeId_ = st.activeId;
    currentId_ = st.currentId;
    wantsMouse_ = st.wantsMouse;
    stats_.widgets = static_cast<int>(st.widgets.size());
    stats_.drawCalls = st.r2d ? st.r2d->GetStats().drawCalls : 0;
    stats_.activeWidget = static_cast<int>(st.activeId & 0x7FFFFFFFu);
    stats_.hoveredWidget = static_cast<int>(st.hoveredId & 0x7FFFFFFFu);
    stats_.focusedWidget = static_cast<int>(st.focusedId & 0x7FFFFFFFu);
    tooltip_ = st.tooltip;
}

void UiContext::RenderOverlays() {
    UiImpl& st = ui_internal::CtxState(*this);
    if (!st.r2d) return;
    st.renderOverlays();
}

// ---------------------------------------------------------------------------
// UiContext - помощники ввода
// ---------------------------------------------------------------------------
bool UiContext::IsHovered(const Rect& r) const {
    const UiImpl& st = ui_internal::CtxState(*this);
    if (!st.input || st.inputBlocked) return false;
    // Пока открыт dropdown / popup, клики вне его поглощаются.
    if (st.popupOpen && st.activeDropdown != kUiIdNone) {
        auto it = st.popups.find(st.activeDropdown);
        if (it != st.popups.end() && it->second.open) return it->second.rect.Contains(st.input->MousePos());
    }
    return r.Contains(st.input->MousePos());
}

Vec2 UiContext::MousePos() const {
    const UiImpl& st = ui_internal::CtxState(*this);
    return st.input ? st.input->MousePos() : Vec2{};
}

bool UiContext::MouseDown(int button) const {
    const UiImpl& st = ui_internal::CtxState(*this);
    if (!st.input || st.inputBlocked) return false;
    return st.input->MouseDown(static_cast<MouseButton>(button));
}

bool UiContext::MouseClicked(int button) const {
    const UiImpl& st = ui_internal::CtxState(*this);
    if (!st.input || st.inputBlocked) return false;
    return st.input->MousePressed(static_cast<MouseButton>(button));
}

bool UiContext::MouseReleased(int button) const {
    const UiImpl& st = ui_internal::CtxState(*this);
    if (!st.input || st.inputBlocked) return false;
    return st.input->MouseReleased(static_cast<MouseButton>(button));
}

void UiContext::SetCursor(int cursor) {
    UiImpl& st = ui_internal::CtxState(*this);
    st.cursor = cursor;
}

// ---------------------------------------------------------------------------
// UiContext - id / фокус
// ---------------------------------------------------------------------------
void UiContext::PushId(const char* name, int index) {
    UiImpl& st = ui_internal::CtxState(*this);
    const UiId base = UiHash(name, index);
    UiId id = base ^ (st.currentId * 0x9E3779B97F4A7C15ULL);
    if (id == kUiIdNone) id = base | 1u;
    idStack_.push_back(st.currentId);
    st.scopeStack.push_back(name ? name : "");
    st.currentId = id;
    currentId_ = id;
}

void UiContext::PushId(UiId id) {
    UiImpl& st = ui_internal::CtxState(*this);
    idStack_.push_back(st.currentId);
    st.scopeStack.push_back(std::string());
    st.currentId = id == kUiIdNone ? 1u : id;
    currentId_ = st.currentId;
}

void UiContext::PopId() {
    UiImpl& st = ui_internal::CtxState(*this);
    if (idStack_.empty()) return;
    st.currentId = idStack_.back();
    idStack_.pop_back();
    if (!st.scopeStack.empty()) st.scopeStack.pop_back();
    currentId_ = st.currentId;
}

UiId UiContext::MakeId(const char* name, int index) const {
    const UiImpl& st = ui_internal::CtxState(*this);
    const UiId base = UiHash(name, index);
    const UiId id = base ^ (st.currentId * 0x9E3779B97F4A7C15ULL);
    return id == kUiIdNone ? (base | 1u) : id;
}

void UiContext::SetFocus(UiId id) {
    UiImpl& st = ui_internal::CtxState(*this);
    st.focusedId = id;
    focusedId_ = id;
}

void UiContext::ClearFocus() {
    UiImpl& st = ui_internal::CtxState(*this);
    st.focusedId = kUiIdNone;
    st.activeId = kUiIdNone;
    focusedId_ = kUiIdNone;
    activeId_ = kUiIdNone;
}

void UiContext::RequestNextFocus(bool backwards) {
    UiImpl& st = ui_internal::CtxState(*this);
    st.pendingFocus = st.nextFocusable(st.focusedOrder, backwards);
    st.requestFocusChange = true;
}

// ---------------------------------------------------------------------------
// UiContext - раскладка
// ---------------------------------------------------------------------------
void UiContext::PushLayoutPadding(f32 pad) {
    UiImpl& st = ui_internal::CtxState(*this);
    st.paddingStack.push_back(pad < 0 ? 0.0f : pad);
    if (!st.layouts.empty()) {
        LayoutContainer& c = st.layouts.back();
        c.padding = pad < 0 ? 0.0f : pad;
        c.contentRect = c.rect.Inset(c.padding);
    }
}

void UiContext::PopLayoutPadding() {
    UiImpl& st = ui_internal::CtxState(*this);
    if (st.paddingStack.empty()) return;
    st.paddingStack.pop_back();
    const f32 pad = st.paddingStack.empty() ? 0.0f : st.paddingStack.back();
    if (!st.layouts.empty()) {
        LayoutContainer& c = st.layouts.back();
        c.padding = pad;
        c.contentRect = c.rect.Inset(pad);
    }
}

void UiContext::PushLayoutSpacing(f32 spacing) {
    UiImpl& st = ui_internal::CtxState(*this);
    st.spacingStack.push_back(spacing < 0 ? 0.0f : spacing);
    if (!st.layouts.empty()) st.layouts.back().spacing = spacing < 0 ? 0.0f : spacing;
}

void UiContext::PopLayoutSpacing() {
    UiImpl& st = ui_internal::CtxState(*this);
    if (st.spacingStack.empty()) return;
    st.spacingStack.pop_back();
    const f32 sp = st.spacingStack.empty() ? theme_.spacing : st.spacingStack.back();
    if (!st.layouts.empty()) st.layouts.back().spacing = sp;
}

void UiContext::BeginPanel(const char* id, const Rect& rect, bool drawBackground) {
    UiImpl& st = ui_internal::CtxState(*this);
    LayoutContainer c;
    c.kind = LayoutContainer::Kind::Panel;
    c.dir = LayoutDir::Vertical;
    c.rect = rect;
    c.padding = st.paddingStack.empty() ? theme_.padding : st.paddingStack.back();
    c.spacing = st.spacingStack.empty() ? theme_.spacing : st.spacingStack.back();
    c.contentRect = rect.Inset(c.padding);
    c.id = MakeId(id, 0);
    st.layouts.push_back(c);
    PushId(c.id);

    if (!st.r2d) return;
    Renderer2D& r = *st.r2d;
    if (drawBackground) {
        if (PatchUsable(theme_.panelPatch)) {
            r.Image9(*theme_.panelPatch.texture, rect, theme_.panelPatch.patch, Color::White,
                     theme_.panelPatch.scale);
        } else {
            r.FillRoundedRect({rect.x + 2, rect.y + 3, rect.w, rect.h}, theme_.rounding, theme_.shadow);
            r.FillRoundedRect(rect, theme_.rounding, theme_.panel);
            r.StrokeRoundedRect(rect, theme_.rounding, theme_.border, theme_.borderWidth);
        }
    }
    r.Save();
    r.ClipRect(rect.x, rect.y, rect.w, rect.h);
}

void UiContext::EndPanel() {
    UiImpl& st = ui_internal::CtxState(*this);
    if (st.r2d) st.r2d->Restore();
    PopId();
    if (!st.layouts.empty()) st.layouts.pop_back();
}

void UiContext::BeginGroup(const char* id, const Rect& rect) {
    UiImpl& st = ui_internal::CtxState(*this);
    LayoutContainer c;
    c.kind = LayoutContainer::Kind::Panel;
    c.dir = LayoutDir::Vertical;
    c.rect = rect;
    c.padding = st.paddingStack.empty() ? 0.0f : st.paddingStack.back();
    c.spacing = st.spacingStack.empty() ? theme_.spacing : st.spacingStack.back();
    c.contentRect = rect.Inset(c.padding);
    c.id = MakeId(id, 1);
    st.layouts.push_back(c);
    PushId(c.id);
    if (st.r2d) {
        st.r2d->Save();
        st.r2d->ClipRect(rect.x, rect.y, rect.w, rect.h);
    }
}

void UiContext::EndGroup() {
    UiImpl& st = ui_internal::CtxState(*this);
    if (st.r2d) st.r2d->Restore();
    PopId();
    if (!st.layouts.empty()) st.layouts.pop_back();
}

void UiContext::BeginRow(const char* id, const Rect& rect, f32 spacing) {
    UiImpl& st = ui_internal::CtxState(*this);
    LayoutContainer c;
    c.kind = LayoutContainer::Kind::Row;
    c.dir = LayoutDir::Horizontal;
    c.rect = rect;
    c.padding = st.paddingStack.empty() ? 0.0f : st.paddingStack.back();
    c.spacing = spacing >= 0.0f ? spacing : (st.spacingStack.empty() ? theme_.spacing : st.spacingStack.back());
    c.contentRect = rect.Inset(c.padding);
    c.id = MakeId(id, 2);
    st.layouts.push_back(c);
    PushId(c.id);
}

void UiContext::EndRow() {
    UiImpl& st = ui_internal::CtxState(*this);
    PopId();
    if (!st.layouts.empty()) st.layouts.pop_back();
}

void UiContext::BeginColumn(const char* id, const Rect& rect, f32 spacing) {
    UiImpl& st = ui_internal::CtxState(*this);
    LayoutContainer c;
    c.kind = LayoutContainer::Kind::Column;
    c.dir = LayoutDir::Vertical;
    c.rect = rect;
    c.padding = st.paddingStack.empty() ? 0.0f : st.paddingStack.back();
    c.spacing = spacing >= 0.0f ? spacing : (st.spacingStack.empty() ? theme_.spacing : st.spacingStack.back());
    c.contentRect = rect.Inset(c.padding);
    c.id = MakeId(id, 3);
    st.layouts.push_back(c);
    PushId(c.id);
}

void UiContext::EndColumn() {
    UiImpl& st = ui_internal::CtxState(*this);
    PopId();
    if (!st.layouts.empty()) st.layouts.pop_back();
}

Rect UiContext::GridCell(const Rect& area, int columns, int rows, int col, int row, f32 spacing) {
    if (columns <= 0 || rows <= 0) return area;
    f32 cw = (area.w - spacing * static_cast<f32>(columns - 1)) / static_cast<f32>(columns);
    f32 ch = (area.h - spacing * static_cast<f32>(rows - 1)) / static_cast<f32>(rows);
    if (cw < 0) cw = 0;
    if (ch < 0) ch = 0;
    return {area.x + static_cast<f32>(col) * (cw + spacing), area.y + static_cast<f32>(row) * (ch + spacing),
            cw, ch};
}

Rect UiContext::Alloc(f32 width, f32 height, LayoutSize size) {
    UiImpl& st = ui_internal::CtxState(*this);
    LayoutContainer* c = st.topLayout();
    if (!c) return {0, 0, width, height};

    const bool horiz = c->dir == LayoutDir::Horizontal;
    const f32 avail = horiz ? c->contentRect.w : c->contentRect.h;
    const f32 gap = c->spacing > 0.0f ? c->spacing : 0.0f;

    // Внутренний (содержимый) размер: запрошенная вызывающим протяжённость — лучшая
    // доступная в immediate mode информация. Явный ноль Fixed/Grow учитывается;
    // когда протяжённость не запрошена вовсе, используется консервативное значение
    // по умолчанию, чтобы слоты по содержимому оставались видимыми.
    f32 intrinsic = horiz ? width : height;
    const bool explicitZero = (size.mode == SizeMode::Fixed || size.mode == SizeMode::Grow) &&
                              size.value == 0.0f;
    if (intrinsic <= 0.0f && size.mode != SizeMode::Fixed && !explicitZero)
        intrinsic = horiz ? 40.0f : 20.0f;

    c->requests.push_back(size);
    c->intrinsics.push_back(intrinsic);

    // Пересчитываем всю строку: fixed / percent / content точны, а grow-элементы
    // делят остаток поровну. Два прохода: измерение, затем распределение.
    ui_internal::SolveFlex(c->requests, c->intrinsics, avail, gap, &c->solved);

    const usize n = c->solved.size();
    f32 pos = 0.0f;
    for (usize i = 0; i + 1 < n; ++i) pos += c->solved[i] + gap;

    f32 main = c->solved[n - 1];
    if (main < 0) main = 0;

    f32 cross = horiz ? height : width;
    const f32 crossAvail = horiz ? c->contentRect.h : c->contentRect.w;
    if (cross <= 0 && main > 0.0f && !(size.mode == SizeMode::Fixed && size.value == 0.0f))
        cross = horiz ? 40.0f : 20.0f;
    if (cross > crossAvail) cross = crossAvail;
    if (cross < 0) cross = 0;

    Rect out;
    if (horiz)
        out = {c->contentRect.x + pos, c->contentRect.y + c->crossCursor, main, cross};
    else
        out = {c->contentRect.x + c->crossCursor, c->contentRect.y + pos, cross, main};

    ++c->items;
    c->cursor = pos + main;
    // Никогда не сообщаем протяжённость больше контейнера: излишек обрезается
    // при отрисовке, а не записывается наружу.
    c->extent = std::min(c->cursor, avail);
    c->extentCross = std::max(c->extentCross, cross);
    return out;
}

void UiContext::Separator() {
    UiImpl& st = ui_internal::CtxState(*this);
    LayoutContainer* c = st.topLayout();
    if (!c) return;
    if (c->dir == LayoutDir::Horizontal) {
        Rect r = Alloc(1.0f, c->contentRect.h, LayoutSize::Fixed(1.0f));
        if (st.r2d) st.r2d->FillRect({r.Center().x, c->contentRect.y + 2, 1.0f, c->contentRect.h - 4}, theme_.border);
    } else {
        Rect r = Alloc(c->contentRect.w, 1.0f, LayoutSize::Fixed(1.0f));
        if (st.r2d) st.r2d->FillRect({c->contentRect.x, r.Center().y, c->contentRect.w, 1.0f}, theme_.border);
    }
}

void UiContext::Spacer(f32 size) {
    UiImpl& st = ui_internal::CtxState(*this);
    LayoutContainer* c = st.topLayout();
    if (!c) return;
    if (c->dir == LayoutDir::Horizontal)
        Alloc(size, c->contentRect.h, LayoutSize::Fixed(size));
    else
        Alloc(c->contentRect.w, size, LayoutSize::Fixed(size));
}

void UiContext::Dummy(f32 width, f32 height) {
    UiImpl& st = ui_internal::CtxState(*this);
    LayoutContainer* c = st.topLayout();
    if (!c) return;
    if (c->dir == LayoutDir::Horizontal)
        Alloc(width, height, LayoutSize::Fixed(width));
    else
        Alloc(width, height, LayoutSize::Fixed(height));
}

// ---------------------------------------------------------------------------
// Области прокрутки
// ---------------------------------------------------------------------------
Rect UiContext::BeginScrollView(const char* id, const Rect& rect, f32 contentHeight, bool horizontal) {
    UiImpl& st = ui_internal::CtxState(*this);
    const UiId sid = MakeId(id, 4);
    WidgetState& ws = st.widget(sid);

    LayoutContainer c;
    c.kind = LayoutContainer::Kind::ScrollView;
    c.dir = LayoutDir::Vertical;
    c.rect = rect;
    c.padding = st.paddingStack.empty() ? 0.0f : st.paddingStack.back();
    c.spacing = st.spacingStack.empty() ? theme_.spacing : st.spacingStack.back();
    c.contentRect = rect.Inset(c.padding);
    c.scrollable = true;
    c.horizontal = horizontal;
    c.id = sid;
    c.scrollX = ws.scrollX;
    c.scrollY = ws.scrollY;
    c.velX = ws.scrollVelX;
    c.velY = ws.scrollVelY;

    const f32 viewH = c.contentRect.h;
    const f32 viewW = c.contentRect.w;
    const bool canScrollY = contentHeight > viewH + 0.5f;
    const bool canScrollX = horizontal && contentHeight > viewW + 0.5f;

    const Input* in = st.input;
    if (in && !st.inputBlocked) {
        const bool inside = rect.Contains(in->MousePos());
        const f32 wheel = in->ScrollDelta().y;
        if (inside && std::fabs(wheel) > 0.0f && canScrollY) {
            c.velY = 0.0f;
            c.scrollY = ui_internal::ClampScroll(c.scrollY - wheel * 40.0f, contentHeight, viewH);
            st.wantsMouse = true;
        }
        if (inside && canScrollX) {
            const f32 hw = in->ScrollDelta().x;
            if (std::fabs(hw) > 0.0f) {
                c.velX = 0.0f;
                c.scrollX = ui_internal::ClampScroll(c.scrollX - hw * 40.0f, contentHeight, viewW);
            }
        }

        // Перетаскивание касанием (с инерцией) — любое касание, начавшееся внутри области.
        f32 touchDy = 0.0f;
        bool touchDrag = false;
        for (int i = 0; i < in->TouchCount(); ++i) {
            const TouchPoint& tp = in->Touches()[i];
            if ((tp.phase == TouchPhase::Down || tp.phase == TouchPhase::Move) && rect.Contains(tp.start)) {
                touchDrag = true;
                touchDy = tp.pos.y - tp.start.y;
            }
        }

        // Перетаскивание мышью: нажатие внутри объявляет виджет активным.
        if (in->MousePressed() && inside && canScrollY) {
            st.activeId = sid;
            activeId_ = sid;
            st.dragActiveScroll = true;
            st.lastScrollMouse = in->MousePos();
            st.wantsMouse = true;
        }
        if (st.dragActiveScroll && st.activeId == sid && in->MouseDown() && canScrollY) {
            const f32 dy = in->MousePos().y - st.lastScrollMouse.y;
            st.lastScrollMouse = in->MousePos();
            c.scrollY = ui_internal::ClampScroll(c.scrollY - dy, contentHeight, viewH);
            c.velY = Clamp(-dy / st.dt, -6000.0f, 6000.0f);
            st.wantsMouse = true;
        } else if (st.dragActiveScroll && in->MouseReleased()) {
            st.dragActiveScroll = false;
            if (st.activeId == sid) {
                st.activeId = kUiIdNone;
                activeId_ = kUiIdNone;
            }
        }
        if (touchDrag && canScrollY) {
            c.scrollY = ui_internal::ClampScroll(c.scrollY - touchDy * 0.05f, contentHeight, viewH);
            st.wantsMouse = true;
        }
        if (inside) st.wantsMouse = true;
    }

    ui_internal::StepScroll(&c.scrollY, &c.velY, contentHeight, viewH, st.dt, 4.5f);
    if (canScrollX)
        ui_internal::StepScroll(&c.scrollX, &c.velX, contentHeight, viewW, st.dt, 4.5f);
    else {
        c.scrollX = 0;
        c.velX = 0;
    }
    ws.scrollX = c.scrollX;
    ws.scrollY = c.scrollY;
    ws.scrollVelX = c.velX;
    ws.scrollVelY = c.velY;

    // Полоса прокрутки.
    if (canScrollY && theme_.scrollbarWidth > 0.0f) {
        const f32 bw = theme_.scrollbarWidth;
        const Rect track{rect.Right() - bw, rect.y, bw, rect.h};
        f32 thumbH = viewH * (viewH / contentHeight);
        if (thumbH < 24.0f) thumbH = 24.0f;
        const f32 maxScroll = contentHeight - viewH;
        const f32 frac = maxScroll > 0 ? c.scrollY / maxScroll : 0.0f;
        const Rect thumb{track.x + 2.0f, track.y + frac * (track.h - thumbH), bw - 4.0f, thumbH};
        const bool hot = in && !st.inputBlocked && track.Contains(in->MousePos());
        if (st.r2d) {
            Renderer2D& r = *st.r2d;
            r.FillRoundedRect(track, bw * 0.5f, theme_.scrollTrack);
            r.FillRoundedRect(thumb, (bw - 4.0f) * 0.5f, hot ? theme_.scrollThumbHover : theme_.scrollThumb);
        }
        if (hot) st.wantsMouse = true;
        if (hot && in->MousePressed()) {
            st.activeId = sid;
            activeId_ = sid;
            if (!thumb.Contains(in->MousePos())) {
                const bool after = in->MousePos().y > thumb.Center().y;
                c.scrollY = ui_internal::ClampScroll(c.scrollY + (after ? viewH : -viewH), contentHeight, viewH);
                ws.scrollY = c.scrollY;
                st.dragActiveScroll = true;
                st.lastScrollMouse = in->MousePos();
            } else {
                st.dragScrollThumb = true;
                st.scrollGrabOffset = in->MousePos().y - thumb.y;
            }
        }
        if (st.dragScrollThumb && st.activeId == sid) {
            if (in->MouseDown()) {
                const f32 dy = in->MousePos().y - st.scrollGrabOffset - track.y;
                const f32 t = Clamp(dy / (track.h - thumbH + 0.0001f), 0.0f, 1.0f);
                c.scrollY = ui_internal::ClampScroll(t * maxScroll, contentHeight, viewH);
                ws.scrollY = c.scrollY;
                st.wantsMouse = true;
            } else {
                st.dragScrollThumb = false;
            }
        }
    }

    c.scrollExtent = contentHeight;
    c.extent = contentHeight;
    c.contentRectOut = Rect{rect.x + c.padding - c.scrollX, rect.y + c.padding - c.scrollY,
                            rect.w - c.padding * 2.0f + (canScrollX ? contentHeight - viewW : 0.0f),
                            contentHeight};
    if (c.contentRectOut.w < 0) c.contentRectOut.w = 0;
    st.layouts.push_back(c);
    PushId(sid);

    if (st.r2d) {
        Renderer2D& r = *st.r2d;
        r.Save();
        r.ClipRect(rect.x, rect.y, rect.w, rect.h);
        r.Translate(-c.scrollX, -c.scrollY);
    }
    return c.contentRectOut;
}

void UiContext::EndScrollView() {
    UiImpl& st = ui_internal::CtxState(*this);
    if (st.r2d) st.r2d->Restore();
    PopId();
    if (!st.layouts.empty()) st.layouts.pop_back();
}

// ---------------------------------------------------------------------------
// Текст
// ---------------------------------------------------------------------------
void UiContext::Text(const std::string& utf8, const Color* color) {
    UiImpl& st = ui_internal::CtxState(*this);
    LayoutContainer* c = st.topLayout();
    const f32 size = theme_.textSize;
    const f32 w = TextWidth(utf8, size);
    const f32 h = size * 1.35f;
    Rect r;
    if (c)
        r = (c->dir == LayoutDir::Horizontal) ? Alloc(w, h, LayoutSize::Content())
                                              : Alloc(c->contentRect.w, h, LayoutSize::Fixed(h));
    else
        r = Rect{0, 0, w, h};
    if (!st.r2d || !theme_.font) return;
    st.r2d->DrawText(*theme_.font, utf8, r.x, r.Center().y, color ? *color : theme_.text, size,
                     TextAlign::Left, TextBaseline::Middle, 0.0f);
}

void UiContext::TextAt(const std::string& utf8, const Vec2& pos, const Color& color, f32 size,
                       TextAlign align, TextBaseline baseline) {
    UiImpl& st = ui_internal::CtxState(*this);
    if (!st.r2d || !theme_.font) return;
    st.r2d->DrawText(*theme_.font, utf8, pos.x, pos.y, color, size > 0 ? size : theme_.textSize, align,
                     baseline, 0.0f);
}

void UiContext::TextCentered(const Rect& r, const std::string& utf8, const Color& color, f32 size) {
    UiImpl& st = ui_internal::CtxState(*this);
    if (!st.r2d || !theme_.font) return;
    st.r2d->DrawText(*theme_.font, utf8, r.Center().x, r.Center().y, color,
                     size > 0 ? size : theme_.textSize, TextAlign::Center, TextBaseline::Middle, 0.0f);
}

void UiContext::Heading(const std::string& utf8) {
    UiImpl& st = ui_internal::CtxState(*this);
    LayoutContainer* c = st.topLayout();
    const f32 h = theme_.titleSize * 1.35f;
    Rect r;
    if (c && c->dir == LayoutDir::Vertical)
        r = Alloc(c->contentRect.w, h, LayoutSize::Fixed(h));
    else if (c)
        r = Alloc(TextWidth(utf8, theme_.titleSize), h, LayoutSize::Content());
    else
        r = Rect{0, 0, 200, h};
    if (!st.r2d || !theme_.font) return;
    st.r2d->DrawText(*theme_.font, utf8, r.x, r.Center().y, theme_.text, theme_.titleSize,
                     TextAlign::Left, TextBaseline::Middle, 0.0f);
}

void UiContext::Label(const std::string& text, const std::string& value) {
    UiImpl& st = ui_internal::CtxState(*this);
    LayoutContainer* c = st.topLayout();
    const f32 h = theme_.itemHeight;
    Rect r = c ? Alloc(c->contentRect.w, h, LayoutSize::Fixed(h)) : Rect{0, 0, 200, h};
    if (!st.r2d || !theme_.font) return;
    st.r2d->DrawText(*theme_.font, text, r.x, r.Center().y, theme_.textDim, theme_.textSize,
                     TextAlign::Left, TextBaseline::Middle, 0.0f);
    st.r2d->DrawText(*theme_.font, value, r.Right(), r.Center().y, theme_.text, theme_.textSize,
                     TextAlign::Right, TextBaseline::Middle, 0.0f);
}

void UiContext::TextWrapped(const Rect& r, const std::string& utf8, const Color& color, f32 size) {
    UiImpl& st = ui_internal::CtxState(*this);
    if (!st.r2d || !theme_.font) return;
    st.r2d->DrawTextBox(*theme_.font, utf8, r, color, size > 0 ? size : theme_.textSize, TextBreak::Word,
                        1.25f, TextAlign::Left, 0);
}

f32 UiContext::TextWidth(const std::string& utf8, f32 size) const {
    if (!theme_.font) return 0.0f;
    return MeasureText(*theme_.font, utf8, size > 0 ? size : theme_.textSize).width;
}

// ---------------------------------------------------------------------------
// Регистрация / звуки / стилизация
// ---------------------------------------------------------------------------
void UiContext::RegisterWidget(UiId id, const Rect& rect, bool focusable) {
    UiImpl& st = ui_internal::CtxState(*this);
    if (id == kUiIdNone) return;
    for (usize i = 0; i < st.widgets.size(); ++i) {
        if (st.widgets[i].id == id) {
            st.widgets[i].rect = rect;  // храним последний rect (виджеты могут двигаться)
            st.widgets[i].focusable = st.widgets[i].focusable || focusable;
            return;
        }
    }
    st.widgets.push_back({id, rect, focusable});
    st.touchState(id);
    if (st.hotId == kUiIdNone && st.input && !st.inputBlocked && rect.Contains(st.input->MousePos()))
        st.hotId = id;
}

WidgetState& UiContext::State(UiId id) {
    UiImpl& st = ui_internal::CtxState(*this);
    return st.widget(id);
}

void UiContext::PlayClickSound() {
    UiImpl& st = ui_internal::CtxState(*this);
    if (!st.uiSounds) return;
    // Аудио может быть не инициализировано: тогда каждый вызов ниже — безопасный no-op.
    Audio& a = Audio::Get();
    if (!a.Initialised()) return;
    static AudioClip clip = AudioClip::MakeTone(880.0f, 0.05f);
    PlayParams p;
    p.volume = 0.35f;
    a.Play(clip, p);
}

void UiContext::PlayHoverSound() {
    UiImpl& st = ui_internal::CtxState(*this);
    if (!st.uiSounds) return;
    Audio& a = Audio::Get();
    if (!a.Initialised()) return;
    static AudioClip clip = AudioClip::MakeTone(1320.0f, 0.03f);
    PlayParams p;
    p.volume = 0.15f;
    a.Play(clip, p);
}

void UiContext::SetButton9Patch(const Texture* normal, const Texture* hover, const Texture* pressed,
                                const NinePatch& patch) {
    theme_.buttonNormal.texture = normal;
    theme_.buttonNormal.patch = patch;
    theme_.buttonHover.texture = hover ? hover : normal;
    theme_.buttonHover.patch = patch;
    theme_.buttonPressed.texture = pressed ? pressed : normal;
    theme_.buttonPressed.patch = patch;
    theme_.buttonDisabled.texture = nullptr;
}

// ---------------------------------------------------------------------------
// Процедурная 9-patch графика
// ---------------------------------------------------------------------------
Texture MakeRoundedRectTexture(int size, f32 radius, const Color& fill, const Color& border,
                               f32 borderWidth) {
    Texture tex;
    if (size <= 0) return tex;
    std::vector<u8> pixels;
    ui_internal::GenerateRoundedRectPixels(size, radius, fill, border, borderWidth, &pixels);
    if (pixels.empty()) return tex;
    // Нужен живой GL-контекст. Без него текстура просто остаётся невалидной, и
    // вызывающие откатываются к процедурному (path-based) виду.
    tex.Create(size, size, PixelFormat::RGBA8, pixels.data(), TextureFilter::Linear,
               TextureWrap::ClampToEdge, false);
    return tex;
}

}  // namespace crossrender
