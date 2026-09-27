# crossrender/gfx/Texture.h — текстуры, форматы пикселей и режимы смешивания

Базовые графические типы, общие для всех рендереров: перечисление форматов
пикселей, фильтрация и адресация текстур, режимы смешивания и класс `Texture`
(двумерная текстура, 3D-текстура и cubemap).

## Заголовок

```cpp
#include "crossrender/gfx/Texture.h"
```

## Обзор

Заголовок делится на три части:

1. **Форматы и хелперы.** `PixelFormat` описывает внутренний формат GPU,
   а свободные функции `PixelFormatSize`, `PixelFormatIsDepth`,
   `PixelFormatIsFloat` позволяют посчитать размер пикселя и отличить формат
   глубины от цветового, не заглядывая в таблицы OpenGL.
2. **Состояние сэмплера.** `TextureFilter` (фильтрация) и `TextureWrap`
   (адресация, то есть поведение за пределами диапазона `0..1`) задаются при
   создании текстуры и меняются через `SetFilter` / `SetWrap`.
3. **`BlendMode` / `BlendState`.** Режим смешивания объявлен здесь, потому что
   это общий тип графического слоя; его используют `Renderer2D`, системы
   частиц и Lottie, а `Material` из `crossrender/gfx/Mesh.h` имеет только `AlphaMode`
   (см. раздел `BlendMode`).

`Texture` — это RAII-обёртка над именем текстуры OpenGL (`glGenTextures`).
Объект нельзя копировать, но можно перемещать; деструктор сам вызывает
`Destroy()`. Типичный порядок работы:

1. Создать: `Create`, `Create3D`, `CreateCubemap`, `LoadFromFile`,
   `LoadFromMemory`, `CreateSolid` или `CreateCheckerboard`.
2. При необходимости дописать пиксели через `Update` / `Update3D`.
3. Настроить сэмплер: `SetFilter`, `SetWrap`, `SetAnisotropy`.
4. Использовать в шейдере. **У `Texture` нет собственного `Bind`** — привязка
   к текстурному юниту выполняется через `Shader::SetTexture` (или вручную
   через `crossrender/gfx/GL.h`).
5. Освободить ресурс: `Destroy()` или деструктор.

Отдельная группа членов работает **на CPU и не требует контекста OpenGL**:
`ImageData`, `DecodeImage`, `DecodeImageFile` и `EncodePng`. Это позволяет
готовить и проверять ассеты в headless-режиме (без окна и драйвера).

#### Поведение без контекста OpenGL

Движок умеет работать headless (например, в тестах или в серверной сборке).
В этом режиме функции загрузки библиотеки OpenGL остаются пустыми указателями,
и создание GPU-ресурсов деградирует предсказуемо:

* `Create` пишет `ENG_LOGW("texture", "no GL context; texture %dx%d not created")`
  и возвращает `false`, оставляя `Valid() == false`;
* `Create3D` и `CreateCubemap` возвращают `false` без сообщения;
* `LoadFromFile` / `LoadFromMemory` в этом случае тоже возвращают `false`,
  потому что внутри вызывают `Create`;
* `Destroy`, `Update`, `Update3D`, `GenerateMipmaps`, `SetFilter`, `SetWrap`,
  `SetAnisotropy` при `id_ == 0` просто ничего не делают — падения нет;
* декодирование и кодирование изображений (`DecodeImage`, `DecodeImageFile`,
  `EncodePng`) **работает** и без GPU, потому что это код stb_image на CPU.

#### Честные ограничения

* `PixelFormat::Unknown` не является ошибкой для `Create`: `PixelFormatSize`
  вернёт `0`, а `ToGl` подставит `RGBA8`, при этом `Format()` продолжит
  возвращать `Unknown`. Не создавайте текстуры с этим форматом.
* `TextureWrap::ClampToBorder` не реализован как «бордюрный» режим — он
  молча отображается на `GL_CLAMP_TO_EDGE`, то есть ведёт себя как
  `ClampToEdge`.
* Для форматов глубины `Create` принудительно ставит `GL_NEAREST` и для
  `MAG_FILTER`, и для `MIN_FILTER`; аргумент `filter` в этом случае
  игнорируется.
* `SetFilter` для `MIN_FILTER` различает только `Nearest` и «всё остальное»
  (`GL_LINEAR`): мипмап-варианты через него не включить. Мипмапы включаются
  аргументом `mipmaps` у `Create` или вызовом `GenerateMipmaps`.
