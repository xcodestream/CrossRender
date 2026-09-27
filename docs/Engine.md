# crossrender/Engine.h — фасад движка и главный цикл

`Engine` — единственная точка входа в движок: он владеет окном, контекстом
OpenGL, 2D- и 3D-рендерерами, интерфейсом, менеджером сцен, звуком, кэшем
ресурсов и всем главным циклом. Заголовок объявляет конфигурацию
`EngineConfig`, снимок статистики `EngineStats`, сам класс `Engine` и
вспомогательную функцию `RunExample`.

## Заголовок

```cpp
#include "crossrender/Engine.h"
```

## Обзор

Типичное приложение создаёт один `Engine`, заполняет `EngineConfig` и
вызывает `Init()`, затем регистрирует сцены и запускает `Run()` — блокирующий
цикл «шаг за шагом». Платформы, где цикл принадлежит системе (Android, WASM),
вызывают `Step()` по одному кадру сами, а приложение читает состояние через
`ShouldQuit()`.

Движок ничего не знает об игре: вся логика живёт в сценах (`docs/scene/Scene.md`),
а `Engine` лишь владеет подсистемами и вызывает их в строгом порядке. Поэтому
порядок кадра — самое важное, что нужно понимать при чтении этого заголовка.

### Порядок кадра целиком

Внутри `Engine::Step` подсистемы вызываются ровно так (реализация —
`engine/src/scene/Engine.cpp`):

1. **Проверка готовности.** Если `Init()` не завершился успешно, `Step()`
   молча выходит.
2. **Ввод.** Выбирается источник (`window_->GetInput()` на настольной
   платформе, `headlessInput_` в headless-режиме), вызывается
   `Input::BeginFrame()` — сбрасываются флаги «нажато/отпущено в этом кадре»,
   затем `Window::PollEvents()`; запрос закрытия окна превращается в `Quit()`.
3. **Вьюпорт.** `UpdateViewport()` заново считает логический прямоугольник из
   размера фреймбуфера и `DpiScale()`, применяет безопасную зону; в
   retro-режиме вьюпорт становится виртуальным разрешением, а `ctx.dpiScale`
   — единицей.
4. **Часы.** `Clock::Tick()`; `dt` берётся из аргумента `Step`, если он
   неотрицателен, иначе из `Clock::Delta()`, и ограничивается сверху
   `EngineConfig::maxDeltaTime`. На паузе масштабированное время равно нулю.
5. **Обновление сцены.** Если приложение не на паузе, `SceneManager::Update`
   продвигает переход и вызывает `Scene::Update`; затем всегда вызывается хук
   `onUpdate`.
6. **Цель кадра.** Считаются размеры фреймбуфера, вызывается
   `RenderTarget::SetDefaultViewport`, выбирается цель: в headless-режиме
   `EnsureHeadlessTarget` создаёт (или пересоздаёт) offscreen-таргет под
   размер кадра, в retro-режиме берётся виртуальный буфер, при включённых
   фильтрах — композитный таргет, иначе экран по умолчанию. Цель
   привязывается и очищается цветом `Scene::ClearColor()`.
7. **Подготовка 3D.** Если сцена вернула `Wants3D()`, вызывается
   `Scene::Prepare3D` — камера, свет и окружение на этот кадр.
8. **3D-проход.** `Renderer3D::BeginFrame` → `Scene::Render3D` →
   `Renderer3D::EndFrame`. Результат уходит прямо в цель кадра либо в
   HDR-таргет постобработки.
9. **Постобработка.** Если `enablePostProcessing` включён, сцена просит
   `WantsPostProcessing()`, а `PostProcessor::Valid()` истинно, то
   `PostProcessor::Apply` разрешает HDR-цепочку прямо в цель кадра.
10. **Начало 2D.** `Renderer2D::BeginFrame(frameW, frameH, dpi)`; при
    необходимости очищается экран по умолчанию.
11. **Начало UI.** `UiContext::BeginFrame` получает ввод кадра и логический
    вьюпорт.
12. **2D-слой сцены.** `SceneManager::Render2D()` — вызывается всегда, в том
    числе для 3D-сцены.
13. **Оверлеи приложения.** Хук `onOverlay`.
14. **Переход и оверлеи UI.** `SceneManager::RenderTransition`, затем
    `UiContext::EndFrame()`, `UiContext::RenderOverlays()` и — если включён —
    отладочный оверлей (F1).
15. **Конец 2D.** `Renderer2D::EndFrame()` отправляет накопленную геометрию в
    GPU.
16. **Фильтры и retro.** При `enableFilters` работает `FilterChain::Apply`;
    в retro-режиме `RetroDisplay::EndFrame` разрешает виртуальный буфер в
    цель кадра.
17. **Статистика.** Заполняется `EngineStats` текущего кадра.
18. **Звук.** `Audio::Update(scaled)`.
19. **Конец кадра.** `Input::EndFrame()` (сбрасывает дельты мыши) и
    `Window::SwapBuffers()`.

```cpp
// Скелет одного кадра: те же вызовы и тот же порядок, что внутри Engine::Step.
void StepSkeleton(crossrender::Engine& engine, crossrender::f32 dtOverride) {
    // 1. без успешного Init() Step() не делает ничего

    // 2-3. ввод и пересчёт вьюпорта (Retro/DpiScale/безопасная зона)
    crossrender::Input& input = engine.GetInput();
    input.BeginFrame();
    if (!engine.Headless()) engine.GetWindow().PollEvents();

    // 4. часы и дельта кадра
    engine.GetClock().Tick();
    crossrender::f32 dt = dtOverride >= 0.0f ? dtOverride : engine.GetClock().Delta();
    dt = crossrender::MinT(dt, engine.Config().maxDeltaTime);
    const crossrender::f32 scaled = engine.Paused() ? 0.0f : dt;

    // 5. переход + Scene::Update, затем глобальный хук
    if (!engine.Paused()) engine.Scenes().Update(scaled);
    if (engine.onUpdate) engine.onUpdate(engine, scaled);

    // 6. выбор и очистка цели кадра (headless / retro / фильтры / экран)
    const crossrender::Rect viewport = engine.Viewport();
    // В retro-режиме здесь были бы виртуальные размеры и frameDpi = 1.
    const int fbW = static_cast<int>(viewport.w * engine.DpiScale());
    const int fbH = static_cast<int>(viewport.h * engine.DpiScale());

    // 7-9. камера до 3D-прохода, сам проход и постобработка
    engine.Scenes().Prepare3D();
    engine.Scenes().Render3D();
    // PostProcessor::Apply(...) — только если сцена вернула WantsPostProcessing()

    // 10-12. кадр 2D открыт, кадр UI открыт, рисуется слой сцены
    engine.R2D().BeginFrame(fbW, fbH, engine.DpiScale());
    engine.UI().BeginFrame(engine.R2D(), input, viewport, scaled);
    engine.Scenes().Render2D();

    // 13-15. оверлеи, переход, закрытие UI и отправка 2D-геометрии
    if (engine.onOverlay) engine.onOverlay(engine);
    engine.Scenes().RenderTransition(engine.R2D(), viewport);
    engine.UI().EndFrame();
    engine.UI().RenderOverlays();
    engine.R2D().EndFrame();

    // 16-18. фильтры/retro, статистика, микшер
    crossrender::Audio::Get().Update(scaled);

    // 19. конец кадра
    input.EndFrame();
    if (!engine.Headless()) engine.GetWindow().SwapBuffers();
}
```

