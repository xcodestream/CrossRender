// Платформенный слой Windows: нативное окно Win32 + контекст WGL (без GLFW/SDL).
//
// GL-контекст создаётся в два шага — единственный способ получить современный
// core profile через WGL:
//   1. скрытое «bootstrap»-окно с legacy пиксельным форматом даёт контекст,
//      достаточный для разрешения точек входа расширений ARB;
//   2. wglChoosePixelFormatARB / wglCreateContextAttribsARB строят настоящий
//      контекст OpenGL 3.3 core на настоящем окне (3.2, затем legacy-откат).
#include "crossrender/platform/Window.h"

#include "crossrender/gfx/GL.h"
#include "crossrender/core/Log.h"
#include "crossrender/core/Time.h"
#include "crossrender/platform/Platform.h"

#if defined(ENG_PLATFORM_WINDOWS)

#include <windows.h>

#include "Win32Common.h"

#include <string>
#include <vector>
#include <cstring>

// Поверхность WGL-расширений, конверсия UTF-8/UTF-16, запросы per-monitor DPI
// и доступ к буферу обмена живут в Win32Common.h, чтобы PlatformWindows.cpp мог ими пользоваться.
namespace crossrender {
namespace {


// ---------------------------------------------------------------------------
// Помощники scan code (физическая идентичность клавиши + левые/правые варианты модификаторов).
// ---------------------------------------------------------------------------
int ScanCodeOf(LPARAM lparam) { return static_cast<int>((lparam >> 16) & 0xFF); }

bool IsExtendedKey(LPARAM lparam) {
    return (static_cast<unsigned long long>(lparam) & (1ULL << 24)) != 0;
}

// Переводит виртуальную клавишу в физический set-1 scan code. Используется для
// обобщённых VK_SHIFT / VK_CONTROL / VK_MENU и как запас, когда поле scan code
// сообщения равно нулю (синтезированный ввод).
int VkToScancode(int vk) {
    const UINT sc = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
    return static_cast<int>(sc & 0xFF);
}

bool KeyDownAnywhere(int vk) { return (GetKeyState(vk) & 0x8000) != 0; }

// Отображение физической раскладки: scan code — единственная надёжная идентичность
// клавиши на не-US раскладках (Windows сообщает VK_OEM_* для ряда пунктуации).
Key KeyFromScancode(int sc, bool extended) {
    switch (sc) {
        case 0x01: return Key::Escape;
        case 0x02: return Key::Num1;
        case 0x03: return Key::Num2;
        case 0x04: return Key::Num3;
        case 0x05: return Key::Num4;
        case 0x06: return Key::Num5;
        case 0x07: return Key::Num6;
        case 0x08: return Key::Num7;
        case 0x09: return Key::Num8;
        case 0x0A: return Key::Num9;
        case 0x0B: return Key::Num0;
        case 0x0C: return Key::Minus;
        case 0x0D: return Key::Equal;
        case 0x0E: return Key::Backspace;
        case 0x0F: return Key::Tab;
        case 0x10: return Key::Q;
        case 0x11: return Key::W;
        case 0x12: return Key::E;
        case 0x13: return Key::R;
        case 0x14: return Key::T;
        case 0x15: return Key::Y;
        case 0x16: return Key::U;
        case 0x17: return Key::I;
        case 0x18: return Key::O;
        case 0x19: return Key::P;
        case 0x1A: return Key::LeftBracket;
        case 0x1B: return Key::RightBracket;
        case 0x1C: return extended ? Key::KeypadEnter : Key::Enter;
        case 0x1D: return extended ? Key::RightControl : Key::LeftControl;
        case 0x1E: return Key::A;
        case 0x1F: return Key::S;
        case 0x20: return Key::D;
        case 0x21: return Key::F;
        case 0x22: return Key::G;
        case 0x23: return Key::H;
        case 0x24: return Key::J;
        case 0x25: return Key::K;
        case 0x26: return Key::L;
        case 0x27: return Key::Semicolon;
        case 0x28: return Key::Apostrophe;
        case 0x29: return Key::Grave;
        case 0x2A: return Key::LeftShift;
        case 0x2B: return Key::Backslash;
        case 0x2C: return Key::Z;
        case 0x2D: return Key::X;
        case 0x2E: return Key::C;
        case 0x2F: return Key::V;
        case 0x30: return Key::B;
        case 0x31: return Key::N;
        case 0x32: return Key::M;
        case 0x33: return Key::Comma;
        case 0x34: return Key::Period;
        case 0x35: return extended ? Key::KeypadDivide : Key::Slash;
        case 0x36: return Key::RightShift;
        case 0x37: return extended ? Key::PrintScreen : Key::KeypadMultiply;
        case 0x38: return extended ? Key::RightAlt : Key::LeftAlt;
        case 0x39: return Key::Space;
        case 0x3A: return Key::CapsLock;
        case 0x3B: return Key::F1;
        case 0x3C: return Key::F2;
        case 0x3D: return Key::F3;
        case 0x3E: return Key::F4;
        case 0x3F: return Key::F5;
        case 0x40: return Key::F6;
        case 0x41: return Key::F7;
        case 0x42: return Key::F8;
        case 0x43: return Key::F9;
        case 0x44: return Key::F10;
        case 0x45: return Key::NumLock;
        case 0x46: return Key::ScrollLock;
        case 0x47: return extended ? Key::Home : Key::Keypad7;
        case 0x48: return extended ? Key::Up : Key::Keypad8;
        case 0x49: return extended ? Key::PageUp : Key::Keypad9;
        case 0x4A: return Key::KeypadSubtract;
        case 0x4B: return extended ? Key::Left : Key::Keypad4;
        case 0x4C: return Key::Keypad5;
        case 0x4D: return extended ? Key::Right : Key::Keypad6;
        case 0x4E: return Key::KeypadAdd;
        case 0x4F: return extended ? Key::End : Key::Keypad1;
        case 0x50: return extended ? Key::Down : Key::Keypad2;
        case 0x51: return extended ? Key::PageDown : Key::Keypad3;
        case 0x52: return extended ? Key::Insert : Key::Keypad0;
        case 0x53: return extended ? Key::Delete : Key::KeypadDecimal;
        case 0x56: return Key::Unknown;  // дополнительная ISO-клавиша на некоторых раскладках
        case 0x57: return Key::F11;
        case 0x58: return Key::F12;
        case 0x59: return Key::KeypadEqual;
        case 0x64: return Key::F13;
        case 0x65: return Key::F14;
        case 0x66: return Key::F15;
        case 0x67: return Key::F16;
        case 0x68: return Key::F17;
        case 0x69: return Key::F18;
        case 0x6A: return Key::F19;
        case 0x6B: return Key::F20;
        case 0x6C: return Key::F21;
        case 0x6D: return Key::F22;
        case 0x6E: return Key::F23;
        case 0x6F: return Key::F24;
        case 0x5B: return Key::LeftSuper;
        case 0x5C: return Key::RightSuper;
        case 0x5D: return Key::Menu;
        default: return Key::Unknown;
    }
}

// Обобщённый перевод VK -> Key, используется, когда scan code ни на что не отображается.
Key KeyFromVirtualKey(int vk) {
    if (vk >= 'A' && vk <= 'Z') return static_cast<Key>(static_cast<int>(Key::A) + (vk - 'A'));
    if (vk >= '0' && vk <= '9') return static_cast<Key>(static_cast<int>(Key::Num0) + (vk - '0'));
    if (vk >= VK_F1 && vk <= VK_F24)
        return static_cast<Key>(static_cast<int>(Key::F1) + (vk - VK_F1));
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        return static_cast<Key>(static_cast<int>(Key::Keypad0) + (vk - VK_NUMPAD0));
    switch (vk) {
        case VK_SPACE: return Key::Space;
        case VK_OEM_7: return Key::Apostrophe;
        case VK_OEM_COMMA: return Key::Comma;
        case VK_OEM_MINUS: return Key::Minus;
        case VK_OEM_PERIOD: return Key::Period;
        case VK_OEM_2: return Key::Slash;
        case VK_OEM_1: return Key::Semicolon;
        case VK_OEM_PLUS: return Key::Equal;
        case VK_OEM_4: return Key::LeftBracket;
        case VK_OEM_5: return Key::Backslash;
        case VK_OEM_6: return Key::RightBracket;
        case VK_OEM_3: return Key::Grave;
        case VK_ESCAPE: return Key::Escape;
        case VK_RETURN: return Key::Enter;
        case VK_TAB: return Key::Tab;
        case VK_BACK: return Key::Backspace;
        case VK_INSERT: return Key::Insert;
        case VK_DELETE: return Key::Delete;
        case VK_RIGHT: return Key::Right;
        case VK_LEFT: return Key::Left;
        case VK_DOWN: return Key::Down;
        case VK_UP: return Key::Up;
        case VK_PRIOR: return Key::PageUp;
        case VK_NEXT: return Key::PageDown;
        case VK_HOME: return Key::Home;
        case VK_END: return Key::End;
        case VK_CAPITAL: return Key::CapsLock;
        case VK_SCROLL: return Key::ScrollLock;
        case VK_NUMLOCK: return Key::NumLock;
        case VK_SNAPSHOT: return Key::PrintScreen;
        case VK_PAUSE: return Key::Pause;
        case VK_DECIMAL: return Key::KeypadDecimal;
        case VK_DIVIDE: return Key::KeypadDivide;
        case VK_MULTIPLY: return Key::KeypadMultiply;
        case VK_SUBTRACT: return Key::KeypadSubtract;
        case VK_ADD: return Key::KeypadAdd;
        case VK_LWIN: return Key::LeftSuper;
        case VK_RWIN: return Key::RightSuper;
        case VK_APPS: return Key::Menu;
        default: return Key::Unknown;
    }
}

// VK_SHIFT / VK_CONTROL / VK_MENU в Win32 «обобщённые»: сообщение не несёт
// информации левое/правое, поэтому физический scan code (запас через MapVirtualKey),
// флаг extended и предыдущее состояние парной клавиши объединяются
// для выбора правильного варианта.
Key ModifierKey(int vk, int sc, bool extended) {
    switch (vk) {
        case VK_SHIFT: {
            if (sc == 0x36) return Key::RightShift;
            if (sc == 0x2A) return Key::LeftShift;
            if (extended) return Key::RightShift;
            const bool leftDown = KeyDownAnywhere(VK_LSHIFT);
            const bool rightDown = KeyDownAnywhere(VK_RSHIFT);
            if (leftDown && !rightDown) return Key::RightShift;
            return Key::LeftShift;
        }
        case VK_CONTROL: {
            if (sc == 0x1D) return extended ? Key::RightControl : Key::LeftControl;
            if (extended) return Key::RightControl;
            const bool leftDown = KeyDownAnywhere(VK_LCONTROL);
            const bool rightDown = KeyDownAnywhere(VK_RCONTROL);
            if (rightDown && !leftDown) return Key::LeftControl;
            return Key::LeftControl;
        }
        case VK_MENU: {
            if (sc == 0x38) return extended ? Key::RightAlt : Key::LeftAlt;
            if (extended) return Key::RightAlt;
            const bool leftDown = KeyDownAnywhere(VK_LMENU);
            const bool rightDown = KeyDownAnywhere(VK_RMENU);
            if (rightDown && !leftDown) return Key::LeftAlt;
            return Key::LeftAlt;
        }
        default: return Key::Unknown;
    }
}

Key KeyFromMessage(int vk, LPARAM lparam) {
    const int sc = ScanCodeOf(lparam);
    const bool extended = IsExtendedKey(lparam);
    if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) {
        const Key modifier = ModifierKey(vk, sc, extended);
        if (modifier != Key::Unknown) return modifier;
    }
    int useSc = sc;
    if (useSc == 0) useSc = VkToScancode(vk);
    Key k = KeyFromScancode(useSc, extended);
    if (k != Key::Unknown) return k;
    k = KeyFromVirtualKey(vk);
    if (k != Key::Unknown) return k;
    if (vk == VK_SHIFT) return Key::LeftShift;
    if (vk == VK_CONTROL) return Key::LeftControl;
    if (vk == VK_MENU) return Key::LeftAlt;
    return Key::Unknown;
}

MouseButton MouseButtonFromMessage(UINT msg, WPARAM wparam) {
    switch (msg) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK: return MouseButton::Left;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_RBUTTONDBLCLK: return MouseButton::Right;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MBUTTONDBLCLK: return MouseButton::Middle;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
        case WM_XBUTTONDBLCLK:
            return (HIWORD(wparam) == XBUTTON2) ? MouseButton::X2 : MouseButton::X1;
        default: return MouseButton::Left;
    }
}

