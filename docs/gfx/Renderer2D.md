# crossrender/gfx/Renderer2D.h — 2D-векторный рендерер

Рендерер немедленного режима (immediate mode) для векторной 2D-графики: пути,
фигуры, градиенты, изображения и текст в логических координатах, которые
собираются в один динамический вершинный буфер и рисуются пачками за несколько
вызовов отрисовки на кадр.

## Заголовок

```cpp
#include "crossrender/gfx/Renderer2D.h"
```

## Обзор

`Renderer2D` — рабочая лошадка интерфейса и 2D-сцен: практически каждый кадр
движка проходит через него. Он не хранит список объектов, а рисует сразу:
каждый вызов вида `FillRect` / `Fill` / `DrawText` тут же кладёт готовую
геометрию в буфер. Вся тяжёлая работа — разбиение кривых, триангуляция,
построение каймы сглаживания — выполняется на CPU, в вершинный шейдер попадают
уже координаты экранного пространства.

### Модель кадра

Рисование возможно только **между** `BeginFrame` и `EndFrame`. Именно
`BeginFrame` сбрасывает батч, стек состояния и статистику, а `EndFrame`
вызывает `Flush` и отправляет накопленную геометрию в GPU. Порядок вызовов:

```cpp
r2d.Init();                                   // один раз, при живом контексте OpenGL

while (running) {
    r2d.BeginFrame(fbWidth, fbHeight, dpiScale);  // начало кадра: сброс батча и состояния
    DrawScene(r2d);                               // FillRect / DrawText / Image / пути
    r2d.EndFrame();                               // Flush: все батчи уходят в GPU
}

r2d.Shutdown();                               // при выходе
```

Что из этого следует:

* `Init()` вызывается **один раз** после создания контекста OpenGL; повторный
  вызов безвреден.
* Между `BeginFrame` и `EndFrame` рендерер не привязывает фреймбуфер и не
  очищает его. Если нужен закадровый буфер (offscreen framebuffer), его
  привязывает вызывающий код, а `BeginFrame` лишь задаёт проекцию, размеры
  экрана и состояние GL. Ненулевой `target` запоминается, но сам рендерер его
  не биндит — это ответственность того, кто вызвал `BeginFrame`.
* Состояние (цвет, толщина, преобразование, отсечение) — это **стек**:
  `Save` кладёт копию, `Restore` возвращает. Любой сеттер действует на все
  последующие вызовы до конца кадра или до `Restore`.
* Пачки собираются по ключу (текстура, тип заливки, режим смешивания,
  scissor, шейдерная программа). Соседние примитивы с одинаковым ключом
  попадают в один вызов отрисовки, поэтому порядок важен: изображение,
  вставленное между двумя текстами, разрывает батч текста.

### Инертный режим

Если текущего контекста OpenGL нет (или не собрались встроенные шейдеры),
`Init()` всё равно возвращает `true`, но рендерер переходит в **инертный
режим**: пути, раскладка текста, измерение и попадание по координатам
работают как обычно, а отправка геометрии в GPU пропускается. Благодаря этому
UI-код и тесты можно прогонять без окна и видеокарты. Проверять готовность
следует не по результату `Init()`, а по логам (`r2d`): при отказе шейдеров
пишется предупреждение.

```cpp
// Без окна и контекста этот код проходит целиком: раскладка считается,
// а отправка геометрии в GPU просто пропускается.
crossrender::Renderer2D r2d;
const bool ok = r2d.Init();                       // true даже в инертном режиме
r2d.BeginFrame(1280, 800);
const crossrender::Rect box = r2d.CurrentClip();
r2d.FillRoundedRect(box, 8.0f, crossrender::Color::White);
r2d.EndFrame();
ENG_LOGI("demo", "Init=%d, вызовов отрисовки: %d", ok ? 1 : 0, r2d.GetStats().drawCalls);
```

### Ловушка с типом заливки

Порядок значений в `enum class Paint::Type` **не совпадает** с константами
`uType` спрайтового шейдера:

| `Paint::Type` | Порядок в enum | Константа `uType` в шейдере |
|---|---|---|
| `Solid` | 0 | 0 (сплошной цвет) |
| `LinearGradient` | 1 | 1 (линейный градиент) |
| `RadialGradient` | 2 | 2 (радиальный градиент) |
| `BoxGradient` | 3 | **4** (градиент рамки) |
| `ImagePattern` | 4 | **3** (изображение) |

Рендерер переводит одно в другое явным `switch`, а не приведением типа.
Поэтому никогда не полагайтесь на числовое значение `Paint::Type`, не
приводите его к `int` вручную и не сверяйтесь с нумерацией из заголовка:
всегда создавайте заливку статическими фабриками `Paint::Solid`,
`Paint::Linear`, `Paint::Radial`, `Paint::Box`, `Paint::Image`.

```cpp
// Правильно: тип заливки задаёт фабрика, рендерер сам переведёт его в uType.
crossrender::Paint ok = crossrender::Paint::Box(center, w, h, 8.0f, 12.0f, inner, outer);
r2d.FillPaint(ok);
r2d.RoundedRect(x, y, w, h, 8.0f);
r2d.Fill();

// Ошибка: 3 — это BoxGradient в объявлении enum, но 3 же значит «изображение»
// в шейдере, поэтому такая заливка нарисуется текстурой вместо градиента.
// crossrender::Paint bad;
// bad.type = static_cast<crossrender::Paint::Type>(3);
```

### Координаты, сглаживание и статистика

* Начало координат — левый верхний угол, ось Y направлена **вниз**;
  координаты логические, `ScreenSize()` возвращает размер в логических
  единицах (`fbWidth / dpiScale`).
* Заливки получают однопиксельную аналитическую кайму сглаживания; обводки
  сглаживаются MSAA, поэтому при выключенном `AntiAlias` края заливок
  становятся жёсткими.
* `GetStats()` обнуляется в `BeginFrame` и наполняется только в `Flush`,
  то есть осмыслен **после** `EndFrame`. До конца кадра число неотправленных
  вершин показывает `PendingVertices()`.

```cpp
const crossrender::Vec2 screen = r2d.ScreenSize();   // логический размер, а не пиксели
r2d.FillRect(0, 0, screen.x, 48.0f, crossrender::Color::FromRGB(0x101527));   // Y растёт вниз
r2d.EndFrame();

const crossrender::Renderer2D::Stats& s = r2d.GetStats();   // осмысленно только здесь
ENG_LOGI("perf", "отправлено треугольников: %d", s.vertices);
```

## Члены класса

### Типы: Paint

Заливка описывает, чем заполняется путь: сплошным цветом, линейным,
радиальным или рамонным градиентом либо изображением. Один и тот же `Paint`
применяется и к путям (`FillPaint`), и к обводкам (`StrokePaint`).

```cpp
crossrender::Renderer2D r2d;
crossrender::Paint fill = crossrender::Paint::Linear({0, 0}, {0, rect.h}, crossrender::Color::FromRGB(0x6BA1FF),
                                     crossrender::Color::FromRGB(0x2F6FE0));
r2d.FillPaint(fill);
r2d.RoundedRect(rect.x, rect.y, rect.w, rect.h, 8.0f);
r2d.Fill();
```

### `enum class Paint::Type : u8`

Тип заливки. Значения объявлены в порядке `Solid`, `LinearGradient`,
`RadialGradient`, `BoxGradient`, `ImagePattern`, который не совпадает с
константами `uType` шейдера (см. таблицу выше) — не полагайтесь на числа.

```cpp
crossrender::Paint p = crossrender::Paint::Solid(crossrender::Color::White);
if (p.type == crossrender::Paint::Type::Solid) {
    ENG_LOGI("r2d", "заливка сплошным цветом");
}
p.type = crossrender::Paint::Type::LinearGradient;   // поля p0/p1/innerColor/outerColor
```

### `Paint::Type type`

Текущий тип заливки. По умолчанию `Type::Solid`. При смене типа вручную
нужно заполнить соответствующие поля (`p0`/`p1` для градиентов,
`image` для изображения).

```cpp
crossrender::Paint glass;
glass.type = crossrender::Paint::Type::BoxGradient;
glass.p0 = panel.Center();
glass.p1 = {panel.w, panel.h};
glass.r0 = 8.0f;
glass.feather = 12.0f;
glass.innerColor = crossrender::Color{1, 1, 1, 0.18f};
glass.outerColor = crossrender::Color{1, 1, 1, 0.0f};
```

### `Color color`

Основной цвет. Используется типом `Solid` как цвет заливки, а типом
`ImagePattern` — как множитель (tint) текстуры.

```cpp
crossrender::Paint flat = crossrender::Paint::Solid(crossrender::Color::FromRGB(0x1D2029));
flat.color = flat.color.WithAlpha(0.85f);   // полупрозрачная панель
r2d.FillRect(panel, flat.color);
```

### `Color innerColor / Color outerColor`

Цвета градиента: `innerColor` — у начала оси или в центре, `outerColor` — у
конца или на внешнем радиусе. Для `Solid` и `ImagePattern` не используются.

```cpp
crossrender::Paint sunset = crossrender::Paint::Linear({0, 0}, {0, 200}, crossrender::Color::White, crossrender::Color::Black);
sunset.innerColor = crossrender::Color::FromRGB(0xFFD36E);
sunset.outerColor = crossrender::Color::FromRGB(0xE4574F);
r2d.FillPaint(sunset);
r2d.Rect(0, 0, 320, 200);
r2d.Fill();
```

### `Vec2 p0, p1`

Геометрия заливки: для линейного градиента — начало и конец оси, для
радиального — центр (оба поля равны), для рамонного — центр и размеры
(`p1` хранит ширину и высоту), для изображения — начало узора.

```cpp
// Диагональный линейный градиент по карточке.
crossrender::Paint diag = crossrender::Paint::Linear(card.Min(), card.Max(), accent, crossrender::Color::Transparent);
r2d.FillPaint(diag);
r2d.RoundedRect(card.x, card.y, card.w, card.h, 10.0f);
r2d.Fill();
```

### `f32 r0 = 0, r1 = 1`

Радиусы радиального градиента (`r0` — внутренний, `r1` — внешний) либо
радиус углов для рамонного градиента (`r0`). Значения домножаются на масштаб
текущего преобразования при отправке в шейдер.

```cpp
// Мягкое свечение: непрозрачный центр и прозрачный внешний радиус.
crossrender::Paint crossrender = crossrender::Paint::Radial(orb, 0.0f, radius * 2.5f, crossrender::Color{1, 1, 1, 0.6f},
                                     crossrender::Color{0.1f, 0.35f, 0.9f, 0.0f});
crossrender.r0 = radius * 0.25f;   // плоское ядро без «дырки» в центре
r2d.FillPaint(crossrender);
r2d.Circle(orb.x, orb.y, radius * 2.5f);
r2d.Fill();
```

### `f32 feather`

Мягкость рамонного градиента в логических пикселях: ширина перехода от
`innerColor` к `outerColor`. Ноль даёт резкую границу.

```cpp
crossrender::Paint halo = crossrender::Paint::Box(button.Center(), button.w, button.h, 8.0f, 0.0f, accent,
                                  crossrender::Color::Transparent);
halo.feather = 14.0f;   // размытый ореол вокруг кнопки
r2d.FillPaint(halo);
r2d.RoundedRect(button.x - 14.0f, button.y - 14.0f, button.w + 28.0f, button.h + 28.0f, 14.0f);
r2d.Fill();
```

### `f32 angle`

Поворот узора изображения в радианах (для `ImagePattern`). На градиенты не
влияет.

```cpp
crossrender::Paint hatch = crossrender::Paint::Image(hatchTexture, crossrender::Color{1, 1, 1, 0.35f});
hatch.angle = crossrender::Radians(15.0f);   // наклонная штриховка
r2d.FillPaint(hatch);
r2d.Rect(0, 0, 240, 120);
r2d.Fill();
```

