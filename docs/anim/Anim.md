# crossrender/anim/Anim.h — скелетная анимация: треки, клипы, блендинг, твины и пружины

Модуль анимации скелета: ключевые треки (`Track`), клипы (`AnimationClip`),
иерархия костей (`Skeleton`), покадровое смешивание поз (`Animator`) и
вспомогательные процедурные аниматоры (`Spring`, `Tween`, `ApplyEase`).

## Заголовок

```cpp
#include "crossrender/anim/Anim.h"
```

## Обзор

Модуль разделён на три слоя, и путать их не стоит.

**Слой данных.** `Track<T>` — это одна анимируемая величина во времени: он
хранит отсортированный список `Keyframe<T>` и умеет выбирать значение в момент
`time`. В заголовке объявлены три готовых псевдонима: `Vec3Track` (вектор,
обычно смещение или масштаб), `QuatTrack` (поворот) и `FloatTrack` (число).
Шаблон специализирован ровно для `Vec3`, `Quat` и `f32` — собственный тип в
`Track<T>` подставить не получится, он не слинкуется. `AnimationClip` собирает
треки по костям в `JointTrack` (смещение + поворот + масштаб одной кости) и
хранит метаданные: длительность, частоту тиков, границы цикла и события
`AnimationEvent`.

**Локальное пространство.** Клип не знает ни скелета, ни bind-позы: он оперирует
только локальными TRS костей. Мировые матрицы появляются позже, когда
`Pose::ComputeWorld` проходит иерархию и умножает локальные матрицы родителей на
локальные матрицы детей. Отсюда следует главное правило: **клипы сэмплируются в
локальном пространстве**, а извлечение root motion (`applyRootMotion`,
`rootJoint`) объявлено в структуре, но реализацией не используется — корневое
смещение придётся забирать из `JointWorld(rootJoint)` вручную.

**Как работает семплирование клипа.** `AnimationClip::Sample` заполняет переданные векторы TRS и локальных матриц:

1. Размер выхода — максимум из номера последней кости в треках и текущих
   размеров переданных векторов. Векторы **только расширяются**: уже лежащие в
   них значения сохраняются.
2. Время приводится к рабочему диапазону: при `looping` оно заворачивается в
   `[base, base + dur]`, иначе зажимается. Точка `base + dur` при заворачивании
   не преобразуется — на самом шве семплируется последний ключ, чтобы режим
   `ClampForever` мог его удержать.
3. По каждому `JointTrack` соответствующая величина **заменяется** значением
   трека (если у канала есть ключи и выставлен флаг `hasTranslation` /
   `hasRotation` / `hasScale`). Локальная матрица кости пересобирается как
   `Translate * Rotation * Scale`.

> **Ловушка (смещение hips).** Трансляция именно *заменяется*, а не
> складывается с bind-позой: `AnimationClip` не знает bind-трансляции кости.
> Если ключи смещения корневой кости (hips) записаны как «чистое» смещение
> относительно нуля, персонаж «упадёт» — ступни уйдут под пол, потому что
> bind-высота таза потеряется. Ключи смещения должны **нести bind-смещение в
> себе**: например, при bind-высоте таза `0.12` каждый ключ hips обязан
> содержать `y = 0.12 + авторское смещение`.

> **Ловушка (`locals` без TRS).** Если вызвать `Sample(time, &locals)` и не
> передать векторы `translations` / `rotations` / `scales`, то для кости из
> трека сборка локальной матрицы пойдёт от нулевого смещения и единичного
> масштаба: `locals[j]` получит `Translate(0) * sampledRotation * Scale(1)`.
> `Animator` поэтому всегда передаёт все четыре вектора, предварительно
> заполнив их bind-позой. Повторяйте этот приём.

**Поиск сегмента и неоднозначные ключи.** Внутренняя функция `FindSegment` (она приватная, но её поведение видно через
`Sample`) выбирает сегмент `[ia, ib]` и нормированный параметр `t`:

* время до первого ключа → `t = 0` на первом сегменте;
* время на последнем ключе и **после него** → `t = 1` на последнем сегменте;
* сегмент нулевой длины (два ключа в один момент) → `t = 1`.

Из-за этого два ключа с одинаковым временем **неоднозначны**: побеждает
последний из них, а первый не наблюдается ни при одной интерполяции. Не
авторьте дубликаты ключей по времени — это не «мгновенный переход», а молча
потерянный ключ. Тот же `t = 1` означает, что интерполяция `Step` переключается
на значение *следующего* ключа ровно в его момент.

**Как слои `Animator` складываются в позу.** `Animator::Update` каждый кадр начинает с bind-позы скелета и затем проходит
слои по порядку индексов (0 — база). Порядок важен: каждый следующий слой
накладывается на уже накопленный результат. Вес слоя раскладывается на
`weight * fadeOutT` (устойчивый вклад) и `fadeInT` (рампа кроссфейда); сама
рампа не применяется дважды.

| Режим `AnimBlendMode` | Что делает |
|---|---|
| `Override` | покомпонентный `Lerp` / `Slerp` от накопленной позы к семплу по весу |
| `Masked` | то же, но вес каждой кости умножается на её значение из `AnimLayer::mask` |
| `Additive` | семпл сначала превращается в bind-относительную локальную дельту (`bind.local⁻¹ * sample.local`), затем композитится поверх: смещение `+= delta * w`, поворот `*= slerp(identity, delta, w)`, масштаб `*= lerp(1, delta, w)` |
| `Multiply` | в текущей реализации выполняется тем же кодом, что и `Additive`; отдельного умножения нет |

Маска по именам костей строится методом `RebuildLayerMasks`: он заполняет
`mask.weights` нулями и ставит `1` костям из `boneMaskNames`. Слой с `weight = 0`
не участвует, а если ни один слой не дал вклада, поза остаётся bind-позой.

**Как устроен кроссфейд.** `Animator::Play(clip, fadeSeconds, ...)` не хранит две дорожки времени: он
запоминает **снимок текущей позы** в `scratchB_` и запускает у нового слоя
рампу `fadeIn`. Снимок всего один, поэтому любой другой незавершённый
кроссфейд при этом принудительно доводится до конца. В `Update` слой, который
сейчас в рамке и ещё не «вложил» свой вклад (`!contributed`), смешивается как
`lerp(bind, lerp(snapshot, sample, fade), weight)`; остальные — обычным
`BlendLayerInto`. После завершения рампы слой ведёт себя как `Override` с
постоянным весом.

**Что такое blend space.** `PlayBlendSpace1D` — одномерное пространство смешивания: набор клипов с
пороговыми значениями параметра. Между двумя соседними порогами игрок
«запекает» один клип как `lerp(клип A, клип B, t)` и проигрывает его как
обычный клип. Запекание создаёт **новый** клип и добавляет его в список
`clips_`, поэтому вызывать `PlayBlendSpace1D` каждый кадр нельзя — список
клипов будет расти бесконечно. Двумерного варианта в реализации нет, несмотря
на комментарий в заголовке.

**Твины и пружины.** `Spring` и `Tween` — процедурная альтернатива клипу для одиночных скалярных
величин: позиций UI, масштабов карточек, параметров шейдеров, «вторичной»
моторики поверх скелетной анимации. `ApplyEase` даёт 28 семейств кривых easing
с точными концами (при `t ≤ 0` — ровно 0, при `t ≥ 1` — ровно 1, даже для
перелетающих `Back` и `Elastic`).

Выбор прост: если анимация **авторская**, многосуставная, зацикленная и должна
смешиваться с другими — берите клип и `Animator`; если величина **одна**,
цель меняется во время выполнения, а ассеты не нужны — берите `Spring`
(он подшаговый и не зависит от частоты кадров) или `Tween` (одноразовый
переход с задержкой и easing).

Ограничения модуля, о которых стоит помнить: клипы локальны; root motion не
извлекается; `ticksPerSecond` — только метаданные, семплирование всегда идёт в
секундах; `AnimBlendMode::Multiply` ведёт себя как `Additive`; двумерных blend
space нет; `BlendPoses` с режимом `Masked` вырождается в `Override` (маски в
сигнатуре нет).

## Члены класса

Ниже описаны все публичные члены заголовка. Поля структур сгруппированы по
роли: на группу полей даётся одна таблица и **один общий пример** (в тексте
таких разделов это оговорено), а каждый метод, конструктор, фабрика и
перечисление получают отдельный подраздел со своим примером.

### crossrender::Joint

Одна кость скелета: имя, индекс родителя и локальная bind-поза. Индекс
родителя `-1` означает корень. Поля заполняются через `Skeleton::AddJoint` и
уточняются в `Skeleton::Finalise` (там считаются мировые и обратные bind-
матрицы).

