# crossrender/core/Math.h — векторы, матрицы, кватернионы, цвет и прямоугольник

Единственный математический заголовок движка: типы `Vec2`/`Vec3`/`Vec4`,
матрица `Mat4`, кватернион `Quat`, цвет `Color`, прямоугольник `Rect`,
2D-трансформ `Transform2D`, ограничивающий объём `Bounds`, генератор
случайных чисел `Random` и свободные функции для интерполяции, ограничения,
отражения, пересечения луча и сглаживания анимаций.

## Заголовок

```cpp
#include "crossrender/core/Math.h"
```

## Обзор

`Math.h` не содержит ни одной виртуальной функции, ни одного владения
ресурсом и не зависит ни от чего, кроме `crossrender/core/Base.h` и стандартной
библиотеки. Все типы — простые агрегаты: их можно копировать, класть в
`std::vector`, возвращать по значению и передавать через границу кадра.
Большинство операций помечено `constexpr` или `inline`, поэтому тривиальные
вызовы (`Dot`, `Length`, `Lerp`) компилятор разворачивает на месте.

Заголовок покрывает весь путь данных кадра — от позиции вершины до байтов
пикселя:

```text
ввод/физика -> Vec2/Vec3 -> Transform2D или Mat4::TRS -> Mat4 (proj * view * model)
            -> Quat для поворотов -> Rect для UI -> Color для пикселя
```

**Соглашения.** Их не нужно угадывать — они зафиксированы во всём движке и
проверены тестами `tests/test_core.cpp`:

* **Матрицы — column-major.** `Mat4` хранит 16 чисел в одном массиве `m`;
  элемент с индексами `(col, row)` лежит в `m[col * 4 + row]`, для читаемости
  есть `at(col, row)`. Трансляция — это последний столбец: `m[12]`, `m[13]`,
  `m[14]`. `data()` отдаёт указатель на 16 подряд идущих `f32`, поэтому
  матрицу можно без копии отдать в `glUniformMatrix4fv` с `transpose =
  GL_FALSE`.
* **Порядок умножения — `proj * view * model`.** Вектор-столбец стоит справа:
  `M * v`. В произведении `A * B` первой к вектору применяется `B`.
  `TransformPoint` принимает точку (`w = 1`) и делит на `w` — так работает
  перспектива; `TransformDir` принимает направление (`w = 0`) и не делит.
* **Все углы — в радианах.** Ни одна функция этого заголовка не принимает
  градусы: `Mat4::RotateX/Y/Z`, `Mat4::Rotate`, `Mat4::RotateEuler`,
  `Quat::FromAxisAngle`, `Quat::FromEuler`, `Vec2::Rotate`, `SmoothStep` и
  остальные работают в радианах. Градусы переводят только `crossrender::Radians()` и
  `crossrender::Degrees()` из `docs/core/Base.md`. Исключение по смыслу — `Color::HSL`,
  где тон задаётся долей оборота в `[0, 1)`, а не углом.
* **Система координат — правая (right-handed).** `Cross(X, Y) == Z`, ось +Y
  направлена вверх, камера смотрит вдоль **−Z**: `LookAt` и `Perspective`
  строят стандартную OpenGL-матрицу. Диапазон глубины — `[-1, 1]`; ближняя
  плоскость даёт `z = -1`, дальняя `z = +1`, reverse-Z не используется.
* **`Quat` — это `(x, y, z, w)`**, где `w` — скалярная (вещественная) часть.
  Кватернион по умолчанию и `Quat::Identity()` равны `{0, 0, 0, 1}`.
  `FromAxisAngle` сам нормализует ось, угол в радианах; вращение — против
  часовой стрелки, если смотреть с конца вектора оси.
* **`FromMat4` и `ToMat4` — обратная пара.** `Quat::FromMat4(q.ToMat4())`
  воспроизводит тот же поворот, что и `q`, но знак может оказаться
  противоположным: `q` и `-q` — это один и тот же поворот. Для сравнения
  берите `std::fabs(Dot(a, b))` — у совпадающих поворотов он равен 1.
  Основывается `FromMat4` только на верхнем левом `3x3`, масштаб из матрицы
  игнорируется.
* **Порядок Эйлера — XYZ (extrinsic).** `Mat4::RotateEuler(e)` равно
  `RotateZ(e.z) * RotateY(e.y) * RotateX(e.x)`: для вектора-столбца поворот
  применяется сначала вокруг X, затем вокруг Y, затем вокруг Z.
  `Quat::FromEuler(e)` даёт ровно тот же поворот (матрицы совпадают с
  точностью до погрешности `f32`), поэтому угол можно хранить и как `Vec3`, и
  как `Quat` без расхождений.
* **`Rect` хранит `x, y, w, h`** — левый верхний угол и размер, а не
  `min`/`max`. Начало координат 2D — левый верхний угол экрана, ось **+Y
  направлена вниз**: `Mat4::Ortho2D(width, height)` отображает `(0, 0)` в
  левый верхний угол NDC, а `(width, height)` — в правый нижний. Проверка
  попадания `Contains` использует полуинтервал `[x, x+w) x [y, y+h)`, поэтому
  соседние тайлы не перекрываются на общей границе.
* **`Color` — RGBA из `f32` в диапазоне `[0, 1]`** (значения «линейные на
  глаз», без гамма-коррекции). `Color::ToRGBA8()` упаковывает байты как
  `0xAABBGGRR`, то есть в памяти на little-endian они идут в порядке
  `R, G, B, A` — ровно так, как их ждёт `glTexImage2D` с форматом `GL_RGBA`.
* **`Transform2D::ToMat4`** собирает матрицу как `Translate * RotateZ * Scale`,
  то есть масштаб применяется первым, поворот — вторым, перенос — последним.
* **`Random` детерминирован**: это xorshift128+, и один и тот же seed даёт
  одну и ту же последовательность на всех платформах. Отдельного глобального
  генератора в заголовке нет — экземпляр создаёт вызывающий код.
* Типы не имеют оператора вывода в поток и форматирования: строки собирает
  вызывающий код (см. `docs/core/Log.md`).

## Члены класса

Заголовок состоит из нескольких независимых типов, поэтому раздел разбит по
типам: `### crossrender::Vec2`, `### crossrender::Vec3` и так далее. Внутри каждого типа
сначала идут поля и конструкторы, затем операторы и методы, а после них —
свободные функции, перегруженные для этого типа (`Dot`, `Length`, `Lerp`, …).
Свободная функция с разными типами аргументов описана в разделе того типа, для
которого она объявлена.

### crossrender::Vec2

Двумерный вектор из двух `f32`. Используется для позиций UI, координат мыши,
направлений на плоскости и текстурных координат.

```cpp
// Позиция прицела в координатах экрана (0,0 — левый верхний угол).
crossrender::Vec2 crosshair{viewportW * 0.5f, viewportH * 0.5f};
crosshair += crossrender::Vec2{mouse.dx, mouse.dy} * lookSensitivity;
```

### `f32 x`

Первая компонента — координата по горизонтали (вправо).

```cpp
// Не даём панели выехать за правый край экрана.
crossrender::Vec2 panelPos = panel.position;
if (panelPos.x + panel.w > viewportW) panelPos.x = viewportW - panel.w;
```

### `f32 y`

Вторая компонента — координата по вертикали. В 2D-координатах движка она
растёт **вниз**.

```cpp
// Прижимаем подсказку к нижней части экрана.
crossrender::Vec2 hintPos{16.0f, viewportH - hint.h - 16.0f};
```

### `constexpr Vec2() = default`

Конструктор по умолчанию: обе компоненты равны нулю.

```cpp
// Пока геймпад не подключён, смещение стика остаётся нулевым.
crossrender::Vec2 stick;
if (input.HasGamepad()) stick = input.LeftStick();
```

### `constexpr Vec2(f32 x_, f32 y_)`

Конструктор из двух компонент — основной способ задать точку.

```cpp
// Точка спавна игрока — по центру нижней части уровня.
const crossrender::Vec2 spawn{screenW * 0.5f, screenH - 64.0f};
player.position = spawn;
```

### `explicit constexpr Vec2(f32 s)`

Конструктор с одним значением: обе компоненты становятся равны `s`.
Объявлен `explicit`, чтобы `f32` не превращался в вектор неявно.

```cpp
crossrender::Vec2 margin{12.0f};       // 12 по обеим осям
// crossrender::Vec2 bad = 12.0f;      // ошибка компиляции — так и задумано
```

### `constexpr Vec2 operator+(const Vec2& o) const`

Поэлементное сложение. Обычно складывает позицию со смещением.

```cpp
// Смещаем иконку на величину «дрожания» от попадания.
crossrender::Vec2 shaken = icon.position + shakeOffset;
```

### `constexpr Vec2 operator-(const Vec2& o) const`

Поэлементное вычитание. Даёт вектор от `o` к `*this`.

```cpp
// Вектор от игрока к цели — основа для прицеливания.
const crossrender::Vec2 toTarget = enemy.position - player.position;
```

### `constexpr Vec2 operator*(f32 s) const`

Умножение обеих компонент на скаляр — масштабирование и учёт времени кадра.

```cpp
// Перемещение за кадр: скорость, умноженная на dt.
player.position += player.velocity * dt;
```

### `constexpr Vec2 operator*(const Vec2& o) const`

Поэлементное умножение (покомпонентное). Применяется для независимого
масштаба по осям.

```cpp
// Растягиваем спрайт по X и Y независимо.
const crossrender::Vec2 scaled = sprite.size * sprite.scale2D;
```

### `constexpr Vec2 operator/(f32 s) const`

Деление обеих компонент на скаляр.

```cpp
// Средняя точка двух маркеров — делим сумму на их количество.
const crossrender::Vec2 middle = (markerA + markerB) / 2.0f;
```

### `constexpr Vec2 operator-() const`

Унарный минус: разворачивает вектор в противоположную сторону.

```cpp
// Отдача толкает игрока назад, противоположно направлению выстрела.
player.velocity += -shootDir * recoilSpeed;
```

### `Vec2& operator+=(const Vec2& o)`

Прибавляет вектор на месте и возвращает ссылку на себя. Основная операция
интеграции движения.

```cpp
for (const crossrender::Vec2& force : forces) body.position += force * dt;
```

### `Vec2& operator-=(const Vec2& o)`

Вычитает вектор на месте.

```cpp
// Тратим запас топлива на манёвр.
ship.fuel -= crossrender::Vec2{fuelCost.x, fuelCost.y};
```

### `Vec2& operator*=(f32 s)`

Умножает обе компоненты на скаляр на месте. Удобно для затухания.

```cpp
// Трение: скорость плавно гасится каждый кадр.
velocity *= 0.92f;
```

### `constexpr bool operator==(const Vec2& o) const`

Точное поэлементное сравнение. Для вещественных чисел после вычислений
используйте `NearlyEqual` или `Length(a - b)`.

```cpp
if (gridPos == lastGridPos) return;   // клетка не изменилась — не перестраиваем
```

### `constexpr bool operator!=(const Vec2& o) const`

Отрицание `operator==`.

```cpp
if (target != current) { pathDirty = true; }
```

### `constexpr f32 operator[](int i) const`

Доступ к компоненте по индексу: `0` — `x`, `1` — `y`. Нужен для циклов по
осям.

```cpp
// Применяем отскок к каждой оси отдельно.
for (int axis = 0; axis < 2; ++axis)
    if (pos[axis] < levelMin[axis] || pos[axis] > levelMax[axis]) vel[axis] = -vel[axis];
```

