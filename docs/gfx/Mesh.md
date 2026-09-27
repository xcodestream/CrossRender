# crossrender/gfx/Mesh.h — вершины, меши, материалы, камера и свет

Всё, из чего собирается трёхмерная сцена: формат вершины, построители
примитивов (`MeshData`), загрузка геометрии на GPU (`Mesh`), PBR-материал,
камера с орбитальным контроллером, источники света и параметры окружения.

## Заголовок

```cpp
#include "crossrender/gfx/Mesh.h"
```

## Обзор

Заголовок делится на четыре смысловые части.

**Геометрия.** `Vertex` — фиксированный interleaved-формат вершины (position,
normal, uv, tangent, color, uv2); на него жёстко настроены атрибуты VAO.
`VertexAttribute` — битовые маски для описания раскладки. `MeshData` —
геометрия на CPU: массивы вершин и индексов, границы (`Bounds`), подмеши
(sub-mesh) и статические построители примитивов. `Mesh` — та же геометрия,
загруженная в вершинный буфер (VBO) и индексный буфер (EBO) под одним VAO.

**Материал.** `Material` — металличность-шероховатость (metallic-roughness)
без собственного кода: структура только хранит параметры, а применяет их
`Renderer3D`. Текстуры подключаются указателями `const Texture*`, то есть
материал **не владеет** ими и не продлевает им жизнь.

**Камера.** `Camera` — положение, цель и параметры проекции; `View`, `Proj` и
`ViewProj` возвращают матрицы, а `RayFromNdc` / `RayFromScreen` /
`WorldToScreen` обслуживают подбор объектов мышью (picking).
`CameraController` — орбитальный контроллер «орбита + полёт на WASD».

**Свет и окружение.** `Light` — направленный, точечный, прожектор и
площадной источник; `Environment` — аналитическое приближение освещения от
окружения (IBL), градиент неба, туман и сетка.

Порядок работы с геометрией:

1. Построить `MeshData` примитивами или загрузить модель.
2. При необходимости: `ComputeNormals`, `ComputeTangents`, `ComputeBounds`,
   `Merge` нескольких частей.
3. Загрузить в `Mesh::Create(const MeshData&)`.
4. Описать внешний вид в `Material` и повесить текстуры.
5. Поставить `Camera`, `Light` и `Environment` и передать всё это в
   `Renderer3D`.

#### Честные ограничения

* `Mesh::Create(const void* vertices, usize vertexBytes, const std::vector<u32>& indices,
  const std::vector<u32>& layout)` **игнорирует** аргумент `layout`: параметр
  явно помечен `(void)layout`, и всегда используется фиксированная раскладка
  `Vertex` со шагом `sizeof(Vertex)`. Передать чужой формат вершины нельзя.
* Индексы всегда 32-битные (`GL_UNSIGNED_INT`); поле `index32_` фактически не
  влияет ни на что.
* Шаг вершин жёстко равен `sizeof(Vertex)`, поэтому «сжать» вершину
  (например, до position+uv) нельзя.
* Без контекста OpenGL `Mesh::Create` возвращает `false`, а
  `ENG_LOGW("mesh", ...)` пишется только в случае пустой геометрии. Тихая
  неудача без GPU — ожидаемое поведение, а не ошибка.
* `Material::Metal` выставляет `metallic = 1.0`, а при такой металличности
  диффузного отклика нет: если нет карты окружения (IBL выключен через
  `Environment::useIbl = false` или нет подходящих данных), поверхность
  выглядит чёрной. Для металла всегда включайте IBL.
* У `Material` нет режима аддитивного смешивания: `AlphaMode` умеет только
  `Opaque`, `Mask` и `Blend`, а `Blend`-проход прямого рендерера смешивает
  строго по `SRC_ALPHA / ONE_MINUS_SRC_ALPHA`. `BlendMode::Additive` из
  `crossrender/gfx/Texture.h` к материалам не применяется.
* `MeshData::Merge` складывает только вершины и индексы: подмеши (sub-mesh)
  и `materialIndex` частей теряются, у результата `subMeshes` пуст.
* `Mesh::GetPrimitive` создаёт меши в статическом кэше и никогда их не
  освобождает (по замыслу — кэш живёт до конца процесса); неизвестный ключ
  молча даёт куб.
* `CameraController::Update` всегда перезаписывает `position`, `target` и
  `up` камеры, даже когда `active == false`; крена (roll) у контроллера нет.

## Члены класса

### `struct Vertex`

Вершина в фиксированном interleaved-формате. Именно этот формат ожидает VAO,
созданный `Mesh::Create`: атрибут `0` — позиция, `1` — нормаль, `2` — UV,
`3` — касательная, `4` — цвет, `5` — `uv2`. Смещения берутся через
`offsetof`, поэтому менять порядок и типы полей нельзя, не поправив
`Mesh.cpp`.

```cpp
crossrender::Vertex v;
v.position = {1.0f, 0.0f, 0.0f};
v.normal = {0.0f, 1.0f, 0.0f};
v.uv = {0.5f, 0.5f};
ENG_LOGI("demo", "вершина: %zu байт", sizeof(crossrender::Vertex));
```

### `Vec3 Vertex::position`

Позиция вершины в локальном пространстве модели. Используется
`ComputeBounds` и `ComputeNormals`.

```cpp
crossrender::Vertex v;
v.position = vertexA.position + vertexB.position;
```

### `Vec3 Vertex::normal`

Нормаль для освещения; по умолчанию `{0, 1, 0}` (вверх). Если геометрия
процедурная, нормали надо заполнить или вызвать `MeshData::ComputeNormals`.

```cpp
crossrender::Vertex v;
v.normal = crossrender::Normalize(crossrender::Vec3{0.3f, 1.0f, 0.2f});
```

### `Vec2 Vertex::uv`

Текстурные координаты основного набора (атлас, albedo-карта). Для вокселей
часть данных упакована в `uv2`.

```cpp
crossrender::Vertex v;
v.uv = {0.25f, 0.75f};
```

### `Vec4 Vertex::tangent`

Касательная для normal mapping: `xyz` — направление, `w` — знак базиса
(обычно `±1`). По умолчанию `{1, 0, 0, 1}`; корректные значения считает
`MeshData::ComputeTangents`.

```cpp
crossrender::Vertex v;
v.tangent = crossrender::Vec4{1.0f, 0.0f, 0.0f, 1.0f};
```

### `Vec4 Vertex::color`

Цвет вершины (по умолчанию белый). Учитывается материалом при
`Material::vertexColors == true` — так окрашиваются воксели и low-poly
модели.

```cpp
crossrender::Vertex v;
v.color = crossrender::Color::Red.ToVec4();
```

### `Vec2 Vertex::uv2`

Второй набор UV. Для вокселей в него упакованы индекс палитры и ambient
occlusion (затенение от окружения).

```cpp
crossrender::Vertex v;
v.uv2 = {3.0f, 0.75f};        // индекс палитры 3, AO 0.75
```

### `enum class VertexAttribute : u32`

Битовые маски полей вершины. Набор удобен, чтобы описывать, какие атрибуты
реально нужны проходу (например, теневому достаточно позиции), и чтобы
собирать произвольные раскладки в инструментах.

| Значение | Бит | Что включает |
|---|---|---|
| `VertexAttribute::Position` | `1 << 0` | `position` |
| `VertexAttribute::Normal` | `1 << 1` | `normal` |
| `VertexAttribute::UV` | `1 << 2` | `uv` |
| `VertexAttribute::Tangent` | `1 << 3` | `tangent` |
| `VertexAttribute::Color` | `1 << 4` | `color` |
| `VertexAttribute::UV2` | `1 << 5` | `uv2` |

