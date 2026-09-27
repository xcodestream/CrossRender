// Платформенный слой Android: точка входа NativeActivity с нуля плюс командный
// цикл в духе android_native_app_glue, контекст EGL/OpenGL ES 3, обработка
// ввода и сервисы на базе JNI.
//
// Библиотека glue (libandroid_native_app_glue / <android_native_app_glue.h>) намеренно
// НЕ используется: этот файл реализует тот же дизайн сам —
//   * ANativeActivity_onCreate устанавливает ANativeActivityCallbacks и порождает
//     поток приложения,
//   * колбэки переправляют работу в этот поток через pipe (тот же дизайн
//     msgwrite/msgread, что у glue), поэтому вся работа GL и ввода идёт
//     в одном потоке,
//   * поток приложения прокручивает ALooper с fd активности + AInputQueue и
//     движет crossrender::AppHooks (RunApp) или crossrender::Window::PollEvents (Engine::Run).
#include "crossrender/platform/Window.h"

#include "crossrender/core/Log.h"
#include "crossrender/core/Time.h"
#include "crossrender/platform/Platform.h"

#include "AndroidPlatform.h"

#if defined(ENG_PLATFORM_ANDROID)

#include <jni.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <EGL/egl.h>
#include <pthread.h>
#include <GLES3/gl3.h>
#include <EGL/eglext.h>
#include <android/input.h>
#include <android/looper.h>
#include <android/keycodes.h>
#include <android/configuration.h>
#include <android/native_window.h>
#include <android/native_activity.h>

#include <mutex>
#include <cerrno>
#include <chrono>
#include <string>
#include <thread>
#include <cstring>
#include <condition_variable>

// Точка входа приложения. Приложение Android определяет этот символ
// (extern "C" void eng_android_main();) и обычно вызывает из него crossrender::RunApp().
// Он объявлен weak, чтобы платформенный слой линковался — и продолжал обслуживать
// жизненный цикл activity — для приложений без такой точки входа.
extern "C" void EngAndroidMain() __attribute__((weak));

namespace crossrender {
namespace {

constexpr int kLooperIdMain = 1;   // команды activity (pipe)
constexpr int kLooperIdInput = 2;  // AInputQueue
constexpr f32 kFrameBudgetS = 1.0f / 60.0f;

// Id команд, отправляемых колбэками ANativeActivity в поток приложения.
enum AppCmd : int {
    kCmdStart = 1,
    kCmdResume,
    kCmdPause,
    kCmdStop,
    kCmdDestroy,
    kCmdWindowCreated,
    kCmdWindowDestroyed,
    kCmdWindowResized,
    kCmdWindowRedraw,
    kCmdFocusGained,
    kCmdFocusLost,
    kCmdInputCreated,
    kCmdInputDestroyed,
    kCmdConfigChanged,
    kCmdLowMemory,
    kCmdContentRect,
};

struct AppState {
    ANativeActivity* activity = nullptr;
    AConfiguration* config = nullptr;

    std::mutex mutex;  // охраняет слоты pending*, окно и флаги фокуса/resume
    std::condition_variable cv;
    // Слоты, заполняемые колбэками и потребляемые потоком приложения.
    ANativeWindow* pendingWindowCreated = nullptr;
    ANativeWindow* pendingWindowDestroyed = nullptr;
    AInputQueue* pendingInputCreated = nullptr;
    AInputQueue* pendingInputDestroyed = nullptr;
    bool ackWindowDestroyed = false;
    bool ackInputDestroyed = false;

    ANativeWindow* window = nullptr;  // наш (acquire), пока не null
    AInputQueue* attachedInput = nullptr;
    int windowWidth = 0;
    int windowHeight = 0;
    bool hasFocus = false;
    bool resumed = false;
    bool destroyed = false;
    bool loopActive = false;       // поток находится внутри PumpOnce/цикла
    bool loopFinished = false;     // поток приложения покинул свой цикл
    bool lowMemory = false;
    bool quitRequested = false;
    f32 density = 1.0f;
    std::string internalDataPath;

    int msgPipe[2] = {-1, -1};
    ALooper* looper = nullptr;
    bool pipeRegistered = false;

    std::thread thread;

    // AppHooks, переданные в RunApp() (регистрируются RunAppAndroid).
    AppHooks hooks;
    bool hooksValid = false;
    bool hooksInitialised = false;
    bool shutdownCalled = false;

