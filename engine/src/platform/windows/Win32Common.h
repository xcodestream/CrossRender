//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: общие помощники Win32: конвертация UTF-8/UTF-16 и утилиты для оконного и платформенного слоёв.
//
#pragma once

#if defined(ENG_PLATFORM_WINDOWS)

#include "crossrender/core/Base.h"

#include <windows.h>

#include <string>

namespace crossrender {
namespace win32 {

// ---------------------------------------------------------------------------
// Поверхность WGL-расширений (объявлена локально; движок намеренно избегает
// включения каких-либо системных GL-заголовков).
// ---------------------------------------------------------------------------
using PFNWGLCHOOSEPIXELFORMATARBPROC = BOOL(WINAPI*)(HDC, const int*, const FLOAT*, UINT, int*,
                                                     UINT*);
using PFNWGLCREATECONTEXTATTRIBSARBPROC = HGLRC(WINAPI*)(HDC, HGLRC, const int*);
using PFNWGLSWAPINTERVALEXTPROC = BOOL(WINAPI*)(int);

constexpr int kWglDrawToWindowArb = 0x2001;
constexpr int kWglAccelerationArb = 0x2003;
constexpr int kWglDoubleBufferArb = 0x2011;
constexpr int kWglPixelTypeArb = 0x2013;
constexpr int kWglColorBitsArb = 0x2014;
constexpr int kWglAlphaBitsArb = 0x201B;
constexpr int kWglDepthBitsArb = 0x2022;
constexpr int kWglStencilBitsArb = 0x2023;
constexpr int kWglSamplesArb = 0x2042;
constexpr int kWglSampleBuffersArb = 0x2041;
constexpr int kWglTypeRgbaArb = 0x202B;

constexpr int kWglContextMajorVersionArb = 0x2091;
constexpr int kWglContextMinorVersionArb = 0x2092;
constexpr int kWglContextProfileMaskArb = 0x9126;
constexpr int kWglContextCoreProfileBitArb = 0x00000001;
constexpr int kWglContextForwardCompatibleBitArb = 0x00000002;

constexpr unsigned int kGlVersion = 0x1F02;

// wglGetProcAddress отвечает только за расширения, поэтому точки входа GL 1.1
// резолвятся из ICD opengl32.dll как запасной вариант.
inline void* GLGetProcAddress(const char* name) {
    if (!name) return nullptr;
    void* p = reinterpret_cast<void*>(wglGetProcAddress(name));
    if (p) return p;
    static HMODULE gl = nullptr;
    if (!gl) gl = LoadLibraryA("opengl32.dll");
    if (!gl) return nullptr;
    return reinterpret_cast<void*>(GetProcAddress(gl, name));
}

struct WglApi {
    PFNWGLCHOOSEPIXELFORMATARBPROC choosePixelFormat = nullptr;
    PFNWGLCREATECONTEXTATTRIBSARBPROC createContextAttribs = nullptr;
    PFNWGLSWAPINTERVALEXTPROC swapInterval = nullptr;
    bool available = false;
};

// Создаёт скрытое bootstrap-окно с legacy-контекстом, чтобы разрешить ARB-точки
// входа. Безопасно вызывать повторно (результат кэшируется).
inline WglApi& Wgl() {
    static WglApi api;
    static bool tried = false;
    if (tried) return api;
    tried = true;

    static const wchar_t* kBootstrapClass = L"EngGLBootstrap";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kBootstrapClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return api;

    HWND hwnd = CreateWindowExW(0, kBootstrapClass, L"", WS_OVERLAPPEDWINDOW, 0, 0, 1, 1, nullptr,
                                nullptr, wc.hInstance, nullptr);
    if (!hwnd) return api;
    HDC dc = GetDC(hwnd);

    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;

    const int fmt = ChoosePixelFormat(dc, &pfd);
    if (fmt != 0 && SetPixelFormat(dc, fmt, &pfd)) {
        HGLRC rc = wglCreateContext(dc);
        if (rc) {
            if (wglMakeCurrent(dc, rc)) {
                api.choosePixelFormat = reinterpret_cast<PFNWGLCHOOSEPIXELFORMATARBPROC>(
                    wglGetProcAddress("wglChoosePixelFormatARB"));
                api.createContextAttribs = reinterpret_cast<PFNWGLCREATECONTEXTATTRIBSARBPROC>(
                    wglGetProcAddress("wglCreateContextAttribsARB"));
                api.swapInterval = reinterpret_cast<PFNWGLSWAPINTERVALEXTPROC>(
                    wglGetProcAddress("wglSwapIntervalEXT"));
                api.available =
                    api.choosePixelFormat != nullptr && api.createContextAttribs != nullptr;
                wglMakeCurrent(nullptr, nullptr);
            }
            wglDeleteContext(rc);
        }
    }
    ReleaseDC(hwnd, dc);
    DestroyWindow(hwnd);
    return api;
}

// ---------------------------------------------------------------------------
// UTF-8 <-> UTF-16
// ---------------------------------------------------------------------------
inline std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n =
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

inline std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0,
                                      nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n, nullptr,
                        nullptr);
    return out;
}

