# crossrender/gfx/Renderer3D.h — 3D-рендерер с тенями, небом и отладочной отрисовкой

Прямой (forward) PBR-lite рендерер: рисует 3D-сцену с каскадными тенями,
градиентным небом, отладочной геометрией и CPU-пикингом, а также служит точкой
входа для смешивания 3D с 2D-интерфейсом.

## Заголовок

```cpp
#include "crossrender/gfx/Renderer3D.h"
```

## Обзор

`Renderer3D` — рендерер прямого вывода: непрозрачная геометрия затеняется сразу,
без G-буфера и отложенного освещения. Внутри — metallic-roughness («PBR-lite»),
каскадные тени от направленного источника, небо по трём цветам градиента,
линейная отрисовка отладочной геометрии и треугольно-точный пикинг на CPU.

Главная особенность API — **отложенная (deferred) отправка команд**. Кадр
делится на две фазы:

1. `BeginFrame(camera, width, height, target, clearColor, clearDepth)` — начать
   кадр: сохранить камеру и размеры, привязать `RenderTarget` (или стандартный
   фреймбуфер, если `target == nullptr`), очистить буферы и обнулить списки
   команд.
2. Между `BeginFrame` и `EndFrame` вы **только записываете** команды.
   `Draw`, `DrawInstanced`, `DrawInstancedBuffer` кладут элемент в список кадра;
   `AddLight` добавляет источник света; `DrawLine`, `DrawSphere`, `DrawWireBox`
   и родственные методы копят вершины отладочных линий; `AddPostDraw`
   регистрирует колбэк. Ни один из этих вызовов не рисует немедленно и не
   меняет состояние OpenGL — важно, например, что `Draw` можно звать в любом
   порядке, сортировкой займётся `EndFrame`.
3. `EndFrame()` — выполняет весь конвейер целиком: отсечение, сортировку,
   теневые проходы, небо, непрозрачное, сетку, прозрачное, пользовательские
   пост-проходы и отладочные линии. После этого списки кадра очищаются.

Отсюда правило: **команды отрисовки** (`Draw*`, отладочные линии, `DrawSky`,
`DrawGrid`, `AddPostDraw`) вне пары `BeginFrame`/`EndFrame` бессмысленны — их
данные будут отброшены. Источники света, впрочем, переживают кадр: список
источников живёт до `ClearLights()`, поэтому `AddLight` можно звать и до
`BeginFrame`.

Порядок проходов внутри `EndFrame()`:

| № | Шаг | Что происходит |
|---|---|---|
| 1 | Отсечение и сортировка | Элементы делятся на непрозрачные и прозрачные (`AlphaMode::Blend`), отсекаются по пирамиде видимости (при `frustumCulling`) и сортируются: непрозрачные — от ближних к дальним, прозрачные — от дальних к ближним. |
| 2 | Свет и каскады | Первые источники (не больше `maxLights` и не больше 8) упаковываются в uniform-массивы; для главного источника с `castShadows` считаются матрицы каскадов. |
| 3 | Теневые карты | Проход только с записью глубины по непрозрачным элементам, по одному проходу на активный каскад. |
| 4 | Depth prepass | Необязательный проход только с глубиной (`depthPrepass`), ускоряющий затенение плотной геометрии. |
| 5 | Небо | Полноэкранный треугольник с градиентом — если вызван `DrawSky()` или включён `Environment::drawSky`. |
| 6 | Непрозрачные | Материалы `Opaque` и `Mask`, от ближних к дальним, с записью глубины. |
| 7 | Сетка | Сетка на плоскости XZ — если вызван `DrawGrid()` или включён `Environment::drawGrid`. |
| 8 | Прозрачные | Материалы `Blend`, от дальних к ближним, со смешиванием `SRC_ALPHA` / `ONE_MINUS_SRC_ALPHA` и **выключенной** записью глубины. |
| 9 | Пост-проходы | Колбэки `AddPostDraw` в порядке регистрации: после всей геометрии и до отладочных линий. |
| 10 | Отладочные линии | Накопленные `DrawLine` / `DrawSphere` / `DrawWireBox` / …: сначала пакет с тестом глубины, затем пакет «поверх всего», внутри — группировка по толщине линии. |
| 11 | Восстановление | Возвращаются глубина, смешивание, `glColorMask`, VAO и текущая шейдерная программа. |

**Смешивание: только альфа.** `Renderer3D` вызывает ровно
`glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)` — и только для прозрачных
элементов, сетки и отладочных линий. Аддитивного пути (`GL_ONE, GL_ONE`),
`GL_SRC_ALPHA_SATURATE` или раздельных `glBlendFuncSeparate` в рендерере нет.
Практическое следствие: эффекты, которым нужен аддитивный вклад (свечение,
световые шлейфы, вспышки), нельзя получить одним прозрачным мешем — их делают
**альфа-смешанными оболочками** (несколько вложенных полупрозрачных слоёв) или
выносят в `AddPostDraw`, где колбэк сам владеет состоянием GL и шейдером.

Честные ограничения текущей реализации:

* **Тени сэмплирует только один источник света за кадр.** Шейдер объявляет три
  каскада одной матрицы; главным становится первый направленный источник с
  `castShadows`, а если такого нет — первый точечный или прожектор (для
  прожектора каскад точный, для точечного источника это документированное
  приближение одной гранью 90°).
* **Поля, которые реализация не читает:** `lodBias`, `softShadows`,
  `maxShadowCasters`, `pointShadowMapSize` и `sortTransparent`. Они объявлены в
  структурах настроек, но текущий код `Renderer3D.cpp` к ним не обращается:
  прозрачные сортируются всегда, мягкость тени регулируется `pcfRadius`, а
  размер карты теней берётся из `directionalMapSize`.
* **Толщина отладочных линий** зажимается диапазоном 0.5…1.0: в core-профиле
  `glLineWidth` больше единицы — ошибка GL.
* **Потолок источников света — 8** (`kMaxLights` в шейдере), даже если
  `maxLights` больше.
* **`DrawInstancedBuffer`** не может отсечь отдельные инстансы (данные матриц
  живут в чужом буфере) и отбрасывает тень только в базовой позе.
* **Пикинг** хранит не больше 4096 объектов, а **отладочные линии** — не больше
  262144 вершин за кадр; излишек молча отбрасывается.

## Члены класса

### `struct ShadowSettings`

Настройки теней: включение, размеры карт, каскады, смещения и фильтрация.
Все поля — публичные данные, поэтому менять их можно напрямую через
`Renderer3D::Shadows()`.

| Поле | По умолчанию | Смысл |
|---|---|---|
| `enabled` | `true` | включены ли теневые проходы и сэмплирование |
| `directionalMapSize` | `2048` | сторона квадратной карты теней, px |
| `cascadeCount` | `3` | число каскадов, 1…3 |
| `cascadeSplitLambda` | `0.85` | баланс равномерного и логарифмического разбиения |
| `cascadeDistance` | `60.0` | дальность теней, мировые единицы |
| `depthBias` | `0.0025` | смещение глубины против acne |
| `normalBias` | `0.02` | смещение по нормали |
| `pcfRadius` | `1.5` | радиус PCF-фильтра в текселях |
| `pointShadowMapSize` | `512` | зарезервировано под точечные тени |
| `maxShadowCasters` | `256` | зарезервировано под бюджет отбрасывающих тень |
| `softShadows` | `true` | зарезервировано под общий переключатель мягкости |