* Если запросить `filter = LinearMipmapLinear` при `mipmaps = false`,
  `MIN_FILTER` станет `GL_LINEAR_MIPMAP_LINEAR` без сгенерированных мипмапов —
  текстура будет считаться неполной и сэмплироваться как чёрная.
* `SetAnisotropy` всегда привязывает `GL_TEXTURE_2D`, поэтому для cubemap и
  3D-текстуры вызов бессмысленен; значение не ограничивается
  `GL_MAX_TEXTURE_MAX_ANISOTROPY`.
* `Update` для cubemap передаёт в `glTexSubImage2D` цель
  `GL_TEXTURE_CUBE_MAP`, что не является допустимой целью для этой функции:
  пофейсовое обновление cubemap не поддерживается, куб нужно пересоздавать.
* `Update`/`Update3D`/`GenerateMipmaps` не проверяют `Valid()` и не сообщают
  об ошибке — при `id_ == 0` это тихий no-op.

## Члены класса

### `enum class PixelFormat : u8`

Внутренний формат текстуры (то, как данные лежат в памяти GPU). От него
зависит `PixelFormatSize`, возможность сэмплировать глубину и то, попадёт ли
значение в `GL_RGBA16F`-цепочку постобработки.

| Значение | Байт на пиксель | Смысл |
|---|---|---|
| `PixelFormat::Unknown` | 0 | не задан; `Create` подставит `RGBA8`, но `Format()` вернёт `Unknown` |
| `PixelFormat::R8` | 1 | одна нормированная компонента |
| `PixelFormat::RG8` | 2 | две компоненты |
| `PixelFormat::RGB8` | 3 | три компоненты без альфы |
| `PixelFormat::RGBA8` | 4 | обычный цвет с альфой |
| `PixelFormat::SRGBA8` | 4 | `RGBA8` в sRGB-пространстве (для цветовых карт) |
| `PixelFormat::R16F` | 2 | half-float, одна компонента |
| `PixelFormat::RG16F` | 4 | half-float, две компоненты |
| `PixelFormat::RGB16F` | 6 | half-float, три компоненты |
| `PixelFormat::RGBA16F` | 8 | HDR-цвет; формат сцены у `PostProcessor` |
| `PixelFormat::R32F` | 4 | float, одна компонента (например, карта высот) |
| `PixelFormat::RGBA32F` | 16 | float, полный цвет высокой точности |
| `PixelFormat::Depth16` | 2 | только глубина |
| `PixelFormat::Depth24` | 3 | только глубина, 24 бита |
| `PixelFormat::Depth24Stencil8` | 4 | глубина + трафарет |
| `PixelFormat::Depth32F` | 4 | float-глубина |
| `PixelFormat::RGB10A2` | 4 | упакованный HDR-подобный формат (10+10+10+2 бита) |

```cpp
const crossrender::PixelFormat fmt = crossrender::PixelFormat::RGBA16F;
ENG_LOGI("demo", "формат: %d байт на пиксель, float=%d, depth=%d",
         crossrender::PixelFormatSize(fmt), crossrender::PixelFormatIsFloat(fmt) ? 1 : 0,
         crossrender::PixelFormatIsDepth(fmt) ? 1 : 0);
```

### `int PixelFormatSize(PixelFormat f)`

Возвращает число байт на пиксель для несжатого формата. Для
`PixelFormat::Unknown` возвращает `0`. Удобно для расчёта размера буфера при
`ReadPixels`-подобных операциях и для валидации ассетов.

```cpp
const crossrender::usize bytes = static_cast<crossrender::usize>(crossrender::PixelFormatSize(fmt)) * w * h;
ENG_LOGI("demo", "понадобится %zu байт", bytes);
```

### `bool PixelFormatIsDepth(PixelFormat f)`

`true` для `Depth16`, `Depth24`, `Depth24Stencil8` и `Depth32F`. Такие
текстуры нельзя использовать как цветовые, а `Create` фиксирует для них
`GL_NEAREST`.

```cpp
crossrender::Texture shadowDepth;
shadowDepth.Create(1024, 1024, crossrender::PixelFormat::Depth24);
if (crossrender::PixelFormatIsDepth(shadowDepth.Format())) {
    ENG_LOGI("demo", "глубинный формат, фильтрация будет Nearest");
}
```

### `bool PixelFormatIsFloat(PixelFormat f)`

`true` для всех half-float и float форматов (`R16F`…`RGBA32F`). По этому
признаку удобно выбирать HDR-путь постобработки.

```cpp
crossrender::Texture hdr;
hdr.Create(320, 180, crossrender::PixelFormat::RGBA16F);
if (crossrender::PixelFormatIsFloat(hdr.Format())) {
    ENG_LOGI("demo", "текстура в HDR, тонмаппинг обязателен");
} else {
    ENG_LOGW("demo", "текстура в LDR, bloom будет выглядеть иначе");
}
```

