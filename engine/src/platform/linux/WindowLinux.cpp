// Платформенный слой Linux: нативное окно X11 + контекст GLX (без GLFW/SDL).
#include "crossrender/platform/Window.h"

#include "crossrender/core/Log.h"
#include "crossrender/core/Time.h"
#include "crossrender/platform/Platform.h"

#if defined(ENG_PLATFORM_LINUX)

#include <GL/glx.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>

#include <dlfcn.h>

#include <cstdio>
#include <string>
#include <vector>
#include <cstdlib>
#include <cstring>

#if defined(__clang__) || defined(__GNUC__)
// glXChooseFBConfig и подобные помечены устаревшими в новых заголовках в пользу
// написаний ...ARB; использование переносимых имён намеренно.
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

namespace crossrender {
namespace {

// ---------------------------------------------------------------------------
// Поверхность GLX 1.3 / GLX_ARB_get_proc_address. Объявлена вручную, потому что
// движок намеренно избегает включения системных GL-заголовков.
// ---------------------------------------------------------------------------
using PFNGLXCREATECONTEXTATTRIBSARBPROC = GLXContext (*)(Display*, GLXFBConfig, GLXContext, Bool,
                                                         const int*);
using PFNGLXSWAPINTERVALEXTPROC = void (*)(Display*, GLXDrawable, int);

constexpr int kGlxContextMajorVersionArb = 0x2091;
constexpr int kGlxContextMinorVersionArb = 0x2092;
constexpr int kGlxContextProfileMaskArb = 0x9126;
constexpr int kGlxContextCoreProfileBitArb = 0x00000001;
constexpr int kGlxSampleBuffersArb = 0x2041;
constexpr int kGlxSamplesArb = 0x2042;

constexpr unsigned int kGlVersion = 0x1F02;

void* LinuxGLGetProcAddress(const char* name) {
    if (!name) return nullptr;
    // glXGetProcAddressARB отвечает за расширения и core-точки входа 1.2+;
    // символы GL 1.1 приходят из слинкованной libGL (RTLD_DEFAULT).
    void* p = reinterpret_cast<void*>(
        glXGetProcAddressARB(reinterpret_cast<const GLubyte*>(name)));
    if (p) return p;
    // Точки входа GL 1.1 экспортируются напрямую libGL, которая уже слинкована
    // с процессом, поэтому запасным вариантом служит глобальная таблица символов.
    return dlsym(RTLD_DEFAULT, name);
}

// ---------------------------------------------------------------------------
// Keysym -> Key движка. Латинские буквы и цифры в таблице keysym X11 идут
// подряд, поэтому таблица нужна только для управляющих/символьных клавиш.
// ---------------------------------------------------------------------------
Key KeyFromKeysym(KeySym ks) {
    if (ks >= XK_a && ks <= XK_z)
        return static_cast<Key>(static_cast<int>(Key::A) + (ks - XK_a));
    if (ks >= XK_A && ks <= XK_Z)
        return static_cast<Key>(static_cast<int>(Key::A) + (ks - XK_A));
    if (ks >= XK_0 && ks <= XK_9)
        return static_cast<Key>(static_cast<int>(Key::Num0) + (ks - XK_0));
    if (ks >= XK_F1 && ks <= XK_F24)
        return static_cast<Key>(static_cast<int>(Key::F1) + (ks - XK_F1));
    if (ks >= XK_KP_0 && ks <= XK_KP_9)
        return static_cast<Key>(static_cast<int>(Key::Keypad0) + (ks - XK_KP_0));

    switch (ks) {
        case XK_space: return Key::Space;
        case XK_apostrophe: return Key::Apostrophe;
        case XK_comma: return Key::Comma;
        case XK_minus: return Key::Minus;
        case XK_period: return Key::Period;
        case XK_slash: return Key::Slash;
        case XK_semicolon: return Key::Semicolon;
        case XK_equal: return Key::Equal;
        case XK_bracketleft: return Key::LeftBracket;
        case XK_backslash: return Key::Backslash;
        case XK_bracketright: return Key::RightBracket;
        case XK_grave: return Key::Grave;
        case XK_Escape: return Key::Escape;
        case XK_Return: return Key::Enter;
        case XK_Tab: return Key::Tab;
        case XK_BackSpace: return Key::Backspace;
        case XK_Insert: return Key::Insert;
        case XK_Delete: return Key::Delete;
        case XK_Right: return Key::Right;
        case XK_Left: return Key::Left;
        case XK_Down: return Key::Down;
        case XK_Up: return Key::Up;
        case XK_Page_Up: return Key::PageUp;
        case XK_Page_Down: return Key::PageDown;
        case XK_Home: return Key::Home;
        case XK_End: return Key::End;
        case XK_Caps_Lock: return Key::CapsLock;
        case XK_Scroll_Lock: return Key::ScrollLock;
        case XK_Num_Lock: return Key::NumLock;
        case XK_Print: return Key::PrintScreen;
        case XK_Pause: return Key::Pause;
        case XK_KP_Decimal: return Key::KeypadDecimal;
        case XK_KP_Divide: return Key::KeypadDivide;
        case XK_KP_Multiply: return Key::KeypadMultiply;
        case XK_KP_Subtract: return Key::KeypadSubtract;
        case XK_KP_Add: return Key::KeypadAdd;
        case XK_KP_Enter: return Key::KeypadEnter;
        case XK_KP_Equal: return Key::KeypadEqual;
        case XK_Shift_L: return Key::LeftShift;
        case XK_Shift_R: return Key::RightShift;
        case XK_Control_L: return Key::LeftControl;
        case XK_Control_R: return Key::RightControl;
        case XK_Alt_L: return Key::LeftAlt;
        case XK_Alt_R: return Key::RightAlt;
        case XK_Meta_L: return Key::LeftSuper;
        case XK_Meta_R: return Key::RightSuper;
        case XK_Super_L: return Key::LeftSuper;
        case XK_Super_R: return Key::RightSuper;
        case XK_Menu: return Key::Menu;
        default: return Key::Unknown;
    }
}

KeySym KeysymFromEvent(Display* display, XKeyEvent* event) {
    // Индекс 0 — keysym без шифта, т.е. стабильная физическая идентичность
    // клавиши для Key-энума движка (XLookupString применила бы модификаторы
    // и превратила Shift+1 в XK_exclam).
    if (display) {
        const KeySym ks = XkbKeycodeToKeysym(display, static_cast<KeyCode>(event->keycode), 0, 0);
        if (ks != NoSymbol) return ks;
    }
    KeySym ks = NoSymbol;
    char buffer[8];
    XLookupString(event, buffer, sizeof(buffer), &ks, nullptr);
    return ks;
}

// Преобразует keysym в кодовую точку Unicode (Latin-1 и диапазон смещения
// 0x01000000, который X11 использует для Unicode-keysym).
u32 CodepointFromKeysym(KeySym ks) {
    if (ks == NoSymbol) return 0;
    if (ks >= 0x01000000 && ks <= 0x0110FFFF) return static_cast<u32>(ks - 0x01000000);
    if (ks >= 0x20 && ks <= 0x7E) return static_cast<u32>(ks);
    if (ks >= 0xA0 && ks <= 0xFF) return static_cast<u32>(ks);
    return 0;
}

// ---------------------------------------------------------------------------
// HiDPI: в X11 нет масштаба на окно. Обычный источник — X-ресурс Xft.dpi
// (задаётся средами рабочего стола / `xrdb`); когда его нет, физический DPI
// вычисляется из размера экрана, иначе используется 1.0.
// ---------------------------------------------------------------------------
f32 QueryDpiScale(Display* display, bool* fromXft) {
    if (fromXft) *fromXft = false;
    if (!display) return 1.0f;

    char* resource = XResourceManagerString(display);
    if (resource) {
        char* value = strstr(resource, "Xft.dpi:");
        if (value) {
            value += 8;
            while (*value == ' ' || *value == '\t') ++value;
            const double dpi = strtod(value, nullptr);
            if (dpi > 1.0) {
                if (fromXft) *fromXft = true;
                return static_cast<f32>(dpi / 96.0);
            }
        }
    }

    const int screen = DefaultScreen(display);
    const int widthMm = DisplayWidthMM(display, screen);
    const int widthPx = DisplayWidth(display, screen);
    if (widthMm > 0 && widthPx > 0) {
        const f32 scale = (static_cast<f32>(widthPx) * 25.4f) /
                          (static_cast<f32>(widthMm) * 96.0f);
        // Игнорируем абсурдные значения EDID.
        if (scale > 0.5f && scale < 4.0f) return scale;
    }
    return 1.0f;
}

// ---------------------------------------------------------------------------
// Буфер обмена. В X11 нет хранилища буфера: владелец отдаёт выделение по
// запросу. Чтение — поэтому круговой обмен запрос/ответ с таймаутом;
// запись означает объявление себя владельцем выделения и ответы на
// события SelectionRequest во время опроса.
// ---------------------------------------------------------------------------
struct ClipboardState {
    Display* display = nullptr;
    ::Window owner = 0;  // скрытое окно в роли requestor/owner
    Atom clipboard = None;
    Atom utf8 = None;
    Atom targets = None;
    Atom property = None;
    std::string outgoing;
    bool owns = false;

