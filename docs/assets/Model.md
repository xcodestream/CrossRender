# crossrender/assets/Model.h — импорт моделей и контейнер геометрии

Загрузчик готовых ассетов: читает Wavefront OBJ/MTL, glTF 2.0, PLY, STL и
MagicaVoxel `.vox`, складывает результат в контейнер `Model` (меши, материалы,
плоский список узлов) и умеет загрузить его на GPU.

## Заголовок

```cpp
#include "crossrender/assets/Model.h"
```

## Обзор

`Model.h` закрывает разрыв между файлами, которые отдают художники, и
внутренним форматом движка. Загрузчик ничего не рисует сам: он наполняет
`MeshData` (геометрия на CPU), `Material` (параметры поверхности) и
`ModelNode` (иерархия), после чего приложение вызывает `UploadToGpu` и рисует
меши через `Renderer3D`.

#### Поддерживаемые форматы

| Формат | Как определяется | Что читается | Что игнорируется |
|---|---|---|---|
| OBJ + MTL | расширение `.obj` или первое ключевое слово (`v`, `vt`, `vn`, `f`, `o`, `g`, `s`, `usemtl`, `mtllib`) в первых 4 КиБ | `v`, `vt`, `vn`, `g`, `o`, `s`, `usemtl`, `f` (полигоны веером), MTL: `Kd`, `Ks`, `Ke`, `Ns`, `d`/`Tr`, `map_Kd`, `map_Ke`, `map_Bump`/`bump`/`norm` | `vp`, `l`, `p`, `mtllib` внутри парсера, незнакомые ключевые слова, четвёртая координата `v` (`w`), `Ka`, `Ni`, `illum`, `map_Ks`, `map_d` |
| glTF 2.0 (`.gltf`) | расширение, либо первый непробельный символ `{` | `buffers`, `bufferViews`, `accessors`, `materials`, `images`, `samplers`, `textures`, `meshes`, `nodes` | skins, анимации, morph targets, `TEXCOORD_1`, `JOINTS_0`, `WEIGHTS_0`, sparse-аксессоры (берётся базовая часть), `scenes`/`scene`, расширения вроде Draco и KHR-материалов |
| GLB (`.glb`) | магия `glTF` в первых 4 байтах | чанк `JSON` и чанк `BIN\0`, дальше как `.gltf` | то же, что у `.gltf` |
| PLY | магия `ply` + пробел, либо расширение `.ply` | заголовок, `element vertex` (`x`, `y`, `z`, `nx`, `ny`, `nz`, `s`/`t`/`u`/`v`, `red`/`green`/`blue`/`alpha` и `diffuse_*`), `element face` со списком индексов; ascii, binary little-endian, binary big-endian | произвольные элементы и свойства, текстуры (в PLY их нет) |
| STL | расширение `.stl`, магия `solid`, либо точное совпадение размера с заголовком бинарного STL (`84 + 50 * count`) | треугольники, нормали фасетов; ascii и binary | материалы и UV (в STL их нет) |
| MagicaVoxel `.vox` | магия `VOX ` | чанки `SIZE`, `XYZI`, `RGBA`, `nTRN` (только трансляция кадра 0), `nSHP` | `nGRP` и незнакомые чанки, повороты и масштабы графа сцены |
| FBX | расширение `.fbx` | **ничего**: `Model::Load` распознаёт формат (`ModelFormat::FbxLite`) и сразу пишет `ENG_LOGE("model", "FBX import is not supported: ...")` | весь файл |

Формат выбирается по расширению и по содержимому одновременно
(`DetectFormat`). Если расширение неизвестно, решает магия; если расширение
известно, оно имеет приоритет, кроме glTF: текстовый `.gltf` и бинарный `.glb`
различаются только по содержимому, поэтому перепутанное расширение
исправляется автоматически.

#### Как материалы попадают в `Material`

* **OBJ.** Имена материалов берутся из строк `usemtl` (каждое новое имя
  добавляется в `Model::Materials()`), а их параметры — из библиотек `mtllib`,
  которые читаются относительно каталога `.obj`. Соответствие полей:
  `Kd` → `baseColor` (альфа сохраняется), `d`/`Tr` → `baseColor.a` (`Tr`
  инвертируется), `Ns` → `roughness = clamp(1 - sqrt(Ns / 1000), 0.04, 1)`,
  `Ks` → `metallic = clamp(0.5 * яркость, 0, 1)` (грубое приближение
  металличности), `Ke` → `emissive`, `map_Kd` → `baseColorTex`,
  `map_Ke` → `emissiveTex`, `map_Bump`/`bump`/`norm` → `normalTex`. Если
  альфа оказалась меньше `0.999`, материал получает `AlphaMode::Blend`.
