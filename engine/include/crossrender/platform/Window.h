//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: нативное окно, контекст OpenGL и снимок состояния ввода.
//
#pragma once

#include "crossrender/core/Base.h"
#include "crossrender/core/Math.h"

#include <memory>
#include <string>
#include <vector>
#include <functional>

namespace crossrender {

// ---------------------------------------------------------------------------
// Клавиши
// ---------------------------------------------------------------------------
enum class Key : int {
    Unknown = 0,
    Space, Apostrophe, Comma, Minus, Period, Slash,
    Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    Semicolon, Equal,
    A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    LeftBracket, Backslash, RightBracket, Grave,
    Escape, Enter, Tab, Backspace, Insert, Delete, Right, Left, Down, Up,
    PageUp, PageDown, Home, End, CapsLock, ScrollLock, NumLock, PrintScreen, Pause,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    F13, F14, F15, F16, F17, F18, F19, F20, F21, F22, F23, F24,
    Keypad0, Keypad1, Keypad2, Keypad3, Keypad4, Keypad5, Keypad6, Keypad7, Keypad8, Keypad9,
    KeypadDecimal, KeypadDivide, KeypadMultiply, KeypadSubtract, KeypadAdd, KeypadEnter, KeypadEqual,
    LeftShift, LeftControl, LeftAlt, LeftSuper,
    RightShift, RightControl, RightAlt, RightSuper,
    Menu,
    Count
};

enum class KeyAction : int { Release = 0, Press = 1, Repeat = 2 };
enum class MouseButton : int { Left = 0, Right = 1, Middle = 2, X1 = 3, X2 = 4, Count = 5 };
enum class TouchPhase : int { Down, Move, Up, Cancel };

const char* KeyName(Key key);

// ---------------------------------------------------------------------------
// Снимок ввода (опрос раз в кадр, пересобирается в BeginFrame/EndFrame)
// ---------------------------------------------------------------------------
constexpr int kMaxTouches = 10;

struct TouchPoint {
    i32 id = -1;
    Vec2 pos;
    Vec2 start;
    TouchPhase phase = TouchPhase::Cancel;
    f32 pressure = 1.0f;
};

class Input {
public:
    static constexpr int kKeyCount = static_cast<int>(Key::Count);
    static constexpr int kMouseCount = static_cast<int>(MouseButton::Count);

    void BeginFrame();
    void EndFrame();

    // ---- клавиатура -----------------------------------------------------
    void OnKey(Key key, KeyAction action, bool repeat);
    [[nodiscard]] bool KeyDown(Key k) const { return keyDown_[Idx(k)]; }
    [[nodiscard]] bool KeyPressed(Key k) const { return keyPressed_[Idx(k)]; }
    [[nodiscard]] bool KeyReleased(Key k) const { return keyReleased_[Idx(k)]; }
    [[nodiscard]] bool AnyKeyDown() const;
    [[nodiscard]] bool CtrlDown() const { return KeyDown(Key::LeftControl) || KeyDown(Key::RightControl); }
    [[nodiscard]] bool ShiftDown() const { return KeyDown(Key::LeftShift) || KeyDown(Key::RightShift); }
    [[nodiscard]] bool AltDown() const { return KeyDown(Key::LeftAlt) || KeyDown(Key::RightAlt); }
    [[nodiscard]] bool SuperDown() const { return KeyDown(Key::LeftSuper) || KeyDown(Key::RightSuper); }

    // ---- текст ---------------------------------------------------------
    void OnText(u32 codepoint) { textInput_.push_back(codepoint); }
    [[nodiscard]] const std::vector<u32>& TextInput() const { return textInput_; }

    // ---- мышь --------------------------------------------------------
    void OnMouseMove(Vec2 pos);
    void OnMouseButton(MouseButton b, bool down, Vec2 pos);
    void OnScroll(Vec2 delta) { scroll_ += delta; }

    [[nodiscard]] Vec2 MousePos() const { return mouse_; }
    [[nodiscard]] Vec2 MouseDelta() const { return mouseDelta_; }
    [[nodiscard]] Vec2 ScrollDelta() const { return scroll_; }
    [[nodiscard]] bool MouseDown(MouseButton b = MouseButton::Left) const { return mouseDown_[Mi(b)]; }
    [[nodiscard]] bool MousePressed(MouseButton b = MouseButton::Left) const { return mousePressed_[Mi(b)]; }
    [[nodiscard]] bool MouseReleased(MouseButton b = MouseButton::Left) const { return mouseReleased_[Mi(b)]; }
    [[nodiscard]] bool MouseDoubleClick(MouseButton b = MouseButton::Left) const { return doubleClick_[Mi(b)]; }

    // ---- касания --------------------------------------------------------
    void OnTouch(const TouchPoint& tp);
    [[nodiscard]] const TouchPoint* Touches() const { return touches_; }
    [[nodiscard]] int TouchCount() const { return touchCount_; }