### `f32& operator[](int i)`

Не константный доступ по индексу — позволяет писать в компоненту.

```cpp
crossrender::Vec2 uv{0.0f, 0.0f};
uv[0] = static_cast<crossrender::f32>(tileX) / atlasCols;
uv[1] = static_cast<crossrender::f32>(tileY) / atlasRows;
```

### `constexpr f32 Dot(const Vec2& a, const Vec2& b)`

Скалярное произведение. Для единичных векторов даёт косинус угла: `1` —
одинаковое направление, `0` — перпендикуляр, `-1` — противоположное.

```cpp
// Проверяем, смотрит ли турель на цель (в пределах конуса).
const crossrender::Vec2 aim = crossrender::Normalize(target - turret.pos);
if (crossrender::Dot(aim, turret.forward) > 0.9f) turret.Fire();
```

### `constexpr f32 Cross(const Vec2& a, const Vec2& b)`

Псевдоскалярное (двумерное) произведение. Знак показывает, по какую сторону
от `a` лежит `b`: положительный — против часовой стрелки.

```cpp
// Определяем, обходит ли бот игрока слева или справа.
const f32 side = crossrender::Cross(bot.forward, player.pos - bot.pos);
bot.turn = side > 0.0f ? -1 : 1;
```

### `f32 Length(const Vec2& v)`

Длина вектора, `sqrt(Dot(v, v))`. Требует квадратного корня — если нужен
только порядок сравнения, берите `LengthSq`.

```cpp
// Радиус взрыва проверяем по расстоянию до центра.
if (crossrender::Length(unit.pos - blastPos) < blastRadius) unit.TakeDamage(10);
```

### `f32 LengthSq(const Vec2& v)`

Квадрат длины, без квадратного корня. Дешёвый способ сравнить расстояния.

```cpp
// Отбрасываем далёкие объекты, не извлекая корень.
if (crossrender::LengthSq(enemy.pos - cam.pos) > cullRadius * cullRadius) continue;
```

### `Vec2 Normalize(const Vec2& v)`

Возвращает вектор единичной длины. Если вход короче `kEpsilon`, возвращает
`{0, 0}` — деления на ноль не происходит.

```cpp
// Направление отстрела; при совпадении позиций получим безопасный ноль.
const crossrender::Vec2 dir = crossrender::Normalize(target - muzzle);
```

### `Vec2 Lerp(const Vec2& a, const Vec2& b, f32 t)`

Линейная интерполяция: `a + (b - a) * t`. При `t = 0` вернёт `a`, при
`t = 1` — `b`; значения вне `[0, 1]` экстраполируют.

```cpp
// Сглаживание позиции камеры за целью.
camPos = crossrender::Lerp(camPos, target.pos, 0.15f);
```

### `Vec2 Rotate(const Vec2& v, f32 rad)`

Поворачивает вектор на угол в радианах против часовой стрелки (в системе с
+Y вверх; на экране с +Y вниз знак визуально меняется).

```cpp
// Разворачиваем направление башни вдоль её угла.
const crossrender::Vec2 muzzle = turret.pos + crossrender::Rotate(crossrender::Vec2{0.0f, 1.0f}, turret.angle) * 24.0f;
```

### `Vec2 Perp(const Vec2& v)`

Поворот на 90 градусов против часовой стрелки: `{-y, x}`. Обычно даёт
нормаль к отрезку.

```cpp
// Нормаль к стене для расчёта скольжения вдоль неё.
const crossrender::Vec2 wallNormal = crossrender::Normalize(crossrender::Perp(wallEnd - wallStart));
```

### `Vec2 Min(const Vec2& a, const Vec2& b)`

Покомпонентный минимум. Стандартный способ расширить AABB точкой.

```cpp
aabbMin = crossrender::Min(aabbMin, particle.position);
```

### `Vec2 Max(const Vec2& a, const Vec2& b)`

Покомпонентный максимум — вторая половина расширения AABB.

```cpp
aabbMax = crossrender::Max(aabbMax, particle.position);
```

### `Vec2 Abs(const Vec2& v)`

Покомпонентный модуль. Применяется для симметричных зон и «мёртвых зон»
стика.

```cpp
// Зона нечувствительности стика: реагируем только на заметное отклонение.
if (crossrender::Abs(stick).x > deadZone) move.x = stick.x;
```

### crossrender::Vec3

Трёхмерный вектор из `f32` — рабочая лошадка движка: позиции, направления,
нормали, скорости, цвета в линейном пространстве. Именно `Vec3` принимают
`Mat4::TransformPoint`, `Quat::operator*` и функции пересечения луча.

```cpp
// Смещение врага к игроку за текущий кадр.
const crossrender::Vec3 toPlayer = player.position - enemy.position;
enemy.position += crossrender::Normalize(toPlayer) * enemy.speed * dt;
```

### `f32 x`

Компонента по оси X (вправо в правой системе координат).

```cpp
// Игрок вышел за правую границу арены — возвращаем его назад.
if (player.position.x > arena.max.x) player.position.x = arena.max.x;
```

### `f32 y`

Компонента по оси Y. В 3D-мире **+Y направлена вверх**.

```cpp
// Приземление: не даём персонажу уйти под пол.
if (player.position.y < groundY) player.position.y = groundY;
```

### `f32 z`

Компонента по оси Z. Камера смотрит вдоль **−Z**, поэтому «вперёд» — это
отрицательное приращение `z` в пространстве камеры.

```cpp
// Двигаем объект вглубь сцены вместе с конвейером.
crate.position.z -= conveyorSpeed * dt;
```

### `constexpr Vec3() = default`

Конструктор по умолчанию: все три компоненты равны нулю.

```cpp
// Накопленная за кадр сила: начинаем с нуля.
crossrender::Vec3 force;
for (const auto& spring : springs) force += spring.Pull();
```

### `constexpr Vec3(f32 x_, f32 y_, f32 z_)`

Конструктор из трёх компонент — основной способ задать точку или вектор.

```cpp
// Начальная позиция камеры: чуть выше и позади сцены.
crossrender::Vec3 eye{0.0f, 3.0f, 8.0f};
```

### `explicit constexpr Vec3(f32 s)`

Заполняет все три компоненты одним значением. `explicit` защищает от
случайного неявного преобразования числа в вектор.

```cpp
// Единичный масштаб для матрицы трансформации.
const crossrender::Vec3 one{1.0f};
entity.transform = crossrender::Mat4::TRS(entity.position, entity.eulerRad, one);
```

### `constexpr Vec3(const Vec2& v, f32 z_)`

Поднимает `Vec2` в 3D, добавляя `z`. Конструктор неявный, поэтому `Vec2`
подставляется в функции, ожидающие `Vec3`.

```cpp
// Спрайт из UI превращаем в точку на плоскости z = 0 для физики.
crossrender::Vec2 cursor = input.MousePosition();
crossrender::Vec3 worldPoint{cursor, 0.0f};
```

### `constexpr Vec3 operator+(const Vec3& o) const`

Поэлементное сложение — суммирование сил, смещений и позиций.

```cpp
// Итоговая сила: гравитация плюс тяга двигателя.
const crossrender::Vec3 totalForce = gravity + thrust;
```

### `constexpr Vec3 operator-(const Vec3& o) const`

Поэлементное вычитание; даёт вектор от `o` к `*this`.

```cpp
// Вектор от камеры к объекту — база для проверки видимости.
const crossrender::Vec3 view = mesh.center - camera.position;
```

### `constexpr Vec3 operator*(f32 s) const`

Умножение на скаляр. Самая частая операция — умножение на `dt`.

```cpp
// Интеграция Эйлера: скорость, умноженная на шаг времени.
position += velocity * dt;
```

### `constexpr Vec3 operator*(const Vec3& o) const`

Покомпонентное умножение — независимое масштабирование по осям и тонирование
цвета.

```cpp
// Модулируем цвет вспышки выстрела.
const crossrender::Vec3 muzzleColor = baseColor * flashIntensity;
```

### `constexpr Vec3 operator/(f32 s) const`

Деление всех компонент на скаляр.

```cpp
// Центр облака частиц — среднее арифметическое их позиций.
const crossrender::Vec3 center = sum / static_cast<crossrender::f32>(particles.size());
```

### `constexpr Vec3 operator/(const Vec3& o) const`

Покомпонентное деление. Если в знаменателе может оказаться ноль, проверьте
его заранее: защита внутри оператора не предусмотрена.

```cpp
// Нормируем веса так, чтобы их сумма по осям равнялась единице.
const crossrender::Vec3 weights = raw / (raw + other + crossrender::Vec3{kEpsilon});
```

### `constexpr Vec3 operator-() const`

Унарный минус: меняет направление вектора на противоположное.

```cpp
// Свет падает с противоположной стороны от направления на источник.
const crossrender::Vec3 lightToSurface = -crossrender::Normalize(lightPos - surfacePos);
```

### `Vec3& operator+=(const Vec3& o)`

Добавляет вектор на месте. Основная операция накопления импульса.

```cpp
for (const crossrender::Vec3& impulse : contacts) velocity += impulse;
```

### `Vec3& operator-=(const Vec3& o)`

Вычитает вектор на месте.

```cpp
// Расходуем выносливость на рывок.
stamina -= crossrender::Vec3{dashCost, 0.0f, 0.0f};
```

### `Vec3& operator*=(f32 s)`

Умножает все компоненты на скаляр на месте — затухание и ускорение.

```cpp
// Аэродинамическое торможение в вакууме станции.
velocity *= 0.995f;
```

### `constexpr bool operator==(const Vec3& o) const`

Точное поэлементное сравнение. Годится для целочисленных координат и для
проверки «ничего не изменилось»; для результатов вычислений используйте
`NearlyEqual` или `Length`.

```cpp
if (chunkCoord == lastChunkCoord) return;   // чанк тот же, перестройка не нужна
```

### `constexpr bool operator!=(const Vec3& o) const`

Отрицание `operator==`.

```cpp
if (camera.position != savedPosition) dirty = true;
```

### `constexpr f32 operator[](int i) const`

Доступ к компоненте по индексу: `0` — `x`, `1` — `y`, `2` — `z`. Так
пересечение луча и AABB обходит оси одним циклом.

```cpp
int axis = 0;
if (std::fabs(normal.x) < std::fabs(normal.y)) axis = 1;
if (std::fabs(normal[axis]) < std::fabs(normal.z)) axis = 2;
```

### `f32& operator[](int i)`

Не константный доступ по индексу — запись в компоненту.

```cpp
crossrender::Vec3 p;
for (int i = 0; i < 3; ++i) p[i] = rng.Range(-halfSize[i], halfSize[i]);
```

### `constexpr Vec2 xy() const`

Отбрасывает `z` и возвращает первые две компоненты. Нужен, чтобы перевести
3D-точку в экранные или плоскостные 2D-координаты.

```cpp
// Тень на земле — это позиция объекта без высоты.
const crossrender::Vec2 shadowPos = projectile.position.xy();
```

### `constexpr f32 Dot(const Vec3& a, const Vec3& b)`

Скалярное произведение трёхмерных векторов. Через него считают освещение,
углы и проекции.

```cpp
// Диффузное освещение: доля света, падающая на поверхность.
const f32 ndl = crossrender::MaxT(0.0f, crossrender::Dot(surfaceNormal, lightDir));
```

### `constexpr Vec3 Cross(const Vec3& a, const Vec3& b)`

Векторное произведение. Результат перпендикулярен обоим аргументам, а его
направление подчиняется правилу правой руки: `Cross(X, Y) == Z`.