### `const Texture* image`

Текстура для типа `ImagePattern`; может быть `nullptr`. Текстура должна быть
жива, пока заливка используется: `Paint` хранит только указатель.

```cpp
crossrender::Texture logo;
logo.LoadFromFile("textures/logo.png");
crossrender::Paint stamp = crossrender::Paint::Image(logo, crossrender::Color::White.WithAlpha(0.8f));
if (stamp.image != nullptr && stamp.image->Valid()) {
    r2d.FillPaint(stamp);
    r2d.Rect(16, 16, 128, 128);
    r2d.Fill();
}
```

### `TextureFilter filter`

Фильтрация текстуры для типа `ImagePattern` (по умолчанию
`TextureFilter::Linear`; для пиксель-арта ставьте `Nearest`).

```cpp
crossrender::Paint pixels = crossrender::Paint::Image(spriteSheet, crossrender::Color::White);
pixels.filter = crossrender::TextureFilter::Nearest;   // без размытия при масштабе
r2d.FillPaint(pixels);
r2d.Rect(0, 0, 256, 256);
r2d.Fill();
```

### `static Paint Solid(const Color& c)`

Фабрика сплошной заливки — самый частый случай.

```cpp
r2d.FillPaint(crossrender::Paint::Solid(crossrender::Color::FromRGB(0xE4574F)));
r2d.Circle(120, 120, 48);
r2d.Fill();
```

### `static Paint Linear(const Vec2& a, const Vec2& b, const Color& inner, const Color& outer)`

Линейный градиент вдоль оси от `a` к `b`: `inner` в начале оси, `outer` в
конце. Цвет за пределами оси продолжается крайними цветами.

```cpp
// Прогресс-бар: заливка меняет цвет слева направо.
r2d.FillPaint(crossrender::Paint::Linear({bar.x, bar.y}, {bar.Right(), bar.y}, crossrender::Color::FromRGB(0x3FBF7F),
                                 crossrender::Color::FromRGB(0xF2B33D)));
r2d.RoundedRect(bar.x, bar.y, bar.w * progress, bar.h, bar.h * 0.5f);
r2d.Fill();
```

### `static Paint Radial(const Vec2& center, f32 inner, f32 outer, const Color& cin, const Color& cout)`

Радиальный градиент: цвет `cin` до радиуса `inner`, затем переход к `cout` на
радиусе `outer`.

```cpp
const crossrender::Vec2 c = orb.Center();
r2d.FillPaint(crossrender::Paint::Radial(c, 0.0f, 64.0f, crossrender::Color::White, crossrender::Color::FromRGB(0x2F6FE0)));
r2d.Circle(c.x, c.y, 64.0f);
r2d.Fill();
```

### `static Paint Box(const Vec2& center, f32 w, f32 h, f32 radius, f32 feather, const Color& cin, const Color& cout)`

Градиент рамки: прямоугольник со скруглёнными углами `radius` вокруг центра
`center`, размером `w` на `h`, с мягким переходом шириной `feather`. Удобен
для подсветки и ореолов вокруг элементов UI.

```cpp
r2d.FillPaint(crossrender::Paint::Box(button.Center(), button.w + 24.0f, button.h + 24.0f, 12.0f, 10.0f,
                              crossrender::Color::FromRGB(0x6BA1FF), crossrender::Color{0.05f, 0.12f, 0.35f, 0.0f}));
r2d.RoundedRect(button.x - 16.0f, button.y - 16.0f, button.w + 32.0f, button.h + 32.0f, 14.0f);
r2d.Fill();
```

### `static Paint Image(const Texture& tex, const Color& tint, f32 angleRad = 0)`

Заливка текстурой: изображение повторяется в координатах пути (для путей это
UV-координаты вершин), `tint` домножается на цвет текстуры, `angleRad`
поворачивает узор.

```cpp
r2d.FillPaint(crossrender::Paint::Image(panelTex, crossrender::Color{1, 1, 1, 0.9f}));
r2d.RoundedRect(panel.x, panel.y, panel.w, panel.h, 12.0f);
r2d.Fill();
```

### `enum class LineCap : u8`

Форма концов незамкнутой обводки.

| Значение | Смысл |
|---|---|
| `LineCap::Butt` | обрез ровно по конечной точке (по умолчанию) |
| `LineCap::Round` | полукруг радиусом в половину толщины |
| `LineCap::Square` | прямоугольник, выступающий на половину толщины |

```cpp
r2d.LineCap(crossrender::LineCap::Round);
r2d.StrokeWidth(6.0f);
r2d.DrawLine(40, 40, 240, 40, crossrender::Color::White);
```

### `enum class LineJoin : u8`

Форма стыка двух сегментов обводки.

| Значение | Смысл |
|---|---|
| `LineJoin::Miter` | острый «ус», ограниченный `MiterLimit` (по умолчанию) |
| `LineJoin::Round` | скруглённый стык |
| `LineJoin::Bevel` | срезанный стык |

```cpp
const crossrender::Vec2 zig[4] = {{0, 40}, {60, 0}, {120, 40}, {180, 0}};
r2d.LineJoin(crossrender::LineJoin::Round);
r2d.LineCap(crossrender::LineCap::Round);
r2d.StrokeWidth(8.0f);
r2d.Polyline(zig, 4);
r2d.Stroke();
```

### `enum class Winding : i8`

Направление обхода контура: против часовой стрелки (`CCW = -1`) или по
часовой (`CW = 1`). Влияет на выбор дуги и на правила заливки.

```cpp
// Верхняя половина окружности против часовой стрелки.
r2d.BeginPath();
r2d.Arc(120, 120, 60.0f, crossrender::kPi, 0.0f, crossrender::Winding::CCW);
r2d.ClosePath();
r2d.Fill();
```

### `enum class TextAlign : u8`

Горизонтальное выравнивание текста относительно точки привязки.

| Значение | Смысл |
|---|---|
| `TextAlign::Left` | точка привязки — левый край (по умолчанию) |
| `TextAlign::Center` | точка привязки — середина строки |
| `TextAlign::Right` | точка привязки — правый край |
| `TextAlign::Justify` | выравнивание по ширине (для многострочного текста) |

```cpp
r2d.DrawText(font, "По центру", panel.Center().x, panel.y + 24.0f, crossrender::Color::White, 18.0f,
             crossrender::TextAlign::Center, crossrender::TextBaseline::Middle);
```

### `enum class TextBaseline : u8`

Вертикальная привязка текста.

| Значение | Смысл |
|---|---|
| `TextBaseline::Top` | `y` — верх строки (по умолчанию) |
| `TextBaseline::Middle` | `y` — середина строки |
| `TextBaseline::Bottom` | `y` — низ строки |
| `TextBaseline::Alphabetic` | `y` — базовая линия |

```cpp
// Подпись по центру кнопки: и по горизонтали, и по вертикали.
r2d.DrawText(font, "ОК", button.Center().x, button.Center().y, crossrender::Color::Black, 16.0f,
             crossrender::TextAlign::Center, crossrender::TextBaseline::Middle);
```

### `enum class TextBreak : u8`

Правило переноса строк в `WrapText` и `DrawTextBox`.

| Значение | Смысл |
|---|---|
| `TextBreak::None` | переносов нет, абзац остаётся одной строкой |
| `TextBreak::Char` | перенос по любому символу (для CJK и логов) |
| `TextBreak::Word` | перенос по словам (по умолчанию в текстовых полях) |

```cpp
const std::vector<std::string> lines = crossrender::WrapText(font, description, box.w, 16.0f,
                                                     crossrender::TextBreak::Word);
for (crossrender::usize i = 0; i < lines.size(); ++i) {
    r2d.DrawText(font, lines[i], box.x, box.y + static_cast<crossrender::f32>(i) * 20.0f, crossrender::Color::White, 16.0f);
}
```

### Типы: NinePatch

Описание девятипатчевой (nine-patch) растяжки: текстурная рамка, у которой
углы не масштабируются, а края и центр тянутся. Применяется для фонов кнопок
и панелей, которые должны сохранять толщину обводки при любом размере.

```cpp
crossrender::NinePatch patch = crossrender::NinePatch::Uniform(12.0f);
r2d.Image9(buttonTex, button, patch, crossrender::Color::White);
```

### `f32 left, top, right, bottom`

Отступы в пикселях текстурного пространства: сколько пикселей по каждому краю
не растягивается. По умолчанию нули, то есть обычное растяжение целиком.

```cpp
crossrender::NinePatch frame;
frame.left = 8.0f;
frame.top = 8.0f;
frame.right = 8.0f;
frame.bottom = 8.0f;
r2d.Image9(panelTex, panel, frame, crossrender::Color::White, r2d.DpiScale());
```

### `static NinePatch Uniform(f32 v)`

Задаёт одинаковый отступ со всех четырёх сторон — обычный случай для
симметричной рамки.

```cpp
const crossrender::NinePatch buttonFrame = crossrender::NinePatch::Uniform(10.0f);
r2d.Image9(buttonTex, button, buttonFrame, crossrender::Color::White);
```

### Типы: TextStyle

`TextStyle` собирает в одном месте всё, что умеет богатый текст: заливку,
обводку, свечение, тень, преобразования глифов, искривление строки и
многострочную раскладку. Он передаётся в `DrawTextStyled` и производные
методы. Готовые наборы дают статические фабрики `Filled`, `Outlined`,
`Shadowed`, `GradientText`; связанный тип `FontStyle` описывает запечённый
стиль шрифта.

```cpp
crossrender::TextStyle title = crossrender::TextStyle::GradientText(crossrender::Color::FromRGB(0xFFD36E),
                                                    crossrender::Color::FromRGB(0xE4574F));
title.align = crossrender::TextAlign::Center;
title.baseline = crossrender::TextBaseline::Middle;
title.shadowColor = crossrender::Color{0, 0, 0, 0.55f};
title.shadowOffset = {0, 3};
r2d.DrawTextStyled(font, "УРОВЕНЬ 7", {screen.w * 0.5f, 64.0f}, 34.0f, title);
```

### Поля заливки: `color`, `innerColor`, `outerColor`, `gradient`, `gradientRadial`, `gradientCenter`

| Поле | Смысл |
|---|---|
| `color` | основной цвет глифа, когда градиент выключен |
| `innerColor` | цвет у верхнего края глифа (или у центра при радиальном градиенте) |
| `outerColor` | цвет у нижнего края глифа (или на периферии) |
| `gradient` | включает двухцветную заливку вместо сплошной |
| `gradientRadial` | делает градиент радиальным от `gradientCenter` |
| `gradientCenter` | центр радиального градиента в долях габаритов глифа |

```cpp
crossrender::TextStyle st = crossrender::TextStyle::Filled(crossrender::Color::White);
st.gradient = true;
st.gradientRadial = true;
st.gradientCenter = {0.5f, 0.35f};
st.innerColor = crossrender::Color::FromRGB(0xFFFFFF);
st.outerColor = crossrender::Color::FromRGB(0x2F6FE0);
r2d.DrawTextStyled(font, "СОКРОВИЩЕ", {240.0f, 120.0f}, 40.0f, st);
```

### Поля обводки: `outlineColor`, `outlineWidth`

Обводка рисуется вокруг глифа: `outlineWidth` — толщина в логических
пикселях, ноль отключает её. Для SDF-шрифтов контур аналитический, для
растровых он имитируется смещёнными копиями.

```cpp
crossrender::TextStyle st = crossrender::TextStyle::Filled(crossrender::Color::FromRGB(0xFFD36E));
st.outlineColor = crossrender::Color::FromRGB(0x3A2410);
st.outlineWidth = 3.0f;
r2d.DrawTextStyled(font, "БОСС", {320.0f, 80.0f}, 44.0f, st);
```