    // ---- геймпад (простой цифровой снимок) ----------------------------
    void SetGamepad(int index, bool connected, const float* axes, int axisCount, u32 buttons);
    [[nodiscard]] bool GamepadConnected(int index = 0) const;
    [[nodiscard]] u32 GamepadButtons(int index = 0) const;
    [[nodiscard]] Vec2 GamepadStick(int index = 0, int stick = 0) const;

private:
    static int Idx(Key k) {
        int i = static_cast<int>(k);
        return (i >= 0 && i < kKeyCount) ? i : 0;
    }
    static int Mi(MouseButton b) {
        int i = static_cast<int>(b);
        return (i >= 0 && i < kMouseCount) ? i : 0;
    }

    bool keyDown_[kKeyCount]{};
    bool keyPressed_[kKeyCount]{};
    bool keyReleased_[kKeyCount]{};
    bool mouseDown_[kMouseCount]{};
    bool mousePressed_[kMouseCount]{};
    bool mouseReleased_[kMouseCount]{};
    bool doubleClick_[kMouseCount]{};
    Vec2 mouse_, prevMouse_, mouseDelta_, scroll_;
    f64 lastClickTime_[kMouseCount]{};
    std::vector<u32> textInput_;
    TouchPoint touches_[kMaxTouches];
    int touchCount_ = 0;
    struct PadState {
        bool connected = false;
        u32 buttons = 0;
        float axes[8]{};
        int axisCount = 0;
    };
    PadState pads_[4];
};

// ---------------------------------------------------------------------------
// Окно
// ---------------------------------------------------------------------------
enum class WindowMode { Windowed, Fullscreen, Borderless };

struct WindowDesc {
    std::string title = "CrossRender";
    int width = 1280;
    int height = 720;
    int minWidth = 320;
    int minHeight = 240;
    bool resizable = true;
    bool vsync = true;
    bool highDpi = true;
    int msaaSamples = 4;
    WindowMode mode = WindowMode::Windowed;
    bool transparent = false;
    bool depthBuffer = true;
    bool stencilBuffer = false;
    int glMajor = 3;
    int glMinor = 3;
};

// События, доставляемые в `WindowCallbacks` дополнительно к опрашиваемому состоянию Input.
struct WindowCallbacks {
    std::function<void(int, int)> onResize;
    std::function<void()> onClose;
    std::function<void(bool)> onFocus;
    std::function<void(Vec2)> onDropFiles;  // не используется: ниже вариант с одним путём
    std::function<void(const std::vector<std::string>&)> onFilesDropped;
    std::function<void(f32)> onDpiChanged;
};

class Window {
public:
    Window();
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Создаёт нативное окно и GL-контекст. Возвращает false при неудаче.
    bool Create(const WindowDesc& desc);
    void Destroy();

    // Обрабатывает очередь сообщений платформы (неблокирующе).
    void PollEvents();
    // Выводит задний буфер на экран.
    void SwapBuffers();
    // Просил ли пользователь закрыть окно.
    [[nodiscard]] bool ShouldClose() const;
    void RequestClose();
    void SetTitle(const std::string& title);
    void SetSize(int w, int h);
    void SetMode(WindowMode mode);
    void SetVSync(bool enabled);
    void Minimize();
    void Maximize();
    void Restore();
    void Show();
    void Hide();
    void Focus();
    // Делает GL-контекст текущим в этом потоке.
    void MakeCurrent();
    // Блокирует до следующего vsync (используется в режиме энергосбережения / редактора).
    void WaitEventsTimeout(f32 seconds);
    void SetCursorVisible(bool visible);
    void SetCursorMode(int mode);  // 0 обычный, 1 скрытый, 2 отключённый/относительный
    void SetClipboardText(const std::string& text);
    [[nodiscard]] std::string GetClipboardText() const;

    [[nodiscard]] int Width() const;
    [[nodiscard]] int Height() const;
    // Размер фреймбуфера в пикселях (отличается от размера окна на HiDPI).
    [[nodiscard]] int FramebufferWidth() const;
    [[nodiscard]] int FramebufferHeight() const;
    [[nodiscard]] f32 DpiScale() const;
    [[nodiscard]] f32 Aspect() const;
    [[nodiscard]] bool IsFocused() const;
    [[nodiscard]] bool IsMinimized() const;
    [[nodiscard]] bool IsFullscreen() const;
    [[nodiscard]] Vec2 MousePosition() const;
    [[nodiscard]] const std::string& Title() const;

    [[nodiscard]] Input& GetInput() { return input_; }
    [[nodiscard]] const Input& GetInput() const { return input_; }
    WindowCallbacks callbacks;

    // Нативные дескрипторы (HWND / NSWindow* / Display* / ANativeWindow* / ...)
    [[nodiscard]] void* NativeHandle() const;
    [[nodiscard]] void* NativeDisplay() const;
    // Возвращает функцию, разрешающую точки входа GL для этого контекста.
    [[nodiscard]] void* (*GLGetProcAddress() const)(const char*);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    Input input_;
};

// Реализуется в папке каждой платформы: engine/src/platform/<os>/Window<Os>.cpp
bool PlatformInit();
void PlatformShutdown();
std::string PlatformName();
// Возвращает указатель на платформенный разрешатель GL-процедур.
void* (*PlatformGLGetProcAddress())(const char*);

}  // namespace crossrender