```cpp
// Базис камеры: вправо, вверх и назад.
const crossrender::Vec3 right = crossrender::Cross(forward, worldUp);
const crossrender::Vec3 up = crossrender::Cross(right, forward);
```

### `f32 Length(const Vec3& v)`

Длина вектора. Для сравнения расстояний без корня используйте `LengthSq`.

```cpp
// Наносим урон, если снаряд в радиусе взрыва.
if (crossrender::Length(target - blastCenter) <= blastRadius) target.TakeDamage(damage);
```

### `f32 LengthSq(const Vec3& v)`

Квадрат длины. Основной инструмент отсечения по расстоянию.

```cpp
const f32 cullSq = cullDistance * cullDistance;
if (crossrender::LengthSq(entity.position - camera.position) > cullSq) continue;
```

### `f32 Distance(const Vec3& a, const Vec3& b)`

Расстояние между двумя точками, `Length(b - a)`. Эквивалентно
`Length(a - b)`.

```cpp
// Выбираем уровень детализации модели по дистанции до камеры.
const f32 d = crossrender::Distance(mesh.center, camera.position);
mesh.lod = d < 10.0f ? 0 : (d < 40.0f ? 1 : 2);
```

### `Vec3 Normalize(const Vec3& v)`

Единичный вектор того же направления. Вектор короче `kEpsilon` даёт
`{0, 0, 0}`, поэтому вызывающий код не получает `NaN`.

```cpp
// Нормаль треугольника — из векторного произведения его сторон.
const crossrender::Vec3 n = crossrender::Normalize(crossrender::Cross(b - a, c - a));
```

### `Vec3 Lerp(const Vec3& a, const Vec3& b, f32 t)`

Линейная интерполяция позиций, нормалей и цветов. `t` обычно приходит из
кривой сглаживания.

```cpp
// Пролёт камеры между двумя ключевыми точками анимации.
const crossrender::Vec3 camPos = crossrender::Lerp(shot.start, shot.end, tween.t);
```

### `Vec3 Min(const Vec3& a, const Vec3& b)`

Покомпонентный минимум — расширение AABB точкой.

```cpp
aabb.min = crossrender::Min(aabb.min, vertex.position);
```

### `Vec3 Max(const Vec3& a, const Vec3& b)`

Покомпонентный максимум — вторая половина расширения AABB.

```cpp
aabb.max = crossrender::Max(aabb.max, vertex.position);
```

### `Vec3 Reflect(const Vec3& v, const Vec3& n)`

Отражает вектор `v` относительно плоскости с единичной нормалью `n`:
`v - n * 2 * Dot(v, n)`. Нормаль должна быть нормирована, иначе отражение
исказится.

```cpp
// Рикошет пули от стены.
const crossrender::Vec3 rd = crossrender::Normalize(crossrender::Reflect(ray.dir, hitNormal));
SpawnTracer(hitPoint, rd);
```

### `Vec3 Clamp(const Vec3& v, const Vec3& lo, const Vec3& hi)`

Покомпонентно ограничивает вектор диапазоном `[lo, hi]`.

```cpp
// Держим курсор внутри игрового поля.
const crossrender::Vec3 clamped = crossrender::Clamp(cursor3D, field.min, field.max);
```

### `f32 RaySphere(const Vec3& ro, const Vec3& rd, const Vec3& c, f32 r)`

Пересечение луча с сферой: начало `ro`, направление `rd`, центр `c`, радиус
`r`. Возвращает ближайшее **неотрицательное** `t` или `-1`, если пересечения
нет. `rd` не обязан быть нормированным, но тогда `t` — не расстояние.

```cpp
// Проверяем попадание по врагу, у которого есть сфера попадания.
const f32 t = crossrender::RaySphere(camera.position, aimDir, enemy.center, enemy.hitRadius);
if (t >= 0.0f) ApplyHit(crossrender::Vec3{camera.position + aimDir * t});
```

### `f32 RayPlane(const Vec3& ro, const Vec3& rd, const Vec3& p, const Vec3& n)`

Пересечение луча с плоскостью, проходящей через точку `p` с нормалью `n`.
Возвращает `t` вдоль `rd`; если луч параллелен плоскости (или `rd`
перпендикулярен `n`), возвращает `-1`. В отличие от `RaySphere`, здесь нет
проверки знака: плоскость позади начала луча тоже даст `t`.

```cpp
// Где луч курсора пересекает горизонтальную плоскость на высоте пола.
const f32 t = crossrender::RayPlane(camera.position, cursorRay, floorPoint, up);
const crossrender::Vec3 ground = camera.position + cursorRay * t;
```

### `bool RayAabb(const Vec3& ro, const Vec3& rd, const Vec3& lo, const Vec3& hi, f32* tOut)`

Пересечение луча с axis-aligned bounding box (AABB, осевыровненный
ограничивающий объём) методом slabs. Возвращает `true` при попадании. В
`*tOut` записывается расстояние до входа в объём, а если начало луча уже
внутри — до выхода (`tOut` можно передать как `nullptr`). Луч, направленный
от объёма, даёт `false`, и тогда содержимое `*tOut` не определено —
ориентируйтесь только на возвращаемое значение.

```cpp
// Отсекаем узлы octree: доходим до листа, только если луч задел его AABB.
f32 t = 0.0f;
if (crossrender::RayAabb(ray.origin, ray.dir, node.min, node.max, &t) && t < maxDistance) {
    node.Visit(ray, t);
}
```

### crossrender::Vec4

Четырёхкомпонентный вектор. В движке он встречается там, где нужна однородная
координата (`Vec4(p, 1.0f)`), упакованный цвет (`Color::ToVec4`) или
перспективное деление. Отдельных математических операций у `Vec4` минимум —
только `Dot` и `Lerp`.

```cpp
// Однородная точка: w = 1 включает перенос, перспективу и деление на w.
const crossrender::Vec4 clip = mvp * crossrender::Vec4{worldPos, 1.0f};
```

### `f32 x`

Первая компонента. В однородных координатах — это `x` после умножения на
матрицу.

```cpp
// Проверяем, попала ли вершина в левую половину клип-пространства.
if (clip.x < -clip.w) return Cull;
```

### `f32 y`

Вторая компонента; для цвета — канал зелёного.

```cpp
// Вершинный цвет, посчитанный во фрагментном шейдере.
const crossrender::Vec4 tint{1.0f, 0.8f, 0.6f, 1.0f};
```

### `f32 z`

Третья компонента; для проекции — глубина в диапазоне `[-1, 1]`.

```cpp
// Сравниваем глубину двух фрагментов (обычный, не reverse-Z тест).
if (candidate.z < depthSoFar) depthSoFar = candidate.z;
```

### `f32 w`

Четвёртая компонента. Для точек равна 1, для направлений 0; после умножения
на перспективную матрицу в `w` попадает `-z` пространства вида.

```cpp
// Перспективное деление: переводим клип-пространство в NDC.
const crossrender::Vec3 ndc = clip.xyz() / clip.w;
```

### `constexpr Vec4() = default`

Конструктор по умолчанию: все четыре компоненты равны нулю.

```cpp
// Аккумулятор для усреднения цвета по выборке.
crossrender::Vec4 sum;
for (const crossrender::Vec4& s : samples) sum += s;
```

### `constexpr Vec4(f32 x_, f32 y_, f32 z_, f32 w_)`

Конструктор из четырёх компонент.

```cpp
// Плоскость отсечения в мировых координатах: n.x, n.y, n.z, d.
const crossrender::Vec4 plane{0.0f, 1.0f, 0.0f, -groundY};
```

### `constexpr Vec4(const Vec3& v, f32 w_)`

Дополняет `Vec3` четвёртой компонентой. Неявный — именно так точка
превращается в однородную.

```cpp
// Переводим мировую точку в клип-пространство и делаем перспективное деление.
const crossrender::Vec4 clip = viewProj * crossrender::Vec4(worldPos, 1.0f);
```

### `explicit constexpr Vec4(f32 s)`

Заполняет все четыре компоненты одним значением.

```cpp
// Однородный масштаб уровня детализации для сетки.
const crossrender::Vec4 atlasScale{1.0f / atlasCols};
```

### `constexpr Vec4 operator+(const Vec4& o) const`

Поэлементное сложение.

```cpp
sum += crossrender::Vec4{sample.r, sample.g, sample.b, 1.0f};
```

### `constexpr Vec4 operator-(const Vec4& o) const`

Поэлементное вычитание.

```cpp
// Разница двух однородных точек — направление в мире.
const crossrender::Vec4 delta = targetPoint - originPoint;
```

### `constexpr Vec4 operator*(f32 s) const`

Умножение всех компонент на скаляр.

```cpp
// Масштабируем упакованный цвет целиком, включая альфу.
crossrender::Vec4 faded = color.ToVec4() * fadeAlpha;
```

### `constexpr Vec4 operator*(const Vec4& o) const`

Покомпонентное умножение — модуляция цвета без изменения прозрачности.

```cpp
const crossrender::Vec4 modulated = baseColor.ToVec4() * tint.ToVec4();
```

### `constexpr Vec4 operator/(f32 s) const`

Деление всех компонент на скаляр — обычно на `w` при перспективном делении.

```cpp
crossrender::Vec4 ndc4 = clip / clip.w;
```

### `Vec4& operator+=(const Vec4& o)`

Прибавляет вектор на месте.

```cpp
for (const crossrender::Vec4& contribution : lights) colorSum += contribution * attenuation;
```

### `constexpr bool operator==(const Vec4& o) const`

Точное поэлементное сравнение. Оператора `!=` у `Vec4` нет — используйте
`!(a == b)`.

```cpp
if (cachedPlane == newPlane) return;   // плоскость отсечения не изменилась
```

### `constexpr f32 operator[](int i) const`

Доступ к компоненте по индексу от `0` до `3`. Позволяет единообразно обойти
все оси одной ветвью кода.

```cpp
f32 sum = 0.0f;
for (int i = 0; i < 4; ++i) sum += v[i] * w[i];
```

### `f32& operator[](int i)`

Не константный доступ по индексу — запись в компоненту.

```cpp
// Плоскость отсечения: первые три компоненты — нормаль, четвёртая — смещение.
crossrender::Vec4 plane;
plane[0] = normal.x;
plane[1] = normal.y;
plane[2] = normal.z;
plane[3] = -offset;
```

### `constexpr Vec3 xyz() const`

Отбрасывает `w` — обратное преобразование к `Vec4(Vec3, w)`.

```cpp
// После перспективного деления оставляем только координаты.
const crossrender::Vec4 ndc4 = clip / clip.w;
const crossrender::Vec3 ndc = ndc4.xyz();
```

### `constexpr Vec2 xy() const`

Возвращает первые две компоненты — экранные координаты из NDC.

```cpp
// NDC [-1,1] переводим в пиксели экрана.
const crossrender::Vec2 ndc2 = ndc4.xy();
const crossrender::Vec2 pixel{(ndc2.x * 0.5f + 0.5f) * screenW, (0.5f - ndc2.y * 0.5f) * screenH};
```

### `constexpr f32 Dot(const Vec4& a, const Vec4& b)`

Четырёхмерное скалярное произведение. В однородных координатах оно даёт
знаковое расстояние от точки до плоскости.

```cpp
// Проверяем, по какую сторону плоскости отсечения находится вершина.
const f32 d = crossrender::Dot(crossrender::Vec4{vertex, 1.0f}, clipPlane);
if (d < 0.0f) return Cull;
```

### `Vec4 Lerp(const Vec4& a, const Vec4& b, f32 t)`