### `enum class TextureFilter : u8`

Фильтрация текстуры: как выбирается цвет между текселями и между уровнями
мипмапов.

| Значение | Смысл |
|---|---|
| `TextureFilter::Nearest` | ближайший тексель; пиксельный вид, без сглаживания |
| `TextureFilter::Linear` | линейная интерполяция внутри уровня |
| `TextureFilter::NearestMipmapNearest` | ближайший мипмап, ближайший тексель |
| `TextureFilter::LinearMipmapLinear` | трилинейная фильтрация (мипмапы обязательны) |

```cpp
crossrender::Texture tex;
tex.Create(64, 64, crossrender::PixelFormat::RGBA8, nullptr, crossrender::TextureFilter::Nearest);
tex.SetFilter(crossrender::TextureFilter::Linear);   // сгладить
```

### `enum class TextureWrap : u8`

Адресация (обёртка) текстурных координат за пределами `[0, 1]`.

| Значение | Смысл |
|---|---|
| `TextureWrap::Repeat` | повторять текстуру (тайлинг) |
| `TextureWrap::ClampToEdge` | растянуть крайний тексель — значение по умолчанию |
| `TextureWrap::MirroredRepeat` | повторять с зеркальным отражением |
| `TextureWrap::ClampToBorder` | в движке **отображается на `ClampToEdge`**, отдельного бордюра нет |

```cpp
crossrender::Texture atlas;
atlas.Create(512, 512, crossrender::PixelFormat::RGBA8, nullptr, crossrender::TextureFilter::Linear,
             crossrender::TextureWrap::ClampToEdge);   // атлас: не повторять за краями
```

### `enum class BlendMode : u8`

Режим смешивания — какие множители применяются к источнику и приёмнику.
Тип общий для графического слоя: его понимают `Renderer2D`, система частиц и
Lottie. В `Material` из `crossrender/gfx/Mesh.h` поля `BlendMode` нет — там только
`AlphaMode`, поэтому 3D-форвард-рендерер всегда смешивает прозрачные объекты
по формуле `SRC_ALPHA / ONE_MINUS_SRC_ALPHA`; аддитивного пути у материала нет.

| Значение | Смысл |
|---|---|
| `BlendMode::None` | смешивание выключено |
| `BlendMode::Alpha` | обычная прозрачность (`glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)` в 2D-батче) |
| `BlendMode::Premultiplied` | premultiplied alpha, тот же блендинг, что у `Alpha` |
| `BlendMode::Additive` | аддитивное свечение (`GL_ONE, GL_ONE`) |
| `BlendMode::Multiply` | умножение (`GL_DST_COLOR, GL_ZERO`) |
| `BlendMode::Screen` | экранный режим (`GL_ONE, GL_ONE_MINUS_SRC_COLOR`) |
| `BlendMode::Min` | поэлементный минимум (уравнение смешивания `GL_MIN`) |
| `BlendMode::Max` | поэлементный максимум (`GL_MAX`) |
| `BlendMode::Opaque` | как `None`: смешивание выключено, непрозрачная геометрия |

```cpp
// Частицы: аддитивное свечение искр.
crossrender::BlendState sparkle;
sparkle.mode = crossrender::BlendMode::Additive;
ENG_LOGI("demo", "режим смешивания искр: %d", static_cast<int>(sparkle.mode));
```

### `struct BlendState`

Небольшая обёртка над `BlendMode`, чтобы состояние смешивания можно было
сравнивать и кэшировать (рендерер не переключает `glBlendFunc` без нужды).

```cpp
crossrender::BlendState a;                    // по умолчанию BlendMode::Alpha
crossrender::BlendState b;
b.mode = crossrender::BlendMode::Alpha;
ENG_ASSERT(a == b);
```

### `BlendMode BlendState::mode`

Единственное поле: сам режим смешивания. Значение по умолчанию —
`BlendMode::Alpha`.

```cpp
crossrender::BlendState state;
state.mode = crossrender::BlendMode::Multiply;   // затемнить фон
```

### `bool BlendState::operator==(const BlendState& o) const`

Сравнивает состояния по полю `mode`. Именно это сравнение позволяет
рендереру пропускать повторную установку `glBlendFunc`.

```cpp
crossrender::BlendState cur, want;
want.mode = crossrender::BlendMode::Additive;
if (!(cur == want)) {
    ENG_LOGD("demo", "смена режима смешивания");
    cur = want;
}
```