// ---------------------------------------------------------------------------
// Пиксельный формат / создание контекста (путь ARB с legacy-откатом).
// ---------------------------------------------------------------------------
bool ChooseArbPixelFormat(HDC dc, const WindowDesc& desc, int* outFormat, UINT* outSamples) {
    win32::WglApi& api = win32::Wgl();
    if (!api.available) return false;

    int samples = desc.msaaSamples > 1 ? desc.msaaSamples : 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
        int attrs[40];
        int i = 0;
        attrs[i++] = win32::kWglDrawToWindowArb;
        attrs[i++] = win32::kWglAccelerationArb;
        attrs[i++] = win32::kWglDoubleBufferArb;
        attrs[i++] = win32::kWglPixelTypeArb;
        attrs[i++] = win32::kWglTypeRgbaArb;
        attrs[i++] = win32::kWglColorBitsArb;
        attrs[i++] = 32;
        attrs[i++] = win32::kWglAlphaBitsArb;
        attrs[i++] = 8;
        attrs[i++] = win32::kWglDepthBitsArb;
        attrs[i++] = desc.depthBuffer ? 24 : 0;
        attrs[i++] = win32::kWglStencilBitsArb;
        attrs[i++] = desc.stencilBuffer ? 8 : 0;
        if (samples > 1) {
            attrs[i++] = win32::kWglSampleBuffersArb;
            attrs[i++] = 1;
            attrs[i++] = win32::kWglSamplesArb;
            attrs[i++] = samples;
        }
        attrs[i++] = 0;

        int format = 0;
        UINT numFormats = 0;
        if (api.choosePixelFormat(dc, attrs, nullptr, 1, &format, &numFormats) && numFormats > 0 &&
            format != 0) {
            *outFormat = format;
            *outSamples = static_cast<UINT>(samples > 1 ? samples : 0);
            return true;
        }
        if (samples <= 1) break;
        ENG_LOGW("platform", "MSAA %d unavailable, retrying without", samples);
        samples = 0;
    }
    return false;
}

