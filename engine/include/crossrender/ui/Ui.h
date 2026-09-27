//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: UI немедленного режима: тема, компоновка и набор виджетов.
//
#pragma once

#include "crossrender/core/Base.h"
#include "crossrender/core/Math.h"
#include "crossrender/gfx/Texture.h"
#include "crossrender/gfx/Renderer2D.h"

#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>

namespace crossrender {

class Font;
class Input;
class Audio;
class UiContext;

namespace ui_internal {
struct UiImpl;
// Внутреннее: возвращает состояние кадра контекста (реализация разбита по
// файлам engine/src/ui/*.cpp). Объявлено для аксессора ниже.
UiImpl* UiImplOf(UiContext& ctx);
}  // namespace ui_internal

using UiId = u64;
constexpr UiId kUiIdNone = 0;

// Стабильный id из строки + необязательного индекса (FNV-1a).
UiId UiHash(const char* str, int index = 0);
UiId UiHash(const std::string& str, int index = 0);

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------
struct UiTheme {
    // Цвета
    Color bg = Color::FromARGB(0xFF14161C);
    Color panel = Color::FromARGB(0xFF1D2029);
    Color panelAlt = Color::FromARGB(0xFF252935);
    Color border = Color::FromARGB(0xFF333849);
    Color text = Color::FromARGB(0xFFE6E9F2);
    Color textDim = Color::FromARGB(0xFF9AA1B4);
    Color textDisabled = Color::FromARGB(0xFF5A6072);
    Color accent = Color::FromARGB(0xFF4C8DFF);
    Color accentHover = Color::FromARGB(0xFF6BA1FF);
    Color accentActive = Color::FromARGB(0xFF2F6FE0);
    Color success = Color::FromARGB(0xFF3FBF7F);
    Color warning = Color::FromARGB(0xFFF2B33D);
    Color danger = Color::FromARGB(0xFFE4574F);
    Color shadow = Color{0, 0, 0, 0.35f};
    Color overlay = Color{0, 0, 0, 0.55f};
    Color selection = Color::FromARGB(0x664C8DFF);
    Color scrollTrack = Color::FromARGB(0xFF1A1D25);
    Color scrollThumb = Color::FromARGB(0xFF3C4254);
    Color scrollThumbHover = Color::FromARGB(0xFF525A70);

    // Метрики
    f32 padding = 8.0f;
    f32 spacing = 6.0f;
    f32 rounding = 6.0f;
    f32 borderWidth = 1.0f;
    f32 textSize = 16.0f;
    f32 titleSize = 22.0f;
    f32 smallSize = 13.0f;
    f32 buttonHeight = 36.0f;
    f32 itemHeight = 28.0f;
    f32 scrollbarWidth = 12.0f;
    f32 shadowSize = 8.0f;
    f32 animationSpeed = 12.0f;

    Font* font = nullptr;
    Font* iconFont = nullptr;

    // Необязательные 9-patch текстуры для состояний виджетов (nullptr = процедурный вид).
    struct NinePatchStyle {
        const Texture* texture = nullptr;
        NinePatch patch;
        f32 scale = 1.0f;
        bool valid() const { return texture != nullptr && texture->Valid(); }
    };
    NinePatchStyle buttonNormal;
    NinePatchStyle buttonHover;
    NinePatchStyle buttonPressed;
    NinePatchStyle buttonDisabled;
    NinePatchStyle panelPatch;
    NinePatchStyle framePatch;
    NinePatchStyle scrollbarThumb;