| Поле | Смысл |
|---|---|
| `std::string name` | имя кости; по нему работают `FindJoint`, маски и `MirrorPose` |
| `int parent` | индекс родителя или `-1` для корня |
| `Mat4 inverseBindMatrix` | обратная мировая bind-матрица; считается в `Finalise` |
| `Mat4 localBindPose` | локальная bind-матрица, собранная из TRS ниже |
| `Vec3 localTranslation` | bind-смещение кости в системе родителя |
| `Quat localRotation` | bind-поворот кости |
| `Vec3 localScale` | bind-масштаб кости |

```cpp
crossrender::Skeleton skel;
skel.AddJoint("Hips", -1, crossrender::Mat4::Translate(crossrender::Vec3{0, 1.0f, 0}));
skel.Finalise();

// Кость по индексу: имя, родитель и разложенная bind-поза.
const crossrender::Joint& hips = skel.JointAt(0);
ENG_LOGI("anim", "кость '%s', родитель %d, bind y=%.2f", hips.name.c_str(), hips.parent,
         hips.localTranslation.y);
```

### crossrender::Skeleton

Иерархия костей и их bind-поза. Скелет — это разделяемый ресурс: `Animator`
хранит указатель на него (`SetSkeleton`) и не владеет им, поэтому скелет обязан
жить дольше аниматора.

```cpp
crossrender::Skeleton skel = crossrender::Skeleton::MakeHumanoid();
ENG_LOGI("anim", "костей %d, глубина %d", skel.JointCount(), skel.MaxDepth());
```

### `Skeleton()`

Конструктор по умолчанию: создаёт пустой скелет без костей. После создания
нужно добавить кости и вызвать `Finalise`.

```cpp
crossrender::Skeleton skel;                 // ноль костей
ENG_ASSERT(skel.JointCount() == 0);
```

### `void Clear()`

Удаляет все кости и очищает таблицу имён. Указатели и индексы, выданные
раньше, становятся недействительными — не вызывайте `Clear` у скелета, пока
его использует `Animator`.

```cpp
crossrender::Skeleton skel = crossrender::Skeleton::MakeChain(3);
skel.Clear();
ENG_LOGI("anim", "после Clear костей: %d", skel.JointCount());
```

### `int AddJoint(const std::string& name, int parent, const Mat4& localBind = Mat4::Identity())`

Добавляет кость и возвращает её индекс. Локальная bind-матрица раскладывается
на `localTranslation` / `localRotation` / `localScale`. Если `parent` выходит за
границы уже добавленных костей, пишется предупреждение, а кость становится
корнем. Повторяющееся имя не перезаписывает прежнюю регистрацию — при поиске
победит первая.

```cpp
crossrender::Skeleton skel;
const int root = skel.AddJoint("Root", -1, crossrender::Mat4::Translate(crossrender::Vec3{0, 0, 0}));
const int hand = skel.AddJoint("Hand", root, crossrender::Mat4::Translate(crossrender::Vec3{0.5f, 0, 0}));
ENG_LOGI("anim", "root=%d, hand=%d", root, hand);
```

### `void Finalise()`

Завершает построение: пересчитывает `localBindPose`, обходит иерархию в
топологическом порядке (родители раньше детей), заполняет мировые и обратные
bind-матрицы. Порядок хранения костей значения не имеет; при обнаружении цикла
родителей остаток трактуется как корни. Вызывайте **после** добавления всех
костей и **повторно** после ручной правки bind-полей.

```cpp
crossrender::Skeleton skel;
skel.AddJoint("A", -1);
skel.AddJoint("B", 0, crossrender::Mat4::Translate(crossrender::Vec3{1, 0, 0}));
skel.Finalise();

const crossrender::Mat4& inv = skel.JointAt(1).inverseBindMatrix;   // готова к скиннингу
(void)inv;
```

### `int JointCount() const`

Количество костей в скелете.

```cpp
crossrender::Skeleton skel = crossrender::Skeleton::MakeHumanoid();
const int n = skel.JointCount();
std::vector<crossrender::Mat4> locals(static_cast<crossrender::usize>(n), crossrender::Mat4::Identity());
```

### `Joint& JointAt(int i)` / `const Joint& JointAt(int i) const`

Доступ к кости по индексу без проверки границ. Индекс должен быть в
`[0, JointCount())`; для поиска по имени есть `FindJoint`.

```cpp
crossrender::Skeleton skel = crossrender::Skeleton::MakeChain(2);
skel.JointAt(1).localScale = crossrender::Vec3{2, 2, 2};
skel.Finalise();                                     // пересобрать bind после правки
```

### `int FindJoint(const std::string& name) const`

Возвращает индекс кости по имени или `-1`. Сначала ищет в хеш-таблице, затем
делает линейный проход (на случай скелета, собранного вручную без `Finalise`).

```cpp
crossrender::Skeleton skel = crossrender::Skeleton::MakeHumanoid();
const int head = skel.FindJoint("Head");
if (head < 0) ENG_LOGW("anim", "в скелете нет кости Head");
```

### `const std::vector<Joint>& Joints() const` / `std::vector<Joint>& Joints()`

Прямой доступ ко всему массиву костей — например для скиннинга или отладочной
отрисовки. Ссылка живёт до следующего `AddJoint` / `Clear`.

```cpp
crossrender::Skeleton skel = crossrender::Skeleton::MakeHumanoid();
for (const crossrender::Joint& j : skel.Joints()) {
    if (j.parent < 0) ENG_LOGI("anim", "корень: %s", j.name.c_str());
}
```

### `int MaxDepth() const`

Максимальная длина цепочки родителей (скелет из одной кости даёт `1`). Полезно
для оценки стоимости скиннинга и для диагностики случайных циклов.

```cpp
crossrender::Skeleton skel = crossrender::Skeleton::MakeChain(6);
ENG_LOGI("anim", "глубина цепочки: %d", skel.MaxDepth());   // 6
```

### `static Skeleton MakeHumanoid()`

Готовая человекообразная заготовка: `Hips`, позвоночник, голова, руки и ноги с
осмысленными bind-смещениями (в метрах). Уже финализирована. Удобна для
прототипов и тестов: имена костей совпадают с лево/правыми парами, которые
понимает `MirrorPose`.

```cpp
crossrender::Skeleton rig = crossrender::Skeleton::MakeHumanoid();
const int foot = rig.FindJoint("LeftFoot");
ENG_LOGI("anim", "Humanoid: %d костей, LeftFoot=%d", rig.JointCount(), foot);
```

### `static Skeleton MakeChain(int count = 4, f32 length = 1.0f)`

Простая цепочка костей вдоль оси X: кость `0` — корень, каждая следующая
смещена на `length`. `count` меньше единицы поднимается до `1`, нулевая длина
заменяется на `1`. Часто используется как «псевдоскелет» для примеров.

```cpp
crossrender::Skeleton chain = crossrender::Skeleton::MakeChain(5, 0.25f);
std::vector<crossrender::Vec3> jointPositions;   // сюда сложим мировые позиции
jointPositions.reserve(static_cast<crossrender::usize>(chain.JointCount()));
```

### `enum class Interpolation : u8`

Режим интерполяции между двумя ключами трека. Режим берётся у **левого** ключа
сегмента.

| Значение | Поведение |
|---|---|
| `Step` | значение держится до следующего ключа, переключение ровно в его момент |
| `Linear` | линейная интерполяция; для поворотов — `Slerp` по короткой дуге |
| `CubicSpline` | кубический Эрмит по тангенсам `inTangent` / `outTangent`; без тангенсов для поворотов вырождается в `Slerp` |
| `Bezier` | контрольных точек у ключей нет, поэтому используется сглаженный `t²(3−2t)` |

```cpp
crossrender::Vec3Track track;
track.AddKey(0.0f, crossrender::Vec3{0, 0, 0}, crossrender::Interpolation::Step);
track.AddKey(0.5f, crossrender::Vec3{1, 0, 0}, crossrender::Interpolation::Linear);
track.AddKey(1.0f, crossrender::Vec3{2, 0, 0}, crossrender::Interpolation::CubicSpline);
```

### `template <typename T> struct Keyframe`

Один ключ трека. Поля публичные; для `Vec3Track` / `QuatTrack` / `FloatTrack`
тип `T` — это соответственно `Vec3`, `Quat` и `f32`.

| Поле | Смысл |
|---|---|
| `f32 time` | момент ключа в секундах |
| `T value` | значение |
| `T inTangent` | входящая касательная (использует `CubicSpline`) |
| `T outTangent` | исходящая касательная (использует `CubicSpline`) |
| `Interpolation interp` | режим интерполяции до следующего ключа |

```cpp
crossrender::Keyframe<crossrender::Vec3> k;
k.time = 0.25f;
k.value = crossrender::Vec3{0, 1, 0};
k.interp = crossrender::Interpolation::Linear;
```

### `template <typename T> struct Track`

Последовательность ключей одной величины с выборкой по времени. Ключи всегда
поддерживаются отсортированными по `time`: `AddKey` вставляет в нужное место
двоичным поиском, `Sort` досортировывает набор после ручных правок. Шаблон
инстанцирован только для `Vec3`, `Quat` и `f32`.

