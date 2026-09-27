# crossrender/gfx/FilterChain.h — стек пост-обработки из накладываемых фильтров

`FilterChain` — это отдельный, общий стек полноэкранных проходов
(full-screen passes), которые накладываются поверх HDR-разрешения движка или
любой другой текстуры. Он не имеет ничего общего с встроенным
bloom/tonemap-конвейером `PostProcessor` из `RenderTarget.h`.

## Заголовок

```cpp
#include "crossrender/gfx/FilterChain.h"
```

## Обзор

Устройство простое и намеренно «плоское»: цепочка — это упорядоченный список
`FilterInstance`, каждый из которых включает один `FilterType` и один общий блок
параметров `FilterParams`. Список можно редактировать (`Add`, `Remove`,
`MoveUp`, `MoveDown`, `Clear`) и выполнять целиком (`Apply`) или по одному
фильтру (`ApplySingle`).

#### Это не `PostProcessor`

В движке есть **две разные** системы пост-обработки, и путать их нельзя:

| | `PostProcessor` (`RenderTarget.h`) | `FilterChain` |
|---|---|---|
| назначение | встроенный HDR-конвейер сцены | произвольный стек эффектов |
| состав | bloom по мип-цепочке, tonemap, FXAA, виньетка, зерно, аберрация | 31 фильтр, включая те же эффекты в упрощённом виде |
| настройки | `PostProcessSettings`, булевы флаги | список `FilterInstance` с параметрами |
| включение | `EngineConfig::enablePostProcessing` + `Scene::WantsPostProcessing()` | `EngineConfig::enableFilters`, `Engine::Filters()` |
| где живёт | `engine/src/gfx/RenderTarget.cpp` | `engine/src/gfx/FilterChain.cpp` |

`FilterChain` применяется **после** композита сцены: у движка — к кадру
(`Engine::Step`), а при включённом ретро-режиме — к виртуальному изображению до
ретро-resolve. Обе цепочки могут работать одновременно, но `FilterChain` не
заменяет `PostProcessor` и не читает его настройки.

#### Ресурсы и порядок проходов

Цепочка владеет:

* двумя ping-pong таргетами `A` и `B` (два буфера, которые по очереди служат
  источником и приёмником; `RGBA16F`, при недоступности формата — `RGBA8`,
  фильтрация `Linear`, без глубины и стенсила);
* целевым «истории» (`history_`) — для `Feedback`; при создании он очищается в
  чёрный, чтобы первый кадр не размазывал мусор;
* отдельным scratch-таргетом для разделяемого размытия (`blurScratch`);
* тремя шейдерами: «убер-шейдер» всех фильтров (`uFilter` выбирает эффект),
  разделяемый blur-шейдер и шейдер копирования.

`Apply` выполняет список по порядку и **переключает** приёмники между `A` и `B`:

1. каждый включённый фильтр читает текстуру предыдущего шага (`current`);
2. `Blur`/`GaussianBlur` идут через `BlurPass` — два прохода разделяемого
   размытия: горизонтальный в scratch, затем вертикальный в приёмник;
3. все остальные — один проход убер-шейдера (`RunPass`) сразу в приёмник;
4. `Feedback` дополнительно читает `history` и **после себя** копирует результат
   в `history` на следующий кадр;
5. последний результат копируется в `targetFbo`, если он не совпал с
   внутренним таргетом; при пустом списке исходная текстура копируется как есть.

Соответственно `Stats::passes` считает именно проходы: разделяемое размытие
добавляет **два** прохода, `Feedback` — один проход фильтра (копия в историю не
считается).

#### Как цепочка выглядит в игре

```cpp
// Один раз при инициализации сцены.
crossrender::FilterChain* fx = engine.Filters();          // nullptr, если enableFilters == false
if (fx) {
    fx->Clear();
    fx->Add(crossrender::FilterType::Bloom, crossrender::FilterChain::Defaults(crossrender::FilterType::Bloom));
    fx->Add(crossrender::FilterType::Vignette, crossrender::FilterChain::Defaults(crossrender::FilterType::Vignette));
}
```

Дальше движок сам вызывает `Apply` каждый кадр; вручную `Apply` нужен, только
если вы обрабатываете собственную текстуру.

#### Какие поля читает каждый фильтр

`FilterParams` — один общий блок; каждый фильтр берёт только нужные поля и
игнорирует остальные. Полная карта:

| Фильтр | Читает |
|---|---|
| `Bloom` | `amount`, `radius`, `bloomThreshold`, `useThreshold` |
| `Blur` | `radius`, `sigma`, `amount` |
| `GaussianBlur` | `radius`, `sigma`, `amount` |
| `RadialBlur` | `amount` (дистанция в UV), `center` |
| `ZoomBlur` | `amount`, `center` |
| `MotionBlur` | `amount`, `direction` |
| `Sharpen` | `amount`, `radius`, `sigma` |
| `EdgeDetect` | `amount` |
| `Emboss` | `amount`, `direction` |
| `Pixelate` | `cellSize`, `amount` |
| `Posterize` | `levels`, `amount` |
| `Halftone` | `cellSize`, `amount` |
| `Dither` | `levels`, `amount` |
| `Scanlines` | `scanlineStrength`, `amount` |
| `Crt` | `center`, `curvature`, `scanlineStrength`, `aberration` |
| `Vignette` | `center`, `vignette`, `aspect` |
| `FilmGrain` | `grain`, `amount` |
| `ChromaticAberration` | `center`, `aberration`, `amount` |
| `BarrelDistort` | `amount`, `center`, `aspect` |
| `WaveDistort` | `frequency`, `amplitude`, `amount` |
| `Glitch` | `amount`; сдвиг RGB зашит в шейдер (`amount * 0.015`), поле `aberration` для него не читается |
| `Kaleidoscope` | `segments`, `angle`, `center` |
| `Fisheye` | `amount`, `center`, `aspect` |
| `Swirl` | `amount`, `angle`, `center` |
| `ColorGrade` | `lift`, `gain`, `gamma`, `saturation`, `temperature`, `tint`, `amount` |
| `HueShift` | `angle`, `amount` |
| `Invert` | `amount` |
| `Threshold` | `threshold`, `amount` |
| `Sepia` | `tint`, `amount` |
| `Bleed` | `radius`, `amount` |
| `Feedback` | `feedback`, `amount` |