// Legacy-путь PIXELFORMATDESCRIPTOR, используется при отсутствии точек входа ARB.
bool ChooseLegacyPixelFormat(HDC dc, const WindowDesc& desc, int* outFormat) {
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;
    pfd.cDepthBits = desc.depthBuffer ? 24 : 0;
    pfd.cStencilBits = desc.stencilBuffer ? 8 : 0;
    pfd.iLayerType = PFD_MAIN_PLANE;

    const int fmt = ChoosePixelFormat(dc, &pfd);
    if (fmt == 0) return false;
    PIXELFORMATDESCRIPTOR got{};
    if (!DescribePixelFormat(dc, fmt, sizeof(got), &got)) return false;
    if (!SetPixelFormat(dc, fmt, &got)) return false;
    *outFormat = fmt;
    return true;
}

// Создаёт настоящий контекст с GL 3.3 core profile, деградируя до 3.2 core
// и в конце до того, что даст legacy-путь.
HGLRC CreateRealContext(HDC dc, const WindowDesc& desc, bool haveArb, bool* outCore) {
    *outCore = false;
    if (haveArb) {
        win32::WglApi& api = win32::Wgl();
        const int wantedMajor = desc.glMajor > 0 ? desc.glMajor : 3;
        const int wantedMinor = desc.glMinor > 0 ? desc.glMinor : 3;
        const int versions[][2] = {{wantedMajor, wantedMinor}, {3, 2}};
        for (const auto& version : versions) {
            const int attribs[] = {
                win32::kWglContextMajorVersionArb, version[0],
                win32::kWglContextMinorVersionArb, version[1],
                win32::kWglContextProfileMaskArb, win32::kWglContextCoreProfileBitArb,
                0,
            };
            HGLRC rc = api.createContextAttribs(dc, nullptr, attribs);
            if (rc) {
                *outCore = true;
                if (version[0] != wantedMajor || version[1] != wantedMinor) {
                    ENG_LOGW("platform", "OpenGL %d.%d requested but %d.%d core was created",
                             wantedMajor, wantedMinor, version[0], version[1]);
                }
                return rc;
            }
        }
        // Некоторые драйверы начисто отвергают маску core profile; пробуем ещё раз
        // без неё, прежде чем откатиться к legacy-пути.
        const int attribs[] = {win32::kWglContextMajorVersionArb, 3,
                               win32::kWglContextMinorVersionArb, 2, 0};
        HGLRC rc = api.createContextAttribs(dc, nullptr, attribs);
        if (rc) {
            *outCore = true;
            return rc;
        }
        ENG_LOGW("platform", "wglCreateContextAttribsARB failed; using a legacy context");
    }
    HGLRC rc = wglCreateContext(dc);
    if (rc) *outCore = false;
    return rc;
}