```cpp
crossrender::FloatTrack opacity;
opacity.AddKey(0.0f, 0.0f);
opacity.AddKey(1.0f, 1.0f);
const crossrender::f32 mid = opacity.Sample(0.5f);   // 0.5
```

### `T Track<T>::Sample(f32 time) const`

Значение трека в момент `time`. Для пустого трека возвращается
`Vec3{0,0,0}` / `Quat::Identity()` / `0.0f`; для единственного ключа — его
значение независимо от времени. Время до первого и после последнего ключа
зажимается к крайним ключам.

```cpp
crossrender::QuatTrack turn;
turn.AddKey(0.0f, crossrender::Quat::Identity());
turn.AddKey(1.0f, crossrender::Quat::FromAxisAngle(crossrender::Vec3{0, 1, 0}, crossrender::kPi * 0.5f));

const crossrender::Quat q = turn.Sample(0.5f);      // четверть оборота пополам
(void)q;
```

### `void Track<T>::AddKey(f32 time, const T& value, Interpolation interp = Interpolation::Linear)`

Добавляет ключ, сохраняя порядок по времени. Ключи с одинаковым временем
вставляются после уже существующих (вставка «верхней границей»), но
семплирование всё равно неоднозначно — см. ловушку о `FindSegment` в обзоре.

```cpp
crossrender::Vec3Track position;
position.AddKey(0.0f, crossrender::Vec3{0, 0, 0});
position.AddKey(1.5f, crossrender::Vec3{0, 0.4f, 0}, crossrender::Interpolation::CubicSpline);
```

### `void Track<T>::Sort()`

Стабильно сортирует ключи по времени. Нужен, если ключи заполнялись вручную
через публичное поле `keys`, а не через `AddKey`.

```cpp
crossrender::FloatTrack track;
track.keys.push_back({0.5f, 1.0f});
track.keys.push_back({0.0f, 0.0f});
track.Sort();                                  // 0.0 идёт первым
```

### `using Vec3Track` / `using QuatTrack` / `using FloatTrack`

Псевдонимы над `Track<Vec3>`, `Track<Quat>` и `Track<f32>`. Именно они
используются в `AnimationClip::JointTrack`; отдельного `Track` для произвольного
типа реализация не предоставляет.

```cpp
crossrender::Vec3Track  offset;      // смещение или масштаб кости
crossrender::QuatTrack  rotation;    // поворот кости
crossrender::FloatTrack weight;      // скалярный параметр
offset.AddKey(0.0f, crossrender::Vec3{0, 0, 0});
rotation.AddKey(0.0f, crossrender::Quat::Identity());
weight.AddKey(0.0f, 1.0f);
```

### `struct AnimationEvent`

Игровое событие внутри клипа: момент времени, имя и произвольная строка-полезная
нагрузка. События не воспроизводятся сами: `AnimationClip::CollectEvents`
находит пересечённые за кадр события, а `Animator` рассылает их через
`FiredEvents` и `SetEventCallback`.

| Поле | Смысл |
|---|---|
| `f32 time` | момент события в секундах от начала клипа |
| `std::string name` | имя события для игровой логики |
| `std::string payload` | дополнительная строка (номер кадра, имя звука, …) |

```cpp
crossrender::AnimationClip clip;
clip.duration = 1.0f;
clip.events.push_back({0.5f, "Footstep", "left"});
clip.events.push_back({0.9f, "Sound", "swing.wav"});
```

### crossrender::AnimationClip

Клип — это набор дорожек по костям плюс метаданные и события. Он не привязан к
конкретному скелету: индексы костей в `JointTrack` — это индексы того скелета,
с которым клип будут проигрывать.

```cpp
crossrender::AnimationClip walk = crossrender::AnimationClip::MakeWalk(12, 1.0f);
ENG_LOGI("anim", "клип '%s': %.2f c, дорожек %d", walk.name.c_str(), walk.duration,
         static_cast<int>(walk.tracks.size()));
```

### Поля `name`, `duration`, `ticksPerSecond`, `looping`, `loopStart`, `loopEnd`

Метаданные клипа. Поля публичные и задаются напрямую. `EffectiveDuration`
возвращает `loopEnd - loopStart`, если границы заданы, иначе `duration`.

| Поле | Смысл |
|---|---|
| `std::string name` | имя; по нему работает `Animator::FindClip` |
| `f32 duration` | полная длительность в секундах |
| `f32 ticksPerSecond` | частота тиков исходного экспорта — **только метаданные**: семплирование всегда идёт в секундах |
| `bool looping` | зацикливать ли клип; влияет на `Sample` и на сбор событий |
| `f32 loopStart`, `f32 loopEnd` | границы рабочего диапазона; если `loopEnd > loopStart`, длительность равна их разности |

```cpp
crossrender::AnimationClip clip;
clip.name = "Idle";
clip.duration = 2.0f;
clip.ticksPerSecond = 30.0f;
clip.looping = true;
clip.loopStart = 0.25f;      // пропускаем вступление
clip.loopEnd = 1.75f;        // EffectiveDuration() == 1.5
```

### `struct AnimationClip::JointTrack`

Дорожка одной кости: индекс кости, три трека и три флага наличия каналов.
Треки rotation/translation/scale независимы: невыставленный флаг означает, что
канал не трогается и сохраняет bind-позу.

| Поле | Смысл |
|---|---|
| `int joint` | индекс кости в скелете |
| `Vec3Track translation` | ключи локального смещения |
| `QuatTrack rotation` | ключи локального поворота |
| `Vec3Track scale` | ключи локального масштаба |
| `bool hasTranslation`, `hasRotation`, `hasScale` | какие каналы реально анимируются |

> **Ловушка.** Недостаточно вызвать `translation.AddKey(...)`: семплирование
> сработает только при выставленном `hasTranslation`. Ключи без флага молча
> игнорируются.

```cpp
crossrender::AnimationClip::JointTrack t;
t.joint = 1;
t.hasRotation = true;                                  // без флага канал не сработает
t.rotation.AddKey(0.0f, crossrender::Quat::Identity());
t.rotation.AddKey(0.5f, crossrender::Quat::FromAxisAngle(crossrender::Vec3{0, 0, 1}, 0.4f));
```

### Поля `tracks`, `events`, `applyRootMotion`, `rootJoint`

Содержимое клипа: список дорожек, список событий и объявленные, но не
реализованные поля извлечения корневого движения. `applyRootMotion` и
`rootJoint` сохраняются в структуре, однако `Animator` их не читает: чтобы
получить корневое смещение, берите мировую матрицу корневой кости через
`Animator::JointWorld`.

```cpp
crossrender::AnimationClip clip = crossrender::AnimationClip::MakeBounce(1.0f);
clip.events.push_back({0.5f, "Impact", ""});
clip.applyRootMotion = false;      // задел: реализацией не используется
clip.rootJoint = 0;

const int trackCount = static_cast<int>(clip.tracks.size());
```

### `int FindTrack(int joint) const`

Индекс дорожки для кости или `-1`. Линейный поиск по `tracks`.

```cpp
crossrender::AnimationClip clip = crossrender::AnimationClip::MakeWalk(4, 1.0f);
const int i = clip.FindTrack(2);
if (i >= 0) ENG_LOGI("anim", "у кости 2 есть дорожка, ключей: %d",
                     static_cast<int>(clip.tracks[static_cast<crossrender::usize>(i)].rotation.keys.size()));
```

### `f32 EffectiveDuration() const`

Рабочая длительность: `loopEnd - loopStart`, если границы заданы и
положительны, иначе `duration`. Именно её используют `Sample`, `Animator` и
режимы проигрывания.

```cpp
crossrender::AnimationClip clip;
clip.duration = 3.0f;
clip.loopStart = 1.0f;
clip.loopEnd = 2.5f;
const crossrender::f32 d = clip.EffectiveDuration();   // 1.5
```

### `void Sample(f32 time, std::vector<Mat4>* locals, std::vector<Vec3>* translations = nullptr, std::vector<Quat>* rotations = nullptr, std::vector<Vec3>* scales = nullptr) const`

Главный метод клипа: заполняет позу в момент `time`. Векторы расширяются до
нужного размера, существующие значения сохраняются, а для костей из дорожек
соответствующие каналы **заменяются**. В `locals` для каждой дорожки пишется
`Translate * Rotation * Scale`; кости без дорожек в `locals` не трогаются.

**Передавайте все четыре вектора, предварительно заполнив их bind-позой** —
ровно так делает `Animator`. Вызов только с `locals` соберёт локальные матрицы
анимированных костей от нулевого смещения (см. ловушку в обзоре).