### Поля свечения: `glowColor`, `glowRadius`, `glowIntensity`

Внешнее свечение — несколько увеличенных и ослабленных копий позади глифа.
`glowRadius` — на сколько пикселей расширяется копия, `glowIntensity` —
яркость; при нулевом радиусе свечение не рисуется.

```cpp
crossrender::TextStyle st = crossrender::TextStyle::Filled(crossrender::Color::White);
st.glowColor = crossrender::Color{1.0f, 0.85f, 0.4f, 0.55f};
st.glowRadius = 8.0f;
st.glowIntensity = 1.2f;
r2d.DrawTextStyled(font, "МАГИЯ", {200.0f, 200.0f}, 36.0f, st);
```

### Поля тени: `shadowColor`, `shadowOffset`, `shadowSoftness`, `shadowSamples`

Тень — копия глифа, смещённая на `shadowOffset`. При `shadowSoftness > 0`
рисуется несколько копий по кольцу (`shadowSamples` штук, от 1 до 12), что
даёт дешёвое размытие.

```cpp
crossrender::TextStyle st = crossrender::TextStyle::Filled(crossrender::Color::White);
st.shadowColor = crossrender::Color{0, 0, 0, 0.65f};
st.shadowOffset = {3.0f, 4.0f};
st.shadowSoftness = 2.0f;
st.shadowSamples = 5;
r2d.DrawTextStyled(font, "Тень", {80.0f, 300.0f}, 32.0f, st);
```

### Поля раскладки: `letterSpacing`, `lineHeightMul`, `rotation`, `scale`, `skew`, `twist`, `waveAmplitude`, `waveFrequency`

| Поле | Смысл |
|---|---|
| `letterSpacing` | дополнительный трекинг между глифами в пикселях |
| `lineHeightMul` | множитель межстрочного интервала |
| `rotation` | поворот всей строки в радианах |
| `scale` | масштаб глифов по осям |
| `skew` | наклон глифов (сдвиг, а не поворот) |
| `twist` | дополнительный поворот на каждый пиксель продвижения пера |
| `waveAmplitude` | амплитуда вертикальной волны вдоль строки |
| `waveFrequency` | частота этой волны |

```cpp
crossrender::TextStyle wave = crossrender::TextStyle::Filled(crossrender::Color::FromRGB(0x6BA1FF));
wave.waveAmplitude = 6.0f;
wave.waveFrequency = 0.08f;
wave.letterSpacing = 2.0f;
r2d.DrawTextStyled(font, "ВОЛНА", {40.0f, 160.0f}, 28.0f, wave);
```

### Поля жирности: `bold`, `boldAmount`

Синтетическая жирность: при `bold = true` глиф рисуется несколько раз со
сдвигом, `boldAmount` задаёт величину утолщения. Полезно, когда в шрифте нет
жирного начертания.

```cpp
crossrender::TextStyle st = crossrender::TextStyle::Outlined(crossrender::Color::White, crossrender::Color::Black, 2.0f);
st.bold = true;
st.boldAmount = 0.8f;
r2d.DrawTextStyled(font, "ВАЖНО", {120.0f, 220.0f}, 26.0f, st);
```

### Поля многострочности: `align`, `baseline`, `wrap`, `maxLines`

Поведение при выводе через `DrawTextBoxStyled`: горизонтальное выравнивание,
вертикальная привязка, правило переноса и ограничение числа строк.
`maxLines = 0` означает «без ограничения».

```cpp
crossrender::TextStyle body = crossrender::TextStyle::Filled(crossrender::Color{0.85f, 0.88f, 0.95f, 1.0f});
body.align = crossrender::TextAlign::Left;
body.baseline = crossrender::TextBaseline::Top;
body.wrap = crossrender::TextBreak::Word;
body.maxLines = 4;
r2d.DrawTextBoxStyled(font, description, textBox, 16.0f, body);
```

### `static TextStyle Filled(const Color& c)`

Сплошная заливка глифов — базовый стиль.

```cpp
r2d.DrawTextStyled(font, "Обычный текст", {24.0f, 24.0f}, 18.0f,
                   crossrender::TextStyle::Filled(crossrender::Color::White));
```

### `static TextStyle Outlined(const Color& fill, const Color& outline, f32 width)`

Заливка плюс обводка указанной толщины. Часто используется для текста поверх
пёстрого фона.

```cpp
r2d.DrawTextStyled(font, "ЧИТАЕМО", {200.0f, 40.0f}, 24.0f,
                   crossrender::TextStyle::Outlined(crossrender::Color::White, crossrender::Color::FromRGB(0x101527), 3.0f));
```

### `static TextStyle Shadowed(const Color& fill, const Vec2& offset = {2, 3})`

Заливка плюс жёсткая тень со смещением по умолчанию `{2, 3}`.

```cpp
r2d.DrawTextStyled(font, "Заголовок", {40.0f, 48.0f}, 30.0f,
                   crossrender::TextStyle::Shadowed(crossrender::Color::White));
```

### `static TextStyle GradientText(const Color& inner, const Color& outer)`

Включает вертикальный градиент от `inner` (верх глифа) к `outer` (низ).

```cpp
r2d.DrawTextStyled(font, "ЗОЛОТО", {160.0f, 90.0f}, 46.0f,
                   crossrender::TextStyle::GradientText(crossrender::Color::FromRGB(0xFFF3B0),
                                                crossrender::Color::FromRGB(0xE09B2D)));
```

### Жизненный цикл

Создание, инициализация и покадровая работа. Рендерер создаётся один раз на
приложение (обычно как поле класса `Engine`), инициализируется при живом
контексте OpenGL и рисует строго между `BeginFrame` и `EndFrame`.

```cpp
crossrender::Renderer2D r2d;
if (!r2d.Init()) ENG_LOGE("demo", "рендерер недоступен");

while (running) {
    r2d.BeginFrame(width, height, dpi);
    DrawHud(r2d);
    r2d.EndFrame();
}

r2d.Shutdown();
```

### `Renderer2D()` / `~Renderer2D()`

Конструктор создаёт внутренние структуры, деструктор вызывает `Shutdown()`.
Копирование запрещено — рендерер владеет ресурсами GPU, поэтому храните его
по значению в одном месте или передавайте по ссылке.

```cpp
struct GameRenderer {
    crossrender::Renderer2D r2d;   // один экземпляр на приложение
};

GameRenderer renderer;
renderer.r2d.Init();
// GameRenderer copy = renderer;   // ошибка компиляции — так и задумано
```

### `bool Init()`

Создаёт шейдеры, VAO и динамические буферы. Вызывается после создания
контекста OpenGL; повторный вызов ничего не делает. Если контекста нет или
шейдеры не собрались, возвращает `true`, но рендерер переходит в инертный
режим — смотрите логи.

```cpp
crossrender::Renderer2D r2d;
r2d.Init();
if (!r2d.PendingVertices() && r2d.GetStats().drawCalls == 0) {
    ENG_LOGD("demo", "проверьте лог r2d: возможен инертный режим");
}
```

### `void Shutdown()`

Освобождает шейдеры, буферы и вспомогательную текстуру. Вызывается
деструктором; вручную — при потере контекста или перед его уничтожением.
После `Shutdown()` можно снова вызвать `Init()`.

```cpp
void OnContextLost(crossrender::Renderer2D& r2d) {
    r2d.Shutdown();
    ENG_LOGW("demo", "контекст потерян, ресурсы рендерера освобождены");
}
```

### `void BeginFrame(int fbWidth, int fbHeight, f32 dpiScale = 1.0f, RenderTarget* target = nullptr)`

Начинает кадр: пересчитывает проекцию и размер экрана, очищает батч, стек
состояния и статистику, включает смешивание и отключает тест глубины.
`fbWidth`/`fbHeight` — размеры в физических пикселях, `dpiScale` переводит их
в логические единицы. Ненулевой `target` запоминается, но рендерер **не
привязывает** фреймбуфер — это делает вызывающий код.

```cpp
const int fbW = window.FramebufferWidth();
const int fbH = window.FramebufferHeight();
r2d.BeginFrame(fbW, fbH, window.DpiScale());
// экран теперь имеет логический размер ScreenSize(), проекция пересчитана
```

### `void EndFrame()`

Завершает кадр: вызывает `Flush()` и отправляет все накопленные пачки в GPU.
После этого статистика за кадр готова к чтению. Повторный вызов без
предшествующего `BeginFrame` ничего не делает.

```cpp
r2d.BeginFrame(fbW, fbH, dpi);
r2d.FillRect(hud, crossrender::Color::FromRGB(0x101527));
r2d.EndFrame();

const int calls = r2d.GetStats().drawCalls;   // осмысленно только здесь
ENG_LOGI("demo", "кадр: %d вызовов отрисовки", calls);
```

### `void Flush()`

Немедленно отправляет накопленную геометрию в GPU, не завершая кадр. Нужен
редко: например, чтобы нарисовать 2D-слой до чужого прохода, который сам
меняет состояние OpenGL. После отправки батч очищается, статистика
накапливается.

```cpp
r2d.Save();
r2d.ClipRect(0, 0, screen.w, headerHeight);
DrawHeader(r2d);
r2d.Restore();
r2d.Flush();          // отправляем до того, как UI-слой сменит программу
```

### Состояние и преобразования

Стек состояния и матрица преобразования. Каждый сеттер действует на все
последующие команды рисования, а `Save`/`Restore` позволяют менять состояние
локально, не восстанавливая его вручную.

```cpp
r2d.Save();
r2d.Translate(card.x, card.y);         // дальше координаты локальны карточке
r2d.Rotate(crossrender::Radians(5.0f));
r2d.FillRoundedRect({0, 0, card.w, card.h}, 10.0f, crossrender::Color::FromRGB(0x1D2029));
r2d.Restore();                          // преобразование и цвета восстановлены
```

### `void Save()`

Кладёт копию текущего состояния (преобразование, цвета, толщину, отсечение,
альфу, режим смешивания) в стек. Каждому `Save` должен соответствовать
`Restore`.

```cpp
r2d.Save();
r2d.GlobalAlpha(0.5f);
r2d.FillCircle(pulse.x, pulse.y, pulse.r, crossrender::Color::Cyan);
r2d.Restore();
```

### `void Restore()`

Возвращает состояние, сохранённое последним `Save`. Лишний `Restore` на
пустом стеке безопасен и ничего не делает.

```cpp
r2d.Save();
r2d.Composite(crossrender::BlendMode::Additive);
r2d.FillCircle(crossrender.x, crossrender.y, crossrender.r, crossrender::Color{1, 0.9f, 0.5f, 0.4f});
r2d.Restore();   // режим смешивания снова обычный
```

### `void Reset()`

Сбрасывает всё состояние к начальному, очищает стек и текущий путь, а также
сбрасывает накопленную геометрию через `Flush()`. Удобно между независимыми
проходами внутри одного кадра.

```cpp
DrawWorldOverlay(r2d);
r2d.Reset();
DrawDebugConsole(r2d);   // начинаем с чистого состояния
```

### `void ResetTransform()`

Меняет только матрицу преобразования на единичную, не трогая цвета и стек.

```cpp
r2d.ResetTransform();             // возвращаемся в экранные координаты
r2d.DrawText(font, "FPS 60", 12.0f, 12.0f, crossrender::Color::White, 14.0f);
```

### `void ResetState()`

Синоним `Reset()` — сбрасывает и состояние, и стек, и путь.

```cpp
r2d.ResetState();
r2d.FillColor(crossrender::Color::White);   // дальше снова известное состояние
```

### `void GlobalAlpha(f32 alpha)`

Задаёт общую прозрачность для последующих примитивов. Значение ограничивается
диапазоном `[0, 1]` и перемножается с альфой каждого цвета.

