# crossrender/gfx/GL.h — собственный загрузчик OpenGL

Заголовок объявляет переносимую поверхность OpenGL: типы, константы и все
точки входа (entry points) в пространстве имён `crossrender::gl`, а также загрузчик,
который заполняет указатели на функции конкретными адресами драйвера.

## Заголовок

```cpp
#include "crossrender/gfx/GL.h"
```

## Обзор

Движок намеренно **не подключает ни один системный GL-заголовок** и не
использует glad, GLEW или `gl3.h`. Вместо этого `GL.h` сам объявляет ровно тот
набор GL, который нужен рендерерам, в `namespace crossrender::gl`. Причины:

1. **Один заголовок на все платформы.** Один и тот же файл обслуживает
   настольный OpenGL 3.3 core (macOS, Windows, Linux) и OpenGL ES 3.0
   (iOS, Android, WASM). Различаются только типы оконного бэкенда, а не код
   рендеринга.
2. **Никаких конфликтов с `gl.h`.** Типы вроде `GLuint` живут в `crossrender::gl`, а не
   в глобальном пространстве имён, поэтому движок может собираться там, где
   одновременно доступны GLX/WGL/EGL и заголовки платформы.
3. **Совпадение значений.** Перечисления (`GL_TRIANGLES`, `GL_RGBA8`, …) — это
   `constexpr GLenum`, а не `#define`, и их значения одинаковы в GL 3.3 core и
   GLES 3.0, поэтому одна и та же константа работает на обеих ветках.

**Как устроен загрузчик.**

Все точки входа перечислены **одним макросом** `ENG_GL_FUNCS(X)`. Макрос
раскрывается дважды:

* в заголовке — в объявления `inline PFN_glXxx glXxx = nullptr;`, то есть
  `glXxx` это **переменная-указатель на функцию**, а не функция;
* в реализации — в вызовы резолвера, которые записывают в эти указатели
  настоящие адреса.

Сам загрузчик и его реализация (`LoadFunctions`, `UnloadFunctions`,
`QueryGpuInfo`, `CheckErrorImpl`, `ErrorString`) лежат не в `gfx/`, а в
`engine/src/core/Log.cpp` — исторически проверка GL-ошибок делит файл с
логгером, потому что пишет в него же. Заголовок при этом остаётся в
`crossrender/gfx/GL.h`.

Отсюда главное правило: **`crossrender::gl::glXxx(...)` можно вызывать только после
`gl::LoadFunctions(...)`**. До загрузки указатель равен `nullptr`, и вызов
превращается в обращение по нулевому адресу. Резолвер предоставляет
платформенный слой:

```cpp
// Порядок инициализации: контекст -> загрузчик -> рендереры.
if (!window.Create(1280, 720, "Demo")) ENG_FATAL("нет окна");
if (!crossrender::gl::LoadFunctions(window.GLGetProcAddress())) {
    ENG_FATAL("не удалось загрузить точки входа OpenGL");
}
crossrender::Renderer3D r3d;
if (!r3d.Init()) ENG_FATAL("Renderer3D не инициализировался");
```

Вызов синтаксически обычный — скобки после имени:

```cpp
crossrender::gl::glClearColor(0.05f, 0.06f, 0.08f, 1.0f);
crossrender::gl::glClear(crossrender::gl::GL_COLOR_BUFFER_BIT | crossrender::gl::GL_DEPTH_BUFFER_BIT);
```

**Преамбула шейдеров.**

Поскольку заголовок один, а GLSL на настольном GL и на GLES разный, движок
переключает не заголовок, а **преамбулу шейдеров**: `builtin::Preamble()`
возвращает `#version 300 es` с `precision`-квалификаторами, когда определён
`ENG_GLES`, и `#version 330 core` иначе. Оба варианта определяют макрос
`ENG_GLES` для самого GLSL, чтобы шейдерный код мог ветвиться. Подробности —
в `docs/gfx/Shader.md`.

**Чего в загрузчике нет.**

Список функций — это ровно то, что используют рендереры движка, а не весь
OpenGL. Здесь нет, например, тесселяции, геометрических шейдеров, `glMapBuffer`
и отладочного колбэка `glDebugMessageCallback`: их отсутствие — осознанное решение,
а не недоделка. Часть объявленных функций (например `glGetTexImage` или
`glDrawBuffer`) недоступна в GLES 3.0, поэтому после загрузки их указатели
могут остаться `nullptr`; вызывающий код обязан проверять указатель.

## Члены класса

### Типы (`GLenum`, `GLuint`, …)

Заголовок определяет полный набор типов OpenGL как псевдонимы над типами
фиксированной ширины. Используйте именно их: `unsigned int` в подписи
uniform-а выглядит правильно, но не документирует намерение.

