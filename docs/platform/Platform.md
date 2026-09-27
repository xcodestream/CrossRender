# crossrender/platform/Platform.h — сервисы платформы, жизненный цикл приложения и headless-контекст

Небольшой платформенный API движка: системные диалоги и пути, метрики
машины и экрана, таймер и сон потока, мобильные хуки, цикл приложения
(`RunApp`) и offscreen-контекст OpenGL для тестов и CI.

## Заголовок

```cpp
#include "crossrender/platform/Platform.h"
```

## Обзор

`Platform.h` — это «всё, что зависит от операционной системы, но не является
окном». Заголовок намеренно маленький: здесь нет ни классов с состоянием, ни
ресурсов, которыми нужно владеть. Каждая функция реализована в своей папке
`engine/src/platform/<os>/` и почти всегда имеет осмысленную реализацию на
настольных платформах и заглушку там, где возможности нет.

Создание окна и контекста живёт отдельно — в `crossrender/platform/Window.h`
(`PlatformInit`, `PlatformShutdown`, `PlatformGetProcAddress`). Обратите
внимание, что `PlatformName()` объявлена **в обоих** заголовках с одинаковой
сигнатурой; определение одно, в платформенном файле.

#### Что реально работает, а что нет

| Функция | Windows | macOS | Linux | iOS | Android | Web |
|---|---|---|---|---|---|---|
| `ShowMessageBox` | модальный `MessageBoxW` | модальный `NSAlert` | лог + `zenity`, если есть | `UIAlertController`, **не блокирует** | только запись в logcat | `alert()` в браузере |
| `OpenFileDialog` | нативный диалог | `NSOpenPanel` | **заглушка**, пустая строка | **заглушка** | **заглушка** | **заглушка** |
| `SaveFileDialog` | нативный диалог | `NSSavePanel` | **заглушка** | **заглушка** | **заглушка** | **заглушка** |
| `OpenUrl` | `ShellExecuteW` | `NSWorkspace` | `fork` + `xdg-open` | `openURL:` | `Intent.ACTION_VIEW` | `window.open` |
| `SleepMs` | `Sleep` | `usleep` | `usleep` | `nanosleep` | `usleep` | `emscripten_sleep` (нужен `-sASYNCIFY`) |
| `SetSoftKeyboardVisible` | no-op | no-op | no-op | реальный вызов | реальный вызов | фокус на скрытом `input` |
| `SetKeepScreenAwake` | no-op | `IOPMAssertion` | no-op | реальный вызов | `FLAG_KEEP_SCREEN_ON` | Screen Wake Lock API |
| `Vibrate` | no-op | no-op | no-op | системная вибрация | реальная вибрация | `navigator.vibrate` |
| `RunApp` | блокирует | блокирует | блокирует | `UIApplicationMain`, не возвращается до выхода | цикл native activity | ставит `requestAnimationFrame` и сразу возвращает управление |

Отдельно про Web: `CpuCoreCount()` и `CurrentThreadId()` возвращают `1`, если
модуль собран без `-pthread`; `ExecutablePath()` всегда пуст (в песочнице
браузера нет понятия «путь к исполняемому файлу»), а `ExecutableDir()` равен
`"/"` — корню виртуальной файловой системы Emscripten, куда обычно
предзагружаются ассеты; `TotalPhysicalMemory()` возвращает размер wasm-кучи,
а не физическую память машины.

#### Headless-контекст OpenGL

`CreateHeadlessGLContext` / `DestroyHeadlessGLContext` дают контекст OpenGL
**без окна**. Это то, на чём работают `gameengine_tests`, CI и режим
`--headless`: тесты и скриншоты выполняются на машине без дисплея, а
`tests/test_main.cpp` держит один контекст на весь прогон.