```cpp
crossrender::ShadowSettings sh;      // значения по умолчанию
sh.enabled = true;
sh.cascadeCount = 3;
sh.cascadeDistance = 80.0f;
r3d.Shadows() = sh;          // применить целиком
```

### `bool ShadowSettings::enabled`

Включает и теневые проходы, и сэмплирование карт в форвардном шейдере. При
`false` каскады не строятся, а `uShadowEnabled` уходит в шейдер нулём.

```cpp
r3d.Shadows().enabled = false;   // ночная сцена, свет только от emissive-геометрии
```

### `int ShadowSettings::directionalMapSize`

Сторона квадратной карты теней для главного источника, в пикселях. Значение
зажимается в диапазон 64…8192; карты пересоздаются только при смене размера,
поэтому дёргать поле каждый кадр бессмысленно.

```cpp
r3d.Shadows().directionalMapSize = 4096;   // качество важнее памяти
```

### `int ShadowSettings::cascadeCount`

Число каскадов, 1…3. Верхняя граница задана шейдером (`kMaxCascades`), поэтому
значение вне диапазона зажимается, а не приводит к ошибке.

```cpp
r3d.Shadows().cascadeCount = 2;   // компромисс: меньше проходов, крупнее тексель
```

### `f32 ShadowSettings::cascadeSplitLambda`

Баланс между равномерным (`0`) и логарифмическим (`1`) распределением границ
каскадов. Больше значение — плотнее каскады у камеры.

```cpp
r3d.Shadows().cascadeSplitLambda = 0.9f;
```

### `f32 ShadowSettings::cascadeDistance`

Дальность теней в мировых единицах. Итоговая дальность — `min(camera.farZ,
cascadeDistance)`, так что тени никогда не уходят за плоскость дальней
отсечки камеры.

```cpp
r3d.Shadows().cascadeDistance = 120.0f;   // большая открытая сцена
```

### `f32 ShadowSettings::depthBias`

Смещение глубины в теневом сравнении (`uShadowBias`). Лечит «акне» — полосы
самозатенения на почти параллельных свету поверхностях.

```cpp
r3d.Shadows().depthBias = 0.004f;
```

### `f32 ShadowSettings::normalBias`

Смещение точки выборки по нормали (`uShadowNormalBias`). Помогает там, где
одного `depthBias` мало: на склонах и тонкой геометрии.

```cpp
r3d.Shadows().normalBias = 0.03f;
```

### `f32 ShadowSettings::pcfRadius`

Радиус PCF-фильтра (percentage-closer filtering, сглаживание края тени) в
текселях карты; уходит в шейдер как `uShadowPcfRadius`. Это и есть основной
регулятор мягкости тени — поле `softShadows` реализация не читает.

```cpp
r3d.Shadows().pcfRadius = 2.5f;   // заметно мягче край
```

### `int ShadowSettings::pointShadowMapSize`

Зарезервировано под отдельные карты точечных источников. Текущая реализация
поле не читает: и направленный, и локальный источник получают карту размера
`directionalMapSize`.

```cpp
// Поле существует в API, но сейчас ни на что не влияет.
r3d.Shadows().pointShadowMapSize = 1024;
```

### `int ShadowSettings::maxShadowCasters`

Зарезервировано под ограничение числа объектов, отбрасывающих тень. Текущая
реализация поле не читает: в теневой проход попадают все непрозрачные элементы
списка кадра, прошедшие отсечение по каскаду.

```cpp
// Поле существует в API, но сейчас ни на что не влияет.
r3d.Shadows().maxShadowCasters = 64;
```

### `bool ShadowSettings::softShadows`

Зарезервировано под общий переключатель мягких теней. Текущая реализация поле
не читает — мягкость задаётся радиусом `pcfRadius`.

```cpp
r3d.Shadows().softShadows = true;   // значение по умолчанию, на код не влияет
```

### `struct Renderer3DSettings`

Общие переключатели рендерера: отсечение, сортировка, отладочные режимы,
предел источников света и смещение LOD.

| Поле | По умолчанию | Смысл |
|---|---|---|
| `frustumCulling` | `true` | отсекать элементы вне пирамиды видимости |
| `sortTransparent` | `true` | объявлено для сортировки прозрачных (реализация сортирует всегда) |
| `wireframe` | `false` | рисовать меши линиями (отладка) |
| `showShadowCascades` | `false` | подкрашивать геометрию по номеру каскада |
| `depthPrepass` | `false` | проход только с глубиной перед затенением |
| `backfaceCulling` | `true` | отсекать задние грани там, где материал не double-sided |
| `maxLights` | `8` | предел источников света за кадр |
| `lodBias` | `1.0` | объявлено для выбора уровня детализации (реализация не читает) |

```cpp
crossrender::Renderer3DSettings s;
s.frustumCulling = true;
s.maxLights = 4;          // хватит: солнце, лампа, две вспышки
r3d.Settings() = s;
```

### `bool Renderer3DSettings::frustumCulling`

Включает отсечение по пирамиде видимости на CPU: AABB каждого элемента
трансформируется в мир и проверяется против шести плоскостей камеры. Отсечённые
элементы не попадают ни в цветовой, ни в теневой проход и считаются в
`Render3DStats::culledObjects`.

```cpp
r3d.Settings().frustumCulling = false;   // отладка: видеть, что рендерер получает всё
```

### `bool Renderer3DSettings::sortTransparent`

Объявлено как переключатель сортировки прозрачной геометрии. Текущая
реализация поле не читает: прозрачные элементы **всегда** сортируются от
дальних к ближним, потому что без этого альфа-смешивание даёт неверный
результат.

```cpp
// Поле существует в API, но сортировка прозрачных выполняется всегда.
r3d.Settings().sortTransparent = true;
```

### `bool Renderer3DSettings::wireframe`

Отладочный режим: вместо индексированных треугольников рисуются пары линий по
тому же индексному буферу. Счётчик `Render3DStats::lines` растёт, а
`triangles` — нет.

```cpp
r3d.Settings().wireframe = true;   // каркас всей сцены
```

### `bool Renderer3DSettings::showShadowCascades`

Отладочная подсветка: каждый элемент получает оттенок (красноватый, зелёный,
синеватый) по номеру каскада, в который он попадает. Удобно проверять границы
каскадов и `cascadeDistance`.

```cpp
r3d.Settings().showShadowCascades = true;
```

### `bool Renderer3DSettings::depthPrepass`

Включает проход только с записью глубины перед основным затенением. Ускоряет
сцены с высокой перерисовкой, но добавляет лишние draw call-ы; alpha-masked
материалы в prepass не участвуют, чтобы не записать глубину отброшенных
текселей.

```cpp
r3d.Settings().depthPrepass = true;   // плотная геометрия, мало прозрачных
```

### `bool Renderer3DSettings::backfaceCulling`

Глобальный выключатель отсечения задних граней. Материал может отказаться от
отсечения и сам (`doubleSided` или `CullMode::None`) — тогда побеждает
материал. Теневой проход рисует двусторонне независимо от этого поля.

