# crossrender/text/Font.h — шрифты: загрузка, растеризация и SDF-атлас глифов

Заголовок объявляет загрузку TrueType и OpenType/CFF с нуля, растеризацию
глифов в атлас, генерацию Signed Distance Field (SDF — знакового поля
расстояний) и публичные типы для раскладки текста: `FontDesc`, `Glyph`,
`GlyphOutline`, `FontAtlasPage`, `Font`, `FontManager` и утилиты UTF-8.

## Заголовок

```cpp
#include "crossrender/text/Font.h"
```

## Обзор

Лица, которые использует пример: `assets/fonts/ubuntu.ttf` — основной интерфейсный шрифт (`mega::Assets::FontRegular()`, Ubuntu Regular 16 px), `ubuntu_bold.ttf` — заголовки (`FontLarge()`, 48 px), `ubuntu_light.ttf` — светлое начертание, `ubuntu_mono.ttf` — моноширинный для ASCII-сцены (`FontMono()`). Все они лежат в `tools/fonts/` и раскладываются в `assets/` генератором ассетов.

`Font` — CPU-first-компонент: разбор таблиц шрифта, растеризация и упаковка
глифов в атлас выполняются на процессоре, а GPU нужен только для загрузки
готовых страниц атласа. Таблицы `glyf`/`loca`/`cmap`/`hmtx`/`kern`, а также
интерпретатор CFF Type2 charstring реализованы в движке; stb_truetype
намеренно не используется, чтобы контуры TrueType и OpenType/CFF делили один
аналитический растеризатор.

Типичный порядок работы:

1. Описать начертание через `FontDesc` (размер запекания, SDF, атлас, стиль).
2. Загрузить файл: `LoadFromFile` или `LoadFromMemory` (или взять
   `FontManager::Load` / `FontManager::DefaultFont`, чтобы не возиться с
   владением).
3. При необходимости настроить фолбэки: `AddFallback`, затем `Resolve`.
4. Требовать глифы: `GetGlyph`, `HasGlyph`, `GlyphIndex`, `GetKerning`,
   `Prebake` / `PrebakeAscii`.
5. Рисовать через `Renderer2D::DrawText` / `DrawTextStyled` (они сами вызывают
   `GetGlyph` и `AtlasPage`), а для векторного рендерера — брать контуры через
   `GetGlyphOutline` / `GetGlyphOutlineUnits`.
6. Освободить: `Destroy()` или деструктор.

### Единицы измерения и масштаб

В API сосуществуют три системы единиц, и путать их нельзя:

| Что | В каких единицах |
|---|---|
| `UnitsPerEm()` | единицы шрифта (font units) на один em; обычно 1000 или 2048, у процедурного начертания — 1024 |
| `Ascender`, `Descender`, `LineGap`, `LineHeight`, `CapHeight`, `XHeight`, `UnderlinePosition`, `UnderlineThickness` | пиксели **при запечённом** `FontDesc::pixelHeight` |
| `Glyph::advance`, `bearingX`, `bearingY`, `width`, `height` | пиксели при `FontDesc::pixelHeight` |
| `GetKerning(left, right)` | **единицы шрифта** — пиксели получаются умножением на `size / UnitsPerEm()` |
| `GetGlyphOutline(cp, size, out)` | пиксели при запрошенном `size`, ось Y вниз, начало — перо на базовой линии |
| `GetGlyphOutlineUnits(cp, out)` | единицы шрифта, без масштабирования |

Метрики пересчитываются в пиксели один раз при загрузке: значение из таблицы
умножается на `pixelHeight / UnitsPerEm`. `Descender` отрицателен (базовая
линия — ноль, ось Y направлена вниз), а `LineHeight` равен
`Ascender - Descender + LineGap`.

`ScaleForSize(size)` возвращает множитель от запечённого размера к
запрошенному: `size / pixelHeight` (при `size <= 0` — единицу). Именно он
превращает метрики и `advance` запечённого начертания в пиксели нужного кегля.

```cpp
const crossrender::Glyph* g = font.GetGlyph('A');
if (g) {
    const crossrender::f32 scale = font.ScaleForSize(24.0f);            // 24 / pixelHeight
    const crossrender::f32 advancePx = g->advance * scale;              // пиксели при 24 px
    const crossrender::f32 kernPx = font.GetKerning('A', 'V') * 24.0f / font.UnitsPerEm();
    ENG_LOGI("demo", "advance %.2f px, кернинг %.2f px", advancePx, kernPx);
}
```

### Одно запечённое начертание — много размеров

Атлас глифов запекается **один раз**, в размере `FontDesc::pixelHeight`, и
`GetGlyph` растеризует глиф ровно в этом размере. Любая отрисовка другого
кегля — это масштабирование уже готового битмапа через `ScaleForSize`.
Отсюда практическое правило: выбирайте размер запекания близким к тому, которым
текст реально рисуется.

* Сильное уменьшение (атлас 48 px, рисунок 12 px) размывает буквы: тонкие
  штрихи теряются при усреднении.
* Сильное увеличение (атлас 16 px, рисунок 48 px) даёт мыло или пиксели и
  не добавляет деталей — их в атласе нет.
* Поэтому демо-приложение печёт UI-начертание на 16 px (основная масса
  подписей 10–18 px), заголовочное — на 48 px (крупный текст), а SDF-начертание
  — на 64 px: SDF переносит масштаб заметно лучше, потому что хранит расстояние
  до контура, а не покрытие.
* Два `Font` одной и той же семьи, запечённые на разных размерах, — это два
  независимых атласа и два независимых набора метрик. `FontManager` кэширует их
  отдельно, потому что `FontDesc` целиком входит в ключ кэша.

```cpp
crossrender::FontDesc ui;
ui.pixelHeight = 16.0f;      // подписи и мелкие метки
ui.hinting = true;

crossrender::FontDesc heading;
heading.pixelHeight = 48.0f; // заголовки: 1:1 или мягкое уменьшение

crossrender::Font uiFace, headingFace;
uiFace.LoadFromFile("assets/fonts/ubuntu.ttf", ui);
headingFace.LoadFromFile("assets/fonts/ubuntu.ttf", heading);
ENG_LOGI("demo", "UI: ascender %.1f, заголовок: ascender %.1f", uiFace.Ascender(),
         headingFace.Ascender());
```

### Атлас глифов, страницы и отложенная загрузка

Глифы укладываются на квадратные страницы со стороной `atlasSize` (не меньше
16 px) полочным (shelf) алгоритмом: курсор идёт слева направо, когда строка
заполнена — вниз на высоту самой высокой полки. Когда места нет, открывается
новая страница; страниц может быть не больше **восьми**. Если и они исчерпаны,
глиф отбрасывается с предупреждением `atlas full (8 pages); glyph U+XXXX
dropped`, `GetGlyph` возвращает `nullptr`, и попытка повторится при следующем
вызове (неудачный глиф не кэшируется).

Для отрисовки одного глифа нужны три вещи: `Glyph::page`, его
`u0/v0/u1/v1` и текстура `AtlasPage(page).texture`. Готовая четвёрка
координат и прямоугольник упакованы в `GlyphQuad` — им пользуется векторный
шейдерный рендерер текста.

Особый случай — **отсутствие контекста OpenGL в момент загрузки** (headless-
тесты, прогрев UI до создания окна). `Texture::Create` тогда возвращает
невалидную текстуру, но страница атласа всё равно создаётся: пиксели глифа
складываются в очередь отложенной загрузки, а сам глиф получает корректные
`page` и `uv` и кэшируется. Метрики, раскладка и измерение текста работают
полностью. Первый же вызов `GetGlyph` после появления контекста замечает флаг
ожидающих загрузок, создаёт текстуру страницы и «проигрывает» накопленные
`Texture::Update`. Пустые глифы (пробел) вообще не нуждаются в GL.

Для SDF-начертаний каждая страница получает метаданные через
`Texture::SetSdfParams(spread, pixelHeight)`, чтобы шейдер знал, как перевести
8-битное расстояние обратно в пиксели.

```cpp
// Без окна: глифы растеризуются, метрики считаются, загрузка в GPU отложена.
crossrender::Font font;
font.LoadFromFile("assets/fonts/ubuntu.ttf", crossrender::FontDesc{});
const crossrender::Glyph* g = font.GetGlyph('R');
if (g) ENG_LOGI("demo", "глиф U+%04X на странице %d, %.1fx%.1f px", g->codepoint, g->page,
                g->width, g->height);
// После создания контекста тот же вызов создаст текстуры и загрузит пиксели.
```

### Три вида начертаний: TrueType, OpenType/CFF и растровое

| Вид | `FontFormat` | Что внутри |
|---|---|---|
| TrueType | `TrueType` | таблицы `glyf`/`loca`, простые и составные контуры (глубина композиции до 8) |
| OpenType/CFF | `OpenTypeCFF` | Type2 charstrings, CID `FDArray`/`FDSelect`, `seac`-композиция |
| Коллекция TTC | формат первой гарнитуры | читается только гарнитура 0 (с предупреждением) |
| Растровое (процедурное) | `Bitmap` | встроенная таблица 5x7 для ASCII и псевдо-кириллицы |

Разница между растровым, bitmap- и SDF-начертаниями:

* **Bitmap-начертание** (`FontDesc::sdf = false`) растеризует контуры в
  покрытие (coverage, 0..255): по вертикали — суперсэмплинг
  (`oversample`, зажимается в `1..4` и умножается на 2), по горизонтали —
  аналитическое покрытие пролётов. Гамма (`FontDesc::gamma`) применяется к
  покрытию степенью. Результат — обычный альфа-атлас.
