# crossrender/core/File.h — виртуальная файловая система, пути и корни ресурсов

Абстракция файловой системы движка: чтение и запись ассетов и пользовательских
данных, разбор путей и два корня — `assets` (только чтение) и `user` (запись).

## Заголовок

```cpp
#include "crossrender/core/File.h"
```

## Обзор

`File.h` — это тонкий слой над платформенным вводом-выводом. Интерфейс
`FileSystem` описывает шесть операций (чтение, запись, проверка, листинг,
разрешение пути, время изменения), а глобальный экземпляр подставляет
платформенный слой. Если его никто не подставил, `crossrender::FS()` лениво создаёт
встроенную реализацию поверх `std::filesystem` — именно она используется в
сборках для настольных платформ.

Пути записываются **только через `/`**, независимо от платформы. Разрешение
пути при чтении (`FileSystem::ResolvePath`) идёт в таком порядке:

1. абсолютный путь (`/...`, а в Windows — `C:...`) используется как есть;
2. путь, начинающийся с `user/`, заменяется на `<userRoot>/...`;
3. `<assetRoot>/<путь>`, если такой файл существует;
4. `<assetRoot>/<путь без префикса assets/>`, если путь начинался с `assets/`;
5. `<путь>` относительно текущего рабочего каталога, если он существует;
6. иначе возвращается пустая строка — «не найдено».

Запись (`FileSystem::WriteFile`) устроена иначе: абсолютный путь пишется как
есть, `user/...` — в пользовательский корень, `assets/...` — в корень ассетов
(если он задан), а всё остальное — тоже в пользовательский корень. Родительские
каталоги создаются автоматически.

Такое разделение удобно держать в голове: **читать** можно из ассетов и из
пользовательских данных, а **писать** по умолчанию — только в пользовательский
корень. Именно поэтому сохранения, настройки и скриншоты складывают через пути
вида `user/saves/slot1.json`.

Порядок работы:

1. На старте движок задаёт корни: `crossrender::SetAssetRoot` (из настроек или
   командной строки) и `crossrender::SetUserRoot`.
2. Игровой код читает ресурсы через `crossrender::ReadTextFile` / `crossrender::ReadBinaryFile`
   и проверяет существование через `crossrender::FileExists`.
3. Запись идёт через `crossrender::WriteTextFile` / `crossrender::WriteBinaryFile` либо через
   `crossrender::FS().WriteFile`, если данные уже лежат в памяти как `ByteBuffer`.

Если корни не заданы явно, `GetAssetRoot` подбирает каталог `assets` (или
`examples/assets` — так ассеты лежат внутри папки примера) обходом вверх от
исполняемого файла (до шести уровней), а `GetUserRoot` — платформенный каталог
данных приложения (см. описание ниже).

## Члены класса

### `using ByteBuffer = std::vector<u8>`

Буфер сырых байтов — то, чем движок обменивается с файловой системой. Используется
для текстур, звука, шрифтов и любых бинарных ассетов.

```cpp
// Читаем бинарный ассет и смотрим его размер, не разбирая содержимое.
crossrender::ByteBuffer blob = crossrender::ReadBinaryFile("assets/textures/checker.png");
if (blob.empty()) {
    ENG_LOGW("assets", "checker.png не найден — рисую заглушку");
} else {
    ENG_LOGI("assets", "checker.png: %d байт", static_cast<int>(blob.size()));
}
```

### `class FileSystem`

Интерфейс файловой системы. Наследуйтесь от него, чтобы подменить ввод-вывод
целиком: тестовый виртуальный диск, упакованный архив, сетевой поток. Экземпляр
устанавливается через `FileSystemBind`, владение остаётся за вызывающим кодом —
объект должен жить, пока на него смотрит движок.