| Платформа | Как сделано | Возможные отказы |
|---|---|---|
| macOS | CGL, offscreen, профиль 3.2 core | при отказе аппаратного формата пробуется программный рендеринг |
| Windows | скрытое окно 16x16 + контекст WGL 3.3 | почти всегда доступно |
| Linux | GLX pbuffer 16x16, иначе невидимое окно | нужен X-сервер: без `DISPLAY` вернётся `false` |
| Android | EGL pbuffer, ES 3 | драйвер может не дать pbuffer-конфигурацию |
| iOS | weak-заглушки, всегда `false` | контекст создаёт только реальное окно |
| Web | offscreen-canvas + WebGL 2 | в Node нет WebGL: entry points остаются нулевыми |

Последний случай стоит проговорить отдельно, потому что он обманывает
ожидания: на Web `CreateHeadlessGLContext()` **всегда возвращает `true`** —
даже когда WebGL 2 недоступен. При этом `HasHeadlessGLContext()` тоже
вернёт `true`, а `HeadlessGLGetProcAddress()` — нулевые указатели, так что
`gl::LoadFunctions()` честно провалится. Проверяйте именно загрузку функций,
а не только факт создания контекста.

На настольных платформах и Android контекст **ref-counted**: повторный
`CreateHeadlessGLContext()` увеличивает счётчик, а `DestroyHeadlessGLContext()`
уничтожает контекст только при обнулении. На Web счётчика нет.

#### Корни ресурсов (`assets` и `user`)

Сами корни объявлены не здесь, а в `crossrender/core/File.h`
(`FileSystemBind`, `SetAssetRoot`, `GetAssetRoot`, `SetUserRoot`,
`GetUserRoot`), но подбирает их платформенный слой, и поведение стоит знать:

* **Корень ассетов** ищется обходом вверх от каталога исполняемого файла —
  до шести уровней — в поисках папки `assets`; если поиск не удался, берётся
  `assets` из рабочего каталога. Так работают и `build/examples/sources`, и
  бандл приложения.
* **Пользовательский корень** — это каталог данных приложения
  (`%APPDATA%/CrossRender`, `~/Library/Application Support/CrossRender`,
  `~/.local/share/CrossRender`, `/tmp/CrossRender` на мобильных и Web). Он
  создаётся и проверяется **пробной записью**; если запись не удалась (типичный
  случай — песочница или CI), корень молча подменяется каталогом во временной
  папке с предупреждением `ENG_LOGW("fs", ...)`. Именно поэтому движок никогда
  не остаётся без записываемого места, но путь к сохранениям в песочнице может
  отличаться от ожидаемого.

```cpp
// Проверяем оба корня до первой записи: так сюрпризы видны сразу в логе.
ENG_LOGI("platform", "ассеты: %s", crossrender::GetAssetRoot().c_str());
ENG_LOGI("platform", "данные: %s", crossrender::GetUserRoot().c_str());
if (!crossrender::WriteTextFile("user/probe.txt", "1")) {
    ENG_LOGW("platform", "пользовательский корень недоступен для записи");
}
```

#### Порядок работы

1. На старте вызывается `PlatformInit()` (объявлена в `Window.h`): на Linux она
   заранее открывает общее соединение с X-сервером (и пишет `ENG_LOGW`, если
   `DISPLAY` недоступен), на остальных платформах просто возвращает `true`.
   Парная `PlatformShutdown()` закрывает это соединение.
2. Приложение либо вызывает `RunApp(hooks)` и отдаёт цикл платформе, либо
   строит цикл само через `crossrender::Engine` (см. `docs/scene/Engine.md`).
3. Диагностика и телеметрия берут `PlatformName`, `PlatformArch`, `CpuName`,
   `TotalPhysicalMemory`, `CpuCoreCount` и `HighResTimerSeconds` из этого
   заголовка.
4. Тесты и скриншоты вызывают `CreateHeadlessGLContext()` и работают без окна.

