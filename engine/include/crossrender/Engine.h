//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: фасад движка и главный цикл: владеет окном, контекстом и стеком сцен.
//
#pragma once

#include "crossrender/ui/Ui.h"
#include "crossrender/core/Base.h"
#include "crossrender/core/Time.h"
#include "crossrender/gfx/Retro.h"
#include "crossrender/text/Font.h"
#include "crossrender/audio/Audio.h"
#include "crossrender/scene/Scene.h"
#include "crossrender/gfx/Renderer2D.h"
#include "crossrender/gfx/Renderer3D.h"
#include "crossrender/gfx/FilterChain.h"
#include "crossrender/platform/Window.h"
#include "crossrender/gfx/RenderTarget.h"

#include <memory>
#include <string>
#include <vector>
#include <functional>

namespace crossrender {

struct EngineConfig {
    WindowDesc window;
    bool enable3D = true;
    bool enableAudio = true;
    bool enableUI = true;
    bool enablePostProcessing = false;
    bool enableDebugOverlay = false;
    bool enableHotReload = false;
    u32 randomSeed = 1337;
    std::string title = "CrossRender Example";
    std::string assetsPath;   // необязательное переопределение пути ассетов
    std::string startScene;   // имя сцены, открываемой первой
    f32 fixedTimeStep = 1.0f / 60.0f;
    f32 maxDeltaTime = 0.1f;
    int targetFps = 0;        // 0 = без ограничения (vsync всё равно действует)
    bool headless = false;    // без окна / offscreen GL-контекст (тесты, CI)
    RenderTargetDesc headlessTarget{};
    // Retro-дисплей (пиксель-арт / ASCII). RetroSettings::mode == Off отключает его.
    RetroSettings retro{};
    // Комбинируемые фильтры постобработки, применяемые после композитинга сцены.
    bool enableFilters = false;
    bool filtersDefaultEnabled = false;
};

struct EngineStats {
    f32 frameMs = 0;
    f32 cpuMs = 0;
    f32 gpuMs = 0;
    f32 fps = 0;
    int drawCalls = 0;
    int triangles = 0;
    int vertices2D = 0;
    u64 frame = 0;
    int sceneStack = 0;
    std::string sceneName;
    int activeVoices = 0;
};

class Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Создаёт платформенное окно, GL-контекст и все подсистемы.
    bool Init(const EngineConfig& config);
    void Shutdown();

    // Запускает блокирующий главный цикл (десктоп) или один шаг на мобильных/WASM.
    int Run();
    // Одна итерация, экспортирована для управляемых платформой циклов и тестов.
    void Step(f32 dt = -1.0f);
    // Запрашивает выход в конце текущего фрейма.
    void Quit() { quit_ = true; }
    [[nodiscard]] bool ShouldQuit() const { return quit_; }

    // ---- доступ к подсистемам --------------------------------------------
    // Возвращает нативное окно. В headless-режиме вместо разыменования null
    // возвращается отдельный, ни разу не создававшийся экземпляр Window, поэтому
    // код, запрашивающий ввод или размеры, не может упасть; для ветвления по
    // реальным возможностям платформы используйте Headless().
    [[nodiscard]] Window& GetWindow();
    [[nodiscard]] const Window& GetWindow() const;
    // Состояние ввода для текущего фрейма (реальное окно на десктопе,
    // внутренний буфер в headless-режиме).
    [[nodiscard]] Input& GetInput() { return window_ ? window_->GetInput() : headlessInput_; }
    [[nodiscard]] Renderer2D& R2D() { return r2d_; }
    [[nodiscard]] Renderer3D& R3D() { return r3d_; }
    [[nodiscard]] UiContext& UI() { return ui_; }
    [[nodiscard]] SceneManager& Scenes() { return scenes_; }
    [[nodiscard]] Audio& GetAudio() { return Audio::Get(); }
    [[nodiscard]] PostProcessor* Post() { return post_.get(); }
    // Настройки, используемые, когда активная сцена запрашивает постобработку
    // (Scene::WantsPostProcessing() == true). Сцены могут их изменять.
    [[nodiscard]] PostProcessSettings& PostSettings() { return postSettings_; }
    [[nodiscard]] const PostProcessSettings& PostSettings() const { return postSettings_; }
    // Пиксель-арт / ASCII дисплей. Всегда валиден; сначала проверьте RetroActive().
    [[nodiscard]] RetroDisplay& Retro() { return *retro_; }
    [[nodiscard]] RetroSettings& RetroCfg() { return config_.retro; }
    [[nodiscard]] const RetroSettings& RetroCfg() const { return config_.retro; }
    [[nodiscard]] bool RetroActive() const { return config_.retro.mode != RetroMode::Off; }
    [[nodiscard]] bool RetroInitialised() const { return retro_ != nullptr; }
    // Комбинируемая цепочка фильтров (null, если не задан EngineConfig::enableFilters).
    [[nodiscard]] FilterChain* Filters() { return filters_.get(); }
    [[nodiscard]] bool FiltersActive() const;
    [[nodiscard]] RenderTarget* SceneTarget() { return sceneTarget_.get(); }
    [[nodiscard]] Clock& GetClock() { return clock_; }
    [[nodiscard]] const EngineConfig& Config() const { return config_; }
    [[nodiscard]] const EngineStats& Stats() const { return stats_; }
    [[nodiscard]] SceneContext& Context() { return ctx_; }
    [[nodiscard]] bool Headless() const { return config_.headless; }
    // Логический прямоугольник экрана (независимый от DPI) с учётом safe area.
    [[nodiscard]] Rect Viewport() const { return viewport_; }
    [[nodiscard]] f32 DpiScale() const { return dpiScale_; }
    [[nodiscard]] Font* DefaultFont() const { return defaultFont_; }
    [[nodiscard]] Font* DefaultSdfFont() const { return defaultSdfFont_; }
    [[nodiscard]] class ResourceCache& Resources() { return *resources_; }