```cpp
crossrender::Skeleton skel = crossrender::Skeleton::MakeChain(4);
crossrender::AnimationClip clip = crossrender::AnimationClip::MakeWalk(skel.JointCount(), 1.0f);

const int n = skel.JointCount();
std::vector<crossrender::Vec3> t(static_cast<crossrender::usize>(n));
std::vector<crossrender::Quat> r(static_cast<crossrender::usize>(n));
std::vector<crossrender::Vec3> s(static_cast<crossrender::usize>(n));
std::vector<crossrender::Mat4> locals(static_cast<crossrender::usize>(n));
for (int i = 0; i < n; ++i) {                       // сеем bind-позу
    const crossrender::Joint& j = skel.JointAt(i);
    t[static_cast<crossrender::usize>(i)] = j.localTranslation;
    r[static_cast<crossrender::usize>(i)] = j.localRotation;
    s[static_cast<crossrender::usize>(i)] = j.localScale;
}
clip.Sample(0.35f, &locals, &t, &r, &s);
```

### `void CollectEvents(f32 from, f32 to, std::vector<const AnimationEvent*>* out) const`

Собирает события в полуинтервале `(from, to]`, то есть ровно один раз за
пересечение. Для зацикленного клипа диапазон разрезается по шву цикла, а если
кадр накрыл целый цикл и больше, возвращаются все события в порядке
пересечения. `out` очищается в начале. Возвращаются указатели **внутрь**
`events` клипа, поэтому клип должен жить дольше результата.

```cpp
crossrender::AnimationClip clip;
clip.duration = 1.0f;
clip.looping = true;
clip.events.push_back({0.25f, "Step", ""});
clip.events.push_back({0.75f, "Step", ""});

std::vector<const crossrender::AnimationEvent*> crossed;
clip.CollectEvents(0.2f, 0.8f, &crossed);      // оба события
```

### `static AnimationClip MakeWave(f32 duration = 2.0f)`

Готовая синусоида: кость `0` ездит вверх-вниз по `Y` с амплитудой `0.5` на 16
линейных ключах. Неположительная длительность заменяется на `2` секунды.

```cpp
crossrender::AnimationClip wave = crossrender::AnimationClip::MakeWave(4.0f);
ENG_LOGI("anim", "wave: %.1f c, дорожек %d", wave.duration,
         static_cast<int>(wave.tracks.size()));
```

### `static AnimationClip MakeBounce(f32 duration = 1.5f)`

Отскок: кость `0` прыгает по `Y` и «плющится» масштабом на шести ключах.
Неположительная длительность заменяется на `1.5`.

```cpp
crossrender::AnimationClip bounce = crossrender::AnimationClip::MakeBounce();
animator.PlayAdditive(animator.AddClip(bounce), 0.6f, 1);
```

### `static AnimationClip MakeWalk(int jointCount, f32 duration = 1.0f)`

Походка для `jointCount` костей: каждой кости даётся поворот из двух
гармоник со сдвигом фазы `0.7` на кость, корневой кости — ещё и лёгкое
вертикальное покачивание. Число костей меньше единицы поднимается до `1`.

```cpp
crossrender::Skeleton rig = crossrender::Skeleton::MakeHumanoid();
crossrender::AnimationClip walk = crossrender::AnimationClip::MakeWalk(rig.JointCount(), 1.2f);
const int walkClip = animator.AddClip(walk);
```

### `static AnimationClip MakeSpin(f32 duration = 1.0f)`

Вращение кости `0` вокруг оси Y на полный оборот. Ключи стоят через четверть
оборота — так каждый `Slerp` идёт по короткой дуге и не «схлопывается».
Неположительная длительность заменяется на `1`.

```cpp
crossrender::AnimationClip spin = crossrender::AnimationClip::MakeSpin(0.8f);
animator.Play(animator.AddClip(spin), 0.1f, 0, true, crossrender::AnimPlayMode::Loop);
```

### crossrender::Pose

Поза скелета: локальные TRS, локальные матрицы и мировые матрицы. Все векторы
имеют длину, равную числу костей. `Pose` — рабочий тип `Animator`
(`CurrentPose`), но его можно использовать и отдельно: засемплировать клип,
смешать две позы утилитами и посчитать мировые матрицы.

```cpp
crossrender::Pose pose;
pose.Resize(rig.JointCount());
pose.Reset();
pose.ComputeWorld(rig);
```

### Поля `translations`, `rotations`, `scales`, `locals`, `world`

Пять параллельных массивов. TRS — источник истины для `Pose::ComputeWorld`,
`locals` — собранные локальные матрицы, `world` — заполняется только
`ComputeWorld`. Мутируя TRS вручную, вызывайте `ComputeWorld` перед чтением
`world`.

| Поле | Смысл |
|---|---|
| `std::vector<Vec3> translations` | локальные смещения костей |
| `std::vector<Quat> rotations` | локальные повороты |
| `std::vector<Vec3> scales` | локальные масштабы |
| `std::vector<Mat4> locals` | локальные матрицы (`Translate * Rotation * Scale`) |
| `std::vector<Mat4> world` | мировые матрицы иерархии |

```cpp
crossrender::Pose pose;
pose.Resize(2);
pose.translations[1] = crossrender::Vec3{0, 1, 0};
pose.rotations[1] = crossrender::Quat::FromAxisAngle(crossrender::Vec3{0, 0, 1}, 0.5f);
pose.scales[1] = crossrender::Vec3{1, 1, 1};
pose.ComputeWorld(rig);
```

### `void Pose::Resize(int jointCount)`

Приводит все пять векторов к нужному размеру. Векторы только растут: уже
лежащие значения сохраняются, новые заполняются значениями по умолчанию
(нулевое смещение, единичный поворот, единичный масштаб, единичные матрицы).
Отрицательное значение трактуется как ноль.

```cpp
crossrender::Pose pose;
pose.Resize(14);                              // поза под Humanoid
ENG_LOGI("anim", "костей в позе: %d", pose.JointCount());
```

### `int Pose::JointCount() const`

Число костей в позе — размер `rotations`. По нему `Animator` сверяет позу со
скелетом.

```cpp
crossrender::Pose pose;
pose.Resize(3);
if (pose.JointCount() != rig.JointCount()) pose.Resize(rig.JointCount());
```

### `void Pose::Reset()`

Обнуляет позу: смещения в ноль, повороты в единицу, масштабы в единицу,
матрицы в единичные. Это **не** bind-поза скелета — для bind-позы служит
`Animator::Reset` или ручное заполнение из `Joint`.

```cpp
pose.Reset();
pose.ComputeWorld(rig);                       // все кости в начале координат
```

### `void Pose::ComputeWorld(const Skeleton& skeleton)`

Считает `world` по иерархии: кости обходятся так, что родитель всегда раньше
ребёнка, затем `world[i] = world[parent] * locals[i]`. Если `locals[i]` ещё
единичная, а TRS непустые, локальная матрица собирается из TRS на лету — это
совместимость с позами, заполненными только через TRS. Длина обхода —
`min(locals.size(), skeleton.JointCount())`.

```cpp
crossrender::Pose pose;
pose.Resize(rig.JointCount());
pose.Reset();
pose.ComputeWorld(rig);

const crossrender::Mat4& head = pose.world[static_cast<crossrender::usize>(rig.FindJoint("Head"))];
ENG_LOGI("anim", "мировая высота головы: %.2f", head.at(3, 1));
```

### `struct BlendWeights`

Покостный вес для маскированного смешивания: `0` — берём значение из опорной
позы, `1` — из целевой. Пустой массив всюду трактуется как «вес 1 для всех
костей». Маска применяется в `BlendPosesMasked`, в `AnimBlendMode::Masked` и в
`AnimLayer::mask`.

```cpp
crossrender::BlendWeights mask;                  // по умолчанию массив пуст
if (mask.weights.empty()) ENG_LOGI("anim", "маска не ограничивает кости");
```

### Поле `weights`

Единственное поле структуры: покостные веса. Длина массива должна совпадать с
числом костей скелета — за это отвечает `Resize`. Если массив короче позы,
недостающие кости трактуются как «вес 1», а пустой массив означает «затронуты
все кости».

| Поле | Смысл |
|---|---|
| `std::vector<f32> weights` | вес каждой кости: `0` — опорная поза, `1` — целевая |

```cpp
crossrender::BlendWeights mask;
mask.Resize(rig.JointCount(), 0.0f);
mask.weights[static_cast<crossrender::usize>(rig.FindJoint("Head"))] = 1.0f;
```

### `void BlendWeights::Resize(int jointCount, f32 value = 1.0f)`

Задаёт размер маски и заполняет все элементы одним значением. Значение по
умолчанию `1.0` означает «маска ни на что не влияет»; для маски по именам
костей `Animator::RebuildLayerMasks` вызывает `Resize(count, 0.0f)` и затем
поднимает единицы выбранным костям.

```cpp
crossrender::BlendWeights mask;
mask.Resize(rig.JointCount(), 0.0f);          // по умолчанию ничего не берём
mask.weights[0] = 1.0f;                        // кроме корневой кости
```

### `enum class AnimBlendMode : u8`

Режим наложения позы слоя на накопленный результат.

