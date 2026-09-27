# crossrender/gfx/SpriteAtlas.h — атласы спрайтов: дескрипторы, упаковка и отрисовка

Атлас спрайтов — это одна текстура (страница), в которой упаковано много
именованных областей (регионов), плюс дескриптор, описывающий, где каждая
область лежит. Заголовок объявляет структуры региона, страницы, анимации,
параметры упаковки и класс `SpriteAtlas`, который умеет читать дескрипторы
популярных упаковщиков, собирать атлас из отдельных картинок и рисовать
регионы через `Renderer2D`.

## Заголовок

```cpp
#include "crossrender/gfx/SpriteAtlas.h"
```

## Обзор

`SpriteAtlas` — CPU-first-компонент. Разбор дескриптора никогда не трогает GPU:
текст дескриптора читается с диска, из него строятся `AtlasRegion` и
`AtlasPageDesc`, а изображения страниц загружаются отдельно, обычным
`Texture::LoadFromFile`. Из этого следуют два важных свойства:

* атлас можно разобрать и осмотреть без окна и видеокарты — регионы, имена,
  предупреждения и статистика доступны всегда;
* отсутствие картинки страницы не является ошибкой загрузки: регионы остаются,
  `Valid()` возвращает `true`, но рисование таких регионов становится
  невозможным.

Типичный порядок работы:

1. Загрузить дескриптор: `LoadFromFile`, `LoadFromMemory`, `LoadPlainImage` или
   построить атлас из картинок через `BuildFromFiles`.
2. Проверить `Warnings()` (и, при необходимости, `PageTexture(i).Valid()`) —
   именно там окажутся сообщения об отсутствующих страницах.
3. Найти регион: `IndexOf`, `Find`, `Has`, `RegionAt`, `Regions`, `Names`,
   `NamesWithPrefix`.
4. Рисовать: `Draw`, `DrawAnchored`, `DrawRegion`, `DrawNinePatch`, `DrawTiled`,
   `DrawAnimation`, `DrawAnimationFrame`.
5. Для процедурной генерации — `AddRegion` + `SetPageImage`, для сохранения —
   `SaveToFile`.

### Поддерживаемые форматы дескрипторов

Формат определяется по **содержимому** файла, а не по расширению
(`DetectFormat`). Понимаются шесть текстовых раскладок и «голая картинка»:

| `AtlasFormat` | Что это | Характерные признаки |
|---|---|---|
| `EngineJson` | собственный формат движка (надмножество хеш-раскладки TexturePacker) | `meta.app == "CrossRender"` либо наличие `pages` / `animations` |
| `TexturePackerHash` | JSON TexturePacker, `frames` — объект с ключами-именами | `meta.app` содержит `texturepacker`, `frames` — объект |
| `TexturePackerArray` | JSON TexturePacker, `frames` — массив | `frames` — массив без `duration` у первого кадра |
| `Aseprite` | JSON-экспорт Aseprite | `meta.app` содержит `aseprite`, есть `frameTags` или `duration` у кадров |
| `SparrowXml` | XML `<TextureAtlas><SubTexture .../></TextureAtlas>` | первый непробельный символ `<` и есть `<textureatlas` |
| `LibGdx` | текстовый формат `.atlas` | в тексте есть `xy:` и `size:`, первая строка не начинается с `<`, `{`, `[` и не содержит `:` |
| `PlainImage` | одна картинка = один регион | одна строка без перевода строки, оканчивающаяся на известное расширение изображения |
| `Unknown` | не распознано | всё остальное |

```cpp
// Формат — свойство содержимого: переименование .atlas в .json ничего не меняет.
const crossrender::AtlasFormat a = crossrender::SpriteAtlas::DetectFormat("atlas.png\nsize: 512,512\n");
const crossrender::AtlasFormat b = crossrender::SpriteAtlas::DetectFormat("{\"frames\":{}}");
ENG_LOGI("demo", "%s / %s", crossrender::SpriteAtlas::FormatName(a), crossrender::SpriteAtlas::FormatName(b));
```

### Определение формата и запасной путь «голая картинка»

`LoadFromFile` читает файл целиком и вызывает `DetectFormat`. Если формат
`Unknown`, движок пробует декодировать файл как изображение (`DecodeImageFile`)
и в случае успеха делегирует в `LoadPlainImage`: так открывается обычный PNG без
всякого дескриптора. Если и это не удалось — записывается предупреждение
`atlas: unrecognised descriptor format: ...` и возвращается `false`.

Если `DetectFormat` вернул `PlainImage`, содержимое файла трактуется как путь к
картинке, а путь разрешается относительно каталога дескриптора (`PathDir`).

```cpp
crossrender::SpriteAtlas atlas;
// hero.png — не дескриптор: сработает запасной путь, имя региона будет "hero".
if (atlas.LoadFromFile("assets/hero.png")) {
    ENG_LOGI("demo", "формат %s, регион '%s'", crossrender::SpriteAtlas::FormatName(atlas.Format()),
             atlas.Names().front().c_str());
}
```

### Система координат региона: `uv`, `frame`, `spriteSourceSize`, `sourceSize`

У одного региона четыре разных прямоугольника, и путать их нельзя:

| Поле | Пространство | Смысл |
|---|---|---|
| `frame` | пиксели страницы, начало — левый верхний угол, Y вниз | прямоугольник, **реально занимаемый** спрайтом в картинке страницы; для повёрнутого региона хранит уже транспонированный (повёрнутый) прямоугольник |
| `uv` | нормализованный `[0,1]` внутри страницы | то же, что `frame`, поделённое на ширину и высоту страницы; это и есть текстурные координаты для отрисовки |
| `spriteSourceSize` | пиксели исходного изображения | где обрезанный прямоугольник лежал внутри необрезанного исходника: `x`,`y` — смещение обрезки, `w`,`h` — размер обрезанной (отображаемой) части |
| `sourceSize` | пиксели исходного изображения | полный размер исходника до обрезки (у обрезанных спрайтов больше `spriteSourceSize`) |
| `Size()` | пиксели | размер региона **как он отображается**: для повёрнутого региона возвращает `frame` с переставленными сторонами |
| `OriginalSize()` | пиксели | `sourceSize`, а если он не задан — `Size()` |

`pivot` — точка привязки внутри региона в долях от его отображаемого размера:
`(0,0)` — левый верхний угол, `(1,1)` — правый нижний. По умолчанию
`(0.5, 0.5)`, то есть центр; `DrawAnchored` вычитает `pivot * size` из позиции.

`trimmed` — признак того, что исходник обрезали перед упаковкой. Если
дескриптор не содержит явного булева `trimmed`, движок выводит его сам:
сравнивает `spriteSourceSize` с `frame` (в формате Sparrow — с
`frameX`/`frameY`/`frameWidth`/`frameHeight`).

```cpp
const crossrender::AtlasRegion* r = atlas.Find("hero");
if (r) {
    ENG_LOGI("demo", "в странице: %.1f,%.1f %.1fx%.1f; uv %.3f,%.3f %.3fx%.3f", r->frame.x,
             r->frame.y, r->frame.w, r->frame.h, r->uv.x, r->uv.y, r->uv.w, r->uv.h);
    ENG_LOGI("demo", "отображается %.1fx%.1f, исходник %.1fx%.1f, обрезан=%d", r->Size().x,
             r->Size().y, r->OriginalSize().x, r->OriginalSize().y, r->trimmed ? 1 : 0);
    ENG_LOGI("demo", "смещение обрезки: %.1f,%.1f", r->spriteSourceSize.x, r->spriteSourceSize.y);
}
```

`polygon` — необязательный полигон попадания, нормализованный к региону
(значения больше `1` в дескрипторе трактуются как пиксели и делятся на размер
региона). Движок хранит его и переносит из дескриптора в дескриптор, но сам
попадание по нему не считает: это данные для вызывающего кода.

```cpp
for (const crossrender::Vec2& p : atlas.RegionAt(0).polygon) {
    ENG_LOGD("demo", "вершина полигона попадания: %.3f, %.3f", p.x, p.y);
}
```

### Повёрнутые регионы

`rotated = true` означает, что упаковщик положил спрайт в страницу повёрнутым
**на 90 градусов по часовой стрелке**. Логический пиксель `(x, y)` исходника
оказывается в странице в точке `(H - 1 - y, x)`, где `H` — логическая высота
спрайта. Поэтому `frame` повёрнутого региона шире/выше, чем отображаемый
спрайт, а `Size()` переставляет стороны обратно.

Рисование разворачивает регион: `Renderer2D::Save`, перенос в центр
назначения, поворот на `-π/2` (четверть оборота против часовой стрелки, так как
положительный угол в `Renderer2D` вращает по часовой) и отрисовка квада с
переставленными шириной и высотой. Никакого отдельного пути отрисовки нет —
каждый пиксель всё равно проходит через `Renderer2D::Image`.

