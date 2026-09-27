# crossrender/gfx/SlugText.h — векторный текст прямо из контуров глифов

Аналитический рендеринг текста в стиле Slug: глифы хранятся как квадратичные
кривые Безье (quadratic Bezier curves) и попадают в data-текстуры, а фрагментный
шейдер считает точное покрытие пикселя лучом по кривым. Атлас глифов не
используется вообще.

## Заголовок

```cpp
#include "crossrender/gfx/SlugText.h"
```

## Обзор

Это оригинальная реализация метода Эрика Лендьела «GPU-Centered Font Rendering
Directly from Glyph Outlines» (JCGT 2017) — той же техники, что лежит в основе
библиотеки Slug. Отличие от SDF- и bitmap-путей движка принципиальное:

* **никакого атласа**: глиф не растеризуется ни в текстуру, ни в битовую карту;
* контуры шрифта (`Font::GetGlyphOutline`) разбираются на квадратичные кривые в
  единицах em (кегельная площадка), кладутся в две float-текстуры и
  восстанавливаются в шейдере;
* кубические сегменты CFF/OTF конвертируются в квадратичные **на CPU**, потому
  что шейдер умеет решать только квадратные уравнения;
* покрытие пикселя считается аналитически: из пикселя вправо пускается луч,
  квадратное уравнение даёт точки пересечения, из знака и расстояния до края
  получается `coverage`.

Поэтому `SlugTextRenderer` — это **альтернатива** обычному текстовому пути
(`Renderer2D::DrawText` по bitmap/SDF-атласу), а не его замена: он даёт точные
кривые на любом кегле и повороте, но требует валидного контекста OpenGL и
поддерживает меньше возможностей оформления.

#### Когда использовать

* крупный заголовочный текст, логотипы, текст на кривой/дуге, где видны
  ступеньки атласа;
* текст с произвольным поворотом и масштабом, для которого не хочется печь
  отдельный атлас;
* тесты и инструменты, которым нужно покрытие глифа на CPU — для этого есть
  `CoverageAt` и `WindingAt`, работающие без GL.

Не стоит использовать его для длинных абзацев мелкого текста: батчинг
ограничен, и каждый новый кегль — это заново разобранные контуры.

#### Как устроены данные на GPU

Кривые и полосы (bands) держатся в двух двумерных текстурах формата `RGBA32F`
шириной 4 текселя:

* **Текстура кривых.** Одна кривая занимает **одну строку** (два текселя), чтобы
  не пересекать границу строки:
  `texel (0 + 2*(i%2), i/2) = (p0.x, p0.y, p1.x, p1.y)`,
  `texel (1 + 2*(i%2), i/2) = (p2.x, p2.y, bandIndex, flags)`.
* **Текстура полос.** Одна полоса — тоже одна строка:
  `texel (0 + 2*(b%2), b/2) = (firstCurve, curveCount, 0, 0)`.
  Полосы глифа лежат в общем массиве подряд, поэтому `SlugGlyph::bandOffsetY`
  хранит **абсолютный индекс первой полосы**, а `bandOffsetX` не используется:
  шейдер сам выводит и колонку, и строку из этого индекса.

Формат именно `RGBA32F`, а не более очевидный `R32F`: текстура `R32F`, созданная
через `Texture::Create` (`GL_R32F` + `GL_RED` + `GL_FLOAT`), корректно
загружается, но на части драйверов GL 3.2 **сэмплируется как ноль**, что молча
отключает всю таблицу полос. Оба массива используют один проверенный формат;
числа в них — небольшие точные целые (`< 2^24`), а шейдер переводит их через
`int(v + 0.5)`, так что целочисленный формат текстуры не нужен.

#### Полосы и границы цикла

Шейдер обязан иметь **постоянную** границу цикла (требование GLES3/WebGL2),
поэтому перебор кривых в полосе идёт до `SLUG_MAX_CURVES_PER_BAND == 128` с
досрочным `break`. `PrepareGlyph` разбивает глиф на 8…16 горизонтальных полос и
для каждой хранит непрерывный диапазон кривых, которые её пересекают. Если
диапазон превышает 128, полоса передаётся соседней (её диапазон расширяется на
оба интервала по `y`), а сама остаётся пустой — так константная граница не
нарушается и кривые не теряются. Оставшийся перебор (например, у самой верхней
полосы нет соседа) обрезается до 128 с предупреждением в лог.

#### Честные ограничения

* **Батчинг — не более 8 глифов на один розыгрыш (draw).** Глифы с одинаковыми
  `rotation` и `scale` собираются в один instanced-вызов через uniform-массивы
  (`glDrawArraysInstanced`); прогон с посимвольным поворотом (`twist`,
  `DrawOnPath`, `DrawOnArc`) деградирует до **одного розыгрыша на глиф**. Slug
  упаковывает весь прогон в горсть вершинных записей — здесь так не сделано.
