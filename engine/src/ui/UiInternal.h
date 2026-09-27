//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: внутренние помощники UI: общие структуры компоновки и виджетов для тестов.
//
#pragma once

#include "crossrender/ui/Ui.h"
#include "crossrender/core/Base.h"
#include "crossrender/core/Math.h"
#include "crossrender/text/Font.h"
#include "crossrender/gfx/Texture.h"
#include "crossrender/gfx/Renderer2D.h"
#include "crossrender/platform/Window.h"

#include <memory>
#include <string>
#include <vector>
#include <cstddef>
#include <functional>
#include <unordered_map>

namespace crossrender {
namespace ui_internal {

// WidgetState — замороженная публичная структура; реализация хранит нужную ей
// учётную информацию (трогали ли виджет в этом кадре) в производном типе,
// лежащем в map каждого контекста. State() выдаёт базовый подобъект.
struct WidgetStateEx : WidgetState {
    bool used = false;     // зарегистрирован/тронут в текущем кадре
    bool animInit = false; // значение анимации задано в первом кадре
};
// `ui_internal::UiImplOf` объявлен (и дружественен) в crossrender/ui/Ui.h, потому
// что UiContext должен дать ему доступ к своему приватному указателю `impl_`;
// определение живёт в Ui.cpp.

// ---------------------------------------------------------------------------
// Решатель flex
// ---------------------------------------------------------------------------
// Одноосный flex-решатель. `sizes` — запрос каждого элемента, `intrinsic` —
// измеренный размер содержимого для SizeMode::Content, `available` — протяжённость
// контейнера по главной оси, `spacing` — зазор между соседними элементами.
//
// Правила (задокументированы, точные):
//   * Fixed   -> value
//   * Percent -> value * (available - totalSpacing), т.е. доля от пространства,
//                реально пригодного для содержимого. Доли внутри строки
//                поэтому суммируются точно в размер контейнера.
//   * Content -> intrinsic[i]
//   * Grow    -> остаток пространства (available - spacing - fixed/percent/
//                content), поделённый поровну между всеми элементами Grow.
//   * Каждый результат ограничивается снизу нулём (переполнение не уходит в минус).
// `out` всегда получает sizes.size() элементов.
void SolveFlex(const std::vector<LayoutSize>& sizes, const std::vector<f32>& intrinsic, f32 available,
               f32 spacing, std::vector<f32>* out);

// Полная протяжённость решённой раскладки по главной оси, включая spacing.
f32 FlexTotal(const std::vector<f32>& solved, f32 spacing);
// Свободное пространство внутри `available` (отрицательно при переполнении).
f32 FlexLeftover(const std::vector<f32>& solved, f32 available, f32 spacing);

// ---------------------------------------------------------------------------
// Прокрутка
// ---------------------------------------------------------------------------
// Ограничивает смещение прокрутки так же, как BeginScrollView. Результат всегда
// в [0, max(0, content - view)]. Без std::max для float, чтобы точный IEEE
// результат был предсказуем для тестов.
f32 ClampScroll(f32 offset, f32 content, f32 view);

// Интеграция инерции: `offset` обновляется на `velocity*dt`, скорость
// затухает с экспоненциальным трением и останавливается, став ничтожной.
void StepScroll(f32* offset, f32* velocity, f32 content, f32 view, f32 dt, f32 friction);

// ---------------------------------------------------------------------------
// Математика редактирования текста (чистая: ширины приходят из колбэка, так что
// тесты могут подставить синтетическую метрику без реального шрифта).
// ---------------------------------------------------------------------------
// Смещения в байтах позиций каретки, являющихся корректными точками вставки.
// Элемент i — позиция после i кодовых символов, поэтому вектор всегда
// начинается с 0 и заканчивается text.size().
void CaretPositions(const std::string& text, std::vector<usize>* out);

// Возвращает наибольшее число ведущих кодовых символов `text`, чья измеренная
// ширина <= x. Рассматриваются `count` позиций [0..text.size()].
usize CaretIndexFromWidth(const std::string& text, f32 x,
                          const std::function<f32(const std::string&)>& widthFn);
// Смещение в байтах каретки, ближайшей к `x` (клик по правой половине глифа
// прыгает на позицию после него).
usize CaretIndexFromClick(const std::string& text, f32 x,
                          const std::function<f32(const std::string&)>& widthFn);

// Сдвигает каретку на один кодовый символ влево/вправо (с учётом UTF-8,
// последовательность никогда не разрезается).
usize CaretPrev(const std::string& text, usize index);
usize CaretNext(const std::string& text, usize index);
// Home / End логической строки, содержащей `index`.
usize LineStart(const std::string& text, usize index);
usize LineEnd(const std::string& text, usize index);
// Навигация по словам (семантика Ctrl+стрелок): `<` / `>`.
usize WordLeft(const std::string& text, usize index);
usize WordRight(const std::string& text, usize index);
// Каретка в том же столбце на визуальную строку выше/ниже (TextArea).
usize LineUp(const std::string& text, usize index, const std::function<f32(f32)>& xToCaret);
usize LineDown(const std::string& text, usize index, const std::function<f32(f32)>& xToCaret);

// Нормализованный (первый,последний) выбор в смещениях байтов; `last` может равняться `first`.
void NormalizeSelection(int selectionStart, int selectionEnd, usize* first, usize* last);

// Удаляет кодовый символ перед `index` (2 байта для кириллицы, 4 для эмодзи,
// 1 для ASCII). Возвращает новый индекс каретки. При index 0 ничего не делает.
usize BackspaceAt(std::string* text, usize index);
// Удаляет кодовый символ в `index`. Возвращает (неизменённый) индекс каретки.
usize DeleteAt(std::string* text, usize index);

// Ограничивает индекс каретки диапазоном [0, text.size()] по границе кодового символа.
usize ClampCaret(const std::string& text, usize index);

// Добавляет один кодовый символ UTF-32 как UTF-8 (для Input::TextInput()).
void AppendCodepointUtf8(std::string* out, u32 codepoint);
// Вставляет UTF-8 текст в позицию каретки.
void InsertUtf8(std::string* out, usize index, const std::string& utf8);

// ---------------------------------------------------------------------------
// Процедурная 9-patch графика (только CPU, проверяемо без GL)
// ---------------------------------------------------------------------------
// Рисует сглаженный прямоугольник со скруглёнными углами в плотно упакованные
// пиксели RGBA8. Результат — корректный 9-patch: тексели углового `radius` —
// единственные несущие кривизну, прямые края и центр плоские, поэтому
// текстуру можно растягивать через NinePatch::Uniform(radius + borderWidth).
void GenerateRoundedRectPixels(int size, f32 radius, const Color& fill, const Color& border,
                               f32 borderWidth, std::vector<u8>* outRgba);
// Покрытие (0..1) скруглённого прямоугольника в центре пикселя (px, py) с данным
// полуразмером и радиусом угла. Точное знаковое расстояние, сглаживание 1px.
f32 RoundedRectCoverage(f32 px, f32 py, f32 halfW, f32 halfH, f32 radius);

// ---------------------------------------------------------------------------
// Идентификаторы
// ---------------------------------------------------------------------------
UiId Fnv1a(const char* str, usize len, int index);

// Стабильные суффиксы id виджетов. Виджеты подмешивают индекс в хеш, чтобы два
// виджета с одинаковой меткой в одной области оставались различимы; тесты
// используют эти помощники, чтобы адресовать состояние виджета без дублирования констант.
enum WidgetIdIndex : int {
    kIdButton = 10,
    kIdIconButton = 11,
    kIdToggle = 12,
    kIdCheckbox = 13,
    kIdRadio = 14,
    kIdSlider = 20,
    kIdDragFloat = 21,
    kIdProgress = 22,
    kIdTextField = 30,
    kIdTextArea = 31,
    kIdScrollbar = 32,
    kIdList = 40,
    kIdDropdown = 50,
    kIdHeader = 60,
    kIdTabs = 61,
    kIdColor = 70,
    kIdImageButton = 80,
    kIdModal = 90,
    kIdPopup = 91,
};
inline UiId ScrollViewId(UiContext& ctx, const char* id) { return ctx.MakeId(id, 4); }
inline UiId ButtonId(UiContext& ctx, const char* label) { return ctx.MakeId(label, kIdButton); }
inline UiId TextFieldId(UiContext& ctx, const char* id) { return ctx.MakeId(id, kIdTextField); }
inline UiId ListId(UiContext& ctx, const char* id) { return ctx.MakeId(id, kIdList); }
inline UiId CheckboxId(UiContext& ctx, const char* label) { return ctx.MakeId(label, kIdCheckbox); }
inline UiId SliderId(UiContext& ctx, const char* label) { return ctx.MakeId(label, kIdSlider); }

// ---------------------------------------------------------------------------
// Состояние реализации кадра
// ---------------------------------------------------------------------------
struct LayoutContainer {
    enum class Kind : u8 { Panel, Row, Column, ScrollView };
    Kind kind = Kind::Column;
    LayoutDir dir = LayoutDir::Vertical;
    Rect rect{};
    Rect contentRect{};  // rect минус padding (view rect для scroll view)
    f32 padding = 0;
    f32 spacing = 6;
    f32 cursor = 0;  // курсор главной оси, относительно contentRect.origin
    f32 crossCursor = 0;
    int items = 0;
    f32 extent = 0;       // протяжённость по главной оси, покрытая выделенными слотами
    f32 extentCross = 0;  // наибольшая поперечная протяжённость из виденных
    f32 scrollExtent = 0;
    // Запрос каждого элемента, записанный Alloc, чтобы одноосная flex-строка
    // пересчитывалась при добавлении нового слота (см. UiContext::Alloc).
    std::vector<LayoutSize> requests;
    std::vector<f32> intrinsics;
    std::vector<f32> solved;
    u32 clipToken = 0;
    // Состояние scroll view.
    bool scrollable = false;
    bool horizontal = false;
    f32 scrollX = 0, scrollY = 0;
    f32 velX = 0, velY = 0;
    bool dragActive = false;
    UiId id = kUiIdNone;
    Rect scrollbarRect{};  // экранные координаты, пуст, когда не рисуется
    Rect contentRectOut{}; // rect, возвращаемый BeginScrollView (пространство содержимого)
};

struct PopupState {
    bool open = false;
    Rect rect{};
    Vec2 position{};
    Vec2 size{};
    int openedFrame = -1;
    int generation = -1;  // кадр, в котором popup был начат последним
    UiId owner = kUiIdNone;
};

struct ToastState {
    std::string message;
    f32 remaining = 0;
    f32 duration = 3.0f;
    f32 anim = 0;
    Color color{};
    bool hasColor = false;
};

// Колбэк отрисовки, отложенный до прохода оверлеев (списки dropdown / combo).
struct OverlayDraw {
    int kind = 0;
    std::function<void(Renderer2D&)> draw;
    Rect hitRect{};
};

struct UiImpl {
    Renderer2D* r2d = nullptr;
    const Input* input = nullptr;
    Rect screen{};
    f32 dt = 1.0f / 60.0f;
    f32 dpi = 1.0f;
    u64 frame = 0;
    int frameGen = 0;