```cpp
// Обёртка, считающая обращения: удобно, чтобы найти забытые чтения из кадра.
class CountingFs final : public crossrender::FileSystem {
public:
    explicit CountingFs(crossrender::FileSystem& inner) : inner_(inner) {}

    bool ReadFile(const std::string& path, crossrender::ByteBuffer* out) override {
        ++reads_;
        return inner_.ReadFile(path, out);
    }
    bool WriteFile(const std::string& path, const void* data, crossrender::usize size) override {
        return inner_.WriteFile(path, data, size);
    }
    bool Exists(const std::string& path) override { return inner_.Exists(path); }
    std::vector<std::string> ListDir(const std::string& path) override { return inner_.ListDir(path); }
    std::string ResolvePath(const std::string& path) override { return inner_.ResolvePath(path); }

    int reads() const { return reads_; }

private:
    crossrender::FileSystem& inner_;
    int reads_ = 0;
};
```

### `virtual ~FileSystem() = default`

Виртуальный деструктор. Он позволяет удалять конкретную реализацию через
указатель на базовый тип — без него `delete` по `FileSystem*` был бы
неопределённым поведением.

```cpp
std::unique_ptr<crossrender::FileSystem> fs = std::make_unique<CountingFs>(crossrender::FS());
// ... работаем ...
fs.reset();   // деструктор CountingFs вызывается через базовый указатель
```

### `bool ReadFile(const std::string& path, ByteBuffer* out)`

Читает файл целиком в `out`, предварительно изменяя его размер под длину файла.
Путь разрешается по общим правилам чтения (ассеты, `user/`, рабочий каталог).

* **Возвращает:** `true`, если файл открыт и прочитан.
* **Контекст:** при ошибке `out` не трогается; путь, который не удалось
  разрешить, тоже даёт `false`.

```cpp
crossrender::ByteBuffer data;
if (!crossrender::FS().ReadFile("assets/fonts/ui.ttf", &data)) {
    ENG_LOGE("font", "не удалось прочитать ui.ttf (%d байт не загружено)",
             static_cast<int>(data.size()));
    return false;
}
ENG_LOGI("font", "ui.ttf загружен: %d байт", static_cast<int>(data.size()));
```

### `bool WriteFile(const std::string& path, const void* data, usize size)`

Пишет `size` байт из `data` по пути `path`. Родительские каталоги создаются
автоматически. Относительный путь без префикса попадает в пользовательский
корень, `assets/...` — в корень ассетов, абсолютный — пишется как есть.

* **Возвращает:** `true`, если запись прошла успешно; при неудаче пишет
  `ENG_LOGE("fs", ...)`.

```cpp
const std::string json = "{\"level\":1}";
if (!crossrender::FS().WriteFile("user/saves/slot1.json", json.data(), json.size())) {
    ENG_LOGE("save", "слот 1 не сохранён");
}
// Результат: <userRoot>/saves/slot1.json, каталог saves создан автоматически
```

### `bool Exists(const std::string& path)`

Проверяет, что путь разрешается и существует на диске.

* **Возвращает:** `true` и для файла, и для каталога — метод не различает их.

```cpp
bool TryEnterLevel(crossrender::SceneManager& scenes, const std::string& name) {
    const std::string path = "assets/scenes/" + name + ".scene";
    if (!crossrender::FS().Exists(path)) {
        ENG_LOGW("scene", "%s отсутствует — остаёмся в текущей сцене", path.c_str());
        return false;
    }
    return scenes.SetScene(name);   // сцена зарегистрирована под именем name
}
```

### `std::vector<std::string> ListDir(const std::string& path)`

Возвращает содержимое каталога — **только имена**, без пути. Порядок обхода не
определён: сортируйте сами, если он важен. Неразрешимый путь даёт пустой вектор,
а не ошибку.

```cpp
// Перебираем все сохранения в пользовательском корне.
std::vector<std::string> files = crossrender::FS().ListDir("user/saves");
std::sort(files.begin(), files.end());
for (const std::string& name : files) {
    if (crossrender::PathExt(name) != ".json") continue;
    ENG_LOGI("save", "найден слот '%s'", name.c_str());
}
```