* **Тень — не размытие, а кольцо дрожащих копий.** При ненулевом
  `shadowOffset` глиф рисуется 1…5 раз: копии смещаются по золотому углу
  (`2.39996323 * t`) на радиус, пропорциональный `softness`, и делят между собой
  альфу `shadowColor`. Это дешёвое приближение blur, а не гаусс.
* **`SlugTextStyle::billboard` игнорируется.** Поле описывает 3D-билборд-текст и
  не имеет смысла для экранного 2D-прогона; реализация к нему не обращается.
* **Нет depth pre-pass и нет ключа порядка кривых.** Перекрывающиеся глифы
  рисуются в порядке строки обычным «алгоритмом художника»; полноценного
  depth-прохода, как в Slug, нет.
* **Градиента нет.** В шейдере есть ветка `uUseGradient`, и она включается для
  любого цвета кроме белого, но `uInnerColor` и `uOuterColor` выставляются в
  один и тот же `style.color`, а полей для двух концов градиента в
  `SlugTextStyle` нет — видимого градиента не будет.
* **`DrawOnArc` всегда идёт против часовой стрелки** (в сторону роста угла):
  флага `clockwise` в контракте нет, сторону выбирает только `outside`.
* **Бюджеты данных.** Начальные ёмкости — 65536 кривых и 16384 полосы; они
  зажимаются значением `GL_MAX_TEXTURE_SIZE`. Когда бюджет исчерпан, поздние
  глифы пропускаются с однократным предупреждением.
* **`BandCount()` возвращает размер плоского массива пар** `(firstCurve,
  curveCount)`, то есть **удвоенное** число полос. `CurveCount()` — число
  разобранных кривых, `GlyphCount()` — число подготовленных глифов.
* **`Draw` без GL ничего не рисует.** Если контекста нет, `Init` возвращает
  `false`, `Valid() == false`, но `PrepareGlyph`, `Measure`, `GetGlyph`,
  `CoverageAt` и `WindingAt` продолжают работать — это CPU-путь для тестов.
* **`Stats::curvesEvaluated`** считает кривые, ставшие доступными шейдеру для
  нарисованных глифов, а не фактическое число вычислений на фрагмент: последнее
  знает только GPU.

## Члены класса

### `struct SlugCurve`

Один квадратичный сегмент Безье в пространстве глифа: единицы em, `+y` вниз,
начало координат — перо на базовой линии. Прямые отрезки кодируются
вырожденной квадратикой (`p0 == p1`).

```cpp
crossrender::SlugCurve c;
c.p0 = crossrender::Vec2{0.0f, 0.0f};
c.p1 = crossrender::Vec2{0.5f, 1.0f};
c.p2 = crossrender::Vec2{1.0f, 0.0f};
ENG_LOGI("demo", "кривая: (%.2f %.2f) -> (%.2f %.2f)", c.p0.x, c.p0.y, c.p2.x, c.p2.y);
```

### `Vec2 SlugCurve::p0`

Начальная точка сегмента (на кривой). Для замкнутого контура конец предыдущего
сегмента совпадает с ней.

```cpp
crossrender::SlugCurve c;
c.p0 = crossrender::Vec2{0.1f, 0.2f};
ENG_LOGI("demo", "старт: %.2f %.2f", c.p0.x, c.p0.y);
```

### `Vec2 SlugCurve::p1`

Управляющая точка (control point) — в общем случае **не** лежит на кривой.
Именно она задаёт кривизну квадратики.

```cpp
crossrender::SlugCurve c;
c.p0 = crossrender::Vec2{0, 0};
c.p1 = crossrender::Vec2{0.5f, 0.8f};          // «горб» вверх
c.p2 = crossrender::Vec2{1, 0};
```

### `Vec2 SlugCurve::p2`

Конечная точка сегмента; для замкнутого контура она же — начало следующего
сегмента, поэтому при подсчёте пересечений используется полуоткрытый интервал
`[0, 1)`.

```cpp
crossrender::SlugCurve c;
c.p2 = crossrender::Vec2{1.0f, 0.0f};
ENG_LOGI("demo", "финиш: %.2f %.2f", c.p2.x, c.p2.y);
```

### `struct SlugGlyph`

Глиф, подготовленный к Slug-рендерингу: метрики, границы и ссылки на диапазоны
кривых и полос в общих массивах. Заполняется `PrepareGlyph` и кэшируется; для
пробела и прочих пустых глифов выставляется `empty = true`.

```cpp
crossrender::SlugGlyph g;
ENG_ASSERT(g.empty == true);           // ещё ничего не подготовлено
```

### `u32 SlugGlyph::codepoint`

Кодпойнт Unicode, которому принадлежит глиф. Служит ключом кэша
`SlugTextRenderer::GetGlyph`.

