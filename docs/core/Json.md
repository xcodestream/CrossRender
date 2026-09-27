# crossrender/core/Json.h — минимальный DOM-разбор и запись JSON

Разбор и генерация JSON без внешних зависимостей: одно значение `JsonValue`,
плюс псевдонимы массива и объекта. Используется для Lottie-анимаций, файлов
сцен, дескрипторов атласов и сетевых сообщений.

## Заголовок

```cpp
#include "crossrender/core/Json.h"
```

## Обзор

Заголовок даёт ровно одну сущность — `JsonValue`, значение JSON любого типа.
Тип хранится внутри (`JsonValue::Type`), массивы и объекты лежат в
`std::shared_ptr`, скаляры — прямо в объекте. Сверху есть два псевдонима —
`JsonArray` (`std::vector<JsonValue>`) и `JsonObject`
(`std::map<std::string, JsonValue>`) — и два конструктора-помощника
`JsonMakeArray` / `JsonMakeObject`.

Читать документ можно двумя способами:

```cpp
crossrender::JsonValue root = crossrender::JsonValue::Parse(text);   // строка
crossrender::JsonValue root;                                  // или сразу из файла
if (!crossrender::JsonValue::ParseFile("assets/scenes/level01.json", &root, &error)) { ... }
```

Дальше документ обходят через `Find` / `operator[]` и безопасные геттеры с
значением по умолчанию (`GetInt`, `GetFloat`, `GetBool`, `GetString`) — они не
бросают исключений и не падают на отсутствующем ключе.

Собирать документ удобно так: `JsonMakeObject` / `JsonMakeArray` создают пустой
контейнер, `Set` кладёт поле, `Push` добавляет элемент массива, `Dump` печатает
результат.

Порядок работы:

1. Разбирайте файл через `JsonValue::ParseFile` — он сам читает его из
   виртуальной файловой системы (`docs/core/File.md`) и возвращает ошибку в
   понятном виде.
2. Проверяйте тип перед доступом: `Find` возвращает указатель, поэтому
   конструкция `if (const JsonValue* v = root.Find("size"); v && v->IsArray())`
   безопасна.
3. Для чисел и строк предпочитайте геттеры с `def`: частичный или устаревший
   документ не должен ронять загрузку.
4. Для записи используйте `Dump(2)` — читаемый файл с отступами.

Две особенности, о которых стоит помнить сразу:

* копия `JsonValue` **разделяет** массив или объект с оригиналом — это
  `shared_ptr`, а не глубокое копирование;
* `Dump` печатает ключи объекта в лексикографическом порядке, потому что
  `JsonObject` — это `std::map`.

## Члены класса

### `using JsonArray = std::vector<JsonValue>`

Массив JSON. Обычный `std::vector`, поэтому доступны все его операции, а индексы
принимает и сам `JsonValue`.

```cpp
crossrender::JsonArray layers;
layers.push_back(crossrender::JsonValue("background"));
layers.push_back(crossrender::JsonValue("hero"));

crossrender::JsonValue doc = crossrender::JsonMakeObject();
doc.Set("layers", crossrender::JsonValue(layers));
ENG_LOGI("scene", "слоёв в документе: %d", static_cast<int>(layers.size()));
```

### `using JsonObject = std::map<std::string, JsonValue>`

Объект JSON: отсортированное по ключу отображение. Порядок ключей при обходе —
лексикографический, а не тот, в котором они были записаны в файле.

```cpp
crossrender::JsonObject stats;
stats["draws"] = crossrender::JsonValue(42);
stats["tris"]  = crossrender::JsonValue(1024);

for (const auto& kv : stats) {
    ENG_LOGI("gfx", "статистика %s = %d", kv.first.c_str(), kv.second.AsInt());
}
```

### `class JsonValue`

Значение JSON любого типа: `null`, `bool`, число, строка, массив или объект.
Копирование стоит дёшево, но массив и объект при копировании разделяются —
глубокая копия делается через `Dump` + `Parse`.