### `Texture()`

Создаёт пустой объект: имя текстуры `0`, размеры `0`, `Valid() == false`.
Обращения к GPU не происходит, поэтому конструктор безопасен до создания
контекста OpenGL.

```cpp
crossrender::Texture placeholder;             // ещё не текстура, просто владелец
ENG_ASSERT(!placeholder.Valid());
```

### `~Texture()`

Вызывает `Destroy()`: удаляет GPU-текстуру, если она была создана. Никогда не
бросает исключений — деструктор безопасен для стека и контейнеров.

```cpp
{
    crossrender::Texture local;
    local.CreateSolid(crossrender::Color::Red);
}   // здесь имя текстуры освобождается автоматически
```

### `Texture(Texture&& o) noexcept`

Перемещающий конструктор: забирает ресурс у `o`, оставляя его пустым
(`o.Valid() == false`). Нужен для хранения текстур в `std::vector` и
`RenderTarget`.

```cpp
std::vector<crossrender::Texture> pages;
pages.push_back(crossrender::Texture{});             // перемещение, не копирование
ENG_ASSERT(!pages[0].Valid());
```

### `Texture& operator=(Texture&& o) noexcept`

Перемещающее присваивание. Сначала освобождает собственный ресурс, затем
переносит ресурс `o`; самоприсваивание (`t = std::move(t)`) безопасно.

```cpp
crossrender::Texture a, b;
b = std::move(a);                     // a снова пустая, b владеет текстурой
ENG_ASSERT(!a.Valid());
```

### `Texture(const Texture&) = delete`, `Texture& operator=(const Texture&) = delete`

Копирование запрещено: объект владеет ресурсом GPU, и копия привела бы к
двойному удалению имени текстуры. Передавайте текстуры по ссылке или
указателю.

```cpp
void UseTexture(crossrender::Shader& s, const crossrender::Texture& albedo) {
    s.SetTexture("uAlbedo", albedo, 0);   // по ссылке, без копии
}
```

### `bool Create(int width, int height, PixelFormat format, const void* pixels = nullptr, TextureFilter filter = TextureFilter::Linear, TextureWrap wrap = TextureWrap::ClampToEdge, bool mipmaps = false)`

Создаёт двумерную текстуру и, если `pixels != nullptr`, сразу заливает её
данными (ожидается упакованный массив в формате `format`). Предыдущее
содержимое объекта освобождается. Возвращает `false`, если размер
неположительный (с `ENG_LOGE("texture", ...)`) или если контекста OpenGL нет
(с `ENG_LOGW("texture", "no GL context; texture %dx%d not created")`).
Мипмапы при `mipmaps = true` генерируются сразу и `MIN_FILTER` становится
трилинейным.

* **Параметры:** `pixels` может быть `nullptr` — тогда текстура не
  инициализирована; `filter`/`wrap` задают сэмплер.
* **Возвращает:** `true`, если текстура создана и `Valid()`.

```cpp
const crossrender::u8 checker[4 * 4 * 4] = {0};                    // 4x4 RGBA8
crossrender::Texture tex;
if (!tex.Create(4, 4, crossrender::PixelFormat::RGBA8, checker)) {
    ENG_LOGW("demo", "нет GPU — работаем без текстуры");
}
```

### `bool Create3D(int width, int height, int depth, PixelFormat format, const void* pixels = nullptr, TextureFilter filter = TextureFilter::Nearest, TextureWrap wrap = TextureWrap::ClampToEdge)`

Создаёт трёхмерную текстуру (`GL_TEXTURE_3D`), которую использует
воксельный рендерер (например, палитра чанка). Мипмапов у 3D-текстуры нет —
параметра `mipmaps` здесь не существует; фильтрация по умолчанию
`TextureFilter::Nearest`, потому что палитру нельзя размывать.

```cpp
const crossrender::u8 palette[8 * 8 * 8] = {0};                    // 8x8x8 R8
crossrender::Texture voxels;
voxels.Create3D(8, 8, 8, crossrender::PixelFormat::R8, palette);
ENG_LOGI("demo", "глубина 3D-текстуры: %d", voxels.Depth());
```

### `bool CreateCubemap(int size, PixelFormat format, const void* const faces[6], TextureFilter filter = TextureFilter::Linear)`

Создаёт cubemap (кубическую карту) из шести граней одного размера. Порядок
граней: `+X`, `-X`, `+Y`, `-Y`, `+Z`, `-Z`. Если `faces == nullptr` или
размер неположительный — возвращает `false`. Адресация всегда
`ClampToEdge`, мипмапы не создаются, а `MIN_FILTER` различает только
`Nearest` и `Linear`.