#### Честные ограничения

* **`Feedback` требует таргета истории, который стартует чёрным.** Цепочка создаёт
  его сама и очищает в `{0, 0, 0, 0}`; если создание не удалось, фильтр
  деградирует: `RunPass` подставляет вместо истории сам источник, и `Feedback`
  фактически ничего не меняет. Смешивание считается как
  `f = clamp(feedback * clamp(amount, 0, 1), 0, 0.98)` — то есть примесь никогда
  не доходит до `1.0`, и изображение не «залипает» навсегда.
* **`Bloom` — однопроходное 13-тапное свечение**, а не мип-цепочка. Встроенный
  `PostProcessor` делает настоящий bloom по мипам.
* **`Blur`/`GaussianBlur` внутри убер-шейдера — только приближение (13 тапов).**
  Настоящую работу делает разделяемый blur-шейдер; если scratch-таргет или этот
  шейдер недоступны, цепочка откатывается на приближение, и результат будет
  грубее.
* **Диапазон размытия ограничен.** Шаг между тапами равен 2 пикселям (линейная
  фильтрация покрывает два текселя), число тапов —
  `clamp(ceil(radius / 2), 1, 32)`; радиус больше ~64 пикселей уже не
  увеличивает размытие.
* **`Bleed` — дешёвое смазывание в 8 направлений** с весом по близости яркости,
  а не полноценный edge-aware алгоритм.
* **Внутренние таргеты создаются лениво** под размер вызова. При смене размера
  (`EnsureTargets`) они пересоздаются, а `history` теряет накопленный кадр.
* **`Apply`/`ApplySingle`/`PreviewGrid` сбрасывают `Stats` в начале**, поэтому
  статистика описывает последний вызов, а не всю жизнь объекта.
* **`At` вне диапазона возвращает общий статический `FilterInstance`.** Это
  защищает от чтения за границей вектора, но запись в него меняет глобальную
  «заглушку» для всех: используйте `At` только для чтения.
* **`Clear()` очищает только список**, ресурсы GPU остаются занятыми до
  `Shutdown`.
* **Порядок значений `FilterType` зафиксирован** `static_assert` в
  `FilterChain.cpp`: числовые id должны совпадать с ветками `uFilter` в GLSL,
  поэтому вставлять новые фильтры в середину перечисления нельзя.
* **Нет истории ни у одного фильтра, кроме `Feedback`** (EdgeDetect, Emboss,
  Dither и остальные работают в пределах одного кадра).
* **`Apply(const RenderTarget&, ...)`** внутри снимает константность через
  `const_cast`, потому что `RenderTarget::ColorTexture()` не имеет константной
  перегрузки — это не меняет объект, но и не потокобезопасно.

## Члены класса

### `enum class FilterType : u8`

Идентификатор фильтра. Числовые значения совпадают с id в убер-шейдере
(`uFilter`), поэтому менять порядок нельзя. `Count` — служебный счётчик (31
фильтр). Как и другие большие перечисления, значения документируются **одной
таблицей** с одним примером на группу; параметры каждого фильтра описаны в
разделе `FilterParams` и в таблице «Какие поля читает каждый фильтр».

| Значение | Что делает |
|---|---|
| `FilterType::Bloom` | мягкое однопроходное свечение вокруг ярких участков |
| `FilterType::Blur` | разделяемое размытие прямоугольным ядром, радиус в пикселях |
| `FilterType::GaussianBlur` | разделяемое гауссово размытие (`radius` + `sigma`) |
| `FilterType::RadialBlur` | размытие, растущее с удалением от `center` |
| `FilterType::ZoomBlur` | радиальный «разлёт» лучей из центра |
| `FilterType::MotionBlur` | линейное размытие вдоль `direction` |
| `FilterType::Sharpen` | усиление резкости (unsharp mask) |
| `FilterType::EdgeDetect` | детектор границ Собеля |
| `FilterType::Emboss` | направленный рельеф |
| `FilterType::Pixelate` | укрупнение пикселей по сетке `cellSize` |
| `FilterType::Posterize` | квантование цвета по числу уровней `levels` |
| `FilterType::Halftone` | растровые точки по яркости |
| `FilterType::Dither` | упорядоченный дизеринг Байера 8x8 |
| `FilterType::Scanlines` | чередующиеся тёмные строки |
| `FilterType::Crt` | бочка + скан-линии + апертурная маска |
| `FilterType::Vignette` | затемнение к краям |
| `FilterType::FilmGrain` | анимированный шум яркости |
| `FilterType::ChromaticAberration` | радиальное расхождение RGB-каналов |
| `FilterType::BarrelDistort` | дисторсия объектива (подушка при `amount < 0`) |
| `FilterType::WaveDistort` | синусоидальная рябь |
| `FilterType::Glitch` | блочные сдвиги + расщепление RGB |
| `FilterType::Kaleidoscope` | зеркальные радиальные секторы |
| `FilterType::Fisheye` | «рыбий глаз» |
| `FilterType::Swirl` | закручивание вокруг центра |
| `FilterType::ColorGrade` | lift/gamma/gain + насыщенность + температура |
| `FilterType::HueShift` | поворот тона с сохранением насыщенности |
| `FilterType::Invert` | инверсия цвета |
| `FilterType::Threshold` | жёсткое чёрно-белое по порогу яркости |
| `FilterType::Sepia` | тёплый монохром |
| `FilterType::Bleed` | дешёвое смазывание с учётом краёв (приближение) |
| `FilterType::Feedback` | подмешивание предыдущего кадра (шлейфы) |
| `FilterType::Count` | служебный счётчик; фильтра с этим значением нет |

