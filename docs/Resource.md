# crossrender/Resource.h — кэш ресурсов

`ResourceCache` — единственный владелец ресурсов, загружаемых с диска:
текстур, шрифтов, моделей, звуковых клипов и шейдеров. Каждый ресурс
загружается один раз на своё сочетание параметров и переиспользуется по
строковому ключу, а вызывающий код получает указатель, которым владеет кэш.

## Заголовок

```cpp
#include "crossrender/Resource.h"
```

## Обзор

Кэш устроен как пять независимых таблиц (`textures_`, `fonts_`, `models_`,
`audio_`, `shaders_`), в каждой — по одной записи `Entry<T>`: сам ресурс,
путь и время изменения файла. Геттеры `Texture_`, `Font_`, `Model_`,
`Audio_`, `Shader_` следуют одному сценарию:

1. Строится ключ и выполняется поиск в таблице — при попадании сразу
   возвращается сохранённый указатель, файл повторно не читается.
2. Иначе создаётся пустой ресурс, путь склеивается с корнем ассетов
   (`PathJoin(assetRoot_, path)`) и вызывается загрузчик.
3. При успехе запись попадает в таблицу, и возвращается «сырой» указатель на
   объект внутри неё; кэш владеет объектом до `Clear()` / `Shutdown()`.

Отсюда два практических следствия. Во-первых, указатель **устойчив**: пока
запись жива, он не меняется, поэтому его можно спокойно хранить в сцене.
Во-вторых, указатель **не продлевает жизнь** ресурсу: после `Clear()` или
`Shutdown()` он становится висячим.

```cpp
crossrender::ResourceCache cache;
cache.SetAssetRoot(crossrender::GetAssetRoot());

// Второй вызов не читает файл заново: это тот же указатель.
crossrender::Texture* first = cache.Texture_("textures/tile.png");
crossrender::Texture* second = cache.Texture_("textures/tile.png");
ENG_LOGI("res", "один и тот же ресурс: %s", (first == second) ? "да" : "нет");
```

### Ключи кэша

Ключ — не всегда просто путь: параметры загрузки входят в него, иначе
ресурсы с разными настройками перепутались бы.

| Геттер | Ключ |
|---|---|
| `Texture_` | `path` плюс `"\|srgb"` при `srgb = true` |
| `Font_` | `path \| pixelHeight \| sdf \| atlasSize` (строка формата `"%s\|%.1f\|%d\|%d"`) |
| `Model_`, `Audio_`, `Shader_` | сам `path` / `basePath` |
| `ShaderFromSource` | синтетический `key`, который задаёт вызывающий |

`Shader_` и `ShaderFromSource` пишут в **одну** таблицу `shaders_`, поэтому
синтетический ключ не должен совпадать с базовым путём файлового шейдера.

```cpp
// Два разных ключа — две разные текстуры в кэше.
crossrender::Texture* linear = cache.Texture_("textures/ui.png", /*srgb=*/false);
crossrender::Texture* srgb = cache.Texture_("textures/ui.png", /*srgb=*/true);
ENG_LOGI("res", "ресурсов в кэше: %d", static_cast<int>(cache.Count()));
```

### Пути и корень ассетов

Все геттеры принимают путь **относительно корня ассетов**. Корень задаётся
`SetAssetRoot` (в `Engine::Init` он выставляется в `GetAssetRoot()`) и
приклеивается к пути через `PathJoin`. Абсолютный или неверный путь даст
ошибку загрузки, а не «тихий» пропуск: в лог уходит строка категории `res`.

При ошибке загрузки запись в таблицу **не** добавляется и возвращается
`nullptr`. Это значит, что следующий вызов `Texture_` / `Font_` / … снова
попробует прочитать файл — полезно, если ассет появился на диске позже, но
вредно, если файла нет вовсе: ошибка будет повторяться каждый кадр.

```cpp
crossrender::Texture* tex = cache.Texture_("textures/missing.png");
if (tex == nullptr) {
    // Файл не найден: запись не создана, следующий вызов повторит попытку.
    ENG_LOGW("res", "ассет недоступен, использую белую текстуру");
    tex = cache.WhiteTexture();
}
```

### Что важно знать

* Кэш **не потокобезопасен**: загрузка идёт из того потока, который вызвал
  геттер, и обычно это поток рендера.
* `Engine` создаёт собственный `ResourceCache` в `Engine::Init` и отдаёт его
  через `Engine::Resources()`. Отдельный экземпляр нужен только в тестах или
  утилитах.
* `ReloadChanged` и `Count` — «честные» счётчики: см. описание каждого ниже,
  у обоих есть неочевидные ограничения.
* Процедурные текстуры (`WhiteTexture`, `BlackTexture`, `NormalFlatTexture`)
  создаются лениво и лежат **вне** пяти таблиц, поэтому в `Count()` не
  попадают и `Clear()` их не трогает.