```cpp
const crossrender::AtlasRegion* r = atlas.Find("turret");
if (r && r->rotated) {
    // frame хранится повёрнутым, Size() — уже развёрнутый размер.
    ENG_LOGI("demo", "в странице %.1fx%.1f, на экране %.1fx%.1f", r->frame.w, r->frame.h,
             r->Size().x, r->Size().y);
    atlas.DrawAnchored(r2d, "turret", {400.0f, 300.0f});   // нарисуется upright
}
```

#### Одна задокументированная неоднозначность формата libGDX

Для повёрнутого региона в `.atlas` есть ключ `size`. Собственный упаковщик
libGDX пишет туда **неповёрнутый** размер региона, а `xy` задаёт лишь положение
прямоугольника в странице; некоторые сторонние экспортёры пишут в `size` сам
прямоугольник страницы. Движок принимает обе трактовки и различает их
эвристикой: если `size` хотя бы по одной оси больше `orig`, это прямоугольник
страницы (его нужно транспонировать), иначе — неповёрнутый размер.

Эвристика опирается на наличие `orig`. Если у повёрнутого региона есть `size`,
но нет `orig`, размер **всегда** считается неповёрнутым (конвенция libGDX), и
дескриптор стороннего экспортёра, который записал туда прямоугольник страницы
без `orig`, будет прочитан с переставленными сторонами. Это единственная
известная неоднозначность формата.

```cpp
// Обе раскладки дают одинаковый отображаемый размер 64x32:
//   libGDX:     rotate: true / xy: 0,0 / size: 64,32 / orig: 64,32
//   сторонний:  rotate: true / xy: 0,0 / size: 32,64 / orig: 64,32
crossrender::SpriteAtlas a;
a.LoadFromFile("assets/atlas/atlas_libgdx.atlas");
const crossrender::AtlasRegion* r = a.Find("turret");
if (r) ENG_LOGI("demo", "отображается %.1fx%.1f", r->Size().x, r->Size().y);
```

### Анимации и «естественный порядок»

Анимации бывают из дескриптора и построенные на лету:

* `EngineJson` хранит блок `animations` с именами кадров, длительностями и
  флагом `loop`; имена кадров разрешаются в индексы регионов через таблицу
  поиска.
* Aseprite хранит `meta.frameTags`; тег разворачивается в диапазон кадров
  `from..to`, при `direction: "reverse"` диапазон переворачивается, а
  длительности берутся из `duration` кадров и умножаются на `repeat`.
* Если после разбора анимаций не осталось, автоматически вызывается
  `BuildAnimationsFromPrefixes()` с разделителем `"_"`.

`BuildAnimationsFromPrefixes` делит имя региона по **последнему** вхождению
разделителя: `run_02` → база `run`, суффикс `02`. Группируются только имена,
у которых суффикс целиком состоит из цифр, база не пуста и в группе не меньше
двух регионов. Внутри группы кадры сортируются по числовому значению суффикса,
а при равных числах — «естественным» сравнением имён. Группа пропускается,
если анимация с таким именем уже существует. Длительность кадра берётся из
`AtlasRegion::durationMs`, а если её нет — 100 мс.

«Естественный порядок» (`NamesWithPrefix`, `AnimationNames`, `SortRegionsByName`)
означает, что цепочки цифр сравниваются как числа: `run_2` идёт раньше
`run_10`, а не наоборот, как при побайтовом сравнении. Внутри одинаковых чисел
меньшее количество ведущих нулей сортируется раньше.

```cpp
crossrender::SpriteAtlas atlas;
atlas.LoadFromFile("assets/atlas/atlas_aseprite.json");
// Если дескриптор не дал ни одной анимации, группы вида "run_0..run_9" соберутся сами.
const int created = atlas.BuildAnimationsFromPrefixes("_");
for (const std::string& n : atlas.AnimationNames()) ENG_LOGI("demo", "анимация '%s'", n.c_str());

const std::vector<std::string> run = atlas.NamesWithPrefix("run");
ENG_LOGI("demo", "создано %d, первый по естественному порядку: '%s'", created,
         run.empty() ? "-" : run.front().c_str());
```

### Честные ограничения и ловушки

1. **Нет картинки страницы — нет рисования, но `Valid()` истинно.** Регионы
   разбираются, имена и `uv` доступны, `Valid()` возвращает `true`, потому что
   регионы и страницы непусты. При этом `PageTexture(i).Valid() == false`, а
   все методы рисования молча возвращают `false`. Проверяйте и `Warnings()`, и
   `Valid()` конкретной текстуры — одного `Valid()` атласа мало.
2. **Отсутствие контекста OpenGL выглядит так же.** Если картинка страницы
   существует и даже декодируется, но загрузить её в GPU нельзя,
   записывается отдельное предупреждение `... decoded but not uploaded (no GL
   context)`. Разница только в тексте предупреждения.
3. **Без размера страницы `uv` вырождается.** Если ни дескриптор, ни картинка
   не дали размер страницы, `uv` остаётся `{0,0,0,0}`, а
   `AtlasRegion::Valid()` возвращает `false`. Размер берётся из дескриптора, из
   загруженной текстуры, из декодированной картинки, а в крайнем случае — как
   объединение `frame` всех регионов страницы.
4. **Страницы из дескриптора не настраивают сэмплер.** Они загружаются через
   `Texture::LoadFromFile`, то есть с `TextureWrap::Repeat` и без мипмапов;
   движок не добавляет отступы (padding) вокруг регионов и не расширяет края
   (bleed). При линейной фильтрации на границе региона возможен захват соседних
   текселей. Страницы, собранные `BuildFromFiles`, наоборот, создаются с
   `TextureWrap::ClampToEdge`.
5. **Обрезка (trim) не восстанавливается при рисовании.** `Draw` и
   `DrawAnchored` рисуют отображаемый (обрезанный) прямоугольник в заданном
   `dst`; `spriteSourceSize` и `sourceSize` — только данные. Кадры обрезанной
   анимации без ручной компенсации смещения будут «дрожать», а
   `DrawAnchored` центрирует обрезанный прямоугольник, а не исходный.
6. **`SaveToFile` пишет пиксели страниц только тогда, когда они есть в
   CPU-памяти.** `PagePixels` заполняется только путями `BuildFromFiles` и
   `LoadPlainImage`. У загруженного с диска атласа CPU-пикселей нет, поэтому
   `writePageImages = true` для него просто запишет дескриптор со ссылками на
   исходные картинки, без новых PNG.
7. **`RegionAt` и `Page` не бросают исключений.** При индексе вне диапазона они
   возвращают ссылку на статический пустой объект, а не ошибку.
8. **`DrawTiled` имеет предохранитель.** Если тайлов получается больше 100 000,
   функция возвращает `false`, ничего не нарисовав. Регион меньше одного
   пикселя по любой оси тоже не тайлится.
9. **`DrawNinePatch` масштабирует отступы.** Если суммарные отступы не
   помещаются в регион или в `dst`, они пропорционально уменьшаются, чтобы ни
   один из девяти квадов не вывернулся; при этом `false` возвращается только
   тогда, когда квадов не осталось вовсе.
10. **Полигон попадания не используется.** `polygon` переносится, но проверки
    попадания по нему в движке нет — это ответственность вызывающего кода.

```cpp
crossrender::SpriteAtlas atlas;
if (!atlas.LoadFromFile("assets/atlas/atlas.json")) {
    ENG_LOGE("demo", "дескриптор не разобран");
} else if (!atlas.PageTexture(0).Valid()) {
    // Valid() атласа истинно: регионы есть, но рисовать нечем.
    for (const std::string& w : atlas.Warnings()) ENG_LOGW("demo", "%s", w.c_str());
}
```

## Члены класса

### `struct AtlasRegion`

Один регион атласа: имя, положение в странице, исходные размеры, точка привязки
и необязательные метаданные упаковщика. Это чистая структура данных: все поля
публичны и заполняются либо парсером дескриптора, либо упаковщиком, либо
вызывающим кодом при ручном построении атласа.

```cpp
crossrender::AtlasRegion r;
r.name = "coin";
r.frame = crossrender::Rect{0, 0, 24, 24};
r.uv = crossrender::Rect{0.0f, 0.0f, 0.046875f, 0.046875f};   // 24 / 512
r.sourceSize = {24, 24};
r.spriteSourceSize = r.frame;
r.pivot = {0.5f, 0.5f};
r.page = 0;
ENG_ASSERT(r.Valid() && r.Size() == crossrender::Vec2{24, 24});
```

### `std::string AtlasRegion::name`

Имя региона — ключ для всех запросов (`IndexOf`, `Find`, `Has`, `Draw`). Именно
это имя используется как ключ анимаций и как база при группировке по
префиксам.

```cpp
crossrender::AtlasRegion r;
r.name = "explosion_03";
ENG_LOGI("demo", "регион '%s'", r.name.c_str());
```

### `Rect AtlasRegion::uv`

Нормализованный прямоугольник внутри картинки страницы: те же значения, что
`frame`, поделённые на ширину и высоту страницы. Начало координат — левый
верхний угол страницы, ось Y направлена вниз. Это готовые текстурные
координаты для `Renderer2D::Image`.