Линейная интерполяция всех четырёх компонент — обычно весов скиннинга или
цвета с альфой.

```cpp
// Плавное затухание цвета частицы к прозрачному.
const crossrender::Vec4 c = crossrender::Lerp(particle.color.ToVec4(), crossrender::Vec4{0, 0, 0, 0}, normalizedAge);
```

### crossrender::Mat4

Матрица `4x4` с одинарной точностью — единственный матричный тип движка. Она
хранится **column-major** (см. `## Обзор`) и используется для трансформаций,
вида, проекции и передачи нормалей. Умножение матриц некоммутативно, поэтому
порядок всегда `proj * view * model`: сначала к точке применяется `model`,
затем `view`, затем `proj`.

```cpp
// Полный конвейер кадра для одного объекта.
const crossrender::Mat4 model = crossrender::Mat4::TRS(entity.position, entity.eulerRad, entity.scale);
const crossrender::Mat4 mvp = camera.Proj(aspect) * camera.View() * model;
shader.Set("uMVP", mvp);
```

### `f32 m[16]`

Открытый массив из 16 чисел в порядке столбцов: `m[0..3]` — первый столбец,
`m[12..14]` — перенос. Массив публичный, чтобы матрицу можно было без копии
отдать в графическое API.

```cpp
crossrender::Mat4 model = crossrender::Mat4::Identity();
// Последний столбец — позиция объекта в мире.
model.m[12] = entity.position.x;
model.m[13] = entity.position.y;
model.m[14] = entity.position.z;
// OpenGL ждёт column-major и transpose = GL_FALSE.
glUniformMatrix4fv(loc, 1, GL_FALSE, model.data());
```

### `f32& at(int c, int r)` / `constexpr f32 at(int c, int r) const`

Доступ к элементу по столбцу и строке: `at(c, r) == m[c * 4 + r]`. Читаемее,
чем ручная арифметика индексов, и не зависит от порядка хранения.

```cpp
// Проверяем, что базис ортонормирован: диагональ близка к единице.
const bool orthonormal = crossrender::NearlyEqual(model.at(0, 0), 1.0f, 1e-4f) &&
                         crossrender::NearlyEqual(model.at(1, 1), 1.0f, 1e-4f) &&
                         crossrender::NearlyEqual(model.at(2, 2), 1.0f, 1e-4f);
```

### `const f32* data() const` / `f32* data()`

Указатель на 16 подряд идущих `f32` — для `glUniformMatrix4fv`, `memcpy` и
сохранения в файл.

```cpp
// Копируем матрицу в буфер скелетной анимации.
std::memcpy(&boneMatrices[slot * 16], skinMatrix.data(), 16 * sizeof(crossrender::f32));
```

### `static Mat4 Identity()`

Единичная матрица — значение по умолчанию. Умножение на неё ничего не меняет.

```cpp
// Объект без трансформации: модель совпадает с мировой системой.
entity.model = crossrender::Mat4::Identity();
```

### `static Mat4 Zero()`

Матрица из нулей. Нужна как заготовка под ручное заполнение или как
нейтральный элемент для накопления.

```cpp
// Нулевая матрица — заготовка, которую заполняем вручную.
crossrender::Mat4 basis = crossrender::Mat4::Zero();
basis.at(0, 0) = right.x;   basis.at(1, 0) = right.y;   basis.at(2, 0) = right.z;
basis.at(0, 1) = up.x;      basis.at(1, 1) = up.y;      basis.at(2, 1) = up.z;
basis.at(0, 2) = -fwd.x;    basis.at(1, 2) = -fwd.y;    basis.at(2, 2) = -fwd.z;
basis.at(3, 3) = 1.0f;
```

### `static Mat4 Translate(const Vec3& t)`

Матрица переноса: последний столбец равен `t`, остальное — единица.

```cpp
// Ставим объект в точку спавна, не трогая его поворот.
crossrender::Mat4 m = crossrender::Mat4::Translate(spawnPoint);
```

### `static Mat4 Scale(const Vec3& s)`

Матрица масштаба: диагональ `m[0]`, `m[5]`, `m[10]` равна `s.x`, `s.y`,
`s.z`. Нулевая компонента схлопывает геометрию по этой оси.

```cpp
// Сплющиваем траву по Y, чтобы она росла у самой земли.
const crossrender::Mat4 squash = crossrender::Mat4::Scale({1.0f, 0.8f, 1.0f});
```

### `static Mat4 RotateX(f32 a)`

Поворот вокруг оси X на угол `a` **в радианах**. Для положительного угла
точка `+Y` уходит в `+Z`.

```cpp
// Наклоняем камеру вниз на 30 градусов (угол переводим из градусов).
const crossrender::Mat4 pitch = crossrender::Mat4::RotateX(crossrender::Radians(-30.0f));
```

### `static Mat4 RotateY(f32 a)`

Поворот вокруг оси Y на угол в радианах (рыскание, yaw). Именно так
поворачивают персонажей и камеру в горизонтальной плоскости.

```cpp
// Поворот модели по направлению движения.
const crossrender::Mat4 yaw = crossrender::Mat4::RotateY(std::atan2(dir.x, dir.z));
```

### `static Mat4 RotateZ(f32 a)`

Поворот вокруг оси Z на угол в радианах (крен, roll). В 2D это основной
поворот спрайта.

```cpp
// Наклоняем спрайт самолёта при повороте.
const crossrender::Mat4 roll = crossrender::Mat4::RotateZ(ship.bankAngle);
```

### `static Mat4 Rotate(const Vec3& axis, f32 a)`

Поворот на угол `a` радиан вокруг произвольной оси. Ось внутри
нормализуется; если передать нулевой вектор, поворот вырождается в масштаб
`cos(a)` по всем осям, поэтому ось должна быть ненулевой.

```cpp
// Вращаем вентилятор вокруг наклонной оси.
const crossrender::Mat4 spin = crossrender::Mat4::Rotate(crossrender::Normalize(crossrender::Vec3{0.2f, 1.0f, 0.1f}), angle);
```

### `static Mat4 RotateEuler(const Vec3& e)`

Поворот из углов Эйлера в радианах, порядок XYZ (extrinsic):
`RotateZ(e.z) * RotateY(e.y) * RotateX(e.x)`. Такой же поворот даёт
`Quat::FromEuler(e).ToMat4()`, поэтому оба представления взаимозаменяемы.

```cpp
// Ориентация камеры: рыскание, тангаж и крен из контроллера.
const crossrender::Vec3 euler{look.pitch, look.yaw, look.roll};
const crossrender::Mat4 orientation = crossrender::Mat4::RotateEuler(euler);
```

### `static Mat4 Perspective(f32 fovYRad, f32 aspect, f32 zn, f32 zf)`

Перспективная проекция. `fovYRad` — вертикальный угол обзора **в радианах**,
`aspect` — отношение ширины к высоте, `zn`/`zf` — расстояния до ближней и
дальней плоскостей. Ближняя плоскость отображается в `z = -1`, дальняя в
`z = +1` (OpenGL, диапазон `[-1, 1]`, без reverse-Z).

```cpp
// Проекция камеры для кадра 1280x720.
const crossrender::Mat4 proj = crossrender::Mat4::Perspective(crossrender::Radians(60.0f), 1280.0f / 720.0f, 0.1f, 200.0f);
```

### `static Mat4 Ortho(f32 l, f32 r_, f32 b, f32 t, f32 zn, f32 zf)`

Ортографическая проекция по границам `l`, `r_`, `b`, `t` и глубинам `zn`,
`zf`. Параметры `zn` и `zf` могут быть отрицательными — это обычная ситуация
для 2D.

```cpp
// Ортокамера для изометрического редактора уровней.
const crossrender::Mat4 ortho = crossrender::Mat4::Ortho(-40.0f, 40.0f, -22.0f, 22.0f, -100.0f, 100.0f);
```

### `static Mat4 Ortho2D(f32 width, f32 height)`

Готовая 2D-проекция с началом координат в **левом верхнем углу** и осью +Y
вниз: `Ortho(0, width, height, 0, -1, 1)`. Пиксельные координаты UI совпадают
с координатами мира.

```cpp
// Раскладываем интерфейс по пикселям: (0,0) — левый верхний угол.
crossrender::Mat4 uiProj = crossrender::Mat4::Ortho2D(framebufferW, framebufferH);
crossrender::Vec4 clip = uiProj * crossrender::Vec4{button.x, button.y, 0.0f, 1.0f};
```

### `static Mat4 LookAt(const Vec3& eye, const Vec3& center, const Vec3& up)`

Матрица вида: камера в `eye` смотрит в `center`, вектор `up` задаёт верх
(обычно `{0, 1, 0}`). Камера смотрит вдоль **−Z**, поэтому базис строится из
`f`, `s = Cross(f, up)`, `u = Cross(s, f)`.

```cpp
// Камера от третьего лица следит за игроком и смотрит на него.
const crossrender::Mat4 view = crossrender::Mat4::LookAt(camera.position, player.position, {0.0f, 1.0f, 0.0f});
```

### `Mat4 operator*(const Mat4& o) const`

Произведение матриц. В `A * B` к вектору первой применяется `B`. Порядок
`proj * view * model` обязателен для согласованности с шейдерами движка.

```cpp
// Собираем матрицу модель-вид-проекция для шейдера.
const crossrender::Mat4 mvp = proj * view * model;
shader.Set("uMVP", mvp);
```

### `Vec4 operator*(const Vec4& v) const`

Умножение матрицы на вектор-столбец: `r[rw] = sum_k at(k, rw) * v[k]`. Это
низкоуровневая операция; для точек и направлений удобнее `TransformPoint` и
`TransformDir`.

```cpp
// Копия мировой точки в однородных координатах.
const crossrender::Vec4 clip = viewProj * crossrender::Vec4{worldPos, 1.0f};
```

### `Vec3 TransformPoint(const Vec3& p) const`

Преобразует **точку** (неявный `w = 1`) и делит результат на `w`, если он не
равен нулю. Именно так перспективная проекция переводит мир в NDC.
Рекомендуется для позиций.

```cpp
// Мировая позиция кости — в экранные координаты для прицела UI.
const crossrender::Vec4 clip = viewProj * crossrender::Vec4{bone.worldPos, 1.0f};
const crossrender::Vec3 ndc = clip.xyz() / clip.w;
```

### `Vec3 TransformDir(const Vec3& d) const`

Преобразует **направление** (неявный `w = 0`), перенос не применяется и
деления на `w` нет. Перспективная матрица направления корректно не
преобразует — для них берите видовую часть или `NormalMatrix`.

```cpp
// Нормаль в мире после поворота модели.
const crossrender::Vec3 worldNormal = model.TransformDir(localNormal);
```

### `Mat4 Transposed() const`

Транспонированная матрица. Для ортонормированного поворота транспонирование
совпадает с обращением.

```cpp
// Обратный поворот без вычисления полного обратного произведения.
const crossrender::Mat4 viewInverse = view.Transposed();  // только для поворота без масштаба
```

### `Mat4 Inverse() const`

Полное обращение `4x4` через разложение по кофакторам. Если определитель
близок к нулю (`|det| < 1e-12`), возвращается **единичная** матрица — это
защита от деления на ноль, а не признак корректного обращения. В горячем
цикле обратную матрицу лучше вычислить один раз и переиспользовать.

```cpp
// Переводим точку из мира в локальное пространство объекта (пикинг).
const crossrender::Mat4 invModel = model.Inverse();
const crossrender::Vec3 local = invModel.TransformPoint(worldPoint);
```

