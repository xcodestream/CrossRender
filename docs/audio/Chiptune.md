# crossrender/audio/Chiptune.h — 8- и 16-битный синтез, трекер и ChipPlayer

Эмуляция звуковых чипов (NES 2A03, Game Boy, SPC-подобный семплер, FM и
PC-speaker) плюс маленький трекер: песня описывается паттернами и списком
порядка, а `ChipPlayer` либо играет её в реальном времени, либо **запекает** в
обычный `AudioClip`.

## Заголовок

```cpp
#include "crossrender/audio/Chiptune.h"
```

## Обзор

Модуль не требует ни одного звукового файла: и волновые таблицы, и встроенные
песни синтезируются кодом. Результат — обычный PCM, который играет общий
микшер `crossrender::Audio`.

### Модель чипа: APU-каналы и SPC-семплер

`ChipType` выбирает семейство каналов, `ChipWave` — генератор внутри канала;
пара (`ChipType`, `ChipWave`) отображается на внутренний «вид голоса»:

* `Nes2A03` — два импульсных канала (скважность 12.5/25/50/75 %), треугольник
  (4-битная ступенчатая волна, классический бас), шум на 15-битном LFSR и
  DMC-подобный 1-битный дельта-канал. Волны `Saw`/`Sine` падают на
  семплерный голос.
* `GameBoy` — импульсные каналы, волновой канал (32 сэмпла, 4 бита) и шум с
  7-битным LFSR.
* `SnesSpc` — 8 семплерных голосов с ADSR, питч-модуляцией и эхо-блоком
  (8-таповый FIR) плюс общий 2-полюсный фильтр низких частот.
* `SegaYM` — 2-операторный FM-голос.
* `PcSpeaker` — 1-битный бипер.

Все голоса считаются на **внутренней частоте 32000 Гц**. NES/Game Boy
переводят нужную частоту в аппаратный период в тактах CPU (1.789773 МГц NTSC,
1.662607 МГц PAL; PAL выбирается при `framesPerSecond <= 50`; Game Boy —
4.194304 МГц), поэтому шаг duty-секвенсора и тактирование LFSR повторяют
арифметику настоящего чипа. Стерео-микс затем ресемплируется в частоту вывода
(`SetSampleRate`) дробным линейным интерполятором, проходит однополюсный
DC-блокер на ~20 Гц и мягкий лимитер.

```cpp
// 8-битная NES-песня: два импульса, треугольник и шум.
crossrender::ChipSong nes = crossrender::ChipPlayer::Make8BitSong(1u);
crossrender::ChipPlayer player;
player.SetSong(nes);
player.SetSampleRate(44100);
player.Play();

// Тот же трекер, но на SPC-семплере с эхом и фильтром.
crossrender::ChipSong snes = crossrender::ChipPlayer::Make16BitSong(2u);
player.SetSong(snes);
player.Play();
```

### Трекерный формат: паттерны, порядок, ряды, тики, эффекты

Песня (`ChipSong`) состоит из:

* **каналов** — `channels`, по `ChipChannelDef` на канал: волна, громкость
  0…15, огибающая (attack/decay/sustain/release в 4-битных единицах по 6 мс),
  панорама, детюн, свипы, арпеджио, посыл в эхо;
* **паттернов** — `patterns`, каждый это сетка `rowCount x channelCount` из
  `ChipNote`;
* **списка порядка** — `order`: индексы паттернов в порядке проигрывания;
  `loopOrder` задаёт, куда вернуться в конце (`-1` — не зацикливать).

Время измеряется **тиками**: `framesPerSecond` — частота тиков (обычно 50 или
60 Гц), `ticksPerRow` — сколько тиков длится один ряд. Именно `ticksPerRow`
авторитетен: ряд звучит `ticksPerRow / framesPerSecond` секунд, а
`beatsPerMinute`/`rowsPerBeat` — музыкальные метаданные, которые фабрики держат
согласованными (`bpm = framesPerSecond * 60 / (ticksPerRow * rowsPerBeat)`).
Если `ticksPerRow <= 0`, плеер выводит его из BPM; `SetSpeed` переопределяет
его на лету.

Ячейка `ChipNote` содержит полутон (`-1` — пусто, `-2` — note off, `0` — C-0),
инструмент, громкость (`0xFF` — по умолчанию), эффект и параметр. Эффекты
применяются раз в тик (или раз в ряд, где это отмечено): арпеджио, слайды,
портаменто, вибрато, слайд громкости, смена скважности, прыжок по списку
порядка, смена скорости, обрезка ноты, ретриггер, эхо и детюн. Для нотного
шумового канала номер ноты — это индекс таблицы периодов шума, а не высота
тона.

```cpp
crossrender::ChipSong song = crossrender::ChipPlayer::Make8BitSong(1u);
song.title = "My Tune";
song.ticksPerRow = 6;                        // скорость: 6 тиков на ряд
song.beatsPerMinute = song.framesPerSecond * 60 / (song.ticksPerRow * song.rowsPerBeat);

crossrender::ChipPattern p;
p.name = "Intro";
p.Resize(16, 2);                             // 16 рядов, 2 канала
p.At(0, 0).semitone = 69;                    // A-4
p.At(0, 0).volume = 13;
p.At(8, 0).semitone = 72;
p.At(8, 0).effect = crossrender::ChipFxNoteCut;      // оборвать ноту
p.At(8, 0).param = 2;                        // через 2 тика
song.patterns.push_back(p);                  // или song.AddPattern(p)
song.order = {0};
song.loopOrder = 0;                          // играть по кругу
```

### Как `Bake` превращает песню в клип

`ChipPlayer::Bake` рендерит песню **офлайн** во временный `AudioClip`:

1. создаётся локальный `ChipPlayer`, которому отдаётся копия песни;
2. берётся длительность: `seconds`, если он больше нуля, иначе
   `ChipSong::DurationSeconds(-1)`, и обрезается по `maxSeconds`;
3. рендерится `frames + fade` кадров, где `fade` — длина кроссфейда из
   `loopFadeSeconds`;
4. голова клипа смешивается с продолжением петли, поэтому последний сэмпл
   переходит в первый без щелчка и без паузы;
5. результат кодируется в 16-битный WAV **в памяти** и декодируется штатным
   `AudioClip::LoadFromMemory`, то есть клип проходит через обычный декодер
   движка.

Операция дорогая: это полный синтез песни на CPU (десятки секунд звука), к тому
же аллоцирующая. Делайте её **один раз при входе в сцену** (или при смене
трека), а не каждый кадр. Если `seconds` даёт нулевую длительность или у плеера
нет песни, возвращается невалидный клип.

```cpp
crossrender::ChipSong song = crossrender::ChipPlayer::MakeNamed("title", 5u);
crossrender::ChipPlayer player;
player.SetSong(song);
player.SetSampleRate(22050);

// 24 секунды музыки, кроссфейд 0.05 с, потолок 48 с — один раз при входе.
crossrender::AudioClip clip = player.Bake(22050, 24.0f, 0.05f, 48);
if (clip.Valid()) {
    crossrender::PlayParams p;
    p.looping = true;
    p.volume = 0.5f;
    crossrender::Audio::Get().Play(clip, p);
} else {
    ENG_LOGW("chiptune", "Bake не дал звука — играем без музыки");
}
```

