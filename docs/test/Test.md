# crossrender/test/Test.h — лёгкий фреймворк модульных тестов

Собственный тест-фреймворк движка без внешних зависимостей: макросы
регистрации и проверок, авто-реестр тест-сьютов, раннер с фильтром и
счётчиком ссылок на один общий контекст OpenGL.

## Заголовок

```cpp
#include "crossrender/test/Test.h"
```

## Обзор

Фреймворк намеренно крошечный: тест — это обычная функция `void()`, которая
при неудачной проверке бросает исключение `TestFailure`. Никаких фикстур,
параметризации и `main` в заголовке нет.

Порядок работы:

1. В любом `.cpp`, подключённом к тестовому бинарнику, пишется
   `ENG_TEST(Сьют, Имя) { ... }`. Макрос создаёт статическую функцию и
   статический объект `Registrar`, который на этапе статической инициализации
   добавляет `TestCase` в глобальный реестр (`Registry()` — синглтон Мейерса,
   поэтому реестр один на все единицы трансляции).
2. Раннер вызывает `RunAll(filter, verbose)`, который сортирует тесты по
   имени сьюта и имени кейса и выполняет их по очереди, ловя `TestFailure`.
3. Тест, которому нужен GPU, начинается с `ENG_REQUIRE_GL()`: если контекст
   недоступен, тест **пропускается**, а не падает.

**Сьют** (suite) — это первый аргумент `ENG_TEST`, то есть имя группы; оно
печатается в раннере как `[Сьют]` и участвует в фильтре и в `--list`. Оба
аргумента подставляются в имя символа (`eng_test_##suite##_##name`), поэтому
должны быть корректными идентификаторами C++.

#### Как добавить тест

```cpp
// tests/test_my_feature.cpp
#include "crossrender/test/Test.h"
#include "crossrender/core/Log.h"

ENG_TEST(MyFeature, AddsTwoNumbers) {
    const int sum = 2 + 2;
    ENG_CHECK_EQ(sum, 4);
}
```

CMake собирает `crossrender_tests` из `tests/*.cpp` по маске, поэтому новый файл
подхватывается сам — регистрировать его нигде не нужно. Сьюта `MyFeature`
раньше не было: она появится в выводе автоматически.

#### Кто определяет раннер

Заголовок объявляет, но **не определяет** `Registry`, `ReportFailure`,
`ReportSkip`, `RequireGLContext`, `EnsureGLContext`, `ReleaseGLContext`,
`TempFilePath`, `RandomFloat01` и `RunAll`. Их реализации лежат в
`tests/test_main.cpp` и собираются только в тестовый бинарник; библиотека
`crossrender` их не содержит. Практические следствия:

* `ENG_TEST` и макросы проверок можно использовать в любом коде, который
  линкуется вместе с раннером;
* приложение, которое линкует только `crossrender`, обязано само определить эти
  символы (или не использовать `RunAll`/`ENG_CHECK`);
* `RunAll` не является частью публичного API движка — это соглашение тестового
  бинарника.

#### Модель отказа и пропуска

`ReportFailure` не просто записывает ошибку, а **бросает** `TestFailure`.
Поэтому после первой неудачной проверки остаток теста не выполняется: раннер
переходит к следующему кейсу. Проверки «через запятую» без остановки не
работают — для этого нужно несколько `ENG_TEST` или собственная логика.

`ReportSkip` только выставляет флаг «пропущен», после чего нужно выйти из
теста; именно это делают `ENG_SKIP` и `ENG_REQUIRE_GL()`. Раннер считает тест
пропущенным по **флагу**, а не по типу исключения: если флаг выставлен, тест
попадёт в `skipped` даже тогда, когда следом прилетел `TestFailure`.
Непойманное исключение другого типа (например, `std::exception` из
`std::vector::at`) всё равно делает тест провальным. Пропуск **не является**
ошибкой и не влияет на код возврата процесса.

#### Один контекст OpenGL на весь прогон