| Тип | Псевдоним | Где встречается |
|---|---|---|
| `GLenum` | `unsigned int` | перечисления, режимы, форматы |
| `GLboolean` | `unsigned char` | флаги (`GL_TRUE` / `GL_FALSE`) |
| `GLbitfield` | `unsigned int` | маски `glClear` |
| `GLbyte`, `GLshort`, `GLint` | знаковые целые | размеры, координаты |
| `GLubyte`, `GLushort`, `GLuint` | беззнаковые целые | имена объектов |
| `GLsizei` | `int` | количества и размеры |
| `GLfloat`, `GLclampf` | `float` | цвета, координаты, uniform-ы |
| `GLdouble` | `double` | редкие double-вызовы |
| `GLchar` | `char` | исходники шейдеров и логи |
| `GLintptr`, `GLsizeiptr` | `std::intptr_t`, `std::ptrdiff_t` | смещения и размеры буферов |
| `GLint64`, `GLuint64` | `std::int64_t`, `std::uint64_t` | 64-битные результаты запросов |

```cpp
crossrender::gl::GLuint vao = 0;
crossrender::gl::glGenVertexArrays(1, &vao);          // имя объекта — GLuint
crossrender::gl::GLsizei stride = static_cast<crossrender::gl::GLsizei>(sizeof(crossrender::Vertex));
crossrender::gl::glVertexAttribPointer(0, 3, crossrender::gl::GL_FLOAT, crossrender::gl::GL_FALSE, stride, nullptr);
crossrender::gl::GLboolean depthOn = crossrender::gl::glIsEnabled(crossrender::gl::GL_DEPTH_TEST);
```

### Константы (`GLenum`)

Все перечисления объявлены как `constexpr GLenum`, то есть их видно
компилятору, они не ломают пространство имён и не требуют `#undef`. Значения
совпадают с каноническими значениями OpenGL.

| Группа | Примеры | Назначение |
|---|---|---|
| Примитивы | `GL_POINTS`, `GL_LINES`, `GL_TRIANGLES`, `GL_TRIANGLE_STRIP` | режим отрисовки |
| Биты очистки | `GL_COLOR_BUFFER_BIT`, `GL_DEPTH_BUFFER_BIT`, `GL_STENCIL_BUFFER_BIT` | маска `glClear` |
| Сравнения | `GL_NEVER`…`GL_ALWAYS`, `GL_LESS`, `GL_LEQUAL` | `glDepthFunc` |
| Смешивание | `GL_SRC_ALPHA`, `GL_ONE_MINUS_SRC_ALPHA`, `GL_ONE`, `GL_FUNC_ADD`, `GL_MIN`, `GL_MAX` | `glBlendFunc`, `glBlendEquation` |
| Грани и состояния | `GL_FRONT`, `GL_BACK`, `GL_CW`, `GL_CCW`, `GL_DEPTH_TEST`, `GL_CULL_FACE`, `GL_BLEND`, `GL_SCISSOR_TEST`, `GL_FRAMEBUFFER_SRGB` | `glEnable`, `glCullFace` |
| Форматы | `GL_RED`…`GL_RGBA`, `GL_R8`…`GL_RGBA32F`, `GL_DEPTH_COMPONENT24`, `GL_DEPTH24_STENCIL8`, `GL_SRGB8_ALPHA8` | внутренние форматы текстур |
| Типы данных | `GL_BYTE`…`GL_FLOAT`, `GL_HALF_FLOAT`, `GL_UNSIGNED_INT_24_8` | тип элементов |
| Буферы | `GL_ARRAY_BUFFER`, `GL_ELEMENT_ARRAY_BUFFER`, `GL_UNIFORM_BUFFER`, `GL_STREAM_DRAW`, `GL_STATIC_DRAW`, `GL_DYNAMIC_DRAW` | цели и подсказки буферов |
| Шейдеры | `GL_VERTEX_SHADER`, `GL_FRAGMENT_SHADER`, `GL_COMPILE_STATUS`, `GL_LINK_STATUS`, `GL_ACTIVE_UNIFORMS` | компиляция и линковка |
| Текстуры | `GL_TEXTURE_2D`, `GL_TEXTURE_3D`, `GL_TEXTURE_2D_ARRAY`, `GL_TEXTURE_CUBE_MAP`, `GL_TEXTURE_MIN_FILTER`, `GL_TEXTURE_WRAP_S`, `GL_TEXTURE_COMPARE_MODE`, `GL_TEXTURE_MAX_ANISOTROPY_EXT` | параметры и цели текстур |
| Фильтры и обёртки | `GL_NEAREST`, `GL_LINEAR`, `GL_LINEAR_MIPMAP_LINEAR`, `GL_REPEAT`, `GL_CLAMP_TO_EDGE`, `GL_MIRRORED_REPEAT` | сэмплирование |
| Фреймбуферы | `GL_FRAMEBUFFER`, `GL_READ_FRAMEBUFFER`, `GL_DRAW_FRAMEBUFFER`, `GL_RENDERBUFFER`, `GL_COLOR_ATTACHMENT0`, `GL_DEPTH_ATTACHMENT`, `GL_FRAMEBUFFER_COMPLETE` | FBO и renderbuffer |
| Пределы | `GL_MAX_TEXTURE_SIZE`, `GL_MAX_TEXTURE_IMAGE_UNITS`, `GL_MAX_VERTEX_ATTRIBS`, `GL_MAX_DRAW_BUFFERS`, `GL_MAX_SAMPLES` | запросы возможностей |
| Текстурные юниты | `GL_TEXTURE0`…`GL_TEXTURE15` | `glActiveTexture` |
| Строки | `GL_VENDOR`, `GL_RENDERER`, `GL_VERSION`, `GL_SHADING_LANGUAGE_VERSION`, `GL_EXTENSIONS`, `GL_NUM_EXTENSIONS` | `glGetString` |
| Ошибки | `GL_INVALID_ENUM`, `GL_INVALID_VALUE`, `GL_INVALID_OPERATION`, `GL_OUT_OF_MEMORY`, `GL_INVALID_FRAMEBUFFER_OPERATION` | результат `glGetError` |
| Синхронизация и отладка | `GL_SYNC_GPU_COMMANDS_COMPLETE`, `GL_ALREADY_SIGNALED`, `GL_TIMEOUT_EXPIRED`, `GL_SYNC_FLUSH_COMMANDS_BIT`, `GL_DEBUG_OUTPUT` | запросы и отладочный вывод |