```cpp
r3d.Settings().backfaceCulling = false;   // отладка тонких плоскостей
```

### `int Renderer3DSettings::maxLights`

Сколько источников света реально доедет до шейдера за кадр. Итоговое число —
`min(LightCount(), maxLights, 8)`: восемь — жёсткий потолок шейдера
(`kMaxLights`). `AddLight` сверх этого предела возвращает `-1` с
предупреждением в лог.

```cpp
r3d.Settings().maxLights = 4;
ENG_LOGI("demo", "в шейдер уйдёт не больше %d источников", r3d.MaxLights());
```

### `f32 Renderer3DSettings::lodBias`

Объявлено как смещение выбора уровня детализации. Текущая реализация
`Renderer3D.cpp` поле не читает — выбор LOD в рендерере не реализован, значение
ни на что не влияет.

```cpp
// Поле существует в API, но сейчас ни на что не влияет.
r3d.Settings().lodBias = 1.5f;
```

### `struct Render3DStats`

Счётчики кадра, заполняемые `EndFrame()`. Сбрасываются в начале каждого
`BeginFrame`, поэтому читать их нужно после `EndFrame()`.

| Поле | Тип | Смысл |
|---|---|---|
| `drawCalls` | `int` | вызовы отрисовки, включая теневые проходы и отладочные линии |
| `triangles` | `int` | треугольники, включая ушедшие в теневые карты |
| `vertices` | `int` | вершины (по числу вершин меша на вызов) |
| `shadowCasters` | `int` | сколько вызовов ушло в теневые карты |
| `lights` | `int` | сколько источников реально упаковано (≤ 8) |
| `instancedDraws` | `int` | вызовы инстансированной отрисовки |
| `culledObjects` | `int` | элементы, отсечённые по пирамиде видимости |
| `lines` | `int` | отрезки отладочных линий и сетки |
| `cpuFrameMs` | `f32` | процессорное время самого `EndFrame()`, мс |

```cpp
const crossrender::Render3DStats& st = r3d.GetStats();
ENG_LOGI("demo", "draw calls %d, треугольников %d (%d отсечено), теней %d, свет %d, %.2f мс",
         st.drawCalls, st.triangles, st.culledObjects, st.shadowCasters, st.lights, st.cpuFrameMs);
```

### `int Render3DStats::drawCalls`

Число вызовов отрисовки за кадр: непрозрачные и прозрачные элементы, проходы по
каскадам, сетка, небо и отладочные линии. Хороший индикатор того, помогает ли
инстансинг.

```cpp
if (r3d.GetStats().drawCalls > 1500) ENG_LOGW("demo", "слишком много draw call-ов за кадр");
```

### `int Render3DStats::triangles`

Треугольники за кадр, включая теневой проход — то есть один и тот же меш,
попавший в цветовой проход и в три каскада, посчитается четыре раза.

```cpp
ENG_LOGI("demo", "пропускная способность: %d треугольников за кадр", r3d.GetStats().triangles);
```

### `int Render3DStats::vertices`

Вершины за кадр. Для инстансированных элементов умножается на число инстансов.

```cpp
const float perVertex = static_cast<float>(r3d.GetStats().cpuFrameMs) /
                        static_cast<float>(std::max(1, r3d.GetStats().vertices));
ENG_LOGI("demo", "%.4f мс на киловершину", perVertex * 1000.0f);
```

### `int Render3DStats::shadowCasters`

Сколько вызовов отрисовки ушло в теневые проходы за кадр. Если значение равно
нулю при включённых тенях, значит ни один непрозрачный элемент не попал в
каскад — проверьте `cascadeDistance` и `castShadows` у источника.

```cpp
if (r3d.Shadows().enabled && r3d.GetStats().shadowCasters == 0) {
    ENG_LOGW("demo", "тени включены, но ни один объект не отбрасывает тень");
}
```

### `int Render3DStats::lights`

Число источников, реально доехавших до шейдера: `min(LightCount(), maxLights,
8)`. Может быть меньше `LightCount()`, если `maxLights` снижен.

```cpp
if (r3d.LightCount() > r3d.GetStats().lights) {
    ENG_LOGW("demo", "часть источников не попала в шейдер: %d из %d", r3d.GetStats().lights,
             r3d.LightCount());
}
```

### `int Render3DStats::instancedDraws`

Число вызовов инстансированной отрисовки. Один `DrawInstanced` с сотней
инстансов даёт единицу здесь и сто — в `drawCalls` теневого прохода (тени
рисуются по инстансам отдельными вызовами).

```cpp
ENG_LOGI("demo", "инстансированных вызовов: %d", r3d.GetStats().instancedDraws);
```

### `int Render3DStats::culledObjects`

Сколько элементов отсекла пирамида видимости. Полезно, чтобы понять,
работает ли `frustumCulling` и насколько сцена выходит за кадр.

```cpp
r3d.Settings().frustumCulling = true;
ENG_LOGI("demo", "отсечено объектов: %d", r3d.GetStats().culledObjects);
```

### `int Render3DStats::lines`

Отрезки, нарисованные отладочным проходом: `DrawLine`, `DrawLines`, `DrawSphere`,
`DrawWireBox`, `DrawArrow`, `DrawCapsule`, `DrawFrustum` и сетка.

```cpp
ENG_LOGI("demo", "отрезков отладки: %d", r3d.GetStats().lines);
```

### `f32 Render3DStats::cpuFrameMs`

Процессорное время, которое занял `EndFrame()`: отсечение, сортировка, подсчёт
матриц каскадов и все вызовы GL. Не включает время драйвера на GPU.

```cpp
if (r3d.GetStats().cpuFrameMs > 8.0f) ENG_LOGW("demo", "CPU-часть кадра слишком дорогая");
```

### `struct Ray`

Луч в мировых координатах: начало и направление. Используется `Raycast`.

| Поле | Тип | Смысл |
|---|---|---|
| `origin` | `Vec3` | начало луча |
| `direction` | `Vec3` | направление; нормализуется внутри `Raycast` |

```cpp
crossrender::Ray ray;
ray.origin = camera.position;
ray.direction = camera.Forward();
```

### `Vec3 Ray::origin`

Начало луча. Для пикинга из камеры это обычно `Camera::position`.

```cpp
crossrender::Ray ray;
ray.origin = {0.0f, 2.0f, 6.0f};   // позиция игрока
ray.direction = {0.0f, 0.0f, -1.0f};
```

### `Vec3 Ray::direction`

Направление луча. Нулевое направление недопустимо: `Raycast` подменяет его
вектором `{0, 0, -1}`, чтобы не делить на ноль.

```cpp
crossrender::Ray ray{{0, 1, 0}, crossrender::Normalize(crossrender::Vec3{1, -1, 0})};
```

### `struct RayHit`

Результат `Raycast` / `PickAtScreen`. При промахе возвращается `false`, а
структура остаётся в состоянии по умолчанию (`hit == false`).

