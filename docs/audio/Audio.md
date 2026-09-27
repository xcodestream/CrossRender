# crossrender/audio/Audio.h — микшер, аудиоклипы и 3D-звук

Звуковой движок: декодирование WAV/OGG/MP3 в PCM, программный миксер с
голосами и шинами, позиционное 3D-звучание и платформенный вывод
(CoreAudio / WASAPI / ALSA / OpenSL ES / WebAudio) с обязательным «тихим»
режимом, когда устройства нет.

## Заголовок

```cpp
#include "crossrender/audio/Audio.h"
```

## Обзор

Заголовок делится на три части: `AudioClip` (неизменяемый буфер PCM),
параметрические структуры (`PlayParams`, `ListenerState`, `AudioBus`) и
синглтон-микшер `Audio`.

### Устройство микшера: голоса, шины, мастер

`Audio` — один на процесс (`Audio::Get()`), потому что владеет платформенным
устройством вывода. Внутри — фиксированный массив из 64 **голосов** (voice:
проигрываемый в данный момент экземпляр клипа) и таблица **шин** (bus:
групповая громкость). Шина с индексом `0` всегда называется `"master"` и
создаётся конструктором.

Итоговый коэффициент каждого голоса считается так:

```
gain = voice.volume * gain(шина и все её родители) * masterVolume * distance
```

* `SetMasterVolume` — общий множитель поверх всего.
* `SetBusVolume("music", …)` / `SetBusMuted` — групповая ручка; мут любой шины
  в цепочке родителей обнуляет звук целиком.
* `PlayParams::bus` — индекс шины; неизвестный индекс сводится к `0`.
* `SetVoiceVolume` / `SetVoicePitch` — на конкретный голос.
* Сверху всего стоит мягкий лимитер (`SoftClip`): ниже порога `0.75` сигнал
  линеен, выше — асимптотически сжимается, поэтому «горячий» микс не
  заворачивается по знаку.

Голосов ровно 64; если свободных нет, микшер **отбирает** самый тихий
(при равенстве — самый старый). Шин максимум 32.

```cpp
crossrender::Audio& audio = crossrender::Audio::Get();
audio.Init(44100, 2, 1024);

const int music = audio.AddBus("music", 0);   // дочерняя шина мастера
const int sfx = audio.AddBus("sfx", 0);

audio.SetBusVolume("music", 0.6f);
audio.SetBusVolume("sfx", 0.9f);
audio.SetMasterVolume(0.8f);

crossrender::PlayParams p;
p.bus = music;
p.looping = true;
crossrender::VoiceId theme = audio.Play(themeClip, p);
```

### Тихий режим: `Silent()` и `Initialised()`

`Audio::Init` **всегда** возвращает `true`: он инициализирует миксер и лишь
сообщает через `Silent()`, удалось ли поднять реальное устройство. Сборка без
поддержки звука, запуск с `--headless`, отсутствие звуковой карты или
переменная окружения `ENG_AUDIO_NULL=1` дают `Silent() == true`.

В этом состоянии **все вызовы обязаны оставаться безопасными** — и остаются:
`Play` возвращает корректный `VoiceId`, микшер считает время, но звук никуда
не уходит. Разница только в том, кто двигает миксер:

* с устройством — платформенный колбэк вызывает `Mix` из аудиопотока, а
  `Update(dt)` становится no-op;
* без устройства — микшер нужно продвигать самому: `Update(dt)` (в игровом
  цикле) или `PumpSilent(frames)` (в тестах).

Поэтому типовой код выглядит так: `Init` → работа → `Update(dt)` каждый кадр.
Дополнительно полезно проверять `Silent()` перед дорогой подготовкой звука.

```cpp
crossrender::Audio& audio = crossrender::Audio::Get();
audio.Init();
ENG_LOGI("audio", "инициализирован=%d, тихий=%d", audio.Initialised(), audio.Silent());

if (audio.Silent()) {
    ENG_LOGW("audio", "устройства нет: звук не слышен, но микшер работает");
}

// Без устройства микшер двигает игровой цикл.
audio.Update(dt);
```

### Время жизни клипа и голоса

`Play` **не копирует** PCM: голос хранит указатель на `AudioClip`. Клип должен
жить (и не перемещаться) до остановки голоса. `AudioClip` — move-only:
копирование запрещено, поэтому держите клипы в устойчивом месте (поле сцены,
`std::shared_ptr`, стабильный контейнер), а не во временном выражении.

Второе правило: **не доверяйте клипу**. Результат `LoadFromFile` /
`LoadFromMemory` надо проверять через `Valid()`, а `Play` с пустым клипом
возвращает `kInvalidVoice` и пишет предупреждение. Любой другой вызов с
`kInvalidVoice` или устаревшим идентификатором — безопасный no-op.

Идентификатор голоса — `u32`, в котором упакованы индекс слота и «поколение»:
слот можно переиспользовать, но старый `VoiceId` после этого перестаёт
находиться, поэтому `IsPlaying(старый)` вернёт `false`, а не «чужой» звук.

```cpp
crossrender::AudioClip clip;
crossrender::VoiceId voice = crossrender::kInvalidVoice;
if (clip.LoadFromFile("audio/click.wav") && clip.Valid()) {
    voice = crossrender::Audio::Get().Play(clip);
}
if (voice == crossrender::kInvalidVoice) {
    ENG_LOGW("audio", "щелчок не загрузился — просто молчим");
}
// clip должен жить всё время, пока voice играет.
```

### Зацикливание и затухания

* `PlayParams::looping` зацикливает воспроизведение. Если в WAV был чанк
  `smpl` с точками петли, повтор идёт по этому региону (`LoopStart`/`LoopEnd`),
  иначе — по всему клипу.
* `PlayParams::fadeIn` — линейное появление за указанное число секунд.
* `Stop(voice, fadeOut)` — линейное затухание; голос остаётся активным, пока
  затухание не закончится (то есть `IsPlaying` ещё некоторое время `true`).
* `StopAll(fadeOut)` и `StopBus("sfx", fadeOut)` останавливают группу целиком.
* `Pause`/`Resume` останавливают счёт позиции, но **не** освобождают голос:
  приостановленный голос продолжает считаться активным и занимать слот.

