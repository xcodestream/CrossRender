# crossrender/platform/Window.h — нативное окно, контекст OpenGL и снимок ввода

Заголовок даёт движку окно операционной системы, контекст OpenGL и состояние
ввода за кадр. Одна и та же пара `Input` + `Window` работает на настольных
платформах, в браузере и на мобильных системах, а весь платформенный код
спрятан за одним интерфейсом.

## Заголовок

```cpp
#include "crossrender/platform/Window.h"
```

## Обзор

Движок сознательно **не использует GLFW и SDL**: для каждой операционной
системы написан собственный бэкенд, который создаёт нативное окно и нативный
контекст OpenGL. Заголовок один, а реализаций шесть — по одной на платформу:

| Платформа | Окно | Контекст OpenGL | Файл бэкенда |
|---|---|---|---|
| macOS | AppKit `NSWindow` / `NSView` | `NSOpenGLContext` (CGL) | `engine/src/platform/macos/WindowMacOS.mm` |
| Windows | Win32 `HWND` | WGL | `engine/src/platform/windows/WindowWindows.cpp` |
| Linux | X11 | GLX | `engine/src/platform/linux/WindowLinux.cpp` |
| Web | canvas Emscripten | WebGL 2 | `engine/src/platform/wasm/WindowWasm.cpp` |
| Android | `ANativeWindow` | EGL | `engine/src/platform/android/WindowAndroid.cpp` |
| iOS | `UIWindow` / `UIView` | EAGL | `engine/src/platform/ios/WindowIOS.mm` |

Из этого следует главное практическое правило: **`Window::Create` возвращает
`false`, если окно или контекст создать не удалось** — например, на машине без
дисплея (`XOpenDisplay` вернул `nullptr`), без графического драйвера, без
canvas-элемента в браузере или до прихода нативного окна на Android. Это не
аварийная ситуация: `tests/test_platform.cpp` превращает такой отказ в
пропуск теста (`ENG_SKIP("no display available")`), и вызывающий код должен
поступать так же — сообщить и не продолжать.

Поэтому любой цикл приложения начинается с проверки создания окна:

```cpp
crossrender::Window window;
crossrender::WindowDesc desc;
desc.title = "Моя игра";
desc.width = 1280;
desc.height = 720;

if (!window.Create(desc)) {
    ENG_LOGW("app", "окно недоступно: работаем без графики (headless)");
    return RunHeadless();
}
ENG_LOGI("app", "окно %dx%d, кадровый буфер %dx%d, DPI %.2f", window.Width(), window.Height(),
         window.FramebufferWidth(), window.FramebufferHeight(), window.DpiScale());
```

#### Модель ввода

`Input` — это **снимок состояния за кадр**, а не поток событий. Платформенный
бэкенд во время `Window::PollEvents()` вызывает методы `OnKey`, `OnMouseMove`,
`OnMouseButton`, `OnScroll`, `OnText` и `OnTouch`, а игровой код читает готовое
состояние. Типичный кадр на настольной платформе выглядит так:

```cpp
// Один кадр: сначала сбрасываем «краевые» события, затем принимаем новые.
input.BeginFrame();          // очищает pressed/released/double-click, delta, scroll, text
window.PollEvents();         // очередь ОС -> Input::OnKey / OnMouseMove / ... / OnTouch

if (input.KeyPressed(crossrender::Key::Escape)) window.RequestClose();
if (input.KeyDown(crossrender::Key::W)) player.MoveForward(dt);
for (crossrender::u32 cp : input.TextInput()) name.PushCodepoint(cp);
if (input.MouseDoubleClick(crossrender::MouseButton::Left)) OpenInventory();

input.EndFrame();            // запоминает позицию мыши для следующего кадра
window.SwapBuffers();        // показываем задний буфер
```

`BeginFrame` сбрасывает только **краевое** состояние: `KeyPressed`,
`KeyReleased`, `MousePressed`, `MouseReleased`, `MouseDoubleClick`,
`MouseDelta`, `ScrollDelta` и `TextInput`. Флаги удержания (`KeyDown`,
`MouseDown`) остаются `true`, пока не придёт соответствующее событие отпускания.

* **`KeyPressed` против `KeyDown`.** `KeyPressed(k)` истинно только в том кадре,
  в котором клавиша была нажата (`edges`), а `KeyDown(k)` — всё время, пока её
  держат. Для одиночных действий (прыжок, выстрел) берите `KeyPressed`, для
  непрерывных (движение, ускорение) — `KeyDown`.
* **Автоповтор.** `KeyAction::Repeat` выставляет **и** `KeyDown`, **и**
  `KeyPressed`, чтобы автоповтор работал в текстовых полях и списках. При этом
  параметр `bool repeat` у `Input::OnKey` реализацией **принимается, но
  игнорируется** — в `engine/src/platform/Input.cpp` стоит `(void)repeat`.
  Решение «это повтор или новое нажатие» принимает сам бэкенд, отправляя
  `KeyAction::Repeat` отдельным вызовом (так делают все бэкенды: macOS, Windows,
  Linux, Android и Web).
* **Мышь.** `OnMouseMove` **накапливает** `MouseDelta()` (сумма всех перемещений
  внутри кадра) и обновляет `MousePos()`; `OnScroll` накапливает `ScrollDelta()`.
  Оба накопления сбрасываются в `BeginFrame`.
* **Двойной клик.** Определяется как два нажатия в пределах **400 мс**.
  В комментарии исходника рядом упомянут ещё и порог расстояния 6 px, но
  реализация сравнивает **только время** — расстояние не проверяется.
* **Текст.** `OnText` дописывает кодовую точку Unicode, `TextInput()` очищается
  каждый `BeginFrame`. Это правильный способ читать набранные символы: он
  не зависит от раскладки, тогда как `KeyDown(Key::A)` сообщает о **физической**
  клавише и на русской раскладке тоже сработает.
* **Касания и геймпад.** `OnTouch` ведёт до `kMaxTouches` одновременных точек,
  обновляет `TouchPoint::pos`, удаляет точку по `Up`/`Cancel` и **дополнительно**
  двигает виртуальную мышь (`OnMouseMove` + левая кнопка), поэтому UI-код
  работает на мобильных без изменений. `SetGamepad` — push-API: внутри движка
  её сегодня **никто не вызывает**, состояние обязан подавать сам приложение;
  поддерживаются 4 геймпада по 8 осей.

#### Ловушка координат

Самое частое место ошибок — смешение **логических** координат окна и **пикселей
кадрового буфера**:

* В комментариях сцен (`examples/sources/scenes/GamePong.cpp`,
  `examples/sources/scenes/GameBreakout.cpp`) написано, что `Input::MousePos()` —
  «сырое пространство кадрового буфера», а `UiContext::MousePos()` — «логическое
  (DPI-масштабированное)». **На самом деле это не так:**
  `UiContext::MousePos()` просто переадресует вызов в `Input::MousePos()`
  (`engine/src/ui/Ui.cpp`), то есть обе функции возвращают одно и то же.
* Платформенные бэкенды подают координаты **в логическом пространстве окна**:
  macOS — точки `NSView`, Windows — клиентские пиксели, Android — координаты,
  поделённые на плотность дисплея (`framebuffer / DpiScale`).
* `Renderer2D` раскладывает UI в логическом пространстве, деля кадровый буфер
  на `DpiScale()` (`Renderer2D::BeginFrame(fbWidth, fbHeight, dpiScale)`).

Реальный инвариант один: **`FramebufferWidth() == Width() * DpiScale()`**.
Поэтому `Input::MousePos()` и `Window::MousePosition()` — это логические
координаты окна, и любой код, который сравнивает их с чем-то, измеренным в
пикселях (render target, `FramebufferWidth()`, `FramebufferHeight()`), обязан
умножить или поделить на `DpiScale()`. На HiDPI-дисплее без этого попадание
по кнопке окажется смещённым (на Retina — вдвое):

```cpp
// НЕВЕРНО: логические координаты мыши сравниваются с пикселями кадрового буфера.
const bool bad = crossrender::Length(input.MousePos() - buttonCenterPx) < radiusPx;

// ВЕРНО: приводим обе величины к одному пространству через DpiScale().
const crossrender::f32 dpi = window.DpiScale() > 0.0f ? window.DpiScale() : 1.0f;
const crossrender::Vec2 buttonCenterLogical = buttonCenterPx / dpi;   // пиксели -> логические
const crossrender::f32  radiusLogical       = radiusPx / dpi;
const bool good = crossrender::Length(input.MousePos() - buttonCenterLogical) < radiusLogical;
```

`Window::MousePosition()` при этом читает **живую** позицию курсора у
операционной системы (и может выйти за пределы окна), а `Input::MousePos()` —
последнюю позицию, доставленную событием. Для UI надёжнее второе.

#### Режимы курсора

`SetCursorMode(mode)` понимает три значения: `0` — обычный курсор, `1` — скрытый,
`2` — отключённый/относительный (указатель захвачен, движение читается через
`MouseDelta()`). Настоящие все три работают на macOS, Windows и Linux (захват
мыши, скрытие, перецентровка курсора); в браузере доступно только скрытие —
относительный захват требует асинхронного Pointer Lock API; на Android и iOS
значение просто запоминается, захвата мыши там нет. `SetCursorVisible` — простая
форма «показать/скрыть».

#### Честность по платформам

* Linux: `WaitEventsTimeout` ждёт события через `XPending` и `select` на
  файловом дескрипторе X-соединения, затем вызывает `PollEvents()`.
* Web: `WaitEventsTimeout` — **пустышка** (Emscripten не умеет спать без
  Asyncify, а блокировать вкладку нельзя). `GetClipboardText()` всегда
  возвращает пустую строку и один раз пишет предупреждение: буфер обмена в
  браузере доступен только асинхронно. `SetClipboardText` опирается на жест
  пользователя и при его отсутствии молча не срабатывает.
* Linux: `GetClipboardText` выполняет round-trip через X-выбор, ждёт ответ
  не дольше 100 мс и при таймауте один раз пишет предупреждение и возвращает
  пустую строку.
* Мобильные: `Minimize`, `Maximize` и `Restore` — no-op (окно принадлежит ОС),
  а `IsFullscreen()` на iOS всегда `true` — приложение владеет всем экраном.
* iOS: MSAA **не реализован** (предупреждение при `msaaSamples > 1`); на Android
  мультисэмплинг тоже не запрашивается — конфигурацию EGL выбирает платформа.
* Web: `IsMinimized()` — это `!visible`.
* Windows и Linux умеют читать буфер обмена по-настоящему, но при занятом или
  неотвечающем буфере пишут предупреждение (на Linux — один раз) и возвращают
  **пустую строку**, а не ошибку. `SetClipboardText` ничего не возвращает:
  неудачу записи можно заметить только по пустому чтению.

#### Обратные вызовы окна

`onClose` вызывается **ровно один раз** из `RequestClose()` (повторный вызов
ничего не делает). `onResize` получает размер **кадрового буфера** в пикселях.
`onFocus` на Android и iOS передаёт уход приложения в фон и возврат в
foreground. `onDpiChanged` срабатывает на macOS, Windows, Android и Web.
`onDropFiles` и `onFilesDropped` **объявлены, но ни один бэкенд их не вызывает** —
drag-and-drop файлов сегодня не подключён. Отдельной ловушки «потерян контекст
OpenGL» в `WindowCallbacks` нет: на Android и Web она отображается в
`onFocus(false)`, а полноценные хуки `onContextLost` / `onContextRestored` живут
в `AppHooks` из `crossrender/platform/Platform.h`.

#### Что ещё нужно знать

* `Window::Create` создаёт **настоящий** контекст OpenGL, поэтому требует
  дисплея и драйвера. Скрытый headless-контекст для тестов и CI объявлен не
  здесь, а в `crossrender/platform/Platform.h` (`CreateHeadlessGLContext`,
  `DestroyHeadlessGLContext`, `HasHeadlessGLContext`, `HeadlessGLGetProcAddress`).
* `Window` не копируется (`Window(const Window&) = delete`) и владеет одним
  экземпляром `Input`, который доступен через `GetInput()`.