* **glTF.** Поля переносятся напрямую: `baseColorFactor` → `baseColor`,
  `metallicFactor`/`roughnessFactor`, `emissiveFactor`, `normalTexture.scale` →
  `normalStrength`, `occlusionTexture.strength` → `occlusionStrength`,
  `alphaMode`/`alphaCutoff`, `doubleSided` (включает `CullMode::None`), а
  индексы текстур — в `baseColorTex`, `normalTex`, `metallicRoughnessTex`,
  `emissiveTex`, `occlusionTex`. Сэмплеры переносят только `wrapS`/`wrapT` и
  фильтр минификации.
* **VOX.** Всегда ровно один материал `"voxel"`: `baseColor` белый,
  `metallic = 0`, `roughness = 0.9`, `vertexColors = true`, а `baseColorTex`
  указывает на сгенерированную палитровую текстуру 16×16.
* **PLY и STL** материалов не несут. `FinalizeImportedModel` добавляет
  материал `"default"`, если список пуст, и подставляет его всем подмешам с
  некорректным `materialIndex`.

**Отсутствующая текстура — не ошибка загрузки.** `Texture::LoadFromFile`
возвращает `false`, загрузчик пишет предупреждение
(`"OBJ: cannot load texture ... (no GL context?)"` или
`"glTF: image N could not be decoded (no GL context?)"`) и оставляет
соответствующий указатель `nullptr`. Геометрия и материал при этом валидны, а
поверхность просто закрашивается по `baseColor`. Именно поэтому headless-импорт
(без контекста OpenGL) не падает, но и не даёт текстур.

#### Иерархия узлов и отрисовка

Иерархия **не сплющивается** в один меш. Узлы лежат плоским массивом
`std::vector<ModelNode>`: у каждого есть индекс родителя `parent`, список
`children` и ссылки `mesh`/`material` на массивы `Model::Meshes()` и
`Model::Materials()`. Локальная матрица (`localMatrix`) берётся из поля
`matrix` glTF либо собирается из `translation * rotation * scale`. Чтобы
нарисовать модель, вызывающий код сам обходит `Nodes()`, накапливает мировую
матрицу (`parentMatrix * node.localMatrix`) и вызывает `Renderer3D::Draw` для
`GpuMesh(node.mesh)` с нужным материалом. Единственное место, где движок
проходит цепочку родителей, — расчёт габаритов: `Model::Bounds()` учитывает
преобразования узлов. Если трансформ нужно запечь в вершины (например, чтобы
привести Z-up модель к Y-up), для этого есть `BakeTransform`.

Для OBJ, PLY, STL и VOX узлов в файле нет, поэтому создаётся по одному узлу на
меш с именем меша. У glTF берётся **весь** массив `nodes` файла, без выбора
активной сцены.

#### Соглашения о координатах и единицах

Импортёр **ничего не переворачивает и не масштабирует**: позиции, нормали и UV
переносятся из файла как есть, порядок обхода вершин (winding) не меняется,
оси не переставляются. Поэтому:

* glTF/GLB приходят в Y-up — так требует спецификация;
* OBJ, PLY и STL формат-агностичны: какая ось была у экспортёра, такая и
  останется (Z-up модель останется Z-up);
* единицы измерения — условные «единицы файла», движок их не интерпретирует;
* трансформы узлов не запекаются в вершины, поэтому `MeshData` хранит
  геометрию в локальном пространстве узла, а мировая матрица — на вызывающем.

`BakeTransform(Mat4)` — штатный способ привести модель к нужной ориентации или
масштабу (нормали и касательные при этом пересчитываются через нормальную
матрицу).

#### Раскладка загруженных мешей в памяти

`UploadToGpu` создаёт для каждого `MeshData` отдельный `Mesh`, то есть
отдельные VAO, VBO и EBO, в том же порядке, что и `Meshes()`. Формат вершины
фиксирован — interleaved `Vertex`: `position` (3 × f32), `normal` (3 × f32),
`uv` (2 × f32), `tangent` (4 × f32), `color` (4 × f32), `uv2` (2 × f32), итого
18 чисел = 72 байта на вершину; индексы — 32-битные. Подмеши `MeshData`
становятся группами индексов (`Mesh::SubMeshes()`), а не отдельными буферами.
Загрузка идемпотентна: повторный `UploadToGpu` ничего не делает, пока
`Uploaded() == true`.

#### Честные ограничения

* **Нет скелетной анимации, анимаций и morph targets.** Атрибуты `JOINTS_0` /
  `WEIGHTS_0` не читаются, блок `animations` не разбирается, `skins`
  игнорируются. Анимировать можно только узлы вручную.
* **Нет сжатых расширений**: Draco, meshopt, KTX2/Basis и прочие
  `KHR_*`/`EXT_*` не поддерживаются; sparse-аксессоры читаются как обычные
  (берётся базовая часть, пишется предупреждение). `KHR_materials_emissive_strength`
  игнорируется, поэтому `emissiveStrength` остаётся `1`.
* **FBX не поддерживается** — расширение распознаётся, импорт отклоняется.
* **Текстуры грузятся на этапе импорта** и требуют контекста OpenGL; в
  headless-режиме остаются только предупреждения и пустые указатели.