    static UiTheme Dark();
    static UiTheme Light();
    static UiTheme Neon();
};

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------
enum class LayoutDir : u8 { Horizontal, Vertical };
enum class Align : u8 { Start, Center, End, Stretch };
enum class Justify : u8 { Start, Center, End, SpaceBetween, SpaceAround };
enum class SizeMode : u8 { Fixed, Content, Percent, Grow };

struct LayoutSize {
    SizeMode mode = SizeMode::Fixed;
    f32 value = 0;
    static LayoutSize Fixed(f32 v) { return {SizeMode::Fixed, v}; }
    static LayoutSize Content() { return {SizeMode::Content, 0}; }
    static LayoutSize Percent(f32 p) { return {SizeMode::Percent, p}; }
    static LayoutSize Grow() { return {SizeMode::Grow, 1}; }
};

// Якоря в стиле RectTransform для UI, независимого от разрешения.
struct Anchor {
    // Нормализованные якоря относительно прямоугольника родителя (0..1).
    Vec2 min{0, 0};
    Vec2 max{0, 0};
    // Смещения: когда min == max, прямоугольник привязан к точке и `offset`
    // задаёт его размер; иначе `offset` — отступ от каждого края якоря.
    Vec2 offset{0, 0};
    Vec2 size{100, 30};
    static Anchor TopLeft(Vec2 size, Vec2 pos = {0, 0}) {
        Anchor a;
        a.min = a.max = {0, 0};
        a.size = size;
        a.offset = pos;
        return a;
    }
    static Anchor Stretch(Vec2 inset = {0, 0}) {
        Anchor a;
        a.min = {0, 0};
        a.max = {1, 1};
        a.offset = inset;
        return a;
    }
    static Anchor Center(Vec2 size, Vec2 nudge = {0, 0}) {
        Anchor a;
        a.min = a.max = {0.5f, 0.5f};
        a.size = size;
        a.offset = nudge;
        return a;
    }
    static Anchor Bottom(Vec2 size, Vec2 nudge = {0, 0}) {
        Anchor a;
        a.min = a.max = {0.5f, 1.0f};
        a.size = size;
        a.offset = nudge;
        return a;
    }
    // Вычисляется относительно прямоугольника родителя.
    [[nodiscard]] Rect Resolve(const Rect& parent) const;
    // Вычисляется с явным коэффициентом масштаба для размера (например, масштаб экрана).
    [[nodiscard]] Rect ResolveScaled(const Rect& parent, f32 scale) const;
};

// Отступы safe-area (вырезы / скруглённые углы на мобильных).
struct SafeArea {
    f32 left = 0, top = 0, right = 0, bottom = 0;
    [[nodiscard]] Rect Apply(const Rect& r) const {
        return {r.x + left, r.y + top, r.w - left - right, r.h - top - bottom};
    }
    static SafeArea Query();
};

// ---------------------------------------------------------------------------
// Состояние виджета
// ---------------------------------------------------------------------------
struct WidgetState {
    bool hovered = false;
    bool pressed = false;
    bool active = false;
    bool focused = false;
    bool toggled = false;
    f32 hoverAnim = 0;
    f32 pressAnim = 0;
    f32 scrollX = 0, scrollY = 0;
    f32 scrollVelX = 0, scrollVelY = 0;
    f32 caretBlink = 0;
    int caretIndex = 0;
    int selectionStart = -1, selectionEnd = -1;
    f32 dragValue = 0;
    bool dragging = false;
    f32 lastClickTime = 0;
    int clickCount = 0;
    std::string editBuffer;
    std::vector<bool> expanded;
    int hoveredItem = -1;
    f32 animValue = 0;
    f32 animTarget = 0;
};

// ---------------------------------------------------------------------------
// UiContext
// ---------------------------------------------------------------------------
struct UiFrameStats {
    int widgets = 0;
    int drawCalls = 0;
    int activeWidget = 0;
    int hoveredWidget = 0;
    int focusedWidget = 0;
};

class UiContext {
public:
    UiContext();
    ~UiContext();
    UiContext(const UiContext&) = delete;
    UiContext& operator=(const UiContext&) = delete;

    void Init(Renderer2D* r2d = nullptr);
    void Shutdown();