* **SDF-начертание** (`FontDesc::sdf = true`) вместо покрытия считает знаковое
  расстояние до контура в пределах `sdfSpread` пикселей. Глиф получается шире
  исходного на `sdfSpread` с каждой стороны, `width`/`height` и
  `bearingX`/`bearingY` это учитывают. Все четыре канала текстуры получают одно
  и то же значение расстояния, а страница несёт `SdfSpread()`/`SdfSize()`.
* **Процедурное начертание** (`FontManager::DefaultFont()`) вообще не имеет
  контуров: глифы рисуются из встроенной таблицы 5x7, масштабируются
  ближайшим соседом, поддерживают синтетическую жирность и наклон.
  Неизвестные символы и части кириллицы заменяются полой рамкой.

```cpp
crossrender::FontDesc bitmap;
bitmap.pixelHeight = 16.0f;
bitmap.sdf = false;

crossrender::FontDesc sdf;
sdf.pixelHeight = 64.0f;
sdf.sdf = true;
sdf.sdfSpread = 8.0f;
sdf.hinting = false;   // grid-fit искажает расстояния

crossrender::Font bmp, sdfFont;
bmp.LoadFromFile("assets/fonts/ubuntu.ttf", bitmap);
sdfFont.LoadFromFile("assets/fonts/ubuntu.ttf", sdf);
ENG_LOGI("demo", "bitmap sdf=%d, sdf sdf=%d", bmp.IsSdf() ? 1 : 0, sdfFont.IsSdf() ? 1 : 0);
```

### Контуры: контракт `GetGlyphOutline`

Векторные контуры нужны аналитическому рендереру текста, поэтому у них жёсткий
и узкий контракт:

* **Только квадратичные кривые.** `GlyphPoint::onCurve == 0` — контрольная
  точка квадратичной кривой, `1` — точка на контуре. Кубические сегменты CFF
  конвертируются в цепочку квадратичных (с допуском 0.2 px) до того, как
  попасть в `GlyphOutline`.
* **Контуры всегда замкнуты** (`GlyphContour::closed == true`) и начинаются и
  заканчиваются на on-curve точке; последняя точка повторяет первую.
* **`bounds` — плотный прямоугольник чернил**, а не охват контрольных точек:
  учитываются настоящие экстремумы квадратичных кривых. Пространство то же:
  пиксели, ось Y вниз, начало координат — перо на базовой линии, поэтому
  `bounds.y` обычно отрицателен.
* **Пустой глиф — не ошибка.** Пробел, неразрывный пробел и любой глиф без
  чернил возвращают `true` с `empty = true` и пустым списком контуров (если
  codepoint вообще отображён в `cmap`; иначе сработает правило ниже).
  `false` означает только «шрифт не может дать этот глиф»: `out == nullptr`,
  шрифт невалиден, у начертания нет ни `glyf`, ни CFF, либо codepoint не
  отображён (`GlyphIndex == 0`).
* `out->advance` считается в запрошенном размере (для
  `GetGlyphOutlineUnits` — в единицах шрифта), в отличие от `Glyph::advance`,
  который всегда в пикселях при `pixelHeight`.
* Наклон (`FontDesc::italic`) применяется и к контурам, и к растеризации;
  синтетическая жирность (`FontDesc::bold`) — только к растеризации.

```cpp
crossrender::GlyphOutline outline;
if (font.GetGlyphOutline('A', 32.0f, &outline)) {
    if (outline.empty) {
        ENG_LOGI("demo", "у глифа нет чернил (например, пробел)");
    } else {
        ENG_LOGI("demo", "контуров %zu, чернила %.1fx%.1f, advance %.1f", outline.contours.size(),
                 outline.bounds.w, outline.bounds.h, outline.advance);
    }
}
```

### Цепочка фолбэков

`FontManager` — синглтон с кэшем по ключу «путь + все поля `FontDesc`»: он
владеет загруженными шрифтами до `Clear()`/`Shutdown()` и возвращает сырые
указатели. Он **не связывает фолбэки автоматически**, но предоставляет
непробиваемое последнее звено:

* `DefaultFont()` — процедурное начертание из встроенной таблицы, почти никогда
  не даёт сбоя и синтезирует хоть какой-то глиф для любого codepoint;
* `DefaultSdfFont()` — то же с `sdf = true`, а при неудаче возвращает
  `DefaultFont()`.

Цепочку строит вызывающий: `AddFallback` добавляет шрифт в конец списка,
`Resolve(codepoint, &glyph)` сначала ищет глиф в самом шрифте, затем обходит
фолбэки **в порядке добавления** и возвращает первый непустой глиф, а также
указатель на шрифт-владелец. Демонстрационное приложение связывает все
найденные начертания друг с другом и последним ставит `DefaultFont()`, поэтому
любой текст получает хоть какое-то изображение.

```cpp
crossrender::Font latin, cjk;
latin.LoadFromFile("assets/fonts/ubuntu.ttf");
cjk.LoadFromFile("assets/fonts/cjk.ttf");
latin.AddFallback(&cjk);
if (crossrender::Font* builtin = crossrender::FontManager::Get().DefaultFont()) latin.AddFallback(builtin);

const crossrender::Glyph* g = nullptr;
crossrender::Font* owner = latin.Resolve(0x4E2D /* 中 */, &g);   // уйдёт в CJK-фолбэк
if (owner && g) ENG_LOGI("demo", "глиф дал шрифт '%s'", owner->FamilyName().c_str());
```

### Честные ограничения и ловушки

1. **Метрики зависят от запечённого размера.** `Ascender()`, `Descender()`,
   `LineHeight()` у одной и той же семьи, запечённой на 16 px и на 48 px, —
   разные числа (примерно втрое), хотя отношение к `pixelHeight` одинаковое.
   Не сравнивайте абсолютные значения между разными `Font` и не переносите
   метрики из одного `FontDesc` в другой.
2. **Отсутствующий codepoint уходит в фолбэк.** `GetGlyph` вернёт `nullptr`,
   `HasGlyph` — `false`; раскладка через `Resolve` попробует фолбэки и лишь
   затем вернёт `nullptr`. `HasGlyph` проверяет только `cmap` и ничего не
   говорит о том, есть ли у глифа чернила или контуры.
3. **Некоторые начертания реально пустые.** Бывает, что `cmap` отображает
   codepoint, но контуров нет (пробел в интерпретаторе CFF, пустой глиф,
   символ без глифа в начертании). Тогда `GetGlyphOutline` вернёт `true` с
   `empty = true`, а `GetGlyph` — глиф с `isEmpty() == true`. Демо помечает
   такие файлы как «blank glyphs» и не считает их сломанными.
4. **`firstCodepoint` / `lastCodepoint` не управляют запеканием.**
   `FontDesc::prebake = true` вызывает `PrebakeAscii()`, то есть диапазон
   32..126, а объявленный диапазон участвует только в ключе кэша
   `FontManager` (два описания с разными диапазонами — разные записи кэша).
   Чтобы запечь произвольный набор, вызовите `Prebake` вручную.
5. **Hinting — эвристика, а не интерпретатор.** Подтягиваются вертикальные
   штрихи шириной примерно 0.35–1.2 px и базовая линия; программы `cvt`/`fpgm`
   не выполняются, а cap-height/x-height «синие зоны» в текущем вызове не
   передаются (нули). Для SDF-начертаний hinting лучше выключать: grid-fit
   смещает источник расстояний.
6. **`DefaultSdfFont()` — SDF только по метаданным.** Процедурное начертание
   не имеет контуров, поэтому его растеризатор всегда выдаёт покрытие из
   таблицы 5x7, хотя `IsSdf()` возвращает `true`, а страницы получают
   `SdfSpread`/`SdfSize`. SDF-шейдер, применённый к такому атласу, будет
   трактовать покрытие как расстояние.
7. **Атлас конечен.** Больше восьми страниц не создаётся: глиф отбрасывается с
   предупреждением и не кэшируется, поэтому `GetGlyph` будет возвращать
   `nullptr` при каждом обращении. Увеличивайте `atlasSize`, прежде чем
   увеличивать число страниц.
8. **`atlasPadding = 0` не убирает зазор полностью.** Отступ участвует в
   проверке вместимости страницы, но курсор внутри страницы всегда сдвигается
   на фиксированный 1 px.
9. **Ленивый кэш и потокобезопасность.** `GetGlyph` объявлен `const`, но
   растеризует глиф, меняет `glyphCache_`, страницы и очередь отложенной
   загрузки. Один `Font` нельзя безопасно использовать из нескольких потоков
   одновременно.
10. **`GetKerning` — в единицах шрифта, а `Glyph::advance` — в пикселях.**
    Смешивать их без множителя `size / UnitsPerEm()` нельзя.
11. **TTC и CFF2.** Из коллекции читается только первая гарнитура, а
    `FontFormat::Collection` текущей реализацией не выставляется — вернётся
    формат гарнитуры. CFF2 (вариативные шрифты) не поддерживается: пишется
    предупреждение и происходит откат на `glyf`.
12. **Указатели менеджера живут до `Clear()`.** Всё, что выдано `Load`,
    `DefaultFont` и `DefaultSdfFont`, становится висячим после
    `FontManager::Clear()` / `Shutdown()`.