    UiTheme theme{};

    UiId currentId = kUiIdNone;
    UiId hotId = kUiIdNone;
    UiId activeId = kUiIdNone;
    UiId focusedId = kUiIdNone;
    UiId hoveredId = kUiIdNone;
    UiId activateId = kUiIdNone;
    UiId pendingFocus = kUiIdNone;
    bool requestFocusChange = false;
    UiId editingId = kUiIdNone;  // текстовое поле, владеющее клавиатурой в этом кадре
    int cursor = 0;

    std::vector<LayoutContainer> layouts;
    std::vector<u32> clipStack;
    u32 nextClipToken = 1;
    std::vector<f32> paddingStack;
    std::vector<f32> spacingStack;
    bool dragActiveScroll = false;

    // Виджеты, зарегистрированные в этом кадре (порядок фокуса = порядок регистрации).
    struct Reg {
        UiId id;
        Rect rect;
        bool focusable;
    };
    std::vector<Reg> widgets;
    int focusedOrder = -1;
    std::vector<UiId> dirtyWidgets;

    std::unordered_map<UiId, WidgetStateEx> states;

    // Попапы / модальные окна.
    std::unordered_map<UiId, PopupState> popups;
    bool popupOpen = false;
    UiId popupId = kUiIdNone;
    UiId activeDropdown = kUiIdNone;
    UiId hoveredDropdownItem = kUiIdNone;