* **Примитивы glTF только трёх видов**: `mode` 4 (triangles) берётся как есть,
  5 (triangle strip) и 6 (triangle fan) конвертируются в треугольники; все
  остальные режимы пропускаются с предупреждением.
* **Треугольники glTF с индексами вне диапазона молча отбрасываются** (в лог
  идёт количество отброшенных); примитивы без `POSITION` тоже пропускаются.
* **Материалы OBJ — аппроксимация**: `Ks` → `metallic` и `Ns` → `roughness`
  не являются физически корректным PBR-переводом, а `map_Ks`, `map_d`, `Ka`,
  `Ni`, `illum` вообще не читаются.
* **У `.vox` из графа сцены берутся только трансляции** (кадр 0), поэтому
  повёрнутые или отмасштабированные модели соберутся неверно; сетка всех
  моделей ограничена 64 000 000 ячеек.
* **`Load` полностью сбрасывает модель** (`Destroy()` в начале), поэтому
  неудачная загрузка оставляет `Model` пустым, а `Format()` — уже
  выставленным; проверяйте `Valid()`.
* **`LoadFromMemory` не имеет базового каталога**, поэтому внешние `uri` у
  glTF (буферы и картинки) пропускаются с предупреждениями — работают только
  `data:`-URI и `bufferView`-изображения.
* **Индексация доступа не проверяется**: `MeshAt`, `MaterialAt`, `NodeAt` и
  `GpuMesh` — это `operator[]` без валидации; сначала смотрите счётчики.

## Члены класса

### `enum class ModelFormat : u8`

Формат, который распознал загрузчик. Значение `Unknown` означает, что ни
расширение, ни магия не дали ответа; `FbxLite` — распознанный, но
неподдерживаемый FBX.

| Значение | Смысл |
|---|---|
| `ModelFormat::Unknown` | формат не определён |
| `ModelFormat::Obj` | Wavefront OBJ (+ библиотеки MTL) |
| `ModelFormat::Gltf` | текстовый glTF 2.0 (JSON) |
| `ModelFormat::Glb` | бинарный GLB с чанком `BIN\0` |
| `ModelFormat::Ply` | PLY: ascii, binary little-endian, binary big-endian |
| `ModelFormat::Stl` | STL: ascii или binary |
| `ModelFormat::Vox` | MagicaVoxel `.vox` |
| `ModelFormat::FbxLite` | расширение `.fbx`; импорт не поддерживается |

```cpp
const crossrender::ModelFormat fmt = crossrender::ModelFormat::Glb;
if (fmt == crossrender::ModelFormat::Glb || fmt == crossrender::ModelFormat::Gltf) {
    ENG_LOGI("demo", "бинарный и текстовый glTF грузятся одним путём");
}
```

### `struct ModelNode`

Узел иерархии: имя, связи с родителем и детьми, локальный трансформ и ссылки
на меш и материал. Узлы хранятся плоским массивом, а не деревом указателей,
поэтому индексы стабильны и сериализуемы.

```cpp
crossrender::ModelNode node;
node.name = "wheel";
node.mesh = 2;
node.localMatrix = crossrender::Mat4::Translate(crossrender::Vec3{0.5f, 0.3f, 0.0f});
ENG_LOGI("demo", "узел '%s' ссылается на меш %d", node.name.c_str(), node.mesh);
```

### `std::string ModelNode::name`

Имя узла из файла (`node0`, `node1`, … — если в glTF имени не было). Для
отладки, логов и поиска узла по имени.

```cpp
for (int i = 0; i < model.NodeCount(); ++i) {
    ENG_LOGD("demo", "узел %d: '%s'", i, model.NodeAt(i).name.c_str());
}
```

### `int ModelNode::parent`

Индекс родителя в `Model::Nodes()` или `-1` для корня. Заполняется в конце
разбора glTF по спискам `children`.

```cpp
const crossrender::ModelNode& n = model.NodeAt(3);
if (n.parent < 0) ENG_LOGI("demo", "'%s' — корневой узел", n.name.c_str());
```

### `std::vector<int> ModelNode::children`

Индексы дочерних узлов. Обратный список по отношению к `parent`, оба
заполняются загрузчиком.

```cpp
for (int child : model.NodeAt(0).children) {
    ENG_LOGD("demo", "потомок индекса %d", child);
}
```

### `Vec3 ModelNode::translation`

Локальный сдвиг. У glTF берётся из `translation`, а если был задан `matrix` —
извлекается из её последнего столбца (`m[12..14]`).

```cpp
crossrender::ModelNode n;
n.translation = crossrender::Vec3{1.0f, 2.0f, 3.0f};
n.localMatrix = crossrender::Mat4::Translate(n.translation);
```

### `Vec3 ModelNode::rotationEuler`