```cpp
crossrender::PlayParams p;
p.volume = 0.8f;
p.looping = true;
p.fadeIn = 1.5f;                  // плавное вступление музыки
crossrender::VoiceId music = audio.Play(themeClip, p);

audio.Stop(music, 0.5f);          // голос доживёт полсекунды и исчезнет сам
audio.StopAll(0.0f);              // мгновенно всё
```

### 3D-позиционный звук

Для позиционного звука включите `PlayParams::spatial` и задайте `position`.
Микшер каждый буфер считает для голоса:

1. расстояние `dist` от `position` до `Audio::Listener().position`;
2. коэффициент расстояния по `rolloff`: `0` — нет затухания, `1` (по
   умолчанию) — `minDistance / dist`, `2` — линейно до нуля на `maxDistance`,
   `3` — `(minDistance / dist)²`. Ближе `minDistance` — всегда `1`, дальше
   `maxDistance` — `0`;
3. панораму из проекции направления на слушателя: `right = Normalize(Cross(
   listener.forward, listener.up))`, `pan = Dot(delta / dist, right)`, затем
   равномощностная кривая `cos/sin`.

`SetVoicePosition` двигает источник без перезапуска, `SetListener` — уши.
Слушателя удобно обновлять из камеры один раз в кадр.

```cpp
crossrender::ListenerState l;
l.position = camera.position;
l.forward = camera.Forward();
l.up = camera.Up();
audio.SetListener(l);

crossrender::PlayParams p;
p.spatial = true;
p.position = {4.0f, 1.0f, -2.0f};
p.minDistance = 1.0f;
p.maxDistance = 30.0f;
p.rolloff = 1;                    // inverse
crossrender::VoiceId engine = audio.Play(engineLoop, p);
audio.SetVoicePosition(engine, {4.5f, 1.0f, -2.5f});   // источник поехал
```

## Члены класса

### `enum class AudioFormat : u8`

Формат контейнера, из которого декодирован клип, и подсказка для
`LoadFromMemory`. Клипы, синтезированные кодом (`MakeTone`, `MakeNoise`,
`MakeSilence`) и клипы, испечённые `ChipPlayer::Bake`, сообщают `Unknown` —
PCM уже готов, контейнера у него нет.

| Значение | Смысл |
|---|---|
| `AudioFormat::Unknown` | Формат не определён (обычно процедурный клип). |
| `AudioFormat::Wav` | RIFF/WAVE: PCM 8/16/24/32 бит или IEEE float 32/64. |
| `AudioFormat::Ogg` | Ogg Vorbis (stb_vorbis). |
| `AudioFormat::Mp3` | MPEG Audio (minimp3). |

```cpp
crossrender::AudioClip clip;
if (clip.LoadFromFile("audio/theme.ogg")) {
    if (clip.Format() == crossrender::AudioFormat::Ogg) ENG_LOGI("audio", "загружен Vorbis");
}
```

### `class AudioClip`

Декодированный PCM: плоский массив `f32` в диапазоне примерно `[-1, 1]`,
чередующийся по каналам (interleaved). Класс владеет буфером, не копируется,
но перемещается. Один и тот же клип можно проигрывать несколькими голосами
одновременно.

```cpp
crossrender::AudioClip clip;
if (!clip.LoadFromFile("audio/step.wav")) {
    ENG_LOGE("audio", "шаг не загрузился");
}
ENG_LOGI("audio", "%d каналов, %d Гц, %.2f с", clip.Channels(), clip.SampleRate(),
         clip.Duration());
```

### `AudioClip::AudioClip()`

Конструктор по умолчанию: пустой (невалидный) клип. Годится как поле класса,
которое заполняется позже.

```cpp
crossrender::AudioClip clip;
ENG_LOGI("audio", "валиден=%d, каналов=%d", clip.Valid(), clip.Channels());
```

### `AudioClip::~AudioClip()`

Освобождает буфер. Клип обязан пережить все голоса, которые его играют:
`Play` не продлевает ему жизнь. Если клип уничтожить раньше, микшер заметит
невалидный источник на следующем буфере и снимет голос.

```cpp
{
    crossrender::AudioClip oneShot;
    oneShot.LoadFromFile("audio/blip.wav");
    crossrender::VoiceId v = crossrender::Audio::Get().Play(oneShot);
    // Клип умрёт здесь; голос v будет снят на ближайшем буфере.
    (void)v;
}
```

### `AudioClip::AudioClip(AudioClip&&) noexcept`

Перемещающий конструктор: буфер переезжает без копирования, источник остаётся
пустым. Именно поэтому клипы удобно возвращать из функций и складывать в
контейнеры.

```cpp
crossrender::AudioClip MakeBlip() {
    crossrender::AudioClip c = crossrender::AudioClip::MakeTone(880.0f, 0.1f);
    return c;                       // перемещение, а не копия
}

crossrender::AudioClip blip = MakeBlip();
ENG_LOGI("audio", "длительность %.3f с", blip.Duration());
```

### `AudioClip& AudioClip::operator=(AudioClip&&) noexcept`

Перемещающее присваивание: старый буфер получателя освобождается, содержимое
источника переезжает. Копирование (`AudioClip(const AudioClip&)`) удалено —
случайное копирование большого PCM не скомпилируется.

```cpp
crossrender::AudioClip current = crossrender::AudioClip::MakeTone(440.0f, 0.5f);
current = crossrender::AudioClip::MakeNoise(0.25f);      // перемещение
ENG_LOGI("audio", "теперь это шум на %.2f с", current.Duration());
```

### `bool AudioClip::LoadFromFile(const std::string& path)`

Читает файл через файловую систему движка и декодирует его. Формат
подсказывается расширением (`.wav`/`.wave`, `.ogg`/`.oga`, `.mp3`), иначе —
сигнатурой. При успехе заполняет `SourcePath()`. При ошибке пишет `ENG_LOGE` и
возвращает `false`, **не трогая** предыдущее содержимое клипа.

```cpp
crossrender::AudioClip music;
if (!music.LoadFromFile("audio/music_theme.wav")) {
    ENG_LOGW("audio", "музыка недоступна, играем без неё");
} else {
    ENG_LOGI("audio", "загружено %.1f с из %s", music.Duration(), music.SourcePath().c_str());
}
```

