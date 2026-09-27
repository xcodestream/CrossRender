# crossrender/ui/Ui.h — UI немедленного режима: тема, компоновка и набор виджетов

Заголовок описывает всю систему интерфейса движка: палитру и метрики темы
(оформление), nine-patch арт, контейнеры компоновки (панель, группа, строка,
колонка, сетка, якоря), полный набор виджетов (кнопка, флажок, переключатель,
слайдер, поле ввода, список, полоса прокрутки, выпадающий список, прогресс,
вкладки, выбор цвета, подсказка, модальное окно) и контекст `UiContext`, который
всем этим управляет.

## Заголовок

```cpp
#include "crossrender/ui/Ui.h"
```

Один `UiContext` живёт столько же, сколько приложение, и вызывается один раз за
кадр между `BeginFrame` и `EndFrame`:

```cpp
crossrender::UiContext ui;
ui.Init(&r2d);                                   // один раз, при живом рендерере

while (running) {
    input.BeginFrame();                          // платформа обновила ввод
    ui.BeginFrame(&r2d, input, screen, dt);      // начало UI-кадра
    DrawHud(ui, screen);                         // виджеты, контейнеры, текст
    ui.EndFrame();                               // закрываем кадр, считаем статистику
    ui.RenderOverlays();                         // тултипы, тосты, списки поверх всего
}

ui.Shutdown();
```

## Обзор

`UiContext` — контекст интерфейса **немедленного режима** (immediate mode):
никакого дерева виджетов не хранится. Каждый кадр сцена заново вызывает
`ui.Button(...)`, `ui.Slider(...)` и остальные методы; сам виджет рисует себя,
проверяет попадание мыши и возвращает `true`, если пользователь что-то изменил.
Из этого следуют три правила, на которых держится весь модуль:

1. Виджеты вызываются **каждый кадр**, их нельзя «создать один раз». Порядок
   вызовов внутри кадра — это и есть порядок отрисовки: что нарисовано позже,
   то лежит выше. Поэтому попадание мыши разбирается в обратном порядке
   (`EndFrame` идёт по зарегистрированным виджетам с конца), и последний
   нарисованный виджет выигрывает.
2. Состояние живёт **по идентификатору**, а не по указателю на объект:
   `WidgetState& st = ui.State(ui.MakeId("volume"))` вернёт ту же запись и в
   следующем кадре. В `WidgetState` лежат наведение (`hovered`), нажатие
   (`pressed`, `active`), фокус (`focused`), прокрутка (`scrollX`, `scrollY`),
   каретка и выделение текста (`caretIndex`, `selectionStart`, `selectionEnd`),
   буфер редактирования (`editBuffer`) и анимированные значения (`hoverAnim`,
   `pressAnim`, `animValue`).
3. Виджет — это **функция от прямоугольника**. Прямоугольник сцена получает у
   компоновщика (`Alloc`, `GridCell`) или у якорей (`Anchor::Resolve`), а
   виджет только рисует в нём и обрабатывает ввод.

### Модель кадра

Рисовать UI можно только **между** `BeginFrame` и `EndFrame`. `BeginFrame`
очищает списки контейнеров и зарегистрированных виджетов, сбрасывает горячий
(`hot`) и наведённый (`hovered`) идентификаторы, статистику и блокировку ввода,
публикует тему и сужает отсечение до прямоугольника экрана. `EndFrame` считает
итоговое наведение, отрабатывает клавиатурную навигацию, обновляет анимации,
раскладывает накопленные тосты и публикует статистику кадра.

Порядок вызовов:

```cpp
crossrender::UiContext ui;
ui.Init(&r2d);                                     // шейдеры/рендерер уже готовы

// Кадр 1..N
ui.BeginFrame(&r2d, input, screen, dt);            // screen — логический прямоугольник
ui.BeginPanel("hud", hudRect);                     // контейнер: задаёт layout и clip
ui.Heading("Уровень 3");
if (ui.Button("Пауза", ui.Alloc(120.0f, 32.0f, crossrender::LayoutSize::Fixed(32.0f)))) {
    paused = true;                                 // клик случился именно в этом кадре
}
ui.EndPanel();
ui.EndFrame();                                     // статистика и наведение готовы здесь
ui.RenderOverlays();                               // тултипы/тосты/списки — поверх всего
```

`BeginFrame` принимает `screen` — **логический** прямоугольник (размер окна в
пикселях, делённый на `DpiScale`). Логические координаты совпадают с системой
координат `Renderer2D`: начало — левый верхний угол, ось Y растёт **вниз**.

### Компоновка: курсор и `Alloc`

`BeginPanel`, `BeginGroup`, `BeginRow`, `BeginColumn` и `BeginScrollView`
кладут на стек контейнер с прямоугольником содержимого (`rect` минус
`padding`), направлением (`LayoutDir::Horizontal` / `Vertical`), промежутком
(`spacing`) и курсором по главной оси. `Alloc(width, height, size)` занимает
**следующий слот** этого контейнера и продвигает курсор:

```cpp
ui.BeginColumn("stats", statsRect, 6.0f);                 // отступы и промежуток из темы
ui.Label("Выстрелов", "128");                             // Alloc внутри
ui.Text("Точность 64%");                                  // ещё один слот
ui.EndColumn();
```

Главная ось — та, вдоль которой раскладывает контейнер (у колонки это Y). Её
размер берётся из `LayoutSize`, а **не** из аргумента `width`/`height`:

| `LayoutSize` | Размер по главной оси |
|---|---|
| `LayoutSize::Fixed(v)` | ровно `v` (по умолчанию `Fixed(0)` — ноль) |
| `LayoutSize::Percent(p)` | `p` от свободного места строки |
| `LayoutSize::Content()` | запрошенный `width`/`height`, иначе 20 (колонка) или 40 (строка) |
| `LayoutSize::Grow()` | равная с остальными `Grow`-слотами доля остатка |

Вся линия пересчитывается при каждом `Alloc`, поэтому у `Grow`-слотов размер
зависит от того, сколько слотов уже добавлено: первый `Grow` в строке забирает
весь остаток на момент своего вызова, а каждый следующий делит остаток поровну
с предыдущими, так что после последнего слота `Grow`-элементы равны.
Поперечная ось — аргумент `width` для строки и `height` для колонки; она
обрезается размером контейнера по поперечной оси.

### Три ловушки компоновки

Их нужно знать, иначе панель собирается «не так, как написано».

**1. Третий аргумент `Alloc` по умолчанию — `LayoutSize::Fixed(0)`.** Это
значит, что `ui.Alloc(320.0f, 28.0f)` — слот **нулевой** высоты: имя размера по
главной оси не задано, а `Fixed(0)` не подпадает под запасное значение. Именно
так однажды «исчезла» панель настроек. Запрашивайте размер явно:

```cpp
ui.BeginColumn("form", formRect, 8.0f);
const crossrender::Rect collapsed = ui.Alloc(320.0f, 28.0f);                          // высота 0!
const crossrender::Rect slider   = ui.Alloc(320.0f, 28.0f, crossrender::LayoutSize::Fixed(28.0f));
const crossrender::Rect stretch  = ui.Alloc(0.0f, 28.0f, crossrender::LayoutSize::Grow());    // займёт остаток
ui.EndColumn();
```

**2. Нулевая поперечная ось подменяется запасным значением.** Если размер по
поперечной оси не задан (или отрицательный), а по главной слот не пуст, движок
подставляет 20 логических пикселей для колонки и 40 для строки, а затем
**обрезает** поперечный размер размером контейнера. Виджет молча получает не
тот прямоугольник, который просили:

```cpp
// height == 0 в колонке -> h станет 20, а не 0 и не 28.
const crossrender::Rect r = ui.Alloc(320.0f, 0.0f, crossrender::LayoutSize::Content());
// width больше контейнера -> w обрежется до ширины содержимого панели.
const crossrender::Rect clipped = ui.Alloc(4000.0f, 24.0f, crossrender::LayoutSize::Fixed(24.0f));
```

**3. `Alloc` нельзя вызывать дважды за один и тот же виджет.** Каждый вызов
занимает новый слот: повторный вызов сдвигает курсор, и все последующие
элементы «уезжают» вниз на лишнюю строку. Сохраняйте прямоугольник в
переменную, если он нужен дважды (например, для отрисовки и для подсказки).

По главной оси слот **не** обрезается: `Fixed`-элемент шире контейнера
сохраняет свой размер и просто выходит за отсечение, а следующий слот встаёт
правее (или ниже), а не поверх него.

### `##` в подписи: отображаемый текст против идентификатора

Идентификатор виджета — это `MakeId(label, index)`, то есть хеш **всей**
строки, включая часть после `##`. Отображается же только часть **до** `##`:

| Подпись | Что нарисовано | Из чего собран id |
|---|---|---|
| `"Играть"` | `Играть` | `hash("Играть")` |
| `"Играть##menu-item"` | `Играть` | `hash("Играть##menu-item")` |
| `"##card-3"` | ничего | `hash("##card-3")` |

Это позволяет отрисовать карточку своими руками (своим артом, своим текстом) и
при этом получить стандартное поведение кнопки с собственным уникальным id:

```cpp
ui.BeginGroup("menu", menuRect);
for (int i = 0; i < 4; ++i) {
    const crossrender::Rect card = ui.Alloc(260.0f, 64.0f, crossrender::LayoutSize::Fixed(64.0f));
    // "##" оставляет подпись пустой — карточку рисуем сами, id уникален.
    if (ui.InvisibleButton("##menu-card", card)) StartGame(i);
    ui.TextCentered(card, kTitles[i], ui.Theme().text, ui.Theme().titleSize);
}
ui.EndGroup();
```

Тот же контракт у `Button`, `ButtonStyled`, `ToggleButton`, `Checkbox`,
`RadioButton`, `IconButton`, `CollapsingHeader`, `Slider` и `DragFloat`. Если
подпись — динамическая строка (имя предмета, счётчик), добавляйте `##` с
постоянным суффиксом, иначе id будет меняться каждый кадр и состояние
сбросится: `ui.Checkbox((name + "##row").c_str(), ...)`.

### Кто рисует подпись сам

Эти виджеты рисуют подпись внутри себя, и **дополнительный** `ui.Text` поверх
них не нужен:

* `Button`, `ButtonStyled`, `IconButton`, `ToggleButton`;
* `Checkbox`, `RadioButton`, `CollapsingHeader`;
* `TabBar` (подписи вкладок), `Dropdown` (выбранный элемент), `ComboBox`;
* `Slider` (подпись и значение над дорожкой), `DragFloat` (подпись и значение
  по центру), `ProgressBar` (если передан `label`);
* `TextField`, `TextArea` (текст, placeholder, каретка, выделение),
  `ColorPicker` (поле HEX, причём оно само является редактируемым).

Сами ничего не подписывают `Image`, `ImageButton`, `Draw9`, `Spinner`,
`InvisibleButton`, `ProgressBar` без `label` и `ListViewCustom` — для них текст
(или арт) рисует вызывающий код.

```cpp
// Виджет подписывает себя сам — второй Text здесь был бы дублем.
if (ui.Button("Начать", playButton)) Start();
// Свой арт — подпись рисует сцена.
if (ui.InvisibleButton("##card", cardRect)) SelectCard();
ui.TextCentered(cardRect, "Новая игра", ui.Theme().text);
```

### Группы, отсечение и прокрутка

`BeginPanel` рисует фон и **сужает отсечение** до своего прямоугольника;
`BeginGroup` делает то же самое, но без фона (удобно для карточки со своим
артом). `BeginRow`/`BeginColumn` отсечение **не** меняют — они только меняют
направление компоновки, поэтому содержимое строки может вылезти за её пределы,
если не обрезать его группой.

