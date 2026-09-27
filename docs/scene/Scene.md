# crossrender/scene/Scene.h — сцены, переходы и стек сцен

Система сцен: одна сцена — это один экран приложения (меню, уровень,
настройки). Заголовок объявляет базовый класс `Scene`, структуру
`SceneContext` со ссылками на подсистемы, описание перехода `TransitionDesc`
с перечислением `TransitionType` и менеджер `SceneManager`, который владеет
сценами, стеком и переходами между ними.

## Заголовок

```cpp
#include "crossrender/scene/Scene.h"
```

## Обзор

Движок никогда не работает с игрой напрямую: он знает только одну активную
сцену и вызывает её виртуальные методы. Всё, что делает приложение —
наследует `Scene`, переопределяет нужные хуки и регистрирует класс в
`SceneManager` под именем. `SceneManager` создаёт сцены через фабрики, хранит
текущую сцену, стек вложенных сцен и активный переход.

### Как написать свою сцену

Обязателен ровно один метод — `Name()`; остальные хуки имеют пустые
реализации по умолчанию. Обычно переопределяют такой набор:

| Метод | Когда вызывается | Зачем |
|---|---|---|
| `Name()` | при входе в сцену | имя для логов и `CurrentName()` |
| `OnEnter` | сцена стала активной | загрузка ресурсов, настройка камеры |
| `OnExit` | сцену заменяют | освобождение ресурсов |
| `Update` | каждый кадр | логика, ввод, таймеры |
| `Prepare3D` | перед 3D-проходом, если `Wants3D()` | камера, свет, окружение именно этого кадра |
| `Render3D` | внутри 3D-прохода, если `Wants3D()` | отрисовка мира |
| `Render2D` | всегда | HUD, интерфейс, оверлей поверх 3D |
| `OnResize` | при входе и при явном `SceneManager::Resize` | пересчёт раскладки |
| `OnBackPressed` | из `SceneManager::Back()` | своя реакция на «назад»; `true` — событие поглощено |

Копирование запрещено: сцена — уникальный объект, которым владеет менеджер.

```cpp
class PauseScene final : public crossrender::Scene {
public:
    [[nodiscard]] const char* Name() const override { return "pause"; }
    void OnEnter(crossrender::SceneContext& ctx) override { pausedAt_ = ctx.dt; }
    void Update(crossrender::SceneContext& ctx, crossrender::f32 dt) override {
        if (ctx.engine->GetInput().KeyPressed(crossrender::Key::Escape)) ctx.scenes->Pop();
    }
    void Render2D(crossrender::SceneContext& ctx) override {
        ctx.r2d->FillRect(ctx.viewport, crossrender::Color{0, 0, 0, 0.55f});
    }

private:
    crossrender::f32 pausedAt_ = 0.0f;
};
```

### Порядок вызовов за кадр

`Engine::Step` вызывает хуки активной сцены строго в одном порядке; на это
можно опираться:

1. `SceneManager::Update(dt)` продвигает таймер перехода, в его середине
   (когда экран полностью закрыт) выполняет отложенную смену сцен, а затем
   вызывает `Scene::Update(ctx, dt)` **той сцены, которая активна после
   смены**.
2. `Scene::Prepare3D(ctx)` — только если `Wants3D()` вернула `true`.
3. `Scene::Render3D(ctx)` — только если `Wants3D()`.
4. `Scene::Render2D(ctx)` — **всегда**, в том числе для 3D-сцены: это её слой
   интерфейса поверх 3D.
5. Поверх результата `SceneManager::RenderTransition` рисует оверлей
   перехода.

События жизненного цикла при этом приходят так: `OnEnter` → сразу за ним
`OnResize` → далее кадры с `Update`/`Prepare3D`/`Render3D`/`Render2D` →
`OnExit` при замене. Если поверх активной сцены выдвигают другую
(`SceneManager::Push`), первой приходит `OnPause`, а при `Pop` —
`OnResume`. Метод `Scene::PostFrame` в текущей реализации **не вызывается
никем** — это зарезервированный хук.

```cpp
// Так выглядит один кадр с точки зрения сцены: движок сам вызывает эти хуки.
void OneFrame(crossrender::Engine& engine, crossrender::f32 dt) {
    crossrender::SceneManager& scenes = engine.Scenes();
    if (!engine.Paused()) scenes.Update(dt);   // 1: Scene::Update
    if (scenes.Current() && scenes.Current()->Wants3D()) {
        scenes.Prepare3D();                    // 2: Scene::Prepare3D
        scenes.Render3D();                     // 3: Scene::Render3D
    }
    scenes.Render2D();                         // 4: Scene::Render2D
}
```

### Что даёт SceneContext

`SceneContext` — это «пульт» сцены: указатели на движок и его подсистемы
плюс параметры текущего кадра. Движок заполняет его один раз в
`Engine::Init` и обновляет `viewport`, `dpiScale` и `dt` перед каждым кадром.
Через `ctx.engine` доступно всё остальное: ресурсы, постобработка,
`UserData()` и статистика. Единственное, чего в контексте нет, — владения:
все указатели принадлежат движку и живут дольше сцены.

```cpp
void PrintFrameInfo(crossrender::SceneContext& ctx) {
    ENG_LOGI("demo", "кадр: dt=%.3f, вьюпорт %.0fx%.0f, dpi=%.2f, кадр движка %llu", ctx.dt,
             ctx.viewport.w, ctx.viewport.h, ctx.dpiScale,
             static_cast<unsigned long long>(ctx.engine->Stats().frame));
    (void)ctx.scenes;
    (void)ctx.r2d;
    (void)ctx.r3d;
    (void)ctx.ui;
}
```

### Постобработка по запросу сцены

Постобработка (post-processing — цепочка эффектов поверх готового кадра)
включается только для 3D-сцены и только по её просьбе. Сцена переопределяет
`WantsPostProcessing()` (а значит, обязана вернуть `true` и из `Wants3D()`) и
заполняет `Engine::PostSettings()` — движок читает эти настройки в
`Engine::Step` и прогоняет HDR-цепочку bloom/tonemap сам, между 3D-проходом
и 2D-проходом. Есть два неочевидных условия:

* `EngineConfig::enablePostProcessing` должен быть включён (в примере — флаг
  `--post`). Без него `Engine::Post()` вернёт `nullptr`, и сцена отрисуется
  без эффектов.
* Движок создаёт `PostProcessor`, но **не** вызывает его `Init()`. Пока
  сцена не сделает это сама, `Post()->Valid()` равно `false` и цепочка не
  запустится. Вызывайте `Init()` в `OnEnter` или в `Prepare3D`.