```cpp
// Типичная стартовая диагностика: одной строкой в лог.
ENG_LOGI("platform", "%s %s, CPU: %s, ядер %d, памяти %llu МБ",
         crossrender::PlatformName().c_str(), crossrender::PlatformArch().c_str(), crossrender::CpuName().c_str(),
         crossrender::CpuCoreCount(),
         static_cast<unsigned long long>(crossrender::TotalPhysicalMemory() / (1024ull * 1024ull)));
```

## Члены класса

### `enum class MessageBoxType`

Вид модального сообщения. Влияет только на оформление: на настольных
платформах выбирает иконку и стиль, на Linux — уровень `zenity`, на мобильных
и Web вид либо логируется, либо игнорируется.

| Значение | Смысл |
|---|---|
| `MessageBoxType::Info` | нейтральное сообщение, значение по умолчанию |
| `MessageBoxType::Warning` | предупреждение: что-то не так, но работа продолжается |
| `MessageBoxType::Error` | ошибка операции |
| `MessageBoxType::Question` | вопрос пользователю |

Таблица содержит все значения перечисления.

```cpp
crossrender::ShowMessageBox("Сохранение", "Слот перезаписан", crossrender::MessageBoxType::Info);
crossrender::ShowMessageBox("Диск", "Не удалось записать файл", crossrender::MessageBoxType::Error);
```

### `void ShowMessageBox(const std::string& title, const std::string& message, MessageBoxType type = MessageBoxType::Info)`

Показывает модальное окно с сообщением. На Windows и macOS вызов
**блокирует** поток до нажатия «OK»; на iOS `UIAlertController` асинхронен и
возвращается сразу; на Android и Linux сообщение дублируется в лог, а на Linux
дополнительно вызывается `zenity`, если он установлен.

* **Контекст:** на macOS/Windows диалог выполняется в главном потоке — вызов
  из рабочего потока перенаправляется туда и ждёт результата. Не вызывайте
  его из кадра: это остановит игру.
* **Ограничение:** на Linux без X-сервера и без `zenity` пользователь ничего
  не увидит, кроме записи в лог.

```cpp
if (!crossrender::FileExists("assets/levels/level02.json")) {
    crossrender::ShowMessageBox("Уровень недоступен",
                        "Файл assets/levels/level02.json не найден",
                        crossrender::MessageBoxType::Warning);
}
```

### `std::string OpenFileDialog(const std::string& title, const std::string& filter)`

Открывает нативный диалог выбора одного существующего файла. Фильтр — строка
вида `"*.png;*.jpg"` или `"png,jpg"`; на macOS она превращается в список
допустимых типов, `*` или `*.*` означают «любой файл».

* **Возвращает:** абсолютный путь к файлу или **пустую строку**, если
  пользователь отменил выбор либо диалог не поддержан.
* **Ограничение:** на Linux (нет зависимости от GTK), iOS, Android и Web это
  заглушка: пишется `ENG_LOGW("platform", ...)` и возвращается пустая строка.
  Всегда обрабатывайте отмену.

```cpp
const std::string path = crossrender::OpenFileDialog("Открыть уровень", "*.json");
if (path.empty()) {
    ENG_LOGI("ui", "выбор файла отменён или недоступен на этой платформе");
} else {
    ENG_LOGI("ui", "выбран %s", path.c_str());
}
```

### `std::string SaveFileDialog(const std::string& title, const std::string& defaultName)`

Открывает нативный диалог сохранения файла. `defaultName` подставляется в поле
имени; полный путь собирает сама платформа.

* **Возвращает:** путь или **пустую строку** при отмене/недоступности.
* **Ограничение:** как и у `OpenFileDialog`, на Linux/iOS/Android/Web — заглушка.
  Расширение файла диалог не добавляет: дописывайте его сами.

```cpp
const std::string out = crossrender::SaveFileDialog("Сохранить скриншот", "frame.png");
if (!out.empty()) {
    crossrender::WriteTextFile(out, crossrender::ReadTextFile("user/screenshots/frame0.raw"));
}
```