| Значение | Смысл |
|---|---|
| `Override` | интерполяция к целевому семплу; обычный режим базы |
| `Additive` | семпл трактуется как bind-относительная дельта и композитится сверху |
| `Multiply` | объявлен, но в текущей реализации выполняется как `Additive` |
| `Masked` | `Override` только для костей, выбранных `BlendWeights` |

```cpp
crossrender::AnimLayer layer;
layer.blendMode = crossrender::AnimBlendMode::Override;   // база
layer.weight = 1.0f;
```

### `void BlendPoses(Pose* out, const Pose& a, const Pose& b, f32 t, AnimBlendMode mode = AnimBlendMode::Override)`

Смешивает две позы в `out`. `t` зажимается в `[0, 1]`. Для `Additive` и
`Multiply` вызов перенаправляется в `AdditivePose` (то есть `Multiply` ведёт
себя как сложение). Для `Masked` маски в сигнатуре нет, поэтому режим
вырождается в обычный `Override` — для маски вызывайте `BlendPosesMasked`.
Смещение и масштаб интерполируются, поворот — через `Slerp` по короткой дуге.
`out` может совпадать с `a`.

```cpp
crossrender::Pose idle, walk, out;
idle.Resize(rig.JointCount());
walk.Resize(rig.JointCount());
crossrender::BlendPoses(&out, idle, walk, 0.3f);        // 30% походки
```

### `void BlendPosesMasked(Pose* out, const Pose& a, const Pose& b, const BlendWeights& weights)`

«Override с маской»: для каждой кости берётся вес из `weights` (пустая или
короткая маска трактуется как `1`), по нему смешиваются TRS и пересобирается
локальная матрица. `out` расширяется только по числу костей `a`.

```cpp
crossrender::BlendWeights upperBody;
upperBody.Resize(rig.JointCount(), 0.0f);
for (const char* bone : {"Spine", "Chest", "Head", "LeftUpperArm", "RightUpperArm"}) {
    const int j = rig.FindJoint(bone);
    if (j >= 0) upperBody.weights[static_cast<crossrender::usize>(j)] = 1.0f;
}
crossrender::BlendPosesMasked(&out, idle, wave, upperBody);   // машет только верхом
```

### `void AdditivePose(Pose* out, const Pose& base, const Pose& additive, f32 weight)`

Композитит аддитивную позу поверх опорной. API не получает скелет, поэтому
`additive` **уже должен быть bind-относительной локальной дельтой** (её bind —
единичное локальное преобразование). Вес зажимается в `[0, 1]`:

* смещение: `out = base + additive * weight`;
* поворот: `out = base * Slerp(identity, additive, weight)`;
* масштаб: `out = base * Lerp(1, additive, weight)`.

Нулевая дельта — идеальный no-op.

```cpp
crossrender::Pose base, delta, result;
base.Resize(rig.JointCount());
delta.Resize(rig.JointCount());
delta.translations[0] = crossrender::Vec3{0, 0.1f, 0};   // подъём корня на 10 см
crossrender::AdditivePose(&result, base, delta, 1.0f);
```

### `void NormalizePose(Pose* pose)`

Нормализует все кватернионы позы и пересобирает `locals`. Полезно после ручных
операций над поворотами, которые могли накопить численную ошибку.

```cpp
crossrender::Pose pose;
pose.Resize(rig.JointCount());
pose.rotations[0] = pose.rotations[0] * 1.0001f;   // накопили ошибку длины
crossrender::NormalizePose(&pose);
```

### `void MirrorPose(Pose* pose, const Skeleton& skeleton)`

Зеркалит позу относительно плоскости YZ: смещение `x → -x`, поворот
`(x, y, z, w) → (x, -y, -z, w)`, масштаб не меняется. Перед зеркалированием
лево/правые кости меняются значениями: имена сопоставляются по парам
`Left`/`Right`, `left`/`right`, `_L`/`_R`, `.L`/`.R`, ` L`/` R`. Обрабатываются
только первые `min(pose.JointCount(), skeleton.JointCount())` костей.

```cpp
// Тот же шаг, но с другой ноги: зеркалим позу перед записью в клип.
crossrender::Pose step;
step.Resize(rig.JointCount());
crossrender::MirrorPose(&step, rig);
```

### `enum class AnimPlayMode : u8`

Режим проигрывания слоя.

| Значение | Смысл |
|---|---|
| `Once` | играет до конца и останавливается на последнем кадре с `finished = true` |
| `Loop` | зациклен, время заворачивается в `[loopStart, loopStart + dur)` |
| `PingPong` | ходит вперёд-назад; полный цикл равен `2 * dur`, события в обратном проходе летят в обратном порядке |
| `ClampForever` | как `Once`, но явно удерживает последний кадр |

```cpp
crossrender::Animator animator;
animator.Play(0, 0.2f, 0, true, crossrender::AnimPlayMode::Once);
```

### `struct AnimLayer`

Один слой воспроизведения: что играет, как быстро, с каким весом и как
смешивается. Поля публичные, но менять их напрямую стоит осторожно: список
слоёв возвращается как `const` через `Animator::Layers`, а создать слои с
нужным числом заранее позволяет `EnsureLayerCount`.

| Поле | Смысл |
|---|---|
| `std::string name` | имя слоя (`Base`, `Layer1`, …) |
| `int clipIndex` | индекс клипа в аниматоре; `-1` — слой пуст |
| `f32 time` | текущее время слоя (внутреннее представление режима) |
| `f32 speed` | множитель скорости; отрицательный играет назад |
| `f32 weight` | устойчивый вес слоя |
| `AnimPlayMode mode` | режим проигрывания |
| `AnimBlendMode blendMode` | способ смешивания |
| `bool enabled` | выключенный слой пропускается |
| `f32 fadeIn`, `fadeOut`, `fadeTimer` | параметры рамп кроссфейда |
| `bool fadingOut`, `finished` | состояние затухания и завершения |
| `BlendWeights mask` | покостная маска (пустая — все кости) |
| `std::vector<std::string> boneMaskNames` | имена костей маски; разворачиваются в `mask` методом `RebuildLayerMasks` |

```cpp
crossrender::AnimLayer layer;
layer.name = "UpperBody";
layer.clipIndex = 2;
layer.weight = 0.8f;
layer.mode = crossrender::AnimPlayMode::Loop;
layer.blendMode = crossrender::AnimBlendMode::Masked;
layer.boneMaskNames = {"Spine", "Chest", "Head"};
```

### crossrender::Animator

Проигрыватель клипов: владеет списком клипов, набором слоёв и текущей позой.
Порядок работы:

1. `SetSkeleton(&rig)` — привязать скелет (аниматор хранит указатель).
2. `AddClip(clip)` — загрузить клипы, запомнить индексы.
3. `Play(...)` / `PlayAdditive(...)` / `PlayMasked(...)` — запустить слои.
4. `Update(dt)` раз в кадр; отсюда же приходят события.
5. `CurrentPose()` / `WorldMatrices()` / `JointWorld(i)` — забрать результат.

```cpp
crossrender::Animator animator;
animator.SetSkeleton(&rig);
const int idle = animator.AddClip(crossrender::AnimationClip::MakeWave(2.0f));
animator.Play(idle, 0.0f);
animator.Update(1.0f / 60.0f);
```

### `Animator()`

Конструктор по умолчанию: аниматор без скелета, без клипов и без слоёв. До
`SetSkeleton` вызов `Update` ничего не делает.

```cpp
crossrender::Animator animator;
ENG_ASSERT(animator.ClipCount() == 0);
ENG_ASSERT(animator.GetSkeleton() == nullptr);
```

### `void SetSkeleton(Skeleton* skeleton)`

Привязывает скелет и сбрасывает состояние: `Reset` строит bind-позу, создаётся
хотя бы один слой, пересобираются маски. Указатель не копируется — скелет
должен жить дольше аниматора и не двигаться в памяти (`std::vector<Skeleton>`
с реаллокацией — плохая идея).

```cpp
crossrender::Skeleton rig = crossrender::Skeleton::MakeHumanoid();
crossrender::Animator animator;
animator.SetSkeleton(&rig);
ENG_ASSERT(animator.GetSkeleton() == &rig);
```

### `Skeleton* GetSkeleton() const`

Текущий скелет или `nullptr`. Нужен, чтобы строить маски и проверять, что поза
соответствует скелету.

```cpp
crossrender::Animator animator;
if (animator.GetSkeleton() == nullptr) {
    ENG_LOGW("anim", "аниматор без скелета: Update будет пустым");
}
```

### `int AddClip(AnimationClip clip)`

Забирает клип по значению (`std::move` внутрь) и возвращает его индекс.
Индексы стабильны до конца жизни аниматора, но `PlayBlendSpace1D` добавляет
клипы сам — не полагайтесь на «последний индекс», храните возвращённые.