```cpp
crossrender::SlugGlyph g;
g.codepoint = 'A';
ENG_LOGI("demo", "глиф U+%04X", g.codepoint);
```

### `f32 SlugGlyph::advance`

Горизонтальное продвижение пера в единицах em (`outline.advance /
emResolution`). Используется и в `Measure`, и в раскладке `Draw`.

```cpp
crossrender::SlugGlyph g;
g.advance = 0.6f;
ENG_LOGI("demo", "перо уйдёт на %.3f em", g.advance);
```

### `Rect SlugGlyph::bounds`

Плотные границы чернил (ink bounds) в единицах em, `+y` вниз. По ним
строится расширенный четырёхугольник глифа и локальная шкала градиента.

```cpp
crossrender::SlugGlyph g;
g.bounds = crossrender::Rect{0.05f, 0.0f, 0.5f, 1.0f};
ENG_LOGI("demo", "чернила %.2fx%.2f", g.bounds.w, g.bounds.h);
```

### `int SlugGlyph::firstCurve`

Индекс первой кривой глифа в общей текстуре кривых.

```cpp
crossrender::SlugGlyph g;
g.firstCurve = 12;
ENG_LOGI("demo", "кривые с индекса %d", g.firstCurve);
```

### `int SlugGlyph::curveCount`

Число кривых глифа. Для пустого глифа — `0`.

```cpp
crossrender::SlugGlyph g;
g.curveCount = 24;
ENG_LOGI("demo", "в глифе %d кривых", g.curveCount);
```

### `int SlugGlyph::firstBand`

Индекс первой полосы глифа в общем массиве полос (в терминах полос, не
текселей).

```cpp
crossrender::SlugGlyph g;
g.firstBand = 4;
ENG_LOGI("demo", "первая полоса: %d", g.firstBand);
```

### `int SlugGlyph::bandCount`

Число горизонтальных полос глифа; всегда в диапазоне 8…16 для непустого глифа.

```cpp
crossrender::SlugGlyph g;
g.bandCount = 8;
const float bandHeight = g.bounds.h / static_cast<float>(g.bandCount);
ENG_LOGI("demo", "высота полосы: %.4f em", bandHeight);
```

### `u32 SlugGlyph::bandOffsetX`

**Не используется.** Зарезервирован под прямоугольный блок полос; текущая
реализация размещает полосы подряд и вычисляет тексель из абсолютного индекса,
поэтому `PrepareGlyph` всегда пишет сюда `0`. Поле оставлено для совместимости
контракта.

```cpp
crossrender::SlugGlyph g;
ENG_LOGI("demo", "bandOffsetX всегда %u", g.bandOffsetX);
```

### `u32 SlugGlyph::bandOffsetY`

Абсолютный индекс первой полосы глифа в упакованной текстуре полос (не номер
строки текстуры). Именно по нему шейдер находит диапазон кривых для фрагмента.

```cpp
crossrender::SlugGlyph g;
g.bandOffsetY = 16;
ENG_LOGI("demo", "полосы начинаются с абсолютного индекса %u", g.bandOffsetY);
```

### `bool SlugGlyph::empty`

`true` для глифа без контуров: пробел, перевод строки, а также глиф, который не
удалось разобрать. Пустые глифы всё равно регистрируются в кэше, чтобы `Draw`
мог продвинуть перо без повторного обращения к шрифту.

```cpp
crossrender::SlugGlyph g;
g.empty = true;                        // например, пробел
ENG_ASSERT(g.curveCount == 0);
```

### `struct SlugTextStyle`

Параметры оформления прогона текста: цвет, обводка, тень, сглаживание и
преобразования. Передаётся в `Draw`, `DrawOnPath` и `DrawOnArc`.

```cpp
crossrender::SlugTextStyle st;                 // белый текст без обводки и тени
st.color = crossrender::Color::White;
st.softness = 1.0f;
```

### `Color SlugTextStyle::color`

Основной цвет заливки, по умолчанию белый. Альфа участвует в premultiplied-
композитинге вместе с покрытием.

```cpp
crossrender::SlugTextStyle st;
st.color = crossrender::Color{1.0f, 0.9f, 0.4f, 1.0f};   // тёплый жёлтый
```

### `Color SlugTextStyle::outlineColor`

Цвет обводки (stroke), по умолчанию непрозрачный чёрный. Читается только при
`outlineWidth > 0`.

```cpp
crossrender::SlugTextStyle st;
st.outlineColor = crossrender::Color::FromRGB(0x101020);
st.outlineWidth = 0.03f;
```

### `f32 SlugTextStyle::outlineWidth`

Толщина обводки в единицах em, по умолчанию `0` (обводки нет). Обводка
композитится **под** заливкой в premultiplied alpha, поэтому она расширяет
глиф наружу, а не съедает его.