    // Состояние EGL (трогается только прокачивающим потоком).
    EGLDisplay eglDisplay = EGL_NO_DISPLAY;
    EGLConfig eglConfig = nullptr;
    EGLContext eglContext = EGL_NO_CONTEXT;
    EGLSurface eglSurface = EGL_NO_SURFACE;
    bool glReady = false;
};

AppState* g_app = nullptr;
Window* g_activeWindow = nullptr;
// Увеличивается Window::SwapBuffers(), чтобы цикл RunApp показывал кадр за
// приложение, только когда onFrame() сам не показал (onFrame, вызывающий
// Engine::Step(), уже делает swap в конце кадра; второй
// eglSwapBuffers стоил бы целый vsync).
u32 g_swapCount = 0;

// ---------------------------------------------------------------------------
// Малые помощники
// ---------------------------------------------------------------------------
void PostCommand(AppState* app, int cmd) {
    if (!app || app->msgPipe[1] < 0) return;
    // Записывающий конец остаётся блокирующим: полный pipe означает, что поток
    // приложения заклинило; блокировка здесь — то же поведение, что у glue.
    const char* bytes = reinterpret_cast<const char*>(&cmd);
    size_t remaining = sizeof(cmd);
    while (remaining > 0) {
        const ssize_t n = ::write(app->msgPipe[1], bytes, remaining);
        if (n > 0) {
            bytes += n;
            remaining -= static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        ENG_LOGW("platform", "failed to post activity command %d (errno %d)", cmd, errno);
        return;
    }
}

float DensityFromConfig(AConfiguration* config) {
    if (!config) return 1.0f;
    const int32_t density = AConfiguration_getDensity(config);
    if (density <= 0 || density == ACONFIGURATION_DENSITY_ANY ||
        density == ACONFIGURATION_DENSITY_NONE) {
        return 1.0f;
    }
    return static_cast<f32>(density) / 160.0f;  // dp -> px
}

// ---------------------------------------------------------------------------
// Помощники JNI. Каждый вызов считает null env признаком «недоступно».
// ---------------------------------------------------------------------------
JNIEnv* AcquireEnv(bool* didAttach) {
    if (didAttach) *didAttach = false;
    if (!g_app || !g_app->activity || !g_app->activity->vm) return nullptr;
    JavaVM* vm = g_app->activity->vm;
    JNIEnv* env = nullptr;
    const jint status = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (status == JNI_OK && env) return env;
    if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
    if (didAttach) *didAttach = true;
    return env;
}

void ReleaseEnv(bool didAttach) {
    if (didAttach && g_app && g_app->activity && g_app->activity->vm) {
        g_app->activity->vm->DetachCurrentThread();
    }
}

bool QueryDisplayMetrics(int* outWidth, int* outHeight, float* outDensity) {
    bool attached = false;
    JNIEnv* env = AcquireEnv(&attached);
    if (!env || !g_app->activity) return false;
    bool ok = false;
    jobject activityObj = g_app->activity->clazz;
    jclass activityClass = env->GetObjectClass(activityObj);
    if (!activityClass) {
        ReleaseEnv(attached);
        return false;
    }
    jmethodID getResources =
        env->GetMethodID(activityClass, "getResources", "()Landroid/content/res/Resources;");
    jobject resources =
        getResources ? env->CallObjectMethod(activityObj, getResources) : nullptr;
    jclass resourcesClass = resources ? env->GetObjectClass(resources) : nullptr;
    jmethodID getMetrics = resourcesClass
                               ? env->GetMethodID(resourcesClass, "getDisplayMetrics",
                                                  "()Landroid/util/DisplayMetrics;")
                               : nullptr;
    jobject metrics = getMetrics ? env->CallObjectMethod(resources, getMetrics) : nullptr;
    jclass metricsClass = metrics ? env->GetObjectClass(metrics) : nullptr;
    if (metricsClass && !env->ExceptionCheck()) {
        jfieldID widthField = env->GetFieldID(metricsClass, "widthPixels", "I");
        jfieldID heightField = env->GetFieldID(metricsClass, "heightPixels", "I");
        jfieldID densityField = env->GetFieldID(metricsClass, "density", "F");
        if (widthField && heightField) {
            const jint w = env->GetIntField(metrics, widthField);
            const jint h = env->GetIntField(metrics, heightField);
            const jfloat d = densityField ? env->GetFloatField(metrics, densityField) : 0.0f;
            if (outWidth) *outWidth = static_cast<int>(w);
            if (outHeight) *outHeight = static_cast<int>(h);
            if (outDensity && d > 0.0f) *outDensity = static_cast<float>(d);
            ok = w > 0 && h > 0;
        }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (metrics) env->DeleteLocalRef(metrics);
    if (resources) env->DeleteLocalRef(resources);
    if (activityClass) env->DeleteLocalRef(activityClass);
    ReleaseEnv(attached);
    return ok;
}

// Вызывает Activity.getSystemService(name) и возвращает объект сервиса.
jobject GetSystemService(JNIEnv* env, const char* name) {
    if (!env || !g_app || !g_app->activity) return nullptr;
    jobject activityObj = g_app->activity->clazz;
    jclass activityClass = env->GetObjectClass(activityObj);
    if (!activityClass) return nullptr;
    jmethodID getService = env->GetMethodID(activityClass, "getSystemService",
                                            "(Ljava/lang/String;)Ljava/lang/Object;");
    jobject service = nullptr;
    if (getService) {
        jstring jname = env->NewStringUTF(name);
        if (jname) {
            service = env->CallObjectMethod(activityObj, getService, jname);
            env->DeleteLocalRef(jname);
        }
    }
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        ENG_LOGW("platform", "getSystemService('%s') threw", name);
    }
    env->DeleteLocalRef(activityClass);
    return service;
}

std::string JavaStringToUtf8(JNIEnv* env, jstring value) {
    if (!env || !value) return {};
    const char* chars = env->GetStringUTFChars(value, nullptr);
    std::string out = chars ? chars : "";
    if (chars) env->ReleaseStringUTFChars(value, chars);
    return out;
}

// ---------------------------------------------------------------------------
// Отображение клавиш: полная таблица AKEYCODE_* -> crossrender::Key.
// ---------------------------------------------------------------------------
Key KeyFromAndroidKeyCode(int32_t code) {
    // Буквы (AKEYCODE_A..AKEYCODE_Z) и цифры (AKEYCODE_0..AKEYCODE_9).
    if (code >= AKEYCODE_A && code <= AKEYCODE_Z)
        return static_cast<Key>(static_cast<int>(Key::A) + (code - AKEYCODE_A));
    if (code >= AKEYCODE_0 && code <= AKEYCODE_9)
        return static_cast<Key>(static_cast<int>(Key::Num0) + (code - AKEYCODE_0));
    // Кнопки геймпада откатываются к Unknown (геймпады движок опрашивает отдельно).
    if (code >= AKEYCODE_F1 && code <= AKEYCODE_F12)
        return static_cast<Key>(static_cast<int>(Key::F1) + (code - AKEYCODE_F1));
    if (code >= AKEYCODE_NUMPAD_0 && code <= AKEYCODE_NUMPAD_9)
        return static_cast<Key>(static_cast<int>(Key::Keypad0) + (code - AKEYCODE_NUMPAD_0));
    switch (code) {
        case AKEYCODE_DPAD_UP: return Key::Up;
        case AKEYCODE_DPAD_DOWN: return Key::Down;
        case AKEYCODE_DPAD_LEFT: return Key::Left;
        case AKEYCODE_DPAD_RIGHT: return Key::Right;
        case AKEYCODE_DPAD_CENTER: return Key::Enter;
        case AKEYCODE_ALT_LEFT: return Key::LeftAlt;
        case AKEYCODE_ALT_RIGHT: return Key::RightAlt;
        case AKEYCODE_SHIFT_LEFT: return Key::LeftShift;
        case AKEYCODE_SHIFT_RIGHT: return Key::RightShift;
        case AKEYCODE_CTRL_LEFT: return Key::LeftControl;
        case AKEYCODE_CTRL_RIGHT: return Key::RightControl;
        case AKEYCODE_META_LEFT: return Key::LeftSuper;
        case AKEYCODE_META_RIGHT: return Key::RightSuper;
        case AKEYCODE_TAB: return Key::Tab;
        case AKEYCODE_SPACE: return Key::Space;
        case AKEYCODE_ENTER: return Key::Enter;
        case AKEYCODE_NUMPAD_ENTER: return Key::KeypadEnter;
        case AKEYCODE_DEL: return Key::Backspace;
        case AKEYCODE_FORWARD_DEL: return Key::Delete;
        case AKEYCODE_ESCAPE: return Key::Escape;
        case AKEYCODE_GRAVE: return Key::Grave;
        case AKEYCODE_MINUS: return Key::Minus;
        case AKEYCODE_EQUALS: return Key::Equal;
        case AKEYCODE_LEFT_BRACKET: return Key::LeftBracket;
        case AKEYCODE_RIGHT_BRACKET: return Key::RightBracket;
        case AKEYCODE_BACKSLASH: return Key::Backslash;
        case AKEYCODE_SEMICOLON: return Key::Semicolon;
        case AKEYCODE_APOSTROPHE: return Key::Apostrophe;
        case AKEYCODE_COMMA: return Key::Comma;
        case AKEYCODE_PERIOD: return Key::Period;
        case AKEYCODE_SLASH: return Key::Slash;
        case AKEYCODE_PLUS: return Key::Equal;
        case AKEYCODE_AT: return Key::Num2;
        case AKEYCODE_STAR: return Key::Num8;
        case AKEYCODE_POUND: return Key::Num3;
        case AKEYCODE_CAPS_LOCK: return Key::CapsLock;
        case AKEYCODE_SCROLL_LOCK: return Key::ScrollLock;
        case AKEYCODE_NUM_LOCK: return Key::NumLock;
        case AKEYCODE_SYSRQ: return Key::PrintScreen;
        case AKEYCODE_BREAK: return Key::Pause;
        case AKEYCODE_INSERT: return Key::Insert;
        case AKEYCODE_MOVE_HOME: return Key::Home;
        case AKEYCODE_MOVE_END: return Key::End;
        case AKEYCODE_PAGE_UP: return Key::PageUp;
        case AKEYCODE_PAGE_DOWN: return Key::PageDown;
        case AKEYCODE_NUMPAD_DIVIDE: return Key::KeypadDivide;
        case AKEYCODE_NUMPAD_MULTIPLY: return Key::KeypadMultiply;
        case AKEYCODE_NUMPAD_SUBTRACT: return Key::KeypadSubtract;
        case AKEYCODE_NUMPAD_ADD: return Key::KeypadAdd;
        case AKEYCODE_NUMPAD_DOT: return Key::KeypadDecimal;
        case AKEYCODE_NUMPAD_COMMA: return Key::KeypadDecimal;
        case AKEYCODE_NUMPAD_EQUALS: return Key::KeypadEqual;
        case AKEYCODE_MENU: return Key::Menu;
        // Системные клавиши (HOME/BACK/VOLUME) намеренно не отображаются, чтобы
        // сохранить поведение по умолчанию; см. IsSystemKey ниже.
        default: return Key::Unknown;
    }
}

bool IsSystemKey(int32_t code) {
    switch (code) {
        case AKEYCODE_HOME:
        case AKEYCODE_BACK:
        case AKEYCODE_VOLUME_UP:
        case AKEYCODE_VOLUME_DOWN:
        case AKEYCODE_VOLUME_MUTE:
        case AKEYCODE_POWER:
        case AKEYCODE_CAMERA:
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// EGL
// ---------------------------------------------------------------------------
bool InitEGLDisplay(AppState* app) {
    if (app->eglDisplay != EGL_NO_DISPLAY) return true;
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) {
        ENG_LOGE("platform", "eglGetDisplay failed (0x%04x)", eglGetError());
        return false;
    }
    EGLint major = 0;
    EGLint minor = 0;
    if (!eglInitialize(display, &major, &minor)) {
        ENG_LOGE("platform", "eglInitialize failed (0x%04x)", eglGetError());
        return false;
    }
    const EGLint configAttribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_DEPTH_SIZE,      24,
        EGL_STENCIL_SIZE,    8,
        EGL_NONE,
    };
    EGLint numConfigs = 0;
    if (!eglChooseConfig(display, configAttribs, &app->eglConfig, 1, &numConfigs) ||
        numConfigs < 1) {
        // Повтор без depth/stencil: некоторые устройства капризничают с ES3 + 24/8.
        const EGLint fallback[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
            EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
            EGL_RED_SIZE,        8,
            EGL_GREEN_SIZE,      8,
            EGL_BLUE_SIZE,       8,
            EGL_NONE,
        };
        if (!eglChooseConfig(display, fallback, &app->eglConfig, 1, &numConfigs) ||
            numConfigs < 1) {
            ENG_LOGE("platform", "eglChooseConfig failed (0x%04x)", eglGetError());
            eglTerminate(display);
            return false;
        }
    }
    app->eglDisplay = display;
    const char* vendor = eglQueryString(display, EGL_VENDOR);
    ENG_LOGI("platform", "EGL %d.%d initialised (vendor %s)", (int)major, (int)minor,
             vendor ? vendor : "?");
    return true;
}

bool EnsureEGLContext(AppState* app) {
    if (app->eglContext != EGL_NO_CONTEXT) return true;
    if (!InitEGLDisplay(app)) return false;
    const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    app->eglContext = eglCreateContext(app->eglDisplay, app->eglConfig, EGL_NO_CONTEXT,
                                       contextAttribs);
    if (app->eglContext == EGL_NO_CONTEXT) {
        ENG_LOGE("platform", "eglCreateContext(ES3) failed (0x%04x)", eglGetError());
        return false;
    }
    return true;
}

bool EnsureEGLSurface(AppState* app) {
    if (app->glReady) return true;
    if (!app->window) return false;
    if (!EnsureEGLContext(app)) return false;
    app->eglSurface = eglCreateWindowSurface(app->eglDisplay, app->eglConfig,
                                             reinterpret_cast<EGLNativeWindowType>(app->window),
                                             nullptr);
    if (app->eglSurface == EGL_NO_SURFACE) {
        ENG_LOGE("platform", "eglCreateWindowSurface failed (0x%04x)", eglGetError());
        return false;
    }
    if (!eglMakeCurrent(app->eglDisplay, app->eglSurface, app->eglSurface, app->eglContext)) {
        ENG_LOGE("platform", "eglMakeCurrent failed (0x%04x)", eglGetError());
        eglDestroySurface(app->eglDisplay, app->eglSurface);
        app->eglSurface = EGL_NO_SURFACE;
        return false;
    }
    eglQuerySurface(app->eglDisplay, app->eglSurface, EGL_WIDTH, &app->windowWidth);
    eglQuerySurface(app->eglDisplay, app->eglSurface, EGL_HEIGHT, &app->windowHeight);
    app->glReady = true;
    ENG_LOGI("platform", "EGL surface ready (%dx%d)", app->windowWidth, app->windowHeight);
    return true;
}

// Уничтожает drawable. `notifyLost` передаёт семантику APP_CMD_TERM_WINDOW через
// AppHooks::onContextLost (плюс смапленный onFocus(false), потому что
// у WindowCallbacks нет хука потери контекста).
void ReleaseEGLSurface(AppState* app, bool notifyLost) {
    if (!app->glReady && app->eglSurface == EGL_NO_SURFACE) return;
    if (app->eglDisplay != EGL_NO_DISPLAY) {
        eglMakeCurrent(app->eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (app->eglSurface != EGL_NO_SURFACE) {
            eglDestroySurface(app->eglDisplay, app->eglSurface);
            app->eglSurface = EGL_NO_SURFACE;
        }
    }
    const bool wasReady = app->glReady;
    app->glReady = false;
    if (wasReady && notifyLost) {
        ENG_LOGW("platform",
                 "GL context lost (APP_CMD_TERM_WINDOW): mapped to WindowCallbacks::onFocus");
        if (app->hooksValid && app->hooks.onContextLost) app->hooks.onContextLost(app->hooks.user);
        if (g_activeWindow && g_activeWindow->callbacks.onFocus) {
            g_activeWindow->callbacks.onFocus(false);
        }
    }
}

void DestroyEGL(AppState* app) {
    ReleaseEGLSurface(app, false);
    if (app->eglDisplay != EGL_NO_DISPLAY) {
        if (app->eglContext != EGL_NO_CONTEXT) {
            eglDestroyContext(app->eglDisplay, app->eglContext);
            app->eglContext = EGL_NO_CONTEXT;
        }
        eglTerminate(app->eglDisplay);
        app->eglDisplay = EGL_NO_DISPLAY;
    }
}

// ---------------------------------------------------------------------------
// Ввод
// ---------------------------------------------------------------------------
void HandleKeyEvent(AppState* app, AInputEvent* event) {
    (void)app;
    Window* w = g_activeWindow;
    if (!w) return;
    const int32_t action = AKeyEvent_getAction(event);
    const int32_t keyCode = AKeyEvent_getKeyCode(event);
    const int32_t metaState = AKeyEvent_getMetaState(event);
    const int32_t repeatCount = AKeyEvent_getRepeatCount(event);
    const Key key = KeyFromAndroidKeyCode(keyCode);

    if (action == AKEY_EVENT_ACTION_DOWN || action == AKEY_EVENT_ACTION_MULTIPLE) {
        if (key != Key::Unknown) {
            w->GetInput().OnKey(key, KeyAction::Press, repeatCount > 0);
            if (repeatCount > 0) w->GetInput().OnKey(key, KeyAction::Repeat, true);
        }
        // Текстовый ввод: пропускаем при зажатом командном модификаторе (как в
        // десктопных бэкендах) и когда клавиша не дала символа.
        const bool commandModifier =
            (metaState & (AMETA_CTRL_ON | AMETA_ALT_ON | AMETA_META_ON)) != 0;
        if (!commandModifier) {
            const int32_t unicode = AKeyEvent_getUnicodeChar(event);
            if (unicode >= 32 && unicode != 0x7F) {
                w->GetInput().OnText(static_cast<u32>(unicode));
            }
        }
    } else if (action == AKEY_EVENT_ACTION_UP && key != Key::Unknown) {
        w->GetInput().OnKey(key, KeyAction::Release, false);
    }
}

void HandleMotionEvent(AppState* app, AInputEvent* event) {
    (void)app;
    Window* w = g_activeWindow;
    if (!w) return;
    const float density = app->density > 0.0f ? app->density : 1.0f;
    const int32_t source = AInputEvent_getSource(event);
    const int32_t action = AMotionEvent_getAction(event);
    const int32_t masked = action & AMOTION_EVENT_ACTION_MASK;
    const int32_t pointerCount = static_cast<int32_t>(AMotionEvent_getPointerCount(event));

    // Мышь (или стилус/трекпад, сообщаемый как мышь): кормим состояние мыши
    // в десктопном стиле вместо массива касаний.
    if ((source & AINPUT_SOURCE_MOUSE) == AINPUT_SOURCE_MOUSE &&
        masked != AMOTION_EVENT_ACTION_SCROLL) {
        const float x = AMotionEvent_getX(event, 0) / density;
        const float y = AMotionEvent_getY(event, 0) / density;
        w->GetInput().OnMouseMove({x, y});
        if (masked == AMOTION_EVENT_ACTION_DOWN || masked == AMOTION_EVENT_ACTION_BUTTON_PRESS) {
            const int32_t buttons = AMotionEvent_getButtonState(event);
            MouseButton b = MouseButton::Left;
            if (buttons & AMOTION_EVENT_BUTTON_SECONDARY) b = MouseButton::Right;
            else if (buttons & AMOTION_EVENT_BUTTON_TERTIARY) b = MouseButton::Middle;
            w->GetInput().OnMouseButton(b, true, {x, y});
        } else if (masked == AMOTION_EVENT_ACTION_UP ||
                   masked == AMOTION_EVENT_ACTION_BUTTON_RELEASE) {
            w->GetInput().OnMouseButton(MouseButton::Left, false, {x, y});
            w->GetInput().OnMouseButton(MouseButton::Right, false, {x, y});
            w->GetInput().OnMouseButton(MouseButton::Middle, false, {x, y});
        }
        return;
    }

    if (masked == AMOTION_EVENT_ACTION_SCROLL) {
        w->GetInput().OnScroll({AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HSCROLL, 0),
                                AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_VSCROLL, 0)});
        return;
    }
    if (masked == AMOTION_EVENT_ACTION_HOVER_MOVE ||
        masked == AMOTION_EVENT_ACTION_HOVER_ENTER ||
        masked == AMOTION_EVENT_ACTION_HOVER_EXIT) {
        w->GetInput().OnMouseMove({AMotionEvent_getX(event, 0) / density,
                                   AMotionEvent_getY(event, 0) / density});
        return;
    }

    const int32_t changedIndex =
        (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
        AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;

    for (int32_t i = 0; i < pointerCount; ++i) {
        TouchPhase phase = TouchPhase::Move;
        switch (masked) {
            case AMOTION_EVENT_ACTION_DOWN: phase = TouchPhase::Down; break;
            case AMOTION_EVENT_ACTION_POINTER_DOWN:
                phase = (i == changedIndex) ? TouchPhase::Down : TouchPhase::Move;
                break;
            case AMOTION_EVENT_ACTION_UP: phase = TouchPhase::Up; break;
            case AMOTION_EVENT_ACTION_POINTER_UP:
                phase = (i == changedIndex) ? TouchPhase::Up : TouchPhase::Move;
                break;
            case AMOTION_EVENT_ACTION_CANCEL:
            case AMOTION_EVENT_ACTION_OUTSIDE:
                phase = TouchPhase::Cancel;
                break;
            default: phase = TouchPhase::Move; break;
        }
        TouchPoint tp;
        tp.id = static_cast<i32>(AMotionEvent_getPointerId(event, static_cast<size_t>(i)));
        // Позиции движка — в логических points: framebuffer / DpiScale.
        tp.pos = Vec2{AMotionEvent_getX(event, static_cast<size_t>(i)) / density,
                      AMotionEvent_getY(event, static_cast<size_t>(i)) / density};
        tp.start = tp.pos;
        tp.phase = phase;
        const float pressure = AMotionEvent_getPressure(event, static_cast<size_t>(i));
        tp.pressure = pressure > 0.0f ? pressure : 1.0f;
        // Input::OnTouch также двигает виртуальную мышь, поэтому UI-код продолжает
        // работать с касаниями без двойной подачи мыши (иначе дельта мыши
        // удвоилась бы).
        w->GetInput().OnTouch(tp);
    }
}

bool HandleInputEvent(AppState* app, AInputEvent* event) {
    const int32_t type = AInputEvent_getType(event);
    if (type == AINPUT_EVENT_TYPE_KEY) {
        const int32_t code = AKeyEvent_getKeyCode(event);
        if (IsSystemKey(code)) return false;  // пусть фреймворк обработает
        HandleKeyEvent(app, event);
        return true;
    }
    if (type == AINPUT_EVENT_TYPE_MOTION) {
        HandleMotionEvent(app, event);
        return true;
    }
    return false;
}

void DrainInputQueue(AppState* app) {
    if (!app->attachedInput) return;
    AInputEvent* event = nullptr;
    while (AInputQueue_getEvent(app->attachedInput, &event) >= 0) {
        if (AInputQueue_preDispatchEvent(app->attachedInput, event)) continue;
        const bool handled = HandleInputEvent(app, event);
        AInputQueue_finishEvent(app->attachedInput, event, handled ? 1 : 0);
    }
}

// ---------------------------------------------------------------------------
// Обработка команд (поток приложения)
// ---------------------------------------------------------------------------
void HandleCommand(AppState* app, int cmd) {
    switch (cmd) {
        case kCmdWindowCreated: {
            ANativeWindow* created = nullptr;
            {
                std::lock_guard<std::mutex> lock(app->mutex);
                created = app->pendingWindowCreated;
                app->pendingWindowCreated = nullptr;
            }
            if (!created) break;
            if (app->window && app->window != created) {
                ReleaseEGLSurface(app, true);
                ANativeWindow_release(app->window);
            }
            app->window = created;  // владение acquire(), взятым в колбэке
            const int w0 = ANativeWindow_getWidth(created);
            const int h0 = ANativeWindow_getHeight(created);
            if (w0 > 0) app->windowWidth = w0;
            if (h0 > 0) app->windowHeight = h0;
            EnsureEGLSurface(app);
            if (app->glReady && app->hooksValid && app->hooksInitialised &&
                app->hooks.onContextRestored) {
                ENG_LOGI("platform", "GL context restored (APP_CMD_INIT_WINDOW)");
                app->hooks.onContextRestored(app->hooks.user);
            }
            if (g_activeWindow && g_activeWindow->callbacks.onFocus) {
                g_activeWindow->callbacks.onFocus(true);
            }
            if (g_activeWindow && g_activeWindow->callbacks.onResize) {
                g_activeWindow->callbacks.onResize(app->windowWidth, app->windowHeight);
            }
            break;
        }
        case kCmdWindowDestroyed: {
            ANativeWindow* destroyed = nullptr;
            {
                std::lock_guard<std::mutex> lock(app->mutex);
                destroyed = app->pendingWindowDestroyed;
                app->pendingWindowDestroyed = nullptr;
            }
            ReleaseEGLSurface(app, true);
            if (destroyed) {
                if (app->window == destroyed) app->window = nullptr;
                ANativeWindow_release(destroyed);
            }
            {
                std::lock_guard<std::mutex> lock(app->mutex);
                app->ackWindowDestroyed = true;
            }
            app->cv.notify_all();
            break;
        }
        case kCmdWindowResized: {
            if (app->window) {
                const int w0 = ANativeWindow_getWidth(app->window);
                const int h0 = ANativeWindow_getHeight(app->window);
                if (w0 > 0 && h0 > 0 && (w0 != app->windowWidth || h0 != app->windowHeight)) {
                    app->windowWidth = w0;
                    app->windowHeight = h0;
                    if (app->eglDisplay != EGL_NO_DISPLAY && app->eglSurface != EGL_NO_SURFACE) {
                        eglQuerySurface(app->eglDisplay, app->eglSurface, EGL_WIDTH,
                                        &app->windowWidth);
                        eglQuerySurface(app->eglDisplay, app->eglSurface, EGL_HEIGHT,
                                        &app->windowHeight);
                    }
                    if (g_activeWindow && g_activeWindow->callbacks.onResize) {
                        g_activeWindow->callbacks.onResize(app->windowWidth, app->windowHeight);
                    }
                }
            }
            break;
        }
        case kCmdWindowRedraw:
        case kCmdStart:
        case kCmdResume:
            if (cmd == kCmdResume) ENG_LOGI("platform", "activity resumed");
            break;
        case kCmdPause:
        case kCmdStop:
            // EGL-поверхность сохраняется: фреймворк гарантирует
            // APP_CMD_TERM_WINDOW перед исчезновением нативного окна и заново
            // шлёт APP_CMD_INIT_WINDOW, только если окно было уничтожено.
            ENG_LOGI("platform", "activity %s", cmd == kCmdPause ? "paused" : "stopped");
            if (g_activeWindow && g_activeWindow->callbacks.onFocus) {
                g_activeWindow->callbacks.onFocus(false);
            }
            break;
        case kCmdFocusGained:
        case kCmdFocusLost: {
            const bool focused = cmd == kCmdFocusGained;
            {
                std::lock_guard<std::mutex> lock(app->mutex);
                app->hasFocus = focused;
            }
            if (g_activeWindow && g_activeWindow->callbacks.onFocus) {
                g_activeWindow->callbacks.onFocus(focused);
            }
            break;
        }
        case kCmdInputCreated: {
            AInputQueue* queue = nullptr;
            {
                std::lock_guard<std::mutex> lock(app->mutex);
                queue = app->pendingInputCreated;
                app->pendingInputCreated = nullptr;
            }
            if (!queue || !app->looper) break;
            if (app->attachedInput && app->attachedInput != queue) {
                AInputQueue_detachLooper(app->attachedInput);
                app->attachedInput = nullptr;
            }
            AInputQueue_attachLooper(queue, app->looper, kLooperIdInput, nullptr, app);
            app->attachedInput = queue;
            break;
        }
        case kCmdInputDestroyed: {
            AInputQueue* queue = nullptr;
            {
                std::lock_guard<std::mutex> lock(app->mutex);
                queue = app->pendingInputDestroyed;
                app->pendingInputDestroyed = nullptr;
            }
            if (app->attachedInput && (!queue || app->attachedInput == queue)) {
                AInputQueue_detachLooper(app->attachedInput);
                app->attachedInput = nullptr;
            }
            {
                std::lock_guard<std::mutex> lock(app->mutex);
                app->ackInputDestroyed = true;
            }
            app->cv.notify_all();
            break;
        }
        case kCmdConfigChanged: {
            app->density = DensityFromConfig(app->config);
            if (g_activeWindow && g_activeWindow->callbacks.onDpiChanged) {
                g_activeWindow->callbacks.onDpiChanged(app->density);
            }
            break;
        }
        case kCmdLowMemory:
            ENG_LOGW("platform", "low memory warning");
            if (app->hooksValid && app->hooks.onLowMemory) app->hooks.onLowMemory(app->hooks.user);
            break;
        case kCmdContentRect:
        case kCmdDestroy:
        default:
            break;
    }
}

void DrainCommands(AppState* app) {
    int cmd = 0;
    while (::read(app->msgPipe[0], &cmd, sizeof(cmd)) == static_cast<ssize_t>(sizeof(cmd))) {
        HandleCommand(app, cmd);
    }
}

// Один опрос looper (неблокирующий при timeoutMs == 0) с обработкой команд
// activity и событий ввода. Безопасно из любого потока: looper принадлежит
// вызывающему потоку.
void PumpOnce(AppState* app, int timeoutMs) {
    if (!app || app->destroyed) return;
    if (!app->looper) {
        app->looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
        if (!app->looper) {
            ENG_LOGE("platform", "ALooper_prepare failed");
            return;
        }
    }
    if (!app->pipeRegistered && app->msgPipe[0] >= 0) {
        ALooper_addFd(app->looper, app->msgPipe[0], kLooperIdMain, ALOOPER_EVENT_INPUT, nullptr,
                      nullptr);
        app->pipeRegistered = true;
    }
    {
        std::lock_guard<std::mutex> lock(app->mutex);
        app->loopActive = true;
    }
    int ident = ALooper_pollOnce(timeoutMs, nullptr, nullptr, nullptr);
    if (ident == ALOOPER_POLL_ERROR) {
        ENG_LOGE("platform", "ALooper_pollOnce reported an error");
        std::lock_guard<std::mutex> lock(app->mutex);
        app->loopActive = false;
        return;
    }
    if (ident == kLooperIdMain) DrainCommands(app);
    if (ident == kLooperIdInput) DrainInputQueue(app);
    // Обрабатываем прочее уже стоящее в очереди (ограничено, чтобы не зациклиться).
    for (int i = 0; i < 16; ++i) {
        if (ALooper_pollOnce(0, nullptr, nullptr, nullptr) < 0) break;
        DrainCommands(app);
        DrainInputQueue(app);
        if (app->destroyed) break;
    }
    std::lock_guard<std::mutex> lock(app->mutex);
    app->loopActive = false;
}

// ---------------------------------------------------------------------------
// Колбэки activity (главный поток) -> pipe
// ---------------------------------------------------------------------------
void OnStart(ANativeActivity* activity) {
    (void)activity;
    PostCommand(g_app, kCmdStart);
}
void OnResume(ANativeActivity* activity) {
    (void)activity;
    if (g_app) {
        std::lock_guard<std::mutex> lock(g_app->mutex);
        g_app->resumed = true;
    }
    PostCommand(g_app, kCmdResume);
}
void OnPause(ANativeActivity* activity) {
    (void)activity;
    if (g_app) {
        std::lock_guard<std::mutex> lock(g_app->mutex);
        g_app->resumed = false;
    }
    PostCommand(g_app, kCmdPause);
}
void OnStop(ANativeActivity* activity) {
    (void)activity;
    PostCommand(g_app, kCmdStop);
}
void OnSaveInstanceState(ANativeActivity* activity, void* savedState, size_t savedStateSize) {
    (void)activity;
    (void)savedState;
    (void)savedStateSize;
}
void OnWindowFocusChanged(ANativeActivity* activity, int hasFocus) {
    (void)activity;
    PostCommand(g_app, hasFocus ? kCmdFocusGained : kCmdFocusLost);
}
void OnNativeWindowCreated(ANativeActivity* activity, ANativeWindow* window) {
    (void)activity;
    if (!g_app || !window) return;
    ANativeWindow_acquire(window);
    {
        std::lock_guard<std::mutex> lock(g_app->mutex);
        if (g_app->pendingWindowCreated) ANativeWindow_release(g_app->pendingWindowCreated);
        g_app->pendingWindowCreated = window;
        g_app->ackWindowDestroyed = false;
    }
    PostCommand(g_app, kCmdWindowCreated);
}
void OnNativeWindowResized(ANativeActivity* activity, ANativeWindow* window) {
    (void)activity;
    (void)window;
    PostCommand(g_app, kCmdWindowResized);
}
void OnNativeWindowRedrawNeeded(ANativeActivity* activity, ANativeWindow* window) {
    (void)activity;
    (void)window;
    PostCommand(g_app, kCmdWindowRedraw);
}
void OnNativeWindowDestroyed(ANativeActivity* activity, ANativeWindow* window) {
    (void)activity;
    AppState* app = g_app;
    if (!app) {
        if (window) ANativeWindow_release(window);
        return;
    }
    bool acked = false;
    {
        std::unique_lock<std::mutex> lock(app->mutex);
        app->pendingWindowDestroyed = window;
        app->ackWindowDestroyed = false;
        const bool loopRunning = app->loopActive;
        PostCommand(app, kCmdWindowDestroyed);
        if (loopRunning) {
            // Ждём, пока поток приложения отпустит EGL-поверхность, прежде чем
            // фреймворк снесёт окно (зеркалит поведение android_native_app_glue для
            // APP_CMD_TERM_WINDOW). Таймаут лишь страхует от заклинившего
            // потока приложения.
            app->cv.wait_for(lock, std::chrono::milliseconds(700),
                             [app] { return app->ackWindowDestroyed; });
        }
        acked = app->ackWindowDestroyed;
        if (!acked) {
            // Владение остаётся у нас, поэтому поток приложения не должен освобождать его позже.
            app->pendingWindowDestroyed = nullptr;
            if (app->window == window) app->window = nullptr;
        }
    }
    if (!acked && window) ANativeWindow_release(window);
}
void OnInputQueueCreated(ANativeActivity* activity, AInputQueue* queue) {
    (void)activity;
    if (!g_app || !queue) return;
    {
        std::lock_guard<std::mutex> lock(g_app->mutex);
        g_app->pendingInputCreated = queue;
        g_app->ackInputDestroyed = false;
    }
    PostCommand(g_app, kCmdInputCreated);
}
void OnInputQueueDestroyed(ANativeActivity* activity, AInputQueue* queue) {
    (void)activity;
    AppState* app = g_app;
    if (!app) return;
    std::unique_lock<std::mutex> lock(app->mutex);
    app->pendingInputDestroyed = queue;
    app->ackInputDestroyed = false;
    const bool loopRunning = app->loopActive;
    PostCommand(app, kCmdInputDestroyed);
    if (loopRunning) {
        app->cv.wait_for(lock, std::chrono::milliseconds(500),
                         [app] { return app->ackInputDestroyed; });
    }
}
void OnContentRectChanged(ANativeActivity* activity, const void* rect) {
    (void)activity;
    (void)rect;
    PostCommand(g_app, kCmdContentRect);
}
void OnConfigurationChanged(ANativeActivity* activity) {
    (void)activity;
    if (g_app && g_app->config) {
        AConfiguration_fromAssetManager(g_app->config, activity->assetManager);
    }
    PostCommand(g_app, kCmdConfigChanged);
}
void OnLowMemory(ANativeActivity* activity) {
    (void)activity;
    if (g_app) g_app->lowMemory = true;
    PostCommand(g_app, kCmdLowMemory);
}
void OnDestroy(ANativeActivity* activity) {
    (void)activity;
    AppState* app = g_app;
    if (!app) return;
    {
        std::unique_lock<std::mutex> lock(app->mutex);
        app->destroyed = true;
        const bool loopRunning = app->loopActive;
        PostCommand(app, kCmdDestroy);
        if (loopRunning) {
            app->cv.wait_for(lock, std::chrono::seconds(2), [app] { return app->loopFinished; });
        }
    }
    bool loopStopped = false;
    if (app->thread.joinable()) {
        if (app->loopFinished) {
            app->thread.join();
            loopStopped = true;
        } else {
            // Цикл приложения всё ещё работает (например, блокирующий Engine::Run());
            // не трогаем отсюда его EGL-состояние и позволяем потоку погибнуть
            // вместе с процессом.
            ENG_LOGW("platform", "app thread did not stop; detaching it");
            app->thread.detach();
        }
    }
    if (loopStopped) {
        DestroyEGL(app);
    } else {
        ENG_LOGW("platform", "skipping EGL teardown (the app thread still owns the context)");
    }
    if (app->config) {
        AConfiguration_delete(app->config);
        app->config = nullptr;
    }
    if (app->msgPipe[0] >= 0) ::close(app->msgPipe[0]);
    if (app->msgPipe[1] >= 0) ::close(app->msgPipe[1]);
    activity->instance = nullptr;
    g_app = nullptr;
    delete app;
}

ANativeActivityCallbacks* Callbacks() {
    static ANativeActivityCallbacks callbacks = {};
    callbacks.onStart = OnStart;
    callbacks.onResume = OnResume;
    callbacks.onSaveInstanceState = OnSaveInstanceState;
    callbacks.onPause = OnPause;
    callbacks.onStop = OnStop;
    callbacks.onDestroy = OnDestroy;
    callbacks.onWindowFocusChanged = OnWindowFocusChanged;
    callbacks.onNativeWindowCreated = OnNativeWindowCreated;
    callbacks.onNativeWindowResized = OnNativeWindowResized;
    callbacks.onNativeWindowRedrawNeeded = OnNativeWindowRedrawNeeded;
    callbacks.onNativeWindowDestroyed = OnNativeWindowDestroyed;
    callbacks.onInputQueueCreated = OnInputQueueCreated;
    callbacks.onInputQueueDestroyed = OnInputQueueDestroyed;
    callbacks.onContentRectChanged = OnContentRectChanged;
    callbacks.onConfigurationChanged = OnConfigurationChanged;
    callbacks.onLowMemory = OnLowMemory;
    return &callbacks;
}

// Тело потока приложения. Если приложение предоставляет точку входа — это weak-символ
// ниже; иначе поток только обслуживает жизненный цикл activity (достаточно для
// приложения, ведущего собственный цикл через
// Window::PollEvents()/Engine::Run()).
void AppThreadMain(AppState* app) {
    ENG_LOGI("platform", "native activity thread started");
    if (EngAndroidMain != nullptr) {
        EngAndroidMain();  // точка входа приложения (обычно вызывает RunApp)
    } else {
        ENG_LOGW("platform",
                 "no eng_android_main() entry point found; only the activity lifecycle will be "
                 "serviced. Define `extern \"C\" void eng_android_main()` in the application and "
                 "call crossrender::RunApp() (or drive crossrender::Engine::Run()) from it.");
    }
    // Продолжаем обслуживать activity (команды, ввод), пока фреймворк её не
    // уничтожит, даже если собственный цикл приложения вернулся рано.
    while (!app->destroyed) PumpOnce(app, 100);
    {
        std::lock_guard<std::mutex> lock(app->mutex);
        app->loopFinished = true;
    }
    app->cv.notify_all();
    ENG_LOGI("platform", "native activity thread finished");
}

}  // namespace

// ---------------------------------------------------------------------------
// Точка входа native activity
// ---------------------------------------------------------------------------
extern "C" void ANativeActivity_onCreate(ANativeActivity* activity, void* savedState,
                                         size_t savedStateSize) {
    (void)savedState;
    (void)savedStateSize;
    if (g_app) {
        ENG_LOGW("platform", "ANativeActivity_onCreate called twice; ignoring");
        return;
    }
    AppState* app = new AppState();
    app->activity = activity;
    if (activity->internalDataPath) app->internalDataPath = activity->internalDataPath;
    app->config = AConfiguration_new();
    if (app->config) AConfiguration_fromAssetManager(app->config, activity->assetManager);
    app->density = DensityFromConfig(app->config);
    if (::pipe(app->msgPipe) != 0) {
        ENG_LOGE("platform", "pipe() failed (%d); the activity cannot be driven", errno);
        app->msgPipe[0] = -1;
        app->msgPipe[1] = -1;
    } else {
        // Неблокирующее чтение, чтобы DrainCommands() не мог застопорить поток приложения.
        const int flags = ::fcntl(app->msgPipe[0], F_GETFL, 0);
        if (flags >= 0) ::fcntl(app->msgPipe[0], F_SETFL, flags | O_NONBLOCK);
    }
    activity->instance = app;
    activity->callbacks = Callbacks();
    g_app = app;
    app->thread = std::thread(AppThreadMain, app);
    ENG_LOGI("platform", "ANativeActivity_onCreate done (sdk %d, density %.2f)",
             (int)activity->sdkVersion, (double)app->density);
}

namespace {

// Прокручивает цикл native activity и движет `hooks`. Всё (EGL, ввод,
// жизненный цикл) происходит в вызывающем потоке.
int RunLoop(AppState* app, const AppHooks& hooks) {
    ENG_LOGI("platform", "RunApp: native activity loop starting");
    f64 last = NowSeconds();
    while (!app->destroyed && !app->quitRequested) {
        const f64 frameStart = NowSeconds();
        PumpOnce(app, 0);
        if (app->destroyed) break;

        if (app->window && !app->glReady) EnsureEGLSurface(app);
        if (app->glReady && !app->hooksInitialised) {
            app->hooksInitialised = true;
            if (hooks.onInit) hooks.onInit(hooks.user);
        }

        Window* w = g_activeWindow;
        if (app->glReady && app->hooksValid) {
            const f64 now = NowSeconds();
            f32 dt = static_cast<f32>(now - last);
            last = now;
            if (!(dt > 0.0f) || dt > 1.0f) dt = 1.0f / 60.0f;
            const u32 swapsBefore = g_swapCount;
            if (hooks.onFrame && !hooks.onFrame(hooks.user, dt)) {
                ENG_LOGI("platform", "onFrame requested exit");
                app->quitRequested = true;
                break;
            }
            if (w && g_swapCount == swapsBefore) w->SwapBuffers();
        }

        // Досыпаем остаток бюджета кадра, продолжая обслуживать события.
        const f64 elapsed = NowSeconds() - frameStart;
        const int remainingMs = static_cast<int>((kFrameBudgetS - elapsed) * 1000.0);
        PumpOnce(app, remainingMs > 0 ? remainingMs : 0);
    }

    if (app->hooksInitialised && !app->shutdownCalled) {
        app->shutdownCalled = true;
        if (hooks.onShutdown) hooks.onShutdown(hooks.user);
    }
    ENG_LOGI("platform", "RunApp: native activity loop finished");
    return 0;
}

}  // namespace

int RunAppAndroid(const AppHooks& hooks) {
    AppState* app = g_app;
    if (!app) {
        ENG_LOGE("platform", "RunApp() called before ANativeActivity_onCreate");
        return 1;
    }
    {
        std::lock_guard<std::mutex> lock(app->mutex);
        if (app->hooksValid) {
            ENG_LOGW("platform", "RunApp() called twice; ignoring the second call");
            return 1;
        }
        app->hooks = hooks;
        app->hooksValid = true;
    }
    app->cv.notify_all();
    const int rc = RunLoop(app, hooks);
    {
        std::lock_guard<std::mutex> lock(app->mutex);
        app->hooksValid = false;
    }
    return rc;
}

// ---------------------------------------------------------------------------
// Аксессоры, используемые PlatformAndroid.cpp и методами Window
// ---------------------------------------------------------------------------
Window* AndroidActiveWindow() { return g_activeWindow; }
ANativeActivity* AndroidActivity() { return g_app ? g_app->activity : nullptr; }

const char* AndroidInternalDataPath() { return g_app ? g_app->internalDataPath.c_str() : ""; }

float AndroidDisplayDensity() {
    int w = 0;
    int h = 0;
    float density = 0.0f;
    if (g_app && QueryDisplayMetrics(&w, &h, &density)) return density;
    return g_app ? g_app->density : 1.0f;
}

bool AndroidAppIsForeground() {
    if (!g_app) return false;
    std::lock_guard<std::mutex> lock(g_app->mutex);
    return g_app->resumed;
}

bool AndroidNativeWindowSize(int* outWidth, int* outHeight) {
    if (!g_app || !g_app->window) return false;
    const int w = ANativeWindow_getWidth(g_app->window);
    const int h = ANativeWindow_getHeight(g_app->window);
    if (w <= 0 || h <= 0) return false;
    if (outWidth) *outWidth = w;
    if (outHeight) *outHeight = h;
    return true;
}

void AndroidScreenSizeDp(int* outWidth, int* outHeight) {
    int w = 0;
    int h = 0;
    float density = 1.0f;
    if (QueryDisplayMetrics(&w, &h, &density) && w > 0 && h > 0) {
        if (outWidth) *outWidth = static_cast<int>(w / density);
        if (outHeight) *outHeight = static_cast<int>(h / density);
        return;
    }
    if (AndroidNativeWindowSize(&w, &h)) {
        const float d = AndroidDisplayDensity();
        if (outWidth) *outWidth = static_cast<int>(w / d);
        if (outHeight) *outHeight = static_cast<int>(h / d);
        return;
    }
    // Крайний случай: разумное значение, чтобы раскладка UI продолжала работать.
    if (outWidth) *outWidth = 1280;
    if (outHeight) *outHeight = 720;
}

// ---------------------------------------------------------------------------
// Сервисы на базе JNI (все при неудаче деградируют до предупреждения)
// ---------------------------------------------------------------------------
void AndroidPlatformVibrate(unsigned int milliseconds) {
    if (!g_app || !g_app->activity) return;
    bool attached = false;
    JNIEnv* env = AcquireEnv(&attached);
    if (!env) {
        ENG_LOGW("platform", "Vibrate: no JNIEnv available");
        return;
    }
    jobject vibrator = GetSystemService(env, "vibrator");
    if (vibrator) {
        jclass vibratorClass = env->GetObjectClass(vibrator);
        jmethodID vibrate = vibratorClass ? env->GetMethodID(vibratorClass, "vibrate", "(J)V")
                                          : nullptr;
        if (vibrate) {
            env->CallVoidMethod(vibrator, vibrate, static_cast<jlong>(milliseconds));
        } else {
            ENG_LOGW("platform", "Vibrate: AVibrator.vibrate(long) unavailable");
        }
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            ENG_LOGW("platform", "Vibrate: VIBRATE permission missing?");
        }
        if (vibratorClass) env->DeleteLocalRef(vibratorClass);
        env->DeleteLocalRef(vibrator);
    } else {
        ENG_LOGW("platform", "Vibrate: no vibrator service");
    }
    ReleaseEnv(attached);
}

void AndroidSetKeepScreenOn(bool enabled) {
    if (!g_app || !g_app->activity) return;
    if (enabled) {
        ANativeActivity_setWindowFlags(g_app->activity, AWINDOW_FLAG_KEEP_SCREEN_ON, 0);
    } else {
        ANativeActivity_setWindowFlags(g_app->activity, 0, AWINDOW_FLAG_KEEP_SCREEN_ON);
    }
}

void AndroidSetSoftKeyboardVisible(bool visible) {
    if (!g_app || !g_app->activity) return;
    if (visible) {
        ANativeActivity_showSoftInput(g_app->activity, ANATIVEACTIVITY_SHOW_SOFT_INPUT_FORCED);
    } else {
        ANativeActivity_hideSoftInput(g_app->activity, ANATIVEACTIVITY_HIDE_SOFT_INPUT_NOT_ALWAYS);
    }
}

void AndroidSetFullscreen(bool fullscreen) {
    if (!g_app || !g_app->activity) return;
    if (fullscreen) {
        ANativeActivity_setWindowFlags(g_app->activity, AWINDOW_FLAG_FULLSCREEN, 0);
    } else {
        ANativeActivity_setWindowFlags(g_app->activity, 0, AWINDOW_FLAG_FULLSCREEN);
    }
}

void AndroidClipboardSetText(const char* utf8) {
    if (!g_app || !g_app->activity || !utf8) return;
    bool attached = false;
    JNIEnv* env = AcquireEnv(&attached);
    if (!env) return;
    jobject clipboard = GetSystemService(env, "clipboard");
    if (clipboard) {
        jclass clipboardClass = env->GetObjectClass(clipboard);
        jmethodID setText = clipboardClass
                                ? env->GetMethodID(clipboardClass, "setText",
                                                   "(Ljava/lang/CharSequence;)V")
                                : nullptr;
        if (setText) {
            jstring text = env->NewStringUTF(utf8);
            if (text) {
                env->CallVoidMethod(clipboard, setText, text);
                env->DeleteLocalRef(text);
            }
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (clipboardClass) env->DeleteLocalRef(clipboardClass);
        env->DeleteLocalRef(clipboard);
    }
    ReleaseEnv(attached);
}

void AndroidClipboardGetText(char* out, int cap) {
    if (!out || cap <= 0) return;
    out[0] = '\0';
    if (!g_app || !g_app->activity) return;
    bool attached = false;
    JNIEnv* env = AcquireEnv(&attached);
    if (!env) return;
    jobject clipboard = GetSystemService(env, "clipboard");
    if (clipboard) {
        jclass clipboardClass = env->GetObjectClass(clipboard);
        jmethodID getText = clipboardClass
                                ? env->GetMethodID(clipboardClass, "getText",
                                                   "()Ljava/lang/CharSequence;")
                                : nullptr;
        jmethodID toString = nullptr;
        jobject sequence = getText ? env->CallObjectMethod(clipboard, getText) : nullptr;
        jstring text = nullptr;
        if (sequence) {
            jclass sequenceClass = env->GetObjectClass(sequence);
            toString = sequenceClass
                           ? env->GetMethodID(sequenceClass, "toString", "()Ljava/lang/String;")
                           : nullptr;
            if (toString) text = static_cast<jstring>(env->CallObjectMethod(sequence, toString));
            if (sequenceClass) env->DeleteLocalRef(sequenceClass);
        }
        if (text) {
            const std::string utf8 = JavaStringToUtf8(env, text);
            std::strncpy(out, utf8.c_str(), static_cast<size_t>(cap) - 1);
            out[cap - 1] = '\0';
            env->DeleteLocalRef(text);
        }
        if (sequence) env->DeleteLocalRef(sequence);
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (clipboardClass) env->DeleteLocalRef(clipboardClass);
        env->DeleteLocalRef(clipboard);
    }
    ReleaseEnv(attached);
}

// ---------------------------------------------------------------------------
// Window::Impl
// ---------------------------------------------------------------------------
struct Window::Impl {
    ANativeWindow* window = nullptr;  // одолжено у состояния activity
    bool shouldClose = false;
    bool focused = true;
    bool cursorVisible = true;
    int cursorMode = 0;
    int width = 0;
    int height = 0;
    int fbWidth = 0;
    int fbHeight = 0;
    f32 dpiScale = 1.0f;
    WindowMode mode = WindowMode::Windowed;
    WindowDesc desc;
    std::string title;
};

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------
Window::Window() : impl_(new Impl()) { g_activeWindow = this; }

Window::~Window() {
    Destroy();
    if (g_activeWindow == this) g_activeWindow = nullptr;
    impl_.reset();
}

bool Window::Create(const WindowDesc& desc) {
    if (!g_app) {
        ENG_LOGE("platform", "Window::Create before ANativeActivity_onCreate");
        return false;
    }
    if (!g_app->window) {
        // Нативное окно приходит с APP_CMD_INIT_WINDOW; onInit из RunApp вызывается
        // только после его появления, поэтому попадание сюда означает, что приложение
        // создало Window без окна (например из JNI_OnLoad).
        ENG_LOGE("platform", "Window::Create: the native window is not available yet");
        return false;
    }
    impl_->desc = desc;
    impl_->title = desc.title;
    impl_->mode = desc.mode;
    impl_->window = g_app->window;
    g_activeWindow = this;
    impl_->dpiScale = AndroidDisplayDensity();
    impl_->fbWidth = ANativeWindow_getWidth(impl_->window);
    impl_->fbHeight = ANativeWindow_getHeight(impl_->window);
    if (impl_->fbWidth <= 0) impl_->fbWidth = desc.width;
    if (impl_->fbHeight <= 0) impl_->fbHeight = desc.height;
    impl_->width = static_cast<int>(impl_->fbWidth / impl_->dpiScale);
    impl_->height = static_cast<int>(impl_->fbHeight / impl_->dpiScale);
    if (desc.msaaSamples > 1) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            ENG_LOGW("platform",
                     "MSAA (%d samples) is not requested on Android: the EGL config is chosen "
                     "by InitEGLDisplay() and does not enable multisampling",
                     desc.msaaSamples);
        }
    }
    AndroidSetFullscreen(desc.mode != WindowMode::Windowed);
    MakeCurrent();
    ENG_LOGI("platform", "window created %dx%d dp (fb %dx%d, dpi %.2f)", impl_->width,
             impl_->height, impl_->fbWidth, impl_->fbHeight, impl_->dpiScale);
    return true;
}

