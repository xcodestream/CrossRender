// Сервисы платформы Android, объявленные в crossrender/platform/Platform.h.
//
// Половина native-activity (ANativeActivity_onCreate, цикл приложения, EGL, ввод,
// JNI-примитивы) живёт в WindowAndroid.cpp; эта единица трансляции реализует
// поверхность Platform.h поверх неё.
#include "crossrender/platform/Platform.h"

#include "crossrender/core/Log.h"
#include "crossrender/platform/Window.h"

#include "AndroidPlatform.h"

#if defined(ENG_PLATFORM_ANDROID)

#include <sys/resource.h>
#include <jni.h>
#include <unistd.h>
#include <EGL/egl.h>
#include <pthread.h>
#include <GLES3/gl3.h>
#include <EGL/eglext.h>
#include <android/log.h>
#include <sys/sysinfo.h>
#include <android/native_activity.h>

#include <cstdio>
#include <string>
#include <cstring>

namespace crossrender {
namespace {

// eglGetProcAddress резолвит только расширения, экспонируемые текущим
// контекстом; большинство точек входа движка приходят из статически
// слинкованной libGLESv3 (поэтому null-результат для них ожидаем и безвреден).
void* ResolveGL(const char* name) {
    void* fn = reinterpret_cast<void*>(eglGetProcAddress(name));
    return fn;
}

// Читает поле из /proc/cpuinfo (например "Hardware" на ARM, "model name" на
// эмуляторах Android x86).
std::string CpuInfoField(const char* key) {
    FILE* file = std::fopen("/proc/cpuinfo", "r");
    if (!file) return {};
    char line[512];
    std::string result;
    const size_t keyLen = std::strlen(key);
    while (std::fgets(line, sizeof(line), file)) {
        if (std::strncmp(line, key, keyLen) == 0) {
            const char* colon = std::strchr(line, ':');
            if (!colon) continue;
            const char* value = colon + 1;
            while (*value == ' ' || *value == '\t') ++value;
            size_t len = std::strlen(value);
            while (len > 0 && (value[len - 1] == '\n' || value[len - 1] == '\r' ||
                               value[len - 1] == ' ')) {
                --len;
            }
            result.assign(value, len);
            if (!result.empty()) break;
        }
    }
    std::fclose(file);
    return result;
}

}  // namespace

// ---------------------------------------------------------------------------
// Жизненный цикл платформы (объявлен в crossrender/platform/Window.h)
// ---------------------------------------------------------------------------
bool PlatformInit() {
    ENG_LOGI("platform", "Android platform initialised (sdk %d)",
             AndroidActivity() ? static_cast<int>(AndroidActivity()->sdkVersion) : -1);
    return true;
}

void PlatformShutdown() {}

std::string PlatformName() { return "Android"; }

void* AndroidGLGetProcAddress(const char* name) { return ResolveGL(name); }

void* (*PlatformGLGetProcAddress())(const char*) { return &AndroidGLGetProcAddress; }

// ---------------------------------------------------------------------------
// Диалоги / оболочка
// ---------------------------------------------------------------------------
void ShowMessageBox(const std::string& title, const std::string& message, MessageBoxType type) {
    // Синхронного нативного диалога из нативного кода не достичь (AlertDialog
    // пришлось бы публиковать в UI-поток, и он асинхронен),
    // поэтому пишем в logcat, как указано в задаче.
    static const char* const kTypes[] = {"info", "warning", "error", "question"};
    const int index = static_cast<int>(type);
    const char* kind = (index >= 0 && index < 4) ? kTypes[index] : "info";
    __android_log_print(ANDROID_LOG_INFO, "CrossRender", "[%s] %s: %s", kind, title.c_str(),
                        message.c_str());
    ENG_LOGI("platform", "message box [%s] %s: %s", kind, title.c_str(), message.c_str());
}

// В NDK Android нет синхронного выбора файлов (Java-стороне потребовался бы
// колбэк Activity Result), поэтому оба диалога сообщают об «отмене».
std::string OpenFileDialog(const std::string& title, const std::string& filter) {
    ENG_LOGW("platform", "OpenFileDialog('%s', '%s') is not implemented on Android", title.c_str(),
             filter.c_str());
    return {};
}

std::string SaveFileDialog(const std::string& title, const std::string& defaultName) {
    ENG_LOGW("platform", "SaveFileDialog('%s', '%s') is not implemented on Android",
             title.c_str(), defaultName.c_str());
    return {};
}

bool OpenUrl(const std::string& url) {
    ANativeActivity* activity = AndroidActivity();
    if (!activity || !activity->vm || !activity->clazz || url.empty()) {
        ENG_LOGW("platform", "OpenUrl('%s'): no activity/JNIEnv", url.c_str());
        return false;
    }
    JavaVM* vm = activity->vm;
    JNIEnv* env = nullptr;
    bool attached = false;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return false;
        attached = true;
    }
    bool ok = false;
    jclass intentClass = env->FindClass("android/content/Intent");
    jmethodID intentCtor = intentClass
                               ? env->GetMethodID(intentClass, "<init>",
                                                  "(Ljava/lang/String;Landroid/net/Uri;)V")
                               : nullptr;
    jclass uriClass = env->FindClass("android/net/Uri");
    jmethodID parse = uriClass ? env->GetStaticMethodID(uriClass, "parse",
                                                        "(Ljava/lang/String;)Landroid/net/Uri;")
                               : nullptr;
    if (intentCtor && parse) {
        jstring action = env->NewStringUTF("android.intent.action.VIEW");
        jstring urlString = env->NewStringUTF(url.c_str());
        jobject uri = env->CallStaticObjectMethod(uriClass, parse, urlString);
        jobject intent = env->NewObject(intentClass, intentCtor, action, uri);
        jclass activityClass = env->GetObjectClass(activity->clazz);
        jmethodID startActivity =
            activityClass
                ? env->GetMethodID(activityClass, "startActivity", "(Landroid/content/Intent;)V")
                : nullptr;
        if (intent && startActivity) {
            env->CallVoidMethod(activity->clazz, startActivity, intent);
            ok = !env->ExceptionCheck();
        }
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            ENG_LOGW("platform", "OpenUrl: no activity handles '%s'", url.c_str());
        }
        if (activityClass) env->DeleteLocalRef(activityClass);
        if (intent) env->DeleteLocalRef(intent);
        if (uri) env->DeleteLocalRef(uri);
        if (urlString) env->DeleteLocalRef(urlString);
        if (action) env->DeleteLocalRef(action);
    }
    if (uriClass) env->DeleteLocalRef(uriClass);
    if (intentClass) env->DeleteLocalRef(intentClass);
    if (attached) vm->DetachCurrentThread();
    return ok;
}