### Детерминизм и ассеты `audio/song_*.chip`

Встроенные песни **детерминированы**: `Make8BitSong(seed)` и родственные
фабрики сеют `crossrender::Random` этим seed-ом, поэтому при одном и том же seed вы
получаете идентичную партитуру и, следовательно, идентичный PCM. Меняется
только то, что действительно случайно, — громкость хэтов и вариант филла
барабанов. Это делает песни пригодными для тестов и для повторных запусков.

Формат песни — обычный текст (`Serialize`/`Deserialize`), поэтому трек можно
хранить в репозитории, читать глазами и править руками. Инструмент ассетов
`tools/mkassets` обходит `ChipPlayer::BuiltinNames()`, вызывает
`MakeNamed(name, 7)`, пишет `assets/audio/song_<name>.chip` через
`Serialize` + `WriteTextFile` и сразу проверяет round-trip через
`LoadFromFile`. Сцена может предпочесть файл встроенной песне, а при
отсутствии файла — сгенерировать её на месте.

```cpp
// Сначала пробуем ассет, сгенерированный tools/mkassets, потом — фабрику.
crossrender::ChipSong song = crossrender::ChipPlayer::MakeNamed("8bit", 7u);
const std::string path = crossrender::PathJoin(crossrender::GetAssetRoot(), "audio/song_8bit.chip");
crossrender::ChipSong loaded;
std::string error;
if (crossrender::FileExists(path) && crossrender::ChipPlayer::LoadFromFile(path, &loaded, &error)) {
    song = loaded;                     // играем то, что лежит в ассетах
} else if (!error.empty()) {
    ENG_LOGW("chiptune", "%s не прочитался: %s", path.c_str(), error.c_str());
}
for (const std::string& name : crossrender::ChipPlayer::BuiltinNames()) {
    ENG_LOGI("chiptune", "встроенный трек: %s", name.c_str());
}
```

## Члены класса

### `enum class ChipType : u8`

Семейство эмуляции. Выбирает, какие генераторы получат каналы, и какой CPU
используется для пересчёта частоты в период.

| Значение | Смысл |
|---|---|
| `ChipType::Nes2A03` | 2 импульса + треугольник + шум + DMC (8 бит). |
| `ChipType::GameBoy` | 2 импульса + волновой канал + шум (8 бит). |
| `ChipType::SnesSpc` | 8 семплерных голосов + эхо (16 бит). |
| `ChipType::SegaYM` | 6 FM-подобных голосов (16-битный оттенок). |
| `ChipType::PcSpeaker` | 1-битный бипер. |
| `ChipType::Count` | Служебный счётчик значений, каналом быть не может. |

```cpp
crossrender::ChipSong song;
song.chip = crossrender::ChipType::SnesSpc;     // включит семплерные голоса и эхо
song.echoEnabled = true;
song.lowPass = true;
song.lowPassCutoff = 7000;
```

### `enum class ChipWave : u8`

Генератор канала. Волны `Pulse*` и `Square` на NES/Game Boy дают импульсный
канал, `Triangle` — треугольник/волновой канал, `Noise` — шум, `Sample` — DMC
или семплерный голос.

| Значение | Смысл |
|---|---|
| `ChipWave::Pulse12` | Импульс со скважностью 12.5 %. |
| `ChipWave::Pulse25` | Скважность 25 %. |
| `ChipWave::Pulse50` | Скважность 50 % (по умолчанию). |
| `ChipWave::Pulse75` | Скважность 75 %. |
| `ChipWave::Triangle` | Треугольник (4-битная ступень на NES). |
| `ChipWave::Saw` | Пила (на NES уходит в семплерный голос). |
| `ChipWave::Sine` | Синус (семплерный голос). |
| `ChipWave::Square` | Прямоугольник 50 % (на SPC — яркий семпл). |
| `ChipWave::Noise` | Шум: NES/Game Boy LFSR или детерминированный цикл. |
| `ChipWave::Sample` | Семплерный голос / DMC. |
| `ChipWave::Count` | Служебный счётчик значений. |

```cpp
crossrender::ChipChannelDef lead;
lead.wave = crossrender::ChipWave::Pulse50;
crossrender::ChipChannelDef bass;
bass.wave = crossrender::ChipWave::Triangle;
crossrender::ChipChannelDef perc;
perc.wave = crossrender::ChipWave::Noise;
```

### `struct ChipChannelDef`

Настройки одного канала-«инструмента». Поля сгруппированы в таблицу (это
параметрическая структура, а не набор методов); пример общий для всей группы.

| Поле | По умолчанию | Смысл |
|---|---|---|
| `ChipWave wave` | `Pulse50` | Генератор канала. |
| `int volume` | `12` | Аппаратная громкость 0…15; нота может её переопределить. |
| `int dutySweep` | `0` | LFO скважности (шагов/с); на семплерных голосах — частота питч-модуляции. |
| `int pitchSweep` | `0` | NES-свип: изгиб высоты за ряд (1/16 полутона); на семплах — глубина модуляции в центах. |
| `bool arpeggio` | `false` | Циклически играть терцию и квинту каждый тик. |
| `int echoVolume` | `0` | Только SPC: посыл голоса в эхо 0…15 (глобальный эхо-блок общий). |
| `int attack` | `0` | Атака в единицах по 6 мс (0 — мгновенно, 15 — 90 мс). |
| `int decay` | `0` | Спад до уровня sustain в тех же единицах. |
| `int sustain` | `12` | Удерживаемый уровень 0…15. |
| `int release` | `0` | Затухание после note off. |
| `int pan` | `0` | Панорама `-8…8` (равномощностная кривая). |
| `int detune` | `0` | Статический детюн в центах. |

```cpp
crossrender::ChipChannelDef pad;
pad.wave = crossrender::ChipWave::Square;
pad.volume = 10;
pad.attack = 6;      // 36 мс
pad.decay = 8;
pad.sustain = 10;
pad.release = 11;
pad.pan = -3;
pad.detune = 4;      // чуть выше строя
pad.echoVolume = 12; // посыл в эхо (SPC)
```

### `struct ChipNote`

Одна ячейка паттерна. Поля сгруппированы.

| Поле | По умолчанию | Смысл |
|---|---|---|
| `i16 semitone` | `-1` | `-1` — пусто, `-2` — note off, `0` — C-0, дальше полутон `октава * 12 + нота`. |
| `u8 instrument` | `0` | Номер инструмента (задел формата). |
| `u8 volume` | `0xFF` | Громкость 0…15; `0xFF` — значение канала по умолчанию. |
| `u8 effect` | `0` | Код эффекта, см. `ChipEffect`. |
| `u8 param` | `0` | Параметр эффекта (ниблы или значение — зависит от эффекта). |
| `u8 arp` | `0xFF` | Фиксированный сдвиг в полутонах на всю ноту; `0xFF` — нет. |