`BeginScrollView` возвращает прямоугольник содержимого в **координатах
содержимого** (`rect` минус прокрутка) и сдвигает рисование на `-scrollY` /
`-scrollX`. Виджеты внутри прокручиваемой области нужно строить **от этого
прямоугольника через `Alloc`** — тогда отрисованное и попадание мыши совпадут.
Жёстко заданные координаты внутри `BeginScrollView` разъезжаются с попаданием:
клик ищется в непреобразованных экранных координатах.

```cpp
const crossrender::Rect content = ui.BeginScrollView("friends", view, rows * rowH);
for (int i = 0; i < rows; ++i) {
    const crossrender::Rect r = ui.Alloc(content.w - 20.0f, rowH);   // прокрутка уже учтена
    if (ui.InvisibleButton("##friend", r)) selected = i;
    ui.TextAt(names[i], {r.x + 8.0f, r.Center().y}, ui.Theme().text, ui.Theme().textSize,
              crossrender::TextAlign::Left, crossrender::TextBaseline::Middle);
}
ui.EndScrollView();
```

### Отсутствие рендерера и контекста OpenGL

`UiContext` умеет работать вообще без `Renderer2D`. `BeginFrame(nullptr, input,
screen, dt)` — легальная перегрузка: компоновка, попадание мыши, фокус,
клавиатурная навигация, прокрутка и машина состояний виджетов работают
полностью, а все вызовы рисования пропускаются. Именно так UI тестируется без
окна и видеокарты. Виджеты в этом режиме безопасны: текст и `Draw9` проверяют
указатель, а `ButtonStyled` и подобные не вызываются без рендерера — если
`UiContext` создан с `Init(nullptr)`, вызывайте только те виджеты, которые сами
себя рисуют, либо заведите инертный `Renderer2D` (`r2d.Init()` без контекста
OpenGL возвращает `true` и работает как заглушка — см. `docs/gfx/Renderer2D.md`).

Текст без рендерера или без шрифта — тихий no-op, `TextWidth` возвращает 0.
`BeginFrame` сам подставляет `FontManager::Get().DefaultFont()`, если тема
пришла с `font == nullptr`.

```cpp
// Тот же код проходит без окна и видеокарты: раскладка считается,
// попадание мыши работает, отправка геометрии пропускается.
crossrender::Renderer2D* r2d = RendererIfReady();          // может быть nullptr
ui.BeginFrame(r2d, input, screen, dt);
ui.Label("Загрузка", "42%");
ui.EndFrame();
```

### Шрифт сцены

Шрифт живёт в теме, а тема — на общем `UiContext`. Когда сцена хочет другой
шрифт (пиксельный, моноширинный), она подменяет `Theme().font` на входе и
возвращает прежнюю тему на выходе:

```cpp
void OnEnterScene(crossrender::UiContext& ui) {
    savedTheme_ = ui.Theme();                 // запомнили то, что было
    crossrender::UiTheme t = ui.Theme();
    t.font = pixelFont_;                       // свой шрифт для этой сцены
    ui.SetTheme(t);
}

void OnExitScene(crossrender::UiContext& ui) {
    ui.SetTheme(savedTheme_);                   // вернули общий шрифт
}
```

`SetTheme` виден следующему кадру: `BeginFrame` публикует тему заново перед
любым виджетом, поэтому правки `Theme()` между кадрами не теряются.

## Члены класса

### Тема и оформление

Тема задаётся один раз при входе в сцену и меняется через `SetTheme`; всё
остальное в этом разделе — её поля, палитры и nine-patch арт.

```cpp
crossrender::UiTheme ui_theme = crossrender::UiTheme::Dark();
ui_theme.padding = 10.0f;
ui_theme.spacing = 8.0f;
ui_theme.accent = crossrender::Color::FromARGB(0xFF4C8DFF);
ui.SetTheme(ui_theme);
```

#### `UiId / kUiIdNone / UiHash(const char*, int) / UiHash(const std::string&, int)`

Свободные помощники идентификаторов. `UiId` — это `u64`, `kUiIdNone == 0`.
`UiHash` считает FNV-1a по строке и подмешивает индекс, поэтому
`UiHash("row", 0) != UiHash("row", 1)`; ноль никогда не возвращается, так как
это признак «идентификатора нет».

```cpp
const crossrender::UiId play = crossrender::UiHash("play");
const crossrender::UiId play10 = crossrender::UiHash(std::string("play"), 10);
if (play != crossrender::kUiIdNone && play != play10) {
    ENG_LOGD("ui", "id кнопки: %llu", static_cast<unsigned long long>(play));
}
```

#### `UiTheme` и палитра: `bg, panel, panelAlt, border, text, textDim, textDisabled, accent, accentHover, accentActive, success, warning, danger, shadow, overlay, selection, scrollTrack, scrollThumb, scrollThumbHover`

`UiTheme` собирает в одном месте всё оформление: цвета, метрики, шрифты и
необязательный nine-patch арт. Палитра по умолчанию — тёмная, как у
`UiTheme::Dark()`.

| Поле | Назначение |
|---|---|
| `bg` | фон полей ввода, дорожек и вкладок |
| `panel` | фон панелей, поповеров и модальных окон |
| `panelAlt` | фон кнопок и наведения на строку списка |
| `border` | обычная обводка |
| `text` | основной цвет текста и активного элемента |
| `textDim` | подписи, невыбранные вкладки, второй план |
| `textDisabled` | неактивные виджеты |
| `accent` | акцент: дорожка слайдера, выбранный пункт, фокус |
| `accentHover` | акцент под курсором |
| `accentActive` | акцент в момент нажатия |
| `success` / `warning` / `danger` | семантические цвета (статусы, ошибки) |
| `shadow` | цвет и прозрачность тени под панелями и кнопками |
| `overlay` | затемнение фона под модальным окном |
| `selection` | подсветка выделенного текста |
| `scrollTrack` | фон дорожки полосы прокрутки |
| `scrollThumb` / `scrollThumbHover` | ползунок и ползунок под курсором |

```cpp
crossrender::UiTheme theme = crossrender::UiTheme::Dark();
theme.panel = crossrender::Color::FromARGB(0xFF161A24);
theme.text = crossrender::Color::FromARGB(0xFFE8ECF6);
theme.accent = crossrender::Color::FromARGB(0xFF4C8DFF);
theme.danger = crossrender::Color::FromARGB(0xFFE4574F);
theme.overlay = crossrender::Color{0, 0, 0, 0.55f};
ui.SetTheme(theme);
```

#### `f32 padding, spacing, rounding, borderWidth, textSize, titleSize, smallSize, buttonHeight, itemHeight, scrollbarWidth, shadowSize, animationSpeed`

Метрики темы. `padding` и `spacing` задают отступ содержимого и промежуток
между слотами контейнеров; `rounding` и `borderWidth` — скругление и толщину
обводки; `textSize` / `titleSize` / `smallSize` — кегли обычного текста,
заголовков и мелких подписей; `buttonHeight` и `itemHeight` — типовые высоты
кнопки и строки списка; `scrollbarWidth` — толщина полосы прокрутки;
`shadowSize` — размах тени; `animationSpeed` — скорость экспоненциального
сглаживания наведения и нажатия (в единицах 1/с, не зависит от частоты кадров).

```cpp
crossrender::UiTheme t = crossrender::UiTheme::Neon();
t.padding = 10.0f;
t.spacing = 8.0f;
t.rounding = 4.0f;
t.borderWidth = 1.0f;
t.textSize = 15.0f;
t.titleSize = 21.0f;
t.smallSize = 12.0f;
t.buttonHeight = 34.0f;
t.itemHeight = 26.0f;
t.scrollbarWidth = 10.0f;
t.shadowSize = 6.0f;
t.animationSpeed = 14.0f;
ui.SetTheme(t);
```

#### `Font* font / Font* iconFont`

Шрифт обычного текста и шрифт иконок. `font == nullptr` делает все текстовые
вызовы no-op (компоновка при этом считается), `BeginFrame` подставит шрифт по
умолчанию из `FontManager`. `iconFont` используется приложением для глифов
иконок, сам `UiContext` его не читает.

```cpp
const crossrender::UiTheme& th = ui.Theme();
if (th.font && th.font->Valid()) {
    ui.TextAt("Привет", {24.0f, 24.0f}, th.text, th.textSize);
} else {
    ENG_LOGW("ui", "шрифт темы недоступен — подписи не рисуются");
}
```

#### `UiTheme::NinePatchStyle` (`texture`, `patch`, `scale`, `valid()`) и поля `buttonNormal, buttonHover, buttonPressed, buttonDisabled, panelPatch, framePatch, scrollbarThumb`

`NinePatchStyle` описывает необязательный девятипатчевый (nine-patch) арт
виджета: текстуру, отступы нерастягиваемых краёв в `patch` и масштаб `scale`.
`valid()` возвращает `true`, только если текстура задана и загружена. Если стиль
невалиден, виджет рисуется процедурно — скруглённым прямоугольником с
градиентом, обводкой и тенью (см. `docs/gfx/Renderer2D.md`, тип `NinePatch`).
`buttonNormal` — обычное состояние кнопки, `buttonHover` — под курсором,
`buttonPressed` — нажата, `buttonDisabled` — выключена; `panelPatch` — фон
панели, `framePatch` — рамка выпадающего списка, `scrollbarThumb` —
необязательный арт ползунка.

```cpp
crossrender::UiTheme::NinePatchStyle art;
art.texture = &buttonTex;
art.patch = crossrender::NinePatch::Uniform(10.0f);
art.scale = 1.0f;                              // домножается на DpiScale вызывающим
if (art.valid()) {
    crossrender::UiTheme t = ui.Theme();
    t.buttonNormal = art;
    t.buttonHover = art;
    t.buttonPressed = art;
    ui.SetTheme(t);
}
```

#### `static UiTheme Dark() / static UiTheme Light() / static UiTheme Neon()`

Готовые палитры. `Dark` — базовая тёмная (совпадает с состоянием по
умолчанию), `Light` — светлая для интерфейса на белом фоне, `Neon` — тёмная с
бирюзовым акцентом, меньшим скруглением и более быстрой анимацией.

```cpp
// Дневная/ночная тема по настройке игрока:
ui.SetTheme(darkMode ? crossrender::UiTheme::Dark() : crossrender::UiTheme::Light());
```

### Идентификаторы, состояние и фокус

Идентификатор — адрес состояния виджета. Он собирается из подписи, индекса и
текущей области видимости, а `RegisterWidget` добавляет виджет в списки кадра.

```cpp
ui.PushId("inventory");
const crossrender::UiId rowId = ui.MakeId("row", 7);
ui.RegisterWidget(rowId, row, true);
ui.PopId();
```

#### `UiId CurrentId() const`

Возвращает текущий идентификатор области видимости — то, к чему примешивается
имя в `MakeId`. Вне `PushId` равен `kUiIdNone`.

```cpp
ui.PushId("settings");
if (ui.CurrentId() != crossrender::kUiIdNone) ENG_LOGD("ui", "мы внутри settings");
ui.PopId();
```

#### `void PushId(const char* name, int index = 0) / void PushId(UiId id) / void PopId()`

Стек областей видимости идентификаторов. Внутри `PushId("panelA")` имя `button`
даёт другой id, чем снаружи, поэтому одинаковые подписи в разных панелях не
конфликтуют. `PushId` по имени примешивает хеш имени к текущему id
(`base ^ current * 0x9E3779B97F4A7C15`), вариант с `UiId` задаёт id напрямую.
Лишний `PopId()` на пустом стеке безопасен. Все контейнеры (`BeginPanel`,
`BeginRow`, `BeginScrollView`, ...) вызывают `PushId` сами.