```cpp
const void* faces[6] = {px, nx, py, ny, pz, nz};           // six RGBA8 считанных грань
crossrender::Texture sky;
if (sky.CreateCubemap(128, crossrender::PixelFormat::RGB16F, faces)) {
    ENG_LOGI("demo", "cubemap неба готова");
}
```

### `bool LoadFromFile(const std::string& path, bool srgb = false, bool mipmaps = true)`

Читает файл с диска и создаёт текстуру. Поддерживаются PNG, JPG, TGA, BMP и
HDR (декодер stb_image). Изображение всегда декодируется в четыре канала:
при `srgb = true` создаётся `PixelFormat::SRGBA8`, иначе `RGBA8`; фильтрация
`Linear`, адресация `Repeat`, мипмапы включены по умолчанию. При успехе в
`DebugName()` записывается путь. Возвращает `false`, если файл не читается
(молча) или не декодируется (`ENG_LOGE("texture", "image decode failed: ...")`).

```cpp
crossrender::Texture albedo;
if (!albedo.LoadFromFile("assets/wood/albedo.png", /*srgb=*/true)) {
    ENG_LOGW("demo", "%s не найдена, беру заглушку", "albedo.png");
    albedo.CreateCheckerboard(64);
}
```

### `bool LoadFromMemory(const void* data, usize size, bool srgb = false, bool mipmaps = true)`

То же, что `LoadFromFile`, но источник — буфер в памяти (например, файл уже
прочитан `ReadBinaryFile` или пришёл из упакованного архива). Изображение не
переворачивается (`flipVertically = false`), то есть первая строка файла
остаётся первой строкой текстуры.

```cpp
const crossrender::ByteBuffer bytes = crossrender::ReadBinaryFile("assets/icon.png");
crossrender::Texture icon;
if (!bytes.empty()) icon.LoadFromMemory(bytes.data(), bytes.size(), false, false);
```

### `bool CreateSolid(const Color& c)`

Создаёт текстуру 1x1 из одного цвета — удобную заглушку по умолчанию:
`RGBA8`, фильтрация `Nearest`, адресация `ClampToEdge`, без мипмапов.
Компоненты цвета зажимаются в `0..1`.

```cpp
crossrender::Texture white;
white.CreateSolid(crossrender::Color::White);      // белая заглушка для uBaseColorTex
```

### `bool CreateCheckerboard(int size = 64, Color a = Color::FromRGB(0x333333), Color b = Color::FromRGB(0x777777))`

Создаёт отладочную шахматку `size x size` (`RGBA8`, `Linear`, `Repeat`,
с мипмапами). Размер клетки — `size / 8` (не меньше одного текселя). Хорошо
подходит для проверки развёртки UV и отсутствующих ассетов.

```cpp
crossrender::Texture missing;
missing.CreateCheckerboard(128, crossrender::Color::FromRGB(0x220000), crossrender::Color::FromRGB(0x440000));
missing.SetDebugName("missing-texture");
```

### `void Destroy()`

Удаляет GPU-текстуру и обнуляет состояние; повторный вызов безопасен. Имя
текстуры освобождается только если загружена функция `glDeleteTextures`
(то есть есть контекст). После вызова `Valid() == false`, размеры равны
нулю, а `Format()` остаётся прежним.

```cpp
crossrender::Texture scratch;
scratch.CreateCheckerboard(32);
scratch.Destroy();                          // ресурс освобождён
ENG_ASSERT(!scratch.Valid());
```

### `void Update(const void* pixels, int x = 0, int y = 0, int w = -1, int h = -1)`

Частично обновляет двумерную текстуру через `glTexSubImage2D`, не пересоздавая
её (удобно для процедурных карт и видео-кадров). `w`/`h`, равные `-1`,
означают «до края текстуры». `x`/`y` — левый верхний угол области. При
`id_ == 0`, `pixels == nullptr` или пустой области — тихий no-op. Для
cubemap функция не работает: цель передаётся неверно (см. ограничения выше).

```cpp
std::vector<crossrender::u8> tile(16 * 16 * 4, 255);
crossrender::Texture noise;
noise.Create(64, 64, crossrender::PixelFormat::RGBA8);
noise.Update(tile.data(), 16, 16, 16, 16);   // заплатка в середине
```

### `void Update3D(const void* pixels, int x, int y, int z, int w, int h, int d)`

Обновляет блок вокселов внутри 3D-текстуры (`glTexSubImage3D`). Аргументы
`x, y, z` задают начало блока, `w, h, d` — его размер. Как и `Update`, при
`id_ == 0` или `pixels == nullptr` ничего не делает.