```cpp
crossrender::ChipPattern p;
p.Resize(16, 4);
p.At(0, 0).semitone = 76;                    // E-6
p.At(0, 0).volume = 13;
p.At(4, 0).semitone = 74;
p.At(4, 0).effect = crossrender::ChipFxVibrato;
p.At(4, 0).param = 0x24;                     // скорость 2, глубина 4
p.At(8, 0).semitone = -2;                    // note off
```

### `enum ChipEffect : u8`

Коды эффектов трекера (обычный, не `enum class`, поэтому константы пишутся без
квалификатора перечисления, но с пространством имён — `crossrender::ChipFxSlideUp`).

| Значение | Параметр | Смысл |
|---|---|---|
| `ChipFxNone` | — | Нет эффекта. |
| `ChipFxArpeggio` | ниблы `hi, lo` | Цикл `0, hi, lo` полутонов. |
| `ChipFxSlideUp` | `param / 16` | Подъём высоты на столько полутонов за тик. |
| `ChipFxSlideDown` | `param / 16` | Спуск высоты за тик. |
| `ChipFxPortamento` | `param / 16` | Глиссандо к следующей ноте. |
| `ChipFxVibrato` | `hi` — скорость, `lo` — глубина | Вибрато (глубина в 1/16 полутона). |
| `ChipFxVolumeSlide` | `hi` вверх, `lo` вниз | Слайд громкости в шагах 1/15 за тик. |
| `ChipFxDutyCycle` | `0…3` | Сменить скважность импульса. |
| `ChipFxJump` | индекс порядка | Прыжок в списке порядка (`0xFF` — на `loopOrder`). |
| `ChipFxSpeed` | `1…64` | Новое число тиков на ряд. |
| `ChipFxNoteCut` | тики | Отпустить ноту через `param` тиков. |
| `ChipFxRetrigger` | тики | Ретриггер каждые `param` тиков (сбрасывает LFSR). |
| `ChipFxEcho` | `0`/`1` | Выключить/включить эхо-блок. |
| `ChipFxDetune` | `i8` центов | Детюн, знаковый байт. |

```cpp
crossrender::ChipPattern p;
p.Resize(8, 2);
p.At(0, 0).semitone = 60;
p.At(0, 0).effect = crossrender::ChipFxArpeggio;
p.At(0, 0).param = 0x37;              // 0 -> +3 -> +7 полутонов
p.At(2, 0).effect = crossrender::ChipFxSlideUp;
p.At(2, 0).param = 8;                 // +0.5 полутона за тик
p.At(4, 0).effect = crossrender::ChipFxDutyCycle;
p.At(4, 0).param = 0;                 // скважность 12.5 %
p.At(6, 0).effect = crossrender::ChipFxJump;
p.At(6, 0).param = 0;                 // вернуться к первому порядку
```

### `struct ChipPattern`

Сетка нот: `rowCount x channelCount` ячеек `ChipNote`. Паттерны адресуются
индексами из `ChipSong::order`.

```cpp
crossrender::ChipPattern verse;
verse.name = "Verse";
verse.Resize(32, 4);
verse.At(0, 0).semitone = 69;
verse.At(16, 0).semitone = 72;
ENG_LOGI("chiptune", "паттерн %s: %d рядов", verse.name.c_str(), verse.rowCount);
```

### `std::string ChipPattern::name`

Имя паттерна для интерфейса и для текстового формата. В `Serialize` запятые в
имени заменяются подчёркиваниями (формат хранит имя в списке через запятую).

```cpp
pattern.name = "Chorus";
ENG_LOGI("chiptune", "редактируем %s", pattern.name.c_str());
```

### `std::vector<ChipNote> ChipPattern::rows`

Плоский массив ячеек, размер `rowCount * channelCount`. Обращаться к нему
напрямую почти никогда не нужно — используйте `At(row, channel)`; но `rows`
удобен, чтобы скопировать или очистить паттерн целиком.

```cpp
p.rows.assign(p.rows.size(), crossrender::ChipNote{});   // очистить все ячейки
ENG_LOGI("chiptune", "ячеек: %d", static_cast<int>(p.rows.size()));
```

### `int ChipPattern::rowCount`

Число рядов в паттерне. Устанавливается `Resize`; в текстовом формате
объявляется в строке `pattern=Имя,рядов,каналов`.

```cpp
ENG_LOGI("chiptune", "рядов в паттерне: %d", p.rowCount);
```

### `int ChipPattern::channelCount`

Число каналов паттерна. Может быть меньше `ChipSong::channelCount` — тогда
«лишние» каналы песни получают `note off` в начале ряда.

```cpp
if (p.channelCount < song.channelCount) {
    ENG_LOGW("chiptune", "паттерн уже песни: %d из %d", p.channelCount, song.channelCount);
}
```

### `const ChipNote& ChipPattern::At(int row, int channel) const` и неконстантная перегрузка

Доступ к ячейке; неконстантная версия возвращает `ChipNote&` для записи.
**Границы не проверяются**: индекс считается как
`row * channelCount + channel`, поэтому `row` и `channel` обязаны быть в
диапазоне — иначе чтение/запись мимо массива.

```cpp
crossrender::ChipNote& cell = p.At(3, 1);
cell.semitone = 64;                    // E-4
cell.volume = 12;

const crossrender::ChipPattern& ro = p;
const crossrender::ChipNote& readOnly = ro.At(3, 1);
ENG_LOGI("chiptune", "нота %s", crossrender::ChipPlayer::NoteName(readOnly.semitone).c_str());
```

### `void ChipPattern::Resize(int rows_, int channels_)`

Пересоздаёт сетку: заполняет `rowCount`, `channelCount` и вектор `rows`
значениями по умолчанию (`ChipNote{}` — пустая нота). Существующие ноты
теряются.

```cpp
crossrender::ChipPattern p;
p.Resize(32, 4);                   // 32 ряда, 4 канала
p.name = "Main";
ENG_LOGI("chiptune", "%dx%d", p.rowCount, p.channelCount);
```

### `struct ChipSong`

Песня целиком: метаданные, каналы, паттерны, список порядка и настройки
16-битных эффектов. `ChipPlayer::SetSong` копирует структуру и **нормализует**
её (см. `SetSong`), поэтому исходный объект можно менять дальше.

```cpp
crossrender::ChipSong song;
song.title = "Neon Cavern";
song.author = "CrossRender";
song.chip = crossrender::ChipType::Nes2A03;
song.channelCount = 4;
song.channels.resize(4);
song.framesPerSecond = 50;
song.ticksPerRow = 6;
song.AddPattern(pattern);
song.order = {0};
song.loopOrder = 0;
ENG_LOGI("chiptune", "'%s': %d паттернов, %d рядов", song.title.c_str(), song.PatternCount(),
         song.TotalRows());
```

### Поля `ChipSong`: `title`, `author`, `chip`, `channelCount`

| Поле | По умолчанию | Смысл |
|---|---|---|
| `std::string title` | `""` | Название трека. |
| `std::string author` | `"CrossRender"` | Автор. |
| `ChipType chip` | `Nes2A03` | Семейство эмуляции. |
| `int channelCount` | `4` | Число активных каналов; при `<= 0` берётся из размера `channels`. |

```cpp
song.title = "Circuit Bloom";
song.author = "me";
song.chip = crossrender::ChipType::Nes2A03;
song.channelCount = 4;
```

