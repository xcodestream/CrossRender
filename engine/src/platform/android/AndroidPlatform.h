//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: внутренний мост Android между оконной частью и сервисами Platform.h.
//
#pragma once

#include "crossrender/platform/Window.h"
#include "crossrender/platform/Platform.h"

#if defined(ENG_PLATFORM_ANDROID)

#include <android/native_activity.h>

namespace crossrender {

// --- реализовано в WindowAndroid.cpp ---------------------------------------
// Запускает цикл нативной activity (ALooper + AInputQueue) на вызывающем потоке
// и прокручивает `hooks`, пока activity не уничтожена или onFrame не вернёт false.
int RunAppAndroid(const AppHooks& hooks);
// Окно, которое показывает цикл (задаётся в Window::Create).
Window* AndroidActiveWindow();
// ANativeActivity, переданный в ANativeActivity_onCreate (до этого — null).
ANativeActivity* AndroidActivity();
// ANativeActivity::internalDataPath, либо "" если неизвестно.
const char* AndroidInternalDataPath();
// Плотность экрана в px/dp (DpiScale); 1.0, если неизвестна.
float AndroidDisplayDensity();
// True между onResume и onPause.
bool AndroidAppIsForeground();
// Размер нативного окна в физических пикселях (false, если окна нет).
bool AndroidNativeWindowSize(int* outWidth, int* outHeight);
// Полный размер экрана в dp (с откатом к нативному окну, затем к 1280x720).
void AndroidScreenSizeDp(int* outWidth, int* outHeight);
// Сервисы на основе JNI (защищены: каждый из них деградирует до no-op/лога).
void AndroidPlatformVibrate(unsigned int milliseconds);
void AndroidSetKeepScreenOn(bool enabled);
void AndroidSetSoftKeyboardVisible(bool visible);
void AndroidClipboardSetText(const char* utf8);
// Записывает буфер обмена как NUL-терминированный UTF-8 в `out` (всегда валиден).
void AndroidClipboardGetText(char* out, int cap);
// ANativeActivity_setWindowFlags(AWINDOW_FLAG_FULLSCREEN / FORCE_NOT_FULLSCREEN).
void AndroidSetFullscreen(bool fullscreen);

// --- реализовано в PlatformAndroid.cpp ------------------------------------
// Резолвер на основе eglGetProcAddress, общий для Window::GLGetProcAddress()
// и PlatformGLGetProcAddress().
void* AndroidGLGetProcAddress(const char* name);

}  // namespace crossrender

#endif  // ENG_PLATFORM_ANDROID