### `Mat4 NormalMatrix() const`

Обратная транспонированная матрица для верхнего левого блока `3x3`; четвёртая
строка и столбец обнулены, `m[15] = 1`. Нужна для корректного преобразования
нормалей при неравномерном масштабе.

```cpp
// Нормали нельзя умножать на матрицу модели напрямую при сжатии.
const crossrender::Mat4 normalMat = model.NormalMatrix();
shader.Set("uNormalMatrix", normalMat);
```

### `static Mat4 TRS(const Vec3& t, const Vec3& eulerRad, const Vec3& s)`

Готовая матрица трансформации `Translate * RotateEuler * Scale`: сначала
масштаб, затем поворот, затем перенос. Углы Эйлера — в радианах, порядок
XYZ. Стандартный способ собрать матрицу модели.

```cpp
// Модельная матрица вращающегося и подпрыгивающего ящика.
const crossrender::Mat4 model = crossrender::Mat4::TRS(
    box.position,
    crossrender::Vec3{0.0f, box.yaw, 0.0f},
    crossrender::Vec3{box.halfSize.x * 2.0f, box.halfSize.y * 2.0f, box.halfSize.z * 2.0f});
```

### crossrender::Quat

Кватернион для поворотов: хранит `(x, y, z, w)`, не имеет проблем с
гимбал-локом и корректно интерполируется через `Slerp`. Используйте `Quat`
для ориентаций, которые нужно смешивать (анимация, физика, камера), и `Vec3`
углов Эйлера — для ввода и отображения.

```cpp
// Ориентация врага: доворачиваем его к игроку кратчайшей дугой.
const crossrender::Vec3 toPlayer = crossrender::Normalize(player.position - enemy.position);
const crossrender::Quat target = crossrender::Quat::LookRotation(toPlayer, {0.0f, 1.0f, 0.0f});
enemy.rotation = crossrender::Quat::Slerp(enemy.rotation, target, turnSpeed * dt);
```

### `f32 x`

Компонента векторной части по оси X. Знак определяет направление оси
поворота.

```cpp
// Ось вращения, «вынутая» из кватерниона (для отладки).
const crossrender::Vec3 axis = crossrender::Normalize(crossrender::Vec3{spin.x, spin.y, spin.z});
```

### `f32 y`

Компонента векторной части по оси Y.

```cpp
crossrender::Quat q = crossrender::Quat::FromAxisAngle({0.0f, 1.0f, 0.0f}, yaw);
// Для вращения строго вокруг Y компоненты x и z остаются нулевыми.
if (!crossrender::NearlyEqual(q.x, 0.0f) || !crossrender::NearlyEqual(q.z, 0.0f)) hud.ReportBadYaw();
```

### `f32 z`

Компонента векторной части по оси Z.

```cpp
// Крен самолёта вокруг оси Z: знак z говорит о направлении крена.
const crossrender::Quat roll = crossrender::Quat::FromAxisAngle({0.0f, 0.0f, 1.0f}, bank);
if (roll.z < 0.0f) hud.MarkLeftBank();
```

### `f32 w`

Скалярная (вещественная) часть — косинус половины угла поворота.
`w = 1` соответствует единичному повороту.

```cpp
// Угол поворота в радианах, восстановленный из кватерниона.
const f32 angle = 2.0f * std::acos(crossrender::Clamp(q.w, -1.0f, 1.0f));
```

### `constexpr Quat() = default`

Конструктор по умолчанию даёт **единичный** кватернион `{0, 0, 0, 1}`, а не
нулевой: `Quat{}` ничего не поворачивает.

```cpp
// Накопленный поворот начинаем с «без поворота».
crossrender::Quat accumulated;
for (const crossrender::Quat& delta : frameDeltas) accumulated = accumulated * delta;
```

### `constexpr Quat(f32 x_, f32 y_, f32 z_, f32 w_)`

Конструктор из четырёх компонент. Обычно так восстанавливают кватернион из
файла или сети; для создания поворота по углу есть `FromAxisAngle` и
`FromEuler`.

```cpp
// Кватернион, прочитанный из анимационного канала.
const crossrender::Quat key{0.0f, 0.7071f, 0.0f, 0.7071f};   // 90° вокруг Y
```

### `static Quat Identity()`

Явный единичный кватернион — то же, что `Quat{}`. Удобно как начальное
значение и как нейтральный элемент при смешивании.

```cpp
// Анимация стартует с привязки к скелету без поворота.
pose.rotation = crossrender::Quat::Identity();
```

### `static Quat FromAxisAngle(const Vec3& axis, f32 a)`

Поворот на угол `a` радиан вокруг оси `axis`. Ось нормализуется внутри, угол
не ограничивается диапазоном. Для положительного угла вращение идёт против
часовой стрелки, если смотреть с конца оси.

```cpp
// Вращаем колесо вокруг его локальной оси на 30° в секунду.
wheel.rotation = crossrender::Quat::FromAxisAngle(
    crossrender::Normalize(crossrender::Vec3{0.0f, 0.0f, 1.0f}), wheel.angleRad);
```

### `static Quat FromEuler(const Vec3& e)`

Кватернион из углов Эйлера в радианах, порядок XYZ. Даёт ровно тот же
поворот, что `Mat4::RotateEuler(e)`, поэтому выбор представления не меняет
картинку.

```cpp
// Ориентация камеры из контроллера: тангаж, рыскание, крен.
const crossrender::Quat orientation = crossrender::Quat::FromEuler({look.pitch, look.yaw, look.roll});
```

### `static Quat FromMat4(const Mat4& m)`

Извлекает поворот из верхнего левого блока `3x3` матрицы. Масштаб при этом
игнорируется, поэтому матрица должна быть ортонормированной (поворот без
сжатия). Обратен к `ToMat4`: `FromMat4(q.ToMat4())` даёт тот же поворот, но
может вернуть `-q` — сравнивайте через `std::fabs(Dot(a, b))`.

```cpp
// Ориентация, унаследованная из матрицы узла скелета.
const crossrender::Mat4 bindWorld = bone.parentWorld * bone.localBind;
const crossrender::Quat bindRotation = crossrender::Quat::FromMat4(bindWorld);
```

### `Quat operator*(const Quat& o) const`

Композиция поворотов. `a * b` означает «сначала применить `b`, затем `a`» —
так же, как для матриц. Порядок важен: кватернионы некоммутативны.

```cpp
// Складываем поворот башни с поворотом корпуса танка.
const crossrender::Quat world = tank.hullRotation * turret.localRotation;
```

### `Vec3 operator*(const Vec3& v) const`

Поворачивает вектор. Работает и с направлениями, и с точками (точку
поворачивают вокруг начала координат). Результат совпадает с
`ToMat4().TransformDir(v)`.

```cpp
// Переводим локальное направление ствола в мировое.
const crossrender::Vec3 muzzleDir = turret.rotation * crossrender::Vec3{0.0f, 0.0f, -1.0f};
```

### `Quat Conjugate() const`

Сопряжённый кватернион `{-x, -y, -z, w}` — обратный поворот для единичного
кватерниона. Для ненормированного кватерниона это не полный обратный
элемент: сначала вызовите `Normalized()`.

```cpp
// Переводим мировое направление в локальное пространство башни.
const crossrender::Vec3 local = turret.rotation.Conjugate() * worldDir;
```

### `Quat Normalized() const`

Приводит кватернион к единичной длине. После нескольких умножений и
смешиваний накапливается погрешность, поэтому результат стоит нормировать.
Нулевой кватернион заменяется на `Quat{}`.

```cpp
// После серии умножений нормируем, чтобы не росла ошибка.
entity.rotation = (parent.rotation * local).Normalized();
```

### `Mat4 ToMat4() const`

Преобразует кватернион в матрицу поворота `4x4` (без переноса и масштаба).
Результат согласован с `operator*(Vec3)`.

```cpp
// Отдаём ориентацию в шейдер как часть модельной матрицы.
const crossrender::Mat4 model = crossrender::Mat4::Translate(entity.position) * entity.rotation.ToMat4();
```

### `static Quat Slerp(const Quat& a, Quat b, f32 t)`

Сферическая линейная интерполяция по кратчайшей дуге: если скалярное
произведение отрицательно, `b` инвертируется. При почти совпадающих
кватернионах (`d > 0.9995`) переключается на быстрый `NLerp`. Второй
параметр передаётся по значению, поэтому исходный кватернион не меняется.

```cpp
// Половина пути между двумя ключами анимации поворота.
const crossrender::Quat mid = crossrender::Quat::Slerp(keyA.rotation, keyB.rotation, 0.5f);
```

### `static Quat NLerp(const Quat& a, const Quat& b, f32 t)`

Нормализованная линейная интерполяция: покомпонентный `Lerp`, затем
`Normalized()`. Дешевле `Slerp`, но **не** разворачивает знак: при
отрицательном скалярном произведении интерполяция идёт длинным путём и
проходит через вырождение. Для коротких дуг и почти одинаковых поворотов
разница незаметна.

```cpp
// Быстрое сглаживание ориентации, когда углы заведомо близки.
enemy.rotation = crossrender::Quat::NLerp(enemy.rotation, desired, 0.2f);
```

### `static Quat LookRotation(const Vec3& dir, const Vec3& up)`

Строит поворот, при котором локальная ось **−Z** смотрит вдоль `dir`, а
локальная +Y — вверх вдоль `up`. Если `dir` параллелен `up`, вектор `s`
вырождается и результат теряет смысл — выбирайте `up` не параллельным
направлению.

```cpp
// Разворачиваем прожектор вдоль направления на цель.
const crossrender::Quat aim = crossrender::Quat::LookRotation(crossrender::Normalize(target - lamp.position),
                                              {0.0f, 1.0f, 0.0f});
lamp.rotation = crossrender::Quat::Slerp(lamp.rotation, aim, 0.1f);
```

### crossrender::Color

Цвет в формате RGBA из четырёх `f32` в диапазоне `[0, 1]`. По умолчанию —
непрозрачный белый. Тип хранит значения «как есть», без гамма-коррекции, и
умеет конвертироваться в байты, HEX и HSL.

```cpp
// Подсвечиваем кнопку при наведении курсора.
const crossrender::Color idle = crossrender::Color::FromRGB(0x2E3440);
const crossrender::Color hover = crossrender::Color::FromRGB(0x3B4252);
ui.Button(rect, hovered ? hover : idle);
```

### `f32 r`

Красный канал в `[0, 1]`.

```cpp
// Мигание полосы здоровья: красный растёт по мере потери HP.
healthBar.color.r = 1.0f - hpFraction;
```

### `f32 g`

Зелёный канал в `[0, 1]`.

```cpp
// Полоса здоровья: чем больше HP, тем зеленее.
hpBar.color.g = hpFraction;
```

### `f32 b`

Синий канал в `[0, 1]`.

```cpp
// Ночное освещение подмешивает синеву в цвет тумана.
fogColor.b = crossrender::Clamp(fogColor.b + 0.1f, 0.0f, 1.0f);
```

### `f32 a`

Альфа-канал в `[0, 1]`: `0` — полностью прозрачный, `1` — непрозрачный.

```cpp
// Плавно проявляем подсказку при появлении.
tooltip.color.a = crossrender::SmoothStep(0.0f, 0.25f, hintTimer);
```

### `constexpr Color() = default`

Конструктор по умолчанию: непрозрачный белый `{1, 1, 1, 1}` — безопасное
значение «без тонирования».