```cpp
r2d.GlobalAlpha(0.35f);
r2d.FillRect(dim, crossrender::Color::Black);   // затемнение модального фона
r2d.GlobalAlpha(1.0f);
```

### `f32 GetGlobalAlpha() const`

Возвращает текущую общую прозрачность.

```cpp
const crossrender::f32 previous = r2d.GetGlobalAlpha();
r2d.GlobalAlpha(previous * 0.5f);
DrawGhost(r2d);
r2d.GlobalAlpha(previous);
```

### `void Composite(BlendMode mode)`

Задаёт режим смешивания для последующих примитивов: `Alpha`, `Additive`,
`Multiply`, `Screen`, `Min`, `Max`, `Premultiplied`, `None`, `Opaque`. Режим
входит в ключ пачки, поэтому его смена разрывает вызов отрисовки.

```cpp
r2d.Save();
r2d.Composite(crossrender::BlendMode::Additive);
r2d.FillCircle(spark.x, spark.y, spark.r, crossrender::Color{1.0f, 0.9f, 0.4f, 0.8f});
r2d.Restore();
```

### `void AntiAlias(bool enabled)`

Включает или выключает сглаживание заливок. Обводки и текст сглаживаются
средствами MSAA и на этот флаг не реагируют.

```cpp
// Пиксель-арту нужны жёсткие края.
r2d.AntiAlias(false);
r2d.FillRect(pixelRect, crossrender::Color::FromRGB(0xE4574F));
r2d.AntiAlias(true);
```

### `void LineCap(LineCap cap)`

Задаёт форму концов незамкнутых обводок: `Butt`, `Round`, `Square`.

```cpp
r2d.LineCap(crossrender::LineCap::Round);
r2d.StrokeWidth(10.0f);
r2d.MoveTo(40, 200);
r2d.LineTo(240, 200);
r2d.Stroke();
```

### `void LineJoin(LineJoin join)`

Задаёт форму стыков сегментов обводки: `Miter`, `Round`, `Bevel`.

```cpp
r2d.LineJoin(crossrender::LineJoin::Bevel);
r2d.Polygon(spikes, 5);
r2d.StrokeColor(crossrender::Color::White);
r2d.Stroke();
```

### `void MiterLimit(f32 limit)`

Ограничивает длину острого «уса» при `LineJoin::Miter`: если ус длиннее
`limit` половин толщины, стык срезается. Значение по умолчанию — 4.

```cpp
r2d.LineJoin(crossrender::LineJoin::Miter);
r2d.MiterLimit(2.0f);   // короткие острые углы без длинных шипов
r2d.Polygon(star, 10);
r2d.Stroke();
```

### `void StrokeWidth(f32 width)`

Толщина обводки в логических пикселях. Внутренне ограничивается снизу, чтобы
линия не исчезала при масштабировании.

```cpp
r2d.StrokeColor(crossrender::Color::FromRGB(0x6BA1FF));
r2d.StrokeWidth(3.0f);
r2d.RoundedRect(panel.x, panel.y, panel.w, panel.h, 12.0f);
r2d.Stroke();
```

### `void FillPaint(const Paint& paint)`

Задаёт заливку для `Fill()` и для удобных фигур, которые вызывают `Fill()`
внутри (`FillRoundedRect`, `FillCircle`, `FillEllipse`).

```cpp
r2d.FillPaint(crossrender::Paint::Linear({bar.x, bar.y}, {bar.Right(), bar.y}, accent, crossrender::Color::Black));
r2d.RoundedRect(bar.x, bar.y, bar.w, bar.h, 6.0f);
r2d.Fill();
```

### `void StrokePaint(const Paint& paint)`

Задаёт заливку для `Stroke()`. Градиентная обводка так же допустима, как и
сплошная.

```cpp
r2d.StrokePaint(crossrender::Paint::Linear({0, 0}, {0, 200}, crossrender::Color::Cyan, crossrender::Color::Magenta));
r2d.StrokeWidth(6.0f);
r2d.MoveTo(20, 20);
r2d.LineTo(20, 200);
r2d.Stroke();
```

### `void FillColor(const Color& c)`

Краткая форма для сплошной заливки: эквивалент
`FillPaint(Paint::Solid(c))`.

```cpp
r2d.FillColor(crossrender::Color::FromRGB(0x141821));
r2d.FillRoundedRect(panel, 12.0f);
```

### `void StrokeColor(const Color& c)`

Краткая форма для сплошной обводки: эквивалент
`StrokePaint(Paint::Solid(c))`.

```cpp
r2d.StrokeColor(crossrender::Color{1, 1, 1, 0.25f});
r2d.StrokeWidth(1.0f);
r2d.RoundedRect(panel.x, panel.y, panel.w, panel.h, 12.0f);
r2d.Stroke();
```

### `const Paint& CurrentFill() const`

Возвращает текущую заливку — например, чтобы временно подменить её и вернуть
прежнюю.

```cpp
const crossrender::Paint saved = r2d.CurrentFill();
r2d.FillColor(crossrender::Color::Red);
r2d.FillRect(alertRect, crossrender::Color::Red);
r2d.FillPaint(saved);
```

### `const Paint& CurrentStroke() const`

Возвращает текущую обводку.

```cpp
const crossrender::Paint savedStroke = r2d.CurrentStroke();
r2d.StrokeColor(crossrender::Color::Yellow);
DrawSelection(r2d);
r2d.StrokePaint(savedStroke);
```

### `void Translate(f32 x, f32 y)`

Домножает текущее преобразование на сдвиг. Все последующие координаты
смещаются на `x`, `y`.

```cpp
r2d.Save();
r2d.Translate(panel.x, panel.y);   // рисуем панель в локальных координатах
r2d.FillRect(0, 0, panel.w, 24.0f, headerColor);
r2d.Restore();
```

### `void Rotate(f32 radians)`

Домножает преобразование на поворот вокруг начала координат (в радианах;
для градусов используйте `crossrender::Radians`).

```cpp
r2d.Save();
r2d.Translate(center.x, center.y);
r2d.Rotate(crossrender::Radians(30.0f));
r2d.FillRect(-40.0f, -6.0f, 80.0f, 12.0f, crossrender::Color::White);
r2d.Restore();
```

### `void Scale(f32 x, f32 y)`

Домножает преобразование на масштаб по осям. Отрицательные значения
отражают содержимое; толщина обводки учитывает масштаб.

```cpp
r2d.Save();
r2d.Translate(icon.Center().x, icon.Center().y);
r2d.Scale(pressed ? 0.92f : 1.0f, pressed ? 0.92f : 1.0f);
DrawIcon(r2d, icon);
r2d.Restore();
```

### `void SkewX(f32 radians)`

Наклоняет содержимое по горизонтали (сдвиг верхних строк относительно
нижних). Используется для имитации курсива и «скоростных» полос.

```cpp
r2d.Save();
r2d.SkewX(crossrender::Radians(-12.0f));
r2d.FillRect(speedLine, crossrender::Color{1, 1, 1, 0.35f});
r2d.Restore();
```

### `void SkewY(f32 radians)`

Наклоняет содержимое по вертикали.

```cpp
r2d.Save();
r2d.SkewY(crossrender::Radians(8.0f));
r2d.FillRoundedRect(plate, 6.0f, plateColor);
r2d.Restore();
```

### `void Transform(f32 a, f32 b, f32 c, f32 d, f32 e, f32 f)`

Домножает преобразование на матрицу с элементами `[a b; c d]` и переносом
`(e, f)` — та же раскладка, что у `setTransform` из Canvas.

```cpp
// Нестандартная матрица: небольшой сдвиг и наклон одной командой.
r2d.Transform(1.0f, 0.0f, 0.2f, 1.0f, 0.0f, 0.0f);
r2d.FillRect(0, 0, 200, 40, crossrender::Color::FromRGB(0x2F6FE0));
```

### `Vec2 TransformPoint(const Vec2& p) const`

Переводит точку из локальных координат в экранные текущей матрицей. Полезно
для попадания мышью и для стыковки 2D- и 3D-содержимого.

```cpp
const crossrender::Vec2 screenPos = r2d.TransformPoint({card.w * 0.5f, card.h * 0.5f});
if (crossrender::Rect{screenPos.x - 24.0f, screenPos.y - 24.0f, 48.0f, 48.0f}.Contains(mouse)) {
    r2d.StrokeRoundedRect(card, 10.0f, crossrender::Color::Yellow, 2.0f);
}
```

### Отсечение

Отсечение реализовано через scissor (прямоугольную область отсечения на
уровне GPU). Области вкладываются друг в друга и пересекаются, а вместе с
состоянием их сохраняют `Save`/`Restore`. Прямоугольник задаётся в текущих
локальных координатах и переводится в экранные.

```cpp
r2d.Save();
r2d.ClipRect(list.x, list.y, list.w, list.h);   // область прокрутки
for (const Row& row : rows) DrawRow(r2d, row);
r2d.Restore();                                   // отсечение снято
```

### `void ClipRect(f32 x, f32 y, f32 w, f32 h)`

Сужает текущую область отсечения до пересечения с заданным прямоугольником.
Если отсечение уже включено, новая область только уменьшается.

```cpp
r2d.Save();
r2d.ClipRect(scrollRegion.x, scrollRegion.y, scrollRegion.w, scrollRegion.h);
r2d.Translate(0.0f, -scrollOffset);
DrawInventory(r2d);
r2d.Restore();
```

### `void ClipRoundedRect(f32 x, f32 y, f32 w, f32 h, f32 r)`

Отсечение по скруглённому прямоугольнику. Реализация аппроксимирует его
обычным прямоугольником: параметр `r` принимается, но GPU-ножницы остаются
прямоугольными — для точного скругления обрежьте путь через `ClipPath`.

```cpp
r2d.Save();
r2d.ClipRoundedRect(card.x, card.y, card.w, card.h, cardRadius);
r2d.Image(photo, card.Inset(6.0f));
r2d.Restore();
```

### `void ClipPath()`

Отсекает по текущему пути. Так как ножницы прямоугольные, используется
ограничивающий прямоугольник пути — это документированное приближение.
Вызывайте после построения пути и перед рисованием.

```cpp
r2d.RoundedRect(viewport.x, viewport.y, viewport.w, viewport.h, 16.0f);
r2d.ClipPath();                       // грубое отсечение по габаритам пути
r2d.Image(levelPreview, viewport);
r2d.ResetClip();
```

### `void ResetClip()`

Полностью снимает отсечение (в том числе вложенное), возвращая область
размером с экран. В отличие от `Restore`, не трогает остальное состояние.

```cpp
r2d.ClipRect(0, 0, screen.w, 200.0f);
DrawTopBar(r2d);
r2d.ResetClip();     // дальше рисуем без ограничений
```

### `Rect CurrentClip() const`

Возвращает текущую область отсечения в логических координатах; если
отсечение не включено — прямоугольник размером с экран.

```cpp
const crossrender::Rect clip = r2d.CurrentClip();
if (row.y + row.h < clip.y || row.y > clip.Bottom()) return;   // строка вне видимой области
DrawRow(r2d, row);
```

### Пути и заливка

Путь строится командами `MoveTo`/`LineTo`/`BezierTo`/`QuadTo`/`Arc` и
завершается `Fill` (заливка) или `Stroke` (обводка). Команды накапливают
контуры на CPU; ни одна из них ничего не рисует сама по себе.

```cpp
r2d.BeginPath();
r2d.MoveTo(40.0f, 160.0f);
r2d.BezierTo(90.0f, 60.0f, 190.0f, 260.0f, 240.0f, 160.0f);
r2d.StrokeColor(crossrender::Color::FromRGB(0x6BA1FF));
r2d.StrokeWidth(4.0f);
r2d.Stroke();
```

### `void BeginPath()`

Очищает текущий путь, начиная новый. Предыдущий путь при этом отбрасывается
без рисования.

