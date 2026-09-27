# crossrender/voxel/Voxel.h — воксельный мир, чанки и меширование

Воксельный модуль: плотные чанки 32×32×32, разреженная карта чанков, палитра
материалов, greedy meshing (жадное слияние граней) с ambient occlusion и
запечённым светом, генераторы ландшафта, палитровый атлас, трассировка луча по
сетке и режим raymarching по 3D-текстуре.

## Заголовок

```cpp
#include "crossrender/voxel/Voxel.h"
```

## Обзор

Модуль состоит из трёх слоёв: **данные** (`VoxelChunk`), **мир**
(`VoxelWorld`) и **представление** (`MeshData` + `Mesh`).

#### Хранение: чанк, воксель, координаты

* Воксель описывается структурой `Voxel` (`id`, `light`, `ao`, `flags`), но
  `VoxelChunk` хранит **два параллельных массива** `u8` по
  `kVoxelChunkVolume = 32 * 32 * 32 = 32768` байт: идентификаторы палитры и
  уровни света. Поля `ao` и `flags` из `Voxel` нигде не сохраняются — AO
  считается мешером на лету.
* Порядок укладки документирован в начале `Voxel.cpp`:
  `index = x + z * 32 + y * 32 * 32`, то есть быстрее всех меняется `x`, затем
  `z`, затем `y`. Индексы вне `[0, 32)` не ошибка: чтение отдаёт «воздух»
  (id `0`) и «полный свет» (15), запись игнорируется.
* Чанки адресуются `ChunkCoord` (в чанках) и лежат в
  `std::unordered_map<ChunkCoord, std::unique_ptr<VoxelChunk>, ChunkCoordHash>`,
  то есть мир **разреженный**: существуют только явно созданные чанки.
  `GetChunk` ничего не создаёт, `GetOrCreateChunk` создаёт и сразу прописывает
  в новый чанк его координаты и обратный указатель `VoxelChunk::world`.
* Мировые координаты знаковые и переводятся в чанк/локальные через
  `FloorDiv`/`FloorMod`, поэтому `-1` попадает в чанк `-1` (локальная
  координата 31), а `-33` — в чанк `-2`. Обычное деление C++ с усечением тут
  не используется.
* Мировые единицы равны вокселям: один воксель — 1.0, чанк — 32.0,
  `WorldOrigin()` = `Coord() * 32`. Меш чанка строится в **локальных**
  координатах `[0, 32)`, а `VoxelWorld::Render` добавляет перенос
  `Mat4::Translate(WorldOrigin())`.
* `VoxelWorld::GetVoxel` для несуществующего чанка возвращает `0` — «воздух».
  Это важно для меширования (см. проблему соседей ниже).

#### Палитра

Индекс `0` — всегда пустота (воздух). `VoxelPalette::Default()` заполняет 23
записи: 0 воздух, 1 камень, 2 трава, 3 земля, 4 бок травы, 5 песок, 6 дерево,
7 листва, 8 вода, 9 стекло, 10 кирпич, 11 снег, 12 лёд, 13 лава, 14 булыжник,
15 доски, 16 гравий, 17 обсидиан, 18 светокамень, 19 металл, 20 золото,
21 красная шерсть, 22 синяя шерсть. У воздуха альфа 0, у воды 0.72, у стекла
0.30, у льда 0.78 — именно по этим альфам мешер решает, какие грани не
скрывают друг друга.

* `Get(id)` для id вне вектора возвращает `Color::Magenta` (заметная ошибка
  вместо падения).
* `Set(id, c)` при необходимости расширяет вектор до `id + 1`, заполняя
  промежуток белым.
* `VoxelPalette::Ice()` — тот же набор, но с холодными цветами (камень, трава,
  земля, песок, вода, листва, дерево, лёд).

Палитра задаёт и цвет вершин (режим без текстуры), и содержимое палитрового
атласа (режим с текстурой), и материальное различие прозрачности.

#### Меширование

`VoxelChunk::RebuildMesh` — классический greedy mesher. Для каждой из трёх осей
и каждого знака он проходит слои вдоль оси, строит двумерную маску
видимых граней и сливает одинаковые соседние ячейки в прямоугольники
(сначала по `u`, затем по `v`). Итог — `MeshData` с квадами по 4 вершины и
6 индексов; квады ориентированы против часовой стрелки снаружи, чтобы
работало отсечение задних граней.

Что именно делает каждый флаг мешинга:

| Флаг | Что меняет |
|---|---|
| `greedy` | `true` — слияние копланарных граней одного id в прямоугольники; `false` — по кваду на воксель |
| `textured` | `true` — UV указывают в ячейку палитрового атласа (16×16), цвет вершины становится белым, умноженным на затенение; `false` — UV остаются `0..1` внутри квада, а цвет вершины берётся из палитры |
| `ambientOcclusion` | считает AO в каждом углу грани (3 выборки), пишет его в `uv2.x`, домножает цвет вершины на `0.45 + 0.55 * ao` и перекидывает диагональ квада, чтобы не было анизотропии |
| `bakeLight` | берёт уровень света из `lights_` вокселя грани, пишет `0.15 + 0.85 * light / 15` в цвет вершины, а `light / 15` — в `uv2.y`; `false` — свет не учитывается |
| `cullInterior` | `false` — **каждая** грань считается видимой, внутренние грани тоже; `true` — обычное отсечение |

Отсечение грани (`faceVisible`) устроено так: сосед-воздух — грань видна;
`cullInterior == false` — видна всегда; сосед с тем же id — скрыта; сосед с
другим id — видна только если он прозрачный (альфа палитры `< 0.99`). Поэтому
вода и стекло не «съедают» грани друг друга, а одинаковые блоки внутри массива
не генерируют геометрию.

Важные детали, которые видно только в реализации:

* `greedy`/`textured`/`ao` приходят аргументами, а `bakeLight` и `cullInterior`
  мешер читает **из `Options()` владельца** (`owner->Options()`), а не из
  аргументов; при `owner == nullptr` оба считаются `true`.
* UV текстурированного квада растягивают **всю** ячейку атласа на весь
  слитый прямоугольник: повтор воксельной текстуры внутри квада не делается,
  потому что это требует `fract()` во фрагментном шейдере, а форвардный шейдер
  движка его не делает.
* Трава (id 2) на боковых гранях использует ячейку 4, а на верхней/нижней —
  ячейку 2 (функция `FaceTextureId`).
* `RebuildMesh` только строит геометрию на CPU; загрузка на GPU — отдельный
  `UploadMesh`.
* У пустого чанка (`Empty() == true`) меш очищается, `subMeshes` сбрасывается,
  а существующий GPU-меш уничтожается.

**Когда чанк перестраивается.** `VoxelChunk::Set`, `Fill` и `Clear` ставят
`dirty_ = true`. `VoxelWorld::SetVoxel`, записавший воксель на границе чанка
(`lx == 0`, `lx == 31`, …), дополнительно помечает грязными **уже
существующие** соседние чанки — их отсечение зависит от этого вокселя.
Реальное построение делает `VoxelWorld::UpdateMeshes`: он собирает все грязные
чанки, вызывает для каждого `RebuildMesh` с текущими опциями палитры и
`UploadMesh`. `Render` ничего не перестраивает. Автоматики нет: между
`SetVoxel` и `UpdateMeshes` меш устаревший.

#### Палитровый атлас