```cpp
// Типичная диагностика: глиф есть в cmap, но пустой, и его нет в контурах.
if (font.HasGlyph('A') && font.GetGlyph('A') && font.GetGlyph('A')->isEmpty()) {
    ENG_LOGW("demo", "начертание отображает 'A', но не рисует её");
}
crossrender::GlyphOutline o;
if (font.GetGlyphOutline('A', 32.0f, &o) && o.empty) {
    ENG_LOGW("demo", "у 'A' нет контуров — это растровое или пустое начертание");
}
```

## Члены класса

### `enum class FontFormat : u8`

Источник контуров начертания. Значение информационное: на растеризацию и
метрики оно не влияет, но по нему удобно выбирать путь отрисовки (SDF-шейдер
или обычный) и показывать диагностику.

| Значение | Смысл |
|---|---|
| `FontFormat::Unknown` | шрифт не загружен или формат не распознан |
| `FontFormat::TrueType` | sfnt с таблицами `glyf`/`loca` |
| `FontFormat::OpenTypeCFF` | sfnt `OTTO` с Type2 charstrings |
| `FontFormat::Collection` | объявлено для TTC, но текущая реализация его не выставляет: возвращается формат первой гарнитуры |
| `FontFormat::Bitmap` | растровое начертание без контуров; так помечен процедурный встроенный шрифт |

```cpp
crossrender::Font font;
font.LoadFromFile("assets/fonts/ubuntu.ttf");
switch (font.Format()) {
    case crossrender::FontFormat::OpenTypeCFF:
        ENG_LOGI("demo", "OTF/CFF: контуры кубические, для Slug их конвертируют");
        break;
    case crossrender::FontFormat::Bitmap:
        ENG_LOGI("demo", "растровое начертание: контуров нет");
        break;
    default:
        ENG_LOGI("demo", "TrueType или неизвестный формат");
        break;
}
```

### `struct FontDesc`

Описание того, **как** запечь начертание: размер растеризации, SDF, hinting,
параметры атласа, диапазон codepoint'ов и синтетические стили. Полностью
участвует в ключе кэша `FontManager`, поэтому два описания, различающиеся
любым полем, дают два разных `Font`.

```cpp
crossrender::FontDesc desc;
desc.pixelHeight = 32.0f;
desc.sdf = false;
desc.hinting = true;
desc.atlasSize = 1024;
crossrender::Font* font = crossrender::FontManager::Get().Load("assets/fonts/ubuntu.ttf", desc);
```

### `f32 FontDesc::pixelHeight`

Размер растеризации в пикселях (по умолчанию 48). Глифы растеризуются ровно в
этом размере, и все метрики возвращаются в этих же пикселях. Отрисовка других
кеглей — масштабирование битмапа, поэтому выбирайте размер близким к реально
используемому.

```cpp
crossrender::FontDesc ui;
ui.pixelHeight = 16.0f;   // 93% подписей демо рисуются не крупнее 18 px
crossrender::FontDesc hero;
hero.pixelHeight = 48.0f; // крупные заголовки
```

### `bool FontDesc::sdf`

Строить знаковое поле расстояний вместо обычного покрытия. Включено —
атлас хранит расстояние до контура в пределах `sdfSpread`, глиф шире исходного
на величину spread, а страница получает SDF-параметры для шейдера.

```cpp
crossrender::FontDesc desc;
desc.pixelHeight = 64.0f;
desc.sdf = true;
desc.sdfSpread = 8.0f;
ENG_LOGI("demo", "SDF-начертание: %d", desc.sdf ? 1 : 0);
```

### `f32 FontDesc::sdfSpread`

Диапазон расстояния в пикселях (по умолчанию 6; значения меньше 1
зажимаются к 1). Слишком маленький spread обрезает сглаживание на границах,
слишком большой раздувает каждую ячейку атласа и снижает ёмкость страницы.
Имеет смысл только при `sdf = true`.

```cpp
crossrender::FontDesc desc;
desc.sdf = true;
desc.sdfSpread = 4.0f;   // уже поле — плотнее атлас, резче контур
```

### `bool FontDesc::hinting`

Включать grid-fit штрихов и базовой линии для более чёткого мелкого текста
(по умолчанию `true`). Работает перед обоими растеризаторами, но для SDF его
обычно выключают, чтобы не искажать расстояния.

```cpp
crossrender::FontDesc small;
small.pixelHeight = 13.0f;
small.hinting = true;    // без него штрихи «плывут» по пиксельной сетке
```

### `u32 FontDesc::atlasSize`

Сторона страницы атласа в пикселях (по умолчанию 1024, минимум 16). Страница
растёт до размера самого большого глифа, но не более чем на восемь страниц;
для широких наборов (CJK, эмодзи) берите 2048.

```cpp
crossrender::FontDesc cjk;
cjk.pixelHeight = 24.0f;
cjk.atlasSize = 2048;    // больше глифов до переполнения
```

### `int FontDesc::atlasPadding`

Отступ между глифами при проверке вместимости страницы (по умолчанию 2).
Положительное значение задаёт зазор, `0` отключает его при подборе страницы,
отрицательное возвращает внутренний зазор в 1 px. Курсор внутри страницы всё
равно сдвигается на 1 px, поэтому полностью убрать зазор этим полем нельзя.

```cpp
crossrender::FontDesc desc;
desc.atlasPadding = 4;   // запас против «протекания» при линейной фильтрации
```

### `u32 FontDesc::firstCodepoint`

Первый codepoint объявленного диапазона (по умолчанию 32). Сейчас участвует
только в ключе кэша `FontManager`; на предварительное запекание не влияет
(см. ограничение 4).

```cpp
crossrender::FontDesc desc;
desc.firstCodepoint = 0x20;
ENG_LOGI("demo", "диапазон начинается с U+%04X", desc.firstCodepoint);
```

### `u32 FontDesc::lastCodepoint`

Последний codepoint объявленного диапазона (по умолчанию `0x2FFF`). Как и
`firstCodepoint`, пока влияет только на ключ кэша: остальные глифы
растеризуются по требованию при первом `GetGlyph`.

```cpp
crossrender::FontDesc desc;
desc.lastCodepoint = 0x04FF;   // вся основная кириллица
ENG_LOGI("demo", "диапазон до U+%04X", desc.lastCodepoint);
```

### `bool FontDesc::prebake`

Запечь диапазон заранее, чтобы первый кадр не тратил время на растеризацию.
Фактически вызывает `PrebakeAscii()` (32..126), а не объявленный диапазон;
для произвольного набора используйте `Prebake` вручную.

```cpp
crossrender::FontDesc desc;
desc.prebake = true;   // прогреваем ASCII при загрузке
crossrender::Font font;
font.LoadFromFile("assets/fonts/ubuntu.ttf", desc);
```

### `f32 FontDesc::gamma`

Показатель гаммы для покрытия (по умолчанию 1, то есть без коррекции).
Значение применяется степенью к покрытию перед упаковкой в байт: числа меньше
1 делают текст жирнее и мягче, больше 1 — тоньше.

```cpp
crossrender::FontDesc light;
light.gamma = 1.4f;   // тонкие светлые подписи на тёмном фоне
```

### `bool FontDesc::bold`

Синтетическая жирность: глиф растеризуется несколько раз со смещениями
(для контуров) или сдвигается и объединяется (для процедурного начертания).
Полезно, когда в семье нет жирного начертания; контуры при этом не меняются.

```cpp
crossrender::FontDesc desc;
desc.bold = true;
desc.boldAmount = 0.8f;
ENG_LOGI("demo", "синтетическая жирность: %.1f", desc.boldAmount);
```

### `f32 FontDesc::boldAmount`

Величина утолщения для синтетической жирности (по умолчанию 0.6). У контурных
начертаний задаёт радиус разброса дополнительных проходов, у процедурного —
сдвиг в пикселях, зависящий от `pixelHeight` (зажимается в `1..4`).

```cpp
crossrender::FontDesc desc;
desc.bold = true;
desc.boldAmount = 0.4f;   // аккуратное утолщение
```

### `bool FontDesc::italic`

Синтетический наклон (shear), если в семье нет курсива. Применяется и к
растеризации, и к контурам из `GetGlyphOutline`, поэтому текст и векторная
отрисовка выглядят одинаково.

```cpp
crossrender::FontDesc desc;
desc.italic = true;
desc.italicSlant = 0.25f;
ENG_LOGI("demo", "наклон %.2f", desc.italicSlant);
```

### `f32 FontDesc::italicSlant`

Коэффициент наклона (по умолчанию 0.25): горизонтальный сдвиг на единицу
высоты. Ноль отключает наклон даже при `italic = true`.

```cpp
crossrender::FontDesc desc;
desc.italic = true;
desc.italicSlant = 0.18f;   // мягкий курсив
```

### `u32 FontDesc::oversample`

Суперсэмплинг вертикального покрытия для растровых глифов (по умолчанию 2).
Значение зажимается в `1..4`, а затем умножается на 2: горизонтальное покрытие
считается аналитически и от этого поля не зависит. На SDF не влияет.

```cpp
crossrender::FontDesc crisp;
crisp.oversample = 4;   // максимум качества, дороже загрузка
```

### `struct Glyph`

Один растеризованный глиф в атласе: код символа, метрики в пикселях при
запечённом размере и адрес ячейки на странице. Глифы кэшируются внутри `Font`
и выдаются указателем на элемент кэша.

```cpp
const crossrender::Glyph* g = font.GetGlyph('g');
if (g) ENG_LOGI("demo", "U+%04X: advance %.1f, ячейка %.1fx%.1f", g->codepoint, g->advance,
                g->width, g->height);
```

### `u32 Glyph::codepoint`

Код символа Unicode (codepoint), для которого растеризован глиф. Именно по
нему устроен кэш `Font`, а не по индексу глифа в файле.