```cpp
// Проверка сообщения, пришедшего из сети: доверять ему нельзя.
std::string error;
crossrender::JsonValue msg = crossrender::JsonValue::Parse(payload, &error);
if (!error.empty() || !msg.IsObject()) {
    ENG_LOGW("net", "сообщение не разобрано как объект: %s", error.c_str());
    return;
}

crossrender::JsonValue copy = msg;                 // массив и объект разделяются с оригиналом
copy.Set("handled", crossrender::JsonValue(true)); // то же изменение видно и в msg
const std::string type = msg.GetString("type");
if (type == "spawn" && msg.Find("pos")) {
    SpawnEntity(msg.GetFloat("x"), msg.GetFloat("y"));
}
```

### `enum class Type`

Тип значения внутри `JsonValue`. Возвращается методом `GetType` и лежит в основе
всех предикатов `Is*`.

| Значение | Смысл |
|---|---|
| `Type::Null` | отсутствие значения, значение по умолчанию |
| `Type::Bool` | логическое `true` / `false` |
| `Type::Number` | число; хранится как `double`, независимо от того, как записано |
| `Type::String` | строка UTF-8 |
| `Type::Array` | массив, доступен через `Array()` |
| `Type::Object` | объект, доступен через `Object()` |

```cpp
crossrender::JsonValue v = crossrender::JsonValue::Parse("[1, 2, 3]");
switch (v.GetType()) {
    case crossrender::JsonValue::Type::Array:
        ENG_LOGI("json", "массив из %d элементов", static_cast<int>(v.Size()));
        break;
    case crossrender::JsonValue::Type::Object:
        ENG_LOGI("json", "объект");
        break;
    default:
        ENG_LOGW("json", "ожидался массив или объект");
        break;
}
```

### `JsonValue()`

Конструкторы. Первый создаёт `null`; остальные перегрузки задают тип по
аргументу: `nullptr_t`, `bool`, `double`, `int`, `i64`, `u32` (все числа
приводятся к `double`), `const char*`, `std::string`, `JsonArray`, `JsonObject`.
`const char*` равный `nullptr` превращается в пустую строку.

```cpp
crossrender::JsonValue nothing;                                   // null
crossrender::JsonValue alive(true);                               // bool
crossrender::JsonValue volume(0.75);                              // number (double)
crossrender::JsonValue lives(3);                                  // number (int)
crossrender::JsonValue name("hero");                              // string
crossrender::JsonValue tags(crossrender::JsonArray{"player", "friendly"}); // array
crossrender::JsonValue empty(crossrender::JsonObject{});                   // object

ENG_LOGI("scene", "%s: жив=%d, громкость=%.2f", name.AsString().c_str(),
         alive.AsBool(), volume.AsNumber());
```

### `Type GetType() const`

Возвращает тип значения. Удобно, когда вариантов больше двух и `switch` читается
лучше цепочки `Is*`.

```cpp
crossrender::JsonValue doc = crossrender::JsonValue::Parse("{\"speed\": 3.5}");
const crossrender::JsonValue& node = doc["speed"];
const crossrender::JsonValue::Type t = node.GetType();
if (t == crossrender::JsonValue::Type::Number) {
    ENG_LOGI("json", "узел — число %f", node.AsNumber());
}
```

### `bool IsNull() const`

Проверяет, что значение — `null`. Возвращает `true` и для значения по
умолчанию, поэтому «ключа нет» и «ключ равен null» различайте через `Find`.

```cpp
crossrender::JsonValue v = crossrender::JsonValue::Parse("null");
if (v.IsNull()) {
    ENG_LOGI("json", "значение отсутствует, используем настройки по умолчанию");
}
```

### `bool IsBool() const`

Проверяет, что значение — логическое.

```cpp
if (settings.Find("vsync") && settings.Find("vsync")->IsBool()) {
    ENG_LOGI("gfx", "vsync = %d", settings.GetBool("vsync"));
}
```

### `bool IsNumber() const`

Проверяет, что значение — число. Истинно для целых и дробных чисел: отдельного
целочисленного типа в JSON нет.

```cpp
const crossrender::JsonValue* fpsCap = settings.Find("fpsCap");
if (fpsCap && fpsCap->IsNumber()) {
    ENG_LOGI("gfx", "ограничение %d кадров/с", fpsCap->AsInt(60));
}
```

### `bool IsString() const`

Проверяет, что значение — строка.

```cpp
if (entry.Find("path") && entry.Find("path")->IsString()) {
    LoadAsset(entry.GetString("path"));
}
```

### `bool IsArray() const`

Проверяет, что значение — массив. Перед обходом по индексам это обязательная
проверка: у значения другого типа `Size()` вернёт `0`.

