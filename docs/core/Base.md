# crossrender/core/Base.h — базовые определения

Фундаментальный заголовок движка: определяет платформу, целочисленные типы,
макросы экспорта, математические константы и вспомогательные шаблоны, на
которых построен весь остальной код.

## Заголовок

```cpp
#include "crossrender/core/Base.h"
```

## Обзор

`Base.h` не содержит классов с состоянием — только определения времени
компиляции и несколько маленьких шаблонов. Его подключает практически каждый
другой заголовок движка, поэтому он намеренно зависит только от стандартной
библиотеки.

Порядок работы с заголовком:

1. На этапе компиляции выбирается платформа (`ENG_PLATFORM_*`) и архитектура
   (`ENG_ARCH_*`) — от этого зависит, например, доступность оконных бэкендов.
2. Всё, что нужно коду, использует типы из пространства имён `crossrender`
   (`crossrender::u8`, `crossrender::f32`, …) вместо `stdint.h`, чтобы код одинаково выглядел
   на всех платформах.
3. Для «сделать что-то при выходе из области видимости» используется
   `OnScopeExit` / `ENG_DEFER`.

## Макросы определения платформы

| Макрос | Когда определён | Значение |
|---|---|---|
| `ENG_PLATFORM_WASM` | сборка под Emscripten | WebGL 2 / GLES 3 |
| `ENG_PLATFORM_WINDOWS` | `_WIN32` / `_WIN64` | Win32 + WGL |
| `ENG_PLATFORM_ANDROID` | `__ANDROID__` | GLES 3 + EGL |
| `ENG_PLATFORM_IOS` | Apple + `TARGET_OS_IPHONE` | GLES 3 + EAGL |
| `ENG_PLATFORM_MACOS` | Apple, не iOS | OpenGL 3.3 core + CGL |
| `ENG_PLATFORM_LINUX` | `__linux__` | GLX |
| `ENG_GLES` | `1`, если доступен только GLES | переключает преамбулу шейдеров |
| `ENG_ARCH_ARM64`, `ENG_ARCH_X64`, `ENG_ARCH_UNKNOWN` | по архитектуре | для выбора SIMD |
| `ENG_COMPILER_MSVC`, `ENG_COMPILER_GCC` | по компилятору | `ENG_DEBUG_BREAK()` |
| `ENG_API` | при `ENG_BUILD_SHARED` | видимость символов |

Неподдерживаемая платформа приводит к `#error` на этапе компиляции — это
сделано специально, чтобы порт не «собирался молча» без оконного бэкенда.

```cpp
// Разный код для настольных и мобильных платформ.
#if defined(ENG_PLATFORM_IOS) || defined(ENG_PLATFORM_ANDROID)
    ENG_LOGI("demo", "мобильная сборка: используем тач-ввод");
#else
    ENG_LOGI("demo", "настольная сборка: мышь и клавиатура");
#endif
```

## Члены класса

### `using i8 / i16 / i32 / i64 / u8 / u16 / u32 / u64`

Знаковые и беззнаковые целые фиксированной ширины. Псевдонимы над
`std::int8_t` и т. д., поэтому ширина гарантирована на всех платформах.

```cpp
crossrender::u8  small  = 200;      // байт
crossrender::i32 index  = -1;
crossrender::u64 hashed = 0x9E3779B97F4A7C15ull;
```

### `using f32 / f64`

`f32` — это `float`, `f64` — `double`. Движок использует `f32` для графики и
`f64` для времени и накоплений.

```cpp
crossrender::f32 alpha = 0.5f;
crossrender::f64 now   = 12345.678;
```

### `using usize`

Беззнаковый размер — псевдоним `std::size_t`. Применяется для индексов и
размеров контейнеров, избавляя от предупреждений о сужении типа.

```cpp
std::vector<int> values{1, 2, 3};
for (crossrender::usize i = 0; i < values.size(); ++i) ENG_LOGI("demo", "%d", values[i]);
```

### `constexpr f32 kPi`

Число π с одинарной точностью.

```cpp
crossrender::f32 halfTurn = crossrender::kPi;         // 180 градусов
```

### `constexpr f32 kTau`

Полный оборот (2π). Удобно для углов, чтобы не писать `2 * kPi`.

```cpp
// Полный круг за 3 секунды.
crossrender::f32 angle = crossrender::kTau * (elapsed / 3.0f);
```

### `constexpr f32 kDeg2Rad`

Множитель перевода градусов в радианы.

```cpp
crossrender::f32 rad = 45.0f * crossrender::kDeg2Rad;   // 0.785...
```

### `constexpr f32 kRad2Deg`

Множитель перевода радиан в градусы.

```cpp
ENG_LOGI("demo", "угол камеры: %.1f градусов", yaw * crossrender::kRad2Deg);
```

### `constexpr f32 kEpsilon`

Малое число для сравнения вещественных значений.

```cpp
if (std::fabs(a - b) < crossrender::kEpsilon) ENG_LOGI("demo", "значения совпали");
```

### `[[nodiscard]] inline f32 Radians(f32 deg)`