```cpp
const crossrender::Glyph* g = font.GetGlyph(0x0416);
if (g) ENG_LOGI("demo", "глиф для U+%04X", g->codepoint);
```

### `f32 Glyph::advance`

Горизонтальное продвижение пера в пикселях при `FontDesc::pixelHeight`. Чтобы
получить продвижение при другом кегле, умножьте на
`ScaleForSize(size)`.

```cpp
const crossrender::Glyph* g = font.GetGlyph('W');
const crossrender::f32 penAdvance = g ? g->advance * font.ScaleForSize(20.0f) : 0.0f;
```

### `f32 Glyph::bearingX`

Левое боковое поле (left side bearing): смещение левого края чернил от позиции
пера, в пикселях при `pixelHeight`. Может быть отрицательным (например, у
курсивных или выступающих глифов).

```cpp
const crossrender::Glyph* g = font.GetGlyph('j');
if (g) ENG_LOGI("demo", "чернила начинаются на %.1f px правее пера", g->bearingX);
```

### `f32 Glyph::bearingY`

Верхнее боковое поле (top side bearing): расстояние от базовой линии вверх до
верхнего края чернил, в пикселях. Положительное значение означает чернила над
базовой линией, поэтому при раскладке верх рисуется как `baseline - bearingY`.

```cpp
const crossrender::Glyph* g = font.GetGlyph('A');
if (g) ENG_LOGI("demo", "верх чернил на %.1f px выше базовой линии", g->bearingY);
```

### `f32 Glyph::width`

Ширина ячейки чернил в пикселях. Для глифа без чернил (пробел) — `0`.

```cpp
const crossrender::Glyph* g = font.GetGlyph('A');
r2d.Image(font.AtlasPage(g->page).texture, crossrender::Rect{0, 0, g->width, g->height},
          crossrender::Rect{g->u0, g->v0, g->u1 - g->u0, g->v1 - g->v0});
```

### `f32 Glyph::height`

Высота ячейки чернил в пикселях. У SDF-глифа включает поле `sdfSpread` сверху
и снизу, поэтому больше видимого контура.

```cpp
const crossrender::Glyph* g = font.GetGlyph('g');
if (g) ENG_LOGI("demo", "ячейка %.1fx%.1f px", g->width, g->height);
```

### `int Glyph::page`

Индекс страницы атласа, на которой лежит глиф. Текстура берётся как
`AtlasPage(page).texture`; страниц не может быть больше восьми.

```cpp
const crossrender::Glyph* g = font.GetGlyph('A');
if (g && g->page < font.AtlasPageCount()) {
    const crossrender::Texture& tex = font.AtlasPage(g->page).texture;
    ENG_LOGI("demo", "глиф на странице %d (%dx%d)", g->page, tex.Width(), tex.Height());
}
```

### `f32 Glyph::u0, Glyph::v0, Glyph::u1, Glyph::v1`

Нормализованные координаты ячейки внутри страницы атласа: `(u0, v0)` — левый
верхний угол, `(u1, v1)` — правый нижний. Начало координат — левый верхний угол
страницы, ось V направлена вниз; это готовые UV для `Renderer2D::Image`.

```cpp
const crossrender::Glyph* g = font.GetGlyph('A');
if (g) {
    const crossrender::Rect uv{g->u0, g->v0, g->u1 - g->u0, g->v1 - g->v0};
    ENG_LOGI("demo", "uv: %.4f, %.4f, %.4fx%.4f", uv.x, uv.y, uv.w, uv.h);
}
```

### `bool Glyph::isEmpty() const`

`true`, если у глифа нет чернил: `width <= 0` или `height <= 0`. Так выглядят
пробел, неразрывный пробел и любой символ, который начертание отображает, но
не рисует. Пустой глиф всё ещё имеет корректный `advance`, поэтому его нужно
учитывать в раскладке.

```cpp
const crossrender::Glyph* g = font.GetGlyph(' ');
if (g && g->isEmpty()) ENG_LOGI("demo", "пробел: чернил нет, advance %.1f", g->advance);
```

### `struct GlyphPoint`

Точка контура: координаты и признак «на кривой». Координаты — в том
пространстве, которое задал вызывающий метод (`GetGlyphOutline` — пиксели,
`GetGlyphOutlineUnits` — единицы шрифта), ось Y направлена вниз.

```cpp
crossrender::GlyphPoint p;
p.p = {12.0f, -8.0f};
p.onCurve = 1;   // точка лежит на контуре, а не управляет кривой
ENG_LOGI("demo", "точка %.1f, %.1f, onCurve=%d", p.p.x, p.p.y, p.onCurve);
```

### `Vec2 GlyphPoint::p`

Координаты точки. Начало координат — перо на базовой линии, ось Y вниз,
поэтому верх чернил имеет отрицательный `y`.

```cpp
for (const crossrender::GlyphPoint& pt : outline.contours.front().points) {
    ENG_LOGD("demo", "%.2f, %.2f", pt.p.x, pt.p.y);
}
```

### `u8 GlyphPoint::onCurve`

`1` — точка лежит на контуре, `0` — это контрольная точка квадратичной кривой.
Кубических контрольных точек в `GlyphOutline` не бывает: они преобразованы в
квадратичные.

```cpp
const crossrender::GlyphContour& c = outline.contours.front();
for (crossrender::usize i = 0; i < c.points.size(); ++i) {
    if (c.points[i].onCurve == 0) ENG_LOGD("demo", "точка %zu — управляющая", i);
}
```

### `struct GlyphContour`

Замкнутый контур глифа. Всегда закрыт: последняя точка повторяет первую, а
начало и конец лежат на кривой.

```cpp
crossrender::GlyphContour contour;
contour.points.push_back(crossrender::GlyphPoint{{0.0f, 0.0f}, 1});
contour.points.push_back(crossrender::GlyphPoint{{10.0f, 0.0f}, 1});
contour.points.push_back(crossrender::GlyphPoint{{0.0f, 0.0f}, 1});   // замыкание
contour.closed = true;
ENG_LOGI("demo", "точек в контуре: %zu", contour.points.size());
```

### `std::vector<GlyphPoint> GlyphContour::points`

Точки контура по порядку обхода. Чередование on/off-curve задаёт квадратичные
сегменты: каждая пара «on-curve — off-curve — on-curve» описывает одну дугу, а
две подряд идущие off-curve точки — сглаженный узел.

```cpp
crossrender::usize onCurve = 0;
for (const crossrender::GlyphPoint& pt : outline.contours[0].points) onCurve += pt.onCurve ? 1 : 0;
ENG_LOGI("demo", "on-curve точек: %zu", onCurve);
```

### `bool GlyphContour::closed`

Признак замкнутости контура. В `GlyphOutline` всегда `true`: незамкнутых
контуров API не отдаёт.

```cpp
for (const crossrender::GlyphContour& c : outline.contours) {
    ENG_ASSERT(c.closed);
}
```

### `struct GlyphOutline`

Векторное представление глифа: контуры, плотный прямоугольник чернил,
продвижение пера и признак «чернил нет». Именно эту структуру потребляет
аналитический (Slug-подобный) рендерер текста.

```cpp
crossrender::GlyphOutline outline;
if (font.GetGlyphOutline('Я', 48.0f, &outline) && !outline.empty) {
    ENG_LOGI("demo", "чернила %.1fx%.1f, контуров %zu", outline.bounds.w, outline.bounds.h,
             outline.contours.size());
}
```

### `std::vector<GlyphContour> GlyphOutline::contours`

Контуры глифа в порядке обхода. У составных глифов (например, «Ё» или
акцентированных букв) контуров несколько; направление обхода влияет на правило
заливки ненулевого индекса.

```cpp
for (crossrender::usize i = 0; i < outline.contours.size(); ++i) {
    ENG_LOGD("demo", "контур %zu: %zu точек", i, outline.contours[i].points.size());
}
```

### `Rect GlyphOutline::bounds`

Плотный прямоугольник чернил в том же пространстве, что и точки: пиксели,
ось Y вниз, начало — перо на базовой линии. Учитываются настоящие экстремумы
квадратичных кривых, а не только on-curve точки, поэтому это именно граница
изображения, а не охват контрольных точек. У пустого глифа — `{0,0,0,0}`.

```cpp
const crossrender::Rect b = outline.bounds;
// Верх чернил над базовой линией отрицателен по Y.
ENG_LOGI("demo", "чернила: x %.1f, верх %.1f, %.1fx%.1f", b.x, b.y, b.w, b.h);
```

### `f32 GlyphOutline::advance`

Продвижение пера для этого глифа в том же масштабе, что и контуры: в пикселях
запрошенного размера у `GetGlyphOutline` и в единицах шрифта у
`GetGlyphOutlineUnits`. Это отличается от `Glyph::advance`, который всегда
относится к `pixelHeight`.

```cpp
crossrender::GlyphOutline o;
font.GetGlyphOutline('A', 32.0f, &o);
const crossrender::f32 widthPx = o.advance;   // уже в пикселях при 32 px
```

### `bool GlyphOutline::empty`

`true`, если у глифа нет контуров. Возврат `true` с `empty = true` — штатный
результат для пробела и других глифов без чернил; ошибкой считается только
`false` от самого метода.

```cpp
crossrender::GlyphOutline o;
if (font.GetGlyphOutline(' ', 24.0f, &o) && o.empty) {
    ENG_LOGI("demo", "пробел: контуров нет, advance %.1f", o.advance);
}
```

### `struct FontAtlasPage`