int GetMouseX(LPARAM lparam) { return static_cast<int>(static_cast<short>(LOWORD(lparam))); }
int GetMouseY(LPARAM lparam) { return static_cast<int>(static_cast<short>(HIWORD(lparam))); }

}  // namespace
// (внешнее пространство имён crossrender, открытое выше блока помощников, продолжается ниже)

// ---------------------------------------------------------------------------
// Window::Impl
// ---------------------------------------------------------------------------
struct Window::Impl {
    HWND window = nullptr;
    HDC dc = nullptr;
    HGLRC glrc = nullptr;
    // Обратный указатель, которым (статическая) оконная процедура добирается до владельца.
    void* ownerWindow = nullptr;
    bool coreProfile = false;
    bool shouldClose = false;
    bool focused = true;
    bool fullscreen = false;
    bool trackingLeave = false;
    bool relativeMouse = false;
    WindowMode mode = WindowMode::Windowed;
    WindowDesc desc;
    int width = 0, height = 0;
    int fbWidth = 0, fbHeight = 0;
    f32 dpiScale = 1.0f;
    UINT dpi = 96;
    std::string title;
    int cursorMode = 0;

    // Сохранённое состояние окна, чтобы выход из полноэкранного режима восстановил точную геометрию.
    LONG_PTR savedStyle = 0;
    LONG_PTR savedExStyle = 0;
    WINDOWPLACEMENT savedPlacement{};
};

