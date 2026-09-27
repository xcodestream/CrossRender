# crossrender/anim/Lottie.h — проигрыватель Lottie (bodymovin JSON)

Разбор и воспроизведение векторной анимации Lottie без внешних библиотек:
документ, слои, фигуры, маски, изображения и текст рисуются через `Renderer2D`.

## Заголовок

```cpp
#include "crossrender/anim/Lottie.h"
```

## Обзор

Lottie (bodymovin) — это формат векторной анимации, который экспортирует Adobe
After Effects: один JSON-файл описывает композицию, слои, фигуры, ключевые
кадры и easing. Модуль реализует собственный парсер и собственный проигрыватель
поверх `Renderer2D`, поэтому файл `.json` можно положить рядом с ассетами и
рисовать без сторонних зависимостей.

**Модель документа.** Документ — это дерево из четырёх уровней:

1. **`LottieAnimation`** — композиция: размеры, частота кадров, длительность,
   список слоёв (`Layers`), список ассетов (`Assets`) и исходный JSON (`Raw`).
   Класс одновременно является и разобранным документом, и плеером: он хранит
   текущий кадр, скорость, границы сегмента и флаги проигрывания.
2. **`LottieLayer`** — слой: тип (фигура, сплошной цвет, изображение,
   прекомпозиция, текст, null), собственный трансформ, список фигур, маски,
   ссылка на маску-мат (`matteLayer` / `matteMode`) и временной диапазон
   `inTime`/`outTime`/`startTime`/`timeStretch`.
3. **`LottieShape`** — фигура: геометрия (`Rectangle`, `Ellipse`, `Path`,
   `PolyStar`), стили (`Fill`, `Stroke`, `GradientFill`, `GradientStroke`),
   модификаторы (`Trim`, `RoundedCorners`, `Repeater`, `Merge`, смещение пути) и
   группы (`Group`) с рекурсивным списком `items`.
4. **`LottieProperty`** — анимируемое значение: либо статическое (`animated ==
   false`, значение лежит в `scalar` / `vec2` / `vec3` / `color` /
   `pathPoints`), либо набор ключей `keys`. Время ключей после разбора хранится
   **в секундах** (кадры делятся на частоту композиции), поэтому `Evaluate*`
   принимают секунды.

Вспомогательные структуры: **`LottieTransform`** (anchor, position, scale,
rotation, opacity, skew, skewAxis) с методом `Evaluate(time)` →
`LottieTransform::Result`, **`LottieMask`** (путь, режим, инверсия, прозрачность)
и **`LottieAsset`** (встроенная прекомпозиция или внешнее/встроенное
изображение).

**Иерархия и порядок отрисовки.** `LottieAnimation::Render` и `LottieAnimation::RenderAt` (отдельного метода
`Draw` в API нет) создают локальный контекст и вызывают внутренний обход
`DrawLayerList`. Обход идёт по массиву слоёв **в обратном порядке**, потому что
bodymovin хранит слои сверху вниз, а рисует движок снизу вверх. Для каждого
слоя:

1. локальное время считается как `compTime − startTime`, делится на
   `timeStretch`, при наличии заменяется time-remap-ом, после чего слой
   отсекается по `inTime`/`outTime`;
2. собирается цепочка родителей (`parent` разрешается по полю `index`), в
   которой трансформы предков умножаются **слева**: `chain = chain * parent.Matrix(time)`, затем `mat = chain * layer.Matrix(time)`;
3. матрица передаётся в `Renderer2D` через `Transform` (используются только
   2D-компоненты `a, b, c, d, e, f`);
4. применяются маски и маска-мат (клиппингом), затем рисуется содержимое слоя:
   сплошной цвет — `FillRect`, изображение — `Image`, фигуры — пути с
   `Fill`/`Stroke`, текст — `DrawText`.

Порядок композиции трансформа, который реализует `LottieTransform::Result::Matrix`:

```
T(position) · Rz(rotation) · [Rz(skewAxis) · ShearX(tan(skew)) · Rz(−skewAxis)] · S(scale / 100) · T(−anchor)
```

`Mat4` умножает вектор справа, поэтому `−anchor` — самый внутренний множитель,
а `position` — самый внешний. Трансформы групп фигур складываются так же:
`local = parent · group.Matrix(time)`.

**Как рисуется кадр.** Обе точки входа отличаются только размещением:

* `Render(r, dst, alpha, tint)` вписывает композицию в прямоугольник `dst`
  **с сохранением пропорций** (letterbox/pillarbox) и центрирует её, домножает
  все прозрачности на `alpha`, а цвета — на `tint`. Именно здесь используется
  `SetImageLoader`.
* `RenderAt(r, position, size, rotation, alpha)` растягивает композицию ровно в
  `size` (неравномерный масштаб допустим), поворачивает на `rotation` и
  размещает так, что `position` — центр. Тинт здесь всегда белый, а
  зарегистрированный загрузчик изображений **не вызывается**.

Оба метода рисуют через `Renderer2D`, а значит наследуют его модель: стек
состояния (`Save`/`Restore`), глобальную прозрачность (`GlobalAlpha`), режимы
смешивания (`Composite`) и отсечение (`ClipPath`). В конце оба метода сбрасывают
`GlobalAlpha(1.0)`.

**Ассеты и изображения.** `LottieAsset` бывает двух видов: прекомпозиция (`isPrecomp == true`, со своими
`layers`) или изображение. Ссылки разрешаются на этапе `Parse`: слой с `refId`
сопоставляется ассету по `name`, а если не нашлось — по строковому `id`, и
получает либо `imageAssetIndex`, либо `precompIndex`. Путь к картинке
складывается из полей `u` (папка) и `p` (файл); если `p` начинается с `data:`,
изображение считается встроенным — base64-часть декодируется в
`LottieAsset::imageData`.

При отрисовке `ResolveImage` пробует по порядку: загрузчик, заданный
`SetImageLoader` (по имени слоя, владение текстурой снаружи), затем встроенные
`imageData` → `Texture::LoadFromMemory`, затем `imagePath` →
`Texture::LoadFromFile`. Изображение рисуется в размере **текстуры**, а не
ассета.

**Загрузка из файла.** Документ читается и разбирается одной строкой:

```cpp
crossrender::LottieAnimation anim;
std::string error;
if (!anim.LoadFromFile("assets/lottie/loading.json", &error)) {
    ENG_LOGE("lottie", "не удалось прочитать файл: %s", error.c_str());
}
```

`LoadFromFile` читает текст через `ReadTextFile` (`crossrender/core/File.h`) и
передаёт его в `LoadFromJson`; тот разбирает JSON и вызывает `Parse`. Для
кэширования по пути есть `LottieLibrary::Load`, а для встроенных демо-роликов —
`LottieLibrary::LoadOrGenerate`, который генерирует bodymovin-совместимый JSON
функцией `GenerateLottieJson` и прогоняет его через тот же парсер.

**Честные ограничения.** Реализация рисует через `Renderer2D` и во многом
аппроксимирует эффекты After Effects. Что важно знать:

* **Маски приближены клиппингом.** У `Renderer2D` нет разности путей, поэтому
  несколько масок пересекаются вложенными клипами, режим `n` пропускается,
  `a` и `i` обрабатываются одинаково (клип по контуру), а вычитание работает
  только через флаг `inv` (прямоугольник композиции с «дыркой»). Прозрачность
  маски (`LottieMask::opacity`) не применяется.
* **Маска-мат — тоже клип.** Силуэт слоя-мата (его уплощённые фигуры или
  прямоугольник для сплошного цвета/картинки) используется как область
  отсечения; различия alpha/luma и инверсия теряются, а сам слой-мат скрывается.
* **Режимы смешивания.** Поддержаны только `0` (Alpha), `1` (Multiply),
  `2` (Screen) и `14` (Additive); прочие один раз логируют предупреждение и
  рисуются как Alpha.
* **Градиенты двухцветные.** `Paint` в `Renderer2D` хранит только внутренний и
  внешний цвет, поэтому многостоповый градиент сэмплируется в точках 15% и 85%;
  радиальный градиент использует расстояние между `s` и `e` как радиус.
* **`Merge` (`mm`) не применяется.** Режим слияния разбирается и хранится, но
  пути рисуются независимо друг от друга.
* **Текст ограничен.** Используется один шрифт (`SetFont` или шрифт по
  умолчанию из `FontManager`); `fontFamily` не разрешается. Аниматоры текста
  поддерживают только диапазон, трекинг, цвет и прозрачность. Ширина символа
  берётся из глифа, а при его отсутствии — как `0.55 · size`.
* **Выражения не вычисляются.** Поле `e` (путь к изображению через выражение)
  только логирует предупреждение.
* **Градиентные остановки статичны.** Многостоповый градиент разбирается, но
  ключевые остановки не интерполируются: берётся одно значение цвета.