```cpp
crossrender::FilterChain chain;
chain.Add(crossrender::FilterType::Crt, crossrender::FilterChain::Defaults(crossrender::FilterType::Crt));
ENG_LOGI("demo", "%s: %s", crossrender::FilterChain::FilterName(crossrender::FilterType::Crt),
         crossrender::FilterChain::FilterDescription(crossrender::FilterType::Crt));
```

### `struct FilterParams`

Единый блок параметров для всех фильтров: каждый фильтр читает только свои поля
и игнорирует остальные (см. таблицу в разделе `Обзор`). Значения по умолчанию
подобраны «нейтрально»; для красивого результата удобнее брать
`FilterChain::Defaults(type)`.

```cpp
crossrender::FilterParams p;                   // значения по умолчанию
p.amount = 0.8f;
p.radius = 6.0f;
crossrender::FilterInstance f;
f.type = crossrender::FilterType::Blur;
f.params = p;
```

### `f32 FilterParams::amount`

Общая сила/примесь эффекта в диапазоне `0..1` (некоторые фильтры допускают
больше: Fisheye клампится в `-0.9..2.0`, Glitch — в `0..2`). У радиальных и
линейных размытий (`RadialBlur`, `ZoomBlur`, `MotionBlur`) это **дистанция в UV**,
а не доля. По умолчанию `1.0`.

```cpp
crossrender::FilterParams p;
p.amount = 0.5f;                       // половина силы эффекта
```

### `f32 FilterParams::radius`

Радиус размытия в пикселях, по умолчанию `4.0`. Используется `Blur`,
`GaussianBlur`, `Bloom`, `Sharpen` (радиус unsharp-маски) и `Bleed`.

```cpp
crossrender::FilterParams p;
p.radius = 12.0f;                      // широкое размытие
```

### `f32 FilterParams::sigma`

Стандартное отклонение гаусса в пикселях, по умолчанию `2.0`. Читается
`GaussianBlur`, а также `Bloom`/`Sharpen`, когда они используют гауссово ядро.

```cpp
crossrender::FilterParams p;
p.radius = 10.0f;
p.sigma = 5.0f;                        // мягкие края
```

### `Vec2 FilterParams::center`

Нормированный (`0..1`) центр радиального эффекта, по умолчанию `{0.5, 0.5}`.
Читают `RadialBlur`, `ZoomBlur`, `Vignette`, `ChromaticAberration`,
`BarrelDistort`, `Fisheye`, `Swirl`, `Kaleidoscope`, `Crt`.

```cpp
crossrender::FilterParams p;
p.center = crossrender::Vec2{0.5f, 0.35f};     // виньетка выше центра
```

### `Vec2 FilterParams::direction`

Нормированная ось 2D, по умолчанию `{1, 0}`. Читают `MotionBlur` и `Emboss`;
в шейдере вектор нормализуется с защитой от нуля.

```cpp
crossrender::FilterParams p;
p.direction = crossrender::Vec2{0.707f, 0.707f};   // диагональное движение
```

### `f32 FilterParams::angle`

Угол в радианах, по умолчанию `0`. Читают `HueShift` (поворот тона),
`Kaleidoscope` (поворот сектора) и `Swirl` (добавочный закрут).

```cpp
crossrender::FilterParams p;
p.angle = crossrender::Radians(90.0f);         // сдвиг тона на четверть круга
```

### `f32 FilterParams::frequency`

Частота синусоиды для `WaveDistort` — число периодов по изображению, по
умолчанию `20.0`.

```cpp
crossrender::FilterParams p;
p.frequency = 12.0f;                   // крупная волна
```

### `f32 FilterParams::amplitude`

Амплитуда искажения `WaveDistort` в единицах UV, по умолчанию `0.01`
(примерно 1% ширины кадра).

```cpp
crossrender::FilterParams p;
p.amplitude = 0.02f;                   // заметная рябь
```

### `f32 FilterParams::threshold`

Порог яркости в `0..1` для `Threshold`, по умолчанию `0.5`. Всё, что светлее,
становится белым, остальное — чёрным.

```cpp
crossrender::FilterParams p;
p.threshold = 0.35f;                   // больше белого
```

### `f32 FilterParams::levels`

Число уровней квантования (`>= 2`) для `Posterize` и `Dither`, по умолчанию
`6.0`. В шейдере зажимается снизу значением `2`.

```cpp
crossrender::FilterParams p;
p.levels = 4.0f;                       // четыре ступени яркости
```