```cpp
const crossrender::u32 shadowMask = static_cast<crossrender::u32>(crossrender::VertexAttribute::Position);
const crossrender::u32 fullMask = shadowMask |
                          static_cast<crossrender::u32>(crossrender::VertexAttribute::Normal) |
                          static_cast<crossrender::u32>(crossrender::VertexAttribute::UV);
ENG_LOGI("demo", "маска вершин: 0x%X", fullMask);
```

### `struct MeshData`

Геометрия на CPU: вершины, индексы, границы, имя и подмеши. Все построители
примитивов — статические методы этой структуры, поэтому собрать меш можно без
единого обращения к GPU.

```cpp
crossrender::MeshData data = crossrender::MeshData::Cube(1.0f);
ENG_LOGI("demo", "куб: %d вершин, %d индексов", static_cast<int>(data.vertices.size()),
         static_cast<int>(data.indices.size()));
```

### `std::vector<Vertex> MeshData::vertices`

Вершины в порядке укладки в буфер. Именно размер этого массива становится
`vertexCount_` при загрузке.

```cpp
data.vertices.push_back(crossrender::Vertex{});
```

### `std::vector<u32> MeshData::indices`

Индексы треугольников (`3 * число треугольников`). `Mesh::Create` загружает
их как `GL_UNSIGNED_INT`.

```cpp
data.indices.insert(data.indices.end(), {0, 1, 2});
```

### `Bounds MeshData::bounds`

Ограничивающий параллелепипед (AABB, axis-aligned bounding box). Заполняется
`ComputeBounds`; `Mesh::Create` копирует его в `Mesh::Bounds()` и считает
заново, если поле осталось невалидным.

```cpp
data.ComputeBounds();
ENG_LOGI("demo", "радиус меша: %.3f", data.bounds.Radius());
```

### `std::string MeshData::name`

Имя меша. Построители заполняют его сами (`"cube"`, `"sphere"`,
`"icosphere"`, `"plane"`, `"cylinder"`, `"cone"`, `"torus"`, `"capsule"`,
`"quad"`, `"merged"`); `Mesh::Create` переносит непустое имя в `Mesh`.

```cpp
crossrender::MeshData data = crossrender::MeshData::Torus();
data.name = "reactor-ring";
```

### `std::vector<SubMesh> MeshData::subMeshes`

Группы индексов, соответствующие отдельным материалам (OBJ `usemtl`,
примитивы glTF). Если список пуст, `Mesh::Create` создаст одну группу на всю
геометрию. `Merge` подмеши не переносит.

```cpp
ENG_LOGI("demo", "групп материалов: %d", static_cast<int>(data.subMeshes.size()));
```

### `struct MeshData::SubMesh`

Один подмеш (группа индексов) с номером материала. Позволяет нарисовать
одну геометрию несколькими материалами через `Mesh::DrawSub`.

```cpp
crossrender::MeshData::SubMesh group;
group.indexOffset = 0;
group.indexCount = 36;
group.materialIndex = 1;
group.name = "lid";
data.subMeshes.push_back(group);
```

### `u32 MeshData::SubMesh::indexOffset`

Смещение первого индекса группы внутри общего индексного буфера.

```cpp
ENG_LOGI("demo", "группа начинается с индекса %u", group.indexOffset);
```

### `u32 MeshData::SubMesh::indexCount`

Число индексов в группе.

```cpp
ENG_ASSERT(group.indexOffset + group.indexCount <= data.indices.size());
```

### `int MeshData::SubMesh::materialIndex`

Номер материала группы — индекс во внешнем массиве материалов, который ведёт
загрузчик модели. Сам `Material` здесь не хранится.

```cpp
if (group.materialIndex == 0) { /* группа использует первый материал */ }
```

### `std::string MeshData::SubMesh::name`

Имя группы (например, имя материала из OBJ или имя примитива glTF). Нужно
для отладки и сопоставления с ассетом.

```cpp
ENG_LOGD("demo", "подмеш '%s'", group.name.c_str());
```

### `void MeshData::ComputeBounds()`

Пересчитывает `bounds` по всем вершинам. Для пустого меша оставляет
невалидные границы (`Bounds::Valid() == false`).

```cpp
crossrender::MeshData data = crossrender::MeshData::Quad(2.0f, 1.0f);
data.ComputeBounds();
ENG_LOGI("demo", "центр %.2f %.2f %.2f", data.bounds.Center().x, data.bounds.Center().y,
         data.bounds.Center().z);
```

### `void MeshData::ComputeNormals(bool smooth = true)`

Считает нормали из геометрии треугольников. `smooth = true` суммирует нормали
треугольников в вершинах (сглаженное затенение), `smooth = false` присваивает
нормаль треугольника каждой из трёх его вершин (плоские грани). Индексы вне
диапазона пропускаются; нулевые нормали заменяются на `{0, 1, 0}`.

```cpp
crossrender::MeshData data = crossrender::MeshData::Plane(4.0f, 4.0f, 8);
data.ComputeNormals(/*smooth=*/false);   // LowPoly-вид
```

### `void MeshData::ComputeTangents()`

Считает касательные из позиций и UV (нужны для normal mapping). Треугольники
с вырожденными UV пропускаются; для вершин без валидной касательной
записывается `{1, 0, 0, 1}`. Требует уже посчитанных нормалей, поэтому
сначала вызывайте `ComputeNormals`.

```cpp
crossrender::MeshData data = crossrender::MeshData::Sphere(1.0f, 32, 24);
data.ComputeNormals(true);
data.ComputeTangents();
```

### `static MeshData MeshData::Cube(f32 size = 1.0f, bool smoothNormals = false)`

Куб с длиной ребра `size` (шесть квадов, по 4 вершины на грань). При
`smoothNormals = true` нормали заменяются на `normalize(position)`, из-за чего
куб затеняется как сфера — приём для LowPoly-стилистики, но геометрически это
по-прежнему куб.

```cpp
crossrender::MeshData cube = crossrender::MeshData::Cube(2.0f);
crossrender::MeshData soft = crossrender::MeshData::Cube(2.0f, /*smoothNormals=*/true);
```

### `static MeshData MeshData::Sphere(f32 radius = 1.0f, int segments = 24, int rings = 16)`

**UV-сфера**: параллели и меридианы, полюса и шов продублированы вершинами,
UV раскладываются регулярно. `segments` (не меньше 3) — число делений по
долготе, `rings` (не меньше 2) — по широте. Треугольники в полюсах
вырождаются, это нормально для UV-развёртки. Если нужна равномерная сетка без
полюсных артефактов, берите `IcoSphere`.

```cpp
crossrender::MeshData ball = crossrender::MeshData::Sphere(0.5f, 32, 24);
ball.ComputeTangents();
```

### `static MeshData MeshData::IcoSphere(f32 radius = 1.0f, int subdivisions = 2)`

Икосаэдр, рекурсивно разбитый на `subdivisions` уровней (0 — 12 вершин,
каждый уровень умножает число треугольников на 4). Даёт равномерное
распределение вершин без полюсов; в отличие от `Sphere`, UV
восстанавливаются из направления (`atan2` / `asin`), поэтому шов всё же есть,
а касательные считаются сразу.

```cpp
crossrender::MeshData ball = crossrender::MeshData::IcoSphere(1.0f, 3);   // 642 вершины
ENG_LOGI("demo", "icosphere: %d вершин", static_cast<int>(ball.vertices.size()));
```

### `static MeshData MeshData::Plane(f32 width = 1.0f, f32 depth = 1.0f, int subdiv = 1, Vec2 uvScale = {1, 1})`

Плоскость в плоскости XZ (нормаль вверх), размером `width x depth` с сеткой
`subdiv x subdiv` (минимум 1). `uvScale` умножает UV, то есть задаёт
тайлинг текстуры. Удобна для пола и для сетки.

