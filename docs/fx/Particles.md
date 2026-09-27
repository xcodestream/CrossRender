# crossrender/fx/Particles.h — система частиц

Позиционные (3D) частицы: CPU-симуляция с кривыми по времени жизни, эмиттеры
с формами и силами, суб-эмиттеры, аттракторы, столкновение с землёй, следы
(trail) и отрисовка инстансами через встроенный шейдер `Renderer3D`.

## Заголовок

```cpp
#include "crossrender/fx/Particles.h"
```

## Обзор

`Particles.h` описывает **описание** системы (`ParticleEmitterDesc` — эмиттер),
**состояние** одной частицы (`Particle`) и **владельца** пулов и отрисовки
(`ParticleSystem`). Вспомогательные типы (`Curve`, `ColorCurve`, `RangeF`,
`RangeV3`) задают значения, которые эмиттер сэмплирует при рождении частицы и
во время её жизни.

### Система обновляется и рисуется сценой

В движке **нет** автоматического шага частиц: ни `Scene`, ни `Engine` не
вызывают `Simulate`. Сцена сама, в своём `Update`, вызывает `Simulate(dt)`, а в
`Render` — `Render(renderer, camera)` (или один вызов `Update(renderer, camera,
dt)`). Из этого следуют две важные особенности:

* **Система, которую перестали обновлять, продолжает рисовать последнее
  состояние.** `Render` только собирает инстансы из текущих пулов и ничего не
  интегрирует. Забыли `Simulate` — частицы «замрут» в воздухе.
* Систему можно обновлять реже кадра (например, раз в два кадра) или с
  фиксированным шагом — это решение вызывающего кода.

```cpp
class FireScene : public crossrender::Scene {
public:
    const char* Name() const override { return "Fire"; }
    [[nodiscard]] bool Wants3D() const override { return true; }

    void OnEnter(crossrender::SceneContext& ctx) override {
        (void)ctx;
        // Один раз при входе в сцену: пулы, RNG и описания эмиттеров.
        fire_.Init(crossrender::ParticleSystem::PresetFire(), 12345u);
        fire_.SetTransform(crossrender::Mat4::Translate({0.0f, 0.2f, -3.0f}));
    }

    void Update(crossrender::SceneContext& ctx, crossrender::f32 dt) override {
        (void)ctx;
        // Движок сам не двигает частицы — это делает сцена.
        fire_.Simulate(dt);
    }

    void Render3D(crossrender::SceneContext& ctx) override {
        // Если убрать Simulate выше, Render3D продолжит рисовать ту же картину.
        fire_.Render(*ctx.r3d, ctx.r3d->GetCamera());
    }

private:
    crossrender::ParticleSystem fire_;
};
```

### Пресет или эмиттер, собранный вручную

**Пресет** — это статическая фабрика (`PresetFire`, `PresetSmoke`, …), которая
возвращает готовый `std::vector<ParticleEmitterDesc>`. Это обычные значения:
их можно менять до `Init` или уже после него через `Emitter(i)`, потому что
`Simulate` пересчитывает кэш кривых и имён.

**Эмиттер, собранный вручную**, — это та же структура, заполненная полями:
форма, скорость, время жизни, гравитация, режим отрисовки. Пресет нужен как
отправная точка и пример разумных чисел, а не как отдельный «класс» частиц.

```cpp
// Вручную: маленький искровой фонтан из точки.
crossrender::ParticleEmitterDesc d;
d.name = "Sparks";
d.rate = 120.0f;
d.burst = 20;
d.maxParticles = 400;
d.lifetime = {0.4f, 0.9f};
d.startSpeed = {3.0f, 6.0f};
d.startSize = {0.03f, 0.07f};
d.shape = crossrender::EmitterShape::Point;
d.gravity = {0.0f, -9.81f, 0.0f};
d.renderMode = crossrender::ParticleRenderMode::StretchedBillboard;
d.blendMode = crossrender::ParticleBlendMode::Additive;

crossrender::ParticleSystem sys;
sys.Init({d}, 7u);

// Пресет: то же самое, но с готовыми кривыми и цветами.
crossrender::ParticleSystem preset;
preset.Init(crossrender::ParticleSystem::PresetMagic(), 7u);
// Правка после Init: имя и кривые подхватятся на следующем Simulate.
preset.Emitter(0).rate *= 0.5f;
```

### Пространство эмиттера: World и Local

`ParticleEmitterDesc::space` решает, где живут координаты частиц.

* `SimulationSpace::World` — позиция, форма и направление трансформируются
  матрицей `SetTransform` **в момент рождения** частицы; дальше частица
  движется в мировых координатах и не следует за эмиттером.
* `SimulationSpace::Local` — частица симулируется в локальных координатах, а
  трансформация применяется на CPU при сборке инстансов. Двигая
  `SetTransform`, вы двигаете всю уже рождённую систему целиком (как «дым от
  едущей машины»).

Встроенный вершинный шейдер частиц применяет `uModel` только в режиме `Mesh`,
поэтому в остальных режимах `uModel` всегда единичный, а локальное
преобразование запекается на CPU.

```cpp
crossrender::ParticleSystem trail;
trail.Init(crossrender::ParticleSystem::PresetTrail(), 99u);
trail.Emitter(0).space = crossrender::SimulationSpace::Local;   // частицы едут вместе с эмиттером

// Матрица задаёт положение/поворот всей системы целиком.
trail.SetTransform(crossrender::Mat4::TRS({1.0f, 0.5f, 0.0f}, {0.0f, 1.2f, 0.0f}, {1, 1, 1}));
trail.Simulate(1.0f / 60.0f);
```

### Бюджет, лимиты и переработка

Бюджет частиц **жёсткий** и проверяется на двух уровнях:

* Суммарный лимит движка — 200 000 слотов на одну `ParticleSystem`. Если сумма
  `maxParticles` всех эмиттеров больше, каждый пул пропорционально ужимается, и
  в лог уходит предупреждение `ENG_LOGW("fx", ...)`.
* Пул каждого эмиттера — `max(1, maxParticles * scale)`. Когда пул заполнен,
  новые частицы **не создаются** (`Spawn` возвращает ошибку), а накопленный
  «хвост» эмиссии обрезается: `accumulator` не превышает 1. Живые частицы при
  этом не убиваются — они спокойно доживают свой `lifetime`, и эмиссия
  возобновляется, как только слоты освободятся.
* Единственное, что реально перерабатывается по кругу, — **история следа**:
  режим `Trail` хранит на частицу кольцевой буфер из 8 последних позиций, и
  новая точка вытесняет самую старую.

```cpp
crossrender::ParticleSystem budget;
budget.Init(crossrender::ParticleSystem::PresetSnow(), 1u);
const int cap = budget.Capacity();          // суммарное число слотов после ужатия
budget.EmitBurst(0, cap * 4);               // лишние частицы будут отброшены
ENG_LOGI("fx", "занято %d из %d", budget.AliveCount(), cap);
```

### Как частицы попадают на экран

Частицы рисуются **не** через `Renderer2D`: это инстансная 3D-геометрия
(билборд-квад или меш), которую собирает `ParticleSystem::Render`. Так как
`Renderer3D` батчит геометрию между `BeginFrame` и `EndFrame`, система не
рисует сразу, а **откладывает** вызов через `Renderer3D::AddPostDraw`: колбэк
выполняется в конце кадра, уже после непрозрачной и прозрачной геометрии,
поэтому частицы композитируются с правильным порядком по глубине.

Внутри колбэка система сама настраивает GL-состояние (тест глубины включён,
запись глубины выключена, если эмиттер не `lit` и не `castShadows`, отсечение
выключено) и сама привязывает per-instance атрибуты (локации 3…6, шаг 64
байта, divisor 1), после чего рисует `Mesh::DrawInstanced` и восстанавливает
состояние. Встроенный шейдер частиц домножает цвет на альфу, поэтому факторы
смешивания для `Alpha`/`Premultiplied`/`Additive` — `ONE`-based.

