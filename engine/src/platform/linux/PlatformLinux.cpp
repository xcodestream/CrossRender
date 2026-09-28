// Сервисы платформы Linux: системная информация из /proc, метрики экрана X11,
// интеграция с оболочкой, цикл AppHooks и headless-контекст GLX pbuffer.
#include "crossrender/platform/Platform.h"

#include "crossrender/core/Log.h"
#include "crossrender/core/Time.h"
#include "crossrender/platform/Window.h"

#if defined(ENG_PLATFORM_LINUX)

#include <GL/glx.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <sys/resource.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>

#include <ctime>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <sstream>
#include <functional>

namespace {

// ---------------------------------------------------------------------------
// Поверхность расширений GLX, нужная headless-пути (объявлена локально, потому
// что движок никогда не включает системный GL-заголовок).
// ---------------------------------------------------------------------------
using PFNGLXCREATECONTEXTATTRIBSARBPROC = GLXContext (*)(Display*, GLXFBConfig, GLXContext, Bool,
                                                         const int*);

constexpr int kGlxContextMajorVersionArb = 0x2091;
constexpr int kGlxContextMinorVersionArb = 0x2092;
constexpr int kGlxContextProfileMaskArb = 0x9126;
constexpr int kGlxContextCoreProfileBitArb = 0x00000001;

void* LinuxGLGetProcAddress(const char* name) {
    if (!name) return nullptr;
    void* p = reinterpret_cast<void*>(glXGetProcAddressARB(
        reinterpret_cast<const GLubyte*>(name)));
    if (p) return p;
    return dlsym(RTLD_DEFAULT, name);
}

// ---------------------------------------------------------------------------
// Чтение /proc
// ---------------------------------------------------------------------------
bool ReadWholeFile(const char* path, std::string* out) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return false;
    std::ostringstream stream;
    stream << file.rdbuf();
    *out = stream.str();
    return !out->empty();
}

std::string FirstLineMatching(const std::string& text, const char* key) {
    const size_t keyLen = std::strlen(key);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        if (text.compare(pos, keyLen, key) == 0) {
            size_t value = text.find(':', pos);
            if (value != std::string::npos && value < end) {
                size_t start = value + 1;
                while (start < end && (text[start] == ' ' || text[start] == '\t')) ++start;
                size_t stop = end;
                while (stop > start && (text[stop - 1] == ' ' || text[stop - 1] == '\t' ||
                                        text[stop - 1] == '\r')) {
                    --stop;
                }
                return text.substr(start, stop - start);
            }
        }
        pos = end + 1;
    }
    return std::string();
}

// ---------------------------------------------------------------------------
// Общий X-дисплей для платформенных сервисов (размер экрана, диалоги,
// headless-контекст). Открывается лениво и закрывается в PlatformShutdown.
// ---------------------------------------------------------------------------
Display* g_sharedDisplay = nullptr;
bool g_sharedDisplayAttempted = false;

Display* SharedDisplay(bool openIfMissing = true) {
    if (!g_sharedDisplay && openIfMissing && !g_sharedDisplayAttempted) {
        g_sharedDisplayAttempted = true;
        g_sharedDisplay = XOpenDisplay(nullptr);
    }
    return g_sharedDisplay;
}

void CloseSharedDisplay() {
    if (g_sharedDisplay) {
        XCloseDisplay(g_sharedDisplay);
        g_sharedDisplay = nullptr;
    }
}

bool HaveDisplay() { return SharedDisplay() != nullptr; }

}  // namespace