    // Начинает кадр. `screen` — логический (масштабированный по DPI) прямоугольник экрана.
    void BeginFrame(Renderer2D& r2d, const Input& input, const Rect& screen, f32 dt);
    // Headless-перегрузка: передача nullptr допустима (компоновка и hit testing
    // работают без рендерера; вызовы отрисовки становятся no-op). Предпочитайте
    // её ссылочной форме, когда GL-контекста нет.
    void BeginFrame(Renderer2D* r2d, const Input& input, const Rect& screen, f32 dt);
    void EndFrame();
    // Вызывать после EndFrame: рисует тултипы/попапы, которые должны быть поверх.
    void RenderOverlays();

    [[nodiscard]] UiTheme& Theme() { return theme_; }
    [[nodiscard]] const UiTheme& Theme() const { return theme_; }
    void SetTheme(const UiTheme& t) { theme_ = t; }
    [[nodiscard]] Renderer2D& R2D() { return *r2d_; }
    [[nodiscard]] const Input& InputRef() const { return *input_; }
    [[nodiscard]] Rect Screen() const { return screen_; }
    [[nodiscard]] f32 DpiScale() const { return dpiScale_; }
    void SetDpiScale(f32 s) { dpiScale_ = s; }
    [[nodiscard]] f32 DeltaTime() const { return dt_; }
    [[nodiscard]] const UiFrameStats& Stats() const { return stats_; }

    // ---- помощники ввода --------------------------------------------------
    [[nodiscard]] bool IsHovered(const Rect& r) const;
    [[nodiscard]] Vec2 MousePos() const;
    [[nodiscard]] bool MouseDown(int button = 0) const;
    [[nodiscard]] bool MouseClicked(int button = 0) const;
    [[nodiscard]] bool MouseReleased(int button = 0) const;
    void SetCursor(int cursor);  // 0 стрелка, 1 рука, 2 текст, 3 изменение размера, 4 перекрестие

    // ---- id / фокус -----------------------------------------------------
    [[nodiscard]] UiId CurrentId() const { return currentId_; }
    void PushId(const char* name, int index = 0);
    void PushId(UiId id);
    void PopId();
    [[nodiscard]] UiId MakeId(const char* name, int index = 0) const;
    void SetFocus(UiId id);
    void ClearFocus();
    [[nodiscard]] UiId FocusedId() const { return focusedId_; }
    [[nodiscard]] bool IsFocused(UiId id) const { return focusedId_ == id; }
    // Навигация с клавиатуры между виджетами, зарегистрированными в этом кадре.
    void SetKeyboardNavEnabled(bool e) { keyboardNav_ = e; }
    void RequestNextFocus(bool backwards = false);
    // Перехватывает ввод мыши, чтобы геймплей не реагировал на клики по UI.
    [[nodiscard]] bool WantsMouse() const { return wantsMouse_; }
    [[nodiscard]] bool WantsKeyboard() const { return focusedId_ != kUiIdNone; }
    // Блокирует весь ввод (модальные диалоги).
    void SetInputBlocked(bool blocked) { inputBlocked_ = blocked; }

    // ---- контейнеры -----------------------------------------------------
    void BeginPanel(const char* id, const Rect& rect, bool drawBackground = true);
    void EndPanel();
    void BeginGroup(const char* id, const Rect& rect);
    void EndGroup();
    // Возвращает прокрученную высоту содержимого; завершайте вызовом EndScrollView().
    Rect BeginScrollView(const char* id, const Rect& rect, f32 contentHeight, bool horizontal = false);
    void EndScrollView();
    void BeginRow(const char* id, const Rect& rect, f32 spacing = -1);
    void BeginColumn(const char* id, const Rect& rect, f32 spacing = -1);
    void EndRow();
    void EndColumn();
    // Помощник сеточной компоновки: возвращает прямоугольник ячейки.
    Rect GridCell(const Rect& area, int columns, int rows, int col, int row, f32 spacing);
    // Flex-компоновка: выделяет следующий слот в текущей компоновке.
    Rect Alloc(f32 width, f32 height, LayoutSize size = LayoutSize::Fixed(0));
    void PushLayoutPadding(f32 pad);
    void PopLayoutPadding();
    void PushLayoutSpacing(f32 spacing);
    void PopLayoutSpacing();
    void Separator();
    void Spacer(f32 size);
    void Dummy(f32 width, f32 height);