```cpp
// Настройка сэмплирования 2D-текстуры: константы доступны без префикса GL_.
crossrender::gl::glTexParameteri(crossrender::gl::GL_TEXTURE_2D, crossrender::gl::GL_TEXTURE_MIN_FILTER,
                         crossrender::gl::GL_LINEAR_MIPMAP_LINEAR);
crossrender::gl::glTexParameteri(crossrender::gl::GL_TEXTURE_2D, crossrender::gl::GL_TEXTURE_WRAP_S,
                         crossrender::gl::GL_CLAMP_TO_EDGE);
crossrender::gl::glPixelStorei(crossrender::gl::GL_UNPACK_ALIGNMENT, 1);
```

### `bool LoadFunctions(void* (*getProcAddress)(const char*))`

Загружает все 114 точек входа из `ENG_GL_FUNCS(X)`, вызывая переданный
резолвер для каждого имени. Резолвер — это функция платформенного слоя,
которая по строке возвращает адрес символа (`glXGetProcAddressARB`, `wglGetProcAddress`,
`eglGetProcAddress`, `dlsym` — на разных платформах по-разному).

* **Параметр:** `getProcAddress` — указатель на резолвер; `nullptr`
  недопустим и приводит к `false` с сообщением в лог.
* **Возвращает:** `true`, если загрузка признана успешной.
* **Контекст:** требует **текущего** контекста OpenGL; вызывать после создания
  окна и до `Renderer3D::Init()` / `Renderer2D::Init()`.

Поведение при отсутствующих символах важнее, чем кажется: загрузчик считает
неудачи и возвращает `false` только тогда, когда их **больше двенадцати**, то
есть когда пропал целый обязательный набор (буферы, VAO, шейдеры, текстуры).
Единичные отсутствующие функции (типичная ситуация для GLES, где нет
`glGetTexImage` или `glDrawBuffer`) дают предупреждение в лог, но загрузка
считается успешной — соответствующие указатели остаются `nullptr`.

```cpp
if (!crossrender::gl::LoadFunctions(window.GLGetProcAddress())) {
    ENG_LOGE("gl", "загрузчик не нашёл обязательный набор точек входа");
    return false;
}
```

### `void UnloadFunctions()`

Обнуляет **все** указатели на точки входа. Используется при завершении работы и
при потере контекста: после вызова любой `crossrender::gl::glXxx` снова превращается в
нулевой указатель, поэтому вызывать её следует последней, когда все рендереры
уже сделали `Shutdown()`.

```cpp
renderer3d.Shutdown();
renderer2d.Shutdown();
crossrender::gl::UnloadFunctions();   // контекст уже недействителен
```