void Window::Destroy() {
    if (!impl_) return;
    impl_->window = nullptr;  // окном владеет состояние приложения
    impl_->shouldClose = false;
}

void Window::PollEvents() {
    // Неблокирующе: обслуживает командный pipe activity и очередь ввода из того
    // потока, который движет движком (Engine::Run) — EGL-контекст создаётся
    // в том же потоке.
    AppState* app = g_app;
    if (!app) return;
    if (app->window && !app->glReady) {
        EnsureEGLSurface(app);
        if (app->glReady) MakeCurrent();
    }
    PumpOnce(app, 0);
    std::lock_guard<std::mutex> lock(app->mutex);
    impl_->fbWidth = app->windowWidth > 0 ? app->windowWidth : impl_->fbWidth;
    impl_->fbHeight = app->windowHeight > 0 ? app->windowHeight : impl_->fbHeight;
    impl_->focused = app->hasFocus;
}

void Window::SwapBuffers() {
    AppState* app = g_app;
    if (!app || !app->glReady || app->eglDisplay == EGL_NO_DISPLAY ||
        app->eglSurface == EGL_NO_SURFACE) {
        return;
    }
    ++g_swapCount;
    if (!eglSwapBuffers(app->eglDisplay, app->eglSurface)) {
        const EGLint err = eglGetError();
        if (err == EGL_BAD_SURFACE || err == EGL_CONTEXT_LOST || err == EGL_BAD_CONTEXT) {
            ENG_LOGW("platform", "eglSwapBuffers failed (0x%04x); dropping the surface", err);
            ReleaseEGLSurface(app, true);
            impl_->shouldClose = false;
        } else if (err != EGL_SUCCESS) {
            ENG_LOGW("platform", "eglSwapBuffers failed (0x%04x)", err);
        }
    }
}