* Значения `WindowDesc` по умолчанию: 1280x720, изменяемое окно, vsync,
  поддержка HiDPI, 4x MSAA, OpenGL 3.3 core, оконный режим.
* `Window::GLGetProcAddress()` возвращает резолвер точек входа **для конкретного
  контекста**, а свободная `PlatformGLGetProcAddress()` — глобальный резолвер
  платформы.

## Члены класса

Заголовок содержит два независимых блока: перечисление клавиш, структуры
описания и класс `Input`, а затем окно (`WindowMode`, `WindowDesc`,
`WindowCallbacks`, `Window`) и свободные функции платформы.

### `enum class Key : int`

Физические клавиши клавиатуры. Перечисление намеренно похоже на раскладку
GLFW, чтобы код было легко переносить, но зависимостей от GLFW нет. Значения
`Key::Unknown` и `Key::Count` — не клавиши: первое означает «не распознано»
(и именно его возвращает `Idx` для любого выхода за диапазон), второе — размер
перечисления (`Input::kKeyCount`).

Таблица ниже **полная**: перечислены все 119 значений `Key`. Для читаемости она
разбита на несколько блоков, но других значений в заголовке нет.

Базовые клавиши и знаки:

| Значение | Смысл |
|---|---|
| `Key::Unknown` | клавиша не распознана |
| `Key::Space` | пробел |
| `Key::Apostrophe` | апостроф `'` |
| `Key::Comma` | запятая `,` |
| `Key::Minus` | минус/дефис `-` |
| `Key::Period` | точка `.` |
| `Key::Slash` | косая черта `/` |
| `Key::Num0` | цифра `0` верхнего ряда |
| `Key::Num1` | цифра `1` верхнего ряда |
| `Key::Num2` | цифра `2` верхнего ряда |
| `Key::Num3` | цифра `3` верхнего ряда |
| `Key::Num4` | цифра `4` верхнего ряда |
| `Key::Num5` | цифра `5` верхнего ряда |
| `Key::Num6` | цифра `6` верхнего ряда |
| `Key::Num7` | цифра `7` верхнего ряда |
| `Key::Num8` | цифра `8` верхнего ряда |
| `Key::Num9` | цифра `9` верхнего ряда |
| `Key::Semicolon` | точка с запятой `;` |
| `Key::Equal` | знак равенства `=` |

Буквы латинского алфавита (физические клавиши, не зависят от раскладки):

| Значение | Смысл |
|---|---|
| `Key::A` | буква `A` |
| `Key::B` | буква `B` |
| `Key::C` | буква `C` |
| `Key::D` | буква `D` |
| `Key::E` | буква `E` |
| `Key::F` | буква `F` |
| `Key::G` | буква `G` |
| `Key::H` | буква `H` |
| `Key::I` | буква `I` |
| `Key::J` | буква `J` |
| `Key::K` | буква `K` |
| `Key::L` | буква `L` |
| `Key::M` | буква `M` |
| `Key::N` | буква `N` |
| `Key::O` | буква `O` |
| `Key::P` | буква `P` |
| `Key::Q` | буква `Q` |
| `Key::R` | буква `R` |
| `Key::S` | буква `S` |
| `Key::T` | буква `T` |
| `Key::U` | буква `U` |
| `Key::V` | буква `V` |
| `Key::W` | буква `W` |
| `Key::X` | буква `X` |
| `Key::Y` | буква `Y` |
| `Key::Z` | буква `Z` |

Скобки и прочие знаки:

| Значение | Смысл |
|---|---|
| `Key::LeftBracket` | левая квадратная скобка `[` |
| `Key::Backslash` | обратная косая черта `\` |
| `Key::RightBracket` | правая квадратная скобка `]` |
| `Key::Grave` | обратный апостроф/тильда `` ` `` |

Управление и навигация:

| Значение | Смысл |
|---|---|
| `Key::Escape` | Escape |
| `Key::Enter` | Enter |
| `Key::Tab` | Tab |
| `Key::Backspace` | Backspace |
| `Key::Insert` | Insert |
| `Key::Delete` | Delete |
| `Key::Right` | стрелка вправо |
| `Key::Left` | стрелка влево |
| `Key::Down` | стрелка вниз |
| `Key::Up` | стрелка вверх |
| `Key::PageUp` | Page Up |
| `Key::PageDown` | Page Down |
| `Key::Home` | Home |
| `Key::End` | End |

Системные клавиши и блокировки:

| Значение | Смысл |
|---|---|
| `Key::CapsLock` | Caps Lock |
| `Key::ScrollLock` | Scroll Lock |
| `Key::NumLock` | Num Lock |
| `Key::PrintScreen` | Print Screen |
| `Key::Pause` | Pause |

Функциональные клавиши:

| Значение | Смысл |
|---|---|
| `Key::F1` | функциональная клавиша F1 |
| `Key::F2` | функциональная клавиша F2 |
| `Key::F3` | функциональная клавиша F3 |
| `Key::F4` | функциональная клавиша F4 |
| `Key::F5` | функциональная клавиша F5 |
| `Key::F6` | функциональная клавиша F6 |
| `Key::F7` | функциональная клавиша F7 |
| `Key::F8` | функциональная клавиша F8 |
| `Key::F9` | функциональная клавиша F9 |
| `Key::F10` | функциональная клавиша F10 |
| `Key::F11` | функциональная клавиша F11 |
| `Key::F12` | функциональная клавиша F12 |
| `Key::F13` | функциональная клавиша F13 |
| `Key::F14` | функциональная клавиша F14 |
| `Key::F15` | функциональная клавиша F15 |
| `Key::F16` | функциональная клавиша F16 |
| `Key::F17` | функциональная клавиша F17 |
| `Key::F18` | функциональная клавиша F18 |
| `Key::F19` | функциональная клавиша F19 |
| `Key::F20` | функциональная клавиша F20 |
| `Key::F21` | функциональная клавиша F21 |
| `Key::F22` | функциональная клавиша F22 |
| `Key::F23` | функциональная клавиша F23 |
| `Key::F24` | функциональная клавиша F24 |

Цифровой блок (keypad):

| Значение | Смысл |
|---|---|
| `Key::Keypad0` | цифра `0` на цифровом блоке |
| `Key::Keypad1` | цифра `1` на цифровом блоке |
| `Key::Keypad2` | цифра `2` на цифровом блоке |
| `Key::Keypad3` | цифра `3` на цифровом блоке |
| `Key::Keypad4` | цифра `4` на цифровом блоке |
| `Key::Keypad5` | цифра `5` на цифровом блоке |
| `Key::Keypad6` | цифра `6` на цифровом блоке |
| `Key::Keypad7` | цифра `7` на цифровом блоке |
| `Key::Keypad8` | цифра `8` на цифровом блоке |
| `Key::Keypad9` | цифра `9` на цифровом блоке |
| `Key::KeypadDecimal` | десятичный разделитель на цифровом блоке |
| `Key::KeypadDivide` | деление `/` на цифровом блоке |
| `Key::KeypadMultiply` | умножение `*` на цифровом блоке |
| `Key::KeypadSubtract` | вычитание `-` на цифровом блоке |
| `Key::KeypadAdd` | сложение `+` на цифровом блоке |
| `Key::KeypadEnter` | Enter на цифровом блоке |
| `Key::KeypadEqual` | равенство `=` на цифровом блоке |

Модификаторы:

| Значение | Смысл |
|---|---|
| `Key::LeftShift` | левый Shift |
| `Key::LeftControl` | левый Control |
| `Key::LeftAlt` | левый Alt |
| `Key::LeftSuper` | левая клавиша-супер (Command/Windows) |
| `Key::RightShift` | правый Shift |
| `Key::RightControl` | правый Control |
| `Key::RightAlt` | правый Alt |
| `Key::RightSuper` | правая клавиша-супер |

Прочее:

| Значение | Смысл |
|---|---|
| `Key::Menu` | контекстное меню |
| `Key::Count` | размер перечисления, а не клавиша (`Input::kKeyCount`) |

```cpp
// Перебираем весь диапазон: Key::Count — это размер, а не клавиша.
for (int i = 0; i < static_cast<int>(crossrender::Key::Count); ++i) {
    const crossrender::Key k = static_cast<crossrender::Key>(i);
    if (input.KeyDown(k)) ENG_LOGD("input", "удерживается %s", crossrender::KeyName(k));
}
```

### `enum class KeyAction : int`

Что именно произошло с клавишей. Именно это значение бэкенд передаёт в
`Input::OnKey`.

| Значение | Число | Смысл |
|---|---|---|
| `KeyAction::Release` | 0 | клавишу отпустили |
| `KeyAction::Press` | 1 | клавишу нажали впервые |
| `KeyAction::Repeat` | 2 | автоповтор удержания |

```cpp
// Ручная подача события: удобно в тестах и в скриптовых сценах.
input.BeginFrame();
input.OnKey(crossrender::Key::Enter, crossrender::KeyAction::Press, false);
ENG_ASSERT(input.KeyPressed(crossrender::Key::Enter));
input.OnKey(crossrender::Key::Enter, crossrender::KeyAction::Release, false);
```

### `enum class MouseButton : int`

Кнопки мыши. `Count` — размер массива состояний (`Input::kMouseCount`), а не
кнопка.

| Значение | Число | Смысл |
|---|---|---|
| `MouseButton::Left` | 0 | левая кнопка |
| `MouseButton::Right` | 1 | правая кнопка |
| `MouseButton::Middle` | 2 | средняя кнопка |
| `MouseButton::X1` | 3 | дополнительная кнопка 1 |
| `MouseButton::X2` | 4 | дополнительная кнопка 2 |
| `MouseButton::Count` | 5 | размер перечисления, а не кнопка |

```cpp
// Правая кнопка — контекстное меню, средняя — сброс камеры.
if (input.MousePressed(crossrender::MouseButton::Right)) ui.OpenContextMenu(input.MousePos());
if (input.MousePressed(crossrender::MouseButton::Middle)) camera.Reset();
```

### `enum class TouchPhase : int`

Фаза касания в `TouchPoint::phase`.

| Значение | Смысл |
|---|---|
| `TouchPhase::Down` | палец коснулся экрана; именно эта фаза создаёт новую точку |
| `TouchPhase::Move` | палец переместился |
| `TouchPhase::Up` | палец отпущен; точка удаляется из списка |
| `TouchPhase::Cancel` | касание отменено (жест перехвачен системой); точка удаляется |

```cpp
// Реакция на отпускание: превращаем жест в «клик» по миру.
for (int i = 0; i < input.TouchCount(); ++i) {
    const crossrender::TouchPoint& t = input.Touches()[i];
    if (t.phase == crossrender::TouchPhase::Up) SpawnAt(t.pos);
}
```

### `const char* KeyName(Key key)`

Человекочитаемое имя клавиши: `"A"`, `"Space"`, `"F11"`, для знаков — сам знак
(`","`, `"\\"`). Для `Key::Unknown`, `Key::Count` и любого значения вне
перечисления возвращает `"Unknown"` — указатель никогда не бывает `nullptr`,
поэтому результат безопасно печатать и сравнивать.

* **Возвращает:** указатель на статическую строку; освобождать её не нужно.
* **Контекст:** удобна для отладочного вывода и настроек управления.

```cpp
// Строка вида "LeftShift+W" для экрана управления.
const std::string combo = std::string(crossrender::KeyName(crossrender::Key::LeftShift)) + "+" +
                          crossrender::KeyName(crossrender::Key::W);
ENG_LOGI("input", "назначена комбинация %s", combo.c_str());
```

### `constexpr int kMaxTouches = 10`

Максимум одновременных точек касания, которые хранит `Input`. Массив точек
фиксированного размера, поэтому лишние пальцы игнорируются, а не выделяют
память. Это же число задаёт размер буфера, который возвращает
`Input::Touches()`.

```cpp
// Заранее предупреждаем, если жест требует больше пальцев, чем поддерживает движок.
if (pinchRequiredFingers > crossrender::kMaxTouches) {
    ENG_LOGW("input", "жест требует %d пальцев, движок хранит только %d", pinchRequiredFingers,
             crossrender::kMaxTouches);
}
```