### Резолвер точек входа: `PlatformGLGetProcAddress()`

Загрузчик не знает, откуда брать адреса, — эту роль выполняет платформенный
слой. `PlatformGLGetProcAddress()` возвращает указатель на функцию вида
`void* (*)(const char*)`, которую `LoadFunctions` вызывает для каждого имени.
У окна есть и метод-обёртка `Window::GLGetProcAddress()`, возвращающий тот же
резолвер; в headless-режиме используется `HeadlessGLGetProcAddress(name)`.

| Функция | Заголовок | Когда использовать |
|---|---|---|
| `PlatformGLGetProcAddress()` | `crossrender/platform/Window.h` | обычная инициализация после создания окна |
| `Window::GLGetProcAddress()` | `crossrender/platform/Window.h` | когда резолвер удобнее взять у окна |
| `HeadlessGLGetProcAddress(name)` | `crossrender/platform/Platform.h` | офскрин-рендеринг без окна (тесты, CI) |

```cpp
// Оба варианта эквивалентны; выберите тот, что ближе к вашему коду.
if (!crossrender::gl::LoadFunctions(crossrender::PlatformGLGetProcAddress())) return false;
if (!crossrender::gl::LoadFunctions(window.GLGetProcAddress())) return false;
```

### `GpuInfo QueryGpuInfo()`

Читает строки драйвера (`GL_VENDOR`, `GL_RENDERER`, `GL_VERSION`,
`GL_SHADING_LANGUAGE_VERSION`), разбирает из версии числа `major.minor` и
запрашивает предельные значения, а также пробегает список расширений через
`glGetStringi`, чтобы найти анизотропную фильтрацию и отладочный вывод.

* **Возвращает:** заполненную структуру `GpuInfo`; если указатели ещё не
  загружены, возвращается пустая структура (все строки пусты, числа нули) —
  функция не падает.
* **Контекст:** требует загруженных точек входа и текущего контекста.

Поле `isGLES` выставляется, если строка версии содержит `"OpenGL ES"` **или**
сборка скомпилирована с ненулевым `ENG_GLES`. Это единственный надёжный способ
отличить настольный GL от GLES во время выполнения.

```cpp
const crossrender::gl::GpuInfo gpu = crossrender::gl::QueryGpuInfo();
if (gpu.maxTextureSize < 2048) {
    ENG_LOGW("gl", "драйвер сообщает maxTextureSize=%d — текстуры будут урезаны",
             gpu.maxTextureSize);
}
ENG_LOGI("gl", "GPU: %s | %s | GL %d.%d | GLES=%s", gpu.vendor.c_str(), gpu.renderer.c_str(),
         gpu.major, gpu.minor, gpu.isGLES ? "да" : "нет");
```

### `struct GpuInfo`

Результат `QueryGpuInfo`: человекочитаемое описание GPU и его возможностей.
Все строки — в кодировке драйвера, обычно ASCII.

| Поле | Тип | Смысл |
|---|---|---|
| `vendor` | `std::string` | производитель (`GL_VENDOR`) |
| `renderer` | `std::string` | имя ускорителя (`GL_RENDERER`) |
| `version` | `std::string` | версия контекста (`GL_VERSION`) |
| `glslVersion` | `std::string` | версия GLSL (`GL_SHADING_LANGUAGE_VERSION`) |
| `isGLES` | `bool` | контекст OpenGL ES, а не настольный GL |
| `major`, `minor` | `int` | разобранные из `version` числа |
| `maxTextureSize` | `int` | `GL_MAX_TEXTURE_SIZE` |
| `maxSamples` | `int` | `GL_MAX_SAMPLES` (потолок MSAA) |
| `maxDrawBuffers` | `int` | `GL_MAX_DRAW_BUFFERS` (MRT) |
| `maxVertexAttribs` | `int` | `GL_MAX_VERTEX_ATTRIBS` |
| `hasAnisotropy` | `bool` | найдено расширение с `"anisotropic"` |
| `hasDebugOutput` | `bool` | найдено расширение с `"EXT_debug"` |

```cpp
crossrender::gl::GpuInfo gpu = crossrender::gl::QueryGpuInfo();
const int samples = gpu.maxSamples >= 4 ? 4 : 1;
const bool mrt = gpu.maxDrawBuffers >= 2;
ENG_LOGI("gl", "%s: MSAA x%d, MRT=%s, debug output=%s", gpu.renderer.c_str(), samples,
         mrt ? "да" : "нет", gpu.hasDebugOutput ? "да" : "нет");
```

### `ENG_GL_CHECK(expr)`