    // ---- текст ------------------------------------------------------------
    void Text(const std::string& utf8, const Color* color = nullptr);
    void TextAt(const std::string& utf8, const Vec2& pos, const Color& color, f32 size = 0,
                TextAlign align = TextAlign::Left, TextBaseline baseline = TextBaseline::Top);
    void TextCentered(const Rect& r, const std::string& utf8, const Color& color, f32 size = 0);
    void Heading(const std::string& utf8);
    void Label(const std::string& text, const std::string& value);
    void TextWrapped(const Rect& r, const std::string& utf8, const Color& color, f32 size = 0);
    [[nodiscard]] f32 TextWidth(const std::string& utf8, f32 size = 0) const;

    // ---- виджеты ---------------------------------------------------------
    bool Button(const char* label, const Rect& rect, bool enabled = true, const char* tooltip = nullptr);
    bool ButtonStyled(const char* label, const Rect& rect, const UiTheme::NinePatchStyle& style,
                      bool enabled = true);
    bool IconButton(const char* icon, const Rect& rect, bool enabled = true,
                    const char* tooltip = nullptr);
    bool ToggleButton(const char* label, const Rect& rect, bool* value, bool enabled = true);
    bool Checkbox(const char* label, const Rect& rect, bool* value, bool enabled = true);
    bool RadioButton(const char* label, const Rect& rect, int* value, int optionValue,
                     bool enabled = true);
    bool Slider(const char* label, const Rect& rect, f32* value, f32 min, f32 max,
                const char* format = "%.2f", bool enabled = true);
    bool DragFloat(const char* label, const Rect& rect, f32* value, f32 speed = 0.01f, f32 min = 0,
                   f32 max = 0, const char* format = "%.2f", bool enabled = true);
    bool DragInt(const char* label, const Rect& rect, int* value, f32 speed = 0.1f, int min = 0,
                 int max = 0, bool enabled = true);
    bool ProgressBar(const Rect& rect, f32 fraction, const char* label = nullptr,
                     const Color* fillColor = nullptr);
    // Текстовое поле: возвращает true, когда значение изменилось. `submitted` выставляется по Enter.
    bool TextField(const char* id, const Rect& rect, std::string* text,
                   const char* placeholder = nullptr, bool password = false, bool* submitted = nullptr,
                   bool enabled = true);
    bool TextArea(const char* id, const Rect& rect, std::string* text, int maxLines = 8,
                  bool enabled = true);
    // Полоса прокрутки (отдельная). `value` в [0, page/(content+page)].
    bool Scrollbar(const char* id, const Rect& rect, f32* value, f32 pageFraction,
                   bool horizontal = false, bool enabled = true);
    // Список с выделением, навигацией с клавиатуры и прокруткой. Возвращает true,
    // когда выделение изменилось.
    bool ListView(const char* id, const Rect& rect, const std::vector<std::string>& items,
                  int* selected, f32 itemHeight = 0, bool enabled = true);
    // Список с собственной отрисовкой: `drawItem` получает (index, rect, selected).
    bool ListViewCustom(const char* id, const Rect& rect, int itemCount, int* selected,
                        const std::function<void(int, const Rect&, bool)>& drawItem,
                        f32 itemHeight = 0);
    bool Dropdown(const char* id, const Rect& rect, const std::vector<std::string>& items,
                  int* selected, bool enabled = true);
    bool ComboBox(const char* id, const Rect& rect, const std::vector<std::string>& items,
                  int* selected, const char* label = nullptr, bool enabled = true);
    bool CollapsingHeader(const char* label, const Rect& rect, bool* open, bool enabled = true);
    // Вкладки: возвращает true, когда выделение изменилось.
    bool TabBar(const char* id, const Rect& rect, const std::vector<std::string>& tabs, int* selected);
    bool ColorPicker(const char* id, const Rect& rect, Color* color);
    bool Spinner(const Rect& rect, f32 phase, const Color* color = nullptr);
    // Виджет изображения (без взаимодействия) и кнопка-изображение.
    void Image(const Texture& tex, const Rect& rect, const Color& tint = Color::White);
    bool ImageButton(const char* id, const Texture& tex, const Rect& rect, const Color& tint = Color::White);
    // Универсальный помощник отрисовки 9-patch.
    void Draw9(const Texture& tex, const Rect& rect, const NinePatch& patch, const Color& tint,
               f32 scale = 1.0f);
    // Тултип, рисуемый у курсора (вызывайте внутри виджета или после него).
    void SetTooltip(const char* text);
    void Tooltip(const char* text);
    // Помощники модальных диалогов.
    bool BeginModal(const char* id, const Vec2& size, const char* title);
    void EndModal();
    bool BeginPopup(const char* id, const Vec2& position, const Vec2& size);
    void EndPopup();
    void OpenPopup(const char* id);
    void ClosePopup();
    [[nodiscard]] bool IsPopupOpen(const char* id) const;
    void OpenDialog(const char* id) { OpenPopup(id); }