`RunAll` создаёт контекст до цикла (`EnsureGLContext()`) и уничтожает после
(`ReleaseGLContext()`), а после каждого теста вызывает `ReleaseGLContext()`
ещё раз. На настольных платформах `CreateHeadlessGLContext()` считает ссылки,
поэтому счётчик, взятый раннером в начале, не даёт контексту закрыться между
тестами. Это сделано специально: создание и уничтожение контекста на каждый
тест обнуляло бы кэшированные GPU-ресурсы, которые тесты держат в статических
переменных функций (атласы шрифтов, текстуры), и следующий тест видел бы
«висячие» идентификаторы.

Отсюда правило: **не вызывайте `DestroyHeadlessGLContext()` в тесте**, если
хотите, чтобы после вас работали другие. Пары
`CreateHeadlessGLContext`/`DestroyHeadlessGLContext` балансируйте сами, а для
«нужен GPU» используйте `ENG_REQUIRE_GL()`.

#### Запуск из командной строки

```bash
./crossrender_tests                     # все тесты, лог уровня Error
./crossrender_tests -v                  # подробный вывод: время каждого теста
./crossrender_tests Net                 # только тесты, где в "Сьют.Кейс" есть "Net"
./crossrender_tests --filter=Window.Create
./crossrender_tests --list              # перечислить "Сьют.Кейс" и выйти
./crossrender_tests --loglevel debug    # trace|debug|info|warn|off (по умолчанию error)
```

Аргумент без ведущего `-` — это фильтр (подстрока строки `"Сьют.Кейс"`), а не
имя сьюта целиком, поэтому `Net` поймает и `Net.TcpLoopback`, и
`WebSocket.Send`. Флаг `-v`/`--verbose` печатает время каждого теста и
принудительно поднимает уровень логов до `Info`, даже если `--loglevel` задан
ниже. Возвращаемый код процесса — `0`, если провалов нет, и `1` в противном
случае; пропущенные тесты провалом не считаются.

## Члены класса

### `struct TestCase`

Одна зарегистрированная единица работы: имя сьюта, имя кейса, указатель на
функцию и место объявления. Заполняется макросом `ENG_TEST` и попадает в
`Registry()`.

```cpp
for (const crossrender::test::TestCase& tc : crossrender::test::Registry()) {
    ENG_LOGI("test", "зарегистрирован %s.%s (%s:%d)", tc.suite, tc.name, tc.file, tc.line);
}
```

### `const char* suite = ""`

Имя сьюта — строковая форма первого аргумента `ENG_TEST`. По нему раннер
группирует вывод и сортирует тесты.

```cpp
for (const crossrender::test::TestCase& tc : crossrender::test::Registry()) {
    if (std::strcmp(tc.suite, "Net") != 0) continue;
    ENG_LOGI("test", "тест сети: %s", tc.name);
}
```

### `const char* name = ""`

Имя кейса — строковая форма второго аргумента `ENG_TEST`. В выводе раннера
печатается как `Сьют.Кейс`.

```cpp
for (const crossrender::test::TestCase& tc : crossrender::test::Registry()) {
    ENG_LOGI("test", "полное имя: %s.%s", tc.suite, tc.name);
}
```

### `void (*fn)() = nullptr`

Функция теста. Раннер вызывает её без аргументов и ловит `TestFailure`.

```cpp
// Ручной запуск одного кейса — например, из отладочного меню.
for (const crossrender::test::TestCase& tc : crossrender::test::Registry()) {
    if (std::strcmp(tc.name, "TcpLoopback") == 0 && tc.fn) {
        try {
            tc.fn();
        } catch (const crossrender::test::TestFailure& f) {
            ENG_LOGE("test", "кейс упал: %s", f.message.c_str());
        }
    }
}
```

### `const char* file = ""`

Файл, в котором объявлен тест (`__FILE__` на момент раскрытия макроса). Нужен
для сообщения о падении.

```cpp
ENG_LOGI("test", "тест объявлен в %s", crossrender::test::Registry().front().file);
```

### `int line = 0`

Номер строки объявления теста (`__LINE__`). Вместе с `file` даёт точку входа
для перехода из IDE.

```cpp
const crossrender::test::TestCase& first = crossrender::test::Registry().front();
ENG_LOGI("test", "объявление: %s:%d", first.file, first.line);
```