Макрос проверки ошибок: выполняет выражение и сразу читает `glGetError()`.
В отладочной сборке (`!NDEBUG`) при ненулевом коде пишет ошибку с файлом,
строкой, именем функции и текстовым описанием кода; в релизной сборке
разворачивается ровно в `(expr)` и не стоит ничего.

Полезно помнить: `CheckErrorImpl` **осушает** очередь ошибок до первой, чтобы
одна старая ошибка не «прилипала» к последующим проверкам. Не оборачивайте в
`ENG_GL_CHECK` горячие вызовы в релизе и не оборачивайте то, что и так
проверяется самим движком.

```cpp
crossrender::gl::glBindVertexArray(vao);
ENG_GL_CHECK(crossrender::gl::glBindVertexArray(vao));
ENG_GL_CHECK(crossrender::gl::glDrawElements(crossrender::gl::GL_TRIANGLES, count, crossrender::gl::GL_UNSIGNED_INT, nullptr));
```

### `void CheckErrorImpl(const char* file, int line, const char* expr)`

Реализация проверки, стоящая за `ENG_GL_CHECK`. Вызывайте её напрямую, только
если нужно передать собственные `file`/`line` (например из собственного
макроса-обёртки). В `NDEBUG`-сборке функция пуста.

* **Параметры:** `file`, `line` — место вызова; `expr` — текст проверяемого
  выражения, попадающий в сообщение.
* **Контекст:** требует загруженного `glGetError`; если указатель пуст, функция
  молча выходит.

Не путайте её с `GLCheckError` из `crossrender/core/Log.h`: та функция объявлена, но её
реализация пуста, и реальную проверку выполняет только `CheckErrorImpl` (через
`ENG_GL_CHECK`).

```cpp
#define MY_GL_CHECK(call) \
    do { (call); ::crossrender::gl::CheckErrorImpl(__FILE__, __LINE__, #call); } while (0)

MY_GL_CHECK(crossrender::gl::glClear(crossrender::gl::GL_COLOR_BUFFER_BIT));
```

### `const char* ErrorString(GLenum err)`

Переводит код ошибки в строку: `GL_INVALID_ENUM`, `GL_INVALID_VALUE`,
`GL_INVALID_OPERATION`, `GL_OUT_OF_MEMORY`,
`GL_INVALID_FRAMEBUFFER_OPERATION`, `GL_NO_ERROR` (для нуля) или
`GL_UNKNOWN_ERROR`. Возвращаемая строка статическая — владеть ею не нужно.

```cpp
const crossrender::gl::GLenum err = crossrender::gl::glGetError();
if (err != 0) ENG_LOGW("gl", "неожиданная ошибка: %s (0x%04X)", crossrender::gl::ErrorString(err), err);
```

### Таблица точек входа

Ниже — **полный** список того, что объявляет `ENG_GL_FUNCS(X)`: все 114 точек
входа, сгруппированные по семействам. Таблица исчерпывающая; примеры кода
даны **по одному на семейство**, а не на каждую функцию — иначе документ
превратился бы в справочник OpenGL. Если функции нет в таблице, значит её нет и
в загрузчике.

**Буферы и вершинные массивы.**

| Функция | Назначение |
|---|---|
| `glGenBuffers` / `glDeleteBuffers` | создать / удалить имена буферов |
| `glBindBuffer` | привязать буфер к цели (`GL_ARRAY_BUFFER`, …) |
| `glBufferData` | выделить и заполнить хранилище буфера |
| `glBufferSubData` | обновить часть хранилища |
| `glGenVertexArrays` / `glDeleteVertexArrays` | создать / удалить VAO |
| `glBindVertexArray` | сделать VAO текущим |
| `glEnableVertexAttribArray` / `glDisableVertexAttribArray` | включить / выключить атрибут |
| `glVertexAttribPointer` | описать вещественный атрибут (тип, шаг, смещение) |
| `glVertexAttribIPointer` | описать целочисленный атрибут |
| `glVertexAttribDivisor` | шаг инстансинга для атрибута |
| `glVertexAttrib4f` | задать постоянное значение атрибута |

```cpp
crossrender::gl::GLuint vbo = 0, vao = 0;
crossrender::gl::glGenBuffers(1, &vbo);
crossrender::gl::glGenVertexArrays(1, &vao);
crossrender::gl::glBindVertexArray(vao);
crossrender::gl::glBindBuffer(crossrender::gl::GL_ARRAY_BUFFER, vbo);
crossrender::gl::glBufferData(crossrender::gl::GL_ARRAY_BUFFER, bytes, data, crossrender::gl::GL_STATIC_DRAW);
crossrender::gl::glEnableVertexAttribArray(0);
crossrender::gl::glVertexAttribPointer(0, 3, crossrender::gl::GL_FLOAT, crossrender::gl::GL_FALSE, stride, nullptr);
```