* **Интерполяция путей** идёт покоординатно и требует одинакового числа вершин
  в соседних ключах, иначе берётся форма из начального ключа. Пространственные
  касательные (`ti`/`to`) поддерживаются для двумерной позиции.
* **Глубина и лимиты.** Рекурсия прекомпозиций — 8 уровней, слоёв — 8, фигур
  при разборе — 12, при обходе — 10, репитер — не более 64 копий.
* **Время слоя смешано с кадрами.** Ключи трансформа после разбора хранятся в
  секундах, но проверка видимости слоя сравнивает время в секундах с
  `inTime`/`outTime` в кадрах композиции, а `startTime` вычитается как секунды.
  Для типичных документов (`st = 0`, `ip = 0`) это не мешает, но отсечение
  слоёв с ненулевыми границами работает приблизительно.
* **Кэш текстур живёт один вызов.** `ResolveImage` складывает текстуры в
  локальный контекст, который уничтожается в конце `Render`/`RenderAt`, поэтому
  внешние и встроенные изображения декодируются заново каждый кадр. Для
  продакшена регистрируйте `SetImageLoader` со своим постоянным кэшем.
* **Метаданные без поведения.** `is3D`, `positionZ`, `direction`, `fillRule`,
  `trimEnabled`, `LottieLayer::textAlign` и размеры ассета заполняются
  (или существуют), но на отрисовку не влияют: 3D-слои проецируются как 2D.

## Члены класса

Ниже описаны все публичные члены заголовка. Поля структур сгруппированы по
роли: на группу полей даётся одна таблица и **один общий пример** (в тексте
таких разделов это оговорено), а каждый метод, конструктор, деструктор и
перечисление получают отдельный подраздел со своим примером.

### crossrender::LottieProperty

Анимируемое свойство документа: одно значение, статическое или ключевое.
`Kind` задаёт, какое поле значения использовать; `animated` показывает, есть ли
ключи. Все `Evaluate*` принимают **секунды**.

```cpp
crossrender::LottieProperty opacity;               // статическая прозрачность
opacity.kind = crossrender::LottieProperty::Kind::Scalar;
opacity.scalar = 75.0f;
opacity.animated = false;

const crossrender::f32 o = opacity.EvaluateScalar(0.0f);   // 75
```

### `enum class LottieProperty::Kind : u8`

Тип значения свойства. Определяет, какие поля структуры заполнены и какой
метод оценки имеет смысл вызывать.

| Значение | Значение лежит в | Метод оценки |
|---|---|---|
| `Scalar` | `scalar` | `EvaluateScalar` |
| `Vec2` | `vec2` (и `vec3` с `z = 0`) | `EvaluateVec2` |
| `Vec3` | `vec3` (и `vec2` из `x`, `y`) | `EvaluateVec2`, `EvaluateScalar` |
| `Color` | `color` | `EvaluateColor` |
| `Path` | `pathPoints` | `EvaluatePath` |
| `Gradient` | остановки в боковой таблице реализации | — |

```cpp
crossrender::LottieProperty p;
p.kind = crossrender::LottieProperty::Kind::Vec2;
p.vec2 = crossrender::Vec2{10.0f, 20.0f};
```

### Поля `kind`, `scalar`, `vec2`, `vec3`, `color`, `pathPoints`, `animated`

Статическое значение свойства и признак анимации. Если `animated == false` или
список ключей пуст, `Evaluate*` возвращают именно эти поля. Поля `vec2`/`vec3`/
`scalar` синхронизируются парсером: у `Vec2`-свойства `scalar` равен `x`.

| Поле | Смысл |
|---|---|
| `Kind kind` | тип значения |
| `f32 scalar` | число (прозрачность, размер, угол, …) |
| `Vec2 vec2` | двумерное значение (позиция, масштаб, якорь) |
| `Vec3 vec3` | трёхмерное значение |
| `Color color` | цвет |
| `std::vector<Vec2> pathPoints` | плоский список вершин пути |
| `bool animated` | есть ли ключи (`false` для статических свойств) |

```cpp
crossrender::LottieProperty colour;
colour.kind = crossrender::LottieProperty::Kind::Color;
colour.color = crossrender::Color::FromRGB(0x4CAF50);
colour.animated = false;
ENG_LOGI("lottie", "цвет: %.2f %.2f %.2f", colour.color.r, colour.color.g, colour.color.b);
```

### `struct LottieProperty::Key`

Один ключ свойства. Время уже в секундах. У ключа есть и скалярное, и
векторное, и цветовое, и путевое значение — используется то, что соответствует
`Kind`. `easingIn` / `easingOut` хранят контрольные точки в виде строки
`"x1,y1"` (или `"x1,y1,x2,y2"`), `hold` включает ступенчатую интерполяцию.

| Поле | Смысл |
|---|---|
| `f32 time` | момент ключа в секундах |
| `f32 value` | скалярное значение |
| `Vec2 v2`, `Vec3 v3` | векторные значения |
| `Color col` | цвет |
| `std::vector<Vec2> pts` | точки пути (тройками «вершина, вход, выход») |
| `std::string easingIn`, `easingOut` | кривые входа и выхода |
| `bool hold` | держать значение до следующего ключа |

```cpp
crossrender::LottieProperty::Key k;
k.time = 0.5f;
k.value = 100.0f;
k.easingOut = "0.42,0";
k.easingIn = "0.58,1";
k.hold = false;
```

### Поля `keys`, `spatial` и `struct LottieProperty::SpatialTangent`

Список ключей и (для позиции) пространственные касательные движения по кривой.
Каждому ключу соответствует одна запись `spatial`: `outTangent` у левого ключа
и `inTangent` у правого задают контрольные точки кубической траектории.
Ненулевые касательные включают криволинейное движение в `EvaluateVec2`.

| Поле | Смысл |
|---|---|
| `std::vector<Key> keys` | ключи свойства |
| `std::vector<SpatialTangent> spatial` | касательные траектории, по одной на ключ |
| `Vec2 SpatialTangent::inTangent` | входящая касательная |
| `Vec2 SpatialTangent::outTangent` | исходящая касательная |

```cpp
crossrender::LottieProperty position;
position.kind = crossrender::LottieProperty::Kind::Vec2;
position.animated = true;

crossrender::LottieProperty::Key a;
a.time = 0.0f;
a.v2 = crossrender::Vec2{0, 0};
crossrender::LottieProperty::Key b;
b.time = 1.0f;
b.v2 = crossrender::Vec2{100, 0};
position.keys = {a, b};
position.spatial = {{crossrender::Vec2{0, -40}, crossrender::Vec2{0, -40}}, {crossrender::Vec2{0, -40}, crossrender::Vec2{0, -40}}};
```

### `f32 LottieProperty::EvaluateScalar(f32 time) const`

Скалярное значение в момент `time` (секунды). Без анимации возвращается
`scalar`; на ключе и за его пределами — значение крайнего ключа. Между ключами
применяется easing, а при `hold` значение держится до следующего ключа.

```cpp
crossrender::LottieProperty opacity;
opacity.kind = crossrender::LottieProperty::Kind::Scalar;
opacity.animated = true;
opacity.keys.resize(2);
opacity.keys[0].time = 0.0f;
opacity.keys[0].value = 0.0f;
opacity.keys[1].time = 1.0f;
opacity.keys[1].value = 100.0f;

const crossrender::f32 mid = opacity.EvaluateScalar(0.5f);   // 50
```

### `Vec2 LottieProperty::EvaluateVec2(f32 time) const`

Двумерное значение. Если у сегмента заданы пространственные касательные,
точка считается по кубической кривой `CubicAt`, иначе — линейно. `hold` и
выход за границы ключей дают значение крайнего ключа.

```cpp
crossrender::LottieProperty anchor;
anchor.kind = crossrender::LottieProperty::Kind::Vec2;
anchor.vec2 = crossrender::Vec2{50.0f, 50.0f};
anchor.animated = false;

const crossrender::Vec2 a = anchor.EvaluateVec2(0.0f);
ENG_LOGI("lottie", "якорь: (%.1f, %.1f)", a.x, a.y);
```

### `Color LottieProperty::EvaluateColor(f32 time) const`

Цвет в момент `time`. Между ключами цвета линейно интерполируются
покомпонентно вместе с альфой.

```cpp
crossrender::LottieProperty fill;
fill.kind = crossrender::LottieProperty::Kind::Color;
fill.color = crossrender::Color::White;
fill.animated = false;

const crossrender::Color c = fill.EvaluateColor(0.0f);
ENG_LOGI("lottie", "альфа заливки: %.2f", c.a);
```

### `std::vector<Vec2> LottieProperty::EvaluatePath(f32 time) const`