### `bool AudioClip::LoadFromMemory(const void* data, usize size, AudioFormat hint = AudioFormat::Unknown)`

Декодирует PCM прямо из памяти: удобно для встроенных ассетов и для клипа,
испечённого `ChipPlayer::Bake` (тот отдаёт настоящий WAV в памяти). `hint`
позволяет не угадывать формат. При неудаче возвращает `false`, логирует
причину и оставляет клип в прежнем состоянии.

```cpp
// Клип из «сырых» байтов в памяти (например, скачанных по сети).
std::vector<crossrender::u8> bytes = DownloadBytes("https://example.invalid/beep.wav");
crossrender::AudioClip clip;
if (!clip.LoadFromMemory(bytes.data(), bytes.size(), crossrender::AudioFormat::Wav)) {
    ENG_LOGE("audio", "декодирование из памяти не удалось");
}
```

### `void AudioClip::Destroy()`

Освобождает PCM и сбрасывает все метаданные: каналы, частоту, точки петли,
формат и путь. После вызова `Valid() == false`, `Duration() == 0`. Полезно,
чтобы вернуть память, не дожидаясь разрушения клипа.

```cpp
clip.Destroy();
ENG_LOGI("audio", "после Destroy: %d каналов, валиден=%d", clip.Channels(), clip.Valid());
```

### `bool AudioClip::Valid() const`

`true`, если в клипе есть хотя бы один сэмпл. Это **обязательная** проверка
перед `Play` и перед сложной подготовкой звука.

```cpp
crossrender::AudioClip clip;
clip.LoadFromFile("audio/explosion.wav");
if (clip.Valid()) {
    crossrender::Audio::Get().Play(clip);
} else {
    ENG_LOGW("audio", "взрыв без звука: клип пуст");
}
```

### `const std::vector<f32>& AudioClip::Samples() const`

Доступ к плоскому буферу PCM без копирования. Чередование по каналам:
кадр `f` канала `c` лежит по индексу `f * Channels() + c`. Полезно для
осциллограмм, анализа и собственных эффектов.

```cpp
const std::vector<crossrender::f32>& pcm = clip.Samples();
ENG_LOGI("audio", "сэмплов: %d", static_cast<int>(pcm.size()));
if (!pcm.empty() && clip.Channels() == 1) {
    ENG_LOGI("audio", "первый сэмпл %.4f", pcm[0]);
}
```

### `int AudioClip::Channels() const`

Число каналов (1 или 2 для типичных файлов; декодер WAV принимает до 8).
Пустой клип сообщает 0.

```cpp
if (clip.Channels() == 2) {
    ENG_LOGI("audio", "стерео: используем панораму");
} else {
    ENG_LOGI("audio", "моно: микшер продублирует канал");
}
```

### `int AudioClip::SampleRate() const`

Частота дискретизации клипа в герцах. Микшер сам пересчитывает её в частоту
устройства и учитывает в `pitch`; несовпадение частот — норма.

```cpp
ENG_LOGI("audio", "клип %d Гц, устройство %d Гц", clip.SampleRate(),
         crossrender::Audio::Get().SampleRate());
```

### `f32 AudioClip::Duration() const`

Длительность в секундах: `frameCount() / SampleRate()`. У пустого клипа или
клипа с нулевой частотой — `0`.

```cpp
const crossrender::f32 d = clip.Duration();
if (d > 0.0f && d < 0.2f) ENG_LOGI("audio", "это короткий эффект (%.3f с)", d);
```

### `usize AudioClip::frameCount() const`

Число кадров (сэмплов на канал). Имя намеренно в нижнем регистре, чтобы не
путать с `Channels()`: кадр — это набор всех каналов в один момент времени.
Для пустого клипа — 0.

```cpp
const crossrender::usize frames = clip.frameCount();
ENG_LOGI("audio", "кадров: %d", static_cast<int>(frames));
```

### `const std::string& AudioClip::SourcePath() const`

Путь, из которого клип был загружен через `LoadFromFile`. Для клипов из памяти
и процедурных — пустая строка.

```cpp
if (!clip.SourcePath().empty()) ENG_LOGI("audio", "источник: %s", clip.SourcePath().c_str());
```

### `AudioFormat AudioClip::Format() const`

Формат-контейнер загруженного клипа (`Unknown` для процедурных и «испечённых»
клипов).

```cpp
if (clip.Format() == crossrender::AudioFormat::Unknown) {
    ENG_LOGI("audio", "клип синтезирован кодом, а не прочитан из файла");
}
```

### `u32 AudioClip::LoopStart() const`

Начало региона петли из WAV-чанка `smpl`, в кадрах. `0`, если чанка нет или
регион бессмысленный (тогда `PlayParams::looping` зацикливает весь клип).

```cpp
if (clip.LoopStart() != 0 || clip.LoopEnd() != 0) {
    ENG_LOGI("audio", "файл задаёт петлю %u..%u", clip.LoopStart(), clip.LoopEnd());
}
```

### `u32 AudioClip::LoopEnd() const`

Конец региона петли в кадрах (полуоткрытый интервал: значение уже
нормировано к размеру клипа). `0`, если петли нет.

```cpp
const crossrender::u32 end = clip.LoopEnd();
if (end > clip.LoopStart()) ENG_LOGI("audio", "петля длиной %u кадров", end - clip.LoopStart());
```

### `static bool AudioClip::DecodeWav(const void* data, usize size, std::vector<f32>* out, int* channels, int* sampleRate, std::string* error)`

Прямой вызов WAV-декодера (собственный RIFF-парсер): PCM 8/16/24/32 бита,
IEEE float 32/64, `WAVE_FORMAT_EXTENSIBLE`, до 8 каналов. Пишет PCM в `out`,
метаданные — в `channels`/`sampleRate`, текст ошибки — в `error`. Удобно для
тестов и для собственного разбора файлов в обход `AudioClip`.

```cpp
std::vector<crossrender::f32> pcm;
int channels = 0, rate = 0;
std::string error;
if (!crossrender::AudioClip::DecodeWav(bytes.data(), bytes.size(), &pcm, &channels, &rate, &error)) {
    ENG_LOGE("audio", "WAV не разобрался: %s", error.c_str());
}
```

### `static bool AudioClip::DecodeOgg(const void* data, usize size, std::vector<f32>* out, int* channels, int* sampleRate, std::string* error)`