GPU-ресурсы (VBO инстансов, юнит-квад, шейдер) создаются **лениво на первом
`Render`**: у `Init` нет контекста OpenGL, и он обязан работать headless (тесты
вызывают `Init`/`Simulate` без окна). Если ленивое создание не удалось,
симуляция продолжается, а рисование молча пропускается.

```cpp
// Внутри Scene::Render: Renderer3D сам вызовет эту работу в конце кадра.
sys.Render(r3d, r3d.GetCamera());
// Порядок в кадре: BeginFrame -> обычная геометрия -> AddPostDraw -> EndFrame.
```

## Члены класса

### `struct Curve`

Скалярная кривая по времени жизни частицы: `t` нормирован в `[0, 1]`
(0 — рождение, 1 — смерть). Кривая — это отсортированный список ключей плюс
диапазон значений, который используется для случайного разброса.

```cpp
// Прозрачность: быстро проявиться и медленно погаснуть.
crossrender::Curve alpha = crossrender::Curve::FromPoints({{0.0f, 0.0f}, {0.15f, 1.0f}, {1.0f, 0.0f}}, 0.4f);
crossrender::ParticleEmitterDesc d;
d.alphaOverLifetime = alpha;
```

### `struct Curve::Key` и его поля

Один ключ кривой. Поля сгруппированы в таблицу (это данные параметрической
структуры, а не методы), пример ниже общий для всей группы.

| Поле | По умолчанию | Смысл |
|---|---|---|
| `f32 t` | `0` | Время ключа в `[0, 1]`; ключи хранятся отсортированными по `t`. |
| `f32 v` | `0` | Значение кривой в этом ключе. |
| `f32 outX, outY` | `0.33f, 0.33f` | Правая ручка Безье (easing «как в Lottie»). |
| `f32 inX, inY` | `0.67f, 0.67f` | Левая ручка Безье следующего сегмента. |
| `int easing` | `0` | `0` — линейно, `1` — Безье, `2` — удержание значения (`hold`). |

```cpp
crossrender::Curve::Key k;
k.t = 0.5f;
k.v = 2.0f;
k.easing = 1;                 // сглаженный сегмент
k.outX = 0.4f;  k.outY = 0.0f;
k.inX = 0.6f;   k.inY = 1.0f;

crossrender::Curve c;
c.keys.push_back(k);          // ключи можно добавлять и напрямую
```

### Поля `Curve`: `keys`, `minValue`, `maxValue`

Данные кривой сгруппированы: сам список ключей и диапазон значений, из
которого `RandomValue` тянет случайное число. Один пример на группу.

| Поле | По умолчанию | Смысл |
|---|---|---|
| `std::vector<Key> keys` | пусто | Отсортированные ключи; пустой список означает константу `minValue`. |
| `f32 minValue` | `0` | Нижняя граница значений (обновляется `AddKey` и фабриками). |
| `f32 maxValue` | `1` | Верхняя граница значений. |

```cpp
crossrender::Curve c = crossrender::Curve::FromPoints({{0.0f, 0.5f}, {1.0f, 3.0f}});
ENG_LOGI("fx", "ключей %d, диапазон [%.2f, %.2f]",
         static_cast<int>(c.keys.size()), c.minValue, c.maxValue);
```

### `static Curve Curve::Constant(f32 v)`

Фабрика: кривая, у которой значение не меняется. Возвращает ключи `(0, v)` и
`(1, v)` и сразу выставляет `minValue = maxValue = v`; симуляция распознаёт
такую кривую и не вызывает `Evaluate` на каждом шаге.

```cpp
crossrender::ParticleEmitterDesc d;
d.sizeOverLifetime = crossrender::Curve::Constant(1.0f);    // размер не меняется
d.speedOverLifetime = crossrender::Curve::Constant(1.0f);   // строгий no-op для движения
```

### `static Curve Curve::FromPoints(const std::vector<Vec2>& points, f32 smoothness = 0.0f)`

Фабрика «ломаной» по парам `(t, value)`. Точки сортируются по `t`, дубликаты
времени схлопываются (побеждает последнее значение). При `smoothness > 0`
сегменты сглаживаются: считаются касательные по конечным разностям
(Catmull-Rom) и превращаются в ручки Безье. Пустой список даёт пустую кривую,
одна точка — `Constant`.

```cpp
// Размер растёт, затем резко падает к концу жизни.
crossrender::Curve size = crossrender::Curve::FromPoints(
    {{0.0f, 0.45f}, {0.25f, 1.0f}, {0.7f, 0.75f}, {1.0f, 0.05f}}, 0.6f);
crossrender::ParticleEmitterDesc d;
d.sizeOverLifetime = size;
```

### `f32 Curve::Evaluate(f32 t) const`

Возвращает значение кривой в момент `t`. До первого ключа — значение первого
ключа, после последнего — последнего. Для `easing == 2` значение держится до
следующего ключа, для `easing == 1` используется решатель Безье
(Ньютон–Рафсон с бисекцией как запасным вариантом). Сложность — двоичный поиск.

```cpp
crossrender::Curve c = crossrender::Curve::FromPoints({{0.0f, 1.0f}, {1.0f, 0.0f}});
const crossrender::f32 atStart = c.Evaluate(0.0f);   // 1.0
const crossrender::f32 middle  = c.Evaluate(0.5f);   // 0.5
const crossrender::f32 atEnd   = c.Evaluate(1.0f);   // 0.0
```

### `void Curve::AddKey(f32 t, f32 v, int easing = 0)`

Вставляет ключ с сохранением сортировки. `t` зажимается в `[0, 1]`; при
совпадении времени ключ заменяется, и диапазон `minValue`/`maxValue`
пересчитывается по всем ключам.

```cpp
crossrender::Curve size = crossrender::Curve::Constant(0.2f);
size.AddKey(0.5f, 1.4f);       // всплеск в середине жизни
size.AddKey(1.0f, 0.1f);
ENG_LOGI("fx", "диапазон теперь [%.2f, %.2f]", size.minValue, size.maxValue);
```

### `f32 Curve::RandomValue(Random& rng) const`

Случайное значение **в диапазоне кривой** (не по форме кривой): просто
`rng.Range(minValue, maxValue)`. Используется, когда нужно разбросать
начальные значения, а не промодулировать их по времени.

```cpp
crossrender::Random rng(2024u);
crossrender::Curve speed = crossrender::Curve::FromPoints({{0.0f, 1.0f}, {1.0f, 5.0f}});
const crossrender::f32 v = speed.RandomValue(rng);   // где-то между 1 и 5
ENG_LOGI("fx", "случайная скорость %.3f", v);
```

### `struct ColorCurve`

Цветовая кривая: список пар `(t, Color)`. Между ключами цвет линейно
интерполируется покомпонентно (включая альфу).

```cpp
crossrender::ColorCurve cc = crossrender::ColorCurve::Gradient({{0.0f, crossrender::Color::FromRGB(0xFFFFFF)},
                                                {1.0f, crossrender::Color::FromRGB(0xE0431A)}});
crossrender::ParticleEmitterDesc d;
d.colorOverLifetime = cc;
```

### `std::vector<std::pair<f32, Color>> ColorCurve::keys`

Данные цветовой кривой. Ключи хранятся отсортированными по времени; пустой
список означает белый цвет, список из одного ключа — постоянный цвет.

```cpp
crossrender::ColorCurve cc;
cc.keys.push_back({0.0f, crossrender::Color(1, 1, 0, 1)});
cc.keys.push_back({1.0f, crossrender::Color(1, 0, 0, 0)});
ENG_LOGI("fx", "ключей цвета: %d", static_cast<int>(cc.keys.size()));
```

### `static ColorCurve ColorCurve::Constant(const Color& c)`