Плоский список точек пути: тройками «вершина, входящая касательная, исходящая
касательная», где касательные заданы **относительно своей вершины**. Замкнутый
путь повторяет первую вершину в конце. При интерполяции точки смешиваются
покоординатно; если у соседних ключей разное число вершин, возвращается форма
начального ключа.

```cpp
crossrender::LottieProperty path;
path.kind = crossrender::LottieProperty::Kind::Path;
path.pathPoints = {crossrender::Vec2{0, 0}, crossrender::Vec2{0, 0}, crossrender::Vec2{0, 0},
                   crossrender::Vec2{10, 0}, crossrender::Vec2{0, 0}, crossrender::Vec2{0, 0}};
path.animated = false;

const std::vector<crossrender::Vec2> pts = path.EvaluatePath(0.0f);
ENG_LOGI("lottie", "точек в списке: %d", static_cast<int>(pts.size()));
```

### crossrender::LottieTransform

Трансформ слоя или группы: якорь, позиция, масштаб (в процентах), поворот (в
градусах), прозрачность (0…100), скос и ось скоса. `Evaluate(time)` собирает
`Result`, `Matrix(time)` сразу возвращает матрицу.

```cpp
crossrender::LottieTransform tr;
tr.anchor.scalar = 0.0f;
tr.position.vec2 = crossrender::Vec2{120.0f, 120.0f};
tr.scale.vec2 = crossrender::Vec2{100.0f, 100.0f};
tr.rotation.scalar = 45.0f;
tr.opacity.scalar = 100.0f;

const crossrender::Mat4 m = tr.Matrix(0.0f);
```

### Поля `anchor`, `position`, `scale`, `rotation`, `opacity`, `skew`, `skewAxis`

Семь анимируемых свойств трансформа. Масштаб хранится в процентах (`100` —
исходный размер), прозрачность — в диапазоне `0…100`, поворот — в градусах.
Скос применяется сдвигом вдоль оси `skewAxis` и только при ненулевом `skew`.

| Поле | Смысл |
|---|---|
| `LottieProperty anchor` | точка отсчёта, вокруг которой идут поворот и масштаб |
| `LottieProperty position` | смещение слоя |
| `LottieProperty scale` | масштаб в процентах |
| `LottieProperty rotation` | поворот в градусах |
| `LottieProperty opacity` | прозрачность `0…100` |
| `LottieProperty skew` | угол скоса в градусах |
| `LottieProperty skewAxis` | ось скоса в градусах |

```cpp
crossrender::LottieTransform tr;
tr.scale.vec2 = crossrender::Vec2{150.0f, 150.0f};     // +50% к размеру
tr.opacity.scalar = 50.0f;                     // полупрозрачный
tr.skew.scalar = 12.0f;
tr.skewAxis.scalar = 0.0f;
tr.rotation.scalar = -30.0f;
```

### `struct LottieTransform::Result`

Разрешённый на конкретный момент трансформ: готовые числа вместо свойств.
Метод `Matrix()` собирает из них матрицу в порядке
`T(position) · Rz(rotation) · shear · S(scale/100) · T(−anchor)`. Это
единственное место, где зафиксирован порядок композиции.

| Поле | Смысл |
|---|---|
| `Vec2 anchor` | разрешённый якорь |
| `Vec2 position` | разрешённая позиция |
| `Vec2 scale` | разрешённый масштаб в процентах (по умолчанию `{100, 100}`) |
| `f32 rotation` | разрешённый поворот в градусах |
| `f32 opacity` | разрешённая прозрачность `0…100` |
| `f32 skew`, `skewAxis` | разрешённые скос и ось скоса |

```cpp
crossrender::LottieTransform::Result r;
r.position = crossrender::Vec2{100.0f, 50.0f};
r.scale = crossrender::Vec2{200.0f, 200.0f};
r.rotation = 90.0f;

const crossrender::Mat4 m = r.Matrix();                 // масштаб вокруг якоря, затем поворот
```

### `LottieTransform::Result LottieTransform::Evaluate(f32 time) const`

Считает все семь свойств в момент `time` и складывает в `Result`. Поля, которые
не были заданы, остаются значениями по умолчанию (`scale = {100, 100}`,
`opacity = 100`).

```cpp
crossrender::LottieTransform tr;
tr.position.vec2 = crossrender::Vec2{10.0f, 20.0f};
tr.opacity.scalar = 80.0f;

const crossrender::LottieTransform::Result r = tr.Evaluate(0.0f);
ENG_LOGI("lottie", "позиция (%.1f, %.1f), прозрачность %.0f", r.position.x, r.position.y,
         r.opacity);
```

### `Mat4 LottieTransform::Matrix(f32 time) const`

Короткая форма: `Evaluate(time).Matrix()`. Удобна там, где нужна только
матрица.

```cpp
crossrender::LottieTransform tr;
tr.position.vec2 = crossrender::Vec2{60.0f, 0.0f};
tr.rotation.scalar = 15.0f;

const crossrender::Mat4 local = tr.Matrix(0.0f);
// Мировая матрица ребёнка: parent * local.
const crossrender::Mat4 world = crossrender::Mat4::Identity() * local;
```

### `enum class LottieLayerType : u8`

Тип слоя. Значение `Unknown` получается для неизвестного `ty` и отмечается
`Validate` как ошибка; слой `Audio` не рисуется.

| Значение | `ty` | Что рисуется |
|---|---|---|
| `Precomp` | 0 | содержимое ассета-прекомпозиции, рекурсивно |
| `Solid` | 1 | залитый прямоугольник `width × height` вокруг начала слоя |
| `Image` | 2 | текстура из ассета изображения |
| `Null` | 3 | ничего (служебный слой для иерархии) |
| `Shape` | 4 | фигуры слоя |
| `Text` | 5 | текст через `Font` |
| `Audio` | 6 | ничего |
| `Unknown` | прочее | ничего |

```cpp
crossrender::LottieLayer layer;
layer.type = crossrender::LottieLayerType::Solid;
layer.width = 100.0f;
layer.height = 40.0f;
layer.solidColor = crossrender::Color::FromRGB(0x222831);
```

### crossrender::LottieShape

Фигура: геометрия, стиль или модификатор. Группа (`Group`) хранит вложенные
фигуры в `items`, а её собственный трансформ лежит в `transform`. Порядок
элементов внутри группы важен: стили применяются к геометрии того же уровня,
даже если объявлены после неё (обход собирает стили отдельным проходом).

> **Ловушка копирования.** Из-за `std::vector<std::unique_ptr<LottieShape>>`
> внутри `items` структура **перемещаемая, но не копируемая**: в
> `std::vector<LottieShape>` её можно положить только через `std::move`. То же
> ограничение по цепочке распространяется на `LottieLayer` и `LottieAsset`.

```cpp
crossrender::LottieShape rect;
rect.type = crossrender::LottieShape::Type::Rectangle;
rect.name = "card";
rect.size.vec2 = crossrender::Vec2{200.0f, 120.0f};
rect.roundness.scalar = 16.0f;
```

### `enum class LottieShape::Type : u8`

Тип фигуры. Геометрические типы (`Rectangle`, `Ellipse`, `Path`, `PolyStar`)
дают контуры; `Fill`/`Stroke`/`GradientFill`/`GradientStroke` — стили;
`Trim`, `RoundedCorners`, `Repeater`, `Merge`, `Transform` — модификаторы и
групповой трансформ; `Unknown` означает нераспознанный `ty`.

| Значение | `ty` | Роль |
|---|---|---|
| `Group` | `gr` | контейнер вложенных фигур |
| `Rectangle` | `rc` | прямоугольник со скруглением |
| `Ellipse` | `el` | эллипс |
| `Path` | `sh` | произвольный кубический путь |
| `PolyStar` | `sr` | многоугольник или звезда |
| `Fill` | `fl` | сплошная заливка |
| `Stroke` | `st` | обводка |
| `GradientFill` | `gf` | градиентная заливка |
| `GradientStroke` | `gs` | градиентная обводка |
| `Transform` | `tr` | трансформ группы |
| `Merge` | `mm` | слияние путей (не применяется) |
| `Trim` | `tm` | обрезка пути по длине |
| `Repeater` | `rp` | повторение содержимого |
| `RoundedCorners` | `rd` | скругление углов пути |
| `Unknown` | прочее | не поддерживается |

```cpp
crossrender::LottieShape star;
star.type = crossrender::LottieShape::Type::PolyStar;
star.points.scalar = 6.0f;
star.outerRadius.scalar = 60.0f;
star.innerRadius.scalar = 28.0f;
star.starType.scalar = 2.0f;      // 2 = звезда, 1 = многоугольник
```

### Поля `type`, `name`, `hidden`, `transform`

Идентификация и трансформ фигуры. Скрытые фигуры (`hidden`) пропускаются и
обходом, и сбором стилей.