### Поля тайминга: `ticksPerRow`, `framesPerSecond`, `rowsPerBeat`, `beatsPerMinute`, `loopOrder`

| Поле | По умолчанию | Смысл |
|---|---|---|
| `int ticksPerRow` | `6` | Тиков на ряд — авторитетная скорость. |
| `int framesPerSecond` | `60` | Частота тиков (кадров трекера) в герцах. |
| `int rowsPerBeat` | `4` | Рядов в доле (метаданные). |
| `int beatsPerMinute` | `125` | BPM (метаданные; выводится из тиков, если `ticksPerRow <= 0`). |
| `int loopOrder` | `0` | Индекс порядка для возврата; `-1` — не зацикливать. |

```cpp
song.framesPerSecond = 60;
song.rowsPerBeat = 4;
song.ticksPerRow = 9;
song.beatsPerMinute = song.framesPerSecond * 60 / (song.ticksPerRow * song.rowsPerBeat);   // 100
song.loopOrder = 0;      // играть по кругу с начала
```

### Поля данных: `channels`, `patterns`, `order`

| Поле | По умолчанию | Смысл |
|---|---|---|
| `std::vector<ChipChannelDef> channels` | пусто | Настройки каналов; при нехватке дополняется значениями по умолчанию. |
| `std::vector<ChipPattern> patterns` | пусто | Паттерны; индекс — то, на что ссылается `order`. |
| `std::vector<int> order` | пусто | Последовательность индексов паттернов. |

Список `order` не может быть пустым: `SetSong` подставит `{0}`, а
`Deserialize` отвергнет текст без него.

```cpp
song.channels.resize(4);
song.channels[0] = lead;
song.channels[1] = harmony;
song.AddPattern(intro);
song.AddPattern(main);
song.order = {0, 1, 1};
```

### Поля 16-битных эффектов: `echoEnabled`, `echoDelayMs`, `echoFeedback`, `echoVolume`, `lowPass`, `lowPassCutoff`

| Поле | По умолчанию | Смысл |
|---|---|---|
| `bool echoEnabled` | `false` | Включить эхо-блок. |
| `int echoDelayMs` | `120` | Задержка эха в миллисекундах (зажимается в 1…1000). |
| `int echoFeedback` | `90` | Обратная связь 0…255. |
| `int echoVolume` | `60` | Уровень эха 0…255. |
| `bool lowPass` | `false` | Включить 2-полюсный ФНЧ на выходе. |
| `int lowPassCutoff` | `8000` | Частота среза ФНЧ в герцах. |

```cpp
song.echoEnabled = true;
song.echoDelayMs = 180;
song.echoFeedback = 150;
song.echoVolume = 110;
song.lowPass = true;
song.lowPassCutoff = 7000;
```

### `int ChipSong::PatternCount() const`

Число паттернов в песне. Не путать с длиной `order`: один паттерн может
встречаться в списке несколько раз.

```cpp
if (song.PatternCount() == 0) ENG_LOGW("chiptune", "в песне нет паттернов");
ENG_LOGI("chiptune", "паттернов %d, в порядке %d", song.PatternCount(),
         static_cast<int>(song.order.size()));
```

### `f32 ChipSong::DurationSeconds(int orderIndex = -1) const`

Оценка длительности в секундах: сумма `rowCount` всех паттернов от `orderIndex`
(по умолчанию — от начала) до конца списка, умноженная на
`ticksPerRow / framesPerSecond`. Зацикливание не учитывается, поэтому для
зацикленной песни это длина одного прохода — именно её использует `Bake`,
когда `seconds == 0`.

```cpp
const crossrender::f32 full = song.DurationSeconds();
const crossrender::f32 fromThird = song.DurationSeconds(3);
ENG_LOGI("chiptune", "полная %.1f с, с третьего порядка %.1f с", full, fromThird);
```

### `int ChipSong::TotalRows() const`

Суммарное число рядов по всем паттернам из `order` (тоже без учёта цикла).
Удобно для оценки объёма трека и для прогресс-бара.

```cpp
ENG_LOGI("chiptune", "в треке %d рядов", song.TotalRows());
```

### `void ChipSong::AddPattern(const ChipPattern& p)`

Добавляет паттерн в конец `patterns` и **не** трогает `order` — не забудьте
дописать индекс, иначе паттерн никто не сыграет.

```cpp
crossrender::ChipPattern p;
p.Resize(16, 4);
song.AddPattern(p);
song.order.push_back(song.PatternCount() - 1);   // и сразу в порядок
```

### `class ChipPlayer`

Проигрыватель: хранит собственную копию песни (через `std::shared_ptr<const
ChipSong>`), ведёт секвенсор, синтезирует голоса и отдаёт PCM либо в реальном
времени (`RenderAdd`), либо офлайн (`Bake`). Объект move-независим, но
копирование запрещено.

```cpp
crossrender::ChipPlayer player;
player.SetSong(crossrender::ChipPlayer::MakeNamed("boss", 3u));
player.SetSampleRate(44100);
player.SetMasterVolume(0.8f);
player.Play();
ENG_LOGI("chiptune", "играем '%s'", player.Song().title.c_str());
```

### `ChipPlayer::ChipPlayer()`

Создаёт плеер без песни: `HasSong() == false`, рендерить нечего. Всё
остальное (частота, громкость, зацикливание) уже имеет разумные значения по
умолчанию.

```cpp
crossrender::ChipPlayer player;
if (!player.HasSong()) ENG_LOGI("chiptune", "плеер пуст, ждём SetSong");
```

### `ChipPlayer::~ChipPlayer()`

Освобождает внутреннее состояние и копию песни. Плеер должен умирать раньше
клипа, который из него испекли, — сам клип владеет своими сэмплами и от плеера
не зависит.

```cpp
crossrender::AudioClip baked;
{
    crossrender::ChipPlayer player;
    player.SetSong(crossrender::ChipPlayer::Make8BitSong(1u));
    baked = player.Bake(44100, 8.0f);    // клип живёт дальше плеера
}
ENG_LOGI("chiptune", "испекли %.1f с", baked.Duration());
```

### `void ChipPlayer::SetSong(const ChipSong& song)`

Отдаёт плееру песню: она **копируется** и нормализуется — `channelCount`
зажимается в 1…32, список `channels` дополняется, `framesPerSecond` — в
1…1000, пустой `order` заменяется на `{0}`, индексы порядка зажимаются по
числу паттернов, `loopOrder` вне диапазона становится `-1`. Настройки
сбрасываются: позиция на начало, голоса пересозданы, эхо пересчитано.
Зацикливание включается автоматически, если `loopOrder >= 0`.

```cpp
crossrender::ChipSong song = crossrender::ChipPlayer::MakeTitleSong(5u);
crossrender::ChipPlayer player;
player.SetSong(song);

// song можно менять дальше: плеер держит собственную копию.
song.title = "changed after SetSong";
ENG_LOGI("chiptune", "у плеера всё ещё '%s'", player.Song().title.c_str());
```

### `const ChipSong& ChipPlayer::Song() const`