```cpp
r2d.BeginPath();
r2d.MoveTo(0.0f, 0.0f);
r2d.LineTo(100.0f, 0.0f);
r2d.LineTo(100.0f, 60.0f);
```

### `void MoveTo(f32 x, f32 y)`

Начинает новый подконтур в точке `(x, y)`, разрывая текущий. Если контур не
был замкнут, он остаётся незамкнутым.

```cpp
r2d.BeginPath();
r2d.MoveTo(20.0f, 20.0f);   // зубчатая линия из двух независимых отрезков
r2d.LineTo(60.0f, 8.0f);
r2d.MoveTo(20.0f, 40.0f);
r2d.LineTo(60.0f, 28.0f);
r2d.Stroke();
```

### `void LineTo(f32 x, f32 y)`

Добавляет отрезок от текущей точки до `(x, y)`. Если путь пуст, ведёт себя
как `MoveTo`.

```cpp
r2d.BeginPath();
r2d.MoveTo(0, 0);
for (const crossrender::Vec2& p : polylinePoints) r2d.LineTo(p.x, p.y);
r2d.StrokeColor(crossrender::Color::White);
r2d.Stroke();
```

### `void BezierTo(f32 c1x, f32 c1y, f32 c2x, f32 c2y, f32 x, f32 y)`

Кубическая кривая Безье от текущей точки с двумя контрольными точками.
Кривая разбивается на отрезки адаптивно, с проверкой отклонения от хорды.

```cpp
r2d.BeginPath();
r2d.MoveTo(40.0f, 200.0f);
r2d.BezierTo(80.0f, 90.0f, 200.0f, 310.0f, 260.0f, 200.0f);
r2d.FillPaint(crossrender::Paint::Linear({40, 140}, {260, 260}, crossrender::Color::Cyan, crossrender::Color::Magenta));
r2d.Stroke();
```

### `void QuadTo(f32 cx, f32 cy, f32 x, f32 y)`

Квадратичная кривая Безье с одной контрольной точкой — дешевле кубической и
достаточна для скруглений.

```cpp
r2d.BeginPath();
r2d.MoveTo(0.0f, 40.0f);
r2d.QuadTo(40.0f, 0.0f, 80.0f, 40.0f);   // «горка»
r2d.StrokeWidth(5.0f);
r2d.Stroke();
```

### `void ArcTo(f32 x1, f32 y1, f32 x2, f32 y2, f32 radius)`

Строит дугу, вписанную в угол между текущей точкой и точкой `(x2, y2)` через
промежуточную `(x1, y1)`. Радиус задаёт величину скругления.

```cpp
r2d.BeginPath();
r2d.MoveTo(tab.x, tab.Bottom());
r2d.LineTo(tab.x, tab.y + 8.0f);
r2d.ArcTo(tab.x, tab.y, tab.x + 8.0f, tab.y, 8.0f);   // скруглённый верхний левый угол
r2d.LineTo(tab.Right(), tab.y);
r2d.LineTo(tab.Right(), tab.Bottom());
r2d.ClosePath();
r2d.Fill();
```

### `void Arc(f32 cx, f32 cy, f32 r, f32 a0, f32 a1, Winding dir)`

Добавляет дугу окружности от угла `a0` до `a1`; направление задаёт `dir`
(`Winding::CW` или `Winding::CCW`). Число сегментов подбирается по величине
угла. Если путь пуст, дуга начинается с `MoveTo`.

```cpp
r2d.BeginPath();
r2d.Arc(gauge.Center().x, gauge.Center().y, gauge.w * 0.5f, crossrender::kPi * 0.75f,
        crossrender::kPi * 0.75f + crossrender::kTau * value, crossrender::Winding::CW);
r2d.StrokeColor(crossrender::Color::FromRGB(0x3FBF7F));
r2d.StrokeWidth(8.0f);
r2d.Stroke();
```

### `void ClosePath()`

Замыкает текущий подконтур отрезком в его начало и завершает его. Замкнутые
контуры дают корректные стыки обводки и участвуют в заливке с дырками.

```cpp
r2d.BeginPath();
r2d.MoveTo(0.0f, 0.0f);
r2d.LineTo(80.0f, 0.0f);
r2d.LineTo(40.0f, 70.0f);
r2d.ClosePath();
r2d.FillColor(crossrender::Color::FromRGB(0xF2B33D));
r2d.Fill();
```

### `void PathWinding(Winding dir)`

Задаёт ожидаемое направление обхода следующих контуров. Рендерер всё равно
определяет ориентацию по площади, поэтому это скорее подсказка для
многосоставных путей с отверстиями.

```cpp
r2d.PathWinding(crossrender::Winding::CCW);
r2d.RoundedRect(0, 0, 200, 120, 12.0f);   // внешний контур
r2d.Circle(100, 60, 30.0f);               // отверстие
r2d.Fill();
```

### `void Fill()`

Заливает текущий путь построенной заливкой. Несколько контуров трактуются
как фигура с дырками по правилу ненулевого обхода; вокруг каждой контурной
линии добавляется кайма сглаживания. После заливки путь очищается.

```cpp
r2d.FillPaint(crossrender::Paint::Linear({0, 0}, {0, 120}, crossrender::Color::FromRGB(0x3FBF7F),
                                 crossrender::Color::FromRGB(0x14432F)));
r2d.BeginPath();
r2d.RoundedRect(0, 0, 240, 120, 14.0f);
r2d.Fill();
```

### `void Stroke()`

Обводит текущий путь текущими цветом, толщиной, концами и стыками. Обводка
разворачивается в полосу треугольников; края сглаживаются MSAA. После
обводки путь очищается.

```cpp
r2d.BeginPath();
r2d.MoveTo(path[0].x, path[0].y);
for (crossrender::usize i = 1; i < path.size(); ++i) r2d.LineTo(path[i].x, path[i].y);
r2d.StrokeColor(crossrender::Color{1, 1, 1, 0.8f});
r2d.StrokeWidth(2.0f);
r2d.LineJoin(crossrender::LineJoin::Round);
r2d.Stroke();
```

### Фигуры

Готовые фигуры: одни только строят путь (`Rect`, `RoundedRect`, `Circle`,
`Polygon`, `Spline`) и требуют `Fill`/`Stroke`, другие рисуют сразу, принимая
цвет (`FillRect`, `StrokeRect`, `FillCircle`, `DrawLine`).

```cpp
// Путь + заливка: полный контроль над краской.
r2d.FillPaint(crossrender::Paint::Radial(badge.Center(), 0.0f, badge.w, gold, ember));
r2d.Circle(badge.Center().x, badge.Center().y, badge.w * 0.5f);
r2d.Fill();

// Готовая фигура: одна строка.
r2d.FillRoundedRect(button, 8.0f, crossrender::Color::FromRGB(0x2F6FE0));
```

### `void Rect(f32 x, f32 y, f32 w, f32 h)`

Строит прямоугольный путь (замыкает его). Ширина и высота могут быть
отрицательными — контур просто окажется вывернутым.

```cpp
r2d.StrokeColor(crossrender::Color{1, 1, 1, 0.2f});
r2d.StrokeWidth(1.0f);
r2d.Rect(cell.x, cell.y, cell.w, cell.h);
r2d.Stroke();
```

### `void RoundedRect(f32 x, f32 y, f32 w, f32 h, f32 r)`

Строит прямоугольник со скруглёнными углами. Радиус автоматически
ограничивается половиной меньшей стороны.

```cpp
r2d.FillColor(crossrender::Color::FromRGB(0x1D2029));
r2d.RoundedRect(panel.x, panel.y, panel.w, panel.h, 14.0f);
r2d.Fill();
```

### `void RoundedRectVarying(f32 x, f32 y, f32 w, f32 h, f32 tl, f32 tr, f32 br, f32 bl)`

Скруглённый прямоугольник с независимыми радиусами углов: левый верхний,
правый верхний, правый нижний, левый нижний. Годится для «пузырей» и вкладок.

```cpp
// Вкладка со скруглённым верхом и прямым низом.
r2d.FillColor(tabColor);
r2d.RoundedRectVarying(tab.x, tab.y, tab.w, tab.h, 10.0f, 10.0f, 0.0f, 0.0f);
r2d.Fill();
```

### `void Ellipse(f32 cx, f32 cy, f32 rx, f32 ry)`

Строит замкнутый эллипс с радиусами `rx` и `ry` (сорок восемь сегментов).

```cpp
r2d.FillColor(crossrender::Color{1, 1, 1, 0.08f});
r2d.Ellipse(floorPoint.x, floorPoint.y, 60.0f, 18.0f);
r2d.Fill();
```

### `void Circle(f32 cx, f32 cy, f32 r)`

Строит замкнутую окружность — частный случай эллипса.

```cpp
r2d.StrokeColor(crossrender::Color::Cyan);
r2d.StrokeWidth(2.0f);
r2d.Circle(target.x, target.y, target.r);
r2d.Stroke();
```

### `void Polyline(const Vec2* pts, int count)`

Строит незамкнутый путь по массиву точек (минимум две). Пустой указатель или
меньшее число точек игнорируется.

```cpp
const crossrender::Vec2 trail[5] = {{0, 0}, {30, -12}, {60, 6}, {90, -4}, {120, 0}};
r2d.StrokeColor(crossrender::Color::FromRGB(0xFF8AC4));
r2d.StrokeWidth(3.0f);
r2d.Polyline(trail, 5);
r2d.Stroke();
```

### `void Polygon(const Vec2* pts, int count)`

Строит замкнутый многоугольник по массиву точек (минимум три). Ориентация
обхода определяется по площади.

```cpp
const crossrender::Vec2 hex[6] = {{0, -30}, {26, -15}, {26, 15}, {0, 30}, {-26, 15}, {-26, -15}};
r2d.FillPaint(crossrender::Paint::Linear({-26, -30}, {26, 30}, crossrender::Color::FromRGB(0x35C7D6),
                                 crossrender::Color::FromRGB(0xA86BFF)));
r2d.Polygon(hex, 6);
r2d.Fill();
```

### `void Spline(const Vec2* pts, int count, f32 tension = 0.5f)`

Строит сглаженный открытый путь Catmull-Rom через точки. `tension` управляет
«натяжением»: меньше — ближе к ломаной, больше — плавнее. При числе точек
меньше трёх вырождается в `Polyline`.

```cpp
std::vector<crossrender::Vec2> samples = SampleWave(9);
r2d.LineCap(crossrender::LineCap::Round);
r2d.LineJoin(crossrender::LineJoin::Round);
r2d.StrokePaint(crossrender::Paint::Linear({0, 0}, {320, 0}, crossrender::Color::Cyan, crossrender::Color::Magenta));
r2d.StrokeWidth(3.0f);
r2d.Spline(samples.data(), static_cast<int>(samples.size()), 0.5f);
r2d.Stroke();
```

### `void FillRect(f32 x, f32 y, f32 w, f32 h, const Color& c)`

Самая дешёвая команда: сразу рисует прямоугольник заданным цветом. При
включённом сглаживании строится четырёхвершинный прямоугольник с каймой.
Есть перегрузка `FillRect(const Rect& r, const Color& c)`.

```cpp
r2d.FillRect(screen.x, screen.y, screen.w, 48.0f, crossrender::Color::FromRGB(0x101527));
r2d.FillRect(crossrender::Rect{0, 0, 32, 32}, crossrender::Color::FromRGB(0xE4574F));
```

### `void FillRoundedRect(const Rect& r, f32 radius, const Color& c)`

Строит скруглённый прямоугольник и сразу заливает его сплошным цветом.
Обратите внимание: перегрузки с координатами нет — принимается только `Rect`.

```cpp
r2d.FillRoundedRect(crossrender::Rect{panel.x, panel.y, panel.w, panel.h}, 12.0f,
                    crossrender::Color::FromRGB(0x1A1F2B));
```