| Поле | Смысл |
|---|---|
| `Type type` | тип фигуры |
| `std::string name` | имя из `nm` |
| `bool hidden` | фигура скрыта |
| `LottieTransform transform` | трансформ группы (для `Group` и `Repeater`) |

```cpp
crossrender::LottieShape group;
group.type = crossrender::LottieShape::Type::Group;
group.name = "cardGroup";
group.hidden = false;
group.transform.position.vec2 = crossrender::Vec2{150.0f, 100.0f};
```

### Поля геометрии `size`, `position`, `roundness`, `direction`, `innerRadius`, `outerRadius`, `points`, `rotation`, `starType`

Параметры контуров. Для прямоугольника и эллипса используются `position` и
`size`, для прямоугольника ещё `roundness`, для `PolyStar` — радиусы, число
точек, поворот и тип звезды. Поле `direction` объявлено, но парсером не
заполняется и на отрисовку не влияет.

| Поле | Смысл |
|---|---|
| `LottieProperty size` | размер прямоугольника/эллипса |
| `LottieProperty position` | центр фигуры |
| `LottieProperty roundness` | радиус скругления (прямоугольник или `RoundedCorners`) |
| `LottieProperty direction` | направление обхода (не используется) |
| `LottieProperty innerRadius`, `outerRadius` | внутренний и внешний радиусы звезды |
| `LottieProperty points` | число лучей/вершин |
| `LottieProperty rotation` | поворот звезды |
| `LottieProperty starType` | `1` — многоугольник, `2` — звезда |

```cpp
crossrender::LottieShape ellipse;
ellipse.type = crossrender::LottieShape::Type::Ellipse;
ellipse.position.vec2 = crossrender::Vec2{120.0f, 120.0f};
ellipse.size.vec2 = crossrender::Vec2{160.0f, 160.0f};
```

### Поля модификаторов `start`, `end`, `offset`, `copies`, `pathData`, `trimEnabled`

Модификаторы контура: обрезка по длине (`Trim`), повторение (`Repeater`) и
данные пути. Для `Trim` доли задаются в процентах (`0…100`), `offset` сдвигает
начало обрезки. `pathData` хранит путь: статический — в `pathPoints`,
ключевой — в `keys[].pts`. `trimEnabled` — просто отметка разбора.

| Поле | Смысл |
|---|---|
| `LottieProperty start`, `end` | начало и конец обрезки в процентах |
| `LottieProperty offset` | сдвиг обрезки в градусах (делится на 360) |
| `LottieProperty copies` | число копий репитера (ограничено 64) |
| `LottieProperty pathData` | геометрия пути (`ks`) |
| `bool trimEnabled` | фигура является `Trim` |

```cpp
crossrender::LottieShape trim;
trim.type = crossrender::LottieShape::Type::Trim;
trim.start.scalar = 0.0f;
trim.end.scalar = 50.0f;         // рисуем половину контура
trim.offset.scalar = 0.0f;
trim.trimEnabled = true;
```

### Поля стиля `colour`, `strokeWidth`, `strokeOpacity`, `fillOpacity`, `opacity`, `dashLength`, `dashGap`

Цвет и прозрачность заливки/обводки. У обводки дополнительно есть штрихпунктир:
пара `dashLength`/`dashGap` включает ручную нарезку пути (если хотя бы одно
значение больше `0.01`). Поле `opacity` дублирует `fillOpacity` и в
отрисовке не участвует.

| Поле | Смысл |
|---|---|
| `LottieProperty colour` | цвет заливки или обводки |
| `LottieProperty strokeWidth` | толщина обводки (минимум `1`) |
| `LottieProperty strokeOpacity` | прозрачность обводки `0…100` |
| `LottieProperty fillOpacity` | прозрачность заливки `0…100` |
| `LottieProperty opacity` | копия `fillOpacity` (метаданные) |
| `LottieProperty dashLength`, `dashGap` | длина штриха и промежутка |

```cpp
crossrender::LottieShape stroke;
stroke.type = crossrender::LottieShape::Type::Stroke;
stroke.colour.color = crossrender::Color::White;
stroke.strokeWidth.scalar = 8.0f;
stroke.strokeOpacity.scalar = 100.0f;
stroke.dashLength.scalar = 12.0f;    // штрихпунктир: 12 px штрих...
stroke.dashGap.scalar = 6.0f;        // ...и 6 px промежуток
```

### Поля обводки `fillRule`, `cap`, `join`, `miterLimit`

Параметры заливки и стыков обводки. `cap`/`join` задаются теми же
перечислениями, что и у `Renderer2D`. `fillRule` разбирается, но при заливке не
используется (действует правило ненулевого обхода рендерера).

| Поле | Смысл |
|---|---|
| `int fillRule` | правило заливки из `r` (метаданные) |
| `LineCap cap` | форма концов: `Butt`, `Round`, `Square` |
| `LineJoin join` | форма стыков: `Miter`, `Round`, `Bevel` |
| `f32 miterLimit` | предел среза для `Miter` (по умолчанию `4`) |

```cpp
crossrender::LottieShape outline;
outline.type = crossrender::LottieShape::Type::Stroke;
outline.colour.color = crossrender::Color::FromRGB(0x6BA1FF);
outline.strokeWidth.scalar = 6.0f;
outline.cap = crossrender::LineCap::Round;
outline.join = crossrender::LineJoin::Round;
outline.miterLimit = 4.0f;
```

### Поля градиента `gradientStops`, `gradientStart`, `gradientEnd`, `gradientType` и `struct LottieShape::GradientStop`

Описание градиента заливки/обводки. `gradientType` равен `1` для линейного и
`2` для радиального градиента; для радиального радиусом служит расстояние
между `gradientStart` и `gradientEnd`. Поскольку `Paint` рендерера двухцветный,
многостоповый градиент сэмплируется в точках 15% и 85%.

| Поле | Смысл |
|---|---|
| `std::vector<GradientStop> gradientStops` | остановки градиента |
| `LottieProperty gradientStart`, `gradientEnd` | начало и конец оси градиента |
| `int gradientType` | `1` — линейный, `2` — радиальный |
| `GradientStop::offset` | позиция остановки `0…1` |
| `GradientStop::color` | цвет остановки |

```cpp
crossrender::LottieShape gradient;
gradient.type = crossrender::LottieShape::Type::GradientFill;
gradient.gradientType = 1;
gradient.gradientStops = {{0.0f, crossrender::Color::FromRGB(0x4CA1FF)},
                          {1.0f, crossrender::Color::FromRGB(0x14213D)}};
gradient.gradientStart.vec2 = crossrender::Vec2{-100.0f, -60.0f};
gradient.gradientEnd.vec2 = crossrender::Vec2{100.0f, 60.0f};
```

### Поле `items`

Вложенные фигуры группы. Парсер складывает сюда всё содержимое `it`, кроме
трансформа группы, который попадает в `transform`. Обход рекурсивен, глубина
ограничена 10 уровнями.

```cpp
crossrender::LottieShape group;
group.type = crossrender::LottieShape::Type::Group;
group.name = "ringGroup";

auto ellipse = std::make_unique<crossrender::LottieShape>();
ellipse->type = crossrender::LottieShape::Type::Ellipse;
ellipse->size.vec2 = crossrender::Vec2{160.0f, 160.0f};
group.items.push_back(std::move(ellipse));
```

### crossrender::LottieMask

Маска слоя: контур, режим и инверсия. Плеер приближает маски клиппингом,
поэтому режимы `a` и `i` обрабатываются одинаково, `n` пропускается, а
вычитание работает только через `inverted`.

| Поле | Смысл |
|---|---|
| `LottieProperty path` | контур маски |
| `int mode` | символ режима: `'a'` добавить, `'s'` вычесть, `'i'` пересечь, `'n'` нет |
| `bool inverted` | инвертировать маску (прямоугольник композиции с «дыркой») |
| `f32 opacity` | прозрачность маски (разбирается, но не применяется) |

```cpp
crossrender::LottieMask mask;
mask.path = ellipseShape.pathData;
mask.mode = 'a';
mask.inverted = false;
mask.opacity = 100.0f;
layer.masks.push_back(mask);
layer.hasMask = true;
```

### crossrender::LottieLayer

Слой документа. Собирает в себе всё, что относится к одному элементу
композиции: тип, время жизни, трансформ, фигуры, маски, маску-мат, данные
текста и ссылку на ассет. Слои, попавшие в `assets` прекомпозиции, имеют ту же
структуру. Слой перемещаемый, но не копируемый (из-за `std::vector<LottieShape>`
внутри), поэтому в `LottieAsset::layers` его добавляют через `std::move`.

```cpp
crossrender::LottieLayer layer;
layer.index = 1;
layer.name = "card";
layer.type = crossrender::LottieLayerType::Shape;
layer.inTime = 0.0f;
layer.outTime = 36.0f;
layer.timeStretch = 1.0f;
```