### `std::string ResolvePath(const std::string& path)`

Превращает логический путь движка в путь файловой системы по правилам чтения из
обзора. Это же метод используется всеми остальными операциями чтения.

* **Возвращает:** реальный путь или **пустую строку**, если путь не разрешился.
* **Ограничение:** метод проверяет существование, поэтому им нельзя «вычислить
  будущий путь» для файла, который ещё не создан. Для записи используйте
  соглашение о корнях (`user/...`).

```cpp
const std::string real = crossrender::FS().ResolvePath("assets/audio/hit.wav");
if (real.empty()) {
    ENG_LOGW("audio", "hit.wav не найден, звук удара отключён");
} else {
    ENG_LOGI("audio", "hit.wav разрешился в %s", real.c_str());
}
```

### `i64 FileTime(const std::string& path)`

Метка времени последнего изменения файла. Базовый класс возвращает `0`, поэтому
проверяйте значение, если реализация может её не поддерживать.

* **Возвращает:** счётчик `std::filesystem::file_time_type` или `0`, если файл
  не разрешился.
* **Ограничение:** начало отсчёта определяется стандартной библиотекой и не
  совпадает с Unix-временем. Пригодно для сравнения «новее/старее», но не для
  вывода даты.

```cpp
const crossrender::i64 cached = crossrender::FS().FileTime("user/cache/atlas.bin");
const crossrender::i64 source = crossrender::FS().FileTime("assets/atlas.json");
if (cached == 0 || cached < source) {
    ENG_LOGI("assets", "кэш атласа устарел — пересобираем");
}
```

### `void FileSystemBind(FileSystem* fs)`

Устанавливает процесс-глобальный экземпляр файловой системы. Вызывается
платформенным слоем на старте. Владение не передаётся: объект должен жить, пока
движок на него ссылается. Если передать `nullptr`, следующий вызов `crossrender::FS()`
снова создаст встроенную реализацию.

```cpp
CountingFs counting(crossrender::FS());
crossrender::FileSystemBind(&counting);   // с этого момента все чтения идут через счётчик
crossrender::ReadTextFile("assets/ui/theme.json");
ENG_LOGI("fs", "прочитано %d файлов", counting.reads());
crossrender::FileSystemBind(nullptr);     // возвращаемся к встроенной реализации
```

### `FileSystem& FS()`

Доступ к текущей файловой системе. Если платформенный слой ничего не
подставил, лениво создаётся встроенная реализация поверх `std::filesystem`.
Ссылка живёт до конца процесса — сохранять её надолго безопасно.

```cpp
// Один раз получаем ссылку и работаем с ней напрямую.
crossrender::FileSystem& fs = crossrender::FS();
if (fs.Exists("assets/ui/theme.json")) {
    ENG_LOGI("ui", "тема найдена");
}
```

### `std::string ReadTextFile(const std::string& path)`

Удобная обёртка над `FileSystem::ReadFile`: читает файл и возвращает его как
строку.

* **Возвращает:** содержимое файла или **пустую строку** при любой ошибке —
  отличить пустой файл от отсутствующего по результату нельзя.

```cpp
const std::string ttfConfig = crossrender::ReadTextFile("assets/fonts/ui.cfg");
if (ttfConfig.empty()) {
    ENG_LOGW("font", "ui.cfg пуст или отсутствует — берём настройки по умолчанию");
}
```

### `ByteBuffer ReadBinaryFile(const std::string& path)`

Читает файл целиком в буфер байтов. Основной способ загрузки текстур, звуков и
моделей.

* **Возвращает:** буфер или пустой вектор при ошибке.

```cpp
crossrender::ByteBuffer png = crossrender::ReadBinaryFile("assets/textures/hero.png");
if (png.empty()) {
    ENG_LOGE("gfx", "текстура героя не загрузилась");
} else {
    texture.LoadFromMemory(png.data(), png.size());
}
```

