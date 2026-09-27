#include "crossrender/test/Test.h"

#include "crossrender/gfx/GL.h"
#include "crossrender/core/File.h"
#include "crossrender/core/Math.h"
#include "crossrender/core/Time.h"
#include "crossrender/platform/Platform.h"

#include <cstdio>
#include <cstring>
#include <algorithm>

namespace crossrender {
namespace test {

std::vector<TestCase>& Registry() {
    static std::vector<TestCase> registry;
    return registry;
}

namespace {
bool g_skipped = false;
std::string g_skipReason;
int g_glRefCount = 0;
}  // namespace

void ReportFailure(const char* file, int line, const std::string& message) {
    char buf[2048];
    std::snprintf(buf, sizeof(buf), "%s:%d: %s", file, line, message.c_str());
    throw TestFailure{buf};
}

void ReportSkip(const std::string& reason) {
    g_skipped = true;
    g_skipReason = reason;
}

void RequireGLContext(const char* file, int line) {
    if (!EnsureGLContext()) {
        ReportSkip("no GL context");
        throw TestFailure{std::string(file) + ":" + std::to_string(line) + ": skipped"};
    }
}

bool EnsureGLContext() {
    if (HasHeadlessGLContext()) {
        ++g_glRefCount;
        return true;
    }
    if (!CreateHeadlessGLContext()) return false;
    if (!gl::LoadFunctions(HeadlessGLGetProcAddress)) {
        DestroyHeadlessGLContext();
        return false;
    }
    ++g_glRefCount;
    return true;
}

void ReleaseGLContext() {
    if (g_glRefCount > 0) --g_glRefCount;
    if (g_glRefCount == 0) DestroyHeadlessGLContext();
}

std::string TempFilePath(const char* name) {
    // Предпочитаем системный временный каталог: он доступен на запись, даже когда
    // каталог пользовательских данных изолирован, что позволяет гонять тесты в CI.
    std::string base;
#if defined(ENG_PLATFORM_WINDOWS)
    const char* tmp = std::getenv("TEMP");
    if (!tmp) tmp = std::getenv("TMP");
    if (!tmp) tmp = ".";
    base = tmp;
#else
    const char* tmp = std::getenv("TMPDIR");
    base = tmp ? tmp : "/tmp";
#endif
    std::string dir = PathJoin(base, "gameengine-tests");
    FS().WriteFile(PathJoin(dir, ".keep"), "", 0);
    return PathJoin(dir, name);
}

f32 RandomFloat01(u32 seed) {
    Random r(seed);
    return r.NextFloat();
}

RunResult RunAll(const std::string& filter, bool verbose) {
    RunResult result;
    auto& registry = Registry();
    std::stable_sort(registry.begin(), registry.end(), [](const TestCase& a, const TestCase& b) {
        int c = std::strcmp(a.suite, b.suite);
        return c != 0 ? c < 0 : std::strcmp(a.name, b.name) < 0;
    });

    std::printf("\n=== CrossRender test suite (%zu cases) ===\n", registry.size());

    // Держим ОДИН GL-контекст живым на весь прогон. Тесты, которым нужен контекст,
    // вызывают ENG_REQUIRE_GL(), увеличивающий счётчик ссылок; создание и удаление
    // контекста на каждый тест сделало бы недействительными все кэшированные
    // GPU-ресурсы (атласы шрифтов, текстуры), удерживаемые function-local statics
    // между тестами.
    EnsureGLContext();
    f64 start = NowSeconds();
    const char* currentSuite = nullptr;

    for (const TestCase& tc : registry) {
        std::string full = std::string(tc.suite) + "." + tc.name;
        if (!filter.empty() && full.find(filter) == std::string::npos) continue;
        if (!currentSuite || std::strcmp(currentSuite, tc.suite) != 0) {
            currentSuite = tc.suite;
            std::printf("\n[%s]\n", currentSuite);
        }
        g_skipped = false;
        g_skipReason.clear();
        const int refBefore = g_glRefCount;
        f64 t0 = NowSeconds();
        bool failed = false;
        std::string failureMessage;
        try {
            tc.fn();
        } catch (const TestFailure& f) {
            failed = !g_skipped;
            failureMessage = f.message;
        } catch (const std::exception& e) {
            failed = true;
            failureMessage = std::string("unhandled std::exception: ") + e.what();
        } catch (...) {
            failed = true;
            failureMessage = "unhandled non-standard exception";
        }
        f64 dt = (NowSeconds() - t0) * 1000.0;

        if (g_skipped && !failed) {
            ++result.skipped;
            std::printf("  - %-40s skipped (%s)\n", tc.name, g_skipReason.c_str());
        } else if (failed) {
            ++result.failed;
            result.failures.push_back(full + ": " + failureMessage);
            std::printf("  x %-40s FAILED  %s\n", tc.name, failureMessage.c_str());
        } else {
            ++result.passed;
            if (verbose)
                std::printf("  + %-40s ok (%.1f ms)\n", tc.name, dt);
            else
                std::printf("  + %s\n", tc.name);
        }
        // Снимаем ровно те ссылки, что взял тест. Безусловное освобождение раньше
        // съедало собственную ссылку раннера на первом же тесте без GL, уничтожая
        // общий контекст и пересоздавая его (с новым предупреждением LoadFunctions
        // и свежим пустым кэшем GPU-ресурсов) для каждого последующего GL-теста.
        while (g_glRefCount > refBefore) ReleaseGLContext();

        // Восстанавливаем инвариант, на который опирается остальная сюита: один
        // живой загруженный контекст, *актуальный для этого потока*.
        //
        // Тест может сломать его двумя способами. Создание Engine делает свой
        // headless-контекст и закрывает его, утягивая с собой контекст раннера.
        // А создание настоящего окна делает его контекст текущим, так что после
        // уничтожения окна у потока вовсе не остаётся текущего контекста, хотя
        // headless-объект ещё существует - это проявляется как null glGetString
        // и падение в следующем GPU-тесте.
        if (g_glRefCount > 0) {
            if (!HasHeadlessGLContext()) {
                if (CreateHeadlessGLContext() && !gl::glCreateShader) {
                    gl::LoadFunctions(HeadlessGLGetProcAddress);
                }
            } else {
                // CreateHeadlessGLContext() перепривязывает существующий контекст к
                // вызывающему потоку; уравновешиваем взятую им ссылку.
                if (CreateHeadlessGLContext()) DestroyHeadlessGLContext();
                if (!gl::glCreateShader) gl::LoadFunctions(HeadlessGLGetProcAddress);
            }
            // glGetError() - очередь, а не флаг: оставленная тестом ошибка заставила бы
            // проверку "нет ошибок GL" следующего теста упасть за то, чего он не делал.
            // Начинаем каждый тест с чистой очереди.
            if (gl::glGetError) {
                int guard = 0;
                while (gl::glGetError() != 0 && ++guard < 64) {
                }
            }
        }
    }
    ReleaseGLContext();  // уравновешивает EnsureGLContext() выше
    result.seconds = NowSeconds() - start;
    std::printf("\n-------------------------------------------\n");
    std::printf("passed %d, failed %d, skipped %d in %.2fs\n", result.passed, result.failed,
                result.skipped, result.seconds);
    if (!result.failures.empty()) {
        std::printf("\nFailures:\n");
        for (const std::string& f : result.failures) std::printf("  %s\n", f.c_str());
    }
    std::printf("===========================================\n\n");
    return result;
}

}  // namespace test
}  // namespace crossrender