```cpp
std::vector<crossrender::u8> block(4 * 4 * 4, 1);
crossrender::Texture palette;
palette.Create3D(64, 64, 64, crossrender::PixelFormat::R8);
palette.Update3D(block.data(), 0, 0, 0, 4, 4, 4);
```

### `void GenerateMipmaps()`

Строит цепочку мипмапов для текущей текстуры (2D или cubemap). Полезно, если
текстура создавалась с `mipmaps = false`, но позже понадобилось сглаживание.
Чтобы мипмапы реально использовались, `MIN_FILTER` должен быть
мипмап-фильтром: после `SetFilter(Linear)` он равен `GL_LINEAR`, и цепочка
не задействуется.

```cpp
crossrender::Texture atlas;
atlas.Create(256, 256, crossrender::PixelFormat::RGBA8, nullptr, crossrender::TextureFilter::Linear, 
             crossrender::TextureWrap::ClampToEdge, /*mipmaps=*/false);
atlas.GenerateMipmaps();
```

### `void SetFilter(TextureFilter f)`

Меняет фильтрацию существующей текстуры. Для `MAG_FILTER` ставится
соответствующий режим, для `MIN_FILTER` — только `Nearest` или `Linear`
(мипмап-варианты сворачиваются). Для текстуры с `id_ == 0` — no-op.

```cpp
crossrender::Texture pixelArt;
pixelArt.Create(32, 32, crossrender::PixelFormat::RGBA8);
pixelArt.SetFilter(crossrender::TextureFilter::Nearest);   // без сглаживания
```

### `void SetWrap(TextureWrap w)`

Меняет адресацию по `S` и `T`, а для 3D-текстуры — ещё и по `R`.
`ClampToBorder` ведёт себя как `ClampToEdge`.

```cpp
crossrender::Texture ground;
ground.Create(512, 512, crossrender::PixelFormat::RGBA8);
ground.SetWrap(crossrender::TextureWrap::Repeat);          // тайлинг травы
```

### `void SetAnisotropy(f32 level)`

Включает анизотропную фильтрацию (`GL_TEXTURE_MAX_ANISOTROPY_EXT`) — заметно
улучшает качество текстур на наклонных поверхностях. Работает только для
двумерных текстур и только если расширение доступно; при `id_ == 0` — no-op.

```cpp
crossrender::Texture road;
road.LoadFromFile("assets/road.png");
road.SetAnisotropy(8.0f);      // мягче «мыло» у горизонта
```

### `bool Valid() const`

`true`, если текстура действительно создана (`id_ != 0`). Это основной способ
отличить рабочий ресурс от результата неудачного `Create` в headless-режиме.
Не проверяет, полна ли текстура с точки зрения OpenGL.

```cpp
crossrender::Texture tex;
tex.LoadFromFile("assets/hero.png");
if (!tex.Valid()) ENG_LOGW("demo", "текстура не загрузилась, рисую заглушку");
```

### `unsigned int Id() const`

Возвращает имя текстуры OpenGL. Нужно для ручной привязки через
`crossrender/gfx/GL.h` и для передачи в чужие фреймбуферы. Не освобождайте это имя
вручную — им владеет объект.

```cpp
crossrender::Texture tex;
tex.CreateCheckerboard();
crossrender::gl::glActiveTexture(crossrender::gl::GL_TEXTURE0 + 3);
crossrender::gl::glBindTexture(crossrender::gl::GL_TEXTURE_2D, tex.Id());   // ручная привязка на юнит 3
```

### `int Width() const`

Ширина в текселях; `0` у пустой текстуры.

```cpp
ENG_LOGI("demo", "атлас %dx%d", atlas.Width(), atlas.Height());
```

### `int Height() const`

Высота в текселях; `0` у пустой текстуры.

```cpp
const float texelY = 1.0f / static_cast<float>(atlas.Height());
```

### `int Depth() const`

Глубина 3D-текстуры. Для обычной 2D-текстуры и cubemap равна `1`.

```cpp
crossrender::Texture palette;
palette.Create3D(64, 64, 64, crossrender::PixelFormat::R8);
if (palette.Depth() > 1) ENG_LOGI("demo", "это 3D-текстура, слоёв %d", palette.Depth());
```

### `PixelFormat Format() const`

Формат, с которым текстура создавалась (или `Unknown` у пустой). Помните,
что для `Unknown` реальный GL-формат — `RGBA8`, а обратное преобразование
невозможно.