Фабрика постоянного цвета: два ключа с одинаковым значением. Такой цвет
распознаётся симуляцией как константа и умножается на `startColor` один раз.

```cpp
crossrender::ParticleEmitterDesc d;
d.startColor = crossrender::ColorCurve::Constant(crossrender::Color(1.0f, 0.8f, 0.4f, 1.0f));
d.colorOverLifetime = crossrender::ColorCurve::Constant(crossrender::Color::White);
```

### `static ColorCurve ColorCurve::Gradient(const std::vector<std::pair<f32, Color>>& keys)`

Фабрика градиента: ключи сортируются по времени, дальше цвет интерполируется
линейно. Типичный способ задать «огненный» или «дымный» переход.

```cpp
// Огонь: белый -> жёлтый -> оранжевый -> тёмно-красный.
crossrender::ColorCurve fire = crossrender::ColorCurve::Gradient({
    {0.0f,  crossrender::Color::FromRGB(0xFFF3B0)},
    {0.35f, crossrender::Color::FromRGB(0xFFA02A)},
    {0.75f, crossrender::Color::FromRGB(0xE0431A)},
    {1.0f,  crossrender::Color::FromRGB(0x40100A)},
});
crossrender::ParticleEmitterDesc d;
d.colorOverLifetime = fire;
```

### `Color ColorCurve::Evaluate(f32 t) const`

Цвет в момент `t`. До первого ключа — первый цвет, после последнего —
последний. Для `t` между ключами — линейная интерполяция `Lerp`.

```cpp
crossrender::ColorCurve cc = crossrender::ColorCurve::Gradient({{0.0f, crossrender::Color::FromRGB(0x000000)},
                                                {1.0f, crossrender::Color::FromRGB(0xFFFFFF)}});
const crossrender::Color mid = cc.Evaluate(0.5f);
ENG_LOGI("fx", "серый: %.2f", mid.r);
```

### `struct RangeF`

Диапазон вещественных значений. При рождении частицы эмиттер вызывает
`Sample(rng)` и получает конкретное число; так задаются разбросы времени
жизни, скорости, размера, угла и т. д.

```cpp
crossrender::ParticleEmitterDesc d;
d.lifetime = crossrender::RangeF(0.8f, 1.6f);
d.startSize = crossrender::RangeF(0.05f);
```

### Конструкторы и поля `RangeF`: `min`, `max`

Данные и конструкторы сгруппированы: это параметрическая структура.

| Член | Смысл |
|---|---|
| `f32 min`, `f32 max` | Границы диапазона; по умолчанию обе `0`. |
| `RangeF()` | Диапазон `{0, 0}`. |
| `RangeF(f32 v)` | Вырожденный диапазон `{v, v}` — фиксированное значение. |
| `RangeF(f32 a, f32 b)` | Диапазон `{a, b}`. |

```cpp
crossrender::RangeF exact(1.0f);          // всегда 1.0
crossrender::RangeF spread(0.5f, 2.5f);   // случайное значение в [0.5, 2.5]
crossrender::RangeF manual;
manual.min = -1.0f;
manual.max = 1.0f;
ENG_LOGI("fx", "границы: %.2f..%.2f", manual.min, manual.max);
```

### `f32 RangeF::Sample(Random& rng) const`

Случайное значение, равномерно распределённое в `[min, max]`. Именно этот
вызов делает каждый экземпляр частицы немного другим.

```cpp
crossrender::Random rng(5u);
crossrender::RangeF speed(1.0f, 3.0f);
for (int i = 0; i < 3; ++i) ENG_LOGI("fx", "скорость %.2f", speed.Sample(rng));
```

### `struct RangeV3`

Трёхмерный диапазон: покомпонентно независимый разброс вектора. Применяется,
когда эмиттер должен сэмплировать вектор (например, свой ветер на частицу).

```cpp
crossrender::RangeV3 wind{{-0.2f, 0.0f, -0.2f}, {0.2f, 0.1f, 0.2f}};
crossrender::Random rng(9u);
const crossrender::Vec3 w = wind.Sample(rng);
ENG_LOGI("fx", "ветер %.2f %.2f %.2f", w.x, w.y, w.z);
```

### Конструкторы и поля `RangeV3`: `min`, `max`

Сгруппированные данные и конструкторы.

| Член | Смысл |
|---|---|
| `Vec3 min`, `Vec3 max` | Покомпонентные границы (по умолчанию нули). |
| `RangeV3()` | Оба вектора нулевые. |
| `RangeV3(const Vec3& v)` | Вырожденный диапазон: обе границы равны `v`. |
| `RangeV3(const Vec3& a, const Vec3& b)` | Границы `a` и `b`. |

```cpp
crossrender::RangeV3 fixed(crossrender::Vec3(0.0f, 1.0f, 0.0f));
crossrender::RangeV3 box(crossrender::Vec3(-1, -1, -1), crossrender::Vec3(1, 1, 1));
ENG_LOGI("fx", "min.y=%.1f max.y=%.1f", box.min.y, fixed.max.y);
```

### `Vec3 RangeV3::Sample(Random& rng) const`

Вектор, у которого каждая компонента независимо сэмплируется из своего
диапазона.

```cpp
crossrender::Random rng(11u);
crossrender::RangeV3 jitter(crossrender::Vec3(-0.5f), crossrender::Vec3(0.5f));
const crossrender::Vec3 v = jitter.Sample(rng);
ENG_LOGI("fx", "смещение %.3f %.3f %.3f", v.x, v.y, v.z);
```

### `enum class EmitterShape : u8`

Форма области, из которой рождаются частицы; она же задаёт начальное
направление движения.

| Значение | Смысл |
|---|---|
| `EmitterShape::Point` | Одна точка в начале координат, направление — `+Y`. |
| `EmitterShape::Sphere` | Равномерно по объёму шара радиуса `shapeRadius`; направление — от центра. |
| `EmitterShape::Hemisphere` | То же, но только верхняя (`+Y`) половина. |
| `EmitterShape::Box` | Внутри параллелепипеда `shapeBox` (полные размеры). |
| `EmitterShape::Cone` | Конус вокруг `+Y`; `coneAngle` — **половинный** угол, выборка равномерна по телесному углу. |
| `EmitterShape::Circle` | Круг в плоскости XZ. |
| `EmitterShape::Edge` | Отрезок по X длиной `shapeBox.x`. |
| `EmitterShape::Mesh` | Случайная точка внутри AABB меша (у `Mesh` нет CPU-данных о вершинах). |

```cpp
crossrender::ParticleEmitterDesc d;
d.shape = crossrender::EmitterShape::Cone;
d.coneAngle = 20.0f * crossrender::kDeg2Rad;      // половинный угол раствора
d.shapeRadius = {0.0f, 0.15f};            // стартовое кольцо у вершины
```

### `enum class SimulationSpace : u8`

Пространство, в котором живут частицы (см. раздел «Пространство эмиттера»).

| Значение | Смысл |
|---|---|
| `SimulationSpace::World` | Позиция и направление трансформируются в мир при рождении; эмиттер дальше можно двигать. |
| `SimulationSpace::Local` | Частицы живут в локальных координатах, трансформация применяется при отрисовке. |

```cpp
crossrender::ParticleSystem sys;
sys.Init(crossrender::ParticleSystem::PresetSmoke(), 1u);
sys.Emitter(0).space = crossrender::SimulationSpace::Local;   // дым едет вместе с объектом
sys.SetTransform(crossrender::Mat4::Translate({2.0f, 0.0f, 0.0f}));
```

### `enum class ParticleRenderMode : u8`

Как инстанс превращается в геометрию на экране.

