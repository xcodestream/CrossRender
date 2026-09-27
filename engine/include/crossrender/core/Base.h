//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: базовые определения движка: платформа, целочисленные типы и общие вспомогательные средства.
//
#pragma once

#include <string>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>

// ---------------------------------------------------------------------------
// Определение платформы / архитектуры
// ---------------------------------------------------------------------------
#if defined(__EMSCRIPTEN__)
#  define ENG_PLATFORM_WASM 1
#  define ENG_GLES 1
#elif defined(_WIN32) || defined(_WIN64)
#  define ENG_PLATFORM_WINDOWS 1
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#elif defined(__ANDROID__)
#  define ENG_PLATFORM_ANDROID 1
#  define ENG_GLES 1
#elif defined(__APPLE__)
#  include <TargetConditionals.h>
#  if TARGET_OS_IPHONE
#    define ENG_PLATFORM_IOS 1
#    define ENG_GLES 1
#  else
#    define ENG_PLATFORM_MACOS 1
#  endif
#elif defined(__linux__)
#  define ENG_PLATFORM_LINUX 1
#else
#  error "CrossRender: unsupported platform"
#endif

#if !defined(ENG_GLES)
#  define ENG_GLES 0
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
#  define ENG_ARCH_ARM64 1
#elif defined(__x86_64__) || defined(_M_X64)
#  define ENG_ARCH_X64 1
#else
#  define ENG_ARCH_UNKNOWN 1
#endif

#if defined(_MSC_VER)
#  define ENG_COMPILER_MSVC 1
#  define ENG_DEBUG_BREAK() __debugbreak()
#elif defined(__clang__) || defined(__GNUC__)
#  define ENG_COMPILER_GCC 1
#  define ENG_DEBUG_BREAK() __builtin_trap()
#else
#  define ENG_DEBUG_BREAK() ((void)0)
#endif

#if defined(ENG_PLATFORM_WINDOWS)
#  ifdef ENG_BUILD_SHARED
#    define ENG_API __declspec(dllexport)
#  else
#    define ENG_API
#  endif
#else
#  ifdef ENG_BUILD_SHARED
#    define ENG_API __attribute__((visibility("default")))
#  else
#    define ENG_API
#  endif
#endif

// ---------------------------------------------------------------------------
// Базовые типы
// ---------------------------------------------------------------------------
namespace crossrender {

using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using f32 = float;
using f64 = double;
using usize = std::size_t;

constexpr f32 kPi = 3.14159265358979323846f;
constexpr f32 kTau = 6.28318530717958647692f;
constexpr f32 kDeg2Rad = kPi / 180.0f;
constexpr f32 kRad2Deg = 180.0f / kPi;
constexpr f32 kEpsilon = 1e-6f;

[[nodiscard]] inline f32 Radians(f32 deg) { return deg * kDeg2Rad; }
[[nodiscard]] inline f32 Degrees(f32 rad) { return rad * kRad2Deg; }

// Миксины, запрещающие копирование / перемещение.
struct NonCopyable {
    NonCopyable() = default;
    NonCopyable(const NonCopyable&) = delete;
    NonCopyable& operator=(const NonCopyable&) = delete;
};

// Scope guard: вызывает callable при выходе из области видимости.
template <typename F>
struct ScopeExit {
    F fn;
    explicit ScopeExit(F f) : fn(std::move(f)) {}
    ~ScopeExit() { fn(); }
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
};

template <typename F>
[[nodiscard]] ScopeExit<F> OnScopeExit(F f) {
    return ScopeExit<F>(std::move(f));
}

#define ENG_CONCAT_IMPL(a, b) a##b
#define ENG_CONCAT(a, b) ENG_CONCAT_IMPL(a, b)
#define ENG_DEFER(code) auto ENG_CONCAT(_eng_defer_, __LINE__) = ::crossrender::OnScopeExit([&]() { code; })

}  // namespace crossrender