```cpp
crossrender::MeshData floorMesh = crossrender::MeshData::Plane(20.0f, 20.0f, 4, crossrender::Vec2{8.0f, 8.0f});
```

### `static MeshData MeshData::Cylinder(f32 radius = 0.5f, f32 height = 1.0f, int segments = 24, bool capped = true)`

Цилиндр по оси Y высотой `height` (от `-height/2` до `+height/2`). При
`capped = true` добавляются крышки (центр плюс веер); нормали боковой
поверхности радиальные, крышек — вдоль оси.

```cpp
crossrender::MeshData pipe = crossrender::MeshData::Cylinder(0.2f, 3.0f, 16, /*capped=*/true);
```

### `static MeshData MeshData::Cone(f32 radius = 0.5f, f32 height = 1.0f, int segments = 24)`

Конус по оси Y с вершиной в `+height/2`. Основание (дно) всегда закрыто
веером; нормали боковых треугольников плоские.

```cpp
crossrender::MeshData spike = crossrender::MeshData::Cone(0.35f, 1.4f, 12);
```

### `static MeshData MeshData::Torus(f32 major = 1.0f, f32 minor = 0.25f, int majorSeg = 32, int minorSeg = 16)`

Тор: `major` — радиус кольца, `minor` — радиус трубки. Нормали
аналитические, UV регулярные.

```cpp
crossrender::MeshData ring = crossrender::MeshData::Torus(1.0f, 0.15f, 48, 12);
```

### `static MeshData MeshData::Capsule(f32 radius = 0.5f, f32 height = 1.0f, int segments = 16, int rings = 8)`

Капсула: цилиндр с двумя полусферами. `height` — полная высота, поэтому
длина цилиндрической части равна `max(0, height/2 - radius)`; при
`height <= 2 * radius` получается просто сфера. `rings` делится пополам между
полусферами (минимум 2), `segments` — деления по кругу.

```cpp
crossrender::MeshData body = crossrender::MeshData::Capsule(0.4f, 1.8f, 20, 12);
```

### `static MeshData MeshData::Quad(f32 w = 1.0f, f32 h = 1.0f)`

Один плоский квад в плоскости XY с нормалью `{0, 0, 1}` — база для
спрайтов, декалей и UI-панелей. Имя меша — `"quad"`.

```cpp
crossrender::MeshData panel = crossrender::MeshData::Quad(2.0f, 1.0f);
```

### `static MeshData MeshData::Merge(const std::vector<MeshData>& parts)`

Склеивает несколько мешей в один: вершины дописываются подряд, индексы
перебазируются на смещение. Границы пересчитываются. Подмеши и
`materialIndex` частей **не переносятся** — если нужно рисовать части разными
материалами, заполняйте `subMeshes` вручную.

```cpp
std::vector<crossrender::MeshData> parts = {crossrender::MeshData::Cube(0.5f), crossrender::MeshData::Sphere(0.3f, 16, 8)};
crossrender::MeshData castle = crossrender::MeshData::Merge(parts);
ENG_LOGI("demo", "объединённый меш: %d вершин", static_cast<int>(castle.vertices.size()));
```

### `class Mesh`

Геометрия на GPU: VAO, VBO и EBO. Копировать нельзя, перемещать можно;
деструктор вызывает `Destroy()`. Все методы рисования — тихие no-op, если меш
не создан, поэтому проверять `Valid()` нужно только там, где важно знать
результат.

```cpp
crossrender::Mesh gpu;
crossrender::MeshData data = crossrender::MeshData::Cube();
if (!gpu.Create(data)) ENG_LOGW("demo", "меш не загрузился (нет GPU?)");
```

### `Mesh()`

Создаёт пустой объект: ни VAO, ни буферов, `Valid() == false`, счётчики нули.
Обращений к OpenGL нет.

```cpp
crossrender::Mesh placeholder;
ENG_ASSERT(!placeholder.Valid());
```

### `~Mesh()`

Вызывает `Destroy()` — удаляет VAO и буферы, если они были созданы.

```cpp
{
    crossrender::Mesh local;
    local.Create(crossrender::MeshData::Quad());
}   // буферы освобождены
```

### `Mesh(Mesh&&) noexcept`

Перемещающий конструктор: переносит имена буферов, счётчики, границы,
подмеши и имя, оставляя источник пустым. Нужен для `std::vector<Mesh>`.

```cpp
std::vector<crossrender::Mesh> scene;
scene.push_back(crossrender::Mesh{});
```

### `Mesh& operator=(Mesh&&) noexcept`

Перемещающее присваивание: освобождает текущие буферы и забирает ресурсы
другого объекта. Самоприсваивание безопасно.

```cpp
crossrender::Mesh a, b;
b = std::move(a);       // ресурсы переехали в b
```

### `Mesh(const Mesh&) = delete`, `Mesh& operator=(const Mesh&) = delete`

Копирование запрещено: VAO/VBO/EBO — уникальные ресурсы GPU. Храните меши по
значению в контейнере с перемещением или по указателю.

```cpp
void Submit(const crossrender::Mesh& mesh, const crossrender::Material& material) {
    mesh.Draw();                    // по ссылке, без копии
}
```

### `bool Create(const MeshData& data)`

Основной способ загрузки: создаёт (или переиспользует) VAO, VBO и EBO,
заливает вершины и индексы, настраивает шесть атрибутов по фиксированным
смещениям. Переносит `bounds`, `subMeshes` и непустое `name` из `MeshData`.
Если `subMeshes` пуст, создаётся одна группа на всю геометрию.

* **Возвращает:** `false` без контекста OpenGL или при пустой геометрии
  (в последнем случае пишет `ENG_LOGW("mesh", "mesh '%s' has no vertices")`).
* **Счётчики:** `VertexCount()`/`IndexCount()` заполняются даже при неудаче.

```cpp
crossrender::Mesh mesh;
crossrender::MeshData data = crossrender::MeshData::Sphere(1.0f, 32, 16);
data.ComputeTangents();
if (!mesh.Create(data)) {
    ENG_LOGE("demo", "не удалось загрузить сферу");
}
```

### `bool Create(const void* vertices, usize vertexBytes, const std::vector<u32>& indices, const std::vector<u32>& layout)`

Загрузка «сырых» вершин: `vertexBytes` байт из `vertices` и готовый список
индексов. `vertexCount_` считается как `vertexBytes / sizeof(Vertex)`.
Аргумент `layout` не используется — всегда применяется раскладка `Vertex`;
передавайте массив именно этих структур. Подмеши и границы не заполняются.
Возвращает `false` без контекста OpenGL.

```cpp
std::vector<crossrender::Vertex> verts(3);
std::vector<crossrender::u32> idx = {0, 1, 2};
crossrender::Mesh tri;
const std::vector<crossrender::u32> layout = {3, 3, 2, 4, 4, 2};
tri.Create(verts.data(), verts.size() * sizeof(crossrender::Vertex), idx, layout);
```

### `void Destroy()`

Удаляет EBO, VBO и VAO, обнуляет счётчики и очищает список подмешей.
Повторный вызов безопасен. `name_` и `bounds_` при этом сохраняются.

```cpp
mesh.Destroy();
ENG_ASSERT(!mesh.Valid());
```

### `void Bind() const`

Привязывает VAO (`glBindVertexArray`) без рисования — если нужно выставить
дополнительные атрибуты (например, инстансные) перед вызовом отрисовки.

```cpp
mesh.Bind();
crossrender::gl::glDrawArrays(crossrender::gl::GL_TRIANGLES, 0, 3);
```

### `void Draw() const`

Рисует все индексы меша как `GL_TRIANGLES`. Если VAO нет или индексный
буфер пуст — ничего не делает, поэтому безопасен для условно загруженных
объектов.

```cpp
if (mesh.Valid()) mesh.Draw();
```

### `void DrawSub(u32 indexOffset, u32 indexCount) const`