### `int FilterParams::cellSize`

Размер ячейки/блока в пикселях (`>= 1`) для `Pixelate` и `Halftone`, по
умолчанию `6`. Для halftone внутри дополнительно зажимается снизу `2`.

```cpp
crossrender::FilterParams p;
p.cellSize = 10;                       // крупные «пиксели»
```

### `f32 FilterParams::aspect`

Дополнительный множитель пропорций для круглых искажений (`1` — использовать
собственные пропорции цели), по умолчанию `1.0`. Читают `Vignette`,
`BarrelDistort`, `Fisheye`.

```cpp
crossrender::FilterParams p;
p.aspect = 16.0f / 9.0f;               // круг не превращается в овал
```

### `Color FilterParams::tint`

Цветовой множитель (RGB), по умолчанию белый. Читают `ColorGrade` (тон
результата) и `Sepia` (оттенок сепии).

```cpp
crossrender::FilterParams p;
p.tint = crossrender::Color{0.72f, 0.95f, 1.25f, 1.0f};   // холодный подводный тон
```

### `Color FilterParams::lift`

Прибавка к цвету **до** гаммы (подъём теней), по умолчанию прозрачно-чёрный
`{0, 0, 0, 0}`. Читается только `ColorGrade`.

```cpp
crossrender::FilterParams p;
p.lift = crossrender::Color{0.02f, 0.02f, 0.03f, 0.0f};    // приподнятые тени
```

### `Color FilterParams::gain`

Множитель цвета **до** гаммы, по умолчанию белый `{1, 1, 1, 1}`. Читается
`ColorGrade`.

```cpp
crossrender::FilterParams p;
p.gain = crossrender::Color{1.1f, 1.05f, 0.95f, 1.0f};     // тёплый баланс
```

### `f32 FilterParams::gamma`

Показатель степени `ColorGrade`; применяется как `pow(c, 1/gamma)`, по
умолчанию `1.0`. Внутри зажимается снизу `0.05`.

```cpp
crossrender::FilterParams p;
p.gamma = 0.95f;                       // чуть светлее
```

### `f32 FilterParams::saturation`

Насыщенность `ColorGrade`: `1` (по умолчанию) — без изменений, `0` — оттенки
серого, больше `1` — усиление цвета.

```cpp
crossrender::FilterParams p;
p.saturation = 0.0f;                   // нуар
```

### `f32 FilterParams::temperature`

Тёплый (`+`) / холодный (`-`) сдвиг в `ColorGrade`, по умолчанию `0`. В шейдере
это прибавка `{+0.12, +0.02, -0.12} * temperature`.

```cpp
crossrender::FilterParams p;
p.temperature = 0.3f;                  // закатный оттенок
```

### `f32 FilterParams::vignette`

Сила затемнения по краям для `Vignette`, по умолчанию `0.35`; зажимается в
`0..1.5`. `Crt` использует собственное встроенное затемнение.

```cpp
crossrender::FilterParams p;
p.vignette = 0.6f;                     // тяжёлая виньетка
```

### `f32 FilterParams::grain`

Амплитуда шума `FilmGrain`, по умолчанию `0.04`. Шум анимирован: в хэш
подмешивается текущее время, поэтому кадры не повторяются.

```cpp
crossrender::FilterParams p;
p.grain = 0.08f;                       // зернистая плёнка
```

### `f32 FilterParams::scanlineStrength`

Сила затемнения строк для `Scanlines` и `Crt`, по умолчанию `0.3`; зажимается
в `0..1`.

```cpp
crossrender::FilterParams p;
p.scanlineStrength = 0.4f;
```

### `f32 FilterParams::curvature`

Кривизна «трубки» для `Crt`, по умолчанию `0.06`. UV смещаются на
`cc * dot(cc, cc) * curvature * 4.0`; за пределами изображения получается чёрный
цвет.

```cpp
crossrender::FilterParams p;
p.curvature = 0.1f;                    // сильно выпуклый экран
```

### `f32 FilterParams::aberration`

Расщепление RGB в единицах UV для `ChromaticAberration` и `Crt`, по умолчанию
`0.003`. Для `Glitch` поле фактически не читается: там сдвиг зашит как
`amount * 0.015`, хотя комментарий в заголовке и упоминает Glitch.

```cpp
crossrender::FilterParams p;
p.aberration = 0.006f;                 // заметная цветная кайма
```

### `f32 FilterParams::bloomThreshold`

Порог яркости для `Bloom`, по умолчанию `1.0`. Действует только при
`useThreshold == true`; иначе в свечение попадает любой ненулевой пиксель.

```cpp
crossrender::FilterParams p;
p.bloomThreshold = 0.7f;
p.useThreshold = true;
```

### `bool FilterParams::useThreshold`

`false` (по умолчанию) — в `Bloom` вносит вклад каждый ненулевой пиксель;
`true` — только то, что ярче `bloomThreshold`. Порог отсекается относительно
яркости (`brightPass`).

```cpp
crossrender::FilterParams p;
p.useThreshold = true;                 // светятся только яркие блики
```

### `f32 FilterParams::feedback`

Доля предыдущего кадра, подмешиваемая `Feedback`, по умолчанию `0.85`. Итоговый
коэффициент — `clamp(feedback * clamp(amount, 0, 1), 0, 0.98)`, поэтому шлейф
никогда не становится бесконечным.

```cpp
crossrender::FilterParams p;
p.feedback = 0.9f;                     // длинный шлейф
p.amount = 1.0f;
```