```cpp
for (int i = 0; i < 3; ++i) {
    ui.PushId("slot", i);            // или ui.PushId(0x1000ULL + i);
    ui.Label("Сохранение " + std::to_string(i + 1), saveNames[static_cast<crossrender::usize>(i)]);
    ui.PopId();
}
```

#### `UiId MakeId(const char* name, int index = 0) const`

Считает идентификатор от имени, индекса и текущей области видимости. Именно им
пользуются виджеты, принимающие `id`/`label`. Результат никогда не равен
`kUiIdNone`. Не вызывайте `MakeId` для динамических подписей без стабильного
суффикса `##` — см. раздел про `##` в обзоре.

```cpp
const crossrender::UiId rowId = ui.MakeId("inventory-row", 7);
ui.RegisterWidget(rowId, row, true);
```

#### `WidgetState` и `WidgetState& State(UiId id)` / `bool IsActive(UiId id) const`

`WidgetState` — вся память виджета между кадрами: `hovered`, `pressed`,
`active`, `focused`, `toggled`, `hoverAnim` / `pressAnim` / `animValue` /
`animTarget`, прокрутка (`scrollX`, `scrollY`, `scrollVelX`, `scrollVelY`),
текстовое редактирование (`caretBlink`, `caretIndex`, `selectionStart`,
`selectionEnd`, `editBuffer`), перетаскивание (`dragValue`, `dragging`),
двойной клик (`lastClickTime`, `clickCount`), прокрутка списка
(`expanded`, `hoveredItem`). `State(id)` создаёт запись при первом обращении и
**помечает её использованной в текущем кадре**; записи, к которым за кадр никто
не обратился, `EndFrame` удаляет. `IsActive` отвечает, удерживает ли виджет
нажатие прямо сейчас (то же, что `WidgetState::active`).

```cpp
const crossrender::UiId id = ui.MakeId("health-bar");
crossrender::WidgetState& st = ui.State(id);
st.animTarget = health / maxHealth;
if (ui.IsActive(id)) st.pressAnim = 1.0f;      // виджет удерживает нажатие
```

#### `void RegisterWidget(UiId id, const Rect& rect, bool focusable)`

Регистрирует виджет для попадания и клавиатурной навигации. Регистрация нужна
виджетам, которые рисует сцена: она добавляет прямоугольник в список кадра (по
нему `EndFrame` считает наведение и `Tab`/стрелки — порядок обхода), помечает
состояние использованным и, если под курсором ещё нет «горячего» виджета,
делает этот виджет горячим. Повторный вызов с тем же id **обновляет**
прямоугольник и складывает фокусность через `||` (виджет мог переехать при
прокрутке). Порядок регистрации — это порядок обхода по `Tab`.

```cpp
void DrawCard(crossrender::UiContext& ui, const crossrender::Rect& card, int index) {
    const crossrender::UiId id = ui.MakeId("card", index);
    ui.RegisterWidget(id, card, true);          // попадание + обход по Tab
    if (ui.IsHovered(card)) ui.SetCursor(1);
    if (ui.IsActive(id)) ui.SetTooltip("Удерживайте, чтобы переставить");
}
```

#### `bool IsHot(UiId id) const` / `bool IsHovered(const Rect& r) const`

`IsHot` — виджет первым получил курсор в этом кадре (побеждает последний
зарегистрированный под курсором). `IsHovered` — курсор внутри прямоугольника,
с учётом блокировки ввода: пока открыт выпадающий список, `IsHovered` истинно
только внутри его прямоугольника, поэтому клики «сквозь» список не проходят.
Обе функции возвращают `false`, если ввод заблокирован.

```cpp
const crossrender::Rect card = {40.0f, 40.0f, 260.0f, 72.0f};
if (ui.IsHot(ui.MakeId("card", 0))) ui.SetCursor(1);
if (ui.IsHovered(card)) ui.SetTooltip("Перетащите, чтобы поменять порядок");
```

#### `void SetFocus(UiId id) / void ClearFocus() / UiId FocusedId() const / bool IsFocused(UiId id) const / void SetKeyboardNavEnabled(bool e) / void RequestNextFocus(bool backwards = false)`

Фокус — один на кадр. `SetFocus` забирает клавиатуру виджету (текстовое поле
начинает принимать символы), `ClearFocus` снимает и фокус, и удержание,
`IsFocused`/`FocusedId` читают текущее значение. `SetKeyboardNavEnabled`
включает автоматический обход (`Tab`, стрелки, `Enter`/`Space`) — по умолчанию
включён; `RequestNextFocus` просит перевести фокус вручную (например, по
кнопке «Далее»), `backwards = true` идёт в обратную сторону.

```cpp
const crossrender::UiId name = ui.MakeId("name-field");
if (ui.Button("Заполнить", {40, 40, 140, 32})) ui.SetFocus(name);
if (ui.IsFocused(name)) ENG_LOGD("ui", "поле имени активно");
ui.RequestNextFocus(false);                     // как Tab
ui.SetKeyboardNavEnabled(true);
ui.ClearFocus();
```

#### `bool WantsMouse() const / bool WantsKeyboard() const / void SetInputBlocked(bool blocked)`

Шлюз между UI и игрой. `WantsMouse` истинно, если в этом кадре виджет забрал
мышь (наведение, перетаскивание ползунка или прокрутка над областью) — по нему
сцена не должна вращать камеру под интерфейсом. `WantsKeyboard` истинно, когда
у кого-то есть фокус. `SetInputBlocked(true)` выключает весь ввод в UI
(модальные диалоги; `BeginModal`/`EndModal` делают это сами). Флаг сэмплируется
в `BeginFrame`, поэтому выставляйте его **между** кадрами, а не посреди
текущего.

```cpp
if (!ui.WantsMouse() && ui.MouseDown(0)) RotateCamera(ui.MousePos());
if (ui.WantsKeyboard()) return;                 // не обрабатывать WASD
ui.SetInputBlocked(pauseMenuOpen);
```

### Компоновка и группы

Контейнер задаёт направление и отступы, `Alloc` выдаёт следующий слот, якоря
и сетка дают прямоугольники вне потока компоновки.

```cpp
ui.BeginPanel("inventory", crossrender::Anchor::Center({480.0f, 340.0f}).Resolve(screen));
ui.BeginRow("toolbar", ui.Alloc(456.0f, 32.0f, crossrender::LayoutSize::Fixed(32.0f)), 6.0f);
ui.Button("Использовать", ui.Alloc(140.0f, 30.0f, crossrender::LayoutSize::Fixed(30.0f)));
ui.Button("Выбросить", ui.Alloc(120.0f, 30.0f, crossrender::LayoutSize::Fixed(30.0f)));
ui.EndRow();
ui.EndPanel();
```

#### `enum class LayoutDir : u8` / `enum class Align : u8` / `enum class Justify : u8` / `enum class SizeMode : u8`

Перечисления компоновки. `LayoutDir` задаёт главную ось контейнера:
`Horizontal` (строка) или `Vertical` (колонка). `Align` выравнивает по
поперечной оси: `Start`, `Center`, `End`, `Stretch`. `Justify` распределяет
свободное место по главной оси: `Start`, `Center`, `End`, `SpaceBetween`
(промежутки между элементами), `SpaceAround` (промежутки и по краям).
`SizeMode` — режим размера в `LayoutSize`: `Fixed`, `Content`, `Percent`,
`Grow`.

| Значение | Смысл |
|---|---|
| `LayoutDir::Horizontal` | слоты идут слева направо |
| `LayoutDir::Vertical` | слоты идут сверху вниз |
| `Align::Stretch` | растянуть по поперечной оси |
| `Justify::SpaceBetween` | раздвинуть элементы, края прижаты |
| `Justify::SpaceAround` | раздвинуть элементы, края с отступом |
| `SizeMode::Fixed` | ровно `LayoutSize::value` |
| `SizeMode::Content` | по запрошенному контенту |
| `SizeMode::Percent` | доля свободного места строки |
| `SizeMode::Grow` | равная доля остатка |

```cpp
const crossrender::LayoutDir dir = crossrender::LayoutDir::Vertical;
const crossrender::Align cross = crossrender::Align::Stretch;
const crossrender::Justify main = crossrender::Justify::SpaceBetween;
const crossrender::SizeMode mode = crossrender::SizeMode::Grow;
if (dir == crossrender::LayoutDir::Horizontal && cross == crossrender::Align::Center) {
    ENG_LOGD("ui", "горизонтальная строка по центру, распределение %d, режим %d",
             static_cast<int>(main), static_cast<int>(mode));
}
```

#### `struct LayoutSize` (`Fixed`, `Content`, `Percent`, `Grow`, `mode`, `value`)

Запрос размера слота по главной оси. `Fixed(v)` — ровно `v`, `Content()` —
по запрошенному контенту (аргумент `width`/`height` в `Alloc`), `Percent(p)` —
доля свободного места строки, `Grow()` — равная доля остатка. Поля `mode` и
`value` можно заполнить вручную, но фабрики читаются лучше. Свободное место
считается как `available - spacing * (n - 1)`, поэтому сумма `Percent(p) == 1`
занимает строку целиком. Линия пересчитывается на каждом `Alloc`: `Grow`-слот
получает весь остаток на момент своего вызова, а следующий `Grow` делит его
пополам с ним.

```cpp
ui.BeginRow("toolbar", toolbarRect, 6.0f);
ui.Button("Назад", ui.Alloc(90.0f, 30.0f, crossrender::LayoutSize::Fixed(90.0f)));
ui.Button("Полоса", ui.Alloc(0.0f, 30.0f, crossrender::LayoutSize::Grow()));
ui.Button("25%", ui.Alloc(0.0f, 30.0f, crossrender::LayoutSize::Percent(0.25f)));
ui.Button("По тексту", ui.Alloc(0.0f, 0.0f, crossrender::LayoutSize::Content()));
ui.EndRow();
```

#### `struct Anchor` (`min`, `max`, `offset`, `size`) и `TopLeft / Stretch / Center / Bottom`

Якорь (anchor) — способ задать прямоугольник относительно родителя в
нормированных координатах `0..1`, не завися от разрешения. `min`/`max` —
нормированные границы, `offset` — сдвиг, `size` — размер. Если `min == max` по
обеим осям, прямоугольник привязан к точке, а `offset` задаёт сдвиг размера;
если `min != max` хотя бы по одной оси, `offset` работает как симметричный
врез, а `size` — как минимальный размер. Фабрики: `TopLeft(size, pos)` — от
левого верхнего угла, `Stretch(inset)` — растянуть на весь родитель с врезом,
`Center(size, nudge)` — по центру, `Bottom(size, nudge)` — по центру нижней
кромки. `Resolve(parent)` решает якорь в прямоугольник, `ResolveScaled(parent,
scale)` домножает `size` на масштаб (например, на DPI).

```cpp
const crossrender::Rect dialog = crossrender::Anchor::Center({460.0f, 260.0f}).Resolve(screen);
const crossrender::Rect dim = crossrender::Anchor::Stretch({12.0f, 12.0f}).Resolve(screen);
const crossrender::Rect hotbar = crossrender::Anchor::Bottom({520.0f, 56.0f}, {0.0f, -18.0f}).Resolve(screen);
const crossrender::Rect hud = crossrender::Anchor::TopLeft({320.0f, 40.0f}, {24.0f, 24.0f})
                          .ResolveScaled(screen, ui.DpiScale());
ui.BeginPanel("pause", dialog);
ui.EndPanel();
```

#### `struct SafeArea` (`left`, `top`, `right`, `bottom`, `Apply`, `static Query`)