Рисует один подмеш: `indexCount` индексов, начиная со смещения
`indexOffset` (смещение задаётся в индексах, а не в байтах). Так рисуют
геометрию с разными материалами.

```cpp
for (int i = 0; i < mesh.SubMeshCount(); ++i) {
    const crossrender::MeshData::SubMesh& group = mesh.SubMeshes()[i];
    mesh.DrawSub(group.indexOffset, group.indexCount);
}
```

### `void DrawInstanced(int count) const`

Рисует весь меш `count` раз одним вызовом (`glDrawElementsInstanced`). При
`count <= 0` — no-op. Инстансные атрибуты (матрица, цвет) должен подготовить
вызывающий.

```cpp
crossrender::Mesh* grass = crossrender::Mesh::GetPrimitive("quad");
if (grass) grass->DrawInstanced(500);        // 500 травинок
```

### `void DrawSubInstanced(u32 indexOffset, u32 indexCount, int count) const`

Инстансная отрисовка одного подмеша. При `count <= 0` или пустом диапазоне —
no-op.

```cpp
const crossrender::MeshData::SubMesh& leaves = mesh.SubMeshes()[1];
mesh.DrawSubInstanced(leaves.indexOffset, leaves.indexCount, 120);
```

### `bool Valid() const`

`true`, если VAO создан. Основная проверка перед рисованием, особенно в
headless-режиме.

```cpp
if (!mesh.Valid()) ENG_LOGW("demo", "меш без GPU, рисовать нечего");
```

### `u32 IndexCount() const`

Число индексов (не треугольников). Заполняется даже при неудачном `Create`,
поэтому не путайте его с `Valid()`.

```cpp
ENG_LOGI("demo", "треугольников: %u", mesh.IndexCount() / 3);
```

### `u32 VertexCount() const`

Число вершин меша.

```cpp
ENG_LOGI("demo", "вершин: %u, индексов: %u", mesh.VertexCount(), mesh.IndexCount());
```

### `const Bounds& Bounds() const`

Ограничивающий параллелепипед меша в локальном пространстве. Пригодится для
отсечения (culling) и для центрирования камеры.

```cpp
const crossrender::Bounds& b = mesh.Bounds();
camera.target = b.Center();
camera.position = b.Center() + crossrender::Vec3{0, 0, b.Radius() * 2.5f};
```

### `const MeshData::SubMesh* SubMeshes() const`

Указатель на массив подмешей (`nullptr`, если список пуст). Действителен до
следующего изменения меша.

```cpp
const crossrender::MeshData::SubMesh* groups = mesh.SubMeshes();
for (int i = 0; i < mesh.SubMeshCount(); ++i) ENG_LOGI("demo", "группа %d", i);
```

### `int SubMeshCount() const`

Число подмешей. После `Create(const MeshData&)` минимум один, если геометрия
непустая.

```cpp
ENG_LOGI("demo", "материальных групп: %d", mesh.SubMeshCount());
```

### `const std::string& Name() const`

Имя меша. `Create(const MeshData&)` копирует его из `MeshData::name`, если
там непусто; иначе остаётся имя, заданное ранее.

```cpp
ENG_LOGI("demo", "рисую '%s'", mesh.Name().c_str());
```

### `void SetName(std::string n)`

Задаёт имя меша вручную — для логов, профайлера и отладочного UI.

```cpp
mesh.SetName("hero-body");
```

### `static Mesh* GetPrimitive(const std::string& key)`

Возвращает меш примитива из статического кэша, создавая его при первом
обращении. Поддерживаемые ключи: `"cube"`, `"sphere"`, `"quad"`, `"plane"`,
`"cylinder"`, `"cone"`, `"torus"`, `"capsule"`; любой другой ключ молча даёт
куб. Меш живёт до конца процесса и не освобождается. Требует контекста
OpenGL: без него вернётся невалидный меш, который останется в кэше.

```cpp
crossrender::Mesh* box = crossrender::Mesh::GetPrimitive("cube");
if (box) box->Draw();
```

### `enum class AlphaMode : u8`

Как материал обходится с альфой. Прозрачные объекты (`Blend`) откладываются и
рисуются после непрозрачных.

| Значение | Смысл |
|---|---|
| `AlphaMode::Opaque` | альфа игнорируется, объект непрозрачный |
| `AlphaMode::Mask` | альфа ниже `alphaCutoff` отбрасывается, остальное — непрозрачно |
| `AlphaMode::Blend` | обычное смешивание `SRC_ALPHA / ONE_MINUS_SRC_ALPHA` |

```cpp
crossrender::Material foliage;
foliage.alphaMode = crossrender::AlphaMode::Mask;     // листья: жёсткий порог
foliage.alphaCutoff = 0.4f;
```

### `enum class CullMode : u8`

Отсечение граней (backface culling).

| Значение | Смысл |
|---|---|
| `CullMode::None` | рисовать обе стороны |
| `CullMode::Back` | отсекать задние грани (по умолчанию, против часовой — лицевая) |
| `CullMode::Front` | отсекать передние грани (для внутренних поверхностей) |

```cpp
crossrender::Material bottle;
bottle.cullMode = crossrender::CullMode::Front;      // видно только «изнутри»
```

### `struct Material`

Параметры поверхности в модели металличность-шероховатость: базовый цвет,
`metallic`, `roughness`, эмиссия и пять необязательных текстур. Структура
ничего не рисует — значения читает `Renderer3D` при применении материала.
Текстуры подключаются невладеющими указателями, поэтому время жизни текстур
контролирует вызывающий.

```cpp
crossrender::Material m;
m.baseColor = crossrender::Color::FromRGB(0x4C8BF5);
m.roughness = 0.4f;
m.metallic = 0.0f;
```

### `std::string Material::name`

Имя материала (для отладки, логов и выбора в редакторе). На рендеринг не
влияет.

```cpp
crossrender::Material m;
m.name = "WoodenCrate";
```

### `Color Material::baseColor`

Базовый (albedo) цвет, по умолчанию серый `0.8`. Умножается на `tint` и на
`baseColorTex`, если она есть.

```cpp
m.baseColor = crossrender::Color::White;
```

### `f32 Material::metallic`

Металличность `0..1`. При `0` поверхность диэлектрическая, при `1` —
металлическая. **При `metallic = 1` без карты окружения (IBL) поверхность
чёрная**: диффузного отклика у металла нет, а отражать нечего. Пресет
`Material::Metal` выставляет ровно `1.0`, так что включайте
`Environment::useIbl = true`.

```cpp
crossrender::Material steel = crossrender::Material::Metal(crossrender::Color::FromRGB(0xB0B4B8), 0.25f);
steel.metallic = 1.0f;    // без IBL станет чёрным
```

### `f32 Material::roughness`

Шероховатость `0` (зеркало) … `1` (полностью матовая). Вместе с `metallic`
задаёт вид освещения; значение по умолчанию — `0.7`.

```cpp
m.roughness = 0.15f;      // полированный пластик
```

### `Color Material::emissive`

Цвет собственного свечения. Итоговая эмиссия — `emissive * emissiveStrength`,
и она прибавляется к освещению (в материалах это не источник света для
сцены).

```cpp
m.emissive = crossrender::Color::FromRGB(0x00FF88);
```

### `f32 Material::emissiveStrength`

Множитель эмиссии. `0` полностью выключает свечение, значения больше `1` дают
пересвет и хорошо попадают в bloom.

```cpp
m.emissiveStrength = 3.0f;      // неон
```

### `f32 Material::normalStrength`

Насколько сильно применяется карта нормалей (`normalTex`). `0` отключает
эффект рельефа, `1` — полная сила.

```cpp
m.normalTex = &brickNormals;
m.normalStrength = 1.5f;
```

### `f32 Material::occlusionStrength`

