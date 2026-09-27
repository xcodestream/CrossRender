# crossrender/core/Time.h — время, покадровые дельты и счётчик FPS

Монотонное время процесса, игровые часы `Clock` со сглаженной дельтой, масштабом
времени, фиксированным шагом и счётчиком кадров в секунду.

## Заголовок

```cpp
#include "crossrender/core/Time.h"
```

## Обзор

Заголовок отвечает на два разных вопроса, и их важно не путать.

**Время настенных часов** — `NowSeconds()`: монотонные секунды от произвольной
точки (момента первого вызова в процессе). Это `f64`, растущее равномерно и
независимо от кадров. Им измеряют таймауты, профилируют загрузку, ставят дедлайны.

**Дельта кадра** — `Clock::Delta()`: сколько игрового времени прошло с прошлого
`Tick()`. Она умножается на масштаб времени, поэтому её можно замедлить,
ускорить или остановить, не трогая `NowSeconds()`. Ею двигают анимации, физику и
скрипты.

`Clock` устроен так:

1. Конструктор запоминает текущее время в `last_` — поэтому первый `Tick()`
   даёт почти нулевую дельту, а не весь промежуток с запуска процесса.
2. Каждый кадр вызывается `Tick()`: он считает сырую дельту, ограничивает её
   сверху значением `0.25` с (защита от отладчика, перетаскивания окна и
   сворачивания), умножает на `timeScale_`, накапливает `time_`, сглаживает
   значение для `SmoothedDelta()` и обновляет `FPS()`.
3. Если нужна детерминированная симуляция, задаётся `SetFixedStep`, а в кадре
   вызывается `ConsumeFixedSteps()`, возвращающий количество шагов, которые
   следует выполнить.

Порядок вызовов в кадре:

```text
Tick() -> Delta() (анимации, ввод) -> ConsumeFixedSteps() (физика) -> FixedAlpha() (интерполяция)
```

Отдельных классов-таймеров в этом заголовке нет: всё, что нужно для отсчёта
интервалов, собирается из `NowSeconds()` и `Clock`. Функция `SleepMs` живёт в
`crossrender/platform/Platform.h` и описана ниже отдельно.

## Члены класса

### `class Clock`

Игровые часы кадра. Один экземпляр на приложение, `Tick()` ровно раз в кадр.
Держит масштаб времени (пауза — это `SetTimeScale(0)`), накопитель для
фиксированного шага и окно подсчёта FPS.

```cpp
// Часы живут рядом с игровым циклом, а не внутри отдельной системы.
class GameLoop {
public:
    void Frame() {
        clock_.Tick();
        Update(clock_.Delta());
        Render(clock_.SmoothedDelta());
    }

    crossrender::Clock& MutableClock() { return clock_; }   // пауза, слоу-мо, отладка

private:
    void Update(float dt) { (void)dt; }
    void Render(float alpha) { (void)alpha; }

    crossrender::Clock clock_;
};
```

### `f64 NowSeconds()`

Монотонные секунды от момента первого вызова функции в процессе (используется
`std::chrono::steady_clock`). Часы не переводятся назад и не зависят от системного
времени.

* **Возвращает:** `f64`, обычно начиная с долей микросекунды после старта.
* **Важно:** это не Unix-время и не «секунды с запуска приложения» в строгом
  смысле — отсчёт идёт от первой инициализации статической переменной внутри
  функции.

```cpp
// Замер стоимости загрузки текстуры.
const crossrender::f64 t0 = crossrender::NowSeconds();
crossrender::Texture hero;
hero.LoadFromFile("assets/textures/hero.png");
const crossrender::f64 elapsed = crossrender::NowSeconds() - t0;
ENG_LOGI("assets", "текстура загружена за %.1f мс", elapsed * 1000.0);
```

### `Clock()`

Конструктор. Запоминает текущее монотонное время как начало отсчёта, поэтому
дельта первого кадра — это время от создания часов до первого `Tick()`, обычно
близкое к нулю.

```cpp
// Часы создаются перед игровым циклом; первый Tick() не «съест» время инициализации.
crossrender::Clock clock;
ENG_LOGI("loop", "часы заведены, прошедшее время %.3f с", clock.TotalSeconds());
```

### `void Tick()`

Продвигает часы на один кадр. Вызывается ровно один раз за итерацию цикла, до
чтения `Delta()`.

Что происходит внутри:

* сырая дельта ограничивается диапазоном `[0, 0.25]` секунды;
* `Delta()` получается умножением на масштаб времени, а `UnscaledDelta()` — нет;
* `Time()` и `TotalSeconds()` растут на `Delta()`, счётчик `Frame()`
  увеличивается на единицу;
* накопитель фиксированного шага пополняется на `Delta()`;
* раз в 0.25 с накопленного сырого времени пересчитывается `FPS()`.

```cpp
crossrender::Clock clock;
for (int frame = 0; frame < 3; ++frame) {
    clock.Tick();
    ENG_LOGI("loop", "кадр %llu: dt=%.4f с, всего %.3f с",
             static_cast<unsigned long long>(clock.Frame()), clock.Delta(),
             clock.TotalSeconds());
}
```

### `f32 Delta() const`

Дельта текущего кадра в секундах, **уже умноженная** на масштаб времени. Именно
её передают в обновление анимаций, физики и скриптов.

* **Возвращает:** значение в диапазоне `[0, 0.25 * timeScale]` для положительного
  масштаба.
* **Первый кадр:** практически `0`.

```cpp
crossrender::Clock clock;
clock.Tick();
player.position += player.velocity * clock.Delta();
ENG_LOGI("player", "сдвиг за кадр: %.4f м", static_cast<double>(clock.Delta()));
```

### `f32 UnscaledDelta() const`

Сырая дельта кадра без учёта масштаба времени. Нужна там, где пауза не должна
останавливать процесс: интерфейс, отсчёт реального времени, статистика.

```cpp
crossrender::Clock clock;
clock.Tick();
if (clock.TimeScale() == 0.0f) {
    // Игра на паузе, но меню продолжает мигать в реальном времени.
    menu.Advance(clock.UnscaledDelta());
}
```

### `f32 Time() const`

Игровое время, накопленное с момента создания часов. Растёт на `Delta()`, поэтому
зависит от масштаба времени и не переживает паузу.

```cpp
crossrender::Clock clock;
clock.Tick();
ENG_LOGI("loop", "игровое время: %.3f с", static_cast<double>(clock.Time()));
```

### `u64 Frame() const`

Номер текущего кадра. После конструктора равен `0`, после каждого `Tick()` — на
единицу больше. Удобно для «сделать раз в N кадров».

```cpp
crossrender::Clock clock;
clock.Tick();
if (clock.Frame() % 60 == 0) {
    ENG_LOGI("loop", "прошла секунда игрового времени: кадр %llu",
             static_cast<unsigned long long>(clock.Frame()));
}
```

### `f32 FPS() const`

Кадров в секунду, посчитанных по **сырой** дельте. Значение пересчитывается
каждые 0.25 с накопленного времени, поэтому сразу после старта `FPS()` равен `0`
и остаётся нулевым первые 0.25 с.

```cpp
crossrender::Clock clock;
clock.Tick();
if (clock.FPS() > 0.0f && clock.FPS() < 30.0f) {
    ENG_LOGW("perf", "низкий FPS: %.1f", static_cast<double>(clock.FPS()));
}
```

### `f32 SmoothedDelta() const`

Сглаженная дельта: экспоненциальное среднее с коэффициентом `0.1`. В первом
кадре равна `Delta()`, дальше ведёт себя спокойнее и не дёргается от случайного
длинного кадра. Полезна для визуальных эффектов и предсказания.

```cpp
crossrender::Clock clock;
clock.Tick();
// Плавно гасим тряску камеры, не реагируя на одиночные провалы кадров.
camera.shakeDecay -= camera.shakeDecay * clock.SmoothedDelta() * 4.0f;
```

### `f64 TotalSeconds() const`

То же, что `Time()`, но в `f64`. Пригодится для длинных сессий и передачи в
код, работающий с `NowSeconds()`.

```cpp
crossrender::Clock clock;
clock.Tick();
ENG_LOGI("loop", "сессия идёт %.3f с", clock.TotalSeconds());
```

### `void SetTimeScale(f32 s)`

Задаёт множитель игрового времени. `0` — пауза, `0.5` — замедление, `2` —
ускорение. На `UnscaledDelta()` и `FPS()` не влияет.

* **Ограничение:** отрицательные значения не запрещены — время начнёт идти назад,
  а `Delta()` станет отрицательной. Полагаться на это не стоит.