Декодер Ogg Vorbis (stb_vorbis). Работает только с целым потоком в памяти.

```cpp
std::vector<crossrender::f32> pcm;
int channels = 0, rate = 0;
std::string error;
if (crossrender::AudioClip::DecodeOgg(oggBytes.data(), oggBytes.size(), &pcm, &channels, &rate, &error)) {
    ENG_LOGI("audio", "Vorbis: %d канал(ов), %d Гц", channels, rate);
}
```

### `static bool AudioClip::DecodeMp3(const void* data, usize size, std::vector<f32>* out, int* channels, int* sampleRate, std::string* error)`

Декодер MPEG Audio (minimp3). Как и остальные декодеры, не бросает исключений:
ошибка возвращается строкой.

```cpp
std::vector<crossrender::f32> pcm;
int channels = 0, rate = 0;
std::string error;
if (!crossrender::AudioClip::DecodeMp3(mp3Bytes.data(), mp3Bytes.size(), &pcm, &channels, &rate, &error)) {
    ENG_LOGW("audio", "MP3 не декодировался: %s", error.c_str());
}
```

### `static AudioFormat AudioClip::DetectFormat(const void* data, usize size)`

Определяет формат по сигнатуре: `RIFF`+`WAVE`, `OggS`, `ID3` или синхрослово
MPEG-кадра. Возвращает `Unknown`, если ничего не совпало — тогда
`LoadFromMemory` честно сообщит об unrecognised-формате.

```cpp
const crossrender::AudioFormat fmt = crossrender::AudioClip::DetectFormat(bytes.data(), bytes.size());
if (fmt == crossrender::AudioFormat::Unknown) {
    ENG_LOGE("audio", "неизвестный контейнер, нужен hint");
}
```

### `static AudioClip AudioClip::MakeTone(f32 frequency, f32 durationSeconds, f32 sampleRate = 44100.0f, int channels = 2)`

Синтезирует синусоиду — заглушка для тестов и для прототипов без ассетов.
`channels < 1` поднимается до 1, `sampleRate <= 0` — до 44100,
отрицательная длительность — до 0. Формат — `Unknown`, путь пуст. Фаза
накапливается в `f64`, поэтому на длинных тонах нет «уплывающей» частоты.

```cpp
crossrender::AudioClip beep = crossrender::AudioClip::MakeTone(660.0f, 0.15f, 44100.0f, 1);
if (beep.Valid()) crossrender::Audio::Get().Play(beep);
```

### `static AudioClip AudioClip::MakeNoise(f32 durationSeconds, f32 sampleRate = 44100.0f, int channels = 2)`

Синтезирует белый шум **детерминированно** (фиксированный seed
`0x5EED1234`): два вызова дают одинаковый буфер, что удобно для тестов.

```cpp
crossrender::AudioClip noise = crossrender::AudioClip::MakeNoise(0.3f);
crossrender::PlayParams p;
p.volume = 0.4f;
p.pan = -0.5f;
crossrender::Audio::Get().Play(noise, p);
```

### `static AudioClip AudioClip::MakeSilence(f32 durationSeconds, f32 sampleRate = 44100.0f, int channels = 2)`

Синтезирует тишину заданной длительности — полезно как «пустышка», чтобы
сохранить структуру кода с клипом, и как точный отсчёт длительности в тестах.

```cpp
crossrender::AudioClip gap = crossrender::AudioClip::MakeSilence(0.5f);
ENG_LOGI("audio", "пауза %.2f с, каналов %d", gap.Duration(), gap.Channels());
```

### `using VoiceId = u32`

Идентификатор голоса: `u32`, в котором упакованы индекс слота и поколение.
Ноль зарезервирован под «невалидный» (`kInvalidVoice`), поэтому значение
`0` никогда не выдаётся за живой голос.

```cpp
crossrender::VoiceId v = crossrender::Audio::Get().Play(clip);
ENG_LOGI("audio", "голос %u", static_cast<unsigned>(v));
```

### `constexpr VoiceId kInvalidVoice = 0`

Сентинел «голоса нет». Возвращается из `Play`, если клип пуст или если слот
выделить не удалось. Все методы, принимающие `VoiceId`, безопасно принимают
`kInvalidVoice` и ничего не делают.

```cpp
crossrender::VoiceId v = crossrender::kInvalidVoice;
if (clip.Valid()) v = crossrender::Audio::Get().Play(clip);
if (v == crossrender::kInvalidVoice) {
    ENG_LOGW("audio", "звук не запущен");
} else {
    crossrender::Audio::Get().Stop(v, 0.1f);
}
```

### `struct PlayParams`

Параметры одного запуска: громкость, тон, панорама, цикл, затухание, шина и
3D. Поля сгруппированы по ролям (2D-воспроизведение и 3D-воспроизведение) —
это параметрическая структура, а не набор методов.

```cpp
crossrender::PlayParams p;
p.volume = 0.7f;
p.pitch = 1.1f;      // чуть выше — «тот же» звук звучит иначе
p.pan = -0.3f;       // левее центра
p.looping = false;
crossrender::Audio::Get().Play(stepClip, p);
```

### Поля `PlayParams`: `volume`, `pitch`, `pan`, `looping`, `stream`, `startTimeOffset`, `fadeIn`, `bus`

| Поле | По умолчанию | Смысл |
|---|---|---|
| `f32 volume` | `1.0f` | Громкость голоса; отрицательная поднимается до 0. |
| `f32 pitch` | `1.0f` | Множитель скорости/тона; `<= 0` заменяется на 1. |
| `f32 pan` | `0.0f` | Панорама 2D: `-1` влево, `+1` вправо; зажимается в `[-1, 1]`. |
| `bool looping` | `false` | Зациклить воспроизведение. |
| `bool stream` | `false` | Принимается для совместимости, но **игнорируется**: клип уже в памяти. |
| `f64 startTimeOffset` | `0.0` | Старт не с начала, в секундах. |
| `f32 fadeIn` | `0.0f` | Линейное появление за секунды; `<= 0` — без него. |
| `int bus` | `0` | Индекс шины; неизвестный сводится к мастер-шине `0`. |

