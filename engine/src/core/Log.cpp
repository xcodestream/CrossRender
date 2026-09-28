#include "crossrender/core/Log.h"

#include "crossrender/core/Time.h"

#include <mutex>
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>
#include <cstdarg>

#include "crossrender/gfx/GL.h"

#if defined(ENG_METAL)
#include "MetalLoader.h"
#endif

namespace crossrender {
namespace {

std::mutex g_logMutex;
std::atomic<int> g_level{static_cast<int>(LogLevel::Info)};
std::vector<std::pair<LogSink, void*>> g_sinks;
std::vector<std::string> g_history;
bool g_historyEnabled = false;
constexpr usize kMaxHistory = 512;

const char* LevelName(LogLevel l) {
    switch (l) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO ";
        case LogLevel::Warn: return "WARN ";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
        default: return "?????";
    }
}

const char* LevelColor(LogLevel l) {
    switch (l) {
        case LogLevel::Trace: return "\x1b[90m";
        case LogLevel::Debug: return "\x1b[36m";
        case LogLevel::Info: return "\x1b[32m";
        case LogLevel::Warn: return "\x1b[33m";
        case LogLevel::Error: return "\x1b[31m";
        case LogLevel::Fatal: return "\x1b[35m";
        default: return "";
    }
}

}  // namespace

void LogSetLevel(LogLevel level) { g_level.store(static_cast<int>(level), std::memory_order_relaxed); }
LogLevel LogGetLevel() { return static_cast<LogLevel>(g_level.load(std::memory_order_relaxed)); }

void LogAddSink(LogSink sink, void* user) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_sinks.emplace_back(sink, user);
}

void LogClearSinks() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_sinks.clear();
}

void LogEnableHistory(bool enable) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_historyEnabled = enable;
    if (!enable) g_history.clear();
}

std::vector<std::string> LogHistory() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    return g_history;
}

void LogWrite(LogLevel level, const char* category, const char* fmt, ...) {
    if (static_cast<int>(level) < g_level.load(std::memory_order_relaxed)) return;
    if (level == LogLevel::Off) return;

    char buffer[4096];
    va_list args;
    va_start(args, fmt);
    int n = std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    if (n < 0) return;

    // Отрезаем завершающий перевод строки (вызывающий код иногда его включает).
    usize len = std::strlen(buffer);
    while (len > 0 && (buffer[len - 1] == '\n' || buffer[len - 1] == '\r')) buffer[--len] = '\0';

    std::vector<std::pair<LogSink, void*>> sinks;
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
    std::fprintf(stderr, "%s[%s] %-7s %s\x1b[0m\n", LevelColor(level), category ? category : "crossrender",
                 LevelName(level), buffer);
    std::fflush(stderr);

    if (g_historyEnabled) {
        g_history.push_back(std::string("[") + LevelName(level) + "] " + buffer);
        if (g_history.size() > kMaxHistory) g_history.erase(g_history.begin());
        }
        sinks = g_sinks;
    }
    for (auto& sink : sinks) {
        if (sink.first) sink.first(level, category, buffer, sink.second);
    }
}

void LogFatal(const char* category, const char* fmt, ...) {
    char buffer[4096];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    std::fprintf(stderr, "\x1b[35m[%s] FATAL   %s\x1b[0m\n", category ? category : "crossrender", buffer);
    std::fflush(stderr);

    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        for (auto& sink : g_sinks) {
            if (sink.first) sink.first(LogLevel::Fatal, category, buffer, sink.second);
        }
    }
    std::abort();
}

void GLCheckError(const char* file, int line, const char* expr) {
    (void)file;
    (void)line;
    (void)expr;
}