**Шейдеры и программы.**

| Функция | Назначение |
|---|---|
| `glCreateShader` / `glDeleteShader` | создать / удалить шейдер |
| `glShaderSource` | передать исходник GLSL |
| `glCompileShader` | скомпилировать шейдер |
| `glGetShaderiv` / `glGetShaderInfoLog` | статус компиляции / лог компиляции |
| `glCreateProgram` / `glDeleteProgram` | создать / удалить программу |
| `glAttachShader` / `glDetachShader` | присоединить / отсоединить шейдер |
| `glLinkProgram` | слинковать программу |
| `glGetProgramiv` / `glGetProgramInfoLog` | статус линковки / лог линковки |
| `glUseProgram` | сделать программу текущей |
| `glGetUniformLocation` / `glGetAttribLocation` | найти расположение uniform / атрибута |
| `glBindAttribLocation` | привязать индекс атрибута до линковки |
| `glUniform1i` / `glUniform2i` | целочисленные uniform-ы (сэмплеры, режимы) |
| `glUniform1f` / `glUniform2f` / `glUniform3f` / `glUniform4f` | вещественные uniform-ы |
| `glUniform1fv` / `glUniform2fv` / `glUniform3fv` / `glUniform4fv` | массивы вещественных uniform-ов |
| `glUniform1iv` | массив целых |
| `glUniformMatrix3fv` / `glUniformMatrix4fv` | матрицы 3×3 и 4×4 |

```cpp
const char* vs = "#version 330 core\nvoid main() { gl_Position = vec4(0.0); }";
crossrender::gl::GLuint sh = crossrender::gl::glCreateShader(crossrender::gl::GL_VERTEX_SHADER);
crossrender::gl::glShaderSource(sh, 1, &vs, nullptr);
crossrender::gl::glCompileShader(sh);
crossrender::gl::GLint ok = 0;
crossrender::gl::glGetShaderiv(sh, crossrender::gl::GL_COMPILE_STATUS, &ok);
if (!ok) ENG_LOGE("gl", "вершинный шейдер не скомпилировался");
crossrender::gl::glUseProgram(program);
crossrender::gl::glUniformMatrix4fv(loc, 1, crossrender::gl::GL_FALSE, matrix.data());
```

**Текстуры.**

| Функция | Назначение |
|---|---|
| `glGenTextures` / `glDeleteTextures` | создать / удалить имена текстур |
| `glBindTexture` | привязать текстуру к цели |
| `glActiveTexture` | выбрать текстурный юнит (`GL_TEXTURE0`…) |
| `glTexImage2D` | создать или перезаписать уровень 2D-текстуры |
| `glTexSubImage2D` | обновить часть уровня 2D-текстуры |
| `glTexImage3D` / `glTexSubImage3D` | то же для 3D-текстур и массивов |
| `glTexParameteri` / `glTexParameterf` / `glTexParameterfv` | фильтрация, обёртка, сравнение |
| `glGenerateMipmap` | построить mip-цепочку |

```cpp
crossrender::gl::glActiveTexture(crossrender::gl::GL_TEXTURE0);
crossrender::gl::glBindTexture(crossrender::gl::GL_TEXTURE_2D, tex);
crossrender::gl::glTexImage2D(crossrender::gl::GL_TEXTURE_2D, 0, crossrender::gl::GL_RGBA8, w, h, 0, crossrender::gl::GL_RGBA,
                      crossrender::gl::GL_UNSIGNED_BYTE, pixels);
crossrender::gl::glTexParameteri(crossrender::gl::GL_TEXTURE_2D, crossrender::gl::GL_TEXTURE_MIN_FILTER,
                         crossrender::gl::GL_LINEAR_MIPMAP_LINEAR);
crossrender::gl::glGenerateMipmap(crossrender::gl::GL_TEXTURE_2D);
```

**Фреймбуферы и renderbuffer.**

| Функция | Назначение |
|---|---|
| `glGenFramebuffers` / `glDeleteFramebuffers` | создать / удалить FBO |
| `glBindFramebuffer` | привязать FBO (`GL_FRAMEBUFFER`, read/draw) |
| `glFramebufferTexture2D` | прикрепить текстуру к точке прикрепления |
| `glCheckFramebufferStatus` | проверить полноту FBO |
| `glGenRenderbuffers` / `glDeleteRenderbuffers` / `glBindRenderbuffer` | создать, удалить, привязать renderbuffer |
| `glRenderbufferStorage` / `glRenderbufferStorageMultisample` | выделить хранилище, в том числе MSAA |
| `glFramebufferRenderbuffer` | прикрепить renderbuffer к FBO |
| `glBlitFramebuffer` | скопировать или отмасштабировать между FBO |
| `glDrawBuffers` | задать список colour-прикреплений для MRT |