```cpp
crossrender::PlayParams p;
p.volume = 0.9f;
p.pitch = 0.95f;
p.pan = 0.25f;
p.looping = false;
p.stream = false;            // поле есть, но ничего не меняет
p.startTimeOffset = 0.75;    // начать с 0.75 с
p.fadeIn = 0.2f;
p.bus = crossrender::Audio::Get().AddBus("sfx", 0);
crossrender::Audio::Get().Play(sfxClip, p);
```

### Поля `PlayParams` для 3D: `spatial`, `position`, `minDistance`, `maxDistance`, `rolloff`

| Поле | По умолчанию | Смысл |
|---|---|---|
| `bool spatial` | `false` | Включить позиционное затухание и панораму. |
| `Vec3 position` | `{0, 0, 0}` | Позиция источника в мире. |
| `f32 minDistance` | `1.0f` | Ближе — полная громкость. |
| `f32 maxDistance` | `30.0f` | Дальше — тишина. |
| `int rolloff` | `1` | `0` нет, `1` inverse, `2` linear, `3` exponential. |

```cpp
crossrender::PlayParams p;
p.spatial = true;
p.position = {8.0f, 0.0f, 0.0f};
p.minDistance = 2.0f;
p.maxDistance = 40.0f;
p.rolloff = 2;                 // линейное затухание
crossrender::Audio::Get().Play(windLoop, p);
```

### `struct ListenerState`

Положение и ориентация «ушей» слушателя, от которых считается 3D-звук.
Обычно заполняется из камеры один раз в кадр. Поля сгруппированы.

| Поле | По умолчанию | Смысл |
|---|---|---|
| `Vec3 position` | `{0, 0, 0}` | Точка прослушивания. |
| `Vec3 forward` | `{0, 0, -1}` | Направление взгляда. |
| `Vec3 up` | `{0, 1, 0}` | Вектор «вверх». |
| `Vec3 velocity` | `{0, 0, 0}` | Скорость слушателя; хранится, но текущий миксер её не использует. |

```cpp
crossrender::ListenerState l;
l.position = {0.0f, 1.7f, 0.0f};
l.forward = {0.0f, 0.0f, -1.0f};
l.up = {0.0f, 1.0f, 0.0f};
crossrender::Audio::Get().SetListener(l);
```

### `struct AudioBus`

Шина групповой громкости. Индекс `0` — всегда `"master"` с `parent == -1`;
остальные создаются `AddBus` и ссылаются на родителя по индексу. Поля
сгруппированы.

| Поле | По умолчанию | Смысл |
|---|---|---|
| `std::string name` | `""` | Имя для поиска (`SetBusVolume` и т. п.). |
| `f32 volume` | `1.0f` | Множитель; отрицательный поднимается до 0. |
| `bool muted` | `false` | Мут: обнуляет всю ветку, включая потомков. |
| `int parent` | `-1` | Индекс родительской шины; `-1` — корень. |

```cpp
crossrender::AudioBus master;
master.name = "master";
master.volume = 1.0f;
master.parent = -1;
ENG_LOGI("audio", "шина %s, родитель %d", master.name.c_str(), master.parent);
```

### `class Audio`

Синглтон-микшер. Создаётся лениво при первом `Get()`, живёт до конца
программы, в деструкторе вызывает `Shutdown()`. Потокобезопасен: все мутации
состояния голосов и шин идут под мьютексом, а `Mix` вызывается из
аудиопотока и не выделяет память.

```cpp
crossrender::Audio& audio = crossrender::Audio::Get();
audio.Init(48000, 2, 512);
audio.SetMasterVolume(0.9f);
ENG_LOGI("audio", "%d Гц, %d каналов", audio.SampleRate(), audio.Channels());
```

### `static Audio& Audio::Get()`

Единственная точка доступа к микшеру (Meyers singleton). Первый вызов создаёт
объект; уничтожается он при выходе из процесса.

```cpp
crossrender::Audio& a = crossrender::Audio::Get();
crossrender::Audio& same = crossrender::Audio::Get();
ENG_LOGI("audio", "это один объект: %d", (&a == &same) ? 1 : 0);
```

### `bool Audio::Init(int sampleRate = 44100, int channels = 2, int bufferFrames = 1024)`

Инициализирует миксер и пытается поднять платформенное устройство.
`sampleRate <= 0` → 44100, `channels == 1` → моно, иначе стерео,
`bufferFrames <= 0` → 1024. Повторный вызов безвреден: если уже
инициализировано, сразу возвращает `true`.

**Всегда возвращает `true`.** Отсутствие устройства — не ошибка, а
`Silent() == true`; после вызова `Initialised()` тоже `true`, и можно играть
звук «в никуда».

```cpp
crossrender::Audio& audio = crossrender::Audio::Get();
if (!audio.Init(44100, 2, 1024)) {
    ENG_LOGE("audio", "микшер не поднялся");      // практически недостижимо
}
ENG_LOGI("audio", "тихий режим: %d", audio.Silent() ? 1 : 0);
```

### `void Audio::Shutdown()`

Останавливает устройство и глушит все голоса, затем сбрасывает
`Initialised() == false` и `Silent() == true`. Повторный вызов безопасен.
После `Shutdown` микшер можно снова поднять тем же `Init`.

```cpp
audio.Shutdown();
ENG_LOGI("audio", "инициализирован=%d, тихий=%d", audio.Initialised(), audio.Silent());

audio.Init();       // поднимаем заново
```

### `bool Audio::Initialised() const`

`true` после успешного `Init` и `false` после `Shutdown`. **Не** означает, что
есть звук: за это отвечает `Silent()`. Именно эту пару стоит проверять перед
дорогой подготовкой (например, перед `ChipPlayer::Bake`).

```cpp
if (!audio.Initialised() || audio.Silent()) {
    ENG_LOGW("audio", "звука нет — пропускаем тяжёлый синтез");
    return;
}
```

### `bool Audio::Silent() const`

`true`, когда реального устройства вывода нет: миксер считает время, но
результат никуда не отправляется. Ставится в `true` при неудаче
`PlatformAudioInit`, при `ENG_AUDIO_NULL=1` и после `Shutdown`.

```cpp
if (audio.Silent()) {
    // Тяжёлую музыку можно не печь: её всё равно никто не услышит.
}
```

### `int Audio::SampleRate() const`