```cpp
crossrender::SlugTextStyle st;
st.outlineWidth = 0.02f;               // примерно 2% кегля
```

### `Color SlugTextStyle::shadowColor`

Цвет тени, по умолчанию `{0, 0, 0, 0.6}`. Альфа делится на число копий тени,
поэтому несколько дрожащих копий не дают резкого удвоения плотности.

```cpp
crossrender::SlugTextStyle st;
st.shadowColor = crossrender::Color{0, 0, 0, 0.5f};
st.shadowOffset = crossrender::Vec2{0.06f, 0.06f};
```

### `Vec2 SlugTextStyle::shadowOffset`

Смещение тени в единицах em, по умолчанию `{0, 0}` (тень не рисуется).
Умножается на `pixelSize`, так что тень масштабируется вместе с текстом.
Ненулевое значение запускает проход тени; `softness > 1` добавляет до четырёх
дополнительных дрожащих копий.

```cpp
crossrender::SlugTextStyle st;
st.shadowOffset = crossrender::Vec2{0.04f, 0.04f};
st.softness = 2.0f;                    // тень мягче: 1 + min(4, 2) = 3 копии
```

### `f32 SlugTextStyle::softness`

Ширина сглаживания (anti-aliasing) в пикселях, по умолчанию `1.0`. В шейдере
зажимается снизу значением `0.25`; заодно управляет числом дрожащих копий тени
(`taps = 1 + min(4, int(softness))` при `softness > 1`).

```cpp
crossrender::SlugTextStyle st;
st.softness = 1.5f;                    // чуть мягче край
```

### `f32 SlugTextStyle::dilation`

Сдвиг границы глифа в единицах em, по умолчанию `0`. В шейдере покрытие
считается как `clamp(0.5 + (dist - dilation) / soft, 0, 1)`, где `dist`
**положительно внутри** глифа. Поэтому положительное значение сдвигает границу
внутрь и делает глиф тоньше, а отрицательное — жирнее. Комментарий в
`SlugText.cpp` утверждает обратное («positive fattens the glyph»), но формула
однозначна: чтобы получить синтетический полужирный, задавайте отрицательное
значение.

```cpp
crossrender::SlugTextStyle st;
st.dilation = -0.01f;                  // синтетический полужирный
```

### `f32 SlugTextStyle::rotation`

Поворот всего прогона в радианах, по умолчанию `0`. Поворот применяется к
каждому глифу вокруг его точки пера, так что прогон разворачивается целиком.

```cpp
crossrender::SlugTextStyle st;
st.rotation = crossrender::Radians(15.0f);     // наклонный заголовок
```

### `Vec2 SlugTextStyle::scale`

Масштаб прогона по осям, по умолчанию `{1, 1}`. Нулевая компонента подменяется
единицей. Учитывается при раскладке и попадает в базисные векторы глифа; на
возвращаемое `Draw` значение ширины `scale.x` не влияет.

```cpp
crossrender::SlugTextStyle st;
st.scale = crossrender::Vec2{1.0f, 1.4f};      // вытянутый по вертикали текст
```

### `f32 SlugTextStyle::twist`

Линейный «закрут»: к повороту глифа прибавляется `twist * x` (в радианах на
единицу em), по умолчанию `0`. Даёт эффект скрученной ленты. Поскольку поворот
становится посимвольным, батчинг отключается — по одному розыгрышу на глиф.

```cpp
crossrender::SlugTextStyle st;
st.twist = 0.6f;                       // текст «закручивается» вправо
```

### `bool SlugTextStyle::billboard`

**Игнорируется реализацией.** Поле описывает 3D-билборд-текст (поворот к
камере) и не имеет смысла для экранного 2D-прогона; шейдер и `Draw` к нему не
обращаются. Оставлено в контракте на будущее.

```cpp
crossrender::SlugTextStyle st;
st.billboard = false;                  // на рендеринг не влияет
```

### `SlugTextRenderer()`

Создаёт объект без шрифта и без GL-ресурсов: `Valid() == false`, шейдер не
собран. Обращений к GPU нет, поэтому конструктор безопасен до создания
контекста.

```cpp
crossrender::SlugTextRenderer slug;            // ещё не готов
ENG_ASSERT(!slug.Valid());
```

### `~SlugTextRenderer()`

Вызывает `Shutdown()`: удаляет шейдер, VAO/VBO, текстуры кривых и полос,
забывает шрифт.

```cpp
{
    crossrender::SlugTextRenderer local;
    local.Init(crossrender::FontManager::Get().DefaultFont());
}   // ресурсы освобождены
```

### `SlugTextRenderer(const SlugTextRenderer&) = delete`

Копирование запрещено: объект владеет ресурсами GPU и кэшем контуров.
Передавайте по ссылке или указателю.

