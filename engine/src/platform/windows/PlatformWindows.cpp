// Сервисы платформы Windows: диалоги, пути, сведения о системе, цикл AppHooks и
// headless GL-контекст на скрытом окне.
#include "crossrender/platform/Platform.h"

#include "crossrender/core/Log.h"
#include "crossrender/core/Time.h"
#include "crossrender/platform/Window.h"

#if defined(ENG_PLATFORM_WINDOWS)

#include <windows.h>
#include <psapi.h>
#include <commdlg.h>
#include <shellapi.h>

#include "Win32Common.h"

#include <cstdio>
#include <string>
#include <cstring>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#define ENG_CPUID_AVAILABLE 1
#elif defined(ENG_ARCH_X64)
#include <cpuid.h>
#define ENG_CPUID_AVAILABLE 1
#endif

namespace crossrender {

// ---------------------------------------------------------------------------
// Жизненный цикл / идентификация
// ---------------------------------------------------------------------------
bool PlatformInit() { return true; }

void PlatformShutdown() {}

std::string PlatformName() { return "Windows"; }

std::string PlatformArch() {
#if defined(ENG_ARCH_ARM64)
    return "arm64";
#elif defined(ENG_ARCH_X64)
    return "x86_64";
#else
    return "unknown";
#endif
}

// ---------------------------------------------------------------------------
// CPU / память
// ---------------------------------------------------------------------------
namespace {

// Собирает 48-байтную строку бренда из трёх листов, которые её несут.
std::string CpuBrandString() {
#if defined(ENG_ARCH_X64) && defined(ENG_CPUID_AVAILABLE)
    auto regs = [](unsigned int leaf, unsigned int out[4]) {
#if defined(_MSC_VER) && !defined(__clang__)
        int r[4];
        __cpuid(reinterpret_cast<int*>(r), static_cast<int>(leaf));
        out[0] = static_cast<unsigned int>(r[0]);
        out[1] = static_cast<unsigned int>(r[1]);
        out[2] = static_cast<unsigned int>(r[2]);
        out[3] = static_cast<unsigned int>(r[3]);
#else
        unsigned int a = 0, b = 0, c = 0, d = 0;
        __get_cpuid(leaf, &a, &b, &c, &d);
        out[0] = a;
        out[1] = b;
        out[2] = c;
        out[3] = d;
#endif
    };

    unsigned int reg[4] = {0, 0, 0, 0};
    regs(0x80000000u, reg);
    if (reg[0] < 0x80000004u) return std::string();  // строка бренда не поддерживается
    char brand[49] = {};
    unsigned int* words = reinterpret_cast<unsigned int*>(brand);
    for (unsigned int leaf = 0; leaf < 3; ++leaf) {
        regs(0x80000002u + leaf, reg);
        words[leaf * 4 + 0] = reg[0];
        words[leaf * 4 + 1] = reg[1];
        words[leaf * 4 + 2] = reg[2];
        words[leaf * 4 + 3] = reg[3];
    }
    std::string s(brand);
    // CPUID дополняет строку бренда ведущими пробелами.
    const size_t first = s.find_first_not_of(" \t");
    return first == std::string::npos ? std::string() : s.substr(first);
#else
    return std::string();
#endif
}

std::string RegistryCpuName() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ,
                      &key) != ERROR_SUCCESS) {
        return std::string();
    }
    wchar_t buf[256] = {};
    DWORD size = sizeof(buf);
    DWORD type = 0;
    const LONG rc = RegQueryValueExW(key, L"ProcessorNameString", nullptr, &type,
                                     reinterpret_cast<LPBYTE>(buf), &size);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS || type != REG_SZ) return std::string();
    return win32::WideToUtf8(buf);
}

}  // namespace

std::string CpuName() {
    std::string name = CpuBrandString();
    if (name.empty()) name = RegistryCpuName();
    if (name.empty()) name = PlatformArch();
    return name;
}

u64 TotalPhysicalMemory() {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) return 0;
    return static_cast<u64>(status.ullTotalPhys);
}

int CpuCoreCount() {
    SYSTEM_INFO info{};
    GetNativeSystemInfo(&info);
    if (info.dwNumberOfProcessors > 0) return static_cast<int>(info.dwNumberOfProcessors);
    return 1;
}

// ---------------------------------------------------------------------------
// Пути
// ---------------------------------------------------------------------------
usize ProcessResidentBytes() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(),
                         reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    return static_cast<usize>(pmc.WorkingSetSize);
}