| Значение | Смысл |
|---|---|
| `ParticleRenderMode::Billboard` | Квад, всегда развёрнутый к камере. |
| `ParticleRenderMode::StretchedBillboard` | Квад, растянутый вдоль скорости (множитель — `stretchScale`). |
| `ParticleRenderMode::HorizontalBillboard` | Квад, лежащий в горизонтальной плоскости. |
| `ParticleRenderMode::VerticalBillboard` | Квад, вытянутый по вертикали. |
| `ParticleRenderMode::Mesh` | Рисуется меш эмиттера (`mesh`), а не квад. |
| `ParticleRenderMode::Trail` | Цепочка растянутых квадов по 8 последним позициям частицы. |

```cpp
crossrender::ParticleEmitterDesc sparks;
sparks.renderMode = crossrender::ParticleRenderMode::StretchedBillboard;
sparks.stretchScale = 0.06f;      // насколько сильно тянуть по скорости

crossrender::ParticleEmitterDesc trail;
trail.renderMode = crossrender::ParticleRenderMode::Trail;   // отдельного ленточного шейдера нет
```

Честное ограничение режима `Mesh`: встроенный вершинный шейдер частиц читает
UV с атрибута-локации 1, а VAO меша движка связывает с этой локацией нормаль
(см. `Mesh.cpp`). Юнит-квад самой системы это учитывает, а произвольный меш —
нет, поэтому у `Mesh`-частиц развёртка текстуры может отличаться от ожидаемой;
исправлять это без изменения замороженных `Mesh`/`Shader` нельзя.

### `enum class ParticleBlendMode : u8`

Режим смешивания. Встроенный фрагментный шейдер частиц уже домножает `rgb` на
альфу, поэтому факторы GL заданы в premultiplied-виде.

| Значение | Смысл |
|---|---|
| `ParticleBlendMode::Alpha` | Обычная прозрачность (`ONE`, `ONE_MINUS_SRC_ALPHA`). |
| `ParticleBlendMode::Additive` | Свечение (`ONE`, `ONE`) — дым, искры, магия. |
| `ParticleBlendMode::Multiply` | Затемнение (`DST_COLOR`, `ZERO`). |
| `ParticleBlendMode::Premultiplied` | То же, что `Alpha`, для уже домноженных данных. |
| `ParticleBlendMode::Screen` | Экранное осветление (`ONE`, `ONE_MINUS_SRC_COLOR`). |

```cpp
crossrender::ParticleEmitterDesc smoke;
smoke.blendMode = crossrender::ParticleBlendMode::Alpha;      // плотный дым
crossrender::ParticleEmitterDesc fire;
fire.blendMode = crossrender::ParticleBlendMode::Additive;    // светящееся пламя
```

### `struct ParticleEmitterDesc`

Полное описание эмиттера: эмиссия, время жизни, форма, силы, материал и
суб-эмиттеры. Ниже поля сгруппированы по ролям (каждая группа — таблица и
один общий пример); методы и фабрики получили отдельные подразделы.

```cpp
crossrender::ParticleEmitterDesc d;                 // все значения по умолчанию разумны
d.name = "Fountain";
d.rate = 420.0f;
d.maxParticles = 2200;
d.shape = crossrender::EmitterShape::Cone;
d.lifetime = {1.5f, 2.3f};
crossrender::ParticleSystem sys;
sys.Init({d}, 42u);
```

### Поля эмиссии: `name`, `rate`, `burst`, `duration`, `looping`, `maxParticles`

| Поле | По умолчанию | Смысл |
|---|---|---|
| `std::string name` | `"Particles"` | Имя эмиттера; по нему суб-эмиттеры находят цель. |
| `f32 rate` | `100.0f` | Частиц в секунду. |
| `int burst` | `0` | Залп при старте (и при каждом новом цикле зацикленного эмиттера). |
| `f32 duration` | `0.0f` | Длительность эмиссии в секундах; `0` — бесконечно. |
| `bool looping` | `true` | Повторять ли эмиссию после `duration`. |
| `int maxParticles` | `2000` | Размер пула эмиттера (до глобального ужатия бюджета). |

```cpp
crossrender::ParticleEmitterDesc burst;
burst.name = "Explosion";
burst.rate = 0.0f;          // без постоянной эмиссии
burst.burst = 300;          // только залп
burst.duration = 0.12f;     // эмиссия живёт 0.12 с
burst.looping = false;      // один раз
burst.maxParticles = 600;
```

### Поля времени жизни и начального состояния

| Поле | По умолчанию | Смысл |
|---|---|---|
| `RangeF lifetime` | `{1.0f, 2.0f}` | Время жизни частицы в секундах. |
| `RangeF startSpeed` | `{1.0f, 2.0f}` | Начальная скорость вдоль направления формы. |
| `RangeF startSize` | `{0.05f, 0.15f}` | Начальный размер (мировые единицы). |
| `RangeF startRotation` | `{0.0f, kTau}` | Начальный поворот в радианах. |
| `RangeF startAngularVelocity` | `{-2.0f, 2.0f}` | Угловая скорость в радианах в секунду. |

```cpp
crossrender::ParticleEmitterDesc d;
d.lifetime = {2.5f, 4.5f};                 // долгий дым
d.startSpeed = {0.25f, 0.7f};
d.startSize = {0.5f, 1.1f};
d.startRotation = {0.0f, crossrender::kTau};        // полный случайный оборот
d.startAngularVelocity = {-0.6f, 0.6f};
```

### Кривые и цвет: `startColor`, `sizeOverLifetime`, `alphaOverLifetime`, `speedOverLifetime`, `rotationOverLifetime`, `colorOverLifetime`

Все кривые сэмплируются по нормированному возрасту `t` (см. `Curve`).
`startColor` и `colorOverLifetime` перемножаются, `alphaOverLifetime`
дополнительно домножает альфу.

| Поле | По умолчанию | Смысл |
|---|---|---|
| `ColorCurve startColor` | `Constant(White)` | Цвет при рождении. |
| `Curve sizeOverLifetime` | `Constant(1.0f)` | Множитель размера. |
| `Curve alphaOverLifetime` | `FromPoints({{0,1},{1,0}})` | Множитель альфы (по умолчанию — затухание). |
| `Curve speedOverLifetime` | `Constant(1.0f)` | Множитель **интегрируемого** движения (не сил). |
| `Curve rotationOverLifetime` | `Constant(0.0f)` | Добавка к повороту в радианах. |
| `ColorCurve colorOverLifetime` | `Constant(White)` | Цветовой градиент по жизни. |

```cpp
crossrender::ParticleEmitterDesc d;
d.startColor = crossrender::ColorCurve::Constant(crossrender::Color::White);
d.sizeOverLifetime = crossrender::Curve::FromPoints({{0.0f, 0.4f}, {0.3f, 1.0f}, {1.0f, 1.9f}}, 0.5f);
d.alphaOverLifetime = crossrender::Curve::FromPoints({{0.0f, 0.0f}, {0.15f, 0.5f}, {1.0f, 0.0f}}, 0.5f);
d.speedOverLifetime = crossrender::Curve::Constant(1.0f);   // строгий no-op
d.rotationOverLifetime = crossrender::Curve::FromPoints({{0.0f, 0.0f}, {1.0f, 3.14f}});
d.colorOverLifetime = crossrender::ColorCurve::Gradient({{0.0f, crossrender::Color::FromRGB(0x9AA0A6)},
                                                 {1.0f, crossrender::Color::FromRGB(0x2B2E33)}});
```

### Поля формы: `shape`, `shapeRadius`, `shapeBox`, `coneAngle`

| Поле | По умолчанию | Смысл |
|---|---|---|
| `EmitterShape shape` | `Sphere` | Форма области рождения. |
| `RangeF shapeRadius` | `{0.1f, 0.5f}` | Радиус для `Sphere`/`Hemisphere`/`Cone`/`Circle`. |
| `Vec3 shapeBox` | `{1, 1, 1}` | **Полные** размеры для `Box` и длина для `Edge`. |
| `f32 coneAngle` | `25°` | Половинный угол конуса (в радианах). |