void GetScreenSize(int* w, int* h) { AndroidScreenSizeDp(w, h); }

// ---------------------------------------------------------------------------
// Сведения о процессе / системе
// ---------------------------------------------------------------------------
std::string PlatformArch() {
#if defined(__aarch64__)
    return "arm64";
#elif defined(__arm__)
    return "armv7";
#elif defined(__x86_64__)
    return "x86_64";
#elif defined(__i386__)
    return "x86";
#else
    return "unknown";
#endif
}

std::string CpuName() {
    std::string name = CpuInfoField("Hardware");
    if (name.empty()) name = CpuInfoField("model name");
    if (name.empty()) name = CpuInfoField("Processor");
    if (name.empty()) name = PlatformArch();
    return name;
}

u64 TotalPhysicalMemory() {
    // sysconf() возвращает страницы; в Android есть и sysinfo(), дающая реальный
    // общий объём RAM на устройствах, где _SC_PHYS_PAGES ограничено.
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long pageSize = sysconf(_SC_PAGESIZE);
    if (pages > 0 && pageSize > 0) {
        return static_cast<u64>(pages) * static_cast<u64>(pageSize);
    }
    struct sysinfo info;
    if (sysinfo(&info) == 0) {
        return static_cast<u64>(info.totalram) * static_cast<u64>(info.mem_unit);
    }
    return 0;
}

int CpuCoreCount() {
    const long cores = sysconf(_SC_NPROCESSORS_ONLN);
    return cores > 0 ? static_cast<int>(cores) : 1;
}

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
    // Приложение Android — разделяемая библиотека, загружаемая zygote; осмысленного
    // пути исполняемого файла нет. Корни ресурсов/пользователя движок настраивает
    // из ANativeActivity::internalDataPath.
    return {};
}

std::string ExecutableDir() {
    const char* path = AndroidInternalDataPath();
    if (path && *path) return std::string(path);
    return "/";
}

// ---------------------------------------------------------------------------
// Время / потоки
// ---------------------------------------------------------------------------
void SleepMs(u32 milliseconds) {
    if (milliseconds == 0) return;
    usleep(static_cast<useconds_t>(milliseconds) * 1000u);
}

u64 CurrentThreadId() {
    // bionic экспонирует gettid() напрямую.
    return static_cast<u64>(static_cast<unsigned long>(gettid()));
}