`BuildPaletteAtlas(cellSize)` собирает текстуру 16×16 ячеек (размер ячейки
зажимается в `4..128`). Каждая ячейка — процедурный узор из цвета палитры:
шум, кирпичная кладка, кольца дерева, кромка и блик стекла, волны воды и т. д.;
альфа берётся из палитры. `PaletteUV(id)` возвращает `Rect` ячейки в
нормированных координатах (`x = (id % 16) / 16`, `y = (id / 16) / 16`,
размер `1/16`). Мешер жёстко рассчитывает на сетку 16×16 и размер ячейки
`1/16`, поэтому собственная текстура-атлас должна сохранять эту раскладку.
Атлас требует контекста OpenGL: без него текстура остаётся невалидной.

#### Трассировка луча

`VoxelWorld::Raycast` — алгоритм DDA (Amanatides & Woo). Направление
нормализуется; вырожденное (`LengthSq < 0.5`) или `maxDistance <= 0` сразу
дают промах. Луч стартует в вокселе, содержащем `origin`, и шагает по сетке,
всегда переходя через ближайшую границу (`tMaxX/Y/Z`). Поля `VoxelRayHit`:

* `hit` — было ли попадание; промах возвращает значение по умолчанию
  (`hit = false`, остальные поля нули);
* `x`, `y`, `z` — воксель, в который попал луч;
* `nx`, `ny`, `nz` — нормаль грани, через которую луч вошёл (это `-step` по
  оси перехода). Если `origin` уже внутри твёрдого вокселя, попадание
  происходит на `distance = 0` с нулевой нормалью — вызывающий код должен это
  учитывать;
* `distance` — параметр луча `t` в момент входа в воксель (в мировых
  единицах, так как направление нормализовано);
* `id` — идентификатор палитры попавшего вокселя.

Твёрдым считается **любой** ненулевой id, поэтому вода и стекло тоже
останавливают луч. Обход прерывается, когда `t > maxDistance`; жёсткий
предохранитель — 100 000 шагов. Пустые (не созданные) чанки для луча — воздух,
поэтому луч может «прошить» границу загруженной области.

#### Проблема соседей на краю загруженной области

Мешер отсекает грань, только если видит соседний воксель. Внутри чанка он
берёт его из `voxels_`, а на границе — через `owner->GetVoxel(...)`. Но
`GetVoxel` возвращает `0` и когда в соседнем чанке действительно воздух, и
когда **чанка просто нет**. Различить эти случаи мешер не может, а
`faceVisible(id, 0) == true`, поэтому у вокселя на границе загруженной области
вырастает лицевая грань — сплошная **стена** по периметру загруженного
прямоугольника.

Это не теоретическая тонкость: сцена-стриминг (пример `GameMinecraft`)
реализует вокруг игрока всего 2×2 чанка и мешит их по мере появления. Чанк,
отмешированный до того, как появился его сосед, получает стену с этой стороны;
внутренние грани между двумя уже существующими чанками отсекаются корректно
(тест `Voxel.BorderFaceCulling`: пара чанков даёт по 5 квадов, одиночный — 6).

Что с этим делать:

* создавать (или хотя бы заполнять) соседей **до** `UpdateMeshes` — тогда
  границы отсекутся сразу;
* помнить, что `GetOrCreateChunk` сам по себе не помечает соседей грязными:
  после появления нового чанка рядом с уже отмешированным нужно вызвать
  `MarkAllDirty()` или `MarkDirty()` у затронутых чанков и повторить
  `UpdateMeshes` (запись через `SetVoxel` на границе помечает соседей, но
  только существующих);
* либо принять стену как видимую границу мира — например, прятать её туманом,
  как это делают сцены-демо.

#### Честные ограничения

* **Свет запекается на CPU и не пересчитывается автоматически.** После
  генерации нужно вызвать `ComputeLighting`, а после правок — вызвать его
  снова. `SetVoxel` по умолчанию записывает свет `15`, поэтому поставленный
  блок светится, пока вы не пересчитаете освещение, а удалённый оставляет
  старое значение в `lights_`.
* **Параметр `sunHeight` у `ComputeLighting` игнорируется** (`(void)sunHeight`):
  вертикальный проход всегда идёт сверху вниз по существующим чанкам, а чанк
  без чанка сверху считается открытым небом. `ComputeLighting` никогда не
  создаёт чанки, поэтому свет не «дотянется» до ещё не загруженной области.
* **Меши перестраиваются синхронно и жадно.** `UpdateMeshes` в один вызов
  перестраивает и загружает **все** грязные чанки; нет фоновой очереди и
  разбиения по кадрам. Приложение, которое хочет держать бюджет кадра, само
  копит правки и вызывает `UpdateMeshes` не чаще раза в кадр (так сделано в
  `GameMinecraft`: флаг `pendingRebuild_`). Полная пересборка острова радиуса 4
  занимает десятки миллисекунд.
* **Прозрачность не сортируется.** `Render` обходит
  `std::unordered_map`, то есть порядок отрисовки чанков не определён; вода и
  стекло рисуются в том же проходе, что и камень, — корректного смешивания
  «сзади вперёд» нет.
* **`textured` без атласа и без GL ничего не покажет**: `Render` строит атлас
  лениво, и без контекста `PaletteAtlas()` останется невалидной, а меш — с
  UV в никуда.
* **Единый проход без материалов на блок**: у всех вокселей один `Material`;
  разные `alphaMode`/`cullMode` на разные id не предусмотрены.
* **AO и свет — приближения**: AO считается по трём соседям на угол
  (классическая схема), свет — линейное затухание 1/2/4 на шаг, без цветного
  света и без распространения «вверх» из полости.
* **`RemoveChunk` не пересчитывает габариты**, а `RecomputeBounds` описывает
  чанки целиком (AABB по 32³), а не фактические границы вокселей.
* **`Raycast` не различает материалы**: любой ненулевой id твёрдый; флагов
  «проходимости» у вокселя нет.
* **Режим raymarching — отдельная ветка**: он игнорирует палитровый атлас и
  AO, зажимает объём 3D-текстуры до 384 по оси, а его шейдер и коробка живут
  в статике до конца процесса и никогда не освобождаются.

## Члены класса

### `constexpr int kVoxelChunkSize`, `constexpr int kVoxelChunkVolume`

Размер чанка и его объём в вокселях. Это фундаментальные константы модуля: от
них зависят и раскладка памяти, и `WorldOrigin`, и локальные координаты в
`VoxelChunk::Set`/`Get`.

| Константа | Значение | Смысл |
|---|---|---|
| `kVoxelChunkSize` | `32` | длина ребра чанка по каждой оси |
| `kVoxelChunkVolume` | `32768` | `32 * 32 * 32`, размер плотного массива чанка |

```cpp
const int bytesPerChunk = crossrender::kVoxelChunkVolume;          // 32768 байт на id
ENG_LOGI("demo", "чанк %d^3, объём %d", crossrender::kVoxelChunkSize, bytesPerChunk);
```

### `struct Voxel`

Описание одного вокселя: идентификатор палитры, уровень света, ambient
occlusion и произвольные флаги. Это «логическая» запись вокселя; внутри
`VoxelChunk` идентификатор и свет лежат раздельно, а `ao` и `flags` не
хранятся вовсе.

```cpp
crossrender::Voxel v;                  // по умолчанию: id 0 (воздух), light 0, ao 255
v.id = 8;                      // вода
v.light = 12;
ENG_LOGI("demo", "воксель id=%u light=%u", v.id, v.light);
```

### `u8 Voxel::id`

Индекс в `VoxelPalette`: `0` — пусто, `1..255` — материал. Именно это значение
хранит и возвращает `VoxelChunk`.

```cpp
crossrender::Voxel v;
v.id = 0;                      // воздух
if (v.id == 0) ENG_LOGI("demo", "пустой воксель");
```

### `u8 Voxel::light`