### `struct TouchPoint`

Одно касание: идентификатор пальца, текущая и начальная позиции, фаза и сила
нажатия. Все поля имеют значения по умолчанию, поэтому `TouchPoint tp;` — это
корректная «отменённая» точка с `id == -1`. Заполняет структуру бэкенд, а
`Input::OnTouch` только складывает её в свой массив.

```cpp
// Готовим точку касания вручную — так же это делают мобильные бэкенды.
crossrender::TouchPoint tp;
tp.id = 0;
tp.pos = {320.0f, 240.0f};
tp.start = tp.pos;
tp.phase = crossrender::TouchPhase::Down;
tp.pressure = 1.0f;
input.OnTouch(tp);
```

### `i32 id`

Идентификатор пальца, который назначает операционная система. Он стабилен между
событиями одного касания, поэтому по `id` и находят уже существующую точку.
Значение `-1` означает «слот свободен»: `Input::OnTouch` создаёт новую точку
только для фазы `Down` и только если `touchCount_ < kMaxTouches`.

```cpp
// Ищем уже отслеживаемое касание, чтобы отличить новый палец от движения.
const crossrender::TouchPoint* tracked = nullptr;
for (int i = 0; i < input.TouchCount(); ++i) {
    if (input.Touches()[i].id == tp.id) tracked = &input.Touches()[i];
}
if (tracked == nullptr) ENG_LOGI("input", "новый палец id=%d", tp.id);
```

### `Vec2 pos`

Текущая позиция касания в **логических** координатах окна (на Android бэкенд
делит координаты события на плотность дисплея, на iOS и Web отдаёт точки, а не
пиксели). Именно это значение `Input::OnTouch` перекладывает в виртуальную
мышь, поэтому UI-код видит касание как обычный курсор.

```cpp
// Следим за пальцем, который ведёт прицел.
for (int i = 0; i < input.TouchCount(); ++i) {
    const crossrender::TouchPoint& t = input.Touches()[i];
    if (t.phase == crossrender::TouchPhase::Move) crosshair = t.pos;
}
```

### `Vec2 start`

Начальная позиция касания — задумана как точка, от которой считают смещение
жеста (свайп, перетаскивание). Важная оговорка: **текущие бэкенды присваивают
`start = pos` на каждом событии** (Android, iOS, Web), поэтому `start` фактически
совпадает с `pos` и не является устойчивым началом нажатия. Если нужен
настоящий «якорь» жеста, запоминайте позицию фазы `Down` сами.

```cpp
// Свой якорь жеста: запоминаем позицию в момент касания.
crossrender::Vec2 swipeStart{};
for (int i = 0; i < input.TouchCount(); ++i) {
    const crossrender::TouchPoint& t = input.Touches()[i];
    if (t.phase == crossrender::TouchPhase::Down) swipeStart = t.pos;
    if (t.phase == crossrender::TouchPhase::Up) {
        const crossrender::Vec2 swipe = t.pos - swipeStart;   // свой якорь надёжнее TouchPoint::start
        if (swipe.x > 80.0f) ui.NextPage();
    }
}
```

### `TouchPhase phase`

Что произошло с этим касанием. `Input::OnTouch` удаляет точку при `Up` и
`Cancel`; `Down` создаёт новую; `Move` только обновляет позицию. Если `Move`
или `Up` приходит для неизвестного `id`, он игнорируется.

```cpp
// Индикатор удержания: считаем только активные касания.
int active = 0;
for (int i = 0; i < input.TouchCount(); ++i) {
    const crossrender::TouchPhase p = input.Touches()[i].phase;
    if (p == crossrender::TouchPhase::Down || p == crossrender::TouchPhase::Move) ++active;
}
ui.Label(crossrender::Fmt("пальцев: %d", active), rect);
```

### `f32 pressure`

Сила нажатия в диапазоне `[0, 1]`. Не все платформы её отдают: Android и iOS
передают значение датчика (с подстановкой `1.0`, если устройство вернуло ноль),
а Web всегда ставит `1.0`, потому что DOM-событие касания силу не сообщает.
Значение по умолчанию — `1.0f`.

```cpp
// Рисуем кисть тем толще, чем сильнее нажатие (на Web всегда максимум).
const float width = 2.0f + 6.0f * crossrender::Clamp(input.Touches()[0].pressure, 0.0f, 1.0f);
brush.Stroke(lastPoint, input.Touches()[0].pos, width);
```

### `class Input`

Снимок состояния ввода за кадр: массивы клавиш, кнопок мыши, активных касаний,
накопленный текст и простое цифровое состояние геймпада. Класс платформенно
независим: весь разбор событий ОС делают бэкенды в `Window<Os>.cpp`, а логика
краёв, накоплений и виртуальной мыши живёт в `engine/src/platform/Input.cpp`.

Экземпляр создаётся один раз внутри `Window` и доступен через `GetInput()`.
Копировать `Input` бессмысленно (но технически возможно), а создавать свой
экземпляр стоит только в тестах:

```cpp
// Тест краевой логики без окна и дисплея: Input полностью автономен.
crossrender::Input in;
in.BeginFrame();
in.OnKey(crossrender::Key::W, crossrender::KeyAction::Press, false);
ENG_ASSERT(in.KeyPressed(crossrender::Key::W));
in.BeginFrame();                       // край сбрасывается, удержание остаётся
ENG_ASSERT(in.KeyDown(crossrender::Key::W) && !in.KeyPressed(crossrender::Key::W));
```

### `static constexpr int kKeyCount`

Размер массивов клавиш, равный `static_cast<int>(Key::Count)` (сегодня 119).
Используется во внутренних циклах и удобен для прикладного кода, который
перебирает все клавиши или заводит свои таблицы той же длины.

```cpp
// Своя таблица привязок ровно на все клавиши перечисления.
std::array<std::string, crossrender::Input::kKeyCount> keyBindings{};
keyBindings[static_cast<int>(crossrender::Key::Space)] = "прыжок";
```

### `static constexpr int kMouseCount`

Размер массивов кнопок мыши, равный `static_cast<int>(MouseButton::Count)` (5).
Нужен, если вы пишете собственные обёртки над состоянием кнопок.

```cpp
// Сколько кнопок мыши реально отслеживает движок.
ENG_LOGI("input", "движок ведёт %d кнопок мыши", crossrender::Input::kMouseCount);
```

### `void BeginFrame()`

Начинает новый кадр ввода: очищает краевое состояние — `KeyPressed`,
`KeyReleased`, `MousePressed`, `MouseReleased`, `MouseDoubleClick`, `MouseDelta`,
`ScrollDelta` и `TextInput`. Удержания (`KeyDown`, `MouseDown`) не трогает.
Вызывайте её **до** `Window::PollEvents()`, иначе края, пришедшие из очереди
событий, будут стёрты.

```cpp
// Начало кадра: сбрасываем края, затем принимаем события ОС.
input.BeginFrame();
window.PollEvents();
if (input.KeyPressed(crossrender::Key::Space)) player.Jump();
```

### `void EndFrame()`

Завершает кадр ввода: запоминает текущую позицию мыши в `prevMouse_`,
подготавливая следующий кадр. Игровую логику менять не нужно, но вызывать метод
стоит каждый кадр, до `SwapBuffers()`, чтобы накопления не «переехали» в
следующий кадр.

```cpp
// Конец кадра: сначала игровая логика, потом фиксация состояния ввода.
UpdateGame(dt, input);
input.EndFrame();
window.SwapBuffers();
```

### `void OnKey(Key key, KeyAction action, bool repeat)`

Подаёт событие клавиши. Это основной вход для платформенных бэкендов.
Поведение по действиям:

* `Press` — выставляет `keyPressed` только если клавиша ещё не была нажата, и
  всегда поднимает `keyDown`;
* `Release` — выставляет `keyReleased` только если клавиша была нажата, и
  опускает `keyDown`;
* `Repeat` — выставляет **и** `keyDown`, **и** `keyPressed`.

Параметр `repeat` сохранён для совместимости, но реализацией **не используется**
(в `Input.cpp` стоит `(void)repeat`). Значение `key` вне диапазона
`[0, kKeyCount)` подменяется на индекс `0` (`Key::Unknown`) — то есть
`KeyDown(static_cast<Key>(999))` читает состояние `Unknown`.

```cpp
// Так бэкенд превращает удержание клавиши в автоповтор для текстового поля.
input.OnKey(crossrender::Key::Backspace, crossrender::KeyAction::Press, false);
input.OnKey(crossrender::Key::Backspace, crossrender::KeyAction::Repeat, true);   // repeat игнорируется
ENG_ASSERT(input.KeyDown(crossrender::Key::Backspace) && input.KeyPressed(crossrender::Key::Backspace));
```

### `bool KeyDown(Key k) const`

`true`, пока клавиша удерживается. Сбрасывается только событием `Release`.
Основной запрос для непрерывных действий.

* **Возвращает:** состояние удержания; для значения вне диапазона — состояние
  `Key::Unknown`.

```cpp
// Непрерывное движение: чем дольше держат, тем дальше уезжает игрок.
const float speed = 240.0f;
if (input.KeyDown(crossrender::Key::Right)) player.pos.x += speed * dt;
if (input.KeyDown(crossrender::Key::Left)) player.pos.x -= speed * dt;
```

### `bool KeyPressed(Key k) const`

`true` только в том кадре, в котором клавиша перешла из отпущенного состояния
в нажатое (край). `BeginFrame` следующего кадра сбросит флаг, даже если клавиша
всё ещё удерживается. Для одиночных действий.

* **Возвращает:** `true`, если нажатие произошло в текущем кадре.

```cpp
// Одиночное действие: прыжок не должен повторяться каждый кадр удержания.
if (input.KeyPressed(crossrender::Key::Space)) player.Jump();
```

### `bool KeyReleased(Key k) const`

`true` только в кадре отпускания клавиши. Полезно для завершения действий,
начатых по `KeyPressed` (натянуть лук — отпустить стрелу).

```cpp
// Заряжаем по нажатию, стреляем по отпусканию.
if (input.KeyPressed(crossrender::Key::E)) bow.BeginDraw();
if (input.KeyReleased(crossrender::Key::E)) bow.Release();
```

### `bool AnyKeyDown() const`

`true`, если удержана хотя бы одна клавиша — включая модификаторы и системные.
Типичное применение — экран «нажмите любую клавишу».

```cpp
// Стартовый экран: ждём любое нажатие, чтобы войти в меню.
if (state == State::Title && input.AnyKeyDown()) state = State::Menu;
```

### `bool CtrlDown() const`

`true`, если удержан левый **или** правый Control. Готовая проверка модификатора
без ручного перебора двух клавиш.

```cpp
// Ctrl+S — быстрое сохранение.
if (input.CtrlDown() && input.KeyPressed(crossrender::Key::S)) SaveGame();
```

### `bool ShiftDown() const`

`true`, если удержан левый **или** правый Shift.

```cpp
// Shift+клик — выделение диапазона в списке.
if (input.ShiftDown() && input.MousePressed(crossrender::MouseButton::Left)) list.SelectRange();
```

### `bool AltDown() const`

`true`, если удержан левый **или** правый Alt.

```cpp
// Alt+Enter — переключение полноэкранного режима.
if (input.AltDown() && input.KeyPressed(crossrender::Key::Enter)) {
    window.SetMode(window.IsFullscreen() ? crossrender::WindowMode::Windowed : crossrender::WindowMode::Fullscreen);
}
```

### `bool SuperDown() const`

`true`, если удержана левая **или** правая клавиша-супер (Command на macOS,
Windows-клавиша на Windows, Super на Linux).

```cpp
// Command/Windows+Q — выход, привычный для конкретной ОС.
if (input.SuperDown() && input.KeyPressed(crossrender::Key::Q)) window.RequestClose();
```

### `void OnText(u32 codepoint)`

Добавляет одну кодовую точку Unicode в буфер `TextInput()`. Бэкенды вызывают
метод для «печатных» символов (управляющие и функциональные клавиши
отфильтровываются, суррогатные пары UTF-16 склеиваются в одну точку). Именно
так правильно читать набранный текст — независимо от раскладки.