```cpp
crossrender::ResourceCache cache;
cache.SetAssetRoot(crossrender::GetAssetRoot());

// Процедурные текстуры живут отдельно от таблиц и переживают Clear().
crossrender::Texture* white = cache.WhiteTexture();
cache.Clear();
ENG_ASSERT(white != nullptr && white->Valid());
```

## Члены класса

### `ResourceCache()` / `~ResourceCache()`

Конструктор по умолчанию: кэш пуст, корень ассетов не задан. Деструктор
освобождает все записи через `unique_ptr`, но **не** вызывает `Shutdown()` —
если в кэше есть ресурсы GPU, лучше уничтожить их явно, пока контекст
OpenGL ещё жив.

```cpp
{
    crossrender::ResourceCache cache;
    cache.SetAssetRoot("assets");
    crossrender::Texture* tex = cache.Texture_("textures/tile.png");
    (void)tex;
    cache.Shutdown();   // освобождаем GL-ресурсы при живом контексте
}   // здесь деструктор уже не найдёт ни одной записи
```

### `Texture* Texture_(const std::string& path, bool srgb = false)`

Загружает текстуру из файла относительно корня ассетов. `srgb = true`
помечает её как цветовую (sRGB) — так загружаются albedo-текстуры; карты
нормалей, масок и шероховатости берутся с `false`. Успешная загрузка
запоминает отладочное имя, равное исходному пути. Возвращает `nullptr` при
ошибке и пишет её в категорию `res`.

```cpp
crossrender::Texture* albedo = cache.Texture_("textures/brick_albedo.png", /*srgb=*/true);
crossrender::Texture* normal = cache.Texture_("textures/brick_normal.png", /*srgb=*/false);
crossrender::Texture* fallback = cache.WhiteTexture();
crossrender::Material mat = crossrender::Material::Default();
mat.baseColorTex = albedo ? albedo : fallback;
mat.normalTex = normal ? normal : cache.NormalFlatTexture();
```

### `Font* Font_(const std::string& path, const FontDesc& desc = {})`

Загружает TTF/OTF и растеризует глифы по описанию `FontDesc`. Именно
`pixelHeight`, `sdf` и `atlasSize` входят в ключ кэша, поэтому один и тот же
файл в двух размерах — это два независимых ресурса. Для интерфейса обычно
берут шрифт по умолчанию из `Engine::DefaultFont()`, а кэш — для особых
начертаний.

```cpp
crossrender::FontDesc desc;
desc.pixelHeight = 64.0f;
desc.sdf = true;          // сглаживание при любом масштабе
desc.atlasSize = 2048;

crossrender::Font* title = cache.Font_("fonts/Heading.ttf", desc);
if (title == nullptr) {
    ENG_LOGW("res", "заголовочный шрифт недоступен, беру шрифт движка");
    title = crossrender::FontManager::Get().DefaultFont();
}
```

### `Model* Model_(const std::string& path)`

Загружает модель (glTF/OBJ и другие поддерживаемые форматы) и сразу
выгружает её на GPU через `UploadToGpu()` — отдельный вызов не нужен.
Меши модели лежат в её собственных буферах, а материалы и текстуры
загружаются загрузчиком по мере необходимости.

```cpp
crossrender::Model* model = cache.Model_("models/robot.glb");
if (model != nullptr && model->Valid()) {
    for (int i = 0; i < model->MeshCount(); ++i) {
        ctx.r3d->Draw(model->GpuMesh(i), model->MaterialAt(i), crossrender::Mat4::Identity());
    }
}
```

### `AudioClip* Audio_(const std::string& path)`

Загружает звуковой клип в память (WAV/OGG/MP3 — что поддержано сборкой).
Клип не запускается сам: его нужно передать в микшер через `Audio::Play` или
`Audio::PlayMusic`. Загружайте клипы заранее, а не в момент выстрела: чтение
файла в кадре даст заметный рывок.

```cpp
crossrender::AudioClip* hit = cache.Audio_("audio/hit.wav");
if (hit != nullptr && hit->Valid()) {
    crossrender::PlayParams params;
    params.volume = 0.8f;
    params.spatial = false;   // обычный 2D-звук интерфейса
    crossrender::Audio::Get().Play(*hit, params);
}
```

### `Shader* Shader_(const std::string& basePath)`

Собирает шейдер из пары файлов `"<basePath>.vert"` и `"<basePath>.frag"`.
Путь задаётся без расширения: `Shader_("shaders/sprite")` читает
`shaders/sprite.vert` и `shaders/sprite.frag`. При ошибке компиляции
возвращает `nullptr`; текст ошибки уже выведен компилятором шейдеров в лог
категории `gl`.