Запечённый уровень света `0..15` (небо плюс рассеянный свет блоков). В
`VoxelChunk` это отдельный массив, который заполняет `ComputeLighting`, а
читает мешер при `bakeLight`.

```cpp
crossrender::Voxel v;
v.light = 15;                  // открытая поверхность под небом
ENG_LOGI("demo", "свет: %u/15", v.light);
```

### `u8 Voxel::ao`

Ambient occlusion `0..255`. Поле описательное: `VoxelChunk` его не хранит, а
мешер считает AO из окружения и кладёт результат в `uv2.x` вершины.

```cpp
crossrender::Voxel v;
v.ao = 255;                    // полностью открыт
const float ao01 = static_cast<float>(v.ao) / 255.0f;
```

### `u8 Voxel::flags`

Свободное поле под пользовательские биты (например, «механизм включён»). Ни
чанк, ни мешер, ни генераторы его не читают — это место для ваших данных.

```cpp
enum : crossrender::u8 { kVoxelFlagPowered = 1u << 0 };
crossrender::Voxel v;
v.flags |= kVoxelFlagPowered;
```

### `struct VoxelPalette`

Соответствие «индекс → цвет». Палитра невладеющая по смыслу: она просто
вектор цветов, который передаётся в мешер и в построитель атласа. Один и тот
же id во всех чанках мира означает один и тот же материал.

```cpp
crossrender::VoxelPalette p = crossrender::VoxelPalette::Default();
ENG_LOGI("demo", "палитра: %d записей", static_cast<int>(p.colors.size()));
```

### `std::vector<Color> VoxelPalette::colors`

Цвета по индексам. `colors[0]` — воздух с нулевой альфой. Вектор можно
свободно читать и править: альфа управляет и прозрачностью при отрисовке, и
решением мешера о перекрытии граней.

```cpp
crossrender::VoxelPalette p;
p.colors.push_back(crossrender::Color{0, 0, 0, 0});          // 0: воздух
p.colors.push_back(crossrender::Color::FromRGB(0x8A8A90));   // 1: камень
ENG_LOGI("demo", "камней в палитре: %d", static_cast<int>(p.colors.size() - 1));
```

### `VoxelPalette()`

Конструктор по умолчанию создаёт 23 записи белого цвета и делает запись `0`
полностью прозрачной. Это «пустая» палитра-заготовка: чтобы получить
нормальные цвета, берите `Default()` или `Ice()`.

```cpp
crossrender::VoxelPalette p;
ENG_ASSERT(p.colors.size() == 23);
ENG_ASSERT(p.Get(0).a == 0.0f);
```

### `Color VoxelPalette::Get(u8 id) const`

Возвращает цвет материала. Для id за пределами вектора — `Color::Magenta`,
то есть заметный «missing texture» вместо выхода за границы.

* **`id`:** индекс палитры; `Get(0)` — воздух.

```cpp
const crossrender::VoxelPalette p = crossrender::VoxelPalette::Default();
const crossrender::Color grass = p.Get(2);
const crossrender::Color missing = p.Get(200);      // magenta
ENG_LOGI("demo", "трава %.2f %.2f %.2f, неизвестный id %.0f", grass.r, grass.g, grass.b,
         missing.r);
```

### `void VoxelPalette::Set(u8 id, const Color& c)`

Задаёт цвет. Если `id` больше текущего размера, вектор расширяется до
`id + 1` с заполнением белым; существующие записи не сдвигаются.

```cpp
crossrender::VoxelPalette p = crossrender::VoxelPalette::Default();
p.Set(40, crossrender::Color::FromRGB(0x00FFCC).WithAlpha(0.9f));
ENG_LOGI("demo", "записей после Set: %d", static_cast<int>(p.colors.size()));
```

### `static VoxelPalette VoxelPalette::Default()`

Пресет «день»: 23 материала, на которые рассчитывают генераторы
(`GenerateFlat` по умолчанию кладёт траву id 2 поверх земли id 3). Воздух
прозрачный, вода и стекло полупрозрачные, остальное непрозрачное.

```cpp
crossrender::VoxelPalette p = crossrender::VoxelPalette::Default();
ENG_LOGI("demo", "вода alpha=%.2f, стекло alpha=%.2f", p.Get(8).a, p.Get(9).a);
```

### `static VoxelPalette VoxelPalette::Ice()`

Пресет «лёд/ночь»: те же индексы, но холодные цвета камня, травы, земли,
песка, дерева, листвы, воды и льда. Удобен как второй биом без изменения
генераторов.

```cpp
crossrender::VoxelWorld world;
world.SetPalette(crossrender::VoxelPalette::Ice());
world.BuildPaletteAtlas(16);        // цвета запекаются в атлас
```

### `struct ChunkCoord`

Координата чанка в чанках (не в вокселях). Служит и ключом в разреженной
карте мира, и хранилищем координат внутри `VoxelChunk`.

Поля `x`, `y`, `z` сгруппированы в одну таблицу по правилу стандарта для
структур-параметров; пример один на всю группу.

| Поле | Смысл |
|---|---|
| `int x` | координата чанка по X |
| `int y` | координата чанка по Y (вертикаль) |
| `int z` | координата чанка по Z |

```cpp
crossrender::ChunkCoord c{2, 0, -1};
ENG_LOGI("demo", "чанк (%d, %d, %d), мировой минимум %.0f %.0f %.0f", c.x, c.y, c.z,
         static_cast<float>(c.x * crossrender::kVoxelChunkSize), static_cast<float>(c.y * crossrender::kVoxelChunkSize),
         static_cast<float>(c.z * crossrender::kVoxelChunkSize));
```

### `bool ChunkCoord::operator==(const ChunkCoord& o) const`

Поэлементное сравнение — нужно и для поиска в контейнерах, и для проверки
«воксель всё ещё в том же чанке».

```cpp
const crossrender::ChunkCoord a{1, 0, -2};
const crossrender::ChunkCoord b{1, 0, -2};
if (a == b) ENG_LOGI("demo", "это один и тот же чанк");
```

### `struct ChunkCoordHash`

Функтор хеширования для `std::unordered_map`. Смешивает три координаты
константами `73856093`, `19349663`, `83492791` (классический тройной хеш) и
работает с отрицательными значениями за счёт приведения к `usize`.

```cpp
crossrender::ChunkCoordHash hash;
const crossrender::usize h = hash(crossrender::ChunkCoord{-3, 1, 7});
ENG_LOGI("demo", "хеш чанка: %zu", h);
```

### `usize ChunkCoordHash::operator()(const ChunkCoord& c) const`

Собственно хеш-функция. Используется как третий параметр шаблона карты
чанков; вручную вызывать нужно редко — в основном в своих контейнерах.

```cpp
std::unordered_map<crossrender::ChunkCoord, int, crossrender::ChunkCoordHash> flags;
flags[crossrender::ChunkCoord{0, 0, 0}] = 1;
ENG_LOGI("demo", "чанков в карте: %d", static_cast<int>(flags.size()));
```

### `class VoxelChunk`

Плотный блок 32³ с ленивой копией меша на GPU. Чанк не знает, где он
находится, пока ему не задали `Coord`, и не умеет искать соседей без
`world`/`owner`. Именно чанк — единица хранения, меширования и отрисовки.

```cpp
crossrender::VoxelChunk chunk;
chunk.SetCoord(crossrender::ChunkCoord{0, 0, 0});
chunk.Set(1, 1, 1, 1);
ENG_LOGI("demo", "твёрдых вокселей: %d", chunk.SolidCount());
```

### `VoxelChunk()`