### `bool WriteTextFile(const std::string& path, const std::string& text)`

Записывает строку в файл. Сокращение для `WriteBinaryFile` с `text.data()`.

```cpp
const std::string log = ReportAsJson(report);
if (!crossrender::WriteTextFile("user/logs/last_run.json", log)) {
    ENG_LOGW("log", "не удалось сохранить отчёт о запуске");
}
```

### `bool WriteBinaryFile(const std::string& path, const void* data, usize size)`

Записывает произвольный блок памяти. Используется для скриншотов, кэшей и
сериализованных сцен.

```cpp
std::vector<crossrender::u8> pixels = framebuffer.ReadPixels();
if (!crossrender::WriteBinaryFile("user/screenshots/frame0.raw", pixels.data(), pixels.size())) {
    ENG_LOGE("gfx", "скриншот не сохранён");
}
```

### `bool FileExists(const std::string& path)`

Свободная функция-обёртка над `FileSystem::Exists`. Проверяет и файлы, и
каталоги.

```cpp
// Путь можно проверить до регистрации сцены в SceneManager.
if (crossrender::FileExists("assets/scenes/level01.scene")) {
    scenes.Register("level01", [] { return std::make_unique<Level01>(); });
} else {
    ENG_LOGW("scene", "файл сцены не найден, уровень будет недоступен");
}
```

### `bool DirectoryExists(const std::string& path)`

Проверяет, что по пути находится именно каталог. В отличие от `FileExists`, для
файла вернёт `false`.

```cpp
if (!crossrender::DirectoryExists("user/saves")) {
    ENG_LOGI("save", "каталог сохранений ещё не создан");
}
```

### `bool CreateDirectories(const std::string& path)`

Создаёт каталог и все недостающие родительские — аналог `mkdir -p`. Безопасно
вызывать повторно.

* **Возвращает:** `true`, если каталог существует **после** вызова.
* **Важно:** путь разрешается как путь чтения. Относительный путь без префикса
  `user/` и без совпадения в ассетах создастся рядом с рабочим каталогом, а не в
  пользовательском корне. Для пользовательских данных всегда пишите
  `CreateDirectories("user/...")`.

```cpp
if (!crossrender::CreateDirectories("user/cache")) {
    ENG_LOGE("assets", "не удалось создать user/cache — кэширование отключено");
}
```

### `std::string PathJoin(const std::string& a, const std::string& b)`

Склеивает два пути, вставляя `/` только при необходимости. Если `b` —
абсолютный путь, он побеждает и `a` игнорируется. Нормализации не выполняет:
`..` и двойные слэши остаются как есть (для них есть `PathNormalize`).

```cpp
const std::string dir  = crossrender::PathDir("assets/scenes/level01.scene");   // "assets/scenes"
const std::string full = crossrender::PathJoin(dir, "level01.light.json");
ENG_LOGI("scene", "ищем свет в %s", full.c_str());
```

### `std::string PathDir(const std::string& p)`

Возвращает каталог пути — всё до последнего слэша.

* **Возвращает:** `"."`, если слэшей нет, и `"/"` для пути в корне.

```cpp
// Рядом с дескриптором атласа лежат страницы текстур.
const std::string base = crossrender::PathDir("assets/atlas/hero.json");   // "assets/atlas"
const crossrender::ByteBuffer page = crossrender::ReadBinaryFile(crossrender::PathJoin(base, "hero_0.png"));
```

### `std::string PathBase(const std::string& p)`

Возвращает последний компонент пути — имя файла без каталогов.

```cpp
ENG_LOGI("assets", "загружаем %s", crossrender::PathBase("assets/scenes/level01.scene").c_str());
// в лог уйдёт "level01.scene"
```

### `std::string PathExt(const std::string& p)`

Возвращает расширение файла в нижнем регистре, **вместе с точкой**.