```cpp
crossrender::Shader* sprite = cache.Shader_("shaders/sprite");
if (sprite == nullptr || !sprite->Valid()) {
    ENG_LOGE("res", "шейдер спрайта не собрался");
    return false;
}
sprite->Bind();
```

### `Shader* ShaderFromSource(const std::string& key, const char* vert, const char* frag)`

Собирает шейдер прямо из строк исходников. `key` — синтетическое имя для
кэша и для отладочных сообщений: повторный вызов с тем же ключом вернёт уже
собранную программу, **не** пересобирая её. Ключ живёт в общей таблице
шейдеров, поэтому называйте его так, чтобы он не совпал с путём файлового
шейдера (например, с префиксом `inline/`).

```cpp
static const char* kVignetteFrag =
    "in vec2 vUv;\n"
    "uniform sampler2D uTex;\n"
    "out vec4 oColor;\n"
    "void main() { float d = distance(vUv, vec2(0.5));\n"
    "  oColor = texture(uTex, vUv) * (1.0 - d * 0.6); }\n";

crossrender::Shader* shader =
    cache.ShaderFromSource("inline/vignette", crossrender::builtin::kPostVert, kVignetteFrag);
if (shader != nullptr) shader->Bind();
```

### `Texture* WhiteTexture()`

Лениво создаёт общую белую текстуру 1×1 и возвращает её. Нужна как
безопасная заглушка вместо ненайденного ассета и как множитель цвета в
материалах. Создаётся один раз на кэш и живёт до `Shutdown()`.

```cpp
crossrender::Texture* white = cache.WhiteTexture();
mat.baseColorTex = white;   // белая текстура не меняет цвет материала
```

### `Texture* BlackTexture()`

Лениво создаёт чёрную текстуру 1×1. Удобна как «нулевая» заглушка для карт,
которые не должны ничего добавлять (например, emissive-карта).

```cpp
crossrender::Texture* black = cache.BlackTexture();
mat.emissiveTex = black;    // эмиссии нет
```

### `Texture* NormalFlatTexture()`

Лениво создаёт текстуру нормалей 1×1 с пикселем `(128, 128, 255)` — это
«плоская» нормаль, направленная на наблюдателя. Используется вместо
отсутствующей normal map, чтобы освещение не ломалось.

```cpp
crossrender::Texture* flat = cache.NormalFlatTexture();
mat.normalTex = flat;
mat.normalStrength = 0.0f;   // и явно выключаем влияние карты нормалей
```

### `void SetAssetRoot(const std::string& root)`

Задаёт корень, от которого отсчитываются все пути геттеров. Обычно
выставляется один раз при старте; `Engine` делает это сам и берёт значение
из `GetAssetRoot()`. Смена корня не сбрасывает кэш: уже загруженные ресурсы
останутся, а новые пути будут разрешаться относительно нового каталога.

```cpp
cache.SetAssetRoot("/opt/game/assets");
ENG_LOGI("res", "корень ассетов: %s", cache.AssetRoot().c_str());
```

### `const std::string& AssetRoot() const`

Возвращает текущий корень ассетов — например, чтобы показать его в
отладочной панели или склеить путь для диагностического сообщения.

```cpp
if (tex == nullptr) {
    ENG_LOGE("res", "нет текстуры по пути %s",
             crossrender::PathJoin(cache.AssetRoot(), "textures/tile.png").c_str());
}
```

### `int ReloadChanged()`

Проходит по записям текстур, моделей и звука, сравнивает сохранённое время
изменения файла с текущим и возвращает число изменившихся файлов.

Здесь важно быть точным, потому что название обманчиво: **ресурс не
перезагружается**. Метод только замечает изменение и запоминает новое время,
после чего пишет одну строку в лог. Кроме того он **не смотрит** таблицы
шрифтов и шейдеров, а время берётся для пути в том виде, в каком он был
передан, — с непустым корнем ассетов файл может не найтись, и тогда
изменение не определится. Для настоящей горячей перезагрузки освобождайте
ресурс (`Clear()` или отдельный кэш) и загружайте заново.

```cpp
// Раз в секунду, например из Update сцены.
const int changed = cache.ReloadChanged();
if (changed > 0) {
    ENG_LOGW("res", "%d файлов изменились, но кэш ещё хранит старые данные", changed);
}
```

### `void Clear()`

Удаляет все записи из пяти таблиц. Процедурные текстуры (`WhiteTexture`,
`BlackTexture`, `NormalFlatTexture`) остаются. **Все ранее выданные
указатели становятся висячими** — после `Clear()` их нельзя ни разыменовать,
ни сравнить с чем-либо, кроме `nullptr`.

```cpp
// Перезагружаем все ассеты с диска.
cache.Clear();
crossrender::Texture* tex = cache.Texture_("textures/tile.png");
if (tex == nullptr) ENG_LOGE("res", "повторная загрузка не удалась");
```