Одна страница атласа глифов: текстура, состояние полочного упаковщика и
сторона страницы. Публичная структура, потому что рендерер текста берёт из неё
текстуру и SDF-параметры.

```cpp
for (int i = 0; i < font.AtlasPageCount(); ++i) {
    const crossrender::FontAtlasPage& p = font.AtlasPage(i);
    ENG_LOGI("demo", "страница %d: %dx%d, занято %dx%d", i, p.size, p.size, p.usedWidth,
             p.usedHeight);
}
```

### `Texture FontAtlasPage::texture`

Текстура страницы (`RGBA8`, фильтрация `Linear`, адресация `ClampToEdge`, без
мипмапов). Может быть невалидной, если контекста OpenGL ещё не было: пиксели
тогда ждут в очереди отложенной загрузки, а текстура создастся при первом
`GetGlyph` с живым контекстом.

```cpp
const crossrender::FontAtlasPage& page = font.AtlasPage(0);
if (page.texture.Valid()) {
    r2d.Image(page.texture, crossrender::Rect{0, 0, 256, 256}, crossrender::Rect{0, 0, 1, 1}, crossrender::Color::White);
}
```

### `int FontAtlasPage::usedWidth`

Курсор текущей полки по горизонтали: сколько пикселей строки уже занято
(включая зазор). Новый глиф ставится именно в эту позицию.

```cpp
const crossrender::FontAtlasPage& p = font.AtlasPage(0);
ENG_LOGI("demo", "полка заполнена на %d из %d px", p.usedWidth, p.size);
```

### `int FontAtlasPage::usedHeight`

Вертикальная позиция начала текущей полки. Когда строка заполнена,
`usedHeight` увеличивается на `rowHeight` плюс зазор, а `rowHeight` сбрасывается.

```cpp
ENG_LOGI("demo", "текущая полка начинается с y=%d", font.AtlasPage(0).usedHeight);
```

### `int FontAtlasPage::rowHeight`

Высота самой высокой ячейки на текущей полке. По ней вычисляется, куда
перейдёт упаковщик, когда строка закончится.

```cpp
ENG_LOGI("demo", "высота полки: %d px", font.AtlasPage(0).rowHeight);
```

### `int FontAtlasPage::size`

Сторона квадратной страницы в пикселях. Берётся из `FontDesc::atlasSize`
(минимум 16) и увеличивается, если самый большой глиф в неё не помещается.
Страница не растёт после создания: при нехватке места открывается новая.

```cpp
ENG_LOGI("demo", "атлас: %d страниц по %d px", font.AtlasPageCount(), font.AtlasPage(0).size);
```

### `struct GlyphQuad`

Готовая четвёрка для отрисовки одного глифа: куда рисовать, что сэмплировать и
с какой страницы. Используется рендерером текста, но полезна и в своём коде,
если нужно нарисовать глиф вручную.

```cpp
crossrender::GlyphQuad quad;
const crossrender::Glyph* g = font.GetGlyph('A');
if (g && !g->isEmpty()) {
    quad.dst = crossrender::Rect{100.0f, 100.0f, g->width, g->height};
    quad.uv = crossrender::Rect{g->u0, g->v0, g->u1 - g->u0, g->v1 - g->v0};
    quad.page = g->page;
}
```

### `Rect GlyphQuad::dst`

Прямоугольник назначения в пикселях, отсчитанный от позиции пера на базовой
линии: `x = bearingX`, `y = -bearingY`, размер — `width` на `height`.

```cpp
crossrender::GlyphQuad quad;
quad.dst = crossrender::Rect{0.0f, -12.0f, 10.0f, 12.0f};   // над базовой линией
```

### `Rect GlyphQuad::uv`

UV-прямоугольник ячейки в странице атласа: `{u0, v0, u1 - u0, v1 - v0}`.

```cpp
const crossrender::Glyph* g = font.GetGlyph('A');
crossrender::GlyphQuad quad;
quad.uv = crossrender::Rect{g->u0, g->v0, g->u1 - g->u0, g->v1 - g->v0};
```

### `int GlyphQuad::page`

Индекс страницы, с которой нужно сэмплировать глиф: `AtlasPage(page).texture`.

```cpp
crossrender::GlyphQuad quad;
quad.page = 0;
if (quad.page < font.AtlasPageCount()) {
    r2d.Image(font.AtlasPage(quad.page).texture, quad.dst, quad.uv, crossrender::Color::White);
}
```

### `Font()`

Создаёт пустой объект: нет файла, нет страниц, `Valid() == false`, формат
`Unknown`. Конструктор не обращается ни к диску, ни к GPU, поэтому его можно
вызывать до создания контекста OpenGL и хранить `Font` полем класса.

```cpp
struct Hud {
    crossrender::Font font;   // ещё не загружен, но уже безопасен
};
```

### `~Font()`

Освобождает страницы атласа (вместе с их текстурами), кэш глифов, список
фолбэков и копию файла шрифта. Деструктор не бросает исключений.

```cpp
{
    crossrender::Font local;
    local.LoadFromFile("assets/fonts/ubuntu.ttf");
}   // здесь атлас и текстуры освобождаются
```

### `Font(const Font&) = delete`, `Font& operator=(const Font&) = delete`

Копирование запрещено: `Font` владеет атласом GPU-текстур и кэшем глифов.
Храните шрифт по значению в одном месте или передавайте по ссылке либо
указателю.

```cpp
void DrawCaption(crossrender::Renderer2D& r2d, const crossrender::Font& font) {
    r2d.DrawText(font, "Подпись", 16.0f, 16.0f, crossrender::Color::White, 14.0f);
}
```

### `bool LoadFromFile(const std::string& path, const FontDesc& desc = {})`

Читает файл целиком (`ReadBinaryFile`) и загружает начертание с параметрами
`desc`. При неудачном чтении пишет `ENG_LOGE("font", "cannot read '%s'")` и
возвращает `false`. При успехе `SourcePath()` получает путь к файлу. Всё, что
было загружено раньше, предварительно освобождается (`Destroy()` внутри
`LoadFromMemory`).

```cpp
crossrender::Font font;
crossrender::FontDesc desc;
desc.pixelHeight = 20.0f;
if (!font.LoadFromFile("assets/fonts/ubuntu.ttf", desc)) {
    ENG_LOGW("demo", "шрифт не загрузился, беру встроенный");
}
```

### `bool LoadFromMemory(const void* data, usize size, const FontDesc& desc = {})`

Загружает шрифт из буфера в памяти. Данные **копируются** внутрь `Font`,
поэтому исходный буфер можно освободить сразу после вызова. Минимальный размер
— 12 байт; иначе пишется `ENG_LOGE` и возвращается `false`. Если это коллекция
TTC, читается только гарнитура 0 (с предупреждением). Файл CFF2 не
поддерживается: пишется предупреждение и происходит откат на `glyf`. При
`FontDesc::prebake` сразу вызывается `PrebakeAscii()`.

```cpp
const crossrender::ByteBuffer bytes = crossrender::ReadBinaryFile("assets/fonts/ubuntu.ttf");
crossrender::Font font;
if (!bytes.empty() && font.LoadFromMemory(bytes.data(), bytes.size(), crossrender::FontDesc{})) {
    ENG_LOGI("demo", "загружено семейство '%s'", font.FamilyName().c_str());
}
```

### `void Destroy()`

Возвращает шрифт в пустое состояние: очищает страницы атласа, кэш глифов,
**список фолбэков** и копию файла шрифта; `Valid()` становится `false`, формат
— `Unknown`. Вызывается в начале каждой загрузки, поэтому перезагрузка поверх
существующего `Font` полностью его заменяет. Учтите, что вместе с кэшем
пропадают и фолбэки — их нужно добавлять заново.

```cpp
font.Destroy();
ENG_ASSERT(!font.Valid());
font.LoadFromFile("assets/fonts/other.ttf");   // загрузка сама вызывает Destroy
```

### `bool Valid() const`

`true`, если шрифт разобран и готов растеризовать глифы. Не говорит ни о том,
есть ли у начертания контуры, ни о том, загрузились ли текстуры страниц (без
контекста OpenGL они создаются позже, а `Valid()` уже истинно).

```cpp
if (!font.Valid()) {
    ENG_LOGE("demo", "невалидный шрифт: ни один GetGlyph не сработает");
}
```

### `bool IsSdf() const`

`true`, если начертание запекалось с `FontDesc::sdf`. Для такого атласа нужен
SDF-шейдер текста, а `AtlasPage(i).texture` несёт `SdfSpread()`/`SdfSize()`.

```cpp
r2d.BeginFrame(1280, 720);
if (font.IsSdf()) textShader.Set("uSdfSpread", font.AtlasPage(0).texture.SdfSpread());
```

### `FontFormat Format() const`

Формат загруженного начертания (`TrueType`, `OpenTypeCFF` или `Bitmap`), либо
`Unknown` у незагруженного. Для TTC возвращается формат первой гарнитуры.

```cpp
if (font.Format() == crossrender::FontFormat::Bitmap) {
    ENG_LOGW("demo", "контуров нет: GetGlyphOutline вернёт пустой результат");
}
```

### `const FontDesc& Desc() const`

Описание, с которым начертание загружено. Возвращается ссылка на внутреннюю
копию, поэтому по ней можно узнать `pixelHeight` (размер запекания), `sdf` и
остальные параметры — например, чтобы передать их дальше или построить ключ
кэша.

```cpp
ENG_LOGI("demo", "запечено на %.0f px, sdf=%d, атлас %u px", font.Desc().pixelHeight,
         font.Desc().sdf ? 1 : 0, font.Desc().atlasSize);
```