Создаёт пустой чанк: `voxels_` заполнен 32768 нулями (воздух), массив света
ещё не выделен, `dirty_ == true`, `meshValid_ == false`, `world == nullptr`.

```cpp
crossrender::VoxelChunk chunk;
ENG_ASSERT(chunk.Empty());
ENG_ASSERT(chunk.Dirty());
```

### `void VoxelChunk::Set(int x, int y, int z, u8 id, u8 light = 15)`

Записывает воксель в локальных координатах `0..31`. Выход за диапазон молча
игнорируется. Счётчик `SolidCount()` меняется только при переходе
«воздух ↔ не воздух», а `dirty_` ставится при любом реальном изменении
(id или света), поэтому повторная запись того же значения ничего не портит.

* **`light`:** уровень света `0..15`; по умолчанию 15.

```cpp
crossrender::VoxelChunk chunk;
chunk.Set(0, 0, 0, 1);            // камень у самой границы чанка
chunk.Set(31, 31, 31, 9, 4);      // стекло в дальнем углу, свет 4
ENG_LOGI("demo", "твёрдых: %d", chunk.SolidCount());
```

### `u8 VoxelChunk::Get(int x, int y, int z) const`

Возвращает id материала. Координаты вне `0..31` дают `0` (воздух), поэтому
функция безопасна в циклах с выходом за границу — но помните, что это не
«сосед из другого чанка», а именно воздух.

```cpp
const crossrender::u8 id = chunk.Get(5, 5, 5);
if (id == 0) ENG_LOGI("demo", "в (5,5,5) пусто");
```

### `u8 VoxelChunk::GetLight(int x, int y, int z) const`

Возвращает запечённый свет `0..15`. Пока массив света не создан (чанк не
трогали), а также вне диапазона возвращается 15 — «полный свет».

```cpp
const crossrender::u8 light = chunk.GetLight(5, 5, 5);
ENG_LOGI("demo", "свет в (5,5,5): %u", light);
```

### `bool VoxelChunk::IsEmpty(int x, int y, int z) const`

`true`, если в ячейке воздух (`Get(...) == 0`). Короткая замена сравнению с
нулём.

```cpp
if (chunk.IsEmpty(5, 5, 5)) chunk.Set(5, 5, 5, 1);
```

### `bool VoxelChunk::IsSolid(int x, int y, int z) const`

`true`, если в ячейке любой ненулевой id. «Твёрдость» здесь означает лишь
«не воздух»: вода и стекло тоже твёрдые.

```cpp
for (int y = 0; y < crossrender::kVoxelChunkSize; ++y) {
    if (chunk.IsSolid(8, y, 8)) ENG_LOGD("demo", "столб твёрд на y=%d", y);
}
```

### `void VoxelChunk::Fill(u8 id)`

Заливает весь чанк одним материалом, сбрасывает свет в 15, обновляет
`SolidCount()` и ставит `dirty_`. `Fill(0)` эквивалентен `Clear()`.

```cpp
crossrender::VoxelChunk chunk;
chunk.Fill(1);                                  // сплошной камень
ENG_ASSERT(chunk.SolidCount() == crossrender::kVoxelChunkVolume);
```

### `void VoxelChunk::Clear()`

Полностью очищает чанк (`Fill(0)`): все воксели воздух, свет 15,
`SolidCount() == 0`.

```cpp
chunk.Clear();
ENG_ASSERT(chunk.Empty());
```

### `ChunkCoord VoxelChunk::Coord() const`

Координата чанка в мире. По умолчанию `{0, 0, 0}`, у чанков из
`VoxelWorld::GetOrCreateChunk` заполняется автоматически.

```cpp
const crossrender::ChunkCoord c = chunk.Coord();
ENG_LOGI("demo", "чанк (%d, %d, %d)", c.x, c.y, c.z);
```

### `void VoxelChunk::SetCoord(const ChunkCoord& c)`

Задаёт координату вручную — нужно, если чанк создаётся не через мир.
`WorldOrigin()` и выборка соседей при мешировании начнут считаться от неё.

```cpp
crossrender::VoxelChunk chunk;
chunk.SetCoord(crossrender::ChunkCoord{-2, 0, 3});
ENG_LOGI("demo", "начало чанка: %.0f %.0f %.0f", chunk.WorldOrigin().x, chunk.WorldOrigin().y,
         chunk.WorldOrigin().z);
```

### `Vec3 VoxelChunk::WorldOrigin() const`

Мировая позиция угла чанка с минимальными координатами:
`Coord() * kVoxelChunkSize`. Ровно этот сдвиг `Render` добавляет к локальному
мешу.

```cpp
const crossrender::Vec3 o = chunk.WorldOrigin();
ENG_LOGI("demo", "чанк начинается в (%.0f, %.0f, %.0f)", o.x, o.y, o.z);
```

### `bool VoxelChunk::Dirty() const`

`true`, если меш устарел и чанк нужно перестроить. Ставится в `Set`, `Fill`,
`Clear`, `MarkDirty` и снимается в начале `RebuildMesh`.

```cpp
if (chunk.Dirty()) ENG_LOGI("demo", "чанк ждёт пересборки");
```

### `void VoxelChunk::MarkDirty()`

Помечает меш устаревшим вручную — например, после смены палитры, опций
мешинга или появления соседа. Обычно вызывается у всех чанков через
`VoxelWorld::MarkAllDirty`.

```cpp
world.Palette().Set(2, crossrender::Color::FromRGB(0x77CC33));
for (crossrender::VoxelChunk* c : world.AllChunks()) c->MarkDirty();
```

### `bool VoxelChunk::Empty() const`

`true`, если в чанке нет ни одного непустого вокселя. Основано на счётчике
`nonEmptyCount_`, поэтому проверка O(1). Пустые чанки `Render` пропускает, а
`RecomputeBounds` не учитывает.

```cpp
if (chunk.Empty()) ENG_LOGD("demo", "чанк пуст, рисовать нечего");
```

### `int VoxelChunk::SolidCount() const`

Число непустых вокселей. Удобно для статистики и мини-карты; после `Fill(0)`
равно нулю, после `Fill(id)` — `kVoxelChunkVolume`.

```cpp
ENG_LOGI("demo", "заполнено %d из %d", chunk.SolidCount(), crossrender::kVoxelChunkVolume);
```

### `class VoxelWorld* VoxelChunk::world`

Обратный указатель на мир-владелец. `GetOrCreateChunk` выставляет его сам;
при ручном создании чанка его нужно задать, иначе мешер не сможет смотреть
соседей и на всех границах вырастут стены.

```cpp
crossrender::VoxelChunk chunk;
crossrender::VoxelWorld world;
chunk.world = &world;               // теперь видны соседние чанки
```

### `void VoxelChunk::RebuildMesh(const VoxelPalette& palette, bool greedy, bool textured, bool ao, const VoxelWorld* owner)`

Полностью пересобирает меш чанка на CPU. `owner` нужен, чтобы смотреть воксели
за границей чанка и чтобы прочитать `bakeLight`/`cullInterior` из его
`Options()`; при `owner == nullptr` соседей нет, а оба этих флага считаются
`true`.

* **`greedy`:** сливать ли копланарные грани.
* **`textured`:** писать ли UV в палитровый атлас.
* **`ao`:** считать ли ambient occlusion.
* **Сбрасывает** `dirty_` в `false` и не загружает меш на GPU.

```cpp
crossrender::VoxelWorld world;
crossrender::VoxelChunk* c = world.GetOrCreateChunk(crossrender::ChunkCoord{0, 0, 0});
c->Set(1, 1, 1, 1);
c->RebuildMesh(world.Palette(), /*greedy=*/true, /*textured=*/true, /*ao=*/true, &world);
ENG_LOGI("demo", "индексов: %d", static_cast<int>(c->Mesh().indices.size()));
```