### `bool OpenUrl(const std::string& url)`

Открывает URL в браузере по умолчанию. На Linux делается `fork` + `xdg-open` и
родитель не ждёт браузер; на Web — `window.open(..., '_blank')`.

* **Возвращает:** `true`, если запрос удалось передать системе. Это **не**
  значит, что страница открылась: браузер может отклонить popup, а `xdg-open`
  может отсутствовать.
* **Ограничение:** пустой URL даёт `false`.

```cpp
if (!crossrender::OpenUrl("https://example.com/manual")) {
    ENG_LOGW("ui", "не удалось открыть браузер — покажите справку внутри игры");
}
```

### `void GetScreenSize(int* w, int* h)`

Возвращает размер основного экрана. На macOS и Web это логические единицы
(points / CSS-пиксели), на Windows — пиксели `SM_CXSCREEN`, на Linux — размер
X-экрана, а без X-сервера — значения по умолчанию 1920x1080.

* **Контекст:** любой указатель может быть `nullptr`, тогда значение просто не
  записывается.

```cpp
int screenW = 0, screenH = 0;
crossrender::GetScreenSize(&screenW, &screenH);
const bool wide = screenW >= 1920;
ENG_LOGI("platform", "экран %dx%d (широкий: %s)", screenW, screenH, wide ? "да" : "нет");
```

### `std::string PlatformName()`

Короткое имя платформы для логов и телеметрии: `"Windows"`, `"macOS"`,
`"Linux"`, `"iOS"`, `"Android"`, `"Web"`. Не зависит от локали и никогда не
пусто.

```cpp
ENG_LOGI("platform", "запуск на %s", crossrender::PlatformName().c_str());
```

### `std::string PlatformArch()`

Имя архитектуры: `"arm64"`, `"x86_64"`, `"armv7"`, `"x86"`, а на Web —
`"wasm32"` / `"wasm64"`. Если архитектура неизвестна, возвращается
`"unknown"`.

```cpp
// На Apple silicon и на x86-64 набор SIMD-путей разный.
ENG_LOGI("platform", "архитектура: %s", crossrender::PlatformArch().c_str());
```

### `std::string CpuName()`

Человекочитаемое имя процессора. На macOS это `machdep.cpu.brand_string`, на
Apple silicon — идентификатор модели (`hw.model`), на iOS — идентификатор
устройства, на Android — поле `Hardware`/`model name` из `/proc/cpuinfo`, на
Web — строка `"WebAssembly"` (браузер не раскрывает реальный CPU).

```cpp
ENG_LOGI("platform", "CPU: %s", crossrender::CpuName().c_str());
```

### `u64 TotalPhysicalMemory()`

Объём оперативной памяти в байтах. На Web возвращает размер wasm-кучи
(`emscripten_get_heap_size()`), то есть память, доступную движку, а не
физическую память машины.

* **Возвращает:** `0`, если значение получить не удалось; проверяйте результат.

```cpp
const crossrender::u64 bytes = crossrender::TotalPhysicalMemory();
if (bytes > 0) {
    ENG_LOGI("platform", "памяти: %llu МБ", static_cast<unsigned long long>(bytes >> 20));
} else {
    ENG_LOGW("platform", "объём памяти неизвестен — берём консервативные настройки");
}
```

### `int CpuCoreCount()`

Число логических ядер. Никогда не меньше единицы. На Web без `-pthread`
сборка однопоточная, поэтому функция честно возвращает `1`, даже если у
машины 16 ядер.

```cpp
const int cores = crossrender::CpuCoreCount();
const int workers = cores > 2 ? cores - 1 : 1;
ENG_LOGI("platform", "потоков для задач: %d", workers);
```

### `std::string ExecutablePath()`

Полный путь к исполняемому файлу. На macOS/iOS путь дополнительно
разрешается через `realpath`.