### `int FilterParams::segments`

Число зеркальных секторов `Kaleidoscope` (`>= 3`), по умолчанию `6`. В шейдере
зажимается снизу значением `3`.

```cpp
crossrender::FilterParams p;
p.segments = 8;                        // восемь «лепестков»
```

### `struct FilterInstance`

Один элемент цепочки: тип фильтра, флаг включения и его параметры. Именно эти
структуры хранит `FilterChain::Filters()`.

```cpp
crossrender::FilterInstance f;
f.type = crossrender::FilterType::Vignette;
f.enabled = true;
f.params = crossrender::FilterChain::Defaults(crossrender::FilterType::Vignette);
```

### `FilterType FilterInstance::type`

Тип фильтра. По умолчанию `FilterType::Blur`. Значение превращается в
`uniform int uFilter` и должно совпадать с ветками GLSL.

```cpp
crossrender::FilterInstance f;
f.type = crossrender::FilterType::HueShift;
ENG_LOGI("demo", "фильтр: %s", crossrender::FilterChain::FilterName(f.type));
```

### `bool FilterInstance::enabled`

Выключенный фильтр пропускается в `Apply`/`ApplySingle`, но остаётся в списке —
удобно для «галочек» в редакторе. По умолчанию `true`.

```cpp
crossrender::FilterInstance f;
f.type = crossrender::FilterType::FilmGrain;
f.enabled = false;                     // оставляем в списке, но не применяем
```

### `FilterParams FilterInstance::params`

Параметры этого фильтра. Копия `FilterParams` на каждый элемент, поэтому
настройки соседних фильтров независимы.

```cpp
crossrender::FilterInstance f;
f.type = crossrender::FilterType::Sharpen;
f.params.amount = 1.2f;
f.params.radius = 2.0f;
```

### `FilterChain()`

Создаёт пустую цепочку без ресурсов: список фильтров пуст, GL-объекты ещё не
созданы, `Valid() == false`. Инициализация произойдёт в `Init()` (или лениво при
первом `Apply`).

```cpp
crossrender::FilterChain chain;
ENG_ASSERT(chain.Count() == 0);
```

### `~FilterChain()`

Вызывает `Shutdown()`: уничтожает ping-pong таргеты, историю, scratch и
шейдеры. Деструктор не бросающий и безопасен для контейнеров.

```cpp
{
    crossrender::FilterChain local;
    local.Init();
}   // ресурсы освобождены
```

### `FilterChain(const FilterChain&) = delete`

Копирование запрещено: объект владеет GL-ресурсами через `unique_ptr`.
Копия привела бы к двойному освобождению.

```cpp
void ApplyFx(crossrender::FilterChain& chain, const crossrender::Texture& src) {
    chain.Apply(src, 0, 1280, 720);    // по ссылке
}
```

### `FilterChain(FilterChain&&) noexcept`

Перемещающий конструктор: переносит список, оба ping-pong таргета, историю,
шейдеры и статистику. Перемещённый объект остаётся с `width_ == 0`,
`initialized_ == false` и пустой статистикой. Нужен для возврата из
`MakePreset()` по значению.

```cpp
crossrender::FilterChain chain = crossrender::FilterChain::MakePreset("Noir");
ENG_LOGI("demo", "в пресете %d фильтров", chain.Count());
```

### `FilterChain& operator=(FilterChain&&) noexcept`

Перемещающее присваивание: сначала `Shutdown()` собственных ресурсов, затем
перенос ресурсов `o`. Самоприсваивание (`c = std::move(c)`) безопасно.

```cpp
crossrender::FilterChain current;
current = crossrender::FilterChain::MakePreset("Pixel");
ENG_LOGI("demo", "пресет применён, фильтров %d", current.Count());
```

### `bool Init()`

Собирает шейдеры (`filter-chain`, `filter-chain-blur`,
`filter-chain-copy`) и создаёт полноэкранный quad. Идемпотентен по смыслу: без
контекста OpenGL пишет `ENG_LOGW("filter", "no GL context; FilterChain stays in
copy-through mode")` и возвращает `false`, но объект остаётся рабочим — `Apply`
в этом случае просто копирует источник в цель.

* **Возвращает:** `true`, если убер-шейдер собран и цепочка готова.
* **Контекст:** требует текущего контекста OpenGL.

```cpp
crossrender::FilterChain chain;
if (!chain.Init()) {
    ENG_LOGW("demo", "фильтры недоступны — кадр пройдёт насквозь");
}
```

### `void Shutdown()`

Освобождает все внутренние ресурсы: ping-pong таргеты, историю, scratch,
шейдеры; сбрасывает размеры, флаг инициализации и статистику. Список фильтров
при этом **не** очищается — после повторного `Init` цепочка снова готова.

```cpp
crossrender::FilterChain chain;
chain.Add(crossrender::FilterType::Bloom);
chain.Init();
chain.Shutdown();
ENG_ASSERT(!chain.Valid());
```

### `bool Valid() const`

`true`, если `Init()` прошёл и убер-шейдер валиден. При `false` вызовы `Apply`
деградируют до простого копирования, а не падают.

```cpp
if (chain.Valid()) ENG_LOGI("demo", "стек фильтров готов");
```

### `void Clear()`

Очищает список фильтров. Ресурсы GPU (таргеты, история, шейдеры) остаются
занятыми до `Shutdown` — это осознанно, чтобы `Clear` + `Add` в игровом цикле не
пересоздавал текстуры.