### `const MeshData& VoxelChunk::Mesh() const`

Геометрия чанка на CPU. Живёт внутри чанка и переиспользует capacity между
пересборками, поэтому ссылка валидна до следующего `RebuildMesh`.

```cpp
const crossrender::MeshData& md = chunk.Mesh();
ENG_LOGI("demo", "вершин %d, индексов %d, границы валидны: %d",
         static_cast<int>(md.vertices.size()), static_cast<int>(md.indices.size()),
         md.bounds.Valid() ? 1 : 0);
```

### `const Mesh& VoxelChunk::GpuMesh() const`

Копия меша на GPU. Валидна только после успешного `UploadMesh`; при пустом
чанке или без контекста OpenGL рисовать нечего.

```cpp
if (chunk.MeshValid()) {
    ENG_LOGI("demo", "на GPU: %u вершин", chunk.GpuMesh().VertexCount());
}
```

### `bool VoxelChunk::MeshValid() const`

`true`, если GPU-меш создан. У пустого чанка всегда `false`: `RebuildMesh`
уничтожает меш, а `UploadMesh` его не создаёт.

```cpp
if (!chunk.MeshValid()) ENG_LOGW("demo", "меш чанка не загружен (нет GL?)");
```

### `void VoxelChunk::UploadMesh()`

Загружает собранный `MeshData` на GPU. Пустой меш уничтожает прежнюю копию и
оставляет `MeshValid() == false`. Без контекста OpenGL `Mesh::Create` тихо
возвращает `false`.

```cpp
chunk.RebuildMesh(world.Palette(), true, true, true, &world);
chunk.UploadMesh();
ENG_ASSERT(chunk.MeshValid() == chunk.GpuMesh().Valid());
```

### `struct VoxelMeshingOptions`

Набор переключателей мешера. Хранится в `VoxelWorld` и применяется ко всем
чанкам при `UpdateMeshes`. Значения по умолчанию — всё включено.

```cpp
crossrender::VoxelMeshingOptions opts;
opts.ambientOcclusion = false;      // быстрее, но плоско
opts.textured = false;              // цвет вершин из палитры
world.SetMeshing(opts);
world.MarkAllDirty();
world.UpdateMeshes();
```

### `bool VoxelMeshingOptions::greedy`

Слияние копланарных граней одного материала в прямоугольники. Включено по
умолчанию: сплошной куб 8³ даёт 6 квадов вместо 384. Отключение полезно для
отладки и для шейдеров, которым нужна гранулярность «один воксель — один
квад».

```cpp
crossrender::VoxelMeshingOptions opts;
opts.greedy = false;               // по кваду на воксель
world.SetMeshing(opts);
```

### `bool VoxelMeshingOptions::textured`

Писать UV в ячейку палитрового атласа (и в `Render` подключать атлас как
`baseColorTex`). При `false` UV остаются локальными, а цвет берётся из
вершин — `Render` включает `Material::vertexColors`.

```cpp
crossrender::VoxelMeshingOptions opts;
opts.textured = false;             // палитра красит вершины, без атласа
world.SetMeshing(opts);
world.UpdateMeshes();
```

### `bool VoxelMeshingOptions::ambientOcclusion`

Считать ли AO в углах граней. Даёт мягкие затемнения в углах и впадинах;
стоит три выборки из мира на угол квада, то есть влияет и на время мешинга.

```cpp
crossrender::VoxelMeshingOptions opts;
opts.ambientOcclusion = true;
world.SetMeshing(opts);
world.MarkAllDirty();
world.UpdateMeshes();
```

### `bool VoxelMeshingOptions::bakeLight`

Учитывать запечённый свет (`lights_`) в цвете вершин и в `uv2.y`. Читается
мешером из `Options()` владельца; без света (`false`) поверхность равномерно
яркая.

```cpp
crossrender::VoxelMeshingOptions opts;
opts.bakeLight = false;            // отключить запечённое затенение
world.SetMeshing(opts);
```

### `bool VoxelMeshingOptions::cullInterior`

Отсекать ли внутренние грани. При `false` каждая грань каждого вокселя
попадает в меш — годится для отладки и для эффектов вроде растворения, но
даёт кратный рост геометрии.

```cpp
crossrender::VoxelMeshingOptions opts;
opts.cullInterior = false;         // видеть всю сетку насквозь
world.SetMeshing(opts);
world.MarkAllDirty();
world.UpdateMeshes();
```

### `struct VoxelRayHit`

Результат `VoxelWorld::Raycast`: факт попадания, координаты вокселя, нормаль
грани, расстояние и материал. Промах — значение по умолчанию, где `hit`
равен `false`.

```cpp
const crossrender::VoxelRayHit hit = world.Raycast(origin, dir, 100.0f);
if (!hit.hit) ENG_LOGI("demo", "луч ушёл в пустоту");
```

### `bool VoxelRayHit::hit`

`true`, если луч встретил ненулевой воксель. Всегда проверяйте это поле перед
чтением остальных.

```cpp
const crossrender::VoxelRayHit hit = world.Raycast(origin, dir);
if (hit.hit) ENG_LOGI("demo", "попадание в (%d,%d,%d)", hit.x, hit.y, hit.z);
```

### `int VoxelRayHit::x`, `y`, `z`

Координаты попавшего вокселя в мировых воксельных единицах. Сгруппированы в
одну таблицу по правилу стандарта для полей структур-результатов.

| Поле | Смысл |
|---|---|
| `int x` | координата вокселя по X |
| `int y` | координата вокселя по Y |
| `int z` | координата вокселя по Z |

```cpp
const crossrender::VoxelRayHit hit = world.Raycast(origin, dir);
const crossrender::Vec3 center{static_cast<float>(hit.x) + 0.5f, static_cast<float>(hit.y) + 0.5f,
                       static_cast<float>(hit.z) + 0.5f};
ENG_LOGI("demo", "центр блока: %.1f %.1f %.1f", center.x, center.y, center.z);
```

### `int VoxelRayHit::nx`, `ny`, `nz`

Нормаль грани, через которую луч вошёл в воксель: одна из компонент `±1`,
остальные нули. Нужна, чтобы поставить новый блок **рядом** с ударенным:
`hit + n`. При старте внутри твёрдого вокселя нормаль нулевая.

| Поле | Смысл |
|---|---|
| `int nx` | компонента нормали по X (`-1`, `0` или `1`) |
| `int ny` | компонента нормали по Y |
| `int nz` | компонента нормали по Z |

```cpp
const crossrender::VoxelRayHit hit = world.Raycast(origin, dir, 64.0f);
if (hit.hit) {
    world.SetVoxel(hit.x + hit.nx, hit.y + hit.ny, hit.z + hit.nz, /*id=*/1);
}
```

### `f32 VoxelRayHit::distance`

Параметр луча `t` в момент входа в воксель. Так как `Raycast` нормализует
направление, это расстояние в мировых единицах от `origin` до дальней грани
пройденной ячейки. При старте внутри блока равно нулю.

```cpp
const crossrender::VoxelRayHit hit = world.Raycast(origin, dir, 32.0f);
if (hit.hit) ENG_LOGI("demo", "до блока %.2f единиц", hit.distance);
```

### `u8 VoxelRayHit::id`

Идентификатор палитры вокселя, в который попал луч. По нему выбирают, что
делать: копать, ставить блок, наносить урон.

```cpp
const crossrender::VoxelRayHit hit = world.Raycast(origin, dir);
if (hit.hit && hit.id == 3) ENG_LOGI("demo", "под прицелом земля");
```

