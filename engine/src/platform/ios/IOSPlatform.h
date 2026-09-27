//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: внутренний мост iOS между C++ и Objective-C++ частями платформенного слоя.
//
#pragma once

#include "crossrender/platform/Platform.h"

#if defined(ENG_PLATFORM_IOS)

extern "C" {
// Инсеты safe-area в points. Определены в SafeAreaIOS.cpp и заполняются из
// WindowIOS.mm (EngGLView.viewSafeAreaInsetsDidChange), когда UIKit сообщает
// новые инсеты. Обе функции — только для главного потока.
void eng_ios_set_safe_area(float left, float top, float right, float bottom);
void eng_ios_get_safe_area(float* left, float* top, float* right, float* bottom);

// Сервисы на основе UIKit, реализованы в WindowIOS.mm.
//
// PlatformIOS.cpp — обычная C++ единица трансляции (корневой CMake glob
// подхватывает только ios/*.cpp и ios/*.mm, а .cpp не может включать UIKit),
// поэтому каждый сервис, которому нужен Objective-C, открыт здесь как
// C-точка входа. Если не сказано иное, вызывать их нужно из главного потока.
void eng_ios_show_message_box(const char* title, const char* message, int type);
bool eng_ios_open_url(const char* url);
void eng_ios_get_screen_size(int* outW, int* outH);
// Копирует содержимое pasteboard в UTF-8 в `out` (NUL-терминировано,
// усечено до `cap`). Если читать нечего, всегда пишет валидную пустую строку.
void eng_ios_get_clipboard(char* out, int cap);
void eng_ios_set_clipboard(const char* utf8);
void eng_ios_set_soft_keyboard(bool visible);
void eng_ios_set_keep_awake(bool enabled);
bool eng_ios_is_app_foreground(void);
}  // extern "C"

namespace crossrender {

// Разрешает GL-точку входа через dlsym (фреймворк OpenGLES слинкован с
// процессом, поэтому все символы OpenGL ES 3.0 экспортированы). Реализовано в
// PlatformIOS.cpp, общий для Window::GLGetProcAddress()/PlatformGLGetProcAddress().
void* IOSGLGetProcAddress(const char* name);

// Реализовано в WindowIOS.mm: запускает настоящий run loop UIApplicationMain()
// и прокручивает `hooks` через CADisplayLink. До UIApplicationMain нельзя
// добраться из .cpp TU, поэтому RunApp() из PlatformIOS.cpp переадресует сюда.
int RunAppIOS(const AppHooks& hooks);

}  // namespace crossrender

#endif  // ENG_PLATFORM_IOS