### Поля `index`, `parent`, `name`, `type`, `startTime`, `inTime`, `outTime`, `timeStretch`, `precompIndex`, `hidden`

Идентификация, иерархия и время жизни слоя. `index` — собственный номер слоя в
bodymovin, `parent` — номер родителя (не индекс в массиве). `inTime`/`outTime`
заданы в кадрах композиции; `timeStretch` растягивает время (неположительное
значение заменяется на `1`). `precompIndex` — индекс ассета-прекомпозиции в
`LottieAnimation::Assets()`.

| Поле | Смысл |
|---|---|
| `int index` | номер слоя в документе |
| `int parent` | номер родительского слоя или `-1` |
| `std::string name` | имя слоя |
| `LottieLayerType type` | тип слоя |
| `f32 startTime` | сдвиг начала слоя (`st`) |
| `f32 inTime`, `outTime` | границы видимости (`ip`, `op`) |
| `f32 timeStretch` | множитель скорости слоя (`sr`) |
| `int precompIndex` | индекс ассета-прекомпозиции |
| `bool hidden` | слой скрыт (в том числе слои-маты) |

```cpp
crossrender::LottieLayer layer;
layer.index = 3;
layer.parent = 1;                       // слой следует за слоем с index == 1
layer.name = "arm";
layer.timeStretch = 2.0f;               // играет вдвое медленнее
layer.hidden = false;
```

### Поля `width`, `height`, `solidColor`, `imageName`, `imageAssetIndex`

Данные сплошного слоя и слоя-изображения. Для `Solid` важны `width`/`height` и
`solidColor` (прямоугольник рисуется **вокруг начала слоя**, а не от левого
верхнего угла). Для `Image` — `imageName` (значение `refId`) и разрешённый
`imageAssetIndex` в списке ассетов.

| Поле | Смысл |
|---|---|
| `f32 width`, `height` | размер сплошного слоя (`sw`, `sh`) |
| `Color solidColor` | цвет сплошного слоя (`sc`) |
| `std::string imageName` | ссылка `refId` на ассет |
| `int imageAssetIndex` | индекс ассета изображения или `-1` |

```cpp
crossrender::LottieLayer floor;
floor.type = crossrender::LottieLayerType::Solid;
floor.width = 180.0f;
floor.height = 8.0f;
floor.solidColor = crossrender::Color::FromRGB(0x1A2233);
```

### Поле `transform`

Трансформ слоя (`ks`): якорь, позиция, масштаб, поворот, прозрачность, скос.
Именно он попадает в цепочку родителей и в итоговую матрицу слоя.

```cpp
crossrender::LottieLayer layer;
layer.transform = crossrender::LottieTransform{};
layer.transform.anchor.vec2 = crossrender::Vec2{0.0f, 0.0f};
layer.transform.position.vec2 = crossrender::Vec2{120.0f, 110.0f};
layer.transform.scale.vec2 = crossrender::Vec2{100.0f, 100.0f};
layer.transform.rotation.scalar = 0.0f;
```

### Поля `shapes`, `masks`, `hasMask`

Содержимое слоя: фигуры (для `Shape`/`Null`) и маски. `hasMask` выставляется
парсером при наличии `masksProperties`; отрисовка дополнительно проверяет, что
список масок не пуст, так что одно поле без списка ничего не даёт.

```cpp
crossrender::LottieLayer layer;
layer.type = crossrender::LottieLayerType::Shape;
layer.hasMask = true;
layer.masks.push_back(crossrender::LottieMask{});
ENG_LOGI("lottie", "фигур: %d, масок: %d", static_cast<int>(layer.shapes.size()),
         static_cast<int>(layer.masks.size()));
```

### Поля `matteLayer`, `matteMode`

Маска-мат: `matteLayer` хранит `index` слоя-источника, `matteMode` — режим
(`0` — нет, `1` alpha, `2` alpha-inverted, `3` luma, `4` luma-inverted).
Парсер сам связывает соседние слои: слой с ненулевым `td` скрывается и
становится матом для следующего снизу. В отрисовке все ненулевые режимы
трактуются одинаково — клип по силуэту.

| Поле | Смысл |
|---|---|
| `int matteLayer` | `index` слоя-мата |
| `int matteMode` | режим мата (`td`), `0` — мат не используется |

```cpp
crossrender::LottieLayer target;
target.matteLayer = 7;        // index слоя выше
target.matteMode = 1;         // alpha-мат
```

### Поля текста `text`, `fontFamily`, `fontSize`, `textColor`, `textTracking`, `textLineHeight`, `textAlign`, `justification`, `strokeOverFill`, `textStrokeColor`, `textStrokeWidth`

Плоские копии данных текстового слоя (те же значения лежат в боковой таблице
реализации и используются при отрисовке). `justification` — исходное число
`j`: `0` слева, `1` справа, `2` по центру; `textAlign` — его производное, в
отрисовке не участвует. `fontFamily` не разрешается в конкретный шрифт: плеер
рисует тем `Font`, который задан `SetFont` или взят по умолчанию.

| Поле | Смысл |
|---|---|
| `std::string text` | сам текст |
| `std::string fontFamily` | имя шрифта из документа (метаданные) |
| `f32 fontSize` | размер шрифта |
| `Color textColor` | цвет заливки текста |
| `f32 textTracking` | дополнительный трекинг на символ |
| `f32 textLineHeight` | высота строки |
| `TextAlign textAlign` | выравнивание (производное, не используется) |
| `int justification` | `j`: `0` слева, `1` справа, `2` по центру |
| `bool strokeOverFill` | рисовать обводку поверх заливки (метаданные) |
| `Color textStrokeColor`, `f32 textStrokeWidth` | параметры обводки (метаданные) |

```cpp
crossrender::LottieLayer title;
title.type = crossrender::LottieLayerType::Text;
title.text = "Победа";
title.fontSize = 48.0f;
title.textColor = crossrender::Color::White;
title.justification = 2;              // по центру
title.textLineHeight = 56.0f;
```

### Поля `is3D`, `positionZ`

Флаги трёхмерного слоя. `is3D` заполняется из `ddd`, но плеер проецирует слои
как двумерные: `positionZ` не заполняется и в матрицу не попадает.

| Поле | Смысл |
|---|---|
| `bool is3D` | слой помечен трёхмерным (`ddd`) |
| `Vec3 positionZ` | заготовка под z-позицию (не используется) |

```cpp
crossrender::LottieLayer layer;
layer.is3D = false;
layer.positionZ = crossrender::Vec3{0, 0, 0};   // плеер игнорирует z
```

### crossrender::LottieAsset

Ассет композиции: либо прекомпозиция со своим списком слоёв, либо изображение.
Ссылки на ассеты разрешаются в `Parse` по `name`, затем по строковому `id`.

| Поле | Смысл |
|---|---|
| `int id` | идентификатор ассета |
| `std::string name` | имя (`nm`) |
| `int width`, `height` | размеры ассета (метаданные) |
| `bool isPrecomp` | ассет содержит слои прекомпозиции |
| `std::vector<LottieLayer> layers` | слои прекомпозиции |
| `std::string imagePath` | путь `u + p` или `data:`-URI |
| `std::vector<u8> imageData` | декодированные байты встроенного изображения |

```cpp
crossrender::LottieAnimation anim;
std::string err;
anim.LoadFromFile("assets/lottie/card.json", &err);

for (const crossrender::LottieAsset& asset : anim.Assets()) {
    ENG_LOGI("lottie", "ассет '%s': прекомпозиция=%d, картинка=%s", asset.name.c_str(),
             asset.isPrecomp ? 1 : 0, asset.imagePath.c_str());
}
```

### crossrender::LottieAnimation

Разобранная композиция и одновременно плеер. Один объект хранит и дерево
документа, и состояние воспроизведения (текущий кадр, скорость, сегмент).
Объект некопируемый (внутри `unique_ptr`), но перемещаемый; для повторного
использования есть `Destroy`, для разбора — `LoadFromFile` / `LoadFromJson` /
`Parse`.

```cpp
crossrender::LottieAnimation anim;
if (anim.LoadFromFile("assets/lottie/loading.json")) {
    anim.SetLoop(true);
    anim.Play();
    anim.Advance(1.0f / 60.0f);
}
```

### `LottieAnimation()`

Конструктор по умолчанию: создаёт пустой невалидный документ (нет слоёв,
`valid_ == false`). Полезных данных до `LoadFromFile` / `LoadFromJson` / `Parse`
в нём нет.

```cpp
crossrender::LottieAnimation anim;
ENG_ASSERT(!anim.Valid());
ENG_ASSERT(anim.LayerCount() == 0);
```

### `~LottieAnimation()`

Деструктор освобождает дерево документа, исходный JSON и боковые таблицы
реализации. Все указатели, выданные наружу (например `Layers()` и `Assets()`),
после этого недействительны.