### `std::vector<TestCase>& Registry()`

Доступ к глобальному реестру тестов. Возвращает ссылку на статический вектор,
который живёт до конца процесса. Сам вектор никто не сортирует до `RunAll`,
поэтому порядок до запуска — это порядок статической инициализации, то есть по
сути не определён.

```cpp
const crossrender::usize count = crossrender::test::Registry().size();
ENG_LOGI("test", "зарегистрировано кейсов: %llu", static_cast<unsigned long long>(count));
```

### `struct Registrar`

Хелпер статической регистрации: конструктор добавляет `TestCase` в реестр.
Используется только макросом `ENG_TEST`; вручную создавать его не нужно, но
можно, если тест генерируется кодом.

```cpp
// Эквивалент ENG_TEST(Demo, Manual) без макроса.
static void DemoManualTest() {
    ENG_CHECK(true);
}
static ::crossrender::test::Registrar demoReg("Demo", "Manual", &DemoManualTest, __FILE__, __LINE__);
```

### `Registrar(const char* suite, const char* name, void (*fn)(), const char* file, int line)`

Создаёт запись реестра. Строки не копируются — передавайте литералы или
данные, живущие весь процесс.

```cpp
crossrender::test::Registrar reg("Demo", "AdHoc", [] {}, __FILE__, __LINE__);
ENG_LOGI("test", "добавлен вручную, всего кейсов: %llu",
         static_cast<unsigned long long>(crossrender::test::Registry().size()));
```

### `struct TestFailure`

Исключение, которым проверки сообщают о провале. Раннер ловит его, считает
тест упавшим и продолжает прогон; всё, что не является `TestFailure`,
считается непойманным исключением и тоже оформляется как провал.

```cpp
try {
    ENG_TEST_FAIL("ручной провал для проверки раннера");
} catch (const crossrender::test::TestFailure& f) {
    ENG_LOGI("test", "поймали ожидаемое падение: %s", f.message.c_str());
}
```

### `std::string message`

Текст ошибки: у `ReportFailure` — `"файл:строка: сообщение"`, у пропуска —
строка вида `"файл:строка: skipped"`. Именно она печатается в блоке
`Failures:`.

```cpp
crossrender::test::TestFailure failure{"demo.cpp:42: check failed: value > 0"};
if (failure.message.find("check failed") != std::string::npos) {
    ENG_LOGW("test", "падение проверки: %s", failure.message.c_str());
}
```

### `void ReportFailure(const char* file, int line, const std::string& message)`

Центральная точка сообщения о провале. Реализация форматирует
`"файл:строка: сообщение"` в буфер и **бросает** `TestFailure`, поэтому вызов
прерывает текущий тест. Обычно вызывается макросами проверок.

```cpp
if (texture.Width() <= 0) {
    crossrender::test::ReportFailure(__FILE__, __LINE__, "нулевая ширина текстуры");
}
// Сюда управление уже не придёт: ReportFailure бросил исключение.
```

### `void ReportSkip(const std::string& reason)`

Помечает текущий тест пропущенным и запоминает причину. Сам по себе не
прерывает выполнение — чтобы выйти из теста, используйте `ENG_SKIP` или
`ENG_REQUIRE_GL()`.

* **Важно:** флаг пропуска «сильнее» проверок. Если после `ReportSkip` тест
  упадёт на `ENG_CHECK`, раннер всё равно запишет его в `skipped`, а текст
  падения потеряется. Поэтому после `ReportSkip` всегда выходите из теста,
  а не продолжайте работу.

```cpp
if (graphicsDriverTooOld) {
    crossrender::test::ReportSkip("драйвер не поддерживает нужное расширение");
    return;   // без return тест продолжится
}
```

### `void RequireGLContext(const char* file, int line)`

Требует GPU-контекст: если `EnsureGLContext()` вернул `false`, помечает тест
пропущенным и бросает `TestFailure` с текстом `"файл:строка: skipped"`.
Предназначена для вызова из вспомогательных функций, куда нельзя вставить
макрос `ENG_REQUIRE_GL()`.

