//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: внутренний мост WASM между оконной частью и сервисами Platform.h.
//
#pragma once

#include "crossrender/platform/Window.h"
#include "crossrender/platform/Platform.h"

#if defined(ENG_PLATFORM_WASM)

namespace crossrender {

// Окно, которое показывает прокрутчик кадров (задаётся в Window::Create,
// сбрасывается в Window::Destroy); определено в WindowWasm.cpp.
Window* WasmActiveWindow();

// Резолвер GL-точек входа. Определён в PlatformWasm.cpp: функции WebGL2
// статически слинкованы в сборках Emscripten, а emscripten_webgl_get_proc_address
// — лишь подспорье для кода, который настаивает на разрешении указателей.
void* WasmGLGetProcAddress(const char* name);

// Регистрирует DOM-обработчики событий для `canvasTarget`. Определено в
// WindowWasm.cpp; вызывается из Window::Create.
void WasmInstallEventHandlers(const char* canvasTarget);

}  // namespace crossrender

#endif  // ENG_PLATFORM_WASM