Доступ к внутренней (нормализованной) копии песни. Если песни нет, возвращает
ссылку на статическую пустую `ChipSong` — безопасно, но `HasSong()` всё равно
стоит проверять, чтобы не рисовать пустой трекер.

```cpp
if (player.HasSong()) {
    const crossrender::ChipSong& s = player.Song();
    ENG_LOGI("chiptune", "%s, %d каналов, %.1f с", s.title.c_str(), s.channelCount,
             s.DurationSeconds());
}
```

### `bool ChipPlayer::HasSong() const`

`true`, если песня задана и её можно играть или печь.

```cpp
crossrender::ChipPlayer player;
if (!player.HasSong()) player.SetSong(crossrender::ChipPlayer::MakeNamed("dance", 4u));
```

### `void ChipPlayer::Play()`

Запускает воспроизведение. Если плеер ещё не играл или песня закончилась,
состояние сбрасывается на начало и сразу триггерится первый ряд; иначе просто
снимается пауза. Позиция сохраняется при `Pause`.

```cpp
player.Play();
ENG_LOGI("chiptune", "играем=%d", player.IsPlaying() ? 1 : 0);
```

### `void ChipPlayer::Pause()`

Ставит секвенсор на паузу: позиция, тик и фазы голосов сохраняются, звук
прекращается. Продолжить — `Play()`.

```cpp
if (menuOpen) player.Pause();
else          player.Play();
```

### `void ChipPlayer::Stop()`

Останавливает и **сбрасывает** воспроизведение на начало (в отличие от
`Pause`). Следующий `Play()` начнёт песню заново.

```cpp
player.Stop();
player.SetOrder(2);     // начнём с третьего порядка
player.Play();
```

### `void ChipPlayer::SetLoop(bool loop)`

Включает/выключает зацикливание на уровне плеера (независимо от
`ChipSong::loopOrder`). При `loop == false` конец списка порядка завершает
песню: голоса отпускаются, а после полусекунды тишины `IsPlaying()` станет
`false`. `SetSong` выставляет флаг по `loopOrder >= 0`.

```cpp
player.SetLoop(true);      // играть по кругу
// ...
player.SetLoop(false);     // доиграть до конца и остановиться
```

### `bool ChipPlayer::IsPlaying() const`

`true`, пока секвенсор идёт (в том числе в хвосте после конца песни, пока
звучат отпущенные голоса).

```cpp
if (!player.IsPlaying()) {
    ENG_LOGI("chiptune", "трек закончился");
}
```

### `void ChipPlayer::SetSpeed(int ticksPerRow)`

Переопределяет `ticksPerRow` на лету; значение зажимается в 1…64. Действует до
следующего `SetSong`. Эффект `ChipFxSpeed` в песне делает то же самое.

```cpp
player.SetSpeed(3);        // быстрее
player.SetSpeed(12);       // медленнее
```

### `void ChipPlayer::SetOrder(int orderIndex)`

Переход к другому элементу списка порядка: индекс зажимается в
`[0, order.size() - 1]`, ряд и тик сбрасываются, флаги «конец» и «прыжок»
снимаются. Если плеер играет (или уже стартовал), соответствующий ряд
триггерится сразу, поэтому звук меняется без перезапуска.

```cpp
// Прыжок к припеву без остановки.
player.SetOrder(3);
ENG_LOGI("chiptune", "порядок %d, ряд %d", player.CurrentOrder(), player.CurrentRow());
```

### `void ChipPlayer::SetChannelMute(int channel, bool muted)`

Заглушает отдельный канал (например, соло-дорожку в редакторе). Индекс вне
диапазона игнорируется; состояние видно в `ChannelStates()` и
`ChannelMuted()`.

```cpp
player.SetChannelMute(3, true);      // выключили барабаны
if (player.ChannelMuted(3)) ENG_LOGI("chiptune", "канал 3 в муте");
```

### `bool ChipPlayer::ChannelMuted(int channel) const`

Текущее состояние мута канала; для индекса вне диапазона — `false`.

```cpp
for (int ch = 0; ch < player.Song().channelCount; ++ch) {
    ENG_LOGI("chiptune", "канал %d: %s", ch, player.ChannelMuted(ch) ? "мут" : "играет");
}
```

### `void ChipPlayer::SetMasterVolume(f32 v)`

Общая громкость синтеза; зажимается в 0…4 и применяется до мягкого лимитера.
Не путать с `crossrender::Audio::SetMasterVolume` — тот работает уже после микшера.

```cpp
player.SetMasterVolume(0.55f);
```

### `void ChipPlayer::SetSampleRate(int hz)`

Частота, в которую ресемплируется внутренний поток 32000 Гц; зажимается в
8000…192000. Совпадение с частотой устройства (`Audio::SampleRate()`) избавляет
от повторного ресемплинга в микшере.

```cpp
player.SetSampleRate(crossrender::Audio::Get().SampleRate());
player.SetSampleRate(22050);      // дешевле по CPU и памяти
```

### `void ChipPlayer::RenderAdd(f32* out, int frameCount)`

Главный метод реального времени: продвигает секвенсор и **добавляет**
`frameCount` стерео-кадров в `out`. Буфер не обнуляется — вызывающий обязан
подготовить его (обычно нулями). Рендер идёт кусками, выровненными по границам
тиков; после конца песни плеер ждёт ~0.5 с тишины и только потом снимает
`IsPlaying`.

Этот же метод вызывает `Bake`; для синхронизации интерфейса без звука есть
`Advance`.

```cpp
// Свой цикл: добавляем чиптюн поверх уже готового микса.
std::vector<crossrender::f32> block(512 * 2, 0.0f);
player.RenderAdd(block.data(), 512);
// block теперь содержит чиптюн; сложите его с остальным миксом.
```

### `void ChipPlayer::Advance(f32 dt)`

Продвигает секвенсор на `dt` секунд **без** синтеза звука. Нужен тихому
режиму и экранному трекеру: подсветка ряда и счётчики остаются в синхроне,
даже когда аудио никто не слушает. Если песня закончилась, `IsPlaying()`
снимается.

```cpp
// Headless: звука нет, но UI-трекер должен бежать.
if (crossrender::Audio::Get().Silent()) {
    player.Advance(dt);
}
```

### `using RowCallback = void (*)(int order, int row, void* user)`

Тип колбэка о начале ряда. Это обычный указатель на функцию (не
`std::function`): захватывать состояние нельзя, контекст передаётся через
`user`.

```cpp
void OnRow(int order, int row, void* user) {
    auto* ui = static_cast<MyTrackerUi*>(user);
    ui->Highlight(order, row);
}
```

### `void ChipPlayer::SetRowCallback(RowCallback cb, void* user)`

Регистрирует колбэк, который вызывается при триггере каждого ряда — для
подсветки в интерфейсе. `user` вернётся в колбэк без изменений. Передайте
`nullptr`, чтобы отключить.

```cpp
MyTrackerUi ui;
crossrender::ChipPlayer player;
player.SetRowCallback(&OnRow, &ui);
player.SetSong(crossrender::ChipPlayer::Make8BitSong(1u));
player.Play();
```