* **Ограничение:** сам макрос `ENG_REQUIRE_GL()` эту функцию не вызывает — он
  работает через `EnsureGLContext()` и `ENG_SKIP`. Поведение у обоих путей
  одинаковое: тест считается пропущенным.

```cpp
// Хелпер, который сам решает, доступен ли GPU.
void UploadTestTextureIfPossible(const char* file, int line) {
    crossrender::test::RequireGLContext(file, line);   // бросит, если контекста нет
    // ... работа с GL ...
}
```

### `struct RunResult`

Итог прогона: сколько тестов прошло, упало и было пропущено, сколько это
заняло секунд и список сообщений о падениях. Возвращается из `RunAll`.

```cpp
crossrender::test::RunResult result;
result.passed = 10;
result.failed = 1;
result.failures.push_back("Net.TcpLoopback: connect refused");
ENG_LOGI("test", "провалов: %d", result.failed);
```

### `int passed = 0`

Число успешно выполненных тестов. Пропущенные сюда не попадают.

```cpp
crossrender::test::RunResult result;
if (result.passed == 0 && result.failed == 0) {
    ENG_LOGW("test", "фильтр не совпал ни с одним тестом");
}
```

### `int failed = 0`

Число провалов. Код возврата процесса устроен так, что `failed == 0`
соответствует успеху.

```cpp
const int exitCode = (result.failed == 0) ? 0 : 1;
ENG_LOGI("test", "код возврата: %d", exitCode);
```

### `int skipped = 0`

Число пропущенных тестов — тех, что вызвали `ReportSkip` /
`ENG_SKIP` / `ENG_REQUIRE_GL()`. Пропуск не делает прогон неуспешным.

```cpp
if (result.skipped > 0) {
    ENG_LOGW("test", "пропущено %d тестов: скорее всего, нет GL-контекста", result.skipped);
}
```

### `double seconds = 0`

Полное время прогона в секундах, измеренное вокруг цикла тестов. Считается и
при пустом результате.

```cpp
ENG_LOGI("test", "прогон занял %.2f с", result.seconds);
```

### `std::vector<std::string> failures`

Сообщения о падениях в формате `"Сьют.Кейс: текст ошибки"`. Раннер печатает их
отдельным блоком `Failures:` после сводки.

```cpp
for (const std::string& failure : result.failures) {
    ENG_LOGE("test", "%s", failure.c_str());
}
```

### `RunResult RunAll(const std::string& filter = "", bool verbose = false)`

Выполняет все зарегистрированные тесты. `filter` — подстрока строки
`"Сьют.Кейс"`; пустая строка означает «все». `verbose` включает печать времени
каждого теста. Перед циклом создаётся один общий контекст OpenGL, после каждого
теста счётчик ссылок уменьшается.

* **Возвращает:** `RunResult` со сводкой; функция никогда не бросает исключений
  от тестов — они перехватываются внутри.
* **Контекст:** вызывайте один раз за процесс. Повторный вызов возможен, но
  тесты выполнятся заново.

```cpp
const crossrender::test::RunResult all = crossrender::test::RunAll();
ENG_LOGI("test", "итого: passed %d, failed %d, skipped %d", all.passed, all.failed,
         all.skipped);

// Отдельный прогон только сьюта Net, с подробностями.
const crossrender::test::RunResult net = crossrender::test::RunAll("Net", true);
ENG_LOGI("test", "сеть: %d успешных за %.2f с", net.passed, net.seconds);
```

### `bool EnsureGLContext()`

Гарантирует наличие offscreen-контекста OpenGL и загруженных функций GL.
Если контекст уже есть, просто увеличивает счётчик ссылок; иначе создаёт его
через `CreateHeadlessGLContext()` и вызывает
`gl::LoadFunctions(HeadlessGLGetProcAddress)`. При неудаче возвращает `false`,
предварительно уничтожив частично созданный контекст.

* **Возвращает:** `true`, если контекст готов к использованию.
* **Контекст:** вызывайте парно с `ReleaseGLContext()`.