### Что делает Init

`Engine::Init` — единственное место, где создаются подсистемы; порядок внутри
тоже важен:

1. Запоминается конфигурация, включается уровень логирования `Debug`.
2. Задаётся корень ассетов (`SetAssetRoot`), затем инициализируется платформа.
3. Создаётся окно и контекст OpenGL **или**, при `headless = true`, offscreen
   контекст без окна.
4. Загружаются точки входа OpenGL и печатается информация о GPU.
5. Инициализируются `Renderer2D`, при `enable3D` — `Renderer3D`, при
   `enableUI` — `UiContext` с тёмной темой.
6. Создаётся `ResourceCache` с корнем ассетов, подхватываются шрифты по
   умолчанию.
7. При `enableAudio` инициализируется звук; при `enablePostProcessing`
   создаётся (но не инициализируется) `PostProcessor`.
8. Создаются `RetroDisplay` и, при `enableFilters`, `FilterChain`.
9. Считается вьюпорт, заполняется `SceneContext` и связывается с менеджером
   сцен.

Повторный `Init()` после успешного первого **ничего не делает** и возвращает
`true`, не применяя новую конфигурацию.

```cpp
crossrender::EngineConfig cfg;
cfg.window.title = "Моя игра";
cfg.window.width = 1600;
cfg.window.height = 900;
cfg.window.msaaSamples = 4;
cfg.enable3D = true;
cfg.enableAudio = true;
cfg.enableUI = true;
cfg.startScene = "menu";

crossrender::Engine engine;
if (!engine.Init(cfg)) {
    ENG_LOGE("demo", "движок не запустился");
    return 1;
}
// Второй Init(cfg2) уже ничего не поменяет: движок считает себя готовым.
```

### Headless-режим и подставное окно

При `headless = true` окно не создаётся: кадр рисуется в offscreen-таргет
`Engine::EnsureHeadlessTarget`, а пиксели можно прочитать через `ReadPixel` и
`Screenshot`. При этом:

* `GetWindow()` возвращает **подставной** объект `Window`, который никогда не
  создавался: его размеры нулевые, ввод пуст. Это сделано, чтобы код, который
  спрашивает размеры, не падал на `nullptr`, но полагаться на значения нельзя.
* Ввод нужно читать через `Engine::GetInput()`: он вернёт внутренний буфер
  кадра, а не `window.GetInput()`.
* Размер кадра берётся из `EngineConfig::headlessTarget`; если оставить его
  нулевым, `Step` посчитает фреймбуфер как `1x1`, хотя `Viewport()` покажет
  запасные `1280x720`.

```cpp
crossrender::EngineConfig cfg;
cfg.headless = true;
cfg.headlessTarget.width = 1280;    // обязательно задайте размер
cfg.headlessTarget.height = 720;
cfg.headlessTarget.depth = true;

crossrender::Engine engine;
engine.Init(cfg);

// Правильно: ввод кадра, а не окно.
const crossrender::Input& in = engine.GetInput();
if (in.KeyPressed(crossrender::Key::Escape)) engine.Quit();

for (int i = 0; i < 120; ++i) engine.Step(1.0f / 60.0f);   // прогреваем кадры
const crossrender::Color px = engine.ReadPixel(640, 360);
ENG_LOGI("demo", "пиксель в центре: r=%.2f g=%.2f b=%.2f", px.r, px.g, px.b);
```

### Retro-режим

Retro-режим (пиксельный или ASCII) рисует **весь** кадр, включая интерфейс, в
буфер виртуального разрешения и лишь затем растягивает его на экран. Отсюда
следствие, о которое легко споткнуться: `Viewport()` в этом режиме равен
виртуальному разрешению, а не размеру окна, поэтому и UI раскладывается по
виртуальным пикселям. `SceneContext::dpiScale` при этом равен `1`, но
`Engine::DpiScale()` продолжает возвращать DPI окна — это разные величины.

```cpp
// Включаем пиксельный режим 320x180 с палитрой NES прямо в конфигурации.
crossrender::EngineConfig cfg;
cfg.retro.mode = crossrender::RetroMode::Pixel;
cfg.retro.virtualWidth = 320;
cfg.retro.virtualHeight = 180;
cfg.retro.palette = crossrender::RetroPalette::Nes;
cfg.retro.scanlines = true;

// Или на ходу: RetroCfg() — это ссылка на ту же настройку.
engine.RetroCfg().integerScale = true;
if (engine.RetroActive() && engine.Retro().Valid()) {
    ENG_LOGI("demo", "виртуальный экран %dx%d", engine.Retro().VirtualWidth(),
             engine.Retro().VirtualHeight());
}
```

### Фильтры и постобработка

Это два разных механизма, и включаются они независимо:

| Механизм | Что это | Флаг | Где работает |
|---|---|---|---|
| `PostProcessor` | встроенная HDR-цепочка bloom/tonemap/FXAA, одно звено | `EngineConfig::enablePostProcessing` (в примере — `--post`) | только 3D-сцена, между 3D- и 2D-проходом |
| `FilterChain` | стек накладываемых друг на друга 2D-фильтров (виньетка, CRT, глитч…) | `EngineConfig::enableFilters` (в примере — `--filters`) | после 2D-прохода, поверх всего кадра |

`enablePostProcessing` включает именно встроенный `PostProcessor`; цепочка
`FilterChain` им не управляется. Оба механизма по умолчанию выключены, поэтому
«эффекты не работают» почти всегда означает забытый флаг.

```cpp
crossrender::PostProcessor* post = engine.Post();
if (post == nullptr) {
    ENG_LOGW("demo", "постобработка выключена: запустите приложение с --post");
} else if (!post->Valid()) {
    ENG_LOGW("demo", "PostProcessor создан, но не инициализирован сценой");
}

if (crossrender::FilterChain* filters = engine.Filters()) {
    filters->Add(crossrender::FilterType::Vignette);
    ENG_LOGI("demo", "фильтров в цепочке: %d", filters->Count());
}
```

### Скриншоты и чтение пикселей

