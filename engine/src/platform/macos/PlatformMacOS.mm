// Сервисы платформы macOS: нативные диалоги, сведения о системе, цикл AppHooks
// и обработка power assertion.
//
// Окно/GL/headless живут в WindowMacOS.mm (включая PlatformInit,
// PlatformShutdown, PlatformName и PlatformGLGetProcAddress), поэтому этот файл
// намеренно не переопределяет ни один из этих символов.
#include "crossrender/platform/Platform.h"

#include "crossrender/core/Log.h"
#include "crossrender/core/Time.h"
#include "crossrender/platform/Window.h"

#if defined(ENG_PLATFORM_MACOS)

#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>
#import <IOKit/pwr_mgt/IOPMLib.h>

#include <unistd.h>
#include <pthread.h>
#include <sys/sysctl.h>
#include <mach-o/dyld.h>

#include <string>
#include <vector>
#include <cstdlib>
#include <cstring>

namespace crossrender {
namespace {

// ---------------------------------------------------------------------------
// Помощники UTF-8 <-> NSString. Файл компилируется с ARC, так что это простые
// autoreleased-конверсии.
// ---------------------------------------------------------------------------
NSString* ToNSString(const std::string& s) {
    return [NSString stringWithUTF8String:s.c_str()];
}

std::string ToUtf8(NSString* s) {
    if (!s) return std::string();
    const char* utf8 = [s UTF8String];
    return utf8 ? std::string(utf8) : std::string();
}

// Переводит фильтр "*.png;*.jpg" / "png,jpg" в типы файлов NSOpenPanel.
NSArray* AllowedTypes(const std::string& filter) {
    if (filter.empty()) return nil;
    std::string cleaned = filter;
    for (char& c : cleaned) {
        if (c == ',' || c == ';' || c == '|') c = ' ';
    }
    NSMutableArray* types = [NSMutableArray array];
    const char* p = cleaned.c_str();
    while (*p) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        const char* start = p;
        while (*p && *p != ' ' && *p != '\t') ++p;
        std::string token(start, static_cast<size_t>(p - start));
        if (token == "*" || token == "*.*") return nil;  // разрешаем всё
        if (token.rfind("*.", 0) == 0) token = token.substr(2);
        if (!token.empty()) {
            [types addObject:ToNSString(token)];
        }
    }
    return [types count] > 0 ? types : nil;
}

NSString* RunFilePanel(bool save, const std::string& title, const std::string& filterOrName) {
    NSSavePanel* panel = save ? [NSSavePanel savePanel] : (NSSavePanel*)[NSOpenPanel openPanel];
    if (!panel) return nil;
    if (!title.empty()) [panel setTitle:ToNSString(title)];
    if (save) {
        if (!filterOrName.empty()) [panel setNameFieldStringValue:ToNSString(filterOrName)];
    } else {
        NSArray* types = AllowedTypes(filterOrName);
        if (types) [panel setAllowedFileTypes:types];
        if ([panel isKindOfClass:[NSOpenPanel class]]) {
            [(NSOpenPanel*)panel setAllowsMultipleSelection:NO];
        }
    }
    if ([panel runModal] != NSModalResponseOK) return nil;
    NSURL* url = [panel URL];
    if (!url) return nil;
    return [url path];
}

// ---------------------------------------------------------------------------
// Помощник строк sysctl (бренд CPU, идентификаторы модели).
// ---------------------------------------------------------------------------
std::string SysctlString(const char* name) {
    size_t size = 0;
    if (sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0) return std::string();
    std::vector<char> buffer(size, '\0');
    if (sysctlbyname(name, buffer.data(), &size, nullptr, 0) != 0) return std::string();
    // Часть значений завершается NUL, часть — нет.
    if (size > 0 && buffer[size - 1] == '\0') buffer.resize(size - 1);
    return std::string(buffer.data());
}

// Состояние power assertion для SetKeepScreenAwake.
IOPMAssertionID g_sleepAssertion = kIOPMNullAssertionID;

// Показывает модальный NSAlert и ждёт пользователя.
void RunAlert(const std::string& title, const std::string& message, MessageBoxType type) {
    NSAlert* alert = [[NSAlert alloc] init];
    [alert setMessageText:ToNSString(title)];
    [alert setInformativeText:ToNSString(message)];
    switch (type) {
        case MessageBoxType::Warning: [alert setAlertStyle:NSAlertStyleWarning]; break;
        case MessageBoxType::Error: [alert setAlertStyle:NSAlertStyleCritical]; break;
        case MessageBoxType::Question: [alert setAlertStyle:NSAlertStyleInformational]; break;
        case MessageBoxType::Info:
        default: [alert setAlertStyle:NSAlertStyleInformational]; break;
    }
    [alert addButtonWithTitle:@"OK"];
    [alert runModal];
}

}  // namespace