### `void StrokeRect(const Rect& r, const Color& c, f32 width = 1.0f)`

Рисует рамку по границам прямоугольника. Толщина по умолчанию — 1 пиксель.

```cpp
r2d.StrokeRect(crossrender::Rect{slot.x, slot.y, slot.w, slot.h}, crossrender::Color{1, 1, 1, 0.2f});
r2d.StrokeRect(crossrender::Rect{slot.x - 2, slot.y - 2, slot.w + 4, slot.h + 4}, crossrender::Color::Yellow, 2.0f);
```

### `void StrokeRoundedRect(const Rect& r, f32 radius, const Color& c, f32 width = 1.0f)`

Рисует рамку скруглённого прямоугольника — типичная обводка кнопки.

```cpp
r2d.StrokeRoundedRect(button, 8.0f, crossrender::Color{1, 1, 1, 0.35f}, 1.5f);
```

### `void FillCircle(f32 cx, f32 cy, f32 r, const Color& c)`

Рисует залитый круг — точки, маркеры, частицы.

```cpp
for (const crossrender::Vec2& p : waypoints) r2d.FillCircle(p.x, p.y, 4.0f, crossrender::Color::FromRGB(0xF2B33D));
```

### `void FillEllipse(f32 cx, f32 cy, f32 rx, f32 ry, const Color& c)`

Рисует залитый эллипс.

```cpp
r2d.FillEllipse(unit.Pos().x, unit.Pos().y + 6.0f, 18.0f, 6.0f, crossrender::Color{0, 0, 0, 0.35f});
```

### `void DrawLine(f32 x0, f32 y0, f32 x1, f32 y1, const Color& c, f32 width = 1.0f)`

Рисует один отрезок сплошным цветом. Это не обводка пути: концы всегда
прямые, настройки `LineCap` не применяются. Толщина по умолчанию — 1.

```cpp
r2d.DrawLine(grid.x, grid.y, grid.Right(), grid.y, crossrender::Color{1, 1, 1, 0.08f});
r2d.DrawLine(0, 0, 100, 100, crossrender::Color::Red, 3.0f);
```

### Градиенты

Два готовых помощника для прямоугольных градиентов. Для произвольных фигур
используйте `FillPaint(Paint::Linear(...))` и `Fill()` — эти методы лишь
избавляют от лишних строк для частого случая.

```cpp
// Вертикальный градиент карточки и диагональный градиент баннера.
r2d.FillRectGradient(card, crossrender::Color::FromRGB(0x232A3A), crossrender::Color::FromRGB(0x141821));
r2d.FillRectGradient(banner, crossrender::Color::FromRGB(0x6BA1FF), crossrender::Color::FromRGB(0xA86BFF), false);
```

### `void FillRectGradient(const Rect& r, const Color& a, const Color& b, bool vertical = true)`

Заливает прямоугольник линейным градиентом от цвета `a` к цвету `b`. При
`vertical = true` (по умолчанию) переход идёт сверху вниз, иначе слева
направо.

```cpp
r2d.FillRectGradient(healthBar, crossrender::Color::FromRGB(0xE4574F), crossrender::Color::FromRGB(0x7A1F1A));
r2d.FillRectGradient(titleBar, crossrender::Color::FromRGB(0x6BA1FF), crossrender::Color::FromRGB(0x101527), false);
```

### `void FillRectGradient4(const Rect& r, const Color& tl, const Color& tr, const Color& br, const Color& bl)`

Четырёхцветный градиент: у каждого угла свой цвет, переходы интерполируются
по вершинам. Полезен для «кожаных» и «металлических» плашек.

```cpp
r2d.FillRectGradient4(plate, crossrender::Color::FromRGB(0x4A5568), crossrender::Color::FromRGB(0x2D3748),
                      crossrender::Color::FromRGB(0x1A202C), crossrender::Color::FromRGB(0x718096));
```

### Изображения

Вывод текстур: прямоугольником, произвольным четырёхугольником, девятипатчевой
растяжкой и с разным цветом по углам. Все варианты берут UV-координаты в
нормированном виде (0..1), а `tint` перемножается с цветом текстуры и
умножается на `GlobalAlpha`.

```cpp
r2d.Image(icon, crossrender::Rect{16, 16, 32, 32}, crossrender::Rect{0, 0, 1, 1}, crossrender::Color{1, 1, 1, 0.7f});
r2d.Image9(panelTex, panel, crossrender::NinePatch::Uniform(12.0f));
```

### `void Image(const Texture& tex, const Rect& dst, const Rect& srcUV = {0, 0, 1, 1}, const Color& tint = Color::White, f32 cornerRadius = 0.0f)`

Рисует текстуру в прямоугольник `dst`; `srcUV` задаёт вырезаемый кусок
атласа, `tint` — множитель цвета. Если `cornerRadius > 0`, изображение
обрезается по скруглённому прямоугольнику (внутри используется
`Save`/`ClipPath`/`Restore`). Невалидная текстура молча пропускается.

```cpp
r2d.Image(avatar, crossrender::Rect{24, 24, 64, 64}, crossrender::Rect{0, 0, 1, 1}, crossrender::Color::White, 32.0f);
r2d.Image(atlas, crossrender::Rect{96, 24, 32, 32}, crossrender::Rect{0.25f, 0.0f, 0.25f, 0.5f});
```

### `void ImageQuad(const Texture& tex, const Vec2 quad[4], const Rect& srcUV, const Color& tint)`

Рисует текстуру в произвольный четырёхугольник (по часовой стрелке от
левого верхнего угла). Позволяет делать перспективные и скошенные спрайты.

```cpp
const crossrender::Vec2 quad[4] = {{120, 40}, {280, 60}, {260, 200}, {140, 180}};
r2d.ImageQuad(photo, quad, crossrender::Rect{0, 0, 1, 1}, crossrender::Color{1, 1, 1, 0.9f});
```

### `void Image9(const Texture& tex, const Rect& dst, const NinePatch& patch, const Color& tint = Color::White, f32 scale = 1.0f)`

Растягивает текстуру как nine-patch: углы сохраняют размер, края и центр
тянутся. `scale` домножает отступы `patch` — удобно для HiDPI-ресурсов.
Слишком большой отступ автоматически уменьшается, чтобы части не наложились.

```cpp
const crossrender::NinePatch frame = crossrender::NinePatch::Uniform(12.0f);
r2d.Image9(buttonTex, button, frame, crossrender::Color::White, r2d.DpiScale());
```

### `void ImageTinted4(const Texture& tex, const Rect& dst, const Rect& srcUV, const Color& tl, const Color& tr, const Color& br, const Color& bl)`

Рисует текстуру с разным цветом-множителем в каждом углу: получается
градиентная подкраска одной картинки.

```cpp
r2d.ImageTinted4(white, rect, crossrender::Rect{0, 0, 1, 1}, crossrender::Color::FromRGB(0x6BA1FF),
                 crossrender::Color::FromRGB(0x35C7D6), crossrender::Color::FromRGB(0xA86BFF),
                 crossrender::Color::FromRGB(0x101527));
```

### Текст

Текст рисуется из атласа шрифта: для растровых шрифтов — как альфа-спрайты
через общий шейдер, для SDF-шрифтов — через отдельный шейдер расстояний. Оба
варианта используют один и тот же вершинный буфер. Если `size` равен нулю,
берётся `FontDesc::pixelHeight` шрифта.

```cpp
crossrender::Font* font = crossrender::FontManager::Get().DefaultFont();
if (font && font->Valid()) {
    r2d.DrawText(*font, "Здоровье", panel.x + 12.0f, panel.y + 10.0f, label, 16.0f);
    r2d.DrawText(*font, "100 / 100", panel.Right() - 12.0f, panel.y + 10.0f, crossrender::Color::White,
                 16.0f, crossrender::TextAlign::Right);
}
```

### `void DrawText(const Font& font, const std::string& utf8, f32 x, f32 y, const Color& color, f32 size = 0.0f, TextAlign align = TextAlign::Left, TextBaseline baseline = TextBaseline::Top, f32 letterSpacing = 0.0f)`

Основной метод вывода строки UTF-8. `align` и `baseline` определяют, чем
считается точка `(x, y)`; `letterSpacing` добавляет трекинг. Учитывается
кернинг. Возвращаемого значения нет, ширину заранее даёт `MeasureText`.

```cpp
const crossrender::TextMetrics m = crossrender::MeasureText(*font, title, 24.0f);
r2d.DrawText(*font, title, (screen.w - m.width) * 0.5f, 24.0f, crossrender::Color::White, 24.0f,
             crossrender::TextAlign::Left, crossrender::TextBaseline::Top);
```

### `void DrawTextRotated(const Font& font, const std::string& utf8, Vec2 pos, const Color& color, f32 size, f32 rotation, TextAlign align = TextAlign::Center, TextBaseline baseline = TextBaseline::Middle)`

Рисует строку, повёрнутую на `rotation` радиан вокруг точки `pos` (она
временно становится началом координат). Выравнивание по умолчанию —
по центру и по середине строки.

```cpp
r2d.DrawTextRotated(*font, "вертикальная подпись", {axisX, axisY}, crossrender::Color::White, 14.0f,
                    -crossrender::kPi * 0.5f);
```

### `int DrawTextBox(const Font& font, const std::string& utf8, const Rect& box, const Color& color, f32 size = 0.0f, TextBreak brk = TextBreak::Word, f32 lineHeightMul = 1.2f, TextAlign align = TextAlign::Left, int maxLines = 0)`

Переносит текст по ширине `box` и рисует строки сверху вниз, начиная от
верхнего края рамки. Возвращает число нарисованных строк. `maxLines = 0`
означает «без ограничения», текст ниже рамки не рисуется.

```cpp
const crossrender::Rect body{panel.x + 16.0f, panel.y + 48.0f, panel.w - 32.0f, 140.0f};
const int lines = r2d.DrawTextBox(*font, description, body, crossrender::Color{0.85f, 0.88f, 0.95f, 1.0f},
                                  16.0f, crossrender::TextBreak::Word, 1.35f, crossrender::TextAlign::Left, 0);
ENG_LOGD("ui", "в описании %d строк", lines);
```

### `void DrawTextOutline(const Font& font, const std::string& utf8, f32 x, f32 y, const Color& fill, const Color& outline, f32 outlineWidth, f32 size = 0.0f, TextAlign align = TextAlign::Left, TextBaseline baseline = TextBaseline::Top)`

Рисует текст с обводкой: сначала контур, затем заливка. Для SDF-шрифта контур
аналитический, для растрового — восемь смещённых копий.

```cpp
r2d.DrawTextOutline(*font, "ПАУЗА", screen.w * 0.5f, screen.h * 0.5f, crossrender::Color::White,
                    crossrender::Color::FromRGB(0x101527), 4.0f, 48.0f, crossrender::TextAlign::Center,
                    crossrender::TextBaseline::Middle);
```

### `void DrawTextStyled(const Font& font, const std::string& utf8, Vec2 pos, f32 size, const TextStyle& style)`

Единая точка входа всего богатого текста: заливка и градиенты, обводка,
свечение, тень, наклон, скручивание, волна, масштаб и многострочность из
`TextStyle`. `pos` — точка привязки, описываемая `style.align` и
`style.baseline`. Поддерживает переводы строк `\n`.

```cpp
crossrender::TextStyle title = crossrender::TextStyle::GradientText(crossrender::Color::FromRGB(0xFFD36E),
                                                    crossrender::Color::FromRGB(0xE4574F));
title.align = crossrender::TextAlign::Center;
title.baseline = crossrender::TextBaseline::Top;
title.outlineColor = crossrender::Color::FromRGB(0x2A1B08);
title.outlineWidth = 2.0f;
r2d.DrawTextStyled(*font, "ПОБЕДА", {screen.w * 0.5f, 80.0f}, 52.0f, title);
```