`Screenshot` читает текущий кадр, переворачивает строки (OpenGL хранит изображение
снизу вверх) и пишет PNG в **корень пользователя**, а не в рабочий каталог:
`<GetUserRoot()>/screenshots/<имя>`. Каталог создаётся на лету. Без имени файл
называется `screenshot_<номер кадра>.png`.

`ReadPixel` возвращает цвет пикселя для тестов. У него есть неочевидная
асимметрия: в headless-режиме координата `y` отсчитывается **сверху** (метод
сам переворачивает строку), а на настольной платформе вызов уходит прямо в
`glReadPixels`, где начало координат — **снизу**, и читается текущий
`GL_BACK`. Для детерминированных проверок используйте headless-режим.

```cpp
// В headless-режиме читаем центр кадра сверху вниз — предсказуемо.
const crossrender::Color center = engine.ReadPixel(640, 360);
if (center.a == 0) ENG_LOGW("demo", "кадр не отрисован");

// Файл окажется в <user root>/screenshots/, а не рядом с бинарником.
const std::string path = engine.Screenshot("level7.png");
ENG_LOGI("demo", "скриншот: %s", path.c_str());
```

### Escape и возврат в меню

Движок **не** обрабатывает `Escape` и не вызывает `SceneManager::Back()` сам.
Возврат в меню — задача сцены: она читает `GetInput().KeyPressed(Key::Escape)`
в `Update` и вызывает `ctx.scenes->GoTo("menu", …)`. Метод
`SceneManager::Back()` существует для платформенной кнопки «назад» (Android),
и приложение вызывает его из своего кода; тогда сцена получает
`OnBackPressed`, а при отказе снимается верхняя сцена стека.

```cpp
// Типичный обработчик Esc внутри сцены (движок его не делает за вас).
void Update(crossrender::SceneContext& ctx, crossrender::f32 dt) override {
    (void)dt;
    if (ctx.engine->GetInput().KeyPressed(crossrender::Key::Escape)) {
        ctx.scenes->GoTo("menu");            // мгновенный возврат в меню
    }
}
```

### Ловушки

* **`SceneTarget()` всегда `nullptr`.** Поле создаётся и уничтожается, но
  нигде не заполняется; для HDR-цели постобработки используйте
  `Post()->SceneTarget()`.
* **Постобработка — опция.** `enablePostProcessing` по умолчанию `false`, а
  созданный движком `PostProcessor` не инициализируется: `Init()` вызывает
  сцена. Без обоих условий эффектов не будет.
* **Фильтры и постобработка независимы** (`enableFilters` и
  `enablePostProcessing`).
* **Retro-режим меняет систему координат.** UI и сцена раскладываются по
  виртуальному разрешению, а `Engine::DpiScale()` в это время всё ещё равен
  DPI окна — не смешивайте его с `SceneContext::dpiScale`.
* **headless выдаёт подставное окно** с нулевыми размерами; ввод — только
  через `Engine::GetInput()`.
* **`ReadPixel` на десктопе читает `GL_BACK` снизу вверх**, в headless —
  сверху вниз.
* **`Init` идемпотентен и не применяет новую конфигурацию** повторно.
* **`maxDeltaTime` ограничивает и явно переданный `dt`**, поэтому большой шаг
  из теста всё равно будет урезан.
* **На паузе `onUpdate` продолжает вызываться** с нулевой дельтой, а
  `SceneManager::Update` — нет.
* **`Resources()` до `Init()` разыменовывает пустой указатель** — кэш
  создаётся только при инициализации.
* `cpuMs` и `gpuMs` в `EngineStats` никем не заполняются и остаются нулями.

```cpp
// Проверка ловушки с паузой: сцена заморожена, но оверлеи и хук живы.
engine.SetPaused(true);
engine.Step(1.0f / 60.0f);   // Scene::Update не вызовется, onUpdate — вызовется с dt = 0
ENG_ASSERT(engine.Paused());
```

## Члены класса

### Поля EngineConfig: окно, headless и headlessTarget

Первая группа полей отвечает за то, куда движок рисует кадр. Для
компактности, как разрешает стандарт документации, родственные поля
сгруппированы в таблицу, а пример — один общий на группу.

| Поле | Смысл |
|---|---|
| `window` | описание окна `WindowDesc`: заголовок, размеры, vsync, MSAA, режим, границы контекста OpenGL |
| `headless` | `true` — не создавать окно, рисовать в offscreen-таргет (тесты, CI, скриншоты) |
| `headlessTarget` | описание offscreen-таргета `RenderTargetDesc`: размеры, глубина, MSAA; используется только при `headless` |

```cpp
crossrender::EngineConfig cfg;
cfg.window.title = "Без окна";
cfg.window.width = 1920;
cfg.window.height = 1080;
cfg.window.vsync = true;
cfg.window.msaaSamples = 8;

// Тот же конфиг, но для серверной отрисовки без окна.
crossrender::EngineConfig offscreen = cfg;
offscreen.headless = true;
offscreen.headlessTarget.width = 1920;
offscreen.headlessTarget.height = 1080;
offscreen.headlessTarget.depth = true;
offscreen.headlessTarget.samples = 1;
```

### Поля EngineConfig: подсистемы

Вторая группа включает и выключает крупные подсистемы. Отключённая подсистема
не инициализируется, и её объект остаётся в инертном состоянии: например, без
`enableUI` виджеты не рисуются, а без `enableAudio` микшер молчит.

| Поле | По умолчанию | Смысл |
|---|---|---|
| `enable3D` | `true` | инициализировать `Renderer3D`; при `false` сцены с `Wants3D()` не рисуют мир |
| `enableAudio` | `true` | инициализировать звуковой бэкенд; без устройства микшер работает «молча» |
| `enableUI` | `true` | инициализировать `UiContext` и вызывать кадр UI |

```cpp
crossrender::EngineConfig cfg;
cfg.enable3D = true;      // нужен 3D-проход
cfg.enableUI = true;      // нужны виджеты
cfg.enableAudio = false;  // сервер без звуковой карты
```

### Поля EngineConfig: постобработка, фильтры и retro

Третья группа управляет эффектами поверх кадра. `PostProcessor` и
`FilterChain` — разные механизмы (см. «Фильтры и постобработка» выше), а
`retro` описывает пиксельный/ASCII-дисплей целиком, включая палитру,
дизеринг и CRT-эффекты.

| Поле | Смысл |
|---|---|
| `enablePostProcessing` | создать `PostProcessor` (HDR bloom/tonemap); в примере включается флагом `--post` |
| `enableFilters` | создать стек `FilterChain`; в примере — флаг `--filters` |
| `filtersDefaultEnabled` | при создании цепочки сразу добавить пресет `"Clean"` |
| `retro` | настройки `RetroSettings`: `mode`, `virtualWidth`/`virtualHeight`, `palette`, `dither`, `scanlines`, `crtCurvature` и другие; `RetroMode::Off` выключает режим |

