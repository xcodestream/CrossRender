# crossrender/gfx/Retro.h — ретро-режимы вывода: пиксель-арт и ASCII

Ретро-режимы вывода: низкоразрешённый виртуальный кадровый буфер (framebuffer)
с квантованием по палитре, упорядоченным дизерингом (ordered dithering) и
CRT-эффектами (pixel mode), а также преобразование всего кадра в символьную
сетку на GPU (ASCII mode).

## Заголовок

```cpp
#include "crossrender/gfx/Retro.h"
```

## Обзор

Заголовок делится на три части:

1. **Режимы и настройки.** `RetroMode` выбирает выключенный режим, пиксель-арт
   или ASCII; `RetroPalette` — таблицу аппаратных палитр; `AsciiCharset` —
   набор символов (ramp). Всё это лежит в одной структуре `RetroSettings`,
   которую движок читает каждый кадр.
2. **`RetroDisplay`.** Владеет виртуальным `RenderTarget`, двумя resolve-шейдерами
   (шейдер — программа для GPU) и атласом ASCII-глифов. Именно он выполняет
   фактическую отрисовку кадра.
3. **CPU-хелперы и пресеты.** Статические методы `PaletteSize`, `PaletteColor`,
   `Quantize`, `BayerThreshold`, `ApplyPalette`, `BuildAsciiGrid` повторяют ту же
   математику на CPU — для тестов, инструментов и headless-сборок.
   `BuiltinRetroPresets()` отдаёт готовые наборы настроек.

#### Как режим подключён к движку

`RetroDisplay` не нужно создавать самому: фасад `Engine` держит его внутри и
управляет им по кадру. Сцена только заполняет настройки:

```cpp
// Сцена просит пиксель-арт; движок сам создаст виртуальный буфер и resolve.
void Setup(crossrender::Engine& engine) {
    engine.RetroCfg().mode = crossrender::RetroMode::Pixel;   // Off по умолчанию
    engine.RetroCfg().palette = crossrender::RetroPalette::Pico8;
    engine.RetroCfg().virtualWidth = 160;
    engine.RetroCfg().virtualHeight = 144;
}
```

Порядок работы движка (см. `engine/src/scene/Engine.cpp`):

1. `Engine::UpdateViewport()` при `mode != RetroMode::Off` вызывает
   `RetroDisplay::Resize` и **пересчитывает логический экран** (`SceneContext::viewport`,
   `Engine::Viewport()`) в координатах виртуального разрешения, а не окна. UI и
   сцены продолжают верстаться в привычных координатах — просто они становятся
   «низкоразрешёнными».
2. `Step()` направляет 3D-проход, 2D-проход, UI и оверлеи в
   `RetroDisplay::VirtualTarget()` (через `RetroDisplay::BeginFrame`). То есть в
   виртуальный буфер попадает **весь кадр, включая интерфейс**.
3. Если включены фильтры (`EngineConfig::enableFilters`), пост-обработка
   применяется к виртуальному изображению до resolve.
4. `RetroDisplay::EndFrame` рисует виртуальное изображение в текущий
   фреймбуфер: сначала заливка letterbox (чёрные поля вокруг изображения),
   затем один полноэкранный четырёхугольник с пиксельным или ASCII-шейдером.

Режим включается из командной строки примера `examples/sources`:

```
--retro <off|pixel|ascii>   ретро-режим
--retro-palette <name>      nes|gameboy|c64|pico8|cga|ega|spectrum|mono|none
--retro-res <WxH>           виртуальное разрешение (по умолчанию 384x216)
```

Самому `RetroDisplay` доступны те же шаги вручную:

1. `Init()` — собрать шейдеры и quad (нужен контекст OpenGL).
2. `Resize(fbW, fbH, settings)` — создать/пересоздать виртуальный буфер.
3. `BeginFrame(r2d, settings, dpi)` — привязать виртуальный буфер и получить
   логический прямоугольник для вёрстки.
4. Нарисовать сцену и UI.
5. `EndFrame(r2d, settings, fbW, fbH)` — разрешить изображение в текущий
   фреймбуфер.
6. `Shutdown()` — освободить ресурсы (или деструктор).

#### Виртуальный буфер и масштабирование

`Resize` заводит `RenderTarget` размера `virtualWidth x virtualHeight`,
зажатого по каждой оси в `64..1024`; формат — `RGBA8`, есть буфер глубины, без
стенсила и без MSAA, фильтрация `NEAREST` (`desc.name = "retro-virtual"`).
Кадр в него рисуется в масштабе 1 логическая единица = 1 виртуальный пиксель
(`r2d.BeginFrame(virtualW_, virtualH_, 1.0f, virtual_)`).

`ViewportRect` вписывает виртуальное изображение в окно:

* при `integerScale == true` берётся **целый** масштаб
  `floor(min(fbW / vw, fbH / vh))`, минимум 1 — пиксели остаются резкими;
* иначе (или если даже 1:1 не влезает) изображение растягивается с сохранением
  пропорций;
* результат центрируется: `x = floor((fbW - w) / 2)`.

#### Порядок проходов пиксельного режима

`EndFrame` всегда делает ровно два розыгрыша (draw):

1. **Letterbox.** Пиксельный шейдер с `uMode = 0` заливает весь фреймбуфер
   цветом `letterbox`.
2. **Resolve.** Четырёхугольник по `ViewportRect` с `uMode = 1`; при
   `showOverscan == true` он расширяется на
   `border = max(2, floor(max(vp.w, vp.h) * 0.012))`, а шейдер (`uOverscan = 1`)
   подсвечивает полосу, оказавшуюся за пределами изображения, через
   `outsideColor()`.

Порядок операций внутри пиксельного шейдера (важен для понимания, что на что
влияет):

| № | Шаг | Условие |
|---|---|---|
| 1 | искривление UV (`crtCurvature`, `curvature`) | `crtCurvature == true` |
| 2 | выборка виртуального изображения (`NEAREST`) | всегда |
| 3 | насыщенность (`saturation`) | всегда |
| 4 | контраст (`contrast`), яркость (`brightness`) | всегда |
| 5 | упорядоченный дизеринг Байера | `dither` и (`palette != None` или `colorDepth >= 0.5`) |
| 6 | квантование по битам (`colorDepth`) | `colorDepth >= 0.5` |
| 7 | квантование по палитре (взвешенный ближайший цвет) | `palette != None` |
| 8 | 4-тапное свечение (`bloom`) | `bloom > 0` |
| 9 | скан-линии (`scanlines`) | `scanlines == true` |
| 10 | апертурная решётка RGB (`crtMask`) | `crtMask == true` |

Пункт 8 стоит **после** квантования: свечение может вернуть цвета вне палитры,
повторного квантования нет. Дизеринг тоже осмыслен только тогда, когда что-то
квантует результат, — поэтому без палитры и без `colorDepth` он молча
пропускается.

#### Порядок операций ASCII-режима

1. Ячейка сетки: `cell = floor(uv * (asciiCols, rows))`.
2. Яркость ячейки: среднее 3x3 выборок вокруг центра ячейки (шаг
   `0.25 * размер ячейки`), затем `luma` по Rec.709
   (`0.2126 R + 0.7152 G + 0.0722 B`).