```cpp
// Ввод имени игрока: принимаем только буквы и цифры.
for (crossrender::u32 cp : input.TextInput()) {
    if ((cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9')) name.push_back(static_cast<char>(cp));
}
```

### `const std::vector<u32>& TextInput() const`

Все кодовые точки, набранные в текущем кадре, в порядке поступления.
Очищается каждый `BeginFrame`, поэтому обрабатывать текст нужно в том же кадре.
Возвращается ссылка на внутренний вектор.

* **Возвращает:** ссылку на буфер кодовых точек; пустой вектор, если ввод не
  поступал. Ссылка действительна до следующего вызова `OnText`.

```cpp
// Поле поиска: печатаем всё, что пришло за кадр, и правим Backspace отдельно.
for (crossrender::u32 cp : input.TextInput()) search.Append(cp);
if (input.KeyPressed(crossrender::Key::Backspace) && !search.Empty()) search.PopBack();
```

### `void OnMouseMove(Vec2 pos)`

Обновляет позицию мыши и **прибавляет** смещение к `MouseDelta()`:
`mouseDelta_ += pos - mouse_`. Поэтому несколько перемещений внутри одного кадра
корректно суммируются, а `BeginFrame` обнуляет накопление. Координаты — в
логическом пространстве окна.

```cpp
// Ручная подача движения — как это делает бэкенд при обработке события.
input.OnMouseMove({100.0f, 50.0f});
input.OnMouseMove({140.0f, 50.0f});
ENG_ASSERT(crossrender::NearlyEqual(input.MouseDelta().x, 100.0f, 1e-5f));   // 100 + 40
```

### `void OnMouseButton(MouseButton b, bool down, Vec2 pos)`

Подаёт нажатие или отпускание кнопки мыши и заодно фиксирует позицию курсора.
При `down == true` поднимает `MouseDown`/`MousePressed` и запускает проверку
двойного клика; при `false` — опускает `MouseDown` и поднимает `MouseReleased`
(только если кнопка была нажата). Двойной клик — два нажатия в пределах 400 мс;
порог расстояния 6 px из комментария исходника **не проверяется**.

```cpp
// Обработка клика в мире: событие + позиция одним вызовом.
input.OnMouseButton(crossrender::MouseButton::Left, true, {512.0f, 300.0f});
if (input.MouseDoubleClick(crossrender::MouseButton::Left)) building.Upgrade();
```

### `void OnScroll(Vec2 delta)`

Прибавляет дельту колеса к `ScrollDelta()`. Знак и масштаб задаёт платформа:
macOS умножает «точные» дельты трекпада на `0.1`, а событие масштабирования
(`magnify`) превращает в вертикальную прокрутку. Накопление сбрасывается в
`BeginFrame`.

```cpp
// Прокрутка списка с ограничением, чтобы содержимое не уехало за края.
input.OnScroll({0.0f, 1.0f});
scrollOffset -= input.ScrollDelta().y * rowHeight;
scrollOffset = crossrender::Clamp(scrollOffset, 0.0f, maxOffset);
```

### `Vec2 MousePos() const`

Последняя позиция мыши, доставленная событием, в **логических** координатах
окна (не в пикселях кадрового буфера). Это то, что читает UI; если нужно
сравнить с чем-то, измеренным в пикселях, делите или умножайте на
`Window::DpiScale()` (см. ловушку координат в `## Обзор`).

```cpp
// Наводим прицел на точку в логических координатах того же пространства.
const crossrender::Vec2 target = input.MousePos();
turret.AimAt(crossrender::Normalize(target - turret.pos));
```

### `Vec2 MouseDelta() const`

Суммарное перемещение мыши за кадр — разность с позицией на момент
`BeginFrame`. Основной способ читать движение в режиме относительного курсора
(`SetCursorMode(2)`), когда абсолютная позиция не важна.

```cpp
// Обзор от первого лица: чувствительность и инверсия по вертикали.
const crossrender::Vec2 d = input.MouseDelta();
camera.yaw += d.x * lookSensitivity;
camera.pitch -= d.y * lookSensitivity;
```

### `Vec2 ScrollDelta() const`

Накопленная за кадр прокрутка. Компонента `y` — вертикальная (положительная
обычно «вверх/от себя»), `x` — горизонтальная. Обнуляется в `BeginFrame`.

```cpp
// Масштаб камеры колесом мыши.
if (input.ScrollDelta().y != 0.0f) {
    camera.zoom = crossrender::Clamp(camera.zoom * (1.0f - input.ScrollDelta().y * 0.1f), 0.25f, 4.0f);
}
```

### `bool MouseDown(MouseButton b = MouseButton::Left) const`

`true`, пока кнопка удерживается. Параметр по умолчанию — левая кнопка, поэтому
типичный вызов пишется без аргумента.

```cpp
// Перетаскивание объектов: держим левую кнопку и тащим.
if (input.MouseDown() && selected >= 0) objects[selected].pos = input.MousePos();
```

### `bool MousePressed(MouseButton b = MouseButton::Left) const`

`true` только в кадре нажатия кнопки (край). Подходит для кликов и выделения.

```cpp
// Выбор юнита одиночным кликом.
if (input.MousePressed()) selection = PickUnitAt(input.MousePos());
```

### `bool MouseReleased(MouseButton b = MouseButton::Left) const`

`true` только в кадре отпускания кнопки. Завершает перетаскивание.

```cpp
// Бросаем объект там, где отпустили кнопку.
if (input.MouseReleased()) DropDraggedAt(input.MousePos());
```

### `bool MouseDoubleClick(MouseButton b = MouseButton::Left) const`

`true` только в кадре второго нажатия, если между нажатиями прошло меньше
400 мс. Проверяется **только время** — расстояние между кликами не учитывается,
хотя комментарий в исходнике упоминает порог 6 px. После срабатывания счётчик
времени сбрасывается в ноль, поэтому «тройной клик» парой не считается.

```cpp
// Двойной клик по элементу — переход в режим редактирования имени.
if (input.MouseDoubleClick()) item.BeginRename();
```

### `void OnTouch(const TouchPoint& tp)`

Подаёт событие касания и ведёт список активных точек:

* если точка с таким `id` уже есть — обновляет её, а по `Up`/`Cancel` удаляет
  (со сдвигом остальных элементов массива);
* если точки нет и фаза `Down` — добавляет её, пока `touchCount_ < kMaxTouches`;
* **любое** касание дополнительно двигает виртуальную мышь: вызывает
  `OnMouseMove(tp.pos)` и нажимает/отпускает левую кнопку. Благодаря этому
  UI-код, написанный под мышь, работает на телефоне без изменений.

```cpp
// Бэкенд подаёт касания, а игра читает и тач, и «мышь» одинаково.
crossrender::TouchPoint tp;
tp.id = 1;
tp.pos = {200.0f, 150.0f};
tp.phase = crossrender::TouchPhase::Down;
input.OnTouch(tp);
ENG_ASSERT(input.MouseDown(crossrender::MouseButton::Left));   // виртуальная мышь нажата
```

### `const TouchPoint* Touches() const`

Указатель на внутренний массив касаний. Читать нужно только первые
`TouchCount()` элементов — остальные слоты содержат мусор от прошлых касаний.
Указатель указывает внутрь объекта `Input` и живёт столько же, сколько он сам,
но содержимое меняется при каждом `OnTouch`.

* **Возвращает:** указатель на массив из `kMaxTouches` элементов.
* **Контекст:** вызывайте после `PollEvents()` и обрабатывайте в том же кадре.

```cpp
// Рисуем маркеры всех активных пальцев для отладки мультитача.
const crossrender::TouchPoint* pts = input.Touches();
for (int i = 0; i < input.TouchCount(); ++i) {
    debugDraw.Circle(pts[i].pos, 24.0f, crossrender::Color::Yellow);
}
```

### `int TouchCount() const`

Число активных касаний в текущем кадре. Уменьшается при `Up`/`Cancel`, поэтому
после отпускания всех пальцев возвращает `0`.

```cpp
// Различаем жест двумя пальцами и одиночное касание.
if (input.TouchCount() == 2) camera.ZoomBy(gesturePinchDelta);
else if (input.TouchCount() == 1) crosshair = input.Touches()[0].pos;
```

### `void SetGamepad(int index, bool connected, const float* axes, int axisCount, u32 buttons)`

Задаёт цифровое состояние геймпада. Это **push-API**: внутри движка метод
сегодня никто не вызывает — состояние обязан подавать сам игровой код (из своей
библиотеки ввода или из платформенного моста). Индекс `index` должен быть в
диапазоне `[0, 4)`, иначе вызов молча игнорируется. `axisCount` зажимается в
`[0, 8]`; если `axes == nullptr`, оси заполняются нулями. `buttons` — битовая
маска кнопок, её движок не интерпретирует.

```cpp
// Читаем геймпад своей библиотекой и раз в кадр отдаём снимок движку.
float axes[8] = {leftX, leftY, rightX, rightY, lt, rt, 0.0f, 0.0f};
crossrender::u32 buttons = (pad.A() ? 1u : 0u) | (pad.B() ? 2u : 0u);
input.SetGamepad(0, pad.IsConnected(), axes, 8, buttons);
```

### `bool GamepadConnected(int index = 0) const`

`true`, если для слота `index` был подан `connected == true`. Индекс вне
диапазона `[0, 4)` даёт `false`.

```cpp
// Подсказка управления зависит от того, подключён ли геймпад.
ui.Label(input.GamepadConnected(0) ? "A — прыжок" : "Space — прыжок", hintRect);
```

### `u32 GamepadButtons(int index = 0) const`

Битовая маска кнопок, переданная в `SetGamepad`. Смысл битов определяет
приложение; движок только хранит значение. Для индекса вне диапазона — `0`.

```cpp
// Бит 0 — прыжок: своя раскладка кнопок поверх маски движка.
const crossrender::u32 pad = input.GamepadButtons(0);
if ((pad & 1u) != 0u) player.Jump();
```

### `Vec2 GamepadStick(int index = 0, int stick = 0) const`

Возвращает две оси стика: `stick = 0` берёт оси `axes[0]` и `axes[1]`,
`stick = 1` — `axes[2]` и `axes[3]`. Если осей не хватает или индекс геймпада
вне диапазона, возвращается `{0, 0}`. Мёртвую зону и кривые применяет
вызывающий код.

```cpp
// Движение левым стиком с мёртвой зоной.
const crossrender::Vec2 move = input.GamepadStick(0, 0);
if (crossrender::Length(move) > 0.2f) player.velocity = crossrender::Normalize(move) * walkSpeed;
```

### `enum class WindowMode`

Режим окна. Влияет и на стиль окна, и на то, что вернёт `Window::IsFullscreen()`.

| Значение | Смысл |
|---|---|
| `WindowMode::Windowed` | обычное окно с рамкой |
| `WindowMode::Fullscreen` | полноэкранный режим (на Linux — через EWMH, с запасным вариантом «окно во весь экран», если оконный менеджер не поддерживает `_NET_WM_STATE`) |
| `WindowMode::Borderless` | окно без рамки, обычно во весь экран |

```cpp
// Переключаем режим по Alt+Enter и запоминаем выбор.
window.SetMode(window.IsFullscreen() ? crossrender::WindowMode::Windowed : crossrender::WindowMode::Fullscreen);
settings.fullscreen = window.IsFullscreen();
```

### `struct WindowDesc`

Описание окна, которое передаётся в `Window::Create`. Это простой агрегат со
значениями по умолчанию: 1280x720, изменяемое окно, vsync, поддержка HiDPI,
4x MSAA, оконный режим, буфер глубины без трафарета, OpenGL 3.3 core. Достаточно
изменить нужные поля — остальные уже разумны.

Не все поля одинаково понимаются всеми бэкендами (подробности в разделах ниже):
например, `minWidth`/`minHeight` учитывают только macOS и Linux, `highDpi` —
только macOS, а `transparent` — только Windows и Web.

