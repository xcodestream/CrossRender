// Платформонезависимый автомат ввода, общий для всех платформенных бэкендов.
#include "crossrender/core/Log.h"
#include "crossrender/core/Time.h"
#include "crossrender/platform/Window.h"

namespace crossrender {

const char* KeyName(Key key) {
    switch (key) {
        case Key::Unknown: return "Unknown";
        case Key::Space: return "Space";
        case Key::Apostrophe: return "'";
        case Key::Comma: return ",";
        case Key::Minus: return "-";
        case Key::Period: return ".";
        case Key::Slash: return "/";
        case Key::Num0: return "0";
        case Key::Num1: return "1";
        case Key::Num2: return "2";
        case Key::Num3: return "3";
        case Key::Num4: return "4";
        case Key::Num5: return "5";
        case Key::Num6: return "6";
        case Key::Num7: return "7";
        case Key::Num8: return "8";
        case Key::Num9: return "9";
        case Key::Semicolon: return ";";
        case Key::Equal: return "=";
        case Key::A: return "A";
        case Key::B: return "B";
        case Key::C: return "C";
        case Key::D: return "D";
        case Key::E: return "E";
        case Key::F: return "F";
        case Key::G: return "G";
        case Key::H: return "H";
        case Key::I: return "I";
        case Key::J: return "J";
        case Key::K: return "K";
        case Key::L: return "L";
        case Key::M: return "M";
        case Key::N: return "N";
        case Key::O: return "O";
        case Key::P: return "P";
        case Key::Q: return "Q";
        case Key::R: return "R";
        case Key::S: return "S";
        case Key::T: return "T";
        case Key::U: return "U";
        case Key::V: return "V";
        case Key::W: return "W";
        case Key::X: return "X";
        case Key::Y: return "Y";
        case Key::Z: return "Z";
        case Key::LeftBracket: return "[";
        case Key::Backslash: return "\\";
        case Key::RightBracket: return "]";
        case Key::Grave: return "`";
        case Key::Escape: return "Escape";
        case Key::Enter: return "Enter";
        case Key::Tab: return "Tab";
        case Key::Backspace: return "Backspace";
        case Key::Insert: return "Insert";
        case Key::Delete: return "Delete";
        case Key::Right: return "Right";
        case Key::Left: return "Left";
        case Key::Down: return "Down";
        case Key::Up: return "Up";
        case Key::PageUp: return "PageUp";
        case Key::PageDown: return "PageDown";
        case Key::Home: return "Home";
        case Key::End: return "End";
        case Key::CapsLock: return "CapsLock";
        case Key::ScrollLock: return "ScrollLock";
        case Key::NumLock: return "NumLock";
        case Key::PrintScreen: return "PrintScreen";
        case Key::Pause: return "Pause";
        case Key::F1: return "F1";
        case Key::F2: return "F2";
        case Key::F3: return "F3";
        case Key::F4: return "F4";
        case Key::F5: return "F5";
        case Key::F6: return "F6";
        case Key::F7: return "F7";
        case Key::F8: return "F8";
        case Key::F9: return "F9";
        case Key::F10: return "F10";
        case Key::F11: return "F11";
        case Key::F12: return "F12";
        case Key::F13: return "F13";
        case Key::F14: return "F14";
        case Key::F15: return "F15";
        case Key::F16: return "F16";
        case Key::F17: return "F17";
        case Key::F18: return "F18";
        case Key::F19: return "F19";
        case Key::F20: return "F20";
        case Key::F21: return "F21";
        case Key::F22: return "F22";
        case Key::F23: return "F23";
        case Key::F24: return "F24";
        case Key::Keypad0: return "Keypad0";
        case Key::Keypad1: return "Keypad1";
        case Key::Keypad2: return "Keypad2";
        case Key::Keypad3: return "Keypad3";
        case Key::Keypad4: return "Keypad4";
        case Key::Keypad5: return "Keypad5";
        case Key::Keypad6: return "Keypad6";
        case Key::Keypad7: return "Keypad7";
        case Key::Keypad8: return "Keypad8";
        case Key::Keypad9: return "Keypad9";
        case Key::KeypadDecimal: return "KeypadDecimal";
        case Key::KeypadDivide: return "KeypadDivide";
        case Key::KeypadMultiply: return "KeypadMultiply";
        case Key::KeypadSubtract: return "KeypadSubtract";
        case Key::KeypadAdd: return "KeypadAdd";
        case Key::KeypadEnter: return "KeypadEnter";
        case Key::KeypadEqual: return "KeypadEqual";
        case Key::LeftShift: return "LeftShift";
        case Key::LeftControl: return "LeftControl";
        case Key::LeftAlt: return "LeftAlt";
        case Key::LeftSuper: return "LeftSuper";
        case Key::RightShift: return "RightShift";
        case Key::RightControl: return "RightControl";
        case Key::RightAlt: return "RightAlt";
        case Key::RightSuper: return "RightSuper";
        case Key::Menu: return "Menu";
        default: return "Unknown";
    }
}

void Input::BeginFrame() {
    for (int i = 0; i < kKeyCount; ++i) {
        keyPressed_[i] = false;
        keyReleased_[i] = false;
    }
    for (int i = 0; i < kMouseCount; ++i) {
        mousePressed_[i] = false;
        mouseReleased_[i] = false;
        doubleClick_[i] = false;
    }
    mouseDelta_ = Vec2{};
    scroll_ = Vec2{};
    textInput_.clear();
}

void Input::EndFrame() {
    prevMouse_ = mouse_;
}

void Input::OnKey(Key key, KeyAction action, bool repeat) {
    int i = Idx(key);
    if (action == KeyAction::Press) {
        if (!keyDown_[i]) keyPressed_[i] = true;
        keyDown_[i] = true;
    } else if (action == KeyAction::Release) {
        if (keyDown_[i]) keyReleased_[i] = true;
        keyDown_[i] = false;
    } else if (action == KeyAction::Repeat) {
        keyDown_[i] = true;
        keyPressed_[i] = true;
    }
    (void)repeat;
}

bool Input::AnyKeyDown() const {
    for (int i = 0; i < kKeyCount; ++i)
        if (keyDown_[i]) return true;
    return false;
}

void Input::OnMouseMove(Vec2 pos) {
    mouseDelta_ += pos - mouse_;
    mouse_ = pos;
}

void Input::OnMouseButton(MouseButton b, bool down, Vec2 pos) {
    int i = Mi(b);
    mouse_ = pos;
    if (down) {
        if (!mouseDown_[i]) mousePressed_[i] = true;
        mouseDown_[i] = true;
        // Детекция двойного клика: два нажатия в пределах 400 мс и 6 px.
        f64 now = NowSeconds();
        if (now - lastClickTime_[i] < 0.4) {
            doubleClick_[i] = true;
            lastClickTime_[i] = 0;
        } else {
            lastClickTime_[i] = now;
        }
    } else {
        if (mouseDown_[i]) mouseReleased_[i] = true;
        mouseDown_[i] = false;
    }
}

void Input::OnTouch(const TouchPoint& tp) {
    // Обновляем существующий указатель.
    for (int i = 0; i < touchCount_; ++i) {
        if (touches_[i].id == tp.id) {
            if (tp.phase == TouchPhase::Up || tp.phase == TouchPhase::Cancel) {
                for (int j = i; j < touchCount_ - 1; ++j) touches_[j] = touches_[j + 1];
                --touchCount_;
            } else {
                touches_[i] = tp;
            }
            // Касание также двигает виртуальную мышь, чтобы UI-код работал везде.
            OnMouseMove(tp.pos);
            if (tp.phase == TouchPhase::Down) OnMouseButton(MouseButton::Left, true, tp.pos);
            if (tp.phase == TouchPhase::Up) OnMouseButton(MouseButton::Left, false, tp.pos);
            return;
        }
    }
    if (tp.phase == TouchPhase::Down && touchCount_ < kMaxTouches) {
        touches_[touchCount_++] = tp;
        OnMouseMove(tp.pos);
        OnMouseButton(MouseButton::Left, true, tp.pos);
    }
}

void Input::SetGamepad(int index, bool connected, const float* axes, int axisCount, u32 buttons) {
    if (index < 0 || index >= 4) return;
    PadState& p = pads_[index];
    p.connected = connected;
    p.buttons = buttons;
    p.axisCount = Clamp(axisCount, 0, 8);
    for (int i = 0; i < p.axisCount; ++i) p.axes[i] = axes ? axes[i] : 0.0f;
}

bool Input::GamepadConnected(int index) const {
    return index >= 0 && index < 4 && pads_[index].connected;
}

u32 Input::GamepadButtons(int index) const {
    return (index >= 0 && index < 4) ? pads_[index].buttons : 0u;
}

Vec2 Input::GamepadStick(int index, int stick) const {
    if (index < 0 || index >= 4) return {};
    const PadState& p = pads_[index];
    int base = stick * 2;
    if (base + 1 >= p.axisCount) return {};
    return {p.axes[base], p.axes[base + 1]};
}

// ---------------------------------------------------------------------------
// Драйвер AppHooks по умолчанию, общий для десктопов без своей реализации.
// ---------------------------------------------------------------------------
}  // namespace crossrender