3. Тон: `pow(lum, 1/gamma)`, затем контраст и яркость.
4. Индекс символа: `index = min(floor(lum * N), N - 1)`, где `N` — длина
   ramp; при `asciiInvert == true` индекс меняется на `N - 1 - index`.
5. Символ берётся из атласа как альфа-покрытие (`uAtlas.a`); цвет — либо
   исходный цвет ячейки (`asciiColor`), либо `asciiInk`; фон — `asciiPaper`
   (`asciiBackgroundFill`) или `letterbox`.
6. Поверх — сетка ячеек (`asciiShowGrid`) и скан-линии (`asciiScanlines`).

#### Честные ограничения

* **ASCII-режим преобразует весь кадр, включая интерфейс.** UI рисуется в тот же
  виртуальный буфер, что и сцена, поэтому кнопки и текст становятся частью
  символьной сетки. Отдельного «слоя UI поверх ASCII» в движке нет.
* Виртуальное разрешение зажимается в `64..1024` по каждой оси,
  `asciiCols` — в `1..400`, `asciiRows` — в `1..200` (при `0` строки выводятся
  из пропорций: `round(cols * (h / w) / clamp(asciiCellAspect, 0.2, 2))`).
* Палитра в шейдере читается циклом до 64 записей (`for i < 64`), поэтому
  таблица длиннее 64 цветов была бы обрезана. Все встроенные палитры укладываются
  в лимит (самая большая — `Ega64`, ровно 64).
* `Custom`-ramp с менее чем двумя кодпойнтами молча заменяется на `Ramp10`.
* Атлас ASCII строится «через чтение с GPU»: символы рисуются `Renderer2D` в
  offscreen-цель и возвращаются `ReadPixels`. Если контекста нет или чтение не
  удалось, `BuildAsciiAtlas` возвращает `false`, а `EndFrame` в ASCII-режиме
  **оставляет кадр нетронутым**, чтобы не «пробить» в нём дыру.
* Встроенный процедурный bitmap-шрифт сообщает о наличии любого кодпойнта
  (рисует пустую рамку), поэтому символы `Blocks`/`Braille` из него не берутся:
  для них нужен настоящий файл шрифта, иначе атлас подставляет символ из
  `Ramp70` по тому же индексу и пишет предупреждение в лог.
* Дизеринг и квантование по `colorDepth` работают только вместе с палитрой или
  заданной глубиной цвета (см. шаг 5 выше).
* `Quantize`/`ApplyPalette`/`BuildAsciiGrid` — CPU-двойники шейдера. Они
  повторяют ту же математику, но истиной остаётся шейдер: CPU-версии нужны
  тестам и headless-инструментам.
* `ApplyPalette` обрабатывает только RGB, альфа-канал не трогается.

## Члены класса

### `enum class RetroMode : u8`

Режим вывода кадра.

| Значение | Смысл |
|---|---|
| `RetroMode::Off` | кадр идёт прямо во фреймбуфер; `RetroDisplay` не вмешивается |
| `RetroMode::Pixel` | низкоразрешённый буфер, целочисленное масштабирование, палитра, дизеринг, CRT |
| `RetroMode::Ascii` | низкоразрешённый буфер превращается в символьную сетку на GPU |

```cpp
crossrender::RetroSettings s;
s.mode = crossrender::RetroMode::Pixel;           // включить пиксель-арт
if (s.mode != crossrender::RetroMode::Off) ENG_LOGI("demo", "ретро-режим активен");
```

### `enum class RetroPalette : u8`

Аппаратные палитры для пиксельного режима и редактора палитр. Значение
`None` отключает квантование (истинный цвет), `Count` — служебный счётчик, а не
палитра. Список значений документируется **одной таблицей** (отдельного
подраздела на каждую палитру нет — их слишком много и они однотипны); цвета
каждой палитры перечислены в `engine/src/gfx/RetroPalette.cpp`.

| Значение | Цветов | `PaletteName` | Что это |
|---|---|---|---|
| `RetroPalette::None` | 0 | `"None"` | без квантования |
| `RetroPalette::Nes` | 54 | `"NES"` | NES/Famicom (Ricoh 2C02), 54 используемых цвета |
| `RetroPalette::GameBoy` | 4 | `"Game Boy"` | четыре оттенка зелёного DMG-01 |
| `RetroPalette::GameBoyPocket` | 4 | `"Game Boy Pocket"` | серая LCD-шкала |
| `RetroPalette::Cga16` | 16 | `"CGA 16"` | CGA/EGA, 16 цветов (коричневый вместо тёмно-жёлтого) |
| `RetroPalette::Ega64` | 64 | `"EGA 64"` | полный куб 4x4x4 |
| `RetroPalette::C64` | 16 | `"Commodore 64"` | палитра VIC-II (Pepto) |
| `RetroPalette::Pico8` | 16 | `"PICO-8"` | официальная палитра PICO-8 |
| `RetroPalette::Amstrad32` | 27 | `"Amstrad CPC"` | три уровня на канал |
| `RetroPalette::ZxSpectrum` | 15 | `"ZX Spectrum"` | 8 оттенков на двух уровнях яркости (чёрный совпадает) |
| `RetroPalette::Mono` | 2 | `"Monochrome"` | чёрный и белый |
| `RetroPalette::VirtualBoy` | 4 | `"Virtual Boy"` | четыре оттенка красного |
| `RetroPalette::Count` | — | `"Unknown"` | служебный счётчик; `PaletteSize` вернёт `0` |

Все таблицы — ручная транскрипция реального железа (см. комментарии в
`engine/src/gfx/RetroPalette.cpp`).

```cpp
crossrender::RetroSettings s;
s.palette = crossrender::RetroPalette::GameBoy;
ENG_LOGI("demo", "палитра %s: %d цветов", crossrender::RetroDisplay::PaletteName(s.palette),
         crossrender::RetroDisplay::PaletteSize(s.palette));
```

### `enum class AsciiCharset : u8`

Набор символов (ramp), по которому раскладывается яркость ячейки. Порядок —
**от самого тёмного символа к самому светлому**. Значения, как и у палитр,
документируются одной таблицей; сам список кодпойнтов отдаёт
`RetroDisplay::CharsetRamp`.

| Значение | Символов | Что содержит |
|---|---|---|
| `AsciiCharset::Ramp10` | 10 | классический `" .:-=+*#%@"` |
| `AsciiCharset::Ramp70` | 70 | полная 70-уровневая шкала (`" .'\`^\",:;Il!i><~+_-?..."`) |
| `AsciiCharset::Blocks` | 5 | блочные элементы Unicode: `U+0020`, `U+2591`, `U+2592`, `U+2593`, `U+2588` |
| `AsciiCharset::Braille` | 9 | точки Брайля `U+2800` + маска 2x4 (`U+2801`, `U+2803`, …) |
| `AsciiCharset::Custom` | — | ramp из `RetroSettings::customRamp` (UTF-8, тёмные первыми) |
| `AsciiCharset::Count` | — | служебный счётчик |

```cpp
crossrender::RetroSettings s;
s.charset = crossrender::AsciiCharset::Blocks;
const std::vector<crossrender::u32> ramp = crossrender::RetroDisplay::CharsetRamp(s.charset, s.customRamp);
ENG_LOGI("demo", "в ramp %zu символов", ramp.size());
```