### `const std::string& FamilyName() const`

Имя семейства из таблицы `name` (для процедурного начертания — `"Engine
Built-in"`). Если таблица имён отсутствует, возвращается `"(unnamed)"`.

```cpp
ENG_LOGI("demo", "семейство: %s, начертание: %s", font.FamilyName().c_str(),
         font.StyleName().c_str());
```

### `const std::string& StyleName() const`

Имя начертания из таблицы `name` (`"Regular"`, `"Bold"`, `"Italic"`). Может
быть пустым, если файл не содержит подходящей записи; для процедурного
начертания — `"Regular"`.

```cpp
if (font.StyleName().find("Bold") != std::string::npos) {
    ENG_LOGI("demo", "это уже жирное начертание, синтетика не нужна");
}
```

### `const std::string& SourcePath() const`

Путь, из которого шрифт загружен через `LoadFromFile`. У шрифта из памяти и у
процедурного начертания строка пуста.

```cpp
if (font.SourcePath().empty()) ENG_LOGD("demo", "шрифт не из файла");
else ENG_LOGI("demo", "источник: %s", font.SourcePath().c_str());
```

### `f32 Ascender() const`

Высота над базовой линией в пикселях при запечённом размере: расстояние от
базовой линии до верха строки. Используется для вертикальной привязки текста
(`TextBaseline::Top` отступает от базовой линии именно на `Ascender`).

```cpp
const crossrender::f32 scale = font.ScaleForSize(18.0f);
const crossrender::f32 topY = 40.0f + font.Ascender() * scale;
ENG_LOGI("demo", "базовая линия окажется на y=%.1f", topY);
```

### `f32 Descender() const`

Глубина под базовой линией в пикселях — **отрицательное** число. По модулю
примерно равен высоте выносных элементов нижнего регистра и используемых
подстрочных знаков.

```cpp
const crossrender::f32 descent = -font.Descender() * font.ScaleForSize(18.0f);
ENG_LOGI("demo", "под базовой линией нужно %.1f px", descent);
```

### `f32 LineGap() const`

Дополнительный межстрочный интервал из шрифта (в пикселях при запечённом
размере). Может быть нулевым или даже отрицательным; интерфейсы обычно
добавляют к нему собственный коэффициент.

```cpp
const crossrender::f32 gap = font.LineGap() * font.ScaleForSize(16.0f);
ENG_LOGI("demo", "рекомендуемый зазор между строками: %.1f px", gap);
```

### `f32 LineHeight() const`

Полная высота строки в пикселях при запечённом размере:
`Ascender - Descender + LineGap`. Если таблицы дают неположительное значение,
подставляется `pixelHeight`, чтобы раскладка не «схлопнулась».

```cpp
const crossrender::f32 lh = font.LineHeight() * font.ScaleForSize(16.0f);
for (crossrender::usize i = 0; i < lines.size(); ++i) {
    r2d.DrawText(font, lines[i], 24.0f, 24.0f + static_cast<crossrender::f32>(i) * lh, crossrender::Color::White, 16.0f);
}
```

### `f32 UnitsPerEm() const`

Число единиц шрифта в одном em: у TrueType обычно 2048, у CFF — 1000, у
процедурного начертания — 1024. Нужен для перевода кернинга (он в единицах
шрифта) в пиксели.

```cpp
const crossrender::f32 kernPx = font.GetKerning('T', 'o') * (16.0f / font.UnitsPerEm());
ENG_LOGI("demo", "кернинг 'To' при 16 px: %.2f", kernPx);
```

### `f32 CapHeight() const`

Высота прописных букв в пикселях при запечённом размере. Берётся из OS/2, а
если её там нет — оценивается как `0.7 * em`.

```cpp
ENG_LOGI("demo", "высота прописных: %.1f px (x-height %.1f)", font.CapHeight(), font.XHeight());
```

### `f32 XHeight() const`

Высота строчных букв без выносных элементов в пикселях. Берётся из OS/2, иначе
оценивается как `0.5 * em`.

```cpp
if (font.XHeight() < 6.0f) ENG_LOGW("demo", "мелкий кегль: строчные могут слипаться");
```

### `f32 UnderlinePosition() const`

Смещение линии подчёркивания от базовой линии в пикселях (обычно
отрицательное). Если в `post`-таблице значений нет, берётся `-0.1 * em`.

```cpp
const crossrender::f32 y = baseline + font.UnderlinePosition() * font.ScaleForSize(16.0f);
ENG_LOGI("demo", "подчёркивание на y=%.1f", y);
```

### `f32 UnderlineThickness() const`

Толщина линии подчёркивания в пикселях при запечённом размере. Никогда не
меньше 1 px — иначе линия была бы невидимой.

```cpp
const crossrender::f32 th = font.UnderlineThickness() * font.ScaleForSize(16.0f);
r2d.FillRect(crossrender::Rect{x, y, width, th}, crossrender::Color::White);
```

### `f32 ScaleForSize(f32 size) const`

Множитель перевода из запечённого размера в запрошенный: `size / pixelHeight`.
При `size <= 0` возвращает `1`, то есть «натуральный» размер атласа. Метрики,
`advance` и `bearing` запечённого начертания умножаются на это значение.
Кернинг — исключение: он в единицах шрифта и в этом множителе не нуждается.

```cpp
const crossrender::f32 scale = font.ScaleForSize(24.0f);
const crossrender::Glyph* g = font.GetGlyph('A');
if (g) ENG_LOGI("demo", "ячейка при 24 px: %.1fx%.1f", g->width * scale, g->height * scale);
```

### `const Glyph* GetGlyph(u32 codepoint) const`

Главный метод доступа к глифу. При первом обращении растеризует глиф в
запечённом размере, кладёт его в атлас и кэширует; повторные вызовы возвращают
указатель на элемент кэша. Возвращает `nullptr`, если шрифт невалиден, если
codepoint не отображён (и начертание не процедурное), если растеризация или
упаковка не удались (например, атлас переполнен) — неудачный глиф при этом не
кэшируется, и следующий вызов попробует снова.

Метод объявлен `const`, но меняет внутреннее состояние (ленивый кэш, страницы,
очередь отложенной загрузки) и не является потокобезопасным. Перед работой он
проверяет очередь отложенных загрузок и, если контекст OpenGL уже есть,
создаёт текстуры страниц и загружает накопленные пиксели.

```cpp
const crossrender::Glyph* g = font.GetGlyph(0x0416);   // Ж
if (g && !g->isEmpty()) {
    ENG_LOGI("demo", "advance %.1f px, страница %d", g->advance, g->page);
} else {
    ENG_LOGW("demo", "глифа нет или он пуст");
}
```

### `bool GetGlyphOutline(u32 codepoint, f32 size, GlyphOutline* out) const`

Извлекает векторный контур глифа, отмасштабированный к `size` пикселям (размер
em, а не кегль строки). Если `size <= 0`, берётся `FontDesc::pixelHeight`.
Координаты — в пикселях, ось Y вниз, начало координат — перо на базовой линии;
кривые только квадратичные (кубические сегменты CFF преобразованы), контуры
замкнуты и начинаются с on-curve точки, `bounds` — плотный прямоугольник
чернил. Наклон `italic` применяется, синтетическая жирность — нет.

Возвращает `true` и для глифа без чернил, выставляя `out->empty = true`;
`false` означает, что `out == nullptr`, шрифт невалиден, у начертания нет
контуров (`glyf`/CFF отсутствуют) или codepoint не отображён.

```cpp
crossrender::GlyphOutline outline;
if (font.GetGlyphOutline('S', 40.0f, &outline)) {
    for (const crossrender::GlyphContour& c : outline.contours) {
        ENG_LOGD("demo", "контур: %zu точек, замкнут=%d", c.points.size(), c.closed ? 1 : 0);
    }
    ENG_LOGI("demo", "чернила %.1fx%.1f", outline.bounds.w, outline.bounds.h);
}
```

### `bool GetGlyphOutlineUnits(u32 codepoint, GlyphOutline* out) const`

То же, что `GetGlyphOutline`, но без масштабирования: координаты и `advance`
остаются в единицах шрифта. Удобно, чтобы один раз построить атлас
аналитического рендерера и масштабировать контуры на стороне шейдера.

```cpp
crossrender::GlyphOutline units;
if (font.GetGlyphOutlineUnits('S', &units) && !units.empty) {
    ENG_LOGI("demo", "advance %.0f единиц шрифта (em = %.0f)", units.advance, font.UnitsPerEm());
}
```

### `f32 GetKerning(u32 left, u32 right) const`

Кернинг пары символов **в единицах шрифта**. Сначала проверяется таблица
`kern`, затем GPOS PairPos. Если хотя бы один из символов не отображён,
возвращается `0`. Чтобы получить пиксели при кегле `size`, умножьте результат
на `size / UnitsPerEm()` — так это делает `Renderer2D`.

```cpp
const crossrender::f32 units = font.GetKerning('A', 'V');
const crossrender::f32 px = units * (18.0f / font.UnitsPerEm());
ENG_LOGI("demo", "кернинг AV: %.1f единиц (%.2f px при 18 px)", units, px);
```

### `bool HasGlyph(u32 codepoint) const`

`true`, если `cmap` отображает codepoint в реальный глиф (`GlyphIndex != 0`).
Это проверка **наличия отображения**, а не чернил: глиф может оказаться пустым
или вовсе без контуров. У процедурного начертания истинно почти для любого
codepoint младше 65 535.