```cpp
crossrender::Texture grab;
grab.Create(128, 128, crossrender::PixelFormat::RGBA16F);
if (grab.Format() == crossrender::PixelFormat::RGBA16F) {
    ENG_LOGI("demo", "это HDR-таргет, нужен тонмаппинг");
}
```

### `bool IsCubemap() const`

`true`, если текстура создана через `CreateCubemap`. Для неё `Width()` равен
`Height()`, а `Depth()` равен `1`.

```cpp
if (sky.IsCubemap()) ENG_LOGI("demo", "текстура неба — cubemap %d", sky.Width());
```

### `void SetSdfParams(f32 spread, f32 size)`

Запоминает параметры SDF-шрифта (spread — разброс расстояния в текселях,
size — кегль, для которого пеклась карта). На GPU не влияет: это метаданные
для шрифтового рендерера, чтобы правильно выбирать порог сглаживания.

```cpp
crossrender::Texture glyphs;
glyphs.LoadFromFile("assets/fonts/inter_sdf.png");
glyphs.SetSdfParams(4.0f, 48.0f);
```

### `f32 SdfSpread() const`

Возвращает spread, заданный через `SetSdfParams` (`0` по умолчанию).

```cpp
crossrender::Texture glyphs;
glyphs.SetSdfParams(4.0f, 48.0f);
crossrender::Shader textShader;
textShader.Set("uSdfSpread", glyphs.SdfSpread());
```

### `f32 SdfSize() const`

Возвращает кегль SDF-карты, заданный через `SetSdfParams` (`0` по умолчанию).

```cpp
ENG_LOGI("demo", "SDF испечён для кегля %.1f", glyphs.SdfSize());
```

### `const std::string& DebugName() const`

Имя для отладки. `LoadFromFile` записывает сюда путь; для остальных способов
создания имя пустое, пока его не задали через `SetDebugName`.

```cpp
ENG_LOGI("demo", "не загрузилась текстура '%s'", tex.DebugName().c_str());
```

### `void SetDebugName(std::string n)`

Задаёт отладочное имя. На рендеринг не влияет, но очень помогает в логах и
профайлерах GPU.

```cpp
crossrender::Texture shadowMap;
shadowMap.Create(2048, 2048, crossrender::PixelFormat::Depth24);
shadowMap.SetDebugName("sun-shadow-map");
```

### `struct Texture::ImageData`

Результат CPU-декодирования картинки. Декодер всегда отдаёт четыре канала
(RGBA), поэтому `channels` почти всегда равен `4`; поле существует для
совместимости с другими источниками. `pixels` — плотный массив
`width * height * channels` байт, начало координат — левый верхний угол
(если декодирование не переворачивалось).

```cpp
crossrender::Texture::ImageData img;
if (crossrender::Texture::DecodeImageFile("assets/hero.png", &img)) {
    ENG_LOGI("demo", "картинка %dx%d, каналов %d, байт %zu", img.width, img.height, img.channels,
             img.pixels.size());
}
```

### `int Texture::ImageData::width`

Ширина декодированного изображения в пикселях.

```cpp
crossrender::Texture::ImageData img;
crossrender::Texture::DecodeImageFile("assets/hero.png", &img);
const int rowBytes = img.width * img.channels;
```

### `int Texture::ImageData::height`

Высота декодированного изображения в пикселях.

```cpp
ENG_LOGI("demo", "aspect = %.3f", static_cast<float>(img.width) / static_cast<float>(img.height));
```

### `int Texture::ImageData::channels`

Число каналов. `DecodeImage` всегда устанавливает `4` (RGBA8), даже если
исходный файл был серым или без альфы.

```cpp
if (img.channels != 4) ENG_LOGW("demo", "неожиданное число каналов: %d", img.channels);
```

### `std::vector<u8> Texture::ImageData::pixels`

Сами пиксели, `width * height * channels` байт. Владелец — структура; данные
можно передать в `Texture::Create` или `EncodePng`.

```cpp
crossrender::Texture::ImageData img;
crossrender::Texture::DecodeImageFile("assets/hero.png", &img);
crossrender::Texture tex;
tex.Create(img.width, img.height, crossrender::PixelFormat::RGBA8, img.pixels.data());
```

### `static bool DecodeImage(const void* data, usize size, ImageData* out, bool flipVertically = true)`

Декодирует изображение из памяти в `ImageData`, не трогая GPU. По умолчанию
переворачивает изображение по вертикали (`flipVertically = true`), потому что
данные файлов приходят сверху вниз, а текстурные координаты в движке — снизу
вверх. Работает в headless-режиме. При ошибке пишет
`ENG_LOGE("texture", "image decode failed: ...")` и возвращает `false`.