### `struct RetroSettings`

Полный набор настроек ретро-режима: и переключатели режима, и параметры обоих
разрешающих шейдеров. Структура целиком передаётся в `Resize`, `BeginFrame` и
`EndFrame`, поэтому менять её можно в любой момент — новый набор применится на
следующем кадре. Русские пояснения к каждой группе полей — в разделе `Обзор`.

```cpp
crossrender::RetroSettings s;                 // по умолчанию mode == Off
s.mode = crossrender::RetroMode::Pixel;
s.virtualWidth = 256;
s.virtualHeight = 240;
s.palette = crossrender::RetroPalette::Nes;
s.dither = true;
```

### `RetroMode RetroSettings::mode`

Режим вывода. `Off` по умолчанию — это то, что видит движок, пока сцена не
включит ретро сама. Значение читается каждый кадр, поэтому переключение
«на лету» работает без пересоздания `RetroDisplay`.

```cpp
crossrender::RetroSettings s;
s.mode = crossrender::RetroMode::Ascii;       // включить ASCII-режим
ENG_LOGI("demo", "mode=%d", static_cast<int>(s.mode));
```

### `int RetroSettings::virtualWidth`

Ширина виртуального кадра в пикселях, по умолчанию `320`. В `Resize` зажимается
в `64..1024`. Именно в этих координатах верстается UI и рисуется сцена.

```cpp
crossrender::RetroSettings s;
s.virtualWidth = 160;                 // Game Boy-подобная ширина
```

### `int RetroSettings::virtualHeight`

Высота виртуального кадра в пикселях, по умолчанию `180`; зажимается в
`64..1024`. Соотношение `virtualWidth : virtualHeight` задаёт пропорции
изображения внутри окна.

```cpp
crossrender::RetroSettings s;
s.virtualWidth = 160;
s.virtualHeight = 144;                // пропорции Game Boy
```

### `bool RetroSettings::integerScale`

`true` (по умолчанию) — масштаб виртуального изображения округляется вниз до
целого, чтобы пиксели не «мылились»; остальное окно занимает letterbox. `false`
растягивает изображение на всё окно с сохранением пропорций (дробный масштаб).

```cpp
crossrender::RetroSettings s;
s.integerScale = false;               // например, для 512x342 «Mono Mac»
```

### `bool RetroSettings::showOverscan`

Рисовать ли область за пределами виртуального изображения. При `true` шейдер
заливает бордюр letterbox-цветом с подсветкой края, а resolve-четырёхугольник
расширяется на `max(2, floor(max(vp.w, vp.h) * 0.012))` пикселей.

```cpp
crossrender::RetroSettings s;
s.showOverscan = true;                // «кинескопная» рамка вокруг картинки
s.letterbox = crossrender::Color{0.015f, 0.015f, 0.02f, 1.0f};
```

### `Color RetroSettings::letterbox`

Цвет фона за пределами изображения, по умолчанию почти чёрный
`{0.02, 0.02, 0.03, 1}`. Им же заливается весь фреймбуфер перед resolve, и он
же служит фоном ASCII-глифов, когда `asciiBackgroundFill == false`.

```cpp
crossrender::RetroSettings s;
s.letterbox = crossrender::Color::FromRGB(0x0F380F);   // зелёный фон Game Boy
```

### `RetroPalette RetroSettings::palette`

Палитра квантования пиксельного режима; `None` по умолчанию (истинный цвет).
Палитра загружается в текстуру `64x1` `RGBA8` при первом использовании и
кэшируется до смены значения.

```cpp
crossrender::RetroSettings s;
s.palette = crossrender::RetroPalette::C64;
s.dither = true;                      // без дизеринга 16 цветов выглядят грубо
```

### `bool RetroSettings::dither`

Включает упорядоченный дизеринг Байера при квантовании. Работает только если
что-то квантует результат: задана палитра или `colorDepth >= 0.5`.

```cpp
crossrender::RetroSettings s;
s.palette = crossrender::RetroPalette::Mono;
s.dither = true;                      // 1-битная «газета» из полутонов
```

### `int RetroSettings::ditherMatrix`

Размер матрицы Байера: `2`, `4` (по умолчанию) или `8`. Любое значение
округляется вниз к ближайшему из этих трёх (`>= 8` даёт 8, `>= 4` даёт 4, иначе
2). Больше матрица — плавнее полутон, но заметнее регулярный узор.

```cpp
crossrender::RetroSettings s;
s.dither = true;
s.ditherMatrix = 8;                   // самая плавная шкала
```

### `f32 RetroSettings::ditherStrength`

Амплитуда дизеринга: доля шага палитры (или шага глубины цвета), по умолчанию
`1.0`. Значения больше единицы дают более «шумный» результат, `0` отключает
эффект, не выключая сам флаг `dither`.

```cpp
crossrender::RetroSettings s;
s.dither = true;
s.ditherStrength = 0.9f;
```

### `f32 RetroSettings::colorDepth`

Глубина цвета в битах на канал: `0` (по умолчанию) — квантовать только по
палитре; иначе значение от `2` до `6` (округляется). Квантование по глубине
выполняется **до** квантования по палитре.

```cpp
crossrender::RetroSettings s;
s.colorDepth = 3.0f;                  // 3 бита на канал, как у Amstrad CPC
```

### `bool RetroSettings::scanlines`

Рисует горизонтальные затемнённые строки. Строки физические (во фреймбуфере),
их число берётся из `scanlineCount`, а при `0` — из высоты виртуального кадра.

```cpp
crossrender::RetroSettings s;
s.scanlines = true;
s.scanlineStrength = 0.3f;
```

### `f32 RetroSettings::scanlineStrength`

Сила затемнения скан-линий, по умолчанию `0.30` (0 — невидимо, 1 — строка
чёрная). Работает и в ASCII-режиме, но там у него отдельное поле
`asciiScanlineStrength`.

```cpp
crossrender::RetroSettings s;
s.scanlines = true;
s.scanlineStrength = 0.1f;            // едва заметная сетка
```

### `f32 RetroSettings::scanlineCount`

Число скан-линий на высоту изображения; `0` (по умолчанию) — выводить из
`virtualHeight`. Задавайте вручную, если нужна частота «настоящего» телевизора
независимо от виртуального разрешения.

```cpp
crossrender::RetroSettings s;
s.scanlines = true;
s.scanlineCount = 525.0f;             // NTSC-подобная частота строк
```

### `bool RetroSettings::crtCurvature`

Включает искривление координат (бочкообразную дисторсию кинескопа). Пиксели за
пределами изображения после искажения заливаются фоном letterbox/overscan.

```cpp
crossrender::RetroSettings s;
s.crtCurvature = true;
s.curvature = 0.08f;
```

### `f32 RetroSettings::curvature`

Сила искривления, по умолчанию `0.06`. В шейдере координаты умножаются на
`1 + curvature * dot(q, q)`, где `q` — координата в диапазоне `-1..1`.

```cpp
crossrender::RetroSettings s;
s.crtCurvature = true;
s.curvature = 0.12f;                  // заметно выпуклый экран
```

### `bool RetroSettings::crtMask`

Включает апертурную решётку (aperture grille): каждая третья физическая колонка
пикселей получает усиленный красный, зелёный или синий канал.