* **Возвращает:** пустую строку там, где понятия нет: на Android приложение —
  это загруженная зиготой разделяемая библиотека, а на Web путь скрыт
  песочницей браузера.

```cpp
const std::string exe = crossrender::ExecutablePath();
if (exe.empty()) {
    ENG_LOGI("platform", "путь к исполняемому файлу недоступен на этой платформе");
} else {
    ENG_LOGI("platform", "запущено из %s", exe.c_str());
}
```

### `std::string ExecutableDir()`

Каталог исполняемого файла — то, от чего `GetAssetRoot()` начинает обход вверх
в поисках папки `assets`.

* **Возвращает:** `"/"` на Web, `internalDataPath` приложения на Android,
  `"/"` как последний запасной вариант для Android.

```cpp
// Рядом с бинарником принято держать конфиг разработчика.
const std::string localConfig = crossrender::PathJoin(crossrender::ExecutableDir(), "dev.json");
if (crossrender::FileExists(localConfig)) ENG_LOGI("platform", "читаю %s", localConfig.c_str());
```

### `void SleepMs(u32 milliseconds)`

Усыпляет **вызывающий поток** примерно на указанное число миллисекунд. Ноль
миллисекунд — немедленный возврат.

* **Контекст:** используется в циклах ожидания и в тестах; во время сна
  сообщения окна не обрабатываются — для этого есть
  `Window::WaitEventsTimeout`.
* **Ограничение:** на Web реализация — `emscripten_sleep`, и она работает
  только при сборке с `-sASYNCIFY`; без него вызов ничего не делает.

```cpp
// Простейший троттлинг цикла без vsync.
crossrender::SleepMs(16);   // ~60 кадров в секунду
```

### `u64 CurrentThreadId()`

Идентификатор текущего потока — для диагностики и отладочных сообщений
(например, «какой поток держит ресурс»). На Web без `-pthread` всегда `1`,
на Android — `gettid()`.

```cpp
ENG_LOGI("platform", "кадр строится в потоке %llu",
         static_cast<unsigned long long>(crossrender::CurrentThreadId()));
```

### `f64 HighResTimerSeconds()`

Монотонные секунды с высоким разрешением. Определение лежит в
`engine/src/core/Time.cpp` и совпадает с `crossrender::NowSeconds()`; здесь функция
объявлена как платформенный сервис.

* **Контекст:** только для измерения интервалов. Абсолютное значение не имеет
  смысла и не обязано быть временем Unix.

```cpp
const crossrender::f64 t0 = crossrender::HighResTimerSeconds();
crossrender::SleepMs(12);
const crossrender::f64 elapsed = crossrender::HighResTimerSeconds() - t0;
ENG_LOGI("platform", "сон занял %.1f мс", elapsed * 1000.0);
```

### `void SetSoftKeyboardVisible(bool visible)`

Показывает или скрывает экранную клавиатуру на мобильных платформах.

* **Ограничение:** на Windows, macOS и Linux это осознанный no-op (с
  отладочной записью в лог на macOS); на Web создаётся невидимый `<input>` и
  фокус переводится на него — браузер открывает клавиатуру только для полей
  ввода.

```cpp
void BeginRename(crossrender::Window& window) {
    editing_ = true;
    crossrender::SetSoftKeyboardVisible(true);   // на телефоне откроется клавиатура
    window.GetInput().BeginFrame();
}
```

### `void SetKeepScreenAwake(bool enabled)`

Просит платформу не гасить экран: macOS создаёт `IOPMAssertion`, Android
выставляет `FLAG_KEEP_SCREEN_ON`, iOS использует `idleTimerDisabled`, Web
запрашивает Screen Wake Lock.

* **Ограничение:** защита обычно живёт до конца процесса либо до отключения;
  на Windows и Linux вызов ничего не делает. На Web запрос асинхронный и
  требует защищённого контекста, а ошибки проглатываются.