// ---------------------------------------------------------------------------
// Точка входа тест-раннера
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    std::string filter;
    std::string logLevel = "error";
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-v" || arg == "--verbose")
            verbose = true;
        else if (arg == "--loglevel")
            logLevel = argv[++i];
        else if (arg == "--list") {
            for (const auto& tc : crossrender::test::Registry())
                std::printf("%s.%s\n", tc.suite, tc.name);
            return 0;
        } else if (arg.rfind("--filter=", 0) == 0)
            filter = arg.substr(9);
        else if (arg[0] != '-')
            filter = arg;
    }
    // Держим отчёт читабельным: логи движка шумят во время GPU-тестов.
    if (logLevel == "trace")
        crossrender::LogSetLevel(crossrender::LogLevel::Trace);
    else if (logLevel == "debug")
        crossrender::LogSetLevel(crossrender::LogLevel::Debug);
    else if (logLevel == "info")
        crossrender::LogSetLevel(crossrender::LogLevel::Info);
    else if (logLevel == "warn")
        crossrender::LogSetLevel(crossrender::LogLevel::Warn);
    else if (logLevel == "off")
        crossrender::LogSetLevel(crossrender::LogLevel::Off);
    else
        crossrender::LogSetLevel(crossrender::LogLevel::Error);
    if (verbose) crossrender::LogSetLevel(crossrender::LogLevel::Info);
    crossrender::test::RunResult r = crossrender::test::RunAll(filter, verbose);
    return r.failed == 0 ? 0 : 1;
}