```cpp
crossrender::RetroSettings s;
s.crtMask = true;
s.crtMaskStrength = 0.3f;
```

### `f32 RetroSettings::crtMaskStrength`

Сила решётки RGB, по умолчанию `0.25`; смешивает белый цвет с цветной маской.
При `1.0` картинка становится заметно цветной по колонкам.

```cpp
crossrender::RetroSettings s;
s.crtMask = true;
s.crtMaskStrength = 0.25f;
```

### `f32 RetroSettings::bloom`

Простое 4-тапное свечение (crossrender): дополнительные выборки вокруг пикселя
добавляются к уже квантованному изображению. По умолчанию `0` (выключено).
Поскольку шаг идёт после палитры, свечение может вывести цвет за пределы
палитры.

```cpp
crossrender::RetroSettings s;
s.bloom = 0.55f;                      // мягкое свечение ярких участков
```

### `f32 RetroSettings::brightness`

Множитель яркости до квантования, по умолчанию `1.0`. Применяется как
`clamp(col * brightness, 0, 1)`.

```cpp
crossrender::RetroSettings s;
s.brightness = 1.1f;                  // чуть ярче
```

### `f32 RetroSettings::contrast`

Контраст вокруг середины `0.5`: `(col - 0.5) * contrast + 0.5`. По умолчанию
`1.0`.

```cpp
crossrender::RetroSettings s;
s.contrast = 1.25f;                   // более «звонкая» картинка
```

### `f32 RetroSettings::saturation`

Насыщенность: `0` — оттенки серого, `1` (по умолчанию) — без изменений,
больше `1` — усиление цвета. Считается через яркость Rec.709.

```cpp
crossrender::RetroSettings s;
s.saturation = 0.0f;                  // чёрно-белый CRT
```

### `int RetroSettings::asciiCols`

Число колонок ASCII-сетки, по умолчанию `100`; зажимается в `1..400`. Вместе с
`asciiRows` определяет и размер ячейки, и «разрешение» символьной картинки.

```cpp
crossrender::RetroSettings s;
s.mode = crossrender::RetroMode::Ascii;
s.asciiCols = 80;
```

### `int RetroSettings::asciiRows`

Число строк ASCII-сетки; по умолчанию `40`. Если задать `0`, строки выводятся
из пропорций: `round(cols * (h / w) / clamp(asciiCellAspect, 0.2, 2))`.
Ненулевое значение зажимается в `1..200`.

```cpp
crossrender::RetroSettings s;
s.mode = crossrender::RetroMode::Ascii;
s.asciiRows = 0;                      // строки из пропорций виртуального кадра
```

### `AsciiCharset RetroSettings::charset`

Набор символов для ASCII-режима. Смена значения инвалидирует построенный атлас:
`EndFrame` уничтожит его и построит заново.

```cpp
crossrender::RetroSettings s;
s.charset = crossrender::AsciiCharset::Ramp70;
```

### `std::string RetroSettings::customRamp`

Пользовательский ramp в UTF-8, **тёмные символы первыми**. Используется только
при `charset == AsciiCharset::Custom`. Если в строке меньше двух кодпойнтов,
применяется `Ramp10`.

```cpp
crossrender::RetroSettings s;
s.charset = crossrender::AsciiCharset::Custom;
s.customRamp = " .oO@";                // свой ramp из пяти символов
```

### `bool RetroSettings::asciiColor`

`true` (по умолчанию) — символы красятся исходным цветом ячейки, `false` — все
символы получают цвет `asciiInk` (монохромный терминал).

```cpp
crossrender::RetroSettings s;
s.asciiColor = false;                  // зелёный терминал
s.asciiInk = crossrender::Color::FromRGB(0x33FF66);
```

### `bool RetroSettings::asciiInvert`

Меняет отображение яркости на обратное: тёмные участки становятся плотными
символами. Индекс считается как `N - 1 - index`.

```cpp
crossrender::RetroSettings s;
s.asciiInvert = true;                  // «негатив» для светлого фона
```

### `Color RetroSettings::asciiInk`

Цвет чернил для символов при `asciiColor == false`, по умолчанию
`{0.85, 0.95, 0.85, 1}`. При цветном режиме не используется.

```cpp
crossrender::RetroSettings s;
s.asciiInk = crossrender::Color::FromRGB(0xFFB000);   // янтарный терминал
```

### `Color RetroSettings::asciiPaper`

Цвет бумаги — фон под символами, если включён `asciiBackgroundFill`. По
умолчанию `{0.02, 0.03, 0.02, 1}`. При выключенной заливке фоном служит
`letterbox`.

```cpp
crossrender::RetroSettings s;
s.asciiPaper = crossrender::Color{0.01f, 0.03f, 0.01f, 1.0f};
```

### `f32 RetroSettings::asciiGamma`

Гамма-коррекция яркости ячейки: `pow(lum, 1 / gamma)`. По умолчанию `1.0`;
значения меньше `0.01` зажимаются до `0.01`.

```cpp
crossrender::RetroSettings s;
s.asciiGamma = 1.4f;                   // светлее в тенях
```

### `f32 RetroSettings::asciiContrast`

Контраст яркости ячейки вокруг `0.5`, по умолчанию `1.0`. Применяется после
гаммы и до яркости.

```cpp
crossrender::RetroSettings s;
s.asciiContrast = 1.25f;
```

### `f32 RetroSettings::asciiBrightness`

Прибавляется к яркости ячейки после контраста, по умолчанию `0.0`. Удобно,
чтобы приподнять тёмную сцену в терминальном виде.

```cpp
crossrender::RetroSettings s;
s.asciiBrightness = 0.05f;
```

### `f32 RetroSettings::asciiCellAspect`

Отношение ширины символа к высоте, по умолчанию `0.5` (моноширинный символ
вдвое уже, чем выше). Используется только при выводе числа строк из пропорций
(`asciiRows == 0`); зажимается в `0.2..2.0`.

```cpp
crossrender::RetroSettings s;
s.asciiCellAspect = 0.5f;              // стандартный терминальный символ
```

### `bool RetroSettings::asciiScanlines`

Включает скан-линии в ASCII-режиме. Их число берётся из числа строк сетки, а не
из `scanlineCount`.

```cpp
crossrender::RetroSettings s;
s.asciiScanlines = true;
s.asciiScanlineStrength = 0.22f;
```

### `f32 RetroSettings::asciiScanlineStrength`

Сила затемнения скан-линий ASCII, по умолчанию `0.35`. Не зависит от
`scanlineStrength` пиксельного режима.

```cpp
crossrender::RetroSettings s;
s.asciiScanlines = true;
s.asciiScanlineStrength = 0.2f;
```

### `bool RetroSettings::asciiShowGrid`

Показывает сетку ячеек: границы между символами затемняются, что помогает
отлаживать разметку и «читается» как старый терминал.

```cpp
crossrender::RetroSettings s;
s.asciiShowGrid = true;
```

### `bool RetroSettings::asciiBackgroundFill`

`true` (по умолчанию) — под символами заливается `asciiPaper`; `false` — фоном
служит `letterbox`, и символы ложатся прямо на изображение (режим «поверх
картинки»).

```cpp
crossrender::RetroSettings s;
s.asciiBackgroundFill = false;         // символы без «бумаги»
```