```cpp
// Типичная настройка: окно поменьше, без vsync, MSAA выключен — для отладки.
crossrender::WindowDesc desc;
desc.title = "Отладка";
desc.width = 960;
desc.height = 540;
desc.vsync = false;
desc.msaaSamples = 0;
if (!window.Create(desc)) return 1;
```

### `std::string title`

Заголовок окна. Значение по умолчанию — `"CrossRender"`. На macOS, Windows,
Linux и Web попадает в системный заголовок; на Android и iOS заголовка у окна
нет, но значение сохраняется и возвращается через `Window::Title()`.

```cpp
// В заголовок — имя сцены и счётчик кадров.
crossrender::WindowDesc desc;
desc.title = "Уровень 1";
desc.width = 1280;
desc.height = 720;
window.Create(desc);
window.SetTitle("Уровень 1 — 60 FPS");
```

### `int width`

Ширина **логического** клиентского области окна в единицах платформы. По
умолчанию `1280`. На HiDPI-дисплее не равна ширине кадрового буфера: реальный
размер в пикселях даёт `Window::FramebufferWidth()`.

```cpp
// Центрируем игровое поле по логической ширине окна.
crossrender::WindowDesc desc;
desc.width = 1600;
desc.height = 900;
if (window.Create(desc)) field.x = (window.Width() - field.w) * 0.5f;
```

### `int height`

Высота логической клиентской области окна. По умолчанию `720`. Вместе с
`width` задаёт начальное соотношение сторон (`Window::Aspect()`).

```cpp
// Проверяем, что окно достаточно высокое для панели инструментов.
crossrender::WindowDesc desc;
desc.width = 1024;
desc.height = 768;
if (window.Create(desc) && window.Height() >= 600) ShowToolbar();
```

### `int minWidth`

Минимальная ширина окна, которую разрешает оконная система. По умолчанию `320`.
Сегодня применяется на macOS (`setMinSize`) и Linux (size hints); на Windows и
мобильных игнорируется.

```cpp
// Не даём игроку сжать окно до нечитаемого состояния.
crossrender::WindowDesc desc;
desc.minWidth = 800;
desc.minHeight = 600;
desc.width = 1280;
desc.height = 720;
```

### `int minHeight`

Минимальная высота окна. По умолчанию `240`. Как и `minWidth`, действует на
macOS и Linux; проверка `> 0` есть только у Linux.

```cpp
// Минимум под панель инструментов и одну строку консоли.
crossrender::WindowDesc desc;
desc.minWidth = 640;
desc.minHeight = 400;
desc.resizable = true;
```

### `bool resizable`

Разрешить пользователю менять размер окна. По умолчанию `true`. Учитывается на
macOS, Windows и Linux (стиль окна); в браузере и на мобильных размером владеет
страница или ОС, поэтому поле не действует.

```cpp
// Игровое окно — изменяемое, окно заставки — фиксированное.
crossrender::WindowDesc splash;
splash.resizable = false;
splash.width = 640;
splash.height = 360;
```

### `bool vsync`

Включить вертикальную синхронизацию. По умолчанию `true`. Поддерживается всеми
бэкендами: macOS — swap interval `NSOpenGLContext`, Windows — WGL, Linux — GLX,
Web — Emscripten, Android — `eglSwapInterval`, iOS — частота кадров
`CADisplayLink`. Менять на ходу можно через `Window::SetVSync`.

```cpp
// Для замеров производительности синхронизацию лучше выключить.
crossrender::WindowDesc desc;
desc.vsync = false;    // получаем «сырой» FPS
window.Create(desc);
```

### `bool highDpi`

Учитывать плотность пикселей дисплея. По умолчанию `true`. Сегодня это поле
читает только macOS (`wantsBestResolutionOpenGLSurface`), остальные бэкенды и
так всегда считают кадровый буфер отдельно от логического размера — Windows
всегда работает как per-monitor DPI aware, а Web и мобильные берут масштаб из
системы.

```cpp
// Сглаживание по пикселям дисплея: узнаём реальный масштаб после создания окна.
crossrender::WindowDesc desc;
desc.highDpi = true;
if (window.Create(desc)) ENG_LOGI("app", "DPI дисплея: %.2f", window.DpiScale());
```

### `int msaaSamples`

Число выборок мультисэмплинга (MSAA, сглаживание краёв). По умолчанию `4`;
значение `<= 1` выключает сглаживание. macOS, Windows и Linux пробуют запрошенное
число и при отказе драйвера повторяют попытку без MSAA с предупреждением; на Web
поле превращается в булев флаг `antialias`; на Android и iOS мультисэмплинг не
поддерживается вовсе — бэкенды один раз пишут предупреждение.

```cpp
// Пиксель-арт: сглаживание только испортит картинку.
crossrender::WindowDesc desc;
desc.msaaSamples = 0;
window.Create(desc);
```

### `WindowMode mode`

Начальный режим окна. По умолчанию `WindowMode::Windowed`.

```cpp
// Запускаемся сразу в безрамочном полноэкранном режиме.
crossrender::WindowDesc desc;
desc.mode = crossrender::WindowMode::Borderless;
if (!window.Create(desc)) return 1;
```

### `bool transparent`

Прозрачный фон окна. По умолчанию `false`. Реально поддерживается на Windows
(расширенный стиль `WS_EX_LAYERED`) и в Web (alpha у WebGL-контекста); на macOS,
Linux, Android и iOS поле сейчас не используется.

```cpp
// Полупрозрачный оверлей поверх рабочего стола (Windows/Web).
crossrender::WindowDesc desc;
desc.transparent = true;
desc.width = 480;
desc.height = 320;
```

### `bool depthBuffer`

Создавать буфер глубины. По умолчанию `true` — нужно для 3D. Значение `false`
экономит память и корректно для чисто 2D-игр. Учитывается на macOS, Windows,
Linux, Web и iOS; на Android конфигурацию EGL выбирает платформа.

```cpp
// Чисто 2D-проекту глубина не нужна.
crossrender::WindowDesc desc;
desc.depthBuffer = false;
desc.stencilBuffer = false;
window.Create(desc);
```

### `bool stencilBuffer`

Создавать буфер трафарета. По умолчанию `false`. Включайте, если используете
трафаретные эффекты (маски, отражения, тени через stencil). Игнорируется на
Android вместе с `depthBuffer`.

```cpp
// Включаем трафарет для маскирования области рендера.
crossrender::WindowDesc desc;
desc.depthBuffer = true;
desc.stencilBuffer = true;
window.Create(desc);
```

### `int glMajor`

Старшая цифра запрашиваемой версии OpenGL. По умолчанию `3`. Windows и Linux
используют её вместе с `glMinor` (значение `<= 0` заменяется на 3); Web
трактует `glMajor >= 3` как «нужен WebGL 2»; macOS всегда запрашивает профиль
3.2 core, а Android и iOS работают на GLES 3 — там поле не влияет ни на что.

```cpp
// Просим ядро 3.3 — минимум, который ожидают шейдеры движка.
crossrender::WindowDesc desc;
desc.glMajor = 3;
desc.glMinor = 3;
```

### `int glMinor`

Младшая цифра версии OpenGL. По умолчанию `3`, то есть вместе с `glMajor`
получается OpenGL 3.3 core. На Web младшая версия не используется (WebGL 2
всегда `3.0`).

```cpp
// Явно фиксируем 3.3 core для совместимости с шейдерами.
crossrender::WindowDesc desc;
desc.glMajor = 3;
desc.glMinor = 3;
desc.depthBuffer = true;
```

### `struct WindowCallbacks`

Набор необязательных обработчиков событий окна, которые не сводятся к
опросному состоянию `Input`: изменение размера, закрытие, фокус, перетаскивание
файлов и смена DPI. Все поля — пустые `std::function`, поэтому не назначенный
обработчик просто не вызывается. Экземпляр лежит в публичном поле
`Window::callbacks`.

Важно: `onDropFiles` и `onFilesDropped` объявлены, но **ни один бэкенд их не
вызывает** — drag-and-drop файлов пока не подключён.

```cpp
// Подписываемся до Create: часть событий может прийти уже при создании окна.
window.callbacks.onClose = [&] { SaveGame(); };
window.callbacks.onResize = [](int w, int h) { ENG_LOGI("app", "ресайз %dx%d", w, h); };
if (!window.Create(desc)) return 1;
```

### `std::function<void(int, int)> onResize`

Вызывается при изменении размера **кадрового буфера** в пикселях: первый
аргумент — ширина, второй — высота. Это именно пиксели, а не логический размер:
на HiDPI-дисплее значения больше `Width()`/`Height()` в `DpiScale()` раз. Все
бэкенды, у которых размер вообще меняется, вызывают его в том числе при смене
DPI.

```cpp
// Пересоздаём render target под новый размер кадрового буфера.
window.callbacks.onResize = [&](int w, int h) {
    target = crossrender::RenderTarget::Create(w, h);
    ENG_LOGI("app", "кадровый буфер: %dx%d", w, h);
};
```

### `std::function<void()> onClose`

Вызывается ровно один раз, когда окно просят закрыть: из `RequestClose()`
(в том числе при нажатии системной кнопки закрытия). Повторный `RequestClose()`
обработчик не вызовет.

```cpp
// Спрашиваем подтверждение и только потом выходим.
window.callbacks.onClose = [&] {
    if (dirty) pendingExit = true;      // покажем диалог сохранения
    else window.RequestClose();
};
```

### `std::function<void(bool)> onFocus`

Сообщает о получении (`true`) и потере (`false`) фокуса. На Android и iOS этим
же способом передаётся уход приложения в фон и возврат в foreground; в Web сюда
же отображается потеря/восстановление контекста WebGL, потому что отдельного
хука контекста в `WindowCallbacks` нет (он есть в `AppHooks` из
`crossrender/platform/Platform.h`).

```cpp
// Ставим игру на паузу, когда окно теряет фокус.
window.callbacks.onFocus = [&](bool focused) {
    paused = !focused;
    ENG_LOGI("app", focused ? "фокус получен" : "фокус потерян");
};
```

### `std::function<void(Vec2)> onDropFiles`

Задуман как обработчик перетаскивания одного файла: аргумент — позиция курсора
в момент сброса. **Сегодня не вызывается ни одним бэкендом** — drag-and-drop
файлов в движке не подключён. Поле оставлено как точка расширения; не полагайтесь
на него в рабочем коде.

```cpp
// Точка расширения: если бэкенд когда-нибудь начнёт вызывать onDropFiles,
// получим логические координаты сброса.
window.callbacks.onDropFiles = [](crossrender::Vec2 where) {
    ENG_LOGI("app", "файл сброшен в точке %.0f, %.0f", where.x, where.y);
};
```

### `std::function<void(const std::vector<std::string>&)> onFilesDropped`

Задуман как обработчик перетаскивания нескольких файлов: получает список путей.
**Также не вызывается ни одним бэкендом** — как и `onDropFiles`, это только
объявление. Проверяйте, что список не пуст, если будете использовать его в
будущем.

```cpp
// Точка расширения: пакетная загрузка ассетов из перетащенных файлов.
window.callbacks.onFilesDropped = [&](const std::vector<std::string>& paths) {
    for (const std::string& p : paths) assets.QueueLoad(p);
    ENG_LOGI("assets", "в очередь добавлено файлов: %d", static_cast<int>(paths.size()));
};
```

### `std::function<void(f32)> onDpiChanged`

Вызывается при смене масштаба дисплея: аргумент — новый `DpiScale`. Работает на
macOS (смена backing properties), Windows (`WM_DPICHANGED`), Android (смена
конфигурации) и Web (изменение `devicePixelRatio`). На Linux и iOS не
вызывается. После него обычно приходит и `onResize` с новым размером кадрового
буфера.

```cpp
// Пересчитываем размеры интерфейса при переносе окна на другой монитор.
window.callbacks.onDpiChanged = [&](crossrender::f32 dpi) {
    ui.SetScale(dpi);
    ENG_LOGI("app", "новый масштаб DPI: %.2f", dpi);
};
```

### `class Window`