```cpp
crossrender::Clock clock;
clock.SetTimeScale(0.0f);     // пауза
clock.Tick();
ENG_LOGI("game", "на паузе dt=%.4f, сырое dt=%.4f",
         static_cast<double>(clock.Delta()), static_cast<double>(clock.UnscaledDelta()));
clock.SetTimeScale(1.0f);     // продолжили
```

### `f32 TimeScale() const`

Текущий множитель времени. Удобно, чтобы проверить состояние паузы, не заводя
отдельный флаг.

```cpp
crossrender::Clock clock;
clock.SetTimeScale(0.25f);
if (clock.TimeScale() < 1.0f) {
    ENG_LOGI("game", "активен режим замедления x%.2f", static_cast<double>(clock.TimeScale()));
}
```

### `void SetFixedStep(f32 step)`

Задаёт длину шага фиксированного обновления в секундах. По умолчанию —
`1.0f / 60.0f`. Значение `<= 0` полностью выключает фиксированные шаги:
`ConsumeFixedSteps()` начнёт возвращать `0`.

```cpp
crossrender::Clock clock;
clock.SetFixedStep(1.0f / 30.0f);   // физика на 30 Гц, рендер — как получится
ENG_LOGI("phys", "шаг физики: %.1f мс", static_cast<double>(clock.FixedStep()) * 1000.0f);
```

### `f32 FixedStep() const`

Текущая длина шага фиксированного обновления.

```cpp
crossrender::Clock clock;
if (clock.FixedStep() > 0.0f) {
    ENG_LOGI("phys", "детерминированная симуляция включена, шаг %.4f с",
             static_cast<double>(clock.FixedStep()));
}
```

### `u32 ConsumeFixedSteps()`

Забирает из накопителя столько шагов фиксированной длины, сколько накопилось, и
возвращает их количество. Накопитель пополняется в `Tick()` на `Delta()`.

* **Возвращает:** число шагов для выполнения в этом кадре; `0`, если шаг
  отключён (`FixedStep() <= 0`) или времени ещё не хватает.
* **Защита от разгона:** не более **8** шагов за один вызов. Если накопилось
  больше (например, после долгой паузы), накопитель сбрасывается в ноль, и
  «долг» отбрасывается — симуляция не пытается догнать время бесконечно.
* **Изменяет состояние:** метод не `const`, он опустошает накопитель.

```cpp
crossrender::Clock clock;
clock.SetFixedStep(1.0f / 60.0f);
clock.Tick();

const crossrender::u32 steps = clock.ConsumeFixedSteps();
for (crossrender::u32 i = 0; i < steps; ++i) {
    physics.Step(clock.FixedStep());   // ровный, предсказуемый шаг
}
ENG_LOGI("phys", "шагов за кадр: %u", steps);
```

### `f32 FixedAlpha() const`

Доля незакрытого шага — остаток накопителя, делённый на длину шага. Служит
коэффициентом интерполяции между предыдущим и текущим состоянием при отрисовке.

* **Возвращает:** значение в `[0, 1)` после `ConsumeFixedSteps()`.
* **Порядок важен:** до вызова `ConsumeFixedSteps()` метод вернёт накопленную
  долю целиком и может дать значение больше `1`. Если шаг отключён
  (`FixedStep() <= 0`), делителем служит `1.0f`.

```cpp
crossrender::Clock clock;
clock.Tick();
clock.ConsumeFixedSteps();          // сначала «тратим» целые шаги
const crossrender::f32 alpha = clock.FixedAlpha();
const crossrender::f32 smoothX = prevX + (currX - prevX) * alpha;
ENG_LOGI("gfx", "интерполяция позиции с alpha=%.3f -> x=%.3f",
         static_cast<double>(alpha), static_cast<double>(smoothX));
```

### `void ForceDelta(f32 dt)`

Подменяет дельту заданным значением — для тестов и детерминированных прогонов.
Пока подмена активна, реальное время игнорируется, но `last_` всё равно
обновляется в каждом `Tick()`.

* **Ограничение:** выключить подмену можно только отрицательным значением, обычно
  `ForceDelta(-1.0f)`; отдельного `ClearForcedDelta()` в API нет.
* **Важно:** подмена происходит **до** ограничения `0.25` с, поэтому заданная
  дельта тоже обрезается этим пределом.