### `int DrawTextBoxStyled(const Font& font, const std::string& utf8, const Rect& box, f32 size, const TextStyle& style)`

Многострочный богатый текст внутри рамки: перенос берётся из `style.wrap`
(если там `None`, используется `Word`), число строк ограничивает
`style.maxLines`. Возвращает число нарисованных строк.

```cpp
crossrender::TextStyle body = crossrender::TextStyle::Filled(crossrender::Color{0.85f, 0.88f, 0.95f, 1.0f});
body.wrap = crossrender::TextBreak::Word;
body.maxLines = 6;
body.lineHeightMul = 1.4f;
r2d.DrawTextBoxStyled(*font, lore, textBox, 16.0f, body);
```

### `void DrawTextOnPath(const Font& font, const std::string& utf8, const Vec2* path, int pathCount, f32 size, const TextStyle& style, f32 offset = 0.0f, bool closed = false)`

Раскладывает текст вдоль ломаной: глифы поворачиваются по касательной, шаг
берётся по длине дуги. `offset` позволяет начать не с начала пути, `closed`
замыкает путь в кольцо. Пробелы сохраняют ширину.

```cpp
const crossrender::Vec2 arc[5] = {{40, 300}, {120, 260}, {200, 260}, {280, 300}, {320, 360}};
crossrender::TextStyle ribbon = crossrender::TextStyle::Filled(crossrender::Color::FromRGB(0x6BA1FF));
r2d.DrawTextOnPath(*font, "текст по кривой", arc, 5, 20.0f, ribbon);
```

### `void DrawTextOnArc(const Font& font, const std::string& utf8, Vec2 center, f32 radius, f32 startAngle, f32 size, const TextStyle& style, bool outside = true, bool clockwise = false)`

Раскладывает текст по дуге окружности, центрируя его относительно
`startAngle`. `outside` ставит верх глифов наружу, `clockwise` меняет
направление. Радиус меньше половины пикселя игнорируется.

```cpp
// Надпись по верхней дуге печати.
r2d.DrawTextOnArc(*font, "ГИЛЬДИЯ КУЗНЕЦОВ", seal.Center(), seal.w * 0.5f, -crossrender::kPi * 0.5f,
                  18.0f, crossrender::TextStyle::Filled(crossrender::Color::FromRGB(0xFFD36E)));
```

### `void DrawTextShadow(const Font& font, const std::string& utf8, Vec2 pos, f32 size, const Color& fill, const Color& shadow = Color{0, 0, 0, 0.6f}, Vec2 offset = {2, 3}, TextAlign align = TextAlign::Left, TextBaseline baseline = TextBaseline::Top)`

Удобная обёртка над `DrawTextStyled` для заливки с тенью.

```cpp
r2d.DrawTextShadow(*font, "Настройки", {40.0f, 40.0f}, 28.0f, crossrender::Color::White,
                   crossrender::Color{0, 0, 0, 0.6f}, {2.0f, 3.0f});
```

### `void DrawTextTwisted(const Font& font, const std::string& utf8, Vec2 pos, f32 size, const Color& fill, f32 twist, f32 rotation = 0.0f, f32 waveAmplitude = 0.0f)`

Закручивает строку: каждый следующий глиф дополнительно поворачивается на
`twist` радиан за пиксель продвижения, `waveAmplitude` добавляет волну, а
`rotation` наклоняет всю строку целиком.

```cpp
r2d.DrawTextTwisted(*font, "в вихре", {120.0f, 200.0f}, 30.0f, crossrender::Color::FromRGB(0xA86BFF),
                    0.01f, 0.0f, 6.0f);
```

### `void DrawTextGradient(const Font& font, const std::string& utf8, Vec2 pos, f32 size, const Color& inner, const Color& outer, TextAlign align = TextAlign::Left, TextBaseline baseline = TextBaseline::Top)`

Текст с вертикальным градиентом от `inner` (верх глифа) к `outer` (низ).

```cpp
r2d.DrawTextGradient(*font, "ЛЕГЕНДА", {screen.w * 0.5f, screen.h - 64.0f}, 44.0f,
                     crossrender::Color::FromRGB(0xFFF3B0), crossrender::Color::FromRGB(0xE09B2D),
                     crossrender::TextAlign::Center, crossrender::TextBaseline::Top);
```

### Измерение текста

Раскладка текста не зависит от состояния рендерера: измерители возвращают
размеры заранее, чтобы вы могли выровнять элементы до рисования. Свободные
функции работают с чистым текстом, `MeasureStyledText` учитывает эффекты
`TextStyle`.

```cpp
crossrender::Font* font = crossrender::FontManager::Get().DefaultFont();
if (font && font->Valid()) {
    const crossrender::TextMetrics m = crossrender::MeasureText(*font, caption, 15.0f);
    const crossrender::f32 x = box.Right() - m.width - 8.0f;   // прижимаем подпись вправо
    r2d.DrawText(*font, caption, x, box.y + 6.0f, crossrender::Color::White, 15.0f);
}
```

### `Renderer2D::StyledMetrics`

Результат `MeasureStyledText`: `width` и `height` — габариты с учётом
масштаба стиля, `advance` — ширина без масштаба, `glyphCount` и `lineCount` —
число глифов и строк, `bounds` — плотные чернильные границы в локальных
координатах с началом в точке привязки.

```cpp
const crossrender::Renderer2D::StyledMetrics m = r2d.MeasureStyledText(*font, title, 28.0f, style);
r2d.FillRoundedRect(crossrender::Rect{m.bounds.x - 8.0f, m.bounds.y - 4.0f, m.bounds.w + 16.0f,
                              m.bounds.h + 8.0f},
                    6.0f, crossrender::Color{0, 0, 0, 0.45f});
```

### `StyledMetrics MeasureStyledText(const Font& font, const std::string& utf8, f32 size, const TextStyle& style)`

Считает размеры стилизованного текста: учитывает межбуквенный интервал,
масштаб, переносы строк и многострочность. Не рисует ничего, поэтому годится
для раскладки до рисования.

```cpp
const crossrender::Renderer2D::StyledMetrics box = r2d.MeasureStyledText(*font, "ГОТОВ", 22.0f, buttonStyle);
const crossrender::Rect pill{0, 0, box.width + 32.0f, box.height + 16.0f};
r2d.FillRoundedRect(pill, pill.h * 0.5f, crossrender::Color::FromRGB(0x3FBF7F));
```

### `TextMetrics`

Результат `MeasureText`: `width` и `height` — размеры блока, `ascender` и
`descender` — выносные элементы в пикселях для данного размера, `lineCount` —
число строк (по символам `\n`), `glyphCount` — число учтённых глифов.

```cpp
const crossrender::TextMetrics m = crossrender::MeasureText(*font, chatLine, 14.0f);
const crossrender::f32 bubbleH = m.height + 10.0f;   // высота пузыря по строке
r2d.FillRoundedRect(crossrender::Rect{x, y, MaxT(120.0f, m.width + 16.0f), bubbleH}, 8.0f, bubbleColor);
```

### `TextMetrics MeasureText(const Font& font, const std::string& utf8, f32 size = 0.0f, f32 letterSpacing = 0.0f)`

Измеряет размеры строки до рисования: ширина — наибольшая из строк, высота —
высота одной строки. `letterSpacing` должен совпадать с тем, что будет
передан в `DrawText`, иначе выравнивание разъедется.

```cpp
const crossrender::TextMetrics one = crossrender::MeasureText(*font, "Привет", 18.0f);
const crossrender::TextMetrics spaced = crossrender::MeasureText(*font, "Привет", 18.0f, 2.0f);
ENG_LOGI("ui", "ширина %.1f -> %.1f при трекинге 2 px", one.width, spaced.width);
```

### `std::vector<std::string> WrapText(const Font& font, const std::string& utf8, f32 maxWidth, f32 size = 0.0f, TextBreak brk = TextBreak::Word)`

Разбивает текст на строки по ширине `maxWidth` (логические единицы).
`TextBreak::Word` переносит по словам, `TextBreak::Char` — по символам,
`TextBreak::None` вообще не переносит абзац. Символы `\n` задают новые
абзацы. Возвращает готовые строки UTF-8 — их удобно кэшировать.

```cpp
const std::vector<std::string> lines = crossrender::WrapText(*font, questText, log.w - 24.0f, 15.0f);
for (crossrender::usize i = 0; i < lines.size(); ++i) {
    r2d.DrawText(*font, lines[i], log.x + 12.0f, log.y + 8.0f + static_cast<crossrender::f32>(i) * 19.0f,
                 crossrender::Color::White, 15.0f);
}
```

### `std::string EllipsizeText(const Font& font, const std::string& utf8, f32 maxWidth, f32 size = 0.0f)`

Обрезает строку и добавляет многоточие `...` так, чтобы результат помещался в
`maxWidth`. Если строка и так помещается, возвращается она же. Разрезает по
границам кодовых точек UTF-8.

```cpp
const std::string shortName = crossrender::EllipsizeText(*font, item.fullName, slot.w - 16.0f, 14.0f);
r2d.DrawText(*font, shortName, slot.x + 8.0f, slot.y + 4.0f, crossrender::Color::White, 14.0f);
```

### `usize TextIndexAt(const Font& font, const std::string& utf8, f32 x, f32 size = 0.0f)`

Возвращает байтовое смещение в строке для кодовой точки, ближайшей к
локальному смещению `x`. Нужен для установки каретки и выделения по клику.
Если `x` правее конца строки, возвращается её длина.

```cpp
const crossrender::usize caret = crossrender::TextIndexAt(*font, input.text, mouse.x - input.box.x, 16.0f);
const crossrender::f32 caretX = crossrender::MeasureText(*font, input.text.substr(0, caret), 16.0f).width;
r2d.DrawLine(input.box.x + caretX, input.box.y, input.box.x + caretX, input.box.Bottom(),
             crossrender::Color::White, 2.0f);
```

### Статистика и состояние кадра

Счётчики кадра и служебные сведения о координатах. `GetStats()` обнуляется в
`BeginFrame` и наполняется в `Flush`, поэтому читайте его после `EndFrame`;
до этого момента число неотправленных вершин даёт `PendingVertices()`.

```cpp
r2d.BeginFrame(fbW, fbH, dpi);
DrawHud(r2d);
const int pending = r2d.PendingVertices();   // ещё не отправлено
r2d.EndFrame();

const crossrender::Renderer2D::Stats& s = r2d.GetStats();
ENG_LOGI("perf", "%d вызовов, %d треугольников, %d путей, %d глифов", s.drawCalls, s.vertices,
         s.paths, s.textGlyphs);
```

### `Renderer2D::Stats`

Счётчики текущего кадра: `drawCalls` — число реально выполненных вызовов
отрисовки, `vertices` — число **треугольников** (сумма индексов всех вызовов,
делённая на три: имя поля историческое), `paths` — сколько раз запускались
заливка/обводка/готовая фигура, `clipPushes` — сколько раз включалось
отсечение, `textGlyphs` — число нарисованных глифов, `vertexBufferBytes` —
новая ёмкость динамического вершинного буфера, если в этом кадре он
расширялся (иначе ноль, потому что счётчики сбрасываются в `BeginFrame`).

```cpp
const crossrender::Renderer2D::Stats& s = r2d.GetStats();
if (s.drawCalls > 200) {
    ENG_LOGW("perf", "много вызовов отрисовки: %d, проверьте порядок текстур", s.drawCalls);
}
```

### `const Stats& GetStats() const`

Возвращает ссылку на счётчики кадра. Значение имеет смысл только после
`EndFrame`, потому что `Flush` выполняется именно там.

```cpp
r2d.EndFrame();
const crossrender::Renderer2D::Stats& s = r2d.GetStats();
debugOverlay << "draw calls: " << s.drawCalls << ", glyphs: " << s.textGlyphs;
```