Окно операционной системы вместе с контекстом OpenGL и собственным экземпляром
`Input`. Класс неперемещаемый и некопируемый (`Window(const Window&) = delete`),
владеет платформенными ресурсами через `std::unique_ptr<Impl>` и освобождает их
в `Destroy()` и деструкторе. Один поток — один текущий контекст: если вы
рендерите из нескольких потоков, вызывайте `MakeCurrent()` на нужном потоке.

Порядок работы: сконфигурировать `WindowDesc` и обработчики → `Create` →
цикл `BeginFrame` / `PollEvents` / логика / `EndFrame` / `SwapBuffers` →
`Destroy`. `Input&` берётся из окна через `GetInput()`.

```cpp
// Минимальный корректный жизненный цикл окна.
crossrender::Window window;
crossrender::WindowDesc desc;
desc.width = 800;
desc.height = 600;
if (!window.Create(desc)) return;          // нет дисплея — не продолжаем

window.callbacks.onClose = [&] { /* сохранить прогресс */ };
while (!window.ShouldClose()) {
    window.GetInput().BeginFrame();
    window.PollEvents();
    window.GetInput().EndFrame();
    window.SwapBuffers();
}
window.Destroy();
```

### `Window()`

Создаёт объект окна без окна: выделяет только внутреннюю структуру `Impl` и
пустой `Input`. Никаких ресурсов ОС не захватывается — окно и контекст
появляются позже, в `Create`. Конструктор безопасен на любой платформе, даже
если дисплея нет.

```cpp
// Окно можно создать заранее и держать как поле сцены/приложения.
class App {
public:
    App() = default;
    bool Init(const crossrender::WindowDesc& desc) { return window_.Create(desc); }

private:
    crossrender::Window window_;   // конструктор ничего не создаёт в ОС
};
```

### `~Window()`

Освобождает платформенные ресурсы: вызывает `Destroy()`, который уничтожает
контекст OpenGL, окно, курсоры и прочие объекты ОС. Деструктор не виртуальный —
класс не предназначен для наследования. Явный `Destroy()` перед выходом из
`main` полезен, чтобы освободить ресурсы до завершения процесса.

```cpp
// Явное уничтожение: освобождаем контекст до выгрузки библиотек платформы.
{
    crossrender::Window window;
    if (window.Create(crossrender::WindowDesc{})) {
        window.Destroy();   // то же самое сделает и деструктор
    }
}
```

### `bool Create(const WindowDesc& desc)`

Создаёт нативное окно и контекст OpenGL по описанию `desc`. Возвращает `false`,
если окно или контекст создать не удалось, и пишет причину в лог (`ENG_LOGE`);
после неудачи объект остаётся в том же «пустом» состоянии, в каком был до
вызова. Обработчики `Window::callbacks` должны быть назначены **до** `Create`,
потому что часть событий (фокус, ресайз) может прийти уже во время создания.

Причины отказа зависят от платформы: на Linux — нет `DISPLAY` или подходящего
GLX-визуала, на Windows — не зарегистрирован класс окна или не выбран формат
пикселей, на macOS — не создался `NSOpenGLPixelFormat`/`NSOpenGLContext`, в Web —
нет canvas или WebGL-контекста, на Android — нативное окно ещё не пришло из
`APP_CMD_INIT_WINDOW`, на iOS — нет EAGL-контекста. Вызывающий код должен
обработать `false` (в тестах это `ENG_SKIP`).

* **Возвращает:** `true`, если окно и контекст готовы к работе.
* **Контекст:** вызывать из главного потока; повторный `Create` без `Destroy`
  не предусмотрен.
* **Ограничение:** требует настоящего дисплея и драйвера — headless-контекст
  живёт в `crossrender/platform/Platform.h`.

```cpp
// Всегда проверяем результат: без окна дальнейшая работа невозможна.
crossrender::Window window;
crossrender::WindowDesc desc;
desc.title = "Сцена";
desc.width = 1280;
desc.height = 720;
if (!window.Create(desc)) {
    ENG_LOGW("app", "окно не создано: работаем в headless-режиме");
    return RunHeadless();
}
ENG_LOGI("app", "окно готово, кадровый буфер %dx%d", window.FramebufferWidth(),
         window.FramebufferHeight());
```

### `void Destroy()`

Уничтожает окно и контекст, освобождая ресурсы ОС. Идемпотентен: повторный
вызов ничего не делает. Приватные поля сбрасываются, но сам объект `Window`
остаётся пригодным для нового `Create`. Деструктор вызывает `Destroy`
автоматически.

```cpp
// Перезапуск графики с другими параметрами: закрываем и создаём заново.
window.Destroy();
crossrender::WindowDesc vr;
vr.mode = crossrender::WindowMode::Fullscreen;
vr.vsync = false;
if (!window.Create(vr)) ENG_LOGE("app", "повторное создание окна не удалось");
```

### `void PollEvents()`

Обрабатывает очередь сообщений платформы **не блокируясь** и передаёт события в
`Input` (`OnKey`, `OnMouseMove`, `OnMouseButton`, `OnScroll`, `OnText`,
`OnTouch`) и в `WindowCallbacks`. Вызывайте её после `Input::BeginFrame()` и до
чтения состояния ввода. На Android метод прокачивает очередь команд и событий
ввода приложения, в Web — опрашивает браузерные очереди, на настольных системах
разбирает очередь оконных сообщений.

```cpp
// Типичное место PollEvents — сразу после сброса краев ввода.
window.GetInput().BeginFrame();
window.PollEvents();
if (window.GetInput().KeyPressed(crossrender::Key::F1)) ui.ToggleDebugOverlay();
```

### `void SwapBuffers()`

Показывает задний буфер: на настольных платформах выполняется обмен буферов
(`flushBuffer` / `SwapBuffers` / `glXSwapBuffers`), в Web — коммит кадра
WebGL, на мобильных — `eglSwapBuffers`. Вызывайте один раз в конце кадра; при
включённом vsync именно здесь поток ждёт вертикальную синхронизацию.

```cpp
// Рисуем кадр и показываем его: SwapBuffers — последний вызов кадра.
renderer.Clear(crossrender::Color::Black);
scene.Render(renderer);
window.SwapBuffers();
```

### `bool ShouldClose() const`

`true`, если поступил запрос на закрытие окна — пользователь нажал системную
кнопку закрытия или код вызвал `RequestClose()`. Сам флаг ничего не закрывает:
решение о выходе принимает цикл приложения.

```cpp
// Основной цикл: ShouldClose — единственный штатный способ выйти.
while (!window.ShouldClose()) {
    window.GetInput().BeginFrame();
    window.PollEvents();
    UpdateGame(dt, window.GetInput());
    window.GetInput().EndFrame();
    window.SwapBuffers();
}
```

### `void RequestClose()`

Просит закрыть окно: выставляет флаг `ShouldClose` и **ровно один раз** вызывает
`callbacks.onClose`. Повторные вызовы обработчик не дёргают. Именно этот метод
вызывают настольные бэкенды при нажатии кнопки закрытия, поэтому подписка на
`onClose` — правильное место для сохранения состояния.

```cpp
// Выход по Escape и корректное завершение через onClose.
window.callbacks.onClose = [&] { SaveGame(); };
if (window.GetInput().KeyPressed(crossrender::Key::Escape)) window.RequestClose();
ENG_ASSERT(window.ShouldClose());
```

### `void SetTitle(const std::string& title)`

Меняет заголовок окна и запоминает его для `Title()`. На Android и iOS
системного заголовка нет — значение просто сохраняется. В Web обновляется
`document.title`.

```cpp
// Показываем в заголовке номер уровня и текущий FPS.
window.SetTitle(crossrender::Fmt("Уровень %d — %.0f FPS", level, fps));
ENG_ASSERT(window.Title() == crossrender::Fmt("Уровень %d — %.0f FPS", level, fps));
```

### `void SetSize(int w, int h)`

Запрашивает новый размер **логического** клиентского окна. Значения `<= 0`
игнорируются (Linux). В полноэкранном режиме Linux только запоминает размер,
чтобы применить его после выхода из полноэкранного режима; на Android вызов
игнорируется с сообщением в лог (поверхностью владеет ОС); в Web меняется
CSS-размер canvas.

```cpp
// Переключаем окно между двумя удобными размерами.
if (window.Width() > 1000) window.SetSize(960, 540);
else window.SetSize(1600, 900);
ENG_LOGI("app", "логический размер: %dx%d", window.Width(), window.Height());
```

### `void SetMode(WindowMode mode)`

Переключает режим окна (оконный, полноэкранный, безрамочный) и запоминает его
для `IsFullscreen()`. На iOS окна всегда полноэкранные, поэтому меняется только
учётная запись; на Android вызывается платформенный переход в полноэкранный
режим; на Linux при отсутствии поддержки EWMH используется запасной вариант —
окно размером с монитор.

```cpp
// Тумблер «окно / полный экран» по F11.
if (input.KeyPressed(crossrender::Key::F11)) {
    window.SetMode(window.IsFullscreen() ? crossrender::WindowMode::Windowed : crossrender::WindowMode::Fullscreen);
    ENG_LOGI("app", "полный экран: %s", window.IsFullscreen() ? "да" : "нет");
}
```

### `void SetVSync(bool enabled)`

Включает или выключает вертикальную синхронизацию на лету: macOS — swap
interval контекста, Windows — `wglSwapIntervalEXT`, Linux — `glXSwapIntervalEXT`
или `glXSwapIntervalMESA`, Web — Emscripten, Android — `eglSwapInterval`, iOS —
частота `CADisplayLink`. Если платформа не поддерживает смену, вызов тихо
ничего не делает (Linux пишет предупреждение).

```cpp
// Замеряем «сырой» FPS: на время замера отключаем синхронизацию.
window.SetVSync(false);
const float rawFps = MeasureFps(120);
window.SetVSync(true);
ENG_LOGI("app", "FPS без vsync: %.1f", rawFps);
```

### `void Minimize()`

Сворачивает окно: macOS — `miniaturize`, Windows — `SW_MINIMIZE`, Linux —
`XIconifyWindow`. В Web и на Android/iOS это **no-op**: браузерная вкладка и
мобильное приложение сворачиваются средствами системы.

```cpp
// Сворачиваем игру по кнопке «в фон» в настройках.
if (ui.Button("Свернуть")) window.Minimize();
ENG_LOGI("app", "после Minimize: свёрнуто = %s", window.IsMinimized() ? "да" : "нет");
```

### `void Maximize()`

Разворачивает окно на весь экран средствами оконного менеджера: macOS — `zoom`,
Windows — `SW_MAXIMIZE`, Linux — сообщение `_NET_WM_STATE_MAXIMIZED_HORZ/VERT`.
В Web и на мобильных — no-op. Это не то же самое, что `SetMode(Fullscreen)`:
разворачивается обычное окно с рамкой.

```cpp
// Двойной клик по заголовку — штатное разворачивание окна.
window.Maximize();
ENG_LOGI("app", "окно развёрнуто, размер %dx%d", window.Width(), window.Height());
```

### `void Restore()`

Возвращает свёрнутое или развёрнутое окно в обычное состояние: macOS —
`deminiaturize`, Windows — `SW_RESTORE`, Linux — снятие `_NET_WM_STATE_HIDDEN`.
В Web и на мобильных — no-op.

```cpp
// Возвращаем окно, когда игра снова становится активной.
if (pausedByFocusLoss && window.IsMinimized()) window.Restore();
```

### `void Show()`

Показывает окно: macOS — `orderFront`, Windows — `SW_SHOW`, Linux — `XMapWindow`,
Web — `display: block` у canvas. На Android — no-op; на iOS снимается флаг
`hidden` у главного `UIWindow`.

```cpp
// Скрываем окно на время загрузки и показываем, когда всё готово.
window.Hide();
LoadLevel("level1");
window.Show();
window.Focus();
```

### `void Hide()`

Скрывает окно: macOS — `orderOut`, Windows — `SW_HIDE`, Linux — `XUnmapWindow`,
Web — `display: none`. На Android — no-op; на iOS выставляется `hidden = YES`.
Скрытое окно продолжает существовать, контекст не уничтожается.

```cpp
// Прячем окно в трей-подобном режиме, не разрушая контекст OpenGL.
window.Hide();
ENG_LOGI("app", "окно скрыто, минимизировано = %s", window.IsMinimized() ? "да" : "нет");
```