```cpp
void DrawTitle(crossrender::SlugTextRenderer& slug, crossrender::Renderer2D& r2d) {
    slug.Draw(r2d, "TITLE", crossrender::Vec2{40, 60}, 48.0f, crossrender::SlugTextStyle{});
}
```

### `bool Init(Font* font, int emResolution = 64)`

Запоминает шрифт и кегль разбора контуров, очищает кэш и собирает GL-ресурсы:
шейдер `crossrender::SlugText`, quad и (при первом `Flush`) data-текстуры.
`emResolution` зажимается снизу значением `8`. Шрифт должен жить дольше
рендерера — контуры кэшируются по паре (шрифт, кегль).

* **Возвращает:** `false`, если шрифт `nullptr`/невалиден, либо если контекста
  OpenGL нет / шейдер не собрался (в этом случае пишется предупреждение и
  остаётся только CPU-путь: `PrepareGlyph`, `Measure`, `CoverageAt`).
* **Контекст:** для успешного возврата `true` нужен текущий контекст OpenGL.

```cpp
crossrender::Font* font = crossrender::FontManager::Get().DefaultFont();
crossrender::SlugTextRenderer slug;
if (!slug.Init(font, 64)) {
    ENG_LOGW("demo", "Slug недоступен — остаёмся на обычном тексте");
}
```

### `void Shutdown()`

Освобождает все ресурсы и очищает кэш: шейдер, quad, текстуры кривых и полос,
массивы `curves_`/`bands_`/`glyphs_`, индекс глифов. После вызова `Valid()`
возвращает `false`; повторный `Init` снова всё создаст.

```cpp
crossrender::SlugTextRenderer slug;
slug.Init(crossrender::FontManager::Get().DefaultFont());
slug.Shutdown();
ENG_ASSERT(!slug.Valid());
```

### `bool Valid() const`

`true`, только если задан шрифт, GL-ресурсы созданы и шейдер скомпилирован.
Проверяйте перед `Draw`, иначе розыгрыш будет пропущен (но продвижение пера
всё равно посчитается).

```cpp
if (slug.Valid()) ENG_LOGI("demo", "Slug-рендерер готов");
```

### `bool PrepareGlyph(u32 codepoint)`

Разбирает контур глифа один раз и кэширует результат: строит квадратичные
кривые в единицах em, считает границы, разбивает на 8…16 полос, готовит данные
для текстур. Повторный вызов для того же кодпойнта ничего не пересчитывает.

* **Возвращает:** `true`, если у глифа есть контуры; `false` для пробела,
  перевода строки и вообще любого глифа без чернил (при этом пустой глиф
  регистрируется в кэше). Логирует `ENG_LOGW("slug", "U+%04X has no outline")`,
  если шрифт не может отдать контур.

```cpp
if (!slug.PrepareGlyph('g')) {
    ENG_LOGW("demo", "у глифа нет контура — возможно, это пробел");
}
```

### `const SlugGlyph* GetGlyph(u32 codepoint) const`

Возвращает подготовленный глиф или `nullptr`, если кодпойнт ещё не проходил
через `PrepareGlyph`. Пустые глифы (пробел) тоже находятся в кэше — у них
`empty == true`.

```cpp
if (const crossrender::SlugGlyph* g = slug.GetGlyph('A')) {
    ENG_LOGI("demo", "'A': advance %.3f em, %d кривых", g->advance, g->curveCount);
}
```

### `int GlyphCount() const`

Число глифов, уже разобранных и лежащих в кэше (включая пустые).

```cpp
ENG_LOGI("demo", "разобрано глифов: %d", slug.GlyphCount());
```

### `int CurveCount() const`

Общее число квадратичных кривых во всех разобранных глифах. Это размер общей
текстуры кривых, а не число кривых одного глифа.

```cpp
ENG_LOGI("demo", "кривых в кэше: %d", slug.CurveCount());
```

### `int BandCount() const`

Размер плоского массива пар `(firstCurve, curveCount)` — то есть **удвоенное**
число полос всех разобранных глифов. Это осознанное поведение реализации:
наружу выходит размер вектора, а не число полос.

```cpp
ENG_LOGI("demo", "полос (пар * 2): %d", slug.BandCount());
```

### `f32 Measure(const std::string& utf8, f32 letterSpacing = 0.0f, f32 size = 0.0f)`

Измеряет ширину строки UTF-8 в единицах em с учётом кернинга шрифта; при
`size > 0` результат домножается на `size` (пиксели). Отсутствующие глифы
готовятся на месте. Новая строка (`\n`) не обрабатывается — для многострочной
раскладки считайте строки отдельно.

* **Параметры:** `letterSpacing` добавляется к продвижению каждого глифа (в em);
  `size` — необязательный кегль для перевода в пиксели.

```cpp
const crossrender::f32 em = slug.Measure("Score: 42", 0.0f, 0.0f);
const crossrender::f32 px = slug.Measure("Score: 42", 0.0f, 32.0f);
ENG_LOGI("demo", "ширина %.3f em = %.1f px", em, px);
```