```cpp
chain.Clear();
chain.Add(crossrender::FilterType::Crt, crossrender::FilterChain::Defaults(crossrender::FilterType::Crt));
ENG_LOGI("demo", "в цепочке %d фильтр", chain.Count());
```

### `void Add(FilterType type, const FilterParams& params = {}, bool enabled = true)`

Добавляет фильтр в конец списка. Параметры копируются, поэтому временный
`FilterParams` можно не сохранять. По умолчанию фильтр включён.

```cpp
crossrender::FilterParams p = crossrender::FilterChain::Defaults(crossrender::FilterType::Bloom);
p.amount = 0.6f;
chain.Add(crossrender::FilterType::Bloom, p, /*enabled=*/true);
```

### `void Add(FilterInstance f)`

Добавляет уже готовый элемент цепочки (перемещением). Удобно, когда фильтр
собирается отдельно или приходит из редактора.

```cpp
crossrender::FilterInstance inst;
inst.type = crossrender::FilterType::Kaleidoscope;
inst.params.segments = 8;
chain.Add(std::move(inst));
ENG_LOGI("demo", "фильтров в цепочке: %d", chain.Count());
```

### `void Remove(int index)`

Удаляет фильтр по индексу. Индекс вне диапазона — тихий no-op, без исключений и
падений.

```cpp
chain.Remove(0);                       // убрать первый фильтр
ENG_LOGI("demo", "осталось %d", chain.Count());
```

### `void MoveUp(int index)`

Меняет фильтр местами с предыдущим — порядок применения важен (например,
`Bloom` до `Crt` выглядит иначе, чем после). Для `index <= 0` или индекса за
границей ничего не делает.

```cpp
chain.MoveUp(2);                       // поднять третий фильтр выше
```

### `void MoveDown(int index)`

Меняет фильтр местами со следующим. Для последнего индекса или выхода за
диапазон — тихий no-op.

```cpp
chain.MoveDown(0);                     // опустить первый фильтр ниже
```

### `int Count() const`

Число фильтров в списке, включая выключенные. Именно оно, а не `Filters().size()`,
обычно нужно интерфейсу.

```cpp
ENG_LOGI("demo", "в стеке %d фильтров", chain.Count());
```

### `FilterInstance& At(int index)`

Доступ к фильтру по индексу для чтения и записи. Если индекс вне диапазона,
возвращается **общий статический** `FilterInstance` — так исключается чтение за
границей вектора, но запись в него затронет глобальную заглушку, поэтому
используйте `At` только для валидных индексов. Константная перегрузка
возвращает `const FilterInstance&` и ничего не меняет.

```cpp
if (chain.Count() > 0) {
    crossrender::FilterInstance& f = chain.At(0);
    f.params.amount = 0.5f;            // правим первый фильтр
}
```

### `const std::vector<FilterInstance>& Filters() const`

Прямой доступ ко всему списку — для отрисовки редактора или сериализации.
Ссылка живёт, пока список не изменили.

```cpp
for (const crossrender::FilterInstance& f : chain.Filters()) {
    ENG_LOGI("demo", "%s enabled=%d", crossrender::FilterChain::FilterName(f.type),
             f.enabled ? 1 : 0);
}
```

### `void Apply(const Texture& source, unsigned int targetFbo, int width, int height)`

Применяет **все включённые** фильтры к текстуре `source` и пишет результат в
`targetFbo` (`0` — стандартный фреймбуфер). Внутри: сброс статистики, ленивый
`Init`, проверка валидности источника, ленивое создание таргетов под размер,
проход по списку с переключением `A`/`B`, `Feedback` с историей, финальное
копирование в `targetFbo`. Если фильтров нет (или шейдеры недоступны), источник
копируется как есть — кадр никогда не остаётся чёрным.

* **Параметры:** `width`/`height` — размер обработки; внутренние таргеты
  создаются ровно под него.

```cpp
chain.Add(crossrender::FilterType::Bloom, crossrender::FilterChain::Defaults(crossrender::FilterType::Bloom));
chain.Add(crossrender::FilterType::Vignette, crossrender::FilterChain::Defaults(crossrender::FilterType::Vignette));
chain.Apply(sceneTexture, 0, 1280, 720);
ENG_LOGI("demo", "проходов: %d", chain.GetStats().passes);
```

### `void Apply(const RenderTarget& source, unsigned int targetFbo = 0)`

То же самое, но источник — `RenderTarget`: берётся его цветовая текстура, а
размер (`Width()`/`Height()`) подставляется автоматически. Внутри снимается
константность источника, потому что `ColorTexture()` в движке не имеет
константной перегрузки.

```cpp
crossrender::RenderTargetDesc desc;
desc.width = 1280;
desc.height = 720;
desc.colorFormat = crossrender::PixelFormat::RGBA8;
crossrender::RenderTarget target;
target.Create(desc);
chain.Apply(target);                   // в стандартный фреймбуфер
chain.Apply(target, target.Fbo());     // обратно в тот же таргет
```

### `void ApplySingle(const Texture& source, const FilterInstance& filter, unsigned int targetFbo, int width, int height)`

Прогоняет **один** фильтр, не трогая список цепочки. Результат всегда считается
во внутренний таргет `A`, а затем копируется в `targetFbo`, если тот отличается:
это позволяет `Feedback` сохранить снимок в историю, даже когда целью служит
стандартный фреймбуфер. Используется демо-сеткой предпросмотра и редакторами.