```cpp
// Длинная кат-сцена без ввода: экран не должен погаснуть.
crossrender::SetKeepScreenAwake(true);
PlayCinematic();
crossrender::SetKeepScreenAwake(false);
```

### `void Vibrate(u32 milliseconds)`

Включает вибрацию. На iOS доступна только одна фиксированная длительность,
поэтому параметр игнорируется; на Android и Web длительность учитывается.

* **Ограничение:** на настольных платформах это no-op — проверять результат
  нечем, поэтому не строите на вибрации игровую механику.

```cpp
// Тактильный отклик на попадание; на ПК игрок просто ничего не почувствует.
crossrender::Vibrate(40);
```

### `bool IsAppForeground()`

Сообщает, находится ли приложение на переднем плане. На Linux без оконной
системы всегда `true` (фокус некому перехватить), на Web смотрит на
`document.hidden`.

```cpp
// Ставим игру на паузу, когда игрок ушёл в другое окно.
if (!crossrender::IsAppForeground()) {
    ENG_LOGI("game", "приложение ушло в фон — пауза");
    PauseGame();
}
```

### `struct AppHooks`

Набор колбэков, которыми платформа управляет приложением. Это агрегат из
обычных указателей на функции (не `std::function`), поэтому лямбды должны быть
без захвата; произвольное состояние передаётся через поле `user`.

Не все колбэки вызываются на всех платформах: `onContextLost` и
`onContextRestored` осмысленны на Android и Web, где контекст может быть
потерян; на настольных платформах они не вызываются. `onLowMemory` приходит
от мобильной ОС.

```cpp
struct GameState {
    int frames = 0;
};

crossrender::AppHooks hooks;
hooks.user = nullptr;   // заполним ниже
```

### `void (*onInit)(void* user) = nullptr`

Вызывается один раз, когда окно и контекст OpenGL уже созданы. Здесь грузят
ресурсы, настраивают сцены и звук.

```cpp
hooks.onInit = [](void* user) {
    auto* state = static_cast<GameState*>(user);
    ENG_LOGI("game", "инициализация, кадров пока %d", state->frames);
};
```

### `bool (*onFrame)(void* user, f32 dt) = nullptr`

Вызывается один раз за кадр; `dt` — секунды с прошлого кадра (кламп до 0.25 с
на настольных платформах). Возврат `false` — просьба завершить приложение.

```cpp
hooks.onFrame = [](void* user, crossrender::f32 dt) {
    auto* state = static_cast<GameState*>(user);
    ++state->frames;
    // Три кадра — и выходим: этого достаточно для скриншота в CI.
    return state->frames < 3;
};
```

### `void (*onContextLost)(void* user) = nullptr`

Вызывается, когда контекст OpenGL потерян (свернули приложение, уснуло
устройство, браузер выгрузил контекст). Все GPU-ресурсы в этот момент
считаются недействительными.

```cpp
hooks.onContextLost = [](void* user) {
    // Освобождаем всё, что держало GPU: текстуры, буферы, шейдеры.
    ENG_LOGW("game", "контекст потерян — освобождаю GPU-ресурсы");
};
```

### `void (*onContextRestored)(void* user) = nullptr`

Вызывается после восстановления контекста. Здесь ресурсы создаются заново:
проще всего заново вызвать код инициализации рендера.

```cpp
hooks.onContextRestored = [](void* user) {
    ENG_LOGI("game", "контекст восстановлен — пересоздаю ресурсы");
};
```

### `void (*onShutdown)(void* user) = nullptr`

Вызывается один раз перед завершением, после выхода из цикла кадров. Здесь
сохраняют прогресс и закрывают файлы.

```cpp
hooks.onShutdown = [](void* user) {
    auto* state = static_cast<GameState*>(user);
    ENG_LOGI("game", "выход: отрисовано %d кадров", state->frames);
};
```

### `void (*onLowMemory)(void* user) = nullptr`