Углы Эйлера для удобства приложения. Загрузчик glTF **сам их не заполняет**:
он читает кватернион (`rotation`) или матрицу, а `rotationEuler` остаётся
`{0, 0, 0}` — это поле для вашего кода и ручных правок.

```cpp
crossrender::ModelNode n;
n.rotationEuler = crossrender::Vec3{0.0f, crossrender::kPi * 0.5f, 0.0f};
```

### `Quat ModelNode::rotation`

Локальный поворот кватернионом. Для glTF — из `rotation` (нормализуется) или
восстановленный из `matrix` через `Quat::FromMat4`.

```cpp
crossrender::ModelNode n;
n.rotation = crossrender::Quat{0, 0, 0, 1};                 // без поворота
n.localMatrix = n.rotation.ToMat4();
```

### `Vec3 ModelNode::scale`

Локальный масштаб. У glTF — из `scale`; при заданной `matrix` длины столбцов
матрицы становятся компонентами масштаба.

```cpp
crossrender::ModelNode n;
n.scale = crossrender::Vec3{1.0f, 2.0f, 1.0f};
```

### `Mat4 ModelNode::localMatrix`

Готовая локальная матрица: либо из поля `matrix` файла, либо
`Translate(translation) * rotation.ToMat4() * Scale(scale)`. Именно её
перемножает вызывающий код при обходе иерархии.

```cpp
crossrender::Mat4 world = crossrender::Mat4::Identity();
for (int idx : chainFromRootToLeaf) world = world * model.NodeAt(idx).localMatrix;
```

### `int ModelNode::mesh`

Индекс меша в `Model::Meshes()` или `-1`, если узел ничего не рисует
(пустой узел-контейнер). Загрузчик проверяет диапазон и сбрасывает
некорректный индекс в `-1`.

```cpp
const crossrender::ModelNode& n = model.NodeAt(0);
if (n.mesh >= 0) ENG_LOGI("demo", "узел рисует меш %d", n.mesh);
```

### `int ModelNode::material`

Индекс материала в `Model::Materials()` — копия `materialIndex` первого
подмеша меша узла (или `0`, если подмешей нет). Это подсказка: у меша с
несколькими подмешами материалы перечислены в `MeshData::subMeshes`.

```cpp
const crossrender::ModelNode& n = model.NodeAt(0);
ENG_LOGI("demo", "узел '%s', материал %d", n.name.c_str(), n.material);
```

### `bool ModelNode::hasMatrix`

`true`, если в файле у узла была явная матрица `matrix`, а не TRS-параметры.
Нужно, чтобы понять, можно ли безопасно править `translation`/`rotation`/
`scale` (при `hasMatrix == true` они лишь извлечены из матрицы).

```cpp
const crossrender::ModelNode& n = model.NodeAt(1);
if (!n.hasMatrix) n.translation.y += 0.5f;      // TRS можно править
```

### `class Model`

Контейнер загруженного ассета: меши, материалы, узлы, текстуры, габариты и
копии мешей на GPU. Модель перемещаемая, но не копируемая — внутри лежат
`std::vector<Mesh>` с ресурсами GPU.

```cpp
crossrender::Model model;
if (!model.Load("assets/props/crate.obj")) {
    ENG_LOGE("demo", "не удалось загрузить ящик");
}
```

### `Model()`

Конструктор по умолчанию: пустые меши, материалы и узлы, `format_` равен
`ModelFormat::Unknown`, `uploaded_ == false`. Обращений к файловой системе и
OpenGL нет.

```cpp
crossrender::Model empty;
ENG_ASSERT(!empty.Valid());
ENG_ASSERT(empty.MeshCount() == 0);
```

### `~Model()`

Деструктор по умолчанию. `Mesh` внутри `gpuMeshes_` освобождают свои буферы
сами, отдельный `Destroy()` для этого не нужен.

```cpp
{
    crossrender::Model local = crossrender::Model::MakeLowPolyRock(5);
}   // буферы GPU (если были) освобождены
```

### `Model(Model&&) noexcept`

Перемещающий конструктор: переносит все векторы, габариты, флаг загрузки и
формат, оставляя источник пустым. Благодаря ему `Model` можно возвращать из
фабрик по значению и хранить в `std::vector`.

```cpp
crossrender::Model make() { return crossrender::Model::MakeLowPolyTree(1); }
std::vector<crossrender::Model> props;
props.push_back(make());
```

### `Model& operator=(Model&&) noexcept`

Перемещающее присваивание: освобождает текущее содержимое и забирает ресурсы
другого объекта.

```cpp
crossrender::Model a = crossrender::Model::MakeLowPolyTree(1);
crossrender::Model b;
b = std::move(a);          // a снова пуст, b владеет моделью
```

### `Model(const Model&) = delete`, `Model& operator=(const Model&) = delete`

Копирование запрещено: копии мешей на GPU невозможно размножить без создания
новых буферов. Передавайте модель по ссылке.

```cpp
void Draw(const crossrender::Model& model, const crossrender::Material& mat) {
    if (model.Uploaded()) model.GpuMesh(0).Draw();
}
```