```cpp
crossrender::ByteBuffer bytes = crossrender::ReadBinaryFile("assets/hero.png");
crossrender::Texture::ImageData img;
if (!bytes.empty() && crossrender::Texture::DecodeImage(bytes.data(), bytes.size(), &img)) {
    ENG_LOGI("demo", "декодировано %dx%d без GPU", img.width, img.height);
}
```

### `static bool DecodeImageFile(const std::string& path, ImageData* out, bool flipVertically = true)`

Читает файл и вызывает `DecodeImage`. При неудачном чтении пишет
`ENG_LOGE("texture", "cannot read image %s", ...)`. Это самый дешёвый способ
проверить ассет в тесте или инструменте без окна.

```cpp
crossrender::Texture::ImageData img;
if (!crossrender::Texture::DecodeImageFile("assets/hero.png", &img, /*flipVertically=*/false)) {
    ENG_LOGE("demo", "ассет битый");
}
```

### `static bool EncodePng(const std::string& path, int w, int h, int channels, const void* pixels)`

Сохраняет буфер пикселей в PNG. При необходимости создаёт недостающие
каталоги (`CreateDirectories`), шаг строки — `w * channels`. Возвращает
`false`, если буфер пуст или размеры неположительные. Удобно для скриншотов,
снятых через `RenderTarget::ReadPixels`, и для записи отладочных карт.

```cpp
std::vector<crossrender::u8> rgba;                   // например, из RenderTarget::ReadPixels
crossrender::Texture::EncodePng("out/frame.png", 1280, 720, 4, rgba.data());
```

## Пример целиком

```cpp
#include "crossrender/gfx/Texture.h"

#include "crossrender/core/File.h"
#include "crossrender/core/Log.h"
#include "crossrender/gfx/Shader.h"

#include <vector>

// Готовит процедурную текстуру-заглушку, пробует загрузить настоящую карту
// и возвращает ту, которую можно сэмплировать в шейдере.
void LoadOrBakeAlbedo(crossrender::Shader& shader) {
    // 1. Сначала пробуем файл: если ассета нет, остаёмся с процедурной картой.
    crossrender::Texture albedo;
    if (!albedo.LoadFromFile("assets/wood/albedo.png", /*srgb=*/true, /*mipmaps=*/true)) {
        ENG_LOGW("demo", "albedo не найдена, пеку шахматку");
        albedo.CreateCheckerboard(128, crossrender::Color::FromRGB(0x553322), crossrender::Color::FromRGB(0x996644));
        albedo.SetDebugName("procedural-wood");
    }
    albedo.SetAnisotropy(4.0f);

    // 2. Привязка к юниту 0 и установка сэмплера: у Texture нет своего Bind.
    shader.SetTexture("uAlbedo", albedo, 0);
    shader.Set("uHasAlbedo", albedo.Valid() ? 1 : 0);

    // 3. Дописываем пиксели в уже созданную текстуру (штамп поверх карты).
    if (albedo.Valid()) {
        std::vector<crossrender::u8> stamp(16 * 16 * 4, 255);
        albedo.Update(stamp.data(), 0, 0, 16, 16);
    }

    // 4. CPU-чтение: работает и без контекста OpenGL.
    crossrender::Texture::ImageData img;
    if (crossrender::Texture::DecodeImageFile("assets/wood/albedo.png", &img)) {
        ENG_LOGI("demo", "исходник %dx%d, каналов %d", img.width, img.height, img.channels);
    }

    // 5. Сохраняем то, что реально лежит в CPU-памяти, для отладки.
    const crossrender::u8 debugPixel[4] = {255, 0, 255, 255};
    crossrender::Texture::EncodePng("out/debug_stamp.png", 1, 1, 4, debugPixel);

    // 6. При выходе из функции деструктор albedo сам вызовет Destroy().
}
```

## См. также

* `docs/core/Base.md` — типы `u8`, `f32`, `usize` и `ENG_DEFER`.
* `docs/core/Log.md` — макросы `ENG_LOGW` / `ENG_LOGE`, которыми движок
  сообщает об отсутствии контекста OpenGL.
* `docs/gfx/Shader.md` — `Shader::SetTexture`, единственный штатный способ
  привязать текстуру к юниту.
* `docs/gfx/RenderTarget.md` — `RenderTarget::ReadPixels`, GPU-чтение кадра
  обратно в буфер `RGBA8`.
* `docs/gfx/Mesh.md` — `Material`, который хранит указатели на текстуры
  (`const Texture*`), но не владеет ими.