Сила карты затенения (`occlusionTex`), обычно 0..1. Уменьшает за ambient-
освещение в затенённых участках.

```cpp
m.occlusionTex = &brickAO;
m.occlusionStrength = 0.8f;
```

### `f32 Material::alphaCutoff`

Порог для `AlphaMode::Mask`: тексели с альфой ниже порога отбрасываются.
Значение по умолчанию — `0.5`.

```cpp
m.alphaMode = crossrender::AlphaMode::Mask;
m.alphaCutoff = 0.35f;
```

### `AlphaMode Material::alphaMode`

Режим работы с альфой. Это единственный «режим прозрачности» материала:
аддитивного или умноженного смешивания у `Material` нет.

```cpp
m.alphaMode = crossrender::AlphaMode::Blend;    // стекло
```

### `CullMode Material::cullMode`

Какие грани отсекать. Игнорируется, если `doubleSided == true` (тогда
отсечение выключается целиком).

```cpp
m.cullMode = crossrender::CullMode::None;
```

### `bool Material::doubleSided`

`true` — рисовать обе стороны поверхности (отсечение граней выключается).
Нужно для листвы, ткани и тонких плоскостей.

```cpp
m.doubleSided = true;
```

### `bool Material::receiveShadows`

Учитывать ли тени от источников света на этой поверхности. Выключение
экономит выборки из карты теней (например, для неба и частиц).

```cpp
m.receiveShadows = false;       // материал фона
```

### `bool Material::unlit`

`true` — не применять освещение, вывести базовый цвет как есть. Так делаются
интерфейсные панели, маркеры и отладочная геометрия.

```cpp
crossrender::Material marker = crossrender::Material::Unlit(crossrender::Color::Yellow);
```

### `f32 Material::pointSize`

Размер точки (в пикселях) для материалов, которые рисуются точками, —
например, облака точек или отладочные маркеры. На треугольники не влияет.

```cpp
m.pointSize = 3.0f;
```

### `const Texture* Material::baseColorTex`

Карта базового цвета (albedo). `nullptr` — использовать только `baseColor`.
Указатель невладеющий: текстура должна жить дольше материала.

```cpp
m.baseColorTex = &albedo;       // юнит 0
```

### `const Texture* Material::normalTex`

Карта нормалей в касательном пространстве; сила задаётся
`normalStrength`. `nullptr` — плоская нормаль.

```cpp
m.normalTex = &normalMap;       // юнит 1
```

### `const Texture* Material::metallicRoughnessTex`

Совмещённая карта металличности и шероховатости (обычно `G` — шероховатость,
`B` — металличность, как в glTF). Умножается на скалярные `metallic` и
`roughness`.

```cpp
m.metallicRoughnessTex = &ormMap;   // юнит 2
```

### `const Texture* Material::emissiveTex`

Карта эмиссии, умножается на `emissive` и `emissiveStrength`.

```cpp
m.emissiveTex = &crossrender;          // юнит 3
```

### `const Texture* Material::occlusionTex`

Карта ambient occlusion; сила — `occlusionStrength`.

```cpp
m.occlusionTex = &ao;           // юнит 4
```

### `Vec2 Material::uvScale`

Масштаб UV для всех текстур материала (тайлинг). `{1, 1}` — без изменений.

```cpp
crossrender::Material floorMat;
floorMat.uvScale = {4.0f, 4.0f};
```

### `Vec2 Material::uvOffset`

Сдвиг UV для всех текстур материала — анимация «бегущей» воды или прокрутка
атласа. `{0, 0}` — без сдвига.

```cpp
water.uvOffset = {0.0f, static_cast<float>(elapsed) * 0.05f};
```

### `bool Material::vertexColors`

Учитывать цвет вершины (`Vertex::color`) при затенении. Основной путь
окраски вокселей и LowPoly-моделей.

```cpp
m.vertexColors = true;
```

### `Color Material::tint`

Общий множитель цвета материала, применяется поверх `baseColor`. Удобен для
подсветки выбранного объекта или для инстансных вариантов одного меша.

```cpp
m.tint = crossrender::Color::FromRGB(0xFFD070);
```

### `static Material Material::Default()`

Пресет по умолчанию: серый `baseColor`, `metallic = 0`, `roughness = 0.7`,
имя `"Default"`.

```cpp
crossrender::Material fallback = crossrender::Material::Default();
```

### `static Material Material::Unlit(const Color& c)`

Неосвещённый материал заданного цвета: `unlit = true`, `roughness = 1`,
имя `"Unlit"`.

```cpp
crossrender::Material gizmo = crossrender::Material::Unlit(crossrender::Color::Cyan);
```

### `static Material Material::Checker()`

Пресет «шахматка»: белый `baseColor` и `roughness = 0.55`, имя `"Checker"`.
Используется вместе с `Texture::CreateCheckerboard` как заглушка.

```cpp
crossrender::Material missing = crossrender::Material::Checker();
missing.baseColorTex = &checkerTexture;
```

### `static Material Material::Metal(const Color& c, f32 roughness)`

Металлический пресет: `metallic = 1.0`, заданная шероховатость, имя `"Metal"`.
Помните: при `metallic = 1` без карты окружения поверхность чёрная — этот
пресет осмыслен только при включённом IBL или при подставленной cubemap.

```cpp
crossrender::Environment env;
env.useIbl = true;                           // обязательно: без IBL металл чёрный
crossrender::Material chrome = crossrender::Material::Metal(crossrender::Color::FromRGB(0xD8D8D8), 0.1f);
```

### `static Material Material::Emissive(const Color& c, f32 strength)`

Светящийся материал: `emissive = c`, `emissiveStrength = strength`, почти
чёрный `baseColor` и `roughness = 0.4`, имя `"Emissive"`. Хорошо работает в
связке с bloom в постобработке.

```cpp
crossrender::Material lamp = crossrender::Material::Emissive(crossrender::Color::FromRGB(0xFFAA33), 4.0f);
```

### `enum class ProjectionType : u8`

Тип проекции камеры.

| Значение | Смысл |
|---|---|
| `ProjectionType::Perspective` | перспектива по `fovY`, обычная 3D-камера |
| `ProjectionType::Orthographic` | ортопроекция по `orthoHeight`, изометрия и 2D-виды |

```cpp
crossrender::Camera editor;
editor.projection = crossrender::ProjectionType::Orthographic;
editor.orthoHeight = 12.0f;
```

### `class Camera`

Камера-«мешок параметров»: положение, цель, направление вверх, углы обзора и
дальности. Матрицы считаются методами `View`, `Proj`, `ViewProj`; отдельного
«слежения» нет — этим занимается `CameraController` или код приложения.

```cpp
crossrender::Camera cam;
cam.position = {4, 3, 4};
cam.target = {0, 0, 0};
const crossrender::Mat4 viewProj = cam.ViewProj(16.0f / 9.0f);
```

### `Vec3 Camera::position`

Положение камеры в мировых координатах.

```cpp
cam.position = crossrender::Vec3{0.0f, 2.0f, 8.0f};
```

### `Vec3 Camera::target`

Точка, на которую смотрит камера. Задаёт направление вместе с `position`.

```cpp
cam.target = crossrender::Vec3{0.0f, 1.0f, 0.0f};
```

### `Vec3 Camera::up`

Опорный вектор «вверх» (обычно `{0, 1, 0}`). Если он параллелен направлению
взгляда, `View()` вырождается — не ставьте камеру строго над целью.

```cpp
cam.up = crossrender::Vec3{0.0f, 1.0f, 0.0f};
```

### `f32 Camera::fovY`

Вертикальный угол обзора в **радианах**. Значение по умолчанию —
`60 * kDeg2Rad`; для задания в градусах удобно `crossrender::Radians(60.0f)`.

```cpp
cam.fovY = crossrender::Radians(45.0f);      // «телевик»
```