```cpp
if (crossrender::test::EnsureGLContext()) {
    ENG_LOGI("test", "GPU готов, шейдеров в кэше: %d", countCachedShaders());
    crossrender::test::ReleaseGLContext();
} else {
    ENG_LOGW("test", "GPU недоступен — тест следовало бы пропустить");
}
```

### `void ReleaseGLContext()`

Уменьшает счётчик ссылок и уничтожает offscreen-контекст, когда счётчик
достиг нуля. Безопасно вызывать, когда контекста нет: счётчик не уходит ниже
нуля.

```cpp
crossrender::test::EnsureGLContext();
// ... работа с GL ...
crossrender::test::ReleaseGLContext();   // контекст живёт, пока на него есть ссылки
```

### `std::string TempFilePath(const char* name)`

Возвращает путь к временному файлу в подкаталоге `crossrender-tests`
системного каталога временных файлов (`TMPDIR`/`TEMP`). Каталог создаётся
записью пустого файла `.keep`. Файлы не удаляются автоматически — это
ответственность теста.

* **Возвращает:** `"<temp>/crossrender-tests/<name>"`.
* **Контекст:** предпочитайте этот путь пользовательскому корню: временный
  каталог доступен на запись и в песочнице, и в CI.

```cpp
const std::string path = crossrender::test::TempFilePath("probe.bin");
const std::vector<crossrender::u8> bytes{1, 2, 3};
ENG_CHECK(crossrender::WriteBinaryFile(path, bytes.data(), bytes.size()));
ENG_CHECK(crossrender::FileExists(path));
```

### `f32 RandomFloat01(u32 seed)`

Детерминированное «случайное» число в диапазоне `[0, 1)` для одного и того же
`seed`. Внутри создаётся `crossrender::Random(seed)` и берётся `NextFloat()`, поэтому
последовательность воспроизводима между запусками и платформами.

```cpp
const crossrender::f32 first = crossrender::test::RandomFloat01(1234);
const crossrender::f32 again = crossrender::test::RandomFloat01(1234);
ENG_CHECK_NEAR(first, again, 1e-6f);   // сид детерминирован
```

### `ENG_TEST(suite, name)`

Объявляет тест. Разворачивается в статическую функцию `void()` с уникальным
именем, статический `Registrar` (регистрация до `main`) и заголовок функции,
после которого идёт тело теста в фигурных скобках. Оба аргумента должны быть
идентификаторами C++: они попадают и в имя символа, и в строки в кавычках.

```cpp
ENG_TEST(Vector, CrossIsPerpendicular) {
    const crossrender::Vec3 a{1, 0, 0};
    const crossrender::Vec3 b{0, 1, 0};
    const crossrender::Vec3 c = crossrender::Cross(a, b);
    ENG_CHECK_NEAR(c.z, 1.0f, 1e-5f);
}
```

### `ENG_TEST_FAIL(msg)`

Немедленно проваливает тест с произвольным сообщением: вызывает
`ReportFailure`, который бросает `TestFailure`. Удобно для веток, где проверка
не выражается условием.

```cpp
ENG_TEST(Parser, UnknownToken) {
    if (!parser.Handles(token)) {
        ENG_TEST_FAIL("парсер не умеет разбирать токен '" + token + "'");
    }
}
```

### `ENG_CHECK(cond)`

Основная проверка: если `cond` ложно, тест падает с сообщением
`"check failed: <текст условия>"`. Выражение подставляется в сообщение как
есть, поэтому пишите его так, чтобы оно читалось.

```cpp
ENG_TEST(Input, KeyEdgesAreVisible) {
    crossrender::Input in;
    in.BeginFrame();
    in.OnKey(crossrender::Key::W, crossrender::KeyAction::Press, false);
    ENG_CHECK(in.KeyDown(crossrender::Key::W));
    ENG_CHECK(in.KeyPressed(crossrender::Key::W));
}
```

### `ENG_CHECK_MSG(cond, msg)`

Как `ENG_CHECK`, но добавляет собственное сообщение — обычно значения
переменных. Строка собирается через `std::string`, поэтому подойдёт
`std::to_string` или конкатенация.

```cpp
ENG_TEST(Net, Listen) {
    crossrender::TcpSocket server;
    ENG_CHECK_MSG(server.Listen(0), "Listen не удался: " + server.LastError());
}
```