Частота устройства, принятая в `Init` (по умолчанию 44100). Микшер
пересчитывает клипы любой частоты в эту.

```cpp
ENG_LOGI("audio", "устройство %d Гц", audio.SampleRate());
```

### `int Audio::Channels() const`

Число каналов устройства: 1 или 2. Моно-голоса суммируются, стерео — панорамируются.

```cpp
if (audio.Channels() == 1) ENG_LOGI("audio", "моно-вывод");
```

### `VoiceId Audio::Play(const AudioClip& clip, const PlayParams& params = {})`

Запускает клип и возвращает идентификатор голоса. Пустой клип не запускается:
метод пишет `ENG_LOGW` и возвращает `kInvalidVoice`. Если все 64 голоса заняты,
отбирается самый тихий (при равенстве — самый старый). Клип берётся по
указателю — он должен жить, пока голос играет.

Не требует `Initialised()`: в тихом режиме голос всё равно создаётся, и его
состоянием можно управлять (`IsPlaying`, `VoiceTime`).

```cpp
crossrender::Audio& audio = crossrender::Audio::Get();
crossrender::VoiceId v = crossrender::kInvalidVoice;
if (clip.Valid()) {
    crossrender::PlayParams p;
    p.volume = 0.8f;
    p.looping = true;
    v = audio.Play(clip, p);
}
```

### `VoiceId Audio::PlayMusic(const AudioClip& clip, f32 volume = 1.0f, bool looping = true)`

Удобная обёртка: ставит `looping`, `stream = true` (поле всё равно
игнорируется) и автоматически выбирает шину с именем `"music"`, если она
существует; иначе — мастер-шину `0`.

```cpp
crossrender::VoiceId theme = crossrender::Audio::Get().PlayMusic(themeClip, 0.5f, true);
if (theme == crossrender::kInvalidVoice) ENG_LOGW("audio", "музыка не запустилась");
```

### `bool Audio::Stop(VoiceId voice, f32 fadeOut = 0.0f)`

Останавливает голос. При `fadeOut <= 0` голос снимается сразу; при
положительном — включается линейное затухание, и голос живёт ещё `fadeOut`
секунд (в это время `IsPlaying` возвращает `true`). Возвращает `false` для
`kInvalidVoice` и устаревших идентификаторов.

```cpp
if (audio.Stop(v, 0.4f)) {
    ENG_LOGI("audio", "голос уходит в затухание");
} else {
    ENG_LOGI("audio", "голос уже неактуален");
}
```

### `void Audio::StopAll(f32 fadeOut = 0.0f)`

Останавливает **все** активные голоса (включая приостановленные) с общим
затуханием или мгновенно. Типичное применение — смена сцены.

```cpp
void OnLeaveScene() {
    crossrender::Audio::Get().StopAll(0.3f);   // мягкий уход со сцены
}
```

### `void Audio::StopBus(const std::string& bus, f32 fadeOut = 0.0f)`

Останавливает все голоса, которые играют через указанную шину **или любую её
дочернюю** шину (обход дерева родителей). Неизвестное имя — no-op.

```cpp
audio.StopBus("sfx", 0.0f);      // выключить эффекты, музыка остаётся
```

### `void Audio::Pause(VoiceId voice)`

Приостанавливает голос: позиция воспроизведения замирает, звук пропадает, но
слот **не** освобождается. Пауза идёт с точностью до буфера микшера.

```cpp
if (pausePressed) {
    audio.Pause(v);
} else {
    audio.Resume(v);
}
```

### `void Audio::Resume(VoiceId voice)`

Снимает паузу, поставленную `Pause`, продолжая с того же места. Для не
приостановленного голоса — no-op.

```cpp
audio.Pause(v);
audio.Update(dt);        // тишина, но время голоса не идёт
audio.Resume(v);         // играем дальше с той же позиции
```

### `void Audio::SetVoiceVolume(VoiceId voice, f32 volume)`

Меняет громкость голоса на лету (отрицательная поднимается до 0). Годится для
плавного приглушения музыки без перезапуска.

```cpp
if (paused) audio.SetVoiceVolume(v, 0.15f);   // приглушить под меню
else        audio.SetVoiceVolume(v, 0.8f);
```

### `void Audio::SetVoicePitch(VoiceId voice, f32 pitch)`

Меняет тон/скорость голоса; значение зажимается в `[0, 8]`. Меняет и высоту, и
темп (ресемплирование), позиция при этом не сбрасывается.

```cpp
audio.SetVoicePitch(v, 0.8f);    // замедлили и понизили
audio.SetVoicePitch(v, 1.25f);   // ускорили и повысили
```

### `void Audio::SetVoicePosition(VoiceId voice, const Vec3& position)`

Двигает источник 3D-звука. Имеет смысл только для голосов, запущенных с
`PlayParams::spatial == true`: для остальных позиция просто хранится.

```cpp
crossrender::PlayParams p;
p.spatial = true;
p.position = {0.0f, 0.0f, 0.0f};
crossrender::VoiceId engine = audio.Play(engineClip, p);

// Источник едет вместе с машиной.
audio.SetVoicePosition(engine, carPosition);
```

### `bool Audio::IsPlaying(VoiceId voice) const`

`true`, пока голос занимает слот: играет, стоит на паузе или догорает после
`Stop` с затуханием. Для `kInvalidVoice`, несуществующего и уже снятого голоса
— `false`. Это основной способ не запускать звук повторно.

```cpp
if (musicVoice == crossrender::kInvalidVoice || !audio.IsPlaying(musicVoice)) {
    musicVoice = audio.PlayMusic(themeClip, 0.5f, true);
}
```

### `f32 Audio::VoiceTime(VoiceId voice) const`

Сколько секунд голос реально отрендерил (суммарное «стенное» время, а не
позиция в клипе). Растёт и при зацикливании, и во время затухания; для
неизвестного голоса — 0. Удобно для синхронизации анимации с музыкой.

```cpp
const crossrender::f32 t = audio.VoiceTime(musicVoice);
const crossrender::f32 beat = 0.5f;                    // 120 BPM
if (t > 0.0f) {
    const crossrender::f32 phase = std::fmod(t, beat) / beat;
    dancerPose = phase;                        // анимация в такт
}
```

### `int Audio::AddBus(const std::string& name, int parent = 0)`

