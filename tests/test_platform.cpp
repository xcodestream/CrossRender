// Тесты платформенного слоя: автомат состояний ввода, жизненный цикл окна, платформенные сервисы.
#include "crossrender/gfx/GL.h"
#include "crossrender/core/File.h"
#include "crossrender/test/Test.h"
#include "crossrender/platform/Window.h"
#include "crossrender/platform/Platform.h"

#include <thread>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

using namespace crossrender;

// ---------------------------------------------------------------------------
// Ввод
// ---------------------------------------------------------------------------
ENG_TEST(Input, KeyPressReleaseEdges) {
    Input in;
    // Нажатия видны в том же кадре, в котором происходят.
    in.BeginFrame();
    in.OnKey(Key::W, KeyAction::Press, false);
    ENG_CHECK(in.KeyDown(Key::W));
    ENG_CHECK(in.KeyPressed(Key::W));
    ENG_CHECK(!in.KeyReleased(Key::W));
    ENG_CHECK(in.AnyKeyDown());
    in.EndFrame();

    // ...и сбрасываются следующим BeginFrame, пока клавиша остаётся зажатой.
    in.BeginFrame();
    ENG_CHECK(in.KeyDown(Key::W));
    ENG_CHECK(!in.KeyPressed(Key::W));
    in.OnKey(Key::W, KeyAction::Release, false);
    ENG_CHECK(!in.KeyDown(Key::W));
    ENG_CHECK(in.KeyReleased(Key::W));
    in.EndFrame();

    in.BeginFrame();
    ENG_CHECK(!in.KeyDown(Key::W));
    ENG_CHECK(!in.KeyReleased(Key::W));
    ENG_CHECK(!in.AnyKeyDown());
    in.EndFrame();

    // Повтор сообщает и down, и pressed, чтобы текст/UI могли автоповторяться.
    in.BeginFrame();
    in.OnKey(Key::A, KeyAction::Repeat, true);
    ENG_CHECK(in.KeyDown(Key::A));
    ENG_CHECK(in.KeyPressed(Key::A));
    in.EndFrame();
}

ENG_TEST(Input, ModifierHelpers) {
    Input in;
    in.BeginFrame();
    in.OnKey(Key::LeftControl, KeyAction::Press, false);
    in.OnKey(Key::LeftShift, KeyAction::Press, false);
    ENG_CHECK(in.CtrlDown());
    ENG_CHECK(in.ShiftDown());
    ENG_CHECK(!in.AltDown());
    in.OnKey(Key::RightAlt, KeyAction::Press, false);
    ENG_CHECK(in.AltDown());
    in.OnKey(Key::LeftSuper, KeyAction::Press, false);
    ENG_CHECK(in.SuperDown());
    in.EndFrame();
}

ENG_TEST(Input, MousePositionAndButtons) {
    Input in;
    in.BeginFrame();
    in.OnMouseMove({10, 20});
    ENG_CHECK_NEAR(in.MousePos().x, 10.0f, 1e-5f);
    in.OnMouseMove({15, 25});
    ENG_CHECK_NEAR(in.MousePos().x, 15.0f, 1e-5f);
    ENG_CHECK_NEAR(in.MouseDelta().x, 15.0f, 1e-5f);
    ENG_CHECK_NEAR(in.MouseDelta().y, 25.0f, 1e-5f);

    in.OnMouseButton(MouseButton::Left, true, {15, 25});
    ENG_CHECK(in.MouseDown(MouseButton::Left));
    ENG_CHECK(in.MousePressed(MouseButton::Left));
    ENG_CHECK(!in.MouseDown(MouseButton::Right));
    in.EndFrame();

    in.BeginFrame();
    ENG_CHECK(in.MouseDown(MouseButton::Left));
    ENG_CHECK(!in.MousePressed(MouseButton::Left));
    ENG_CHECK_NEAR(in.MouseDelta().x, 0.0f, 1e-5f);
    in.OnMouseButton(MouseButton::Left, false, {15, 25});
    ENG_CHECK(!in.MouseDown(MouseButton::Left));
    ENG_CHECK(in.MouseReleased(MouseButton::Left));
    in.EndFrame();

    in.BeginFrame();
    ENG_CHECK(!in.MouseDown(MouseButton::Left));
    ENG_CHECK(!in.MouseReleased(MouseButton::Left));
    in.EndFrame();
}