### `f32 Draw(Renderer2D& r2d, const std::string& utf8, Vec2 baseline, f32 pixelSize, const SlugTextStyle& style)`

Рисует строку UTF-8, начиная с точки `baseline` (логические пиксели). Внутри:
раскладка с кернингом, `Flush()` для загрузки кривых/полос, при необходимости
проход тени, затем батчи по ≤8 глифов с одинаковым поворотом и масштабом.
Символ `\n` переводит перо на новую строку (`font->LineHeight()`). Состояние GL
(программа, VAO, юниты 0/1, смешивание, отсечение, scissor) восстанавливается
после вызова.

* **Возвращает:** фактически пройденное пером расстояние `advance * pixelSize`
  (в пикселях). Обратите внимание: `style.scale.x` в это значение не входит, а
  если рендерер невалиден, функция всё равно вернёт посчитанное продвижение,
  ничего не нарисовав.

```cpp
crossrender::SlugTextStyle st;
st.color = crossrender::Color::White;
st.shadowOffset = crossrender::Vec2{0.03f, 0.03f};
const crossrender::f32 used = slug.Draw(r2d, "Level 7", crossrender::Vec2{40.0f, 120.0f}, 40.0f, st);
ENG_LOGI("demo", "занято %.1f px", used);
```

### `f32 DrawOnPath(Renderer2D& r2d, const std::string& utf8, const Vec2* path, int pathCount, f32 pixelSize, const SlugTextStyle& style)`

Рисует строку вдоль ломаной: путь параметризуется длиной дуги, каждый глиф
ставится в свою точку и поворачивается по касательной. `twist` для такого
прогона принудительно обнуляется (поворот задаёт путь).

* **Параметры:** `path` — массив точек в логических пикселях, `pathCount >= 2`.
* **Возвращает:** пройденную длину `advance * pixelSize`; `0`, если путь
  вырожден (длина меньше `1e-4`), пуст или строка пуста. Каждый глиф рисуется
  отдельным розыгрышем.

```cpp
const crossrender::Vec2 wave[3] = {{40, 200}, {200, 150}, {360, 200}};
slug.DrawOnPath(r2d, "along the path", wave, 3, 24.0f, crossrender::SlugTextStyle{});
```

### `f32 DrawOnArc(Renderer2D& r2d, const std::string& utf8, Vec2 center, f32 radius, f32 startAngle, f32 pixelSize, const SlugTextStyle& style, bool outside = true)`

Рисует строку по дуге окружности радиуса `radius` с центром `center`. Дуга
всегда обходится **в сторону роста угла** (в системе с `+y` вниз это против
часовой стрелки): флага `clockwise` в контракте нет. `outside` выбирает сторону
окружности — снаружи или внутри (внутри поворот дополнительно разворачивается
на `π`).

* **Возвращает:** пройденную длину `advance * pixelSize`; `0` при
  `radius <= 1e-4`, пустой строке или неположительном кегле.

```cpp
slug.DrawOnArc(r2d, "ROUND AND ROUND", crossrender::Vec2{320, 240}, 120.0f, 0.0f, 28.0f,
               crossrender::SlugTextStyle{}, /*outside=*/true);
```

### `void Flush()`

Загружает накопленные кривые и полосы в data-текстуры. Вызывается
автоматически из `Draw`, поэтому вручную нужен редко — например, после серии
`PrepareGlyph`, чтобы измерить время загрузки. Повторный вызов без изменений
ничего не делает (`dirty_ == false`). Если GL-ресурсы не готовы или массивы
пусты, функция молча выходит.

```cpp
for (crossrender::u32 cp : {'H', 'e', 'l', 'l', 'o'}) slug.PrepareGlyph(cp);
slug.Flush();                          // одна загрузка на все глифы
ENG_LOGI("demo", "загрузка заняла %.2f мс", slug.GetStats().uploadMs);
```

### `struct SlugTextRenderer::Stats`

Счётчики последнего прогона: сколько глифов и «квадов» нарисовано, сколько
кривых стало доступно шейдеру и сколько времени заняла загрузка текстур.
Сбрасывается через `ResetStats()` (или новым `Init`).

```cpp
crossrender::SlugTextRenderer::Stats s;
ENG_ASSERT(s.glyphsDrawn == 0 && s.quads == 0);
```

### `int Stats::glyphsDrawn`

Число глифов, реально отправленных на отрисовку (без пустых). Копии тени в это
число не входят: `Submit` вызывается только для основного прохода.

```cpp
ENG_LOGI("demo", "нарисовано глифов: %d", slug.GetStats().glyphsDrawn);
```

### `int Stats::curvesEvaluated`