Безопасная область экрана: вырез под камеру, «бровь» или скруглённые углы на
мобильных устройствах. `Apply(r)` вычитает врезы из прямоугольника,
`SafeArea::Query()` возвращает текущие врезы платформы. На настольных
платформах и в вебе врезы нулевые (реальные значения приходят только с iOS),
поэтому `Apply` там ничего не меняет.

```cpp
const crossrender::SafeArea safe = crossrender::SafeArea::Query();
const crossrender::Rect usable = safe.Apply(screen);
ui.BeginColumn("hud", usable, 4.0f);
ui.EndColumn();
if (safe.top > 0.0f) ENG_LOGD("ui", "верхний вырез: %.0f px", safe.top);
```

#### `void BeginPanel(const char* id, const Rect& rect, bool drawBackground = true) / void EndPanel()`

Открывает и закрывает панель — основной контейнер интерфейса. Панель задаёт
прямоугольник, вертикальную компоновку, отступы и промежуток из темы
(или из `PushLayoutPadding`/`PushLayoutSpacing`), рисует фон с тенью и
обводкой и **обрезает** всё содержимое своим прямоугольником. `drawBackground =
false` оставляет только компоновку и отсечение. Каждому `BeginPanel` обязан
соответствовать `EndPanel` — он снимает отсечение, область видимости id и
контейнер со стека.

```cpp
ui.BeginPanel("settings", crossrender::Anchor::Center({420.0f, 300.0f}).Resolve(screen));
ui.Heading("Настройки");
ui.Label("Разрешение", "1920x1080");
ui.EndPanel();
```

#### `void BeginGroup(const char* id, const Rect& rect) / void EndGroup()`

Группа — та же панель, но **без фона**: она задаёт отдельную область
компоновки и отсечение. Применяется, когда карточку или список рисует сцена
сама, а обрезать содержимое нужно. Как и панель, группа делает `PushId`, так
что одинаковые подписи внутри разных групп не конфликтуют.

```cpp
ui.BeginGroup("log-clip", {40.0f, 40.0f, 300.0f, 120.0f});
for (int i = 0; i < 20; ++i) {
    const crossrender::Rect row = ui.Alloc(300.0f, 24.0f, crossrender::LayoutSize::Fixed(24.0f));
    ui.TextAt("строка " + std::to_string(i), {row.x + 4.0f, row.Center().y},
              ui.Theme().text, 14.0f);
}
ui.EndGroup();
```

#### `void BeginRow(const char* id, const Rect& rect, f32 spacing = -1) / void EndRow()`

Контейнер с горизонтальной компоновкой. Отсечение **не** меняет — если
содержимое должно обрезаться, оберните строку в `BeginGroup`/`BeginPanel`.
`spacing < 0` означает «взять промежуток из `PushLayoutSpacing` или из темы».

```cpp
ui.BeginRow("toolbar", {0.0f, 0.0f, 640.0f, 36.0f}, 6.0f);
ui.Button("Назад", ui.Alloc(90.0f, 30.0f, crossrender::LayoutSize::Fixed(90.0f)));
ui.Button("Вперёд", ui.Alloc(90.0f, 30.0f, crossrender::LayoutSize::Fixed(90.0f)));
ui.EndRow();
```

#### `void BeginColumn(const char* id, const Rect& rect, f32 spacing = -1) / void EndColumn()`

Контейнер с вертикальной компоновкой — то же, что панель, но без фона и
отсечения. Удобен для формы из нескольких полей.

```cpp
ui.BeginColumn("form", formRect, 8.0f);
ui.Label("Логин", login);
ui.Label("Пароль", std::string(password.size(), '*'));
ui.EndColumn();
```

#### `Rect GridCell(const Rect& area, int columns, int rows, int col, int row, f32 spacing)`

Считает прямоугольник ячейки сетки: область `area` делится на `columns` на
`rows` с промежутками `spacing`. Если `columns <= 0` или `rows <= 0`, функция
возвращает всю область целиком. Это чистый помощник: он ничего не рисует и не
занимает слот контейнера, поэтому сетку можно строить поверх панели и
раскладывать в неё виджеты вручную.

```cpp
const crossrender::Rect grid = {40.0f, 80.0f, 480.0f, 240.0f};
for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
        const crossrender::Rect cell = ui.GridCell(grid, 3, 3, col, row, 8.0f);
        ui.Button("##cell", cell, row != 2);        // третий ряд выключен
    }
}
```

#### `Rect Alloc(f32 width, f32 height, LayoutSize size = LayoutSize::Fixed(0))`

Занимает следующий слот текущего контейнера и возвращает его прямоугольник.
Размер по главной оси берётся из `size` (см. таблицу в обзоре и ловушку про
`Fixed(0)`), поперечная ось — из `width`/`height` с запасным значением 20/40 и
обрезкой по контейнеру. Вне контейнера (до `BeginPanel` или после `EndPanel`)
возвращает `{0, 0, width, height}`, ничего не двигая. Вызывайте ровно один раз
на слот.

```cpp
ui.BeginColumn("form", formRect, 8.0f);
const crossrender::Rect row1 = ui.Alloc(320.0f, 28.0f, crossrender::LayoutSize::Fixed(28.0f));
ui.Slider("Громкость", row1, &volume, 0.0f, 1.0f, "%.0f%%");
const crossrender::Rect row2 = ui.Alloc(0.0f, 28.0f, crossrender::LayoutSize::Grow());
ui.Button("Применить", row2);
ui.EndColumn();
```

#### `void PushLayoutPadding(f32 pad) / void PopLayoutPadding() / void PushLayoutSpacing(f32 spacing) / void PopLayoutSpacing()`

Стек отступов и промежутков контейнеров. `PushLayoutPadding` меняет `padding`
текущего (и всех вложенных) контейнера и его прямоугольник содержимого,
`PushLayoutSpacing` — промежуток между слотами. Оба значения
восстанавливаются `Pop`-версиями; пустой стек означает значения из темы
(для `padding` — `theme.padding`, для `spacing` — `theme.spacing`, при пустом
стеке `Pop` сбрасывает промежуток в 0). Отрицательные значения приводятся к 0.

```cpp
ui.PushLayoutPadding(14.0f);
ui.PushLayoutSpacing(10.0f);
ui.BeginPanel("dense", {20.0f, 20.0f, 320.0f, 240.0f});
ui.Label("Режим", "Полный");
ui.EndPanel();
ui.PopLayoutSpacing();
ui.PopLayoutPadding();
```

#### `void Separator() / void Spacer(f32 size) / void Dummy(f32 width, f32 height)`

Три «невидимых» элемента компоновки. `Separator` занимает тонкий слот (1 px) и
рисует линию цветом `border` по всей ширине (или высоте) контейнера. `Spacer`
занимает пустой слот заданного размера по главной оси. `Dummy` занимает слот
заданного размера и ничего не рисует — им резервируют место под свой арт.
Без активного контейнера все три ничего не делают.

```cpp
ui.BeginRow("bar", {0, 0, 600, 40}, 8.0f);
ui.Button("Слева", ui.Alloc(100.0f, 30.0f, crossrender::LayoutSize::Fixed(100.0f)));
ui.Spacer(0.0f);                                 // гибкая распорка не нужна — растянем Grow
ui.Dummy(0.0f, 0.0f);
ui.Button("Справа", ui.Alloc(100.0f, 30.0f, crossrender::LayoutSize::Fixed(100.0f)));
ui.EndRow();
ui.BeginColumn("details", detailsRect, 6.0f);
ui.Label("Разрешение", "1920x1080");
ui.Separator();
ui.Label("Частота", "144 Гц");
ui.EndColumn();
```

### Фрейм

Кадр открывается `BeginFrame`, закрывается `EndFrame`, а `RenderOverlays`
дорисовывает то, что обязано быть поверх всего.

```cpp
ui.BeginFrame(&r2d, input, screen, dt);
DrawHud(ui, screen);
ui.EndFrame();
ui.RenderOverlays();
```

#### `UiContext() / ~UiContext()`

Конструктор создаёт внутреннее состояние, деструктор освобождает его. Копирование
запрещено — контекст владеет картой состояний виджетов и стеком компоновки,
поэтому храните его по значению в одном месте (обычно полем сцены или движка)
и передавайте по ссылке.

```cpp
struct MenuScene {
    crossrender::UiContext ui;                       // один контекст на приложение
    void Enter(crossrender::Renderer2D* r2d) { ui.Init(r2d); }
    void Leave() { ui.Shutdown(); }
};
```

#### `void Init(Renderer2D* r2d = nullptr) / void Shutdown()`

`Init` подключает рендерер, сбрасывает тему на `UiTheme::Dark()` и чистит все
накопленные состояния, попапы и тосты — после него контекст как новый.
`Shutdown` отвязывает рендерер и ввод и освобождает карты; `Init` после него
можно вызывать снова (например, при пересоздании контекста OpenGL). Рендерер
можно не передавать: `Init(nullptr)` — полностью рабочий headless-режим.

```cpp
crossrender::UiContext ui;
ui.Init(&r2d);                     // обычный запуск
// ...
ui.Shutdown();                     // контекст OpenGL потерян
ui.Init(&newR2d);                  // восстановили
```

#### `void BeginFrame(Renderer2D& r2d, const Input& input, const Rect& screen, f32 dt)`

Начинает UI-кадр. `screen` — логический прямоугольник экрана, `dt` — время
кадра в секундах (не положительное заменяется на `1/60`). Метод публикует
текущую тему, подставляет шрифт по умолчанию, сбрасывает контейнеры,
зарегистрированные виджеты, горячий/наведённый id, тултип, статистику и
блокировку ввода, сужает отсечение рендерера до экрана. Вызывайте его внутри
общего кадра `Renderer2D`, не вместо `r2d.BeginFrame`.

```cpp
ui.BeginFrame(r2d, input, crossrender::Rect{0, 0, 1280, 720}, dt);
ui.BeginPanel("hud", {16, 16, 240, 120});
ui.EndPanel();
```

#### `void BeginFrame(Renderer2D* r2d, const Input& input, const Rect& screen, f32 dt)`

Перегрузка для отсутствующего рендерера. `nullptr` — легальное значение:
компоновка, попадание мыши, фокус, прокрутка и состояние виджетов работают
полностью, вызовы рисования пропускаются. Используйте её в тестах и на
платформах без GL-контекста.

```cpp
crossrender::Renderer2D* r2d = RendererIfReady();          // может быть nullptr
ui.BeginFrame(r2d, input, screen, dt);
ui.Label("Загрузка", "42%");                        // безопасно и без рендерера
ui.EndFrame();
```

#### `void EndFrame()`

Закрывает кадр: считает итоговое наведение (последний зарегистрированный
виджет под курсором побеждает), обрабатывает `Tab`, стрелки, `Enter`/`Space` и
`Escape`, обновляет анимации наведения/нажатия, стареет тосты, удаляет
состояния виджетов, к которым за кадр никто не обратился, и публикует
`Stats()`. Дописывать виджеты после `EndFrame` нельзя — они не попадут ни в
статистику, ни в навигацию.

```cpp
ui.BeginFrame(&r2d, input, screen, dt);
DrawPauseMenu(ui);
ui.EndFrame();
ENG_LOGD("ui", "виджетов за кадр: %d", ui.Stats().widgets);
```

#### `void RenderOverlays()`

Рисует то, что обязано быть поверх всего интерфейса: отложенные списки
выпадающих списков, подсказку у курсора, всплывающие уведомления (тосты) и
отладочную панель. Вызывайте **после** `EndFrame` и до `r2d.EndFrame()`. Без
рендерера метод ничего не делает; в конце он очищает тултип, так что
подсказку нужно ставить каждый кадр заново.

```cpp
ui.EndFrame();
ui.RenderOverlays();          // список Dropdown, тултипы и тосты — поверх сцены
r2d.EndFrame();
```

