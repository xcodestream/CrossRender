//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: сервисы платформы: диалоги, системные пути, метрики экрана и жизненный цикл приложения.
//
#pragma once

#include "crossrender/core/Base.h"

#include <string>
#include <vector>

namespace crossrender {

enum class MessageBoxType { Info, Warning, Error, Question };

// Показывает модальное окно с сообщением (блокирующе). На платформах без нативных
// диалогов (WASM/Android) пишет в лог, а для WASM использует JS alert.
void ShowMessageBox(const std::string& title, const std::string& message,
                    MessageBoxType type = MessageBoxType::Info);

// Нативные файловые диалоги. Возвращает пустую строку при отмене или отсутствии поддержки.
std::string OpenFileDialog(const std::string& title, const std::string& filter);
std::string SaveFileDialog(const std::string& title, const std::string& defaultName);

// Открывает URL в браузере по умолчанию.
bool OpenUrl(const std::string& url);

// Метрики экрана в пунктах.
void GetScreenSize(int* w, int* h);

// Информация о процессе / системе.
std::string PlatformName();          // "macOS", "Windows", "Linux", "iOS", "Android", "Web"
std::string PlatformArch();          // "arm64", "x86_64"
std::string CpuName();
u64 TotalPhysicalMemory();
int CpuCoreCount();
std::string ExecutablePath();
std::string ExecutableDir();

// Метрики процесса для отладочного оверлея (на WASM возвращают 0).
usize ProcessResidentBytes();  // резидентная память процесса, байт
f64 ProcessCpuSeconds();       // суммарное процессорное время процесса, секунд

// Приостанавливает вызывающий поток.
void SleepMs(u32 milliseconds);

// Идентификатор текущего потока (для диагностики системы задач).
u64 CurrentThreadId();

// Высокоточный таймер в секундах.
f64 HighResTimerSeconds();

// Включает/выключает экранную программную клавиатуру (мобильные платформы).
void SetSoftKeyboardVisible(bool visible);

// Просит платформу не давать экрану гаснуть (мобильные / веб).
void SetKeepScreenAwake(bool enabled);

// Виброотклик, no-op там, где не поддерживается.
void Vibrate(u32 milliseconds);

// Находится ли приложение сейчас на переднем плане.
bool IsAppForeground();

// ---------------------------------------------------------------------------
// Жизненный цикл приложения. На десктопе RunApp блокирует до закрытия окна. На
// iOS/Android/WASM платформа движет кадрами из цикла ОС.
// ---------------------------------------------------------------------------
struct AppHooks {
    // Вызывается один раз после создания окна и GL-контекста.
    void (*onInit)(void* user) = nullptr;
    // Вызывается один раз за кадр. Верните false, чтобы запросить выход.
    bool (*onFrame)(void* user, f32 dt) = nullptr;
    // Вызывается при потере/восстановлении GL-контекста (Android, WASM).
    void (*onContextLost)(void* user) = nullptr;
    void (*onContextRestored)(void* user) = nullptr;
    // Вызывается перед завершением работы.
    void (*onShutdown)(void* user) = nullptr;
    // Вызывается при предупреждениях о нехватке памяти.
    void (*onLowMemory)(void* user) = nullptr;
    void* user = nullptr;
};

// Запускает главный цикл приложения с данными хуками. На десктопе блокирует.
int RunApp(const AppHooks& hooks);

// ---------------------------------------------------------------------------
// Headless GL-контекст (внеэкранный). Используется тестами, CI и режимом `--headless`.
// Реализуется в папке каждой платформы; возвращает false, если недоступен.
// ---------------------------------------------------------------------------
bool CreateHeadlessGLContext();
void DestroyHeadlessGLContext();
bool HasHeadlessGLContext();
void* HeadlessGLGetProcAddress(const char* name);

}  // namespace crossrender