### `bool Load(const std::string& path)`

Основной вход: читает файл целиком, определяет формат по расширению и магии,
импортирует геометрию, материалы и узлы, добавляет недостающие узлы и материал
по умолчанию и считает габариты. В начале вызывает `Destroy()`, поэтому модель
всегда полностью перезаписывается.

* **Возвращает:** `true`, если в модели появилась хотя бы одна геометрия.
* **Имя:** `Name()` становится `PathBase(path)` (имя файла с расширением);
  относительные `mtllib` и `uri` разрешаются относительно каталога файла.
* **Ошибки:** неизвестный формат, нечитаемый файл, `.fbx`, пустая геометрия
  или ошибка парсера логируются через `ENG_LOGE("model", ...)`.

```cpp
crossrender::Model model;
if (!model.Load("assets/models/teapot.obj")) {
    ENG_LOGE("demo", "импорт не удался: %s", model.Name().c_str());
    return;
}
ENG_LOGI("demo", "%d меш(ей), %d материал(ов)", model.MeshCount(), model.MaterialCount());
```

### `bool LoadFromMemory(const void* data, usize size, ModelFormat format, const std::string& name = "")`

Импорт из буфера — для встроенных ассетов, сетевых загрузок и тестов.
Базового каталога нет, поэтому внешние `uri` glTF недоступны; `data:`-URI и
изображения из `bufferView` работают.

* **`format`:** подсказка. При `Unknown` формат определяется по магии, а для
  пары glTF/GLB содержимое уточняет вариант.
* **`name`:** имя модели; пустое значение даёт `"memory"`.
* **Возвращает:** `true` при непустой геометрии.

```cpp
extern const char kGltfJson[];      // встроенный ассет
crossrender::Model model;
const bool ok = model.LoadFromMemory(kGltfJson, sizeof(kGltfJson) - 1,
                                     crossrender::ModelFormat::Gltf, "builtin-cube");
ENG_LOGI("demo", "загрузка из памяти: %s", ok ? "ок" : "ошибка");
```

### `void Destroy()`

Полностью очищает модель: меши, материалы, узлы, текстуры, копии GPU,
габариты, имя и формат. `Uploaded()` снова `false`. Вызывается автоматически в
начале `Load` и `LoadFromMemory`.

```cpp
model.Destroy();
ENG_ASSERT(!model.Valid());
ENG_ASSERT(model.Format() == crossrender::ModelFormat::Unknown);
```

### `static bool ImportObj(const char* text, usize size, MeshData* outMesh, std::vector<Material>* outMaterials, std::string* error)`

Прямой разбор OBJ без файловой системы: полезен в тестах и инструментах.
Материалы создаются по именам `usemtl`, но без чтения MTL — их параметры
останутся значениями по умолчанию.

* **`outMaterials`** может быть `nullptr` — тогда группы ссылаются на
  материал с индексом `0`.
* **`error`** может быть `nullptr`; иначе получает текст ошибки.
* **Возвращает:** `false` при пустом вводе, отсутствии `outMesh`, выходе
  индексов за диапазон, грани с менее чем тремя вершинами.

```cpp
const char obj[] = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
crossrender::MeshData mesh;
std::vector<crossrender::Material> mats;
std::string err;
if (!crossrender::Model::ImportObj(obj, sizeof(obj), &mesh, &mats, &err)) {
    ENG_LOGE("demo", "OBJ: %s", err.c_str());
}
```

### `static bool ImportGltf(const void* data, usize size, bool binary, std::vector<MeshData>* outMeshes, std::vector<Material>* outMaterials, std::vector<ModelNode>* outNodes, std::vector<Texture>* outTextures, std::string* error)`

Прямой разбор glTF/GLB. `binary = true` ожидает контейнер GLB, `false` —
JSON (BOM пропускается). Все выходные указатели обязаны быть непустыми, иначе
функция вернёт `false` с текстом `"null output"`.

* **Возвращает:** `false` при пустом вводе, некорректном JSON, плохом GLB
  или невалидных диапазонах аксессоров.
* **Текстуры:** декодируются тут же и требуют контекста OpenGL; без него они
  остаются невалидными, в лог идёт `ENG_LOGW("model", ...)`, но импорт
  завершается успешно.

```cpp
const char gltf[] = R"({"asset":{"version":"2.0"}})";
std::vector<crossrender::MeshData> meshes;
std::vector<crossrender::Material> mats;
std::vector<crossrender::ModelNode> nodes;
std::vector<crossrender::Texture> textures;
std::string err;
crossrender::Model::ImportGltf(gltf, sizeof(gltf), /*binary=*/false, &meshes, &mats, &nodes, &textures, &err);
```

### `static bool ImportPly(const void* data, usize size, MeshData* outMesh, std::string* error)`

Разбор PLY из памяти. Понимает ascii и оба порядка байт binary. Вершины без
нормалей получают сглаженные нормали, рассчитанные из треугольников.