```cpp
crossrender::ParticleEmitterDesc d;
d.shape = crossrender::EmitterShape::Box;
d.shapeBox = {9.0f, 4.0f, 9.0f};        // снегопад над площадкой
d.shapeRadius = {0.0f, 0.0f};           // для Box не используется

d.shape = crossrender::EmitterShape::Cone;
d.coneAngle = 14.0f * crossrender::kDeg2Rad;    // узкий факел
```

### Поля сил: `gravity`, `wind`, `drag`, `turbulenceStrength`, `turbulenceFrequency`

Силы складываются и применяются каждый подшаг. `drag` — экспоненциальное
торможение (`exp(-drag * h)`), турбулентность — детерминированный curl-шум,
зависящий от позиции, времени и индекса эмиттера.

| Поле | По умолчанию | Смысл |
|---|---|---|
| `Vec3 gravity` | `{0, -9.81, 0}` | Постоянное ускорение. |
| `Vec3 wind` | `{0, 0, 0}` | Постоянное ускорение-ветер. |
| `f32 drag` | `0.0f` | Сопротивление среды (1/с). |
| `f32 turbulenceStrength` | `0.0f` | Амплитуда вихревого шума. |
| `f32 turbulenceFrequency` | `1.0f` | Пространственная частота шума. |

```cpp
crossrender::ParticleEmitterDesc d;
d.gravity = {0.0f, 0.25f, 0.0f};     // дым слегка всплывает
d.wind = {0.55f, 0.05f, 0.15f};
d.drag = 0.55f;
d.turbulenceStrength = 0.1f;
d.turbulenceFrequency = 0.7f;
```

### `struct ParticleEmitterDesc::Attractor` и поле `attractors`

Точка притяжения, которая пересчитывается каждый подшаг. Внутри радиуса
частица получает ускорение к центру с линейным затуханием по расстоянию.

| Поле | По умолчанию | Смысл |
|---|---|---|
| `Vec3 position` | `{0, 0, 0}` | Центр притяжения в пространстве симуляции. |
| `f32 strength` | `0` | Сила; знак задаёт притяжение/отталкивание. |
| `f32 radius` | `5.0f` | Радиус действия. |

```cpp
crossrender::ParticleEmitterDesc d;
d.attractors.push_back({{0.0f, 1.1f, 0.0f}, 3.0f, 3.5f});
d.attractors.push_back({{2.0f, 0.5f, 0.0f}, -1.5f, 2.0f});   // отталкивание
```

### Поля вихря: `vortexAxis`, `vortexStrength`

Вихрь поворачивает вектор скорости частицы вокруг оси на угол
`vortexStrength * h` каждый подшаг — получается закрученная спираль.

| Поле | По умолчанию | Смысл |
|---|---|---|
| `Vec3 vortexAxis` | `{0, 1, 0}` | Ось вращения. |
| `f32 vortexStrength` | `0.0f` | Угловая скорость (рад/с). |

```cpp
crossrender::ParticleEmitterDesc d;
d.vortexAxis = {0.0f, 1.0f, 0.0f};
d.vortexStrength = 3.2f;    // закрутка вокруг вертикали
```

### Поля столкновения с землёй: `collideGround`, `groundY`, `bounce`, `friction`

Плоский «пол» на высоте `groundY`. При падении частица отражается с
коэффициентом `bounce`, а горизонтальная скорость умножается на `1 - friction`.
Столкновение может запускать суб-эмиттер с `trigger == 2`.

| Поле | По умолчанию | Смысл |
|---|---|---|
| `bool collideGround` | `false` | Включить столкновение. |
| `f32 groundY` | `0.0f` | Высота пола. |
| `f32 bounce` | `0.4f` | Упругость отскока `[0, 1]`. |
| `f32 friction` | `0.7f` | Потеря горизонтальной скорости `[0, 1]`. |

```cpp
crossrender::ParticleEmitterDesc d;
d.collideGround = true;
d.groundY = 0.0f;
d.bounce = 0.45f;
d.friction = 0.6f;
```

### Поле разброса размера: `sizeVariance`

Дополнительный множитель к сэмплированному `startSize` (по умолчанию `{1, 1}`
— разброса нет). Итоговый размер — `max(0, startSize * sizeVariance)`.

```cpp
crossrender::ParticleEmitterDesc d;
d.startSize = {0.2f, 0.4f};
d.sizeVariance = {0.5f, 1.5f};    // часть частиц заметно крупнее
```

### Поля отрисовки: `renderMode`, `blendMode`, `texture`, `mesh`

| Поле | По умолчанию | Смысл |
|---|---|---|
| `ParticleRenderMode renderMode` | `Billboard` | Способ построения инстанса. |
| `ParticleBlendMode blendMode` | `Additive` | Режим смешивания. |
| `const Texture* texture` | `nullptr` | Текстура частицы; `nullptr` — процедурная круглая маска. |
| `const Mesh* mesh` | `nullptr` | Меш для `Mesh`-режима; `nullptr` — юнит-квад. |

Текстура и меш **не копируются**: они должны жить всё время работы системы.

```cpp
crossrender::ParticleEmitterDesc d;
d.renderMode = crossrender::ParticleRenderMode::Billboard;
d.blendMode = crossrender::ParticleBlendMode::Alpha;
d.texture = &smokeTexture;      // текстура обязана пережить ParticleSystem
d.mesh = nullptr;               // квад по умолчанию
```

### Поля мягких частиц: `softParticles`, `softFadeDistance`

| Поле | По умолчанию | Смысл |
|---|---|---|
| `bool softParticles` | `false` | Включить аналитическое затухание у камеры/пола. |
| `f32 softFadeDistance` | `0.5f` | Расстояние, на котором альфа падает до нуля. |

Мягкость считается **на CPU**: `Renderer3D` не отдаёт буфер глубины
(`SceneColor()` — это HDR-цвет), поэтому пересечение с геометрией
приближается расстоянием до камеры, а при `collideGround` — ещё и близостью к
`groundY`. Униформа шейдера `uSoftFade` остаётся равной 1.

```cpp
crossrender::ParticleEmitterDesc d;
d.softParticles = true;
d.softFadeDistance = 0.35f;   // короче — мягче
```

### Флаги материала: `lit`, `castShadows`, `alignToVelocity`, `stretchScale`, `sortByDepth`, `receiveFog`

| Поле | По умолчанию | Смысл |
|---|---|---|
| `bool lit` | `false` | Материал считается освещённым; включает запись глубины. |
| `bool castShadows` | `false` | Частицы отбрасывают тени; включает запись глубины. |
| `bool alignToVelocity` | `false` | Выравнивание по скорости; реально работает только в `StretchedBillboard`/`Trail`. |
| `f32 stretchScale` | `0.1f` | Множитель растяжения. |
| `bool sortByDepth` | `true` | Сортировать инстансы от дальних к ближним. |
| `bool receiveFog` | `true` | Участвовать в тумане сцены. |

Встроенный шейдер частиц неосвещённый и без теней: `lit`/`castShadows`
влияют на запись глубины, а не на затенение; `alignToVelocity` — свойство
самих режимов растянутого билборда и следа.

```cpp
crossrender::ParticleEmitterDesc d;
d.lit = true;                // разрешить запись в буфер глубины
d.castShadows = false;
d.alignToVelocity = true;    // для StretchedBillboard/Trail
d.stretchScale = 0.06f;
d.sortByDepth = true;
d.receiveFog = true;
```

### Поле пространства: `space`

Пространство симуляции эмиттера — `World` или `Local` (см. раздел «Пространство
эмиттера» выше).

```cpp
crossrender::ParticleEmitterDesc d;
d.space = crossrender::SimulationSpace::Local;
crossrender::ParticleSystem sys;
sys.Init({d}, 3u);
sys.SetTransform(crossrender::Mat4::Translate({0.0f, 4.0f, 0.0f}));
```

