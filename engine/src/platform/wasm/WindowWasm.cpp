// Платформенный слой WebAssembly: контекст WebGL2 на HTML canvas + DOM-колбэки
// событий. Никаких GLFW/SDL, никакой оболочки «GL-эмуляции» Emscripten.
//
// Замечание о потоках: в сборке Emscripten по умолчанию (без pthread) каждый
// колбэк здесь выполняется в главном потоке браузера, поэтому простых глобалов достаточно.
#include "crossrender/platform/Window.h"

#include "crossrender/core/Log.h"
#include "crossrender/platform/Platform.h"

#include "WasmPlatform.h"

#if defined(ENG_PLATFORM_WASM)

#include <emscripten/html5.h>
#include <emscripten/emscripten.h>

#include <string>

// ---------------------------------------------------------------------------
// JavaScript-помощники.
//
// Блоки кода EM_ASM/EM_JS не должны содержать запятую на верхнем уровне: C-
// препроцессор делит аргументы макроса по запятым. Поэтому объектные/массивные
// литералы обёрнуты в скобки, а аргументы вызова остаются в круглых скобках.
// ---------------------------------------------------------------------------

// Записывает CSS-селектор целевого canvas ("#id", по умолчанию Module.canvas,
// затем "#canvas") в `out` и возвращает его длину. Создаёт и добавляет canvas,
// если страница его не предоставила.
EM_JS(int, EngJsCanvasTarget, (char* out, int cap), {
    var target = '#canvas';
    if (typeof Module !== 'undefined' && Module && Module['canvas'] && Module['canvas'].id) {
        target = '#' + Module['canvas'].id;
    }
    if (typeof document === 'undefined') {
        var n0 = Math.min(target.length, cap - 1);
        for (var i0 = 0; i0 < n0; i0++) { HEAPU8[out + i0] = target.charCodeAt(i0); }
        HEAPU8[out + n0] = 0;
        return 0;
    }
    var el = document.querySelector(target);
    if (!el && target === '#canvas') {
        el = document.createElement('canvas');
        el.id = 'canvas';
        el.width = 1280;
        el.height = 720;
        document.body.appendChild(el);
    }
    if (!el) { return -1; }
    var n = Math.min(target.length, cap - 1);
    for (var i = 0; i < n; i++) { HEAPU8[out + i] = target.charCodeAt(i); }
    HEAPU8[out + n] = 0;
    Module.__engCanvasSelector = target;
    return n;
});

EM_JS(void, EngJsSetCursor, (int mode), {
    if (typeof document === 'undefined') { return; }
    var sel = (Module && Module.__engCanvasSelector) ? Module.__engCanvasSelector : '#canvas';
    var el = document.querySelector(sel);
    if (!el) { return; }
    el.style.cursor = (mode === 0) ? 'default' : 'none';
});

EM_JS(void, EngJsSetDisplay, (int show), {
    if (typeof document === 'undefined') { return; }
    var sel = (Module && Module.__engCanvasSelector) ? Module.__engCanvasSelector : '#canvas';
    var el = document.querySelector(sel);
    if (el) { el.style.display = (show !== 0) ? 'block' : 'none'; }
});

EM_JS(int, EngJsIsFullscreen, (void), {
    if (typeof document === 'undefined') { return 0; }
    return document.fullscreenElement ? 1 : 0;
});

EM_JS(void, EngJsRequestFullscreen, (int on), {
    if (typeof document === 'undefined') { return; }
    if (on) {
        var sel = (Module && Module.__engCanvasSelector) ? Module.__engCanvasSelector : '#canvas';
        var el = document.querySelector(sel);
        if (el && el.requestFullscreen) { el.requestFullscreen(); }
    } else if (document.exitFullscreen) {
        document.exitFullscreen();
    }
});

EM_JS(void, EngJsSetClipboard, (const char* utf8), {
    if (typeof navigator === 'undefined' || !navigator.clipboard) { return; }
    var text = UTF8ToString(utf8);
    navigator.clipboard.writeText(text).catch(function () {});
});

EM_JS(void, EngJsFocusWindow, (void), {
    if (typeof window !== 'undefined' && window.focus) { window.focus(); }
});

EM_JS(void, EngJsEnsureCanvasFocusable, (const char* target), {
    if (typeof document === 'undefined') { return; }
    var el = document.querySelector(UTF8ToString(target));
    if (!el) { return; }
    if (!el.hasAttribute('tabindex')) { el.setAttribute('tabindex', '0'); }
    el.style.outline = 'none';
});