    bool EnsureAtoms(Display* dpy) {
        display = dpy;
        if (clipboard != None) return true;
        clipboard = XInternAtom(dpy, "CLIPBOARD", False);
        utf8 = XInternAtom(dpy, "UTF8_STRING", False);
        targets = XInternAtom(dpy, "TARGETS", False);
        property = XInternAtom(dpy, "ENG_CLIPBOARD", False);
        return clipboard != None;
    }

    ::Window EnsureOwnerWindow(Display* dpy) {
        if (owner) return owner;
        const int screen = DefaultScreen(dpy);
        XSetWindowAttributes attrs{};
        attrs.override_redirect = True;
        attrs.event_mask = PropertyChangeMask;
        owner = XCreateWindow(dpy, RootWindow(dpy, screen), 0, 0, 1, 1, 0, CopyFromParent,
                              InputOnly, CopyFromParent, CWOverrideRedirect | CWEventMask, &attrs);
        return owner;
    }

    // Разбирает события (ищет SelectionNotify), пока не истечёт `deadline`.
    bool WaitFor(Display* dpy, Atom expected, Atom* outType, unsigned char** outData,
                 unsigned long* outItems, int timeoutMs = 100) {
        const f64 deadline = NowSeconds() + static_cast<f64>(timeoutMs) / 1000.0;
        while (NowSeconds() < deadline) {
            while (XPending(dpy) > 0) {
                XEvent event;
                XNextEvent(dpy, &event);
                if (event.type == SelectionNotify && event.xselection.selection == expected) {
                    if (event.xselection.property == None) return false;
                    Atom type = None;
                    int format = 0;
                    unsigned long bytesAfter = 0;
                    // Успех — 0; любой другой статус означает, что владелец не смог
                    // отдать данные для этого target.
                    if (XGetWindowProperty(dpy, owner, property, 0, 1 << 20, True,
                                           AnyPropertyType, &type, &format, outItems, &bytesAfter,
                                           outData) == Success) {
                        if (outType) *outType = type;
                        return true;
                    }
                    return false;
                }
                // Пока мы здесь, обслуживаем ожидающий SelectionRequest.
                if (event.type == SelectionRequest) ServeRequest(dpy, &event.xselectionrequest);
            }
            SleepMs(1);
        }
        return false;
    }