```cpp
const crossrender::Rect uv = atlas.RegionAt(0).uv;
r2d.Image(atlas.PageTexture(0), crossrender::Rect{16, 16, 64, 64}, uv, crossrender::Color::White);
```

### `Rect AtlasRegion::frame`

Прямоугольник, который регион реально занимает в странице, в её пикселях. Для
повёрнутого региона это уже повёрнутый (транспонированный) прямоугольник,
поэтому сравнивать `frame.w/frame.h` с отображаемым размером нельзя — для этого
есть `Size()`.

```cpp
const crossrender::AtlasRegion& r = atlas.RegionAt(0);
ENG_LOGI("demo", "регион в странице: x=%.0f y=%.0f w=%.0f h=%.0f", r.frame.x, r.frame.y,
         r.frame.w, r.frame.h);
```

### `Vec2 AtlasRegion::pivot`

Точка привязки внутри отображаемого прямоугольника региона в долях: `(0,0)` —
левый верхний угол, `(1,1)` — правый нижний. По умолчанию `(0.5, 0.5)`.
Используется только `DrawAnchored`.

```cpp
crossrender::AtlasRegion r;
r.pivot = {0.5f, 1.0f};   // «ножки» персонажа: привязка по низу
atlas.AddRegion(r);
atlas.DrawAnchored(r2d, r.name, {320.0f, 200.0f});   // позиция — точка под ногами
```

### `bool AtlasRegion::rotated`

`true`, если упаковщик сохранил регион повёрнутым на 90 градусов по часовой
стрелке. Влияет на `Size()`, на отрисовку (регион разворачивается на `-π/2`) и
на трактовку `frame`.

```cpp
const crossrender::AtlasRegion* r = atlas.Find("arrow");
if (r && r->rotated) ENG_LOGI("demo", "спрайт лежит в странице повёрнутым");
```

### `bool AtlasRegion::trimmed`

`true`, если исходное изображение обрезали перед упаковкой (прозрачные поля
отброшены). Поле информационное: рисование не восстанавливает обрезанные поля
автоматически.

```cpp
if (atlas.RegionAt(0).trimmed) {
    ENG_LOGW("demo", "спрайт обрезан: компенсируйте смещение через spriteSourceSize");
}
```

### `Vec2 AtlasRegion::sourceSize`

Полный размер исходного изображения до обрезки. У необрезанного региона равен
отображаемому размеру. `OriginalSize()` возвращает это поле, подставляя
`Size()`, если оно не задано (оба компонента неположительны).

```cpp
const crossrender::Vec2 src = atlas.RegionAt(0).sourceSize;
ENG_LOGI("demo", "исходник был %.0fx%.0f", src.x, src.y);
```

### `Rect AtlasRegion::spriteSourceSize`

Где обрезанный прямоугольник лежал внутри исходного: `x`,`y` — смещение
обрезки в пикселях исходника, `w`,`h` — размер обрезанной (то есть
отображаемой) части. Для необрезанного региона это `{0, 0, Size()}`.

```cpp
const crossrender::Rect sss = atlas.RegionAt(0).spriteSourceSize;
// Возвращаем обрезанный спрайт на его место внутри исходного прямоугольника.
const crossrender::Rect dst{100.0f + sss.x, 100.0f + sss.y, sss.w, sss.h};
atlas.DrawRegion(r2d, 0, dst, crossrender::Color::White);
```

### `int AtlasRegion::page`

Индекс страницы атласа, на которой лежит регион (с нуля). Именно по нему
`TextureFor` выбирает текстуру, а `DrawRegion` — нужную страницу.

```cpp
const crossrender::AtlasRegion& r = atlas.RegionAt(0);
const crossrender::Texture& tex = atlas.PageTexture(r.page);
ENG_LOGI("demo", "регион на странице %d (%dx%d)", r.page, tex.Width(), tex.Height());
```

### `f32 AtlasRegion::durationMs`

Длительность кадра в миллисекундах. Заполняется только теми форматами, где она
есть (Aseprite, engine JSON); иначе `0`. `BuildAnimationsFromPrefixes` и разбор
тегов Aseprite подставляют вместо нуля 100 мс.

```cpp
crossrender::AtlasAnimation anim;
anim.name = "torch";
anim.frames.push_back(atlas.IndexOf("torch_0"));
anim.durations.push_back(atlas.RegionAt(anim.frames[0]).durationMs);
atlas.AddAnimation(anim);
```

### `std::vector<Vec2> AtlasRegion::polygon`

Необязательный полигон попадания, нормализованный к прямоугольнику региона.
Пустой вектор означает «весь прямоугольник». Движок только хранит и сохраняет
его — проверки попадания по полигону нет.

```cpp
const crossrender::AtlasRegion& r = atlas.RegionAt(0);
if (!r.polygon.empty()) ENG_LOGI("demo", "у региона %zu вершин полигона", r.polygon.size());
```

### `bool AtlasRegion::Valid() const`

`true`, если у региона есть ненулевой `uv` и ненулевая ширина `frame`. Это
проверка пригодности данных для отрисовки, а не проверка существования
текстуры: у атласа с отсутствующей страницей регион может быть валидным, а
рисование всё равно не сработает.

```cpp
for (int i = 0; i < atlas.RegionCount(); ++i) {
    if (!atlas.RegionAt(i).Valid()) ENG_LOGW("demo", "регион %d без геометрии", i);
}
```

### `Vec2 AtlasRegion::Size() const`

Отображаемый размер региона в пикселях: для повёрнутого региона возвращает
`frame` с переставленными сторонами, для обычного — сам `frame`.

```cpp
const crossrender::Vec2 s = atlas.RegionAt(0).Size();
r2d.FillRect(crossrender::Rect{0, 0, s.x, s.y}, crossrender::Color{1, 0, 0, 0.25f});   // отладочная рамка
```

### `Vec2 AtlasRegion::OriginalSize() const`

Размер исходного изображения: `sourceSize`, если он задан (обе компоненты
положительны), иначе `Size()`.

```cpp
// Соотношение сторон исходника — удобно для расчёта коллайдера.
const crossrender::Vec2 o = atlas.RegionAt(0).OriginalSize();
ENG_LOGI("demo", "aspect = %.3f", o.x / o.y);
```

### `enum class AtlasFormat : u8`

Формат дескриптора, определённый по содержимому. Возвращается методом
`Format()` и свободными `DetectFormat` / `FormatName`.

| Значение | Смысл |
|---|---|
| `AtlasFormat::Unknown` | не распознано; `LoadFromFile` попробует трактовать файл как картинку |
| `AtlasFormat::EngineJson` | собственный формат движка (`meta.app == "CrossRender"` или есть `pages`/`animations`) |
| `AtlasFormat::TexturePackerHash` | TexturePacker, `frames` — объект |
| `AtlasFormat::TexturePackerArray` | TexturePacker, `frames` — массив |
| `AtlasFormat::Aseprite` | JSON Aseprite, включая `frameTags` и длительности кадров |
| `AtlasFormat::SparrowXml` | XML Sparrow/Starling |
| `AtlasFormat::LibGdx` | текстовый `.atlas` libGDX |
| `AtlasFormat::PlainImage` | обычная картинка = один регион; этот же формат выставляет `BuildFromFiles` |
| `AtlasFormat::Count` | служебное значение — количество форматов, не реальный формат |

```cpp
crossrender::SpriteAtlas atlas;
atlas.LoadFromFile("assets/atlas/atlas_sparrow.xml");
if (atlas.Format() == crossrender::AtlasFormat::SparrowXml) {
    ENG_LOGI("demo", "XML Sparrow: %s", crossrender::SpriteAtlas::FormatName(atlas.Format()));
}
// BuildFromFiles помечает атлас как PlainImage, хотя SaveToFile пишет engine JSON.
crossrender::SpriteAtlas baked;
baked.BuildFromFiles({"assets/coin.png", "assets/gem.png"});
ENG_ASSERT(baked.Format() == crossrender::AtlasFormat::PlainImage);
```

### `struct AtlasAnimation`

Именованная анимация: список индексов регионов плюс параллельный список
длительностей в миллисекундах. Индексы указывают в `SpriteAtlas::Regions()`,
длительностей может быть меньше, чем кадров — недостающие считаются равными
100 мс.

```cpp
crossrender::AtlasAnimation run;
run.name = "run";
run.frames = {atlas.IndexOf("run_0"), atlas.IndexOf("run_1"), atlas.IndexOf("run_2")};
run.durations = {120.0f, 90.0f, 90.0f};
run.loop = true;
atlas.AddAnimation(run);
```

### `std::string AtlasAnimation::name`

Имя анимации — ключ для `FindAnimation`, `AnimationFrameAt`, `DrawAnimation` и
`DrawAnimationFrame`. `AddAnimation` заменяет анимацию с тем же именем.

```cpp
crossrender::AtlasAnimation walk;
walk.name = "walk";
if (!atlas.FindAnimation(walk.name)) atlas.AddAnimation(walk);
```

### `std::vector<int> AtlasAnimation::frames`

Индексы регионов в порядке проигрывания. Пустой список означает анимацию без
кадров: `FrameAt` вернёт `-1`, а рисование — `false`.