```cpp
crossrender::EngineConfig cfg;
cfg.enablePostProcessing = true;   // bloom + tonemap для 3D
cfg.enableFilters = true;          // стек 2D-фильтров
cfg.filtersDefaultEnabled = true;  // стартуем с пресета "Clean"

cfg.retro.mode = crossrender::RetroMode::Ascii;
cfg.retro.virtualWidth = 240;
cfg.retro.virtualHeight = 135;
cfg.retro.dither = true;
```

### Поля EngineConfig: время, кадр и генератор случайных чисел

Четвёртая группа задаёт ритм главного цикла. `fixedTimeStep` попадает в
`Clock::SetFixedStep`, `maxDeltaTime` защищает от гигантских скачков после
паузы, `targetFps` ограничивает частоту цикла, а `randomSeed` — начальное
зерно генератора.

| Поле | По умолчанию | Смысл |
|---|---|---|
| `fixedTimeStep` | `1/60` | шаг фиксированного обновления в `Clock` (физика, сети) |
| `maxDeltaTime` | `0.1` | верхний предел дельты кадра в секундах |
| `targetFps` | `0` | `0` — без ограничения (vsync всё равно действует); иначе цикл спит до следующего кадра |
| `randomSeed` | `1337` | зерно генератора случайных чисел движка |

```cpp
crossrender::EngineConfig cfg;
cfg.fixedTimeStep = 1.0f / 120.0f;   // физика на 120 Гц
cfg.maxDeltaTime = 0.05f;            // не больше 50 мс на кадр
cfg.targetFps = 60;                  // ограничить цикл 60 кадрами в секунду
cfg.randomSeed = 2024;
```

### Поля EngineConfig: содержимое, пути и отладка

Пятая группа — метаданные приложения. `startScene` удобен тем, что `Engine`
его не открывает сам, а `RunExample` — открывает; `assetsPath` переопределяет
корень ассетов, `enableHotReload` включает слежение за файлами,
`enableDebugOverlay` — стартовое состояние отладочного оверлея.

| Поле | Смысл |
|---|---|
| `title` | заголовок приложения и окна по умолчанию |
| `assetsPath` | необязательный корень ассетов; пустой — используется `GetAssetRoot()` |
| `startScene` | имя сцены, которую `RunExample` откроет после регистрации сцен |
| `enableDebugOverlay` | показать отладочный оверлей сразу после старта |
| `enableHotReload` | включить слежение за изменениями файлов |

```cpp
crossrender::EngineConfig cfg;
cfg.title = "Моя игра";
cfg.assetsPath = "/opt/game/assets";   // иначе — корень ассетов платформы
cfg.startScene = "menu";
cfg.enableDebugOverlay = true;
cfg.enableHotReload = true;
```

### Поля EngineStats: тайминги и счётчики

`EngineStats` — снимок кадра, который заполняется в конце `Engine::Step`.
Родственные счётчики снова собраны в таблицу с одним примером на группу.
Помните, что `cpuMs` и `gpuMs` текущей реализацией не заполняются — они
остаются нулями.

| Поле | Смысл |
|---|---|
| `frameMs` | длительность кадра в миллисекундах (из дельты кадра) |
| `cpuMs` | время CPU в миллисекундах; пока не заполняется |
| `gpuMs` | время GPU в миллисекундах; пока не заполняется |
| `fps` | сглаженная частота кадров из `Clock::FPS()` |

```cpp
const crossrender::EngineStats& s = engine.Stats();
ENG_LOGI("perf", "%.2f мс, %.1f FPS (cpu %.2f, gpu %.2f)", s.frameMs, s.fps, s.cpuMs, s.gpuMs);
```

### Поля EngineStats: геометрия кадра

Счётчики объёма отрисованного. `drawCalls` суммирует 2D- и 3D-вызовы,
`triangles` берётся у 3D-рендерера, `vertices2D` — у 2D-рендерера,
`frame` — номер кадра часов.

| Поле | Смысл |
|---|---|
| `drawCalls` | суммарное число вызовов отрисовки за кадр (2D + 3D) |
| `triangles` | треугольники, отправленные 3D-рендерером |
| `vertices2D` | вершины, отправленные 2D-рендерером |
| `frame` | номер кадра из `Clock::Frame()` |

```cpp
const crossrender::EngineStats& s = engine.Stats();
ENG_LOGI("perf", "кадр %llu: %d вызовов, %d треугольников, %d вершин 2D",
         static_cast<unsigned long long>(s.frame), s.drawCalls, s.triangles, s.vertices2D);
```

### Поля EngineStats: сцена, звук и имя

Остальные поля описывают, что именно рисуется и звучит. `sceneStack` — это
глубина стека сцен, `sceneName` — имя активной сцены из `Scene::Name()`,
`activeVoices` — число звучащих голосов микшера.

| Поле | Смысл |
|---|---|
| `sceneStack` | глубина стека сцен (`SceneManager::StackDepth()`) |
| `sceneName` | имя активной сцены |
| `activeVoices` | число активных голосов `Audio` (0 без звука) |

```cpp
const crossrender::EngineStats& s = engine.Stats();
ENG_LOGI("demo", "сцена '%s', стек %d, голосов %d", s.sceneName.c_str(), s.sceneStack,
         s.activeVoices);
```

### `Engine() / ~Engine()`

Конструктор создаёт пустой фасад: подсистемы ещё не инициализированы, и
`Init()` обязателен. Деструктор вызывает `Shutdown()`, поэтому отдельный
вызов при выходе из `main` не обязателен. Копирование запрещено — движок
владеет окном и контекстом OpenGL.

```cpp
{
    crossrender::Engine engine;
    engine.Init(cfg);
    // ... работа ...
}   // деструктор сам вызовет Shutdown()
```

### `bool Init(const EngineConfig& config)`

Создаёт окно (или offscreen-контекст), загружает OpenGL, инициализирует
рендереры, интерфейс, звук, кэш ресурсов и вьюпорт. Возвращает `false` при
фатальной ошибке (нет платформы, окна, контекста или точек входа OpenGL).
Повторный вызов после успеха ничего не делает и возвращает `true`, **не
применяя** новую конфигурацию. Побочно включает уровень логирования `Debug`.

```cpp
crossrender::Engine engine;
const bool ok = engine.Init(cfg);
if (!ok) {
    ENG_LOGE("demo", "инициализация не удалась — завершаемся");
    return 1;
}
ENG_LOGI("demo", "вьюпорт движка: %.0fx%.0f", engine.Viewport().w, engine.Viewport().h);
```

### `void Shutdown()`

Освобождает всё в обратном порядке: сцены, звук, интерфейс, рендереры,
фильтры, retro-дисплей, постобработку, таргеты, кэш ресурсов, окно и
платформу. Безопасен для повторного вызова и для неинициализированного
движка. После `Shutdown()` можно снова вызвать `Init()`.