namespace gl {

void CheckErrorImpl(const char* file, int line, const char* expr) {
#if !defined(NDEBUG)
    if (!glGetError) return;
    GLenum err = glGetError();
    if (err == 0) return;
    // Вычищаем очередь, чтобы последующие проверки не были загрязнены.
    GLenum next = 0;
    int guard = 0;
    while ((next = glGetError()) != 0 && guard++ < 16) {
        // сохраняем первую ошибку
    }
    LogWrite(LogLevel::Error, "gl", "%s:%d: GL error %s (0x%04X) after: %s", file, line,
             ErrorString(err), err, expr);
#else
    (void)file;
    (void)line;
    (void)expr;
#endif
}

const char* ErrorString(GLenum err) {
    switch (err) {
        case GL_INVALID_ENUM: return "GL_INVALID_ENUM";
        case GL_INVALID_VALUE: return "GL_INVALID_VALUE";
        case GL_INVALID_OPERATION: return "GL_INVALID_OPERATION";
        case GL_OUT_OF_MEMORY: return "GL_OUT_OF_MEMORY";
        case GL_INVALID_FRAMEBUFFER_OPERATION: return "GL_INVALID_FRAMEBUFFER_OPERATION";
        case 0: return "GL_NO_ERROR";
        default: return "GL_UNKNOWN_ERROR";
    }
}

bool LoadFunctions(void* (*getProcAddress)(const char*)) {
#if defined(ENG_METAL)
    // Слой совместимости с Metal: каждая точка входа разрешается либо в
    // реализацию поверх Metal, либо в типизированную заглушку no-op (crossrender/metal).
    (void)getProcAddress;
#define ENG_GL_LOAD(ret, name, params) \
    name = reinterpret_cast<PFN_##name>(mtlgl::ResolveGLProc(#name));
    ENG_GL_FUNCS(ENG_GL_LOAD)
#undef ENG_GL_LOAD
    ENG_LOGI("gl", "GL entry points resolved through the Metal compatibility layer");
    return true;
#else
    if (!getProcAddress) {
        ENG_LOGE("gl", "LoadFunctions: null proc address resolver");
        return false;
    }
    int missing = 0;
    const char* firstMissing = nullptr;
#define ENG_GL_LOAD(ret, name, params)                                            \
    name = reinterpret_cast<PFN_##name>(getProcAddress(#name));                   \
    if (!name) {                                                                  \
        ++missing;                                                                \
        if (!firstMissing) firstMissing = #name;                                   \
    }
    ENG_GL_FUNCS(ENG_GL_LOAD)
#undef ENG_GL_LOAD

    if (missing > 0) {
        // Некоторые сборки GLES не экспортируют остатки ES 3.2 / десктопа;
        // сообщаем о них, но падаем только при отсутствии базового набора
        // (buffers/vao/shader/texture), на что указывает большое число сбоев.
        //
        // Набор доступных точек входа — свойство драйвера, а не конкретного
        // вызова, иначе приложение, многократно пересоздающее контекст,
        // повторяло бы эту же строку при каждой загрузке.
        static bool warned = false;
        if (!warned) {
            warned = true;
            ENG_LOGW("gl", "%d GL entry points unavailable (first: %s, "
                           "this warning is shown once per run)",
                     missing, firstMissing ? firstMissing : "?");
        }
        if (missing > 12) return false;
    }
    return true;
#endif  // ENG_METAL
}

void UnloadFunctions() {
#define ENG_GL_UNLOAD(ret, name, params) name = nullptr;
    ENG_GL_FUNCS(ENG_GL_UNLOAD)
#undef ENG_GL_UNLOAD
}

GpuInfo QueryGpuInfo() {
    GpuInfo info;
    if (!glGetString) return info;
    auto str = [](GLenum e) -> std::string {
        const GLubyte* s = glGetString(e);
        return s ? std::string(reinterpret_cast<const char*>(s)) : std::string();
    };
    info.vendor = str(GL_VENDOR);
    info.renderer = str(GL_RENDERER);
    info.version = str(GL_VERSION);
    info.glslVersion = str(GL_SHADING_LANGUAGE_VERSION);

    const char* v = info.version.c_str();
    while (*v && (*v < '0' || *v > '9')) ++v;
    info.major = std::atoi(v);
    const char* dot = std::strchr(v, '.');
    if (dot) info.minor = std::atoi(dot + 1);

    info.isGLES = info.version.find("OpenGL ES") != std::string::npos ||
                  info.version.find("OpenGL ES") != std::string::npos || ENG_GLES != 0;

    if (glGetIntegerv) {
        GLint val = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &val);
        info.maxTextureSize = val;
        glGetIntegerv(GL_MAX_SAMPLES, &val);
        info.maxSamples = val;
        glGetIntegerv(GL_MAX_DRAW_BUFFERS, &val);
        info.maxDrawBuffers = val;
        glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &val);
        info.maxVertexAttribs = val;
    }
    // Проверка расширений: GLES3 предоставляет glGetStringi с GL_NUM_EXTENSIONS.
    if (glGetStringi) {
        GLint count = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &count);
        for (GLint i = 0; i < count; ++i) {
            const char* ext = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, i));
            if (!ext) continue;
            std::string e(ext);
            if (e.find("anisotropic") != std::string::npos) info.hasAnisotropy = true;
            if (e.find("EXT_debug") != std::string::npos) info.hasDebugOutput = true;
        }
    }
    return info;
}

}  // namespace gl
}  // namespace crossrender