```cpp
crossrender::JsonValue frames = crossrender::JsonValue::Parse("[0, 1, 2, 3]");
if (frames.IsArray()) {
    for (crossrender::usize i = 0; i < frames.Size(); ++i) {
        ENG_LOGI("anim", "кадр %d -> %d", static_cast<int>(i), frames[i].AsInt());
    }
}
```

### `bool IsObject() const`

Проверяет, что значение — объект. Только у объекта работают `Has`, `Find` и
геттеры по ключу.

```cpp
crossrender::JsonValue root = crossrender::JsonValue::Parse("{\"meta\":{\"author\":\"me\"}}");
if (const crossrender::JsonValue* meta = root.Find("meta"); meta && meta->IsObject()) {
    ENG_LOGI("scene", "автор сцены: %s", meta->GetString("author", "неизвестен").c_str());
}
```

### `bool AsBool(bool def = false) const`

Возвращает логическое значение, а `def` — если тип другой.

```cpp
const bool fullscreen = root["video"].AsBool(false);
ENG_LOGI("gfx", "полноэкранный режим: %d", fullscreen);
```

### `double AsNumber(double def = 0) const`

Возвращает число двойной точности, а `def` — если это не число. Основной способ
достать значение, когда точность важнее краткости.

```cpp
const double duration = track["duration"].AsNumber(0.0);
ENG_LOGI("anim", "длительность анимации: %.3f с", duration);
```

### `f32 AsFloat(f32 def = 0) const`

То же, что `AsNumber`, но сразу приводится к `f32` — типу графики движка.

```cpp
const crossrender::f32 scale = node.AsFloat(1.0f);
ENG_LOGI("anim", "масштаб слоя: %.3f", scale);
```

### `i32 AsInt(i32 def = 0) const`

Возвращает целое. Дробная часть **отбрасывается** приведением `static_cast`, без
округления: `2.9` даст `2`.

```cpp
const crossrender::i32 columns = atlas["columns"].AsInt(1);
ENG_LOGI("gfx", "колонок в атласе: %d", columns);
```

### `const std::string& AsString() const`

Ссылка на внутреннюю строку. Для значения другого типа вернёт ссылку на пустую
строку, а не ошибку.

```cpp
crossrender::JsonValue tag = crossrender::JsonValue::Parse("\"ui\"");
ENG_LOGI("json", "тег: %s", tag.AsString().c_str());
```

### `std::string AsString(const std::string& def) const`

Возвращает строку или `def`, если значение строкой не является. Безопасный
вариант для разбора внешних документов.

```cpp
const std::string shader = material.AsString("default");
ENG_LOGI("gfx", "материал использует шейдер '%s'", shader.c_str());
```

### `usize Size() const`

Размер массива или объекта. Для всех остальных типов — `0`.

```cpp
ENG_LOGI("anim", "слоёв: %d, свойств: %d",
         static_cast<int>(layers.Size()), static_cast<int>(props.Size()));
```

### `bool Empty() const`

`true`, если `Size() == 0`. У скаляров тоже `true`.

```cpp
if (frames.Empty()) {
    ENG_LOGW("anim", "в документе нет ни одного кадра");
}
```

### `const JsonValue& operator[](usize i) const`

Доступ к элементу массива по индексу. Если значение не массив или индекс вне
диапазона, возвращается общее значение `null` — исключений не будет.

```cpp
const crossrender::JsonValue& first = frames[0];
ENG_LOGI("anim", "первый кадр: %d", first.AsInt(-1));
```

### `JsonValue& operator[](usize i)`

Изменяемый доступ к элементу массива. Если индекс за концом, массив
**расширяется** до нужного размера. Если значение не массив, возвращается
статическая заглушка, и запись в неё теряется — сначала приведите значение к
массиву через `Push` или присваивание `JsonArray`.

```cpp
crossrender::JsonValue frames = crossrender::JsonMakeArray();
frames[0] = crossrender::JsonValue(10);   // массив вырастет до одного элемента
frames[1] = crossrender::JsonValue(20);
ENG_LOGI("anim", "кадров после записи: %d", static_cast<int>(frames.Size()));
```

### `bool Has(const std::string& key) const`

Проверяет наличие ключа в объекте. Возвращает `true`, даже если значение по
ключу — `null`. Для не-объекта всегда `false`.