Суммарное число кривых, ставших доступными фрагментному шейдеру для
нарисованных глифов. Это **не** число вычислений на фрагмент: сколько кривых
реально проверит GPU, зависит от того, сколько фрагментов попало в каждую
полосу, и на CPU неизвестно.

```cpp
ENG_LOGI("demo", "шейдеру доступно кривых: %d", slug.GetStats().curvesEvaluated);
```

### `int Stats::quads`

Число нарисованных четырёхугольников (инстансов). Совпадает с `glyphsDrawn`,
но подчёркивает, что это именно квады, а не глифы шрифта.

```cpp
ENG_LOGI("demo", "квадов: %d", slug.GetStats().quads);
```

### `f32 Stats::uploadMs`

Время последней загрузки кривых/полос в текстуры, в миллисекундах (замер
`std::chrono::steady_clock` вокруг `Upload`). Помогает ловить всплески при
первом появлении нового кегля.

```cpp
if (slug.GetStats().uploadMs > 2.0f) ENG_LOGW("demo", "дорогая загрузка текстур");
```

### `const Stats& GetStats() const`

Возвращает счётчики по константной ссылке. Данные обновляются во время
`PrepareGlyph`/`Flush`/`Draw`; между кадрами ссылка валидна.

```cpp
const crossrender::SlugTextRenderer::Stats& s = slug.GetStats();
ENG_LOGI("demo", "%d глифов, %.2f мс", s.glyphsDrawn, s.uploadMs);
```

### `void ResetStats()`

Обнуляет счётчики, не трогая кэш глифов и текстуры. Удобно для замера
конкретного кадра или участка кода.

```cpp
slug.ResetStats();
slug.Draw(r2d, "frame", crossrender::Vec2{10, 10}, 24.0f, crossrender::SlugTextStyle{});
ENG_LOGI("demo", "в этом кадре %d глифов", slug.GetStats().glyphsDrawn);
```

### `static f32 CoverageAt(const std::vector<SlugCurve>& curves, Vec2 point, f32 aaWidth = 1.0f)`

CPU-двойник фрагментного шейдера: считает покрытие точки теми же квадратичными
уравнениями и той же формулой
`clamp(0.5 + dist / aaWidth, 0, 1)`. Работает без контекста OpenGL — это путь
для тестов, которые сверяют CPU и GPU. `dist` берётся со знаком: внутри
контура используется расстояние до «входящего» края, снаружи — до «исходящего».

```cpp
// Квадрат из четырёх кривых в единицах em — то, что шейдер делает по контурам.
std::vector<crossrender::SlugCurve> curves;
curves.push_back({crossrender::Vec2{0, 0}, crossrender::Vec2{0, 0}, crossrender::Vec2{1, 0}});
curves.push_back({crossrender::Vec2{1, 0}, crossrender::Vec2{1, 0}, crossrender::Vec2{1, 1}});
curves.push_back({crossrender::Vec2{1, 1}, crossrender::Vec2{1, 1}, crossrender::Vec2{0, 1}});
curves.push_back({crossrender::Vec2{0, 1}, crossrender::Vec2{0, 1}, crossrender::Vec2{0, 0}});
const crossrender::f32 cov = crossrender::SlugTextRenderer::CoverageAt(curves, crossrender::Vec2{0.5f, 0.5f}, 1.0f);
ENG_LOGI("demo", "покрытие центра: %.2f", cov);   // ожидаем ~1.0
```

### `static f32 WindingAt(const std::vector<SlugCurve>& curves, Vec2 point)`

Число витков (winding number) точки относительно набора кривых: сумма знаков
пересечений луча, пущенного вправо. Больше нуля — точка внутри при заливке по
правилу nonzero. Знак зависит от ориентации контура (в системе `+y` вниз
типичный контур TrueType даёт положительное значение), поэтому покрытие
считается по `|winding|`.

```cpp
std::vector<crossrender::SlugCurve> curves;
curves.push_back({crossrender::Vec2{0, 0}, crossrender::Vec2{0, 0}, crossrender::Vec2{1, 0}});
curves.push_back({crossrender::Vec2{1, 0}, crossrender::Vec2{1, 0}, crossrender::Vec2{1, 1}});
curves.push_back({crossrender::Vec2{1, 1}, crossrender::Vec2{1, 1}, crossrender::Vec2{0, 1}});
curves.push_back({crossrender::Vec2{0, 1}, crossrender::Vec2{0, 1}, crossrender::Vec2{0, 0}});
const crossrender::f32 w = crossrender::SlugTextRenderer::WindingAt(curves, crossrender::Vec2{0.5f, 0.5f});
ENG_LOGI("demo", "winding: %.1f", w);   // > 0 — внутри
```

## Пример целиком