```cpp
crossrender::FilterInstance preview;
preview.type = crossrender::FilterType::Glitch;
preview.params = crossrender::FilterChain::Defaults(crossrender::FilterType::Glitch);
chain.ApplySingle(sceneTexture, preview, 0, 320, 180);
ENG_LOGI("demo", "проходов: %d", chain.GetStats().passes);
```

### `void PreviewGrid(const Texture& source, const std::vector<FilterType>& types, int columns, int width, int height)`

Рисует сетку предпросмотра: каждый фильтр из `types` применяется к `source` со
своими `Defaults(type)` и выводится в свою ячейку текущего таргета. Сетка
разбивается на `columns` колонок, ячейка — `width/columns` на `height/rows`.
Перед отрисовкой ячейки фон заливается цветом `{0.06, 0.06, 0.08}`, чтобы
прозрачные углы читались как сетка. Предыдущие framebuffer и viewport
сохраняются и восстанавливаются в конце. `Stats` агрегируется по всем ячейкам.

```cpp
const std::vector<crossrender::FilterType> types = {
    crossrender::FilterType::Bloom, crossrender::FilterType::Crt,
    crossrender::FilterType::Glitch, crossrender::FilterType::Pixelate};
chain.PreviewGrid(sceneTexture, types, /*columns=*/2, 640, 360);
ENG_LOGI("demo", "сетка из %zu ячеек, %d проходов", types.size(),
         chain.GetStats().passes);
```

### `static const char* FilterName(FilterType t)`

Человекочитаемое имя фильтра (`"GaussianBlur"`, `"Feedback"`, …). Для значения
вне таблицы возвращает `"Unknown"`. Строка статическая — копировать не нужно.

```cpp
ENG_LOGI("demo", "фильтр называется %s", crossrender::FilterChain::FilterName(crossrender::FilterType::Bleed));
```

### `static const char* FilterDescription(FilterType t)`

Короткое описание эффекта по-английски, как в таблице метаданных (например,
`"Mixes the previous frame back in (echo/trails)"`). Для неизвестного значения —
`"Unknown filter"`.

```cpp
ENG_LOGI("demo", "%s", crossrender::FilterChain::FilterDescription(crossrender::FilterType::Feedback));
```

### `static std::vector<FilterType> AllFilters()`

Возвращает все 31 фильтр в **стабильном порядке перечисления** (без `Count`).
Удобно для построения меню «добавить фильтр».

```cpp
for (crossrender::FilterType t : crossrender::FilterChain::AllFilters()) {
    ENG_LOGI("demo", "доступен: %s", crossrender::FilterChain::FilterName(t));
}
```

### `static FilterParams Defaults(FilterType t)`

Готовый набор параметров, при котором фильтр сразу выглядит осмысленно
(например, для `Crt`: `curvature = 0.08`, `scanlineStrength = 0.35`,
`aberration = 0.0035`). Для `Count` и неизвестных значений возвращаются
значения по умолчанию `FilterParams`.

```cpp
crossrender::FilterParams p = crossrender::FilterChain::Defaults(crossrender::FilterType::Halftone);
p.cellSize = 5;
chain.Add(crossrender::FilterType::Halftone, p);
```

### `static FilterChain MakePreset(const std::string& name)`

Собирает готовую цепочку по имени: `Clean` (пустая, сквозная), `Dreamy`, `CRT`,
`Glitch`, `Painterly`, `Noir`, `Pixel`, `Underwater`, `Kaleido`, `Dream`.
Неизвестное имя логирует `ENG_LOGW("filter", "unknown filter preset ...")` и
возвращает пустую (сквозную) цепочку. Возврат по значению возможен благодаря
перемещающему конструктору.

```cpp
crossrender::FilterChain noir = crossrender::FilterChain::MakePreset("Noir");
ENG_LOGI("demo", "Noir: %d фильтров", noir.Count());
```

### `static std::vector<std::string> PresetNames()`

Список имён всех встроенных пресетов в порядке определения: `Clean`, `Dreamy`,
`CRT`, `Glitch`, `Painterly`, `Noir`, `Pixel`, `Underwater`, `Kaleido`, `Dream`.

```cpp
for (const std::string& name : crossrender::FilterChain::PresetNames()) {
    crossrender::FilterChain c = crossrender::FilterChain::MakePreset(name);
    ENG_LOGI("demo", "%-12s -> %d", name.c_str(), c.Count());
}
```

### `struct FilterChain::Stats`

Статистика последнего выполнения: сколько проходов сделано, сколько раз
переключались ping-pong таргеты и использовался ли `Feedback`. Сбрасывается в
начале `Apply`, `ApplySingle` и `PreviewGrid`.

```cpp
crossrender::FilterChain::Stats s;
ENG_ASSERT(s.passes == 0 && !s.usedFeedback);
```

### `int Stats::passes`

Число выполненных проходов. `Blur`/`GaussianBlur` дают по два
(горизонтальный + вертикальный), остальные — по одному.

```cpp
ENG_LOGI("demo", "проходов за кадр: %d", chain.GetStats().passes);
```

### `int Stats::pingPong`

Сколько раз приёмник переключался между таргетами `A` и `B`. Помогает оценить
цену цепочки: каждое переключение — это смена framebuffer.

```cpp
ENG_LOGI("demo", "переключений таргета: %d", chain.GetStats().pingPong);
```

### `bool Stats::usedFeedback`