```cpp
// Белый цвет ничего не меняет при модуляции текстуры.
crossrender::Color tint;
sprite.Draw(tex, dst, tint);
```

### `constexpr Color(f32 r_, f32 g_, f32 b_, f32 a_ = 1.0f)`

Конструктор из компонент; альфа по умолчанию непрозрачная.

```cpp
// Полупрозрачная чёрная вуаль для паузы.
const crossrender::Color pauseVeil{0.0f, 0.0f, 0.0f, 0.65f};
```

### `explicit constexpr Color(f32 v)`

Оттенки серого: все каналы, включая альфу, равны `v`. Объявлен `explicit`,
чтобы число не становилось цветом неявно.

```cpp
// Серая заливка заглушки для незагруженной текстуры.
const crossrender::Color placeholder{0.5f};
```

### `constexpr Color(const Vec3& v, f32 a_ = 1.0f)`

Берёт RGB из `Vec3`, альфу задаёт отдельно. Удобно при переносе цвета из
линейной математики в UI.

```cpp
// Цвет, посчитанный как вектор освещения, выводим без прозрачности.
const crossrender::Vec3 lit = albedo * lightIntensity;
interior.panel.Draw(crossrender::Color{lit, 1.0f});
```

### `constexpr Color(const Vec4& v)`

Берёт все четыре канала из `Vec4` — обратная операция к `ToVec4`.

```cpp
// Результат смешивания в Vec4 снова становится цветом.
const crossrender::Vec4 blended = crossrender::Lerp(a.ToVec4(), b.ToVec4(), 0.5f);
const crossrender::Color result{blended};
```

### `Vec4 ToVec4() const`

Преобразует цвет в `Vec4` — для векторной математики и `Lerp`.

```cpp
// Линейно смешиваем два цвета по прогрессу загрузки.
const crossrender::Vec4 c = crossrender::Lerp(crossrender::Color::Red.ToVec4(), crossrender::Color::Green.ToVec4(), progress);
```

### `Vec3 rgb() const`

Возвращает только RGB, отбрасывая альфу.

```cpp
// Цвет тумана не должен зависеть от прозрачности материала.
const crossrender::Vec3 fogRgb = skyColor.rgb();
renderer.SetFog(fogRgb, fogDensity);
```

### `Color operator*(f32 s) const`

Умножает все каналы, **включая альфу**, на скаляр. Для затухания вспышек
часто нужен именно такой вариант; если альфу менять нельзя, используйте
`WithAlpha`.

```cpp
// Затухание вспышки выстрела вместе с прозрачностью.
crossrender::Color flash = explosion.color * fade;
```

### `Color operator*(const Color& o) const`

Покомпонентное произведение — модуляция текстуры цветом (tint) и
перемножение фильтров.

```cpp
// Тонируем спрайт цветом команды.
crossrender::Color tinted = texture.sampleColor * team.color;
```

### `Color operator+(const Color& o) const`

Покомпонентное сложение — аддитивное смешивание источников света и
свечения. Результат может выйти за `[0, 1]`: приводите его через `Clamp`
или полагайтесь на ограничение при упаковке в байты.

```cpp
// Складываем вклад двух ламп на поверхности.
const crossrender::Color lit = lampA.contribution + lampB.contribution;
```

### `bool operator==(const Color& o) const`

Точное сравнение всех четырёх каналов. Оператора `!=` у `Color` нет.

```cpp
if (lastClear == crossrender::Color::Black) return;   // цвет очистки не изменился
```

### `Color WithAlpha(f32 na) const`

Копия цвета с новой альфой; каналы RGB не меняются.

```cpp
// Тот же цвет, но полупрозрачный — для подложки диалога.
ui.Panel(dialogRect, theme.surface.WithAlpha(0.9f));
```

### `static Color FromBytes(u8 r_, u8 g_, u8 b_, u8 a_ = 255)`

Собирает цвет из байтов `0..255`, деля их на 255. Альфа по умолчанию
непрозрачная.

```cpp
// Палитра, заданная художником в байтах.
const crossrender::Color leather = crossrender::Color::FromBytes(112, 78, 44);
```

### `static Color FromRGB(u32 hex)` / `static Color FromARGB(u32 hex)`

Разбирает шестнадцатеричный цвет: `FromRGB` — `0xRRGGBB` (альфа
непрозрачная), `FromARGB` — `0xAARRGGBB` (альфа в старшем байте). Отдельные
функции исключают неоднозначность длины литерала.

```cpp
// Цвета темы оформления прямо из макета.
const crossrender::Color accent = crossrender::Color::FromRGB(0x88C0D0);
const crossrender::Color shadow = crossrender::Color::FromARGB(0x80000000);   // полупрозрачная тень
```

### `static Color HSL(f32 h, f32 s, f32 l, f32 a = 1.0f)`

Цвет из тона, насыщенности и светлоты. Тон `h` задаётся **долей оборота** в
`[0, 1)`, а не углом в градусах: `0.0` — красный, `1/3` — зелёный, `2/3` —
синий. `s` и `l` ограничиваются диапазоном `[0, 1]`.

```cpp
// Радужная обводка: тон зависит от индекса игрока.
const crossrender::Color ring = crossrender::Color::HSL(playerIndex * 0.137f, 0.7f, 0.55f);
```

### `void ToHSL(f32* h, f32* s, f32* l) const`

Раскладывает цвет на тон, насыщенность и светлоту. Любой из указателей можно
передать как `nullptr`, если значение не нужно.

```cpp
// Сдвигаем тон выделения, сохраняя насыщенность и светлоту темы.
f32 h = 0.0f, s = 0.0f, l = 0.0f;
theme.accent.ToHSL(&h, &s, &l);
const crossrender::Color shifted = crossrender::Color::HSL(h + 0.5f, s, l);
```

### `u32 ToRGBA8() const`

Упаковывает цвет в 32 бита: каждый канал ограничивается диапазоном `[0, 1]`
и округляется, байты укладываются как `0xAABBGGRR` (младший байт — красный),
то есть в памяти на little-endian порядок `R, G, B, A`.

```cpp
// Цвет очистки кадра в виде, который понимает графическое API.
const u32 clearValue = backgroundColor.ToRGBA8();
glClearColor(backgroundColor.r, backgroundColor.g, backgroundColor.b, backgroundColor.a);
```

### `static const Color White / Black / Transparent / Red / Green / Blue / Yellow / Cyan / Magenta / Gray`

Готовые константы палитры. Все, кроме `Transparent`, непрозрачны; `Gray` —
это `0.5` по всем каналам RGB. Определены в `Math.cpp`, поэтому при линковке
нужен объектный файл движка.

| Константа | RGBA |
|---|---|
| `Color::White` | `1, 1, 1, 1` |
| `Color::Black` | `0, 0, 0, 1` |
| `Color::Transparent` | `0, 0, 0, 0` |
| `Color::Red` | `1, 0, 0, 1` |
| `Color::Green` | `0, 1, 0, 1` |
| `Color::Blue` | `0, 0, 1, 1` |
| `Color::Yellow` | `1, 1, 0, 1` |
| `Color::Cyan` | `0, 1, 1, 1` |
| `Color::Magenta` | `1, 0, 1, 1` |
| `Color::Gray` | `0.5, 0.5, 0.5, 1` |

```cpp
// Отладочная визуализация: зелёный при попадании, красный при промахе.
debug.DrawLine(hit.point, hit.point + hit.normal, hit.found ? crossrender::Color::Green : crossrender::Color::Red);
```

### crossrender::Rect

Прямоугольник для 2D: хранит `x`, `y`, `w`, `h` — левый верхний угол и
размер. Начало координат — левый верхний угол экрана, ось **+Y направлена
вниз**. `Rect` используется интерфейсом, атласами спрайтов, отсечением и
проверкой попадания курсора.

```cpp
// Раскладываем три кнопки в ряд с одинаковыми отступами.
const crossrender::Rect panel{40.0f, 40.0f, 320.0f, 200.0f};
const crossrender::Rect content = panel.Inset(16.0f);
for (int i = 0; i < 3; ++i) {
    const crossrender::Rect button{content.x, content.y + i * 48.0f, content.w, 40.0f};
    ui.Button(button, labels[i]);
}
```

### `f32 x`

Координата левого края — расстояние от левого края экрана.

```cpp
// Центрируем окно по горизонтали.
dialog.x = (viewportW - dialog.w) * 0.5f;
```

### `f32 y`

Координата верхнего края — расстояние от **верхнего** края экрана.

```cpp
// Прижимаем строку состояния к низу окна.
statusBar.y = viewportH - statusBar.h;
```

### `f32 w`

Ширина прямоугольника. Отрицательной быть не должна: методы `Right()`,
`Contains` и `Intersect` рассчитаны на неотрицательный размер.

```cpp
// Панель занимает треть экрана по ширине.
sidebar.w = viewportW / 3.0f;
```

### `f32 h`

Высота прямоугольника, отсчитывается вниз от `y`.

```cpp
// Строка списка одинакова для всех элементов.
const crossrender::f32 rowH = 28.0f;
row.h = rowH;
```

### `constexpr Rect() = default`

Конструктор по умолчанию: пустой прямоугольник `{0, 0, 0, 0}`.

```cpp
// Пока ничего не выделено, область выделения пуста.
crossrender::Rect selection;
if (dragging) selection = crossrender::Rect::FromMinMax(dragStart, mousePos);
```

### `constexpr Rect(f32 x_, f32 y_, f32 w_, f32 h_)`

Конструктор из позиции и размера — основной способ задать прямоугольник.

```cpp
// Область отсечения для прокручиваемого списка.
const crossrender::Rect clipRect{list.x, list.y, list.w, list.h};
r2d.ClipRect(clipRect);
```

### `static Rect FromMinMax(const Vec2& lo, const Vec2& hi)`

Строит прямоугольник по двум углам: `lo` — левый верхний, `hi` — правый
нижний. Удобно для выделения рамкой, когда порядок точек неизвестен.

```cpp
// Выделение мышью: приводим углы к правильному порядку до вызова.
const crossrender::Vec2 lo{crossrender::MinT(anchor.x, cursor.x), crossrender::MinT(anchor.y, cursor.y)};
const crossrender::Vec2 hi{crossrender::MaxT(anchor.x, cursor.x), crossrender::MaxT(anchor.y, cursor.y)};
const crossrender::Rect marquee = crossrender::Rect::FromMinMax(lo, hi);
```

### `f32 Left() const`

Левый край, то же самое, что `x`.

```cpp
// Выравниваем подпись по левому краю кнопки.
text.Draw(label, {button.Left() + 8.0f, button.y});
```

### `f32 Top() const`

Верхний край, то же самое, что `y`.

```cpp
// Верхняя граница списка — начало координат прокрутки.
scrollTop = list.Top() - scrollOffset;
```

### `f32 Right() const`

Правый край, `x + w`.

```cpp
// Прижимаем счётчик очков к правому краю панели.
scoreText.x = panel.Right() - scoreText.w - 8.0f;
```

### `f32 Bottom() const`

Нижний край, `y + h`.

```cpp
// Курсор перетаскивания не выходит за нижнюю границу списка.
if (dragY > list.Bottom()) dragY = list.Bottom();
```

### `Vec2 Min() const`

Левый верхний угол как `Vec2`, `{x, y}`.

```cpp
// Приводим точку к области виджета перед проверкой попадания.
const crossrender::Vec2 local = world - widget.rect.Min();
```

### `Vec2 Max() const`

Правый нижний угол как `Vec2`, `{x + w, y + h}`.