Переводит градусы в радианы. Эквивалент `deg * kDeg2Rad`, но короче читается.

```cpp
crossrender::Camera cam;
cam.fovY = crossrender::Radians(60.0f);
```

### `[[nodiscard]] inline f32 Degrees(f32 rad)`

Переводит радианы в градусы — обычно для вывода в интерфейсе.

```cpp
ui.Label(crossrender::Fmt("%.0f°", crossrender::Degrees(camera.yaw)), rect);
```

### `struct NonCopyable`

Примесь, запрещающая копирование. Наследуйтесь от неё, когда у класса есть
владение ресурсом (файл, контекст OpenGL, поток).

```cpp
class MyDevice : public crossrender::NonCopyable {
public:
    MyDevice() = default;
};

MyDevice a;
// MyDevice b = a;   // ошибка компиляции — так и задумано
```

### `template <typename F> struct ScopeExit`

Объект, вызывающий переданную функцию в деструкторе. Обычно создаётся не
напрямую, а через `OnScopeExit` или `ENG_DEFER`.

```cpp
{
    crossrender::ScopeExit guard([&] { ENG_LOGI("demo", "выходим из области видимости"); });
    ENG_LOGI("demo", "внутри области видимости");
}   // здесь сработает guard
```

### `template <typename F> [[nodiscard]] ScopeExit<F> OnScopeExit(F f)`

Создаёт `ScopeExit`. Удобно для восстановления состояния, которое нужно
вернуть в любом случае — даже при раннем выходе.

```cpp
void DrawWithScissor(crossrender::Renderer2D& r2d, const crossrender::Rect& clip) {
    r2d.Save();
    auto restore = crossrender::OnScopeExit([&] { r2d.Restore(); });

    r2d.ClipRect(clip.x, clip.y, clip.w, clip.h);
    r2d.FillRect(clip, crossrender::Color::Red);
}   // Restore() вызовется автоматически
```

### `ENG_DEFER(code)`

Макрос-обёртка над `OnScopeExit`: выполняет `code` при выходе из текущей
области видимости. Имя переменной генерируется из номера строки, поэтому в
одной области видимости можно писать несколько `ENG_DEFER`.

```cpp
void UploadTexture(crossrender::Texture& tex) {
    tex.Bind(0);
    ENG_DEFER(tex.Unbind(0));          // отвяжем юнит при выходе
    ENG_DEFER(ENG_LOGI("demo", "загрузка завершена"));

    ENG_LOGI("demo", "загружаем текстуру %dx%d", tex.Width(), tex.Height());
}
```

### `ENG_DEBUG_BREAK()`

Останавливает отладчик на текущей строке (`__debugbreak()` в MSVC,
`__builtin_trap()` в Clang/GCC, no-op у остальных). Используется внутри
`ENG_ASSERT` и при отладке.

```cpp
if (gpuInfo.maxTextureSize == 0) {
    ENG_LOGE("gl", "драйвер сообщил maxTextureSize = 0");
    ENG_DEBUG_BREAK();
}
```

## Пример целиком

```cpp
#include "crossrender/core/Base.h"
#include "crossrender/core/Log.h"

#include <vector>

// Небольшая утилита: считает статистику по массиву углов, заданных в градусах.
struct AngleStats {
    crossrender::f32 meanDegrees = 0.0f;
    crossrender::f32 spreadRadians = 0.0f;
};

AngleStats Analyse(const std::vector<crossrender::f32>& degrees) {
    AngleStats out;
    if (degrees.empty()) return out;

    crossrender::f64 sum = 0.0;                       // f64 для накопления
    for (crossrender::usize i = 0; i < degrees.size(); ++i) sum += degrees[i];
    out.meanDegrees = static_cast<crossrender::f32>(sum / degrees.size());

    crossrender::f32 minRad = crossrender::Radians(degrees[0]);
    crossrender::f32 maxRad = minRad;
    for (crossrender::f32 d : degrees) {
        const crossrender::f32 r = crossrender::Radians(d);
        minRad = (r < minRad) ? r : minRad;
        maxRad = (r > maxRad) ? r : maxRad;
    }
    out.spreadRadians = maxRad - minRad;
    return out;
}

void Demo() {
    auto done = crossrender::OnScopeExit([] { ENG_LOGI("demo", "анализ завершён"); });
    ENG_DEFER(ENG_LOGI("demo", "счётчик вызовов увеличен"));

    const std::vector<crossrender::f32> angles{0.0f, 45.0f, 90.0f, 180.0f};
    const AngleStats stats = Analyse(angles);

    ENG_LOGI("demo", "средний угол %.1f°, разброс %.3f рад (%.1f°)",
             stats.meanDegrees, stats.spreadRadians, crossrender::Degrees(stats.spreadRadians));
}
```

## См. также

* `docs/core/Log.md` — макросы логирования, которые используются во всех
  примерах.
* `docs/core/Math.md` — векторная математика, построенная на типах отсюда.
* `docs/core/Time.md` — время и `crossrender::f64`.