namespace {

void UpdateMetrics(Window::Impl* impl, HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    const UINT dpi = win32::WindowDpi(hwnd);
    const f32 scale = static_cast<f32>(dpi) / 96.0f;
    impl->width = w;
    impl->height = h;
    impl->dpi = dpi;
    impl->dpiScale = scale;
    impl->fbWidth = static_cast<int>(static_cast<f32>(w) * scale + 0.5f);
    impl->fbHeight = static_cast<int>(static_cast<f32>(h) * scale + 0.5f);
}

// В относительном режиме удерживает указатель в центре клиентской области.
void ApplyRelativeMouse(Window::Impl* impl) {
    if (!impl->window || !impl->relativeMouse) return;
    RECT cr{};
    GetClientRect(impl->window, &cr);
    POINT center{(cr.right - cr.left) / 2, (cr.bottom - cr.top) / 2};
    ClientToScreen(impl->window, &center);
    SetCursorPos(center.x, center.y);
}

LRESULT CALLBACK EngWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    auto* impl = reinterpret_cast<Window::Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return DefWindowProcW(hwnd, msg, wparam, lparam);
        }
        case WM_CLOSE: {
            if (impl) {
                auto* owner = reinterpret_cast<Window*>(impl->ownerWindow);
                if (owner) {
                    // RequestClose() идемпотентен и вызывает колбэк onClose.
                    owner->RequestClose();
                } else {
                    impl->shouldClose = true;
                }
            }
            return 0;
        }
        case WM_DESTROY:
            return 0;
        case WM_SIZE: {
            if (!impl) break;
            UpdateMetrics(impl, hwnd);
            if (impl->ownerWindow) {
                auto* owner = reinterpret_cast<Window*>(impl->ownerWindow);
                if (owner->callbacks.onResize)
                    owner->callbacks.onResize(impl->fbWidth, impl->fbHeight);
            }
            return 0;
        }
        case WM_DPICHANGED: {
            if (!impl) break;
            const UINT newDpi = HIWORD(wparam);
            impl->dpi = newDpi != 0 ? newDpi : impl->dpi;
            impl->dpiScale = static_cast<f32>(impl->dpi) / 96.0f;
            const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
            if (suggested) {
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left, suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            UpdateMetrics(impl, hwnd);
            if (impl->ownerWindow) {
                auto* owner = reinterpret_cast<Window*>(impl->ownerWindow);
                if (owner->callbacks.onDpiChanged) owner->callbacks.onDpiChanged(impl->dpiScale);
                if (owner->callbacks.onResize)
                    owner->callbacks.onResize(impl->fbWidth, impl->fbHeight);
            }
            return 0;
        }
        case WM_SETFOCUS: {
            if (impl) {
                impl->focused = true;
                if (impl->ownerWindow) {
                    auto* owner = reinterpret_cast<Window*>(impl->ownerWindow);
                    if (owner->callbacks.onFocus) owner->callbacks.onFocus(true);
                }
            }
            return 0;
        }
        case WM_KILLFOCUS: {
            if (impl) {
                impl->focused = false;
                if (impl->ownerWindow) {
                    auto* owner = reinterpret_cast<Window*>(impl->ownerWindow);
                    if (owner->callbacks.onFocus) owner->callbacks.onFocus(false);
                }
            }
            return 0;
        }
        case WM_MOUSELEAVE: {
            if (impl) impl->trackingLeave = false;
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!impl || !impl->ownerWindow) break;
            auto* owner = reinterpret_cast<Window*>(impl->ownerWindow);
            if (!impl->trackingLeave) {
                TRACKMOUSEEVENT tme{};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                if (TrackMouseEvent(&tme)) impl->trackingLeave = true;
            }
            owner->GetInput().OnMouseMove(
                {static_cast<f32>(GetMouseX(lparam)), static_cast<f32>(GetMouseY(lparam))});
            if (impl->relativeMouse) ApplyRelativeMouse(impl);
            return 0;
        }
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDBLCLK: {
            if (!impl || !impl->ownerWindow) break;
            auto* owner = reinterpret_cast<Window*>(impl->ownerWindow);
            const bool down = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN ||
                              msg == WM_MBUTTONDOWN || msg == WM_XBUTTONDOWN ||
                              msg == WM_LBUTTONDBLCLK || msg == WM_RBUTTONDBLCLK ||
                              msg == WM_MBUTTONDBLCLK || msg == WM_XBUTTONDBLCLK;
            owner->GetInput().OnMouseButton(
                MouseButtonFromMessage(msg, wparam), down,
                {static_cast<f32>(GetMouseX(lparam)), static_cast<f32>(GetMouseY(lparam))});
            // X-кнопки должны вернуть TRUE, чтобы система продолжала их слать.
            if (msg == WM_XBUTTONDOWN || msg == WM_XBUTTONUP) return TRUE;
            return 0;
        }
        case WM_MOUSEWHEEL: {
            if (!impl || !impl->ownerWindow) break;
            auto* owner = reinterpret_cast<Window*>(impl->ownerWindow);
            const f32 ticks = static_cast<f32>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
            owner->GetInput().OnScroll({0.0f, ticks});
            return 0;
        }
        case WM_MOUSEHWHEEL: {
            if (!impl || !impl->ownerWindow) break;
            auto* owner = reinterpret_cast<Window*>(impl->ownerWindow);
            const f32 ticks = static_cast<f32>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
            // Дельты горизонтального колеса инвертированы относительно соглашения
            // движка «положительное = вправо».
            owner->GetInput().OnScroll({-ticks, 0.0f});
            return 0;
        }
        case WM_SYSKEYDOWN:
        case WM_KEYDOWN: {
            if (!impl || !impl->ownerWindow) break;
            auto* owner = reinterpret_cast<Window*>(impl->ownerWindow);
            const bool wasDown = (static_cast<unsigned long long>(lparam) & (1ULL << 30)) != 0;
            const Key key = KeyFromMessage(static_cast<int>(wparam), lparam);
            owner->GetInput().OnKey(key, wasDown ? KeyAction::Repeat : KeyAction::Press, wasDown);
            // Позволяем системе увидеть Alt+F10 (строка меню), остальное поглощаем,
            // чтобы окно не пищало на каждое нажатие Alt.
            if (msg == WM_SYSKEYDOWN && static_cast<int>(wparam) != VK_F10) return 0;
            break;
        }
        case WM_SYSKEYUP:
        case WM_KEYUP: {
            if (!impl || !impl->ownerWindow) break;
            auto* owner = reinterpret_cast<Window*>(impl->ownerWindow);
            owner->GetInput().OnKey(KeyFromMessage(static_cast<int>(wparam), lparam),
                                    KeyAction::Release, false);
            return 0;
        }
        case WM_CHAR:
        case WM_SYSCHAR: {
            if (!impl || !impl->ownerWindow) break;
            auto* owner = reinterpret_cast<Window*>(impl->ownerWindow);
            const u32 unit = static_cast<u32>(wparam);
            // WM_CHAR доставляет кодовые единицы UTF-16, поэтому суррогатные пары
            // склеиваются здесь, до того как кодовая точка дойдёт до Input::OnText.
            static u32 pendingHigh = 0;
            if (unit >= 0xD800 && unit <= 0xDBFF) {
                pendingHigh = unit;
                return 0;
            }
            if (unit >= 0xDC00 && unit <= 0xDFFF) {
                if (pendingHigh != 0) {
                    const u32 cp = 0x10000 + ((pendingHigh - 0xD800) << 10) + (unit - 0xDC00);
                    pendingHigh = 0;
                    owner->GetInput().OnText(cp);
                }
                return 0;
            }
            pendingHigh = 0;
            if (unit >= 32) owner->GetInput().OnText(unit);
            return 0;
        }
        case WM_ERASEBKGND:
            // GL-поверхность владеет каждым пикселем; пропуск стирания убирает мерцание.
            return 1;
        case WM_SETCURSOR: {
            if (impl && LOWORD(lparam) == HTCLIENT) {
                if (impl->cursorMode != 0) {
                    SetCursor(nullptr);
                } else {
                    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                }
                return TRUE;
            }
            break;
        }
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