```cpp
crossrender::gl::glGenFramebuffers(1, &fbo);
crossrender::gl::glBindFramebuffer(crossrender::gl::GL_FRAMEBUFFER, fbo);
crossrender::gl::glFramebufferTexture2D(crossrender::gl::GL_FRAMEBUFFER, crossrender::gl::GL_COLOR_ATTACHMENT0,
                                crossrender::gl::GL_TEXTURE_2D, colorTex, 0);
if (crossrender::gl::glCheckFramebufferStatus(crossrender::gl::GL_FRAMEBUFFER) != crossrender::gl::GL_FRAMEBUFFER_COMPLETE) {
    ENG_LOGE("gl", "фреймбуфер неполный");
}
crossrender::gl::glBindFramebuffer(crossrender::gl::GL_FRAMEBUFFER, 0);
```

**Состояние и растеризация.**

| Функция | Назначение |
|---|---|
| `glClear` / `glClearColor` / `glClearDepthf` | очистка буферов и цвет/глубина очистки |
| `glViewport` / `glScissor` | область вывода и прямоугольник отсечения |
| `glEnable` / `glDisable` / `glIsEnabled` | включить, выключить, проверить состояние |
| `glBlendFunc` / `glBlendFuncSeparate` | функция смешивания |
| `glBlendEquation` / `glBlendEquationSeparate` / `glBlendColor` | уравнение смешивания и константа |
| `glDepthFunc` / `glDepthMask` / `glDepthRangef` | тест, запись и диапазон глубины |
| `glColorMask` | маска записи в цветовые каналы |
| `glCullFace` / `glFrontFace` | отсечение граней и порядок обхода |
| `glLineWidth` | толщина линий |
| `glPolygonOffset` | сдвиг глубины полигонов |
| `glPixelStorei` | выравнивание при передаче пикселей |

```cpp
crossrender::gl::glEnable(crossrender::gl::GL_DEPTH_TEST);
crossrender::gl::glDepthFunc(crossrender::gl::GL_LEQUAL);
crossrender::gl::glDepthMask(1);
crossrender::gl::glEnable(crossrender::gl::GL_BLEND);
crossrender::gl::glBlendFunc(crossrender::gl::GL_SRC_ALPHA, crossrender::gl::GL_ONE_MINUS_SRC_ALPHA);
crossrender::gl::glDisable(crossrender::gl::GL_CULL_FACE);
crossrender::gl::glClearColor(0.05f, 0.06f, 0.08f, 1.0f);
crossrender::gl::glClear(crossrender::gl::GL_COLOR_BUFFER_BIT | crossrender::gl::GL_DEPTH_BUFFER_BIT);
```

**Отрисовка.**

| Функция | Назначение |
|---|---|
| `glDrawArrays` | нарисовать набор вершин без индексов |
| `glDrawElements` | нарисовать по индексному буферу |
| `glDrawArraysInstanced` | то же с инстансингом без индексов |
| `glDrawElementsInstanced` | то же с инстансингом по индексам |

```cpp
crossrender::gl::glBindVertexArray(vao);
crossrender::gl::glDrawElements(crossrender::gl::GL_TRIANGLES, indexCount, crossrender::gl::GL_UNSIGNED_INT, nullptr);
crossrender::gl::glDrawElementsInstanced(crossrender::gl::GL_TRIANGLES, indexCount, crossrender::gl::GL_UNSIGNED_INT, nullptr,
                                 instanceCount);
```

**Запросы, чтение и синхронизация.**

| Функция | Назначение |
|---|---|
| `glGetIntegerv` / `glGetBooleanv` / `glGetFloatv` | прочитать состояние или предел |
| `glGetError` | забрать код ошибки |
| `glGetString` / `glGetStringi` | строки драйвера / список расширений |
| `glReadPixels` | прочитать пиксели из текущего read-буфера |
| `glGetTexImage` | прочитать уровень текстуры (нет в GLES 3.0) |
| `glReadBuffer` / `glDrawBuffer` | выбрать read/draw буфер |
| `glFinish` / `glFlush` | дождаться GPU / отправить команды |
| `glInvalidateFramebuffer` | объявить прикрепления ненужными |
| `glGenQueries` / `glDeleteQueries` | создать / удалить объект запроса |
| `glBeginQuery` / `glEndQuery` | начать / закончить запрос |
| `glGetQueryObjectuiv` / `glGetQueryObjectui64v` | прочитать результат запроса (32 и 64 бита) |