```cpp
class BloomScene final : public crossrender::Scene {
public:
    [[nodiscard]] const char* Name() const override { return "bloom"; }
    [[nodiscard]] bool Wants3D() const override { return true; }
    [[nodiscard]] bool WantsPostProcessing() const override { return true; }

    void OnEnter(crossrender::SceneContext& ctx) override {
        // Движок создал PostProcessor, но Init() оставлен сцене.
        crossrender::PostProcessor* post = ctx.engine->Post();
        if (post != nullptr && !post->Valid()) {
            const int w = static_cast<int>(ctx.viewport.w * ctx.dpiScale);
            const int h = static_cast<int>(ctx.viewport.h * ctx.dpiScale);
            post->Init(w, h);
        }
    }

    void Prepare3D(crossrender::SceneContext& ctx) override {
        camera_.position = {0.0f, 3.0f, 8.0f};
        camera_.fovY = crossrender::Radians(55.0f);
        ctx.r3d->SetCamera(camera_);

        // Эти настройки Engine::Step прочитает сам — при условии, что Wants3D()
        // и WantsPostProcessing() вернули true, а PostProcessor инициализирован.
        crossrender::PostProcessSettings& ps = ctx.engine->PostSettings();
        ps.bloom = true;
        ps.bloomThreshold = 1.1f;
        ps.bloomIntensity = 0.9f;
        ps.tonemap = true;
        ps.vignette = true;
    }

    void Render3D(crossrender::SceneContext& ctx) override { (void)ctx; }
    void Render2D(crossrender::SceneContext& ctx) override { (void)ctx; }

private:
    crossrender::Camera camera_{};
};
```

### Минимальная сцена целиком

```cpp
#include "crossrender/Engine.h"
#include "crossrender/scene/Scene.h"
#include "crossrender/core/Log.h"

#include <cstdio>
#include <memory>

// Сцена из одного экрана: считает время, уходит по Esc и рисует заголовок.
class HelloScene final : public crossrender::Scene {
public:
    [[nodiscard]] const char* Name() const override { return "hello"; }
    [[nodiscard]] const char* Description() const override { return "Минимальная сцена"; }
    [[nodiscard]] bool CustomClear() const override { return false; }

    void OnEnter(crossrender::SceneContext& ctx) override {
        time_ = 0.0f;
        ENG_LOGI("hello", "вошли в сцену, вьюпорт %.0fx%.0f", ctx.viewport.w, ctx.viewport.h);
    }

    void Update(crossrender::SceneContext& ctx, crossrender::f32 dt) override {
        time_ += dt;
        // Escape обрабатывает сама сцена: движок не перехватывает клавиши.
        if (ctx.engine->GetInput().KeyPressed(crossrender::Key::Escape) && ctx.scenes->StackDepth() > 0) {
            ctx.scenes->Pop();
        }
    }

    void Render2D(crossrender::SceneContext& ctx) override {
        crossrender::Renderer2D& r2d = *ctx.r2d;
        crossrender::Font* font = ctx.engine->DefaultFont();
        if (font == nullptr) return;

        const crossrender::Rect view = ctx.viewport;
        r2d.FillRect(view, this->ClearColor());
        r2d.DrawText(*font, "Привет, сцена!", view.Center().x, view.Center().y, crossrender::Color::White,
                     32.0f, crossrender::TextAlign::Center, crossrender::TextBaseline::Middle);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "t = %.2f с", time_);
        r2d.DrawText(*font, buf, view.x + 16.0f, view.y + 16.0f,
                     crossrender::Color{0.7f, 0.75f, 0.85f, 1.0f}, 16.0f);
    }

    [[nodiscard]] crossrender::Color ClearColor() const override {
        return crossrender::Color::FromARGB(0xFF0E1016);
    }

private:
    crossrender::f32 time_ = 0.0f;
};

void SetupGame(crossrender::Engine& engine) {
    engine.Scenes().Register("hello", [] { return std::make_unique<HelloScene>(); });
    engine.Scenes().SetScene("hello");
}
```

### Как добавить сцену в пример MEGA_SCENE

Практический способ зарегистрировать сцену в приложении-примере — макрос
`MEGA_SCENE` из `examples/sources/Mega.h`. Он создаёт статический регистратор,
который добавляет сцену в `mega::SceneRegistry`; приложение затем вызывает
`SceneRegistry::Get().InstallInto(engine)`, и все сцены попадают в
`SceneManager` под своими идентификаторами. Класс, переданный в макрос,
обязан конструироваться из `const char*` — идентификатор передаётся в
`Name()`.

```cpp
// examples/sources/scenes/SceneMine.cpp
#include "../Mega.h"

class MineScene final : public mega::MegaScene {
public:
    explicit MineScene(const char* id) : MegaScene(id, /*wants3D=*/true, "3D") {}
    void OnEnterScene(crossrender::SceneContext& ctx) override { (void)ctx; }
    void OnUpdate(crossrender::SceneContext& ctx, crossrender::f32 dt) override { (void)ctx; (void)dt; }
    void On3D(crossrender::SceneContext& ctx) override { (void)ctx; }
    void OnUI(crossrender::SceneContext& ctx, const crossrender::Rect& content) override { (void)ctx; (void)content; }
};

// Идентификатор, заголовок, категория и описание попадают в список сцен.
MEGA_SCENE(MineScene, "3d-mine", "My Scene", "3D", "Что демонстрирует сцена");
```

### Ловушки

* **Escape и возврат в меню.** Движок не слушает `Escape` и не вызывает
  `SceneManager::Back()`. Клавишу обрабатывает сама сцена в `Update` (см.
  пример выше), а `Back()` предназначен для платформенной кнопки «назад»,
  которую приложение вызывает самостоятельно. Если `OnBackPressed` вернул
  `false`, `Back()` снимает верхнюю сцену со стека.
* **Переход асинхронен.** При `TransitionDesc::type != TransitionType::None`
  и `duration > 0` смена происходит не сразу, а в середине перехода; до этого
  `Transitioning()` истинно, а `InputBlocked()` — только пока экран ещё не
  закрыт (`TransitionProgress() < 0.5`).
* **`crossFade` не реализован.** Поле `TransitionDesc::crossFade` нигде не
  читается: обе сцены одновременно не рисуются. Для взаимного проявления
  используйте `TransitionType::CrossDissolve` (он всё равно рисует
  полупрозрачный оверлей, а не вторую сцену).
* **`SceneManager::Resize` никто не вызывает.** `Engine` не подписан на
  изменение размера окна, поэтому `OnResize` приходит только при входе в
  сцену. Если нужна реакция на ресайз, сравнивайте `ctx.viewport` в `Update`.
* **`Engine::SceneTarget()` всегда `nullptr`** (см. `docs/Engine.md`) — для
  доступа к HDR-цели постобработки используйте `Post()->SceneTarget()`.