### `struct ParticleEmitterDesc::SubEmitter` и поле `subEmitters`

Суб-эмиттер запускает **другой** эмиттер той же системы по событию. Цель
ищется по имени среди эмиттеров, переданных в `Init`; глубина каскада
ограничена тремя уровнями, чтобы цепочка не ушла в бесконечность.

| Поле | По умолчанию | Смысл |
|---|---|---|
| `std::string emitterName` | пусто | Имя целевого эмиттера. |
| `int trigger` | `0` | `0` — рождение, `1` — смерть, `2` — столкновение с землёй. |
| `int count` | `4` | Сколько частиц цели породить на событие. |

```cpp
// Взрыв: каждый умирающий «файербол» выпускает шесть клубов дыма.
crossrender::ParticleEmitterDesc explosion;
explosion.name = "Explosion";
explosion.burst = 300;
explosion.subEmitters.push_back({"ExplosionSmoke", 1, 6});

crossrender::ParticleEmitterDesc smoke;
smoke.name = "ExplosionSmoke";
smoke.rate = 0.0f;              // кормится только суб-эмиттером

crossrender::ParticleSystem sys;
sys.Init({explosion, smoke}, 10u);
```

### `struct Particle`

Состояние одной частицы. Поля публичны, потому что массив пула возвращается
наружу через `Particles` (для тестов и игровой логики), но менять их напрямую
не следует — симуляция перезапишет derived-поля на следующем шаге.

```cpp
crossrender::ParticleSystem sys;
sys.Init(crossrender::ParticleSystem::PresetSparks(), 1u);
sys.Simulate(0.2f);
const crossrender::Particle* pool = sys.Particles(0);
if (pool != nullptr && pool[0].alive) {
    ENG_LOGI("fx", "возраст %.2f из %.2f", pool[0].age, pool[0].lifetime);
}
```

### Поля `Particle`: `position`, `velocity`, `spawnPosition`, `color`, `size`, `rotation`, `angularVelocity`, `age`, `lifetime`, `seed`, `alive`

Поля сгруппированы: это «сырое» состояние пула, а не набор методов.

| Поле | По умолчанию | Смысл |
|---|---|---|
| `Vec3 position` | — | Текущая позиция в пространстве симуляции. |
| `Vec3 velocity` | — | Текущая скорость. |
| `Vec3 spawnPosition` | — | Позиция рождения (пригодится для линий/vortex-запросов). |
| `Color color` | — | Итоговый цвет с уже применённой альфой кривых. |
| `f32 size` | `1` | Текущий размер. |
| `f32 rotation` | `0` | Текущий поворот в радианах. |
| `f32 angularVelocity` | `0` | Угловая скорость. |
| `f32 age` | `0` | Прожитое время в секундах. |
| `f32 lifetime` | `1` | Полное время жизни; не меньше `1e-3`. |
| `f32 seed` | `0` | Случайное число `[0, 1)` для шейдерных/логических вариаций. |
| `bool alive` | `true` | Занят ли слот. |

```cpp
crossrender::Particle p;
p.position = {0.0f, 1.0f, 0.0f};
p.velocity = {0.0f, 2.0f, 0.0f};
p.lifetime = 1.5f;
p.size = 0.1f;
p.angularVelocity = 1.0f;
p.seed = 0.5f;
ENG_LOGI("fx", "t=%.2f", p.NormalizedAge());
```

### `f32 Particle::NormalizedAge() const`

Нормированный возраст в `[0, 1]` — то самое `t`, которым индексируются все
кривые. Если `lifetime <= 0`, возвращает `1` (частица считается мёртвой).

```cpp
crossrender::Particle p;
p.age = 0.75f;
p.lifetime = 3.0f;
const crossrender::f32 t = p.NormalizedAge();    // 0.25
ENG_LOGI("fx", "пройдено %.0f%% жизни", t * 100.0f);
```

### `ParticleSystem::ParticleSystem()`

Создаёт пустую систему: внутренние структуры готовы, но пулов, эмиттеров и
GPU-ресурсов ещё нет. Копирование запрещено (у системы есть владение пулами и
GL-объектами).

```cpp
crossrender::ParticleSystem sys;      // ещё ничего не выделено
ENG_LOGI("fx", "эмиттеров %d, слотов %d", sys.EmitterCount(), sys.Capacity());
```

### `ParticleSystem::~ParticleSystem()`

Вызывает `Shutdown()` и освобождает пулы, описания и GPU-ресурсы. Система
должна умирать раньше, чем текстуры и меши, на которые ссылаются эмиттеры.

```cpp
{
    crossrender::ParticleSystem local;
    local.Init(crossrender::ParticleSystem::PresetFire(), 1u);
}   // здесь пулы и VBO освобождаются автоматически
```

### `void ParticleSystem::Init(const std::vector<ParticleEmitterDesc>& emitters, u64 seed = 12345)`

Готовит систему к работе: копирует описания, сеет RNG, сбрасывает трансформацию
в единичную, ставит `IsPlaying() == true` и **сразу** аллоцирует CPU-пулы
(с ужатием под общий бюджет). GPU-ресурсы не создаются — это происходит лениво
на первом `Render`. Повторный `Init` полностью пересоздаёт систему, поэтому
старые индексы эмиттеров становятся недействительными.

* **Возвращает:** ничего.
* **Контекст:** безопасен без контекста OpenGL (headless, тесты).
* **Seed:** одно и то же `seed` даёт одинаковую последовательность случайных
  чисел, то есть воспроизводимую картину.

```cpp
crossrender::ParticleSystem sys;
sys.Init(crossrender::ParticleSystem::PresetFire(), 12345u);

// Свой набор эмиттеров: главный плюс дым по смерти.
std::vector<crossrender::ParticleEmitterDesc> set = crossrender::ParticleSystem::PresetExplosion();
sys.Init(set, 777u);
ENG_LOGI("fx", "готово: %d эмиттеров, %d слотов", sys.EmitterCount(), sys.Capacity());
```

### `void ParticleSystem::Shutdown()`

Освобождает пулы, описания, кэши имён и GPU-ресурсы, сбрасывает трансформацию
и границы, ставит `IsPlaying() == false`. После `Shutdown` все публичные методы
безопасны и ничего не делают (а `AliveCount`, `Capacity`, `EmitterCount`
возвращают 0, `Particles` — `nullptr`); чтобы работать снова, нужен новый
`Init`. Деструктор вызывает `Shutdown` сам.

```cpp
crossrender::ParticleSystem sys;
sys.Init(crossrender::ParticleSystem::PresetSnow(), 2u);
sys.Simulate(0.5f);
ENG_LOGI("fx", "живых %d", sys.AliveCount());

sys.Shutdown();
sys.Simulate(0.5f);              // безопасно: ничего не происходит
ENG_LOGI("fx", "после Shutdown: слотов %d", sys.Capacity());
```

### `void ParticleSystem::SetTransform(const Mat4& t)`

Задаёт трансформацию эмиттеров. Для `World` она применяется к новым частицам в
момент рождения; для `Local` — ко всем частицам при сборке инстансов. `Init`
сбрасывает трансформацию в единичную.

```cpp
crossrender::ParticleSystem sys;
sys.Init(crossrender::ParticleSystem::PresetTrail(), 4u);
sys.SetTransform(crossrender::Mat4::TRS({0.0f, 1.0f, 0.0f},
                                {0.0f, crossrender::kPi * 0.5f, 0.0f},
                                {1.0f, 1.0f, 1.0f}));
sys.Simulate(1.0f / 60.0f);
```

### `const Mat4& ParticleSystem::Transform() const`

Возвращает текущую трансформацию — например, чтобы дорисовать отладочный
каркас вокруг эмиттера.