    void ServeRequest(Display* dpy, XSelectionRequestEvent* req) {
        XSelectionEvent reply{};
        reply.type = SelectionNotify;
        reply.display = req->display;
        reply.requestor = req->requestor;
        reply.selection = req->selection;
        reply.target = req->target;
        reply.time = req->time;
        reply.property = None;

        if (req->target == targets) {
            const Atom supported[] = {utf8, XA_STRING};
            XChangeProperty(dpy, req->requestor, req->property, XA_ATOM, 32, PropModeReplace,
                            reinterpret_cast<const unsigned char*>(supported), 2);
            reply.property = req->property;
        } else if (req->target == utf8 || req->target == XA_STRING) {
            XChangeProperty(dpy, req->requestor, req->property, req->target, 8, PropModeReplace,
                            reinterpret_cast<const unsigned char*>(outgoing.data()),
                            static_cast<int>(outgoing.size()));
            reply.property = req->property;
        }
        XSendEvent(dpy, req->requestor, False, 0, reinterpret_cast<XEvent*>(&reply));
        XFlush(dpy);
    }
};

ClipboardState& Clipboard() {
    static ClipboardState state;
    return state;
}

}  // namespace

// ---------------------------------------------------------------------------
// Window::Impl
// ---------------------------------------------------------------------------
struct Window::Impl {
    Display* display = nullptr;
    int screen = 0;
    ::Window window = 0;
    Colormap colormap = 0;
    GLXContext context = nullptr;
    XVisualInfo* visual = nullptr;
    XIM inputMethod = nullptr;
    XIC inputContext = nullptr;
    Atom wmDeleteWindow = None;
    // Указатель владельца для (статического) пути ошибок/событий X11.
    void* ownerWindow = nullptr;
    bool ownsDisplay = false;
    bool shouldClose = false;
    bool focused = true;
    bool fullscreen = false;
    bool relativeMouse = false;
    WindowMode mode = WindowMode::Windowed;
    WindowDesc desc;
    int width = 0, height = 0;
    int fbWidth = 0, fbHeight = 0;
    f32 dpiScale = 1.0f;
    std::string title;
    int cursorMode = 0;
    // Оконная геометрия, сохранённая перед переключением в полноэкранный режим.
    int savedX = 0, savedY = 0, savedW = 0, savedH = 0;
    std::vector<Atom> netWmState;
};

namespace {

void UpdateMetrics(Window::Impl* impl) {
    if (!impl->display || !impl->window) return;
    XWindowAttributes attrs{};
    if (!XGetWindowAttributes(impl->display, impl->window, &attrs)) return;
    impl->width = attrs.width;
    impl->height = attrs.height;
    // X11 сообщает пиксели напрямую; HiDPI выражается только через dpiScale.
    impl->fbWidth = attrs.width;
    impl->fbHeight = attrs.height;
}

void WarpToCenter(Window::Impl* impl) {
    if (!impl->display || !impl->window || !impl->relativeMouse) return;
    XWarpPointer(impl->display, None, impl->window, 0, 0, 0, 0, impl->width / 2,
                 impl->height / 2);
    XFlush(impl->display);
}

// Минимальный выбор GLX-конфигурации с поддержкой WM_DELETE_WINDOW. Предпочитает
// конфиг с наибольшим числом сэмплов, не превышающим запрошенное.
GLXFBConfig ChooseFBConfig(Display* display, int screen, const WindowDesc& desc,
                           int* outSamples) {
    *outSamples = 0;
    int samples = desc.msaaSamples > 1 ? desc.msaaSamples : 0;

    for (int attempt = 0; attempt < 2; ++attempt) {
        int attrs[40];
        int i = 0;
        attrs[i++] = GLX_X_RENDERABLE;
        attrs[i++] = True;
        attrs[i++] = GLX_DRAWABLE_TYPE;
        attrs[i++] = GLX_WINDOW_BIT;
        attrs[i++] = GLX_RENDER_TYPE;
        attrs[i++] = GLX_RGBA_BIT;
        attrs[i++] = GLX_RED_SIZE;
        attrs[i++] = 8;
        attrs[i++] = GLX_GREEN_SIZE;
        attrs[i++] = 8;
        attrs[i++] = GLX_BLUE_SIZE;
        attrs[i++] = 8;
        attrs[i++] = GLX_ALPHA_SIZE;
        attrs[i++] = 8;
        attrs[i++] = GLX_DEPTH_SIZE;
        attrs[i++] = desc.depthBuffer ? 24 : 0;
        if (desc.stencilBuffer) {
            attrs[i++] = GLX_STENCIL_SIZE;
            attrs[i++] = 8;
        }
        attrs[i++] = GLX_DOUBLEBUFFER;
        attrs[i++] = True;
        if (samples > 1) {
            attrs[i++] = kGlxSampleBuffersArb;
            attrs[i++] = 1;
            attrs[i++] = kGlxSamplesArb;
            attrs[i++] = samples;
        }
        attrs[i++] = None;

        int count = 0;
        GLXFBConfig* configs = glXChooseFBConfig(display, screen, attrs, &count);
        if (configs && count > 0) {
            if (samples > 1) *outSamples = samples;
            GLXFBConfig chosen = configs[0];
            XFree(configs);
            return chosen;
        }
        if (configs) XFree(configs);
        if (samples <= 1) break;
        ENG_LOGW("platform", "MSAA %d unavailable, retrying without", samples);
        samples = 0;
    }
    return nullptr;
}

bool SetFullscreen(Window::Impl* impl, bool enable, bool borderless) {
    Display* display = impl->display;
    if (!display || !impl->window) return false;
    Atom wmState = XInternAtom(display, "_NET_WM_STATE", False);
    Atom wmFullscreen = XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);
    if (wmState == None || wmFullscreen == None) return false;