```cpp
crossrender::Skeleton rig = crossrender::Skeleton::MakeHumanoid();
crossrender::Animator animator;
animator.SetSkeleton(&rig);
const int run = animator.AddClip(crossrender::AnimationClip::MakeWalk(rig.JointCount(), 0.8f));
animator.Play(run);
```

### `int ClipCount() const`

Сколько клипов сейчас у аниматора. Растёт при каждом `AddClip` и при каждом
`PlayBlendSpace1D` (там печётся новый клип).

```cpp
crossrender::Animator animator;
ENG_LOGI("anim", "клипов: %d", animator.ClipCount());
```

### `AnimationClip& Clip(int i)` / `const AnimationClip& Clip(int i) const`

Доступ к клипу по индексу без проверки границ. Индекс обязан быть в
`[0, ClipCount())`.

```cpp
const int index = animator.AddClip(crossrender::AnimationClip::MakeSpin(1.0f));
animator.Clip(index).looping = false;      // доиграть один раз
```

### `int FindClip(const std::string& name) const`

Индекс клипа с указанным именем или `-1`. Сравнение точное, линейное.

```cpp
const int index = animator.FindClip("Walk");
if (index < 0) ENG_LOGW("anim", "клип Walk не добавлен");
```

### `void Play(int clipIndex, f32 fadeSeconds = 0.2f, int layer = 0, bool restart = true, AnimPlayMode mode = AnimPlayMode::Loop)`

Запускает клип на слое. Если на слое уже что-то играло и `fadeSeconds > 0`,
текущая поза снимается в буфер кроссфейда, а новый слой получает рампу
`fadeIn`. Недействительный индекс логируется и игнорируется. Побочные эффекты:
режим смешивания сбрасывается в `Override`, маска и её имена очищаются, слой
включается, `fadingOut` и `finished` снимаются. При `restart` время
сбрасывается на `loopStart`.

```cpp
crossrender::Animator animator;
const int idle = animator.AddClip(crossrender::AnimationClip::MakeWave(2.0f));
const int run = animator.AddClip(crossrender::AnimationClip::MakeWalk(12, 0.9f));

animator.Play(idle, 0.0f);
// ... позже, за 0.3 секунды переходим на бег:
animator.Play(run, 0.3f, 0, true, crossrender::AnimPlayMode::Loop);
```

### `void Stop(int layer = 0, f32 fadeSeconds = 0.0f)`

Останавливает слой. При `fadeSeconds <= 0` слой выключается сразу и его
`clipIndex` сбрасывается в `-1`; при положительном значении запускается
затухание, по завершении которого слой выключится в `Update`.

```cpp
animator.Stop(1, 0.25f);       // плавно убрать аддитивный слой за 0.25 c
```

### `void Pause(bool paused)` / `bool Paused() const`

Ставит аниматор на паузу и снимает её. На паузе `Update` получает `dt = 0`:
время слоёв не идёт, события не летят, но поза по-прежнему пересчитывается.

```cpp
animator.Pause(true);
animator.Update(1.0f / 60.0f);      // время не сдвинулось
animator.Pause(false);
```

### `void SetLayerWeight(int layer, f32 weight)` / `f32 LayerWeight(int layer) const`

Задаёт и читает устойчивый вес слоя. Отрицательный вес поднимается до нуля;
несуществующий слой при чтении даёт `0`, при записи — создаётся через
`EnsureLayerCount`. Вес вступает в силу со следующего `Update`.

```cpp
animator.SetLayerWeight(1, 0.35f);
ENG_LOGI("anim", "вес слоя 1: %.2f", animator.LayerWeight(1));
```

### `void SetLayerSpeed(int layer, f32 speed)`

Множитель скорости слоя: `2` — вдвое быстрее, `0.5` — вдвое медленнее,
отрицательное значение играет клип назад. Знак учитывается в подсчёте событий
(`PingPong` рассылает их в обратном порядке).

```cpp
animator.SetLayerSpeed(0, -1.0f);      // проиграть клип в обратную сторону
```

### `void PlayAdditive(int clipIndex, f32 weight, int layer = 1)`

Запускает клип как аддитивный слой: клип зацикливается, время сбрасывается в
ноль, вес задаётся явно, режим смешивания — `Additive`. Клип понимается как
bind-относительная дельта, поэтому клип, стоящий на bind-позе, ничего не
меняет. Слой по умолчанию — `1`, то есть поверх базы. Кроссфейда здесь нет, а
маска и её имена очищаются.

```cpp
const int bounce = animator.AddClip(crossrender::AnimationClip::MakeBounce(1.2f));
animator.PlayAdditive(bounce, 0.5f, 1);       // 50% отскока поверх базы
```

### `void PlayMasked(int clipIndex, const std::vector<std::string>& bones, f32 weight, int layer = 1)`

Запускает клип с маской по именам костей: затрагиваются только перечисленные
кости, остальные сохраняют накопленную позу. Внутри сохраняются имена,
выставляется `AnimBlendMode::Masked`, и сразу вызывается `RebuildLayerMasks`.
Режим — `Loop`, время с нуля, без кроссфейда. Неизвестные имена молча
игнорируются.

```cpp
const int wave = animator.AddClip(crossrender::AnimationClip::MakeWave(2.0f));
animator.PlayMasked(wave, {"Spine", "Chest", "Head"}, 1.0f, 1);
```

### `void PlayBlendSpace1D(const std::vector<int>& clipIndices, const std::vector<f32>& thresholds, f32 parameter, f32 fadeSeconds = 0.15f, int layer = 0)`

Одномерное пространство смешивания: клипы и их пороги. Параметр за пределами
крайних порогов просто выбирает крайний клип; между порогами `i` и `i + 1`
печётся новый клип `lerp(A, B, t)` и играется как обычный. Если клип один или
размеры списков не совпали, играется первый клип.

> **Ловушка.** Каждый вызов создаёт и добавляет новый клип; аниматор,
> вызывающий этот метод каждый кадр, будет бесконечно растить `ClipCount()`.
> Запекайте переходы только при смене параметра.

```cpp
const int walk = animator.AddClip(crossrender::AnimationClip::MakeWalk(12, 1.0f));
const int run  = animator.AddClip(crossrender::AnimationClip::MakeWalk(12, 0.7f));
const int all  = animator.AddClip(crossrender::AnimationClip::MakeWalk(12, 0.5f));

// speedNorm — игровой параметр: 0 = шаг, 0.5 = бег, 1 = спринт.
const crossrender::f32 speedNorm = 0.6f;
animator.PlayBlendSpace1D({walk, run, all}, {0.0f, 0.5f, 1.0f}, speedNorm, 0.2f, 0);
```

### `void SetLayerTime(int layer, f32 time)`

Принудительно ставит время слоя с учётом режима: `Once` и `ClampForever`
зажимают в `[base, base + d]`, `PingPong` заворачивает в `[base, base + 2d)`,
`Loop` — в `[base, base + d)`. Удобно для «перемотки» в нужный кадр и для
синхронизации нескольких слоёв. Если клип не назначен, время просто
сохраняется как есть.

```cpp
// Перемотать базовый слой на середину клипа.
animator.SetLayerTime(0, animator.Clip(0).EffectiveDuration() * 0.5f);
```

### `f32 LayerTime(int layer) const`

Текущее время слоя. Для `PingPong` возвращается уже преобразованное
пинг-понг время в диапазоне `[base, base + d]`, а не внутреннее время цикла.
Для несуществующего слоя — `0`.

```cpp
const crossrender::f32 t = animator.LayerTime(0);
ENG_LOGI("anim", "время слоя 0: %.3f c", t);
```

### `f32 LayerNormalizedTime(int layer) const`

Время слоя, нормированное в `[0, 1]` относительно `EffectiveDuration`. Если
длительность нулевая, возвращается `0`. Удобно для прогресс-баров и для
синхронизации с музыкой.

```cpp
const crossrender::f32 u = animator.LayerNormalizedTime(0);
progressBar.w = barWidth * u;
```

### `bool LayerFinished(int layer) const`

`true`, если слой в режиме `Once` / `ClampForever` дошёл до конца. Для
зацикленных режимов флаг не поднимается, а у несуществующего слоя всегда
`false`. Флаг сбрасывается новым `Play` и `Reset`.

```cpp
animator.Play(attack, 0.1f, 0, true, crossrender::AnimPlayMode::Once);
// ... в следующем кадре:
if (animator.LayerFinished(0)) animator.Play(idle, 0.2f);
```

### `void Update(f32 dt)`

Главный шаг: продвигает время слоёв, считает веса, семплирует клипы, смешивает
их в `pose_`, рассылает события и пересчитывает мировые матрицы. Отрицательный
`dt` поднимается до нуля; на паузе `dt` обнуляется. `fired_` очищается в
начале. Без скелета или при нулевом числе костей метод выходит сразу, оставляя
позу как есть.

Порядок внутри: сброс позы в bind → по слоям (затухание, продвижение времени,
события, семплирование, смешивание) → одноразовая правка IK → `ComputeWorld`.