#### `const UiFrameStats& Stats() const`

Статистика последнего кадра: `widgets` (сколько виджетов зарегистрировано),
`drawCalls` (вызовы отрисовки рендерера на момент `EndFrame`), а также
усечённые до 31 бита `activeWidget`, `hoveredWidget` и `focusedWidget` —
идентификаторы для отладки. Значения осмысленны только **после** `EndFrame`.

```cpp
ui.EndFrame();
const crossrender::UiFrameStats& s = ui.Stats();
if (s.widgets > 400) ENG_LOGW("ui", "слишком много виджетов: %d", s.widgets);
```

#### `Rect Screen() const / f32 DpiScale() const / void SetDpiScale(f32 s) / f32 DeltaTime() const`

Параметры текущего кадра. `Screen()` — логический прямоугольник, переданный в
`BeginFrame`; `DpiScale()` — масштаб плотности (по умолчанию берётся из
`Renderer2D::DpiScale()`), `SetDpiScale` задаёт его вручную; `DeltaTime()` —
время кадра, уже проверенное на положительность (им пользуются анимации).

```cpp
const crossrender::Rect screen = ui.Screen();
ui.SetDpiScale(1.25f);
if (ui.DpiScale() > 1.0f) ENG_LOGD("ui", "плотность %.2f, кадр %.1f мс",
                                   ui.DpiScale(), ui.DeltaTime() * 1000.0f);
```

### Ввод

Курсор, кнопки мыши, курсор-подсказка и звук интерфейса; блокировка ввода —
общий выключатель для всего перечисленного.

```cpp
if (ui.WantsMouse()) return;                      // клик забрал интерфейс
if (ui.IsHovered(hotbarSlot) && ui.MouseClicked(0)) SelectSlot(slotIndex);
if (ui.IsHovered(hotbarSlot)) ui.SetCursor(1);
```

#### `Vec2 MousePos() const / bool MouseDown(int button = 0) const / bool MouseClicked(int button = 0) const / bool MouseReleased(int button = 0) const`

Помощники ввода. `button`: 0 — левая, 1 — правая, 2 — средняя (значения
`MouseButton`). `MouseDown` — кнопка удерживается, `MouseClicked` — нажата
именно в этом кадре, `MouseReleased` — отпущена в этом кадре. Без активного
кадра или при заблокированном вводе все три возвращают `false`, а `MousePos`
возвращает `{0, 0}`.

```cpp
const crossrender::Vec2 m = ui.MousePos();
if (ui.MouseClicked(1)) OpenContextMenu(m);          // правая кнопка
if (ui.MouseDown(0) && dragTarget) MoveDragTarget(m);
if (ui.MouseReleased(0)) DropDragTarget();
```

#### `void SetCursor(int cursor)`

Просит платформу сменить курсор: 0 — стрелка, 1 — рука (ссылка), 2 — текстовый,
3 — изменение размера, 4 — перекрестие. Запрос действует один кадр, поэтому
ставьте его каждый кадр, пока курсор находится над элементом; обычно это
делают сами виджеты (`Dropdown`, `ListView`, `TextField`), а для своих
элементов — через `IsHot`/`IsHovered`.

```cpp
if (ui.IsHovered(dividerRect)) ui.SetCursor(3);      // тянем границу панели
if (ui.IsHovered(colorRect)) ui.SetCursor(4);        // пипетка
```

#### `Renderer2D& R2D() / const Input& InputRef() const`

Доступ к рендереру и вводу текущего кадра. `R2D()` — тот же указатель, что
передан в `BeginFrame` (разыменование без проверки: не вызывайте без
рендерера). `InputRef()` возвращает ввод, переданный в `BeginFrame` — удобно
для прокрутки, ввода текста и модификаторов.

```cpp
crossrender::Renderer2D& r2d = ui.R2D();
const crossrender::Input& in = ui.InputRef();
if (in.ScrollDelta().y != 0.0f) ENG_LOGD("ui", "колесо %.2f", in.ScrollDelta().y);
r2d.FillRect({0, 0, 10, 10}, ui.Theme().accent);
```

#### `void SetUiSounds(bool enabled) / void PlayClickSound() / void PlayHoverSound()`

Звуковое сопровождение интерфейса. `SetUiSounds(false)` глушит его целиком,
`PlayClickSound` и `PlayHoverSound` проигрывают короткие встроенные тона
(клик и наведение). Виджеты вызывают их сами; методы нужны для своих
элементов. Если звуковой движок не инициализирован, вызовы безопасны и ничего
не делают.

```cpp
ui.SetUiSounds(settings.uiAudio);
if (ui.InvisibleButton("##card", card)) ui.PlayClickSound();
if (ui.IsHot(ui.MakeId("##card"))) ui.PlayHoverSound();
```

#### `void SetSoftKeyboardEnabled(bool e)`

Разрешает или запрещает показ экранной клавиатуры на мобильных устройствах,
когда текстовое поле получает фокус. На настольных платформах флаг только
хранится.

```cpp
ui.SetSoftKeyboardEnabled(true);                 // мобильная сборка
ui.TextField("chat", {16, 600, 400, 36}, &message, "Сообщение");
```

### Текст

Текстовые помощники: `Text` и `Heading` участвуют в компоновке, `TextAt`,
`TextCentered` и `TextWrapped` рисуют в заданном месте, `TextWidth` измеряет.

```cpp
ui.BeginPanel("item", itemCard);
ui.Heading(item.name);
ui.Label("Урон", std::to_string(item.damage));
ui.TextWrapped({itemCard.x + 12.0f, itemCard.y + 80.0f, itemCard.w - 24.0f, 60.0f},
               item.description, ui.Theme().textDim, ui.Theme().smallSize);
ui.EndPanel();
```

#### `void Text(const std::string& utf8, const Color* color = nullptr)`

Текстовая строка как элемент компоновки: занимает слот в текущем контейнере и
рисует UTF-8 текст слева по центру слота. `color == nullptr` означает
`theme.text`. В строке берёт ширину текста (режим `Content`), в колонке —
всю ширину панели. Без рендерера или шрифта слот всё равно занимается, а
рисование пропускается.

```cpp
ui.BeginColumn("stats", statsRect, 4.0f);
ui.Text("Всего ходов: 42");
ui.Text("Ошибок: 2", &ui.Theme().danger);
ui.EndColumn();
```

#### `void TextAt(const std::string& utf8, const Vec2& pos, const Color& color, f32 size = 0, TextAlign align = TextAlign::Left, TextBaseline baseline = TextBaseline::Top)`

Текст в точке, вне компоновки. `size == 0` означает `theme.textSize`, `align` и
`baseline` задают привязку точки (`TextAlign`, `TextBaseline` из
`crossrender/gfx/Renderer2D.h`). Это основной способ подписать элемент, который сцена
рисует сама.

```cpp
ui.TextAt("+15", {240.0f, 96.0f}, ui.Theme().success, 18.0f,
          crossrender::TextAlign::Left, crossrender::TextBaseline::Middle);
ui.TextAt("3/5", {card.Right() - 8.0f, card.Center().y}, ui.Theme().textDim,
          ui.Theme().smallSize, crossrender::TextAlign::Right, crossrender::TextBaseline::Middle);
```

#### `void TextCentered(const Rect& r, const std::string& utf8, const Color& color, f32 size = 0)`

Текст по центру прямоугольника — и по горизонтали, и по вертикали. Удобно для
подписи на своей карточке или поверх изображения.

```cpp
const crossrender::Rect card = {40.0f, 120.0f, 240.0f, 64.0f};
if (ui.InvisibleButton("##menu-card", card)) StartGame();
ui.TextCentered(card, "Новая игра", ui.Theme().text, ui.Theme().titleSize);
```

#### `void Heading(const std::string& utf8)`

Заголовок раздела кеглем `theme.titleSize` цветом `theme.text`. В колонке
занимает всю ширину, в строке — ширину текста. Используйте вместо `Text` для
заголовков панелей и модальных окон.

```cpp
ui.BeginPanel("settings", settingsRect);
ui.Heading("Настройки графики");
ui.Separator();
ui.EndPanel();
```

#### `void Label(const std::string& text, const std::string& value)`

Строка «подпись — значение» высотой `theme.itemHeight`: слева текст цветом
`textDim`, справа значение цветом `text`. Классический элемент таблицы
характеристик.

```cpp
ui.Label("Разрешение", "2560x1440");
ui.Label("Частота кадров", "144");
ui.Label("Версия", "1.0.3");
```

#### `void TextWrapped(const Rect& r, const std::string& utf8, const Color& color, f32 size = 0)`

Многострочный текст с переносом по словам внутри прямоугольника. Высота не
ограничивается: лишние строки просто выйдут за `r` (текст не обрезается
автоматически), поэтому для длинных описаний оборачивайте вызов в
`BeginGroup`/`BeginPanel`. `size == 0` означает `theme.textSize`.

```cpp
ui.TextWrapped({20.0f, 400.0f, 320.0f, 120.0f},
               "Длинное описание предмета, которое не влезает в одну строку.",
               ui.Theme().textDim, 14.0f);
```

#### `f32 TextWidth(const std::string& utf8, f32 size = 0) const`

Ширина строки в логических пикселях при заданном кегле (`0` — `theme.textSize`).
Без шрифта возвращает 0. Применяется, чтобы отцентрировать кнопку по тексту
или принять решение о переносе.

```cpp
const crossrender::f32 w = ui.TextWidth("Продолжить", ui.Theme().textSize);
const crossrender::Rect btn = {screen.Center().x - w * 0.5f - 16.0f, 300.0f, w + 32.0f, 36.0f};
ui.Button("Продолжить", btn);
```

### Кнопки и переключатели

Кнопки сообщают о клике, переключатели и флажки меняют переданное значение;
принимают либо готовый контейнерный слот, либо свой прямоугольник.

```cpp
ui.BeginColumn("menu", menuRect, 10.0f);
if (ui.Button("Продолжить", ui.Alloc(280.0f, 44.0f, crossrender::LayoutSize::Fixed(44.0f)))) Resume();
ui.Checkbox("Показывать подсказки", ui.Alloc(280.0f, 24.0f), &showHints);
ui.EndColumn();
```

#### `bool Button(const char* label, const Rect& rect, bool enabled = true, const char* tooltip = nullptr)`

Основная кнопка. Возвращает `true` в тот кадр, когда пользователь отпустил
кнопку мыши **внутри** прямоугольника (нажал и увёл курсор — клика нет), либо
когда сработала клавиатурная активация (`Space`/`Enter` при фокусе). Подпись
рисует сама кнопка; `tooltip` показывается, пока курсор над ней, —
это сокращение для `IconButton`/`Button` вместо ручного `SetTooltip`.
`enabled = false` гасит кнопку и запрещает клик.

```cpp
const crossrender::Rect play = crossrender::Anchor::Center({220.0f, 44.0f}).Resolve(screen);
if (ui.Button("Играть", play)) StartNewGame();
if (ui.Button("Удалить", {40, 40, 140, 36}, saveExists, "Удалить выбранное сохранение")) {
    DeleteSave();
}
```

#### `bool ButtonStyled(const char* label, const Rect& rect, const UiTheme::NinePatchStyle& style, bool enabled = true)`

Кнопка с явно заданным nine-patch стилем. Если у стиля есть валидная текстура,
используются арт-состояния темы (`buttonPressed`/`buttonHover`), иначе —
процедурная отрисовка. Подпись, фокусная рамка и звук — как у `Button`.

```cpp
if (ui.ButtonStyled("Играть##menu-item", cardRect, ui.Theme().buttonNormal)) StartNewGame();
```

#### `bool IconButton(const char* icon, const Rect& rect, bool enabled = true, const char* tooltip = nullptr)`

