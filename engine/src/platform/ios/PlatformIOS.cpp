// Сервисы платформы iOS, объявленные в crossrender/platform/Platform.h.
//
// Это обычная единица трансляции C++: Objective-C половина слоя iOS
// (UIWindow, UIAlertController, UIPasteboard, ...) живёт в WindowIOS.mm и
// достигается через небольшой C-мост, объявленный в IOSPlatform.h.
#include "crossrender/platform/Platform.h"

#include "crossrender/core/Log.h"
#include "crossrender/platform/Window.h"

#include "IOSPlatform.h"

#if defined(ENG_PLATFORM_IOS)

#include <time.h>
#include <dlfcn.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/sysctl.h>
#include <mach-o/dyld.h>
#include <AudioToolbox/AudioServices.h>

#include <string>
#include <cstring>

namespace crossrender {
namespace {

// Читает sysctl по имени в `out`; возвращает false, если ключа нет.
bool SysctlString(const char* name, std::string* out) {
    size_t size = 0;
    if (sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0) return false;
    std::string buffer(size, '\0');
    if (sysctlbyname(name, &buffer[0], &size, nullptr, 0) != 0) return false;
    buffer.resize(std::strlen(buffer.c_str()));
    *out = buffer;
    return true;
}

bool SysctlU64(const char* name, u64* out) {
    u64 value = 0;
    size_t size = sizeof(value);
    if (sysctlbyname(name, &value, &size, nullptr, 0) != 0) return false;
    *out = value;
    return true;
}

void* ResolveDlAddr(const char* name) {
    void* fn = dlsym(RTLD_DEFAULT, name);
    if (fn) return fn;
    // Фреймворк OpenGL ES слинкован с приложением, но dlopen сохраняет
    // работоспособность для тулинга, линкующего движок без -framework OpenGLES.
    static void* handle = dlopen(
        "/System/Library/Frameworks/OpenGLES.framework/OpenGLES", RTLD_LAZY | RTLD_NOLOAD);
    if (!handle) handle = dlopen("/System/Library/Frameworks/OpenGLES.framework/OpenGLES", RTLD_LAZY);
    return handle ? dlsym(handle, name) : nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// Жизненный цикл платформы (объявлен в crossrender/platform/Window.h)
// ---------------------------------------------------------------------------
bool PlatformInit() {
    ENG_LOGI("platform", "iOS platform initialised");
    return true;
}

void PlatformShutdown() {}

std::string PlatformName() { return "iOS"; }

void* IOSGLGetProcAddress(const char* name) { return ResolveDlAddr(name); }

void* (*PlatformGLGetProcAddress())(const char*) { return &IOSGLGetProcAddress; }

// ---------------------------------------------------------------------------
// Диалоги / оболочка
// ---------------------------------------------------------------------------
void ShowMessageBox(const std::string& title, const std::string& message, MessageBoxType type) {
    // Реализовано через UIAlertController в WindowIOS.mm. Учтите, что
    // UIAlertController асинхронен: в отличие от десктопных реализаций этот
    // вызов НЕ блокируется до закрытия диалога пользователем.
    eng_ios_show_message_box(title.c_str(), message.c_str(), static_cast<int>(type));
}

// Без сессии UIDocumentPickerViewController модального выбора файлов в iOS нет;
// движок его не поставляет, поэтому оба диалога сообщают об «отмене».
std::string OpenFileDialog(const std::string& title, const std::string& filter) {
    ENG_LOGW("platform", "OpenFileDialog('%s', '%s') is not implemented on iOS", title.c_str(),
             filter.c_str());
    return {};
}

std::string SaveFileDialog(const std::string& title, const std::string& defaultName) {
    ENG_LOGW("platform", "SaveFileDialog('%s', '%s') is not implemented on iOS", title.c_str(),
             defaultName.c_str());
    return {};
}

bool OpenUrl(const std::string& url) { return eng_ios_open_url(url.c_str()); }

void GetScreenSize(int* w, int* h) { eng_ios_get_screen_size(w, h); }

// ---------------------------------------------------------------------------
// Сведения о процессе / системе
// ---------------------------------------------------------------------------
std::string PlatformArch() {
#if defined(__aarch64__) || defined(__arm64__)
    return "arm64";
#elif defined(__arm__)
    return "armv7";
#elif defined(__x86_64__)
    return "x86_64";  // симулятор
#else
    return "unknown";
#endif
}

std::string CpuName() {
    // iOS экспонирует идентификатор SoC ("iPhone15,2"), а не бренд CPU.
    std::string machine;
    if (SysctlString("hw.machine", &machine) && !machine.empty()) return machine;
    if (SysctlString("hw.model", &machine) && !machine.empty()) return machine;
    return "Apple";
}

u64 TotalPhysicalMemory() {
    u64 bytes = 0;
    if (SysctlU64("hw.memsize", &bytes)) return bytes;
    // Откат к оценке по страницам, если sysctl недоступен.
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long pageSize = sysconf(_SC_PAGESIZE);
    if (pages > 0 && pageSize > 0) return static_cast<u64>(pages) * static_cast<u64>(pageSize);
    return 0;
}

int CpuCoreCount() {
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? static_cast<int>(n) : 1;
}

std::string ExecutablePath() {
    char buffer[4096];
    u32 size = static_cast<u32>(sizeof(buffer));
    if (_NSGetExecutablePath(buffer, &size) != 0) return {};
    buffer[sizeof(buffer) - 1] = '\0';
    // Разрешаем компоненты ".."; неудача не фатальна (путь информационный).
    char resolved[4096];
    if (realpath(buffer, resolved) != nullptr) return std::string(resolved);
    return std::string(buffer);
}

std::string ExecutableDir() {
    const std::string path = ExecutablePath();
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return path;
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

// ---------------------------------------------------------------------------
// Время / потоки
// ---------------------------------------------------------------------------
void SleepMs(u32 milliseconds) {
    if (milliseconds == 0) return;
    struct timespec ts;
    ts.tv_sec = static_cast<time_t>(milliseconds / 1000u);
    ts.tv_nsec = static_cast<long>(milliseconds % 1000u) * 1000000L;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
    }
}

u64 CurrentThreadId() {
    u64 tid = 0;
    if (pthread_threadid_np(nullptr, &tid) == 0) return tid;
    return static_cast<u64>(reinterpret_cast<uintptr_t>(pthread_self()));
}

// ---------------------------------------------------------------------------
// Мобильные сервисы
// ---------------------------------------------------------------------------
void SetSoftKeyboardVisible(bool visible) { eng_ios_set_soft_keyboard(visible); }

void SetKeepScreenAwake(bool enabled) { eng_ios_set_keep_awake(enabled); }

void Vibrate(u32 milliseconds) {
    // В iOS вибрация одной фиксированной длительности; запрошенная длительность
    // игнорируется (задокументировано).
    (void)milliseconds;
    AudioServicesPlaySystemSound(kSystemSoundID_Vibrate);
}

bool IsAppForeground() { return eng_ios_is_app_foreground(); }

int RunApp(const AppHooks& hooks) {
    // UIApplicationMain() можно запустить только из Objective-C++; run loop
    // и прокрутка кадров через CADisplayLink живут в WindowIOS.mm.
    return RunAppIOS(hooks);
}

// ---------------------------------------------------------------------------
// Headless GL-контекст.
//
// EAGL нужен CAEAGLLayer с drawable, а iOS не может создать pbuffer или
// offscreen-поверхность EAGL без него, поэтому headless GL на iOS нет.
//
// Эти четыре функции намеренно `weak`: корневой CMakeLists.txt также компилирует
// engine/src/platform/macos/WindowMacOS.mm в цель iOS (glob IOS перечисляет
// ${CR_SRC_DIR}/platform/macos/*.mm), а ветка #else того файла уже определяет
// ту же четвёрку сильными символами. Два сильных определения не слинковались бы;
// с weak-определением Mach-O сегодня оставляет сильное и начнёт вызывать эти
// (с логом-предупреждением), как только macos-заглушка опустеет или glob CMake
// перестанет тянуть macos/*.mm в сборку iOS.
// ---------------------------------------------------------------------------
__attribute__((weak)) bool CreateHeadlessGLContext() {
    ENG_LOGW("platform", "headless GL is unavailable on iOS (no offscreen EAGL surface)");
    return false;
}

__attribute__((weak)) void DestroyHeadlessGLContext() {}

__attribute__((weak)) bool HasHeadlessGLContext() { return false; }

__attribute__((weak)) void* HeadlessGLGetProcAddress(const char* name) {
    (void)name;
    ENG_LOGW("platform", "headless GL is unavailable on iOS");
    return nullptr;
}

}  // namespace crossrender

#else

// В чужих сборках эта единица трансляции компилируется в пустоту.
namespace crossrender {}

#endif  // ENG_PLATFORM_IOS