bool Window::ShouldClose() const { return impl_->shouldClose; }


void Window::RequestClose() {
    if (impl_->shouldClose) return;
    impl_->shouldClose = true;
    if (callbacks.onClose) callbacks.onClose();
    // В Android «закрыть окно» означает завершить activity.
    if (g_app && g_app->activity) ANativeActivity_finish(g_app->activity);
}

void Window::SetTitle(const std::string& title) {
    // У Android нет заголовка окна; храним значение для Title().
    impl_->title = title;
}

void Window::SetSize(int w, int h) {
    impl_->width = w;
    impl_->height = h;
    ENG_LOGI("platform", "SetSize(%d, %d) ignored on Android (the surface is owned by the OS)",
             w, h);
}

void Window::SetMode(WindowMode mode) {
    impl_->mode = mode;
    impl_->desc.mode = mode;
    AndroidSetFullscreen(mode != WindowMode::Windowed);
}

void Window::SetVSync(bool enabled) {
    impl_->desc.vsync = enabled;
    AppState* app = g_app;
    if (app && app->eglDisplay != EGL_NO_DISPLAY) {
        eglSwapInterval(app->eglDisplay, enabled ? 1 : 0);
    }
}

void Window::Minimize() {}
void Window::Maximize() {}
void Window::Restore() {}
void Window::Show() {}
void Window::Hide() {}