f64 ProcessCpuSeconds() {
    FILETIME create{}, exit{}, kernel{}, user{};
    GetProcessTimes(GetCurrentProcess(), &create, &exit, &kernel, &user);
    auto toSec = [](const FILETIME& ft) {
        ULARGE_INTEGER v{};
        v.LowPart = ft.dwLowDateTime;
        v.HighPart = ft.dwHighDateTime;
        return static_cast<f64>(v.QuadPart) / 1e7;  // интервалы по 100 нс
    };
    return toSec(kernel) + toSec(user);
}

std::string ExecutablePath() {
    // Увеличиваем буфер, пока путь не поместится; иначе GetModuleFileNameW обрежет.
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD written =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0) return std::string();
        if (written < buffer.size() - 1) {
            buffer.resize(written);
            break;
        }
        if (buffer.size() >= 65536) {
            buffer.resize(written);
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    return win32::WideToUtf8(buffer);
}

std::string ExecutableDir() {
    const std::string path = ExecutablePath();
    const size_t slash = path.find_last_of("\\/");
    if (slash == std::string::npos) return std::string();
    return path.substr(0, slash);
}

// ---------------------------------------------------------------------------
// Время / потоки
// ---------------------------------------------------------------------------
void SleepMs(u32 milliseconds) { Sleep(static_cast<DWORD>(milliseconds)); }

u64 CurrentThreadId() { return static_cast<u64>(GetCurrentThreadId()); }

// ---------------------------------------------------------------------------
// Диалоги / оболочка
// ---------------------------------------------------------------------------
void ShowMessageBox(const std::string& title, const std::string& message, MessageBoxType type) {
    UINT flags = MB_OK;
    switch (type) {
        case MessageBoxType::Warning: flags |= MB_ICONWARNING; break;
        case MessageBoxType::Error: flags |= MB_ICONERROR; break;
        case MessageBoxType::Question: flags |= MB_ICONQUESTION; break;
        case MessageBoxType::Info:
        default: flags |= MB_ICONINFORMATION; break;
    }
    const std::wstring wtitle = win32::Utf8ToWide(title);
    const std::wstring wmessage = win32::Utf8ToWide(message);
    MessageBoxW(nullptr, wmessage.c_str(), wtitle.c_str(), flags);
}

namespace {

// Превращает "*.png;*.jpg" (или "png,jpg") в завершённый двойным NUL список
// шаблонов, которого ждёт GetOpenFileNameW.
std::wstring BuildFilterPattern(const std::string& filter) {
    std::string cleaned;
    for (char c : filter) cleaned.push_back((c == ',') ? ';' : c);
    if (cleaned.empty() || cleaned == "*" || cleaned == "*.*") return L"*.*";

    std::wstring pattern;
    bool needsDot = true;
    for (char c : cleaned) {
        if (c == ';') {
            pattern.push_back(L';');
            needsDot = true;
            continue;
        }
        if (c == '.' && needsDot) {
            pattern.push_back(L'.');
            needsDot = false;
            continue;
        }
        pattern.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    }
    return pattern;
}

// filter — строка описания вида "Images (*.png;*.jpg)" или просто список
// шаблонов; тогда lpstrFilter остаётся пустым (все файлы).
std::wstring BuildFilterString(const std::string& filter, const wchar_t* allLabel) {
    std::wstring out;
    if (!filter.empty()) {
        const std::string description = filter;
        const std::wstring wdesc = win32::Utf8ToWide(description);
        out += wdesc;
        out.push_back(L'\0');
        out += BuildFilterPattern(filter);
        out.push_back(L'\0');
    }
    out += allLabel;
    out.push_back(L'\0');
    out += L"*.*";
    out.push_back(L'\0');
    out.push_back(L'\0');
    return out;
}

std::string RunFileDialog(bool save, const std::string& title, const std::string& filter,
                          const std::string& defaultNameOrFilter) {
    std::wstring buffer(4096, L'\0');
    if (save && !defaultNameOrFilter.empty()) {
        const std::wstring name = win32::Utf8ToWide(defaultNameOrFilter);
        const size_t copy = name.size() < buffer.size() - 1 ? name.size() : buffer.size() - 1;
        std::memcpy(buffer.data(), name.c_str(), copy * sizeof(wchar_t));
    }

    const std::wstring wtitle = win32::Utf8ToWide(title);
    const std::wstring patterns =
        BuildFilterString(filter, save ? L"All Files" : L"All Files (*.*)");

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = patterns.c_str();
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrTitle = wtitle.empty() ? nullptr : wtitle.c_str();
    ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR;
    if (save) {
        ofn.Flags |= OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    } else {
        ofn.Flags |= OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    }

    const BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    if (!ok) return std::string();  // отменено
    return win32::WideToUtf8(buffer.c_str());
}

}  // namespace