```cpp
if (config.Has("audio")) {
    ENG_LOGI("audio", "секция audio присутствует");
} else {
    ENG_LOGW("audio", "секция audio отсутствует — берём настройки по умолчанию");
}
```

### `const JsonValue& operator[](const std::string& key) const`

Доступ к полю объекта по ключу. Отсутствующий ключ даёт общее значение `null`.

```cpp
const crossrender::JsonValue& meta = root["meta"];
ENG_LOGI("scene", "версия формата: %d", meta["version"].AsInt(1));
```

### `JsonValue& operator[](const std::string& key)`

Изменяемый доступ к полю объекта. **Если значение не объект, оно молча
превращается в объект**, а прежнее содержимое теряется. Это удобно для сборки
документа с нуля и опасно для правки чужого значения.

```cpp
crossrender::JsonValue doc;                 // null
doc["title"] = crossrender::JsonValue("Level 1");   // стал объектом
doc["music"]["track"] = crossrender::JsonValue("theme.ogg");  // вложенный объект
ENG_LOGI("scene", "документ: %s", doc.Dump().c_str());
```

### `f32 GetFloat(const std::string& key, f32 def = 0) const`

Число поля как `f32`, иначе `def`. Ключ ищется через `Find`, поэтому метод
работает только с объектами.

```cpp
const crossrender::f32 gravity = physics.GetFloat("gravity", 9.8f);
ENG_LOGI("phys", "гравитация: %.2f", gravity);
```

### `i32 GetInt(const std::string& key, i32 def = 0) const`

Целое поле, иначе `def`. Дробная часть отбрасывается, а не округляется.

```cpp
const crossrender::i32 width = tileset.GetInt("tileWidth", 16);
ENG_LOGI("gfx", "ширина тайла: %d", width);
```

### `bool GetBool(const std::string& key, bool def = false) const`

Логическое поле, иначе `def`.

```cpp
const bool loop = clip.GetBool("loop", true);
ENG_LOGI("anim", "зацикливание клипа: %d", loop);
```

### `std::string GetString(const std::string& key, const std::string& def = "") const`

Строковое поле. Если ключа нет **или** значение не строка (например, число),
возвращается `def`.

```cpp
ENG_LOGI("scene", "следующая сцена: %s", scene.GetString("next", "none").c_str());
```

### `const JsonValue* Find(const std::string& key) const`

Ищет поле и возвращает указатель на значение или `nullptr`, если ключа нет либо
значение не объект. Самый безопасный способ заглянуть в необязательную секцию.

```cpp
if (const crossrender::JsonValue* pbr = material.Find("pbrMetallicRoughness")) {
    const crossrender::f32 metallic = pbr->GetFloat("metallicFactor", 1.0f);
    ENG_LOGI("gfx", "metallic = %.3f", metallic);
} else {
    ENG_LOGI("gfx", "PBR-секции нет, материал считается диэлектриком");
}
```

### `const JsonObject& Object() const`

Ссылка на объект целиком. Для не-объекта — ссылка на пустую статическую карту,
так что обход безопасен.

```cpp
for (const auto& kv : root.Object()) {
    ENG_LOGI("scene", "ключ '%s'", kv.first.c_str());
}
```

### `const JsonArray& Array() const`

Ссылка на массив целиком. Для не-массива — пустой статический вектор.

```cpp
const crossrender::JsonArray& items = inventory.Array();
ENG_LOGI("game", "предметов в инвентаре: %d", static_cast<int>(items.size()));
```

### `void Push(JsonValue v)`

Добавляет элемент в конец массива. Если значение не массив, оно **превращается в
массив**, а прежнее содержимое теряется.

```cpp
crossrender::JsonValue layers = crossrender::JsonMakeArray();
layers.Push(crossrender::JsonValue("background"));
layers.Push(crossrender::JsonValue("hero"));
layers.Push(crossrender::JsonValue("ui"));
ENG_LOGI("scene", "слоёв: %d", static_cast<int>(layers.Size()));
```

### `void Set(const std::string& key, JsonValue v)`

Кладёт поле в объект, перезаписывая существующее. Не-объект превращается в
объект, прежнее содержимое теряется.