Квадратная кнопка с глифом иконки вместо текста: прозрачный фон, подсветка
акцентом при наведении, обводка, подсказка. `icon` — строка, которую рисует
шрифт темы (символ или лигатура иконочного шрифта).

```cpp
const crossrender::Rect gear = {900.0f, 40.0f, 36.0f, 36.0f};
if (ui.IconButton("gear", gear, true, "Открывает настройки графики")) OpenSettings();
```

#### `bool ToggleButton(const char* label, const Rect& rect, bool* value, bool enabled = true)`

Кнопка-переключатель: слева подпись, справа «пилюля» с ползунком, которая
анимированно едет между выключенным и включённым состоянием. `*value`
переключается по клику, возврат `true` — состояние изменилось в этом кадре.
Первый кадр не анимируется: ползунок сразу встаёт в нужное положение.

```cpp
if (ui.ToggleButton("Полный экран", {40, 250, 220, 32}, &fullscreen)) {
    ApplyFullscreen(fullscreen);
}
```

#### `bool Checkbox(const char* label, const Rect& rect, bool* value, bool enabled = true)`

Флажок (checkbox): квадратик с анимированной галочкой и подпись справа.
`*value` переключается по клику; `rect` задаёт всю строку, а не только
квадратик. Возвращает `true` при изменении.

```cpp
if (ui.Checkbox("Вертикальная синхронизация", {40, 300, 260, 24}, &vsync)) {
    ENG_LOGI("video", "vsync -> %d", vsync ? 1 : 0);
}
```

#### `bool RadioButton(const char* label, const Rect& rect, int* value, int optionValue, bool enabled = true)`

Переключатель (radio) в группе: кружок с точкой и подпись. По клику
`*value = optionValue`, но только если значение действительно изменилось.
Идентификатор включает `optionValue`, поэтому варианты одной группы не
конфликтуют. Группа — это просто несколько вызовов с общим `value`.

```cpp
static int quality = 1;
ui.RadioButton("Низкое", {40, 340, 140, 24}, &quality, 0);
ui.RadioButton("Среднее", {40, 370, 140, 24}, &quality, 1);
ui.RadioButton("Высокое", {40, 400, 140, 24}, &quality, 2);
```

### Слайдеры и числовой ввод

Слайдер меняет значение протяжкой по дорожке, `DragFloat`/`DragInt` — движением
мыши по горизонтали; все три возвращают `true`, пока значение меняется.

```cpp
ui.BeginPanel("audio", audioPanel);
ui.Slider("Музыка", ui.Alloc(audioPanel.w - 24.0f, 40.0f, crossrender::LayoutSize::Fixed(40.0f)),
          &musicVolume, 0.0f, 1.0f, "%.0f%%");
ui.Slider("Эффекты", ui.Alloc(audioPanel.w - 24.0f, 40.0f, crossrender::LayoutSize::Fixed(40.0f)),
          &sfxVolume, 0.0f, 1.0f, "%.0f%%");
ui.EndPanel();
```

#### `bool Slider(const char* label, const Rect& rect, f32* value, f32 min, f32 max, const char* format = "%.2f", bool enabled = true)`

Слайдер: дорожка с ползунком, значение ограничивается `[min, max]`.
Возвращает `true`, пока значение меняется. Если высота слота больше 30 px,
над дорожкой рисуются подпись (справа) и текущее значение (слева); сама
подпись виджет рисует, отдельный `Text` не нужен. Управление: протяжка
мышью; в фокусе — стрелки (шаг 1 % диапазона), `Home`/`End` — края, `Shift`
даёт мелкий шаг 0.1 %. `format` без символа `%` игнорируется и заменяется на
`"%.2f"`.

```cpp
static crossrender::f32 volume = 0.8f;
if (ui.Slider("Громкость", ui.Alloc(320.0f, 40.0f, crossrender::LayoutSize::Fixed(40.0f)),
              &volume, 0.0f, 1.0f, "%.0f%%")) {
    audio.SetMasterVolume(volume);
}
```

#### `bool DragFloat(const char* label, const Rect& rect, f32* value, f32 speed = 0.01f, f32 min = 0, f32 max = 0, const char* format = "%.2f", bool enabled = true)`

Числовое поле-«протяжка»: значение меняется горизонтальным движением мыши с
множителем `speed` за пиксель. `Shift` замедляет в 10 раз, `Ctrl`/`Super`
ускоряет в 10 раз; в фокусе работают стрелки с шагом `speed * 10`. Границы
применяются, только если `min != max` (по умолчанию `0, 0` — без ограничения).
Виджет сам рисует подпись и значение по центру и меняет курсор на «руку»/
«изменение размера».

```cpp
static crossrender::f32 gravity = -9.81f;
if (ui.DragFloat("Гравитация", ui.Alloc(320.0f, 28.0f, crossrender::LayoutSize::Fixed(28.0f)),
                 &gravity, 0.01f, -30.0f, 0.0f, "%.2f м/с2")) {
    physics.SetGravity(gravity);
}
```

#### `bool DragInt(const char* label, const Rect& rect, int* value, f32 speed = 0.1f, int min = 0, int max = 0, bool enabled = true)`

Целочисленный вариант `DragFloat`. Работает через временное `f32`, формат
жёстко `"%.0f"`, результат округляется. Как и у `DragFloat`, границы
применяются только при `min != max`.

```cpp
static int lives = 3;
if (ui.DragInt("Жизни", ui.Alloc(320.0f, 28.0f, crossrender::LayoutSize::Fixed(28.0f)),
               &lives, 0.1f, 1, 99)) {
    ENG_LOGI("game", "жизни -> %d", lives);
}
```

### Полосы и индикаторы

`ProgressBar` и `Spinner` только показывают состояние (`false`), `Scrollbar`
управляет прокруткой и сообщает об изменении.

```cpp
ui.ProgressBar({40, 40, 320, 14}, boss.health, "Здоровье");
ui.Spinner({600.0f, 40.0f, 28.0f, 28.0f}, spinnerPhase);
if (ui.Scrollbar("zoom", {40, 70, 12, 160}, &zoom, 0.25f, false)) ApplyZoom(zoom);
```

#### `bool ProgressBar(const Rect& rect, f32 fraction, const char* label = nullptr, const Color* fillColor = nullptr)`

Индикатор выполнения: дорожка, заливка на `fraction` (0..1) и необязательная
подпись по центру. Заливка плавно догоняет целевое значение (сглаживание
6/с), цвет по умолчанию — `theme.accent`, свой задаётся `fillColor`.
Виджет всегда возвращает `false`: он ничего не меняет. Подпись, если передана,
рисует он сам кеглем `smallSize`.

```cpp
ui.ProgressBar({40, 40, 320, 14}, loaded / static_cast<crossrender::f32>(total), "Загрузка");
const crossrender::Color low = crossrender::Color::FromARGB(0xFFE4574F);
ui.ProgressBar({40, 70, 220, 18}, health / maxHealth, nullptr, &low);
```

#### `bool Scrollbar(const char* id, const Rect& rect, f32* value, f32 pageFraction, bool horizontal = false, bool enabled = true)`

Отдельная полоса прокрутки. `value` — нормализованное смещение в
`[0, 1 - pageFraction]`, `pageFraction` — доля содержимого, видимая в окне
(длина ползунка). Возвращает `true` при изменении. Работает протяжкой ползунка,
кликом по дорожке (страница вперёд/назад) и колесом над собой. Полосы внутри
`BeginScrollView` и `ListView` рисуются автоматически — этот виджет нужен,
когда прокруткой управляет сцена.

```cpp
static crossrender::f32 offset = 0.0f;                 // 0..1-страница
if (ui.Scrollbar("log-offset", {40, 110, 14, 200}, &offset, 0.35f, false)) {
    scrollPixels = offset / (1.0f - 0.35f) * (contentH - viewH);
}
```

#### `bool Spinner(const Rect& rect, f32 phase, const Color* color = nullptr)`

Круговой индикатор ожидания: затухающий «хвост» из 12 дуг, повёрнутый на
`phase` радиан. `phase` сцена увеличивает сама (`phase += dt * 3.0f`), цвет по
умолчанию — `theme.accent`. Возвращает `false`; при радиусе меньше пикселя
ничего не рисует.

```cpp
spinnerPhase += dt * 3.0f;
ui.Spinner({600.0f, 40.0f, 28.0f, 28.0f}, spinnerPhase, &ui.Theme().accent);
```

### Списки и выпадающие списки

`ListView` и `ListViewCustom` дают выделение, прокрутку и клавиатурную
навигацию; `Dropdown` и `ComboBox` раскрывают список поверх остального
интерфейса.

```cpp
ui.BeginColumn("saves", savesRect, 8.0f);
ui.ListView("slots", ui.Alloc(savesRect.w, 180.0f, crossrender::LayoutSize::Fixed(180.0f)),
            saveNames, &selected);
static int sortMode = 0;
ui.Dropdown("sort", ui.Alloc(200.0f, 28.0f, crossrender::LayoutSize::Fixed(28.0f)), sortNames, &sortMode);
ui.EndColumn();
```

#### `bool ListView(const char* id, const Rect& rect, const std::vector<std::string>& items, int* selected, f32 itemHeight = 0, bool enabled = true)`

Список строк с выделением, прокруткой и клавиатурной навигацией. `itemHeight
= 0` означает `theme.itemHeight`. Возвращает `true`, когда выделение
изменилось: клик (и протяжка — выделение следует за курсором), колесо,
а в фокусе — стрелки, `PageUp`/`PageDown`, `Home`/`End` с автопрокруткой к
выделенной строке. Строки рисуются виртуально — только видимый срез. Тонкая
полоса прокрутки появляется сама, когда содержимое не помещается.

```cpp
static int selected = 0;
if (ui.ListView("saves", {40, 40, 300, 200}, saveNames, &selected, 26.0f)) {
    ENG_LOGI("save", "выбран слот %d: %s", selected, saveNames[selected].c_str());
}
```

#### `bool ListViewCustom(const char* id, const Rect& rect, int itemCount, int* selected, const std::function<void(int, const Rect&, bool)>& drawItem, f32 itemHeight = 0)`

Тот же список, но строки рисует сцена: `drawItem(index, rect, isSelected)`
вызывается только для видимых строк, и только для них же считается фон
наведения и выделения. Подпись, иконки и что угодно ещё рисует `drawItem` —
по умолчанию текста нет. Возврат `true` — выделение изменилось.

```cpp
// ItemRow объявлена рядом со списком предметов, а не внутри функции.
struct ItemRow { std::string name; std::string detail; };

void DrawInventory(crossrender::UiContext& ui, const crossrender::Rect& body,
                   const std::vector<ItemRow>& items, int* selected) {
    if (ui.ListViewCustom("inventory", body, static_cast<int>(items.size()), selected,
                          [&](int index, const crossrender::Rect& r, bool isSelected) {
                              const ItemRow& it = items[static_cast<crossrender::usize>(index)];
                              ui.TextAt(it.name, {r.x + 10.0f, r.y + 8.0f},
                                        isSelected ? ui.Theme().text : ui.Theme().textDim,
                                        15.0f, crossrender::TextAlign::Left, crossrender::TextBaseline::Top);
                              ui.TextAt(it.detail, {r.x + 10.0f, r.y + 26.0f},
                                        ui.Theme().textDisabled, 12.0f,
                                        crossrender::TextAlign::Left, crossrender::TextBaseline::Top);
                          },
                          44.0f)) {
        Equip(items[static_cast<crossrender::usize>(*selected)]);
    }
}
```

#### `bool Dropdown(const char* id, const Rect& rect, const std::vector<std::string>& items, int* selected, bool enabled = true)`