```cpp
void ResizeAware(crossrender::SceneContext& ctx) {
    // Ресайз не доставляется: замечаем изменение вьюпорта сами.
    static crossrender::Rect last{};
    if (ctx.viewport.w != last.w || ctx.viewport.h != last.h) {
        last = ctx.viewport;
        ENG_LOGI("demo", "вьюпорт изменился на %.0fx%.0f", last.w, last.h);
    }
}
```

## Члены класса

### `SceneContext::engine`

Указатель на `Engine` — владельца окна, рендереров, ресурсов и настроек.
Через него сцена получает всё, чего нет в самом `SceneContext`:
`Resources()`, `Post()`, `PostSettings()`, `Stats()`, `UserData()`,
`DefaultFont()`. Указатель всегда непуст, пока сцена активна.

```cpp
void OnScore(crossrender::SceneContext& ctx, int score) {
    crossrender::ResourceCache& res = ctx.engine->Resources();
    crossrender::Font* font = ctx.engine->DefaultFont();
    ENG_LOGI("game", "очки: %d, шрифт: %s", score, (font != nullptr) ? "есть" : "нет");
    (void)res;
}
```

### `SceneContext::scenes`

Указатель на `SceneManager`: смена сцены, стек, запросы о текущем состоянии.
Это предпочтительный способ перейти на другой экран — сцена не должна сама
владеть соседними сценами.

```cpp
void ToMenu(crossrender::SceneContext& ctx) {
    crossrender::TransitionDesc t;
    t.type = crossrender::TransitionType::Fade;
    t.duration = 0.3f;
    ctx.scenes->GoTo("menu", t);
}
```

### `SceneContext::r2d`

Указатель на `Renderer2D`. Между `BeginFrame` и `EndFrame` кадра сцены он
уже открыт движком, поэтому сцена просто рисует: `FillRect`, `DrawText`,
пути, изображения, виджеты через `ui`.

```cpp
void DrawHealthBar(crossrender::SceneContext& ctx, crossrender::f32 ratio) {
    crossrender::Renderer2D& r2d = *ctx.r2d;
    const crossrender::Rect back{ctx.viewport.x + 20.0f, ctx.viewport.y + 20.0f, 240.0f, 18.0f};
    r2d.FillRoundedRect(back, 9.0f, crossrender::Color{0.08f, 0.09f, 0.12f, 0.9f});
    crossrender::Rect fill = back;
    fill.w *= crossrender::Clamp(ratio, 0.0f, 1.0f);
    r2d.FillRoundedRect(fill, 9.0f, crossrender::Color::FromRGB(0x3FBF7F));
}
```

### `SceneContext::r3d`

Указатель на `Renderer3D` — доступен всегда, но реально используется только
в `Prepare3D` (настройка камеры) и `Render3D`. Движок уже открыл 3D-проход,
поэтому сцена не вызывает `r3d.BeginFrame`.

```cpp
void On3D(crossrender::SceneContext& ctx, const crossrender::Mesh& mesh, const crossrender::Material& mat) {
    ctx.r3d->Draw(mesh, mat, crossrender::Mat4::Identity());
    ctx.r3d->DrawGrid(20.0f, 20);
}
```

### `SceneContext::ui`

Указатель на `UiContext` — виджеты, тема, состояние ввода интерфейса.
Кадр UI тоже открыт движком; сцена рисует виджеты поверх 2D-слоя. Полезно
спрашивать `ui->WantsMouse()`, чтобы не вращать камеру, когда курсор над
панелью.

```cpp
void OnUI(crossrender::SceneContext& ctx, const crossrender::Rect& content) {
    crossrender::UiContext& ui = *ctx.ui;
    const bool overUi = ui.WantsMouse();
    if (ui.Button("Начать", {content.x + 20.0f, content.y + 20.0f, 160.0f, 36.0f})) {
        ctx.scenes->SetScene("game");
    }
    (void)overUi;
}
```

### `SceneContext::viewport`

Логический прямоугольник кадра в независимых от DPI единицах, **уже с
учётом безопасной зоны** (`SafeArea::Query()` из `crossrender/ui/Ui.h`) и retro-режима.
Это основная сетка координат для раскладки: не считайте размеры от размера
окна. В retro-режиме вьюпорт равен виртуальному разрешению, а не размеру
окна.

```cpp
void Layout(crossrender::SceneContext& ctx) {
    const crossrender::Rect v = ctx.viewport;
    const crossrender::Rect header{v.x, v.y, v.w, 56.0f};
    const crossrender::Rect body{v.x + 12.0f, header.Bottom() + 12.0f, v.w - 24.0f, v.h - 80.0f};
    ENG_LOGI("demo", "шапка %.0fx%.0f, тело %.0fx%.0f", header.w, header.h, body.w, body.h);
}
```

### `SceneContext::dpiScale`

Множитель перевода логических единиц в физические пиксели. Нужен, когда
размер задаётся в пикселях (размер рендер-таргета, толщина в физических
точках). В retro-режиме движок выставляет его в `1.0`.

```cpp
void ResizeHdrTarget(crossrender::SceneContext& ctx, crossrender::RenderTarget& target) {
    const int w = static_cast<int>(ctx.viewport.w * ctx.dpiScale);
    const int h = static_cast<int>(ctx.viewport.h * ctx.dpiScale);
    if (target.Width() != w || target.Height() != h) target.Resize(w, h);
}
```

### `SceneContext::dt`

Дельта времени текущего кадра в секундах, уже ограниченная
`EngineConfig::maxDeltaTime` и обнулённая, если приложение на паузе
(`Engine::SetPaused(true)`). Тот же `dt` приходит и в `Scene::Update`.

```cpp
void Spin(crossrender::SceneContext& ctx, crossrender::f32& angle) {
    angle += 1.5f * ctx.dt;   // 1.5 радиана в секунду
    if (ctx.dt <= 0.0f) ENG_LOGD("demo", "приложение на паузе, время не идёт");
}
```

### `Scene() / ~Scene()`

Конструктор по умолчанию и виртуальный деструктор. Копирование запрещено:
сцена существует в единственном экземпляре и принадлежит `SceneManager`,
поэтому её состояние можно хранить прямо в полях, не боясь копий.
Деструктор срабатывает при замене сцены или в `SceneManager::Reset()`.

```cpp
class InventoryScene final : public crossrender::Scene {
public:
    InventoryScene() = default;
    ~InventoryScene() override { ENG_LOGI("demo", "сцена инвентаря выгружена"); }

private:
    std::vector<crossrender::Texture*> icons_;
};
```

### `const char* Name() const`

Единственный чисто виртуальный метод: возвращает постоянное имя сцены.
Указатель должен жить всё время работы сцены (обычно это строковый литерал).
Именно `Name()` попадает в `SceneManager::CurrentName()` и в лог «entered …»,
а не ключ, под которым сцена зарегистрирована.

