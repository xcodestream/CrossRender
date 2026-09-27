//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: лёгкий встроенный фреймворк модульных тестов без внешних зависимостей.
//
#pragma once

#include "crossrender/core/Log.h"
#include "crossrender/core/Base.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include <functional>

namespace crossrender {
namespace test {

struct TestCase {
    const char* suite = "";
    const char* name = "";
    void (*fn)() = nullptr;
    const char* file = "";
    int line = 0;
};

// Доступ к реестру (синглтон Мейерса, безопасен между единицами трансляции).
std::vector<TestCase>& Registry();

struct Registrar {
    Registrar(const char* suite, const char* name, void (*fn)(), const char* file, int line) {
        Registry().push_back({suite, name, fn, file, line});
    }
};

// Сообщение об ошибках. Бросает TestFailure, чтобы раннер мог продолжить.
struct TestFailure {
    std::string message;
};

void ReportFailure(const char* file, int line, const std::string& message);
void ReportSkip(const std::string& reason);
// Вызывается хинт-макросами движка, чтобы отметить, что тесту нужен GL-контекст.
void RequireGLContext(const char* file, int line);

// Запускает все зарегистрированные тесты. `filter` сравнивается с подстроками "Suite" или "Suite.Case".
struct RunResult {
    int passed = 0;
    int failed = 0;
    int skipped = 0;
    double seconds = 0;
    std::vector<std::string> failures;
};
RunResult RunAll(const std::string& filter = "", bool verbose = false);

// Помощник фикстуры тестов: создаёт скрытое окно и GL-контекст для GPU-тестов.
// Возвращает false, когда контекст недоступен; в этом случае тест должен
// сообщить о пропуске.
bool EnsureGLContext();
void ReleaseGLContext();

// Помощники, используемые несколькими тестами.
std::string TempFilePath(const char* name);
f32 RandomFloat01(u32 seed);

#define ENG_TEST(suite, name)                                                        \
    static void eng_test_##suite##_##name();                                         \
    static ::crossrender::test::Registrar eng_test_reg_##suite##_##name(                     \
        #suite, #name, &eng_test_##suite##_##name, __FILE__, __LINE__);              \
    static void eng_test_##suite##_##name()

#define ENG_TEST_FAIL(msg) ::crossrender::test::ReportFailure(__FILE__, __LINE__, (msg))
#define ENG_CHECK(cond)                                                              \
    do {                                                                             \
        if (!(cond)) ::crossrender::test::ReportFailure(__FILE__, __LINE__, "check failed: " #cond); \
    } while (0)

#define ENG_CHECK_MSG(cond, msg)                                                     \
    do {                                                                             \
        if (!(cond))                                                                 \
            ::crossrender::test::ReportFailure(__FILE__, __LINE__,                            \
                                       std::string("check failed: " #cond " - ") + (msg)); \
    } while (0)

#define ENG_CHECK_EQ(a, b)                                                           \
    do {                                                                             \
        if (!((a) == (b))) {                                                         \
            ::crossrender::test::ReportFailure(__FILE__, __LINE__,                            \
                                       std::string("expected " #a " == " #b));        \
        }                                                                            \
    } while (0)

#define ENG_CHECK_NEAR(a, b, eps)                                                    \
    do {                                                                             \
        double _va = static_cast<double>(a);                                          \
        double _vb = static_cast<double>(b);                                          \
        if (std::fabs(_va - _vb) > static_cast<double>(eps)) {                        \
            ::crossrender::test::ReportFailure(__FILE__, __LINE__,                            \
                                       std::string("expected " #a " ~= " #b " (got ") +  \
                                           std::to_string(_va) + " vs " + std::to_string(_vb) + ")"); \
        }                                                                            \
    } while (0)

#define ENG_CHECK_STR_EQ(a, b)                                                       \
    do {                                                                             \
        std::string _va = (a);                                                        \
        std::string _vb = (b);                                                        \
        if (_va != _vb) {                                                            \
            ::crossrender::test::ReportFailure(__FILE__, __LINE__,                            \
                                       std::string("expected \"") + _va + "\" == \"" + _vb + "\""); \
        }                                                                            \
    } while (0)

#define ENG_CHECK_GT(a, b)                                                           \
    do {                                                                             \
        auto _va = (a);                                                              \
        auto _vb = (b);                                                              \
        if (!(_va > _vb))                                                            \
            ::crossrender::test::ReportFailure(__FILE__, __LINE__, "expected " #a " > " #b);  \
    } while (0)

#define ENG_CHECK_LT(a, b)                                                          \
    do {                                                                             \
        auto _va = (a);                                                              \
        auto _vb = (b);                                                              \
        if (!(_va < _vb))                                                            \
            ::crossrender::test::ReportFailure(__FILE__, __LINE__, "expected " #a " < " #b);  \
    } while (0)

#define ENG_CHECK_LE(a, b)                                                           \
    do {                                                                             \
        auto _va = (a);                                                              \
        auto _vb = (b);                                                              \
        if (!(_va <= _vb))                                                           \
            ::crossrender::test::ReportFailure(__FILE__, __LINE__, "expected " #a " <= " #b); \
    } while (0)

#define ENG_CHECK_GE(a, b)                                                           \
    do {                                                                             \
        auto _va = (a);                                                              \
        auto _vb = (b);                                                              \
        if (!(_va >= _vb))                                                           \
            ::crossrender::test::ReportFailure(__FILE__, __LINE__, "expected " #a " >= " #b); \
    } while (0)

#define ENG_SKIP(reason)                                                             \
    do {                                                                             \
        ::crossrender::test::ReportSkip(reason);                                              \
        return;                                                                      \
    } while (0)

#define ENG_REQUIRE_GL()                                                             \
    do {                                                                             \
        if (!::crossrender::test::EnsureGLContext()) ENG_SKIP("no GL context available");      \
    } while (0)

}  // namespace test
}  // namespace crossrender