### `void Focus()`

Передаёт окну фокус ввода: macOS — `makeKeyAndOrderFront` + активация
приложения, Windows — `SetForegroundWindow` + `SetFocus`, Linux — `XSetInputFocus`
и поднятие окна, Web — фокус на canvas. На Android — no-op (фокусом владеет
Activity); на iOS вызывается `makeKeyAndVisible`.

```cpp
// Возвращаем фокус игре после закрытия внутриигрового меню.
ui.CloseAll();
if (window.IsFocused()) ENG_LOGD("app", "окно уже в фокусе");
else window.Focus();
```

### `void MakeCurrent()`

Делает контекст OpenGL окна текущим на **вызывающем** потоке: `wglMakeCurrent`,
`glXMakeCurrent`, `[NSOpenGLContext makeCurrentContext]`,
`emscripten_webgl_make_context_current`, `eglMakeCurrent`. Нужен после работы с
чужим контекстом и при многопоточном рендере; в однопоточном цикле достаточно
того, что `Create` уже сделал контекст текущим.

```cpp
// Переключаемся обратно на контекст окна после фоновой загрузки текстур.
workerThread.join();
window.MakeCurrent();
ENG_LOGI("app", "контекст окна снова текущий");
```

### `void WaitEventsTimeout(f32 seconds)`

Ждёт события ОС не дольше `seconds` секунд, а затем обрабатывает очередь
(вызывает `PollEvents`). Используется в редакторах и «ленивом» режиме, чтобы не
жечь процессор на 100%. Реализации различаются: macOS — `nextEventMatchingMask`
с датой, Windows — `MsgWaitForMultipleObjectsEx` с `MWMO_INPUTAVAILABLE`, Linux —
`XPending` + `select` на X-соединении, Android — ожидание с таймаутом, iOS —
`SleepMs`. В Web метод — **пустышка**: Emscripten не умеет спать без Asyncify, а
блокировать вкладку нельзя.

```cpp
// Редактор: спим до следующего события, но не дольше 100 мс.
while (!window.ShouldClose()) {
    window.GetInput().BeginFrame();
    window.PollEvents();
    DrawEditorFrame(window.GetInput());
    window.GetInput().EndFrame();
    window.SwapBuffers();
    if (editorIdle) window.WaitEventsTimeout(0.1f);   // не крутим цикл вхолостую
}
```

### `void SetCursorVisible(bool visible)`

Простая форма управления курсором: показать или скрыть его. Реализована на всех
настольных платформах и в Web (скрытие указателя); на Android и iOS только
запоминает значение, потому что системного курсора там нет.

```cpp
// Прячем курсор во время катсцены и возвращаем в меню.
void EnterCutscene(crossrender::Window& window) { window.SetCursorVisible(false); }
void LeaveCutscene(crossrender::Window& window) { window.SetCursorVisible(true); }
```

### `void SetCursorMode(int mode)`

Расширенное управление курсором. Значения: `0` — обычный, `1` — скрытый,
`2` — отключённый/относительный. В режиме `2` указатель захватывается и
перецентровывается, а движение читают через `MouseDelta()`. Честные реализации
всех трёх режимов есть на macOS (`CGAssociateMouseAndMouseCursorPosition`),
Windows (`SetCapture` + `ClipCursor`) и Linux (`XGrabPointer` + центрирование);
в Web доступно только скрытие — относительный захват требует асинхронного
Pointer Lock API; на Android и iOS значение просто запоминается. Любое значение
кроме `2` возвращает курсор в нормальное состояние.

```cpp
// Режим от первого лица: захватываем мышь и читаем только дельту.
window.SetCursorMode(2);
const crossrender::Vec2 look = window.GetInput().MouseDelta();
camera.yaw += look.x * sensitivity;
if (input.KeyPressed(crossrender::Key::Escape)) window.SetCursorMode(0);   // отпускаем курсор
```

### `void SetClipboardText(const std::string& text)`

Кладёт строку UTF-8 в системный буфер обмена. На macOS, Windows, Linux, Android
и iOS используется нативная реализация; Windows при занятом буфере пишет
предупреждение и тихо отказывается, Linux становится владельцем X-выборки.
В Web вызов опирается на жест пользователя и без него молча не срабатывает.

```cpp
// Копируем код приглашения в буфер обмена по кнопке.
if (ui.Button("Копировать код")) {
    window.SetClipboardText("ENGINE-1234-ABCD");
    ENG_LOGI("app", "код скопирован в буфер обмена");
}
```

### `std::string GetClipboardText() const`

Читает строку UTF-8 из буфера обмена. Возвращает пустую строку, если буфер пуст,
занят или чтение не поддерживается. Честные ограничения: в Web метод **всегда**
возвращает пустую строку и один раз пишет предупреждение (браузерный API только
асинхронный); на Linux выполняется round-trip через X-выбор с дедлайном 100 мс,
и при таймауте функция один раз предупреждает и возвращает пустую строку;
на Windows занятый буфер тоже даёт пустую строку с предупреждением.

* **Возвращает:** текст буфера обмена или пустую строку.

```cpp
// Вставляем ранее скопированный путь в поле ввода.
const std::string pasted = window.GetClipboardText();
if (!pasted.empty()) pathField.SetText(pasted);
else ENG_LOGD("app", "буфер обмена пуст или недоступен");
```

### `int Width() const`

Ширина **логической** клиентской области окна. На HiDPI-дисплее это не то же
самое, что `FramebufferWidth()`: справедливо `FramebufferWidth() == Width() *
DpiScale()` (на Android ширина вычисляется как `FramebufferWidth() / DpiScale()`,
на iOS берётся из `view.bounds`).

```cpp
// Центрируем прицел по логическому размеру окна.
const crossrender::Vec2 center{window.Width() * 0.5f, window.Height() * 0.5f};
ui.Image(crosshairTex, {center.x - 16.0f, center.y - 16.0f, 32.0f, 32.0f});
```

### `int Height() const`

Высота логической клиентской области окна. Вместе с `Width()` задаёт систему
координат интерфейса и позицию мыши (`Input::MousePos()`).

```cpp
// Прижимаем подсказку к нижнему краю логического окна.
const float hintY = window.Height() - hint.height - 16.0f;
ui.Label("WASD — движение", {16.0f, hintY, hint.width, hint.height});
```

### `int FramebufferWidth() const`

Ширина кадрового буфера **в пикселях** — то, что нужно `glViewport` и
`Renderer2D::BeginFrame`. На HiDPI-дисплее больше `Width()` в `DpiScale()` раз.
В Web значение читается прямо из размеров canvas, на Android — из
`ANativeWindow`, на macOS — через `convertRectToBacking`.

```cpp
// Настраиваем вьюпорт и 2D-рендерер по реальным пикселям.
gl::glViewport(0, 0, window.FramebufferWidth(), window.FramebufferHeight());
r2d.BeginFrame(window.FramebufferWidth(), window.FramebufferHeight(), window.DpiScale());
ENG_ASSERT(window.FramebufferWidth() == static_cast<int>(window.Width() * window.DpiScale() + 0.5f));
```

### `int FramebufferHeight() const`

Высота кадрового буфера в пикселях. Используется вместе с
`FramebufferWidth()` для вьюпорта, render target'ов и `Aspect()`.

```cpp
// Создаём render target ровно под текущий кадровый буфер.
crossrender::RenderTarget target = crossrender::RenderTarget::Create(window.FramebufferWidth(),
                                                     window.FramebufferHeight());
ENG_LOGI("app", "render target %dx%d", target.Width(), target.Height());
```

### `f32 DpiScale() const`

Масштаб между логическими координатами и пикселями кадрового буфера. Источники:
macOS — `backingScaleFactor`, Windows — системный DPI, Linux — `Xft.dpi`, Web —
`devicePixelRatio`, Android — плотность дисплея, iOS — `contentScaleFactor`.
Значение всегда `> 0`; для платформ без понятия DPI возвращается `1`.

* **Возвращает:** отношение пикселей кадрового буфера к логическим единицам.
* **Контекст:** обязателен для хит-тестов, если эталон измерен в пикселях.

```cpp
// Переводим попадание из пикселей кадрового буфера в логические координаты.
const crossrender::f32 dpi = window.DpiScale() > 0.0f ? window.DpiScale() : 1.0f;
const crossrender::Vec2 hit = {window.FramebufferWidth() * 0.5f / dpi, 40.0f / dpi};
ENG_ASSERT(crossrender::Length(hit - input.MousePos()) >= 0.0f);
```

### `f32 Aspect() const`

Отношение ширины кадрового буфера к высоте — готовое значение для
`Mat4::Perspective`. Если высота равна нулю (вырожденный случай), возвращается
`1`. Считается по **пикселям**, но для пропорций это то же самое, что по
логическим единицам, пока масштаб одинаков по осям.

```cpp
// Проекция камеры всегда соответствует текущим пропорциям окна.
const crossrender::Mat4 proj = crossrender::Mat4::Perspective(crossrender::Radians(60.0f), window.Aspect(), 0.1f, 500.0f);
ENG_LOGI("app", "соотношение сторон: %.3f", window.Aspect());
```

### `bool IsFocused() const`

`true`, если окно активно. Реализации: macOS — `isKeyWindow`, Windows —
`GetForegroundWindow() == hwnd`, Linux — внутренний флаг, полученный из
`FocusIn`/`FocusOut`, Web — флаг фокуса и видимость вкладки, Android — `hasFocus
&& resumed`, iOS — активное приложение и `isKeyWindow`.

```cpp
// При потере фокуса сбрасываем зажатые клавиши, чтобы игрок не «уехал».
if (!window.IsFocused() && wasFocused) player.Stop();
wasFocused = window.IsFocused();
```

### `bool IsMinimized() const`

`true`, если окно свёрнуто или скрыто. Реализации: macOS — `isMiniaturized`,
Windows — `IsIconic`, Linux — окно не отображено (`map_state == IsUnmapped`),
Web — `!visible`, Android — приложение не в состоянии `resumed`, iOS — всегда
`false` (окно приложения не сворачивается отдельно).

```cpp
// Пропускаем тяжёлую отрисовку, пока окно свёрнуто, но не выходим из цикла.
if (window.IsMinimized()) {
    crossrender::SleepMs(16);
    continue;
}
```

### `bool IsFullscreen() const`

`true` в полноэкранном и безрамочном режиме. На iOS всегда `true`: приложение
владеет всем экраном. На Android истинно при `mode != Windowed`, в Web
учитывается и настоящий Fullscreen API браузера.

```cpp
// В полный экран прячем часть интерфейса.
ui.SetVisible(!window.IsFullscreen() || showHud);
ENG_LOGI("app", "полный экран: %s", window.IsFullscreen() ? "да" : "нет");
```

### `Vec2 MousePosition() const`

**Живая** позиция курсора, которую запрашивает операционная система: macOS —
`mouseLocationOutsideOfEventStream`, переведённая в координаты view, Windows —
`GetCursorPos` + `ScreenToClient`, Linux — `XQueryPointer`. Координаты
логические, и если курсор находится **вне** клиентской области, они могут быть
отрицательными или больше `Width()`/`Height()` — код движка их **не
ограничивает**, так что обрезайте значение сами, если это важно. В Web, на
Android и iOS метод просто возвращает `GetInput().MousePos()`. Для интерфейса
обычно достаточно `Input::MousePos()` — это последняя позиция, доставленная
событием.

```cpp
// Автопрокрутка у края окна: обрезаем живую позицию курсора по границам.
crossrender::Vec2 p = window.MousePosition();
p.x = crossrender::Clamp(p.x, 0.0f, static_cast<crossrender::f32>(window.Width()));
p.y = crossrender::Clamp(p.y, 0.0f, static_cast<crossrender::f32>(window.Height()));
if (p.x <= 0.0f) scroll.x -= 400.0f * dt;
```

### `const std::string& Title() const`

Возвращает заголовок, который последним был задан через `WindowDesc::title` или
`SetTitle`. Это внутренняя копия строки, а не запрос к системе, поэтому вызов
дешёвый и работает даже на платформах без заголовка (Android, iOS).