    // ---- хуки ------------------------------------------------------------
    // Вызывается каждый фрейм перед обновлением сцены (геймплей/глобальный ввод).
    std::function<void(Engine&, f32)> onUpdate;
    // Вызывается перед Render2D активной сцены (глобальные оверлеи).
    std::function<void(Engine&)> onOverlay;
    void SetSceneContextExtras(void* user) { user_ = user; }
    [[nodiscard]] void* UserData() const { return user_; }

    // Делает снимок экрана в папку user и возвращает записанный путь.
    std::string Screenshot(const std::string& filename = "");
    // Читает цвет пикселя из последнего отрисованного фрейма (для тестов).
    Color ReadPixel(int x, int y);

    // Глобальный переключатель debug-оверлея движка (F1).
    void ToggleDebugOverlay() { showDebugOverlay_ = !showDebugOverlay_; }
    [[nodiscard]] bool DebugOverlayVisible() const { return showDebugOverlay_; }

    // Масштаб времени уровня приложения (меню паузы).
    void SetPaused(bool paused) { paused_ = paused; }
    [[nodiscard]] bool Paused() const { return paused_; }

private:
    void BeginFrame();
    void EndFrame();
    void UpdateViewport();

    EngineConfig config_{};
    std::unique_ptr<Window> window_;
    Renderer2D r2d_;
    Renderer3D r3d_;
    UiContext ui_;
    SceneManager scenes_;
    Clock clock_;
    std::unique_ptr<PostProcessor> post_;
    PostProcessSettings postSettings_{};
    std::unique_ptr<RetroDisplay> retro_;
    std::unique_ptr<FilterChain> filters_;
    // Вспомогательный таргет для применения фильтров к retro-изображению.
    std::unique_ptr<RenderTarget> scratch_;
    std::unique_ptr<RenderTarget> sceneTarget_;
    // Offscreen-таргет для работы в headless-режиме (без нативного фреймбуфера).
    std::unique_ptr<RenderTarget> headlessTarget_;
    RenderTarget* EnsureHeadlessTarget(int w, int h);
    std::unique_ptr<class ResourceCache> resources_;
    Font* defaultFont_ = nullptr;
    Font* defaultSdfFont_ = nullptr;
    SceneContext ctx_{};
    EngineStats stats_{};
    Rect viewport_;
    f32 dpiScale_ = 1;
    bool quit_ = false, paused_ = false, showDebugOverlay_ = false, initialised_ = false;
    bool platformInitialised_ = false, glContextCreated_ = false, glFunctionsLoaded_ = false;
    void* user_ = nullptr;
    // Состояние ввода при работе в headless-режиме (без окна).
    Input headlessInput_;
    std::string screenshotCounter_;
};

// Вспомогательная точка входа для примеров: создаёт Engine с заданным конфигом,
// вызывает `setup` для регистрации сцен, затем запускает главный цикл.
int RunExample(const EngineConfig& config, const std::function<void(Engine&)>& setup);

}  // namespace crossrender