bool EnsureWindowClass() {
    static bool registered = false;
    if (registered) return true;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    // CS_OWNDC даёт окну приватный DC, которого требует WGL.
    wc.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = EngWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"EngWindowClass";
    if (RegisterClassExW(&wc)) {
        registered = true;
    } else if (GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
        registered = true;
    }
    return registered;
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

    // Резолвим точки входа ARB до появления настоящего окна; bootstrap-окно
    // скрыто и сразу уничтожается.
    win32::Wgl();

    if (!win32::EnablePerMonitorDpi()) {
        ENG_LOGW("platform", "per-monitor DPI awareness unavailable; using system DPI");
    }
    if (!EnsureWindowClass()) {
        ENG_LOGE("platform", "RegisterClassEx failed (%lu)", GetLastError());
        return false;
    }

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    if (desc.resizable) style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
    DWORD exStyle = WS_EX_APPWINDOW;
    if (desc.transparent) exStyle |= WS_EX_LAYERED;

    const std::wstring wtitle = win32::Utf8ToWide(desc.title);
    RECT rc{0, 0, desc.width, desc.height};
    if (win32::Dpi().adjustForDpi) {
        win32::Dpi().adjustForDpi(&rc, style, FALSE, exStyle, 96);
    } else {
        AdjustWindowRectEx(&rc, style, FALSE, exStyle);
    }

    impl_->window =
        CreateWindowExW(exStyle, L"EngWindowClass", wtitle.c_str(), style, CW_USEDEFAULT,
                        CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr,
                        GetModuleHandleW(nullptr), impl_.get());
    if (!impl_->window) {
        ENG_LOGE("platform", "CreateWindowEx failed (%lu)", GetLastError());
        return false;
    }

    impl_->dc = GetDC(impl_->window);
    int format = 0;
    UINT samples = 0;
    if (!impl_->dc || !ChooseArbPixelFormat(impl_->dc, desc, &format, &samples)) {
        samples = 0;
        if (impl_->dc) ChooseLegacyPixelFormat(impl_->dc, desc, &format);
    }
    if (format == 0) {
        ENG_LOGE("platform", "no usable pixel format (ChoosePixelFormat failed)");
        Destroy();
        return false;
    }
    PIXELFORMATDESCRIPTOR pfd{};
    DescribePixelFormat(impl_->dc, format, sizeof(pfd), &pfd);
    if (!SetPixelFormat(impl_->dc, format, &pfd)) {
        ENG_LOGE("platform", "SetPixelFormat failed (%lu)", GetLastError());
        Destroy();
        return false;
    }

    SetLastError(0);
    impl_->glrc = CreateRealContext(impl_->dc, desc, win32::Wgl().available, &impl_->coreProfile);
    if (!impl_->glrc) {
        ENG_LOGE("platform", "failed to create a WGL context (%lu)", GetLastError());
        Destroy();
        return false;
    }
    if (!wglMakeCurrent(impl_->dc, impl_->glrc)) {
        ENG_LOGE("platform", "wglMakeCurrent failed (%lu)", GetLastError());
        Destroy();
        return false;
    }

    // Оконная процедура видит только Impl, поэтому публикуем владельца сейчас.
    impl_->ownerWindow = this;
    SetVSync(desc.vsync);
    Show();
    Focus();

    if (desc.mode == WindowMode::Fullscreen) SetMode(WindowMode::Fullscreen);
    else if (desc.mode == WindowMode::Borderless) SetMode(WindowMode::Borderless);

    UpdateMetrics(impl_.get(), impl_->window);
    const gl::GLubyte* version = nullptr;
    auto getString = reinterpret_cast<const gl::GLubyte*(WINAPI*)(unsigned int)>(
        win32::GLGetProcAddress("glGetString"));
    if (getString) version = getString(win32::kGlVersion);

    ENG_LOGI("platform", "window created %dx%d (fb %dx%d, dpi %.2f, samples %u, %s)", impl_->width,
             impl_->height, impl_->fbWidth, impl_->fbHeight, impl_->dpiScale, samples,
             impl_->coreProfile ? "core profile" : "legacy profile");
    if (version) ENG_LOGI("platform", "GL_VERSION: %s", reinterpret_cast<const char*>(version));
    return true;
}