* **Возвращает:** ссылку на внутреннюю строку; живёт, пока живо окно.

```cpp
// Проверяем, что заголовок действительно применился.
window.SetTitle("Финальная сцена");
ENG_ASSERT(window.Title() == "Финальная сцена");
ENG_LOGI("app", "заголовок окна: %s", window.Title().c_str());
```

### `Input& GetInput()`

Доступ к снимку ввода, которым владеет окно. Есть две перегрузки —
`Input& GetInput()` и `const Input& GetInput() const`; поведение одинаковое,
различается только константность возвращаемой ссылки. Ссылка действительна,
пока живо окно. Это основной способ получить состояние клавиш, мыши, касаний и
геймпада.

* **Возвращает:** ссылку на внутренний `Input`.
* **Контекст:** вызывайте `BeginFrame`/`EndFrame` и читайте состояние через эту
  же ссылку, чтобы не рассинхронизировать кадр.

```cpp
// Не константное окно — читаем и правим состояние ввода через одну ссылку.
crossrender::Input& input = window.GetInput();
input.BeginFrame();
window.PollEvents();
if (input.KeyPressed(crossrender::Key::R)) scene.Reload();

// Константное окно — только чтение (перегрузка для const Window).
const crossrender::Window& view = window;
const crossrender::Input& state = view.GetInput();
ENG_LOGI("app", "мышь в %.0f, %.0f", state.MousePos().x, state.MousePos().y);
```

### `WindowCallbacks callbacks`

Публичное поле с обработчиками событий окна. Назначайте их до `Create`; любое
не назначенное поле — пустая `std::function`, и вызов для него пропускается
(бэкенды проверяют `if (callbacks.onResize)`). Подробности по каждому
обработчику — в разделах `WindowCallbacks` выше.

```cpp
// Подписываемся на все нужные события до создания окна.
window.callbacks.onResize = [](int w, int h) { renderer.Resize(w, h); };
window.callbacks.onClose = [&] { running = false; };
window.callbacks.onFocus = [&](bool f) { paused = !f; };

crossrender::WindowDesc desc;
desc.width = 1280;
desc.height = 720;
if (!window.Create(desc)) return 1;
```

### `void* NativeHandle() const`

Непрозрачный указатель на нативное окно или его аналог. Конкретный тип зависит
от платформы: macOS — `NSWindow*`, Windows — `HWND`, Linux — X11 `Window`
(приведённый к указателю), Android — `ANativeWindow*`, iOS — `UIWindow*`,
Web — хендл WebGL-контекста (canvas как указатель представить нельзя). Нужен для
интеграции со сторонними библиотеками, плагинами и нативными диалогами.

* **Возвращает:** указатель платформенного типа или `nullptr`, если окна нет.

```cpp
// Отдаём HWND/NSWindow* сторонней библиотеке (например, для диалога файла).
void* native = window.NativeHandle();
if (native != nullptr) {
    ENG_LOGI("app", "нативное окно доступно (платформа %s)", crossrender::PlatformName().c_str());
    plugin.AttachWindow(native);
}
```

### `void* NativeDisplay() const`

Непрозрачный указатель на платформенный дисплей/подключение: Linux — `Display*`,
Windows — `HINSTANCE` модуля, Android — `EGLDisplay`, iOS — `UIView*`. На macOS и
в Web возвращается `nullptr`: там отдельного объекта дисплея нет.

* **Возвращает:** указатель платформенного типа или `nullptr`.

```cpp
// Дисплей нужен, чтобы создать общий ресурс (например, контекст шэринга) вручную.
void* display = window.NativeDisplay();
if (display != nullptr) ENG_LOGI("app", "платформенный дисплей получен");
else ENG_LOGD("app", "отдельного объекта дисплея на этой платформе нет");
```

### `void* (*GLGetProcAddress() const)(const char*)`

Возвращает **указатель на функцию** разрешения точек входа OpenGL для контекста
этого окна. Используйте его для загрузки расширений и функций сверх тех, что
линкуются напрямую: macOS — `dlsym` по OpenGL framework, Windows —
`wglGetProcAddress`, Linux — `glXGetProcAddressARB`, Web —
`emscripten_webgl_get_proc_address`, Android — `eglGetProcAddress`, iOS — `dlsym`.

* **Возвращает:** функцию `void* (*)(const char*)`, готовую к вызову, либо
  `nullptr` у неподдерживаемого запроса.

```cpp
// Загружаем функцию расширения через резолвер конкретного контекста.
void* (*resolve)(const char*) = window.GLGetProcAddress();
ENG_ASSERT(resolve != nullptr);
auto glGetErrorFn = reinterpret_cast<unsigned int (*)()>(resolve("glGetError"));
if (glGetErrorFn != nullptr) ENG_LOGI("gl", "glGetError загружена");
```

### `bool PlatformInit()`

Инициализирует платформенные службы до создания окна: логирует версию системы,
подключается к общим ресурсам (на Linux — к X-дисплею, который переиспользуют
остальные службы), настраивает окружение. Сегодня реализация **всегда возвращает
`true`** и не является обязательным условием создания окна: если X-сервер
недоступен, Linux только пишет предупреждение, а отказ проявится позже в
`Window::Create`.

```cpp
// Начинаем приложение с инициализации платформы.
if (!crossrender::PlatformInit()) {
    ENG_LOGE("app", "не удалось инициализировать платформу");
    return 1;
}
ENG_LOGI("app", "платформа: %s", crossrender::PlatformName().c_str());
```

### `void PlatformShutdown()`

Освобождает ресурсы, захваченные `PlatformInit`: на Linux закрывает общий
X-дисплей, на остальных платформах — no-op. Вызывайте один раз при выходе, после
`Window::Destroy`, чтобы не закрыть дисплей под ещё живым окном.

```cpp
// Симметричное завершение: сначала окно, затем платформа.
window.Destroy();
crossrender::PlatformShutdown();
ENG_LOGI("app", "платформа остановлена");
```

### `std::string PlatformName()`

Имя платформы строкой: `"macOS"`, `"Windows"`, `"Linux"`, `"Web"`, `"Android"`,
`"iOS"`. Никогда не пустое. Удобно для логов, выбора настроек и включения
платформенных ветвей кода во время выполнения.

* **Возвращает:** имя платформы.
* **Контекст:** та же функция объявлена в `crossrender/platform/Platform.h`.

```cpp
// Разные значения по умолчанию для настольных и мобильных платформ.
const std::string platform = crossrender::PlatformName();
if (platform == "Android" || platform == "iOS") settings.touchControls = true;
else settings.touchControls = false;
ENG_LOGI("app", "платформа %s, тач-управление: %s", platform.c_str(),
         settings.touchControls ? "вкл" : "выкл");
```

### `void* (*PlatformGLGetProcAddress())(const char*)`

Глобальный резолвер точек входа OpenGL, не привязанный к конкретному окну.
Возвращает функцию, которую можно передать в `gl::LoadFunctions`, — это
канонический способ загрузить таблицу функций OpenGL до или вместо
`Window::GLGetProcAddress()`.

* **Возвращает:** функцию `void* (*)(const char*)`.
* **Ограничение:** в текущем дереве реализация есть в бэкендах macOS, Web, iOS и
  Android; в файлах Windows и Linux определения нет — там используйте
  `Window::GLGetProcAddress()` или headless-резолвер из `crossrender/platform/Platform.h`.

```cpp
// Глобальный резолвер подходит для загрузки таблицы функций движка.
if (!gl::LoadFunctions(crossrender::PlatformGLGetProcAddress)) {
    ENG_LOGE("gl", "не удалось загрузить функции OpenGL");
    return 1;
}
ENG_LOGI("gl", "функции OpenGL загружены на платформе %s", crossrender::PlatformName().c_str());
```

## Пример целиком

```cpp
#include "crossrender/core/Log.h"
#include "crossrender/platform/Window.h"

#include <string>

// Приложение с одним окном: обработчики, ввод, корректный учёт DPI.
int main() {
    crossrender::LogSetLevel(crossrender::LogLevel::Info);

    if (!crossrender::PlatformInit()) return 1;
    ENG_LOGI("app", "платформа: %s", crossrender::PlatformName().c_str());

    crossrender::Window window;
    crossrender::WindowDesc desc;
    desc.title = "Пример окна";
    desc.width = 1280;
    desc.height = 720;
    desc.minWidth = 640;
    desc.minHeight = 360;
    desc.vsync = true;
    desc.msaaSamples = 4;

    bool running = true;
    window.callbacks.onClose = [&] {
        ENG_LOGI("app", "запрос на закрытие окна");
        running = false;
    };
    window.callbacks.onResize = [](int w, int h) {
        ENG_LOGI("app", "кадровый буфер стал %dx%d", w, h);
    };
    window.callbacks.onFocus = [](bool focused) {
        ENG_LOGI("app", focused ? "окно в фокусе" : "окно потеряло фокус");
    };
    window.callbacks.onDpiChanged = [](crossrender::f32 dpi) {
        ENG_LOGI("app", "новый DPI: %.2f", dpi);
    };

    if (!window.Create(desc)) {
        ENG_LOGW("app", "дисплей недоступен — выходим без графики");
        crossrender::PlatformShutdown();
        return 0;
    }

    crossrender::Input& input = window.GetInput();
    crossrender::Vec2 pointer{};
    const crossrender::f32 speed = 300.0f;

    while (running && !window.ShouldClose()) {
        const crossrender::f32 dt = 1.0f / 60.0f;

        input.BeginFrame();
        window.PollEvents();

        // Клавиатура: край — для одиночных действий, удержание — для движения.
        if (input.KeyPressed(crossrender::Key::Escape)) window.RequestClose();
        if (input.KeyPressed(crossrender::Key::F11)) {
            window.SetMode(window.IsFullscreen() ? crossrender::WindowMode::Windowed
                                                 : crossrender::WindowMode::Fullscreen);
        }
        if (input.KeyDown(crossrender::Key::W)) pointer.y -= speed * dt;
        if (input.KeyDown(crossrender::Key::S)) pointer.y += speed * dt;

        // Мышь: абсолютная позиция и накопленная за кадр дельта.
        if (input.MouseDown()) pointer = input.MousePos();
        if (input.MouseDown(crossrender::MouseButton::Right)) pointer += input.MouseDelta();

        // Текст читаем кодовыми точками — раскладка не важна.
        for (crossrender::u32 cp : input.TextInput()) ENG_LOGD("app", "введён код U+%04X", cp);

        // Хит-тест: эталон в пикселях кадрового буфера приводим к логическим координатам.
        const crossrender::f32 dpi = window.DpiScale() > 0.0f ? window.DpiScale() : 1.0f;
        const crossrender::Vec2 buttonPx{window.FramebufferWidth() * 0.5f, 40.0f};
        if (input.MousePressed() && crossrender::Length(input.MousePos() - buttonPx / dpi) < 32.0f) {
            ENG_LOGI("app", "нажата кнопка интерфейса");
        }

        input.EndFrame();
        window.SwapBuffers();
    }

    window.Destroy();
    crossrender::PlatformShutdown();
    return 0;
}
```

## См. также

* `docs/platform/Platform.md` — службы платформы, `AppHooks` с хуками контекста
  и headless-контекст OpenGL (`CreateHeadlessGLContext`).
* `docs/ui/Ui.md` — `UiContext::MousePos()`, который просто переадресует вызов в
  `Input::MousePos()` (см. ловушку координат в `## Обзор`).
* `docs/gfx/Renderer2D.md` — `Renderer2D::BeginFrame(fbWidth, fbHeight, dpiScale)`
  и логическое пространство раскладки интерфейса.
* `docs/core/Math.md` — `Vec2`, которым выражаются позиции мыши и касаний.
* `docs/core/Log.md` — макросы `ENG_LOGI` / `ENG_LOGW` / `ENG_LOGE`, которыми
  бэкенды сообщают об отказах и ограничениях платформы.
* `docs/test/Test.md` — как тесты пропускают оконные проверки без дисплея
  (`tests/test_platform.cpp`, `ENG_SKIP("no display available")`).