    bool modalOpen = false;
    int modalDepth = 0;
    Rect modalRect{};

    // Оверлеи.
    std::string tooltip;
    std::vector<ToastState> toasts;
    std::string debugText;
    std::vector<OverlayDraw> overlayDraws;

    // Взаимодействие.
    bool inputBlocked = false;
    // Устанавливается, когда *модальная* система изменила состояние блокировки,
    // чтобы EndModal знал: можно снять блок, запрошенную самим приложением.
    bool modalDrivenBlock = false;
    bool wantsMouse = false;
    bool keyboardNav = true;
    bool keyboardNavCaptured = false;  // текстовый редактор поглотил стрелки/Tab/Enter
    bool softKeyboard = false;
    bool uiSounds = true;

    // Учёт перетаскивания в scroll view.
    Vec2 lastScrollMouse{};
    bool dragScrollThumb = false;
    f32 scrollGrabOffset = 0;

    std::vector<std::string> scopeStack;

    // Переиспользуемая черновая память (без heap-суеты каждый кадр в горячих путях).
    std::vector<f32> scratchA, scratchB;
    std::vector<usize> caretScratch;
    std::vector<u8> pixelScratch;

    // ---- хуки жизненного цикла (определены в Ui.cpp) -------------------
    void ensureFont();
    void renderOverlays();
    void closePopupById(UiId id);
    // Состояние виджета на кадр (помечает запись как используемую).
    WidgetState& widget(UiId id);
    WidgetState& touchState(UiId id);
    // SetInputBlocked — inline-сеттер в замороженном публичном заголовке и пишет
    // только член контекста, поэтому действующий флаг кадра читается из этого
    // члена здесь (модальное окно накладывает свою блокировку сверху).
    void beginInputBlock(bool appBlocked);
    // Меняет состояние блокировки от имени модальной системы.
    void setInputBlockedWindowed(UiContext& ctx, bool blocked);
    // Читает UiContext::inputBlocked_, не называя приватный член: смещение байта
    // берётся из собственного хранилища публичного сеттера SetInputBlocked,
    // поэтому это всегда согласуется с inline-сеттером в замороженном заголовке.
    static std::size_t AppBlockOffset() {
        static const std::size_t kOff = []() -> std::size_t {
            alignas(UiContext) static unsigned char probe[sizeof(UiContext)];
            auto* c = reinterpret_cast<UiContext*>(probe);
            auto before = reinterpret_cast<unsigned char*>(c);
            c->SetInputBlocked(true);
            std::size_t off = sizeof(UiContext);
            for (std::size_t i = 0; i < sizeof(UiContext); ++i)
                if (before[i] == 1) {
                    off = i;
                    break;
                }
            c->SetInputBlocked(false);
            return off;
        }();
        return kOff;
    }
    static bool AppBlockFlag(const UiContext& ctx) {
        const std::size_t off = AppBlockOffset();
        if (off >= sizeof(UiContext)) return false;
        return *reinterpret_cast<const unsigned char*>(reinterpret_cast<const unsigned char*>(&ctx) + off) != 0;
    }
    // Стандартный immediate-mode автомат нажатия/отпускания: возвращает true,
    // когда виджет отпущен внутри своего rect (клик), иначе false.
    bool interact(UiId id, const Rect& rect, bool enabled);
    UiId nextFocusable(int from, bool backwards) const;
    UiId spatialFocus(int from, Key dir) const;
    LayoutContainer* topLayout() {
        return layouts.empty() ? nullptr : &layouts.back();
    }
    void pushOverlay(std::function<void(Renderer2D&)> fn, int kind) {
        overlayDraws.push_back(OverlayDraw{kind, std::move(fn), Rect{}});
    }
};

// ---------------------------------------------------------------------------
// Доступ к состоянию UiContext, реализован в Ui.cpp. Каждая UI-единица
// трансляции добирается до состояния контекста через эту функцию.
// ---------------------------------------------------------------------------
inline UiImpl& CtxState(UiContext& ctx) { return *UiImplOf(ctx); }
inline UiImpl& CtxState(const UiContext& ctx) {
    return *UiImplOf(const_cast<UiContext&>(ctx));
}

}  // namespace ui_internal
}  // namespace crossrender

// `UiContext::Impl` достраивается здесь (см. примечание выше): unique_ptr в
// классе требует лишь тип размером с указатель, полный к моменту разрушения,
// и Ui.cpp — та единица трансляции, которая его создаёт/уничтожает.
struct crossrender::UiContext::Impl : crossrender::ui_internal::UiImpl {
    Impl() = default;
};