```cpp
class SettingsScene final : public crossrender::Scene {
public:
    [[nodiscard]] const char* Name() const override { return "settings"; }
};

SettingsScene scene;
ENG_LOGI("demo", "имя сцены: %s", scene.Name());
```

### `void OnEnter(SceneContext& ctx)`

Вызывается, когда сцена стала активной. К этому моменту контекст уже
заполнен, но 3D-проход ещё не начинался, поэтому здесь удобно грузить
ресурсы, строить меши и сбрасывать состояние. Сразу после `OnEnter` движок
вызывает `OnResize(ctx, viewport.w, viewport.h)`.

```cpp
void OnEnter(crossrender::SceneContext& ctx) override {
    font_ = ctx.engine->DefaultFont();
    music_ = ctx.engine->Resources().Audio_("audio/level1.ogg");
    if (music_ != nullptr) crossrender::Audio::Get().PlayMusic(*music_, 0.6f, true);
}
```

### `void OnExit(SceneContext& ctx)`

Вызывается перед тем, как сцену заменят другой или снимут со стека. Здесь
останавливают музыку, отписываются от событий и освобождают то, чем сцена
владеет сама. Объект сцены после этого ещё жив: `SceneManager` держит его в
`previous_` до следующей смены.

```cpp
void OnExit(crossrender::SceneContext& ctx) override {
    crossrender::Audio::Get().StopAll(1.0f);   // плавно гасим все голоса сцены
    ENG_LOGI("demo", "покидаем '%s'", Name());
    (void)ctx;
}
```

### `void Update(SceneContext& ctx, f32 dt)`

Главный покадровый хук: игровая логика, чтение ввода, таймеры. `dt` — уже
масштабированный (на паузе равен нулю) и ограниченный сверху интервал.
Вызывается один раз за кадр; физику с фиксированным шагом стройте на
`Engine::GetClock().ConsumeFixedSteps()`.

```cpp
void Update(crossrender::SceneContext& ctx, crossrender::f32 dt) override {
    const crossrender::Input& input = ctx.engine->GetInput();
    if (input.KeyPressed(crossrender::Key::Space)) Jump();
    velocity_ += gravity_ * dt;
    position_ += velocity_ * dt;
    if (input.KeyPressed(crossrender::Key::Escape)) ctx.scenes->Pop();
}
```

### `void Prepare3D(SceneContext& ctx)`

Отдельный хук перед 3D-проходом — он существует именно для того, чтобы
сцена успела выставить камеру, свет и окружение **для этого кадра**, уже
после `Update`. Если положиться только на `Render3D`, настройка камеры тоже
сработает, но часть подсистем рендерера (тени, отсечение) читает её раньше —
поэтому камеру ставьте здесь.

```cpp
void Prepare3D(crossrender::SceneContext& ctx) override {
    camera_.position = target_ + offset_;
    camera_.target = target_;
    ctx.r3d->SetCamera(camera_);
    ctx.r3d->SetEnvironment(env_);
}
```

### `void Render3D(SceneContext& ctx)`

Рисование 3D-мира. Вызывается только при `Wants3D() == true`, внутри уже
открытого 3D-прохода; `BeginFrame`/`EndFrame` рендерера сцена не трогает.
Если сцена включает постобработку, этот метод рисует в HDR-таргет, а не
прямо на экран.

```cpp
void Render3D(crossrender::SceneContext& ctx) override {
    for (const Enemy& e : enemies_) {
        ctx.r3d->Draw(e.mesh, e.material, crossrender::Mat4::Translate(e.position));
    }
    ctx.r3d->DrawGrid(40.0f, 40);
}
```

### `void Render2D(SceneContext& ctx)`

Рисование 2D и интерфейса. Вызывается **всегда**, в том числе у 3D-сцены —
там это слой поверх мира. Кадр `Renderer2D` уже открыт движком, а `ui`
готов к виджетам.

```cpp
void Render2D(crossrender::SceneContext& ctx) override {
    crossrender::Renderer2D& r2d = *ctx.r2d;
    crossrender::Font* font = ctx.engine->DefaultFont();
    if (font == nullptr) return;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "HP %d", hp_);
    r2d.DrawText(*font, buf, ctx.viewport.x + 20.0f, ctx.viewport.y + 20.0f,
                 crossrender::Color::White, 20.0f);
}
```

### `void PostFrame(SceneContext& ctx, f32 dt)`

Зарезервированный хук «после показа кадра» — в текущей реализации движка он
**не вызывается ни разу**: ни `Engine::Step`, ни `SceneManager` его не
вызывают. Не полагайтесь на него; отложенную работу делайте в конце
`Render2D` или в `Update` следующего кадра.

```cpp
// Хук объявлен, но движок его не вызывает: код здесь не выполнится.
struct NeverCalledScene final : crossrender::Scene {
    [[nodiscard]] const char* Name() const override { return "never"; }
    void PostFrame(crossrender::SceneContext& ctx, crossrender::f32 dt) override {
        ENG_LOGW("demo", "эта строка не появится: PostFrame никто не вызывает");
        (void)ctx;
        (void)dt;
    }
};
```

### `void OnResize(SceneContext& ctx, int width, int height)`

Сообщает о смене размера вьюпорта. Движок вызывает его один раз при входе в
сцену с текущими `viewport.w`/`viewport.h`, а `SceneManager::Resize()` — при
явном вызове из вашего кода. Автоматической доставки ресайза окна нет,
поэтому для адаптивной раскладки надёжнее читать `ctx.viewport` каждый кадр.

```cpp
void OnResize(crossrender::SceneContext& ctx, int width, int height) override {
    (void)ctx;
    layoutDirty_ = true;
    ENG_LOGI("demo", "вьюпорт стал %dx%d", width, height);
}
```

### `bool OnBackPressed(SceneContext& ctx)`

Реакция на системную кнопку «назад». Возврат `true` означает «событие
обработано, стек не трогать»; `false` — «менеджер, разбирайся сам», и тогда
`SceneManager::Back()` снимает верхнюю сцену со стека. Движок этот метод сам
не вызывает: его вызывает `SceneManager::Back()`, а `Back()` — платформенный
код или ваша обработка `Escape`.

```cpp
bool OnBackPressed(crossrender::SceneContext& ctx) override {
    if (dialogOpen_) {
        dialogOpen_ = false;      // закрыли диалог — наверх не уходим
        return true;
    }
    (void)ctx;
    return false;                 // пусть SceneManager снимет сцену со стека
}
```

### `void OnPause(SceneContext& ctx)`

Вызывается, когда поверх этой сцены выдвинули другую через
`SceneManager::Push`. Сцена остаётся жива и лежит в стеке, но её `Update`
больше не вызывается. Здесь останавливают музыку и таймеры, чтобы после
возврата не «прыгнуло» время.