void Window::Destroy() {
    if (!impl_) return;
    if (impl_->glrc) {
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(impl_->glrc);
        impl_->glrc = nullptr;
    }
    if (impl_->cursorMode == 2) ClipCursor(nullptr);
    if (impl_->window) {
        if (impl_->dc) {
            ReleaseDC(impl_->window, impl_->dc);
            impl_->dc = nullptr;
        }
        DestroyWindow(impl_->window);
        impl_->window = nullptr;
    } else {
        // Окна нет (частично провалившийся Create не оставляет нативных ресурсов).
        impl_->dc = nullptr;
    }
    impl_->ownerWindow = nullptr;
    impl_->shouldClose = false;
    impl_->fullscreen = false;
}

void Window::PollEvents() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            RequestClose();
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (impl_->relativeMouse) ApplyRelativeMouse(impl_.get());
}

void Window::SwapBuffers() {
    // С указанием области видимости: точка входа Win32 делит имя с этим методом.
    if (impl_->dc) ::SwapBuffers(impl_->dc);
}

bool Window::ShouldClose() const { return impl_->shouldClose; }

void Window::RequestClose() {
    if (impl_->shouldClose) return;
    impl_->shouldClose = true;
    if (callbacks.onClose) callbacks.onClose();
}

void Window::SetTitle(const std::string& title) {
    impl_->title = title;
    if (impl_->window) SetWindowTextW(impl_->window, win32::Utf8ToWide(title).c_str());
}

void Window::SetSize(int w, int h) {
    if (!impl_->window || w <= 0 || h <= 0) return;
    if (impl_->fullscreen) {
        // Геометрию полноэкранного режима диктует монитор, поэтому запоминается
        // только логический размер на случай возврата в оконный режим.
        impl_->width = w;
        impl_->height = h;
        return;
    }
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(impl_->window, GWL_STYLE));
    const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(impl_->window, GWL_EXSTYLE));
    RECT rc{0, 0, w, h};
    if (win32::Dpi().adjustForDpi) {
        win32::Dpi().adjustForDpi(&rc, style, FALSE, exStyle, win32::WindowDpi(impl_->window));
    } else {
        AdjustWindowRectEx(&rc, style, FALSE, exStyle);
    }
    SetWindowPos(impl_->window, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    UpdateMetrics(impl_.get(), impl_->window);
}