* **Возвращает:** например `".png"` или `".json"`; пустую строку, если точки нет.
* **Ограничение:** у файла-точки вроде `.gitignore` расширением считается всё имя
  целиком, потому что точка стоит первой.

```cpp
// Разные загрузчики по расширению.
const std::string ext = crossrender::PathExt(entry);
if (ext == ".json") {
    LoadSpriteAtlas(entry);
} else if (ext == ".png") {
    LoadPlainTexture(entry);
} else {
    ENG_LOGW("assets", "неизвестное расширение '%s' у %s", ext.c_str(), entry.c_str());
}
```

### `std::string PathNormalize(const std::string& p)`

Приводит путь к лексически нормальному виду: убирает `./`, схлопывает `//` и
разбирает `..`, а затем срезает завершающие слэши. Файловая система не
опрашивается — это чисто текстовая операция, поэтому `..` «съедает» предыдущий
компонент даже для несуществующих каталогов.

* **Примеры:** `"a/../b"` → `"b"`, `"./a//b/"` → `"a/b"`, `"a/b/.."` → `"a"`,
  `"/x/y/"` → `"/x/y"`.
* **Ведущие `..` сохраняются:** `"../a"` так и останется `"../a"`.

```cpp
// Командная строка может прийти с завершающим слэшем и "./".
const std::string raw = "../CrossRender/assets/";
ENG_LOGI("fs", "корень ассетов: %s", crossrender::PathNormalize(raw).c_str());
```

### `void SetAssetRoot(const std::string& root)`

Задаёт корень ассетов — каталог только для чтения, который едет вместе с
приложением. Значение прогоняется через `PathNormalize`; существование каталога
не проверяется, поэтому опечатка проявится позже, как «файл не найден».

```cpp
// Приложение получило --assets=... ; иначе корень подберётся автоматически.
if (!options.assetsPath.empty()) {
    crossrender::SetAssetRoot(options.assetsPath);
}
ENG_LOGI("engine", "asset root: %s", crossrender::GetAssetRoot().c_str());
```

### `const std::string& GetAssetRoot()`

Возвращает текущий корень ассетов. Если его не задавали, корень подбирается один
раз и запоминается: обход вверх до шести уровней от каталога исполняемого файла
в поисках папки `assets`, затем `assets` в рабочем каталоге, затем просто
`"assets"`.

```cpp
// Дочерние подсистемы получают уже готовый корень.
resources_->SetAssetRoot(crossrender::GetAssetRoot());
ENG_LOGI("engine", "ресурсы читаются из %s", crossrender::GetAssetRoot().c_str());
```

### `void SetUserRoot(const std::string& root)`

Задаёт пользовательский корень — единственное место, куда движок пишет по
умолчанию. Путь нормализуется, и каталог **сразу создаётся**, если его нет.

```cpp
// Тесты и CI часто хотят писать в отдельный каталог.
crossrender::SetUserRoot("/tmp/gameengine-tests");
ENG_LOGI("fs", "пишем в %s", crossrender::GetUserRoot().c_str());
```

### `const std::string& GetUserRoot()`

Возвращает пользовательский корень, подбирая его при первом обращении:
`%APPDATA%/CrossRender` в Windows, `~/Library/Application Support/CrossRender` в
macOS, `/tmp/CrossRender` на iOS, Android и WASM, `~/.local/share/CrossRender` в
Linux. Каталог создаётся, проверяется пробной записью и при неудаче заменяется
каталогом во временной папке — с предупреждением `ENG_LOGW("fs", ...)`.

```cpp
const std::string saveDir = crossrender::PathJoin(crossrender::GetUserRoot(), "saves");
crossrender::CreateDirectories(saveDir);
ENG_LOGI("save", "сохранения: %s", saveDir.c_str());
```

### `bool FetchUrl(const std::string& url, ByteBuffer* out)`

Загрузка по сети для потоковой подгрузки ассетов. По заголовку предполагалась
реализация только для WASM.