### `AudioClip ChipPlayer::Bake(int sampleRate = 44100, f32 seconds = 0.0f, f32 loopFadeSeconds = 0.05f, int maxSeconds = 240) const`

Офлайн-рендер песни в обычный `AudioClip` (см. раздел «Как `Bake` превращает
песню в клип»). `sampleRate` зажимается в 8000…192000, `maxSeconds` — в
1…3600, а длина кроссфейда — в `[0, frames / 4]`. `seconds == 0` означает
«вся песня по `DurationSeconds`». Возвращает невалидный клип, если песни нет
или длительность нулевая. Константный: сам плеер не меняется, работа идёт на
локальной копии. **Дорого** — вызывайте один раз при входе в сцену.

```cpp
crossrender::ChipPlayer player;
player.SetSong(crossrender::ChipPlayer::MakeNamed("title", 5u));
player.SetSampleRate(22050);
player.SetMasterVolume(0.55f);

// 30 секунд, кроссфейд 0.05 с, не больше минуты — один раз на сцену.
crossrender::AudioClip music = player.Bake(22050, 30.0f, 0.05f, 60);
ENG_LOGI("chiptune", "испекли %.1f с, валиден=%d", music.Duration(), music.Valid());
```

### `void ChipPlayer::RenderTo(std::vector<f32>* out, int frames)`

Рендер офлайн в вектор: `out` **перезаписывается** (assign `frames * 2` нулей,
затем `RenderAdd`). В отличие от `Bake`, ничего не кодирует в WAV и не
зацикливает: просто `frames` стерео-кадров с текущей позиции. Годится для
спектрограмм, анализа и своих форматов экспорта.

```cpp
std::vector<crossrender::f32> pcm;
player.RenderTo(&pcm, 4410);        // 0.1 с при 44100 Гц
ENG_LOGI("chiptune", "сэмплов: %d", static_cast<int>(pcm.size()));
```

### `struct ChipPlayer::ChannelState`

Живое состояние канала для интерфейса: нота, частота, уровень, фаза, эффект.
Поля сгруппированы.

| Поле | Смысл |
|---|---|
| `bool active` | Звучит ли голос. |
| `int note` | Текущий полутон (`-1` — нет). |
| `f32 frequency` | Текущая частота в герцах (с учётом слайдов и вибрато). |
| `int volume` | Уровень 0…15 после огибающей. |
| `f32 phase` | Нормированная фаза генератора 0…1 (для осциллограммы). |
| `int effect` | Последний применённый эффект. |
| `int arpStep` | Шаг арпеджио (0…2) или 0, если арпеджио выключено. |
| `bool muted` | Мут канала. |
| `std::string lastNoteName` | Имя последней взятой ноты (`"A#3"`). |

```cpp
const std::vector<crossrender::ChipPlayer::ChannelState>& states = player.ChannelStates();
for (crossrender::usize ch = 0; ch < states.size(); ++ch) {
    const crossrender::ChipPlayer::ChannelState& s = states[ch];
    ENG_LOGD("chiptune", "канал %d: %s %.1f Гц, громкость %d", static_cast<int>(ch),
             s.lastNoteName.c_str(), s.frequency, s.volume);
}
```

### `const std::vector<ChannelState>& ChipPlayer::ChannelStates() const`

Состояние всех каналов, обновляемое после каждого рендера/`Advance`.
Размер вектора равен `channelCount` песни (0 без песни).

```cpp
const crossrender::usize n = player.ChannelStates().size();
for (crossrender::usize i = 0; i < n; ++i) {
    if (player.ChannelStates()[i].active) {
        // рисуем индикатор канала
    }
}
```

### `int ChipPlayer::CurrentOrder() const`

Индекс в списке порядка, который играет сейчас.

```cpp
ENG_LOGI("chiptune", "порядок %d из %d", player.CurrentOrder(),
         static_cast<int>(player.Song().order.size()));
```

### `int ChipPlayer::CurrentRow() const`

Текущий ряд внутри паттерна. Вместе с `CurrentOrder` даёт позицию для
подсветки в трекере.

```cpp
ENG_LOGI("chiptune", "порядок %d, ряд %d", player.CurrentOrder(), player.CurrentRow());
```

### `int ChipPlayer::CurrentTick() const`

Текущий тик внутри ряда: `0…TicksPerRow()-1`. Обновляется каждый тик, в
отличие от `CurrentRow`.

```cpp
if (player.CurrentTick() == 0) {
    // начало ряда: подсветили строку
}
```

### `f32 ChipPlayer::CurrentTime() const`

Время с начала воспроизведения в секундах (`renderedFrames / sampleRate`).
Растёт и при зацикливании, сбрасывается `Stop`/`Play`/`SetSong`.

```cpp
const crossrender::f32 t = player.CurrentTime();
ENG_LOGI("chiptune", "играем %.2f с", t);
```

### `u64 ChipPlayer::RenderedFrames() const`

Сколько стерео-кадров плеер отдал за всё время — «одометр» синтеза. По нему
удобно считать реальную нагрузку `Bake`.

```cpp
const crossrender::u64 before = player.RenderedFrames();
crossrender::AudioClip clip = player.Bake(44100, 10.0f);
ENG_LOGI("chiptune", "Bake отрендерил %llu кадров",
         static_cast<unsigned long long>(player.RenderedFrames() - before));
```

### `f32 ChipPlayer::LastPeak() const`

Пиковый уровень последнего отрендеренного блока (0…1) — для VU-метра. При
каждом `RenderAdd` сбрасывается и заполняется заново; `Bake` его не портит,
так как работает на копии.

```cpp
const crossrender::f32 peak = player.LastPeak();
vuMeter = peak > vuMeter ? peak : vuMeter * 0.9f;    // плавный спад
```

### `static ChipSong ChipPlayer::Make8BitSong(u64 seed = 1)`

Готовая NES-песня «Neon Cavern»: ля-минор, 4 канала (два импульса,
треугольник, шум), 50 Гц, 6 тиков на ряд (125 BPM), зациклена с нулевого
порядка. `seed` управляет громкостью хэтов и вариантом филла; `0` заменяется
на 1.

```cpp
crossrender::ChipSong song = crossrender::ChipPlayer::Make8BitSong(1u);
crossrender::ChipPlayer player;
player.SetSong(song);
```

### `static ChipSong ChipPlayer::Make16BitSong(u64 seed = 2)`

SPC-песня «Glass Skyline»: 8 семплерных голосов, ре-мажор, эхо 180 мс и
включённый ФНЧ (7000 Гц), 60 Гц, 9 тиков на ряд (100 BPM). Самый «богатый» по
звуку встроенный трек и самый дорогой в рендере.

```cpp
crossrender::ChipSong song = crossrender::ChipPlayer::Make16BitSong(7u);
song.echoFeedback = 110;
song.lowPassCutoff = 6000;
crossrender::ChipPlayer player;
player.SetSong(song);
```

### `static ChipSong ChipPlayer::MakeBossSong(u64 seed = 3)`

Быстрая агрессивная NES-песня «Iron Warden»: 150 BPM, шестнадцатые арпеджио,
драйвовый треугольный бас и шумовые удары.