Вызывается ОС при нехватке памяти. Обработчик должен немедленно освободить
кэши (атласы, звуки, модели), но не трогать данные, нужные текущей сцене.

```cpp
hooks.onLowMemory = [](void* user) {
    ENG_LOGW("game", "мало памяти — чищу кэш текстур");
};
```

### `void* user = nullptr`

Произвольный контекст, который передаётся первым аргументом во все колбэки.
Обычно указывает на состояние приложения; владение остаётся за вызывающим
кодом — платформа его не освобождает.

```cpp
GameState state;
hooks.user = &state;
// Теперь во всех колбэках user == &state.
```

### `int RunApp(const AppHooks& hooks)`

Запускает главный цикл приложения по колбэкам. На Windows, macOS и Linux
функция **блокирует** поток до закрытия окна и сама создаёт окно с настройками
по умолчанию (1280x720, оконный режим, vsync). На iOS вызывается
`UIApplicationMain`, на Android цикл крутит native activity, а на Web
регистрируется `requestAnimationFrame`-цикл и `RunApp` сразу возвращает
управление в цикл событий браузера.

* **Возвращает:** `0` при штатном завершении, `1`, если не удалось создать
  окно (на настольных платформах).
* **Контекст:** не смешивайте с `crossrender::Engine::Run()` — это два разных способа
  владеть циклом кадров.

```cpp
GameState state;
crossrender::AppHooks hooks;
hooks.onInit = [](void* user) { ENG_LOGI("game", "старт"); };
hooks.onFrame = [](void* user, crossrender::f32 dt) {
    (void)dt;
    return true;   // продолжаем, пока окно не закрыто
};
hooks.onShutdown = [](void* user) { ENG_LOGI("game", "финиш"); };
hooks.user = &state;

const int rc = crossrender::RunApp(hooks);
ENG_LOGI("game", "RunApp вернул %d", rc);
```

### `bool CreateHeadlessGLContext()`

Создаёт offscreen-контекст OpenGL без окна и делает его текущим. Нужен тестам,
CI и режиму `--headless`: рендер и скриншоты работают на машине без дисплея.
Повторные вызовы на настольных платформах и Android увеличивают счётчик
ссылок; на iOS всегда `false`, на Web всегда `true` (см. обзор).

* **Возвращает:** `true`, если контекст создан или уже существовал.
* **Контекст:** после успеха загрузите функции через
  `gl::LoadFunctions(HeadlessGLGetProcAddress)`; одним `HasHeadlessGLContext()`
  ограничиваться нельзя.

```cpp
if (!crossrender::CreateHeadlessGLContext()) {
    ENG_LOGW("gl", "headless-контекст недоступен — рендер-тесты будут пропущены");
} else if (!crossrender::gl::LoadFunctions(crossrender::HeadlessGLGetProcAddress)) {
    ENG_LOGE("gl", "контекст создан, но точки входа не загрузились");
}
```

### `void DestroyHeadlessGLContext()`

Освобождает offscreen-контекст. На платформах со счётчиком ссылок контекст
уничтожается только при обнулении счётчика, поэтому парные вызовы обязательны:
`tests/test_main.cpp` держит один контекст на весь прогон и снимает счётчик
после каждого теста.

```cpp
crossrender::DestroyHeadlessGLContext();
// В тесте здесь уместна проверка: HasHeadlessGLContext() должен вернуть false.
ENG_LOGI("gl", "headless-контекст освобождён");
```

### `bool HasHeadlessGLContext()`

Проверяет, есть ли сейчас живой offscreen-контекст. На Web отвечает `true`
даже без WebGL 2, поэтому для проверки работоспособности используйте загрузку
функций, а не этот предикат.

```cpp
if (crossrender::HasHeadlessGLContext()) {
    ENG_LOGI("gl", "работаем без окна");
} else {
    ENG_LOGI("gl", "нужно окно или другой контекст");
}
```

### `void* HeadlessGLGetProcAddress(const char* name)`