### `void ResetStats()`

Обнуляет счётчики, не трогая состояние и геометрию. Нужен, чтобы измерить
стоимость отдельного участка кадра.

```cpp
r2d.ResetStats();
DrawMinimap(r2d);
const int mapCalls = r2d.GetStats().drawCalls;
ENG_LOGD("perf", "миникарта: %d вызовов", mapCalls);
```

### `int PendingVertices() const`

Число вершин, накопленных в буфере и ещё не отправленных в GPU. Полезно для
отладки батчинга и для контроля переполнения буфера до `Flush`.

```cpp
if (r2d.PendingVertices() > 400000) {
    ENG_LOGW("perf", "буфер 2D почти полон, часть геометрии может быть потеряна");
    r2d.Flush();
}
```

### `const Mat4& Projection() const`

Возвращает матрицу проекции текущего кадра (ортографическая проекция
логического пространства). Нужна, если вы вручную рисуете геометрию тем же
шейдером или переводите координаты.

```cpp
const crossrender::Mat4& proj = r2d.Projection();
ENG_LOGD("r2d", "проекция кадра: [0][0] = %.4f", proj.at(0, 0));
```

### `f32 DpiScale() const`

Возвращает масштаб логических единиц к физическим пикселям, переданный в
`BeginFrame`. Используйте его для ресурсов, которые должны оставаться
резкими на HiDPI-экранах.

```cpp
const crossrender::f32 dpi = r2d.DpiScale();
r2d.Image9(buttonTex, button, crossrender::NinePatch::Uniform(12.0f), crossrender::Color::White, dpi);
```

### `Vec2 ScreenSize() const`

Логический размер кадра: `fbWidth / dpiScale` на `fbHeight / dpiScale`.
Именно в этих координатах удобно центрировать элементы.

```cpp
const crossrender::Vec2 screen = r2d.ScreenSize();
r2d.DrawText(*font, "ПАУЗА", screen.x * 0.5f, screen.y * 0.5f, crossrender::Color::White, 48.0f,
             crossrender::TextAlign::Center, crossrender::TextBaseline::Middle);
```

### `void SetViewportBlit(const Texture& colorTex, const crossrender::Rect& dst)`

Аварийный выход для смешивания 3D и 2D: выводит цветовую текстуру
(например, результат `Renderer3D`) в заданный прямоугольник текущего кадра.
Применяется, чтобы наложить 2D-интерфейс поверх 3D-сцены без отдельного
прохода.

```cpp
// Сцена отрисована в Renderer3D и лежит в его цветовой текстуре.
r2d.SetViewportBlit(viewportColor, crossrender::Rect{0, 0, screen.w, screen.h});
r2d.DrawText(*font, "ESC — меню", 16.0f, screen.h - 24.0f, crossrender::Color::White, 14.0f);
```

## Пример целиком

Небольшая HUD-сцена: панель с заголовком и прогресс-баром, кнопка,
прокручиваемая область с журналом, градиентная карточка и путь-индикатор.
Обратите внимание на порядок: сначала `Init`, затем кадр, а статистика
читается только после `EndFrame`.

```cpp
#include "crossrender/core/Log.h"
#include "crossrender/gfx/Renderer2D.h"
#include "crossrender/text/Font.h"

#include <string>
#include <vector>

namespace {

struct PlayerState {
    std::string name = "Странник";
    float health = 0.72f;
    std::vector<std::string> log;
};

// Заголовок панели: сначала измеряем строку, потом раскладываем её по центру.
void DrawHeader(crossrender::Renderer2D& r2d, crossrender::Font& font, const crossrender::Rect& panel,
                const std::string& title) {
    const crossrender::TextMetrics m = crossrender::MeasureText(font, title, 22.0f);
    const crossrender::f32 titleX = panel.Center().x - m.width * 0.5f;
    r2d.DrawText(font, title, titleX, panel.y + 12.0f, crossrender::Color::White, 22.0f);
}

// Прогресс-бар: тёмный желоб, обрезанная градиентная заливка и подпись.
void DrawProgressBar(crossrender::Renderer2D& r2d, crossrender::Font& font, const crossrender::Rect& bar, float value) {
    r2d.FillRoundedRect(bar, bar.h * 0.5f, crossrender::Color{0, 0, 0, 0.45f});
    const crossrender::Rect fill{bar.x, bar.y, bar.w * crossrender::Clamp(value, 0.0f, 1.0f), bar.h};
    r2d.Save();
    r2d.ClipRoundedRect(bar.x, bar.y, bar.w, bar.h, bar.h * 0.5f);
    r2d.FillRectGradient(fill, crossrender::Color::FromRGB(0x3FBF7F), crossrender::Color::FromRGB(0xF2B33D), false);
    r2d.Restore();
    const std::string percent = std::to_string(static_cast<int>(value * 100.0f + 0.5f)) + "%";
    r2d.DrawText(font, percent, bar.Center().x, bar.Center().y, crossrender::Color::White, 13.0f,
                 crossrender::TextAlign::Center, crossrender::TextBaseline::Middle);
}

// Скруглённая кнопка с меняющейся подсветкой.
void DrawButton(crossrender::Renderer2D& r2d, crossrender::Font& font, const crossrender::Rect& button, bool hot) {
    const crossrender::Color base = hot ? crossrender::Color::FromRGB(0x7FB2FF) : crossrender::Color::FromRGB(0x2F6FE0);
    r2d.FillRoundedRect(button, 10.0f, base);
    r2d.StrokeRoundedRect(button, 10.0f, crossrender::Color{1, 1, 1, hot ? 0.6f : 0.25f}, 1.5f);
    r2d.DrawText(font, "Продолжить", button.Center().x, button.Center().y, crossrender::Color::White, 18.0f,
                 crossrender::TextAlign::Center, crossrender::TextBaseline::Middle);
}

// Прокручиваемый журнал: отсечение по области и сдвиг содержимого.
void DrawLog(crossrender::Renderer2D& r2d, crossrender::Font& font, const crossrender::Rect& region,
             const std::vector<std::string>& lines, float scroll) {
    r2d.Save();
    r2d.ClipRect(region.x, region.y, region.w, region.h);
    r2d.Translate(0.0f, -scroll);
    crossrender::f32 y = region.y + 10.0f;
    for (const std::string& line : lines) {
        const std::string shortLine = crossrender::EllipsizeText(font, line, region.w - 20.0f, 15.0f);
        r2d.DrawText(font, shortLine, region.x + 10.0f, y, crossrender::Color{0.85f, 0.88f, 0.95f, 1.0f},
                     15.0f);
        y += 20.0f;
    }
    r2d.Restore();
    // Мягкое затухание у нижнего края, нарисованное уже без отсечения.
    r2d.FillRectGradient(crossrender::Rect{region.x, region.Bottom() - 24.0f, region.w, 24.0f},
                         crossrender::Color{0.06f, 0.07f, 0.10f, 1.0f}, crossrender::Color{0.06f, 0.07f, 0.10f, 0.0f});
}

// Карточка с линейным градиентом и путём-индикатором.
void DrawCard(crossrender::Renderer2D& r2d, const crossrender::Rect& card, const crossrender::Color& accent) {
    r2d.FillPaint(crossrender::Paint::Linear(card.Min(), card.Max(), accent,
                                     crossrender::Color{0.05f, 0.06f, 0.10f, 0.9f}));
    r2d.RoundedRect(card.x, card.y, card.w, card.h, 12.0f);
    r2d.Fill();

    r2d.BeginPath();
    r2d.MoveTo(card.x + 12.0f, card.Bottom() - 20.0f);
    r2d.BezierTo(card.x + card.w * 0.35f, card.y + card.h * 0.35f, card.x + card.w * 0.65f,
                 card.Bottom() - 4.0f, card.Right() - 12.0f, card.Bottom() - 20.0f);
    r2d.StrokeColor(crossrender::Color{1, 1, 1, 0.75f});
    r2d.StrokeWidth(2.5f);
    r2d.LineCap(crossrender::LineCap::Round);
    r2d.Stroke();
}

}  // namespace

int main() {
    crossrender::Renderer2D r2d;
    if (!r2d.Init()) {
        ENG_LOGE("demo", "не удалось инициализировать 2D-рендерер");
        return 1;
    }

    crossrender::Font* font = crossrender::FontManager::Get().DefaultFont();
    if (!font || !font->Valid()) {
        ENG_LOGW("demo", "шрифт по умолчанию недоступен, текст не рисуется");
        r2d.Shutdown();
        return 1;
    }

    PlayerState player;
    player.log = {"Вход в подземелье", "Найден факел", "Здоровье восстановлено",
                  "Получен ключ от ворот"};

    const int fbWidth = 1280;
    const int fbHeight = 800;

    r2d.BeginFrame(fbWidth, fbHeight, 1.0f);
    const crossrender::Vec2 screen = r2d.ScreenSize();

    // Панель HUD: заливка сплошным цветом и рамка.
    const crossrender::Rect panel{24.0f, 24.0f, screen.x - 48.0f, 150.0f};
    r2d.FillRoundedRect(panel, 14.0f, crossrender::Color{0.06f, 0.07f, 0.10f, 0.92f});
    r2d.StrokeRoundedRect(panel, 14.0f, crossrender::Color{1, 1, 1, 0.12f});

    DrawHeader(r2d, *font, panel, "ГЕРОЙ: " + player.name);
    DrawProgressBar(r2d, *font, crossrender::Rect{panel.x + 24.0f, panel.y + 64.0f, panel.w - 48.0f, 22.0f},
                    player.health);

    // Журнал занимает нижнюю половину экрана.
    const crossrender::Rect logRegion{24.0f, panel.Bottom() + 20.0f, screen.x * 0.5f - 36.0f, 260.0f};
    DrawLog(r2d, *font, logRegion, player.log, 12.0f);

    // Карточка и кнопка справа.
    const crossrender::Rect card{screen.x * 0.5f + 12.0f, panel.Bottom() + 20.0f, screen.x * 0.5f - 36.0f,
                         160.0f};
    DrawCard(r2d, card, crossrender::Color::FromRGB(0x6BA1FF));

    const crossrender::Rect button{card.x, card.Bottom() + 20.0f, card.w * 0.5f, 44.0f};
    // В настоящем коде позиция берётся из Input; здесь — для наглядности.
    const crossrender::Vec2 mouse{screen.x * 0.5f, button.Center().y};
    const bool hot = button.Contains(mouse);
    DrawButton(r2d, *font, button, hot);

    r2d.EndFrame();

    // Статистика готова только после EndFrame: там вызывается Flush.
    const crossrender::Renderer2D::Stats& stats = r2d.GetStats();
    ENG_LOGI("demo", "HUD: %d вызовов, %d треугольников, %d путей, %d глифов", stats.drawCalls,
             stats.vertices, stats.paths, stats.textGlyphs);

    r2d.Shutdown();
    return 0;
}
```

## См. также

* `docs/gfx/Texture.md` — текстуры, которые принимают `Image`, `Image9` и
  `Paint::Image`; форматы и фильтрация.
* `docs/gfx/RenderTarget.md` — закадровые буферы для `BeginFrame` и
  `SetViewportBlit`.
* `docs/gfx/Renderer3D.md` — 3D-рендерер, чей результат накладывается поверх
  2D-кадра.
* `docs/text/Font.md` — шрифты, атласы глифов, SDF и `FontStyle`.
* `docs/core/Math.md` — `Vec2`, `Rect`, `Color` и `Mat4`, которыми оперирует
  рендерер.
* `docs/core/Log.md` — макросы `ENG_LOGI` / `ENG_LOGW`, которыми сообщается о
  переходе в инертный режим.
* `docs/ui/Ui.md` — виджеты, построенные поверх `Renderer2D`.