```cpp
#include "crossrender/gfx/SlugText.h"

#include "crossrender/core/Log.h"
#include "crossrender/gfx/Renderer2D.h"
#include "crossrender/text/Font.h"

#include <vector>

// Рисует заголовок аналитическим Slug-рендерером: без атласа, по контурам
// глифов, с обводкой, тенью и текстом по дуге.
void DrawVectorTitle(crossrender::Renderer2D& r2d) {
    crossrender::Font* font = crossrender::FontManager::Get().DefaultFont();

    crossrender::SlugTextRenderer slug;
    if (!slug.Init(font, 64)) {
        // Без контекста OpenGL остаётся CPU-путь: метрики и покрытие.
        ENG_LOGW("demo", "Slug недоступен, но Measure/PrepareGlyph работают");
    }

    // 1. Стиль: обводка + мягкая тень + лёгкий закрут.
    crossrender::SlugTextStyle style;
    style.color = crossrender::Color{1.0f, 0.95f, 0.8f, 1.0f};
    style.outlineColor = crossrender::Color::FromRGB(0x201008);
    style.outlineWidth = 0.015f;
    style.shadowColor = crossrender::Color{0, 0, 0, 0.55f};
    style.shadowOffset = crossrender::Vec2{0.03f, 0.04f};
    style.softness = 2.0f;               // 3 дрожащие копии тени (1 + min(4, 2))
    style.rotation = 0.0f;
    style.twist = 0.0f;
    style.billboard = false;             // игнорируется реализацией

    // 2. Явная подготовка глифов и одна загрузка текстур на всю строку.
    for (crossrender::u32 cp : {'V', 'E', 'C', 'T', 'O', 'R'}) slug.PrepareGlyph(cp);
    slug.Flush();

    // 3. Обычная строка: ширина считается в em и в пикселях.
    const crossrender::f32 em = slug.Measure("VECTOR", 0.0f, 0.0f);
    const crossrender::f32 drawn = slug.Draw(r2d, "VECTOR", crossrender::Vec2{80.0f, 120.0f}, 72.0f, style);
    ENG_LOGI("demo", "ширина %.3f em, нарисовано %.1f px", em, drawn);

    // 4. Текст по ломаной: глифы сами поворачиваются по касательной.
    const crossrender::Vec2 wave[4] = {{40, 240}, {160, 200}, {320, 280}, {440, 240}};
    slug.DrawOnPath(r2d, "along the path", wave, 4, 28.0f, style);

    // 5. Текст по дуге; сторона выбирается флагом outside.
    slug.DrawOnArc(r2d, "AROUND", crossrender::Vec2{240, 480}, 140.0f, crossrender::kPi, 32.0f, style,
                   /*outside=*/true);

    // 6. Кэш и статистика.
    ENG_LOGI("demo", "глифов %d, кривых %d, элементов полос %d", slug.GlyphCount(),
             slug.CurveCount(), slug.BandCount());
    if (const crossrender::SlugGlyph* g = slug.GetGlyph('V')) {
        ENG_LOGI("demo", "'V' занимает %.3fx%.3f em", g->bounds.w, g->bounds.h);
    }
    ENG_LOGI("demo", "%d розыгрышей, загрузка %.2f мс", slug.GetStats().quads,
             slug.GetStats().uploadMs);
    slug.ResetStats();

    // 7. CPU-проверка покрытия без GPU — тот же луч, что и в шейдере.
    std::vector<crossrender::SlugCurve> curves;
    crossrender::SlugCurve c;
    c.p0 = crossrender::Vec2{0, 0};
    c.p1 = crossrender::Vec2{0.5f, 1.0f};
    c.p2 = crossrender::Vec2{1, 0};
    curves.push_back(c);
    ENG_LOGI("demo", "winding %.1f, покрытие %.2f",
             crossrender::SlugTextRenderer::WindingAt(curves, crossrender::Vec2{0.5f, 0.4f}),
             crossrender::SlugTextRenderer::CoverageAt(curves, crossrender::Vec2{0.5f, 0.4f}, 1.0f));

    slug.Shutdown();
}
```

## См. также

* `docs/gfx/Renderer2D.md` — `DrawText`, `DrawTextOnPath`, `DrawTextOnArc`:
  привычный путь по атласу, альтернативой которому служит Slug.
* `docs/gfx/Texture.md` — форматы `RGBA32F`/`R32F` и `Texture::Update`, которыми
  загружаются текстуры кривых и полос.
* `docs/gfx/Shader.md` — `Shader::Set`, `SetTexture` и сборка программы, которую
  использует рендерер.
* `docs/gfx/RenderTarget.md` — цели, в которые рисует `Renderer2D` во время
  прогона.
* `docs/core/Math.md` — `Vec2`, `Rect`, `Rotate`, `Lerp`, `Normalize`.
* `docs/core/Log.md` — `ENG_LOGW`/`ENG_LOGE`, которыми рендерер сообщает об
  отсутствии контура, бюджете кривых и переполнении полосы.