| Поле | Тип | Смысл |
|---|---|---|
| `hit` | `bool` | было ли попадание |
| `distance` | `f32` | расстояние вдоль луча в мировых единицах |
| `point` | `Vec3` | точка попадания в мире |
| `normal` | `Vec3` | нормаль поверхности (по умолчанию `{0, 1, 0}`) |
| `uv` | `Vec2` | интерполированные UV или проекция точки на грани AABB |
| `objectId` | `int` | `id`, переданный в `SubmitPickable` |
| `triangleIndex` | `int` | номер треугольника или `-1` для попадания по AABB |
| `meshName` | `std::string` | имя меша (`Mesh::Name()`) |
| `userData` | `const void*` | пользовательский указатель из `SubmitPickable` |

```cpp
crossrender::RayHit hit;
if (r3d.Raycast(ray, &hit)) {
    ENG_LOGI("demo", "%s: t=%.2f, треугольник %d, id %d", hit.meshName.c_str(), hit.distance,
             hit.triangleIndex, hit.objectId);
}
```

### `bool RayHit::hit`

Флаг попадания. `Raycast` возвращает `true` только вместе с `hit = true`, но
удобно проверять и поле — например, когда структура пришла из другого кода.

```cpp
crossrender::RayHit hit;
r3d.Raycast(ray, &hit);
if (hit.hit) ENG_LOGI("demo", "попадание на расстоянии %.2f", hit.distance);
```

### `f32 RayHit::distance`

Расстояние от начала луча до точки попадания в мировых единицах. Сохраняется
корректным даже при масштабировании модели: трансформация в локальное
пространство не меняет параметр луча.

```cpp
if (r3d.Raycast(ray, &hit) && hit.distance < 2.0f) ENG_LOGI("demo", "цель вплотную");
```

### `Vec3 RayHit::point`

Точка попадания в мировом пространстве: `origin + direction * distance`.

```cpp
r3d.DrawSphere(hit.point, 0.1f, crossrender::Color::Red, 8);   // маркер попадания
```

### `Vec3 RayHit::normal`

Нормаль поверхности в точке попадания, полученная из треугольника и
преобразованная нормальной матрицей объекта. Для попадания по AABB — нормаль
той грани, на которой оказалась точка.

```cpp
const crossrender::Vec3 decalUp = hit.normal;   // ориентируем декаль/искру по поверхности
```

### `Vec2 RayHit::uv`

UV-координаты: интерполированные по треугольнику (барицентрика) при точном
пикинге, либо проекция точки на соответствующую грань AABB — в запасном режиме.

```cpp
ENG_LOGD("demo", "uv попадания: %.3f, %.3f", hit.uv.x, hit.uv.y);
```

### `int RayHit::objectId`

Идентификатор, который вы передали в `SubmitPickable`. Рендерер его не
интерпретирует — это ваш ключ (индекс сущности, слот инвентаря, что угодно).

```cpp
r3d.SubmitPickable(mesh, transform, /*id=*/42);
// ...
if (r3d.Raycast(ray, &hit)) SelectEntity(hit.objectId);
```

### `int RayHit::triangleIndex`

Номер треугольника в `MeshData`, по которому произошло точное попадание, или
`-1`, если точных данных о вершинах нет и сработал запасной путь по AABB.

```cpp
if (hit.triangleIndex >= 0) ENG_LOGI("demo", "точное попадание в треугольник %d", hit.triangleIndex);
else ENG_LOGI("demo", "попадание по ограничивающей коробке");
```

### `std::string RayHit::meshName`

Имя меша (`Mesh::Name()`), в который попал луч. Удобно для отладочного вывода,
когда `objectId` ещё не задан.

```cpp
ENG_LOGI("demo", "луч попал в '%s'", hit.meshName.c_str());
```

### `const void* RayHit::userData`

Пользовательский указатель, переданный в `SubmitPickable`. Позволяет сразу
получить объект сцены, не заводя таблицу «id → сущность».

```cpp
struct Body { crossrender::Vec3 velocity; };
Body body;
r3d.SubmitPickable(mesh, transform, 0, &body);
if (r3d.Raycast(ray, &hit)) {
    auto* picked = static_cast<Body*>(const_cast<void*>(hit.userData));
    if (picked) picked->velocity = {0, 0, 0};
}
```

### `struct PickableObject`

Элемент списка для CPU-пикинга: геометрия, её матрица и обратная матрица,
идентификатор и пользовательские данные. Заполняется через `SubmitPickable`,
напрямую обычно не используется.

| Поле | Тип | Смысл |
|---|---|---|
| `mesh` | `const Mesh*` | геометрия; рендерер не владеет ею |
| `transform` | `Mat4` | матрица объект → мир |
| `inverse` | `Mat4` | заранее посчитанная матрица мир → объект |
| `id` | `int` | ваш идентификатор объекта |
| `userData` | `void*` | ваш указатель |

```cpp
crossrender::PickableObject p;
p.mesh = &mesh;
p.transform = crossrender::Mat4::Translate({0, 1, 0});
p.inverse = p.transform.Inverse();   // SubmitPickable считает её сам
p.id = 1;
p.userData = nullptr;
```

### `const Mesh* PickableObject::mesh`

Указатель на меш. Объект должен жить дольше, чем запись в списке пикинга;
рендерер не копирует геометрию и не владеет ею. Меш без корректных границ
(`Bounds().Valid() == false`) пропускается.

```cpp
crossrender::PickableObject p;
p.mesh = &wheelMesh;   // wheelMesh живёт в сцене
p.id = 7;
r3d.SubmitPickable(*p.mesh, crossrender::Mat4::Identity(), p.id);
```

### `Mat4 PickableObject::transform`

Матрица объекта в мире, переданная в `SubmitPickable`. Именно она определяет,
где луч встречает геометрию.

```cpp
const crossrender::Mat4 tf = crossrender::Mat4::TRS({2, 0, 1}, {0, yaw, 0}, {1, 1, 1});
r3d.SubmitPickable(barrel, tf, 3);
```

### `Mat4 PickableObject::inverse`

Обратная матрица `transform`, посчитанная один раз при регистрации. В неё
переводятся начало и направление луча, поэтому мировые расстояния остаются
корректными и при неравномерном масштабе.

```cpp
// Заполняется автоматически; поле приведено, чтобы объяснить устройство.
// r3d.SubmitPickable(mesh, transform, id)  ->  inverse = transform.Inverse()
```

### `int PickableObject::id`

Идентификатор объекта, попадающий в `RayHit::objectId`.

```cpp
r3d.SubmitPickable(mesh, transform, /*id=*/static_cast<int>(entityIndex));
```

### `void* PickableObject::userData`

Произвольный указатель, который вернётся в `RayHit::userData`. Может быть
`nullptr`.

```cpp
r3d.SubmitPickable(mesh, transform, id, &inventorySlot);
```

### `class Renderer3D`

Сам рендерер. Копировать нельзя (удалены конструктор копирования и
присваивание) — владейте им как полем сцены или движка. Деструктор вызывает
`Shutdown()`, поэтому ресурсы освобождаются даже при раннем выходе.

```cpp
class Level {
public:
    bool Load() {
        if (!r3d_.Init()) return false;
        return true;
    }

private:
    crossrender::Renderer3D r3d_;   // один рендерер на уровень
};
```

### `Renderer3D()`

Создаёт объект и внутреннее состояние, но **не** трогает OpenGL: контекст не
нужен, точки входа загружать не обязательно. Реальные ресурсы создаёт `Init()`.

```cpp
crossrender::Renderer3D r3d;   // дёшево: только выделение внутренней структуры
```