```cpp
extern const unsigned char kPlyBytes[];
crossrender::MeshData mesh;
std::string err;
if (!crossrender::Model::ImportPly(kPlyBytes, sizeof(kPlyBytes), &mesh, &err)) {
    ENG_LOGE("demo", "PLY: %s", err.c_str());
}
```

### `static bool ImportStl(const void* data, usize size, MeshData* outMesh, std::string* error)`

Разбор STL из памяти, ascii или binary. Вершины сварены по квантованным
(позиция, нормаль), поэтому гранёный вид сохраняется; нормаль фасета
используется, если она не выродилась, иначе считается из геометрии.

```cpp
crossrender::MeshData mesh;
std::string err;
crossrender::Model::ImportStl(stlBytes.data(), stlBytes.size(), &mesh, &err);
ENG_LOGI("demo", "треугольников: %d", static_cast<int>(mesh.indices.size() / 3));
```

### `bool Valid() const`

`true`, если в модели есть хотя бы один меш. Самая дешёвая проверка после
`Load`/`LoadFromMemory`.

```cpp
crossrender::Model model;
model.Load("assets/models/rock.ply");
if (!model.Valid()) ENG_LOGW("demo", "модель пуста, рисовать нечего");
```

### `ModelFormat Format() const`

Формат, определённый загрузчиком. Выставляется до импорта, поэтому остаётся
осмысленным даже при неудачной загрузке.

```cpp
switch (model.Format()) {
    case crossrender::ModelFormat::Vox:   ENG_LOGI("demo", "воксельная модель"); break;
    case crossrender::ModelFormat::Gltf:  ENG_LOGI("demo", "текстовый glTF"); break;
    default:                      ENG_LOGI("demo", "другой формат"); break;
}
```

### `const std::string& Name() const`

Имя модели: `PathBase(path)` при загрузке из файла, `"memory"` при пустом
имени в `LoadFromMemory`. Используется в логах и как имя меша для форматов без
внутренних имён.

```cpp
ENG_LOGI("demo", "модель '%s'", model.Name().c_str());
```

### `int MeshCount() const`

Число мешей — верхняя граница для `MeshAt` и `GpuMesh`. У glTF один меш на
запись `meshes` (все примитивы слиты в один `MeshData`).

```cpp
for (int i = 0; i < model.MeshCount(); ++i) {
    ENG_LOGD("demo", "меш %d: %d вершин", i, static_cast<int>(model.MeshAt(i).vertices.size()));
}
```

### `int MaterialCount() const`

Число материалов — верхняя граница для `MaterialAt`. После успешной загрузки
минимум один (при необходимости добавляется `"default"`).

```cpp
ENG_LOGI("demo", "материалов: %d", model.MaterialCount());
for (int i = 0; i < model.MaterialCount(); ++i) {
    ENG_LOGD("demo", "  %d: '%s'", i, model.MaterialAt(i).name.c_str());
}
```

### `int NodeCount() const`

Число узлов иерархии — верхняя граница для `NodeAt`. Для OBJ/PLY/STL/VOX
равно числу мешей; для glTF — числу записей `nodes`.

```cpp
ENG_LOGI("demo", "узлов: %d", model.NodeCount());
```

### `const MeshData& MeshAt(int i) const`

Доступ к геометрии на CPU по индексу, без проверки диапазона. Через неё
удобно пересчитывать нормали, читать границы или клонировать меш.

```cpp
const crossrender::MeshData& md = model.MeshAt(0);
ENG_LOGI("demo", "меш '%s': %d вершин, %d индексов", md.name.c_str(),
         static_cast<int>(md.vertices.size()), static_cast<int>(md.indices.size()));
```

### `const Material& MaterialAt(int i) const`

Доступ к материалу по индексу, без проверки диапазона. Указатели на текстуры
внутри валидны, пока жив объект `Model`.

```cpp
const crossrender::Material& m = model.MaterialAt(0);
crossrender::Material copy = m;               // копия параметров, указатели те же
copy.baseColor = crossrender::Color::White;
```

### `const ModelNode& NodeAt(int i) const`

Доступ к узлу по индексу, без проверки диапазона. Индексы совпадают с
порядком в файле.

```cpp
const crossrender::ModelNode& root = model.NodeAt(0);
ENG_LOGI("demo", "корень '%s', детей %d", root.name.c_str(),
         static_cast<int>(root.children.size()));
```

### `std::vector<ModelNode>& Nodes()`

Изменяемый список узлов — единственный способ поправить иерархию: перевесить
родителя, добавить узел, заменить матрицу. После правок пересчитайте габариты
через `BakeTransform` (он обновляет `Bounds()`) или считайте их сами.

```cpp
std::vector<crossrender::ModelNode>& nodes = model.Nodes();
if (!nodes.empty()) {
    nodes[0].translation = crossrender::Vec3{0, 1, 0};
    nodes[0].localMatrix = crossrender::Mat4::Translate(nodes[0].translation);
}
```