### `ENG_CHECK_EQ(a, b)`

Проверяет `a == b`. Значения не печатаются, поэтому при падении в сообщении
будет только `"expected a == b"`; для чисел с плавающей точкой используйте
`ENG_CHECK_NEAR`.

```cpp
ENG_TEST(Math, IntAddition) {
    ENG_CHECK_EQ(2 + 2, 4);
    ENG_CHECK_EQ(sizeof(crossrender::u8), static_cast<crossrender::usize>(1));
}
```

### `ENG_CHECK_NEAR(a, b, eps)`

Проверяет, что `|a - b| <= eps`. Оба аргумента приводятся к `double`, а при
падении в сообщение попадают фактически полученные значения.

```cpp
ENG_TEST(Math, NormalizeKeepsDirection) {
    const crossrender::Vec3 n = crossrender::Normalize(crossrender::Vec3{3, 0, 4});
    ENG_CHECK_NEAR(n.x, 0.6f, 1e-5f);
    ENG_CHECK_NEAR(n.z, 0.8f, 1e-5f);
}
```

### `ENG_CHECK_STR_EQ(a, b)`

Сравнивает две строки (`std::string`), при падении печатает обе в кавычках.
В отличие от `ENG_CHECK_EQ`, работает и с `const char*`, потому что оба
аргумента приводятся к `std::string`.

```cpp
ENG_TEST(Net, AddressToString) {
    const crossrender::NetAddress a = crossrender::NetAddress::Parse("127.0.0.1", 8080);
    ENG_CHECK_STR_EQ(a.ToString(), "127.0.0.1:8080");
}
```

### `ENG_CHECK_GT(a, b)`

Проверяет `a > b`. Тип выводится через `auto`, поэтому оба аргумента должны
быть одного типа (или приводиться друг к другу).

```cpp
// Счётчики трафика лежат в NetStats: после обмена bytesSent больше нуля.
crossrender::TcpSocket socket;
// ... успешный Connect/Send ...
ENG_CHECK_GT(socket.Stats().bytesSent + 1, static_cast<crossrender::u64>(0));
```

### `ENG_CHECK_LT(a, b)`

Проверяет `a < b`.

```cpp
ENG_TEST(Time, SleepTakesTime) {
    const crossrender::f64 t0 = crossrender::NowSeconds();
    crossrender::SleepMs(2);
    ENG_CHECK_LT(t0, crossrender::NowSeconds());
}
```

### `ENG_CHECK_LE(a, b)`

Проверяет `a <= b`. Удобна для границ диапазонов, где равенство допустимо.

```cpp
ENG_TEST(Gfx, AlphaStaysInRange) {
    const crossrender::f32 alpha = ComputeAlpha(0.5f);
    ENG_CHECK_LE(0.0f, alpha);
    ENG_CHECK_LE(alpha, 1.0f);
}
```

### `ENG_CHECK_GE(a, b)`

Проверяет `a >= b`.

```cpp
ENG_TEST(Platform, AtLeastOneCore) {
    ENG_CHECK_GE(crossrender::CpuCoreCount(), 1);
}
```

### `ENG_SKIP(reason)`

Помечает тест пропущенным и **возвращает управление из теста** (`return`
внутри макроса). Причина попадает в вывод раннера. Используйте, когда условие
зависит от платформы или ресурса.

```cpp
ENG_TEST(Window, Create) {
    crossrender::Window window;
    crossrender::WindowDesc desc;
    desc.width = 320;
    desc.height = 240;
    if (!window.Create(desc)) ENG_SKIP("нет дисплея");
    ENG_CHECK_GT(window.Width(), 0);
}
```

### `ENG_REQUIRE_GL()`

Гарантирует, что у теста есть контекст OpenGL: вызывает
`EnsureGLContext()` и, если контекст недоступен, делает
`ENG_SKIP("no GL context available")`. Благодаря этому GPU-тест на машине без
графики **пропускается чисто**, а не падает и не мешает прогону.