### `~Renderer3D()`

Вызывает `Shutdown()`: удаляет буферы, VAO, шейдеры, карты теней и текстуры.
Требует текущего контекста OpenGL — уничтожайте рендерер до разрушения окна.

```cpp
{
    crossrender::Renderer3D r3d;
    r3d.Init();
    // ...
}   // здесь Shutdown() вызовется автоматически
```

### `bool Init()`

Создаёт шейдерные программы (форвардную, инстансированную, теневую, небо и
линии), VAO/VBO отладочной геометрии, текстуру-заглушку и карты теней.
Повторный вызов безопасен: если рендерер уже инициализирован, возвращается
`true` без работы.

* **Возвращает:** `true`, если рендерер готов. `false` — если не загружены
  ключевые точки входа OpenGL или не собрались шейдеры.
* **Контекст:** требует текущего контекста и выполненного `gl::LoadFunctions`.

```cpp
crossrender::Renderer3D r3d;
if (!r3d.Init()) {
    ENG_LOGE("demo", "3D-рендерер не инициализировался — сцена не будет нарисована");
    return false;
}
```

### `void Shutdown()`

Освобождает все ресурсы GPU и очищает списки кадра; объект после этого снова
можно инициализировать через `Init()`. Вызывается автоматически из деструктора,
поэтому явный вызов нужен только для перезапуска рендерера.

```cpp
r3d.Shutdown();   // контекст ещё жив: ресурсы освобождаются корректно
r3d.Init();       // например, после смены разрешения и пересоздания карт теней
```

### `void BeginFrame(const Camera& camera, int fbWidth, int fbHeight, RenderTarget* target = nullptr, bool clearColor = true, bool clearDepth = true)`

Начинает кадр: запоминает камеру, привязывает целевой фреймбуфер, очищает
буферы и обнуляет список команд и статистику. Если `target == nullptr`,
рисуем в стандартный фреймбуфер окна; иначе вызывается `RenderTarget::Bind()`
и размеры берутся из таргета, а не из `fbWidth`/`fbHeight`.

* **Параметры:** `camera` — камера кадра; `fbWidth`, `fbHeight` — размер
  фреймбуфера по умолчанию; `target` — необязательный офскрин-таргет;
  `clearColor`, `clearDepth` — что очищать. Цвет очистки фиксирован
  (`{0.055, 0.063, 0.078}`), флаг лишь включает очистку цветового буфера.
* **Контекст:** без успешного `Init()` команды не записываются, а последующий
  `EndFrame()` один раз предупредит об этом в лог.

```cpp
crossrender::Renderer3D r3d;
r3d.Init();
r3d.BeginFrame(camera, 1280, 720);                       // прямо в окно
r3d.Draw(mesh, material, transform);
r3d.EndFrame();
```

### `void EndFrame()`

Выполняет весь конвейер кадра (см. порядок проходов в `## Обзор`) и очищает
списки команд. Без парного `BeginFrame` элементы рисоваться не будут; при
неудачном `Init()` вызов игнорируется с однократным предупреждением.

```cpp
r3d.BeginFrame(camera, w, h, nullptr, true, true);
r3d.AddLight(crossrender::Light::Directional({0, -1, 0}, crossrender::Color::White, 2.0f, true));
r3d.Draw(ground, groundMaterial);
r3d.EndFrame();   // только здесь всё это действительно нарисуется
```

### `void SetViewport(f32 x, f32 y, f32 w, f32 h)`

Задаёт область вывода внутри текущего фреймбуфера. Координаты — в пикселях,
**y направлен вниз** (как в оконных системах): рендерер сам переворачивает
прямоугольник для OpenGL. Соотношение сторон камеры пересчитывается по `w/h`,
поэтому 3D-сцена корректно рисуется в подобласти под 2D-интерфейсом.

```cpp
// 3D-вид в левой половине окна, интерфейс займёт правую.
r3d.SetViewport(0.0f, 0.0f, width * 0.5f, static_cast<crossrender::f32>(height));
r3d.BeginFrame(camera, width, height);
```

### `void ResetViewport()`

Возвращает область вывода на весь фреймбуфер и восстанавливает исходное
соотношение сторон.

```cpp
// Сначала рисуем 3D в подобласти, затем возвращаем полный кадр под 2D-интерфейс.
r3d.SetViewport(0.0f, 0.0f, 640.0f, 360.0f);
r3d.ResetViewport();
ENG_LOGI("demo", "область вывода снова занимает весь фреймбуфер");
```

### `const Camera& GetCamera() const`

Текущая камера рендерера — та, что была передана последним `BeginFrame` или
`SetCamera`. Удобно для расчёта лучей пикинга и отладочной отрисовки.

```cpp
crossrender::Ray ray;
ray.origin = r3d.GetCamera().position;
ray.direction = r3d.GetCamera().Forward();
```

### `void SetCamera(const Camera& c)`

Сохраняет камеру, не начиная кадр. В большинстве сцен камеру просто передают в
`BeginFrame`; `SetCamera` нужен, когда камера обновляется отдельно от кадра —
например, из контроллера.

```cpp
crossrender::Camera cam = r3d.GetCamera();
cam.position += {0, 0, -1};
r3d.SetCamera(cam);
```

### `Renderer3DSettings& Settings()`

Доступ к общим настройкам рендерера. Возвращается ссылка, поэтому менять поля
можно на месте. Снимок настроек делается в начале `EndFrame()`, так что
изменение до вызова `EndFrame()` действует уже на текущий кадр.

```cpp
r3d.Settings().frustumCulling = true;
r3d.Settings().maxLights = 4;
```

### `ShadowSettings& Shadows()`

Доступ к настройкам теней. Как и `Settings()`, это ссылка на живой объект;
размер карт теней пересоздаётся только при изменении значения.

```cpp
r3d.Shadows().enabled = true;
r3d.Shadows().cascadeCount = 3;
r3d.Shadows().cascadeDistance = 90.0f;
```

### `Environment& GetEnvironment()`

Доступ к окружению: фоновый свет, цвета неба, туман, флаги `drawSky` и
`drawGrid`, приближение IBL. Ссылка позволяет менять поля на месте.

```cpp
r3d.GetEnvironment().fogEnabled = true;
r3d.GetEnvironment().fogColor = crossrender::Color::FromRGB(0x9AA8B8);
```

### `void SetEnvironment(const Environment& e)`

Заменяет окружение целиком. Полезно, когда сцена хранит окружение у себя и
применяет его раз в кадр.

```cpp
crossrender::Environment night = r3d.GetEnvironment();
night.skyTop = crossrender::Color::FromRGB(0x0B1026);
night.skyHorizon = crossrender::Color::FromRGB(0x24304F);
night.ambientIntensity = 0.15f;
r3d.SetEnvironment(night);
```

### `int AddLight(const Light& light)`

Добавляет источник света в список кадра. Источники живут до `ClearLights()`, а
не до конца кадра, поэтому добавлять их можно и до `BeginFrame`. Рендерер
присваивает копии света собственный `Light::id`.

* **Возвращает:** индекс в списке (`0` и больше) или `-1`, если достигнут
  предел `min(maxLights, 8)`.
* **Контекст:** не требует ни контекста GL, ни активного кадра.