```cpp
void OnPause(crossrender::SceneContext& ctx) override {
    paused_ = true;
    if (music_ != crossrender::kInvalidVoice) crossrender::Audio::Get().Pause(music_);
    (void)ctx;
}
```

### `void OnResume(SceneContext& ctx)`

Парный к `OnPause`: вызывается, когда сцену вернули со стека через `Pop`
или `PopTo`. К этому моменту `ctx.viewport` уже актуален — удобно заново
разложить интерфейс.

```cpp
void OnResume(crossrender::SceneContext& ctx) override {
    paused_ = false;
    screen_ = ctx.viewport;
    if (music_ != crossrender::kInvalidVoice) crossrender::Audio::Get().Resume(music_);
}
```

### `bool Wants3D() const`

Сообщает движку, нужен ли сцене 3D-проход. `false` по умолчанию — тогда
`Prepare3D` и `Render3D` не вызываются, а кадр очищается цветом
`ClearColor()`. От этого флага зависят и постобработка, и способ очистки
экрана.

```cpp
class Viewer3D final : public crossrender::Scene {
public:
    [[nodiscard]] const char* Name() const override { return "viewer"; }
    [[nodiscard]] bool Wants3D() const override { return true; }
    void Render3D(crossrender::SceneContext& ctx) override { (void)ctx; }
};
ENG_LOGI("demo", "3D включён: %s", Viewer3D{}.Wants3D() ? "да" : "нет");
```

### `bool WantsPostProcessing() const`

Просит движок прогнать HDR-цепочку bloom/tonemap поверх 3D-прохода.
Работает только вместе с `Wants3D() == true` и включённым
`EngineConfig::enablePostProcessing`; настройки берутся из
`Engine::PostSettings()`, а сам `PostProcessor` сцена обязана
инициализировать. Для 2D-сцены эффект не включится.

```cpp
[[nodiscard]] bool WantsPostProcessing() const override { return true; }

void OnEnter(crossrender::SceneContext& ctx) override {
    crossrender::PostProcessor* post = ctx.engine->Post();
    if (post != nullptr && !post->Valid()) post->Init(1280, 720);
    ctx.engine->PostSettings().bloomIntensity = 0.8f;
}
```

### `bool WantsUiCapture() const`

Зарезервированный запрос «захватить UI в отдельную текстуру»: объявлен, но
текущей реализацией движка **не читается**. Возврат `true` ничего не меняет.

```cpp
// Флаг объявлен, но движок его не проверяет.
class CaptureWishScene final : public crossrender::Scene {
public:
    [[nodiscard]] const char* Name() const override { return "capture-wish"; }
    [[nodiscard]] bool WantsUiCapture() const override { return true; }   // ни на что не влияет
};
```

### `Color ClearColor() const`

Цвет фона кадра. Движок использует его, когда сам очищает цель кадра:
при `Wants3D() == false`, а также при очистке 3D-цели и виртуального
retro-буфера. Значение по умолчанию — тёмно-синий `0x0E1016FF`.

```cpp
[[nodiscard]] crossrender::Color ClearColor() const override {
    return crossrender::Color::FromARGB(0xFF1A0E12);   // тёплый фон для сцены лавы
}
```

### `bool CustomClear() const`

Задумывался как разрешение сцене очистить кадр самостоятельно, но в текущей
реализации движок его **не проверяет**: очистка всегда выполняется цветом
`ClearColor()`. Если нужен свой фон — рисуйте его первым делом в `Render2D`.

```cpp
[[nodiscard]] bool CustomClear() const override { return true; }   // не действует

void Render2D(crossrender::SceneContext& ctx) override {
    // Свой фон рисуем сами: CustomClear() движок игнорирует.
    ctx.r2d->FillRect(ctx.viewport, crossrender::Color::Black);
}
```

### `const char* Description() const`

Человекочитаемое описание сцены для списка сцен в примере
(`mega::SceneInfo::description`). На работу движка не влияет, но полезно:
`--list` печатает именно его.

```cpp
[[nodiscard]] const char* Description() const override {
    return "Орбитальная камера, тени и постобработка";
}
```

### `enum class TransitionType : u8`

Вид анимации перехода между сценами. `Fade`, слайды и `CircleWipe` рисуют
закрывающий оверлей цветом `TransitionDesc::color`; в первой половине
перехода экран закрывается, во второй — открывается, а сама смена сцен
происходит ровно в середине.

| Значение | Что рисует |
|---|---|
| `TransitionType::None` | ничего — мгновенная смена (по умолчанию) |
| `TransitionType::Fade` | затемнение цветом `color` с прозрачностью по прогрессу |
| `TransitionType::SlideLeft` | шторка въезжает справа и движется влево |
| `TransitionType::SlideRight` | шторка въезжает слева и движется вправо |
| `TransitionType::SlideUp` | шторка въезжает снизу и движется вверх |
| `TransitionType::SlideDown` | шторка въезжает сверху и движется вниз |
| `TransitionType::CircleWipe` | круг из центра; при почти полном радиусе добиваются углы |
| `TransitionType::CrossDissolve` | то же, что `Fade`, но максимум альфы — `0.85` |

```cpp
crossrender::TransitionDesc slide;
slide.type = crossrender::TransitionType::SlideUp;
slide.duration = 0.25f;
slide.color = crossrender::Color::FromARGB(0xFF0E1016);
ctx.scenes->GoTo("level-2", slide);
```

### `TransitionType TransitionDesc::type`

Вид перехода. Значение по умолчанию — `TransitionType::None`, поэтому
программные `GoTo`/`Push`/`Pop` без аргументов срабатывают синхронно.

```cpp
crossrender::TransitionDesc instant;   // type == None
ENG_ASSERT(instant.type == crossrender::TransitionType::None);
ctx.scenes->GoTo("game", instant);   // переключение произойдёт сразу
```

### `f32 TransitionDesc::duration`

Длительность перехода в секундах, по умолчанию `0.35`. `duration <= 0`
равносильна `TransitionType::None` — смена происходит синхронно. Половина
длительности — это момент подмены сцены.

```cpp
crossrender::TransitionDesc slow;
slow.type = crossrender::TransitionType::Fade;
slow.duration = 1.2f;      // долгий кинематографичный переход
ctx.scenes->GoTo("ending", slow);
```

### `Color TransitionDesc::color`

Цвет закрывающего оверлея (по умолчанию чёрный непрозрачный). Для `Fade` и
`CrossDissolve` он задаёт цвет затемнения, для слайдов и `CircleWipe` — цвет
самой шторки. Альфа исходного цвета не важна: движок подставляет её сам.

```cpp
crossrender::TransitionDesc white;
white.type = crossrender::TransitionType::Fade;
white.color = crossrender::Color::White;   // вспышка при переходе
ctx.scenes->SetScene("flash");
```