```cpp
engine.Shutdown();
ENG_LOGI("demo", "движок остановлен; можно инициализировать заново");
```

### `int Run()`

Блокирующий главный цикл для настольных платформ: пока `Quit()` не
установлен, вызывает `Step()`, а при `targetFps > 0` досыпает остаток кадра.
Возвращает `0` при нормальном выходе и `1`, если вызван до успешного `Init()`.
На мобильных и WASM цикл принадлежит системе — там вызывайте `Step()` сами.

```cpp
if (!engine.Scenes().SetScene(engine.Config().startScene)) return 1;
const int rc = engine.Run();   // вернётся после engine.Quit()
return rc;
```

### `void Step(f32 dt = -1.0f)`

Один кадр движка: ввод, вьюпорт, часы, обновление сцены, выбор цели кадра,
3D-проход, постобработка, кадр 2D/UI, оверлеи, фильтры, статистика, звук и
обмен буферов. Аргумент `dt` переопределяет дельту кадра (полезно в тестах и
для детерминированной отрисовки), но всё равно ограничивается
`EngineConfig::maxDeltaTime`. Полный порядок — в «Порядке кадра целиком».

```cpp
// Детерминированные 120 кадров по 1/60 секунды — типичный приём в тестах.
for (int i = 0; i < 120; ++i) engine.Step(1.0f / 60.0f);
```

### `void Quit()`

Просит завершить приложение: выставляет флаг, который `Run()` проверит после
текущего кадра. Внутри кадра ничего не прерывается — `Step` доработает до
конца, включая обмен буферов.

```cpp
if (engine.GetInput().KeyPressed(crossrender::Key::Escape) && engine.Scenes().StackDepth() == 0) {
    engine.Quit();
}
```

### `bool ShouldQuit() const`

Возвращает запрошен ли выход. Нужен платформам, где цикл внешний: код крутит
`while (!engine.ShouldQuit()) engine.Step();`. Запрос выхода ставится также
при закрытии окна пользователем.

```cpp
while (!engine.ShouldQuit()) {
    engine.Step();
}
```

### `Window& GetWindow()`

Возвращает настоящее окно платформы. В headless-режиме вместо `nullptr`
возвращается **подставной**, никогда не создававшийся `Window`: его размеры
нулевые, `ShouldClose()` всегда `false`, а ввод пуст. Проверяйте
`Headless()`, если поведение зависит от реальной платформы.

```cpp
crossrender::Window& window = engine.GetWindow();
if (engine.Headless()) {
    ENG_LOGW("demo", "окна нет: это подставной объект с нулевыми размерами");
} else {
    window.SetTitle("Моя игра — уровень 3");
    ENG_LOGI("demo", "окно %dx%d, dpi %.2f", window.Width(), window.Height(), window.DpiScale());
}
```

### `Input& GetInput()`

Ввод текущего кадра: клавиши, кнопки мыши, геймпады. На настольной платформе
это тот же объект, что `GetWindow().GetInput()`, а в headless-режиме —
внутренний буфер. **Всегда** читайте ввод отсюда: `window.GetInput()` в
headless вернёт пустое состояние другого объекта. Состояние «нажато в этом
кадре» живёт ровно один кадр.

```cpp
const crossrender::Input& in = engine.GetInput();
if (in.KeyPressed(crossrender::Key::Space)) Jump();
if (in.MouseDown(crossrender::MouseButton::Left)) Fire(in.MousePos());
```

### `Renderer2D& R2D()`

2D-рендерер: пути, фигуры, текст, изображения, виджеты. Движок сам открывает
и закрывает его кадр внутри `Step`, поэтому сцена рисует в уже открытый кадр.

```cpp
crossrender::Renderer2D& r2d = engine.R2D();
r2d.FillRect(0.0f, 0.0f, 200.0f, 40.0f, crossrender::Color::FromRGB(0x101527));
ENG_LOGI("demo", "2D-вызовов за кадр: %d", engine.Stats().drawCalls);
```

### `Renderer3D& R3D()`

3D-рендерер: меши, материалы, свет, тени, отладочные линии. Кадр 3D тоже
открывает движок, а сцена настраивает камеру в `Prepare3D` и рисует в
`Render3D`.

```cpp
crossrender::Renderer3D& r3d = engine.R3D();
ENG_LOGI("demo", "источников света: %d, камера: (%.1f, %.1f, %.1f)", r3d.LightCount(),
         r3d.GetCamera().position.x, r3d.GetCamera().position.y, r3d.GetCamera().position.z);
```

### `UiContext& UI()`

Контекст интерфейса: тема, виджеты, состояние ввода и статистика UI. Кадр UI
движок открывает сам, поэтому сцена просто вызывает виджеты в `Render2D`.
Здесь же настраивается тема и шрифт интерфейса.

```cpp
crossrender::UiContext& ui = engine.UI();
ui.Theme().font = engine.DefaultFont();
ui.Theme().buttonHeight = 40.0f;
ENG_LOGI("demo", "виджетов в прошлом кадре: %d", ui.Stats().widgets);
```

### `SceneManager& Scenes()`

Менеджер сцен: регистрация фабрик, смена сцены, стек, переходы. Через него
приложение и сцены управляют навигацией.

```cpp
engine.Scenes().Register("menu", [] { return std::make_unique<MenuScene>(); });
if (!engine.Scenes().SetScene("menu")) {
    ENG_LOGE("demo", "сцена 'menu' не зарегистрирована");
}
```

### `Audio& GetAudio()`

Микшер: воспроизведение клипов и музыки, шины, громкость, 3D-позиционирование.
Возвращает глобальный синглтон `Audio::Get()`; если звук был выключен
конфигурацией, микшер не инициализирован, но методы безопасны и работают
«молча».

```cpp
crossrender::Audio& audio = engine.GetAudio();
crossrender::AudioClip* clip = engine.Resources().Audio_("audio/coin.wav");
if (clip != nullptr && clip->Valid()) audio.Play(*clip);
ENG_LOGI("demo", "голосов: %d", audio.ActiveVoices());
```

### `PostProcessor* Post()`

Возвращает встроенный HDR-постпроцессор или `nullptr`, если
`EngineConfig::enablePostProcessing` выключен. Даже когда объект создан, он
**не инициализирован**: `Valid()` будет `false`, пока сцена или приложение не
вызовет `Init(width, height)`. Только при `Valid()` движок запускает цепочку.

```cpp
crossrender::PostProcessor* post = engine.Post();
if (post != nullptr && !post->Valid()) {
    const bool ok = post->Init(1280, 720);
    ENG_LOGI("demo", "PostProcessor::Init -> %s", ok ? "ок" : "ошибка");
}
```

### `PostProcessSettings& PostSettings()`