### `std::vector<Material>& Materials()`

Изменяемый список материалов: подмена текстур, тюнинг `roughness`/`metallic`,
включение `vertexColors`. Индексы, на которые ссылаются подмеши, не меняйте
без пересборки `subMeshes`.

```cpp
std::vector<crossrender::Material>& mats = model.Materials();
for (crossrender::Material& m : mats) {
    m.receiveShadows = true;
    m.uvScale = {2.0f, 2.0f};
}
```

### `std::vector<MeshData>& Meshes()`

Изменяемая геометрия на CPU. Правки после `UploadToGpu` не попадут на GPU,
пока вы не сбросите копии (например, через `Destroy()` + повторный
`UploadToGpu`).

```cpp
std::vector<crossrender::MeshData>& meshes = model.Meshes();
for (crossrender::MeshData& md : meshes) {
    md.ComputeNormals(true);
    md.ComputeBounds();
}
```

### `std::vector<Texture>& Textures()`

Текстуры, загруженные импортёром (OBJ и glTF; VOX добавляет палитровую).
Вектор стабилен после импорта, поэтому указатели в материалах остаются
валидными.

```cpp
std::vector<crossrender::Texture>& textures = model.Textures();
for (crossrender::Texture& t : textures) {
    ENG_LOGD("demo", "текстура %dx%d, valid=%d", t.Width(), t.Height(), t.Valid() ? 1 : 0);
}
```

### `const Bounds& Bounds() const`

Габариты модели с учётом трансформов узлов (AABB, axis-aligned bounding box).
Если узлов нет, объединяются границы всех мешей. По ним удобно ставить камеру
и считать масштаб.

```cpp
const crossrender::Bounds& b = model.Bounds();
if (b.Valid()) {
    crossrender::Camera cam;
    cam.target = b.Center();
    cam.position = b.Center() + crossrender::Vec3{0, 0, b.Radius() * 2.5f};
}
```

### `void UploadToGpu()`

Создаёт по одному `Mesh` на каждый `MeshData` и заполняет `gpuMeshes_` в том
же порядке. Идемпотентна: пока `Uploaded() == true`, повторные вызовы ничего
не делают. Без контекста OpenGL каждый `Mesh::Create` тихо возвращает `false`,
`uploaded_` остаётся `false`, а в лог идёт `ENG_LOGW("model", ...)`.

```cpp
crossrender::Model model = crossrender::Model::MakeLowPolyCharacter();
model.UploadToGpu();
if (!model.Uploaded()) ENG_LOGW("demo", "GPU-загрузка не удалась (нет контекста?)");
```

### `bool Uploaded() const`

`true`, если **все** меши успешно загружены на GPU. При частичном успехе
(`gpuMeshes_` заполнен не полностью) возвращает `false` — проверяйте перед
рисованием.

```cpp
if (model.Uploaded()) {
    for (int i = 0; i < model.MeshCount(); ++i) model.GpuMesh(i).Draw();
}
```

### `const Mesh& GpuMesh(int i) const`

Копия меша на GPU по индексу (без проверки диапазона). Индексы совпадают с
`Meshes()`. Возвращённый `Mesh` нельзя копировать, но можно рисовать и читать
его границы и подмеши.

```cpp
const crossrender::Mesh& gpu = model.GpuMesh(0);
ENG_LOGI("demo", "на GPU %u вершин, %u индексов", gpu.VertexCount(), gpu.IndexCount());
```

### `void BakeTransform(const Mat4& m)`

Запекает общий трансформ во все вершины всех мешей: позиции умножаются на
`m`, нормали и касательные — на нормальную матрицу. Габариты мешей
пересчитываются, габариты модели считаются заново с учётом узлов, а копии GPU
сбрасываются (`gpuMeshes_.clear()`, `Uploaded() == false`) — после выпекания
нужен повторный `UploadToGpu`. Так приводят Z-up модель к Y-up или переводят
её из сантиметров в метры.

```cpp
crossrender::Model model = crossrender::Model::MakeLowPolyRock(4);
model.BakeTransform(crossrender::Mat4::RotateX(-crossrender::kPi * 0.5f));   // Z-up -> Y-up
model.UploadToGpu();
```

### `void EnsureNormals()`

Достраивает нормали только там, где они нулевые: сначала пробует
`MeshData::ComputeNormals(true)` для мешей с индексами, затем локальный проход
по треугольникам. Уже корректные нормали не трогает, поэтому плоские
(faceted) модели не «заглаживаются».

```cpp
crossrender::Model model;
model.Load("assets/models/scan.stl");
model.EnsureNormals();               // у части вершин нормалей не было
```

### `static Model MakeLowPolyTree(u64 seed = 1)`