```cpp
if (font.HasGlyph(0x4E2D)) {
    ENG_LOGI("demo", "иероглиф есть в таблице символов");
} else {
    ENG_LOGD("demo", "придётся искать фолбэк");
}
```

### `u32 GlyphIndex(u32 codepoint) const`

Индекс глифа в файле шрифта; `0` — это `.notdef`, то есть «не отображён».
Полезен для отладки и для сопоставления с собственными таблицами. Если у
шрифта нет разобранной подтаблицы `cmap`, применяется тривиальное отображение
«codepoint = индекс глифа» (при `codepoint < numGlyphs`).

```cpp
const crossrender::u32 gid = font.GlyphIndex('@');
ENG_LOGI("demo", "'@' -> глиф #%u", gid);
```

### `int AtlasPageCount() const`

Число созданных страниц атласа: от 0 (ни один глиф ещё не запрошен) до 8. При
первом `GetGlyph` появится первая страница.

```cpp
ENG_LOGI("demo", "страниц атласа: %d", font.AtlasPageCount());
```

### `const FontAtlasPage& AtlasPage(int i) const`

Доступ к странице атласа. **Проверки диапазона нет**: при `i < 0` или
`i >= AtlasPageCount()` поведение не определено, поэтому сначала проверяйте
`AtlasPageCount()`. У ещё не тронутого шрифта страниц вообще нет.

```cpp
if (font.AtlasPageCount() > 0) {
    const crossrender::FontAtlasPage& page = font.AtlasPage(0);
    ENG_LOGI("demo", "страница 0: %dx%d, текстура валидна=%d", page.size, page.size,
             page.texture.Valid() ? 1 : 0);
}
```

### `int GlyphCount() const`

Сколько глифов уже лежит в кэше. Значение растёт по мере растеризации: пустой
шрифт даёт `0`, а после `PrebakeAscii()` — около 95. Удобно для тестов и для
оценки прогрева.

```cpp
font.PrebakeAscii();
ENG_LOGI("demo", "в кэше %d глифов на %d страницах", font.GlyphCount(), font.AtlasPageCount());
```

### `f32 AtlasOccupancy() const`

Заполнение атласа в `[0,1]`: площадь, занятая ячейками глифов (с учётом
внутренних зазоров), делённая на суммарную площадь страниц. Пока страниц нет,
возвращает `0`. По значению близко к 1 пора увеличивать `atlasSize` или
разбивать набор на несколько начертаний.

```cpp
if (font.AtlasOccupancy() > 0.9f) {
    ENG_LOGW("demo", "атлас заполнен на %.0f%% — следующий глиф может не поместиться",
             font.AtlasOccupancy() * 100.0f);
}
```

### `void AddFallback(Font* font)`

Добавляет шрифт в конец цепочки фолбэков. `nullptr` и сам себя игнорирует,
повторное добавление того же указателя ничего не меняет. Владение **не**
передаётся: фолбэк должен быть жив, пока жив основной шрифт. Порядок добавления
определяет порядок поиска в `Resolve`. `Destroy()` очищает список.

```cpp
crossrender::Font ui, symbols;
ui.LoadFromFile("assets/fonts/ubuntu.ttf");
symbols.LoadFromFile("assets/fonts/symbols.ttf");
ui.AddFallback(&symbols);
```

### `Font* Resolve(u32 codepoint, const Glyph** glyphOut) const`

Ищет глиф в этом шрифте, затем в фолбэках по порядку добавления. При успехе
записывает указатель на глиф в `*glyphOut` и возвращает шрифт-владелец (то есть
именно у него нужно брать `AtlasPage`), иначе обнуляет `*glyphOut` и возвращает
`nullptr`. У фолбэка принимается только непустой глиф
(`!Glyph::isEmpty()`), поэтому пустой пробел из основного шрифта не помешает
подставить символ из фолбэка. Имя `glyphOut` может быть `nullptr`, если нужен
только владелец.

```cpp
const crossrender::Glyph* g = nullptr;
if (crossrender::Font* owner = ui.Resolve(0x20AC /* € */, &g)) {
    const crossrender::Texture& atlas = owner->AtlasPage(g->page).texture;   // страница владельца!
    ENG_LOGI("demo", "знак евро дал шрифт '%s'", owner->FamilyName().c_str());
}
```

### `void Prebake(const u32* codepoints, int count)`

Растеризует и укладывает в атлас перечисленные codepoint'ы, чтобы первый кадр
не тратил время на растеризацию. `nullptr` и неположительный `count`
игнорируются. Полезно для прогрева экрана загрузки или для тестов, которым
нужен детерминированный набор глифов.

```cpp
const crossrender::u32 title[] = {'У', 'Р', 'О', 'В', 'Е', 'Н', 'Ь', '7'};
font.Prebake(title, 8);
ENG_LOGI("demo", "после прогрева %d глифов", font.GlyphCount());
```

### `void PrebakeAscii()`

Растеризует весь печатаемый ASCII (32..126). Именно этот метод вызывает
`FontDesc::prebake` и `LoadFromMemory` при `prebake = true`; объявленный
диапазон `firstCodepoint..lastCodepoint` на него не влияет.

```cpp
crossrender::Font font;
font.LoadFromFile("assets/fonts/ubuntu.ttf");
font.PrebakeAscii();   // ~95 глифов заранее
ENG_LOGI("demo", "страниц: %d, заполнение %.1f%%", font.AtlasPageCount(),
         font.AtlasOccupancy() * 100.0f);
```

### `static FontManager& Get()`

Доступ к единственному экземпляру менеджера (статическая локальная переменная,
создаётся при первом обращении). Менеджер владеет загруженными шрифтами и
кэширует их по ключу «путь + все поля `FontDesc`».

```cpp
crossrender::FontManager& mgr = crossrender::FontManager::Get();
ENG_LOGI("demo", "менеджер шрифтов доступен: %p", static_cast<void*>(&mgr));
```

### `Font* FontManager::Load(const std::string& path, const FontDesc& desc = {})`

Загружает шрифт и запоминает его в кэше. Повторный вызов с тем же путём и тем
же `FontDesc` возвращает уже загруженный объект, а не читает файл заново; любое
различие в `FontDesc` (включая `pixelHeight`, `sdf` и `atlasSize`) даёт
отдельную запись. Возвращает `nullptr` при пустом пути и при ошибке загрузки (с
`ENG_LOGE`); неудачная попытка не кэшируется, поэтому её можно повторить.
Владельцем остаётся менеджер.

```cpp
crossrender::FontManager& mgr = crossrender::FontManager::Get();
crossrender::FontDesc ui;
ui.pixelHeight = 16.0f;
crossrender::Font* a = mgr.Load("assets/fonts/ubuntu.ttf", ui);
crossrender::Font* b = mgr.Load("assets/fonts/ubuntu.ttf", ui);
ENG_ASSERT(a == b);                       // одна и та же запись кэша
crossrender::FontDesc big = ui;
big.pixelHeight = 48.0f;
crossrender::Font* c = mgr.Load("assets/fonts/ubuntu.ttf", big);
ENG_ASSERT(c != a);                       // другой размер — другой атлас
```

### `Font* FontManager::DefaultFont()`

Возвращает встроенное процедурное начертание (48 px, без SDF) и создаёт его
при первом обращении. Это последнее звено цепочки фолбэков: начертание не
требует файлов на диске, синтезирует хоть какой-то глиф для любого codepoint и
не бывает `nullptr`, кроме случая сбоя создания (тогда пишется `ENG_LOGE`).
Начертание хранится в менеджере и живёт до `Clear()`.

```cpp
crossrender::Font* builtin = crossrender::FontManager::Get().DefaultFont();
if (builtin) ui.AddFallback(builtin);   // гарантированный последний фолбэк
```

### `Font* FontManager::DefaultSdfFont()`

Возвращает встроенное процедурное начертание, помеченное как SDF (48 px,
`sdfSpread = 6`). При неудаче создания возвращает `DefaultFont()`. Учтите
ограничение: процедурный растеризатор всё равно выдаёт покрытие из таблицы
5x7, поэтому настоящего поля расстояний в атласе нет — `IsSdf()` истинно, а
SDF-шейдер к такому атласу применять нельзя.

```cpp
crossrender::Font* sdf = crossrender::FontManager::Get().DefaultSdfFont();
if (sdf && sdf->IsSdf()) {
    ENG_LOGW("demo", "процедурный SDF: пиксели растровые, нужен обычный шейдер");
}
```

### `void SetDefaultFont(Font* f)`

Подменяет шрифт, который `DefaultFont()` будет возвращать дальше. Владение не
передаётся: указатель должен оставаться живым. Полезно, чтобы подменить
встроенное начертение своим (например, загруженным из ассетов).

```cpp
crossrender::Font mine;
mine.LoadFromFile("assets/fonts/ubuntu.ttf");
crossrender::FontManager::Get().SetDefaultFont(&mine);
ENG_ASSERT(crossrender::FontManager::Get().DefaultFont() == &mine);
```

### `void Clear()`

Освобождает все шрифты, которыми владеет менеджер, и очищает кэш вместе с
указателями по умолчанию. После вызова **все** указатели, выданные `Load`,
`DefaultFont` и `DefaultSdfFont`, становятся висячими; `SetDefaultFont`
придётся вызвать заново. Используйте при смене уровня или перед потерей
контекста OpenGL.

```cpp
crossrender::FontManager::Get().Clear();
ENG_LOGW("demo", "указатели на шрифты больше не действительны");
```

### `void Shutdown()`