Создаёт шину и возвращает её индекс. Если шина с таким именем уже есть —
возвращает существующий индекс. `parent` вне диапазона (или отрицательный)
сводится к мастер-шине `0`; при достижении лимита в 32 шины возвращается `-1`
с предупреждением.

```cpp
const int ui = audio.AddBus("ui", 0);
const int musicBus = audio.AddBus("music", 0);
if (ui < 0) ENG_LOGW("audio", "шин больше нет");
ENG_LOGI("audio", "шины: %d и %d", ui, musicBus);
```

### `void Audio::SetBusVolume(const std::string& name, f32 volume)`

Громкость шины (отрицательная поднимается до 0). Работает и с `"master"`:
мастер-шина — обычная шина в дереве, поэтому её громкость умножается поверх
`SetMasterVolume`. Неизвестное имя даёт предупреждение и ничего не меняет.

```cpp
audio.SetBusVolume("music", 0.5f);
audio.SetBusVolume("master", 1.0f);
```

### `void Audio::SetBusMuted(const std::string& name, bool muted)`

Мут шины: при `true` вся ветка (шина и её потомки) замолкает, потому что
`BusGain` возвращает 0 при встрече с мутом. Неизвестное имя — предупреждение.

```cpp
audio.SetBusMuted("sfx", true);    // игрок выключил эффекты в настройках
```

### `f32 Audio::BusVolume(const std::string& name) const`

Собственная громкость шины (без учёта родителей). Для неизвестной шины — `0`
(не `1`), поэтому проверяйте имя заранее, если значение важно.

```cpp
ENG_LOGI("audio", "музыка на %.2f", audio.BusVolume("music"));
if (audio.BusVolume("music") <= 0.0f) ENG_LOGW("audio", "шина music не создана или выключена");
```

### `void Audio::SetMasterVolume(f32 v)`

Общий множитель поверх всех шин и голосов. Не зажимается: значения больше 1
формально допустимы, но упрутся в мягкий лимитер. Обычно это «ползунок
громкости» в настройках.

```cpp
audio.SetMasterVolume(0.7f);
ENG_LOGI("audio", "мастер %.2f", audio.MasterVolume());
```

### `f32 Audio::MasterVolume() const`

Текущий мастер-множитель (по умолчанию 1.0).

```cpp
if (audio.MasterVolume() <= 0.001f) ENG_LOGI("audio", "звук выключен пользователем");
```

### `void Audio::SetListener(const ListenerState& l)`

Задаёт позицию и ориентацию слушателя для 3D-звука. Вызывайте раз в кадр
после обновления камеры.

```cpp
crossrender::ListenerState l;
l.position = camera.position;
l.forward = camera.Forward();
l.up = camera.up;
audio.SetListener(l);
```

### `const ListenerState& Audio::Listener() const`

Текущее состояние слушателя — например, чтобы дорисовать индикатор или
посчитать расстояние до источника вручную.

```cpp
const crossrender::ListenerState& l = audio.Listener();
const crossrender::f32 d = crossrender::Length(crossrender::Vec3(4, 0, 0) - l.position);
ENG_LOGI("audio", "источник в %.2f м", d);
```

### `void Audio::Mix(f32* out, int frameCount)`

Низкоуровневый микс: заполняет `frameCount` кадров чередующегося `f32` в
буфер `out` (сначала обнуляет его, затем складывает голоса). Вызывается
платформенным колбэком из аудиопотока; тесты вызывают его напрямую для
детерминированной проверки. Не выделяет память; мьютекс держится один буфер.

```cpp
// Детерминированная проверка в тесте: 512 кадров стерео.
std::vector<crossrender::f32> buffer(512 * 2, 0.0f);
audio.Mix(buffer.data(), 512);
ENG_LOGI("audio", "после микса голосов активно: %d", audio.ActiveVoices());
```

### `void Audio::Update(f32 dt)`

Продвигает микшер **без устройства**: переводит `dt` в кадры с накоплением
дробной части и вызывает `PumpSilent`. Если устройство есть и `Silent() ==
false`, метод ничего не делает — там миксер двигает колбэк. Вызывайте каждый
кадр, иначе в тихом режиме голоса «замрут» и никогда не закончатся.

```cpp
while (running) {
    const crossrender::f32 dt = frameClock.Delta();
    crossrender::Audio::Get().Update(dt);      // no-op при живом устройстве
    Render();
}
```

### `void Audio::PumpSilent(int frames = 1024)`

Один прогон микшера в тишину: миксует `frames` кадров во внутренний
scratch-буфер. Именно этот вызов делают тесты, чтобы «проиграть» время без
устройства. Кадры можно задавать произвольно, чтобы приблизить поведение
реального колбэка.

```cpp
// Прогоняем 2 секунды звука без устройства: 44100 * 2 кадров.
crossrender::Audio::Get().PumpSilent(44100 * 2);
```

### `int Audio::ActiveVoices() const`

Сколько слотов сейчас занято (включая приостановленные и догорающие). Значение
совпадает с `Stats::voices`, которое микшер пишет в конце буфера.

```cpp
ENG_LOGI("audio", "занято голосов: %d из %d", audio.ActiveVoices(), audio.GetStats().maxVoices);
```

### `struct Audio::Stats`

Снимок состояния микшера, который заполняется в `Mix`. Поля сгруппированы.

| Поле | Смысл |
|---|---|
| `int voices` | Активных голосов в последнем буфере. |
| `int maxVoices` | Предел движка: 64. |
| `f32 cpuLoad` | Грубая оценка `voices / maxVoices`, а не измерение времени. |
| `u64 mixedFrames` | Сколько кадров микшер отрендерил за всё время. |
| `int underruns` | Задуман под счётчик недоборов; см. ограничение ниже. |

```cpp
const crossrender::Audio::Stats& s = audio.GetStats();
ENG_LOGI("audio", "голосов %d/%d, загрузка %.0f%%, кадров %llu", s.voices, s.maxVoices,
         s.cpuLoad * 100.0f, static_cast<unsigned long long>(s.mixedFrames));
```

### `const Stats& Audio::GetStats() const`

Ссылка на текущую статистику. Счётчик `underruns` платформенные бэкенды ведут
в собственной переменной и в `Stats` **не копируют**, поэтому в этой версии он
всегда `0` — не полагайтесь на него.