std::string OpenFileDialog(const std::string& title, const std::string& filter) {
    return RunFileDialog(false, title, filter, std::string());
}

std::string SaveFileDialog(const std::string& title, const std::string& defaultName) {
    return RunFileDialog(true, title, std::string(), defaultName);
}

bool OpenUrl(const std::string& url) {
    const std::wstring wurl = win32::Utf8ToWide(url);
    const HINSTANCE result = ShellExecuteW(nullptr, L"open", wurl.c_str(), nullptr, nullptr,
                                           SW_SHOWNORMAL);
    // ShellExecute при неудаче возвращает значение <= 32.
    return reinterpret_cast<INT_PTR>(result) > 32;
}

void GetScreenSize(int* w, int* h) {
    if (w) *w = GetSystemMetrics(SM_CXSCREEN);
    if (h) *h = GetSystemMetrics(SM_CYSCREEN);
}

// ---------------------------------------------------------------------------
// Заглушки мобильных/консольных функций. На десктопе Windows нет экранной
// клавиатуры, вибрации и удержания экрана, поэтому это намеренные no-op.
// ---------------------------------------------------------------------------
void SetSoftKeyboardVisible(bool visible) { (void)visible; }
void SetKeepScreenAwake(bool enabled) { (void)enabled; }
void Vibrate(u32 milliseconds) { (void)milliseconds; }

bool IsAppForeground() {
    const HWND foreground = GetForegroundWindow();
    if (!foreground) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(foreground, &pid);
    return pid == GetCurrentProcessId();
}

// ---------------------------------------------------------------------------
// Жизненный цикл приложения
// ---------------------------------------------------------------------------
int RunApp(const AppHooks& hooks) {
    WindowDesc desc;
    // Запуск без консоли (сборка GUI-подсистемы) означает, что видимое окно —
    // единственный способ взаимодействия, поэтому оставляем значения по умолчанию.
    desc.vsync = true;

    Window window;
    if (!window.Create(desc)) {
        ENG_LOGE("platform", "RunApp: window creation failed");
        return 1;
    }
    if (!win32::Wgl().available) {
        ENG_LOGW("platform", "RunApp: no WGL_ARB_create_context; running on a legacy context");
    }

    if (hooks.onInit) hooks.onInit(hooks.user);

    f64 previous = NowSeconds();
    int result = 0;
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

        // Без vsync цикл вращал бы ядро; возвращаем системе срез времени.
        if (!desc.vsync) SleepMs(1);
    }

    if (hooks.onShutdown) hooks.onShutdown(hooks.user);
    window.Destroy();
    return result;
}