```cpp
// Обрезаем свечение по краям виджета.
const crossrender::Vec2 limit = widget.rect.Max();
glowPos = crossrender::Min(glowPos, limit);
```

### `Vec2 Size() const`

Размер прямоугольника как `Vec2`, `{w, h}`.

```cpp
// Масштабируем иконку под размер слота инвентаря по каждой оси отдельно.
const crossrender::Vec2 fit{slot.rect.w / icon.texture.w, slot.rect.h / icon.texture.h};
icon.size = icon.size * fit;
```

### `Vec2 Center() const`

Центр прямоугольника; для `Rect` с +Y вниз это по-прежнему геометрический
центр.

```cpp
// Ставим иконку ровно в центр слота.
icon.position = slot.rect.Center() - icon.size * 0.5f;
```

### `bool Contains(const Vec2& p) const`

Проверяет попадание точки внутрь полуинтервала `[x, x+w) x [y, y+h)`.
Правая и нижняя границы не включаются, поэтому соседние тайлы не считают
своей общей границей.

```cpp
// Клик по виджету: переводим координаты окна в координаты интерфейса.
if (widget.rect.Contains(mousePos) && input.MousePressed()) widget.OnClick();
```

### `bool Intersects(const Rect& o) const`

Проверяет пересечение двух прямоугольников; касание границами пересечением
**не** считается. Дешёвая замена `Intersect` там, где нужен только факт.

```cpp
// Пропускаем отрисовку виджетов за пределами видимой области.
if (!widget.rect.Intersects(viewRect)) continue;
```

### `Rect Inset(f32 d) const`

Сжимает прямоугольник со всех сторон на `d`; при `d < 0` — расширяет.
Позволяет одной строкой получить содержимое панели с отступами.

```cpp
// Текст внутри рамки с отступом 12 пикселей.
const crossrender::Rect textRect = frame.Inset(12.0f);
```

### `Rect Inset(f32 dx, f32 dy) const`

Сжимает прямоугольник по осям независимо: `dx` по горизонтали, `dy` по
вертикали. Размер уменьшается на `2 * dx` и `2 * dy`.

```cpp
// Отступы разной величины по горизонтали и вертикали.
const crossrender::Rect content = card.Inset(24.0f, 8.0f);
```

### `Rect Offset(f32 dx, f32 dy) const`

Сдвигает прямоугольник, не меняя размер. Применяется для анимации выезда.

```cpp
// Панель выезжает слева: сдвигаем её на прогресс анимации.
const crossrender::Rect sliding = panel.Offset(-panel.w * (1.0f - tween.t), 0.0f);
```

### `Rect Union(const Rect& o) const`

Наименьший прямоугольник, содержащий оба. Нужен для пересчёта грязной
области интерфейса.

```cpp
// Объединяем изменившиеся области в одну для перерисовки.
dirty = dirty.Union(widget.rect);
```

### `Rect Intersect(const Rect& o) const`

Пересечение прямоугольников. Если они не перекрываются, ширина и высота
результата равны нулю (координаты берутся от `max` левых и верхних краёв),
поэтому проверяйте `w > 0 && h > 0`, прежде чем рисовать.

```cpp
// Пересечение области отсечения и виджета — реально видимая часть.
const crossrender::Rect visible = clipRect.Intersect(widget.rect);
if (visible.w > 0.0f && visible.h > 0.0f) DrawWidget(widget, visible);
```

### crossrender::Transform2D

Простой 2D-трансформ: позиция, масштаб и поворот вокруг оси Z. Используется
сценами и виджетами вместо ручной сборки матриц.

```cpp
// Виджет с анимацией появления: масштаб от нуля к единице.
widget.transform.position = anchor;
widget.transform.scale = crossrender::Vec2{pop, pop};
widget.transform.rotation = 0.0f;
r2d.SetTransform(widget.transform.ToMat4());
```

### `Vec2 position`

Смещение в 2D-координатах (обычно в пикселях экрана).

```cpp
// Ставим миникарту в правый верхний угол.
minimap.transform.position = {viewportW - minimapSize - 16.0f, 16.0f};
```

### `Vec2 scale`

Масштаб по осям; по умолчанию `{1, 1}`. Отрицательная компонента отражает
объект по этой оси.

```cpp
// Отражаем спрайт по горизонтали, когда персонаж идёт влево.
sprite.transform.scale = {facingLeft ? -1.0f : 1.0f, 1.0f};
```

### `f32 rotation`

Поворот вокруг оси Z **в радианах**; по умолчанию `0`.

```cpp
// Стрелка компаса поворачивается вместе с игроком.
compass.transform.rotation = -player.yaw;
```

### `Mat4 ToMat4() const`

Собирает матрицу `Translate * RotateZ * Scale`: сначала масштаб, затем
поворот, затем перенос. Готова для передачи в 2D-рендерер.

```cpp
crossrender::Transform2D xf;
xf.position = card.position;
xf.rotation = card.tiltRad;
xf.scale = {card.zoom, card.zoom};
crossrender::Mat4 m = xf.ToMat4();
shader.Set("uTransform", m);
```

### crossrender::Bounds

Осевыровненный ограничивающий объём (AABB) в 3D: пара `min`/`max`. По
умолчанию «пустой»: `min` больше `max`, и `Valid()` возвращает `false`.
Расширяется точками и другими объёмами.

```cpp
// Считаем габариты модели по её вершинам.
crossrender::Bounds bounds;
for (const crossrender::Vec3& v : mesh.vertices) bounds.Expand(v);
mesh.bounds = bounds;
```

### `Vec3 min`

Минимальный угол объёма. До первого `Expand` равен `{1e30, 1e30, 1e30}` —
это «плюс бесконечность» наоборот, чтобы первая же точка его перекрыла.

```cpp
// Быстрая проверка: луч левее объёма — дальше можно не считать.
if (ray.origin.x < bounds.min.x && ray.dir.x <= 0.0f) return;
```

### `Vec3 max`

Максимальный угол объёма. До первого `Expand` равен `{-1e30, -1e30, -1e30}`.

```cpp
// Высота объёма нужна для позиционирования камеры.
const crossrender::f32 height = bounds.max.y - bounds.min.y;
```

### `void Expand(const Vec3& p)`

Расширяет объём так, чтобы точка `p` попала внутрь. Вызывайте для каждой
вершины или частицы.

```cpp
// Объём, охватывающий все частицы системы за текущий кадр.
crossrender::Bounds live;
for (const crossrender::Particle& p : particles) live.Expand(p.position);
```

### `void Expand(const Bounds& b)`

Расширяет объём другим объёмом. Невалидный (пустой) аргумент игнорируется,
поэтому пустой объём можно передавать безопасно.

```cpp
// Объединяем габариты всех узлов скелета в один объём.
crossrender::Bounds whole;
for (const crossrender::Bounds& boneBounds : bones) whole.Expand(boneBounds);
```

### `bool Valid() const`

`true`, если объём непуст (`min.x <= max.x`). Пока ни одна точка не
добавлена, возвращает `false`.

```cpp
// Рисуем отладочный бокс только для непустых объёмов.
if (bounds.Valid()) debug.DrawAabb(bounds.min, bounds.max, crossrender::Color::Cyan);
```

### `Vec3 Center() const`

Геометрический центр объёма. Для невалидного объёма результат бессмысленен —
сначала проверьте `Valid()`.

```cpp
// Фокусируем камеру на середине выделенной группы объектов.
const crossrender::Vec3 focus = selection.Center();
camera.LookAt(focus);
```

### `Vec3 Extents() const`

Половина размера по каждой оси. Удобно для задания радиуса и построения
матрицы масштаба.

```cpp
// Вписываем модель в куб единичного размера.
const crossrender::Vec3 e = model.bounds.Extents();
const crossrender::f32 k = 0.5f / crossrender::MaxT(crossrender::MaxT(e.x, e.y), e.z);
model.normalizeScale = crossrender::Vec3{k};
```

### `f32 Radius() const`

Радиус описанной сферы: половина длины диагонали. Для невалидного объёма
возвращает `0`.

```cpp
// Отсечение по сфере — дешевле, чем по AABB.
const crossrender::f32 r = bounds.Radius();
if (crossrender::Distance(camera.position, bounds.Center()) > r + farPlane) return;
```

### Скалярные функции

Свободные шаблоны и функции для работы с отдельными числами. Они не привязаны
к векторам и применяются в игровой логике, интерфейсе и анимациях.

```cpp
// Типовой кадр интерфейса: ограничение, сглаживание и постоянная скорость.
health = crossrender::Clamp(health - damage, 0.0f, maxHealth);
panelAlpha = crossrender::SmoothStep(0.0f, 0.25f, appearTimer);
cursorX = crossrender::Damp(cursorX, targetX, 12.0f, dt);
doorOpen = crossrender::MoveTowards(doorOpen, 1.0f, doorSpeed * dt);
if (crossrender::NearlyEqual(health, 0.0f)) OnDeath();
```

### `constexpr T Clamp(T v, T lo, T hi)`

Ограничивает значение диапазоном `[lo, hi]`. Если `lo > hi`, побеждает
верхняя граница: проверка идёт последовательно. Работает с любым типом, для
которого определены `<` и `>`, включая целые.

```cpp
// Не даём здоровью уйти в минус и выше максимума.
hp = crossrender::Clamp(hp - damage, 0, maxHp);
```

### `constexpr T MinT(T a, T b)`

Возвращает меньшее из двух значений. Нужна там, где `std::min` мешает
выводить типы или тянет за собой `<algorithm>`.

```cpp
// Берём меньший шаг, чтобы не проскочить цель за кадр.
const crossrender::f32 step = crossrender::MinT(speed * dt, distanceLeft);
```

### `constexpr T MaxT(T a, T b)`

Возвращает большее из двух значений.

```cpp
// Диффузный свет не может быть отрицательным.
const crossrender::f32 ndl = crossrender::MaxT(0.0f, crossrender::Dot(normal, lightDir));
```

### `constexpr T Lerp(T a, T b, f32 t)`

Скалярная линейная интерполяция: `a + (b - a) * t`. Результат приводится к
типу `T`, поэтому `Lerp` годится и для целочисленных шкал.

```cpp
// Плавно меняем громкость музыки при переходе между уровнями.
volume = crossrender::Lerp(volume, targetVolume, 0.05f);
```

### `constexpr T AbsT(T v)`

Модуль значения без приведения к `f32`. Для беззнаковых типов возвращает
аргумент как есть.

```cpp
// Скорость по модулю — для отображения спидометра.
const crossrender::i32 speed = crossrender::AbsT(velocityX);
```

### `f32 SmoothStep(f32 e0, f32 e1, f32 x)`

Классическое сглаживание Хермита: переводит `x` из диапазона `[e0, e1]` в
`[0, 1]` по кривой `3t² - 2t³` с нулевыми производными на концах. В
знаменатель добавлен `kEpsilon`, поэтому `e0 == e1` не даёт деления на ноль.

```cpp
// Плавное появление тумана на подходе к болоту.
const crossrender::f32 fogAmount = crossrender::SmoothStep(swampEdge - 4.0f, swampEdge, player.position.z);
```

### `f32 MoveTowards(f32 cur, f32 target, f32 maxDelta)`

Двигает `cur` к `target` не более чем на `maxDelta` за вызов, без
перескакивания через цель. В отличие от `Lerp`, скорость приближения
постоянна и не зависит от расстояния.

```cpp
// Створки двери открываются с постоянной скоростью.
doorOpen = crossrender::MoveTowards(doorOpen, 1.0f, doorSpeed * dt);
```