    if (enable) {
        if (!impl->fullscreen) {
            XWindowAttributes attrs{};
            XGetWindowAttributes(display, impl->window, &attrs);
            impl->savedW = attrs.width;
            impl->savedH = attrs.height;
            impl->savedX = attrs.x;
            impl->savedY = attrs.y;
        }
        XEvent event{};
        event.xclient.type = ClientMessage;
        event.xclient.window = impl->window;
        event.xclient.message_type = wmState;
        event.xclient.format = 32;
        event.xclient.data.l[0] = 1;  // _NET_WM_STATE_ADD
        event.xclient.data.l[1] = static_cast<long>(wmFullscreen);
        event.xclient.data.l[2] = 0;
        XSendEvent(display, DefaultRootWindow(display), False,
                   SubstructureRedirectMask | SubstructureNotifyMask, &event);
        impl->fullscreen = true;
    } else {
        XEvent event{};
        event.xclient.type = ClientMessage;
        event.xclient.window = impl->window;
        event.xclient.message_type = wmState;
        event.xclient.format = 32;
        event.xclient.data.l[0] = 0;  // _NET_WM_STATE_REMOVE
        event.xclient.data.l[1] = static_cast<long>(wmFullscreen);
        event.xclient.data.l[2] = 0;
        XSendEvent(display, DefaultRootWindow(display), False,
                   SubstructureRedirectMask | SubstructureNotifyMask, &event);
        impl->fullscreen = false;
    }
    XFlush(display);
    if (borderless) {
        // Borderless выражается снятием подсказки декораций.
        struct MotifHints {
            unsigned long flags;
            unsigned long functions;
            unsigned long decorations;
            long inputMode;
            unsigned long status;
        };
        MotifHints hints{};
        hints.flags = 2;  // MWM_HINTS_DECORATIONS
        hints.decorations = 0;
        const Atom motif = XInternAtom(display, "_MOTIF_WM_HINTS", False);
        if (motif != None) {
            XChangeProperty(display, impl->window, motif, motif, 32, PropModeReplace,
                            reinterpret_cast<unsigned char*>(&hints), 5);
        }
    }
    UpdateMetrics(impl);
    return true;
}

// Обрабатывает одно X-событие; вынесено, чтобы круговой обмен буфера обмена мог продолжать качать события.
void HandleEvent(Window::Impl* impl, XEvent* event) {
    if (!impl || !impl->ownerWindow) return;
    auto* owner = reinterpret_cast<Window*>(impl->ownerWindow);

    switch (event->type) {
        case ClientMessage: {
            if (event->xclient.message_type == XInternAtom(impl->display, "WM_PROTOCOLS", False) &&
                static_cast<Atom>(event->xclient.data.l[0]) == impl->wmDeleteWindow) {
                owner->RequestClose();
            }
            break;
        }
        case ConfigureNotify: {
            if (event->xconfigure.width != impl->width ||
                event->xconfigure.height != impl->height) {
                UpdateMetrics(impl);
                if (owner->callbacks.onResize)
                    owner->callbacks.onResize(impl->fbWidth, impl->fbHeight);
            } else {
                UpdateMetrics(impl);
            }
            break;
        }
        case FocusIn: {
            impl->focused = true;
            if (owner->callbacks.onFocus) owner->callbacks.onFocus(true);
            break;
        }
        case FocusOut: {
            impl->focused = false;
            if (owner->callbacks.onFocus) owner->callbacks.onFocus(false);
            break;
        }
        case MotionNotify: {
            owner->GetInput().OnMouseMove(
                {static_cast<f32>(event->xmotion.x), static_cast<f32>(event->xmotion.y)});
            if (impl->relativeMouse) WarpToCenter(impl);
            break;
        }
        case ButtonPress:
        case ButtonRelease: {
            const bool down = event->type == ButtonPress;
            const unsigned int button = event->xbutton.button;
            if (button == Button4 || button == Button5 || button == Button6 || button == Button7) {
                if (!down) break;  // реагируем только на нажатие, чтобы не считать дважды
                const f32 tick = 1.0f;
                switch (button) {
                    case Button4: owner->GetInput().OnScroll({0.0f, tick}); break;
                    case Button5: owner->GetInput().OnScroll({0.0f, -tick}); break;
                    case Button6: owner->GetInput().OnScroll({tick, 0.0f}); break;
                    case Button7: owner->GetInput().OnScroll({-tick, 0.0f}); break;
                    default: break;
                }
                break;
            }
            MouseButton mb = MouseButton::Left;
            switch (button) {
                case Button1: mb = MouseButton::Left; break;
                case Button2: mb = MouseButton::Middle; break;
                case Button3: mb = MouseButton::Right; break;
                case 8: mb = MouseButton::X1; break;
                case 9: mb = MouseButton::X2; break;
                default: mb = MouseButton::Left; break;
            }
            owner->GetInput().OnMouseButton(
                mb, down,
                {static_cast<f32>(event->xbutton.x), static_cast<f32>(event->xbutton.y)});
            break;
        }
        case KeyPress:
        case KeyRelease: {
            const bool pressed = event->type == KeyPress;
            KeySym ks = KeysymFromEvent(impl->display, &event->xkey);
            // X11 сигнализирует автоповтор парой KeyRelease, сразу за которой идёт
            // KeyPress с тем же timestamp; схлопываем её в Repeat.
            bool repeat = false;
            if (!pressed && XPending(impl->display) > 0) {
                XEvent next;
                XPeekEvent(impl->display, &next);
                if (next.type == KeyPress && next.xkey.time == event->xkey.time &&
                    next.xkey.keycode == event->xkey.keycode) {
                    repeat = true;
                }
            }
            const Key key = KeyFromKeysym(ks);
            if (pressed) {
                owner->GetInput().OnKey(key, repeat ? KeyAction::Repeat : KeyAction::Press, repeat);
                // Текстовый ввод: комбинации Ctrl/Alt — горячие клавиши, а не текст.
                // При наличии контекста ввода предпочтителен Xutf8LookupString
                // (он декодирует локаль в UTF-8); иначе используется XLookupString
                // плюс запасной путь keysym -> кодовая точка.
                const unsigned int state = event->xkey.state;
                if ((state & (ControlMask | Mod1Mask)) == 0) {
                    char buffer[64];
                    KeySym ignored = NoSymbol;
                    int count = 0;
#if defined(X_HAVE_UTF8_STRING)
                    if (impl->inputContext) {
                        XComposeStatus compose{};
                        count = Xutf8LookupString(impl->inputContext, &event->xkey, buffer,
                                                  sizeof(buffer), &ignored, &compose);
                    } else
#endif
                    {
                        count = XLookupString(&event->xkey, buffer, sizeof(buffer), &ignored,
                                              nullptr);
                    }
                    if (count > 0) {
                        // Движку нужны кодовые точки, а не байты UTF-8.
                        const unsigned char* p = reinterpret_cast<const unsigned char*>(buffer);
                        const unsigned char* end = p + count;
                        while (p < end) {
                            u32 cp = 0;
                            if (*p < 0x80) {
                                cp = *p++;
                            } else if ((*p & 0xE0) == 0xC0 && p + 1 < end) {
                                cp = static_cast<u32>((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
                                p += 2;
                            } else if ((*p & 0xF0) == 0xE0 && p + 2 < end) {
                                cp = static_cast<u32>((p[0] & 0x0F) << 12) |
                                     static_cast<u32>((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
                                p += 3;
                            } else if ((*p & 0xF8) == 0xF0 && p + 3 < end) {
                                cp = static_cast<u32>((p[0] & 0x07) << 18) |
                                     static_cast<u32>((p[1] & 0x3F) << 12) |
                                     static_cast<u32>((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
                                p += 4;
                            } else {
                                ++p;
                                continue;
                            }
                            if (cp >= 32) owner->GetInput().OnText(cp);
                        }
                    } else {
                        const u32 cp = CodepointFromKeysym(ks);
                        if (cp >= 32) owner->GetInput().OnText(cp);
                    }
                }
            } else if (!repeat) {
                owner->GetInput().OnKey(key, KeyAction::Release, false);
            }
            break;
        }
        default: break;
    }
}

}  // namespace

Window::Window() : impl_(new Impl()) {}

Window::~Window() {
    Destroy();
    impl_.reset();
}

bool Window::Create(const WindowDesc& desc) {
    impl_->desc = desc;
    impl_->width = desc.width;
    impl_->height = desc.height;
    impl_->title = desc.title;

    impl_->display = XOpenDisplay(nullptr);
    if (!impl_->display) {
        ENG_LOGE("platform", "XOpenDisplay failed (is DISPLAY set?)");
        return false;
    }
    impl_->ownsDisplay = true;
    impl_->screen = DefaultScreen(impl_->display);

    int samples = 0;
    GLXFBConfig config = ChooseFBConfig(impl_->display, impl_->screen, desc, &samples);
    if (!config) {
        ENG_LOGE("platform", "glXChooseFBConfig failed (no matching GLX visual)");
        Destroy();
        return false;
    }
    impl_->visual = glXGetVisualFromFBConfig(impl_->display, config);
    if (!impl_->visual) {
        ENG_LOGE("platform", "glXGetVisualFromFBConfig failed");
        Destroy();
        return false;
    }

    XSetWindowAttributes attrs{};
    attrs.colormap = impl_->colormap =
        XCreateColormap(impl_->display, RootWindow(impl_->display, impl_->visual->screen),
                        impl_->visual->visual, AllocNone);
    attrs.background_pixmap = None;
    attrs.border_pixel = 0;
    attrs.event_mask = KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
                       PointerMotionMask | StructureNotifyMask | FocusChangeMask |
                       EnterWindowMask | LeaveWindowMask | ExposureMask;
    // Указатель Impl припрятан на окне, чтобы события находили своего владельца.
    attrs.event_mask |= PropertyChangeMask;

    impl_->window = XCreateWindow(
        impl_->display, RootWindow(impl_->display, impl_->visual->screen), 0, 0,
        static_cast<unsigned int>(desc.width), static_cast<unsigned int>(desc.height), 0,
        impl_->visual->depth, InputOutput, impl_->visual->visual,
        CWColormap | CWBackPixmap | CWBorderPixel | CWEventMask, &attrs);
    if (!impl_->window) {
        ENG_LOGE("platform", "XCreateWindow failed");
        Destroy();
        return false;
    }
    impl_->ownerWindow = this;

    if (!desc.title.empty()) {
        XStoreName(impl_->display, impl_->window, desc.title.c_str());
        // Также задаём _NET_WM_NAME, чтобы UTF-8 заголовки отображались в современных WM.
        const Atom netName = XInternAtom(impl_->display, "_NET_WM_NAME", False);
        const Atom utf8 = XInternAtom(impl_->display, "UTF8_STRING", False);
        if (netName != None && utf8 != None) {
            XChangeProperty(impl_->display, impl_->window, netName, utf8, 8, PropModeReplace,
                            reinterpret_cast<const unsigned char*>(desc.title.c_str()),
                            static_cast<int>(desc.title.size()));
        }
    }
    if (desc.minWidth > 0 && desc.minHeight > 0) {
        XSizeHints hints{};
        hints.flags = PMinSize;
        hints.min_width = desc.minWidth;
        hints.min_height = desc.minHeight;
        XSetWMNormalHints(impl_->display, impl_->window, &hints);
    }
    XSelectInput(impl_->display, impl_->window, attrs.event_mask);

    impl_->wmDeleteWindow = XInternAtom(impl_->display, "WM_DELETE_WINDOW", False);
    if (impl_->wmDeleteWindow != None) {
        XSetWMProtocols(impl_->display, impl_->window, &impl_->wmDeleteWindow, 1);
    }

    // Контекст ввода для учитывающего локаль (UTF-8) текстового ввода. Неудача не страшна:
    // запасной путь через keysym всё равно даёт кодовые точки для латиницы.
    impl_->inputMethod = XOpenIM(impl_->display, nullptr, nullptr, nullptr);
    if (impl_->inputMethod) {
        impl_->inputContext = XCreateIC(impl_->inputMethod, XNInputStyle,
                                        XIMPreeditNothing | XIMStatusNothing, XNClientWindow,
                                        impl_->window, XNFocusWindow, impl_->window, nullptr);
        if (!impl_->inputContext) {
            XCloseIM(impl_->inputMethod);
            impl_->inputMethod = nullptr;
        }
    }

    // Резолвим расширение контекста до создания представления менеджера окон.
    PFNGLXCREATECONTEXTATTRIBSARBPROC createContextAttribs =
        reinterpret_cast<PFNGLXCREATECONTEXTATTRIBSARBPROC>(
            glXGetProcAddressARB(reinterpret_cast<const GLubyte*>("glXCreateContextAttribsARB")));

    bool coreProfile = false;
    if (createContextAttribs) {
        const int wantedMajor = desc.glMajor > 0 ? desc.glMajor : 3;
        const int wantedMinor = desc.glMinor > 0 ? desc.glMinor : 3;
        const int versions[][2] = {{wantedMajor, wantedMinor}, {3, 2}};
        for (const auto& version : versions) {
            const int contextAttribs[] = {
                kGlxContextMajorVersionArb, version[0],
                kGlxContextMinorVersionArb, version[1],
                kGlxContextProfileMaskArb, kGlxContextCoreProfileBitArb,
                0,
            };
            impl_->context = createContextAttribs(impl_->display, config, nullptr, True,
                                                  contextAttribs);
            if (impl_->context) {
                coreProfile = true;
                if (version[0] != wantedMajor || version[1] != wantedMinor) {
                    ENG_LOGW("platform", "OpenGL %d.%d requested but %d.%d core was created",
                             wantedMajor, wantedMinor, version[0], version[1]);
                }
                break;
            }
        }
    }
    if (!impl_->context) {
        // Legacy-запас: контекст с поддержкой indirect через путь по умолчанию.
        impl_->context = glXCreateNewContext(impl_->display, config, GLX_RGBA_TYPE, nullptr, True);
        if (impl_->context) {
            ENG_LOGW("platform", "glXCreateContextAttribsARB unavailable; using a legacy context");
        }
    }
    if (!impl_->context) {
        ENG_LOGE("platform", "failed to create a GLX context");
        Destroy();
        return false;
    }

    glXMakeCurrent(impl_->display, impl_->window, impl_->context);

    XMapWindow(impl_->display, impl_->window);
    XFlush(impl_->display);

    bool fromXft = false;
    impl_->dpiScale = QueryDpiScale(impl_->display, &fromXft);
    UpdateMetrics(impl_.get());

    PFNGLXSWAPINTERVALEXTPROC swapInterval = reinterpret_cast<PFNGLXSWAPINTERVALEXTPROC>(
        glXGetProcAddressARB(reinterpret_cast<const GLubyte*>("glXSwapIntervalEXT")));
    if (swapInterval) swapInterval(impl_->display, impl_->window, desc.vsync ? 1 : 0);

    if (desc.mode != WindowMode::Windowed) SetMode(desc.mode);

    const GLubyte* version = nullptr;
    auto getString =
        reinterpret_cast<const GLubyte* (*)(unsigned int)>(LinuxGLGetProcAddress("glGetString"));
    if (getString) version = getString(kGlVersion);

    ENG_LOGI("platform",
             "window created %dx%d (fb %dx%d, dpi %.2f%s, samples %d, %s)",
             impl_->width, impl_->height, impl_->fbWidth, impl_->fbHeight, impl_->dpiScale,
             fromXft ? " from Xft.dpi" : "", samples,
             coreProfile ? "core profile" : "legacy profile");
    if (version) ENG_LOGI("platform", "GL_VERSION: %s", reinterpret_cast<const char*>(version));
    return true;
}

void Window::Destroy() {
    if (!impl_) return;
    if (impl_->display) {
        if (impl_->inputContext) {
            XDestroyIC(impl_->inputContext);
            impl_->inputContext = nullptr;
        }
        if (impl_->inputMethod) {
            XCloseIM(impl_->inputMethod);
            impl_->inputMethod = nullptr;
        }
        if (impl_->context) {
            glXMakeCurrent(impl_->display, None, nullptr);
            glXDestroyContext(impl_->display, impl_->context);
            impl_->context = nullptr;
        }
        if (impl_->window) {
            XDestroyWindow(impl_->display, impl_->window);
            impl_->window = 0;
        }
        if (impl_->colormap) {
            XFreeColormap(impl_->display, impl_->colormap);
            impl_->colormap = 0;
        }
        if (impl_->visual) {
            XFree(impl_->visual);
            impl_->visual = nullptr;
        }
        if (impl_->ownsDisplay) {
            XCloseDisplay(impl_->display);
            impl_->ownsDisplay = false;
        }
        impl_->display = nullptr;
    }
    impl_->ownerWindow = nullptr;
    impl_->shouldClose = false;
    impl_->fullscreen = false;
}

void Window::PollEvents() {
    if (!impl_->display) return;
    while (XPending(impl_->display) > 0) {
        XEvent event;
        XNextEvent(impl_->display, &event);
        if (event.type == SelectionRequest) {
            Clipboard().ServeRequest(impl_->display, &event.xselectionrequest);
            continue;
        }
        HandleEvent(impl_.get(), &event);
    }
    if (impl_->relativeMouse) WarpToCenter(impl_.get());
}

void Window::SwapBuffers() {
    if (impl_->display && impl_->window) glXSwapBuffers(impl_->display, impl_->window);
}

bool Window::ShouldClose() const { return impl_->shouldClose; }

void Window::RequestClose() {
    if (impl_->shouldClose) return;
    impl_->shouldClose = true;
    if (callbacks.onClose) callbacks.onClose();
}

void Window::SetTitle(const std::string& title) {
    impl_->title = title;
    if (!impl_->display || !impl_->window) return;
    XStoreName(impl_->display, impl_->window, title.c_str());
    const Atom netName = XInternAtom(impl_->display, "_NET_WM_NAME", False);
    const Atom utf8 = XInternAtom(impl_->display, "UTF8_STRING", False);
    if (netName != None && utf8 != None) {
        XChangeProperty(impl_->display, impl_->window, netName, utf8, 8, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(title.c_str()),
                        static_cast<int>(title.size()));
    }
}

void Window::SetSize(int w, int h) {
    if (!impl_->display || !impl_->window || w <= 0 || h <= 0) return;
    if (impl_->fullscreen) {
        impl_->width = w;
        impl_->height = h;
        return;
    }
    XResizeWindow(impl_->display, impl_->window, static_cast<unsigned int>(w),
                  static_cast<unsigned int>(h));
    XFlush(impl_->display);
    UpdateMetrics(impl_.get());
}

void Window::SetMode(WindowMode mode) {
    impl_->mode = mode;
    if (!impl_->display || !impl_->window) return;

    if (mode == WindowMode::Fullscreen) {
        if (!SetFullscreen(impl_.get(), true, false)) {
            // Нет поддержки EWMH (голый X-сервер): откат к окну размером с монитор
            // override-redirect, чтобы приложение оставалось работоспособным.
            ENG_LOGW("platform", "fullscreen: _NET_WM_STATE unavailable; using a sized window");
            const int screen = impl_->screen;
            SetSize(DisplayWidth(impl_->display, screen), DisplayHeight(impl_->display, screen));
        }
        return;
    }

    if (impl_->fullscreen) {
        SetFullscreen(impl_.get(), false, mode == WindowMode::Borderless);
        if (mode == WindowMode::Windowed && impl_->savedW > 0 && impl_->savedH > 0) {
            XMoveResizeWindow(impl_->display, impl_->window, impl_->savedX, impl_->savedY,
                              static_cast<unsigned int>(impl_->savedW),
                              static_cast<unsigned int>(impl_->savedH));
        }
    } else if (mode == WindowMode::Borderless) {
        SetFullscreen(impl_.get(), false, true);
    }
    XFlush(impl_->display);
    UpdateMetrics(impl_.get());
}

void Window::SetVSync(bool enabled) {
    if (!impl_->display || !impl_->window) return;
    auto swapInterval = reinterpret_cast<PFNGLXSWAPINTERVALEXTPROC>(
        glXGetProcAddressARB(reinterpret_cast<const GLubyte*>("glXSwapIntervalEXT")));
    if (swapInterval) {
        swapInterval(impl_->display, impl_->window, enabled ? 1 : 0);
        return;
    }
    // GLX_MESA_swap_control / SGI не принимают аргумент drawable.
    using PFNGLXSWAPINTERVALMESAPROC = int (*)(unsigned int);
    auto mesa = reinterpret_cast<PFNGLXSWAPINTERVALMESAPROC>(
        glXGetProcAddressARB(reinterpret_cast<const GLubyte*>("glXSwapIntervalMESA")));
    if (mesa) mesa(enabled ? 1u : 0u);
}

void Window::Minimize() {
    if (!impl_->display || !impl_->window) return;
    XIconifyWindow(impl_->display, impl_->window, impl_->screen);
    XFlush(impl_->display);
}

void Window::Maximize() {
    if (!impl_->display || !impl_->window) return;
    const Atom wmState = XInternAtom(impl_->display, "_NET_WM_STATE", False);
    const Atom maxH = XInternAtom(impl_->display, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
    const Atom maxV = XInternAtom(impl_->display, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    if (wmState == None) return;
    XEvent event{};
    event.xclient.type = ClientMessage;
    event.xclient.window = impl_->window;
    event.xclient.message_type = wmState;
    event.xclient.format = 32;
    event.xclient.data.l[0] = 1;  // add
    event.xclient.data.l[1] = static_cast<long>(maxH);
    event.xclient.data.l[2] = static_cast<long>(maxV);
    XSendEvent(impl_->display, DefaultRootWindow(impl_->display), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &event);
    XFlush(impl_->display);
}

void Window::Restore() {
    if (!impl_->display || !impl_->window) return;
    const Atom wmState = XInternAtom(impl_->display, "_NET_WM_STATE", False);
    const Atom hidden = XInternAtom(impl_->display, "_NET_WM_STATE_HIDDEN", False);
    if (wmState != None) {
        XEvent event{};
        event.xclient.type = ClientMessage;
        event.xclient.window = impl_->window;
        event.xclient.message_type = wmState;
        event.xclient.format = 32;
        event.xclient.data.l[0] = 0;  // remove
        event.xclient.data.l[1] = static_cast<long>(hidden);
        XSendEvent(impl_->display, DefaultRootWindow(impl_->display), False,
                   SubstructureRedirectMask | SubstructureNotifyMask, &event);
    }
    XMapWindow(impl_->display, impl_->window);
    XFlush(impl_->display);
}

void Window::Show() {
    if (!impl_->display || !impl_->window) return;
    XMapWindow(impl_->display, impl_->window);
    XFlush(impl_->display);
}

void Window::Hide() {
    if (!impl_->display || !impl_->window) return;
    XUnmapWindow(impl_->display, impl_->window);
    XFlush(impl_->display);
}

void Window::Focus() {
    if (!impl_->display || !impl_->window) return;
    XSetInputFocus(impl_->display, impl_->window, RevertToParent, CurrentTime);
    XRaiseWindow(impl_->display, impl_->window);
    XFlush(impl_->display);
}

void Window::MakeCurrent() {
    if (impl_->display && impl_->window && impl_->context) {
        glXMakeCurrent(impl_->display, impl_->window, impl_->context);
    }
}

void Window::WaitEventsTimeout(f32 seconds) {
    if (!impl_->display) return;
    if (seconds > 0.0f && XPending(impl_->display) == 0) {
        const int fd = ConnectionNumber(impl_->display);
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        timeval timeout{};
        timeout.tv_sec = static_cast<long>(seconds);
        timeout.tv_usec = static_cast<long>((seconds - static_cast<f32>(timeout.tv_sec)) * 1e6f);
        select(fd + 1, &fds, nullptr, nullptr, &timeout);
    }
    PollEvents();
}

void Window::SetCursorVisible(bool visible) {
    if (!impl_->display || !impl_->window) return;
    if (visible) {
        XDefineCursor(impl_->display, impl_->window, None);
    } else {
        static Cursor blank = None;
        if (blank == None) {
            const int screen = impl_->screen;
            char data = 0;
            Pixmap pixmap = XCreateBitmapFromData(impl_->display,
                                                  RootWindow(impl_->display, screen), &data, 1, 1);
            XColor black{};
            blank = XCreatePixmapCursor(impl_->display, pixmap, pixmap, &black, &black, 0, 0);
            XFreePixmap(impl_->display, pixmap);
        }
        XDefineCursor(impl_->display, impl_->window, blank);
    }
    XFlush(impl_->display);
}

void Window::SetCursorMode(int mode) {
    impl_->cursorMode = mode;
    if (!impl_->display || !impl_->window) return;

    switch (mode) {
        case 2: {  // отключён / относительный
            impl_->relativeMouse = true;
            // Захватываем указатель, пока кнопка нажата, и прячем курсор.
            XGrabPointer(impl_->display, impl_->window, True,
                         PointerMotionMask | ButtonPressMask | ButtonReleaseMask,
                         GrabModeAsync, GrabModeAsync, impl_->window, None, CurrentTime);
            SetCursorVisible(false);
            WarpToCenter(impl_.get());
            break;
        }
        case 1:  // скрыт
            impl_->relativeMouse = false;
            XUngrabPointer(impl_->display, CurrentTime);
            SetCursorVisible(false);
            break;
        default:  // обычный
            impl_->relativeMouse = false;
            XUngrabPointer(impl_->display, CurrentTime);
            SetCursorVisible(true);
            break;
    }
    XFlush(impl_->display);
}

void Window::SetClipboardText(const std::string& text) {
    if (!impl_->display || !impl_->window) return;
    ClipboardState& clip = Clipboard();
    if (!clip.EnsureAtoms(impl_->display)) return;
    clip.EnsureOwnerWindow(impl_->display);
    clip.outgoing = text;
    clip.owns = true;
    XSetSelectionOwner(impl_->display, clip.clipboard, clip.owner, CurrentTime);
    XFlush(impl_->display);
}

std::string Window::GetClipboardText() const {
    if (!impl_->display) return {};
    ClipboardState& clip = Clipboard();
    if (!clip.EnsureAtoms(impl_->display)) return {};
    const ::Window requestor = clip.EnsureOwnerWindow(impl_->display);
    if (!requestor) return {};

    const ::Window selectionOwner = XGetSelectionOwner(impl_->display, clip.clipboard);
    if (selectionOwner == None) return {};
    if (selectionOwner == requestor) return clip.outgoing;  // владелец — мы

    XConvertSelection(impl_->display, clip.clipboard, clip.utf8, clip.property, requestor,
                      CurrentTime);
    XFlush(impl_->display);

    Atom type = None;
    unsigned char* data = nullptr;
    unsigned long items = 0;
    if (!clip.WaitFor(impl_->display, clip.clipboard, &type, &data, &items, 100)) {
        // Ровно одно предупреждение: буфер обмена — общий ресурс, и круговой обмен
        // по праву завершается неудачей, когда другой клиент медленный или отсутствует.
        static bool warned = false;
        if (!warned) {
            warned = true;
            ENG_LOGW("platform", "clipboard read timed out; returning an empty string");
        }
        return {};
    }
    std::string result;
    if (data) {
        result.assign(reinterpret_cast<const char*>(data), static_cast<size_t>(items));
        XFree(data);
    }
    return result;
}

int Window::Width() const { return impl_->width; }
int Window::Height() const { return impl_->height; }
int Window::FramebufferWidth() const { return impl_->fbWidth; }
int Window::FramebufferHeight() const { return impl_->fbHeight; }
f32 Window::DpiScale() const { return impl_->dpiScale; }
f32 Window::Aspect() const {
    const int h = FramebufferHeight();
    return h > 0 ? static_cast<f32>(FramebufferWidth()) / static_cast<f32>(h) : 1.0f;
}
bool Window::IsFocused() const { return impl_->focused; }
bool Window::IsMinimized() const {
    if (!impl_->display || !impl_->window) return false;
    XWindowAttributes attrs{};
    if (!XGetWindowAttributes(impl_->display, impl_->window, &attrs)) return false;
    return attrs.map_state == IsUnmapped;
}
bool Window::IsFullscreen() const { return impl_->fullscreen; }

Vec2 Window::MousePosition() const {
    if (!impl_->display || !impl_->window) return {};
    ::Window root = 0;
    ::Window child = 0;
    int rootX = 0, rootY = 0, winX = 0, winY = 0;
    unsigned int mask = 0;
    if (!XQueryPointer(impl_->display, impl_->window, &root, &child, &rootX, &rootY, &winX, &winY,
                       &mask)) {
        return {};
    }
    return {static_cast<f32>(winX), static_cast<f32>(winY)};
}

const std::string& Window::Title() const { return impl_->title; }
void* Window::NativeHandle() const { return reinterpret_cast<void*>(impl_->window); }
void* Window::NativeDisplay() const { return impl_->display; }
void* (*Window::GLGetProcAddress() const)(const char*) { return LinuxGLGetProcAddress; }

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