ENG_TEST(Input, ScrollAccumulatesAndResets) {
    Input in;
    in.BeginFrame();
    in.OnScroll({0, 1.5f});
    in.OnScroll({0, 0.5f});
    ENG_CHECK_NEAR(in.ScrollDelta().y, 2.0f, 1e-5f);
    in.EndFrame();
    in.BeginFrame();
    ENG_CHECK_NEAR(in.ScrollDelta().y, 0.0f, 1e-5f);
    in.EndFrame();
}

ENG_TEST(Input, TextInputCodepoints) {
    Input in;
    in.BeginFrame();
    in.OnText('A');
    in.OnText(0x0414);  // 'Д'
    ENG_CHECK_EQ(in.TextInput().size(), 2u);
    ENG_CHECK_EQ(in.TextInput()[0], static_cast<u32>('A'));
    ENG_CHECK_EQ(in.TextInput()[1], 0x0414u);
    in.EndFrame();
    in.BeginFrame();
    ENG_CHECK_EQ(in.TextInput().size(), 0u);
    in.EndFrame();
}

ENG_TEST(Input, TouchLifecycle) {
    Input in;
    in.BeginFrame();
    TouchPoint tp;
    tp.id = 0;
    tp.pos = {100, 100};
    tp.start = {100, 100};
    tp.phase = TouchPhase::Down;
    in.OnTouch(tp);
    ENG_CHECK_EQ(in.TouchCount(), 1);
    tp.phase = TouchPhase::Move;
    tp.pos = {110, 120};
    in.OnTouch(tp);
    ENG_CHECK_EQ(in.TouchCount(), 1);
    tp.phase = TouchPhase::Up;
    in.OnTouch(tp);
    ENG_CHECK_EQ(in.TouchCount(), 0);
    in.EndFrame();
}

ENG_TEST(Input, GamepadState) {
    Input in;
    float axes[4] = {0.5f, -0.25f, 0.0f, 0.0f};
    in.SetGamepad(0, true, axes, 4, 0x3);
    ENG_CHECK(in.GamepadConnected(0));
    ENG_CHECK_EQ(in.GamepadButtons(0), 0x3u);
    ENG_CHECK_NEAR(in.GamepadStick(0, 0).x, 0.5f, 1e-5f);
    ENG_CHECK(!in.GamepadConnected(1));
}

ENG_TEST(Input, KeyNames) {
    ENG_CHECK(std::strcmp(KeyName(Key::A), "A") == 0);
    ENG_CHECK(std::strlen(KeyName(Key::Space)) > 0);
    ENG_CHECK(std::strlen(KeyName(Key::Unknown)) > 0);
    for (int i = 0; i < static_cast<int>(Key::Count); ++i) {
        const char* n = KeyName(static_cast<Key>(i));
        ENG_CHECK(n != nullptr && n[0] != '\0');
    }
}

// ---------------------------------------------------------------------------
// Платформенные сервисы
// ---------------------------------------------------------------------------
ENG_TEST(Platform, NameAndMetrics) {
    std::string name = PlatformName();
    ENG_CHECK(!name.empty());
    ENG_CHECK(!PlatformArch().empty());
    ENG_CHECK(CpuCoreCount() >= 1);
    ENG_CHECK(TotalPhysicalMemory() > 0);
    ENG_CHECK(!ExecutablePath().empty());
    ENG_CHECK(!ExecutableDir().empty());

    int w = 0, h = 0;
    GetScreenSize(&w, &h);
    ENG_CHECK(w > 0);
    ENG_CHECK(h > 0);
}

