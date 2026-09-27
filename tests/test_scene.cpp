// Интеграционные тесты сцен и движка (headless GL, где возможно).
#include "crossrender/Engine.h"
#include "crossrender/gfx/GL.h"
#include "crossrender/core/File.h"
#include "crossrender/test/Test.h"
#include "crossrender/scene/Scene.h"

#include <memory>

using namespace crossrender;

namespace {

struct TestSceneA : Scene {
    int entered = 0, exited = 0, updates = 0, draws2d = 0, draws3d = 0, resizes = 0;
    f32 lastDt = 0;
    bool want3D = false;
    const char* Name() const override { return "A"; }
    bool Wants3D() const override { return want3D; }
    void OnEnter(SceneContext&) override { ++entered; }
    void OnExit(SceneContext&) override { ++exited; }
    void Update(SceneContext&, f32 dt) override {
        ++updates;
        lastDt = dt;
    }
    void Render3D(SceneContext&) override { ++draws3d; }
    void Render2D(SceneContext&) override { ++draws2d; }
    void OnResize(SceneContext&, int, int) override { ++resizes; }
};

struct TestSceneB : Scene {
    const char* Name() const override { return "B"; }
};

}  // namespace

ENG_TEST(Scene, ManagerRegisterAndSwitch) {
    SceneManager mgr;
    SceneContext ctx;
    Renderer2D r2d;
    ctx.r2d = &r2d;
    mgr.SetContext(&ctx);

    TestSceneA* rawA = nullptr;
    mgr.Register("A", [&rawA]() {
        auto s = std::make_unique<TestSceneA>();
        rawA = s.get();
        return s;
    });
    mgr.Register("B", []() { return std::make_unique<TestSceneB>(); });

    ENG_CHECK(mgr.Exists("A"));
    ENG_CHECK(mgr.Exists("B"));
    ENG_CHECK(!mgr.Exists("Missing"));
    ENG_CHECK_EQ(mgr.RegisteredNames().size(), 2u);

    ENG_CHECK(mgr.SetScene("A"));
    ENG_CHECK_STR_EQ(mgr.CurrentName(), "A");
    ENG_CHECK(rawA != nullptr);
    ENG_CHECK_EQ(rawA->entered, 1);
    ENG_CHECK(mgr.Current() != nullptr);
    ENG_CHECK_STR_EQ(mgr.Current()->Name(), "A");

    mgr.Update(0.016f);
    ENG_CHECK_EQ(rawA->updates, 1);
    ENG_CHECK_NEAR(rawA->lastDt, 0.016f, 1e-6f);

    ENG_CHECK(mgr.SetScene("B"));
    ENG_CHECK_EQ(rawA->exited, 1);
    ENG_CHECK_STR_EQ(mgr.CurrentName(), "B");
    ENG_CHECK(!mgr.SetScene("Nope"));
}

ENG_TEST(Scene, PushAndPopStack) {
    SceneManager mgr;
    SceneContext ctx;
    mgr.SetContext(&ctx);
    mgr.Register("A", []() { return std::make_unique<TestSceneA>(); });
    mgr.Register("B", []() { return std::make_unique<TestSceneB>(); });

    mgr.SetScene("A");
    ENG_CHECK_EQ(mgr.StackDepth(), 0);
    ENG_CHECK(mgr.Push("B"));
    ENG_CHECK_EQ(mgr.StackDepth(), 1);
    ENG_CHECK_STR_EQ(mgr.CurrentName(), "B");
    ENG_CHECK(mgr.Pop());
    ENG_CHECK_EQ(mgr.StackDepth(), 0);
    ENG_CHECK_STR_EQ(mgr.CurrentName(), "A");
    ENG_CHECK(!mgr.Pop());
}

ENG_TEST(Scene, TransitionFadeCompletes) {
    SceneManager mgr;
    SceneContext ctx;
    Renderer2D r2d;
    ctx.r2d = &r2d;
    mgr.SetContext(&ctx);
    mgr.Register("A", []() { return std::make_unique<TestSceneA>(); });
    mgr.Register("B", []() { return std::make_unique<TestSceneB>(); });

    mgr.SetScene("A");
    TransitionDesc t;
    t.type = TransitionType::Fade;
    t.duration = 0.2f;
    ENG_CHECK(mgr.GoTo("B", t));
    ENG_CHECK(mgr.Transitioning());
    ENG_CHECK(mgr.InputBlocked());

    // На середине пути сцена уже сменилась, а ввод всё ещё заблокирован.
    mgr.Update(0.11f);
    ENG_CHECK_STR_EQ(mgr.CurrentName(), "B");
    ENG_CHECK(mgr.Transitioning());

    mgr.Update(0.11f);
    ENG_CHECK(!mgr.Transitioning());
    ENG_CHECK(!mgr.InputBlocked());
    ENG_CHECK_NEAR(mgr.TransitionProgress(), 1.0f, 1e-5f);
}

ENG_TEST(Scene, AllTransitionTypesRender) {
    SceneManager mgr;
    SceneContext ctx;
    Renderer2D r2d;
    ctx.r2d = &r2d;
    mgr.SetContext(&ctx);
    mgr.Register("A", []() { return std::make_unique<TestSceneA>(); });
    mgr.Register("B", []() { return std::make_unique<TestSceneB>(); });

    const TransitionType types[] = {TransitionType::Fade, TransitionType::SlideLeft,
                                    TransitionType::SlideRight, TransitionType::SlideUp,
                                    TransitionType::SlideDown, TransitionType::CircleWipe,
                                    TransitionType::CrossDissolve};
    for (TransitionType type : types) {
        mgr.SetScene("A");
        TransitionDesc t;
        t.type = type;
        t.duration = 0.1f;
        ENG_CHECK(mgr.GoTo("B", t));
        mgr.Update(0.05f);
        // Рендеринг без GL-контекста инертен, но не должен приводить к падению
        // или трогать неинициализированное состояние.
        ENG_CHECK(mgr.TransitionProgress() > 0.0f);
        mgr.Update(0.2f);
        ENG_CHECK(!mgr.Transitioning());
    }
}