// ---------------------------------------------------------------------------
// Идентификация
// ---------------------------------------------------------------------------
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
    std::string brand = SysctlString("machdep.cpu.brand_string");
    if (!brand.empty()) return brand;
    // Apple silicon экспонирует маркетинговый идентификатор через hw.model; имя
    // конкретного ядра доступно только через IORegistry, что потребовало бы
    // обхода устройств IOKit, поэтому используется идентификатор модели.
    std::string model = SysctlString("hw.model");
    if (!model.empty()) return model;
    return PlatformArch();
}

u64 TotalPhysicalMemory() {
    u64 memory = 0;
    size_t size = sizeof(memory);
    if (sysctlbyname("hw.memsize", &memory, &size, nullptr, 0) != 0) return 0;
    return memory;
}

int CpuCoreCount() {
    int count = 0;
    size_t size = sizeof(count);
    // Предпочитаем сумму performance+Efficiency; hw.activecpu — запасной вариант.
    if (sysctlbyname("hw.ncpu", &count, &size, nullptr, 0) == 0 && count > 0) return count;
    size = sizeof(count);
    if (sysctlbyname("hw.activecpu", &count, &size, nullptr, 0) == 0 && count > 0) return count;
    const long online = sysconf(_SC_NPROCESSORS_ONLN);
    return online > 0 ? static_cast<int>(online) : 1;
}

// ---------------------------------------------------------------------------
// Пути
// ---------------------------------------------------------------------------
std::string ExecutablePath() {
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);  // возвращает требуемый размер
    if (size == 0) return std::string();
    std::vector<char> buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) return std::string();
    // Путь может содержать ".."; разрешаем его в реальной файловой системе.
    char resolved[4096];
    if (realpath(buffer.data(), resolved)) return std::string(resolved);
    return std::string(buffer.data());
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
    usleep(static_cast<useconds_t>(milliseconds) * 1000u);
}

u64 CurrentThreadId() {
    // pthread_t непрозрачен; его байтовое представление — стабильный id потока.
    pthread_t self = pthread_self();
    u64 id = 0;
    static_assert(sizeof(id) >= sizeof(self), "pthread_t larger than u64");
    std::memcpy(&id, &self, sizeof(self));
    return id;
}

// ---------------------------------------------------------------------------
// Диалоги / оболочка
// ---------------------------------------------------------------------------
void ShowMessageBox(const std::string& title, const std::string& message, MessageBoxType type) {
    @autoreleasepool {
        if ([NSThread isMainThread]) {
            RunAlert(title, message, type);
        } else {
            // AppKit работает только в главном потоке: перепрыгиваем туда и ждём завершения.
            dispatch_sync(dispatch_get_main_queue(), ^{
                RunAlert(title, message, type);
            });
        }
    }
}

std::string OpenFileDialog(const std::string& title, const std::string& filter) {
    @autoreleasepool {
        // NSOpenPanel работает только в главном потоке; если он недоступен,
        // запускаем модально откуда есть.
        __block std::string result;
        if ([NSThread isMainThread]) {
            result = ToUtf8(RunFilePanel(false, title, filter));
        } else {
            dispatch_sync(dispatch_get_main_queue(), ^{
                result = ToUtf8(RunFilePanel(false, title, filter));
            });
        }
        return result;
    }
}