```cpp
crossrender::ChipPlayer boss;
boss.SetSong(crossrender::ChipPlayer::MakeBossSong(3u));
boss.SetSpeed(6);      // ещё быстрее
boss.Play();
```

### `static ChipSong ChipPlayer::MakeDanceSong(u64 seed = 4)`

Танцевальный трек «Circuit Bloom»: бочка на каждую долю (треугольник с
`ChipFxNoteCut`), бас между долями, хэты на слабых восьмых и клэпы на 2 и 4;
125 BPM.

```cpp
crossrender::ChipPlayer dance;
dance.SetSong(crossrender::ChipPlayer::MakeDanceSong(4u));
ENG_LOGI("chiptune", "'%s' %.1f с", dance.Song().title.c_str(), dance.Song().DurationSeconds());
```

### `static ChipSong ChipPlayer::MakeTitleSong(u64 seed = 5)`

Спокойная мелодичная песня «Quiet Horizon» в до-мажоре: 90 BPM, длинные ноты,
разреженные барабаны. Хороший выбор для меню и «мирных» сцен.

```cpp
crossrender::ChipPlayer title;
title.SetSong(crossrender::ChipPlayer::MakeTitleSong(5u));
title.SetMasterVolume(0.55f);
title.Play();
```

### `static ChipSong ChipPlayer::MakeNamed(const std::string& name, u64 seed = 0)`

Выбор встроенной песни по имени. Понимает `"16bit"` (а также `"16-bit"`,
`"snes"`, `"spc"`), `"boss"`, `"dance"`, `"title"`; всё остальное (включая
`"8bit"`) даёт 8-битную песню. Имя сравнивается без учёта регистра. `seed == 0`
подставляет «родной» seed песни (1…5), поэтому одно и то же имя всегда даёт
один и тот же трек.

```cpp
for (const std::string& name : crossrender::ChipPlayer::BuiltinNames()) {
    crossrender::ChipSong song = crossrender::ChipPlayer::MakeNamed(name, 7u);
    ENG_LOGI("chiptune", "%-6s -> '%s' (%.1f с)", name.c_str(), song.title.c_str(),
             song.DurationSeconds());
}
```

### `static std::vector<std::string> ChipPlayer::BuiltinNames()`

Имена встроенных песен в порядке, который использует инструмент ассетов:
`"8bit"`, `"16bit"`, `"boss"`, `"dance"`, `"title"`. По ним же строятся имена
файлов `audio/song_<name>.chip`.

```cpp
const std::vector<std::string> names = crossrender::ChipPlayer::BuiltinNames();
ENG_LOGI("chiptune", "встроенных треков: %d", static_cast<int>(names.size()));
```

### `static std::string ChipPlayer::Serialize(const ChipSong& song)`

Кодирует песню в компактный текстовый формат (строка `#GE-CHIPTUNE 1`,
пары `ключ=значение`, строки `channel ...`, `pattern=Имя,рядов,каналов`,
`row ...` — по одному на ряд и `order=...`). Ячейка — либо `...` (значения по
умолчанию), либо шесть полей через запятую: нота, инструмент, громкость,
эффект, параметр, арп. Формат терпим к будущим расширениям и удобен для
diff-ов в репозитории.

```cpp
crossrender::ChipSong song = crossrender::ChipPlayer::Make8BitSong(1u);
const std::string text = crossrender::ChipPlayer::Serialize(song);
ENG_LOGI("chiptune", "текст трека: %d байт", static_cast<int>(text.size()));
ENG_LOGD("chiptune", "%s", text.substr(0, 80).c_str());
```

### `static bool ChipPlayer::Deserialize(const std::string& text, ChipSong* out, std::string* error = nullptr)`

Разбирает текст обратно в песню. Терпит пустые строки, комментарии `#` и
неизвестные ключи (они игнорируются), но на структурных ошибках честно
возвращает `false` и заполняет `error`: нет заголовка, битое число, ячейка не
из шести полей, ряд не той длины, порядок ссылается на несуществующий паттерн,
нет паттернов или нет списка порядка. При ошибке `out` остаётся частично
заполненным — используйте результат только при `true`.

```cpp
const std::string text = "#GE-CHIPTUNE 1\ntitle=Test\nchannels=1\n"
                         "channel wave=Pulse50 volume=12\n"
                         "pattern=Main,2,1\n"
                         "row C-4,0,255,0,0,255\n"
                         "row ---,0,255,0,0,255\n"
                         "order=0\n";
crossrender::ChipSong song;
std::string error;
if (!crossrender::ChipPlayer::Deserialize(text, &song, &error)) {
    ENG_LOGE("chiptune", "текст не разобрался: %s", error.c_str());
}
```

### `static bool ChipPlayer::SaveToFile(const ChipSong& song, const std::string& path)`

Пишет `Serialize(song)` через `WriteTextFile`. При ошибке пишет `ENG_LOGE` и
возвращает `false`. Родительский каталог должен существовать.

```cpp
const std::string path = crossrender::PathJoin(crossrender::GetUserRoot(), "song_draft.chip");
if (crossrender::ChipPlayer::SaveToFile(song, path)) {
    ENG_LOGI("chiptune", "черновик сохранён в %s", path.c_str());
}
```

### `static bool ChipPlayer::LoadFromFile(const std::string& path, ChipSong* out, std::string* error = nullptr)`

Читает файл через `ReadTextFile` и разбирает его `Deserialize`. Именно так
читаются ассеты `assets/audio/song_*.chip`, которые пишет `tools/mkassets`.
Пустой или нечитаемый файл даёт `false` и текст ошибки.

```cpp
const std::string path = crossrender::PathJoin(crossrender::GetAssetRoot(), "audio/song_boss.chip");
crossrender::ChipSong song;
std::string error;
if (!crossrender::ChipPlayer::LoadFromFile(path, &song, &error)) {
    ENG_LOGW("chiptune", "%s не прочитан (%s), берём встроенный трек", path.c_str(),
             error.c_str());
    song = crossrender::ChipPlayer::MakeNamed("boss", 3u);
}
```

### `static std::string ChipPlayer::NoteName(int semitone)`

Имя ноты в формате трекера: `"C-4"`, `"A#3"`. Особые случаи: `-1` даёт
`"---"` (пусто), `-2` — `"OFF"` (note off). Отрицательные значения меньше `-2`
трактуются как `0`.

```cpp
ENG_LOGI("chiptune", "57 = %s, 70 = %s", crossrender::ChipPlayer::NoteName(57).c_str(),
         crossrender::ChipPlayer::NoteName(70).c_str());
ENG_LOGI("chiptune", "пусто %s, обрыв %s", crossrender::ChipPlayer::NoteName(-1).c_str(),
         crossrender::ChipPlayer::NoteName(-2).c_str());
```

### `static f32 ChipPlayer::NoteFrequency(int semitone)`

Частота полутона в герцах по стандартному строю (A-4 = 57 = 440 Гц). Для
отрицательных значений — `0` (звука нет).