```cpp
crossrender::Clock clock;                              // crossrender/core/Time.h
while (running) {
    clock.Tick();
    animator.Update(clock.Delta());
    for (const crossrender::AnimationEvent* e : animator.FiredEvents()) {
        if (e->name == "Footstep") PlayFootstepSound();
    }
}
```

### `const Pose& CurrentPose() const` / `Pose& MutablePose()`

Текущая поза после последнего `Update`. `MutablePose` даёт неконстантный
доступ для постобработки (например добавления тряски), но после правки
`world` остаётся старым: вызовите `pose.ComputeWorld(*animator.GetSkeleton())`,
если он нужен сразу.

```cpp
crossrender::Pose& pose = animator.MutablePose();
pose.rotations[0] = pose.rotations[0] * crossrender::Quat::FromAxisAngle(crossrender::Vec3{0, 1, 0}, 0.02f);
pose.ComputeWorld(*animator.GetSkeleton());
```

### `const Mat4* WorldMatrices() const`

Указатель на непрерывный массив мировых матриц текущей позы — то, что нужно
скиннингу. Массив живёт до следующего `Update` / `Reset`; при пустой позе
указатель может быть нулевым, поэтому проверяйте размер через
`CurrentPose().JointCount()`.

```cpp
const crossrender::Mat4* bones = animator.WorldMatrices();
const int jointCount = animator.CurrentPose().JointCount();
for (int i = 0; i < jointCount; ++i) UploadBoneMatrix(i, bones[i]);
```

### `const Mat4& JointWorld(int joint) const`

Мировая матрица одной кости. Для недопустимого индекса возвращается статическая
единичная матрица — ссылка всегда валидна.

```cpp
const int head = rig.FindJoint("Head");
const crossrender::Mat4& m = animator.JointWorld(head);
const crossrender::Vec3 headPos{m.at(3, 0), m.at(3, 1), m.at(3, 2)};
ENG_LOGI("anim", "голова в мире: (%.2f, %.2f, %.2f)", headPos.x, headPos.y, headPos.z);
```

### `const std::vector<const AnimationEvent*>& FiredEvents() const`

События, пересечённые за последний `Update`, в порядке срабатывания. Вектор
очищается в начале каждого `Update`, а указатели смотрят внутрь клипов.

```cpp
animator.Update(dt);
for (const crossrender::AnimationEvent* e : animator.FiredEvents()) {
    ENG_LOGI("anim", "событие '%s' (%s)", e->name.c_str(), e->payload.c_str());
}
```

### `void SetEventCallback(std::function<void(const AnimationEvent&)> cb)`

Подписка на события: колбэк вызывается в момент срабатывания внутри `Update`
(в дополнение к накоплению в `FiredEvents`). Удобно, когда не хочется
разбирать список после шага.

```cpp
animator.SetEventCallback([&](const crossrender::AnimationEvent& e) {
    if (e.name == "Sound") audio.Play(e.payload);
});
```

### `void SetIkTarget(int joint, const Vec3& worldPos, f32 weight)`

Одноразовая IK-подобная правка: после смешивания аниматор сдвигает локальное
смещение кости так, чтобы её мировая позиция приблизилась к `worldPos` на долю
`weight`. Цель хранится до `ClearIkTargets` или `Reset` и применяется **каждый**
`Update`, так что для разового смещения цель нужно снять. Вес зажимается в
`[0, 1]`. Это не полноценный IK-решатель: позиция одной кости подтягивается за
один проход, родители не пересчитываются.

```cpp
const int foot = rig.FindJoint("LeftFoot");
animator.SetIkTarget(foot, crossrender::Vec3{0.2f, 0.0f, 0.1f}, 0.8f);
animator.Update(dt);
```

### `void ClearIkTargets()`

Снимает все IK-цели. Вызывайте, когда персонаж перестал стоять на неровной
поверхности, иначе правка будет применяться и дальше.

```cpp
animator.ClearIkTargets();
animator.Update(dt);          // поза снова чисто анимационная
```

### `void Reset()`

Сбрасывает аниматор в исходное состояние: очищает события и IK-цели, приводит
позу к bind-позе скелета, сбрасывает время слоёв и флаги затухания. Слои с
назначенным клипом остаются включёнными.

```cpp
animator.Reset();
ENG_ASSERT(animator.FiredEvents().empty());
```

### `void RebuildLayerMasks()`

Перестраивает покостные маски всех слоёв из `boneMaskNames`. Слою без имён
маска очищается (значит «все кости»); остальным маска заполняется нулями, а
найденным костям ставится `1`. Вызывайте после смены скелета или правки имён.

```cpp
animator.EnsureLayerCount(2);
// Имена костей маски задаёт PlayMasked; здесь только пересборка из уже заданных имён.
animator.RebuildLayerMasks();
ENG_LOGI("anim", "слоёв %d, маска верхнего слоя пересобрана", animator.LayerCount());
```

### `const std::vector<AnimLayer>& Layers() const`

Список слоёв только для чтения — для отладочных панелей и диагностики.
Изменять слои следует методами аниматора.

```cpp
for (const crossrender::AnimLayer& layer : animator.Layers()) {
    ENG_LOGI("anim", "слой '%s': вес %.2f, включён %d", layer.name.c_str(), layer.weight,
             layer.enabled ? 1 : 0);
}
```

### `int LayerCount() const`

Текущее число слоёв. Слои создаются неявно: `Play`, `SetLayerWeight`,
`PlayAdditive` и другие методы вызывают `EnsureLayerCount`.

```cpp
ENG_LOGI("anim", "слоёв: %d", animator.LayerCount());
```

### `void EnsureLayerCount(int count)`

Гарантирует, что слоёв не меньше `count`, создавая недостающие. Первый слой
называется `Base`, остальные — `Layer1`, `Layer2`, … Отрицательное значение
трактуется как ноль; существующие слои не трогаются.

```cpp
animator.EnsureLayerCount(4);                 // создать Base, Layer1..Layer3
ENG_LOGI("anim", "слоёв теперь: %d", animator.LayerCount());
```

### `enum class EaseType : u8`

Семейства кривых easing. Все кривые возвращают ровно `0` при `t <= 0` и ровно
`1` при `t >= 1`, даже «перелетающие» `Back` и `Elastic`: перелёт возможен
только внутри интервала.

| Значение | Смысл |
|---|---|
| `Linear` | равномерно |
| `InQuad`, `OutQuad`, `InOutQuad` | квадратичные |
| `InCubic`, `OutCubic`, `InOutCubic` | кубические |
| `InQuart`, `OutQuart`, `InOutQuart` | четвёртой степени |
| `InSine`, `OutSine`, `InOutSine` | синусоидальные |
| `InExpo`, `OutExpo`, `InOutExpo` | экспоненциальные |
| `InBack`, `OutBack`, `InOutBack` | с перелётом назад |
| `InElastic`, `OutElastic`, `InOutElastic` | упругие колебания |
| `InBounce`, `OutBounce`, `InOutBounce` | отскок |
| `InCirc`, `OutCirc`, `InOutCirc` | круговые |

```cpp
crossrender::EaseType ease = crossrender::EaseType::OutCubic;
const crossrender::f32 v = crossrender::ApplyEase(ease, 0.25f);
```

### `f32 ApplyEase(EaseType type, f32 t)`

Значение кривой в точке `t`. Вход не зажимается, но выход на концах точный:
`t <= 0` даёт `0`, `t >= 1` — `1`. Для `Bounce` используется стандартная
четырёхсегментная аппроксимация, для `Back` и `Elastic` — общепринятые
константы.

```cpp
for (crossrender::EaseType e : {crossrender::EaseType::Linear, crossrender::EaseType::OutBack}) {
    ENG_LOGI("anim", "ease(%d, 0.5) = %.3f", static_cast<int>(e), crossrender::ApplyEase(e, 0.5f));
}
```

### `f32 ApplyEase01(EaseType type, f32 t)`

То же, но вход предварительно зажимается в `[0, 1]` через `Clamp`. Именно эту
функцию использует `Tween::Update`, чтобы прогресс не выходил за диапазон.

```cpp
const crossrender::f32 raw = elapsed / duration;      // может быть чуть больше 1
const crossrender::f32 eased = crossrender::ApplyEase01(crossrender::EaseType::InOutQuad, raw);
```

### crossrender::Spring

Пружина с положением и скоростью: подшаговый полунеявный метод Эйлера, до
32 подшагов не длиннее `1/60` секунды. Устойчива при переменном `dt` и не
зависит от частоты кадров — предпочтительна для «живых» интерфейсных
элементов, которые должны догонять меняющуюся цель.

| Поле | Смысл |
|---|---|
| `f32 value` | текущее значение |
| `f32 velocity` | текущая скорость |
| `f32 target` | цель, к которой стремится пружина |
| `f32 stiffness` | жёсткость (по умолчанию `180`) |
| `f32 damping` | демпфирование (по умолчанию `22`) |