```cpp
const int sun = r3d.AddLight(
    crossrender::Light::Directional({-0.4f, -1.0f, -0.2f}, crossrender::Color{1.0f, 0.97f, 0.9f, 1.0f}, 2.5f, true));
if (sun < 0) ENG_LOGW("demo", "источник не добавлен: достигнут предел");
```

### `void ClearLights()`

Убирает все источники света. Следующий кадр отрисуется только с фоновым
освещением `Environment` (и без теней, если теневого источника не было).

```cpp
r3d.ClearLights();
r3d.AddLight(sunLight);   // новый набор источников для следующей сцены
```

### `Light* GetLight(int index)`

Доступ к источнику по индексу, возвращённому `AddLight`. Позволяет менять
параметры каждый кадр (двигать лампу, менять интенсивность) без повторного
добавления.

* **Возвращает:** указатель на свет или `nullptr`, если индекс вне диапазона.
* **Время жизни:** указатель действителен до `ClearLights()` или пока вектор
  источников не перерос свою ёмкость; не храните его между кадрами.

```cpp
if (crossrender::Light* lamp = r3d.GetLight(lampIndex)) {
    lamp->position = playerPosition + crossrender::Vec3{0, 2, 0};
    lamp->intensity = flicker;
}
```

### `int LightCount() const`

Число добавленных источников (не обязательно совпадает с числом, попавшим в
шейдер: см. `maxLights`). Не требует кадра.

```cpp
ENG_LOGI("demo", "в сцене %d источников света", r3d.LightCount());
```

### `int MaxLights() const`

Текущий предел источников из `Settings().maxLights` (без учёта жёсткого
потолка шейдера в 8). Используйте, чтобы не добавлять заведомо лишние источники.

```cpp
if (r3d.LightCount() < r3d.MaxLights()) r3d.AddLight(muzzleFlash);
```

### `void Draw(const Mesh& mesh, const Material& material, const Mat4& transform = Mat4::Identity())`

Ставит в очередь отрисовку меша с материалом и матрицей. Материал
**копируется**, поэтому временный объект (`Material::Default()`) безопасен.
Пустой или невалидный меш игнорируется — заведомо пустой draw call в список не
попадёт.

Есть и вторая перегрузка: `void Draw(const Mesh& mesh, const Mat4& transform)` —
это тот же вызов с `Material::Default()`.

```cpp
r3d.BeginFrame(camera, width, height);
r3d.Draw(crateMesh, crateMaterial, crossrender::Mat4::Translate({2, 0.5f, -1}));
r3d.Draw(pillarMesh, crossrender::Mat4::TRS({-3, 1, 0}, {0, 0.5f, 0}, {1, 2, 1}));   // материал по умолчанию
r3d.EndFrame();
```

### `void DrawInstanced(const Mesh& mesh, const Material& material, const Mat4* transforms, int count)`

Рисует `count` копий меша по массиву матриц. Матрицы копируются во внутренний
пул кадра, поэтому исходный массив может быть временным. Отсечение выполняется
по объединению ограничивающих коробок всех инстансов — батч отсекается целиком.

```cpp
std::vector<crossrender::Mat4> instances;
for (int i = 0; i < 200; ++i) {
    instances.push_back(crossrender::Mat4::Translate({static_cast<crossrender::f32>(i % 20) * 1.5f, 0.5f,
                                              static_cast<crossrender::f32>(i / 20) * 1.5f}));
}
r3d.DrawInstanced(foliageMesh, foliageMaterial, instances.data(),
                  static_cast<int>(instances.size()));
```

### `void DrawInstancedBuffer(const Mesh& mesh, const Material& material, u32 instanceBuffer, int count)`

Инстансинг из **внешнего** GL-буфера с матрицами `Mat4`: рендерер лишь
привязывает его к атрибутам 6…9 и не копирует данные. Так рисуют толпы и
частицы, чьи матрицы уже лежат в буфере и обновляются шейдером/вычислительным
кодом. Особенности: отсечение возможно только по локальным границам меша (данные
буфера рендереру недоступны), а в теневую карту такой элемент попадает одним
вызовом в базовой позе.

```cpp
// instanceVbo — ваш буфер с count матрицами Mat4 подряд.
r3d.DrawInstancedBuffer(crowdMesh, crowdMaterial, instanceVbo, crowdCount);
```

### `void AddPostDraw(std::function<void()> fn)`

Регистрирует колбэк, который выполнится в конце кадра: **после** непрозрачной и
прозрачной геометрии и **до** прохода отладочных линий. Колбэк сам владеет
состоянием OpenGL и шейдером — именно так подключаются собственные проходы
(частицы, декали, эффекты), которым важен уже заполненный буфер глубины.
Колбэки выполняются в порядке регистрации и затем очищаются.

```cpp
r3d.AddPostDraw([&particles] {
    particles.Render();   // сам биндит шейдер, буферы и состояние GL
});
```

### `void DrawSky()`

Просит нарисовать небо в этом кадре: полноэкранный треугольник с вертикальным
градиентом из `Environment::skyTop` / `skyHorizon` / `skyBottom`, плюс подсветка
от главного направленного источника. Проход идёт после теней и до непрозрачной
геометрии, с выключенной записью глубины. Альтернатива — флаг
`Environment::drawSky`, включающий небо постоянно.

```cpp
if (camera.position.y > -5.0f) r3d.DrawSky();   // под водой небо не нужно
```

### `void DrawGrid(f32 size = 20.0f, int divisions = 20, const Color& major = Color::FromRGB(0x3A3A44), const Color& minor = Color::FromRGB(0x26262E))`

Просит нарисовать отладочную сетку на плоскости XZ: квадрат `size` на `size` с
`divisions` делениями на сторону. Каждая `divisions / 10`-я линия (но не реже
одной) рисуется цветом `major`, остальные — `minor`; центральные оси X и Z
выделены красным и синим, а яркость линий затухает к краю сетки. Геометрия
кэшируется и пересобирается только при смене параметров (`size` не меньше
0.01, `divisions` не меньше 2). Альтернатива — флаг `Environment::drawGrid`.

```cpp
r3d.DrawGrid(40.0f, 40, crossrender::Color::FromRGB(0x4A4A55), crossrender::Color::FromRGB(0x2A2A32));
```

### `void DrawLine(const Vec3& a, const Vec3& b, const Color& color, bool depthTest = true, f32 width = 1.0f)`

Копит один отрезок для отладочного прохода. При `depthTest = true` линия
скрывается геометрией, при `false` рисуется поверх сцены. Толщина зажимается
диапазоном 0.5…1.0 (ограничение core-профиля), но разные толщины всё равно
группируются в отдельные вызовы.

```cpp
r3d.DrawLine({0, 0, 0}, {0, 3, 0}, crossrender::Color::Red);                    // видна сквозь стены
r3d.DrawLine(hit.point, hit.point + hit.normal * 0.5f, crossrender::Color::Green);   // с тестом глубины
```

### `void DrawLines(const Vec3* points, int count, const Color& color, bool depthTest = true)`

Копит **связную** ломаную из `count` точек: отрезков будет `count - 1`. Точки
не разрываются — для набора независимых отрезков вызывайте `DrawLine`.