### `bool TransitionDesc::crossFade`

Поле объявлено, но текущей реализацией **не используется**: одновременная
отрисовка двух сцен не поддерживается, и `SceneManager::RenderTransition`
его не читает. Полагаться на него нельзя.

```cpp
crossrender::TransitionDesc t;
t.type = crossrender::TransitionType::CrossDissolve;
t.crossFade = true;   // не действует; эффект даёт сам тип CrossDissolve
ctx.scenes->GoTo("menu", t);
```

### `SceneManager() / ~SceneManager()`

Конструктор по умолчанию создаёт пустой менеджер без контекста и фабрик;
деструктор уничтожает все сцены и отложенный переход. Экземпляр менеджера
принадлежит `Engine` и доступен через `Engine::Scenes()` — заводить свой
менеджер нужно только в тестах.

```cpp
crossrender::SceneManager manager;
manager.Register("menu", [] { return std::make_unique<MenuScene>(); });
ENG_LOGI("demo", "менеджер создан, сцен: %d",
         static_cast<int>(manager.RegisteredNames().size()));
```

### `void SetContext(SceneContext* ctx)`

Привязывает контекст, который менеджер передаёт во все хуки сцен. `Engine`
делает это в `Engine::Init`, поэтому в обычном приложении вызывать не нужно.
Без контекста менеджер не сможет вызвать `OnEnter`/`Update`/`Render2D`:
он просто ничего не сделает.

```cpp
crossrender::SceneContext ctx;
ctx.engine = &engine;
ctx.scenes = &manager;
manager.SetContext(&ctx);
```

### `void Reset()`

Полностью очищает менеджер: текущую и предыдущую сцены, стек, фабрики и
переход. Вызывается движком в `Engine::Shutdown()`, чтобы сцены уничтожились
до рендереров. После `Reset()` имена нужно регистрировать заново.

```cpp
void OnHotReload(crossrender::SceneManager& scenes) {
    scenes.Reset();
    RegisterAllScenes(scenes);   // фабрики тоже удалены — регистрируем снова
}
```

### `void SetRenderer2D(Renderer2D* r)`

Передаёт менеджеру 2D-рендерер, который нужен `RenderTransition` для
рисования оверлея. `Engine` вызывает его в `Engine::Init`. Если не задать
указатель, переходы просто не будут рисоваться (смена сцен произойдёт).

```cpp
crossrender::SceneManager manager;
manager.SetRenderer2D(&engine.R2D());
const bool ok = manager.GoTo("menu", crossrender::TransitionDesc{});
ENG_LOGI("demo", "переход начат: %s", ok ? "да" : "нет");
```

### `void Register(const std::string& name, std::function<std::unique_ptr<Scene>()> factory)`

Регистрирует фабрику сцены под именем. Именно по этому имени работают
`SetScene(name)`, `GoTo`, `Push(name)`, `Exists` и `RegisteredNames`.
Повторная регистрация того же имени молча заменяет фабрику. Одна фабрика
создаёт новый экземпляр сцены при каждом входе — состояние между входами не
сохраняется.

```cpp
engine.Scenes().Register("menu", [] { return std::make_unique<MenuScene>(); });
engine.Scenes().Register("game", [] { return std::make_unique<GameScene>(); });
ENG_LOGI("demo", "известные сцены: %d",
         static_cast<int>(engine.Scenes().RegisteredNames().size()));
```

### `bool SetScene(const std::string& name)`

Немедленная смена сцены по имени: старая получает `OnExit`, новая —
`OnEnter` и `OnResize`. Возвращает `false`, если имя не зарегистрировано или
фабрика вернула `nullptr`. Это синхронный вызов: он не ждёт конца кадра.

```cpp
if (!engine.Scenes().SetScene("game")) {
    ENG_LOGE("demo", "сцена 'game' не зарегистрирована");
}
```

### `bool SetScene(std::unique_ptr<Scene> scene)`

То же самое, но менеджер забирает готовый объект, а не создаёт его по
фабрике. Так удобно передавать сцене параметры через конструктор. Такая
сцена **не** попадает в список имён: `Exists("…")` про неё не знает, а
`CurrentName()` вернёт её собственное `Name()`.

```cpp
auto level = std::make_unique<LevelScene>(7, /*seed=*/42);
if (!engine.Scenes().SetScene(std::move(level))) {
    ENG_LOGE("demo", "не удалось открыть уровень");
}
```

### `bool GoTo(const std::string& name, const TransitionDesc& t = {})`

Смена сцены по имени, возможно с анимацией. Без аргумента перехода (или при
`type == None`, `duration <= 0`) это просто `SetScene`. С переходом вызов
только запускает анимацию; подмена произойдёт в середине, а `false`
возвращается, если имени нет среди зарегистрированных.

```cpp
crossrender::TransitionDesc t;
t.type = crossrender::TransitionType::Fade;
t.duration = 0.4f;
if (!ctx.scenes->GoTo("victory", t)) {
    ENG_LOGW("demo", "сцены 'victory' нет, остаёмся на месте");
}
```

### `bool Push(const std::string& name, const TransitionDesc& t = {})`

Кладёт сцену поверх текущей: текущая получает `OnPause` и уходит в стек,
новая — `OnEnter`. Возвращает `false`, если имя не зарегистрировано.
Идеально для паузы, диалога или инвентаря — нижняя сцена не уничтожается.

```cpp
crossrender::TransitionDesc t;
t.type = crossrender::TransitionType::Fade;
t.duration = 0.2f;
if (!ctx.scenes->Push("pause", t)) {
    ENG_LOGW("demo", "сцена паузы недоступна");
}
```

### `void Push(std::unique_ptr<Scene> scene)`

Перегрузка для готового объекта: приостанавливает текущую сцену и делает
новую активной. Возвращаемого значения нет, но `nullptr` игнорируется.
`OnResume` вернувшейся сцены дождётся `Pop`.

```cpp
auto dialog = std::make_unique<ConfirmDialog>("Удалить сохранение?");
ctx.scenes->Push(std::move(dialog));
```

### `bool Pop(const TransitionDesc& t = {})`

Снимает верхнюю сцену со стека: она получает `OnExit`, а сцена под ней —
`OnResume`. Возвращает `false`, если стек пуст (снимать нечего). С
переходом операция выполняется в середине анимации.

```cpp
void OnCancel(crossrender::SceneContext& ctx) {
    if (!ctx.scenes->Pop()) ENG_LOGW("demo", "стек пуст — выходить некуда");
}
```

### `bool PopTo(const std::string& name)`

Снимает сцены со стека, пока активной не станет сцена с указанным именем.
Снятые получают `OnExit`, целевая — `OnResume`. Возвращает `true`, только
если имя действительно нашлось; иначе стек будет опустошён, а активной
останется самая нижняя сцена. Если имя не найдено, `OnExit` для финальной
сцены не вызывается.