```cpp
crossrender::Spring scale;
scale.target = 1.1f;
scale.stiffness = 220.0f;
scale.damping = 26.0f;
```

### `void Spring::Update(f32 dt)`

Интегрирует пружину на шаг `dt`. При `dt <= 0` не делает ничего. Когда
значение и скорость становятся очень малы (порог `1e-5` и `1e-4`), пружина
прищёлкивается к цели и обнуляет скорость — так избегается бесконечное
«дрожание» на последних микрометрах.

```cpp
crossrender::Spring spr;
spr.target = 100.0f;
spr.Update(1.0f / 60.0f);
ENG_LOGI("anim", "пружина: %.2f (скорость %.2f)", spr.value, spr.velocity);
```

### `void Spring::Snap(f32 v)`

Мгновенно ставит значение и цель в `v`, обнуляя скорость. Полезно при
телепорте объекта: без этого пружина будет «долетать» через пол-экрана.

```cpp
crossrender::Spring spr;
spr.target = 100.0f;
spr.Snap(100.0f);              // без анимации, сразу в цели
```

### crossrender::Tween

Одноразовый скалярный переход с задержкой и easing: интерполирует одно число
от `from` до `to` за `duration` секунд и сигналит о завершении. Для нескольких
величин заводите несколько `Tween` (или `Spring`), а не один на всех.

```cpp
crossrender::Tween fade;
fade.To(0.0f, 1.0f, 0.4f, crossrender::EaseType::OutQuad, 0.1f);   // старт через 0.1 c
```

### `void Tween::To(f32 from, f32 to, f32 duration, EaseType ease, f32 delay = 0.0f)`

Настраивает переход и запускает его. Отрицательная задержка поднимается до
нуля, время внутри становится отрицательным (значит «ждём задержку»), значение
сразу ставится в `from`, флаг завершения снимается. Вызов поверх идущего
перехода просто заменяет его.

```cpp
crossrender::Tween t;
t.To(0.0f, 250.0f, 0.35f, crossrender::EaseType::OutBack);   // вылет карточки
```

### `void Tween::Update(f32 dt)`

Продвигает переход на `dt * speed`. Пока время отрицательное, значение
удерживается на `from` (это фаза задержки). При `duration <= 0` переход
завершается мгновенно: значение становится `to`, флаг `finished` поднимается.
После завершения метод ничего не делает до `Restart` или нового `To`.

> Осторожно с отрицательной скоростью: при `elapsed < 0` и `speed < 0` переход
> считается завершённым — так «откат назад» не зависает в задержке.

```cpp
crossrender::Tween t;
t.To(0.0f, 1.0f, 0.5f, crossrender::EaseType::InOutSine);
while (!t.Finished()) t.Update(1.0f / 60.0f);
```

### `f32 Tween::Value() const` / `bool Tween::Finished() const`

Текущее значение и признак завершения. До первого `To` значение равно `0`, а
`Finished()` возвращает `false`.

```cpp
crossrender::Tween t;
t.To(1.0f, 0.0f, 0.2f, crossrender::EaseType::Linear);
t.Update(0.2f);
ENG_LOGI("anim", "значение %.2f, завершено %d", t.Value(), t.Finished() ? 1 : 0);
```

### `void Tween::Restart()`

Перезапускает последний `To` с начала, сохраняя задержку, длительность и
кривую. Значение возвращается к `from`, флаг завершения снимается.

```cpp
crossrender::Tween pulse;
pulse.To(0.0f, 1.0f, 0.25f, crossrender::EaseType::OutQuad);
pulse.Update(0.25f);
pulse.Restart();               // проиграть ещё раз
```

### `void Tween::SetSpeed(f32 s)`

Множитель течения времени перехода: `2` — вдвое быстрее, `0` — заморозка,
отрицательное значение — назад. Влияет только на последующие `Update`.

```cpp
crossrender::Tween t;
t.To(0.0f, 1.0f, 1.0f, crossrender::EaseType::Linear);
t.SetSpeed(2.0f);              // завершится за 0.5 c
```

## Пример целиком

```cpp
#include "crossrender/anim/Anim.h"
#include "crossrender/core/Log.h"

#include <cmath>
#include <utility>
#include <vector>

// Небольшая сцена: робот ходит, изредка взмахивает рукой (аддитивный слой),
// а «дыхание» корпуса делается пружиной поверх скелетной анимации.
class RobotScene {
public:
    void Init() {
        rig_ = crossrender::Skeleton::MakeHumanoid();
        animator_.SetSkeleton(&rig_);

        walkClip_ = animator_.AddClip(crossrender::AnimationClip::MakeWalk(rig_.JointCount(), 1.0f));
        waveClip_ = animator_.AddClip(crossrender::AnimationClip::MakeWave(2.0f));

        animator_.Play(walkClip_, 0.0f, 0, true, crossrender::AnimPlayMode::Loop);
        animator_.SetEventCallback([this](const crossrender::AnimationEvent& e) {
            if (e.name == "Footstep") ENG_LOGI("robot", "шаг (%s)", e.payload.c_str());
        });

        // Вес аддитивного слоя ведёт пружина — получится мягкое «включение» руки.
        wave_.target = 0.0f;
    }

    void Update(crossrender::f32 dt) {
        wave_.Update(dt);
        animator_.SetLayerWeight(1, wave_.value);
        animator_.Update(dt);

        // Корневая кость дышит: правку делаем поверх готовой позы.
        crossrender::Pose& pose = animator_.MutablePose();
        const int hips = rig_.FindJoint("Hips");
        if (hips >= 0) {
            const crossrender::usize u = static_cast<crossrender::usize>(hips);
            pose.translations[u].y += std::sin(breathe_) * 0.01f;
            pose.ComputeWorld(rig_);
        }
        breathe_ += dt * 2.0f;
    }

    void Wave(bool on) { wave_.target = on ? 1.0f : 0.0f; }

    const crossrender::Mat4* Bones() const { return animator_.WorldMatrices(); }
    int BoneCount() const { return animator_.CurrentPose().JointCount(); }

private:
    crossrender::Skeleton rig_;
    crossrender::Animator animator_;
    int walkClip_ = -1;
    int waveClip_ = -1;
    crossrender::Spring wave_;
    crossrender::f32 breathe_ = 0.0f;
};

// Вспомогательная функция: собрать клип вручную из ключей.
crossrender::AnimationClip MakeNod(crossrender::f32 duration, int headJoint) {
    crossrender::AnimationClip clip;
    clip.name = "Nod";
    clip.duration = duration;
    clip.looping = true;
    clip.loopStart = 0.0f;
    clip.loopEnd = duration;
    clip.events.push_back({duration * 0.5f, "Nod", ""});

    crossrender::AnimationClip::JointTrack t;
    t.joint = headJoint;
    t.hasRotation = true;                                   // без флага канал молчит
    t.rotation.AddKey(0.0f, crossrender::Quat::Identity(), crossrender::Interpolation::Linear);
    t.rotation.AddKey(duration * 0.5f,
                      crossrender::Quat::FromAxisAngle(crossrender::Vec3{1, 0, 0}, 0.25f),
                      crossrender::Interpolation::Linear);
    t.rotation.AddKey(duration, crossrender::Quat::Identity(), crossrender::Interpolation::Linear);
    clip.tracks.push_back(std::move(t));
    return clip;
}

int main() {
    RobotScene scene;
    scene.Init();

    // Клип «кивок» можно смешать с базовой походкой как ещё один слой.
    const crossrender::AnimationClip nod = MakeNod(1.0f, scene.BoneCount() > 0 ? 4 : 0);

    // Твин для интерфейсной плашки — не нужен клип ради одного числа.
    crossrender::Tween banner;
    banner.To(0.0f, 1.0f, 0.5f, crossrender::EaseType::OutBack);

    for (int frame = 0; frame < 120; ++frame) {
        const crossrender::f32 dt = 1.0f / 60.0f;
        if (frame == 30) scene.Wave(true);
        if (frame == 90) scene.Wave(false);

        scene.Update(dt);
        banner.Update(dt);
    }

    ENG_LOGI("robot", "кадров отрисовано: %d, костей %d, клип '%s' длится %.2f c", 120,
             scene.BoneCount(), nod.name.c_str(), nod.EffectiveDuration());
    return 0;
}
```

## См. также

* `docs/core/Math.md` — `Vec3`, `Quat`, `Mat4` и `CubicBezierEase`, на которых
  построены треки и позы.
* `docs/core/Log.md` — макросы `ENG_LOGI` / `ENG_LOGW`, которыми модуль
  сообщает о некорректных индексах и родителях костей.
* `docs/anim/Lottie.md` — векторная анимация Lottie: другой формат, другой
  проигрыватель, но те же принципы ключей и easing.
* `docs/gfx/Renderer3D.md` — скиннинг и передача `WorldMatrices()` в GPU.