### `class VoxelWorld`

Разреженный набор чанков плюс палитра, опции мешинга, габариты, атлас и
3D-текстура. Мир — единственная точка, которая знает про все чанки сразу,
поэтому именно он пересобирает меши, считает свет, генерирует ландшафт и
трассирует луч.

```cpp
crossrender::VoxelWorld world;
world.GenerateFlat(2, 2, 4);
world.ComputeLighting();
world.UpdateMeshes();
world.Render(r3d, material);
```

### `VoxelWorld()`

Создаёт пустой мир с палитрой `VoxelPalette::Default()`, опциями мешинга по
умолчанию и невалидными габаритами. Чанков нет.

```cpp
crossrender::VoxelWorld world;
ENG_ASSERT(world.ChunkCount() == 0);
ENG_ASSERT(!world.Bounds().Valid());
```

### `VoxelChunk* VoxelWorld::GetChunk(const ChunkCoord& c)`

Ищет существующий чанк, ничего не создавая. Возвращает `nullptr`, если чанка
нет, — это основной способ отличить «воздух» от «не загружено».

```cpp
if (world.GetChunk({4, 0, -4}) == nullptr) {
    ENG_LOGI("demo", "чанк (4,0,-4) ещё не создан");
}
```

### `VoxelChunk* VoxelWorld::GetOrCreateChunk(const ChunkCoord& c)`

Возвращает чанк, создавая его при необходимости. Новому чанку прописываются
`Coord(c)` и `world = this`, поэтому он сразу готов к мешированию. Соседей
функция **не** помечает грязными.

```cpp
crossrender::VoxelChunk* c = world.GetOrCreateChunk(crossrender::ChunkCoord{0, 0, 0});
c->Fill(1);
ENG_LOGI("demo", "чанков в мире: %d", world.ChunkCount());
```

### `void VoxelWorld::RemoveChunk(const ChunkCoord& c)`

Удаляет чанк из карты вместе с его мешем. Границы и соседние меши не
пересчитываются — после удаления вызовите `RecomputeBounds` и `MarkAllDirty`,
если чанк был видим.

```cpp
world.RemoveChunk({10, 0, 10});
world.RecomputeBounds();
world.MarkAllDirty();
```

### `void VoxelWorld::Clear()`

Удаляет все чанки, обнуляет габариты и уничтожает 3D-текстуру объёма.
Палитра и опции мешинга сохраняются.

```cpp
world.Clear();
ENG_ASSERT(world.ChunkCount() == 0);
```

### `int VoxelWorld::ChunkCount() const`

Число чанков в карте, включая полностью пустые (они создаются
`GetOrCreateChunk` и живут, пока их не удалят). Для числа непустых считайте
`SolidCount()` или `Empty()`.

```cpp
ENG_LOGI("demo", "чанков: %d", world.ChunkCount());
```

### `std::vector<VoxelChunk*> VoxelWorld::AllChunks()`

Список указателей на все чанки. Порядок не определён (`unordered_map`),
поэтому для детерминированных обходов сортируйте по `Coord()`.

```cpp
std::vector<crossrender::VoxelChunk*> chunks = world.AllChunks();
int solids = 0;
for (crossrender::VoxelChunk* c : chunks) solids += c->SolidCount();
ENG_LOGI("demo", "всего вокселей: %d", solids);
```

### `void VoxelWorld::SetVoxel(int x, int y, int z, u8 id, bool createChunk = true)`

Запись вокселя в мировых координатах. Координаты отрицательные тоже
корректны (`FloorDiv`/`FloorMod`). При `createChunk == false` запись в
несуществующий чанк молча игнорируется.

* **Свет:** новому вокселю ставится 15; чтобы получить корректное освещение,
  вызовите `ComputeLighting` заново.
* **Соседи:** запись на границе чанка помечает грязными уже существующие
  соседние чанки, потому что их отсечение зависит от этого вокселя.

```cpp
world.SetVoxel(16, 20, 16, 1);              // поставить камень
world.SetVoxel(16, 20, 16, 0);              // убрать его
world.SetVoxel(-1, 5, -33, 7, /*createChunk=*/false);   // только если чанк уже есть
```

### `u8 VoxelWorld::GetVoxel(int x, int y, int z) const`

Чтение вокселя в мировых координатах. Возвращает `0`, если чанка нет: «не
загружено» и «воздух» неразличимы — на этом построена проблема соседей при
мешировании.

```cpp
const crossrender::u8 id = world.GetVoxel(16, 20, 16);
ENG_LOGI("demo", "в (16,20,16): id=%u", id);
```

### `bool VoxelWorld::IsSolid(int x, int y, int z) const`

`true`, если в мировых координатах лежит ненулевой воксель. Тонкая обёртка над
`GetVoxel`.

```cpp
if (!world.IsSolid(px, py, pz)) ENG_LOGD("demo", "точка свободна");
```

### `VoxelPalette& VoxelWorld::Palette()`

Изменяемая палитра мира. После правки цветов нужно пересобрать атлас
(`BuildPaletteAtlas`) и меши (`MarkAllDirty` + `UpdateMeshes`), потому что цвет
вершин запекается.

```cpp
crossrender::VoxelPalette& p = world.Palette();
p.Set(7, crossrender::Color::FromRGB(0x2E8B2E));
world.BuildPaletteAtlas(16);
world.MarkAllDirty();
world.UpdateMeshes();
```

### `void VoxelWorld::SetPalette(const VoxelPalette& p)`

Полностью заменяет палитру. Удобно для переключения биома (`Default()` ↔
`Ice()`); не забудьте пересобрать атлас и меши.

```cpp
world.SetPalette(crossrender::VoxelPalette::Ice());
world.BuildPaletteAtlas(16);
world.MarkAllDirty();
world.UpdateMeshes();
```

### `VoxelMeshingOptions& VoxelWorld::Meshing()`

Изменяемые опции мешинга. Правка поля сама по себе ничего не пересобирает —
нужен `MarkAllDirty` + `UpdateMeshes`.

```cpp
crossrender::VoxelMeshingOptions& opts = world.Meshing();
opts.greedy = true;
opts.ambientOcclusion = true;
world.MarkAllDirty();
```

### `void VoxelWorld::SetMeshing(const VoxelMeshingOptions& m)`

Заменяет опции целиком. Удобно сохранять/восстанавливать пресеты качества.

```cpp
crossrender::VoxelMeshingOptions fast;
fast.greedy = true;
fast.ambientOcclusion = false;
fast.bakeLight = false;
world.SetMeshing(fast);
world.MarkAllDirty();
world.UpdateMeshes();
```

### `void VoxelWorld::UpdateMeshes()`

Главный шаг обновления: собирает все чанки с `Dirty() == true`, для каждого
вызывает `RebuildMesh` с текущей палитрой и опциями (`greedy`, `textured`,
`ambientOcclusion`) и затем `UploadMesh`. Вызывайте после любой правки
вокселей, генерации или смены палитры/опций. Работа синхронная: все грязные
чанки перестраиваются за один вызов.

```cpp
world.SetVoxel(16, 21, 16, 1);
world.UpdateMeshes();                  // меш этого чанка и соседей обновлён
```

### `void VoxelWorld::MarkAllDirty()`

Помечает грязными все чанки мира. Нужен после смены палитры или опций
мешинга, а также после появления новых чанков рядом с уже отмешированными
(иначе на их границах останутся стены).

```cpp
world.MarkAllDirty();
world.UpdateMeshes();
```

### `void VoxelWorld::Render(Renderer3D& r, const Material& mat)`