```cpp
// Из глубокого меню сразу назад к игре, минуя промежуточные экраны.
if (!ctx.scenes->PopTo("game")) {
    ENG_LOGW("demo", "сцена 'game' не найдена в стеке");
}
```

### `Scene* Current() const`

Активная сцена или `nullptr`, если ни одна ещё не установлена. В `Update`
возвращает уже новую сцену, если в этом кадре произошла смена в середине
перехода. Указатель принадлежит менеджеру — не удаляйте и не храните его
дольше, чем нужно.

```cpp
crossrender::Scene* scene = engine.Scenes().Current();
if (scene == nullptr) {
    ENG_LOGW("demo", "активной сцены нет");
} else {
    ENG_LOGI("demo", "активна сцена '%s'", scene->Name());
}
```

### `Scene* Previous() const`

Предыдущая сцена: та, что была заменена через `SetScene`/`GoTo`, либо
снятая через `Pop`. Жива до следующей смены. В рендере не участвует —
`crossFade` не реализован, — но полезна для возврата состояния или
диагностики.

```cpp
crossrender::Scene* prev = engine.Scenes().Previous();
if (prev != nullptr) ENG_LOGI("demo", "пришли из сцены '%s'", prev->Name());
```

### `const std::string& CurrentName() const`

Имя активной сцены, взятое из её `Name()` в момент входа. Пустая строка,
если сцены нет. Это же имя попадает в `EngineStats::sceneName`.

```cpp
if (engine.Scenes().CurrentName() == "menu") {
    ENG_LOGI("demo", "мы в главном меню");
}
```

### `int StackDepth() const`

Число сцен **под** активной. `0` означает, что `Pop` вернёт `false`.
Счётчик увеличивается только через `Push` и уменьшается через `Pop`/`PopTo`.

```cpp
ENG_LOGI("demo", "глубина стека: %d", engine.Scenes().StackDepth());
if (engine.Scenes().StackDepth() == 0) ENG_LOGW("demo", "Esc ничего не закроет: стек пуст");
```

### `bool Transitioning() const`

Истинно, пока проигрывается анимация перехода. Полезно, чтобы не запускать
второй переход или не читать ввод.

```cpp
if (!ctx.scenes->Transitioning()) {
    ctx.scenes->GoTo("next");
} else {
    ENG_LOGD("demo", "переход уже идёт: %.0f%%", ctx.scenes->TransitionProgress() * 100.0f);
}
```

### `f32 TransitionProgress() const`

Прогресс перехода от `0` до `1`. Когда перехода нет, возвращает `1`.
Движок использует значение для отрисовки оверлея, а сцена — например, для
синхронной анимации камеры.

```cpp
const crossrender::f32 p = ctx.scenes->TransitionProgress();
// Плавно поднимаем камеру, пока экран закрыт переходом.
camera_.position.y = 2.0f + (8.0f - 2.0f) * p;
```

### `bool InputBlocked() const`

Истинно, пока экран ещё не закрыт переходом (`TransitionProgress() < 0.5`).
В этом окне ввод лучше игнорировать: сцена вот-вот сменится. Вторая половина
перехода ввод не блокирует.

```cpp
void Update(crossrender::SceneContext& ctx, crossrender::f32 dt) override {
    if (ctx.scenes->InputBlocked()) return;   // сцена меняется, ввод не читаем
    if (ctx.engine->GetInput().KeyPressed(crossrender::Key::Space)) Jump();
    (void)dt;
}
```

### `void Update(f32 dt)`

Покадровый драйвер менеджера, который вызывает `Engine::Step`. Сначала
продвигает таймер перехода (и в середине выполняет отложенную смену), затем
вызывает `Scene::Update` активной сцены. Именно поэтому смена сцены «внутри
перехода» происходит на кадре, а не мгновенно.

```cpp
// Ядро одного кадра в Engine::Step (упрощённо).
engine.Scenes().Update(dt);        // переход + Scene::Update
engine.Scenes().Prepare3D();       // только если Wants3D()
engine.Scenes().Render3D();
engine.Scenes().Render2D();        // всегда
```

### `void Prepare3D()`

Вызывает `Scene::Prepare3D` активной сцены, но **только** если она вернула
`true` из `Wants3D()`. Движок дёргает этот метод между `Update` и 3D-проходом
— раньше, чем камера понадобится теням и отсечению.

```cpp
// Порядок, который гарантирует Engine::Step для 3D-сцены.
scenes.Update(dt);
if (engine.Scenes().Current() != nullptr && engine.Scenes().Current()->Wants3D()) {
    scenes.Prepare3D();   // камера/свет на этот кадр
    scenes.Render3D();
}
```

### `void Render3D()`

Вызывает `Scene::Render3D` активной сцены, тоже только при `Wants3D()`.
Рендерер уже настроен на нужную цель кадра (обычную или HDR для
постобработки), поэтому сцена просто рисует.

```cpp
void Render3D() {
    if (ctx_ == nullptr || current_ == nullptr) return;
    if (current_->Wants3D()) current_->Render3D(*ctx_);
}
```

### `void Render2D()`

Вызывает `Scene::Render2D` активной сцены **без** проверки `Wants3D()`:
2D-слой есть у каждой сцены и служит оверлеем для 3D. Вызывается после
`ui.BeginFrame`, поэтому виджеты внутри `Render2D` работают.

```cpp
// Даже 3D-сцене Render2D вызывается: это её слой интерфейса.
scenes.Render2D();
if (engine.onOverlay) engine.onOverlay(engine);   // глобальные оверлеи приложения
```

### `void RenderTransition(Renderer2D& r2d, const Rect& screen)`

Рисует оверлей активного перехода в заданном прямоугольнике (обычно
`Engine::Viewport()`). Движок вызывает его после `Scene::Render2D`, внутри
уже открытого кадра `Renderer2D`. Если перехода нет — метод ничего не
делает.

```cpp
void LateFrame(crossrender::SceneManager& scenes, crossrender::Renderer2D& r2d, const crossrender::Rect& screen) {
    // Свой 2D-проход завершаем оверлеем перехода, как это делает движок.
    scenes.RenderTransition(r2d, screen);
}
```

### `void Resize(int w, int h)`

Передаёт новый размер в `Scene::OnResize` активной сцены. В текущей
реализации `Engine` его не вызывает, поэтому о ресайзе окна сцена узнаёт
только сравнивая `ctx.viewport`; метод пригодится, если вы ведёте размеры
сами (например, при ручном управлении вьюпортом).

```cpp
// Явная доставка нового размера, если вы считаете его сами.
engine.Scenes().Resize(1920, 1080);
```

### `bool Back()`