ENG_TEST(Platform, TimerAndSleep) {
    f64 a = HighResTimerSeconds();
    SleepMs(12);
    f64 b = HighResTimerSeconds();
    ENG_CHECK(b > a);
    ENG_CHECK(b - a >= 0.005);
    ENG_CHECK(CurrentThreadId() != 0);
}

ENG_TEST(Platform, ForegroundQueryDoesNotCrash) {
    (void)IsAppForeground();
    SetSoftKeyboardVisible(false);
    SetKeepScreenAwake(false);
    Vibrate(0);
    ENG_CHECK(true);
}

// ---------------------------------------------------------------------------
// Окно
// ---------------------------------------------------------------------------
ENG_TEST(Window, CreateResizeAndInput) {
    Window win;
    WindowDesc desc;
    desc.title = "CrossRender test window";
    desc.width = 320;
    desc.height = 240;
    desc.vsync = false;
    desc.msaaSamples = 0;
    desc.resizable = true;
    if (!win.Create(desc)) ENG_SKIP("no display available");

    ENG_CHECK(win.Width() > 0);
    ENG_CHECK(win.Height() > 0);
    ENG_CHECK(win.FramebufferWidth() > 0);
    ENG_CHECK(win.FramebufferHeight() > 0);
    ENG_CHECK(win.Aspect() > 0.0f);
    ENG_CHECK(win.DpiScale() > 0.0f);
    ENG_CHECK(win.GLGetProcAddress() != nullptr);
    ENG_CHECK(!win.ShouldClose());

    win.SetSize(400, 300);
    win.SetTitle("renamed");
    ENG_CHECK_STR_EQ(win.Title(), "renamed");
    win.PollEvents();
    ENG_CHECK(win.Width() > 0);

    win.SetClipboardText("engine-clipboard-test");
    std::string back = win.GetClipboardText();
    ENG_CHECK_STR_EQ(back, "engine-clipboard-test");

    win.MakeCurrent();
    win.SwapBuffers();
    win.SetVSync(true);
    win.SetVSync(false);
    win.SetCursorVisible(true);
    win.SetCursorMode(0);

    win.RequestClose();
    ENG_CHECK(win.ShouldClose());
    win.Destroy();
}

ENG_TEST(Window, HeadlessContextIsAvailable) {
    // Раннер держит один GL-контекст живым для всего набора, потому что GPU-ресурсы,
    // кэшируемые в function-local statics (шрифтовые атласы, текстуры), умирают
    // вместе с ним. Уничтожение здесь молча сделало бы их недействительными для
    // каждого следующего теста, поэтому путь create/destroy отрабатывается только
    // когда этот тест владеет контекстом.
    if (HasHeadlessGLContext()) {
        ENG_CHECK(HeadlessGLGetProcAddress("glGetError") != nullptr);
        return;
    }
    if (!CreateHeadlessGLContext()) ENG_SKIP("headless GL unavailable");
    ENG_CHECK(HasHeadlessGLContext());
    ENG_CHECK(HeadlessGLGetProcAddress("glGetError") != nullptr);
    DestroyHeadlessGLContext();
    ENG_CHECK(!HasHeadlessGLContext());
}

ENG_TEST(Window, GLFunctionsLoad) {
    // То же правило, что и выше: никогда не выгружать и не уничтожать контекст,
    // который набор ещё использует, и не перезагружать точки входа, если они уже загружены.
    const bool owned = !HasHeadlessGLContext();
    if (owned && !CreateHeadlessGLContext()) ENG_SKIP("headless GL unavailable");
    if (!HasHeadlessGLContext()) ENG_SKIP("headless GL unavailable");
    if (!gl::glCreateShader) ENG_CHECK(gl::LoadFunctions(HeadlessGLGetProcAddress));
    gl::GpuInfo info = gl::QueryGpuInfo();
    ENG_CHECK(!info.version.empty());
    ENG_CHECK(info.maxTextureSize >= 1024);
    ENG_CHECK(gl::glCreateShader != nullptr);
    unsigned int shader = gl::glCreateShader(gl::GL_VERTEX_SHADER);
    ENG_CHECK(shader != 0);
    gl::glDeleteShader(shader);
    if (owned) {
        gl::UnloadFunctions();
        DestroyHeadlessGLContext();
    }
}