// ---------------------------------------------------------------------------
// Headless GL-контекст (тесты / CI / --headless).
//
// Реализован скрытым окном плюс WGL-контекстом: в Windows нет рабочего пути
// только через pbuffer без WGL_ARB_pbuffer, а скрытое окно доступно
// на каждом драйвере. Окно создаётся, но никогда не показывается и живёт,
// пока жив контекст.
// ---------------------------------------------------------------------------
namespace {

struct HeadlessState {
    HWND window = nullptr;
    HDC dc = nullptr;
    HGLRC glrc = nullptr;
    int refCount = 0;
};

HeadlessState& Headless() {
    static HeadlessState state;
    return state;
}

bool CreateHeadlessWindow(HeadlessState& state, const WindowDesc& desc) {
    // Класс нужен до появления DC. Вспомогательный bootstrap уже регистрирует
    // свой класс, поэтому здесь используется простой класс с DefWindowProc.
    static const wchar_t* kClass = L"EngHeadlessGL";
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_OWNDC;
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClass;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            ENG_LOGE("platform", "headless: RegisterClassEx failed (%lu)", GetLastError());
            return false;
        }
        registered = true;
    }
    state.window = CreateWindowExW(0, kClass, L"EngHeadless", WS_OVERLAPPED, 0, 0, 16, 16, nullptr,
                                   nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!state.window) {
        ENG_LOGE("platform", "headless: CreateWindowEx failed (%lu)", GetLastError());
        return false;
    }
    state.dc = GetDC(state.window);

    int format = 0;
    UINT samples = 0;
    const win32::WglApi& api = win32::Wgl();
    if (state.dc && api.available) {
        const int attrs[] = {win32::kWglDrawToWindowArb, win32::kWglAccelerationArb,
                             win32::kWglPixelTypeArb,    win32::kWglTypeRgbaArb,
                             win32::kWglColorBitsArb,    24,
                             win32::kWglDepthBitsArb,    24,
                             win32::kWglStencilBitsArb,  8,
                             0};
        if (api.choosePixelFormat(state.dc, attrs, nullptr, 1, &format, &samples) && format != 0) {
            PIXELFORMATDESCRIPTOR pfd{};
            DescribePixelFormat(state.dc, format, sizeof(pfd), &pfd);
            if (!SetPixelFormat(state.dc, format, &pfd)) format = 0;
        }
    }
    if (format == 0 && state.dc) {
        PIXELFORMATDESCRIPTOR pfd{};
        pfd.nSize = sizeof(pfd);
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 24;
        pfd.cDepthBits = 24;
        const int legacy = ChoosePixelFormat(state.dc, &pfd);
        if (legacy != 0) {
            PIXELFORMATDESCRIPTOR got{};
            DescribePixelFormat(state.dc, legacy, sizeof(got), &got);
            if (SetPixelFormat(state.dc, legacy, &got)) format = legacy;
        }
    }
    if (format == 0) {
        ENG_LOGW("platform", "headless: no usable pixel format");
        return false;
    }

    if (api.available) {
        const int attribs33[] = {win32::kWglContextMajorVersionArb, desc.glMajor,
                                 win32::kWglContextMinorVersionArb, desc.glMinor,
                                 win32::kWglContextProfileMaskArb,
                                 win32::kWglContextCoreProfileBitArb, 0};
        state.glrc = api.createContextAttribs(state.dc, nullptr, attribs33);
        if (!state.glrc) {
            const int attribs32[] = {win32::kWglContextMajorVersionArb, 3,
                                     win32::kWglContextMinorVersionArb, 2,
                                     win32::kWglContextProfileMaskArb,
                                     win32::kWglContextCoreProfileBitArb, 0};
            state.glrc = api.createContextAttribs(state.dc, nullptr, attribs32);
        }
    }
    if (!state.glrc) {
        // Крайний случай: legacy-контекст (GL 1.1 + расширения), чтобы тесты,
        // которым нужен лишь текущий контекст, всё же работали.
        state.glrc = wglCreateContext(state.dc);
        if (state.glrc) ENG_LOGW("platform", "headless: created a legacy context");
    }
    return state.glrc != nullptr;
}

}  // namespace

bool CreateHeadlessGLContext() {
    HeadlessState& state = Headless();
    if (state.glrc) {
        ++state.refCount;
        wglMakeCurrent(state.dc, state.glrc);
        return true;
    }
    WindowDesc desc;
    desc.glMajor = 3;
    desc.glMinor = 3;
    desc.depthBuffer = true;
    desc.stencilBuffer = true;
    if (!CreateHeadlessWindow(state, desc)) {
        if (state.dc && state.window) {
            ReleaseDC(state.window, state.dc);
            state.dc = nullptr;
        }
        if (state.window) {
            DestroyWindow(state.window);
            state.window = nullptr;
        }
        return false;
    }
    if (!wglMakeCurrent(state.dc, state.glrc)) {
        ENG_LOGW("platform", "headless: wglMakeCurrent failed (%lu)", GetLastError());
        wglDeleteContext(state.glrc);
        state.glrc = nullptr;
        return false;
    }
    state.refCount = 1;
    ENG_LOGI("platform", "headless OpenGL context created (hidden window %dx%d)", 16, 16);
    return true;
}

void DestroyHeadlessGLContext() {
    HeadlessState& state = Headless();
    if (!state.glrc) return;
    if (--state.refCount > 0) return;
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(state.glrc);
    state.glrc = nullptr;
    if (state.dc) {
        ReleaseDC(state.window, state.dc);
        state.dc = nullptr;
    }
    if (state.window) {
        DestroyWindow(state.window);
        state.window = nullptr;
    }
    state.refCount = 0;
}

bool HasHeadlessGLContext() { return Headless().glrc != nullptr; }

void* HeadlessGLGetProcAddress(const char* name) { return win32::GLGetProcAddress(name); }

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