### `RetroDisplay()`

Создаёт объект в неинициализированном состоянии: внутренние шейдеры не
собраны, виртуального буфера нет, `Valid() == false`. Обращений к GPU не
происходит, поэтому конструктор безопасен до создания контекста OpenGL.

```cpp
crossrender::RetroDisplay display;             // ещё не готов, просто владелец
ENG_ASSERT(!display.Valid());
```

### `~RetroDisplay()`

Вызывает `Shutdown()`: удаляет шейдеры, VAO/VBO, текстуру палитры, атлас ASCII
и виртуальный буфер.

```cpp
{
    crossrender::RetroDisplay local;
    local.Init();
}   // здесь ресурсы освобождаются автоматически
```

### `RetroDisplay(const RetroDisplay&) = delete`

Копирование запрещено: объект владеет ресурсами GPU. Передавайте его по ссылке
или указателю — как это делает `Engine`.

```cpp
void Tune(crossrender::RetroDisplay& display) {   // по ссылке, без копии
    display.Resize(1280, 720, crossrender::RetroSettings{});
}
```

### `bool Init()`

Собирает оба шейдера (`retro-pixel`, `retro-ascii`) и создаёт quad (VAO + VBO).
Идемпотентен: повторный вызов при готовом объекте сразу возвращает `true`.
Без контекста OpenGL пишет `ENG_LOGE("retro", "no GL context; ...")` и
возвращает `false`.

* **Возвращает:** `true`, если шейдеры собраны и объект готов к работе.
* **Контекст:** требует текущего контекста OpenGL.

```cpp
crossrender::RetroDisplay display;
if (!display.Init()) {
    ENG_LOGW("demo", "ретро недоступно — рисуем обычный кадр");
}
```

### `void Shutdown()`

Освобождает все ресурсы GPU и сбрасывает состояние: шейдеры, палитру, атлас
ASCII, виртуальный буфер, счётчики. Повторный вызов безопасен. После
`Shutdown()` `Valid() == false`, а `Init()` снова создаст ресурсы.

```cpp
crossrender::RetroDisplay display;
display.Init();
display.Shutdown();                    // всё освобождено
ENG_ASSERT(!display.Valid());
```

### `void Resize(int fbWidth, int fbHeight, const RetroSettings& settings)`

Запоминает размер фреймбуфера и настройки, вычисляет виртуальный размер
(зажимая его в `64..1024`), при необходимости **пересоздаёт** виртуальный
`RenderTarget` (`RGBA8` + глубина, `NEAREST`, имя `"retro-virtual"`) и
пересчитывает `Stats`. Если `Resize` не вызывать, `BeginFrame` вызовет его сам,
подставив вместо размера окна запрошенное виртуальное разрешение.

* **Параметры:** `fbWidth`/`fbHeight` — размер окна в физических пикселях;
  `settings` — полный набор настроек.

```cpp
crossrender::RetroSettings s;
s.mode = crossrender::RetroMode::Pixel;
display.Resize(1280, 720, s);          // виртуальный буфер будет создан здесь
ENG_LOGI("demo", "виртуальный кадр %dx%d", display.VirtualWidth(), display.VirtualHeight());
```

### `bool Valid() const`

`true`, если `Init()` прошёл успешно и шейдеры собраны. Это основной способ
отличить рабочий ретро-режим от неудачной инициализации (например, в
headless-сборке без GL).

```cpp
crossrender::RetroDisplay display;
display.Init();
if (display.Valid()) ENG_LOGI("demo", "ретро-режим доступен");
```

### `Rect BeginFrame(Renderer2D& r2d, const RetroSettings& settings, f32 dpiScale)`

Начинает кадр. В ретро-режиме привязывает виртуальный буфер и вызывает
`r2d.BeginFrame(virtualW, virtualH, 1.0f, virtual_)` — одна логическая единица
равна одному виртуальному пикселю. В режиме `Off` (или если буфера нет) просто
начинает обычный кадр в размере фреймбуфера.

* **Возвращает:** логический прямоугольник, против которого должна верстаться
  сцена: `{0, 0, virtualW, virtualH}` в ретро-режиме; `{0, 0, w/dpi, h/dpi}` —
  в обычном.
* **Контекст:** `Valid()` должен быть `true`, иначе функция сама вызовет `Init()`.

```cpp
crossrender::Renderer2D& r2d = engine.R2D();
crossrender::Rect logical = display.BeginFrame(r2d, engine.RetroCfg(), engine.DpiScale());
r2d.FillRect(logical, crossrender::Color::FromARGB(0xFF101820));   // фон сцены
```

### `void EndFrame(Renderer2D& r2d, const RetroSettings& settings, int fbWidth, int fbHeight)`

Завершает кадр: вызывает `r2d.EndFrame()`, отвязывает виртуальный буфер и
разрешает изображение в **текущий связанный фреймбуфер** (тем, что был связан
до `BeginFrame`). Рисует letterbox, затем единственный четырёхугольник с
нужным шейдером. Состояние GL сохраняется и восстанавливается через внутренний
guard, так что вызов не ломает чужие настройки (программу, VAO, вьюпорт,
смешивание).

В ASCII-режиме, если атласа ещё нет, `EndFrame` строит его сам из
`FontManager::Get().DefaultFont()`: размер ячейки берётся как
`clamp(round(vp.w / cols), 4, 64)` на `clamp(round(vp.h / rows), 4, 64)`. Если
шрифта нет или построение не удалось, попытка больше не повторяется
(`atlasAutoFailed`), и кадр остаётся нетронутым.

* **Параметры:** `fbWidth`/`fbHeight` — размер целевого фреймбуфера.

```cpp
display.BeginFrame(r2d, engine.RetroCfg(), 1.0f);
DrawGameScene(r2d);                    // сцена и UI — в виртуальном разрешении
display.EndFrame(r2d, engine.RetroCfg(), 1280, 720);
```

### `RenderTarget* VirtualTarget()`

Возвращает виртуальный цветовой таргет — например, чтобы эффект мог
отсэмплировать то, что уже нарисовано. `nullptr`, пока буфер не создан.
Константная перегрузка возвращает `const RenderTarget*`.

```cpp
if (crossrender::RenderTarget* vt = display.VirtualTarget()) {
    ENG_LOGI("demo", "виртуальный таргет %dx%d", vt->Width(), vt->Height());
}
```

### `int VirtualWidth() const`

Ширина виртуального буфера; `0`, пока `Resize` не создал его. Именно это
значение становится шириной логического экрана у движка.

```cpp
ENG_LOGI("demo", "логический экран %dx%d", display.VirtualWidth(), display.VirtualHeight());
```

### `int VirtualHeight() const`

Высота виртуального буфера; `0`, пока `Resize` не создал его.

```cpp
const float aspect = static_cast<float>(display.VirtualWidth()) /
                     static_cast<float>(display.VirtualHeight());
```

### `Rect ViewportRect(int fbWidth, int fbHeight, const RetroSettings& s) const`

Прямоугольник, в который ложится виртуальное изображение внутри фреймбуфера
(в физических пикселях, начало координат — левый верхний угол). Учитывает
`integerScale`; при дробном масштабе результат может не быть целым.