void Window::Focus() {
    // Программно фокусировать нечего; фокусом владеет activity.
}

void Window::MakeCurrent() {
    AppState* app = g_app;
    if (!app || app->eglDisplay == EGL_NO_DISPLAY || app->eglContext == EGL_NO_CONTEXT) return;
    eglMakeCurrent(app->eglDisplay, app->eglSurface, app->eglSurface, app->eglContext);
}

void Window::WaitEventsTimeout(f32 seconds) {
    AppState* app = g_app;
    if (!app) return;
    PumpOnce(app, static_cast<int>(seconds * 1000.0f));
}

void Window::SetCursorVisible(bool visible) { impl_->cursorVisible = visible; }
void Window::SetCursorMode(int mode) { impl_->cursorMode = mode; }

void Window::SetClipboardText(const std::string& text) {
    AndroidClipboardSetText(text.c_str());
}

std::string Window::GetClipboardText() const {
    char buffer[4096];
    buffer[0] = '\0';
    AndroidClipboardGetText(buffer, static_cast<int>(sizeof(buffer)));
    return std::string(buffer);
}

int Window::Width() const {
    const f32 scale = impl_->dpiScale > 0 ? impl_->dpiScale : 1.0f;
    return static_cast<int>(FramebufferWidth() / scale);
}