Настройки, которые движок применяет, когда активная сцена вернула `true` из
`Scene::WantsPostProcessing()`: включение цепочки, bloom с порогом и
интенсивностью, tonemap с экспозицией и режимом, FXAA, виньетка, зерно и
хроматические аберрации. Сцена вправе менять их каждый кадр.

```cpp
crossrender::PostProcessSettings& ps = engine.PostSettings();
ps.enabled = true;
ps.bloom = true;
ps.bloomThreshold = 1.2f;
ps.bloomIntensity = 0.7f;
ps.tonemap = true;
ps.exposure = 1.1f;
ps.tonemapMode = 0;      // 0 = ACES
ps.fxaa = true;
ps.vignette = true;
ps.vignetteIntensity = 0.3f;
```

### `RetroDisplay& Retro()`

Пиксельный/ASCII-дисплей: виртуальный таргет и его разрешение. Объект
существует всегда после `Init()`, но работоспособность проверяйте через
`Valid()`; при выключенном режиме смысла в нём нет.

```cpp
crossrender::RetroDisplay& retro = engine.Retro();
if (engine.RetroActive() && retro.Valid()) {
    ENG_LOGI("demo", "виртуальный буфер %dx%d", retro.VirtualWidth(), retro.VirtualHeight());
    crossrender::RenderTarget* virtualTarget = retro.VirtualTarget();
    (void)virtualTarget;   // можно семплировать нарисованное
}
```

### `RetroSettings& RetroCfg()`

Ссылка на настройки retro-режима внутри `EngineConfig`. Их можно менять на
ходу: движок перечитывает `mode`, разрешение, палитру, дизеринг и CRT-поля
при каждом кадре.

```cpp
engine.RetroCfg().mode = crossrender::RetroMode::Pixel;
engine.RetroCfg().virtualWidth = 384;
engine.RetroCfg().virtualHeight = 216;
engine.RetroCfg().scanlines = true;
```

### `bool RetroActive() const`

Истинно, если retro-режим включён (`RetroSettings::mode != RetroMode::Off`).
Не гарантирует, что GL-ресурсы готовы, — для этого есть `Retro().Valid()`.

```cpp
if (engine.RetroActive()) {
    ENG_LOGW("demo", "UI раскладывается по виртуальному разрешению, а не по окну");
}
```

### `bool RetroInitialised() const`

Истинно, если объект `RetroDisplay` был создан в `Init()`. Это ещё не значит,
что режим работает: проверяйте `Retro().Valid()`.

```cpp
if (!engine.RetroInitialised()) {
    ENG_LOGW("demo", "retro-дисплей не создан (движок не инициализирован)");
}
```

### `FilterChain* Filters()`

Стек накладываемых фильтров или `nullptr`, если `EngineConfig::enableFilters`
не включён (либо цепочка не инициализировалась). Через него добавляют,
удаляют и переупорядочивают фильтры. Применяется движком после 2D-прохода.

```cpp
if (crossrender::FilterChain* filters = engine.Filters()) {
    filters->Add(crossrender::FilterType::Scanlines);
    filters->Add(crossrender::FilterType::Vignette);
    ENG_LOGI("demo", "в цепочке %d фильтров", filters->Count());
} else {
    ENG_LOGW("demo", "фильтры выключены: нужен флаг --filters");
}
```

### `bool FiltersActive() const`

Истинно, когда цепочка существует, инициализирована и содержит хотя бы один
фильтр. Только в этом случае движок рендерит кадр в промежуточный таргет,
чтобы потом прогнать фильтры.

```cpp
if (engine.FiltersActive()) {
    ENG_LOGI("demo", "кадр пойдёт через %d фильтров", engine.Filters()->Count());
} else {
    ENG_LOGI("demo", "фильтры не активны — рисуем прямо на экран");
}
```

### `RenderTarget* SceneTarget()`

Задумывался как доступ к цели кадра (композитной или сцены), но в текущей
реализации **всегда возвращает `nullptr`**: поле нигде не создаётся, только
сбрасывается в `Shutdown()`. Для HDR-цели постобработки используйте
`Post()->SceneTarget()`.

```cpp
crossrender::RenderTarget* target = engine.SceneTarget();
if (target == nullptr) {
    ENG_LOGW("demo", "SceneTarget() пуст — это ожидаемо; берите Post()->SceneTarget()");
}
```

### `Clock& GetClock()`

Часы кадра: текущая дельта, номер кадра, FPS, масштаб времени и фиксированный
шаг. Движок вызывает `Tick()` сам, но часы полезны сцене: например, для
физики с фиксированным шагом или для паузы через `SetTimeScale`.

```cpp
crossrender::Clock& clock = engine.GetClock();
const crossrender::u32 steps = clock.ConsumeFixedSteps();
for (crossrender::u32 i = 0; i < steps; ++i) {
    StepPhysics(clock.FixedStep());   // ваш фиксированный шаг физики
}
ENG_LOGI("demo", "кадр %llu, %.1f FPS", static_cast<unsigned long long>(clock.Frame()), clock.FPS());
```

### `const EngineConfig& Config() const`

Доступ только для чтения к конфигурации, с которой движок был инициализирован
(или — после повторного `Init` — к первой успешной). Удобно, чтобы узнать
`maxDeltaTime`, `headless` или стартовую сцену.

```cpp
const crossrender::EngineConfig& cfg = engine.Config();
ENG_LOGI("demo", "стартовая сцена '%s', maxDeltaTime %.3f", cfg.startScene.c_str(),
         cfg.maxDeltaTime);
```

### `const EngineStats& Stats() const`

Снимок статистики последнего завершённого кадра. Осмыслен после `Step` (или в
следующем кадре): `drawCalls` 2D-рендерера известны только после `EndFrame`.

```cpp
const crossrender::EngineStats& s = engine.Stats();
ENG_LOGI("perf", "%.2f мс · %d вызовов · %d треугольников · сцена '%s'", s.frameMs, s.drawCalls,
         s.triangles, s.sceneName.c_str());
```

### `SceneContext& Context()`

Контекст, который движок передаёт во все хуки сцен. Через него можно
посмотреть вьюпорт и дельту кадра снаружи, а также взять указатели на
подсистемы. Поля `Context()` — изменяемые: например, можно поменять
`ctx.viewport`, но следующим кадром движок перезапишет его.

```cpp
crossrender::SceneContext& ctx = engine.Context();
ENG_LOGI("demo", "вьюпорт %.0fx%.0f, dpi %.2f, dt %.4f", ctx.viewport.w, ctx.viewport.h,
         ctx.dpiScale, ctx.dt);
```

### `bool Headless() const`

Истинно, если движок запущен без окна (`EngineConfig::headless`). В этом
режиме не полагайтесь на `GetWindow()` и помните, что ввод нужно читать через
`GetInput()`.