```cpp
crossrender::AtlasAnimation blink;
blink.name = "blink";
for (const std::string& n : atlas.NamesWithPrefix("eye_")) blink.frames.push_back(atlas.IndexOf(n));
ENG_LOGI("demo", "кадров: %zu", blink.frames.size());
```

### `std::vector<f32> AtlasAnimation::durations`

Длительности кадров в миллисекундах, параллельные `frames`. Если элементов
меньше, чем кадров, недостающие берутся как 100 мс; отрицательные значения
зажимаются нулём.

```cpp
crossrender::AtlasAnimation idle;
idle.name = "idle";
idle.frames = {0, 1};
idle.durations = {500.0f, 250.0f};
ENG_LOGI("demo", "всего %.0f мс", idle.TotalDuration());
```

### `bool AtlasAnimation::loop`

`true` — время зацикливается по общей длительности, `false` — после последнего
кадра анимация остаётся на нём. Значение по умолчанию — `true`; парсеры
Aseprite и `BuildAnimationsFromPrefixes` тоже ставят `true`.

```cpp
crossrender::AtlasAnimation hit;
hit.name = "hit";
hit.frames = {4, 5, 6};
hit.loop = false;   // один проход и стоп на последнем кадре
atlas.AddAnimation(hit);
```

### `f32 AtlasAnimation::TotalDuration() const`

Суммарная длительность всех кадров в миллисекундах. Отсутствующие длительности
считаются равными 100 мс, отрицательные — нулю. Для пустой анимации — `0`.

```cpp
const crossrender::AtlasAnimation* a = atlas.FindAnimation("run");
if (a) ENG_LOGI("demo", "цикл длится %.0f мс", a->TotalDuration());
```

### `int AtlasAnimation::FrameAt(f32 timeSeconds) const`

Номер кадра, который нужно показать в момент `timeSeconds` (обратите внимание:
аргумент в **секундах**, длительности внутри — в миллисекундах). При `loop`
время берётся по модулю общей длительности, при `loop == false` после конца
возвращается последний кадр; отрицательное время зажимается нулём. Для пустой
анимации возвращается `-1`.

```cpp
const crossrender::AtlasAnimation* a = atlas.FindAnimation("run");
if (a) {
    const int frame = a->FrameAt(1.25f);   // 1.25 секунды от старта
    ENG_LOGI("demo", "кадр %d из %zu", frame, a->frames.size());
}
```

### `struct AtlasPageDesc`

Описание одной страницы атласа: как картинка названа в дескрипторе, какой путь
реально загружен, известные размеры и сама текстура.

```cpp
for (int i = 0; i < atlas.PageCount(); ++i) {
    const crossrender::AtlasPageDesc& p = atlas.Page(i);
    ENG_LOGI("demo", "страница %d: '%s' -> '%s' %dx%d", i, p.image.c_str(), p.resolved.c_str(),
             p.width, p.height);
}
```

### `std::string AtlasPageDesc::image`

Имя картинки в том виде, в каком оно записано в дескрипторе (обычно
относительный путь). Используется для разрешения пути и при `SaveToFile`.

```cpp
crossrender::AtlasPageDesc page = atlas.Page(0);
ENG_LOGI("demo", "в дескрипторе записано '%s'", page.image.c_str());
```

### `std::string AtlasPageDesc::resolved`

Абсолютный (или разрешённый относительно каталога дескриптора) путь, который
фактически пытались загрузить. Полезен, чтобы понять, куда смотрел загрузчик,
когда картинка не нашлась.

```cpp
if (!atlas.PageTexture(0).Valid()) {
    ENG_LOGE("demo", "не загрузилась страница '%s'", atlas.Page(0).resolved.c_str());
}
```

### `int AtlasPageDesc::width`

Ширина страницы в пикселях. Заполняется из дескриптора, иначе из загруженной
текстуры, иначе из декодированной картинки, иначе из объединения `frame`
регионов этой страницы.

```cpp
const int w = atlas.Page(0).width;
ENG_LOGI("demo", "ширина страницы: %d px", w);
```

### `int AtlasPageDesc::height`

Высота страницы в пикселях; определяется теми же источниками, что и `width`.
На пару `width`/`height` опирается расчёт `uv`, поэтому она важна даже тогда,
когда текстура не загрузилась.

```cpp
ENG_LOGI("demo", "страница %dx%d", atlas.Page(0).width, atlas.Page(0).height);
```

### `Texture AtlasPageDesc::texture`

Сама текстура страницы. Если картинка не найдена, не читается или не может быть
загружена в GPU, `texture.Valid()` возвращает `false`, и все методы рисования
атласа для регионов этой страницы вернут `false`. Поле публично, поэтому
страницу можно подменить вручную.

```cpp
if (!atlas.PageTexture(0).Valid()) {
    ENG_LOGW("demo", "подменяю отсутствующую страницу заглушкой");
    // Своя текстура должна быть жива, пока жив атлас: поле хранит значение Texture.
}
```

### `struct SpriteAtlas::PackOptions`

Параметры упаковщика для `BuildFromFiles`: предельный размер страницы, отступ
между регионами, степень двойки, поворот и обрезка. Значения по умолчанию
рассчитаны на обычный атлас интерфейса (2048 px, отступ 2, страницы-степени
двойки, без поворота и обрезки).

```cpp
crossrender::SpriteAtlas atlas;
crossrender::SpriteAtlas::PackOptions opts;
opts.maxSize = 1024;        // сторона страницы не больше 1024
opts.padding = 4;           // 4 px между регионами
opts.powerOfTwo = true;     // сторона — степень двойки
opts.allowRotate = true;    // разрешить поворот на 90°
opts.trim = true;           // обрезать прозрачные поля
opts.background = crossrender::Color{0, 0, 0, 0};
atlas.BuildFromFiles({"assets/coin.png", "assets/gem.png"}, opts);
```

### `int PackOptions::maxSize`

Предельная сторона страницы в пикселях. При `powerOfTwo = true` значение
округляется **вниз** до ближайшей степени двойки; снизу ограничено восемью
пикселями. Картинки, которые не помещаются в этот предел ни в одном
положении, пропускаются с предупреждением.

```cpp
crossrender::SpriteAtlas::PackOptions mobile;
mobile.maxSize = 512;   // жёсткий лимит для слабых GPU
if (mobile.maxSize < 512) ENG_LOGW("demo", "слишком маленькая страница");
```

### `int PackOptions::padding`

Отступ между регионами в пикселях (зажимается снизу нулём). Увеличивайте его,
если спрайты рисуются с линейной фильтрацией и мипмапами: иначе на границах
регионов возможно «протекание» соседних пикселей.

```cpp
crossrender::SpriteAtlas::PackOptions opts;
opts.padding = 8;   // с запасом для мипмапов
ENG_LOGI("demo", "отступ между спрайтами: %d px", opts.padding);
```

### `bool PackOptions::powerOfTwo`

Требовать ли сторону страницы, равную степени двойки. Включено по умолчанию:
такие текстуры совместимы со старыми драйверами и лучше подходят для мипмапов.

```cpp
crossrender::SpriteAtlas::PackOptions tight;
tight.powerOfTwo = false;   // плотная упаковка без выравнивания
tight.maxSize = 1000;
```

### `bool PackOptions::allowRotate`

Разрешить упаковщику поворачивать регионы на 90 градусов, если это экономит
место. Поворачиваются только непрямоугольные по пропорциям регионы, которым иначе
не хватает места; результат помечается `AtlasRegion::rotated`, и рисование
разворачивает спрайт автоматически.

```cpp
crossrender::SpriteAtlas::PackOptions tall;
tall.allowRotate = true;    // длинные спрайты лягут боком и сэкономят страницу
ENG_LOGI("demo", "поворот разрешён: %d", tall.allowRotate ? 1 : 0);
```

### `bool PackOptions::trim`

Обрезать ли полностью прозрачные поля вокруг картинки перед упаковкой. Границы
непрозрачных пикселей считаются по альфа-каналу; полностью прозрачная картинка
сохраняется целиком. Обрезанному региону выставляются `trimmed`,
`spriteSourceSize` и `sourceSize`.

```cpp
crossrender::SpriteAtlas::PackOptions opts;
opts.trim = true;
ENG_LOGI("demo", "обрезка прозрачных полей: %d", opts.trim ? 1 : 0);
```

### `Color PackOptions::background`

Цвет заливки страницы перед компоновкой регионов (по умолчанию полностью
прозрачный чёрный). Компоненты зажимаются в `[0,1]` и переводятся в байты при
создании CPU-страниц.

```cpp
crossrender::SpriteAtlas::PackOptions opts;
opts.background = crossrender::Color{0, 0, 0, 0};
opts.trim = false;   // с непрозрачным фоном обрезка потеряла бы смысл
```

### `struct SpriteAtlas::Stats`

Статистика атласа. Пересчитывается при каждом изменении (загрузка, `AddRegion`,
`SetPageImage`, упаковка, сортировка) и доступна через `GetStats()`. Удобна для
тестов и для отчётов об эффективности упаковки.