namespace crossrender {

// ---------------------------------------------------------------------------
// Жизненный цикл / идентификация
// ---------------------------------------------------------------------------
bool PlatformInit() {
    // Подключаемся сразу: остальные сервисы платформы переиспользуют этот дисплей.
    if (SharedDisplay() == nullptr) {
        ENG_LOGW("platform", "no X display available (DISPLAY unset or X server unreachable)");
    }
    return true;
}

void PlatformShutdown() { CloseSharedDisplay(); }

std::string PlatformName() { return "Linux"; }

std::string PlatformArch() {
#if defined(ENG_ARCH_ARM64)
    return "arm64";
#elif defined(ENG_ARCH_X64)
    return "x86_64";
#else
    return "unknown";
#endif
}

std::string CpuName() {
    std::string text;
    if (ReadWholeFile("/proc/cpuinfo", &text)) {
        // Ядра x86 дают "model name"; ядра ARM обычно выставляют "Hardware",
        // "Processor" или имя модели первого ядра.
        const char* keys[] = {"model name", "Hardware", "cpu model", "Processor"};
        for (const char* key : keys) {
            const std::string value = FirstLineMatching(text, key);
            if (!value.empty()) return value;
        }
    }
    return PlatformArch();
}

u64 TotalPhysicalMemory() {
    std::string text;
    if (ReadWholeFile("/proc/meminfo", &text)) {
        const std::string value = FirstLineMatching(text, "MemTotal");
        if (!value.empty()) {
            // "16317164 kB"
            unsigned long long kb = 0;
            if (std::sscanf(value.c_str(), "%llu", &kb) == 1) return static_cast<u64>(kb) * 1024ull;
        }
    }
    return 0;
}

int CpuCoreCount() {
    const long online = sysconf(_SC_NPROCESSORS_ONLN);
    if (online > 0) return static_cast<int>(online);
    return 1;
}

// ---------------------------------------------------------------------------
// Пути
// ---------------------------------------------------------------------------
usize ProcessResidentBytes() {
    std::ifstream statm("/proc/self/statm");
    u64 total = 0, resident = 0;
    if (statm >> total >> resident) {
        return static_cast<usize>(resident * static_cast<u64>(sysconf(_SC_PAGESIZE)));
    }
    return 0;
}

f64 ProcessCpuSeconds() {
    struct rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return static_cast<f64>(ru.ru_utime.tv_sec) + ru.ru_utime.tv_usec / 1e6 +
           static_cast<f64>(ru.ru_stime.tv_sec) + ru.ru_stime.tv_usec / 1e6;
}

std::string ExecutablePath() {
    char buffer[4096];
    const ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (len > 0) {
        buffer[len] = '\0';
        return std::string(buffer);
    }
    return std::string();
}

std::string ExecutableDir() {
    const std::string path = ExecutablePath();
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return std::string();
    return path.substr(0, slash);
}

// ---------------------------------------------------------------------------
// Время / потоки
// ---------------------------------------------------------------------------
void SleepMs(u32 milliseconds) {
    if (milliseconds == 0) return;
    timespec ts{};
    ts.tv_sec = static_cast<time_t>(milliseconds / 1000u);
    ts.tv_nsec = static_cast<long>(milliseconds % 1000u) * 1000000L;
    // nanosleep прерывается сигналами; досыпаем остаток.
    timespec remaining = ts;
    while (nanosleep(&remaining, &remaining) == -1 && errno == EINTR) {
    }
}

u64 CurrentThreadId() {
    std::hash<std::thread::id> hash;
    return static_cast<u64>(hash(std::this_thread::get_id()));
}

// ---------------------------------------------------------------------------
// Диалоги / оболочка
//
// Десктопонезависимого диалогового API в Linux нет, а движок не должен
// тянуть GTK, поэтому выбор файлов логирует и возвращает пустоту. Окна
// сообщений пробуют zenity при наличии дисплея; без него они не блокируют.
// ---------------------------------------------------------------------------
void ShowMessageBox(const std::string& title, const std::string& message, MessageBoxType type) {
    const char* level = "info";
    switch (type) {
        case MessageBoxType::Warning: level = "warning"; break;
        case MessageBoxType::Error: level = "error"; break;
        case MessageBoxType::Question: level = "question"; break;
        case MessageBoxType::Info:
        default: level = "info"; break;
    }
    ENG_LOGI("platform", "[%s] %s: %s", level, title.c_str(), message.c_str());

    if (!HaveDisplay()) return;
    // Экранируем одинарные кавычки, чтобы оболочка не вышла за пределы аргумента.
    auto quote = [](const std::string& in) {
        std::string out = "'";
        for (char c : in) {
            if (c == '\'') {
                out += "'\\''";
            } else {
                out.push_back(c);
            }
        }
        out.push_back('\'');
        return out;
    };
    const std::string command = "zenity --" + std::string(level) + " --title=" + quote(title) +
                                " --text=" + quote(message) + " >/dev/null 2>&1";
    const int rc = std::system(command.c_str());
    if (rc != 0) ENG_LOGD("platform", "zenity unavailable or dismissed (rc=%d)", rc);
}

std::string OpenFileDialog(const std::string& title, const std::string& filter) {
    (void)filter;
    ENG_LOGW("platform", "OpenFileDialog('%s') is unsupported on Linux (no GTK dependency)",
             title.c_str());
    return std::string();
}

std::string SaveFileDialog(const std::string& title, const std::string& defaultName) {
    ENG_LOGW("platform", "SaveFileDialog('%s', '%s') is unsupported on Linux (no GTK dependency)",
             title.c_str(), defaultName.c_str());
    return std::string();
}

bool OpenUrl(const std::string& url) {
    const pid_t pid = fork();
    if (pid < 0) {
        ENG_LOGE("platform", "OpenUrl: fork failed (%s)", std::strerror(errno));
        return false;
    }
    if (pid == 0) {
        // Потомок: отвязываемся от терминала и передаём URL в xdg-open.
        setsid();
        execlp("xdg-open", "xdg-open", url.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    // Родитель не ждёт: браузеры живут долго. Забираем лениво.
    int status = 0;
    const pid_t reaped = waitpid(pid, &status, WNOHANG);
    (void)reaped;
    return true;
}

void GetScreenSize(int* w, int* h) {
    Display* display = SharedDisplay();
    if (display) {
        const int screen = DefaultScreen(display);
        const int width = DisplayWidth(display, screen);
        const int height = DisplayHeight(display, screen);
        if (width > 0 && height > 0) {
            if (w) *w = width;
            if (h) *h = height;
            return;
        }
    }
    // Headless / нет X-сервера: сообщаем разумное значение по умолчанию.
    if (w) *w = 1920;
    if (h) *h = 1080;
}

// ---------------------------------------------------------------------------
// Заглушки мобильных/консольных функций. На десктопе Linux нет экранной
// клавиатуры, вибрации и удержания экрана, поэтому это намеренные no-op.
// ---------------------------------------------------------------------------
void SetSoftKeyboardVisible(bool visible) { (void)visible; }
void SetKeepScreenAwake(bool enabled) { (void)enabled; }
void Vibrate(u32 milliseconds) { (void)milliseconds; }

bool IsAppForeground() {
    Display* display = SharedDisplay();
    if (!display) return true;  // нет оконной системы: фокус украсть некому
    ::Window focused = None;
    int revert = 0;
    XGetInputFocus(display, &focused, &revert);
    return focused != None && focused != PointerRoot;
}

// ---------------------------------------------------------------------------
// Жизненный цикл приложения
// ---------------------------------------------------------------------------
int RunApp(const AppHooks& hooks) {
    WindowDesc desc;
    desc.vsync = true;

    Window window;
    if (!window.Create(desc)) {
        ENG_LOGE("platform", "RunApp: window creation failed");
        return 1;
    }

    if (hooks.onInit) hooks.onInit(hooks.user);

    f64 previous = NowSeconds();
    while (!window.ShouldClose()) {
        window.GetInput().BeginFrame();
        window.PollEvents();

        const f64 now = NowSeconds();
        f32 dt = static_cast<f32>(now - previous);
        previous = now;
        if (dt < 0.0f) dt = 0.0f;
        if (dt > 0.25f) dt = 0.25f;  // ограничиваем длинные простои (отладчик, suspend)

        if (hooks.onFrame && !hooks.onFrame(hooks.user, dt)) {
            window.RequestClose();
            break;
        }
        window.GetInput().EndFrame();
        window.SwapBuffers();

        // glXSwapBuffers не блокирует без swap interval, поэтому ждём события
        // с коротким таймаутом вместо вращения ядра впустую.
        if (desc.vsync) window.WaitEventsTimeout(0.05f);
    }

    if (hooks.onShutdown) hooks.onShutdown(hooks.user);
    window.Destroy();
    return 0;
}

// ---------------------------------------------------------------------------
// Headless GL-контекст (тесты / CI / --headless).
//
// Использует GLX_PBUFFER, если версия GLX поддерживает; иначе drawable —
// несмонтированное окно XCreateWindow. Обоим нужен живой X-дисплей.
// ---------------------------------------------------------------------------
namespace {

struct HeadlessState {
    Display* display = nullptr;
    GLXContext context = nullptr;
    GLXPbuffer pbuffer = None;
    ::Window window = 0;
    Colormap colormap = 0;
    int refCount = 0;
    bool usingPbuffer = false;
};

HeadlessState& Headless() {
    static HeadlessState state;
    return state;
}

bool CreateHeadlessContext(HeadlessState& state) {
    state.display = XOpenDisplay(nullptr);
    if (!state.display) {
        ENG_LOGW("platform", "headless GL unavailable: no X display");
        return false;
    }
    const int screen = DefaultScreen(state.display);

    const int fbAttribs[] = {GLX_X_RENDERABLE, True,
                             GLX_DRAWABLE_TYPE, GLX_PBUFFER_BIT | GLX_WINDOW_BIT,
                             GLX_RENDER_TYPE, GLX_RGBA_BIT,
                             GLX_RED_SIZE, 8,
                             GLX_GREEN_SIZE, 8,
                             GLX_BLUE_SIZE, 8,
                             GLX_ALPHA_SIZE, 8,
                             GLX_DEPTH_SIZE, 24,
                             GLX_STENCIL_SIZE, 8,
                             None};
    int configCount = 0;
    GLXFBConfig* configs = glXChooseFBConfig(state.display, screen, fbAttribs, &configCount);
    if (!configs || configCount == 0) {
        if (configs) XFree(configs);
        ENG_LOGW("platform", "headless: glXChooseFBConfig returned no configs");
        return false;
    }
    GLXFBConfig config = configs[0];
    XVisualInfo* visual = glXGetVisualFromFBConfig(state.display, config);
    XFree(configs);
    if (!visual) {
        ENG_LOGW("platform", "headless: glXGetVisualFromFBConfig failed");
        return false;
    }

    auto createContextAttribs = reinterpret_cast<PFNGLXCREATECONTEXTATTRIBSARBPROC>(
        glXGetProcAddressARB(reinterpret_cast<const GLubyte*>("glXCreateContextAttribsARB")));
    if (createContextAttribs) {
        const int attribs[] = {kGlxContextMajorVersionArb, 3,
                               kGlxContextMinorVersionArb, 3,
                               kGlxContextProfileMaskArb, kGlxContextCoreProfileBitArb, 0};
        state.context = createContextAttribs(state.display, config, nullptr, True, attribs);
    }
    if (!state.context) {
        state.context = glXCreateNewContext(state.display, config, GLX_RGBA_TYPE, nullptr, True);
        if (state.context) ENG_LOGW("platform", "headless: created a legacy GLX context");
    }

    if (state.context) {
        const int pbAttribs[] = {GLX_PBUFFER_WIDTH, 16, GLX_PBUFFER_HEIGHT, 16, None};
        state.pbuffer = glXCreatePbuffer(state.display, config, pbAttribs);
        if (state.pbuffer != None) {
            state.usingPbuffer = true;
            glXMakeCurrent(state.display, state.pbuffer, state.context);
        } else {
            // Pbuffer недоступны: несмонтированное окно работает так же хорошо.
            ENG_LOGW("platform", "headless: GLX_PBUFFER unavailable; using an unmapped window");
            XSetWindowAttributes attrs{};
            attrs.colormap = state.colormap =
                XCreateColormap(state.display, RootWindow(state.display, visual->screen),
                                visual->visual, AllocNone);
            attrs.border_pixel = 0;
            attrs.event_mask = StructureNotifyMask;
            state.window =
                XCreateWindow(state.display, RootWindow(state.display, visual->screen), 0, 0, 16,
                              16, 0, visual->depth, InputOutput, visual->visual,
                              CWColormap | CWBorderPixel | CWEventMask, &attrs);
            if (state.window) glXMakeCurrent(state.display, state.window, state.context);
        }
    }
    XFree(visual);
    return state.context != nullptr;
}

void DestroyHeadlessContext(HeadlessState& state) {
    if (!state.display) return;
    glXMakeCurrent(state.display, None, nullptr);
    if (state.context) glXDestroyContext(state.display, state.context);
    if (state.pbuffer != None) glXDestroyPbuffer(state.display, state.pbuffer);
    if (state.window) XDestroyWindow(state.display, state.window);
    if (state.colormap) XFreeColormap(state.display, state.colormap);
    XCloseDisplay(state.display);
    // HeadlessState не содержит нетривиальных членов, но обнуляется на месте,
    // поэтому деструктору никогда не приходится запускаться.
    state.display = nullptr;
    state.context = nullptr;
    state.pbuffer = None;
    state.window = 0;
    state.colormap = 0;
    state.refCount = 0;
    state.usingPbuffer = false;
}

}  // namespace

bool CreateHeadlessGLContext() {
    HeadlessState& state = Headless();
    if (state.context) {
        ++state.refCount;
        glXMakeCurrent(state.display, state.usingPbuffer ? static_cast<GLXDrawable>(state.pbuffer)
                                                         : static_cast<GLXDrawable>(state.window),
                       state.context);
        return true;
    }
    if (!CreateHeadlessContext(state)) {
        if (state.display) DestroyHeadlessContext(state);
        return false;
    }
    state.refCount = 1;
    ENG_LOGI("platform", "headless GL context created (%s)",
             state.usingPbuffer ? "GLX pbuffer 16x16" : "unmapped window 16x16");
    return true;
}

void DestroyHeadlessGLContext() {
    HeadlessState& state = Headless();
    if (!state.context) return;
    if (--state.refCount > 0) return;
    DestroyHeadlessContext(state);
}

bool HasHeadlessGLContext() { return Headless().context != nullptr; }

void* HeadlessGLGetProcAddress(const char* name) { return LinuxGLGetProcAddress(name); }

}  // namespace crossrender

#else

// В сборках без Linux эта единица трансляции пуста; каждая платформа
// предоставляет свой файл в engine/src/platform/<os>/.
namespace crossrender {
bool CreateHeadlessGLContext() { return false; }
void DestroyHeadlessGLContext() {}
bool HasHeadlessGLContext() { return false; }
void* HeadlessGLGetProcAddress(const char* name) {
    (void)name;
    return nullptr;
}
}  // namespace crossrender

#endif