```cpp
const crossrender::Mat4& xf = sys.Transform();
r3d.DrawWireBox(crossrender::Bounds{{-1, -1, -1}, {1, 1, 1}}, crossrender::Color::Yellow, xf);
```

### `void ParticleSystem::SetPlaying(bool playing)`

Ставит/снимает флаг игры. `false` останавливает **эмиссию** (новые частицы не
рождаются), но уже живые продолжают интегрироваться и умирать; `true`
возобновляет эмиссию. Отдельного «пауза всего» нет — для полной остановки
используйте `Clear()`.

```cpp
sys.SetPlaying(false);     // перестали сыпать снег, старые хлопья доедут
sys.Simulate(dt);
sys.SetPlaying(true);      // снова пошёл снег
```

### `bool ParticleSystem::IsPlaying() const`

Текущее состояние игры. Дополнительно симуляция **сама** сбрасывает флаг в
`false`, когда все эмиттеры перестали эмитировать и в пулах не осталось живых
частиц (типично для не-зацикленного залпа).

```cpp
if (!sys.IsPlaying()) {
    ENG_LOGI("fx", "взрыв отработал, можно вернуть систему в пул");
}
```

### `void ParticleSystem::Restart()`

Возвращает систему в исходное состояние: убивает всех частиц, пересоздаёт
списки свободных слотов, обнуляет аккумулятор и время эмиттера, заново ставит
`burstPending` для залповых эмиттеров, пересевает RNG **тем же** seed и снова
включает игру. Описания и трансформация сохраняются. `Init` не требуется.

```cpp
if (input.Pressed(crossrender::Key::R)) {
    sys.Restart();             // тот же seed — та же картина заново
}
```

### `void ParticleSystem::Simulate(f32 dt)`

Главный шаг симуляции. `dt` зажимается сверху четырьмя секундами (с
предупреждением), неположительный или не-`finite` `dt` игнорируется. Шаг режется
на подшаги не длиннее `1/60` с (не больше 480 подшагов), считаются эмиссия,
силы, столкновения, кривые и границы. Перед шагом обновляется кэш кривых и имён
эмиттеров, поэтому правки `Emitter(i)` вступают в силу здесь.

```cpp
// Фиксированный шаг удобен для воспроизводимости.
const crossrender::f32 fixed = 1.0f / 60.0f;
sys.Simulate(fixed);

// Большой dt не «телепортирует» частицы: он режется на подшаги.
sys.Simulate(1.0f);            // ~60 подшагов по 1/60
```

### `void ParticleSystem::Render(Renderer3D& renderer, const Camera& camera)`

Собирает инстансы каждого эмиттера (с учётом `sortByDepth`, мягких частиц и
следов) и откладывает их отрисовку в конец кадра через
`Renderer3D::AddPostDraw`. Камера берётся у рендерера (`renderer.GetCamera()`
— источник истины для базиса билбордов); переданная `camera` используется
только если камера рендерера вырождена. Если GL-контекста нет или встроенный
шейдер не собрался, вызов молча ничего не рисует.

Ничего не симулирует: если не позвать `Simulate`, на экране останется прошлое
состояние.

```cpp
// Вызывайте после Simulate; сам вызов лишь ставит колбэк в очередь кадра.
sys.Simulate(dt);
sys.Render(r3d, r3d.GetCamera());
```

### `void ParticleSystem::Update(Renderer3D& renderer, const Camera& camera, f32 dt)`

Удобная обёртка «шаг + отрисовка» для сцен, которым не нужен отдельный
`Simulate`. Порядок внутри — `Simulate(dt)`, затем `Render(renderer, camera)`.

```cpp
void MyScene::Render(crossrender::Renderer3D& r3d, crossrender::f32 dt) {
    sys.Update(r3d, r3d.GetCamera(), dt);   // то же, что Simulate + Render
}
```

### `int ParticleSystem::AliveCount() const`

Сколько частиц сейчас живо во всех эмиттерах. Дёшево: счётчики обновляются в
процессе симуляции. После `Shutdown` — 0.

```cpp
sys.Simulate(dt);
ENG_LOGI("fx", "живых частиц: %d из %d", sys.AliveCount(), sys.Capacity());
```

### `int ParticleSystem::Capacity() const`

Суммарное число слотов во всех пулах **после** применения бюджета. Это оценка
сверху для `AliveCount` и удобная метрика для профилировщика.

```cpp
sys.Init(crossrender::ParticleSystem::PresetSnow(), 1u);
// Snow просит 3500 слотов — бюджет это позволяет.
ENG_LOGI("fx", "выделено слотов: %d", sys.Capacity());
```

### `int ParticleSystem::EmitterCount() const`

Число эмиттеров в системе (равно размеру вектора, переданного в `Init`). После
`Shutdown` — 0.

```cpp
sys.Init(crossrender::ParticleSystem::PresetExplosion(), 1u);   // их два: взрыв и дым
for (int i = 0; i < sys.EmitterCount(); ++i)
    ENG_LOGI("fx", "эмиттер %d: %s", i, sys.Emitter(i).name.c_str());
```

### `ParticleEmitterDesc& ParticleSystem::Emitter(int i)` и константная перегрузка

Доступ к описанию эмиттера по индексу. Константный вариант возвращает
`const ParticleEmitterDesc&`. **Границы не проверяются** — индекс обязан быть в
`[0, EmitterCount())`, иначе поведение неопределено. Правки `rate`, кривых и
имени подхватываются на следующем `Simulate`; изменить `maxParticles` после
`Init` нельзя — пул уже выделен.

```cpp
crossrender::ParticleEmitterDesc& desc = sys.Emitter(0);
desc.rate = 60.0f;
desc.name = "Ash";
desc.gravity = {0.0f, -1.2f, 0.0f};

const crossrender::ParticleSystem& ro = sys;
const crossrender::ParticleEmitterDesc& readOnly = ro.Emitter(0);
ENG_LOGI("fx", "%s: %.0f частиц/с", readOnly.name.c_str(), readOnly.rate);
```

### `const Particle* ParticleSystem::Particles(int emitterIndex) const`

Указатель на начало пула эмиттера (все слоты, включая мёртвые — проверяйте
`Particle::alive`). Индекс вне диапазона даёт `nullptr`; после `Shutdown` тоже
`nullptr`. Указатель действителен до следующего `Init`/`Restart`/`Shutdown`.

```cpp
const crossrender::Particle* pool = sys.Particles(0);
if (pool == nullptr) return;
const int count = sys.Emitter(0).maxParticles;
int alive = 0;
for (int i = 0; i < count; ++i) alive += pool[i].alive ? 1 : 0;
ENG_LOGI("fx", "в пуле 0 живых: %d", alive);
```

### `void ParticleSystem::EmitBurst(int emitterIndex, int count)`

Немедленно рождает `count` частиц у указанного эмиттера, не дожидаясь `rate`.
Публичный залп **не** каскадирует в суб-эмиттеры рождения, поэтому число
рождённых частиц ровно такое, какое помещается в пул. После вызова
`IsPlaying()` снова `true`. Некорректный индекс или `count <= 0` игнорируются.

```cpp
// Выстрел по кнопке: ровно 60 искр из первого эмиттера.
if (input.Pressed(crossrender::Key::Space)) sys.EmitBurst(0, 60);
sys.Simulate(dt);
```

### `void ParticleSystem::Clear()`

Убивает все частицы во всех эмиттерах, восстанавливает списки свободных слотов,
обнуляет аккумуляторы эмиссии и границы. Флаг игры и описания не трогает: если
система играла, эмиссия продолжится с этого же состояния RNG.

```cpp
sys.Clear();
ENG_LOGI("fx", "после Clear: живых %d, слотов %d", sys.AliveCount(), sys.Capacity());
```

### `const Bounds& ParticleSystem::Bounds() const`