std::string SaveFileDialog(const std::string& title, const std::string& defaultName) {
    @autoreleasepool {
        __block std::string result;
        if ([NSThread isMainThread]) {
            result = ToUtf8(RunFilePanel(true, title, defaultName));
        } else {
            dispatch_sync(dispatch_get_main_queue(), ^{
                result = ToUtf8(RunFilePanel(true, title, defaultName));
            });
        }
        return result;
    }
}

bool OpenUrl(const std::string& url) {
    @autoreleasepool {
        NSURL* nsurl = [NSURL URLWithString:ToNSString(url)];
        if (!nsurl) return false;
        return [[NSWorkspace sharedWorkspace] openURL:nsurl] ? true : false;
    }
}

void GetScreenSize(int* w, int* h) {
    // Точки AppKit (не пиксели), что совпадает с Window::Width()/Height().
    @autoreleasepool {
        NSScreen* screen = [NSScreen mainScreen];
        if (screen) {
            const NSRect frame = [screen frame];
            if (w) *w = static_cast<int>(frame.size.width);
            if (h) *h = static_cast<int>(frame.size.height);
            return;
        }
    }
    // Нет экрана Cocoa (редко, например демон): откат к размеру дисплея CG.
    const CGDirectDisplayID display = CGMainDisplayID();
    if (w) *w = static_cast<int>(CGDisplayPixelsWide(display));
    if (h) *h = static_cast<int>(CGDisplayPixelsHigh(display));
}

// ---------------------------------------------------------------------------
// Заглушки мобильных/консольных функций. У macOS нет публичного API для вызова
// экранной клавиатуры (потребовались бы Accessibility API и доверенный процесс),
// поэтому это намеренный no-op. У десктопных Mac нет и вибрации.
// ---------------------------------------------------------------------------
void SetSoftKeyboardVisible(bool visible) {
    (void)visible;
    ENG_LOGD("platform", "SetSoftKeyboardVisible is a no-op on macOS");
}

void Vibrate(u32 milliseconds) {
    (void)milliseconds;
    ENG_LOGD("platform", "Vibrate is a no-op on macOS");
}

void SetKeepScreenAwake(bool enabled) {
    if (enabled) {
        if (g_sleepAssertion != kIOPMNullAssertionID) return;
        IOReturn rc = IOPMAssertionCreateWithName(kIOPMAssertionTypeNoDisplaySleep,
                                                  kIOPMAssertionLevelOn,
                                                  CFSTR("CrossRender keeps the display awake"),
                                                  &g_sleepAssertion);
        if (rc != kIOReturnSuccess) {
            g_sleepAssertion = kIOPMNullAssertionID;
            ENG_LOGW("platform", "SetKeepScreenAwake: IOPMAssertionCreateWithName failed (%d)",
                     static_cast<int>(rc));
        }
    } else {
        if (g_sleepAssertion == kIOPMNullAssertionID) return;
        IOPMAssertionRelease(g_sleepAssertion);
        g_sleepAssertion = kIOPMNullAssertionID;
    }
}

bool IsAppForeground() {
    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        if (!app) return false;
        return [app isActive] ? true : false;
    }
}

// ---------------------------------------------------------------------------
// Жизненный цикл приложения
// ---------------------------------------------------------------------------
int RunApp(const AppHooks& hooks) {
    WindowDesc desc;  // значения движка по умолчанию: 1280x720, оконный, vsync, core 3.3
    Window window;
    if (!window.Create(desc)) {
        ENG_LOGE("platform", "RunApp: window creation failed");
        return 1;
    }

    if (hooks.onInit) hooks.onInit(hooks.user);

    // Бюджет кадра не даёт циклу вращаться, когда vsync не ограничивает
    // (например, когда окно перекрыто в macOS).
    constexpr f32 kMaxFps = 1000.0f;
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
        }
        window.GetInput().EndFrame();
        if (window.ShouldClose()) break;
        window.SwapBuffers();

        if (dt > 0.0f && dt < 1.0f / kMaxFps) SleepMs(1);
    }

    if (hooks.onShutdown) hooks.onShutdown(hooks.user);
    window.Destroy();
    return 0;
}

}  // namespace crossrender

#endif  // ENG_PLATFORM_MACOS