ENG_TEST(Window, FileSystemRootsResolve) {
    const std::string& assets = GetAssetRoot();
    ENG_CHECK(!assets.empty());
    std::string path = PathJoin(assets, "does_not_exist_probe.txt");
    ENG_CHECK(!FileExists(path));
    // Пользовательский корень должен быть доступен для записи.
    std::string userFile = "user/probe_platform.txt";
    ENG_CHECK(WriteTextFile(userFile, "probe"));
    ENG_CHECK(FileExists(userFile));
}

// ---------------------------------------------------------------------------
// Определение корня ассетов
// ---------------------------------------------------------------------------
// Корень ищется подъёмом вверх от *исполняемого файла*, а не от рабочего
// каталога. Разрешение от CWD означало, что приложение находило ассеты, только
// если было запущено из корня репозитория; при любом другом запуске
// (двойной клик, IDE, другая оболочка) оно молча откатывалось к
// процедурному bitmap-шрифту и не загружало ни одной текстуры.
ENG_TEST(File, AssetRootIsResolvedFromTheExecutable) {
    const std::string root = GetAssetRoot();
    ENG_CHECK(!root.empty());
    ENG_CHECK_GT(root.size(), 1u);
    ENG_CHECK_MSG(root[0] == '/', "the asset root must be absolute");
    ENG_CHECK(DirectoryExists(root));
    ENG_CHECK(FileExists(PathJoin(root, "fonts/ubuntu.ttf")));

    // Повторно разрешаем из каталога, не связанного с проектом, и требуем
    // того же ответа.
    char saved[4096];
    const bool haveCwd = getcwd(saved, sizeof(saved)) != nullptr;
    if (chdir("/") == 0) {
        SetAssetRoot("");
        const std::string fromRoot = GetAssetRoot();
        ENG_CHECK_MSG(fromRoot == root,
                      "the asset root must not depend on the working directory");
        ENG_CHECK(FileExists(PathJoin(fromRoot, "fonts/ubuntu.ttf")));
        if (haveCwd) (void)chdir(saved);
    }
    SetAssetRoot(root);
}

// ---------------------------------------------------------------------------
// Изменение размера окна
// ---------------------------------------------------------------------------
// Размер framebuffer, относительно которого движок выкладывает кадр, обязан
// следовать за окном. На macOS GL-drawable дополнительно требует явного
// обновления, иначе кадр рендерится с новым viewport в drawable со старым
// размером, что обрезает изображение и срезает текст.
ENG_TEST(Window, ResizeUpdatesTheFramebufferSize) {
    WindowDesc desc;
    desc.width = 480;
    desc.height = 360;
    desc.title = "resize test";
    Window win;
    if (!win.Create(desc)) ENG_SKIP("no display available");

    const int startW = win.Width();
    ENG_CHECK_GT(startW, 0);

    win.SetSize(640, 480);
    win.PollEvents();          // здесь AppKit обрабатывает изменение размера

    ENG_CHECK_EQ(win.Width(), 640);
    ENG_CHECK_EQ(win.Height(), 480);
    // Framebuffer следует за окном, умноженным на DPI-фактор дисплея.
    const int expectedW = static_cast<int>(640 * win.DpiScale());
    const int expectedH = static_cast<int>(480 * win.DpiScale());
    ENG_CHECK_MSG(win.FramebufferWidth() == expectedW,
                  "framebuffer width must track the window size");
    ENG_CHECK_MSG(win.FramebufferHeight() == expectedH,
                  "framebuffer height must track the window size");

    // Уменьшение работает так же.
    win.SetSize(320, 240);
    win.PollEvents();
    ENG_CHECK_EQ(win.Width(), 320);
    ENG_CHECK_EQ(win.FramebufferWidth(), static_cast<int>(320 * win.DpiScale()));

    win.Destroy();
}