Обрабатывает системную кнопку «назад». Сначала спрашивает активную сцену
(`OnBackPressed`); если та вернула `true` — событие поглощено. Иначе снимает
верхнюю сцену со стека (`Pop`) и возвращает `true`. `false` означает, что
обрабатывать нечего: сцена отказалась и стек пуст.

```cpp
// Платформенный колбэк «назад» из вашего кода (движок его не вызывает).
if (!engine.Scenes().Back()) {
    engine.Quit();   // выходить из приложения больше некуда
}
```

### `bool Exists(const std::string& name) const`

Проверяет, зарегистрирована ли фабрика с таким именем. Сцены, переданные
готовыми объектами через `SetScene(unique_ptr)`/`Push(unique_ptr)`, по имени
не находятся — для них метод вернёт `false`, даже если сцена активна.

```cpp
if (engine.Scenes().Exists("game")) {
    engine.Scenes().GoTo("game");
} else {
    ENG_LOGW("demo", "сцена 'game' не зарегистрирована");
}
```

### `std::vector<std::string> RegisteredNames() const`

Возвращает имена всех зарегистрированных фабрик, отсортированные по
алфавиту. Удобно для отладочного списка или экрана выбора сцены.

```cpp
for (const std::string& name : engine.Scenes().RegisteredNames()) {
    ENG_LOGI("demo", "  сцена: %s", name.c_str());
}
```

## Пример целиком

```cpp
#include "crossrender/Engine.h"
#include "crossrender/scene/Scene.h"
#include "crossrender/core/Log.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

// Игра из двух экранов: меню и уровень с паузой. Показывает регистрацию
// фабрик, переход с анимацией, стек сцен и возврат по Esc.
class MenuScene final : public crossrender::Scene {
public:
    [[nodiscard]] const char* Name() const override { return "menu"; }
    [[nodiscard]] const char* Description() const override { return "Главное меню"; }

    void Render2D(crossrender::SceneContext& ctx) override {
        crossrender::Font* font = ctx.engine->DefaultFont();
        if (font == nullptr) return;
        const crossrender::Rect v = ctx.viewport;
        ctx.r2d->DrawText(*font, "ГЛАВНОЕ МЕНЮ", v.Center().x, v.y + 80.0f, crossrender::Color::White, 40.0f,
                          crossrender::TextAlign::Center, crossrender::TextBaseline::Top);
        if (ctx.ui->Button("Играть", {v.Center().x - 90.0f, v.Center().y, 180.0f, 44.0f})) {
            StartGame(ctx);
        }
    }

private:
    static void StartGame(crossrender::SceneContext& ctx) {
        crossrender::TransitionDesc t;
        t.type = crossrender::TransitionType::Fade;
        t.duration = 0.35f;
        ctx.scenes->GoTo("level", t);
    }
};

class LevelScene final : public crossrender::Scene {
public:
    [[nodiscard]] const char* Name() const override { return "level"; }

    void OnEnter(crossrender::SceneContext& ctx) override {
        score_ = 0;
        ENG_LOGI("level", "уровень открыт, вьюпорт %.0fx%.0f", ctx.viewport.w, ctx.viewport.h);
    }

    void OnPause(crossrender::SceneContext& ctx) override {
        paused_ = true;
        (void)ctx;
    }

    void OnResume(crossrender::SceneContext& ctx) override {
        paused_ = false;
        screen_ = ctx.viewport;   // раскладку пересчитываем на актуальный вьюпорт
    }

    void Update(crossrender::SceneContext& ctx, crossrender::f32 dt) override {
        if (ctx.scenes->InputBlocked()) return;
        const crossrender::Input& input = ctx.engine->GetInput();
        score_ += static_cast<int>(dt * 10.0f);
        if (input.KeyPressed(crossrender::Key::Escape)) {
            ctx.scenes->Push("pause");        // Esc открывает паузу поверх уровня
        }
    }

    void Render2D(crossrender::SceneContext& ctx) override {
        crossrender::Font* font = ctx.engine->DefaultFont();
        if (font == nullptr) return;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Очки: %d", score_);
        ctx.r2d->DrawText(*font, buf, ctx.viewport.x + 20.0f, ctx.viewport.y + 20.0f,
                          crossrender::Color::White, 22.0f);
    }

private:
    int score_ = 0;
    bool paused_ = false;
    crossrender::Rect screen_{};
};

class PauseScene final : public crossrender::Scene {
public:
    [[nodiscard]] const char* Name() const override { return "pause"; }

    void Render2D(crossrender::SceneContext& ctx) override {
        ctx.r2d->FillRect(ctx.viewport, crossrender::Color{0.0f, 0.0f, 0.0f, 0.55f});
        if (ctx.ui->Button("Продолжить", {ctx.viewport.Center().x - 90.0f,
                                          ctx.viewport.Center().y, 180.0f, 44.0f})) {
            ctx.scenes->Pop();               // уровень получит OnResume
        }
    }

    bool OnBackPressed(crossrender::SceneContext& ctx) override {
        (void)ctx;
        return false;                        // SceneManager::Back() снимет паузу
    }
};

int main() {
    crossrender::EngineConfig cfg;
    cfg.window.title = "Сцены";
    cfg.startScene = "menu";

    crossrender::Engine engine;
    if (!engine.Init(cfg)) return 1;

    engine.Scenes().Register("menu", [] { return std::make_unique<MenuScene>(); });
    engine.Scenes().Register("level", [] { return std::make_unique<LevelScene>(); });
    engine.Scenes().Register("pause", [] { return std::make_unique<PauseScene>(); });

    if (!engine.Scenes().SetScene(cfg.startScene)) {
        ENG_LOGE("demo", "нет стартовой сцены '%s'", cfg.startScene.c_str());
        return 1;
    }

    const int rc = engine.Run();
    engine.Shutdown();
    return rc;
}
```

## См. также

* `docs/Engine.md` — `Engine::Scenes()`, `Engine::Context()`, порядок кадра и
  постобработка, которой управляет сцена.
* `docs/gfx/Renderer2D.md` — чем сцена рисует интерфейс в `Render2D`.
* `docs/gfx/Renderer3D.md` — 3D-проход, камера и свет для `Prepare3D`.
* `docs/ui/Ui.md` — виджеты `UiContext` для `SceneContext::ui`; здесь же
  объявлена безопасная зона `SafeArea`, уже учтённая в `SceneContext::viewport`.
* `docs/gfx/FilterChain.md` — цепочка фильтров, которую включает
  `EngineConfig::enableFilters`.
* `docs/gfx/RenderTarget.md` — `PostProcessor` и `PostProcessSettings`,
  которые сцена заполняет через `Engine::PostSettings()`.
* `docs/core/Time.md` — `Clock`, фиксированный шаг и масштаб времени,
  попадающие в `SceneContext::dt`.