```cpp
if (engine.Headless()) {
    ENG_LOGI("demo", "offscreen-режим: окно не создавалось");
} else {
    engine.GetWindow().SetTitle("Моя игра");
}
```

### `Rect Viewport() const`

Логический прямоугольник кадра с учётом DPI и безопасной зоны, а в
retro-режиме — виртуального разрешения. Это основная система координат для
раскладки: не берите размеры из окна.

```cpp
const crossrender::Rect viewport = engine.Viewport();
const crossrender::Rect footer{viewport.x, viewport.Bottom() - 48.0f, viewport.w, 48.0f};
ENG_LOGI("demo", "нижняя панель: %.0f x %.0f", footer.w, footer.h);
```

### `f32 DpiScale() const`

Множитель логических единиц в физические пиксели окна. Нужен, например, для
размеров рендер-таргетов. В retro-режиме остаётся DPI окна — не путайте со
`SceneContext::dpiScale`, который там равен единице.

```cpp
const int pxW = static_cast<int>(engine.Viewport().w * engine.DpiScale());
ENG_LOGI("demo", "кадр в физических пикселях: %d", pxW);
```

### `Font* DefaultFont() const`

Шрифт интерфейса по умолчанию (растровый). Может быть `nullptr`, если шрифты
недоступны; проверяйте `Valid()` перед рисованием текста.

```cpp
crossrender::Font* font = engine.DefaultFont();
if (font != nullptr && font->Valid()) {
    engine.R2D().DrawText(*font, "Готово", 32.0f, 32.0f, crossrender::Color::White, 24.0f);
}
```

### `Font* DefaultSdfFont() const`

Шрифт по умолчанию в SDF-варианте: он остаётся гладким при любом масштабе и
подходит для крупных заголовков. Тоже может быть `nullptr`.

```cpp
crossrender::Font* sdf = engine.DefaultSdfFont();
if (sdf != nullptr && sdf->Valid()) {
    engine.R2D().DrawText(*sdf, "ЗАГОЛОВОК", 640.0f, 80.0f, crossrender::Color::White, 64.0f,
                          crossrender::TextAlign::Center, crossrender::TextBaseline::Top);
}
```

### `ResourceCache& Resources()`

Кэш ресурсов движка с уже настроенным корнем ассетов. Это основной способ
загружать текстуры, шрифты, модели, звуки и шейдеры. Вызов до `Init()`
разыменовывает пустой указатель — сначала инициализируйте движок.

```cpp
crossrender::Texture* tex = engine.Resources().Texture_("textures/logo.png", /*srgb=*/true);
if (tex == nullptr) ENG_LOGW("demo", "логотип не найден");
ENG_LOGI("demo", "ресурсов в кэше: %d", static_cast<int>(engine.Resources().Count()));
```

### `std::function<void(Engine&, f32)> onUpdate`

Хук, вызываемый каждый кадр сразу после `SceneManager::Update`, **до**
3D-прохода. Удобен для глобальной логики вне сцен (профилирование, ввод
уровня приложения, отладка). Вызывается и на паузе, но с нулевой дельтой.

```cpp
engine.onUpdate = [](crossrender::Engine& e, crossrender::f32 dt) {
    if (e.GetInput().KeyPressed(crossrender::Key::F1)) e.ToggleDebugOverlay();
    static crossrender::f32 accumulated = 0.0f;
    accumulated += dt;
};
```

### `std::function<void(Engine&)> onOverlay`

Хук, вызываемый после `SceneManager::Render2D`, но до перехода и закрытия UI.
Рисуйте здесь глобальные оверлеи (FPS, отладочные панели), не привязанные к
конкретной сцене.

```cpp
engine.onOverlay = [](crossrender::Engine& e) {
    crossrender::Font* font = e.DefaultFont();
    if (font != nullptr) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "FPS %.0f", e.Stats().fps);
        e.R2D().DrawText(*font, buf, 12.0f, 12.0f, crossrender::Color::Cyan, 16.0f);
    }
};
```

### `void SetSceneContextExtras(void* user)`

Кладёт произвольный указатель в движок. Само значение в `SceneContext` не
попадает — сцены читают его через `Engine::UserData()`. Удобно, чтобы не
заводить глобальные переменные для состояния приложения.

```cpp
struct GameSave {
    int level = 1;
    int coins = 0;
};

GameSave save;
engine.SetSceneContextExtras(&save);
ENG_LOGI("demo", "состояние приложения привязано к движку");
```

### `void* UserData() const`

Возвращает указатель, переданный в `SetSceneContextExtras`, либо `nullptr`.
Приводите его к своему типу вручную.

```cpp
auto* save = static_cast<GameSave*>(engine.UserData());
if (save != nullptr) {
    ++save->coins;
    ENG_LOGI("demo", "монет: %d", save->coins);
}
```

### `std::string Screenshot(const std::string& filename = "")`

Сохраняет текущий кадр в PNG и возвращает путь, либо пустую строку при
ошибке. Файл пишется в `<user root>/screenshots/`; если имя не задано, оно
формируется как `screenshot_<номер кадра>.png`. В headless-режиме читается
offscreen-таргет, на настольной платформе — текущий `GL_BACK`.

```cpp
const std::string path = engine.Screenshot("boss_fight.png");
if (path.empty()) {
    ENG_LOGE("demo", "скриншот не записался");
} else {
    ENG_LOGI("demo", "сохранено в %s (корень пользователя, не рабочий каталог)", path.c_str());
}
```

### `Color ReadPixel(int x, int y)`

Читает цвет одного пикселя последнего кадра — инструмент тестов и
автоматических проверок. В headless-режиме координата `y` идёт сверху вниз, а
на настольной платформе вызов транслируется прямо в `glReadPixels`, где
начало координат снизу; вне границ или при ошибке возвращается чёрный цвет.

```cpp
// Проверяем, что фон меню действительно тёмно-синий.
const crossrender::Color px = engine.ReadPixel(4, 4);
if (px.r > 0.2f) ENG_LOGW("demo", "неожиданный цвет фона: %.2f", px.r);
```

### `void ToggleDebugOverlay()`

Переключает встроенный отладочный оверлей (FPS, номер кадра, сцена, стек,
вызовы отрисовки, треугольники, GPU). В примере он также вешается на `F1`.

```cpp
engine.ToggleDebugOverlay();
ENG_LOGI("demo", "отладочный оверлей: %s", engine.DebugOverlayVisible() ? "вкл" : "выкл");
```

### `bool DebugOverlayVisible() const`

Сообщает, показан ли сейчас отладочный оверлей. Удобно, чтобы не рисовать
поверх него собственные панели или, наоборот, дополнять его.

```cpp
if (!engine.DebugOverlayVisible()) {
    engine.R2D().DrawText(*engine.DefaultFont(), "F1 — статистика", 12.0f, 12.0f,
                          crossrender::Color{0.6f, 0.65f, 0.75f, 1.0f}, 14.0f);
}
```

