# crossrender/core/Log.h — логирование, проверки и фатальные ошибки

Единая точка вывода диагностики: уровни важности, категории, приёмники
(sinks), история сообщений для внутриигровой консоли и макросы-обёртки.

## Заголовок

```cpp
#include "crossrender/core/Log.h"
```

## Обзор

Логгер устроен просто: есть глобальный уровень, ниже которого сообщения
отбрасываются, и список приёмников — функций, куда сообщение доставляется.
Стандартный приёмник пишет в консоль и окрашивает строку по уровню.

Порядок работы:

1. На старте приложение задаёт уровень через `LogSetLevel` (в тестах —
   `LogLevel::Error`, чтобы вывод не засорялся).
2. Код пишет сообщения макросами `ENG_LOGI` / `ENG_LOGW` / `ENG_LOGE` с
   категорией: `ENG_LOGI("audio", "загружен %s", path.c_str())`. Формат —
   `printf`-совместимый, аргументы проверяются компилятором.
3. Если нужно показать лог в интерфейсе, включается история
   (`LogEnableHistory(true)`) и читается через `LogHistory()`.

Категория — короткая строка, обычно имя модуля: `"gl"`, `"font"`, `"assets"`,
`"ui"`. По ней удобно фильтровать вывод.

## Члены класса

### `enum class LogLevel : int`

Уровни важности. Числа упорядочены: сообщение печатается, если его уровень не
ниже текущего порога.

| Значение | Смысл |
|---|---|
| `LogLevel::Trace` | максимально подробная отладка, обычно выключено |
| `LogLevel::Debug` | отладочные сведения, которые полезны при разработке |
| `LogLevel::Info` | обычные события: загрузка, смена сцены, итоги |
| `LogLevel::Warn` | что-то пошло не так, но работа продолжается |
| `LogLevel::Error` | операция не выполнена; приложение живо |
| `LogLevel::Fatal` | продолжать нельзя, вызывается `LogFatal` |
| `LogLevel::Off` | полностью отключить вывод |

```cpp
crossrender::LogSetLevel(crossrender::LogLevel::Debug);
ENG_LOGD("demo", "это сообщение увидят, потому что уровень Debug");
```

### `using LogSink`

Указатель на функцию-приёмник:

```cpp
void (*)(LogLevel level, const char* category, const char* message, void* user);
```

Через `user` передаётся произвольный контекст, который вы указали при
регистрации. Сообщение приходит уже отформатированным, без перевода строки.

```cpp
// Свой приёмник: складывает ошибки в вектор для последующей проверки.
struct Collector {
    std::vector<std::string> errors;
};

void CollectErrors(crossrender::LogLevel level, const char* category, const char* message, void* user) {
    auto* c = static_cast<Collector*>(user);
    if (level >= crossrender::LogLevel::Error) c->errors.push_back(std::string(category) + ": " + message);
}

Collector collector;
crossrender::LogAddSink(&CollectErrors, &collector);
```

### `void LogSetLevel(LogLevel level)`

Задаёт минимальный уровень, который попадает в вывод. Всё, что ниже,
отбрасывается ещё до форматирования.

```cpp
// В релизной сборке оставляем только предупреждения и ошибки.
#if defined(NDEBUG)
    crossrender::LogSetLevel(crossrender::LogLevel::Warn);
#else
    crossrender::LogSetLevel(crossrender::LogLevel::Debug);
#endif
```

### `LogLevel LogGetLevel()`

Возвращает текущий порог. Полезно, чтобы не собирать дорогую строку, если
сообщение всё равно будет отброшено.

```cpp
if (crossrender::LogGetLevel() <= crossrender::LogLevel::Debug) {
    const std::string dump = BuildExpensiveDump();
    ENG_LOGD("demo", "%s", dump.c_str());
}
```

### `void LogAddSink(LogSink sink, void* user)`

Добавляет приёмник. Приёмников может быть несколько — сообщение уходит во все.

```cpp
crossrender::LogAddSink(&CollectErrors, &collector);   // наш сборщик из примера выше
ENG_LOGE("demo", "эта строка попадёт и в консоль, и в collector");
```

### `void LogClearSinks()`

Убирает все приёмники, включая стандартный вывод. Обычно вызывается в тестах,
чтобы полностью заглушить логгер.

```cpp
crossrender::LogClearSinks();
ENG_LOGE("demo", "этого сообщения никто не увидит");
```

### `void LogWrite(LogLevel level, const char* category, const char* fmt, ...)`

Низкоуровневая запись. Обычно вызывается через макросы, но пригодится, если
уровень и категория известны только во время выполнения.

```cpp
void Report(bool ok) {
    crossrender::LogWrite(ok ? crossrender::LogLevel::Info : crossrender::LogLevel::Warn, "сеть",
                  "соединение %s", ok ? "установлено" : "не установлено");
}
```

### `std::vector<std::string> LogHistory()`

Возвращает накопленные строки (последние `N`), если история включена. Каждая
строка — уже готовый текст сообщения.

```cpp
for (const std::string& line : crossrender::LogHistory()) {
    ui.Label(line, nextRow());     // вывод во внутриигровую консоль
}
```

### `void LogEnableHistory(bool enable)`

Включает или выключает накопление истории. Держите выключенным в релизе: это
постоянно растущий буфер строк.