```cpp
{
    crossrender::LottieAnimation tmp;
    tmp.LoadFromJson("{\"layers\":[]}", nullptr);   // документ без слоёв
}   // здесь дерево освобождается
```

### `LottieAnimation(LottieAnimation&&) noexcept` / `LottieAnimation& operator=(LottieAnimation&&) noexcept`

Перемещающий конструктор и перемещающее присваивание. Копирование запрещено
(внутри `std::unique_ptr`), поэтому документ передаётся по ссылке или
перемещается. Оба метода сгенерированы по умолчанию и просто переносят поля:
логический флаг `valid_` копируется как есть, а списки слоёв и ассетов у
источника оказываются пустыми. Пользоваться перемещённым объектом нельзя —
вызовите у него `Destroy()` или загрузите документ заново.

```cpp
crossrender::LottieAnimation MakeAnim() {
    crossrender::LottieAnimation a;
    a.LoadFromFile("assets/lottie/heart.json");
    return a;                       // перемещение, не копирование
}

crossrender::LottieAnimation anim = MakeAnim();
crossrender::LottieAnimation moved = std::move(anim);
anim.Destroy();                     // источник снова в согласованном состоянии
```

### `bool LoadFromFile(const std::string& path, std::string* error = nullptr)`

Читает файл через `ReadTextFile` и разбирает его. При неудаче пишет ошибку в
`*error` (если указатель не нулевой), логирует её и возвращает `false`;
документ остаётся невалидным. Пустой файл считается ошибкой чтения.

```cpp
crossrender::LottieAnimation anim;
std::string error;
if (!anim.LoadFromFile("assets/lottie/missing.json", &error)) {
    ENG_LOGW("lottie", "анимация недоступна: %s", error.c_str());
}
```

### `bool LoadFromJson(const std::string& json, std::string* error = nullptr)`

Разбирает JSON из строки. Пустой корень, не-объект или синтаксическая ошибка
дают `false` и текст ошибки. Успешный разбор требует хотя бы одного слоя —
иначе `Parse` вернёт `false` с сообщением «composition has no layers».

```cpp
crossrender::LottieAnimation anim;
const std::string json = crossrender::GenerateLottieJson("checkmark");
if (!anim.LoadFromJson(json)) ENG_LOGE("lottie", "сгенерированный JSON не разобрался");
```

### `bool Parse(const JsonValue& root, std::string* error = nullptr)`

Разбор уже готового DOM. Полностью сбрасывает предыдущее содержимое
(`Destroy`), читает `nm`, `fr`, `w`, `h`, `ip`, `op`, разбирает ассеты, слои,
ссылки, маты, тексты и боковые таблицы, затем проверяет, что слои есть.
Длительность считается как `ceil(op − ip) / fr`. Возвращает `true`, только если
слоёв больше нуля.

```cpp
std::string err;
const crossrender::JsonValue dom = crossrender::JsonValue::Parse(jsonText, &err);
crossrender::LottieAnimation anim;
if (!dom.IsNull() && anim.Parse(dom, &err)) ENG_LOGI("lottie", "слоёв: %d", anim.LayerCount());
```

### `void Destroy()`

Полный сброс: освобождает дерево, исходный JSON, ассеты, боковые таблицы и
сбрасывает состояние плеера (кадр, скорость, сегмент, счётчик циклов).
Вызывается в начале `Parse`, поэтому повторный разбор безопасен.

```cpp
anim.Destroy();
ENG_ASSERT(!anim.Valid());
ENG_ASSERT(anim.CurrentFrame() == 0.0f);
```

### `bool Valid() const`

`true`, если документ успешно разобран и содержит хотя бы один слой. Все
методы отрисовки при `Valid() == false` молча ничего не рисуют.

```cpp
if (!anim.Valid()) {
    ENG_LOGW("lottie", "документ не разобран — рисовать нечего");
    return;
}
```

### `const std::string& Name() const`

Имя композиции из поля `nm`. Возвращается ссылка на внутреннюю строку.

```cpp
crossrender::LottieAnimation anim;
anim.LoadFromFile("assets/lottie/success.json");
ENG_LOGI("lottie", "композиция: %s", anim.Name().c_str());
```

### `f32 Duration() const`

Длительность в секундах: `totalFrames / frameRate`. Частота кадров берётся из
`fr` и при некорректном значении поднимается до `60`.

```cpp
const crossrender::f32 d = anim.Duration();
ENG_LOGI("lottie", "длительность: %.2f c", d);
```

### `f32 FrameRate() const`

Частота кадров композиции (`fr`, по умолчанию `60`). Используется для перевода
секунд в кадры в `SetTime` и `Advance`.

```cpp
const crossrender::f32 fps = anim.FrameRate();
anim.SetSpeed(fps > 0.0f ? 1.0f : 0.0f);
```

### `int TotalFrames() const`

Число кадров композиции: `ceil(op − ip)`, минимум `1`. Если `op <= ip`, парсер
подставляет `op = ip + 1`.

```cpp
const int frames = anim.TotalFrames();
for (int f = 0; f < frames; f += 10) {
    anim.SetFrame(static_cast<crossrender::f32>(f));
}
```

### `int Width() const` / `int Height() const`

Размер композиции в пикселях (`w`, `h`). `Render` использует его для
сохранения пропорций, маски — для прямоугольника композиции, сплошные слои —
как запасной размер.

```cpp
const crossrender::Vec2 compSize{static_cast<crossrender::f32>(anim.Width()),
                         static_cast<crossrender::f32>(anim.Height())};
const crossrender::Rect dst{0, 0, compSize.x, compSize.y};
```

### `int LayerCount() const` / `const std::vector<LottieLayer>& Layers() const`

Число слоёв верхнего уровня и сам список. Слои прекомпозиций лежат в
`LottieAsset::layers` и в этот список не входят; порядок — как в документе
(сверху вниз), то есть обратный порядку отрисовки.

```cpp
for (const crossrender::LottieLayer& layer : anim.Layers()) {
    ENG_LOGI("lottie", "слой %d '%s': тип %d", layer.index, layer.name.c_str(),
             static_cast<int>(layer.type));
}
```

### `const std::vector<LottieAsset>& Assets() const`

Список ассетов: прекомпозиции и изображения. `imageAssetIndex` и
`precompIndex` слоёв — это индексы именно в этом векторе.

```cpp
const std::vector<crossrender::LottieAsset>& assets = anim.Assets();
ENG_LOGI("lottie", "ассетов: %d", static_cast<int>(assets.size()));
```

### `const JsonValue& Raw() const`

Исходный разобранный JSON. Пригодится, чтобы прочитать поля, которых нет в
модели: маркеры (`markers`), метаданные экспорта, версию bodymovin.

```cpp
const crossrender::JsonValue& root = anim.Raw();
const crossrender::JsonValue* version = root.Find("v");
if (version && version->IsString()) ENG_LOGI("lottie", "bodymovin %s", version->AsString().c_str());
```

### `usize MemoryUsage() const`

Приблизительная оценка памяти, занимаемой документом: размер объекта, строки,
дамп JSON, слои, фигуры, точки путей и данные изображений. Точное значение не
гарантируется — это инструмент профилирования, а не учёт аллокатора.

```cpp
const crossrender::usize bytes = anim.MemoryUsage();
ENG_LOGI("lottie", "документ занимает ~%.1f КиБ", static_cast<double>(bytes) / 1024.0);
```

### `void Play()`

Запускает воспроизведение. Если зацикливание выключено и текущий кадр уже на
конце (или за концом) активного диапазона, кадр возвращается в начало сегмента,
а флаг завершения снимается — то есть `Play` после `Finished()` перезапускает
ролик.

```cpp
anim.Play();
ENG_ASSERT(anim.Playing());
```

### `void Pause()`

Останавливает продвижение времени, сохраняя текущий кадр. `Advance` после этого
не меняет состояние, пока снова не вызван `Play`.

```cpp
anim.Pause();
anim.Advance(0.5f);                  // кадр не сдвинется
ENG_ASSERT(!anim.Playing());
```

### `void Stop()`

Останавливает воспроизведение и возвращает кадр в начало активного сегмента,
снимая флаг завершения.

```cpp
anim.Stop();
ENG_ASSERT(anim.CurrentFrame() == 0.0f);
```

### `void SetLoop(bool loop)` / `bool Looping() const`

Включает и читает зацикливание. При выключенном цикле `Advance` упирается в
конец диапазона и поднимает `Finished`; при включённом — время заворачивается,
а `LoopCount` считает пройденные циклы.

```cpp
anim.SetLoop(false);                 // одноразовый ролик
ENG_LOGI("lottie", "зациклен: %d", anim.Looping() ? 1 : 0);
```

### `void SetFrame(f32 frame)`