```cpp
const crossrender::SpriteAtlas::Stats& s = atlas.GetStats();
ENG_LOGI("demo", "регионов %d на %d страницах, заполнение %.1f%%", s.regions, s.pages,
         s.occupancy * 100.0f);
```

### `int Stats::regions`

Число регионов в атласе, то же, что `RegionCount()`.

```cpp
ENG_LOGI("demo", "регионов: %d", atlas.GetStats().regions);
```

### `int Stats::pages`

Число страниц, то же, что `PageCount()`.

```cpp
if (atlas.GetStats().pages > 1) ENG_LOGI("demo", "атлас многостраничный");
```

### `int Stats::totalPixels`

Суммарная площадь регионов в пикселях, посчитанная по **отображаемому** размеру
(`Size()`, то есть с учётом поворота).

```cpp
const int used = atlas.GetStats().totalPixels;
ENG_LOGI("demo", "полезная площадь: %d px", used);
```

### `int Stats::pagePixels`

Суммарная площадь всех страниц (`width * height`). Это знаменатель для
`occupancy`.

```cpp
ENG_LOGI("demo", "площадь страниц: %d px", atlas.GetStats().pagePixels);
```

### `f32 Stats::occupancy`

Заполнение атласа в `[0,1]`: `totalPixels / pagePixels`. Ноль, если площадь
страниц неизвестна. Значение ниже примерно `0.5` обычно означает, что атлас
можно перепаковать плотнее.

```cpp
if (atlas.GetStats().occupancy < 0.4f) {
    ENG_LOGW("demo", "атлас заполнен на %.0f%% — есть смысл перепаковать",
             atlas.GetStats().occupancy * 100.0f);
}
```

### `SpriteAtlas()`

Создаёт пустой атлас: ни регионов, ни страниц, `Valid() == false`, формат
`AtlasFormat::Unknown`. Обращений к GPU нет, поэтому конструктор безопасен до
создания контекста OpenGL.

```cpp
crossrender::SpriteAtlas atlas;
ENG_ASSERT(!atlas.Valid() && atlas.RegionCount() == 0 && atlas.PageCount() == 0);
```

### `~SpriteAtlas()`

Освобождает регионы, страницы и их текстуры. Деструктор не бросаёт исключений;
текстуры страниц уничтожаются вместе с атласом (каждая страница владеет своей
`Texture`).

```cpp
{
    crossrender::SpriteAtlas local;
    local.LoadFromFile("assets/atlas/atlas.json");
}   // здесь страницы и их текстуры освобождаются
```

### `SpriteAtlas(const SpriteAtlas&) = delete`, `SpriteAtlas& operator=(const SpriteAtlas&) = delete`

Копирование запрещено: атлас владеет текстурами страниц, и копия привела бы к
двойному владению GPU-ресурсом. Передавайте атлас по ссылке или указателю.

```cpp
void DrawHudFrom(crossrender::SpriteAtlas& atlas, crossrender::Renderer2D& r2d) {
    atlas.Draw(r2d, "hud_panel", crossrender::Rect{0, 0, 320, 48});   // по ссылке, без копии
}
```

### `SpriteAtlas(SpriteAtlas&&) noexcept`

Перемещающий конструктор: переносит регионы, страницы, анимации, предупреждения
и CPU-страницы, оставляя источник пустым (формат `Unknown`, пустая статистика).
Нужен для хранения атласов в `std::vector` и `std::unordered_map`.

```cpp
std::vector<crossrender::SpriteAtlas> atlases;
atlases.push_back(crossrender::SpriteAtlas{});   // перемещение
ENG_ASSERT(!atlases[0].Valid());
```

### `SpriteAtlas& operator=(SpriteAtlas&&) noexcept`

Перемещающее присваивание. Самоприсваивание (`a = std::move(a)`) безопасно и
ничего не делает; предыдущее содержимое приёмника освобождается.

```cpp
crossrender::SpriteAtlas a, b;
b = std::move(a);   // a снова пуст, b владеет данными
ENG_ASSERT(!a.Valid());
```

### `bool LoadFromFile(const std::string& descriptorPath, bool srgb = false)`

Главный способ загрузки: читает дескриптор, определяет формат по содержимому,
разбирает регионы, анимации и страницы, а затем загружает картинки страниц
относительно каталога дескриптора. Предыдущее содержимое освобождается.

Возвращает `false` только тогда, когда дескриптор не читается, не распознан
(и не является картинкой) или не содержит ни одного региона. **Отсутствие
картинки страницы ошибкой не считается** — регионы загружаются, а в
`Warnings()` появляется сообщение. `srgb = true` создаёт страницы в формате
`SRGBA8`.

```cpp
crossrender::SpriteAtlas atlas;
if (!atlas.LoadFromFile("assets/atlas/atlas.json", /*srgb=*/true)) {
    ENG_LOGE("demo", "не удалось разобрать дескриптор");
} else {
    ENG_LOGI("demo", "формат %s, регионов %d", crossrender::SpriteAtlas::FormatName(atlas.Format()),
             atlas.RegionCount());
}
```

### `bool LoadFromMemory(const std::string& descriptor, const std::string& baseDir, const std::string& debugName = "<memory>", bool srgb = false)`

То же, что `LoadFromFile`, но дескриптор уже лежит в памяти (например, пришёл из
упакованного архива или из сети). Относительные пути к картинкам страниц
разрешаются относительно `baseDir`; `debugName` попадает в `SourcePath()` и в
тексты предупреждений. Если формат распознан как `PlainImage`, содержимое
строки трактуется как путь к картинке и тоже разрешается относительно `baseDir`.

```cpp
const crossrender::ByteBuffer bytes = crossrender::ReadBinaryFile("assets/atlas/atlas.json");
crossrender::SpriteAtlas atlas;
if (!bytes.empty()) {
    atlas.LoadFromMemory(std::string(bytes.begin(), bytes.end()), "assets/atlas", "atlas.json");
    ENG_LOGI("demo", "источник: %s", atlas.SourcePath().c_str());
}
```

### `bool LoadPlainImage(const std::string& imagePath, bool srgb = false)`

Открывает обычную картинку как атлас из одного региона. Имя региона — имя файла
без каталога и без известного расширения изображения (`StemOf`); если оно
получилось пустым, берётся `"image"`. Регион занимает всю страницу
(`frame = {0,0,w,h}`, `uv = {0,0,1,1}`), `sourceSize` и `spriteSourceSize` равны
размеру картинки, формат выставляется в `AtlasFormat::PlainImage`. CPU-пиксели
сохраняются, поэтому работают `PagePixels` и `SavePageImage`. Возвращает
`false`, если картинка не декодируется.

```cpp
crossrender::SpriteAtlas atlas;
if (atlas.LoadPlainImage("assets/hero.png", /*srgb=*/false)) {
    const crossrender::AtlasRegion& r = atlas.RegionAt(0);
    ENG_LOGI("demo", "регион '%s' %dx%d", r.name.c_str(), static_cast<int>(r.frame.w),
             static_cast<int>(r.frame.h));
}
```

### `void Clear()`

Возвращает атлас в состояние сразу после конструктора: очищает регионы,
страницы, анимации, предупреждения, таблицу поиска, исходный путь и статистику,
формат снова `Unknown`. Вызывается в начале каждой загрузки, поэтому
`LoadFromFile` поверх существующего атласа полностью его заменяет.

```cpp
atlas.Clear();
ENG_ASSERT(!atlas.Valid() && atlas.Warnings().empty());
atlas.LoadFromFile("assets/atlas/other.json");   // загрузка тоже начинается с Clear
```

### `bool SaveToFile(const std::string& path, bool writePageImages = false) const`

Записывает дескриптор в формате engine JSON (`meta.app == "CrossRender"`), так
что файл читается обратно обычным `LoadFromFile`. Кадры пишутся объектом по
именам, анимации — массивом с именами кадров и длительностями. При
`writePageImages = true` дополнительно сохраняются PNG страниц — но только если
для них есть CPU-пиксели (`PagePixels`), то есть для атласов, собранных
`BuildFromFiles` или `LoadPlainImage`; для загруженного с диска атласа будут
записаны только ссылки на исходные картинки. Возвращает `false` при пустом пути
или ошибке записи.

```cpp
crossrender::SpriteAtlas baked;
baked.BuildFromFiles({"assets/coin.png", "assets/gem.png"});
if (!baked.SaveToFile("out/atlas.json", /*writePageImages=*/true)) {
    ENG_LOGE("demo", "атлас не сохранён");
}
```

### `static AtlasFormat DetectFormat(const std::string& descriptor)`

Определяет формат по содержимому строки: ведущий символ, разбор JSON и наличие
характерных членов, для libGDX — обязательные ключи `xy:`/`size:`. Расширение
файла не учитывается. Пустая строка и всё нераспознанное дают
`AtlasFormat::Unknown`.

```cpp
const crossrender::AtlasFormat fmt = crossrender::SpriteAtlas::DetectFormat("sprites.png");
// Одна строка с расширением картинки — это PlainImage, а не Unknown.
ENG_ASSERT(fmt == crossrender::AtlasFormat::PlainImage);
```