`true`, если в последнем выполнении участвовал `Feedback` и у цепочки есть
валидная история. Если таргет истории создать не удалось, флаг всё равно
выставляется при попытке, но эффекта не будет.

```cpp
if (chain.GetStats().usedFeedback) ENG_LOGI("demo", "в кадре есть шлейф");
```

### `const Stats& GetStats() const`

Возвращает статистику по константной ссылке — без копирования. Данные описывают
последний вызов `Apply`/`ApplySingle`/`PreviewGrid`.

```cpp
const crossrender::FilterChain::Stats& s = chain.GetStats();
ENG_LOGI("demo", "%d проходов, ping-pong %d, feedback %d", s.passes, s.pingPong,
         s.usedFeedback ? 1 : 0);
```

## Пример целиком

```cpp
#include "crossrender/gfx/FilterChain.h"

#include "crossrender/core/Log.h"
#include "crossrender/gfx/RenderTarget.h"
#include "crossrender/gfx/Texture.h"

#include <string>
#include <utility>
#include <vector>

// Собирает стек фильтров из пресета, донастраивает его и прогоняет кадр.
void PostProcessFrame(crossrender::RenderTarget& sceneTarget, unsigned int outputFbo) {
    // 1. Готовая цепочка «Noir»: ColorGrade -> FilmGrain -> Vignette.
    crossrender::FilterChain chain = crossrender::FilterChain::MakePreset("Noir");
    if (!chain.Init()) {
        ENG_LOGW("demo", "шейдеры не собрались: кадр просто скопируется");
    }
    ENG_LOGI("demo", "после Init в цепочке %d фильтров", chain.Count());

    // 2. Правим существующий фильтр и добавляем свой.
    if (chain.Count() > 0) {
        chain.At(0).params.saturation = 0.1f;      // почти чёрно-белый
    }
    crossrender::FilterInstance grain;
    grain.type = crossrender::FilterType::FilmGrain;
    grain.enabled = true;
    grain.params = crossrender::FilterChain::Defaults(crossrender::FilterType::FilmGrain);
    grain.params.grain = 0.07f;
    chain.Add(std::move(grain));

    // 3. Порядок важен: переносим виньетку в самый конец.
    chain.MoveUp(chain.Count() - 1);

    // 4. Применяем весь стек к цветовой текстуре сцены.
    chain.Apply(sceneTarget, outputFbo);
    const crossrender::FilterChain::Stats& s = chain.GetStats();
    ENG_LOGI("demo", "%d проходов, ping-pong %d, feedback %d", s.passes, s.pingPong,
             s.usedFeedback ? 1 : 0);

    // 5. Одиночный фильтр — для предпросмотра в редакторе.
    crossrender::FilterInstance preview;
    preview.type = crossrender::FilterType::Crt;
    preview.params = crossrender::FilterChain::Defaults(crossrender::FilterType::Crt);
    preview.params.curvature = 0.12f;
    chain.ApplySingle(sceneTarget.ColorTexture(), preview, outputFbo, 320, 180);

    // 6. Feedback требует историю: цепочка держит её сама и стартует с чёрного.
    crossrender::FilterChain trails = crossrender::FilterChain::MakePreset("Dream");
    trails.Init();
    for (int frame = 0; frame < 3; ++frame) {
        trails.Apply(sceneTarget, outputFbo);
        ENG_LOGI("demo", "кадр %d: шлейф %s", frame,
                 trails.GetStats().usedFeedback ? "да" : "нет");
    }

    // 7. Сетка предпросмотра: несколько фильтров в одном кадре.
    const std::vector<crossrender::FilterType> grid = {
        crossrender::FilterType::Bloom, crossrender::FilterType::EdgeDetect, crossrender::FilterType::Pixelate,
        crossrender::FilterType::Swirl};
    trails.PreviewGrid(sceneTarget.ColorTexture(), grid, /*columns=*/2, 640, 360);

    // 8. Инвентарь: имена, описания и число доступных фильтров.
    ENG_LOGI("demo", "всего фильтров: %zu", crossrender::FilterChain::AllFilters().size());
    for (const std::string& name : crossrender::FilterChain::PresetNames()) {
        crossrender::FilterChain preset = crossrender::FilterChain::MakePreset(name);
        ENG_LOGI("demo", "пресет %-12s -> %d фильтров", name.c_str(), preset.Count());
    }

    chain.Clear();
    chain.Shutdown();
}
```

## См. также

* `docs/gfx/RenderTarget.md` — `PostProcessor` (`PostProcessSettings`) и
  `RenderTarget`, на котором построена цепочка; там же видно, чем встроенный
  bloom/tonemap отличается от `FilterChain`.
* `docs/gfx/Texture.md` — форматы `RGBA16F`/`RGBA8` и фильтрация, используемые
  внутренними таргетами.
* `docs/gfx/Shader.md` — `Shader::Set`, `SetTexture` и `Build`, которыми
  пользуются проходы.
* `docs/gfx/GL.md` — низкоуровневые вызовы (`glBindFramebuffer`, `glViewport`,
  `glScissor`), которые делает `PreviewGrid`.
* `docs/gfx/Retro.md` — ретро-режим: в движке цепочка применяется к виртуальному
  изображению до ретро-resolve.
* `docs/core/Time.md` — `NowSeconds`, от которого зависят анимированные эффекты
  (`FilmGrain`, `Glitch`, `WaveDistort`).
* `docs/core/Log.md` — `ENG_LOGW`/`ENG_LOGE` о недоступных шейдерах и
  неизвестных пресетах.