```cpp
crossrender::JsonValue profile = crossrender::JsonMakeObject();
profile.Set("name", crossrender::JsonValue("player"));
profile.Set("level", crossrender::JsonValue(7));
profile.Set("level", crossrender::JsonValue(8));   // перезапись: останется 8
ENG_LOGI("game", "профиль: %s", profile.Dump().c_str());
```

### `std::string Dump(int indent = -1) const`

Печатает значение в текст. При `indent < 0` получается компактная строка без
пробелов; при `indent >= 0` — многострочный вывод, где каждый уровень вложенности
сдвинут на `indent` пробелов, а после `:` ставится пробел.

* **Числа:** целые (по модулю меньше `1e15`) печатаются без дробной части,
  остальные — с девятью значащими цифрами (`%.9g`).
* **Строки:** экранируются `\"`, `\\`, `\n`, `\r`, `\t`, `\b`, `\f`, а
  управляющие символы — как `\uXXXX`.
* **Пустые контейнеры:** `[]` и `{}` независимо от отступа.
* **Порядок ключей:** лексикографический (`std::map`), исходный порядок файла не
  сохраняется.

```cpp
crossrender::JsonValue settings = crossrender::JsonMakeObject();
settings.Set("volume", crossrender::JsonValue(0.75));
settings.Set("layers", crossrender::JsonValue(crossrender::JsonArray{1, 2}));

// settings.Dump(-1):  {"layers":[1,2],"volume":0.75}
// settings.Dump(2):
// {
//   "layers": [
//     1,
//     2
//   ],
//   "volume": 0.75
// }
ENG_LOGI("save", "пишем настройки:\n%s", settings.Dump(2).c_str());
```

### `static JsonValue Parse(const std::string& text, std::string* error = nullptr)`

Разбирает JSON из строки. Поддерживается весь стандартный синтаксис: объекты,
массивы, строки с escape-последовательностями (включая `\uXXXX` и суррогатные
пары), числа с дробной частью, экспонентой и знаком, литералы `true` / `false` /
`null`. Лишние символы после значения — ошибка.

* **Возвращает:** разобранное значение или `null` при ошибке.
* **Ошибка:** в `*error` попадает текст вида `"unexpected end of input at offset 6"`
  (только первое сообщение, и только если строка ещё пуста). Дополнительно
  пишется `ENG_LOGW("json", ...)`.
* **Ограничения:** комментариев и висячих запятых нет; `NaN` и `Infinity`
  отвергаются; дубликаты ключей не считаются ошибкой — побеждает последний; при
  ошибке результат неотличим от корректного `null`, поэтому проверяйте `error`.

```cpp
std::string error;
crossrender::JsonValue root = crossrender::JsonValue::Parse("{\"name\": \"level01\"", &error);
if (!error.empty()) {
    ENG_LOGE("json", "документ повреждён: %s", error.c_str());
} else {
    ENG_LOGI("json", "сцена '%s'", root.GetString("name").c_str());
}
```

### `static bool ParseFile(const std::string& path, JsonValue* out, std::string* error = nullptr)`

Читает файл через `ReadTextFile` и разбирает его. Основной способ загрузки
дескрипторов: атласов, сцен, Lottie-анимаций.

* **Возвращает:** `true` при успехе; тогда `*out` получает документ.
* **Ошибка:** пустой файл или ненайденный путь дают
  `"file not found or empty: <path>"`, синтаксическая ошибка — сообщение
  парсера. При `false` значение `*out` не изменяется.
* **Важно:** пустой файл считается ошибкой, поэтому «пустой JSON» так загрузить
  нельзя.

```cpp
crossrender::JsonValue atlas;
std::string error;
if (!crossrender::JsonValue::ParseFile("assets/atlas/hero.json", &atlas, &error)) {
    ENG_LOGW("assets", "атлас героя не загружен: %s", error.c_str());
    return false;
}
ENG_LOGI("assets", "в атласе %d страниц", static_cast<int>(atlas["pages"].Size()));
```

### `JsonValue JsonMakeArray()`

Создаёт пустой массив JSON. Эквивалент `JsonValue(JsonArray{})`.

```cpp
crossrender::JsonValue queue = crossrender::JsonMakeArray();
queue.Push(crossrender::JsonValue("resume"));
ENG_LOGI("game", "очередь: %s", queue.Dump().c_str());   // ["resume"]
```

### `JsonValue JsonMakeObject()`