```cpp
const crossrender::Vec3 path[4] = {{0, 0, 0}, {1, 0, 1}, {2, 0, 1}, {3, 0, 0}};
r3d.DrawLines(path, 4, crossrender::Color::FromRGB(0x66CCFF));
```

### `void DrawWireBox(const Vec3& center, const Vec3& extents, const Color& color, const Mat4& transform = Mat4::Identity())`

Каркас параллелепипеда: центр, полуразмеры (`extents` — половина стороны, не
полный размер), цвет и необязательная матрица. Все двенадцать рёбер рисуются с
тестом глубины. Есть перегрузка для `Bounds`:
`void DrawWireBox(const Bounds& b, const Color& color, const Mat4& transform = Mat4::Identity())`.

```cpp
r3d.DrawWireBox({0, 1, 0}, {0.5f, 1.0f, 0.5f}, crossrender::Color::Yellow);   // коробка 1x2x1
crossrender::Bounds b;
b.Expand({-1, 0, -1});
b.Expand({1, 2, 1});
r3d.DrawWireBox(b, crossrender::Color::Green);
```

### `void DrawAabb(const Bounds& b, const Color& color)`

Каркас ограничивающей коробки без трансформации — сокращение для
`DrawWireBox(b.Center(), b.Extents(), color)`. Пустая (`Bounds::Valid() == false`)
коробка игнорируется.

```cpp
r3d.DrawAabb(mesh.Bounds(), crossrender::Color::FromRGB(0x00FF88));
```

### `void DrawSphere(const Vec3& center, f32 radius, const Color& color, int segments = 16)`

Каркасная сфера: три окружности (XY, XZ, YZ) по `segments` сегментов каждая.
Радиус меньше или равный нулю игнорируется.

```cpp
r3d.DrawSphere(light.position, light.range, crossrender::Color{1.0f, 0.9f, 0.4f, 1.0f}, 24);
```

### `void DrawArrow(const Vec3& from, const Vec3& to, const Color& color, f32 headSize = 0.15f)`

Стрелка из `from` в `to` с наконечником размера `headSize`. Удобна для
визуализации нормалей, направлений света и скоростей.

```cpp
r3d.DrawArrow(hit.point, hit.point + hit.normal * 1.0f, crossrender::Color::Cyan, 0.12f);
```

### `void DrawGizmo(const Mat4& transform, f32 scale = 1.0f, bool depthTest = false)`

Гизмо осей из начала координат `transform`: красная ось X, зелёная Y, синяя Z
со стрелками. По умолчанию рисуется поверх сцены (`depthTest = false`), что
удобно для выделенного объекта.

```cpp
if (selected) r3d.DrawGizmo(entityTransform, 1.5f);
```

### `void DrawCapsule(const Vec3& a, const Vec3& b, f32 radius, const Color& color)`

Каркасная капсула между точками `a` и `b` с радиусом `radius`: кольца на торцах,
четыре боковые линии и две каркасные сферы. Обычно визуализирует капсульный
коллайдер персонажа.

```cpp
r3d.DrawCapsule(feetPosition, feetPosition + crossrender::Vec3{0, height, 0}, 0.35f, crossrender::Color::Magenta);
```

### `void DrawFrustum(const Mat4& viewProj, const Color& color)`

Каркас пирамиды видимости, заданной матрицей `viewProj`: восемь углов
восстанавливаются обратной матрицей, рисуются ближний и дальний прямоугольники
и соединяющие рёбра. Полезно для отладки теневых каскадов.

```cpp
r3d.DrawFrustum(lightViewProj, crossrender::Color{1.0f, 0.6f, 0.2f, 1.0f});
```

### `void SubmitPickable(const Mesh& mesh, const Mat4& transform, int id, void* userData = nullptr)`

Регистрирует объект для CPU-пикинга и физических запросов: меш, его матрицу,
ваш идентификатор и необязательный указатель. Обратная матрица считается сразу.
Список пикинга **не** очищается автоматически между кадрами — обновляйте его
сами через `ClearPickables()`. Всего можно держать до 4096 объектов.

* **Контекст:** не требует кадра; `id` и `userData` вернутся в `RayHit`.

```cpp
r3d.ClearPickables();
for (const Entity& e : entities) r3d.SubmitPickable(e.mesh, e.transform, e.id, e.self);
```

### `void ClearPickables()`

Очищает список объектов пикинга. Вызывайте перед перерегистрацией сцены, иначе
в списке накопятся устаревшие копии.

```cpp
r3d.ClearPickables();
r3d.SubmitPickable(playerMesh, playerTransform, 1, &player);
```

### `bool Raycast(const Ray& ray, RayHit* hit, f32 maxDistance = 1e30f)`

Треугольно-точный луч по зарегистрированным объектам: сначала тест
ограничивающей коробки (slab-тест), затем обход треугольников по Мёллеру—
Трумбору. Если данные вершин недоступны, используется попадание по AABB
(`RayHit::triangleIndex == -1`). Возвращается **ближайшее** попадание в пределах
`maxDistance`.

* **Возвращает:** `true`, если что-то найдено; тогда `*hit` заполнена.
* **Параметры:** `hit` может быть `nullptr`, если нужен только факт попадания.

```cpp
crossrender::Ray ray{{0, 2, 6}, {0, 0, -1}};
crossrender::RayHit hit;
if (r3d.Raycast(ray, &hit, 100.0f)) {
    ENG_LOGI("demo", "попадание в '%s' на %.2f м", hit.meshName.c_str(), hit.distance);
}
```

### `bool PickAtScreen(Vec2 screenPos, Vec2 viewportSize, RayHit* hit)`

Пикинг по экранной позиции: строит луч через `Camera::RayFromScreen` и вызывает
`Raycast`. `screenPos` задаётся в долях 0…1 с началом в левом верхнем углу,
`viewportSize` — размер области в пикселях.

```cpp
const crossrender::Vec2 mouse = window.GetInput().MousePos();
const crossrender::Vec2 viewport{static_cast<crossrender::f32>(width), static_cast<crossrender::f32>(height)};
crossrender::RayHit hit;
// MousePos() в пикселях; PickAtScreen ждёт координаты в долях 0..1, y вниз.
const crossrender::Vec2 normalized{mouse.x / viewport.x, mouse.y / viewport.y};
if (r3d.PickAtScreen(normalized, viewport, &hit)) Select(hit.objectId);
```

### `const Render3DStats& GetStats() const`

Статистика последнего завершённого кадра. Читайте её после `EndFrame()`;
следующий `BeginFrame` обнулит счётчики.

```cpp
const crossrender::Render3DStats& st = r3d.GetStats();
ENG_LOGI("demo", "%d draw call-ов, %d треугольников, %.2f мс CPU", st.drawCalls, st.triangles,
         st.cpuFrameMs);
```

### `void ResetStats()`

Обнуляет счётчики вручную — например, чтобы измерить только часть кадра или
сбросить статистику после смены сцены.

```cpp
r3d.ResetStats();
r3d.BeginFrame(camera, width, height);
r3d.Draw(hugeMesh, material);
r3d.EndFrame();
ENG_LOGI("demo", "только этот меш: %d треугольников", r3d.GetStats().triangles);
```

### `const Mat4& ViewProj() const`