Рисует все непустые чанки с валидным GPU-мешем. Материал копируется: при
`textured == true` в копию подставляется палитровый атлас (при необходимости
строится лениво с `cellSize = 16`), иначе включается `vertexColors = true`.
Каждый чанк рисуется со своим сдвигом `Mat4::Translate(WorldOrigin())`.

```cpp
crossrender::Material mat = crossrender::Material::Default();
mat.roughness = 0.85f;
mat.metallic = 0.0f;
world.Render(r3d, mat);
```

### `const Bounds& VoxelWorld::Bounds() const`

Габариты мира: AABB, охватывающий **целые** чанки (от `WorldOrigin()` до
`WorldOrigin() + 32`), а не отдельные воксели. Обновляются только
`RecomputeBounds` и генераторы.

```cpp
const crossrender::Bounds& b = world.Bounds();
if (b.Valid()) ENG_LOGI("demo", "мир от %.0f до %.0f по Y", b.min.y, b.max.y);
```

### `void VoxelWorld::RecomputeBounds()`

Пересчитывает габариты по текущему набору непустых чанков. Вызывайте после
ручного создания/удаления чанков и после правок вокселей, если габариты
нужны для камеры или отсечения.

```cpp
world.SetVoxel(200, 5, 200, 1);
world.RecomputeBounds();
ENG_LOGI("demo", "максимум по X: %.0f", world.Bounds().max.x);
```

### `void VoxelWorld::GenerateTerrain(int chunksX, int chunksZ, u64 seed = 1337, int baseHeight = 12, int amplitude = 10)`

Детерминированный ландшафт от начала координат: `chunksX × chunksZ` чанков,
высота из fBm-шума (`baseHeight ± amplitude`), пещеры из 3D-шума, вода до
уровня `baseHeight - 2`, жилы руды (золото/металл) и деревья на ровной траве.
Один и тот же `seed` даёт один и тот же мир.

* **`chunksX`/`chunksZ`:** размер области в чанках (минимум 1); область
  занимает `x, z >= 0`.
* **`baseHeight`:** базовая высота (не меньше 3), **`amplitude`:** размах
  рельефа (не меньше 0).
* **Побочные эффекты:** `MarkAllDirty()` и `RecomputeBounds()`; меши и свет
  не считаются — вызывайте `ComputeLighting()` и `UpdateMeshes()` сами.

```cpp
crossrender::VoxelWorld world;
world.GenerateTerrain(/*chunksX=*/2, /*chunksZ=*/2, /*seed=*/4242, /*baseHeight=*/12,
                      /*amplitude=*/8);
world.ComputeLighting();
world.UpdateMeshes();
```

### `void VoxelWorld::GenerateFlat(int chunksX, int chunksZ, int height = 4, u8 top = 2, u8 fill = 3)`

Плоская площадка: `chunksX × chunksZ` чанков от начала координат, столб высотой
`height` (зажимается в `1..128`), верхний слой — `top`, остальное — `fill`.
Значения по умолчанию рассчитаны на `VoxelPalette::Default()`: трава поверх
земли.

```cpp
crossrender::VoxelWorld world;
world.GenerateFlat(2, 1, /*height=*/4, /*top=*/2, /*fill=*/3);
world.ComputeLighting();
world.UpdateMeshes();
```

### `void VoxelWorld::GenerateSphere(const Vec3& center, f32 radius, u8 id)`

Заполняет сферу: воксель попадает внутрь, если расстояние от его центра до
`center` не больше `radius`. Радиус `<= 0` или `id == 0` — no-op. Чанки
создаются по мере надобности.

```cpp
world.GenerateSphere(crossrender::Vec3{16.0f, 8.0f, 16.0f}, 4.0f, /*id=*/13);
world.RecomputeBounds();
```

### `void VoxelWorld::GenerateBox(const Vec3& lo, const Vec3& hi, u8 id)`

Заполняет параллелепипед с полуоткрытым диапазоном:
`x` от `floor(lo.x)` до `ceil(hi.x) - 1` и так далее по осям. Углы можно
передавать в любом порядке — компоненты сортируются через `Min`/`Max`.
`id == 0` — no-op.

```cpp
world.GenerateBox(crossrender::Vec3{2, 2, 2}, crossrender::Vec3{10, 10, 10}, /*id=*/1);
world.MarkAllDirty();
```

### `void VoxelWorld::GenerateIsland(int radiusChunks, u64 seed = 7)`

Структурированный демо-остров: `radiusChunks` (зажимается в `1..12`) задаёт
радиус в чанках, мир занимает `[-R, R)` по X и Z при `R = radiusChunks * 32`.
Внутри — холмы, пляжи, озеро-пруд, океанская отмель, пещеры со светокамнем,
жилы руды и деревья; уровень моря 8, высота до 44. Полностью детерминирован по
`seed`.

```cpp
crossrender::VoxelWorld world;
world.GenerateIsland(/*radiusChunks=*/1, /*seed=*/7);
world.ComputeLighting();
world.BuildPaletteAtlas(16);
world.UpdateMeshes();
```

### `VoxelRayHit VoxelWorld::Raycast(const Vec3& origin, const Vec3& dir, f32 maxDistance = 100.0f) const`

Трассирует луч по воксельной сетке (DDA). Возвращает первое попадание в
ненулевой воксель или `VoxelRayHit{}` при промахе. Метод `const`: мир не
меняется, чанки не создаются.

* **`origin`:** начало луча в мировых координатах.
* **`dir`:** направление; нормализуется внутри. Нулевое или почти нулевое
  (`LengthSq < 0.5`) даёт промах.
* **`maxDistance`:** предел по расстоянию; `<= 0` даёт промах.
* Твёрдыми считаются все ненулевые id, включая воду и стекло.

```cpp
const crossrender::Vec3 origin{16.5f, 30.0f, 16.5f};
const crossrender::Vec3 dir = crossrender::Normalize(crossrender::Vec3{0.0f, -1.0f, 0.2f});
const crossrender::VoxelRayHit hit = world.Raycast(origin, dir, /*maxDistance=*/120.0f);
if (hit.hit) ENG_LOGI("demo", "блок (%d,%d,%d) на %.1f", hit.x, hit.y, hit.z, hit.distance);
```

### `void VoxelWorld::ComputeLighting(int sunHeight = 64)`

Считает запечённый свет. Первый проход — вертикальный «небесный свет»: столбец
просматривается сверху вниз, первый твёрдый воксель остаётся с 15, всё ниже
стартует с 0. Второй проход — BFS-заливка от освещённых клеток с затуханием:
прозрачный сосед стоит 1 (вниз — 2), непрозрачный — 4.

* **`sunHeight` не используется** — параметр оставлен для совместимости,
  вертикальный проход всегда идёт от верхнего существующего чанка.
* **Чанки не создаются**: свет не выходит за пределы уже существующих чанков.
* После правок вокселей вызывайте функцию заново; `UpdateMeshes` сам свет не
  пересчитывает.

```cpp
world.GenerateFlat(1, 1, 4);
world.ComputeLighting();               // аргумент игнорируется
world.MarkAllDirty();
world.UpdateMeshes();
```

### `f32 VoxelWorld::SampleAO(int x, int y, int z, int nx, int ny, int nz) const`

Возвращает ambient occlusion грани вокселя `(x, y, z)` с нормалью `(nx, ny,
nz)`: среднее четырёх угловых значений в диапазоне `0..1`, где 1 — полностью
открыто. Нормаль задаёт ось грани; для нулевой нормали берётся грань по X.
Полезна для подсветки/подсказок, сам мешер считает AO своей внутренней
функцией.