```cpp
if (audio.GetStats().voices >= audio.GetStats().maxVoices) {
    ENG_LOGW("audio", "все 64 голоса заняты: новый звук вытеснит самый тихий");
}
```

### `static bool Audio::PlatformAudioInit(int sampleRate, int channels, int bufferFrames)`

Платформенный хук: поднимает реальный бэкенд (CoreAudio, WASAPI, ALSA,
OpenSL ES или WebAudio). `false` означает, что устройства нет. Обычно
вызывается из `Init`; вручную трогать не нужно. Переменная окружения
`ENG_AUDIO_NULL=1` заставляет сразу вернуть `false`.

```cpp
// Диагностика: есть ли шанс на звук в этой сборке/окружении.
if (!crossrender::Audio::PlatformAudioAvailable()) {
    ENG_LOGW("audio", "платформенный бэкенд не скомпилирован");
}
```

### `static void Audio::PlatformAudioShutdown()`

Платформенный хук остановки бэкенда. Вызывается из `Shutdown`; повторный вызов
безопасен, а при отсутствии активного бэкенда ничего не делает.

```cpp
crossrender::Audio::PlatformAudioShutdown();   // обычно не нужно: Shutdown() сделает это сам
```

### `static bool Audio::PlatformAudioAvailable()`

Сообщает, **скомпилирован** ли бэкенд для текущей платформы (macOS/iOS,
Windows, Linux, Android, WASM). Это не проверка наличия звуковой карты: она
может вернуть `true`, а `Audio::Init` всё равно уйдёт в тихий режим.

```cpp
if (!crossrender::Audio::PlatformAudioAvailable()) {
    ENG_LOGI("audio", "сборка без вывода: работаем в тихом режиме");
}
```

## Пример целиком

```cpp
#include "crossrender/audio/Audio.h"

#include "crossrender/core/Log.h"
#include "crossrender/gfx/Mesh.h"     // crossrender::Camera

#include <vector>

// Звуковой слой сцены: музыка на шине music, эффекты на шине sfx, позиционный
// источник, привязанный к объекту. Работает и без устройства вывода.
class GameAudio {
public:
    void Init() {
        crossrender::Audio& audio = crossrender::Audio::Get();
        audio.Init(44100, 2, 1024);

        musicBus_ = audio.AddBus("music", 0);
        sfxBus_ = audio.AddBus("sfx", 0);
        audio.SetBusVolume("music", 0.6f);
        audio.SetBusVolume("sfx", 0.9f);
        audio.SetMasterVolume(0.8f);

        if (!click_.LoadFromFile("audio/click.wav")) {
            ENG_LOGW("audio", "нет audio/click.wav — играем без щелчков");
        }
        // Музыку берём из памяти: подойдёт и клип, испечённый ChipPlayer::Bake.
        if (!music_.LoadFromMemory(musicBytes_.data(), musicBytes_.size(),
                                   crossrender::AudioFormat::Wav)) {
            ENG_LOGW("audio", "музыкальный клип не декодировался");
        }
    }

    void SetCamera(const crossrender::Camera& camera) {
        crossrender::ListenerState l;
        l.position = camera.position;
        l.forward = camera.Forward();
        l.up = camera.up;
        crossrender::Audio::Get().SetListener(l);
    }

    void StartMusic() {
        if (!music_.Valid()) return;
        crossrender::PlayParams p;
        p.volume = 1.0f;
        p.looping = true;
        p.fadeIn = 1.0f;
        p.bus = musicBus_;
        musicVoice_ = crossrender::Audio::Get().Play(music_, p);
    }

    void PlayClick() {
        if (!click_.Valid()) return;
        crossrender::PlayParams p;
        p.bus = sfxBus_;
        p.volume = 0.7f;
        crossrender::Audio::Get().Play(click_, p);
    }

    VoiceId StartEngine(const crossrender::Vec3& position) {
        crossrender::PlayParams p;
        p.spatial = true;
        p.position = position;
        p.minDistance = 1.0f;
        p.maxDistance = 35.0f;
        p.rolloff = 1;
        p.looping = true;
        engineVoice_ = crossrender::Audio::Get().Play(engine_, p);
        return engineVoice_;
    }

    void Update(crossrender::f32 dt, const crossrender::Vec3& carPosition, bool muted) {
        crossrender::Audio& audio = crossrender::Audio::Get();

        // В тихом режиме именно этот вызов двигает микшер; с устройством — no-op.
        audio.Update(dt);

        audio.SetVoicePosition(engineVoice_, carPosition);
        audio.SetBusMuted("sfx", muted);

        if (musicVoice_ != crossrender::kInvalidVoice && !audio.IsPlaying(musicVoice_)) {
            musicVoice_ = crossrender::kInvalidVoice;   // трек закончился или был остановлен
        }
    }

    void Shutdown() {
        crossrender::Audio& audio = crossrender::Audio::Get();
        audio.StopAll(0.25f);
        audio.Update(0.3f);       // даём затуханию «прозвучать» даже без устройства
        audio.Shutdown();
    }

private:
    crossrender::AudioClip music_;
    crossrender::AudioClip click_;
    crossrender::AudioClip engine_;
    std::vector<crossrender::u8> musicBytes_;
    int musicBus_ = 0;
    int sfxBus_ = 0;
    crossrender::VoiceId musicVoice_ = crossrender::kInvalidVoice;
    crossrender::VoiceId engineVoice_ = crossrender::kInvalidVoice;
};
```

## См. также

* `docs/audio/Chiptune.md` — `ChipPlayer::Bake`, который отдаёт готовый
  `AudioClip`, и трекерный синтез музыки без файлов.
* `docs/core/File.md` — `ReadBinaryFile`, `GetAssetRoot` и `PathExt`, на
  которых стоит `AudioClip::LoadFromFile`.
* `docs/core/Math.md` — `Vec3` и `Cross`/`Normalize`, используемые 3D-миксом;
  `Random` — в `MakeNoise`.
* `docs/core/Log.md` — предупреждения `ENG_LOGW("audio", ...)` о тихом режиме
  и ошибках декодирования.
* `docs/Engine.md` — `--headless` и конфигурация, из-за которых устройство
  может отсутствовать.