// ---------------------------------------------------------------------------
// Мобильные сервисы
// ---------------------------------------------------------------------------
void SetSoftKeyboardVisible(bool visible) { AndroidSetSoftKeyboardVisible(visible); }

void SetKeepScreenAwake(bool enabled) { AndroidSetKeepScreenOn(enabled); }

void Vibrate(u32 milliseconds) { AndroidPlatformVibrate(milliseconds); }

bool IsAppForeground() { return AndroidAppIsForeground(); }

int RunApp(const AppHooks& hooks) {
    // Прокручивает AppHooks из цикла native activity (см. WindowAndroid.cpp).
    return RunAppAndroid(hooks);
}

// ---------------------------------------------------------------------------
// Headless GL-контекст.
//
// Практически в Android нет EGL-контекста без поверхности, но pbuffer входит
// в EGL 1.4 и поддерживается большинством драйверов: пробуем его и сообщаем
// о неудаче (с логом), если устройство откажет.
// ---------------------------------------------------------------------------
namespace {
EGLDisplay g_headlessDisplay = EGL_NO_DISPLAY;
EGLContext g_headlessContext = EGL_NO_CONTEXT;
EGLSurface g_headlessSurface = EGL_NO_SURFACE;
int g_headlessRefCount = 0;
}  // namespace

bool CreateHeadlessGLContext() {
    if (g_headlessContext != EGL_NO_CONTEXT) {
        ++g_headlessRefCount;
        eglMakeCurrent(g_headlessDisplay, g_headlessSurface, g_headlessSurface, g_headlessContext);
        return true;
    }
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) {
        ENG_LOGW("platform", "headless GL unavailable: eglGetDisplay failed");
        return false;
    }
    EGLint major = 0;
    EGLint minor = 0;
    if (!eglInitialize(display, &major, &minor)) {
        ENG_LOGW("platform", "headless GL unavailable: eglInitialize failed (0x%04x)",
                 eglGetError());
        return false;
    }
    const EGLint configAttribs[] = {
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_DEPTH_SIZE,      24,
        EGL_STENCIL_SIZE,    8,
        EGL_NONE,
    };
    EGLConfig config = nullptr;
    EGLint numConfigs = 0;
    if (!eglChooseConfig(display, configAttribs, &config, 1, &numConfigs) || numConfigs < 1) {
        ENG_LOGW("platform", "headless GL unavailable: no pbuffer ES3 config (0x%04x)",
                 eglGetError());
        eglTerminate(display);
        return false;
    }
    const EGLint surfaceAttribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display, config, surfaceAttribs);
    if (surface == EGL_NO_SURFACE) {
        ENG_LOGW("platform", "headless GL unavailable: eglCreatePbufferSurface failed (0x%04x)",
                 eglGetError());
        eglTerminate(display);
        return false;
    }
    const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttribs);
    if (context == EGL_NO_CONTEXT) {
        ENG_LOGW("platform", "headless GL unavailable: eglCreateContext failed (0x%04x)",
                 eglGetError());
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return false;
    }
    if (!eglMakeCurrent(display, surface, surface, context)) {
        ENG_LOGW("platform", "headless GL unavailable: eglMakeCurrent failed (0x%04x)",
                 eglGetError());
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return false;
    }
    g_headlessDisplay = display;
    g_headlessContext = context;
    g_headlessSurface = surface;
    g_headlessRefCount = 1;
    ENG_LOGI("platform", "headless EGL pbuffer context created (ES3, 16x16)");
    return true;
}

void DestroyHeadlessGLContext() {
    if (g_headlessContext == EGL_NO_CONTEXT) return;
    if (--g_headlessRefCount > 0) return;
    eglMakeCurrent(g_headlessDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(g_headlessDisplay, g_headlessContext);
    if (g_headlessSurface != EGL_NO_SURFACE) eglDestroySurface(g_headlessDisplay, g_headlessSurface);
    eglTerminate(g_headlessDisplay);
    g_headlessDisplay = EGL_NO_DISPLAY;
    g_headlessContext = EGL_NO_CONTEXT;
    g_headlessSurface = EGL_NO_SURFACE;
}

bool HasHeadlessGLContext() { return g_headlessContext != EGL_NO_CONTEXT; }

void* HeadlessGLGetProcAddress(const char* name) { return AndroidGLGetProcAddress(name); }

}  // namespace crossrender

#else

// В сборках без Android эта единица трансляции пуста.
namespace crossrender {}

#endif  // ENG_PLATFORM_ANDROID