### `static const char* FormatName(AtlasFormat f)`

Читаемое имя формата для логов и отладочных панелей: `"EngineJson"`,
`"TexturePackerHash"`, `"Aseprite"`, `"SparrowXml"`, `"LibGdx"`,
`"PlainImage"`. Возвращает указатель на статическую строку, владеть ею не
нужно. Для `Unknown` и `Count` возвращает `"Unknown"`.

```cpp
ENG_LOGI("demo", "дескриптор распознан как %s", crossrender::SpriteAtlas::FormatName(atlas.Format()));
```

### `bool BuildFromFiles(const std::vector<std::string>& imagePaths, const PackOptions& opts)`

Собирает новый атлас из отдельных картинок: декодирует их на CPU, при
необходимости обрезает прозрачные поля, раскладывает полками (shelf/skyline),
компонует страницы в памяти и загружает их в GPU. Регионы называются по именам
файлов; дубликаты переименовываются с суффиксом `_N` и предупреждением.
Картинки, не помещающиеся в `maxSize`, пропускаются. Формат атласа выставляется
в `PlainImage`.

Без контекста OpenGL метод всё равно возвращает `true` с валидными регионами и
CPU-пикселями (`PagePixels` работает), но текстуры страниц остаются
невалидными, и на каждую страницу пишется предупреждение.

```cpp
crossrender::SpriteAtlas atlas;
crossrender::SpriteAtlas::PackOptions opts;
opts.maxSize = 2048;
opts.trim = true;
if (!atlas.BuildFromFiles({"assets/a.png", "assets/b.png"}, opts)) {
    ENG_LOGE("demo", "ни одной картинки не декодировано");
}
ENG_LOGI("demo", "%d регионов на %d страницах", atlas.RegionCount(), atlas.PageCount());
```

### `bool BuildFromFiles(const std::vector<std::string>& imagePaths)`

Удобная перегрузка с параметрами по умолчанию (`PackOptions{}`): страница до
2048 px, отступ 2, степень двойки, без поворота и обрезки. Отдельного поведения
у неё нет — она просто передаёт настройки по умолчанию.

```cpp
crossrender::SpriteAtlas atlas;
atlas.BuildFromFiles({"assets/tile_grass.png", "assets/tile_stone.png"});
ENG_LOGI("demo", "заполнение %.0f%%", atlas.GetStats().occupancy * 100.0f);
```

### `void AddRegion(const AtlasRegion& region)`

Добавляет регион вручную — путь для процедурно сгенерированных ассетов и
тестов. Регион копируется как есть, таблица поиска обновляется (одноимённый
регион перекрывает предыдущий в `IndexOf`/`Find`), статистика пересчитывается.
Страницу регион не создаёт: её нужно объявить через `SetPageImage` или
построить атлас другим способом.

```cpp
crossrender::AtlasRegion r;
r.name = "solid";
r.frame = crossrender::Rect{0, 0, 4, 4};
r.uv = crossrender::Rect{0, 0, 4.0f / 64.0f, 4.0f / 64.0f};
r.sourceSize = {4, 4};
r.spriteSourceSize = r.frame;
atlas.AddRegion(r);
ENG_ASSERT(atlas.Has("solid"));
```

### `bool SetPageImage(int page, const std::string& imagePath, bool srgb = false)`

Объявляет или заменяет картинку страницы без дескриптора: создаёт страницы до
нужной включительно, загружает текстуру и **пересчитывает `uv` всех регионов
этой страницы** по фактическим размерам картинки. Возвращает `false` при
отрицательном индексе или неудачной загрузке (с предупреждением). Если текстура
не загрузилась, регионы остаются с прежними `uv`.

```cpp
crossrender::SpriteAtlas atlas;
atlas.AddRegion(regionOnPage0);
if (!atlas.SetPageImage(0, "assets/page_atlas.png")) {
    ENG_LOGW("demo", "страница не загрузилась: %s", atlas.Page(0).resolved.c_str());
}
```

### `void SortRegionsByName()`

Сортирует регионы по имени в «естественном» порядке (цепочки цифр сравниваются
как числа, поэтому `run_2` идёт раньше `run_10`). После сортировки заново
строится таблица поиска, а индексы регионов меняются — сохранённые наружу
индексы и `AtlasAnimation::frames` станут неверными, их нужно пересобрать.

```cpp
atlas.SortRegionsByName();
for (const std::string& n : atlas.Names()) ENG_LOGD("demo", "%s", n.c_str());
```

### `bool Valid() const`

`true`, если в атласе есть хотя бы один регион и хотя бы одна страница. Это
проверка **данных**, а не готовности к рисованию: у атласа с отсутствующими
картинками страниц `Valid()` истинно (см. ограничения выше). Для проверки
готовности смотрите `Warnings()` и `PageTexture(i).Valid()`.

```cpp
if (!atlas.Valid()) {
    ENG_LOGW("demo", "атлас пуст или не загружен");
} else if (!atlas.PageTexture(0).Valid()) {
    ENG_LOGW("demo", "данные есть, но рисовать нечем");
}
```

### `int RegionCount() const`

Число регионов в атласе. Индексы регионов лежат в диапазоне
`[0, RegionCount())` и меняются после `SortRegionsByName`.

```cpp
for (int i = 0; i < atlas.RegionCount(); ++i) ENG_LOGD("demo", "%s", atlas.RegionAt(i).name.c_str());
```

### `int PageCount() const`

Число страниц (картинок) атласа. Больше одной страницы бывает у атласов,
которые не поместились в `PackOptions::maxSize`, и у дескрипторов с массивом
`pages`.

```cpp
ENG_LOGI("demo", "атлас: %d регионов, %d страниц", atlas.RegionCount(), atlas.PageCount());
```

### `const std::vector<AtlasRegion>& Regions() const`

Прямой доступ ко всем регионам в порядке хранения (для загруженного атласа —
порядок дескриптора, для упакованного — порядок размещения, пока не вызван
`SortRegionsByName`). Ссылка живёт, пока атлас не изменён.

```cpp
const std::vector<crossrender::AtlasRegion>& regions = atlas.Regions();
for (crossrender::usize i = 0; i < regions.size(); ++i) {
    if (regions[i].trimmed) ENG_LOGD("demo", "'%s' обрезан", regions[i].name.c_str());
}
```

### `const AtlasRegion& RegionAt(int index) const`

Регион по индексу. При индексе вне диапазона возвращает ссылку на статический
пустой регион (все поля по умолчанию), а не бросает исключение и не возвращает
`nullptr`, — проверяйте `Valid()` или сам индекс.

```cpp
const crossrender::AtlasRegion& r = atlas.RegionAt(3);
if (r.Valid()) ENG_LOGI("demo", "регион '%s'", r.name.c_str());
else ENG_LOGW("demo", "индекс вне диапазона или регион без геометрии");
```

### `int IndexOf(const std::string& name) const`

Индекс региона по имени или `-1`, если имени нет. Поиск идёт по хеш-таблице, то
есть за постоянное время; при повторяющихся именах возвращается последний
добавленный регион.

```cpp
const int i = atlas.IndexOf("hero_idle_0");
if (i >= 0) ENG_LOGI("demo", "индекс региона: %d", i);
```

### `bool Has(const std::string& name) const`

`true`, если регион с таким именем есть. Короткая форма `IndexOf(name) >= 0`.

```cpp
if (atlas.Has("frame")) {
    atlas.Draw(r2d, "frame", crossrender::Rect{8, 8, 240, 160});
} else {
    ENG_LOGW("demo", "в атласе нет региона 'frame'");
}
```

### `const AtlasRegion* Find(const std::string& name) const`

Указатель на регион или `nullptr`. Удобно, когда нужно прочитать `pivot`,
`Size()` или `spriteSourceSize` перед отрисовкой.

```cpp
if (const crossrender::AtlasRegion* r = atlas.Find("panel")) {
    ENG_LOGI("demo", "pivot панели: %.2f, %.2f", r->pivot.x, r->pivot.y);
}
```

### `std::vector<std::string> Names() const`

Копия списка имён в порядке хранения регионов. Для упакованного атласа это
порядок укладки (сначала самые высокие), поэтому для стабильного вида
пользуйтесь `SortRegionsByName` или `NamesWithPrefix`.

```cpp
const std::vector<std::string> all = atlas.Names();
ENG_LOGI("demo", "первое имя: %s", all.empty() ? "-" : all.front().c_str());
```

### `std::vector<std::string> NamesWithPrefix(const std::string& prefix) const`

Имена, начинающиеся с `prefix`, в «естественном» порядке (цепочки цифр
сравниваются как числа: `run_2` раньше `run_10`). Пустой `prefix` возвращает все
имена, тоже отсортированные. Удобно для сборки кадров анимации.

```cpp
const std::vector<std::string> frames = atlas.NamesWithPrefix("run_");
for (const std::string& n : frames) ENG_LOGD("demo", "кадр бега: %s", n.c_str());
```

### `const std::vector<std::string>& Warnings() const`

