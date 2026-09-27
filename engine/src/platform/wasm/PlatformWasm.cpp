// Сервисы WebAssembly-платформы, объявленные в crossrender/platform/Platform.h.
//
// Всё, что требует DOM, идёт через EM_ASM/EM_JS; оконно-вводная половина
// слоя живёт в WindowWasm.cpp.
#include "crossrender/platform/Platform.h"

#include "crossrender/core/Log.h"
#include "crossrender/platform/Window.h"

#include "WasmPlatform.h"

#if defined(ENG_PLATFORM_WASM)

#include <emscripten/heap.h>
#include <emscripten/emscripten.h>
#include <emscripten/html5_webgl.h>

#if defined(__EMSCRIPTEN__)
#include <dlfcn.h>
#endif

#include <string>
#include <cstdint>

#if defined(__EMSCRIPTEN_PTHREADS__)
#include <pthread.h>
#endif

namespace crossrender {
namespace {

AppHooks g_hooks;
bool g_hooksValid = false;
bool g_shutdownCalled = false;
bool g_headlessCreated = false;
double g_lastFrameMs = 0.0;

}  // namespace

// ---------------------------------------------------------------------------
// Точки входа GL
// ---------------------------------------------------------------------------
void* WasmGLGetProcAddress(const char* name) {
    if (!name) return nullptr;
    // Функции WebGL2/GLES3 статически слинкованы в wasm-модуль; этот
    // резолвер существует для прослойки gl::LoadFunctions() движка.
    void* fn = emscripten_webgl_get_proc_address(name);
    if (fn) return fn;
    // Запасной вариант 1: динамический линковщик (чтобы он что-то экспортировал,
    // нужны -sMAIN_MODULE / -sEXPORT_ALL; иначе dlsym просто ничего не находит).
#if defined(__EMSCRIPTEN__)
    if (void* sym = dlsym(RTLD_DEFAULT, name)) return sym;
#endif
    // Запасной вариант 2: EGL, для сборок, где WebGL идёт через EGL/ANGLE. В обычных
    // сборках Emscripten GLES слинкован статически и ENG_GL_USE_EGL не определяется.
#if defined(ENG_GL_USE_EGL)
    if (void* sym = reinterpret_cast<void*>(eglGetProcAddress(name))) return sym;
#endif
    return nullptr;
}

// ---------------------------------------------------------------------------
// Жизненный цикл платформы (объявлен в crossrender/platform/Window.h)
// ---------------------------------------------------------------------------
bool PlatformInit() {
    ENG_LOGI("platform", "Web platform initialised");
    return true;
}

void PlatformShutdown() {}

std::string PlatformName() { return "Web"; }

void* (*PlatformGLGetProcAddress())(const char*) { return &WasmGLGetProcAddress; }

// ---------------------------------------------------------------------------
// Диалоги / оболочка
// ---------------------------------------------------------------------------
void ShowMessageBox(const std::string& title, const std::string& message, MessageBoxType type) {
    // Нативных диалогов в браузере нет: логируем и используем window.alert(). alert()
    // блокирует поток браузера, что соответствует блокирующему контракту функции.
    ENG_LOGI("platform", "message box: %s: %s", title.c_str(), message.c_str());
    (void)type;
    EM_ASM(
        {
            var text = UTF8ToString($0) + String.fromCharCode(10) + UTF8ToString($1);
            if (typeof alert === 'function') {
                alert(text);
            } else if (typeof console !== 'undefined') {
                console.log(text);
            }
        },
        title.c_str(), message.c_str());
}

// Песочница браузера не даёт wasm синхронного выбора файлов (<input
// type="file"> асинхронен), поэтому оба диалога сообщают об «отмене».
std::string OpenFileDialog(const std::string& title, const std::string& filter) {
    ENG_LOGW("platform", "OpenFileDialog('%s', '%s') is not implemented on Web", title.c_str(),
             filter.c_str());
    return {};
}

std::string SaveFileDialog(const std::string& title, const std::string& defaultName) {
    ENG_LOGW("platform", "SaveFileDialog('%s', '%s') is not implemented on Web", title.c_str(),
             defaultName.c_str());
    return {};
}

bool OpenUrl(const std::string& url) {
    if (url.empty()) return false;
    // _blank сохраняет страницу движка живой; блокировщики попапов всё же могут запретить.
    EM_ASM({ window.open(UTF8ToString($0), '_blank'); }, url.c_str());
    return true;
}

void GetScreenSize(int* w, int* h) {
    const int width = EM_ASM_INT({
        if (typeof screen === 'undefined') { return 0; }
        return screen.width;
    });
    const int height = EM_ASM_INT({
        if (typeof screen === 'undefined') { return 0; }
        return screen.height;
    });
    if (w) *w = width;
    if (h) *h = height;
}

// ---------------------------------------------------------------------------
// Сведения о процессе / системе
// ---------------------------------------------------------------------------
std::string PlatformArch() {
#if defined(__wasm64__)
    return "wasm64";
#else
    return "wasm32";
#endif
}

std::string CpuName() {
    // Браузер скрывает реальный CPU; сообщаем среду исполнения.
    return "WebAssembly";
}

u64 TotalPhysicalMemory() {
    // Emscripten не даёт доступа к физической памяти; сообщаем размер кучи wasm
    // (число, из которого движок реально может выделять память).
    const usize bytes = emscripten_get_heap_size();
    return static_cast<u64>(bytes);
}

int CpuCoreCount() {
#if defined(__EMSCRIPTEN_PTHREADS__)
    const int cores = EM_ASM_INT({
        if (typeof navigator === 'undefined' || !navigator.hardwareConcurrency) { return 1; }
        return navigator.hardwareConcurrency;
    });
    return cores > 0 ? cores : 1;
#else
    // Без -pthread модуль работает в один поток.
    return 1;
#endif
}

std::string ExecutablePath() {
    // В песочнице браузера нет пути исполняемого файла; wasm-бинарник загружает
    // JS-оболочка. Задокументированное ограничение.
    return {};
}

std::string ExecutableDir() {
    // Корень виртуальной файловой системы Emscripten (ресурсы обычно
    // предзагружаются в /assets).
    return "/";
}

// ---------------------------------------------------------------------------
// Время / потоки
// ---------------------------------------------------------------------------
void SleepMs(u32 milliseconds) {
    if (milliseconds == 0) return;
    // Требует -sASYNCIFY: без него Emscripten не может размотать C-стек через
    // виток событийного цикла, и вызов превращается в no-op.
    emscripten_sleep(milliseconds);
}

u64 CurrentThreadId() {
#if defined(__EMSCRIPTEN_PTHREADS__)
    return static_cast<u64>(reinterpret_cast<uintptr_t>(pthread_self()));
#else
    return 1;  // однопоточная сборка
#endif
}

// ---------------------------------------------------------------------------
// Мобильные / браузерные сервисы
// ---------------------------------------------------------------------------
void SetSoftKeyboardVisible(bool visible) {
    // Браузер открывает экранную клавиатуру только для сфокусированного поля формы,
    // поэтому по требованию создаётся прозрачный input размером 1px.
    EM_ASM(
        {
            if (typeof document === 'undefined') { return; }
            var el = document.getElementById('crossrender-soft-keyboard');
            if (!el) {
                el = document.createElement('input');
                el.id = 'crossrender-soft-keyboard';
                el.type = 'text';
                el.setAttribute('autocapitalize', 'off');
                el.setAttribute('autocomplete', 'off');
                el.style.position = 'absolute';
                el.style.left = '0px';
                el.style.top = '0px';
                el.style.width = '1px';
                el.style.height = '1px';
                el.style.opacity = '0';
                document.body.appendChild(el);
            }
            if ($0) {
                el.focus();
            } else {
                el.blur();
            }
        },
        visible ? 1 : 0);
    ENG_LOGI("platform", "soft keyboard %s", visible ? "requested" : "hidden");
}

void SetKeepScreenAwake(bool enabled) {
    // API Screen Wake Lock асинхронен и требует защищённого контекста;
    // сбои (включая неподдерживающие браузеры) проглатываются.
    EM_ASM(
        {
            if (typeof navigator === 'undefined' || !navigator.wakeLock) { return; }
            if ($0) {
                navigator.wakeLock.request('screen').then(function (sentinel) {
                    Module.__engWakeLock = sentinel;
                }).catch(function () {});
            } else if (Module.__engWakeLock) {
                Module.__engWakeLock.release();
                Module.__engWakeLock = null;
            }
        },
        enabled ? 1 : 0);
}

void Vibrate(u32 milliseconds) {
    EM_ASM(
        {
            if (typeof navigator === 'undefined' || !navigator.vibrate) { return; }
            navigator.vibrate($0);
        },
        static_cast<int>(milliseconds));
}

bool IsAppForeground() {
    return EM_ASM_INT({
               if (typeof document === 'undefined') { return 1; }
               return document.hidden ? 0 : 1;
           }) != 0;
}

// ---------------------------------------------------------------------------
// Жизненный цикл приложения
// ---------------------------------------------------------------------------
namespace {

void WasmMainLoop(void* arg) {
    (void)arg;
    const double now = emscripten_get_now();
    f32 dt = static_cast<f32>((now - g_lastFrameMs) / 1000.0);
    g_lastFrameMs = now;
    if (!(dt > 0.0f) || dt > 0.5f) dt = 1.0f / 60.0f;

    Window* w = WasmActiveWindow();
    if (w) w->PollEvents();
    if (g_hooksValid && g_hooks.onFrame) {
        if (!g_hooks.onFrame(g_hooks.user, dt)) {
            ENG_LOGI("platform", "onFrame requested exit");
            emscripten_cancel_main_loop();
            if (!g_shutdownCalled) {
                g_shutdownCalled = true;
                if (g_hooks.onShutdown) g_hooks.onShutdown(g_hooks.user);
            }
            return;
        }
    }
    if (w) w->SwapBuffers();
}

}  // namespace

int RunApp(const AppHooks& hooks) {
    g_hooks = hooks;
    g_hooksValid = true;
    g_shutdownCalled = false;

    if (hooks.onInit) hooks.onInit(hooks.user);
    g_lastFrameMs = emscripten_get_now();
    // fps = 0 — цикл движет requestAnimationFrame, а
    // simulate_infinite_loop = 0, поэтому вызов возвращает управление в событийный
    // цикл JS, не разматывая C-стек. Шаг Step() движка выполняется один раз
    // на кадр анимации.
    emscripten_set_main_loop_arg(&WasmMainLoop, nullptr, 0, 0);
    return 0;
}

// ---------------------------------------------------------------------------
// Headless GL.
//
// WebGL требует canvas, поэтому headless-режим использует offscreen canvas. В Node
// нет вообще реализации WebGL; тогда создание контекста фиксируется в логе,
// но четвёрка функций всё равно отвечает «доступно» — именно это позволяет
// прогонять --headless ветку движка под node/wasm (точки входа GL при этом
// нулевые, и gl::LoadFunctions() чисто падает).
// ---------------------------------------------------------------------------
bool CreateHeadlessGLContext() {
    const int ok = EM_ASM_INT(
        {
            try {
                if (typeof OffscreenCanvas !== 'undefined') {
                    var off = new OffscreenCanvas(64, 64);
                    Module.__engHeadlessCanvas = off;
                    Module.__engHeadlessCtx = off.getContext('webgl2');
                    return Module.__engHeadlessCtx ? 1 : 0;
                }
                if (typeof document !== 'undefined') {
                    var c = document.createElement('canvas');
                    c.width = 64;
                    c.height = 64;
                    Module.__engHeadlessCanvas = c;
                    Module.__engHeadlessCtx = c.getContext('webgl2');
                    return Module.__engHeadlessCtx ? 1 : 0;
                }
                return 0;
            } catch (e) {
                return 0;
            }
        });
    g_headlessCreated = true;
    if (ok) {
        ENG_LOGI("platform", "headless WebGL2 context created on an offscreen canvas");
    } else {
        ENG_LOGW("platform",
                 "no WebGL2 implementation available (node?); headless GL entry points are null");
    }
    return true;
}

void DestroyHeadlessGLContext() {
    if (!g_headlessCreated) return;
    g_headlessCreated = false;
    EM_ASM({
        if (Module.__engHeadlessCtx && Module.__engHeadlessCtx.getExtension) {
            var ext = Module.__engHeadlessCtx.getExtension('WEBGL_lose_context');
            if (ext) { ext.loseContext(); }
        }
        Module.__engHeadlessCtx = null;
        Module.__engHeadlessCanvas = null;
    });
}

bool HasHeadlessGLContext() { return g_headlessCreated; }

void* HeadlessGLGetProcAddress(const char* name) { return WasmGLGetProcAddress(name); }

}  // namespace crossrender

#else

// В сборках без WASM эта единица трансляции пуста.
namespace crossrender {}

#endif  // ENG_PLATFORM_WASM