Встроенный низкополигональный дуб: ствол-цилиндр и три яруса кроны (конусы и
икосаэдр сверху). Геометрия детерминирована по `seed` (генератор `crossrender::Random`),
материалы — `"bark"`, `"leaf_dark"`, `"leaf_light"`. `Format()` остаётся
`Unknown`, `Name()` — `"lowpoly_tree"`.

```cpp
crossrender::Model tree = crossrender::Model::MakeLowPolyTree(7);
ENG_LOGI("demo", "'%s': %d вершин", tree.Name().c_str(),
         static_cast<int>(tree.MeshAt(0).vertices.size()));
```

### `static Model MakeLowPolyRock(u64 seed = 2)`

Встроенный валун: один-два икосаэдра с детерминированным дрожанием вершин,
материалы `"rock"` и `"moss"`. После сборки модель сдвигается так, чтобы её
нижняя точка легла на `y = 0` — удобно как наземный проп.

```cpp
crossrender::Model rock = crossrender::Model::MakeLowPolyRock(11);
ENG_LOGI("demo", "камень: min.y = %.2f", rock.Bounds().min.y);
```

### `static Model MakeLowPolyCrystal(u64 seed = 3)`

Встроенный кристалл: несколько бипирамид (пара конусов), материалы
`"crystal_core"` и `"crystal_shard"` с эмиссией (`emissive` и
`emissiveStrength`), поэтому хорошо смотрится с bloom.

```cpp
crossrender::Model crystal = crossrender::Model::MakeLowPolyCrystal(13);
crossrender::Material glowing = crystal.MaterialAt(0);
ENG_LOGI("demo", "эмиссия: %.2f", glowing.emissiveStrength);
```

### `static Model MakeLowPolyCharacter()`

Встроенный низкополигональный персонаж: ноги, торс, руки, голова, нос и
«шапка» волос, материалы `"skin"`, `"shirt"`, `"pants"`, `"hair"`. Меш один,
подмеши разделяют материалы; сид фиксирован, поэтому модель одинакова при
каждом вызове.

```cpp
crossrender::Model hero = crossrender::Model::MakeLowPolyCharacter();
ENG_LOGI("demo", "персонаж: %d групп материалов", hero.MeshAt(0).subMeshes.size());
```

## Пример целиком

```cpp
#include "crossrender/assets/Model.h"
#include "crossrender/core/Log.h"
#include "crossrender/gfx/Renderer3D.h"

// Загружает ассет, при необходимости чинит нормали, печатает отчёт и рисует
// модель с учётом иерархии узлов.
bool LoadAndDraw(crossrender::Renderer3D& r, const std::string& path) {
    crossrender::Model model;
    if (!model.Load(path)) {
        ENG_LOGE("assets", "модель '%s' не загрузилась", path.c_str());
        return false;
    }

    // PLY/STL часто приходят без нормалей.
    model.EnsureNormals();

    // Модели в сантиметрах приводим к метрам: печём масштаб в вершины.
    if (model.Bounds().Valid() && model.Bounds().Radius() > 100.0f) {
        model.BakeTransform(crossrender::Mat4::Scale(crossrender::Vec3{0.01f, 0.01f, 0.01f}));
    }

    model.UploadToGpu();
    if (!model.Uploaded()) {
        ENG_LOGW("assets", "'%s' без GPU: рисование пропущено", model.Name().c_str());
        return false;
    }

    ENG_LOGI("assets", "'%s' (%d): %d меш(ей), %d материал(ов), %d узел(ов)", model.Name().c_str(),
             static_cast<int>(model.Format()), model.MeshCount(), model.MaterialCount(),
             model.NodeCount());

    // Обход плоского списка узлов: накапливаем мировую матрицу от корня.
    std::vector<crossrender::Mat4> world(model.Nodes().size(), crossrender::Mat4::Identity());
    std::vector<crossrender::Mat4>& nodes = model.Nodes();
    for (crossrender::usize i = 0; i < nodes.size(); ++i) {
        const crossrender::ModelNode& node = nodes[i];
        if (node.parent >= 0 && static_cast<crossrender::usize>(node.parent) < i) {
            world[i] = world[static_cast<crossrender::usize>(node.parent)] * node.localMatrix;
        } else {
            world[i] = node.localMatrix;
        }
        if (node.mesh < 0) continue;
        const crossrender::Mesh& gpu = model.GpuMesh(node.mesh);
        const crossrender::Material& mat = model.MaterialAt(node.material < 0 ? 0 : node.material);
        r.Draw(gpu, mat, world[i]);
    }
    return true;
}
```

## См. также

* `docs/gfx/Mesh.md` — `MeshData`, `Material`, `Mesh`, `Bounds` и камера,
  которые использует контейнер.
* `docs/core/File.md` — чтение файлов и `ReadBinaryFile`, на котором построен
  `Load`.
* `docs/core/Log.md` — макросы, которыми загрузчик сообщает о проблемах.
* `docs/voxel/Voxel.md` — воксельный мир, который тоже умеет отдавать
  `MeshData` (и из `.vox`, и из чанков).