// ---------------------------------------------------------------------------
// Per-monitor DPI. Разрешается динамически, чтобы бинарник работал и на
// системах, появившихся до API per-monitor V2.
// ---------------------------------------------------------------------------
using PFN_SetProcessDpiAwarenessContext = BOOL(WINAPI*)(void*);
using PFN_GetDpiForWindow = UINT(WINAPI*)(HWND);
using PFN_AdjustWindowRectExForDpi = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);

inline void* DpiAwarenessPerMonitorV2() {
    return reinterpret_cast<void*>(static_cast<intptr_t>(-4));
}
inline void* DpiAwarenessPerMonitor() {
    return reinterpret_cast<void*>(static_cast<intptr_t>(-3));
}
inline void* DpiAwarenessSystem() {
    return reinterpret_cast<void*>(static_cast<intptr_t>(-2));
}

struct DpiApi {
    PFN_GetDpiForWindow getDpiForWindow = nullptr;
    PFN_AdjustWindowRectExForDpi adjustForDpi = nullptr;
};

inline const DpiApi& Dpi() {
    static DpiApi api = [] {
        DpiApi a;
        HMODULE user32 = GetModuleHandleA("user32.dll");
        if (user32) {
            a.getDpiForWindow =
                reinterpret_cast<PFN_GetDpiForWindow>(GetProcAddress(user32, "GetDpiForWindow"));
            a.adjustForDpi = reinterpret_cast<PFN_AdjustWindowRectExForDpi>(
                GetProcAddress(user32, "AdjustWindowRectExForDpi"));
        }
        return a;
    }();
    return api;
}

// Включает для процесса режим per-monitor V2 с постепенной деградацией.
inline bool EnablePerMonitorDpi() {
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (!user32) return false;
    auto setCtx = reinterpret_cast<PFN_SetProcessDpiAwarenessContext>(
        GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (setCtx && setCtx(DpiAwarenessPerMonitorV2())) return true;
    if (setCtx && setCtx(DpiAwarenessPerMonitor())) return true;
    if (setCtx && setCtx(DpiAwarenessSystem())) return true;
    using PFN_SetProcessDPIAware = BOOL(WINAPI*)();
    auto setAware =
        reinterpret_cast<PFN_SetProcessDPIAware>(GetProcAddress(user32, "SetProcessDPIAware"));
    return setAware ? setAware() != FALSE : false;
}

inline UINT WindowDpi(HWND hwnd) {
    if (hwnd && Dpi().getDpiForWindow) {
        const UINT d = Dpi().getDpiForWindow(hwnd);
        if (d != 0) return d;
    }
    HDC dc = GetDC(hwnd);
    if (dc) {
        const int d = GetDeviceCaps(dc, LOGPIXELSY);
        ReleaseDC(hwnd, dc);
        if (d > 0) return static_cast<UINT>(d);
    }
    return 96;
}

// ---------------------------------------------------------------------------
// Буфер обмена: это общий ресурс с блокировкой, поэтому его открытие повторяется
// некоторое время, а не сразу завершается неудачей.
// ---------------------------------------------------------------------------
inline bool ClipboardOpen() {
    for (int i = 0; i < 8; ++i) {
        if (OpenClipboard(nullptr)) return true;
        Sleep(1);
    }
    return false;
}

}  // namespace win32
}  // namespace crossrender

#endif  // ENG_PLATFORM_WINDOWS