```cpp
// Тест: ровно 20 шагов по 1/50 секунды, независимо от загрузки машины.
crossrender::Clock clock;
clock.SetFixedStep(1.0f / 50.0f);
clock.ForceDelta(1.0f / 50.0f);
int totalSteps = 0;
for (int i = 0; i < 20; ++i) {
    clock.Tick();
    totalSteps += static_cast<int>(clock.ConsumeFixedSteps());
}
clock.ForceDelta(-1.0f);   // вернулись к реальному времени
ENG_LOGI("test", "выполнено шагов: %d, игровое время %.3f с", totalSteps,
         clock.TotalSeconds());
```

### `std::string FormatTime(f32 seconds)`

Форматирует секунды как `mm:ss.mmm` — для таймеров, сплитов и внутриигрового
интерфейса. Отрицательные значения приводятся к нулю.

* **Ограничение:** часы не отделяются — 3725.25 с печатается как `62:05.250`, а
  не `01:02:05.250`.

```cpp
crossrender::Clock clock;
clock.Tick();
ui.Label(crossrender::FormatTime(clock.Time()), hudTimerRect);
ENG_LOGI("ui", "на таймере %s", crossrender::FormatTime(clock.Time()).c_str());
```

### `void SleepMs(u32 milliseconds)`

**Объявлена не в `Time.h`, а в `crossrender/platform/Platform.h`** — раздел приведён
здесь, потому что функция относится к работе со временем. Приостанавливает поток
на заданное число миллисекунд средствами платформы. Внутри движка используется
для ограничения частоты кадров и коротких ожиданий в цикле окна.

```cpp
#include "crossrender/platform/Platform.h"

// Ограничение частоты кадров до 60 без вертикальной синхронизации.
if (frameTime < target) {
    crossrender::SleepMs(static_cast<crossrender::u32>((target - frameTime) * 1000.0f));
}
```

## Пример целиком

```cpp
#include "crossrender/core/Log.h"
#include "crossrender/core/Time.h"

#include <string>

// Мини-цикл: фиксированная физика, интерполяция отрисовки и статистика раз в секунду.
class FixedStepLoop {
public:
    FixedStepLoop() {
        clock_.SetFixedStep(1.0f / 60.0f);   // физика ровно 60 Гц
        ENG_LOGI("loop", "старт, шаг физики %.2f мс",
                 static_cast<double>(clock_.FixedStep()) * 1000.0f);
    }

    void Frame() {
        clock_.Tick();

        // 1. Физика идёт фиксированными шагами, максимум 8 за кадр.
        const crossrender::u32 steps = clock_.ConsumeFixedSteps();
        for (crossrender::u32 i = 0; i < steps; ++i) Simulate(clock_.FixedStep());

        // 2. Отрисовка интерполируется по незакрытой доле шага — после расхода шагов.
        Render(clock_.FixedAlpha());

        // 3. Статистика — по реальному, немасштабированному времени.
        reportAccum_ += clock_.UnscaledDelta();
        if (reportAccum_ >= 1.0f) {
            reportAccum_ = 0.0f;
            ENG_LOGI("loop", "кадр %llu, FPS %.1f, игровое время %s",
                     static_cast<unsigned long long>(clock_.Frame()),
                     static_cast<double>(clock_.FPS()),
                     crossrender::FormatTime(clock_.Time()).c_str());
        }
    }

    void TogglePause() {
        paused_ = !paused_;
        clock_.SetTimeScale(paused_ ? 0.0f : 1.0f);
        ENG_LOGI("loop", "пауза: %s", paused_ ? "вкл" : "выкл");
    }

private:
    void Simulate(float step) { timeInSim_ += step; }
    void Render(float alpha) {
        // Между шагами позиция дорисовывается интерполяцией.
        (void)alpha;
    }

    crossrender::Clock clock_;
    float reportAccum_ = 0.0f;
    float timeInSim_ = 0.0f;
    bool paused_ = false;
};

void Demo() {
    FixedStepLoop loop;
    for (int i = 0; i < 120; ++i) loop.Frame();
    loop.TogglePause();
    loop.Frame();   // на паузе Delta() == 0, Frame() всё равно растёт

    // Дедлайн по настенным часам не зависит от паузы и масштаба времени.
    const crossrender::f64 deadline = crossrender::NowSeconds() + 0.5;
    ENG_LOGI("loop", "ждём до %.3f с", deadline);
}
```

## См. также

* `docs/core/Base.md` — тип `crossrender::f64`, выбранный для времени именно ради
  точности накопления.
* `docs/core/Log.md` — макросы, которыми принято выводить статистику кадра.
* `docs/core/File.md` — метки времени файлов, не связанные с монотонными часами
  отсюда.
