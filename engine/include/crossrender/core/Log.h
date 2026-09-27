//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: логирование: уровни важности, категории, приёмники вывода, проверки и фатальные ошибки.
//
#pragma once

#include "crossrender/core/Base.h"

#include <string>
#include <vector>

namespace crossrender {

enum class LogLevel : int { Trace = 0, Debug, Info, Warn, Error, Fatal, Off };

using LogSink = void (*)(LogLevel level, const char* category, const char* message, void* user);

// Глобальная конфигурация логгера (потокобезопасная).
void LogSetLevel(LogLevel level);
LogLevel LogGetLevel();
void LogAddSink(LogSink sink, void* user);
void LogClearSinks();

void LogWrite(LogLevel level, const char* category, const char* fmt, ...)
#if defined(__clang__) || defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

// Возвращает последние N отформатированных строк лога (используется экранной консолью и тестами).
std::vector<std::string> LogHistory();
void LogEnableHistory(bool enable);

// Fatal: запись в лог + abort. Для неустранимых ошибок движка.
[[noreturn]] void LogFatal(const char* category, const char* fmt, ...);

#define ENG_TRACE(...) ::crossrender::LogWrite(::crossrender::LogLevel::Trace, "crossrender", __VA_ARGS__)
#define ENG_DEBUG(...) ::crossrender::LogWrite(::crossrender::LogLevel::Debug, "crossrender", __VA_ARGS__)
#define ENG_INFO(...) ::crossrender::LogWrite(::crossrender::LogLevel::Info, "crossrender", __VA_ARGS__)
#define ENG_WARN(...) ::crossrender::LogWrite(::crossrender::LogLevel::Warn, "crossrender", __VA_ARGS__)
#define ENG_ERROR(...) ::crossrender::LogWrite(::crossrender::LogLevel::Error, "crossrender", __VA_ARGS__)
#define ENG_FATAL(...) ::crossrender::LogFatal("crossrender", __VA_ARGS__)

#define ENG_LOGT(cat, ...) ::crossrender::LogWrite(::crossrender::LogLevel::Trace, cat, __VA_ARGS__)
#define ENG_LOGD(cat, ...) ::crossrender::LogWrite(::crossrender::LogLevel::Debug, cat, __VA_ARGS__)
#define ENG_LOGI(cat, ...) ::crossrender::LogWrite(::crossrender::LogLevel::Info, cat, __VA_ARGS__)
#define ENG_LOGW(cat, ...) ::crossrender::LogWrite(::crossrender::LogLevel::Warn, cat, __VA_ARGS__)
#define ENG_LOGE(cat, ...) ::crossrender::LogWrite(::crossrender::LogLevel::Error, cat, __VA_ARGS__)

#if defined(NDEBUG)
#  define ENG_ASSERT(cond) ((void)0)
#  define ENG_ASSERT_MSG(cond, ...) ((void)0)
#else
#  define ENG_ASSERT(cond)                                                      \
      do {                                                                      \
          if (!(cond)) {                                                        \
              ::crossrender::LogWrite(::crossrender::LogLevel::Fatal, "assert",                 \
                              "%s:%d: assertion failed: %s", __FILE__, __LINE__, #cond); \
              ENG_DEBUG_BREAK();                                                \
          }                                                                     \
      } while (0)
#  define ENG_ASSERT_MSG(cond, ...)                                             \
      do {                                                                      \
          if (!(cond)) {                                                        \
              ::crossrender::LogWrite(::crossrender::LogLevel::Fatal, "assert",                 \
                              "%s:%d: assertion failed: %s", __FILE__, __LINE__, #cond); \
              ::crossrender::LogWrite(::crossrender::LogLevel::Fatal, "assert", __VA_ARGS__);   \
              ENG_DEBUG_BREAK();                                                \
          }                                                                     \
      } while (0)
#endif

// Проверяет, что вызов OpenGL прошёл успешно (только в debug-сборках, дёшево).
void GLCheckError(const char* file, int line, const char* expr);

}  // namespace crossrender