### `void SetPaused(bool paused)`

Ставит приложение на паузу: `SceneManager::Update` перестаёт вызываться, и
дельта кадра становится нулевой, но кадр продолжает рисоваться — меню паузы
и оверлеи работают. Хук `onUpdate` при этом всё равно вызывается (с `dt = 0`).

```cpp
if (engine.GetInput().KeyPressed(crossrender::Key::P)) {
    engine.SetPaused(!engine.Paused());
    ENG_LOGI("demo", "пауза: %s", engine.Paused() ? "да" : "нет");
}
```

### `bool Paused() const`

Возвращает текущее состояние паузы, выставленное `SetPaused`. Игровая логика
сцены может использовать его, чтобы отличать «заморожено движком» от
собственной паузы.

```cpp
if (engine.Paused() && engine.GetInput().KeyPressed(crossrender::Key::Escape)) {
    engine.SetPaused(false);   // снимаем паузу
}
```

### `int RunExample(const EngineConfig& config, const std::function<void(Engine&)>& setup)`

Готовая обвязка для примеров: создаёт `Engine`, инициализирует его данной
конфигурацией, вызывает `setup` для регистрации сцен, открывает
`config.startScene` и запускает `Run()`, после чего корректно завершает
движок. Возвращает код возврата процесса: `0` при успехе, `1` при ошибке
инициализации или неизвестной стартовой сцене.

```cpp
int main() {
    crossrender::EngineConfig cfg;
    cfg.window.title = "Пример";
    cfg.startScene = "hello";

    return crossrender::RunExample(cfg, [](crossrender::Engine& engine) {
        engine.Scenes().Register("hello", [] { return std::make_unique<HelloScene>(); });
    });
}
```

## Пример целиком

```cpp
#include "crossrender/Engine.h"
#include "crossrender/Resource.h"
#include "crossrender/scene/Scene.h"
#include "crossrender/core/File.h"
#include "crossrender/core/Log.h"

#include <cstdio>
#include <memory>
#include <string>

// Простейшее приложение на фасаде Engine: конфигурация, регистрация сцены,
// глобальные хуки, пауза, скриншот и корректное завершение.
class DemoScene final : public crossrender::Scene {
public:
    [[nodiscard]] const char* Name() const override { return "demo"; }
    [[nodiscard]] const char* Description() const override { return "Демонстрация Engine"; }

    void OnEnter(crossrender::SceneContext& ctx) override {
        banner_ = ctx.engine->Resources().Texture_("textures/banner.png", /*srgb=*/true);
        ENG_LOGI("demo", "сцена открыта, вьюпорт %.0fx%.0f", ctx.viewport.w, ctx.viewport.h);
    }

    void Update(crossrender::SceneContext& ctx, crossrender::f32 dt) override {
        elapsed_ += dt;
        const crossrender::Input& input = ctx.engine->GetInput();
        if (input.KeyPressed(crossrender::Key::P)) ctx.engine->SetPaused(!ctx.engine->Paused());
        if (input.KeyPressed(crossrender::Key::F2)) ctx.engine->Screenshot("demo.png");
        if (input.KeyPressed(crossrender::Key::Escape)) ctx.engine->Quit();
    }

    void Render2D(crossrender::SceneContext& ctx) override {
        crossrender::Renderer2D& r2d = *ctx.r2d;
        crossrender::Font* font = ctx.engine->DefaultFont();
        if (banner_ != nullptr && banner_->Valid()) {
            r2d.Image(*banner_, crossrender::Rect{ctx.viewport.x + 20.0f, ctx.viewport.y + 20.0f, 320.0f, 120.0f});
        }
        if (font == nullptr) return;
        char buf[96];
        std::snprintf(buf, sizeof(buf), "t = %.1f с, кадр %llu", elapsed_,
                      static_cast<unsigned long long>(ctx.engine->Stats().frame));
        r2d.DrawText(*font, buf, ctx.viewport.x + 20.0f, ctx.viewport.y + 160.0f, crossrender::Color::White,
                     22.0f);
    }

private:
    crossrender::Texture* banner_ = nullptr;
    crossrender::f32 elapsed_ = 0.0f;
};

int main() {
    crossrender::EngineConfig cfg;
    cfg.window.title = "Демонстрация движка";
    cfg.window.width = 1280;
    cfg.window.height = 720;
    cfg.enable3D = true;
    cfg.enableAudio = true;
    cfg.enableUI = true;
    cfg.enablePostProcessing = false;   // включается флагом --post
    cfg.enableFilters = false;          // включается флагом --filters
    cfg.startScene = "demo";
    cfg.fixedTimeStep = 1.0f / 60.0f;
    cfg.maxDeltaTime = 0.1f;

    crossrender::Engine engine;
    if (!engine.Init(cfg)) {
        ENG_LOGE("demo", "движок не инициализировался");
        return 1;
    }

    engine.Scenes().Register("demo", [] { return std::make_unique<DemoScene>(); });

    // Глобальный хук: F1 — встроенный отладочный оверлей.
    engine.onUpdate = [](crossrender::Engine& e, crossrender::f32 dt) {
        (void)dt;
        if (e.GetInput().KeyPressed(crossrender::Key::F1)) e.ToggleDebugOverlay();
    };

    if (!engine.Scenes().SetScene(cfg.startScene)) {
        ENG_LOGE("demo", "стартовая сцена '%s' не найдена", cfg.startScene.c_str());
        engine.Shutdown();
        return 1;
    }

    ENG_LOGI("demo", "корень пользователя: %s", crossrender::GetUserRoot().c_str());
    const int rc = engine.Run();
    engine.Shutdown();
    return rc;
}
```

## См. также

* `docs/scene/Scene.md` — как писать сцены, порядок их хуков, переходы и
  стек; главный потребитель этого заголовка.
* `docs/Resource.md` — `Engine::Resources()` и загрузка ассетов.
* `docs/gfx/Renderer2D.md` — 2D-рендерер, кадр которого открывает движок.
* `docs/gfx/Renderer3D.md` — 3D-проход, камера и свет.
* `docs/gfx/RenderTarget.md` — `PostProcessor`, `PostProcessSettings` и
  рендер-таргеты, включая headless-цель.
* `docs/gfx/FilterChain.md` — стек 2D-фильтров (`Engine::Filters()`).
* `docs/gfx/Retro.md` — пиксельный и ASCII-режимы (`Engine::Retro()`).
* `docs/ui/Ui.md` — `UiContext`, тема и безопасная зона, попадающая в
  `Engine::Viewport()`.
* `docs/core/Time.md` — `Clock`, дельта кадра и фиксированный шаг.
* `docs/core/File.md` — `GetUserRoot()`, куда пишутся скриншоты.