### `f32 Camera::nearZ`

Ближняя плоскость отсечения. Слишком маленькое значение ухудшает точность
буфера глубины.

```cpp
cam.nearZ = 0.1f;
```

### `f32 Camera::farZ`

Дальняя плоскость отсечения.

```cpp
cam.farZ = 1000.0f;
```

### `f32 Camera::orthoHeight`

Высота видимой области для ортопроекции (ширина вычисляется из неё и
соотношения сторон). На перспективную проекцию не влияет.

```cpp
cam.projection = crossrender::ProjectionType::Orthographic;
cam.orthoHeight = 10.0f;
```

### `ProjectionType Camera::projection`

Какая проекция используется в `Proj`. По умолчанию перспективная.

```cpp
cam.projection = crossrender::ProjectionType::Perspective;
```

### `Vec3 Camera::Forward() const`

Единичный вектор направления взгляда: `normalize(target - position)`.
Возвращает нулевой вектор, если точка совпадает с положением.

```cpp
const crossrender::Vec3 fwd = cam.Forward();
ENG_LOGI("demo", "камера смотрит в %.2f %.2f %.2f", fwd.x, fwd.y, fwd.z);
```

### `Vec3 Camera::Right() const`

Единичный вектор «вправо» в системе камеры: `normalize(cross(Forward(), up))`.
Удобен для движения стрейфом.

```cpp
cam.position += cam.Right() * speed * dt;
```

### `Vec3 Camera::Up() const`

Единичный вектор «вверх» в системе камеры: `cross(Right(), Forward())`.
Он перпендикулярен направлению взгляда, в отличие от поля `up`.

```cpp
const crossrender::Vec3 localUp = cam.Up();
```

### `Mat4 Camera::View() const`

Матрица вида: `Mat4::LookAt(position, target, up)`.

```cpp
const crossrender::Mat4 view = cam.View();
shader.Set("uView", view);
```

### `Mat4 Camera::Proj(f32 aspect) const`

Матрица проекции для заданного соотношения сторон. Для перспективы это
`Mat4::Perspective(fovY, aspect, nearZ, farZ)`, для ортопроекции —
`Mat4::Ortho(-w, w, -h, h, nearZ, farZ)`, где `h = orthoHeight / 2`,
`w = h * aspect`.

```cpp
const float aspect = static_cast<float>(width) / static_cast<float>(height);
shader.Set("uProj", cam.Proj(aspect));
```

### `Mat4 Camera::ViewProj(f32 aspect) const`

Произведение `Proj(aspect) * View()` — обычная матрица для вершинного
шейдера.

```cpp
const crossrender::Mat4 viewProj = cam.ViewProj(1280.0f / 720.0f);
shader.Set("uViewProj", viewProj);
```

### `void Camera::RayFromNdc(f32 ndcX, f32 ndcY, f32 aspect, Vec3* origin, Vec3* dir) const`

Строит мировой луч по нормализованным координатам устройства (NDC, Normalized
Device Coordinates) в диапазоне `[-1, 1]`. `origin` получает точку на ближней
плоскости, `dir` — нормализованное направление. Любой из указателей может
быть `nullptr`. Внутри обращает `ViewProj`.

```cpp
crossrender::Vec3 origin, dir;
cam.RayFromNdc(0.0f, 0.0f, 16.0f / 9.0f, &origin, &dir);   // луч в центр экрана
```

### `void Camera::RayFromScreen(Vec2 screenPos, Vec2 viewportSize, Vec3* origin, Vec3* dir) const`

То же, но координаты экранные: `0..1`, начало в левом верхнем углу, `y`
вниз. При неположительном размере вьюпорта возвращает `position` и
`Forward()`. Используется для выбора объектов мышью.

```cpp
crossrender::Vec3 origin, dir;
cam.RayFromScreen({0.5f, 0.5f}, {1280.0f, 720.0f}, &origin, &dir);
```

### `bool Camera::WorldToScreen(const Vec3& world, Vec2 viewportSize, Vec2* screenOut) const`

Проецирует мировую точку в экранные координаты `0..1` (`y` вниз).
Возвращает `false`, если точка за камерой (`clip.w <= 0.0001`) или размер
вьюпорта неположительный; `screenOut` может быть `nullptr`, если нужна только
проверка видимости. Так ставятся подписи и маркеры поверх 3D-сцены.

```cpp
crossrender::Vec2 screen;
if (cam.WorldToScreen(light.position, {1280.0f, 720.0f}, &screen)) {
    ENG_LOGI("demo", "метка на %.2f, %.2f", screen.x, screen.y);
}
```

### `class CameraController`

Орбитальный контроллер: вращение вокруг цели (левая или правая кнопка мыши),
панорамирование (средняя кнопка), приближение колесом и полёт на
`WASD`/`QE` (со `Shift` — втрое быстрее). Хранит собственные `yaw`, `pitch`,
`distance` и `target`, а `Update` переписывает положение камеры.

```cpp
crossrender::Camera camera;
crossrender::CameraController controller;
controller.SetOrbit(0.6f, 0.35f, 8.0f, crossrender::Vec3{0, 1, 0});
controller.Update(camera, input, dt, /*active=*/true);
```

### `void CameraController::Update(Camera& cam, const Input& input, f32 dt, bool active)`

Читает ввод и обновляет камеру. При `active == true`: вращение при зажатой
левой/правой кнопке, панорама — средней, зум — колесом, полёт —
`WASD`/`QE` (скорость `moveSpeed`, со `Shift` умножается на 3). В конце
**всегда** записывает `cam.position`, `cam.target` и `cam.up = {0, 1, 0}`,
даже если `active == false`.

```cpp
void TickCamera(crossrender::CameraController& controller, crossrender::Camera& cam, const crossrender::Input& input,
                crossrender::f32 dt) {
    controller.Update(cam, input, dt, /*active=*/true);
}
```

### `void CameraController::Orbit(f32 dx, f32 dy)`

Вращает камеру вокруг цели: `yaw -= dx * 0.01`, `pitch` меняется на
`dy * 0.01` и зажимается в `±1.5533` радиана (чуть меньше 90°), чтобы камера
не переворачивалась.

```cpp
controller.Orbit(mouseDelta.x, mouseDelta.y);
```

### `void CameraController::Zoom(f32 delta)`

Приближает или отдаляет камеру: расстояние умножается на `1 - delta * 0.1` и
зажимается в диапазон `0.3 .. 2000`.

```cpp
const crossrender::Vec2 scroll = {0.0f, 1.0f};       // колесо мыши
if (std::fabs(scroll.y) > 0.001f) controller.Zoom(scroll.y);
```

### `void CameraController::Pan(f32 dx, f32 dy)`

Сдвигает цель в экранной плоскости камеры; масштаб сдвига пропорционален
текущему расстоянию (`distance * 0.0015`), поэтому панорама ощущается
одинаково на любом зуме.

```cpp
controller.Pan(mouseDelta.x, mouseDelta.y);
```

### `void CameraController::SetOrbit(f32 yaw, f32 pitch, f32 distance, Vec3 target = {0, 0, 0})`

Задаёт состояние орбиты одним вызовом. `pitch` зажимается в `±1.55`,
`distance` — в `0.2 .. 5000`.

```cpp
controller.SetOrbit(/*yaw=*/0.8f, /*pitch=*/0.4f, /*distance=*/12.0f, crossrender::Vec3{0, 0, 0});
```

### `f32 CameraController::Distance() const`

Текущее расстояние до цели.

```cpp
ENG_LOGI("demo", "камера на расстоянии %.2f", controller.Distance());
```

### `f32 CameraController::Yaw() const`

Текущий угол поворота вокруг вертикальной оси (радианы).

```cpp
const float yaw = controller.Yaw();
```