```cpp
crossrender::LogEnableHistory(true);       // включаем перед открытием консоли
```

### `[[noreturn]] void LogFatal(const char* category, const char* fmt, ...)`

Печатает сообщение уровня `Fatal` и завершает процесс. Вызывается, когда
продолжать работу нельзя — например, не создался контекст OpenGL.

```cpp
if (!window.Create(1280, 800, "Demo")) {
    crossrender::LogFatal("platform", "не удалось создать окно — дальнейшая работа невозможна");
}
```

### `ENG_TRACE(...)`, `ENG_DEBUG(...)`, `ENG_INFO(...)`, `ENG_WARN(...)`, `ENG_ERROR(...)`

Макросы без категории: подставляют `"crossrender"`. Удобны в библиотечном коде, где
категория не важна.

```cpp
ENG_INFO("движок запущен");
ENG_WARN("текстура %s не найдена, использую заглушку", name.c_str());
```

### `ENG_FATAL(...)`

Макрос без категории, вызывает `LogFatal` и не возвращает управление.

```cpp
if (sizeof(void*) < 8 && !supports32Bit) {
    ENG_FATAL("сборка требует 64-битной платформы");
}
```

### `ENG_LOGT(cat, ...)`

Пишет сообщение уровня `Trace` с указанной категорией.

```cpp
ENG_LOGT("gl", "glBindTexture(unit=%d, id=%u)", unit, id);
```

### `ENG_LOGD(cat, ...)`

Пишет сообщение уровня `Debug` с указанной категорией.

```cpp
ENG_LOGD("font", "загружен %s: %d глифов, upem=%.0f", path.c_str(), count, upem);
```

### `ENG_LOGI(cat, ...)`

Пишет сообщение уровня `Info`. Основной макрос для нормальных событий.

```cpp
ENG_LOGI("scene", "вошли в сцену '%s'", scene->Name());
```

### `ENG_LOGW(cat, ...)`

Пишет предупреждение: что-то не так, но приложение продолжает работать.

```cpp
ENG_LOGW("assets", "атлас '%s' недоступен, рисование отключено", name.c_str());
```

### `ENG_LOGE(cat, ...)`

Пишет ошибку: операция не выполнена. Проверяйте результат и не продолжайте
так, будто всё в порядке.

```cpp
if (!shader.Build(vs, fs, "sprite")) {
    ENG_LOGE("gl", "шейдер спрайта не собрался, рендерер переходит в инертный режим");
}
```

### `ENG_ASSERT(cond)`

В отладочной сборке проверяет условие и при неудаче пишет фатальное сообщение
с файлом и строкой, после чего останавливает отладчик. В `NDEBUG`-сборке
разворачивается в пустоту.

```cpp
void Renderer2D::PushVertex(crossrender::Vec2 pos) {
    ENG_ASSERT(impl_ != nullptr);
    // ...
}
```

### `ENG_ASSERT_MSG(cond, ...)`

Как `ENG_ASSERT`, но добавляет собственное сообщение с `printf`-форматом.

```cpp
ENG_ASSERT_MSG(texture.Width() > 0, "нулевая ширина текстуры у '%s'", name.c_str());
```

### `void GLCheckError(const char* file, int line, const char* expr)`

Проверяет `glGetError()` после вызова OpenGL и пишет сообщение, если код не
`GL_NO_ERROR`. Вызывается макросом `ENG_GL_CHECK`.

```cpp
gl::glBindVertexArray(vao);
ENG_GL_CHECK("glBindVertexArray");
```

## Пример целиком

```cpp
#include "crossrender/core/Log.h"

#include <string>
#include <vector>

// Собирает ошибки загрузки, чтобы показать их пользователю одной панелью.
class AssetLoadReport {
public:
    AssetLoadReport() {
        crossrender::LogEnableHistory(true);
        crossrender::LogAddSink(&AssetLoadReport::OnLog, this);
    }

    ~AssetLoadReport() { crossrender::LogClearSinks(); }

    void Print() const {
        ENG_LOGI("assets", "загрузка завершена: успешно %d, ошибок %d",
                 loaded_, static_cast<int>(failed_.size()));
        for (const std::string& name : failed_) ENG_LOGW("assets", "  не найдено: %s", name.c_str());
    }

private:
    static void OnLog(crossrender::LogLevel level, const char* category, const char* message, void* user) {
        auto* self = static_cast<AssetLoadReport*>(user);
        if (level >= crossrender::LogLevel::Error) self->failed_.push_back(std::string(category) + ": " + message);
        else ++self->loaded_;
    }

    int loaded_ = 0;
    std::vector<std::string> failed_;
};

void Demo() {
    crossrender::LogSetLevel(crossrender::LogLevel::Info);

    AssetLoadReport report;
    ENG_LOGI("assets", "загружаю textures/checker.png");
    ENG_LOGE("assets", "файл textures/missing.png не открылся");
    report.Print();

    // История доступна и целиком, например для внутриигровой консоли.
    const std::vector<std::string> lines = crossrender::LogHistory();
    ENG_LOGI("demo", "в истории %d строк", static_cast<int>(lines.size()));
}
```

## См. также

* `docs/core/Base.md` — `ENG_DEBUG_BREAK()` и типы, используемые в логгере.
* `docs/core/File.md` — файловые операции, которые обычно логируются.
* `docs/test/Test.md` — как тесты управляют уровнем логирования.