Выпадающий список. В закрытом виде показывает выбранный элемент и шеврон, по
клику открывает список **под** прямоугольником (до 240 px высотой, с
собственной прокруткой колесом). Сам список рисуется в оверлейном проходе, то
есть поверх всего, что нарисовано позже, — поэтому вызывать `RenderOverlays()`
после `EndFrame` обязательно. Возвращает `true` в кадр выбора пункта.
Закрывается по выбору, `Escape` или клику вне списка.

```cpp
static int sortMode = 0;
const std::vector<std::string> kSorts{"По имени", "По дате", "По размеру"};
if (ui.Dropdown("sort", {420, 40, 200, 28}, kSorts, &sortMode)) {
    ENG_LOGI("save", "сортировка -> %s", kSorts[sortMode].c_str());
}
```

#### `bool ComboBox(const char* id, const Rect& rect, const std::vector<std::string>& items, int* selected, const char* label = nullptr, bool enabled = true)`

Выпадающий список с подписью над ним: если `label` передан, он рисуется мелким
кеглем в верхней части `rect`, а сам список занимает оставшуюся высоту (не
менее 8 px). Во всём остальном это `Dropdown`. Удобен для выбора значения
перечисления.

```cpp
enum class Quality { Low, Medium, High };
static Quality quality = Quality::Medium;
const std::vector<std::string> kNames{"Низкое", "Среднее", "Высокое"};
int index = static_cast<int>(quality);
if (ui.ComboBox("quality", {420, 110, 220, 34}, kNames, &index, "Качество графики")) {
    quality = static_cast<Quality>(index);
    ApplyQuality(index);
}
```

#### `bool CollapsingHeader(const char* label, const Rect& rect, bool* open, bool enabled = true)`

Сворачиваемая секция: строка с вращающимся треугольником и подписью. По клику
`*open` переключается, возврат `true` — в кадр переключения. Содержимое секции
сцена показывает сама, проверяя `*open`; анимация треугольника плавная, а
первый кадр не анимируется.

```cpp
static bool advanced = false;
if (ui.CollapsingHeader("Дополнительно", {40, 40, 320, 30}, &advanced)) {
    ENG_LOGD("ui", "секция %s", advanced ? "раскрыта" : "свёрнута");
}
if (advanced) ui.DragFloat("Масштаб", {40, 80, 320, 28}, &scale, 0.01f, 0.5f, 4.0f);
```

#### `bool TabBar(const char* id, const Rect& rect, const std::vector<std::string>& tabs, int* selected)`

Панель вкладок: подписи делят ширину поровну, выбранная подсвечена, под ней
анимированно едет подчёркивание акцентом. Возвращает `true` **только в кадр
смены** вкладки, причём выбор срабатывает по **нажатию**, а не по отпусканию
(в отличие от кнопок). Пустой список вкладок — `false`; `selected == nullptr`
допустим (вкладки рисуются, но не выбираются).

```cpp
static int tab = 0;
const std::vector<std::string> tabs{"Общие", "Графика", "Звук"};
if (ui.TabBar("options", {40, 90, 420, 34}, tabs, &tab)) {
    ENG_LOGI("ui", "вкладка -> %s", tabs[tab].c_str());
}
```

### Поля ввода

`TextField` и `TextArea` редактируют строку по месту (UTF-8, выделение, буфер
обмена), `ColorPicker` собирает квадрат насыщенности, полосу тона и HEX-поле.

```cpp
ui.BeginColumn("profile", profileRect, 8.0f);
bool submitted = false;
ui.TextField("nick", ui.Alloc(280.0f, 32.0f, crossrender::LayoutSize::Fixed(32.0f)), &nick, "Ник",
             false, &submitted);
ui.TextArea("about", ui.Alloc(280.0f, 120.0f, crossrender::LayoutSize::Fixed(120.0f)), &about, 8);
ui.EndColumn();
if (submitted) SaveProfile(nick, about);
```

#### `bool TextField(const char* id, const Rect& rect, std::string* text, const char* placeholder = nullptr, bool password = false, bool* submitted = nullptr, bool enabled = true)`

Однострочное текстовое поле. Возвращает `true`, когда текст изменился в этом
кадре; `submitted` (если передан) выставляется в `true` по `Enter`, после чего
поле снимает с себя фокус. `placeholder` виден, пока текст пуст и поле не в
фокусе; `password = true` рисует символы точками (U+2022), не трогая саму
строку. Поддерживаются UTF-8 (каретка не разрывает многобайтовые символы),
выделение мышью и `Shift`+стрелками, `Ctrl+A/C/X/V` (собственный буфер
обмена), `Home`/`End`, `Ctrl`+стрелки по словам, автопрокрутка по каретке.
Клик внутри поля забирает фокус.

```cpp
static std::string playerName;
bool submitted = false;
if (ui.TextField("name", {40, 40, 260, 32}, &playerName, "Имя игрока", false, &submitted)) {
    ENG_LOGD("ui", "имя: %s", playerName.c_str());
}
if (submitted && playerName.empty()) ui.Toast("Имя не может быть пустым", 2.0f, &ui.Theme().danger);
static std::string password;
ui.TextField("password", {40, 84, 260, 32}, &password, "Пароль", true);
```

#### `bool TextArea(const char* id, const Rect& rect, std::string* text, int maxLines = 8, bool enabled = true)`

Многострочное поле ввода. `Enter` вставляет перевод строки (не отправляет
форму), стрелки вверх/вниз ходят по строкам, текст прокручивается по вертикали
внутри прямоугольника. `maxLines` участвует в идентификаторе виджета, а не в
ограничении числа строк: фактический текст не обрезается. Как и `TextField`,
возвращает `true` при изменении текста.

```cpp
static std::string bio;
if (ui.TextArea("bio", {40, 130, 320, 140}, &bio, 10)) {
    ENG_LOGD("ui", "описание: %d символов", static_cast<int>(bio.size()));
}
```

#### `bool ColorPicker(const char* id, const Rect& rect, Color* color)`

Выбор цвета: квадрат «насыщенность/яркость», вертикальная полоса тона и
редактируемое HEX-поле с образцом цвета справа. Альфа не меняется — она
переносится из исходного цвета. Возвращает `true`, пока пользователь тянет по
квадрату или полосе или пока HEX-строка разбирается успешно; при вводе
мусора в HEX цвет не трогается. Курсор над квадратом и полосой — перекрестие.

```cpp
crossrender::Color tint = crossrender::Color::FromARGB(0xFF6BA1FF);
if (ui.ColorPicker("tint", {40, 40, 260, 200}, &tint)) {
    material.tint = tint;
}
```

### Изображения

`Image` и `ImageButton` рисуют текстуру (без и с обработкой клика), `Draw9`
растягивает nine-patch рамку.

```cpp
ui.Image(portrait, {40, 40, 128, 128});
if (ui.ImageButton("avatar", portrait, {180, 40, 64, 64})) OpenProfile();
ui.Draw9(panelTex, {180, 120, 200, 48}, crossrender::NinePatch::Uniform(10.0f), crossrender::Color::White);
```

#### `void Image(const Texture& tex, const Rect& rect, const Color& tint = Color::White)`

Рисует текстуру в прямоугольник без интерактива, `tint` домножается на цвет
пикселей. Если текстура невалидна (не загрузилась), вместо неё рисуется
шашечная заглушка — так ошибка ассетов видна, а компоновка не рушится.
Отсечение контейнера учитывается автоматически.

```cpp
ui.Image(itemIcon, {340, 40, 160, 120});
ui.Image(itemIcon, {340, 180, 160, 120}, ui.Theme().textDim);   // приглушённая
```

#### `bool ImageButton(const char* id, const Texture& tex, const Rect& rect, const Color& tint = Color::White)`

Картинка-кнопка: рисует текстуру и обрабатывает клик (отпускание внутри
прямоугольника). Возвращает `true` в кадр клика, при наведении меняет курсор
на «руку», при отсутствии текстуры рисует подсвечиваемый прямоугольник.

```cpp
const crossrender::Rect thumb = {340, 320, 64, 64};
if (ui.ImageButton("photo", photo, thumb, crossrender::Color::White)) OpenPhoto("photo/02.png");
```

#### `void Draw9(const Texture& tex, const Rect& rect, const NinePatch& patch, const Color& tint, f32 scale = 1.0f)`

Универсальный помощник девятипатчевой растяжки: углы текстуры не
масштабируются, края и центр тянутся, толщина рамки сохраняется при любом
размере. `scale` домножает отступы `patch` (обычно передают `DpiScale()`).
Ничего не делает, если текстура невалидна или прямоугольник пуст. Применяется
для фонов своих элементов, когда nine-patch арт не хочется класть в тему.

```cpp
ui.Draw9(panelTex, {440, 320, 140, 48}, crossrender::NinePatch::Uniform(10.0f), crossrender::Color::White,
         ui.DpiScale());
```

### Всплывающие подсказки и окна

Подсказка живёт один кадр и рисуется поверх всего, попап не блокирует ввод,
модальное окно блокирует его целиком.

```cpp
if (ui.Button("Выход", quitButton, true, "Завершает игру без сохранения")) Quit();
if (ui.Button("Помощь", helpButton)) ui.OpenPopup("help");
if (confirmOpen && ui.BeginModal("confirm", {420.0f, 200.0f}, "Выйти без сохранения?")) {
    ui.Text("Несохранённый прогресс будет потерян.");
    if (ui.Button("Выйти", ui.Alloc(160.0f, 34.0f, crossrender::LayoutSize::Fixed(34.0f)))) Quit();
    ui.EndModal();
}
```

#### `void SetTooltip(const char* text) / void Tooltip(const char* text)`

Ставит подсказку у курсора на текущий кадр. Достаточно вызвать, когда курсор
над элементом; текст рисуется в оверлейном проходе `RenderOverlays()` поверх
всего и прижимается к экрану, чтобы не вылезти за край. Подсказка не
накапливается: `RenderOverlays` очищает её в конце, поэтому вызывать нужно
каждый кадр. `Tooltip` — полный синоним `SetTooltip`. Если тема без шрифта,
подсказка не рисуется.

```cpp
const crossrender::Rect gear = {900.0f, 40.0f, 36.0f, 36.0f};
if (ui.IsHovered(gear)) ui.SetTooltip("Открывает настройки графики");
ui.Tooltip("WASD - движение, мышь - обзор");     // то же самое
```

#### `bool BeginModal(const char* id, const Vec2& size, const char* title) / void EndModal()`

Модальное окно по центру экрана: затемняет фон `theme.overlay`, рисует панель с
заголовком и **блокирует ввод** во всём остальном интерфейсе. Возвращает
`true`, пока окно живо; `false` — если его закрыли (`Escape`), тогда тело
рисовать не нужно. Внутри `BeginModal` уже открыта панель, поэтому содержимое
раскладывается через `Alloc`, а завершает его `EndModal` (а не `EndPanel`).
Вложенные модальные окна поддерживаются (счётчик глубины). `EndModal`
снимает блокировку, когда закрылось последнее.

```cpp
static bool confirmOpen = false;
if (confirmOpen && ui.BeginModal("confirm-delete", {420.0f, 200.0f}, "Удалить сохранение?")) {
    ui.Text("Действие нельзя отменить.");
    ui.BeginRow("modal-buttons", {screen.Center().x - 200.0f, screen.Center().y + 40.0f, 400.0f, 36.0f}, 10.0f);
    if (ui.Button("Удалить", ui.Alloc(160.0f, 34.0f, crossrender::LayoutSize::Fixed(34.0f)))) {
        DeleteSave();
        confirmOpen = false;
    }
    if (ui.Button("Отмена", ui.Alloc(160.0f, 34.0f, crossrender::LayoutSize::Fixed(34.0f)))) confirmOpen = false;
    ui.EndRow();
    ui.EndModal();
}
```