### `f32 CameraController::Pitch() const`

Текущий угол наклона (радианы), ограничен примерно `±1.55`.

```cpp
if (controller.Pitch() > 1.4f) ENG_LOGD("demo", "камера смотрит почти сверху");
```

### `Vec3 CameraController::Target() const`

Точка, вокруг которой вращается камера.

```cpp
const crossrender::Vec3 focus = controller.Target();
```

### `void CameraController::SetTarget(const Vec3& t)`

Перемещает точку вращения, сохраняя ориентацию и расстояние.

```cpp
controller.SetTarget(crossrender::Vec3{5.0f, 0.0f, -3.0f});
```

### `void CameraController::SetDistance(f32 d)`

Задаёт расстояние до цели напрямую (без зажима — в отличие от `SetOrbit`).

```cpp
controller.SetDistance(2.5f);
```

### `f32 CameraController::MoveSpeed() const`

Текущая скорость полёта (единиц в секунду).

```cpp
ENG_LOGI("demo", "скорость полёта: %.1f", controller.MoveSpeed());
```

### `void CameraController::SetMoveSpeed(f32 s)`

Меняет скорость полёта; при зажатом `Shift` она умножается на 3.

```cpp
controller.SetMoveSpeed(12.0f);
```

### `enum class LightType : u8`

Тип источника света. Влияет на то, какие поля `Light` использует шейдер.

| Значение | Что использует |
|---|---|
| `LightType::Directional` | `direction` — свет «из бесконечности» (солнце) |
| `LightType::Point` | `position`, `range` — светит во все стороны |
| `LightType::Spot` | `position`, `direction`, `range`, `innerCone`, `outerCone` |
| `LightType::Area` | `position`, `direction`, `areaSize` — площадной источник |

```cpp
crossrender::Light sun;
sun.type = crossrender::LightType::Directional;
sun.direction = crossrender::Normalize(crossrender::Vec3{-0.5f, -1.0f, -0.3f});
```

### `struct Light`

Источник света: тип, положение/направление, цвет, интенсивность и параметры
теней. Прямой рендерер поддерживает до восьми источников одновременно, а поле
`id` заполняет `Renderer3D` при регистрации — до этого оно равно `-1`.

```cpp
crossrender::Light lamp = crossrender::Light::Point({2, 3, 1}, crossrender::Color::FromRGB(0xFFD9A0), 5.0f, 12.0f);
```

### `LightType Light::type`

Тип источника. По умолчанию `LightType::Directional`.

```cpp
crossrender::Light l;
l.type = crossrender::LightType::Point;
```

### `Vec3 Light::position`

Позиция в мире. Используется точечным, прожекторным и площадным светом; для
направленного игнорируется.

```cpp
l.position = crossrender::Vec3{0.0f, 4.0f, 0.0f};
```

### `Vec3 Light::direction`

Направление света (для направленного и прожектора). Статические фабрики
`Light::Directional` и `Light::Spot` нормализуют его сами.

```cpp
l.direction = crossrender::Normalize(crossrender::Vec3{-0.4f, -1.0f, 0.2f});
```

### `Color Light::color`

Цвет света (вместе с альфой, но на освещение влияет только RGB).

```cpp
l.color = crossrender::Color::FromRGB(0xFFF2E0);
```

### `f32 Light::intensity`

Интенсивность: множитель яркости. Для точечных источников заметно влияет
`range`.

```cpp
l.intensity = 3.5f;
```

### `f32 Light::range`

Радиус действия точечного/прожекторного света. За пределами радиуса вклад
света равен нулю.

```cpp
l.type = crossrender::LightType::Point;
l.range = 15.0f;
```

### `f32 Light::innerCone`

Внутренний угол прожектора в **радианах**: внутри него свет максимальной
яркости. По умолчанию `20°`.

```cpp
crossrender::Light spot = crossrender::Light::Spot({0, 5, 0}, {0, -1, 0}, crossrender::Color::White, 8.0f, 20.0f,
                                   crossrender::Radians(15.0f), crossrender::Radians(30.0f));
```

### `f32 Light::outerCone`

Внешний угол прожектора в радианах: между `innerCone` и `outerCone` яркость
плавно падает до нуля. По умолчанию `35°`.

```cpp
spot.outerCone = crossrender::Radians(40.0f);
```

### `Vec2 Light::areaSize`

Размер площадного источника (`LightType::Area`) в мировых единицах. Для
остальных типов не используется.

```cpp
crossrender::Light panel;
panel.type = crossrender::LightType::Area;
panel.areaSize = {2.0f, 1.0f};
```

### `bool Light::castShadows`

Включает теневой проход для этого источника. По умолчанию выключено;
фабрика `Light::Directional` и `Light::Spot` включают тени, `Light::Point` —
нет.

```cpp
sun.castShadows = true;
```

### `f32 Light::shadowBias`

Смещение глубины при сравнении с картой теней. Слишком малое даёт «акне»
(полосы самозатенения), слишком большое — отрыв тени (peter-panning).
По умолчанию `0.0025`.

```cpp
sun.shadowBias = 0.003f;
```

### `f32 Light::shadowNormalBias`

Дополнительное смещение вдоль нормали — лечит самозатенение на наклонных и
изогнутых поверхностях. По умолчанию `0.02`.

```cpp
sun.shadowNormalBias = 0.03f;
```

### `int Light::shadowMapSize`

Размер карты теней в пикселях (по умолчанию `1024`). Больше — чётче тень и
дороже проход; направленный свет использует каскады.

```cpp
sun.shadowMapSize = 2048;
```

### `int Light::id`

Внутренний номер источника, который присваивает `Renderer3D` при регистрации.
До регистрации равен `-1`; менять вручную не нужно.

```cpp
if (sun.id < 0) ENG_LOGD("demo", "источник ещё не зарегистрирован рендерером");
```

### `static Light Light::Directional(const Vec3& dir, const Color& c, f32 intensity, bool shadows = true)`

Направленный свет (солнце): нормализует `dir`, включает тени по умолчанию.
Позиция не используется.

```cpp
crossrender::Light sun = crossrender::Light::Directional({-0.4f, -1.0f, -0.2f}, crossrender::Color::FromRGB(0xFFF4E5),
                                         2.5f, /*shadows=*/true);
```

### `static Light Light::Point(const Vec3& pos, const Color& c, f32 intensity, f32 range, bool shadows = false)`

Точечный свет: задаёт позицию и радиус. Тени по умолчанию выключены, потому
что точечные карты теней дороги.

```cpp
crossrender::Light torch = crossrender::Light::Point({0, 1.5f, 0}, crossrender::Color::FromRGB(0xFFAA55), 4.0f, 10.0f);
```

### `static Light Light::Spot(const Vec3& pos, const Vec3& dir, const Color& c, f32 intensity, f32 range, f32 inner, f32 outer, bool shadows = true)`

Прожектор: позиция, направление (нормализуется), радиус, внутренний и внешний
углы в радианах. Тени включены по умолчанию.

```cpp
crossrender::Light streetLamp = crossrender::Light::Spot({0, 6, 0}, {0, -1, 0}, crossrender::Color::FromRGB(0xFFE0B0),
                                         6.0f, 25.0f, crossrender::Radians(18.0f), crossrender::Radians(32.0f));
```

### `struct Environment`

Параметры окружения сцены: ambient-свет сверху и снизу, градиент неба, туман
и приблизительная модель освещения от окружения (IBL). Это не панорамная
cubemap, а аналитическая аппроксимация — дёшево и предсказуемо, но без
реальных отражений.

```cpp
crossrender::Environment env;
env.fogEnabled = true;
env.fogColor = crossrender::Color::FromRGB(0xAABBD0);
```

### `Color Environment::ambientSky`

Цвет полусферического ambient-света сверху (небо). По умолчанию
холубоватый.