```cpp
crossrender::gl::GLint maxTexture = 0;
crossrender::gl::glGetIntegerv(crossrender::gl::GL_MAX_TEXTURE_SIZE, &maxTexture);
const crossrender::gl::GLubyte* renderer = crossrender::gl::glGetString(crossrender::gl::GL_RENDERER);
ENG_LOGI("gl", "%s: maxTextureSize=%d", reinterpret_cast<const char*>(renderer), maxTexture);

crossrender::gl::GLuint query = 0;
crossrender::gl::glGenQueries(1, &query);
crossrender::gl::glBeginQuery(crossrender::gl::GL_SYNC_GPU_COMMANDS_COMPLETE, query);
crossrender::gl::glEndQuery(crossrender::gl::GL_SYNC_GPU_COMMANDS_COMPLETE);
crossrender::gl::GLuint result = 0;
crossrender::gl::glGetQueryObjectuiv(query, crossrender::gl::GL_SYNC_GPU_COMMANDS_COMPLETE, &result);
```

## Пример целиком

```cpp
#include "crossrender/core/Log.h"
#include "crossrender/gfx/GL.h"
#include "crossrender/platform/Window.h"

// Минимальная инициализация GL-слоя: загрузить точки входа, узнать пределы,
// создать буфер и сообщить о проблемах.
bool InitGlLayer(crossrender::Window& window) {
    if (!crossrender::gl::LoadFunctions(window.GLGetProcAddress())) {
        ENG_LOGE("gl", "обязательные точки входа OpenGL не загрузились");
        return false;
    }

    const crossrender::gl::GpuInfo gpu = crossrender::gl::QueryGpuInfo();
    ENG_LOGI("gl", "%s | %s | GL %d.%d | GLES=%s", gpu.vendor.c_str(), gpu.renderer.c_str(),
             gpu.major, gpu.minor, gpu.isGLES ? "да" : "нет");
    if (gpu.maxTextureSize < 2048) {
        ENG_LOGW("gl", "maxTextureSize=%d, крупные текстуры будут урезаны", gpu.maxTextureSize);
    }

    crossrender::gl::GLuint vbo = 0, vao = 0;
    crossrender::gl::glGenVertexArrays(1, &vao);
    crossrender::gl::glBindVertexArray(vao);
    crossrender::gl::glGenBuffers(1, &vbo);
    crossrender::gl::glBindBuffer(crossrender::gl::GL_ARRAY_BUFFER, vbo);

    const float triangle[9] = {0.0f, 0.5f, 0.0f, -0.5f, -0.5f, 0.0f, 0.5f, -0.5f, 0.0f};
    crossrender::gl::glBufferData(crossrender::gl::GL_ARRAY_BUFFER, sizeof(triangle), triangle,
                          crossrender::gl::GL_STATIC_DRAW);
    crossrender::gl::glEnableVertexAttribArray(0);
    crossrender::gl::glVertexAttribPointer(0, 3, crossrender::gl::GL_FLOAT, crossrender::gl::GL_FALSE, 3 * sizeof(float),
                                   nullptr);
    ENG_GL_CHECK(crossrender::gl::glBindVertexArray(vao));

    crossrender::gl::glEnable(crossrender::gl::GL_DEPTH_TEST);
    crossrender::gl::glClearColor(0.05f, 0.06f, 0.08f, 1.0f);
    crossrender::gl::glClear(crossrender::gl::GL_COLOR_BUFFER_BIT | crossrender::gl::GL_DEPTH_BUFFER_BIT);
    crossrender::gl::glDrawArrays(crossrender::gl::GL_TRIANGLES, 0, 3);

    const crossrender::gl::GLenum err = crossrender::gl::glGetError();
    if (err != 0) ENG_LOGW("gl", "после кадра осталась ошибка %s", crossrender::gl::ErrorString(err));
    return true;
}
```

## См. также

* `docs/gfx/Renderer3D.md` — первый потребитель загрузчика: рендерер вызывает
  `crossrender::gl::glXxx` и полагается на то, что `LoadFunctions` уже выполнена.
* `docs/gfx/Shader.md` — компиляция GLSL и преамбула `#version 300 es` /
  `#version 330 core`.
* `docs/gfx/RenderTarget.md` — фреймбуферы, renderbuffer-ы и MSAA поверх
  функций из этой таблицы.
* `docs/core/Log.md` — `ENG_LOGE` / `ENG_LOGW`, которыми загрузчик сообщает о
  проблемах, и `GLCheckError`.
* `docs/platform/Window.md` — `Window::GLGetProcAddress()` и создание контекста.