```cpp
const crossrender::f32 ao = world.SampleAO(5, 3, 5, /*nx=*/0, /*ny=*/1, /*nz=*/0);
ENG_LOGI("demo", "AO верхней грани: %.2f", ao);
```

### `void VoxelWorld::BuildPaletteAtlas(int cellSize = 16)`

Строит палитровый атлас 16×16 ячеек из текущей палитры. Каждая ячейка —
процедурный узор (шум, кладка, кольца дерева, блик стекла, волны воды и т. п.)
с альфой из палитры. Размер ячейки зажимается в `4..128`, то есть текстура
получается от 64×64 до 2048×2048. Требует контекста OpenGL; прежний атлас
уничтожается. Мешер рассчитывает ровно на сетку 16×16 и ячейку `1/16`.

```cpp
world.SetPalette(crossrender::VoxelPalette::Default());
world.BuildPaletteAtlas(/*cellSize=*/16);
if (!world.PaletteAtlas().Valid()) ENG_LOGW("demo", "атлас не создан (нет GL?)");
```

### `const Texture& VoxelWorld::PaletteAtlas() const`

Текстура атласа. Невалидна, пока `BuildPaletteAtlas` не выполнена успешно
(или пока `Render` не построил её лениво). `Render` использует её как
`baseColorTex` при `textured == true`.

```cpp
const crossrender::Texture& atlas = world.PaletteAtlas();
if (atlas.Valid()) ENG_LOGI("demo", "атлас %dx%d", atlas.Width(), atlas.Height());
```

### `Rect VoxelWorld::PaletteUV(u8 id) const`

Нормированный прямоугольник ячейки палитры в атласе: `x = (id % 16) / 16`,
`y = (id / 16) / 16`, ширина и высота `1/16`. Ровно эти границы использует
мешер для UV.

```cpp
const crossrender::Rect cell = world.PaletteUV(2);          // трава
ENG_LOGI("demo", "ячейка: x=%.4f y=%.4f w=%.4f h=%.4f", cell.x, cell.y, cell.w, cell.h);
```

### `bool VoxelWorld::BuildVolumeTexture(const Vec3& min, const Vec3& size, u8 fillOutside = 0)`

Упаковывает область мира в 3D-текстуру для режима raymarching. Размеры
округляются вверх и зажимаются в `1..384` по каждой оси, начало берётся как
`floor(min)`. В RGB пишется цвет палитры, в альфу — «занятость × свет»
(`64 + 191 * light / 15`, никогда не ноль, чтобы пещеры оставались видны).
При `fillOutside != 0` пустые ячейки заполняются цветом этого id с альфой 255.

* **Возвращает:** результат `Texture::Create3D`; без контекста OpenGL —
  `false`.

```cpp
const crossrender::Vec3 lo{0, 0, 0};
const crossrender::Vec3 span{64, 48, 64};
if (!world.BuildVolumeTexture(lo, span, /*fillOutside=*/0)) {
    ENG_LOGW("demo", "3D-текстура не собралась");
}
```

### `const Texture& VoxelWorld::VolumeTexture() const`

3D-текстура объёма, собранная `BuildVolumeTexture`. Невалидна, пока объём не
собран (или пока не было контекста OpenGL); `Clear()` её уничтожает.

```cpp
const crossrender::Texture& volume = world.VolumeTexture();
if (volume.Valid()) ENG_LOGI("demo", "объём %dx%d", volume.Width(), volume.Height());
```

### `void VoxelWorld::RenderRaymarched(Renderer3D& r, const Vec3& boxMin, const Vec3& boxSize, const Camera& cam)`

Рисует объём лучом по 3D-текстуре, без геометрии чанков. Ничего не делает,
если текстура объёма невалидна или не инициализирован загрузчик OpenGL.
Внутри лениво собирается собственный шейдер и единичный куб (они живут до
конца процесса), выключается отсечение граней, включается смешивание
`SRC_ALPHA / ONE_MINUS_SRC_ALPHA`, запись глубины отключается. Свет и
параметры шага фиксированы в коде (`uMaxSteps = 256`, шаг `0.8` вокселя).

```cpp
if (world.VolumeTexture().Valid()) {
    world.RenderRaymarched(r3d, lo, span, camera);
} else {
    world.Render(r3d, material);            // обычный путь через меши
}
```

### `const VoxelMeshingOptions& VoxelWorld::Options() const`

Только для чтения — те же опции, что возвращает `Meshing()`. Именно отсюда
мешер берёт `bakeLight` и `cullInterior` при `RebuildMesh`.

```cpp
const crossrender::VoxelMeshingOptions& opts = world.Options();
ENG_LOGI("demo", "bakeLight=%d cullInterior=%d", opts.bakeLight ? 1 : 0, opts.cullInterior ? 1 : 0);
```

## Пример целиком

```cpp
#include "crossrender/core/Log.h"
#include "crossrender/gfx/Renderer3D.h"
#include "crossrender/voxel/Voxel.h"

// Мир, который умеет: сгенерировать остров, запечь свет, собрать атлас и
// меши, отрисовать чанки и дать игроку копать/ставить блоки лучом.
class VoxelDemo {
public:
    void Init() {
        world_.SetPalette(crossrender::VoxelPalette::Default());

        crossrender::VoxelMeshingOptions opts;
        opts.greedy = true;
        opts.textured = true;
        opts.ambientOcclusion = true;
        opts.bakeLight = true;
        opts.cullInterior = true;
        world_.SetMeshing(opts);

        world_.GenerateIsland(/*radiusChunks=*/1, /*seed=*/7);   // остров 64x64 вокселя
        world_.ComputeLighting();                                // запекаем свет
        world_.BuildPaletteAtlas(16);                            // атлас под textured
        world_.MarkAllDirty();
        world_.UpdateMeshes();                                   // пересборка + загрузка

        material_.name = "VoxelChunks";
        material_.roughness = 0.85f;
        material_.metallic = 0.0f;

        ENG_LOGI("demo", "чанков %d, габариты по Y: %.0f..%.0f", world_.ChunkCount(),
                 world_.Bounds().min.y, world_.Bounds().max.y);
    }

    // Правка по лучу из камеры. Возвращает true, если мир изменился.
    bool Edit(const crossrender::Vec3& origin, const crossrender::Vec3& dir, crossrender::u8 block, bool place) {
        const crossrender::VoxelRayHit hit = world_.Raycast(origin, dir, /*maxDistance=*/120.0f);
        if (!hit.hit) return false;

        if (place) {
            const int x = hit.x + hit.nx, y = hit.y + hit.ny, z = hit.z + hit.nz;
            if (y < 0) return false;
            world_.SetVoxel(x, y, z, block);
        } else {
            world_.SetVoxel(hit.x, hit.y, hit.z, 0);
        }

        world_.ComputeLighting();        // свет запечён — пересчитываем после правки
        world_.MarkAllDirty();           // соседи тоже могли изменить отсечение
        world_.UpdateMeshes();
        return true;
    }

    void Draw(crossrender::Renderer3D& r) { world_.Render(r, material_); }

private:
    crossrender::VoxelWorld world_;
    crossrender::Material material_;
};
```

## См. также

* `docs/gfx/Mesh.md` — `MeshData`, `Mesh`, `Material` и `Vertex::uv2`, куда
  мешер складывает AO и свет.
* `docs/gfx/Texture.md` — палитровый атлас и 3D-текстура объёма.
* `docs/gfx/Renderer3D.md` — `Renderer3D::Draw`, которым `VoxelWorld::Render`
  выводит чанки.
* `docs/assets/Model.md` — импорт `.vox`, который строит обычный `MeshData`
  тем же жадным мешером.
* `docs/core/Log.md` — категория `"voxel"` в сообщениях генераторов.