Резолвер точек входа OpenGL для headless-контекста. Возвращает адрес функции
по имени или `nullptr`; именно его передают в `gl::LoadFunctions`.

```cpp
if (crossrender::CreateHeadlessGLContext()) {
    const bool loaded = crossrender::gl::LoadFunctions(crossrender::HeadlessGLGetProcAddress);
    ENG_LOGI("gl", "функции OpenGL загружены: %s", loaded ? "да" : "нет");
}
```

## Пример целиком

```cpp
#include "crossrender/core/File.h"
#include "crossrender/core/Log.h"
#include "crossrender/gfx/GL.h"
#include "crossrender/platform/Platform.h"

// Мини-приложение для CI: рисует три кадра в offscreen-контексте и выходит.
// Окно не создаётся вовсе, поэтому пример работает на машине без дисплея.
struct HeadlessRun {
    int frames = 0;
    crossrender::f64 started = 0;
};

static void OnInit(void* user) {
    auto* run = static_cast<HeadlessRun*>(user);
    run->started = crossrender::HighResTimerSeconds();

    ENG_LOGI("run", "платформа %s (%s), ядер %d", crossrender::PlatformName().c_str(),
             crossrender::PlatformArch().c_str(), crossrender::CpuCoreCount());

    if (!crossrender::CreateHeadlessGLContext()) {
        ENG_LOGW("run", "headless-контекст недоступен: скриншоты пропущены");
        return;
    }
    if (!crossrender::gl::LoadFunctions(crossrender::HeadlessGLGetProcAddress)) {
        ENG_LOGE("run", "функции OpenGL не загрузились");
    }
}

static bool OnFrame(void* user, crossrender::f32 dt) {
    auto* run = static_cast<HeadlessRun*>(user);
    (void)dt;

    // Здесь был бы рендер кадра в framebuffer контекста.
    ++run->frames;
    crossrender::SleepMs(1);                  // имитируем нагрузку кадра
    return run->frames < 3;           // три кадра — и просим выход
}

static void OnShutdown(void* user) {
    auto* run = static_cast<HeadlessRun*>(user);
    const crossrender::f64 total = crossrender::HighResTimerSeconds() - run->started;
    ENG_LOGI("run", "готово: %d кадров за %.1f мс", run->frames, total * 1000.0);
    crossrender::DestroyHeadlessGLContext();
}

void RunHeadless() {
    HeadlessRun run;

    crossrender::AppHooks hooks;
    hooks.onInit = &OnInit;
    hooks.onFrame = &OnFrame;
    hooks.onShutdown = &OnShutdown;
    hooks.user = &run;

    // В обычном режиме цикл отдаётся платформе одной строкой (она блокирует
    // поток до закрытия окна). В headless-режиме окна нет, поэтому цикл крутит
    // сам движок — см. docs/scene/Engine.md.
    // crossrender::RunApp(hooks);
    (void)hooks;

    OnInit(&run);
    while (OnFrame(&run, 1.0f / 60.0f)) {
    }
    OnShutdown(&run);

    // Пользовательский корень может оказаться временным каталогом в песочнице —
    // это нормально, поэтому просто сообщаем, куда пишем.
    ENG_LOGI("run", "сохранения: %s", crossrender::GetUserRoot().c_str());
}
```

## См. также

* `docs/platform/Window.md` — окно, ввод, курсор и `PlatformInit` /
  `PlatformShutdown` / `PlatformGLGetProcAddress`.
* `docs/core/File.md` — корни `assets` и `user`, `FetchUrl`-заглушка и пути.
* `docs/core/Time.md` — `NowSeconds` и время кадра, на котором стоит
  `HighResTimerSeconds`.
* `docs/core/Log.md` — макросы, которыми платформенный слой сообщает об
  отказах (`ENG_LOGW` / `ENG_LOGE`).
* `docs/test/Test.md` — как тесты используют headless-контекст.