Предупреждения, накопленные при последней загрузке или упаковке: отсутствующие
картинки, нераспознанные ключи, пропущенные файлы, переименованные дубликаты.
Каждое предупреждение одновременно уходит в лог с тегом `atlas`. Вектор
очищается в `Clear()` и в начале очередной загрузки.

```cpp
for (const std::string& w : atlas.Warnings()) ENG_LOGW("demo", "атлас: %s", w.c_str());
ENG_LOGI("demo", "предупреждений: %zu", atlas.Warnings().size());
```

### `std::string SourcePath() const`

Путь к дескриптору (или `debugName` из `LoadFromMemory`, а для
`LoadPlainImage` — путь к картинке). Пустая строка у собранного
`BuildFromFiles` атласа: у него нет исходного файла.

```cpp
ENG_LOGI("demo", "атлас загружен из %s", atlas.SourcePath().c_str());
```

### `AtlasFormat Format() const`

Формат, определённый при последней загрузке. После `Clear()` — `Unknown`,
после `BuildFromFiles` — `PlainImage` (даже несмотря на то, что `SaveToFile`
пишет engine JSON).

```cpp
switch (atlas.Format()) {
    case crossrender::AtlasFormat::Aseprite:
        ENG_LOGI("demo", "анимации придут из frameTags");
        break;
    default:
        ENG_LOGI("demo", "формат: %s", crossrender::SpriteAtlas::FormatName(atlas.Format()));
        break;
}
```

### `const AtlasPageDesc& Page(int index) const`

Описание страницы по индексу. При индексе вне диапазона возвращает ссылку на
статическую пустую страницу, поэтому `Page(0).width` безопасен даже у пустого
атласа — просто вернёт `0`.

```cpp
const crossrender::AtlasPageDesc& p = atlas.Page(0);
ENG_LOGI("demo", "'%s' -> '%s' (%dx%d)", p.image.c_str(), p.resolved.c_str(), p.width, p.height);
```

### `const Texture& PageTexture(int index) const`

Текстура страницы; сокращение для `Page(index).texture`. У отсутствующей или
незагруженной страницы вернётся невалидная текстура, поэтому перед ручной
привязкой в шейдер проверяйте `Valid()`.

```cpp
const crossrender::Texture& page0 = atlas.PageTexture(0);
if (page0.Valid()) ENG_LOGI("demo", "страница %dx%d", page0.Width(), page0.Height());
```

### `const Texture& TextureFor(const AtlasRegion& region) const`

Текстура той страницы, на которой лежит регион (по полю `region.page`). Именно
её используют все методы рисования; индекс страницы зажимается при загрузке,
поэтому выход за диапазон здесь невозможен для регионов из атласа, но возможен
для региона, добавленного вручную с чужим `page`.

```cpp
const crossrender::AtlasRegion& r = atlas.RegionAt(0);
const crossrender::Texture& tex = atlas.TextureFor(r);
r2d.Image(tex, crossrender::Rect{0, 0, 64, 64}, r.uv, crossrender::Color::White);
```

### `bool Draw(Renderer2D& r2d, const std::string& name, const Rect& dst, const Color& tint = Color::White) const`

Рисует регион, растягивая его на прямоугольник `dst` (обрезка и поворот
учитываются, `pivot` — нет). Возвращает `false`, если имени нет, если текстура
страницы невалидна (например, картинка отсутствует) или если `dst` вырожден.

```cpp
if (!atlas.Draw(r2d, "hero", crossrender::Rect{100, 100, 128, 128}, crossrender::Color::White)) {
    ENG_LOGW("demo", "спрайт 'hero' не нарисован");
}
```

### `bool DrawAnchored(Renderer2D& r2d, const std::string& name, Vec2 position, const Color& tint = Color::White, f32 scale = 1.0f) const`

Рисует регион в натуральную величину, умноженную на `scale`, позиционируя его
точкой привязки `pivot`: `position` — это место, куда попадёт точка `pivot`
региона. При `pivot = {0.5, 0.5}` регион центрируется на позиции, при
`{0.5, 1}` — «стоит» на ней.

```cpp
// Герой стоит на земле: привязка по центру низа.
if (const crossrender::AtlasRegion* found = atlas.Find("hero")) {
    crossrender::AtlasRegion hero = *found;
    hero.pivot = {0.5f, 1.0f};
    atlas.AddRegion(hero);
    atlas.DrawAnchored(r2d, "hero", {320.0f, 400.0f}, crossrender::Color::White, 1.5f);
}
```

### `bool DrawRegion(Renderer2D& r2d, int regionIndex, const Rect& dst, const Color& tint, bool flipX = false, bool flipY = false) const`

Рисует уже найденный регион по индексу с необязательным зеркалированием по осям.
`flipX`/`flipY` заданы в **логическом** пространстве региона: для повёрнутого
региона движок сам переставляет оси при выборке UV, поэтому спрайт отражается
так, как ожидает вызывающий код. Возвращает `false` при неверном индексе,
невалидной текстуре или вырожденном `dst`.

```cpp
const int i = atlas.IndexOf("hero");
atlas.DrawRegion(r2d, i, crossrender::Rect{0, 0, 64, 64}, crossrender::Color::White, /*flipX=*/true, false);
```

### `bool DrawNinePatch(Renderer2D& r2d, const std::string& name, const Rect& dst, f32 border, const Color& tint = Color::White) const`

Рисует регион как nine-patch (девятипатчевую растяжку) с одинаковым отступом
`border` со всех четырёх сторон: углы не масштабируются, края тянутся в одном
направлении, центр — в двух. Удобно для панелей и кнопок с неизменной толщиной
рамки. Короткая форма перегрузки с четырьмя отступами.

```cpp
// Рамка панели: 12 px по краям остаются неизменными при любом размере.
atlas.DrawNinePatch(r2d, "panel", crossrender::Rect{40, 40, 400, 240}, 12.0f, crossrender::Color::White);
```

### `bool DrawNinePatch(Renderer2D& r2d, const std::string& name, const Rect& dst, f32 left, f32 top, f32 right, f32 bottom, const Color& tint = Color::White) const`

Полная форма nine-patch с независимыми отступами по сторонам. Отступы
зажимаются размерами региона, а если суммарно не помещаются в регион или в
`dst` — пропорционально уменьшаются, чтобы ни один из девяти квадов не
вывернулся. Возвращает `false`, если регион не найден, текстура невалидна или
не осталось ни одного квада.

```cpp
// Асимметричная рамка: сверху место под заголовок, снизу — под кнопки.
atlas.DrawNinePatch(r2d, "dialog", panel, 16.0f, 48.0f, 16.0f, 16.0f, crossrender::Color::White);
```

### `bool DrawTiled(Renderer2D& r2d, const std::string& name, const Rect& dst, const Color& tint = Color::White) const`

Заполняет `dst` тайлами одного региона: регион повторяется слева направо и
сверху вниз, крайние тайлы обрезаются по границе `dst` (последний столбец и
строка рисуются частично). Регион меньше одного пикселя по любой оси не тайлится,
а если тайлов получилось больше 100 000, функция возвращает `false`, ничего не
нарисовав.

```cpp
// Клетчатый пол из одного квадратного тайла.
atlas.DrawTiled(r2d, "tile_grass", crossrender::Rect{0, 0, 1280, 720}, crossrender::Color::White);
```

### `int AddAnimation(const AtlasAnimation& anim)`

Добавляет анимацию или **заменяет** существующую с тем же именем. Возвращает
индекс анимации в списке. Кадры не проверяются на существование — их проверяют
при отрисовке, поэтому индексы должны указывать в `Regions()`.

```cpp
crossrender::AtlasAnimation run;
run.name = "run";
run.frames = {atlas.IndexOf("run_0"), atlas.IndexOf("run_1")};
run.durations = {100.0f, 100.0f};
const int index = atlas.AddAnimation(run);
ENG_LOGI("demo", "анимация записана под индексом %d", index);
```

### `const AtlasAnimation* FindAnimation(const std::string& name) const`

Указатель на анимацию или `nullptr`. Указатель валиден, пока атлас не изменён
(`AddAnimation`, `BuildAnimationsFromPrefixes`, `Clear`, перемещение).

```cpp
if (const crossrender::AtlasAnimation* a = atlas.FindAnimation("run")) {
    ENG_LOGI("demo", "'run': %zu кадров, %.0f мс", a->frames.size(), a->TotalDuration());
}
```

### `std::vector<std::string> AnimationNames() const`

Имена всех анимаций в «естественном» порядке (не в порядке добавления).

```cpp
const std::vector<std::string> names = atlas.AnimationNames();
for (const std::string& n : names) ENG_LOGD("demo", "анимация '%s'", n.c_str());
```

### `int BuildAnimationsFromPrefixes(const std::string& separator = "_")`

Автоматически собирает анимации из имён регионов вида `<база><разделитель><число>`.
Имя делится по **последнему** вхождению `separator`; группа берётся, только если
база не пуста, суффикс целиком цифровой и в группе не меньше двух регионов.
Кадры сортируются по числовому значению суффикса (при равенстве — «естественным»
сравнением имён), длительности берутся из `durationMs` регионов (иначе 100 мс),
`loop` включается. Уже существующая анимация с таким именем не перезаписывается.
Возвращает число созданных анимаций; пустой `separator` даёт `0`. При загрузке
дескриптора метод вызывается автоматически, если анимаций не нашлось.