```cpp
env.ambientSky = crossrender::Color::FromRGB(0x5A7399);
```

### `Color Environment::ambientGround`

Цвет полусферического ambient-света снизу (земля/отражение от пола). По
умолчанию тёплый тёмный.

```cpp
env.ambientGround = crossrender::Color::FromRGB(0x26211E);
```

### `f32 Environment::ambientIntensity`

Общая сила ambient-света. `0` полностью отключает заливочный свет, оставляя
только источники и IBL.

```cpp
env.ambientIntensity = 0.5f;
```

### `Color Environment::skyTop`

Цвет верхней точки градиента неба. Используется и фоновым квадом, и
ambient-моделью.

```cpp
env.skyTop = crossrender::Color::FromRGB(0x2950A0);
```

### `Color Environment::skyHorizon`

Цвет горизонта в градиенте неба.

```cpp
env.skyHorizon = crossrender::Color::FromRGB(0x9EB8DB);
```

### `Color Environment::skyBottom`

Цвет нижней части градиента (под горизонтом).

```cpp
env.skyBottom = crossrender::Color::FromRGB(0x403E42);
```

### `bool Environment::drawSky`

Рисовать ли градиент неба как фон сцены. `false` оставляет фон очищенным
цветом фреймбуфера.

```cpp
env.drawSky = false;        // фон очищается вручную
```

### `bool Environment::drawGrid`

Рисовать ли отладочную сетку на уровне пола. Обычно включают в редакторе и
выключают в финальной игре.

```cpp
env.drawGrid = true;
```

### `bool Environment::fogEnabled`

Включает туман. По умолчанию выключен — сцена без него выглядит «резче».

```cpp
env.fogEnabled = true;
```

### `Color Environment::fogColor`

Цвет тумана. Обычно совпадает с цветом горизонта, иначе виден шов на
горизонте.

```cpp
env.fogColor = env.skyHorizon;
```

### `f32 Environment::fogDensity`

Плотность экспоненциального тумана (`fogMode == 1`). Больше — гуще.

```cpp
env.fogMode = 1;
env.fogDensity = 0.035f;
```

### `f32 Environment::fogStart`

Начало линейного тумана (`fogMode == 0`) в мировых единицах.

```cpp
env.fogStart = 10.0f;
```

### `f32 Environment::fogEnd`

Конец линейного тумана: дальше объекты полностью скрыты цветом тумана.

```cpp
env.fogEnd = 80.0f;
```

### `int Environment::fogMode`

Режим тумана: `0` — линейный (от `fogStart` до `fogEnd`), `1` —
экспоненциальный (по `fogDensity`). Другие значения движок трактует как
линейный.

```cpp
env.fogMode = 0;
```

### `bool Environment::useIbl`

Включает приблизительную модель освещения от окружения (image based
lighting) на основе цветов неба и земли. **Обязателен для металлических
материалов**: без него `Material::Metal` (`metallic = 1`) рисуется чёрным.

```cpp
env.useIbl = true;
ENG_ASSERT(env.useIbl || material.metallic < 1.0f);
```

## Пример целиком

```cpp
#include "crossrender/gfx/Mesh.h"

#include "crossrender/core/Log.h"
#include "crossrender/gfx/Texture.h"

#include <vector>

// Собирает процедурную сцену: меш из нескольких примитивов, материал с
// текстурами, камеру, солнце и параметры окружения.
struct DemoScene {
    crossrender::Mesh mesh;
    crossrender::Material material;
    crossrender::Camera camera;
    crossrender::Light sun;
    crossrender::Environment environment;
};

bool BuildDemoScene(DemoScene& out, crossrender::Texture& albedo, crossrender::Texture& normalMap) {
    // 1. Геометрия: несколько примитивов и объединение в один меш.
    std::vector<crossrender::MeshData> parts;
    parts.push_back(crossrender::MeshData::Plane(20.0f, 20.0f, 4, crossrender::Vec2{8.0f, 8.0f}));  // пол
    crossrender::MeshData pillar = crossrender::MeshData::Cylinder(0.4f, 3.0f, 24, true);
    pillar.name = "pillar";
    parts.push_back(pillar);
    crossrender::MeshData ball = crossrender::MeshData::IcoSphere(0.8f, 2);
    ball.name = "ball";
    ball.ComputeNormals(true);
    ball.ComputeTangents();
    parts.push_back(ball);

    crossrender::MeshData merged = crossrender::MeshData::Merge(parts);
    merged.ComputeBounds();
    ENG_LOGI("demo", "сцена: %d вершин, радиус %.2f", static_cast<int>(merged.vertices.size()),
             merged.bounds.Radius());

    // 2. Загрузка на GPU (в headless-режиме вернёт false — это нормально).
    if (!out.mesh.Create(merged)) {
        ENG_LOGW("demo", "геометрия не загружена: нет контекста OpenGL");
    }
    out.mesh.SetName("demo-scene");

    // 3. Материал и текстурные слоты: 0 — albedo, 1 — нормали, 2 — ORM.
    out.material.name = "Stone";
    out.material.baseColor = crossrender::Color::FromRGB(0xB9B2A6);
    out.material.metallic = 0.0f;              // не металл: IBL не обязателен
    out.material.roughness = 0.65f;
    out.material.baseColorTex = albedo.Valid() ? &albedo : nullptr;   // юнит 0
    out.material.normalTex = normalMap.Valid() ? &normalMap : nullptr;  // юнит 1
    out.material.uvScale = {4.0f, 4.0f};
    out.material.receiveShadows = true;

    // 4. Камера: смотрит на центр сцены.
    out.camera.position = {7.0f, 5.0f, 7.0f};
    out.camera.target = merged.bounds.Center();
    out.camera.fovY = crossrender::Radians(55.0f);
    out.camera.nearZ = 0.1f;
    out.camera.farZ = 300.0f;
    const crossrender::Mat4 viewProj = out.camera.ViewProj(16.0f / 9.0f);
    ENG_LOGD("demo", "view-proj[0] = %.3f", viewProj.m[0]);

    // 5. Свет: солнце с тенями и границами теневой карты.
    out.sun = crossrender::Light::Directional({-0.5f, -1.0f, -0.25f}, crossrender::Color::FromRGB(0xFFF3E0),
                                      2.0f, /*shadows=*/true);
    out.sun.shadowMapSize = 2048;
    out.sun.shadowBias = 0.003f;

    // 6. Окружение: IBL, туман по горизонту, сетка для отладки.
    out.environment.useIbl = true;
    out.environment.ambientIntensity = 0.4f;
    out.environment.fogEnabled = true;
    out.environment.fogMode = 0;
    out.environment.fogColor = out.environment.skyHorizon;
    out.environment.fogStart = 12.0f;
    out.environment.fogEnd = 90.0f;
    out.environment.drawGrid = true;

    // 7. Подбор объекта мышью: экранная точка -> мировой луч.
    crossrender::Vec3 rayOrigin, rayDir;
    out.camera.RayFromScreen({0.5f, 0.5f}, {1280.0f, 720.0f}, &rayOrigin, &rayDir);
    ENG_LOGI("demo", "луч из (%.2f %.2f %.2f)", rayOrigin.x, rayOrigin.y, rayOrigin.z);
    return out.mesh.Valid();
}
```

## См. также

* `docs/core/Base.md` — `kDeg2Rad`, `Radians`, типы `f32` / `u32`.
* `docs/core/Log.md` — макросы логирования, используемые в примерах.
* `docs/gfx/Texture.md` — текстуры для слотов `Material`, включая
  `CreateCheckerboard` и cubemap для неба.
* `docs/gfx/Shader.md` — layout вершинных атрибутов и передача матриц
  камеры в шейдер.
* `docs/gfx/RenderTarget.md` — куда рисуется сцена и как её читать обратно.