    // Уведомления (toast-сообщения).
    void Toast(const std::string& message, f32 duration = 3.0f, const Color* color = nullptr);

    // Виртуальная клавиатура / помощники экранного ввода для мобильных.
    void SetSoftKeyboardEnabled(bool e) { softKeyboard_ = e; }

    // Рисует экранный debug-оверлей (FPS, статистика, фокус).
    void DrawDebugOverlay(const std::string& text);

    // Регистрирует прямоугольник виджета для hit testing и навигации фокуса.
    void RegisterWidget(UiId id, const Rect& rect, bool focusable);

    // Состояние отдельного виджета.
    [[nodiscard]] WidgetState& State(UiId id);
    [[nodiscard]] bool IsActive(UiId id) const { return activeId_ == id; }

    // Кнопка только для взаимодействия: полный hit testing, состояния hover/active/focus и
    // определение кликов без всякой отрисовки. Используйте, когда виджет рисует
    // собственную графику (карточки меню, свои вкладки), но хочет стандартное поведение.
    bool InvisibleButton(const char* id, const Rect& rect, bool enabled = true);
    [[nodiscard]] bool IsHot(UiId id) const { return hotId_ == id; }

    // Проигрывает звук UI через аудиодвижок (click/hover/toggle).
    void SetUiSounds(bool enabled) { uiSounds_ = enabled; }
    void PlayClickSound();
    void PlayHoverSound();

    // Хуки стиля, используемые примером (9-patch графика кнопки).
    void SetButton9Patch(const Texture* normal, const Texture* hover, const Texture* pressed,
                         const NinePatch& patch);

private:
    friend ui_internal::UiImpl* ui_internal::UiImplOf(UiContext& ctx);
    struct Impl;
    std::unique_ptr<Impl> impl_;
    UiTheme theme_;
    Renderer2D* r2d_ = nullptr;
    const Input* input_ = nullptr;
    Rect screen_;
    f32 dt_ = 0, dpiScale_ = 1;
    UiId currentId_ = kUiIdNone, hotId_ = kUiIdNone, activeId_ = kUiIdNone, focusedId_ = kUiIdNone;
    std::vector<UiId> idStack_;
    UiFrameStats stats_{};
    bool wantsMouse_ = false, inputBlocked_ = false, keyboardNav_ = true, uiSounds_ = true;
    bool softKeyboard_ = false;
    std::string tooltip_;
    std::unordered_map<UiId, WidgetState> states_;
};

// ---------------------------------------------------------------------------
// Необязательные помощники
// ---------------------------------------------------------------------------
// Встроенный процедурный генератор 9-patch текстур (скруглённый прямоугольник с рамкой),
// используется, когда графика не задана.
Texture MakeRoundedRectTexture(int size, f32 radius, const Color& fill, const Color& border,
                               f32 borderWidth);

}  // namespace crossrender