**Текущее состояние: это заглушка.** В `engine/src/core/File.cpp` функция
безусловно возвращает `false` на **всех** платформах, включая WASM, и не
изменяет `out`. Никакого HTTP-клиента внутри нет — не рассчитывайте на сетевую
загрузку и обрабатывайте отказ.

```cpp
crossrender::ByteBuffer payload;
if (!crossrender::FetchUrl("https://example.com/levels/level02.json", &payload)) {
    // Так и есть сегодня: на любой платформе. Держите локальную копию ассета.
    ENG_LOGW("assets", "FetchUrl недоступен (заглушка) — читаем локальный файл");
    payload = crossrender::ReadBinaryFile("assets/levels/level02.json");
}
```

## Пример целиком

```cpp
#include "crossrender/core/File.h"
#include "crossrender/core/Json.h"
#include "crossrender/core/Log.h"

#include <string>

// Сохранение и загрузка слота: настройки лежат в user/, схема — в assets/.
struct SaveSlot {
    int level = 1;
    float volume = 0.8f;
};

bool SaveGame(const SaveSlot& slot, int index) {
    if (!crossrender::CreateDirectories("user/saves")) {          // mkdir -p в пользовательском корне
        ENG_LOGE("save", "каталог сохранений недоступен");
        return false;
    }

    crossrender::JsonValue root = crossrender::JsonMakeObject();
    root.Set("level", crossrender::JsonValue(slot.level));
    root.Set("volume", crossrender::JsonValue(static_cast<double>(slot.volume)));

    const std::string path = "user/saves/slot" + std::to_string(index) + ".json";
    if (!crossrender::WriteTextFile(path, root.Dump(2))) {
        ENG_LOGE("save", "слот %d не записан", index);
        return false;
    }
    ENG_LOGI("save", "слот %d сохранён в %s", index, path.c_str());
    return true;
}

bool LoadGame(SaveSlot* slot, int index) {
    const std::string path = "user/saves/slot" + std::to_string(index) + ".json";
    crossrender::JsonValue root;
    std::string error;
    if (!crossrender::JsonValue::ParseFile(path, &root, &error)) {
        ENG_LOGW("save", "слот %d не прочитан: %s", index, error.c_str());
        return false;
    }
    slot->level  = root.GetInt("level", 1);
    slot->volume = root.GetFloat("volume", 0.8f);
    return true;
}

void Demo() {
    // На старте: корень ассетов из настроек, пользовательский — по умолчанию.
    crossrender::SetAssetRoot("assets");
    ENG_LOGI("engine", "asset root: %s", crossrender::GetAssetRoot().c_str());
    ENG_LOGI("engine", "user root:  %s", crossrender::GetUserRoot().c_str());

    // Перечисляем доступные схемы уровней и печатаем их имена без каталогов.
    for (const std::string& name : crossrender::FS().ListDir("assets/levels")) {
        if (crossrender::PathExt(name) != ".json") continue;
        const std::string full = crossrender::PathJoin("assets/levels", name);
        ENG_LOGI("assets", "уровень '%s' (%s)", crossrender::PathBase(full).c_str(),
                 full.c_str());
    }

    SaveSlot slot;
    slot.level = 3;
    if (SaveGame(slot, 1)) {
        SaveSlot restored;
        if (LoadGame(&restored, 1)) {
            ENG_LOGI("save", "восстановлен уровень %d, громкость %.2f",
                     restored.level, restored.volume);
        }
    }
}
```

## См. также

* `docs/core/Json.md` — разбор и запись документов, которыми обмениваются
  `ReadTextFile` / `WriteTextFile`.
* `docs/core/Log.md` — макросы `ENG_LOGI` / `ENG_LOGE`, которыми принято
  сопровождать файловые операции.
* `docs/core/Base.md` — тип `crossrender::usize` и макросы платформы, влияющие на
  подбор корней.