void Window::SetMode(WindowMode mode) {
    impl_->mode = mode;
    if (!impl_->window) return;

    if (mode == WindowMode::Fullscreen) {
        if (impl_->fullscreen) return;
        impl_->savedStyle = GetWindowLongPtrW(impl_->window, GWL_STYLE);
        impl_->savedExStyle = GetWindowLongPtrW(impl_->window, GWL_EXSTYLE);
        impl_->savedPlacement.length = sizeof(WINDOWPLACEMENT);
        GetWindowPlacement(impl_->window, &impl_->savedPlacement);

        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        const HMONITOR monitor = MonitorFromWindow(impl_->window, MONITOR_DEFAULTTONEAREST);
        if (!GetMonitorInfoW(monitor, &mi)) {
            ENG_LOGW("platform", "fullscreen: GetMonitorInfo failed (%lu)", GetLastError());
            return;
        }
        // Borderless + размер монитора вместо ChangeDisplaySettingsEx: разрешение
        // рабочего стола никогда не трогается, и этот путь не может провалиться
        // на экзотических конфигурациях дисплеев.
        SetWindowLongPtrW(impl_->window, GWL_STYLE,
                          (impl_->savedStyle & ~(WS_CAPTION | WS_THICKFRAME)) | WS_POPUP);
        SetWindowPos(impl_->window, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        impl_->fullscreen = true;
        UpdateMetrics(impl_.get(), impl_->window);
        return;
    }

    if (impl_->fullscreen) {
        SetWindowLongPtrW(impl_->window, GWL_STYLE, impl_->savedStyle);
        SetWindowLongPtrW(impl_->window, GWL_EXSTYLE, impl_->savedExStyle);
        SetWindowPlacement(impl_->window, &impl_->savedPlacement);
        SetWindowPos(impl_->window, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        impl_->fullscreen = false;
    }
    if (mode == WindowMode::Borderless) {
        SetWindowLongPtrW(impl_->window, GWL_STYLE,
                          (GetWindowLongPtrW(impl_->window, GWL_STYLE) &
                           ~(WS_CAPTION | WS_THICKFRAME)) |
                              WS_POPUP);
    } else {
        DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        if (impl_->desc.resizable) style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
        SetWindowLongPtrW(impl_->window, GWL_STYLE, style);
    }
    SetWindowPos(impl_->window, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    UpdateMetrics(impl_.get(), impl_->window);
}

void Window::SetVSync(bool enabled) {
    win32::WglApi& api = win32::Wgl();
    if (api.swapInterval) api.swapInterval(enabled ? 1 : 0);
}

void Window::Minimize() {
    if (impl_->window) ShowWindow(impl_->window, SW_MINIMIZE);
}

void Window::Maximize() {
    if (impl_->window) ShowWindow(impl_->window, SW_MAXIMIZE);
}

void Window::Restore() {
    if (impl_->window) ShowWindow(impl_->window, SW_RESTORE);
}

void Window::Show() {
    if (impl_->window) ShowWindow(impl_->window, SW_SHOW);
}

void Window::Hide() {
    if (impl_->window) ShowWindow(impl_->window, SW_HIDE);
}

void Window::Focus() {
    if (!impl_->window) return;
    ShowWindow(impl_->window, SW_RESTORE);
    SetForegroundWindow(impl_->window);
    SetFocus(impl_->window);
}

void Window::MakeCurrent() {
    if (impl_->dc && impl_->glrc) wglMakeCurrent(impl_->dc, impl_->glrc);
}

void Window::WaitEventsTimeout(f32 seconds) {
    if (seconds > 0.0f) {
        DWORD timeout = static_cast<DWORD>(seconds * 1000.0f);
        if (timeout == 0) timeout = 1;
        // MWMO_INPUTAVAILABLE позволяет уже стоящим в очереди сообщениям тоже будить ожидание.
        MsgWaitForMultipleObjectsEx(0, nullptr, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    PollEvents();
}

void Window::SetCursorVisible(bool visible) {
    if (!visible) {
        while (ShowCursor(FALSE) >= 0) {
        }
    } else {
        while (ShowCursor(TRUE) < 0) {
        }
    }
}

void Window::SetCursorMode(int mode) {
    impl_->cursorMode = mode;
    if (!impl_->window) return;

    switch (mode) {
        case 2: {  // отключён / относительный
            impl_->relativeMouse = true;
            SetCapture(impl_->window);
            RECT rc{};
            GetClientRect(impl_->window, &rc);
            POINT tl{rc.left, rc.top};
            POINT br{rc.right, rc.bottom};
            ClientToScreen(impl_->window, &tl);
            ClientToScreen(impl_->window, &br);
            const RECT clip{tl.x, tl.y, br.x, br.y};
            ClipCursor(&clip);
            SetCursor(nullptr);
            ShowCursor(FALSE);
            ApplyRelativeMouse(impl_.get());
            break;
        }
        case 1:  // скрыт
            impl_->relativeMouse = false;
            ClipCursor(nullptr);
            ReleaseCapture();
            SetCursor(nullptr);
            ShowCursor(FALSE);
            break;
        default:  // обычный
            impl_->relativeMouse = false;
            ClipCursor(nullptr);
            ReleaseCapture();
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            ShowCursor(TRUE);
            break;
    }
}

void Window::SetClipboardText(const std::string& text) {
    if (!win32::ClipboardOpen()) {
        ENG_LOGW("platform", "clipboard busy; SetClipboardText dropped");
        return;
    }
    if (!EmptyClipboard()) {
        CloseClipboard();
        return;
    }
    const std::wstring wide = win32::Utf8ToWide(text);
    const size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem) {
        void* dst = GlobalLock(mem);
        if (dst) {
            std::memcpy(dst, wide.c_str(), bytes);
            GlobalUnlock(mem);
            if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
        } else {
            GlobalFree(mem);
        }
    }
    CloseClipboard();
}

std::string Window::GetClipboardText() const {
    if (!win32::ClipboardOpen()) {
        ENG_LOGW("platform", "clipboard busy; GetClipboardText returned empty");
        return {};
    }
    std::string result;
    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (data) {
        const wchar_t* src = static_cast<const wchar_t*>(GlobalLock(data));
        if (src) {
            result = win32::WideToUtf8(std::wstring(src));
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
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
bool Window::IsFocused() const {
    return impl_->window != nullptr && GetForegroundWindow() == impl_->window;
}
bool Window::IsMinimized() const {
    return impl_->window ? IsIconic(impl_->window) != FALSE : false;
}
bool Window::IsFullscreen() const { return impl_->fullscreen; }

Vec2 Window::MousePosition() const {
    if (!impl_->window) return {};
    POINT p{};
    if (!GetCursorPos(&p)) return {};
    if (!ScreenToClient(impl_->window, &p)) return {};
    return {static_cast<f32>(p.x), static_cast<f32>(p.y)};
}

const std::string& Window::Title() const { return impl_->title; }
void* Window::NativeHandle() const { return static_cast<void*>(impl_->window); }
void* Window::NativeDisplay() const { return static_cast<void*>(GetModuleHandleW(nullptr)); }
void* (*Window::GLGetProcAddress() const)(const char*) { return win32::GLGetProcAddress; }

}  // namespace crossrender

#else

// В сборках без Windows эта единица трансляции пуста; каждая платформа
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