int Window::Height() const {
    const f32 scale = impl_->dpiScale > 0 ? impl_->dpiScale : 1.0f;
    return static_cast<int>(FramebufferHeight() / scale);
}

int Window::FramebufferWidth() const {
    int w = 0;
    int h = 0;
    if (AndroidNativeWindowSize(&w, &h) && w > 0) return w;
    return impl_->fbWidth > 0 ? impl_->fbWidth : 1;
}

int Window::FramebufferHeight() const {
    int w = 0;
    int h = 0;
    if (AndroidNativeWindowSize(&w, &h) && h > 0) return h;
    return impl_->fbHeight > 0 ? impl_->fbHeight : 1;
}

f32 Window::DpiScale() const {
    const float density = AndroidDisplayDensity();
    return density > 0.0f ? static_cast<f32>(density) : 1.0f;
}

f32 Window::Aspect() const {
    const int h = FramebufferHeight();
    return h > 0 ? static_cast<f32>(FramebufferWidth()) / static_cast<f32>(h) : 1.0f;
}

bool Window::IsFocused() const {
    if (!g_app) return false;
    std::lock_guard<std::mutex> lock(g_app->mutex);
    return g_app->hasFocus && g_app->resumed;
}

bool Window::IsMinimized() const {
    if (!g_app) return true;
    std::lock_guard<std::mutex> lock(g_app->mutex);
    return !g_app->resumed;
}

bool Window::IsFullscreen() const { return impl_->mode != WindowMode::Windowed; }

Vec2 Window::MousePosition() const { return GetInput().MousePos(); }

const std::string& Window::Title() const { return impl_->title; }

void* Window::NativeHandle() const { return static_cast<void*>(impl_->window); }

void* Window::NativeDisplay() const {
    return g_app ? reinterpret_cast<void*>(g_app->eglDisplay) : nullptr;
}

void* (*Window::GLGetProcAddress() const)(const char*) { return &AndroidGLGetProcAddress; }

}  // namespace crossrender

#else

// В сборках без Android эта единица трансляции пуста.
namespace crossrender {}

#endif  // ENG_PLATFORM_ANDROID