```cpp
crossrender::SpriteAtlas atlas;
atlas.LoadPlainImage("assets/strip.png");          // регионы пока не сгруппировать
atlas.BuildFromFiles({"assets/run_0.png", "assets/run_1.png", "assets/run_10.png"});
const int created = atlas.BuildAnimationsFromPrefixes("_");
// Порядок кадров — 0, 1, 10: «естественный», а не побайтовый.
ENG_LOGI("demo", "создано анимаций: %d", created);
```

### `int AnimationFrameAt(const std::string& name, f32 timeSeconds) const`

Номер кадра анимации в момент `timeSeconds` (секунды). Возвращает `-1`, если
анимации нет или у неё нет кадров. Тонкая обёртка над `AtlasAnimation::FrameAt`.

```cpp
const int frame = atlas.AnimationFrameAt("run", elapsed);
if (frame >= 0) ENG_LOGI("demo", "сейчас кадр %d", frame);
```

### `bool DrawAnimation(Renderer2D& r2d, const std::string& name, const Rect& dst, f32 timeSeconds, const Color& tint = Color::White) const`

Рисует кадр анимации, соответствующий времени `timeSeconds`, растягивая его на
`dst`. Возвращает `false`, если анимация не найдена, у неё нет кадров или кадр
не удалось нарисовать.

```cpp
static crossrender::f32 t = 0.0f;
t += dt;
atlas.DrawAnimation(r2d, "run", crossrender::Rect{200, 200, 96, 96}, t, crossrender::Color::White);
```

### `bool DrawAnimationFrame(Renderer2D& r2d, const std::string& name, int frame, const Rect& dst, const Color& tint = Color::White) const`

Рисует конкретный кадр анимации по номеру. Номер зажимается в
`[0, frames.size() - 1]`, поэтому выход за диапазон не «ломает» отрисовку, а
показывает крайний кадр.

```cpp
atlas.DrawAnimationFrame(r2d, "run", 2, crossrender::Rect{200, 200, 96, 96}, crossrender::Color::White);
```

### `const Stats& GetStats() const`

Статистика атласа, пересчитываемая при каждом изменении. Ссылка действительна,
пока атлас не изменён.

```cpp
const crossrender::SpriteAtlas::Stats& s = atlas.GetStats();
ENG_LOGI("demo", "регионов %d, страниц %d, заполнение %.1f%%", s.regions, s.pages,
         s.occupancy * 100.0f);
```

### `bool PagePixels(int page, std::vector<u8>* rgba, int* width, int* height) const`

Отдаёт CPU-пиксели страницы (`RGBA8`, сверху вниз). Заполнены только у атласов,
собранных `BuildFromFiles` или `LoadPlainImage`; у загруженного с диска атласа
функция вернёт `false`. Любой из выходных указателей может быть `nullptr`, если
нужны не все значения.

```cpp
std::vector<crossrender::u8> rgba;
int w = 0, h = 0;
if (atlas.PagePixels(0, &rgba, &w, &h)) {
    ENG_LOGI("demo", "CPU-копия страницы %dx%d, %zu байт", w, h, rgba.size());
}
```

### `bool SavePageImage(const std::string& pngPath, int page = 0) const`

Сохраняет CPU-пиксели страницы в PNG (через `Texture::EncodePng`, при
необходимости создавая каталоги). Требует, чтобы `PagePixels` вернул данные,
иначе возвращает `false`. Удобно для отладки процедурно собранных атласов.

```cpp
if (!atlas.SavePageImage("out/atlas_page0.png", 0)) {
    ENG_LOGW("demo", "нет CPU-пикселей: атлас был загружен, а не собран");
}
```

## Пример целиком

```cpp
#include "crossrender/gfx/SpriteAtlas.h"

#include "crossrender/core/File.h"
#include "crossrender/core/Log.h"
#include "crossrender/gfx/Renderer2D.h"

#include <string>
#include <vector>

// Рисует кадр интерфейса из атласа: панель-девятипатч, стоящего на земле
// персонажа и бегущую анимацию, а затем собирает и сохраняет второй атлас из
// отдельных картинок.
void DrawAtlasDemo(crossrender::Renderer2D& r2d, crossrender::f32 dt) {
    // 1. Загрузка дескриптора. Отсутствие картинок страниц ошибкой не является:
    //    регионы останутся, а рисование будет молча пропускаться.
    crossrender::SpriteAtlas atlas;
    if (!atlas.LoadFromFile("assets/atlas/atlas.json", /*srgb=*/false)) {
        ENG_LOGE("demo", "дескриптор атласа не разобран");
        return;
    }
    ENG_LOGI("demo", "формат %s, регионов %d, страниц %d",
             crossrender::SpriteAtlas::FormatName(atlas.Format()), atlas.RegionCount(), atlas.PageCount());
    for (const std::string& w : atlas.Warnings()) ENG_LOGW("demo", "атлас: %s", w.c_str());

    // 2. Панель: девять частей, углы не растягиваются.
    const crossrender::Rect panel{40.0f, 40.0f, 420.0f, 260.0f};
    if (atlas.Has("panel")) atlas.DrawNinePatch(r2d, "panel", panel, 12.0f);

    // 3. Персонаж: привязка по низу, чтобы позиция была точкой «под ногами».
    const crossrender::AtlasRegion* hero = atlas.Find("hero");
    if (hero && atlas.PageTexture(hero->page).Valid()) {
        atlas.DrawAnchored(r2d, "hero", {panel.Center().x, panel.Bottom() - 24.0f},
                           crossrender::Color::White, 1.0f);
        ENG_LOGI("demo", "'%s': кадр %.0fx%.0f, обрезан=%d", hero->name.c_str(), hero->Size().x,
                 hero->Size().y, hero->trimmed ? 1 : 0);
    }

    // 4. Анимация: если дескриптор её не принёс, группы вида "run_0..run_9"
    //    соберутся по префиксу, а кадры встанут в естественном порядке.
    if (atlas.AnimationNames().empty()) atlas.BuildAnimationsFromPrefixes("_");
    static crossrender::f32 time = 0.0f;
    time += dt;
    atlas.DrawAnimation(r2d, "run", crossrender::Rect{panel.Right() + 24.0f, panel.y, 96.0f, 96.0f}, time);
    ENG_LOGI("demo", "кадр анимации: %d", atlas.AnimationFrameAt("run", time));

    // 5. Обратный путь: собрать атлас из отдельных файлов, посмотреть статистику
    //    и сохранить его вместе с PNG страниц (CPU-пиксели ещё живы).
    crossrender::SpriteAtlas baked;
    crossrender::SpriteAtlas::PackOptions opts;
    opts.maxSize = 1024;
    opts.padding = 2;
    opts.trim = true;
    if (baked.BuildFromFiles({"assets/coin.png", "assets/gem.png", "assets/key.png"}, opts)) {
        const crossrender::SpriteAtlas::Stats& s = baked.GetStats();
        ENG_LOGI("demo", "упаковано %d регионов, заполнение %.1f%%", s.regions, s.occupancy * 100.0f);
        baked.SortRegionsByName();
        baked.SaveToFile("out/packed_atlas.json", /*writePageImages=*/true);
        baked.SavePageImage("out/packed_atlas_page0.png", 0);
    } else {
        ENG_LOGW("demo", "ни одной картинки не декодировано");
    }

    // 6. Чистый CPU-путь: разобрать дескриптор из памяти без окна и GPU.
    const crossrender::ByteBuffer bytes = crossrender::ReadBinaryFile("assets/atlas/atlas_texturepacker.json");
    crossrender::SpriteAtlas headless;
    if (!bytes.empty()) {
        headless.LoadFromMemory(std::string(bytes.begin(), bytes.end()), "assets/atlas",
                                "atlas_texturepacker.json");
        for (const std::string& name : headless.NamesWithPrefix("tile_")) {
            ENG_LOGD("demo", "тайл: %s", name.c_str());
        }
    }
}
```

## См. также

* `docs/gfx/Texture.md` — `Texture` и `Texture::ImageData`, которыми атлас
  загружает страницы и сохраняет PNG; там же описано поведение без контекста
  OpenGL.
* `docs/gfx/Renderer2D.md` — `Renderer2D`, через который атлас рисует регионы,
  `Image` и `NinePatch`, а также необязательный `NinePatch` для `Image9`.
* `docs/core/File.md` — `ReadTextFile`, `ReadBinaryFile`, `PathJoin`,
  `PathDir`, `PathBase` и `FileExists`, которыми разрешаются пути дескриптора и
  страниц.
* `docs/core/Json.md` — `JsonValue`, используемый разбором и записью
  дескрипторов в формате engine JSON.
* `docs/core/Log.md` — макросы `ENG_LOGW` / `ENG_LOGE`: в этот лог с тегом
  `atlas` попадают все предупреждения атласа.