```cpp
const crossrender::f32 a4 = crossrender::ChipPlayer::NoteFrequency(57);      // 440
const crossrender::f32 c4 = crossrender::ChipPlayer::NoteFrequency(48);      // ~261.6
const crossrender::f32 none = crossrender::ChipPlayer::NoteFrequency(-1);    // 0
ENG_LOGI("chiptune", "A-4=%.1f, C-4=%.1f, пусто=%.1f", a4, c4, none);
```

### `std::vector<f32> MakeSingleCycleWaveform(ChipWave wave, int samples, f32 phase = 0.0f)`

Строит один период волны как таблицу из `samples` сэмплов. Форма считается
рядом Фурье для выбранной волны (`Pulse*`/`Square` — с заданной скважностью,
`Triangle`, `Saw`, `Sine`; `Sample` — яркий гармонически богатый тембр;
`Noise` — детерминированный псевдослучайный цикл). Постоянная составляющая
вычитается, пик нормируется ровно в 1. `samples <= 0` даёт пустой вектор, а
`samples == 1` — единственный нулевой сэмпл.

Эту же функцию использует семплерный движок для таблиц голосов и экранный
дисплей волны.

```cpp
// Таблица для осциллограммы и для собственного волнового канала.
std::vector<crossrender::f32> saw = crossrender::MakeSingleCycleWaveform(crossrender::ChipWave::Saw, 64);
std::vector<crossrender::f32> pulse = crossrender::MakeSingleCycleWaveform(crossrender::ChipWave::Pulse25, 128, 0.25f);
ENG_LOGI("chiptune", "пила: %d сэмплов, импульс: %d", static_cast<int>(saw.size()),
         static_cast<int>(pulse.size()));
```

## Пример целиком

```cpp
#include "crossrender/audio/Chiptune.h"

#include "crossrender/audio/Audio.h"
#include "crossrender/core/File.h"
#include "crossrender/core/Log.h"

#include <memory>
#include <string>
#include <vector>

// Музыкальный слой сцены: берём .chip-ассет (или генерируем трек), печём его
// в AudioClip один раз при входе и играем через обычный микшер. Работает и в
// тихом режиме: тогда просто крутим Advance, чтобы трекер в UI не замер.
class ChipMusic {
public:
    void OnEnter(const std::string& name) {
        const crossrender::ChipSong builtin = crossrender::ChipPlayer::MakeNamed(name, 7u);

        // Ассет, который пишет tools/mkassets, приоритетнее сгенерированной песни.
        const std::string assetPath = crossrender::PathJoin(crossrender::GetAssetRoot(), "audio/song_" + name + ".chip");
        crossrender::ChipSong song = builtin;
        std::string error;
        if (crossrender::FileExists(assetPath) && crossrender::ChipPlayer::LoadFromFile(assetPath, &song, &error)) {
            source_ = "asset";
        } else {
            source_ = "generated";
            if (!error.empty()) {
                ENG_LOGW("chiptune", "%s: %s", assetPath.c_str(), error.c_str());
            }
        }

        song_ = std::make_shared<const crossrender::ChipSong>(song);
        player_.reset(new crossrender::ChipPlayer());
        player_->SetSong(*song_);
        player_->SetSampleRate(22050);
        player_->SetLoop(true);
        player_->SetMasterVolume(0.55f);
        player_->SetRowCallback(&ChipMusic::OnRow, this);
        player_->Play();

        // Bake дорогой: ровно один раз на вход в сцену и только при живом звуке.
        if (!crossrender::Audio::Get().Silent()) {
            clip_ = player_->Bake(22050, 24.0f, 0.05f, 48);
            if (clip_.Valid()) {
                crossrender::PlayParams p;
                p.looping = true;
                p.volume = 0.6f;
                p.bus = 0;
                voice_ = crossrender::Audio::Get().Play(clip_, p);
            }
        }
        ENG_LOGI("chiptune", "'%s' (%s), %d паттернов, %.1f с", song_->title.c_str(),
                 source_, song_->PatternCount(), song_->DurationSeconds());
    }

    void Update(crossrender::f32 dt) {
        if (player_ == nullptr) return;

        // С устройством звук идёт сам; без устройства двигаем секвенсор вручную.
        if (crossrender::Audio::Get().Silent()) player_->Advance(dt);

        // VU-метр из пика последнего блока.
        const crossrender::f32 peak = player_->LastPeak();
        vu_ = peak > vu_ ? peak : vu_ * 0.9f;
    }

    void ToggleChannel(int channel) {
        if (player_ == nullptr) return;
        player_->SetChannelMute(channel, !player_->ChannelMuted(channel));
    }

    void DrawTrackerRow() const {
        if (player_ == nullptr || song_ == nullptr) return;
        // Подсветка текущей позиции: порядок/ряд/тик плюс живое состояние каналов.
        const std::vector<crossrender::ChipPlayer::ChannelState>& states = player_->ChannelStates();
        for (crossrender::usize ch = 0; ch < states.size(); ++ch) {
            const bool playing = states[ch].active;
            ENG_LOGD("chiptune", "o=%d r=%d t=%d ch=%d %s %s", player_->CurrentOrder(),
                     player_->CurrentRow(), player_->CurrentTick(), static_cast<int>(ch),
                     states[ch].lastNoteName.c_str(), playing ? "on" : "off");
        }
    }

    void SaveDraft() {
        if (song_ == nullptr) return;
        const std::string path = crossrender::PathJoin(crossrender::GetUserRoot(), "song_draft.chip");
        if (crossrender::ChipPlayer::SaveToFile(*song_, path)) {
            ENG_LOGI("chiptune", "черновик (%d байт) в %s",
                     static_cast<int>(crossrender::ChipPlayer::Serialize(*song_).size()), path.c_str());
        }
    }

    void OnExit() {
        if (voice_ != crossrender::kInvalidVoice) {
            crossrender::Audio::Get().Stop(voice_, 0.15f);
            voice_ = crossrender::kInvalidVoice;
        }
        if (player_ != nullptr) player_->Stop();
    }

private:
    static void OnRow(int order, int row, void* user) {
        auto* self = static_cast<ChipMusic*>(user);
        self->lastRow_ = row;
        self->lastOrder_ = order;
    }

    std::shared_ptr<const crossrender::ChipSong> song_;
    std::unique_ptr<crossrender::ChipPlayer> player_;
    crossrender::AudioClip clip_;
    crossrender::VoiceId voice_ = crossrender::kInvalidVoice;
    const char* source_ = "none";
    int lastOrder_ = 0;
    int lastRow_ = 0;
    crossrender::f32 vu_ = 0.0f;
};
```

## См. также

* `docs/audio/Audio.md` — микшер, в который отдаётся испечённый `AudioClip`, и
  тихий режим `Silent()`, от которого зависит стратегия воспроизведения.
* `docs/core/File.md` — `WriteTextFile`/`ReadTextFile`, `PathJoin`,
  `GetAssetRoot` и `FileExists`, на которых стоят `SaveToFile`/`LoadFromFile`.
* `docs/core/Math.md` — `Random`, которым сеются встроенные песни.
* `docs/Engine.md` — конфигурация, `--headless` и жизненный цикл сцены, в
  который встраивается `Bake` при входе.
* `docs/core/Log.md` — предупреждения `ENG_LOGW("chiptune", ...)` о битых
  треках и неудачном `Bake`.