Ставит текущий кадр, зажимая его в `[0, TotalFrames()]`, и снимает флаг
завершения. Границы активного сегмента здесь **не** учитываются — для сегмента
используйте `SetSegment`, который сам зажимает кадр.

```cpp
anim.SetFrame(30.0f);                // середина секунды при 60 fps
```

### `void SetTime(f32 seconds)`

То же, что `SetFrame(seconds * FrameRate())`. Удобно для синхронизации ролика
с игровым таймером.

```cpp
anim.SetTime(0.25f);                 // 15-й кадр при 60 fps
```

### `void SetSpeed(f32 speed)` / `f32 Speed() const`

Множитель скорости: `2` — вдвое быстрее, `0.5` — вдвое медленнее, отрицательное
значение играет назад. Скорость учитывается в `Advance`; при отрицательной
скорости и выключенном цикле ролик завершается, дойдя до начала.

```cpp
anim.SetSpeed(2.0f);
ENG_LOGI("lottie", "скорость: %.2f", anim.Speed());
```

### `void SetSegment(f32 startFrame, f32 endFrame)`

Ограничивает воспроизведение явным отрезком в кадрах. Границы при
необходимости меняются местами, текущий кадр зажимается в отрезок, и с этого
момента `Advance`, `Progress` и `Play` работают только внутри него. Снять
ограничение отдельным методом нельзя — только `Destroy` / повторный разбор.

```cpp
anim.SetSegment(15.0f, 45.0f);       // играем только средние полсекунды
anim.Play();
```

### `f32 CurrentFrame() const`

Текущий кадр композиции — то, что передаётся в отрисовку (делится на
`FrameRate`, чтобы получить секунды для трансформов).

```cpp
ENG_LOGI("lottie", "кадр %.1f из %d", anim.CurrentFrame(), anim.TotalFrames());
```

### `f32 CurrentTime() const`

Текущее время в секундах: `currentFrame_ / frameRate_`. При нулевой частоте
делитель берётся равным `1`, поэтому деления на ноль нет.

```cpp
const crossrender::f32 t = anim.CurrentTime();
```

### `f32 Progress() const`

Прогресс внутри активного диапазона в `[0, 1]`. Если диапазон вырожден
(длительность не положительна), возвращается `1`.

```cpp
ENG_LOGI("lottie", "прогресс: %.0f%%", anim.Progress() * 100.0f);
```

### `bool Playing() const` / `bool Finished() const`

Текущее состояние: идёт ли воспроизведение и достигнут ли конец при
выключенном зацикливании. `Finished` снимается новым `Play`, `SetFrame` и
`Stop`.

```cpp
if (anim.Finished()) {
    ENG_LOGI("lottie", "ролик доигран");
}
```

### `int LoopCount() const`

Сколько полных циклов прошёл ролик с момента разбора. Счётчик растёт в
`Advance` при зацикливании; на отрицательной скорости назад он не уменьшается.

```cpp
ENG_LOGI("lottie", "циклов пройдено: %d", anim.LoopCount());
```

### `void Advance(f32 dt)`

Продвигает время на `dt` секунд. Ничего не делает, если ролик на паузе, `dt`
равен нулю или не конечен. Шаг ограничен `±3600` кадров за вызов. При
зацикливании время заворачивается и увеличивается `LoopCount`; без
зацикливания кадр зажимается концами диапазона и поднимается `Finished`.
Ролик с нулевой длительностью сразу считается завершённым.

```cpp
anim.Play();
anim.Advance(1.0f / 60.0f);
anim.Advance(1.0f / 60.0f);
```

### `void Update(f32 dt)`

Устаревший псевдоним `Advance(dt)` — оставлен для совместимости. Новый код
должен вызывать `Advance`.

```cpp
anim.Update(1.0f / 60.0f);           // то же, что Advance
```

### `void Render(Renderer2D& r, const Rect& dst, f32 alpha = 1.0f, const Color& tint = Color::White) const`

Рисует текущий кадр, вписывая композицию в `dst` с сохранением пропорций и
центрированием. `alpha` домножает прозрачности всех слоёв, `tint` домножает
RGB-каналы цветов. Метод сбрасывает статистику, использует загрузчик из
`SetImageLoader` и шрифт из `SetFont` (или шрифт по умолчанию), а в конце
возвращает `GlobalAlpha` в `1`. Вызов внутри `BeginFrame`/`EndFrame` рендерера.

```cpp
crossrender::Renderer2D r2d;
r2d.Init();
r2d.BeginFrame(1280, 800);
const crossrender::Rect box{100.0f, 100.0f, 240.0f, 240.0f};
anim.Render(r2d, box, 0.9f, crossrender::Color::White);
r2d.EndFrame();
```

### `void RenderAt(Renderer2D& r, const Vec2& position, const Vec2& size, f32 rotation = 0, f32 alpha = 1.0f) const`

Рисует кадр, растягивая композицию ровно в `size` и размещая её центром в
`position`; `rotation` задаётся в радианах. Тинт всегда белый, а
зарегистрированный `ImageLoader` **не используется** — изображения берутся
только из встроенных данных или по пути из документа.

```cpp
crossrender::Renderer2D r2d;
r2d.Init();
r2d.BeginFrame(1280, 800);
anim.RenderAt(r2d, crossrender::Vec2{640.0f, 400.0f}, crossrender::Vec2{200.0f, 200.0f}, 0.2f, 1.0f);
r2d.EndFrame();
```

### `using ImageLoader`

Тип функции-загрузчика изображений:
`std::function<const Texture*(const std::string& name)>`. Плеер передаёт ей
`LottieLayer::imageName` и ожидает указатель на живую текстуру; владение
остаётся на стороне вызывающего, плеер её не удаляет. Если загрузчик вернул
`nullptr`, плеер попробует встроенные данные и путь из документа.

```cpp
crossrender::LottieAnimation::ImageLoader loader = [](const std::string& name) -> const crossrender::Texture* {
    return MyAssetCache().FindTexture(name);     // nullptr, если нет
};
```

### `void SetImageLoader(ImageLoader loader)`

Регистрирует загрузчик изображений. Используется только методом `Render`;
`RenderAt` его игнорирует.

```cpp
anim.SetImageLoader([](const std::string& name) -> const crossrender::Texture* {
    return MyAssetCache().FindTexture(name);     // ваш постоянный кэш текстур
});
```

### `void SetFont(Font* font)` / `Font* GetFont() const`

Задаёт и читает шрифт для текстовых слоёв. Указатель не копируется. Если шрифт
не задан, при отрисовке берётся `FontManager::Get().DefaultFont()`;
`GetFont()` при этом по-прежнему возвращает `nullptr` — подстановка происходит
внутри `Render`/`RenderAt`.

```cpp
crossrender::Font* font = crossrender::FontManager::Get().DefaultFont();
if (font && font->Valid()) anim.SetFont(font);
ENG_LOGI("lottie", "шрифт задан: %d", anim.GetFont() != nullptr ? 1 : 0);
```

### `struct LottieAnimation::RenderStats`

Счётчики последнего вызова отрисовки. Обнуляются в начале `Render`/`RenderAt`,
поэтому значения относятся ровно к одному кадру. Удобны для профилирования
сложных роликов.

| Поле | Смысл |
|---|---|
| `int layersDrawn` | сколько слоёв реально отрисовано |
| `int shapesDrawn` | сколько заливок/обводок выпущено |
| `int masksApplied` | сколько масок и матов применено |
| `int imagesDrawn` | сколько изображений нарисовано |
| `int textDrawn` | сколько текстовых строк/символов нарисовано |

```cpp
anim.Render(r2d, box);
const crossrender::LottieAnimation::RenderStats& s = anim.LastRenderStats();
ENG_LOGI("lottie", "слоёв %d, фигур %d, масок %d, картинок %d, текстов %d", s.layersDrawn,
         s.shapesDrawn, s.masksApplied, s.imagesDrawn, s.textDrawn);
```

### `const RenderStats& LastRenderStats() const`

Статистика последнего `Render` / `RenderAt`. Возвращается ссылка на поле
`mutable`, поэтому метод работает и у константного объекта.

```cpp
const crossrender::LottieAnimation& ref = anim;
if (ref.LastRenderStats().layersDrawn == 0) {
    ENG_LOGW("lottie", "кадр пустой: возможно, все слои скрыты");
}
```

### `struct LottieAnimation::ValidationResult`

Результат проверки документа: флаг `ok` и список текстовых ошибок. Проверяются
идентификаторы ассетов, типы слоёв, порядок `op`/`ip`, знак `timeStretch`,
ссылки прекомпозиций и изображений, висячие родители и нераспознанные типы
фигур. `ok == true` означает, что список ошибок пуст.

| Поле | Смысл |
|---|---|
| `bool ok` | ошибок нет |
| `std::vector<std::string> errors` | человекочитаемые описания проблем |