Полный синоним `Clear()` — освобождает кэш и владельцев. Вызывайте при
завершении приложения или перед уничтожением контекста OpenGL, чтобы текстуры
атласов освободились, пока драйвер ещё жив.

```cpp
void OnShutdown() {
    crossrender::FontManager::Get().Shutdown();
    ENG_LOGI("demo", "шрифтовые атласы освобождены");
}
```

### `u32 Utf8Decode(const char* s, usize len, usize* i)`

Декодирует один codepoint UTF-8, начиная с байта `*i`, и передвигает `*i` на
длину последовательности. Поддерживаются последовательности длиной 1–4 байта;
переполненные кодировки, суррогаты и значения выше `0x10FFFF` отбраковываются.
Некорректный байт даёт `U+FFFD` и сдвигает индекс ровно на один байт, поэтому
цикл всегда завершается. Когда `*i >= len`, возвращается `0` и `*i`
устанавливается в `len`. При `s == nullptr` или `i == nullptr` возвращается
`U+FFFD`.

```cpp
const std::string text = "Привет";
crossrender::usize i = 0;
while (i < text.size()) {
    const crossrender::u32 cp = crossrender::Utf8Decode(text.c_str(), text.size(), &i);
    if (cp == 0) break;
    ENG_LOGD("demo", "U+%04X", cp);
}
```

### `std::vector<u32> Utf8ToCodepoints(const std::string& s)`

Разбирает строку целиком в вектор codepoint'ов. Некорректные байты заменяются
на `U+FFFD` — функция никогда не «съедает» данные молча и не зацикливается.

```cpp
const std::vector<crossrender::u32> cps = crossrender::Utf8ToCodepoints("УРОВЕНЬ 7");
ENG_LOGI("demo", "символов: %zu, первый U+%04X", cps.size(), cps.empty() ? 0 : cps.front());
```

### `std::string CodepointsToUtf8(const std::vector<u32>& cps)`

Собирает строку UTF-8 из codepoint'ов. Недопустимые значения (суррогаты и всё
выше `0x10FFFF`) заменяются на `U+FFFD`, поэтому на выходе всегда корректный
UTF-8.

```cpp
const std::vector<crossrender::u32> cps = {'F', 'P', 'S', ' ', 0x221E};   // ∞
const std::string text = crossrender::CodepointsToUtf8(cps);
r2d.DrawText(font, text, 24.0f, 24.0f, crossrender::Color::White, 18.0f);
```

### `usize Utf8Length(const std::string& s)`

Число codepoint'ов в строке — не байт. Удобно для ограничений ввода и для
подсчёта позиции каретки.

```cpp
ENG_LOGI("demo", "байт %zu, символов %zu", text.size(), crossrender::Utf8Length(text));
```

### `usize Utf8Offset(const std::string& s, usize index)`

Байтовое смещение `index`-го codepoint'а (с нуля). Если `index` больше или
равен числу codepoint'ов, возвращается длина строки, поэтому результат всегда
безопасно передавать в `substr`.

```cpp
const std::string tail = text.substr(crossrender::Utf8Offset(text, 3));   // без первых 3 символов
ENG_LOGI("demo", "остаток: %s", tail.c_str());
```

## Пример целиком

```cpp
#include "crossrender/text/Font.h"

#include "crossrender/core/File.h"
#include "crossrender/core/Log.h"
#include "crossrender/gfx/Renderer2D.h"

#include <string>
#include <vector>

// Готовит пару начертаний для UI и заголовков, строит цепочку фолбэков и
// рисует кадр: обычный текст мелким кеглем, крупный текст из отдельного
// атласа, векторный контур и диагностику атласа.
void DrawFontDemo(crossrender::Renderer2D& r2d) {
    // 1. UI-начертание печём на 16 px — ровно под тот кегль, которым рисуем.
    crossrender::FontDesc uiDesc;
    uiDesc.pixelHeight = 16.0f;
    uiDesc.hinting = true;
    uiDesc.atlasSize = 1024;

    // 2. Заголовочное начертание — другой атлас на 48 px: 48 -> 40 это мягкое
    //    уменьшение, а не четырёхкратный «раздув» 16-пиксельного атласа.
    crossrender::FontDesc titleDesc;
    titleDesc.pixelHeight = 48.0f;
    titleDesc.hinting = true;

    crossrender::FontManager& mgr = crossrender::FontManager::Get();
    crossrender::Font* ui = mgr.Load("assets/fonts/ubuntu.ttf", uiDesc);
    crossrender::Font* title = mgr.Load("assets/fonts/ubuntu.ttf", titleDesc);
    if (!ui) ui = mgr.DefaultFont();          // встроенное начертание: без файлов
    if (!title) title = ui;

    // 3. Фолбэк для символов, которых нет в основной семье.
    crossrender::Font symbols;
    if (symbols.LoadFromFile("assets/fonts/symbols.ttf")) ui->AddFallback(&symbols);
    if (crossrender::Font* builtin = mgr.DefaultFont()) ui->AddFallback(builtin);

    // 4. Прогрев: первый кадр не должен растеризовывать ASCII.
    ui->PrebakeAscii();
    const crossrender::u32 titleChars[] = {'У', 'Р', 'О', 'В', 'Е', 'Н', 'Ь'};
    title->Prebake(titleChars, 7);

    // 5. Метрики запечённого начертания — в пикселях, множитель даёт ScaleForSize.
    const crossrender::f32 scale = title->ScaleForSize(40.0f);
    ENG_LOGI("demo", "'%s': ascender %.1f px, строка %.1f px, множитель к 40 px %.2f",
             title->FamilyName().c_str(), title->Ascender(), title->LineHeight(), scale);

    // 6. Отрисовка. Кернинг внутри Renderer2D считается сам.
    r2d.DrawText(*ui, "Здоровье: 100", 24.0f, 24.0f, crossrender::Color::White, 16.0f);
    r2d.DrawText(*title, "УРОВЕНЬ 7", 24.0f, 80.0f, crossrender::Color{1.0f, 0.85f, 0.4f, 1.0f}, 40.0f);
    const crossrender::TextMetrics m = crossrender::MeasureText(*title, "УРОВЕНЬ 7", 40.0f);
    ENG_LOGI("demo", "заголовок занимает %.1fx%.1f px, строк %d", m.width, m.height, m.lineCount);

    // 7. Символ, которого нет в основной семье: Resolve вернёт владельца глифа,
    //    и страницу атласа нужно брать именно у него.
    const crossrender::Glyph* g = nullptr;
    if (crossrender::Font* owner = ui->Resolve(0x20AC /* € */, &g)) {
        ENG_LOGI("demo", "евро рисует '%s', страница %d", owner->FamilyName().c_str(), g->page);
    } else {
        ENG_LOGW("demo", "нет ни фолбэка, ни встроенного начертания");
    }

    // 8. Векторный контур — для аналитического рендерера текста: только
    //    квадратичные кривые, замкнутые контуры, плотный bounds.
    crossrender::GlyphOutline outline;
    if (title->GetGlyphOutline('Я', 64.0f, &outline) && !outline.empty) {
        ENG_LOGI("demo", "'Я': контуров %zu, чернила %.1fx%.1f, advance %.1f",
                 outline.contours.size(), outline.bounds.w, outline.bounds.h, outline.advance);
        for (const crossrender::GlyphContour& c : outline.contours) {
            for (const crossrender::GlyphPoint& pt : c.points) {
                if (pt.onCurve == 0) ENG_LOGD("demo", "управляющая точка %.1f, %.1f", pt.p.x, pt.p.y);
            }
        }
    }

    // 9. Здоровье атласа: число страниц, кэш глифов и заполнение.
    ENG_LOGI("demo", "атлас UI: %d страниц, %d глифов, заполнение %.0f%%", ui->AtlasPageCount(),
             ui->GlyphCount(), ui->AtlasOccupancy() * 100.0f);
    for (int i = 0; i < ui->AtlasPageCount(); ++i) {
        const crossrender::FontAtlasPage& page = ui->AtlasPage(i);
        if (!page.texture.Valid()) ENG_LOGW("demo", "страница %d ещё не загружена в GPU", i);
    }

    // 10. Освобождение. Clear() делает недействительными ui/title — после него
    //     их нельзя использовать, поэтому зовём только на выходе из приложения.
    // crossrender::FontManager::Get().Shutdown();
}
```

## См. также

* `docs/gfx/Renderer2D.md` — `DrawText` и `TextStyle`, а также `MeasureText`,
  `WrapText`, `EllipsizeText`, `TextIndexAt` и `TextMetrics`: измерение и
  раскладка текста живут там, а не в `Font.h`.
* `docs/gfx/Texture.md` — `Texture`, которым является страница атласа, включая
  `SdfSpread`/`SdfSize` и поведение без контекста OpenGL.
* `docs/gfx/SlugText.md` — аналитический рендерер текста, потребляющий
  `GlyphOutline` из `GetGlyphOutline` / `GetGlyphOutlineUnits`.
* `docs/gfx/Shader.md` — шейдер текста и параметры SDF, которые страница
  атласа передаёт через `Texture::SdfSpread` / `Texture::SdfSize`.
* `docs/core/File.md` — `ReadBinaryFile`, которым `LoadFromFile` читает файл
  шрифта, и `ByteBuffer` для загрузки из памяти.
* `docs/core/Log.md` — макросы `ENG_LOGE` / `ENG_LOGW`: в лог с тегом `font`
  попадают сообщения о TTC, CFF2 и переполнении атласа.
* `docs/core/Base.md` — типы `u8`, `u32`, `f32`, `usize`, используемые во всём
  API шрифтов.