AABB всех живых частиц в пространстве симуляции, обновляемый после каждого
`Simulate` (и `EmitBurst`). Пустая система даёт невалидные границы
(`Bounds::Valid() == false`). Удобно для грубого отсечения и для авто-подгонки
камеры.

```cpp
sys.Simulate(dt);
const crossrender::Bounds& b = sys.Bounds();
if (b.Valid()) ENG_LOGI("fx", "центр %.2f %.2f %.2f", b.Center().x, b.Center().y, b.Center().z);
```

### `static std::vector<ParticleEmitterDesc> ParticleSystem::PresetFire()`

Пресет костра: конус `14°`, подъёмная сила (гравитация вверх), турбулентность,
аддитивное смешивание, огненный градиент и мягкие частицы. Один эмиттер
`"Fire"`, 900 слотов, залп 30.

```cpp
crossrender::ParticleSystem fire;
fire.Init(crossrender::ParticleSystem::PresetFire(), 1u);
fire.SetTransform(crossrender::Mat4::Translate({0.0f, 0.1f, 0.0f}));
```

### `static std::vector<ParticleEmitterDesc> ParticleSystem::PresetSmoke()`

Пресет дыма: шар, слабый подъём, сильный ветер и сопротивление, растущий размер,
alpha-смешивание. Один эмиттер `"Smoke"`, 700 слотов.

```cpp
crossrender::ParticleSystem smoke;
smoke.Init(crossrender::ParticleSystem::PresetSmoke(), 2u);
smoke.Emitter(0).wind = {0.9f, 0.05f, 0.2f};    // подправили ветер
```

### `static std::vector<ParticleEmitterDesc> ParticleSystem::PresetSparks()`

Пресет искр: круг, высокая скорость, сильная гравитация, столкновение с полом
и растянутый билборд. Один эмиттер `"Sparks"`, 1500 слотов.

```cpp
crossrender::ParticleSystem sparks;
sparks.Init(crossrender::ParticleSystem::PresetSparks(), 3u);
sparks.EmitBurst(0, 80);     // сноп искр от удара
sparks.Simulate(dt);
```

### `static std::vector<ParticleEmitterDesc> ParticleSystem::PresetMagic()`

Пресет магии: сфера, аттрактор и вихрь, плавные кривые размера и
многоцветный градиент. Один эмиттер `"Magic"`, 1200 слотов.

```cpp
crossrender::ParticleSystem magic;
magic.Init(crossrender::ParticleSystem::PresetMagic(), 4u);
// Аттрактор можно перенести в точку интереса.
magic.Emitter(0).attractors[0].position = {0.0f, 1.6f, 0.0f};
```

### `static std::vector<ParticleEmitterDesc> ParticleSystem::PresetFountain()`

Пресет фонтана: узкий конус, высокая скорость, гравитация вниз, отскок от пола
и голубой градиент. Один эмиттер `"Fountain"`, 2200 слотов.

```cpp
crossrender::ParticleSystem fountain;
fountain.Init(crossrender::ParticleSystem::PresetFountain(), 5u);
fountain.SetTransform(crossrender::Mat4::Translate({0.0f, 1.0f, 0.0f}));
```

### `static std::vector<ParticleEmitterDesc> ParticleSystem::PresetExplosion()`

Пресет взрыва: **два** эмиттера. `"Explosion"` — залп 300 частиц без
постоянной эмиссии, не-зацикленный, с суб-эмиттером смерти; `"ExplosionSmoke"`
— дым, который кормится только этим суб-эмиттером.

```cpp
crossrender::ParticleSystem blast;
blast.Init(crossrender::ParticleSystem::PresetExplosion(), 6u);
blast.Restart();                  // «выстрелить» взрывом заново
ENG_LOGI("fx", "эмиттеров: %d", blast.EmitterCount());   // 2
```

### `static std::vector<ParticleEmitterDesc> ParticleSystem::PresetSnow()`

Пресет снегопада: большой `Box` (`9 x 4 x 9`), медленное падение, ветер,
турбулентность и длинное время жизни. Один эмиттер `"Snow"`, 3500 слотов —
самый «дорогой» из пресетов.

```cpp
crossrender::ParticleSystem snow;
snow.Init(crossrender::ParticleSystem::PresetSnow(), 7u);
snow.SetTransform(crossrender::Mat4::Translate({0.0f, 6.0f, 0.0f}));
snow.IsPlaying();                 // true
```

### `static std::vector<ParticleEmitterDesc> ParticleSystem::PresetTrail()`

Пресет следа: точечный эмиттер и `renderMode == Trail` — цепочка из восьми
растянутых квадов на частицу, аддитивное смешивание. Один эмиттер `"Trail"`,
300 слотов.

```cpp
crossrender::ParticleSystem trail;
trail.Init(crossrender::ParticleSystem::PresetTrail(), 8u);
trail.Emitter(0).space = crossrender::SimulationSpace::Local;
trail.SetTransform(objectTransform);    // след едет за объектом
```

## Пример целиком

```cpp
#include "crossrender/fx/Particles.h"

#include "crossrender/core/Log.h"
#include "crossrender/gfx/Renderer3D.h"

#include <vector>

// Небольшая сцена: костёр на пьедестале, искры по нажатию и дым по смерти
// искр. Систему двигает и обновляет сама сцена — движок этого не делает.
class CampfireScene {
public:
    void OnEnter(crossrender::Renderer3D& r3d) {
        (void)r3d;

        // Собираем набор эмиттеров вручную: пламя + дым от умирающих искр.
        std::vector<crossrender::ParticleEmitterDesc> emitters = crossrender::ParticleSystem::PresetFire();

        crossrender::ParticleEmitterDesc smoke = crossrender::ParticleSystem::PresetSmoke().front();
        smoke.name = "Smoke";                 // имя нужно суб-эмиттеру
        smoke.rate = 0.0f;                    // только по смерти искр
        emitters.push_back(smoke);

        // Пламя теперь дымит: каждая погасшая частица рождает 2 клуба дыма.
        emitters[0].subEmitters.push_back({"Smoke", 1, 2});

        fire_.Init(emitters, 2024u);
        fire_.SetTransform(crossrender::Mat4::Translate({0.0f, 0.4f, 0.0f}));
    }

    void Update(crossrender::f32 dt, bool burstPressed) {
        if (burstPressed) {
            // Публичный залп не каскадирует в суб-эмиттеры рождения.
            fire_.EmitBurst(0, 40);
        }
        fire_.Simulate(dt);

        if (!fire_.IsPlaying()) {
            // Не-зацикленные эмиттеры гаснут сами; перезапускаем вручную.
            fire_.Restart();
        }

        const crossrender::Bounds& b = fire_.Bounds();
        if (b.Valid()) {
            ENG_LOGD("fx", "границы: %d живых, радиус %.2f", fire_.AliveCount(), b.Radius());
        }
    }

    void Render(crossrender::Renderer3D& r3d) {
        // Рисуется в конце кадра через AddPostDraw, поверх обычной геометрии.
        fire_.Render(r3d, r3d.GetCamera());
    }

    void OnExit() { fire_.Shutdown(); }

private:
    crossrender::ParticleSystem fire_;
};
```

## См. также

* `docs/gfx/Renderer3D.md` — рендерер, его камера, освещение, туман и
  `AddPostDraw`, через который частицы попадают на экран.
* `docs/gfx/Mesh.md` — `Mesh`, `MeshData`, `Camera` и `Bounds`, используемые
  эмиттерами и формами.
* `docs/core/Math.md` — `Vec3`, `Color`, `Mat4`, `Random`, `Bounds` и `Lerp`,
  на которых построены кривые и симуляция.
* `docs/core/Log.md` — предупреждения `ENG_LOGW("fx", ...)` о бюджете и
  ленивом создании GPU-ресурсов.
* `docs/scene/Scene.md` — куда встраивать `Simulate`/`Render` в жизненный цикл
  сцены.