```cpp
const crossrender::Rect vp = display.ViewportRect(1280, 720, engine.RetroCfg());
ENG_LOGI("demo", "картинка занимает %.0fx%.0f в (%.0f, %.0f)", vp.w, vp.h, vp.x, vp.y);
```

### `Vec2 MapToVirtual(Vec2 point, int fbWidth, int fbHeight, const RetroSettings& s) const`

Переводит точку из координат фреймбуфера в координаты виртуального изображения
(например, чтобы превратить позицию курсора в «пиксель» сцены). Если размеры
нулевые, возвращает `{0, 0}`.

```cpp
const crossrender::Vec2 v = display.MapToVirtual(crossrender::Vec2{640, 360}, 1280, 720, engine.RetroCfg());
ENG_LOGI("demo", "курсор в виртуальном кадре: %.1f, %.1f", v.x, v.y);
```

### `Vec2 MapFromVirtual(Vec2 point, int fbWidth, int fbHeight, const RetroSettings& s) const`

Обратное преобразование: виртуальный пиксель → координаты фреймбуфера. Если
виртуальный размер нулевой, возвращает левый верхний угол вьюпорта.

```cpp
const crossrender::Vec2 fb =
    display.MapFromVirtual(crossrender::Vec2{10, 10}, 1280, 720, engine.RetroCfg());
ENG_LOGI("demo", "виртуальные (10, 10) — это (%.1f, %.1f) окна", fb.x, fb.y);
```

### `bool BuildAsciiAtlas(Font* font, int cellW, int cellH)`

Строит атлас ASCII-глифов: рисует ramp шрифтом `font` в offscreen-`RenderTarget`
(`cellsX = min(count, 16)` ячеек в ряд, ячейка `cellW x cellH`, кегль
`0.86 * cellH`) и читает результат через `ReadPixels` в текстуру `RGBA8` с
фильтрацией `NEAREST`. Шейдер использует только альфа-канал, поэтому атлас
разрешение-независим. Идемпотентен для той же пары (шрифт, размер ячейки) и
того же ramp; смена `charset`/`customRamp` заставляет перестроить атлас.

* **Возвращает:** `true`, если атлас готов; `false` при невалидном шрифте,
  неположительной ячейке, отсутствии GL или неудачном чтении.
* **Контекст:** требует контекста OpenGL и создаёт временный `Renderer2D`.

```cpp
crossrender::Font* font = crossrender::FontManager::Get().DefaultFont();
if (!display.BuildAsciiAtlas(font, 8, 16)) {
    ENG_LOGW("demo", "атлас не построился — ASCII-режим не заработает");
}
```

### `bool AsciiAtlasReady() const`

`true`, если атлас ASCII построен и валиден. Это быстрая проверка без побочных
эффектов; `EndFrame` использует её, чтобы не строить атлас повторно.

```cpp
if (display.AsciiAtlasReady()) ENG_LOGI("demo", "атлас ASCII на месте");
```

### `const Texture& AsciiAtlas() const`

Текстура атласа ASCII: сетка `AsciiCellsX() x AsciiCellsY()` ячеек, белые
глифы на прозрачном фоне, `NEAREST`, `ClampToEdge`. Глифы индексируются по
порядку ramp. У пустого объекта вернётся невалидная текстура.

```cpp
if (display.AsciiAtlasReady()) {
    ENG_LOGI("demo", "атлас %dx%d px, %d каналов", display.AsciiAtlas().Width(),
             display.AsciiAtlas().Height(), 4);
}
```

### `int AsciiCellsX() const`

Число ячеек атласа по горизонтали (`min(длина ramp, 16)`); `0`, пока атлас не
построен. Шейдер использует это как `uAtlasGrid.x`.

```cpp
ENG_LOGI("demo", "атлас: %dx%d ячеек", display.AsciiCellsX(), display.AsciiCellsY());
```

### `int AsciiCellsY() const`

Число ячеек атласа по вертикали (`ceil(длина ramp / AsciiCellsX())`); `0`, пока
атлас не построен.

```cpp
const int cells = display.AsciiCellsX() * display.AsciiCellsY();
ENG_LOGI("demo", "в атласе %d глифов", cells);
```

### `static std::vector<u32> CharsetRamp(AsciiCharset charset, const std::string& custom)`

Возвращает ramp как список кодпойнтов UTF-8 **от тёмного к светлому**.
`custom` используется только при `charset == Custom`. Если в результате меньше
двух кодпойнтов (пустая или битая строка), возвращается `Ramp10`, чтобы
ASCII-режим всегда было чем рисовать.

```cpp
const std::vector<crossrender::u32> ramp =
    crossrender::RetroDisplay::CharsetRamp(crossrender::AsciiCharset::Custom, " .oO@");
ENG_LOGI("demo", "уровней яркости: %zu", ramp.size());
```

### `static int PaletteSize(RetroPalette p)`

Число цветов в палитре; `0` для `None` и для любого значения вне таблицы
(включая `Count`).

```cpp
const int n = crossrender::RetroDisplay::PaletteSize(crossrender::RetroPalette::Nes);
ENG_LOGI("demo", "в палитре NES %d цветов", n);
```

### `static Color PaletteColor(RetroPalette p, int index)`

Цвет палитры по индексу. Индекс зажимается в `0..size-1`, поэтому выход за
диапазон не читает чужую память. Для пустой палитры возвращается белый цвет.

```cpp
for (int i = 0; i < crossrender::RetroDisplay::PaletteSize(crossrender::RetroPalette::GameBoy); ++i) {
    const crossrender::Color c = crossrender::RetroDisplay::PaletteColor(crossrender::RetroPalette::GameBoy, i);
    ENG_LOGI("demo", "оттенок %d: %.2f %.2f %.2f", i, c.r, c.g, c.b);
}
```

### `static Color Quantize(RetroPalette p, const Color& c)`

Приближает цвет к ближайшей записи палитры. Расстояние евклидово по RGB, но
взвешенное перцептивно: `0.30 R`, `0.59 G`, `0.11 B` (примерно веса яркости
Rec.601), поэтому ошибка в зелёном «дороже» ошибки в синем. Компоненты входа
зажимаются в `0..1`. Для палитры без таблицы возвращается исходный цвет.

```cpp
const crossrender::Color old = crossrender::Color::FromRGB(0x3A6EA5);
const crossrender::Color q = crossrender::RetroDisplay::Quantize(crossrender::RetroPalette::C64, old);
ENG_LOGI("demo", "квантование: (%.2f %.2f %.2f)", q.r, q.g, q.b);
```

### `static const char* PaletteName(RetroPalette p)`

Человекочитаемое имя палитры (`"NES"`, `"PICO-8"`, …). Для значения вне
таблицы возвращает `"Unknown"`.

```cpp
ENG_LOGI("demo", "выбрана палитра %s", crossrender::RetroDisplay::PaletteName(crossrender::RetroPalette::Ega64));
```

### `static f32 BayerThreshold(int x, int y, int matrixSize)`

Значение матрицы Байера в диапазоне `[0, 1)` для пикселя `(x, y)`. Базовая
матрица `M2 = [[0,2],[3,1]]/4`; размеры 4 и 8 строятся рекурсивно и в точности
совпадают с `bayer2/bayer4/bayer8` в шейдере. Любой `matrixSize` приводится к
`2`, `4` или `8`. Используется CPU-версией пайплайна.