Матрица «вид × проекция», которой рендерер пользовался в текущем кадре. Она
учитывает активную область вывода (см. `SetViewport`), поэтому годится для
проекции мировых точек в экранные и для отладочной отрисовки пирамиды.

```cpp
const crossrender::Mat4& vp = r3d.ViewProj();
crossrender::Vec2 screen;
const crossrender::Vec2 viewport{static_cast<crossrender::f32>(width), static_cast<crossrender::f32>(height)};
if (r3d.GetCamera().WorldToScreen(enemyPosition, viewport, &screen)) {
    ENG_LOGD("demo", "враг на экране: %.2f, %.2f (m[0]=%.3f)", screen.x, screen.y, vp.m[0]);
}
```

### `Texture* SceneColor()`

HDR-текстура сцены, если рендеринг идёт через постобработку (`PostProcessor`);
в обычном режиме возвращает `nullptr` или текстуру, установленную вручную.
Это точка входа для полноэкранных эффектов, читающих уже нарисованный кадр.

```cpp
if (crossrender::Texture* scene = r3d.SceneColor()) {
    ENG_LOGI("demo", "сцена доступна как текстура %dx%d", scene->Width(), scene->Height());
}
```

### `void SetSceneColor(Texture* t)`

Сообщает рендереру, в какую HDR-текстуру пишет постобработка. Рендерер не
владеет текстурой и не освобождает её; `nullptr` допустим и означает «сцены
нет».

```cpp
r3d.SetSceneColor(&post.SceneTarget().ColorTexture());
// ... кадр ...
r3d.SetSceneColor(nullptr);   // цепочка постобработки разобрана
```

## Пример целиком

```cpp
#include "crossrender/core/Log.h"
#include "crossrender/gfx/GL.h"
#include "crossrender/gfx/Mesh.h"
#include "crossrender/gfx/Renderer3D.h"

// Кадр 3D-сцены: свет, непрозрачная геометрия, прозрачная вода, небо, сетка,
// отладочная коробка и собственный пост-проход.
class Scene {
public:
    bool Init() {
        if (!r3d_.Init()) {
            ENG_LOGE("scene", "3D-рендерер не инициализировался");
            return false;
        }

        ground_.Create(crossrender::MeshData::Plane(40.0f, 40.0f, 8));
        crate_.Create(crossrender::MeshData::Cube(1.0f));
        water_.Create(crossrender::MeshData::Plane(40.0f, 40.0f, 2));

        groundMat_ = crossrender::Material::Checker();
        crateMat_ = crossrender::Material::Metal(crossrender::Color{0.72f, 0.74f, 0.80f, 1.0f}, 0.35f);
        waterMat_ = crossrender::Material::Default();
        waterMat_.baseColor = crossrender::Color{0.15f, 0.35f, 0.60f, 0.45f};   // alpha < 1
        waterMat_.alphaMode = crossrender::AlphaMode::Blend;                    // пойдёт в прозрачный проход

        // Настройки читаются в EndFrame, поэтому менять их можно в любой момент.
        r3d_.Settings().maxLights = 4;
        r3d_.Settings().frustumCulling = true;
        r3d_.Shadows().enabled = true;
        r3d_.Shadows().cascadeCount = 3;
        r3d_.Shadows().cascadeDistance = 60.0f;

        r3d_.GetEnvironment().fogEnabled = true;
        r3d_.GetEnvironment().fogStart = 20.0f;
        r3d_.GetEnvironment().fogEnd = 90.0f;

        r3d_.AddLight(crossrender::Light::Directional({-0.4f, -1.0f, -0.25f},
                                              crossrender::Color{1.0f, 0.96f, 0.88f, 1.0f}, 2.2f, true));
        r3d_.AddLight(crossrender::Light::Point({4.0f, 3.0f, 4.0f}, crossrender::Color{1.0f, 0.6f, 0.25f, 1.0f},
                                        4.0f, 12.0f));

        // Ящик можно будет выбрать лучом.
        r3d_.ClearPickables();
        r3d_.SubmitPickable(crate_, crossrender::Mat4::Translate({0.0f, 0.5f, 0.0f}), /*id=*/1);
        return true;
    }

    void Frame(const crossrender::Camera& camera, int width, int height) {
        r3d_.BeginFrame(camera, width, height);          // фаза записи команд

        r3d_.DrawSky();                                  // просим небо
        r3d_.DrawGrid(40.0f, 40);                        // и сетку
        r3d_.Draw(ground_, groundMat_, crossrender::Mat4::Identity());
        r3d_.Draw(crate_, crateMat_, crossrender::Mat4::TRS({0.0f, 0.5f, 0.0f}, {0.0f, spin_, 0.0f}, {1, 1, 1}));
        r3d_.Draw(water_, waterMat_, crossrender::Mat4::Translate({0.0f, 0.02f, 0.0f}));  // прозрачная

        r3d_.DrawWireBox(crate_.Bounds(), crossrender::Color::Green);
        r3d_.DrawLine({0, 0, 0}, {0, 3, 0}, crossrender::Color::Red, /*depthTest=*/false);

        r3d_.AddPostDraw([] {
            // Колбэк выполнится после всей геометрии и сам владеет GL-состоянием:
            // здесь место для частиц, декалей и прочих собственных проходов.
        });

        r3d_.EndFrame();                                 // фаза исполнения конвейера

        const crossrender::Render3DStats& st = r3d_.GetStats();
        ENG_LOGD("scene", "%d draw call-ов, %d треугольников, теней %d, свет %d, %.2f мс",
                 st.drawCalls, st.triangles, st.shadowCasters, st.lights, st.cpuFrameMs);
    }

    // Пикинг мышью: экранные координаты в долях 0..1, y вниз.
    bool Pick(crossrender::Vec2 normalizedMouse, crossrender::Vec2 viewport, crossrender::RayHit* out) {
        return r3d_.PickAtScreen(normalizedMouse, viewport, out);
    }

private:
    crossrender::Renderer3D r3d_;
    crossrender::Mesh ground_, crate_, water_;
    crossrender::Material groundMat_, crateMat_, waterMat_;
    crossrender::f32 spin_ = 0.0f;
};
```

Такой класс показывает главное: между `BeginFrame` и `EndFrame` не происходит
ничего, кроме записи команд, — ни один `Draw` не трогает OpenGL. Всё
упорядочивание (отсечение, сортировка, тени, прозрачность) выполняет
`EndFrame`, а `AddPostDraw` даёт единственную легальную точку, где собственный
код может поработать с GL после того, как сцена уже нарисована.

## См. также

* `docs/gfx/GL.md` — загрузчик OpenGL, на который опирается рендерер.
* `docs/gfx/Mesh.md` — `Mesh`, `Material`, `Camera`, `Light` и `Environment`,
  которыми оперирует `Renderer3D`.
* `docs/gfx/RenderTarget.md` — офскрин-таргеты для `BeginFrame` и HDR-сцены
  для постобработки.
* `docs/gfx/Texture.md` — текстуры, которые возвращает `SceneColor()`.
* `docs/gfx/Shader.md` — собственные шейдеры для проходов `AddPostDraw`.
* `docs/core/Math.md` — `Vec3`, `Mat4`, `Bounds` и `Color` в примерах.
* `docs/core/Log.md` — макросы `ENG_LOGI` / `ENG_LOGD`, используемые в примерах.