namespace crossrender {

// Определены после Window::Impl ниже (им нужно состояние окна).
void WasmHandleBrowserResize();
void WasmSetFocused(bool focused);
void WasmHandleContextEvent(int eventType);

namespace {

Window* g_activeWindow = nullptr;
Window::Impl* g_activeImpl = nullptr;  // зеркало g_activeWindow (приватный impl_)
EMSCRIPTEN_WEBGL_CONTEXT_HANDLE g_context = 0;
bool g_handlersInstalled = false;
bool g_focused = true;
bool g_visible = true;
bool g_contextLost = false;

// ---------------------------------------------------------------------------
// DOM keyCode (DOM_VK_*) -> crossrender::Key, с использованием DOM_KEY_LOCATION_*
// для выбора левого/правого варианта клавиш-модификаторов.
// ---------------------------------------------------------------------------
Key KeyFromDomKeyCode(u32 code, u32 location) {
    // Буквы (0x41..0x5A) и цифровой ряд (0x30..0x39).
    if (code >= 0x41 && code <= 0x5A)
        return static_cast<Key>(static_cast<int>(Key::A) + (code - 0x41));
    if (code >= 0x30 && code <= 0x39)
        return static_cast<Key>(static_cast<int>(Key::Num0) + (code - 0x30));
    // Цифры нумпада (0x60..0x69) и F1..F24 (0x70..0x87).
    if (code >= 0x60 && code <= 0x69)
        return static_cast<Key>(static_cast<int>(Key::Keypad0) + (code - 0x60));
    if (code >= 0x70 && code <= 0x87)
        return static_cast<Key>(static_cast<int>(Key::F1) + (code - 0x70));
    switch (code) {
        case 0x08: return Key::Backspace;
        case 0x09: return Key::Tab;
        case 0x0D: return Key::Enter;
        case 0x10:  // обобщённый Shift
            return location == DOM_KEY_LOCATION_RIGHT ? Key::RightShift : Key::LeftShift;
        case 0x11:  // обобщённый Control
            return location == DOM_KEY_LOCATION_RIGHT ? Key::RightControl : Key::LeftControl;
        case 0x12:  // обобщённый Alt
            return location == DOM_KEY_LOCATION_RIGHT ? Key::RightAlt : Key::LeftAlt;
        case 0x13: return Key::Pause;
        case 0x14: return Key::CapsLock;
        case 0x1B: return Key::Escape;
        case 0x20: return Key::Space;
        case 0x21: return Key::PageUp;
        case 0x22: return Key::PageDown;
        case 0x23: return Key::End;
        case 0x24: return Key::Home;
        case 0x25: return Key::Left;
        case 0x26: return Key::Up;
        case 0x27: return Key::Right;
        case 0x28: return Key::Down;
        case 0x2C: return Key::PrintScreen;
        case 0x2D: return Key::Insert;
        case 0x2E: return Key::Delete;
        case 0x5B: return Key::LeftSuper;
        case 0x5C: return Key::RightSuper;
        case 0x5D: return Key::Menu;
        case 0x6A: return Key::KeypadMultiply;
        case 0x6B: return Key::KeypadAdd;
        case 0x6D: return Key::KeypadSubtract;
        case 0x6E: return Key::KeypadDecimal;
        case 0x6F: return Key::KeypadDivide;
        case 0x90: return Key::NumLock;
        case 0x91: return Key::ScrollLock;
        case 0xBA: return Key::Semicolon;
        case 0xBB: return Key::Equal;
        case 0xBC: return Key::Comma;
        case 0xBD: return Key::Minus;
        case 0xBE: return Key::Period;
        case 0xBF: return Key::Slash;
        case 0xC0: return Key::Grave;
        case 0xDB: return Key::LeftBracket;
        case 0xDC: return Key::Backslash;
        case 0xDD: return Key::RightBracket;
        case 0xDE: return Key::Apostrophe;
        default: return Key::Unknown;
    }
}

bool IsScrollKey(Key k) {
    switch (k) {
        case Key::Space:
        case Key::Tab:
        case Key::Backspace:
        case Key::Left:
        case Key::Right:
        case Key::Up:
        case Key::Down:
        case Key::PageUp:
        case Key::PageDown:
        case Key::Home:
        case Key::End:
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// DOM-колбэки событий (все выполняются в главном потоке браузера)
// ---------------------------------------------------------------------------
EM_BOOL OnKeyEvent(int eventType, const EmscriptenKeyboardEvent* e, void* userData) {
    (void)userData;
    Window* w = g_activeWindow;
    if (!w) return EM_FALSE;

    if (eventType == EMSCRIPTEN_EVENT_KEYPRESS) {
        // Текстовый ввод. `charCode` — кодовая точка Unicode для печатных клавиш;
        // у клавиш без текста он равен 0.
        u32 cp = (u32)e->charCode;
        if (cp == 0 && e->charValue[0] != '\0') {
            // Декодируем первую UTF-8 последовательность charValue.
            const unsigned char* s = reinterpret_cast<const unsigned char*>(e->charValue);
            if (s[0] < 0x80) {
                cp = s[0];
            } else if ((s[0] & 0xE0) == 0xC0 && s[1] != 0) {
                cp = ((u32)(s[0] & 0x1F) << 6) | (u32)(s[1] & 0x3F);
            } else if ((s[0] & 0xF0) == 0xE0 && s[1] != 0 && s[2] != 0) {
                cp = ((u32)(s[0] & 0x0F) << 12) | ((u32)(s[1] & 0x3F) << 6) | (u32)(s[2] & 0x3F);
            } else if ((s[0] & 0xF8) == 0xF0 && s[1] != 0 && s[2] != 0 && s[3] != 0) {
                cp = ((u32)(s[0] & 0x07) << 18) | ((u32)(s[1] & 0x3F) << 12) |
                     ((u32)(s[2] & 0x3F) << 6) | (u32)(s[3] & 0x3F);
            }
        }
        if (cp >= 32 && cp != 0x7F) w->GetInput().OnText(cp);
        return EM_FALSE;
    }

    Key k = KeyFromDomKeyCode((u32)e->keyCode, (u32)e->location);
    if (k == Key::Enter && e->location == DOM_KEY_LOCATION_NUMPAD) k = Key::KeypadEnter;
    if (k == Key::Unknown) return EM_FALSE;

    if (eventType == EMSCRIPTEN_EVENT_KEYDOWN) {
        w->GetInput().OnKey(k, KeyAction::Press, e->repeat ? true : false);
        if (e->repeat) w->GetInput().OnKey(k, KeyAction::Repeat, true);
    } else {
        w->GetInput().OnKey(k, KeyAction::Release, false);
    }
    // Поглощаем клавиши, которые иначе прокрутили бы страницу / сдвинули фокус.
    const bool modifier = e->ctrlKey || e->metaKey || e->altKey;
    if (!modifier && IsScrollKey(k)) return EM_TRUE;
    return EM_FALSE;
}

EM_BOOL OnMouseButtonEvent(int eventType, const EmscriptenMouseEvent* e, void* userData) {
    (void)userData;
    Window* w = g_activeWindow;
    if (!w) return EM_FALSE;
    MouseButton b = MouseButton::Left;
    switch (e->button) {
        case 0: b = MouseButton::Left; break;
        case 1: b = MouseButton::Middle; break;
        case 2: b = MouseButton::Right; break;
        case 3: b = MouseButton::X1; break;
        case 4: b = MouseButton::X2; break;
        default: return EM_FALSE;
    }
    const bool down = eventType == EMSCRIPTEN_EVENT_MOUSEDOWN;
    w->GetInput().OnMouseButton(b, down, {(f32)e->targetX, (f32)e->targetY});
    return EM_TRUE;
}

EM_BOOL OnMouseMoveEvent(int eventType, const EmscriptenMouseEvent* e, void* userData) {
    (void)eventType;
    (void)userData;
    Window* w = g_activeWindow;
    if (!w) return EM_FALSE;
    w->GetInput().OnMouseMove({(f32)e->targetX, (f32)e->targetY});
    return EM_TRUE;
}

EM_BOOL OnWheelEvent(int eventType, const EmscriptenWheelEvent* e, void* userData) {
    (void)eventType;
    (void)userData;
    Window* w = g_activeWindow;
    if (!w) return EM_FALSE;
    // Единица прокрутки движка — «одна ступица колеса» (см. бэкенд macOS,
    // который масштабирует точные/пиксельные дельты на 0.1).
    f32 scale = 1.0f;
    switch (e->deltaMode) {
        case 0: scale = 0.1f; break;   // DOM_DELTA_PIXEL
        case 1: scale = 1.0f; break;   // DOM_DELTA_LINE
        case 2: scale = 10.0f; break;  // DOM_DELTA_PAGE
        default: break;
    }
    w->GetInput().OnScroll({(f32)e->deltaX * scale, (f32)e->deltaY * scale});
    return EM_TRUE;  // не даём странице прокручиваться
}

EM_BOOL OnTouchEvent(int eventType, const EmscriptenTouchEvent* e, void* userData) {
    (void)userData;
    Window* w = g_activeWindow;
    if (!w) return EM_FALSE;
    TouchPhase phase = TouchPhase::Cancel;
    switch (eventType) {
        case EMSCRIPTEN_EVENT_TOUCHSTART: phase = TouchPhase::Down; break;
        case EMSCRIPTEN_EVENT_TOUCHMOVE: phase = TouchPhase::Move; break;
        case EMSCRIPTEN_EVENT_TOUCHEND: phase = TouchPhase::Up; break;
        case EMSCRIPTEN_EVENT_TOUCHCANCEL: phase = TouchPhase::Cancel; break;
        default: return EM_FALSE;
    }
    for (int i = 0; i < e->numTouches; ++i) {
        const EmscriptenTouchPoint& t = e->touches[i];
        if (!t.isChanged) continue;  // событие сообщает обо всех активных указателях
        TouchPoint tp;
        tp.id = t.identifier;
        tp.pos = Vec2{(f32)t.targetX, (f32)t.targetY};
        tp.start = tp.pos;
        tp.phase = phase;
        tp.pressure = 1.0f;  // DOM-событие касания давление не экспонирует
        // Input::OnTouch уже двигает виртуальную мышь для UI-кода.
        w->GetInput().OnTouch(tp);
    }
    return EM_TRUE;
}

EM_BOOL OnResizeEvent(int eventType, const EmscriptenUiEvent* e, void* userData) {
    (void)eventType;
    (void)userData;
    WasmHandleBrowserResize();
    return EM_FALSE;
}

EM_BOOL OnFocusEvent(int eventType, const EmscriptenFocusEvent* e, void* userData) {
    (void)e;
    (void)userData;
    WasmSetFocused(eventType == EMSCRIPTEN_EVENT_FOCUS);
    return EM_FALSE;
}

EM_BOOL OnVisibilityChange(int eventType, const EmscriptenVisibilityChangeEvent* e,
                           void* userData) {
    (void)eventType;
    (void)userData;
    g_visible = !e->hidden;
    WasmSetFocused(g_visible && !e->hidden);
    return EM_FALSE;
}

EM_BOOL OnWebGLContextEvent(int eventType, const void* reserved, void* userData) {
    (void)eventType;
    (void)reserved;
    (void)userData;
    WasmHandleContextEvent(eventType);
    return EM_FALSE;
}

void InstallHandler(const char* what, EMSCRIPTEN_RESULT r) {
    if (r != EMSCRIPTEN_RESULT_SUCCESS) {
        ENG_LOGW("platform", "failed to register the %s handler (%d)", what, (int)r);
    }
}

}  // namespace

void WasmInstallEventHandlers(const char* canvasTarget) {
    if (g_handlersInstalled || canvasTarget == nullptr) return;
    g_handlersInstalled = true;
    const char* document = EMSCRIPTEN_EVENT_TARGET_DOCUMENT;
    const char* window = EMSCRIPTEN_EVENT_TARGET_WINDOW;

    InstallHandler("keydown",
                   emscripten_set_keydown_callback_on_thread(document, nullptr, EM_TRUE,
                                                             OnKeyEvent,
                                                             EM_CALLBACK_THREAD_CONTEXT_CALLING_THREAD));
    InstallHandler("keyup",
                   emscripten_set_keyup_callback_on_thread(document, nullptr, EM_TRUE, OnKeyEvent,
                                                           EM_CALLBACK_THREAD_CONTEXT_CALLING_THREAD));
    InstallHandler("keypress",
                   emscripten_set_keypress_callback_on_thread(document, nullptr, EM_TRUE,
                                                              OnKeyEvent,
                                                              EM_CALLBACK_THREAD_CONTEXT_CALLING_THREAD));
    InstallHandler("mousedown",
                   emscripten_set_mousedown_callback_on_thread(canvasTarget, nullptr, EM_TRUE,
                                                               OnMouseButtonEvent,
                                                               EM_CALLBACK_THREAD_CONTEXT_CALLING_THREAD));
    InstallHandler("mouseup",
                   emscripten_set_mouseup_callback_on_thread(canvasTarget, nullptr, EM_TRUE,
                                                             OnMouseButtonEvent,
                                                             EM_CALLBACK_THREAD_CONTEXT_CALLING_THREAD));
    InstallHandler("mousemove",
                   emscripten_set_mousemove_callback_on_thread(canvasTarget, nullptr, EM_TRUE,
                                                               OnMouseMoveEvent,
                                                               EM_CALLBACK_THREAD_CONTEXT_CALLING_THREAD));
    InstallHandler("wheel",
                   emscripten_set_wheel_callback_on_thread(canvasTarget, nullptr, EM_TRUE,
                                                           OnWheelEvent,
                                                           EM_CALLBACK_THREAD_CONTEXT_CALLING_THREAD));
    InstallHandler("touchstart",
                   emscripten_set_touchstart_callback_on_thread(canvasTarget, nullptr, EM_TRUE,
                                                                OnTouchEvent,
                                                                EM_CALLBACK_THREAD_CONTEXT_CALLING_THREAD));
    InstallHandler("touchmove",
                   emscripten_set_touchmove_callback_on_thread(canvasTarget, nullptr, EM_TRUE,
                                                               OnTouchEvent,
                                                               EM_CALLBACK_THREAD_CONTEXT_CALLING_THREAD));
    InstallHandler("touchend",
                   emscripten_set_touchend_callback_on_thread(canvasTarget, nullptr, EM_TRUE,
                                                              OnTouchEvent,
                                                              EM_CALLBACK_THREAD_CONTEXT_CALLING_THREAD));
    InstallHandler("touchcancel",
                   emscripten_set_touchcancel_callback_on_thread(canvasTarget, nullptr, EM_TRUE,
                                                                 OnTouchEvent,
                                                                 EM_CALLBACK_THREAD_CONTEXT_CALLING_THREAD));
    InstallHandler("resize",
                   emscripten_set_resize_callback_on_thread(window, nullptr, EM_TRUE, OnResizeEvent,
                                                            EM_CALLBACK_THREAD_CONTEXT_CALLING_THREAD));
    InstallHandler("focus",
                   emscripten_set_focus_callback_on_thread(window, nullptr, EM_TRUE, OnFocusEvent,
                                                           EM_CALLBACK_THREAD_CONTEXT_CALLING_THREAD));
    InstallHandler("blur",
                   emscripten_set_blur_callback_on_thread(window, nullptr, EM_TRUE, OnFocusEvent,
                                                          EM_CALLBACK_THREAD_CONTEXT_CALLING_THREAD));
    InstallHandler("visibilitychange",
                   emscripten_set_visibilitychange_callback_on_thread(nullptr, EM_TRUE,
                                                                      OnVisibilityChange,
                                                                      EM_CALLBACK_THREAD_CONTEXT_CALLING_THREAD));
    InstallHandler("webglcontextlost",
                   emscripten_set_webglcontextlost_callback_on_thread(canvasTarget, nullptr, EM_TRUE,
                                                                      OnWebGLContextEvent,
                                                                      EM_CALLBACK_THREAD_CONTEXT_CALLING_THREAD));
    InstallHandler("webglcontextrestored",
                   emscripten_set_webglcontextrestored_callback_on_thread(
                       canvasTarget, nullptr, EM_TRUE, OnWebGLContextEvent,
                       EM_CALLBACK_THREAD_CONTEXT_CALLING_THREAD));
    EngJsEnsureCanvasFocusable(canvasTarget);
    ENG_LOGI("platform", "DOM event handlers installed on %s", canvasTarget);
}

// ---------------------------------------------------------------------------
// Window::Impl
// ---------------------------------------------------------------------------
struct Window::Impl {
    bool shouldClose = false;
    bool focused = true;
    bool fullscreen = false;
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
    std::string canvasTarget;
};

Window* WasmActiveWindow() { return g_activeWindow; }

namespace {

// Пересчитывает backing store canvas из его CSS-размера * devicePixelRatio.
void SyncCanvasSize(Window::Impl* impl, bool notify) {
    if (!impl) return;
    double cssW = 0.0, cssH = 0.0;
    if (emscripten_get_element_css_size(impl->canvasTarget.c_str(), &cssW, &cssH) !=
            EMSCRIPTEN_RESULT_SUCCESS ||
        cssW < 1.0 || cssH < 1.0) {
        cssW = impl->desc.width;
        cssH = impl->desc.height;
        if (cssW < 1.0) cssW = 1280.0;
        if (cssH < 1.0) cssH = 720.0;
        emscripten_set_element_css_size(impl->canvasTarget.c_str(), cssW, cssH);
    }
    const double dpr = emscripten_get_device_pixel_ratio();
    const f32 scale = dpr > 0.0 ? (f32)dpr : 1.0f;
    const int fbW = (int)(cssW * scale + 0.5);
    const int fbH = (int)(cssH * scale + 0.5);
    int canvasW = 0, canvasH = 0;
    if (emscripten_get_canvas_element_size(impl->canvasTarget.c_str(), &canvasW, &canvasH) !=
            EMSCRIPTEN_RESULT_SUCCESS ||
        canvasW != fbW || canvasH != fbH) {
        if (emscripten_set_canvas_element_size(impl->canvasTarget.c_str(), fbW, fbH) !=
            EMSCRIPTEN_RESULT_SUCCESS) {
            ENG_LOGW("platform", "failed to resize the canvas to %dx%d", fbW, fbH);
        }
    }
    const bool dpiChanged = impl->dpiScale != scale;
    impl->width = (int)cssW;
    impl->height = (int)cssH;
    impl->fbWidth = fbW;
    impl->fbHeight = fbH;
    impl->dpiScale = scale;
    Window* w = g_activeWindow;
    if (notify && w) {
        if (dpiChanged && w->callbacks.onDpiChanged) w->callbacks.onDpiChanged(scale);
        if (w->callbacks.onResize) w->callbacks.onResize(fbW, fbH);
    }
    ENG_LOGI("platform", "canvas resized: %dx%d css, %dx%d px, dpr %.2f", impl->width, impl->height,
             fbW, fbH, (double)scale);
}

}  // namespace

void WasmHandleBrowserResize() {
    if (!g_activeImpl) return;
    SyncCanvasSize(g_activeImpl, true);
}

void WasmSetFocused(bool focused) {
    g_focused = focused;
    if (g_activeImpl) g_activeImpl->focused = focused;
    Window* w = g_activeWindow;
    if (w && w->callbacks.onFocus) w->callbacks.onFocus(focused);
}

void WasmHandleContextEvent(int eventType) {
    const bool lost = eventType == EMSCRIPTEN_EVENT_WEBGLCONTEXTLOST;
    g_contextLost = lost;
    ENG_LOGW("platform",
             "WebGL context %s (WindowCallbacks has no context hook: mapped to onFocus)",
             lost ? "lost" : "restored");
    Window* w = g_activeWindow;
    if (w && w->callbacks.onFocus) w->callbacks.onFocus(lost ? false : true);
}

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------
Window::Window() : impl_(new Impl()) {
    g_activeWindow = this;
    g_activeImpl = impl_.get();
}

Window::~Window() {
    Destroy();
    if (g_activeWindow == this) g_activeWindow = nullptr;
    if (g_activeImpl == impl_.get()) g_activeImpl = nullptr;
    impl_.reset();
}

bool Window::Create(const WindowDesc& desc) {
    impl_->desc = desc;
    impl_->title = desc.title;
    impl_->mode = desc.mode;
    impl_->width = desc.width;
    impl_->height = desc.height;
    g_activeWindow = this;

    // 1. Находим (или создаём) canvas, указанный в Module.canvas / "#canvas".
    char targetBuffer[256];
    targetBuffer[0] = '\0';
    const int targetLen = EngJsCanvasTarget(targetBuffer, (int)sizeof(targetBuffer));
    if (targetLen < 0) {
        ENG_LOGE("platform", "no canvas element found and it could not be created");
        return false;
    }
    impl_->canvasTarget = targetBuffer[0] != '\0' ? targetBuffer : "#canvas";

    // 2. Задаём размер backing store: canvas.width/height = css-размер * devicePixelRatio.
    SyncCanvasSize(impl_.get(), false);

    // 3. Контекст WebGL2 (OpenGL ES 3.0).
    EmscriptenWebGLContextAttributes attrs;
    emscripten_webgl_init_context_attributes(&attrs);
    attrs.majorVersion = desc.glMajor >= 3 || desc.glMajor == 0 ? 2 : 1;
    attrs.minorVersion = 0;
    attrs.alpha = desc.transparent ? EM_TRUE : EM_FALSE;
    attrs.depth = desc.depthBuffer ? EM_TRUE : EM_FALSE;
    attrs.stencil = desc.stencilBuffer ? EM_TRUE : EM_FALSE;
    attrs.antialias = desc.msaaSamples > 1 ? EM_TRUE : EM_FALSE;
    attrs.premultipliedAlpha = EM_TRUE;
    attrs.preserveDrawingBuffer = EM_FALSE;
    attrs.powerPreference = EM_WEBGL_POWER_PREFERENCE_HIGH_PERFORMANCE;
    attrs.failIfMajorPerformanceCaveat = EM_FALSE;
    attrs.enableExtensionsByDefault = EM_TRUE;
    attrs.explicitSwapControl = EM_FALSE;
    attrs.proxyContextToMainThread = EMSCRIPTEN_WEBGL_CONTEXT_PROXY_DISALLOW;
    attrs.renderViaOffscreenBackBuffer = EM_FALSE;

    g_context = emscripten_webgl_create_context(impl_->canvasTarget.c_str(), &attrs);
    if (g_context <= 0) {
        ENG_LOGW("platform", "WebGL2 context creation failed; retrying with WebGL defaults");
        EmscriptenWebGLContextAttributes fallback;
        emscripten_webgl_init_context_attributes(&fallback);
        fallback.majorVersion = 2;
        g_context = emscripten_webgl_create_context(impl_->canvasTarget.c_str(), &fallback);
    }
    if (g_context <= 0) {
        ENG_LOGE("platform", "failed to create a WebGL context on %s", impl_->canvasTarget.c_str());
        return false;
    }
    if (emscripten_webgl_make_context_current(g_context) != EMSCRIPTEN_RESULT_SUCCESS) {
        ENG_LOGE("platform", "failed to make the WebGL context current");
        return false;
    }

    // 4. DOM-колбэки ввода (устанавливаются один раз за процесс).
    WasmInstallEventHandlers(impl_->canvasTarget.c_str());

    SetVSync(desc.vsync);
    SetCursorMode(0);
    impl_->focused = g_focused && g_visible;
    SyncCanvasSize(impl_.get(), false);
    ENG_LOGI("platform", "WebGL2 context ready on %s (%dx%d px, dpr %.2f)",
             impl_->canvasTarget.c_str(), impl_->fbWidth, impl_->fbHeight, (double)impl_->dpiScale);
    return true;
}

void Window::Destroy() {
    if (!impl_) return;
    if (g_context) {
        if (emscripten_webgl_get_current_context() == g_context) {
            emscripten_webgl_make_context_current(0);
        }
        emscripten_webgl_destroy_context(g_context);
        g_context = 0;
    }
    impl_->shouldClose = false;
}

void Window::PollEvents() {
    // Браузер сам толкает события в DOM-колбэки; качать очередь нечего.
}

void Window::SwapBuffers() {
    // При explicitSwapControl = false браузер показывает drawing buffer в конце
    // колбэка requestAnimationFrame, поэтому делать ничего не нужно.
}

bool Window::ShouldClose() const { return impl_->shouldClose; }

void Window::RequestClose() {
    if (impl_->shouldClose) return;
    impl_->shouldClose = true;
    if (callbacks.onClose) callbacks.onClose();
}

void Window::SetTitle(const std::string& title) {
    impl_->title = title;
    EM_ASM({ document.title = UTF8ToString($0); }, title.c_str());
}

void Window::SetSize(int w, int h) {
    if (w < 1 || h < 1) return;
    impl_->desc.width = w;
    impl_->desc.height = h;
    emscripten_set_element_css_size(impl_->canvasTarget.c_str(), (double)w, (double)h);
    SyncCanvasSize(impl_.get(), true);
}

void Window::SetMode(WindowMode mode) {
    impl_->mode = mode;
    impl_->desc.mode = mode;
    const int fullscreen = mode == WindowMode::Windowed ? 0 : 1;
    EngJsRequestFullscreen(fullscreen);
    impl_->fullscreen = fullscreen != 0;
}

void Window::SetVSync(bool enabled) {
    // requestAnimationFrame всегда заблокирован на vsync; отключить его из wasm
    // нельзя, не перейдя на worker + OffscreenCanvas.
    impl_->desc.vsync = enabled;
}

void Window::Minimize() {}
void Window::Maximize() {}
void Window::Restore() {}
void Window::Show() { EngJsSetDisplay(1); }
void Window::Hide() { EngJsSetDisplay(0); }
void Window::Focus() { EngJsFocusWindow(); }

void Window::MakeCurrent() {
    if (g_context) emscripten_webgl_make_context_current(g_context);
}

void Window::WaitEventsTimeout(f32 seconds) {
    // emscripten_sleep() потребовал бы Asyncify и подвесил бы вкладку браузера.
    (void)seconds;
}

void Window::SetCursorVisible(bool visible) {
    impl_->cursorVisible = visible;
    EngJsSetCursor(visible ? 0 : 1);
}

void Window::SetCursorMode(int mode) {
    impl_->cursorMode = mode;
    // Браузер может скрыть указатель, но не может захватить/заблокировать его
    // относительно без (асинхронного) Pointer Lock API.
    EngJsSetCursor(mode == 0 ? 0 : 1);
}

void Window::SetClipboardText(const std::string& text) {
    // navigator.clipboard.writeText() асинхронен и разрешён только по жесту
    // пользователя; сбои проглатываются в JS-помощнике.
    EngJsSetClipboard(text.c_str());
}

std::string Window::GetClipboardText() const {
    // Синхронно читать буфер обмена в браузере невозможно
    // (navigator.clipboard.readText() возвращает promise, а document.execCommand
    // ('paste') недоступен wasm). Задокументированное ограничение: возвращает "".
    static bool warned = false;
    if (!warned) {
        warned = true;  // UI-код может опрашивать это каждый кадр; логируем только раз
        ENG_LOGW("platform", "GetClipboardText() is not supported on Web (async-only API)");
    }
    return {};
}

int Window::Width() const { return impl_->width; }
int Window::Height() const { return impl_->height; }

int Window::FramebufferWidth() const {
    int w = 0, h = 0;
    if (!impl_->canvasTarget.empty() &&
        emscripten_get_canvas_element_size(impl_->canvasTarget.c_str(), &w, &h) ==
            EMSCRIPTEN_RESULT_SUCCESS &&
        w > 0) {
        return w;
    }
    return impl_->fbWidth > 0 ? impl_->fbWidth : 1;
}

int Window::FramebufferHeight() const {
    int w = 0, h = 0;
    if (!impl_->canvasTarget.empty() &&
        emscripten_get_canvas_element_size(impl_->canvasTarget.c_str(), &w, &h) ==
            EMSCRIPTEN_RESULT_SUCCESS &&
        h > 0) {
        return h;
    }
    return impl_->fbHeight > 0 ? impl_->fbHeight : 1;
}

f32 Window::DpiScale() const {
    const double dpr = emscripten_get_device_pixel_ratio();
    return dpr > 0.0 ? (f32)dpr : 1.0f;
}

f32 Window::Aspect() const {
    const int h = FramebufferHeight();
    return h > 0 ? (f32)FramebufferWidth() / (f32)h : 1.0f;
}

bool Window::IsFocused() const { return impl_->focused && g_visible; }

bool Window::IsMinimized() const { return !g_visible; }

bool Window::IsFullscreen() const {
    return EngJsIsFullscreen() != 0 || impl_->fullscreen;
}

Vec2 Window::MousePosition() const { return GetInput().MousePos(); }

const std::string& Window::Title() const { return impl_->title; }

void* Window::NativeHandle() const {
    // Ссылку на JS-элемент canvas нельзя представить указателем; вместо неё
    // экспонируется handle контекста WebGL.
    return reinterpret_cast<void*>(static_cast<uintptr_t>(g_context));
}

void* Window::NativeDisplay() const { return nullptr; }

void* (*Window::GLGetProcAddress() const)(const char*) { return &WasmGLGetProcAddress; }

}  // namespace crossrender

#else

// В сборках без WASM эта единица трансляции пуста.
namespace crossrender {}

#endif  // ENG_PLATFORM_WASM