```cpp
const crossrender::f32 t = crossrender::RetroDisplay::BayerThreshold(3, 5, 4);
ENG_LOGI("demo", "порог Байера: %.3f", t);   // 0 <= t < 1
```

### `static void ApplyPalette(u8* rgba, int width, int height, const RetroSettings& s)`

Применяет к изображению `RGBA8` в памяти **ту же** цепочку, что и пиксельный
шейдер: насыщенность, контраст, яркость, дизеринг, `colorDepth`, затем
квантование по палитре. Работает на CPU, без контекста OpenGL, и меняет буфер
на месте. Альфа-канал не трогается. Тихий no-op при `rgba == nullptr` или
неположительных размерах.

```cpp
std::vector<crossrender::u8> image(64 * 64 * 4, 200);      // серый RGBA8
crossrender::RetroSettings s;
s.palette = crossrender::RetroPalette::Mono;
s.dither = true;
crossrender::RetroDisplay::ApplyPalette(image.data(), 64, 64, s);
```

### `struct RetroDisplay::AsciiGrid`

Результат CPU-разбора изображения на символьную сетку: уровень яркости и
средний цвет каждой ячейки. Это «то, что увидел бы ASCII-шейдер», без
обращения к GPU — удобно для тестов и для вывода текстовой картинки в консоль.
Уровни **не инвертированы**: инверсию применяет вызывающий (шейдер делает это
сам).

```cpp
crossrender::RetroDisplay::AsciiGrid grid;      // пустая сетка 0x0
ENG_ASSERT(grid.cols == 0 && grid.rows == 0);
```

### `int AsciiGrid::cols`

Число колонок сетки, зажатое в `1..512` (в отличие от настроек, где предел
`400`). `0`, если буфер был пуст.

```cpp
crossrender::RetroDisplay::AsciiGrid grid =
    crossrender::RetroDisplay::BuildAsciiGrid(image.data(), 64, 64, s);
ENG_LOGI("demo", "сетка %dx%d", grid.cols, grid.rows);
```

### `int AsciiGrid::rows`

Число строк сетки: `asciiRows`, если оно больше нуля, иначе выведено из
пропорций изображения. Зажимается в `1..512`.

```cpp
const int cells = grid.cols;           // число колонок ASCII-картинки
ENG_LOGI("demo", "колонок: %d", cells);
```

### `std::vector<u8> AsciiGrid::levels`

Уровень яркости каждой ячейки, `0..255`, длина `cols * rows`, обход по строкам.
Это «сырая» яркость после гаммы, контраста и яркости, округлённая:
`round(lum * 255)`.

```cpp
if (!grid.levels.empty()) {
    ENG_LOGI("demo", "яркость первой ячейки: %d", grid.levels[grid.Index(0, 0)]);
}
```

### `std::vector<Color> AsciiGrid::colors`

Средний цвет каждой ячейки (те же девять выборок, что и для яркости), длина
`cols * rows`. Используется для цветных глифов (`asciiColor`).

```cpp
const crossrender::Color c = grid.colors[grid.Index(0, 0)];
ENG_LOGI("demo", "средний цвет ячейки: %.2f %.2f %.2f", c.r, c.g, c.b);
```

### `int AsciiGrid::Index(int x, int y) const`

Индекс ячейки в плоских массивах `levels` и `colors`: `y * cols + x`.
Границы не проверяются — вызывающий обязан держать `x < cols`, `y < rows`.

```cpp
for (int y = 0; y < grid.rows; ++y) {
    for (int x = 0; x < grid.cols; ++x) {
        Use(grid.levels[grid.Index(x, y)]);
    }
}
```

### `static AsciiGrid BuildAsciiGrid(const u8* rgba, int width, int height, const RetroSettings& s)`

Считает ASCII-сетку на CPU ровно так же, как это делает фрагментный шейдер:
центр ячейки, девять выборок с шагом `0.25` ячейки, `luma` Rec.709, гамма,
контраст, яркость. Возвращает пустую сетку при `rgba == nullptr` или
неположительных размерах. Полезно для тестов и для рендера ASCII без GPU.

```cpp
crossrender::RetroSettings s;
s.asciiCols = 40;
s.asciiRows = 0;                       // строки из пропорций
crossrender::RetroDisplay::AsciiGrid grid =
    crossrender::RetroDisplay::BuildAsciiGrid(image.data(), 320, 180, s);
ENG_LOGI("demo", "ячеек: %zu", grid.levels.size());
```

### `struct RetroDisplay::Stats`

Снимок состояния последнего кадра/`Resize`: виртуальный размер, целый масштаб,
число цветов палитры и параметры ASCII-сетки. Заполняется в `Resize` и
`EndFrame`; удобно для отладочного оверлея и тестов.

```cpp
crossrender::RetroDisplay::Stats snapshot;
ENG_LOGI("demo", "виртуальный кадр %dx%d", snapshot.virtualWidth, snapshot.virtualHeight);
```

### `int Stats::virtualWidth`

Ширина виртуального кадра, с которой работал последний кадр.

```cpp
const crossrender::RetroDisplay::Stats& st = display.GetStats();
ENG_LOGI("demo", "virtualWidth = %d", st.virtualWidth);
```

### `int Stats::virtualHeight`

Высота виртуального кадра, с которой работал последний кадр.

```cpp
if (display.GetStats().virtualHeight == 0) ENG_LOGW("demo", "буфер ещё не создан");
```

### `int Stats::scale`

Наибольший целый масштаб, при котором виртуальный кадр целиком влезает в окно
(`floor(min(fbW/vw, fbH/vh))`, минимум 1). При `integerScale == false` это всё
равно справочная величина: фактический масштаб дробный.

```cpp
ENG_LOGI("demo", "целый масштаб: x%d", display.GetStats().scale);
```

### `int Stats::paletteColors`

Число цветов текущей палитры (`PaletteSize`); `0` для `None`. Дублирует
`PaletteSize`, но избавляет от повторного вызова.

```cpp
ENG_LOGI("demo", "палитра из %d цветов", display.GetStats().paletteColors);
```

### `int Stats::asciiCols`

Число колонок ASCII-сетки, зажатое в `1..400`.

```cpp
if (display.GetStats().usedAscii) ENG_LOGI("demo", "колонок: %d", display.GetStats().asciiCols);
```

### `int Stats::asciiRows`

Число строк ASCII-сетки: заданное (зажатое в `1..200`) либо выведенное из
пропорций при `asciiRows == 0`.

```cpp
const crossrender::RetroDisplay::Stats& st = display.GetStats();
ENG_LOGI("demo", "ASCII %dx%d", st.asciiCols, st.asciiRows);
```

### `bool Stats::usedAscii`

`true`, если последний кадр разрешался ASCII-шейдером, а не пиксельным. Это
ответ на вопрос «какой режим реально отработал», а не «что лежит в настройках».

```cpp
ENG_LOGI("demo", "режим последнего кадра: %s",
         display.GetStats().usedAscii ? "ascii" : "pixel");
```

### `const Stats& GetStats() const`

Возвращает снимок статистики по константной ссылке — без копирования. Данные
обновляются в `Resize` и `EndFrame`, поэтому между кадрами ссылка остаётся
валидной.