* **Контекст:** ставьте первой строкой теста, до любых вызовов GL.
* **Важно:** контекст, созданный здесь, освобождает раннер после теста —
  вызывать `DestroyHeadlessGLContext()` самому не нужно и вредно.

```cpp
ENG_TEST(Shader, CompilesTrivialProgram) {
    ENG_REQUIRE_GL();   // без GPU тест будет пропущен, а не провален

    const unsigned int shader = crossrender::gl::glCreateShader(crossrender::gl::GL_VERTEX_SHADER);
    ENG_CHECK(shader != 0);
    crossrender::gl::glDeleteShader(shader);
}
```

## Пример целиком

```cpp
#include "crossrender/core/File.h"
#include "crossrender/core/Log.h"
#include "crossrender/gfx/GL.h"
#include "crossrender/platform/Platform.h"
#include "crossrender/test/Test.h"

// Сьюта целиком: обычный файл tests/test_my_feature.cpp не требует ни
// регистрации, ни объявлений — всё делает макрос ENG_TEST.

ENG_TEST(MyFeature, DetectsGpu) {
    // GPU-тест: на машине без графики он честно уйдёт в "skipped".
    ENG_REQUIRE_GL();
    ENG_CHECK(crossrender::gl::glCreateShader != nullptr);
}

ENG_TEST(MyFeature, TempFileRoundTrip) {
    const std::string path = crossrender::test::TempFilePath("my_feature_probe.bin");
    const std::vector<crossrender::u8> payload{0xDE, 0xAD, 0xBE, 0xEF};

    ENG_CHECK_MSG(crossrender::WriteBinaryFile(path, payload.data(), payload.size()),
                  "не удалось записать " + path);
    const crossrender::ByteBuffer back = crossrender::ReadBinaryFile(path);
    ENG_CHECK_EQ(back.size(), payload.size());
    ENG_CHECK(back == payload);
}

ENG_TEST(MyFeature, DeterministicRandom) {
    // Один и тот же сид обязан давать одно и то же число.
    ENG_CHECK_NEAR(crossrender::test::RandomFloat01(7), crossrender::test::RandomFloat01(7), 1e-6f);
    ENG_CHECK_GE(crossrender::test::RandomFloat01(7), 0.0f);
    ENG_CHECK_LT(crossrender::test::RandomFloat01(7), 1.0f);
}

ENG_TEST(MyFeature, SkipsWhenPlatformUnsuitable) {
    // Пропуск вместо провала: условие зависит от окружения, а не от кода.
    if (crossrender::PlatformName() == "Web") {
        ENG_SKIP("на Web этот сценарий не поддерживается");
    }
    ENG_CHECK(!crossrender::PlatformName().empty());
}

// Готовый раннер: так выглядит tests/test_main.cpp в миниатюре.
int main(int argc, char** argv) {
    std::string filter;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-v" || arg == "--verbose") verbose = true;
        else if (arg.rfind("--filter=", 0) == 0) filter = arg.substr(9);
        else if (arg[0] != '-') filter = arg;
    }

    crossrender::LogSetLevel(crossrender::LogLevel::Error);   // чтобы логи движка не мешали отчёту
    const crossrender::test::RunResult result = crossrender::test::RunAll(filter, verbose);

    ENG_LOGI("test", "passed %d, failed %d, skipped %d за %.2f с", result.passed,
             result.failed, result.skipped, result.seconds);
    for (const std::string& failure : result.failures) ENG_LOGE("test", "%s", failure.c_str());
    return result.failed == 0 ? 0 : 1;
}
```

## См. также

* `docs/platform/Platform.md` — `CreateHeadlessGLContext` и
  `HeadlessGLGetProcAddress`, на которых стоит `EnsureGLContext`.
* `docs/gfx/GL.md` — `gl::LoadFunctions` и функции, которые вызывают
  GPU-тесты.
* `docs/core/File.md` — `WriteBinaryFile` / `ReadBinaryFile` и временный
  каталог, куда указывает `TempFilePath`.
* `docs/core/Log.md` — уровни логирования, которыми управляет `--loglevel`.
* `docs/core/Math.md` — `crossrender::Random`, которым пользуется `RandomFloat01`.