Создаёт пустой объект JSON. Эквивалент `JsonValue(JsonObject{})`. Удобная точка
старта для документа, который затем наполняется через `Set`.

```cpp
crossrender::JsonValue meta = crossrender::JsonMakeObject();
meta.Set("generator", crossrender::JsonValue("CrossRender"));
meta.Set("version", crossrender::JsonValue(1));
ENG_LOGI("scene", "метаданные: %s", meta.Dump().c_str());
```

## Пример целиком

```cpp
#include "crossrender/core/File.h"
#include "crossrender/core/Json.h"
#include "crossrender/core/Log.h"

#include <string>

// Настройки игрока: собираем документ, пишем его в user/ и читаем обратно.
struct Settings {
    float musicVolume = 0.8f;
    float sfxVolume = 1.0f;
    int   fpsCap = 60;
    bool  vsync = true;
    std::string locale = "ru";
    std::vector<std::string> recentScenes{"level01"};
};

crossrender::JsonValue ToJson(const Settings& s) {
    crossrender::JsonValue root = crossrender::JsonMakeObject();
    root.Set("musicVolume", crossrender::JsonValue(static_cast<double>(s.musicVolume)));
    root.Set("sfxVolume", crossrender::JsonValue(static_cast<double>(s.sfxVolume)));
    root.Set("fpsCap", crossrender::JsonValue(s.fpsCap));
    root.Set("vsync", crossrender::JsonValue(s.vsync));
    root.Set("locale", crossrender::JsonValue(s.locale));

    crossrender::JsonValue recent = crossrender::JsonMakeArray();
    for (const std::string& scene : s.recentScenes) recent.Push(crossrender::JsonValue(scene));
    root.Set("recentScenes", recent);
    return root;
}

Settings FromJson(const crossrender::JsonValue& root, const Settings& defaults) {
    Settings s = defaults;                              // частичный документ допустим
    s.musicVolume = root.GetFloat("musicVolume", s.musicVolume);
    s.sfxVolume   = root.GetFloat("sfxVolume", s.sfxVolume);
    s.fpsCap      = root.GetInt("fpsCap", s.fpsCap);
    s.vsync       = root.GetBool("vsync", s.vsync);
    s.locale      = root.GetString("locale", s.locale);

    if (const crossrender::JsonValue* recent = root.Find("recentScenes");
        recent && recent->IsArray()) {
        s.recentScenes.clear();
        for (const crossrender::JsonValue& item : recent->Array()) {
            if (item.IsString()) s.recentScenes.push_back(item.AsString());
        }
    }
    return s;
}

void Demo() {
    Settings settings;
    settings.recentScenes.push_back("level02");

    // 1. Собираем документ и пишем его в пользовательский корень.
    const std::string path = "user/settings.json";
    const std::string text = ToJson(settings).Dump(2);
    if (!crossrender::WriteTextFile(path, text)) {
        ENG_LOGE("settings", "настройки не сохранены в %s", path.c_str());
        return;
    }
    ENG_LOGI("settings", "записано:\n%s", text.c_str());

    // 2. Читаем обратно и разбираем.
    crossrender::JsonValue root;
    std::string error;
    if (!crossrender::JsonValue::ParseFile(path, &root, &error)) {
        ENG_LOGE("settings", "не удалось прочитать настройки: %s", error.c_str());
        return;
    }

    // 3. Проверяем типы, прежде чем доверять значениям.
    if (root.GetType() != crossrender::JsonValue::Type::Object || !root.Has("musicVolume")) {
        ENG_LOGW("settings", "файл не похож на настройки — берём значения по умолчанию");
    }

    const Settings loaded = FromJson(root, Settings{});
    ENG_LOGI("settings", "громкость музыки %.2f, fps-лимит %d, язык '%s', сцен в истории %d",
             loaded.musicVolume, loaded.fpsCap, loaded.locale.c_str(),
             static_cast<int>(loaded.recentScenes.size()));
}
```

## См. также

* `docs/core/File.md` — виртуальная файловая система, из которой `ParseFile`
  читает документы и в которую пишет `WriteTextFile`.
* `docs/core/Log.md` — предупреждение `ENG_LOGW("json", ...)`, которое парсер
  выдаёт при синтаксической ошибке.
* `docs/core/Base.md` — типы `crossrender::i32`, `crossrender::f32`, `crossrender::usize`, используемые в
  подписях `JsonValue`.