#### `bool BeginPopup(const char* id, const Vec2& position, const Vec2& size) / void EndPopup()`

Поповер (всплывающая панель) в заданной точке. Открывается `OpenPopup` или
щелчком по `Dropdown`; `BeginPopup` возвращает `false`, если попап не открыт,
уже закрыт по `Escape`/клику вне его или активен другой выпадающий список, —
тогда `EndPopup` вызывать не нужно. В отличие от модального окна, ввод **не**
блокируется. Внутри открыта панель, содержимое раскладывается через `Alloc`,
`EndPopup` её закрывает.

```cpp
if (ui.IsPopupOpen("help")) {
    if (ui.BeginPopup("help", {screen.Center().x - 160.0f, 120.0f}, {320.0f, 200.0f})) {
        ui.Heading("Справка");
        ui.Text("WASD - движение, мышь - обзор.");
        if (ui.Button("Закрыть", ui.Alloc(120.0f, 30.0f, crossrender::LayoutSize::Fixed(30.0f)))) {
            ui.ClosePopup();
        }
        ui.EndPopup();
    }
}
```

#### `void OpenPopup(const char* id) / void ClosePopup() / bool IsPopupOpen(const char* id) const / void OpenDialog(const char* id)`

Управление попапом из кода. `OpenPopup` помечает попап открытым и делает его
активным (следующий `BeginPopup` с этим id вернёт `true`), `ClosePopup`
закрывает активный, `IsPopupOpen` проверяет состояние, не открывая его,
`OpenDialog` — документированный синоним `OpenPopup` для модальных сценариев.
Закрытие активного попапа также снимает «захват» кликов: клик вне списка
закрывает его и не доходит до виджетов под ним.

```cpp
if (ui.Button("Справка", helpButton)) ui.OpenPopup("help");
if (ui.Button("Настройки", settingsButton)) ui.OpenDialog("settings");
if (ui.IsPopupOpen("help") && input.KeyPressed(crossrender::Key::F1)) ui.ClosePopup();
```

### Служебные методы

Уведомления, отладочная панель, невидимая кнопка для своего арта и генератор
процедурной nine-patch текстуры.

```cpp
ui.Toast("Чекпойнт достигнут", 2.0f, &ui.Theme().success);
if (debugUi) ui.DrawDebugOverlay("сцена: уровень 2");
if (ui.InvisibleButton("##checkpoint", checkpointRect)) FastTravel(3);
ui.TextCentered(checkpointRect, "Чекпойнт", ui.Theme().text);
```

#### `void Toast(const std::string& message, f32 duration = 3.0f, const Color* color = nullptr)`

Всплывающее уведомление внизу экрана. Живёт `duration` секунд, плавно
появляется и исчезает, соседние тосты встают столбиком вверх. Стек ограничен
шестью сообщениями: седьмое вытесняет самое старое. `color` заменяет фон
панели (например, `theme.danger` для ошибки). Рисуются в `RenderOverlays`.

```cpp
ui.Toast("Сохранено", 2.0f);
ui.Toast("Нет соединения с сервером", 4.0f, &ui.Theme().danger);
if (ui.DeltaTime() > 0.05f) ui.Toast("Просадка кадра", 1.5f, &ui.Theme().warning);
```

#### `void DrawDebugOverlay(const std::string& text)`

Показывает отладочную панель в левом верхнем углу: число виджетов, активный,
горячий и фокусированный идентификаторы, время кадра и FPS, плотность DPI,
позицию мыши и добавленный `text`. Панель рисуется в `RenderOverlays`, поэтому
достаточно вызвать метод раз в кадр (обычно под условием). Текст очищается
после отрисовки — вызов нужно повторять.

```cpp
if (debugUi) ui.DrawDebugOverlay("сцена: меню, сущностей: 42");
```

#### `bool InvisibleButton(const char* id, const Rect& rect, bool enabled = true)`

Кнопка без отрисовки: полное попадание, состояния наведения/нажатия/фокуса,
клик по отпусканию внутри и клавиатурная активация — но **ничего не рисует**.
Применяется, когда сцена сама рисует карточку, вкладку или строку, но хочет
стандартное поведение кнопки. Подпись можно не задавать: id берётся из строки
(удобно передавать `"##card-3"`), пустая строка заменяется на `"invisible"`.
Про звук и смену курсора сцена заботится сама (`PlayClickSound`, `SetCursor`).

```cpp
const crossrender::Rect card = {40.0f, 120.0f, 240.0f, 64.0f};
if (ui.InvisibleButton("##card-3", card)) SelectSlot(3);
if (ui.IsHovered(card)) ui.SetCursor(1);
ui.TextCentered(card, "Сохранение 3", ui.Theme().text);
```

#### `Texture MakeRoundedRectTexture(int size, f32 radius, const Color& fill, const Color& border, f32 borderWidth)`

Свободная функция: генерирует процедурную nine-patch текстуру — скруглённый
прямоугольник с обводкой, сглаженный по внешнему контуру. Прямые края и центр
остаются сплошными, поэтому текстура корректно тянется с
`NinePatch::Uniform(radius + borderWidth)`. Нужен живой контекст OpenGL: без
него текстура создаётся, но остаётся невалидной, и виджеты рисуют процедурный
вид (проверяйте `Texture::Valid()`).

```cpp
const crossrender::Texture buttonTex =
    crossrender::MakeRoundedRectTexture(48, 10.0f, crossrender::Color::FromARGB(0xFF252935),
                                crossrender::Color::FromARGB(0xFF4C8DFF), 1.0f);
if (buttonTex.Valid()) {
    ui.SetButton9Patch(&buttonTex, &buttonTex, &buttonTex, crossrender::NinePatch::Uniform(11.0f));
} else {
    ENG_LOGW("ui", "арт кнопок недоступен — используем процедурный вид");
}
```

## Пример целиком

Панель настроек со слайдером, флажком, выбором качества и формой с проверкой;
она же показывает подсказку при наведении и сохраняет тему сцены.

```cpp
#include "crossrender/core/Log.h"
#include "crossrender/ui/Ui.h"

#include <string>
#include <vector>

// Приходят из остального кода приложения.
void ApplyFullscreen(bool enabled);
struct SettingsState;
void SaveSettings(const SettingsState& settings);

enum class Quality { Low, Medium, High };

struct SettingsState {
    crossrender::f32 volume = 0.8f;
    crossrender::f32 sensitivity = 2.5f;
    bool vsync = true;
    bool fullscreen = false;
    Quality quality = Quality::Medium;
    std::string playerName;
    bool nameSubmitted = false;
};

const std::vector<std::string>& QualityNames() {
    static const std::vector<std::string> names{"Низкое", "Среднее", "Высокое"};
    return names;
}

// Возвращает true, если в этом кадре нажали «Сохранить».
bool DrawSettingsPanel(crossrender::UiContext& ui, const crossrender::Rect& screen, SettingsState& s) {
    const crossrender::Rect body = crossrender::Anchor::Center({440.0f, 380.0f}).Resolve(screen);
    bool save = false;

    // Свой шрифт сцены, пока панель открыта.
    const crossrender::UiTheme saved = ui.Theme();
    crossrender::UiTheme themed = ui.Theme();
    themed.rounding = 6.0f;
    ui.SetTheme(themed);

    ui.BeginPanel("settings", body);
    ui.Heading("Настройки");

    // Слайдер: подпись и значение виджет рисует сам, отдельный Text не нужен.
    if (ui.Slider("Громкость", ui.Alloc(body.w - 24.0f, 40.0f, crossrender::LayoutSize::Fixed(40.0f)),
                  &s.volume, 0.0f, 1.0f, "%.0f%%")) {
        ENG_LOGI("settings", "громкость -> %.2f", s.volume);
    }

    // Протяжка для точного числа: Shift замедляет, Ctrl ускоряет.
    ui.DragFloat("Чувствительность мыши", ui.Alloc(body.w - 24.0f, 28.0f, crossrender::LayoutSize::Fixed(28.0f)),
                 &s.sensitivity, 0.01f, 0.1f, 20.0f, "%.2f");

    // Флажки — каждый рисует свою подпись.
    if (ui.Checkbox("Вертикальная синхронизация", ui.Alloc(body.w - 24.0f, 24.0f), &s.vsync)) {
        ENG_LOGI("settings", "vsync -> %d", s.vsync ? 1 : 0);
    }
    if (ui.Checkbox("Полноэкранный режим", ui.Alloc(body.w - 24.0f, 24.0f), &s.fullscreen)) {
        ApplyFullscreen(s.fullscreen);
    }

    // Выбор перечисления через ComboBox.
    int qualityIndex = static_cast<int>(s.quality);
    if (ui.ComboBox("quality", ui.Alloc(body.w - 24.0f, 40.0f, crossrender::LayoutSize::Fixed(40.0f)),
                    QualityNames(), &qualityIndex, "Качество графики")) {
        s.quality = static_cast<Quality>(qualityIndex);
        ENG_LOGI("settings", "качество -> %d", qualityIndex);
    }

    ui.Separator();

    // Форма с проверкой: поле имени и кнопка, которая ругается на пустое поле.
    const crossrender::Rect nameRow = ui.Alloc(body.w - 24.0f, 32.0f, crossrender::LayoutSize::Fixed(32.0f));
    ui.TextField("player", nameRow, &s.playerName, "Имя игрока", false, &s.nameSubmitted);
    if (s.nameSubmitted && s.playerName.empty()) {
        ui.TextAt("Имя не может быть пустым", {nameRow.x, nameRow.Bottom() + 2.0f},
                  ui.Theme().danger, ui.Theme().smallSize);
    }

    const crossrender::Rect buttons = ui.Alloc(body.w - 24.0f, 36.0f, crossrender::LayoutSize::Fixed(36.0f));
    ui.BeginRow("buttons", buttons, 8.0f);
    const crossrender::Rect saveButton = ui.Alloc(160.0f, 34.0f, crossrender::LayoutSize::Fixed(34.0f));
    if (ui.Button("Сохранить", saveButton, !s.playerName.empty())) save = true;
    if (ui.Button("Сброс", ui.Alloc(120.0f, 34.0f, crossrender::LayoutSize::Fixed(34.0f)))) {
        s = SettingsState{};
    }
    ui.EndRow();

    // Подсказка только пока курсор над кнопкой сохранения.
    if (ui.IsHovered(saveButton)) {
        ui.SetTooltip("Записывает настройки в config.json");
    }

    ui.EndPanel();
    ui.SetTheme(saved);            // возвращаем общий шрифт и метрики сцены
    return save;
}

void OnFrame(crossrender::UiContext& ui, crossrender::Renderer2D& r2d, const crossrender::Input& input,
             const crossrender::Rect& screen, crossrender::f32 dt, SettingsState& settings) {
    ui.BeginFrame(&r2d, input, screen, dt);
    if (settingsOpen && DrawSettingsPanel(ui, screen, settings)) {
        SaveSettings(settings);
        ui.Toast("Настройки сохранены", 2.0f);
    }
    ui.EndFrame();
    ui.RenderOverlays();            // тултипы, тосты и открытые списки — поверх всего
}
```

## См. также

* `docs/gfx/Renderer2D.md` — рендерер, в координатах и кадре которого живёт
  весь UI; там же описаны `NinePatch`, `TextAlign`, `TextBaseline` и инертный
  режим без контекста OpenGL.
* `docs/core/Base.md` — типы `f32`, `u64`, `u8` и макросы логирования,
  используемые в примерах.
* `docs/core/Log.md` — `ENG_LOGI` / `ENG_LOGW`, которыми удобно отмечать
  действия пользователя в интерфейсе.