```cpp
const crossrender::RetroDisplay::Stats& st = display.GetStats();
ENG_LOGI("demo", "кадр %dx%d, масштаб x%d", st.virtualWidth, st.virtualHeight, st.scale);
```

### `struct RetroPreset`

Именованный готовый набор настроек для демо-сцены: циклическое переключение
пресетов меняет палитру, разрешение и эффекты, не трогая состояние движка.

```cpp
std::vector<crossrender::RetroPreset> presets = crossrender::BuiltinRetroPresets();
ENG_LOGI("demo", "доступно пресетов: %zu", presets.size());
```

### `const char* RetroPreset::name`

Имя пресета (например, `"NES"`, `"CRT Arcade"`, `"ASCII Amber"`). Строка живёт
столько же, сколько вектор из `BuiltinRetroPresets()`.

```cpp
for (const crossrender::RetroPreset& p : crossrender::BuiltinRetroPresets()) {
    ENG_LOGI("demo", "пресет: %s", p.name);
}
```

### `RetroSettings RetroPreset::settings`

Настройки пресета целиком: режим, разрешение, палитра и все эффекты. Их можно
смело копировать и донастраивать.

```cpp
std::vector<crossrender::RetroPreset> presets = crossrender::BuiltinRetroPresets();
crossrender::RetroSettings s = presets.front().settings;   // NES 256x240
s.dither = true;                                   // своя правка поверх пресета
```

### `std::vector<RetroPreset> BuiltinRetroPresets()`

Возвращает встроенные пресеты: `NES`, `Game Boy`, `Game Boy Pocket`,
`Commodore 64`, `ZX Spectrum`, `PICO-8`, `EGA 64`, `Mono Mac`, `CRT Arcade`,
`ASCII Terminal (Green)`, `ASCII Amber`, `ASCII Blocks`, `Braille Art`,
`Amstrad CPC` — всего 14 штук. Пресеты с суффиксом `ASCII` включают
`RetroMode::Ascii`, остальные — `RetroMode::Pixel`.

```cpp
std::vector<crossrender::RetroPreset> presets = crossrender::BuiltinRetroPresets();
for (crossrender::usize i = 0; i < presets.size(); ++i) {
    ENG_LOGI("demo", "[%zu] %s (mode=%d)", i, presets[i].name,
             static_cast<int>(presets[i].settings.mode));
}
```

## Пример целиком

```cpp
#include "crossrender/gfx/Retro.h"

#include "crossrender/core/Log.h"
#include "crossrender/gfx/Renderer2D.h"
#include "crossrender/text/Font.h"

#include <vector>

// Прогоняет кадр через выбранный ретро-пресет и заодно проверяет
// CPU-двойники шейдера: квантование палитры и ASCII-сетку.
void RetroShowcase(crossrender::Renderer2D& r2d, int fbW, int fbH) {
    // 1. Берём готовый пресет вместо ручной настройки десятков полей.
    const std::vector<crossrender::RetroPreset> presets = crossrender::BuiltinRetroPresets();
    crossrender::RetroSettings settings = presets[0].settings;   // "NES"
    settings.mode = crossrender::RetroMode::Pixel;

    // 2. Инициализация требует контекста OpenGL; без него просто пропускаем кадр.
    crossrender::RetroDisplay display;
    if (!display.Init()) {
        ENG_LOGW("demo", "ретро недоступно: %s", crossrender::RetroDisplay::PaletteName(settings.palette));
        return;
    }
    display.Resize(fbW, fbH, settings);

    // 3. Кадр рисуется в виртуальном разрешении, вёрстка — по логическому прямоугольнику.
    const crossrender::Rect logical = display.BeginFrame(r2d, settings, 1.0f);
    r2d.FillRect(logical, crossrender::Color::FromARGB(0xFF101820));
    r2d.DrawText(*crossrender::FontManager::Get().DefaultFont(), "RETRO", 24.0f, 24.0f,
                 crossrender::Color::White, 16.0f);

    // 4. Разрешаем изображение в текущий фреймбуфер (letterbox + resolve).
    display.EndFrame(r2d, settings, fbW, fbH);

    // 5. Статистика: какой режим отработал и с каким масштабом.
    const crossrender::RetroDisplay::Stats& st = display.GetStats();
    ENG_LOGI("demo", "%s %dx%d, x%d, %d цветов, ASCII %dx%d",
             st.usedAscii ? "ascii" : "pixel", st.virtualWidth, st.virtualHeight, st.scale,
             st.paletteColors, st.asciiCols, st.asciiRows);

    // 6. На лету переключаемся на ASCII-палитру и перестраиваем атлас.
    settings.mode = crossrender::RetroMode::Ascii;
    settings.charset = crossrender::AsciiCharset::Custom;
    settings.customRamp = " .oO@";
    const std::vector<crossrender::u32> ramp =
        crossrender::RetroDisplay::CharsetRamp(settings.charset, settings.customRamp);
    ENG_LOGI("demo", "в ramp %zu уровней", ramp.size());
    display.BuildAsciiAtlas(crossrender::FontManager::Get().DefaultFont(), 8, 16);

    // 7. CPU-путь: квантуем картинку и строим ASCII-сетку без GPU.
    std::vector<crossrender::u8> image(64 * 64 * 4, 180);
    crossrender::RetroDisplay::ApplyPalette(image.data(), 64, 64, settings);
    crossrender::RetroDisplay::AsciiGrid grid =
        crossrender::RetroDisplay::BuildAsciiGrid(image.data(), 64, 64, settings);
    if (!grid.levels.empty()) {
        ENG_LOGI("demo", "яркость центральной ячейки: %d (порог Байера %.3f)",
                 grid.levels[grid.Index(grid.cols / 2, grid.rows / 2)],
                 crossrender::RetroDisplay::BayerThreshold(1, 1, 8));
    }

    // 8. Координатные преобразования окно <-> виртуальный кадр.
    const crossrender::Vec2 v = display.MapToVirtual(crossrender::Vec2{640, 360}, fbW, fbH, settings);
    const crossrender::Vec2 f = display.MapFromVirtual(v, fbW, fbH, settings);
    ENG_LOGI("demo", "курсор (640, 360) -> виртуальные (%.1f, %.1f) -> окно (%.1f, %.1f)",
             v.x, v.y, f.x, f.y);

    display.Shutdown();
}
```

## См. также

* `docs/core/Base.md` — типы `u8`, `f32`, `usize` и `ENG_ASSERT`.
* `docs/core/Log.md` — макросы `ENG_LOGI` / `ENG_LOGW` / `ENG_LOGE`, которыми
  ретро-слой сообщает о проблемах с контекстом и атласом.
* `docs/core/Math.md` — `Color`, `Rect`, `Vec2` и `Clamp`, используемые во всех
  примерах.
* `docs/gfx/Renderer2D.md` — `BeginFrame`/`EndFrame` и `DrawText`, которыми
  рисуется виртуальный кадр и атлас ASCII.
* `docs/gfx/RenderTarget.md` — виртуальный буфер и чтение пикселей
  (`ReadPixels`), на котором построен атлас ASCII.
* `docs/gfx/Texture.md` — форматы пикселей и фильтрация `NEAREST`.
* `docs/gfx/FilterChain.md` — пост-обработка, которая в движке применяется к
  виртуальному изображению до ретро-resolve.