### `void Shutdown()`

Вызывает `Clear()` и дополнительно освобождает три процедурные текстуры.
Вызывайте его при живом контексте OpenGL — деструктор текстур удаляет
GL-объекты. После `Shutdown()` кэш снова пуст и готов к работе: геттеры
создадут ресурсы заново.

```cpp
void OnContextLost(crossrender::ResourceCache& cache) {
    cache.Shutdown();
    ENG_LOGW("res", "контекст потерян, кэш ресурсов очищен");
}
```

### `usize Count() const`

Возвращает суммарное число записей во всех пяти таблицах. Процедурные
текстуры в подсчёт не входят. Растущее число — полезный сигнал утечки:
например, если ключ шейдера включает адрес или кадр.

```cpp
const crossrender::usize total = cache.Count();
ENG_LOGI("res", "в кэше %d ресурсов", static_cast<int>(total));
if (total > 2000) ENG_LOGW("res", "кэш растёт: проверьте ключи ShaderFromSource");
```

## Пример целиком

```cpp
#include "crossrender/Resource.h"
#include "crossrender/core/File.h"
#include "crossrender/core/Log.h"

#include <cstdio>
#include <string>
#include <vector>

// Загружает содержимое уровня и не падает, если часть ассетов отсутствует:
// каждый указатель проверяется, а вместо отсутствующих берутся заглушки.
class LevelAssets {
public:
    bool Load(crossrender::ResourceCache& cache, const std::string& name) {
        cache.SetAssetRoot(crossrender::GetAssetRoot());

        const std::string base = "levels/" + name + "/";
        tile_ = cache.Texture_(base + "tile.png", /*srgb=*/true);
        sprite_ = cache.Texture_(base + "sprite.png", /*srgb=*/true);
        model_ = cache.Model_(base + "level.glb");
        music_ = cache.Audio_(base + "theme.ogg");
        if (tile_ == nullptr) {
            ENG_LOGW("level", "нет текстуры пола в '%s', использую заглушку", name.c_str());
        }

        // Заглушки всегда доступны и не занимают места в таблицах кэша.
        fallback_ = cache.WhiteTexture();
        flatNormal_ = cache.NormalFlatTexture();
        return tile_ != nullptr || sprite_ != nullptr;
    }

    void Report(crossrender::ResourceCache& cache) const {
        const int missing = (tile_ ? 0 : 1) + (sprite_ ? 0 : 1) + (model_ ? 0 : 1) + (music_ ? 0 : 1);
        ENG_LOGI("level", "загружено, отсутствует %d из 4; всего в кэше %d", missing,
                 static_cast<int>(cache.Count()));
    }

    [[nodiscard]] crossrender::Texture* Tile() const { return tile_ ? tile_ : fallback_; }
    [[nodiscard]] crossrender::Texture* Sprite() const { return sprite_ ? sprite_ : fallback_; }
    [[nodiscard]] crossrender::Texture* FlatNormal() const { return flatNormal_; }

private:
    crossrender::Texture* tile_ = nullptr;
    crossrender::Texture* sprite_ = nullptr;
    crossrender::Model* model_ = nullptr;
    crossrender::AudioClip* music_ = nullptr;
    crossrender::Texture* fallback_ = nullptr;
    crossrender::Texture* flatNormal_ = nullptr;
};

int main() {
    crossrender::ResourceCache cache;
    LevelAssets assets;
    if (!assets.Load(cache, "crypt")) {
        ENG_LOGE("level", "уровень не загрузился");
        return 1;
    }
    assets.Report(cache);

    // Горячая перезагрузка: метод только сообщает об изменениях на диске.
    const int changed = cache.ReloadChanged();
    if (changed > 0) ENG_LOGW("level", "%d файлов изменились", changed);

    cache.Shutdown();   // освобождаем GPU-ресурсы до уничтожения контекста
    return 0;
}
```

## См. также

* `docs/core/File.md` — `GetAssetRoot()`, `GetUserRoot()`, `PathJoin` и
  файловые операции, на которых построен кэш.
* `docs/gfx/Texture.md` — что именно загружает `Texture_`, форматы и
  фильтрация.
* `docs/text/Font.md` — `FontDesc`, SDF и атласы глифов для `Font_`.
* `docs/gfx/Shader.md` — `Shader_` и `ShaderFromSource` изнутри: сборка
  программы и uniform-ы.
* `docs/audio/Audio.md` — `AudioClip`, `Play`, шины и микшер для `Audio_`.
* `docs/assets/Model.md` — модель, её меши и материалы для `Model_`.
* `docs/Engine.md` — `Engine::Resources()` и корень ассетов, который движок
  задаёт кэшу при старте.