### `f32 CubicBezierEase(f32 x1, f32 y1, f32 x2, f32 y2, f32 t)`

Решатель кубической кривой Безье — основа easing-функций интерфейса и
Lottie-анимаций. Управляющие точки задаются как `(x1, y1)` и `(x2, y2)`, вход
`t` — прогресс в `[0, 1]`. Сначала ищется параметр, при котором кривая
проходит через `t` (метод Ньютона с делением пополам в качестве запасного
варианта), затем возвращается ордината. Вне `[0, 1]` результат прижимается к
0 или 1.

```cpp
// Ease-in-out для всплывающего окна.
const crossrender::f32 eased = crossrender::CubicBezierEase(0.42f, 0.0f, 0.58f, 1.0f, tween.t);
dialog.scale = crossrender::Vec2{crossrender::Lerp(0.9f, 1.0f, eased)};
```

### `f32 Damp(f32 cur, f32 target, f32 lambda, f32 dt)`

Экспоненциальное сглаживание: `cur + (target - cur) * (1 - exp(-lambda * dt))`.
В отличие от `Lerp` с постоянным `t`, результат **не зависит от частоты
кадров**, поэтому это правильный выбор для следящей камеры. Чем больше
`lambda`, тем быстрее приближение.

```cpp
// Камера догоняет цель одинаково плавно и при 30, и при 144 кадрах в секунду.
camY = crossrender::Damp(camY, player.position.y + 1.6f, 8.0f, dt);
```

### `bool NearlyEqual(f32 a, f32 b, f32 eps = 1e-5f)`

Сравнивает вещественные числа с допуском по абсолютной разности. Это не
относительное сравнение: для очень больших значений `eps` нужно увеличить.

```cpp
// Проверяем, что анимация действительно дошла до конца.
if (crossrender::NearlyEqual(tween.t, 1.0f)) tween.Finish();
```

### crossrender::Random

Детерминированный генератор случайных чисел на алгоритме xorshift128+.
Один и тот же seed даёт одну и ту же последовательность везде, где
собирается движок. Не является криптостойким; для эффектов, разброса частиц,
процедурной генерации и тестов его достаточно.

```cpp
// Отдельные генераторы для геймплея и для косметики: правки одного
// не сдвигают последовательность другого.
crossrender::Random gameplay(levelSeed);
crossrender::Random cosmetic(0xC0FFEE);
```

### `Random()`

Конструктор по умолчанию использует фиксированный seed
`0x9E3779B97F4A7C15`, поэтому последовательность воспроизводима. Если нужна
разная случайность от запуска к запуску, задайте seed явно.

```cpp
// Воспроизводимый поток для юнит-теста генерации уровня.
crossrender::Random rng;
const int roomCount = rng.RangeInt(4, 9);
```

### `explicit Random(u64 seed)`

Создаёт генератор с заданным seed. Объявлен `explicit`, чтобы число не
превращалось в генератор неявно.

```cpp
// Сид берём из номера забега, чтобы повторить ту же случайность позже.
const crossrender::u64 runSeed = 12345ull;
crossrender::Random rng{runSeed};
```

### `void Seed(u64 seed)`

Переинициализирует состояние генератора. После вызова последовательность
продолжается с начала для этого seed; первые восемь значений прогреваются
внутри, чтобы плохой seed не давал корреляции на старте.

```cpp
// Новый уровень — новый поток случайных чисел.
rng.Seed(levelIndex * 2654435761ull + runIndex);
```

### `u64 NextU64()`

Следующее 64-битное значение. Базовый примитив: остальные методы строятся на
нём.

```cpp
// Хеш для процедурного имени острова.
const crossrender::u64 id = rng.NextU64();
const std::string name = MakeName(id);
```

### `u32 NextU32()`

Следующее 32-битное значение — старшие 32 бита результата `NextU64`.

```cpp
// Раздаём уникальные сетевые идентификаторы сущностей.
const crossrender::u32 netId = rng.NextU32() | 1u;
```

### `f32 NextFloat()`

Случайное число в `[0, 1)` с 24 битами точности. Основа для `Range` и
`Chance`.

```cpp
// Дрожание прицела: небольшое смещение в обе стороны.
const crossrender::f32 sway = (rng.NextFloat() - 0.5f) * 2.0f;
```

### `f32 Range(f32 lo, f32 hi)`

Случайное вещественное число в `[lo, hi)`. Границы можно задавать в любом
порядке.

```cpp
// Разброс урона в пределах 10 процентов.
const crossrender::f32 damage = baseDamage * rng.Range(0.9f, 1.1f);
```

### `i32 RangeInt(i32 lo, i32 hi)`

Случайное целое в **замкнутом** диапазоне `[lo, hi]` — обе границы
включаются.

```cpp
// Выбираем случайную награду из таблицы.
const crossrender::i32 index = rng.RangeInt(0, static_cast<crossrender::i32>(loot.size()) - 1);
```

### `bool Chance(f32 p)`

Возвращает `true` с вероятностью `p`. `p <= 0` никогда не срабатывает,
`p >= 1` срабатывает всегда.

```cpp
// Критический удар с шансом 15 процентов.
if (rng.Chance(0.15f)) damage *= 2;
```

### `Vec2 OnUnitCircle()`

Случайная точка на окружности единичного радиуса — равномерно по углу.
Годится для разлёта частиц и круговых орбит.

```cpp
// Искры разлетаются во все стороны одинаково.
const crossrender::Vec2 dir = rng.OnUnitCircle();
spark.velocity = crossrender::Vec3{dir.x, dir.y, 0.0f} * rng.Range(2.0f, 5.0f);
```

### `Vec3 InUnitSphere()`

Случайная точка внутри единичной сферы (равномерно по объёму). Реализована
отбраковкой: до 16 попыток, после чего возвращается `{0, 0, 0}` — это
практически недостижимо, но не считается ошибкой.

```cpp
// Взрыв раскидывает обломки в пределах сферы.
const crossrender::Vec3 offset = rng.InUnitSphere() * blastRadius;
debris.position = blastCenter + offset;
```

### `template <typename T> void Shuffle(std::vector<T>& v)`

Перемешивает вектор на месте алгоритмом Фишера — Йетса. Требует, чтобы `T`
был перемещаемым (`std::swap`).

```cpp
// Случайный порядок ходов противника в пошаговом режиме.
crossrender::Random rng(turnSeed);
rng.Shuffle(availableMoves);
```

## Пример целиком

```cpp
#include "crossrender/core/Math.h"
#include "crossrender/core/Log.h"

#include <cmath>

// Орбитальная камера: облетает цель, плавно её догоняет и дрожит от взрывов.
struct OrbitCamera {
    crossrender::Vec3 target{0.0f, 1.5f, 0.0f};
    crossrender::Vec3 eye{0.0f, 3.0f, 6.0f};
    crossrender::f32  radius = 6.0f;
    crossrender::f32  yaw = 0.0f;
    crossrender::f32  height = 3.0f;
    crossrender::f32  shake = 0.0f;
};

// Искра от попадания: живёт секунду и гаснет.
struct Spark {
    crossrender::Vec3 position{0.0f, 0.0f, 0.0f};
    crossrender::Vec3 velocity{0.0f, 0.0f, 0.0f};
    crossrender::f32  age = 0.0f;
    crossrender::f32  life = 1.0f;
};

void UpdateSpark(Spark& s, crossrender::f32 dt) {
    s.velocity.y += -9.8f * dt;        // гравитация
    s.position += s.velocity * dt;     // интеграция позиции
    s.age = crossrender::MinT(s.age + dt, s.life);
}

crossrender::Color SparkColor(const Spark& s) {
    const crossrender::f32 t = s.age / s.life;                 // 0 — рождение, 1 — смерть
    const crossrender::Color hot = crossrender::Color::FromRGB(0xFFC24B);
    const crossrender::Color cold = crossrender::Color::FromRGB(0x3B4252);
    // Смешиваем цвета и гасим прозрачность по мере старения.
    return crossrender::Lerp(hot, cold, crossrender::SmoothStep(0.0f, 1.0f, t)).WithAlpha(1.0f - t);
}

void UpdateCamera(OrbitCamera& cam, const crossrender::Vec3& focus, crossrender::f32 dt, crossrender::Random& rng) {
    cam.yaw += 0.35f * dt;                              // медленный облёт
    const crossrender::Vec3 desired{focus.x + std::sin(cam.yaw) * cam.radius,
                            focus.y + cam.height,
                            focus.z + std::cos(cam.yaw) * cam.radius};
    // Damp не зависит от частоты кадров — камера ведёт себя одинаково при любом FPS.
    cam.eye.x = crossrender::Damp(cam.eye.x, desired.x, 6.0f, dt);
    cam.eye.y = crossrender::Damp(cam.eye.y, desired.y, 6.0f, dt);
    cam.eye.z = crossrender::Damp(cam.eye.z, desired.z, 6.0f, dt);
    cam.target = crossrender::Lerp(cam.target, focus, 1.0f - std::exp(-8.0f * dt));

    // Затухающая тряска: случайная точка в единичной сфере, сжатая квадратом амплитуды.
    cam.shake = crossrender::MoveTowards(cam.shake, 0.0f, 1.8f * dt);
    cam.eye += rng.InUnitSphere() * (cam.shake * cam.shake);
}

crossrender::Mat4 CameraView(const OrbitCamera& cam) {
    return crossrender::Mat4::LookAt(cam.eye, cam.target, {0.0f, 1.0f, 0.0f});
}

void Frame(OrbitCamera& cam, Spark& spark, crossrender::Random& rng, crossrender::f32 dt) {
    UpdateSpark(spark, dt);
    UpdateCamera(cam, spark.position, dt, rng);

    // Порядок умножения всегда proj * view * model.
    const crossrender::Mat4 proj = crossrender::Mat4::Perspective(crossrender::Radians(55.0f), 16.0f / 9.0f, 0.1f, 120.0f);
    const crossrender::Mat4 view = CameraView(cam);
    const crossrender::Mat4 model = crossrender::Mat4::TRS(spark.position,
                                           crossrender::Vec3{0.0f, cam.yaw, 0.0f},
                                           crossrender::Vec3{0.05f});
    const crossrender::Mat4 mvp = proj * view * model;

    const crossrender::Color color = SparkColor(spark);
    const crossrender::u32 packed = color.ToRGBA8();
    ENG_LOGI("demo", "искра (%.2f, %.2f, %.2f), цвет 0x%08X, m[15] = %.1f",
             spark.position.x, spark.position.y, spark.position.z, packed, mvp.m[15]);
}
```

## См. также

* `docs/core/Base.md` — `crossrender::Radians` и `crossrender::Degrees` (единственный способ
  перевести градусы в радианы), константы `kPi`, `kTau`, `kEpsilon`, а также
  типы `f32`, `u8`, `u32`, `u64`.
* `docs/core/Log.md` — макросы `ENG_LOGI` и `ENG_LOGE`, которыми удобно
  печатать позиции и матрицы во время отладки.
* `docs/core/Time.md` — источник `dt` для `Damp`, `MoveTowards`, `SmoothStep`
  и интеграции движения.
* `engine/include/crossrender/gfx/Camera.h` — `Camera::View()` и `Camera::Proj()`,
  собранные из `Mat4::LookAt` и `Mat4::Perspective`; `ViewProj` умножает их в
  том же порядке `proj * view`.
* `engine/include/crossrender/gfx/Renderer2D.h` — использует `Mat4::Ortho2D`,
  `Transform2D` и `Rect` для отрисовки интерфейса и спрайтов.