```cpp
const crossrender::LottieAnimation::ValidationResult v = anim.Validate();
if (!v.ok) {
    for (const std::string& e : v.errors) ENG_LOGW("lottie", "проверка: %s", e.c_str());
}
```

### `ValidationResult Validate() const`

Прогоняет проверки по разобранному документу. Полезно в тестах и на этапе
импорта ассетов: парсер намеренно терпим к мусору и не отклоняет документ, а
`Validate` показывает, что именно в нём подозрительно.

```cpp
crossrender::LottieAnimation anim;
anim.LoadFromJson(documentWithMissingImage);
if (!anim.Validate().ok) ENG_LOGW("lottie", "документ содержит ошибки ссылок");
```

### crossrender::LottieLibrary

Singleton-кэш разобранных анимаций. `Load` кэширует по пути, `LoadOrGenerate` —
по имени сгенерированного ролика. Библиотека владеет объектами и живёт до конца
процесса, поэтому выданные указатели остаются валидными до `Clear`.

```cpp
crossrender::LottieAnimation* anim = crossrender::LottieLibrary::Get().LoadOrGenerate("success");
if (anim) anim->Play();
```

### `static LottieLibrary& Get()`

Единственный экземпляр библиотеки (статическая локальная переменная). Потоковой
безопасности нет: вызывайте из потока, который владеет ассетами.

```cpp
crossrender::LottieLibrary& lib = crossrender::LottieLibrary::Get();
lib.Clear();                          // например при выходе из сцены
```

### `LottieAnimation* LottieLibrary::Load(const std::string& path)`

Загружает документ по пути и кэширует его. Повторный вызов с тем же путём
возвращает тот же указатель. При ошибке пишет предупреждение и возвращает
`nullptr` (неудача не кэшируется, так что повторная попытка возможна).

```cpp
crossrender::LottieAnimation* heart = crossrender::LottieLibrary::Get().Load("assets/lottie/heart.json");
if (!heart) ENG_LOGW("lottie", "heart.json не загрузился");
```

### `LottieAnimation* LottieLibrary::LoadOrGenerate(const std::string& name)`

Возвращает процедурный ролик по имени. Поддержаны `"loading"`, `"success"`,
`"heart"`, `"checkmark"`, `"card-flip"`; неизвестное имя (в том числе
`"pulse"`) даёт пульсацию. JSON генерируется `GenerateLottieJson`, по
возможности сохраняется в `<user>/lottie/<name>.json` и разбирается тем же
парсером, что и обычный файл. Кэш ведётся по ключу `"gen:" + name`.

```cpp
crossrender::LottieAnimation* pulse = crossrender::LottieLibrary::Get().LoadOrGenerate("pulse");
if (pulse) {
    pulse->SetLoop(true);
    pulse->Play();
}
```

### `void LottieLibrary::Clear()`

Уничтожает все кэшированные анимации и очищает кэш. Все ранее выданные
указатели становятся висячими — вызывайте только тогда, когда на них никого не
осталось.

```cpp
crossrender::LottieLibrary::Get().Clear();
```

### `std::string GenerateLottieJson(const std::string& name)`

Свободная функция: собирает bodymovin-совместимый JSON встроенного ролика.
Документы самодостаточны (без внешних картинок и шрифтов) и используют только
те возможности, которые реализует плеер, поэтому генератор заодно служит
проверкой парсера.

```cpp
const std::string json = crossrender::GenerateLottieJson("heart");
crossrender::LottieAnimation anim;
if (anim.LoadFromJson(json)) ENG_LOGI("lottie", "сердце: %.2f c", anim.Duration());
```

## Пример целиком

```cpp
#include "crossrender/anim/Lottie.h"
#include "crossrender/core/Log.h"
#include "crossrender/gfx/Renderer2D.h"
#include "crossrender/text/Font.h"

#include <string>

// Показывает ролик результата поверх карточной сцены: одноразовая анимация
// «success», загруженная из файла, с запасным вариантом — сгенерированным.
class ResultBanner {
public:
    void Init(crossrender::Renderer2D& r2d) {
        r2d_ = &r2d;
        font_ = crossrender::FontManager::Get().DefaultFont();

        std::string error;
        if (!anim_.LoadFromFile("assets/lottie/success.json", &error)) {
            ENG_LOGW("lottie", "файл недоступен (%s), берём встроенный ролик", error.c_str());
            anim_.LoadFromJson(crossrender::GenerateLottieJson("success"), &error);
        }

        anim_.SetFont(font_);
        anim_.SetLoop(false);
        anim_.SetSpeed(1.0f);
        anim_.SetImageLoader([this](const std::string& name) -> const crossrender::Texture* {
            return images_.Find(name);        // текстуры живут в нашем кэше
        });

        if (anim_.Valid()) {
            const crossrender::LottieAnimation::ValidationResult v = anim_.Validate();
            if (!v.ok) {
                for (const std::string& e : v.errors) ENG_LOGW("lottie", "документ: %s", e.c_str());
            }
            anim_.Play();
        }
    }

    void Update(crossrender::f32 dt) {
        anim_.Advance(dt);
        // По завершении возвращаемся к началу — баннер снова готов к показу.
        if (anim_.Finished()) anim_.Stop();
    }

    void Draw(const crossrender::Rect& area) {
        if (!anim_.Valid()) return;

        // Тень и подложка рисуются обычным рендерером, ролик — поверх.
        r2d_->FillRoundedRect(area, 12.0f, crossrender::Color{0, 0, 0, 0.45f});

        // Render вписывает композицию в прямоугольник с сохранением пропорций
        // и центрирует её; alpha и tint передаются множителями.
        anim_.Render(*r2d_, area.Inset(8.0f), 1.0f, crossrender::Color::White);

        const crossrender::LottieAnimation::RenderStats& s = anim_.LastRenderStats();
        if (s.layersDrawn == 0 && anim_.Playing()) {
            ENG_LOGD("lottie", "кадр %.1f пуст", anim_.CurrentFrame());
        }
    }

    // Иконка в углу экрана: тем же документом, но растянутым и повёрнутым.
    void DrawIcon(const crossrender::Vec2& center) {
        if (!anim_.Valid()) return;
        anim_.RenderAt(*r2d_, center, crossrender::Vec2{64.0f, 64.0f}, 0.0f, 0.8f);
    }

private:
    crossrender::Renderer2D* r2d_ = nullptr;
    crossrender::Font* font_ = nullptr;
    crossrender::LottieAnimation anim_;
    struct ImageCache {
        const crossrender::Texture* Find(const std::string&) const { return nullptr; }
    } images_;
};

int main() {
    crossrender::Renderer2D r2d;
    if (!r2d.Init()) {
        ENG_LOGE("lottie", "2D-рендерер не инициализировался");
        return 1;
    }

    // Библиотека кэширует по имени: повторные вызовы бесплатны.
    crossrender::LottieAnimation* heart = crossrender::LottieLibrary::Get().LoadOrGenerate("heart");
    if (heart) {
        heart->SetLoop(true);
        heart->SetSpeed(0.75f);
        heart->Play();
    }

    ResultBanner banner;
    banner.Init(r2d);

    const int fbWidth = 1280;
    const int fbHeight = 800;
    for (int frame = 0; frame < 180; ++frame) {
        const crossrender::f32 dt = 1.0f / 60.0f;
        banner.Update(dt);
        if (heart) heart->Advance(dt);

        r2d.BeginFrame(fbWidth, fbHeight, 1.0f);
        if (heart) heart->Render(r2d, crossrender::Rect{40.0f, 40.0f, 96.0f, 96.0f});
        banner.Draw(crossrender::Rect{400.0f, 250.0f, 480.0f, 300.0f});
        banner.DrawIcon(crossrender::Vec2{1200.0f, 120.0f});
        r2d.EndFrame();
    }

    if (heart) {
        ENG_LOGI("lottie", "готово: '%s', %.2f c, слоёв %d, память ~%.1f КиБ", heart->Name().c_str(),
                 heart->Duration(), heart->LayerCount(),
                 static_cast<double>(heart->MemoryUsage()) / 1024.0);
    }
    r2d.Shutdown();
    return 0;
}
```

## См. также

* `docs/anim/Anim.md` — скелетная анимация: треки, клипы и блендинг.
* `docs/gfx/Renderer2D.md` — рендерер, через который Lottie и рисуется:
  пути, заливки, `ClipPath`, режимы смешивания.
* `docs/core/Json.md` — разбор JSON, на котором стоит `LottieAnimation::Parse`
  и `Raw()`.
* `docs/core/File.md` — `ReadTextFile` и пользовательский корень, куда
  `LottieLibrary::LoadOrGenerate` пишет сгенерированные документы.
* `docs/text/Font.md` — шрифт для текстовых слоёв и `FontManager`.