ENG_TEST(Scene, ContextReceivesViewport) {
    SceneManager mgr;
    SceneContext ctx;
    ctx.viewport = Rect{0, 0, 1280, 720};
    ctx.dpiScale = 2.0f;
    mgr.SetContext(&ctx);
    TestSceneA* raw = nullptr;
    mgr.Register("A", [&raw]() {
        auto s = std::make_unique<TestSceneA>();
        raw = s.get();
        return s;
    });
    mgr.SetScene("A");
    mgr.Resize(1920, 1080);
    ENG_CHECK_EQ(raw->resizes, 2);  // один раз при входе, один при resize
}

// ---------------------------------------------------------------------------
// Движок (headless)
// ---------------------------------------------------------------------------
namespace {
bool InitHeadlessEngine(Engine& engine, EngineConfig& cfg) {
    cfg.headless = true;
    cfg.enable3D = true;
    cfg.enableAudio = false;
    cfg.enablePostProcessing = false;
    cfg.headlessTarget.width = 256;
    cfg.headlessTarget.height = 192;
    cfg.window.width = 256;
    cfg.window.height = 192;
    return engine.Init(cfg);
}
}  // namespace

ENG_TEST(Engine, HeadlessInitAndStep) {
    Engine engine;
    EngineConfig cfg;
    if (!InitHeadlessEngine(engine, cfg)) ENG_SKIP("headless GL unavailable");

    TestSceneA* raw = nullptr;
    engine.Scenes().Register("Test", [&raw]() {
        auto s = std::make_unique<TestSceneA>();
        raw = s.get();
        return s;
    });
    ENG_CHECK(engine.Scenes().SetScene("Test"));
    ENG_CHECK(raw != nullptr);

    for (int i = 0; i < 5; ++i) engine.Step(1.0f / 60.0f);
    ENG_CHECK_EQ(raw->updates, 5);
    ENG_CHECK(raw->draws2d >= 5);
    ENG_CHECK_GT(engine.Stats().frame, 0u);
    // Возвращаемое имя — это собственное Name() сцены (id в реестре
    // в примере), а не ключ, под которым она зарегистрирована.
    ENG_CHECK_STR_EQ(engine.Stats().sceneName, raw->Name());
    engine.Shutdown();
}

ENG_TEST(Engine, HeadlessRendersSomething) {
    Engine engine;
    EngineConfig cfg;
    if (!InitHeadlessEngine(engine, cfg)) ENG_SKIP("headless GL unavailable");

    struct ClearScene : Scene {
        const char* Name() const override { return "Clear"; }
        Color ClearColor() const override { return Color::FromARGB(0xFF3366CC); }
    };
    engine.Scenes().Register("Clear", []() { return std::make_unique<ClearScene>(); });
    engine.Scenes().SetScene("Clear");
    engine.Step(1.0f / 60.0f);

    // Кадр должен быть очищен цветом сцены (пиксель внизу слева).
    Color px = engine.ReadPixel(4, 4);
    ENG_CHECK(px.b > 0.6f);
    ENG_CHECK(px.b > px.r);
    engine.Shutdown();
}

ENG_TEST(Engine, PauseStopsSceneUpdates) {
    Engine engine;
    EngineConfig cfg;
    if (!InitHeadlessEngine(engine, cfg)) ENG_SKIP("headless GL unavailable");
    TestSceneA* raw = nullptr;
    engine.Scenes().Register("Test", [&raw]() {
        auto s = std::make_unique<TestSceneA>();
        raw = s.get();
        return s;
    });
    engine.Scenes().SetScene("Test");
    engine.Step(1.0f / 60.0f);
    int before = raw->updates;
    engine.SetPaused(true);
    engine.Step(1.0f / 60.0f);
    ENG_CHECK_EQ(raw->updates, before);
    engine.SetPaused(false);
    engine.Step(1.0f / 60.0f);
    ENG_CHECK_EQ(raw->updates, before + 1);
    engine.Shutdown();
}

ENG_TEST(Engine, ScreenshotWritesFile) {
    Engine engine;
    EngineConfig cfg;
    if (!InitHeadlessEngine(engine, cfg)) ENG_SKIP("headless GL unavailable");
    struct ClearScene : Scene {
        const char* Name() const override { return "Shot"; }
    };
    engine.Scenes().Register("Shot", []() { return std::make_unique<ClearScene>(); });
    engine.Scenes().SetScene("Shot");
    engine.Step(1.0f / 60.0f);
    std::string path = engine.Screenshot("test_shot.png");
    ENG_CHECK(!path.empty());
    ENG_CHECK(FileExists(path));
    engine.Shutdown();
}

ENG_TEST(Engine, QuitFlagStopsRun) {
    Engine engine;
    EngineConfig cfg;
    if (!InitHeadlessEngine(engine, cfg)) ENG_SKIP("headless GL unavailable");
    ENG_CHECK(!engine.ShouldQuit());
    engine.Quit();
    ENG_CHECK(engine.ShouldQuit());
    ENG_CHECK_EQ(engine.Run(), 0);
    engine.Shutdown();
}
