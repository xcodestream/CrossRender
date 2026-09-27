// Chiptune: эмуляция 8-битных (NES 2A03 / Game Boy) и 16-битных (SNES SPC,
// Sega FM) звуковых чипов плюс небольшой трекер/секвенсор; всё сводится в PCM,
// чтобы звучать через обычный микшер crossrender::Audio.
//
// ===========================================================================
// Замечания о конструкции (документация, которую обещает заголовок)
// ===========================================================================
//
// Внутренняя частота семплирования
//   Каждый голос синтезируется на единой фиксированной внутренней частоте
//   (kInternalRate = 32000 Гц). Каналы NES/Game Boy переводят желаемую частоту
//   в аппаратный таймер *в тактах CPU* (1.789773 МГц NTSC, 1.662607 МГц PAL,
//   4.194304 МГц GB), поэтому квантование периода, шаги duty-секвенсора и
//   тактирование LFSR следуют арифметике реального чипа.
//   Стерео-микс затем конвертируется в запрошенную выходную частоту
//   дробным линейным (2-точечным) ресемплером и однополюсным DC-блокиратором,
//   работающими на выходной частоте. Мягкий лимитер удерживает результат в [-1, 1].
//
// Темп
//   `ChipSong::ticksPerRow` — авторитетный источник: ряд длится
//   `ticksPerRow / framesPerSecond` секунд. `beatsPerMinute` и
//   `rowsPerBeat` — музыкальные метаданные, поддерживаемые согласованными
//   фабриками песен (`bpm = framesPerSecond * 60 / (ticksPerRow * rowsPerBeat)`);
//   если `ticksPerRow` <= 0, плеер вычисляет его из BPM.
//   `SetSpeed()` переопределяет тики/ряд во время выполнения.
//
// Огибающие
//   attack/decay/release выражаются в 4-битных единицах по 6 мс (0 = мгновенно,
//   15 = 90 мс). 8-битные голоса квантуют итоговое усиление по 4-битным шагам
//   громкости NES (кратные 1/15). Семпл/FM-голоса используют непрерывное
//   значение. sustain — удерживаемый 4-битный уровень.
//
// «Аппаратные» поля канала
//   wave         выбирает генератор (см. KindForChip).
//   volume       аппаратная громкость 0..15; громкость ноты её переопределяет.
//   dutySweep    LFO duty импульса в шагах/сек; у семпл-голосов это частота
//                LFO модуляции высоты в шагах/сек.
//   pitchSweep   изгиб высоты в стиле NES на ряд (единицы 1/16 полутона); у
//                семпл-голосов это глубина модуляции высоты в центах.
//   arpeggio     цикл тоника / +3 / +7 полутонов каждый тик («каждый фрейм»).
//   echoVolume   только SPC: посыл эха на голос (оставлено для полноты;
//                используется глобальный блок эха).
//   pan          -8..8 -> панорама равной мощности.
//   detune       статическая расстройка в центах.
//
// Ноты и эффекты
//   Шумовой канал трактует номер ноты как один из 16 элементов таблицы
//   аппаратных периодов (0 = самый высокий тон). ChipNote::arp (0..127) —
//   фиксированное смещение в полутонах на время ноты.
//   Семантика эффектов (все применяются раз в тик, если не сказано иного):
//     ChipFxArpeggio     полубайты параметра (hi,lo) = смещения; цикл 0,hi,lo
//     ChipFxSlideUp      param/16 полутонов вверх за тик
//     ChipFxSlideDown    param/16 полутонов вниз за тик
//     ChipFxPortamento   скольжение к следующей ноте со скоростью param/16 полутонов за тик
//     ChipFxVibrato      полубайт hi = скорость, lo = глубина (1/16 полутона)
//     ChipFxVolumeSlide  полубайт hi = вверх, lo = вниз (шаги 1/15 за тик)
//     ChipFxDutyCycle    param 0..3 выбирает duty импульса
//     ChipFxJump         param = индекс порядка для перехода (0xFF = loopOrder)
//     ChipFxSpeed        param = новые тики на ряд (1..31)
//     ChipFxNoteCut      снять ноту через param тиков
//     ChipFxRetrigger    перезапуск каждые param тиков (сбрасывает шумовой LFSR)
//     ChipFxEcho         param != 0 включает, 0 выключает блок эха
//     ChipFxDetune       param реинтерпретируется как i8 центов
//
// Текстовый формат песни (Serialize/Deserialize)
//   Построчный, UTF-8/Latin-1, терпимый к пустым строкам и комментариям '#':
//
//     #GE-CHIPTUNE 1
//     title=My Song
//     author=CrossRender
//     chip=SnesSpc
//     channels=8
//     speed=9            (тики на ряд)
//     fps=60
//     rowsPerBeat=4
//     bpm=100
//     loopOrder=0
//     echoEnabled=1
//     echoDelayMs=180
//     echoFeedback=110
//     echoVolume=90
//     lowPass=1
//     lowPassCutoff=7000
//     channel wave=Pulse50 volume=12 ...
//     pattern=Intro,32
//     row A-4,0,255,0,0,255 ... ... ...
//     ...  (одна строка row на каждый ряд паттерна)
//     order=0,1,2
//
//   Ячейка — это `...` (все значения по умолчанию) или шесть полей через запятую:
//   note(0=имя, `---`=нет, `OFF`=снятие ноты), инструмент, громкость, эффект,
//   параметр, arp. Неизвестные ключи игнорируются, чтобы будущие ревизии оставались
//   читаемыми; некорректные значения аккуратно приводят к сбою Deserialize() со строкой ошибки.
// ===========================================================================

#include <vector>

#include "crossrender/audio/Chiptune.h"

#include "crossrender/core/Log.h"
#include "crossrender/core/File.h"

#include <array>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>
#include <cstring>
#include <algorithm>

namespace crossrender {
namespace {

// ---------------------------------------------------------------------------
// Константы
// ---------------------------------------------------------------------------
constexpr f64 kPiD = 3.14159265358979323846;
constexpr f64 kTauD = 6.28318530717958647692;

constexpr f64 kNesCpuNtsc = 1789773.0;
constexpr f64 kNesCpuPal = 1662607.0;
constexpr f64 kGameBoyCpu = 4194304.0;

constexpr int kInternalRate = 32000;      // Гц, частота работы каждого голоса
constexpr int kMaxInternalBlock = 8192;   // размер scratch (внутренних фреймов)
constexpr int kMaxChannels = 32;
constexpr int kMaxPatterns = 256;
constexpr int kMaxRows = 4096;
constexpr int kSampleTableLen = 64;
// Допуск при сравнении накопленной позиции тика с длиной тика, чтобы дельты f32
// в Advance() не теряли и не приобретали тики.
constexpr f64 kTickEpsilon = 1e-3;

constexpr f32 kSoftKnee = 0.75f;

// Таблицы периодов шума NES, в тактах CPU на сдвиг LFSR.
constexpr int kNesNoiseNtsc[16] = {4,   8,   16,  32,  64,  96,  128,  160,
                                   202, 254, 380, 508, 762, 1016, 2034, 4068};
constexpr int kNesNoisePal[16] = {4,   8,   14,  30,  60,  88,  118,  148,
                                  188, 236, 354, 472, 708, 944, 1890, 3778};

// Последовательности duty импульса NES (0 = 12.5%, 1 = 25%, 2 = 50%, 3 = 75%).
constexpr u8 kDutyTable[4][8] = {
    {0, 1, 0, 0, 0, 0, 0, 0},
    {0, 1, 1, 0, 0, 0, 0, 0},
    {0, 1, 1, 1, 1, 0, 0, 0},
    {1, 0, 0, 1, 1, 1, 1, 1},
};

// ---------------------------------------------------------------------------
// Небольшие хелперы
// ---------------------------------------------------------------------------
inline f64 SemitoneToFreq(f64 semitone) {
    return 440.0 * std::pow(2.0, (semitone - 57.0) / 12.0);
}

inline f64 ClampD(f64 v, f64 lo, f64 hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline f32 ClampF(f32 v, f32 lo, f32 hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline int ClampI(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline bool Finite(f32 v) { return std::isfinite(static_cast<double>(v)); }

// Мягкий лимитер, идентичный по духу crossrender::Audio: линеен ниже «колена»,
// асимптотичен к +/-1 выше него, поэтому громкий чип-микс сжимается, а не
// заворачивается, и Bake() может обещать «никогда не beyond +/-1».
inline f32 SoftClip(f32 x) {
    constexpr f32 c = 1.0f - kSoftKnee;
    if (x > kSoftKnee) {
        f32 o = x - kSoftKnee;
        return kSoftKnee + c * (o / (o + c));
    }
    if (x < -kSoftKnee) {
        f32 o = -x - kSoftKnee;
        return -(kSoftKnee + c * (o / (o + c)));
    }
    return x;
}

inline void PanGains(f32 pan, f32* left, f32* right) {
    f32 p = ClampF(pan, -1.0f, 1.0f);
    f32 a = (p + 1.0f) * 0.25f * kPi;
    *left = std::cos(a);
    *right = std::sin(a);
}

// Хелперы знаковых 4-битных полубайтов.
inline int HiNibble(int v) { return (v >> 4) & 0x0F; }
inline int LoNibble(int v) { return v & 0x0F; }

// ---------------------------------------------------------------------------
// Темп
// ---------------------------------------------------------------------------
int SongTicksPerRow(const ChipSong& s) {
    if (s.ticksPerRow > 0) return s.ticksPerRow;
    if (s.beatsPerMinute > 0 && s.rowsPerBeat > 0 && s.framesPerSecond > 0) {
        f64 t = static_cast<f64>(s.framesPerSecond) * 60.0 /
                (static_cast<f64>(s.beatsPerMinute) * static_cast<f64>(s.rowsPerBeat));
        return ClampI(static_cast<int>(std::lround(t)), 1, 64);
    }
    return 6;
}

// ---------------------------------------------------------------------------
// Одноцикловые таблицы волн
// ---------------------------------------------------------------------------
std::vector<f32> BuildWaveform(ChipWave wave, int samples, f32 phase) {
    std::vector<f32> out;
    if (samples <= 0) return out;
    out.resize(static_cast<usize>(samples));
    if (samples == 1) {
        out[0] = 0.0f;
        return out;
    }

    const int maxH = ClampI(samples / 2 - 1, 1, 32);

    if (wave == ChipWave::Noise) {
        // Детерминированный псевдослучайный «цикл»: без высоты тона, но
        // стабильный, с нулевым средним и нормализованным пиком.
        u32 s = 0x9E3779B9u ^ static_cast<u32>(samples) * 2654435761u;
        for (int i = 0; i < samples; ++i) {
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s << 5;
            out[static_cast<usize>(i)] = static_cast<f32>(static_cast<i32>(s)) / 2147483648.0f;
        }
    } else {
        f64 duty = 0.5;
        switch (wave) {
            case ChipWave::Pulse12: duty = 0.125; break;
            case ChipWave::Pulse25: duty = 0.25; break;
            case ChipWave::Pulse75: duty = 0.75; break;
            case ChipWave::Pulse50:
            case ChipWave::Square: duty = 0.5; break;
            default: break;
        }
        const bool isPulse = wave == ChipWave::Pulse12 || wave == ChipWave::Pulse25 ||
                             wave == ChipWave::Pulse50 || wave == ChipWave::Pulse75 ||
                             wave == ChipWave::Square;
        for (int i = 0; i < samples; ++i) {
            const f64 t = static_cast<f64>(i) / static_cast<f64>(samples) +
                          static_cast<f64>(phase);
            f64 v = 0.0;
            if (isPulse) {
                // Ряд Фурье для импульса +/-1 с заданным duty; DC-составляющая
                // отброшена, поэтому таблица изначально свободна от DC.
                for (int nh = 1; nh <= maxH; ++nh) {
                    const f64 n = static_cast<f64>(nh);
                    const f64 a = 2.0 * std::sin(kTauD * n * duty) / (kPiD * n);
                    const f64 b = 2.0 * (1.0 - std::cos(kTauD * n * duty)) / (kPiD * n);
                    v += a * std::cos(kTauD * n * t) + b * std::sin(kTauD * n * t);
                }
            } else if (wave == ChipWave::Triangle) {
                for (int nh = 1; nh <= maxH; nh += 2) {
                    const f64 n = static_cast<f64>(nh);
                    const f64 sign = (((nh - 1) / 2) % 2 == 0) ? 1.0 : -1.0;
                    v += sign * 8.0 * std::sin(kTauD * n * t) / (kPiD * kPiD * n * n);
                }
            } else if (wave == ChipWave::Saw) {
                for (int nh = 1; nh <= maxH; ++nh) {
                    const f64 n = static_cast<f64>(nh);
                    const f64 sign = (nh % 2 == 1) ? 1.0 : -1.0;
                    v += sign * 2.0 * std::sin(kTauD * n * t) / (kPiD * n);
                }
            } else if (wave == ChipWave::Sine) {
                v = std::sin(kTauD * t);
            } else {  // ChipWave::Sample: яркий, богатый гармониками чип-тембр
                for (int nh = 1; nh <= maxH; ++nh) {
                    const f64 n = static_cast<f64>(nh);
                    const f64 amp = 1.0 / std::pow(n, 1.15);
                    const f64 ph = 0.37 * n * n;
                    v += amp * std::sin(kTauD * n * t + ph);
                }
            }
            out[static_cast<usize>(i)] = static_cast<f32>(v);
        }
    }

    // Убираем DC-составляющую и нормализуем пик ровно к 1.
    f64 mean = 0.0;
    for (f32 v : out) mean += v;
    mean /= static_cast<f64>(out.size());
    f64 peak = 0.0;
    for (f32& v : out) {
        v = static_cast<f32>(v - mean);
        peak = std::max(peak, std::fabs(static_cast<f64>(v)));
    }
    if (peak > 1e-9) {
        const f32 inv = static_cast<f32>(1.0 / peak);
        for (f32& v : out) v *= inv;
    } else {
        std::fill(out.begin(), out.end(), 0.0f);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Виды голосов
// ---------------------------------------------------------------------------
enum class VKind : u8 {
    None,
    NesPulse,
    NesTriangle,
    NesNoise,
    Dmc,
    GbWave,
    GbNoise,
    Sample,
    Fm,
    Beep,
};

VKind KindForChip(ChipType chip, ChipWave wave) {
    const bool isPulse = wave == ChipWave::Pulse12 || wave == ChipWave::Pulse25 ||
                         wave == ChipWave::Pulse50 || wave == ChipWave::Pulse75 ||
                         wave == ChipWave::Square;
    switch (chip) {
        case ChipType::SnesSpc:
            return VKind::Sample;
        case ChipType::SegaYM:
            return VKind::Fm;
        case ChipType::PcSpeaker:
            return VKind::Beep;
        case ChipType::GameBoy:
            if (isPulse) return VKind::NesPulse;
            if (wave == ChipWave::Noise) return VKind::GbNoise;
            return VKind::GbWave;
        case ChipType::Nes2A03:
        default:
            if (isPulse) return VKind::NesPulse;
            if (wave == ChipWave::Triangle) return VKind::NesTriangle;
            if (wave == ChipWave::Noise) return VKind::NesNoise;
            if (wave == ChipWave::Sample) return VKind::Dmc;
            return VKind::Sample;  // Saw / Sine откатываются к волнотабличному голосу
    }
}

int DutyForWave(ChipWave wave) {
    switch (wave) {
        case ChipWave::Pulse12: return 0;
        case ChipWave::Pulse25: return 1;
        case ChipWave::Pulse75: return 3;
        case ChipWave::Pulse50:
        case ChipWave::Square:
        default: return 2;
    }
}

// ---------------------------------------------------------------------------
// Voice
// ---------------------------------------------------------------------------
struct Voice {
    VKind kind = VKind::None;
    bool active = false;
    bool muted = false;
    bool quantVol = true;  // 4-битные шаги громкости в стиле NES
    int channel = 0;

    // pitch
    int note = -1;
    f64 baseFreq = 0.0;
    f64 freq = 0.0;
    f64 pitchBend = 0.0;   // полутоны, от эффектов скольжения
    f64 portaTarget = 0.0; // Гц, цель портаменто
    int portaSpeed = 0;    // 1/16 полутона за тик
    f64 vibPhase = 0.0;
    int vibSpeed = 0;
    int vibDepth = 0;
    int detuneCents = 0;
    int arpFixed = 0xFF;   // ChipNote::arp
    int arpOffsets[3] = {0, 0, 0};
    int arpIndex = 0;
    int arpMode = 0;       // 0 off, 1 channel triad, 2 custom
    int arpTick = 0;
    int slidePerTick = 0;  // 1/16 полутона

    // громкость / огибающая
    f64 chanVol = 1.0;
    f64 noteVol = 1.0;
    f64 gainNow = 1.0;     // chanVol * noteVol
    f64 envLevel = 0.0;
    int envStage = 0;      // 0 attack, 1 decay, 2 sustain, 3 release
    f64 attackSec = 0.0, decaySec = 0.0, releaseSec = 0.0;
    f64 sustainLevel = 0.8;
    f64 releaseRate = 1e9;

    // эффекты отдельной ноты
    int cutTick = -1;
    int retriggerPeriod = 0;
    int retriggerCount = 0;
    bool echoOn = false;
    int lastEffect = 0;

    // импульс
    int duty = 2;
    int dutyStep = 0;
    f64 stepAcc = 0.0;
    f64 stepRate = 0.0;
    int timerPeriod = 0;
    f64 dutyLfoPhase = 0.0;
    f64 dutyLfoStep = 0.0;
    bool mutedPulse = false;

    // треугольник / волна Game Boy
    int triStep = 0;
    int wavePos = 0;
    f64 waveAcc = 0.0;
    int gbShift = 0;
    const std::vector<f32>* table = nullptr;

    // шум
    u32 lfsr = 0x4001;
    f64 noiseAcc = 0.0;
    f64 noiseRate = 0.0;

    // dmc / бипер
    u64 bits = 0;
    int bitPos = 0;
    f64 bitAcc = 0.0;
    f64 bitRate = 0.0;
    int level = 64;

    // семпл / fm
    f64 samplePos = 0.0;
    f64 sampleInc = 0.0;
    f64 modPhase = 0.0;
    f64 modInc = 0.0;
    f64 modIndex = 0.0;

    f32 panL = 0.70710678f, panR = 0.70710678f;
    f32 sendGain = 0.0f;  // посыл эха SPC (из ChipChannelDef::echoVolume)

    // передаётся в UI
    f32 reportedPhase = 0.0f;
};

// ---------------------------------------------------------------------------
// ChipPlayer::Impl
// ---------------------------------------------------------------------------
}  // namespace

struct ChipPlayer::Impl {
    // ---- песня -------------------------------------------------------------
    std::shared_ptr<const ChipSong> song;
    std::array<std::vector<f32>, static_cast<usize>(ChipWave::Count)> tables;
    std::vector<f32> gbWave;

    // ---- секвенсор --------------------------------------------------------
    int orderIndex = 0;
    int row = 0;
    int tick = 0;
    int speedOverride = -1;
    bool playing = false;
    bool ended = false;
    bool started = false;
    bool loop = true;
    bool pendingJump = false;
    int jumpOrder = -1;
    int jumpRow = -1;
    int pendingSpeed = 0;
    f64 frameInTick = 0.0;
    f64 totalFrames = 0.0;
    u64 renderedFrames = 0;
    f32 masterVolume = 1.0f;
    f32 lastPeak = 0.0f;
    int tailSilentFrames = 0;
    ChipPlayer::RowCallback rowCb = nullptr;
    void* rowUser = nullptr;

    // ---- каналы ---------------------------------------------------------
    std::vector<Voice> voices;
    std::vector<ChipPlayer::ChannelState> states;
    std::vector<u8> muted;

    // ---- аудио ------------------------------------------------------------
    int sampleRate = 44100;
    f64 rsStep = 0.0;
    f64 rsPhase = 1.0;
    f32 rsHistL[2] = {0.0f, 0.0f};
    f32 rsHistR[2] = {0.0f, 0.0f};
    f32 dcPrevIn[2] = {0.0f, 0.0f};
    f32 dcPrevOut[2] = {0.0f, 0.0f};
    f64 dcR = 0.997;
    // ФНЧ RBJ 2-го порядка
    f32 lpb0 = 1.0f, lpb1 = 0.0f, lpb2 = 0.0f, lpa1 = 0.0f, lpa2 = 0.0f;
    f32 lpZ1[2] = {0.0f, 0.0f};
    f32 lpZ2[2] = {0.0f, 0.0f};
    // эхо
    std::vector<f32> echoL, echoR;
    int echoWrite = 0;
    bool echoNoteOn = false;
    bool echoAllocated = false;

    std::vector<f32> mixL, mixR;
    // Посыл эха на голос в стиле SPC. Используется, только когда хотя бы один
    // канал просит эхо (echoVolume > 0); иначе блок эха обрабатывает микс.
    std::vector<f32> sendL, sendR;
    bool useSends = false;

    // -----------------------------------------------------------------------
    Impl() {
        mixL.resize(kMaxInternalBlock);
        mixR.resize(kMaxInternalBlock);
        sendL.resize(kMaxInternalBlock);
        sendR.resize(kMaxInternalBlock);
        UpdateResampler();
    }

    // ---- настройка ------------------------------------------------------------
    void NormalizeSong(ChipSong* s) {
        if (s->channelCount <= 0)
            s->channelCount = static_cast<int>(s->channels.size());
        s->channelCount = ClampI(s->channelCount, 1, kMaxChannels);
        if (static_cast<int>(s->channels.size()) < s->channelCount)
            s->channels.resize(static_cast<usize>(s->channelCount));
        s->framesPerSecond = ClampI(s->framesPerSecond, 1, 1000);
        if (s->ticksPerRow < 1) s->ticksPerRow = SongTicksPerRow(*s);
        if (s->rowsPerBeat < 1) s->rowsPerBeat = 4;
        if (s->beatsPerMinute < 0) s->beatsPerMinute = 0;
        if (s->order.empty()) s->order.push_back(0);
        for (int& o : s->order) {
            if (s->patterns.empty()) o = 0;
            else o = ClampI(o, 0, static_cast<int>(s->patterns.size()) - 1);
        }
        if (s->loopOrder >= static_cast<int>(s->order.size())) s->loopOrder = -1;
        if (s->loopOrder < -1) s->loopOrder = -1;
    }

    void BuildTables() {
        for (int w = 0; w < static_cast<int>(ChipWave::Count); ++w) {
            tables[static_cast<usize>(w)] =
                BuildWaveform(static_cast<ChipWave>(w), kSampleTableLen, 0.0f);
        }
        // Волновой канал Game Boy: один цикл из 32 семплов, квантованных в 4 бита.
        std::vector<f32> raw = MakeSingleCycleWaveform(ChipWave::Saw, 32, 0.0f);
        gbWave.assign(32, 0.0f);
        for (usize i = 0; i < raw.size() && i < gbWave.size(); ++i) {
            int q = ClampI(static_cast<int>(std::lround((raw[i] * 0.5f + 0.5f) * 15.0f)), 0, 15);
            gbWave[i] = (static_cast<f32>(q) / 15.0f) * 2.0f - 1.0f;
        }
    }

    void UpdateResampler() {
        rsStep = static_cast<f64>(kInternalRate) / static_cast<f64>(sampleRate);
        dcR = std::exp(-kTauD * 20.0 / static_cast<f64>(sampleRate));
        rsPhase = 1.0;
        rsHistL[0] = rsHistL[1] = 0.0f;
        rsHistR[0] = rsHistR[1] = 0.0f;
        dcPrevIn[0] = dcPrevIn[1] = 0.0f;
        dcPrevOut[0] = dcPrevOut[1] = 0.0f;
        lpZ1[0] = lpZ1[1] = lpZ2[0] = lpZ2[1] = 0.0f;
        UpdateLowPass();
    }

    void UpdateLowPass() {
        lpb0 = 1.0f;
        lpb1 = lpb2 = lpa1 = lpa2 = 0.0f;
        if (!song || !song->lowPass) return;
        const f64 nyq = static_cast<f64>(kInternalRate) * 0.5;
        const f64 fc = ClampD(static_cast<f64>(song->lowPassCutoff), 100.0, nyq * 0.9);
        const f64 w0 = kTauD * fc / static_cast<f64>(kInternalRate);
        const f64 cosw = std::cos(w0);
        const f64 alpha = std::sin(w0) / (2.0 * 0.7071067811865476);
        const f64 a0 = 1.0 + alpha;
        lpb0 = static_cast<f32>(((1.0 - cosw) * 0.5) / a0);
        lpb1 = static_cast<f32>((1.0 - cosw) / a0);
        lpb2 = lpb0;
        lpa1 = static_cast<f32>((-2.0 * cosw) / a0);
        lpa2 = static_cast<f32>((1.0 - alpha) / a0);
    }

    void AllocEcho() {
        const int ms = song ? ClampI(song->echoDelayMs, 1, 1000) : 120;
        const int samples = ClampI(ms * kInternalRate / 1000, 16, kInternalRate);
        echoL.assign(static_cast<usize>(samples), 0.0f);
        echoR.assign(static_cast<usize>(samples), 0.0f);
        echoWrite = 0;
        echoAllocated = true;
    }

    void ConfigureVoice(int ch) {
        const ChipSong& s = *song;
        const ChipChannelDef& def = s.channels[static_cast<usize>(ch)];
        Voice& v = voices[static_cast<usize>(ch)];
        const VKind kind = KindForChip(s.chip, def.wave);
        v = Voice{};
        v.kind = kind;
        v.channel = ch;
        v.muted = muted[static_cast<usize>(ch)] != 0;
        v.quantVol = (s.chip == ChipType::Nes2A03 || s.chip == ChipType::GameBoy ||
                      s.chip == ChipType::PcSpeaker);
        v.chanVol = ClampD(static_cast<f64>(def.volume) / 15.0, 0.0, 1.0);
        v.attackSec = ClampI(def.attack, 0, 15) * 0.006;
        v.decaySec = ClampI(def.decay, 0, 15) * 0.006;
        v.releaseSec = ClampI(def.release, 0, 15) * 0.006;
        v.sustainLevel = ClampI(def.sustain, 0, 15) / 15.0;
        v.detuneCents = ClampI(def.detune, -1200, 1200);
        v.duty = DutyForWave(def.wave);
        v.dutyLfoStep = static_cast<f64>(def.dutySweep) / static_cast<f64>(kInternalRate);
        v.vibSpeed = 0;
        v.modIndex = 3.0;
        PanGains(static_cast<f32>(ClampI(def.pan, -8, 8)) / 8.0f, &v.panL, &v.panR);
        v.sendGain = static_cast<f32>(ClampI(def.echoVolume, 0, 15)) / 15.0f;
        if (def.arpeggio) {
            v.arpMode = 1;
            v.arpOffsets[0] = 0;
            v.arpOffsets[1] = 3;
            v.arpOffsets[2] = 7;
        }
        const int w = static_cast<int>(def.wave);
        if (w >= 0 && w < static_cast<int>(ChipWave::Count))
            v.table = &tables[static_cast<usize>(w)];
        if (kind == VKind::GbWave) v.table = &gbWave;
        v.lfsr = (kind == VKind::GbNoise) ? 0x7F : 0x4001;
        v.active = false;
    }

    void ResetPlayback() {
        orderIndex = 0;
        row = 0;
        tick = 0;
        frameInTick = 0.0;
        totalFrames = 0.0;
        renderedFrames = 0;
        lastPeak = 0.0f;
        ended = false;
        started = false;
        tailSilentFrames = 0;
        pendingJump = false;
        pendingSpeed = 0;
        if (song) {
            for (int i = 0; i < song->channelCount; ++i) ConfigureVoice(i);
        }
        std::fill(echoL.begin(), echoL.end(), 0.0f);
        std::fill(echoR.begin(), echoR.end(), 0.0f);
        echoWrite = 0;
        echoNoteOn = false;
        memset(dcPrevIn, 0, sizeof(dcPrevIn));
        memset(dcPrevOut, 0, sizeof(dcPrevOut));
        memset(lpZ1, 0, sizeof(lpZ1));
        memset(lpZ2, 0, sizeof(lpZ2));
        rsPhase = 1.0;
        rsHistL[0] = rsHistL[1] = 0.0f;
        rsHistR[0] = rsHistR[1] = 0.0f;
        SyncStates();
    }

    void SetSong(const ChipSong& in) {
        auto copy = std::make_shared<ChipSong>(in);
        NormalizeSong(copy.get());
        song = copy;
        BuildTables();
        voices.assign(static_cast<usize>(song->channelCount), Voice{});
        states.assign(static_cast<usize>(song->channelCount), ChipPlayer::ChannelState{});
        muted.assign(static_cast<usize>(song->channelCount), 0);
        for (int i = 0; i < song->channelCount; ++i) {
            muted[static_cast<usize>(i)] = 0;
            states[static_cast<usize>(i)].muted = false;
        }
        AllocEcho();
        useSends = false;
        for (const ChipChannelDef& c : song->channels)
            if (c.echoVolume > 0) useSends = true;
        UpdateLowPass();
        loop = song->loopOrder >= 0;
        speedOverride = -1;
        ResetPlayback();
    }

    int TicksPerRow() const {
        if (speedOverride > 0) return speedOverride;
        return SongTicksPerRow(*song);
    }

    // ---- секвенсор --------------------------------------------------------
    const ChipPattern* CurrentPattern() const {
        if (!song || song->order.empty()) return nullptr;
        if (orderIndex < 0 || orderIndex >= static_cast<int>(song->order.size())) return nullptr;
        const int pi = song->order[static_cast<usize>(orderIndex)];
        if (pi < 0 || pi >= static_cast<int>(song->patterns.size())) return nullptr;
        return &song->patterns[static_cast<usize>(pi)];
    }

    // ChipNote::volume — аппаратная 4-битная громкость (0..15); 0xFF означает
    // «использовать значение по умолчанию инструмента/канала».
    static f64 NoteVolume(u8 v) {
        if (v == 0xFF) return 1.0;
        return ClampI(static_cast<int>(v), 0, 15) / 15.0;
    }

    void TriggerNote(int ch, const ChipNote& n) {
        Voice& v = voices[static_cast<usize>(ch)];
        const ChipChannelDef& def = song->channels[static_cast<usize>(ch)];
        const int semi = n.semitone;
        if (semi < 0) {
            ReleaseVoice(v);
            return;
        }
        v.note = semi;
        v.baseFreq = SemitoneToFreq(static_cast<f64>(semi));
        if (v.baseFreq < 1.0) v.baseFreq = 1.0;
        v.pitchBend = 0.0;
        v.slidePerTick = 0;
        v.portaSpeed = 0;
        v.vibPhase = 0.0;
        v.vibSpeed = 0;
        v.vibDepth = 0;
        v.cutTick = -1;
        v.retriggerPeriod = 0;
        v.retriggerCount = 0;
        v.arpFixed = n.arp;
        v.arpIndex = 0;
        v.arpTick = 0;
        v.lastEffect = n.effect;
        if (def.arpeggio) {
            v.arpMode = 1;
            v.arpOffsets[0] = 0;
            v.arpOffsets[1] = 3;
            v.arpOffsets[2] = 7;
        } else {
            v.arpMode = 0;
        }
        v.noteVol = NoteVolume(n.volume);
        v.gainNow = v.chanVol * v.noteVol;
        v.envStage = 0;
        v.envLevel = (v.attackSec <= 0.0) ? 1.0 : 0.0;
        if (v.attackSec <= 0.0) v.envStage = 1;
        v.releaseRate = 1e9;
        v.duty = DutyForWave(def.wave);
        v.dutyLfoPhase = 0.0;
        // Осцилляторы NES/Game Boy продолжают работать при смене ноты:
        // перезапускается только огибающая. Это и аппаратно достоверно, и без
        // щелчков. ChipFxRetrigger сбрасывает их фазу явно.
        v.bitAcc = 0.0;
        v.bitPos = 0;
        v.level = 64;
        v.samplePos = 0.0;
        v.modPhase = 0.0;
        v.active = true;
        UpdateVoiceFrequency(v, 0.0);
        BuildBitstream(v);
    }

    void ReleaseVoice(Voice& v) {
        if (!v.active) return;
        if (v.envStage != 3) {
            v.envStage = 3;
            if (v.releaseSec <= 0.0) {
                v.envLevel = 0.0;
                v.active = false;
            } else {
                v.releaseRate = v.envLevel / v.releaseSec;
                if (v.releaseRate < 1e-9) v.releaseRate = 1e-9;
            }
        }
    }

    // Строит 1-битный поток, используемый голосами DMC и PC-динамика: два
    // цикла волны канала по 32 бита на цикл. Битовый поток DMC — дельта-кодирование
    // целевого значения, бипер использует сырой знаковый бит.
    void BuildBitstream(Voice& v) {
        if (v.kind != VKind::Dmc && v.kind != VKind::Beep) return;
        const std::vector<f32>* tbl = v.table;
        u64 bits = 0;
        int level = 64;
        for (int i = 0; i < 64; ++i) {
            f32 target = 0.0f;
            if (tbl && !tbl->empty()) {
                usize idx = static_cast<usize>(i * 2) % tbl->size();
                target = (*tbl)[idx];
            }
            if (v.kind == VKind::Beep) {
                if (target > 0.0f) bits |= (1ull << i);
            } else {
                const int want = ClampI(static_cast<int>(std::lround(target * 60.0f)) + 64, 2, 126);
                if (want > level) {
                    bits |= (1ull << i);
                    level += 2;
                } else {
                    level -= 2;
                }
                level = ClampI(level, 0, 127);
            }
        }
        v.bits = bits;
        v.bitPos = 0;
        v.bitRate = std::max(1.0, v.freq * 32.0);
    }

    void UpdateVoiceFrequency(Voice& v, f64 lfoCents) {
        f64 off = v.pitchBend + lfoCents + static_cast<f64>(v.detuneCents) / 100.0;
        if (v.arpMode != 0 && v.arpIndex >= 0 && v.arpIndex < 3)
            off += v.arpOffsets[v.arpIndex];
        if (v.arpFixed != 0xFF) off += v.arpFixed;
        f64 f = v.baseFreq * std::pow(2.0, off / 12.0);
        if (!(f > 0.0) || !std::isfinite(f)) f = v.baseFreq;
        v.freq = ClampD(f, 1.0, 40000.0);
    }

    // Применение эффектов раз в тик.
    void ApplyEffects(int ch, const ChipNote& n) {
        Voice& v = voices[static_cast<usize>(ch)];
        if (!v.active) return;
        const ChipChannelDef& def = song->channels[static_cast<usize>(ch)];

        // арпеджио: цикл тоника / +терция / +квинта (или полубайты ChipFxArpeggio)
        if (v.arpMode != 0) v.arpIndex = (v.arpIndex + 1) % 3;

        // LFO вибрато
        f64 vibCents = 0.0;
        if (v.vibSpeed > 0) {
            v.vibPhase += static_cast<f64>(v.vibSpeed) / 16.0;
            vibCents = std::sin(kTauD * v.vibPhase) * static_cast<f64>(v.vibDepth) / 16.0 * 100.0;
        }

        // модуляция высоты у семпл-голосов (pitchSweep = глубина в центах)
        if (v.kind == VKind::Sample || v.kind == VKind::Fm) {
            const int depth = ClampI(def.pitchSweep, -127, 127);
            if (depth != 0) {
                const f64 rate = (def.dutySweep > 0 ? def.dutySweep : 5) / 16.0;
                v.modPhase += rate;
                vibCents += std::sin(kTauD * v.modPhase) * static_cast<f64>(depth);
            }
        }

        if (v.slidePerTick != 0) {
            v.pitchBend += static_cast<f64>(v.slidePerTick) / 16.0;
            v.pitchBend = ClampD(v.pitchBend, -48.0, 48.0);
        }
        if (v.portaSpeed > 0) {
            const f64 target = v.portaTarget;
            const f64 cur = v.baseFreq;
            const f64 step = static_cast<f64>(v.portaSpeed) / 16.0;
            if (target > cur) {
                v.baseFreq = std::min(target, cur * std::pow(2.0, step / 12.0));
            } else if (target < cur) {
                v.baseFreq = std::max(target, cur * std::pow(2.0, -step / 12.0));
            }
        }

        // перезапуск
        if (v.retriggerPeriod > 0) {
            v.retriggerCount = (v.retriggerCount + 1) % v.retriggerPeriod;
            if (v.retriggerCount == 0) {
                v.envStage = 0;
                v.envLevel = (v.attackSec <= 0.0) ? 1.0 : 0.0;
                if (v.attackSec <= 0.0) v.envStage = 1;
                v.dutyStep = 0;
                v.triStep = 0;
                v.wavePos = 0;
                v.samplePos = 0.0;
                v.lfsr = (v.kind == VKind::GbNoise) ? 0x7F : 0x4001;
                v.bitPos = 0;
                v.level = 64;
            }
        }

        // обратный отсчёт note cut
        if (v.cutTick > 0) {
            --v.cutTick;
            if (v.cutTick == 0) ReleaseVoice(v);
        }

        (void)n;
        UpdateVoiceFrequency(v, vibCents);
    }

    void StartRow() {
        const ChipPattern* p = CurrentPattern();
        tick = 0;
        if (!p) {
            ended = true;
            return;
        }
        for (int ch = p->channelCount; ch < song->channelCount; ++ch)
            ReleaseVoice(voices[static_cast<usize>(ch)]);
        for (int ch = 0; ch < song->channelCount && ch < p->channelCount; ++ch) {
            const ChipNote& n = p->At(row, ch);
            if (n.semitone == -2) {
                ReleaseVoice(voices[static_cast<usize>(ch)]);
                continue;
            }
            if (n.semitone >= 0) {
                if (n.effect == ChipFxPortamento) {
                    Voice& v = voices[static_cast<usize>(ch)];
                    v.portaTarget = SemitoneToFreq(static_cast<f64>(n.semitone));
                    v.portaSpeed = LoNibble(n.param) > 0 ? LoNibble(n.param) : n.param;
                    if (v.portaSpeed == 0) v.portaSpeed = 8;
                    v.note = n.semitone;
                    v.noteVol = NoteVolume(n.volume);
                    v.gainNow = v.chanVol * v.noteVol;
                    if (!v.active) {
                        v.active = true;
                        v.envStage = 0;
                        v.envLevel = (v.attackSec <= 0.0) ? 1.0 : 0.0;
                        if (v.attackSec <= 0.0) v.envStage = 1;
                    }
                    v.lastEffect = n.effect;
                } else {
                    TriggerNote(ch, n);
                }
            } else if (n.volume != 0xFF) {
                // Ячейка только с громкостью.
                Voice& v = voices[static_cast<usize>(ch)];
                v.noteVol = NoteVolume(n.volume);
                v.gainNow = v.chanVol * v.noteVol;
            }
            ApplyRowEffects(ch, n);
        }
        if (rowCb) rowCb(orderIndex, row, rowUser);
    }

    void ApplyRowEffects(int ch, const ChipNote& n) {
        Voice& v = voices[static_cast<usize>(ch)];
        v.lastEffect = n.effect;
        // NES sweep: знаковый сдвиг высоты, применяемый раз в ряд на 8-битных
        // генераторах (у семпл-голосов pitchSweep — глубина LFO модуляции высоты,
        // см. ApplyEffects).
        if (v.kind != VKind::Sample && v.kind != VKind::Fm) {
            const int sweep = ClampI(song->channels[static_cast<usize>(ch)].pitchSweep, -127, 127);
            if (sweep != 0) {
                v.pitchBend = ClampD(v.pitchBend + static_cast<f64>(sweep) / 16.0, -48.0, 48.0);
                UpdateVoiceFrequency(v, 0.0);
            }
        }
        switch (n.effect) {
            case ChipFxArpeggio:
                v.arpMode = 2;
                v.arpOffsets[0] = 0;
                v.arpOffsets[1] = HiNibble(n.param);
                v.arpOffsets[2] = LoNibble(n.param);
                v.arpIndex = 0;
                break;
            case ChipFxSlideUp:
                v.slidePerTick = n.param;
                break;
            case ChipFxSlideDown:
                v.slidePerTick = -n.param;
                break;
            case ChipFxPortamento:
                if (v.portaSpeed == 0 && n.param > 0) v.portaSpeed = n.param;
                break;
            case ChipFxVibrato:
                v.vibSpeed = HiNibble(n.param);
                v.vibDepth = LoNibble(n.param);
                break;
            case ChipFxVolumeSlide:
                v.noteVol = ClampD(v.noteVol + (HiNibble(n.param) - LoNibble(n.param)) / 15.0,
                                   0.0, 1.0);
                v.gainNow = v.chanVol * v.noteVol;
                break;
            case ChipFxDutyCycle:
                v.duty = ClampI(n.param, 0, 3);
                break;
            case ChipFxSpeed:
                if (n.param > 0) pendingSpeed = ClampI(n.param, 1, 64);
                break;
            case ChipFxNoteCut:
                v.cutTick = n.param;
                break;
            case ChipFxRetrigger:
                v.retriggerPeriod = n.param;
                v.retriggerCount = 0;
                break;
            case ChipFxEcho:
                echoNoteOn = (n.param != 0);
                v.echoOn = echoNoteOn;
                break;
            case ChipFxDetune: {
                i8 d = static_cast<i8>(n.param);
                v.detuneCents = ClampI(static_cast<int>(d), -1200, 1200);
                break;
            }
            case ChipFxJump: {
                const int target = (n.param == 0xFF && song->loopOrder >= 0)
                                       ? song->loopOrder
                                       : static_cast<int>(n.param);
                pendingJump = true;
                jumpOrder = target;
                jumpRow = -1;
                break;
            }
            default:
                break;
        }
    }

    void AdvanceTick() {
        if (!song || ended) return;
        if (pendingSpeed > 0) {
            speedOverride = pendingSpeed;
            pendingSpeed = 0;
        }
        const int tpr = TicksPerRow();
        const ChipPattern* p = CurrentPattern();
        if (!p) {
            ended = true;
            return;
        }
        ++tick;
        if (tick < tpr) {
            for (int ch = 0; ch < song->channelCount && ch < p->channelCount; ++ch) {
                ApplyEffects(ch, p->At(row, ch));
            }
            return;
        }
        tick = 0;
        if (pendingJump) {
            pendingJump = false;
            const int target = ClampI(jumpOrder, 0, static_cast<int>(song->order.size()) - 1);
            orderIndex = target;
            row = (jumpRow >= 0) ? jumpRow : 0;
            StartRow();
            return;
        }
        ++row;
        const ChipPattern* cur = CurrentPattern();
        if (cur && row >= cur->rowCount) {
            row = 0;
            ++orderIndex;
            if (orderIndex >= static_cast<int>(song->order.size())) {
                const int loopTarget = (loop && song->loopOrder >= 0) ? song->loopOrder : -1;
                if (loopTarget >= 0) {
                    orderIndex = loopTarget;
                } else {
                    orderIndex = static_cast<int>(song->order.size()) - 1;
                    row = cur->rowCount;
                    ended = true;
                    for (Voice& v : voices) ReleaseVoice(v);
                    // Сообщаем UI о конце списка порядка.
                    SyncStates();
                    return;
                }
            }
        }
        StartRow();
    }

    // ---- синтез --------------------------------------------------------
    f64 AdvanceEnvelope(Voice& v, f64 dt) {
        switch (v.envStage) {
            case 0:
                if (v.attackSec <= 0.0) {
                    v.envLevel = 1.0;
                    v.envStage = 1;
                } else {
                    v.envLevel += dt / v.attackSec;
                    if (v.envLevel >= 1.0) {
                        v.envLevel = 1.0;
                        v.envStage = 1;
                    }
                }
                break;
            case 1:
                if (v.decaySec <= 0.0) {
                    v.envLevel = v.sustainLevel;
                    v.envStage = 2;
                } else {
                    v.envLevel -= dt * (1.0 - v.sustainLevel) / v.decaySec;
                    if (v.envLevel <= v.sustainLevel) {
                        v.envLevel = v.sustainLevel;
                        v.envStage = 2;
                    }
                }
                break;
            case 2:
                v.envLevel = v.sustainLevel;
                break;
            case 3:
            default:
                v.envLevel -= dt * v.releaseRate;
                if (v.envLevel <= 0.0) {
                    v.envLevel = 0.0;
                    v.active = false;
                }
                break;
        }
        return v.envLevel;
    }

    f32 VoiceGain(const Voice& v, f64 env) const {
        f64 g = v.gainNow * env;
        if (g < 0.0) g = 0.0;
        if (v.quantVol) {
            // Шаг 4-битной громкости NES.
            g = std::floor(g * 15.0 + 0.5) / 15.0;
        }
        return static_cast<f32>(g);
    }

    void PrepareVoiceBlock(Voice& v) {
        const ChipChannelDef& def = song->channels[static_cast<usize>(v.channel)];
        const ChipType chip = song->chip;
        const f64 cpu = chip == ChipType::GameBoy ? kGameBoyCpu
                                                  : (song->framesPerSecond <= 50 ? kNesCpuPal
                                                                                 : kNesCpuNtsc);
        v.stepRate = 0.0;
        v.noiseRate = 0.0;
        v.sampleInc = 0.0;
        v.modInc = 0.0;
        switch (v.kind) {
            case VKind::NesPulse: {
                const f64 t = std::round(cpu / (16.0 * v.freq)) - 1.0;
                v.timerPeriod = ClampI(static_cast<int>(t), 0, 2047);
                v.stepRate = cpu / (2.0 * (v.timerPeriod + 1.0));
                break;
            }
            case VKind::NesTriangle: {
                const f64 t = std::round(cpu / (32.0 * v.freq)) - 1.0;
                v.timerPeriod = ClampI(static_cast<int>(t), 0, 2047);
                // Особенность NES: период меньше 2 заглушает канал.
                v.mutedPulse = v.timerPeriod < 2;
                v.stepRate = cpu / (v.timerPeriod + 1.0);
                break;
            }
            case VKind::GbWave: {
                const f64 t = std::round(cpu / (64.0 * v.freq)) - 1.0;
                v.timerPeriod = ClampI(static_cast<int>(t), 0, 2047);
                v.stepRate = cpu / (2.0 * (v.timerPeriod + 1.0));
                const int vol = ClampI(def.volume, 0, 15);
                v.gbShift = vol >= 12 ? 0 : (vol >= 8 ? 1 : (vol >= 4 ? 2 : 3));
                break;
            }
            case VKind::NesNoise:
            case VKind::GbNoise: {
                const int idx = ClampI(v.note, 0, 15);
                if (v.kind == VKind::GbNoise) {
                    static const int base[8] = {8, 16, 32, 48, 64, 80, 96, 112};
                    const int shift = idx >> 4;
                    const f64 cycles = static_cast<f64>(base[idx & 7] << shift);
                    v.noiseRate = kGameBoyCpu / cycles;
                } else {
                    const bool pal = song->framesPerSecond <= 50;
                    const int cycles = pal ? kNesNoisePal[idx] : kNesNoiseNtsc[idx];
                    v.noiseRate = cpu / static_cast<f64>(cycles);
                }
                break;
            }
            case VKind::Dmc:
            case VKind::Beep:
                v.bitRate = std::max(1.0, v.freq * 32.0);
                break;
            case VKind::Sample:
            case VKind::Fm: {
                const usize n = v.table ? v.table->size() : 1;
                v.sampleInc = v.freq * static_cast<f64>(n) / static_cast<f64>(kInternalRate);
                v.modInc = v.freq * 2.0 / static_cast<f64>(kInternalRate);
                v.modIndex = 3.0;
                break;
            }
            default:
                break;
        }
        if (v.stepRate > kInternalRate * 64.0) v.stepRate = kInternalRate * 64.0;
        if (v.noiseRate > kInternalRate * 64.0) v.noiseRate = kInternalRate * 64.0;
    }

    f32 RenderVoiceSample(Voice& v, f64 dt) {
        switch (v.kind) {
            case VKind::NesPulse: {
                if (v.dutyLfoStep != 0.0) {
                    v.dutyLfoPhase += v.dutyLfoStep;
                    if (v.dutyLfoPhase >= 1.0) {
                        v.dutyLfoPhase -= 1.0;
                        v.duty = (v.duty + 1) & 3;
                    }
                }
                v.stepAcc += v.stepRate * dt;
                while (v.stepAcc >= 1.0) {
                    v.stepAcc -= 1.0;
                    v.dutyStep = (v.dutyStep + 1) & 7;
                }
                v.reportedPhase = static_cast<f32>(v.dutyStep) / 8.0f;
                return kDutyTable[ClampI(v.duty, 0, 3)][v.dutyStep] ? 1.0f : -1.0f;
            }
            case VKind::NesTriangle: {
                if (v.mutedPulse) {
                    v.reportedPhase = 0.0f;
                    return 0.0f;
                }
                v.stepAcc += v.stepRate * dt;
                while (v.stepAcc >= 1.0) {
                    v.stepAcc -= 1.0;
                    v.triStep = (v.triStep + 1) & 31;
                }
                const int raw = v.triStep < 16 ? 15 - v.triStep : v.triStep - 16;
                v.reportedPhase = static_cast<f32>(v.triStep) / 32.0f;
                return (static_cast<f32>(raw) / 15.0f) * 2.0f - 1.0f;
            }
            case VKind::NesNoise:
            case VKind::GbNoise: {
                v.noiseAcc += v.noiseRate * dt;
                while (v.noiseAcc >= 1.0) {
                    v.noiseAcc -= 1.0;
                    if (v.kind == VKind::GbNoise) {
                        const u32 fb = (v.lfsr ^ (v.lfsr >> 1)) & 1u;
                        v.lfsr = (v.lfsr >> 1) | (fb << 6);
                    } else {
                        const u32 fb = (v.lfsr ^ (v.lfsr >> 1)) & 1u;
                        v.lfsr = (v.lfsr >> 1) | (fb << 14);
                    }
                }
                v.reportedPhase = static_cast<f32>(v.lfsr & 0xFFFFu) / 65536.0f;
                return (v.lfsr & 1u) ? 1.0f : -1.0f;
            }
            case VKind::Dmc: {
                v.bitAcc += v.bitRate * dt;
                while (v.bitAcc >= 1.0) {
                    v.bitAcc -= 1.0;
                    const int b = static_cast<int>((v.bits >> v.bitPos) & 1ull);
                    v.bitPos = (v.bitPos + 1) & 63;
                    v.level = ClampI(v.level + (b ? 2 : -2), 0, 127);
                }
                v.reportedPhase = static_cast<f32>(v.bitPos) / 64.0f;
                return (static_cast<f32>(v.level) - 64.0f) / 64.0f;
            }
            case VKind::Beep: {
                v.bitAcc += v.bitRate * dt;
                while (v.bitAcc >= 1.0) {
                    v.bitAcc -= 1.0;
                    v.bitPos = (v.bitPos + 1) & 63;
                }
                v.reportedPhase = static_cast<f32>(v.bitPos) / 64.0f;
                return ((v.bits >> v.bitPos) & 1ull) ? 1.0f : -1.0f;
            }
            case VKind::GbWave: {
                v.waveAcc += v.stepRate * dt;
                while (v.waveAcc >= 1.0) {
                    v.waveAcc -= 1.0;
                    v.wavePos = (v.wavePos + 1) & 31;
                }
                const f32 s = gbWave.empty() ? 0.0f
                                             : gbWave[static_cast<usize>(v.wavePos & 31)];
                v.reportedPhase = static_cast<f32>(v.wavePos) / 32.0f;
                const f32 shiftGain = (4 - v.gbShift) / 4.0f;
                return s * shiftGain;
            }
            case VKind::Sample:
            case VKind::Fm: {
                if (!v.table || v.table->empty()) return 0.0f;
                const usize n = v.table->size();
                v.samplePos += v.sampleInc;
                if (!(v.samplePos >= 0.0) || !std::isfinite(v.samplePos)) v.samplePos = 0.0;
                if (v.samplePos >= static_cast<f64>(n))
                    v.samplePos = std::fmod(v.samplePos, static_cast<f64>(n));
                const usize i0 = static_cast<usize>(v.samplePos);
                const usize i1 = (i0 + 1) % n;
                const f32 frac = static_cast<f32>(v.samplePos - static_cast<f64>(i0));
                f32 s = (*v.table)[i0] + ((*v.table)[i1] - (*v.table)[i0]) * frac;
                v.reportedPhase = static_cast<f32>(v.samplePos / static_cast<f64>(n));
                if (v.kind == VKind::Fm) {
                    v.modPhase += v.modInc;
                    if (v.modPhase >= 1.0) v.modPhase -= std::floor(v.modPhase);
                    const f64 mod = std::sin(kTauD * v.modPhase);
                    // 2-операторный FM: фаза несущей смещается модулятором —
                    // именно это даёт «сеговский» тембр.
                    const f64 ph = v.samplePos / static_cast<f64>(n) +
                                   v.modIndex * mod / static_cast<f64>(n);
                    s = static_cast<f32>(std::sin(kTauD * ph));
                    v.reportedPhase = static_cast<f32>(ph - std::floor(ph));
                }
                return ClampF(s, -1.5f, 1.5f);
            }
            default:
                return 0.0f;
        }
    }

    void RenderInternal(int n) {
        if (n <= 0) return;
        const f64 dt = 1.0 / static_cast<f64>(kInternalRate);
        std::fill(mixL.begin(), mixL.begin() + n, 0.0f);
        std::fill(mixR.begin(), mixR.begin() + n, 0.0f);
        const bool sends = useSends;
        if (sends) {
            std::fill(sendL.begin(), sendL.begin() + n, 0.0f);
            std::fill(sendR.begin(), sendR.begin() + n, 0.0f);
        }
        for (Voice& v : voices) {
            if (!v.active || v.muted) continue;
            PrepareVoiceBlock(v);
            f32* ol = mixL.data();
            f32* orr = mixR.data();
            f32* sl = sendL.data();
            f32* sr = sendR.data();
            const f32 pl = v.panL;
            const f32 pr = v.panR;
            const f32 sg = (sends && v.sendGain > 0.0f) ? v.sendGain : 0.0f;
            for (int i = 0; i < n; ++i) {
                const f64 env = AdvanceEnvelope(v, dt);
                if (!v.active && env <= 0.0) {
                    // Всё равно потребляем этот семпл, чтобы фаза оставалась когерентной.
                    RenderVoiceSample(v, dt);
                    continue;
                }
                const f32 g = VoiceGain(v, env);
                const f32 s = RenderVoiceSample(v, dt) * g;
                if (!Finite(s)) continue;
                const f32 l = s * pl;
                const f32 r = s * pr;
                ol[i] += l;
                orr[i] += r;
                if (sg > 0.0f) {
                    sl[i] += l * sg;
                    sr[i] += r * sg;
                }
            }
            if (v.envLevel <= 0.0 && v.envStage == 3) v.active = false;
        }
        ApplyEcho(n);
        ApplyLowPass(n);
    }

    void ApplyEcho(int n) {
        if (echoL.empty()) return;
        const bool on = (song && song->echoEnabled) || echoNoteOn;
        if (!on) return;
        const int len = static_cast<int>(echoL.size());
        const int tapSpacing = std::max(1, len / 8);
        const f32 fb = static_cast<f32>(ClampI(song ? song->echoFeedback : 90, 0, 255)) / 256.0f * 0.9f;
        const f32 ev = static_cast<f32>(ClampI(song ? song->echoVolume : 60, 0, 255)) / 256.0f * 0.8f;
        f32* bufL = echoL.data();
        f32* bufR = echoR.data();
        int w = echoWrite;
        for (int i = 0; i < n; ++i) {
            f32 tapL = 0.0f, tapR = 0.0f;
            for (int t = 0; t < 8; ++t) {
                int idx = w - 1 - t * tapSpacing;
                while (idx < 0) idx += len;
                tapL += bufL[idx];
                tapR += bufR[idx];
            }
            tapL *= 0.125f;
            tapR *= 0.125f;
            const f32 dryL = mixL[static_cast<usize>(i)];
            const f32 dryR = mixR[static_cast<usize>(i)];
            const f32 inL = useSends ? sendL[static_cast<usize>(i)] : dryL;
            const f32 inR = useSends ? sendR[static_cast<usize>(i)] : dryR;
            bufL[w] = inL + tapL * fb;
            bufR[w] = inR + tapR * fb;
            mixL[static_cast<usize>(i)] = dryL + tapL * ev;
            mixR[static_cast<usize>(i)] = dryR + tapR * ev;
            if (++w >= len) w = 0;
        }
        echoWrite = w;
    }

    void ApplyLowPass(int n) {
        if (!song || !song->lowPass) return;
        for (int c = 0; c < 2; ++c) {
            f32* buf = c == 0 ? mixL.data() : mixR.data();
            f32 z1 = lpZ1[c];
            f32 z2 = lpZ2[c];
            for (int i = 0; i < n; ++i) {
                const f32 x = buf[i];
                const f32 y = lpb0 * x + z1;
                z1 = lpb1 * x - lpa1 * y + z2;
                z2 = lpb2 * x - lpa2 * y;
                buf[i] = y;
            }
            lpZ1[c] = z1;
            lpZ2[c] = z2;
        }
    }

    // Сколько внутренних фреймов израсходует ресемплер для `m` выходных
    // фреймов; в точности повторяет цикл выборки в ResampleBlock.
    int CountPulls(int m) const {
        int pulls = 0;
        f64 ph = rsPhase;
        for (int j = 0; j < m; ++j) {
            ph += rsStep;
            while (ph >= 1.0) {
                ph -= 1.0;
                ++pulls;
            }
        }
        return pulls;
    }

    void ResampleBlock(int m, int internalFrames, f32* dst, f32* peakOut) {
        f32 peak = 0.0f;
        int idx = 0;
        for (int j = 0; j < m; ++j) {
            rsPhase += rsStep;
            while (rsPhase >= 1.0) {
                rsPhase -= 1.0;
                rsHistL[0] = rsHistL[1];
                rsHistR[0] = rsHistR[1];
                if (idx < internalFrames) {
                    rsHistL[1] = mixL[static_cast<usize>(idx)];
                    rsHistR[1] = mixR[static_cast<usize>(idx)];
                } else {
                    rsHistL[1] = 0.0f;
                    rsHistR[1] = 0.0f;
                }
                ++idx;
            }
            // Двухточечная (дробная) интерполяция между двумя последними
            // внутренними семплами: дешёвая ступень полифазного/линейного ресемплинга.
            const f32 f = static_cast<f32>(rsPhase);
            const f32 l = rsHistL[0] + (rsHistL[1] - rsHistL[0]) * f;
            const f32 r = rsHistR[0] + (rsHistR[1] - rsHistR[0]) * f;
            // DC-блокиратор (однополюсный ФВЧ на ~20 Гц).
            const f32 dl = l - dcPrevIn[0] + static_cast<f32>(dcR) * dcPrevOut[0];
            const f32 dr = r - dcPrevIn[1] + static_cast<f32>(dcR) * dcPrevOut[1];
            dcPrevIn[0] = l;
            dcPrevIn[1] = r;
            dcPrevOut[0] = dl;
            dcPrevOut[1] = dr;
            const f32 sl = SoftClip(dl * masterVolume);
            const f32 sr = SoftClip(dr * masterVolume);
            dst[static_cast<usize>(j) * 2 + 0] += sl;
            dst[static_cast<usize>(j) * 2 + 1] += sr;
            peak = std::max(peak, std::max(std::fabs(sl), std::fabs(sr)));
        }
        if (peakOut) *peakOut = peak;
    }

    void RenderChunk(f32* dst, int m) {
        if (m <= 0) return;
        int need = CountPulls(m);
        if (need > kMaxInternalBlock) need = kMaxInternalBlock;
        RenderInternal(need);
        f32 peak = 0.0f;
        ResampleBlock(m, need, dst, &peak);
        lastPeak = std::max(lastPeak, peak);
    }

    bool AllSilent() const {
        for (const Voice& v : voices)
            if (v.active) return false;
        return true;
    }

    void RenderAdd(f32* out, int frames) {
        lastPeak = 0.0f;
        if (!out || frames <= 0) return;
        if (!song || !playing) {
            SyncStates();
            return;
        }
        const f64 fps = static_cast<f64>(song->framesPerSecond);
        const f64 framesPerTick = static_cast<f64>(sampleRate) / fps;
        const int maxChunk = std::max(1, static_cast<int>((kMaxInternalBlock - 4) / rsStep));
        int done = 0;
        while (done < frames && playing) {
            int chunk = frames - done;
            if (!ended) {
                const f64 until = framesPerTick - frameInTick;
                int toTick = static_cast<int>(std::ceil(until - 1e-9));
                if (toTick < 1) toTick = 1;
                if (chunk > toTick) chunk = toTick;
            }
            if (chunk > maxChunk) chunk = maxChunk;
            RenderChunk(out + static_cast<usize>(done) * 2, chunk);
            done += chunk;
            totalFrames += chunk;
            if (ended) {
                if (AllSilent()) {
                    tailSilentFrames += chunk;
                    if (tailSilentFrames > sampleRate / 2) playing = false;
                } else {
                    tailSilentFrames = 0;
                }
            } else {
                frameInTick += chunk;
                if (frameInTick + kTickEpsilon >= framesPerTick) {
                    frameInTick -= framesPerTick;
                    if (frameInTick < 0.0) frameInTick = 0.0;
                    AdvanceTick();
                }
            }
        }
        renderedFrames += static_cast<u64>(done);
        SyncStates();
    }

    void Advance(f32 dt) {
        if (!song || !playing || dt <= 0.0f) {
            SyncStates();
            return;
        }
        const f64 fps = static_cast<f64>(song->framesPerSecond);
        const f64 framesPerTick = static_cast<f64>(sampleRate) / fps;
        frameInTick += static_cast<f64>(dt) * static_cast<f64>(sampleRate);
        int guard = 0;
        while (frameInTick + kTickEpsilon >= framesPerTick && playing && !ended &&
               guard < 100000) {
            frameInTick -= framesPerTick;
            AdvanceTick();
            ++guard;
        }
        if (frameInTick < 0.0) frameInTick = 0.0;
        if (ended) playing = false;
        totalFrames += static_cast<f64>(dt) * static_cast<f64>(sampleRate);
        renderedFrames += static_cast<u64>(static_cast<f64>(dt) * sampleRate);
        SyncStates();
    }

    void SyncStates() {
        for (usize i = 0; i < states.size(); ++i) {
            ChipPlayer::ChannelState& st = states[i];
            if (i < voices.size()) {
                const Voice& v = voices[i];
                const int prevNote = st.note;
                st.active = v.active;
                st.note = v.note;
                st.frequency = static_cast<f32>(v.freq);
                st.volume = ClampI(static_cast<int>(std::lround(v.gainNow * v.envLevel * 15.0)), 0,
                                   15);
                st.phase = v.reportedPhase;
                st.effect = v.lastEffect;
                st.arpStep = v.arpMode != 0 ? v.arpIndex : 0;
                if (v.note >= 0 && v.note != prevNote) st.lastNoteName = ChipPlayer::NoteName(v.note);
            }
            if (i < muted.size()) st.muted = muted[i] != 0;
        }
    }
};

// ---------------------------------------------------------------------------
// ChipSong
// ---------------------------------------------------------------------------
f32 ChipSong::DurationSeconds(int orderIndex) const {
    if (order.empty()) return 0.0f;
    int start = orderIndex < 0 ? 0 : orderIndex;
    if (start >= static_cast<int>(order.size())) return 0.0f;
    const int tpr = SongTicksPerRow(*this);
    const int fps = framesPerSecond > 0 ? framesPerSecond : 60;
    f64 rows = 0.0;
    for (int i = start; i < static_cast<int>(order.size()); ++i) {
        const int pi = order[static_cast<usize>(i)];
        if (pi < 0 || pi >= static_cast<int>(patterns.size())) continue;
        rows += patterns[static_cast<usize>(pi)].rowCount;
    }
    return static_cast<f32>(rows * static_cast<f64>(tpr) / static_cast<f64>(fps));
}

int ChipSong::TotalRows() const {
    int total = 0;
    for (int pi : order) {
        if (pi < 0 || pi >= static_cast<int>(patterns.size())) continue;
        total += patterns[static_cast<usize>(pi)].rowCount;
    }
    return total;
}

// ---------------------------------------------------------------------------
// ChipPlayer
// ---------------------------------------------------------------------------
ChipPlayer::ChipPlayer() : impl_(new Impl()) {}
ChipPlayer::~ChipPlayer() = default;

void ChipPlayer::SetSong(const ChipSong& song) {
    impl_->rowCb = rowCb_;
    impl_->rowUser = rowUser_;
    impl_->SetSong(song);
    song_ = impl_->song;
}

const ChipSong& ChipPlayer::Song() const {
    static const ChipSong kEmpty;
    return song_ ? *song_ : kEmpty;
}

void ChipPlayer::Play() {
    impl_->rowCb = rowCb_;
    impl_->rowUser = rowUser_;
    if (!song_) return;
    if (!impl_->started || impl_->ended) {
        impl_->ResetPlayback();
        impl_->playing = true;
        impl_->started = true;
        impl_->StartRow();
    } else {
        impl_->playing = true;
    }
    impl_->SyncStates();
}

void ChipPlayer::Pause() {
    impl_->playing = false;
    impl_->SyncStates();
}

void ChipPlayer::Stop() {
    impl_->playing = false;
    impl_->ResetPlayback();
}

void ChipPlayer::SetLoop(bool loop) { impl_->loop = loop; }
bool ChipPlayer::IsPlaying() const { return impl_->playing; }

void ChipPlayer::SetSpeed(int ticksPerRow) {
    impl_->speedOverride = ClampI(ticksPerRow, 1, 64);
}

void ChipPlayer::SetOrder(int orderIndex) {
    if (!song_) return;
    const int n = static_cast<int>(impl_->song->order.size());
    impl_->orderIndex = ClampI(orderIndex, 0, std::max(0, n - 1));
    impl_->row = 0;
    impl_->tick = 0;
    impl_->frameInTick = 0.0;
    impl_->ended = false;
    impl_->pendingJump = false;
    impl_->tailSilentFrames = 0;
    if (impl_->playing || impl_->started) {
        impl_->started = true;
        impl_->StartRow();
    }
    impl_->SyncStates();
}

void ChipPlayer::SetChannelMute(int channel, bool muted) {
    if (!impl_->song) return;
    if (channel < 0 || channel >= static_cast<int>(impl_->voices.size())) return;
    impl_->muted[static_cast<usize>(channel)] = muted ? 1 : 0;
    impl_->voices[static_cast<usize>(channel)].muted = muted;
    impl_->states[static_cast<usize>(channel)].muted = muted;
}

bool ChipPlayer::ChannelMuted(int channel) const {
    if (channel < 0 || channel >= static_cast<int>(impl_->muted.size())) return false;
    return impl_->muted[static_cast<usize>(channel)] != 0;
}

void ChipPlayer::SetMasterVolume(f32 v) { impl_->masterVolume = ClampF(v, 0.0f, 4.0f); }

void ChipPlayer::SetSampleRate(int hz) {
    impl_->sampleRate = ClampI(hz, 8000, 192000);
    impl_->UpdateResampler();
}

void ChipPlayer::RenderAdd(f32* out, int frameCount) {
    impl_->rowCb = rowCb_;
    impl_->rowUser = rowUser_;
    impl_->RenderAdd(out, frameCount);
}

void ChipPlayer::Advance(f32 dt) {
    impl_->rowCb = rowCb_;
    impl_->rowUser = rowUser_;
    impl_->Advance(dt);
}

const std::vector<ChipPlayer::ChannelState>& ChipPlayer::ChannelStates() const {
    return impl_->states;
}

int ChipPlayer::CurrentOrder() const { return impl_->orderIndex; }
int ChipPlayer::CurrentRow() const { return impl_->row; }
int ChipPlayer::CurrentTick() const { return impl_->tick; }
f32 ChipPlayer::CurrentTime() const {
    return impl_->sampleRate > 0
               ? static_cast<f32>(impl_->totalFrames / static_cast<f64>(impl_->sampleRate))
               : 0.0f;
}
u64 ChipPlayer::RenderedFrames() const { return impl_->renderedFrames; }
f32 ChipPlayer::LastPeak() const { return impl_->lastPeak; }

// ---------------------------------------------------------------------------
// Кодирование WAV (используется Bake, чтобы клип шёл через декодер движка)
// ---------------------------------------------------------------------------
namespace {

void PutU16(std::vector<u8>& v, u16 x) {
    v.push_back(static_cast<u8>(x & 0xFF));
    v.push_back(static_cast<u8>((x >> 8) & 0xFF));
}
void PutU32(std::vector<u8>& v, u32 x) {
    v.push_back(static_cast<u8>(x & 0xFF));
    v.push_back(static_cast<u8>((x >> 8) & 0xFF));
    v.push_back(static_cast<u8>((x >> 16) & 0xFF));
    v.push_back(static_cast<u8>((x >> 24) & 0xFF));
}

std::vector<u8> EncodeWav16(const f32* interleaved, int frames, int channels, int sampleRate) {
    std::vector<u8> out;
    const u32 dataBytes = static_cast<u32>(frames) * static_cast<u32>(channels) * 2u;
    out.reserve(static_cast<usize>(44) + dataBytes);
    const char* riff = "RIFF";
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(riff[i]));
    PutU32(out, 36u + dataBytes);
    const char* wave = "WAVEfmt ";
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<u8>(wave[i]));
    PutU32(out, 16);
    PutU16(out, 1);  // PCM
    PutU16(out, static_cast<u16>(channels));
    PutU32(out, static_cast<u32>(sampleRate));
    PutU32(out, static_cast<u32>(sampleRate * channels * 2));
    PutU16(out, static_cast<u16>(channels * 2));
    PutU16(out, 16);
    const char* data = "data";
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(data[i]));
    PutU32(out, dataBytes);
    for (int i = 0; i < frames * channels; ++i) {
        f32 s = interleaved[i];
        if (!Finite(s)) s = 0.0f;
        s = ClampF(s, -1.0f, 1.0f);
        int q = static_cast<int>(std::lround(s * 32767.0f));
        q = ClampI(q, -32768, 32767);
        PutU16(out, static_cast<u16>(static_cast<i16>(q)));
    }
    return out;
}

}  // namespace

AudioClip ChipPlayer::Bake(int sampleRate, f32 seconds, f32 loopFadeSeconds,
                           int maxSeconds) const {
    AudioClip clip;
    if (!song_) return clip;
    sampleRate = ClampI(sampleRate, 8000, 192000);
    maxSeconds = ClampI(maxSeconds, 1, 3600);

    f64 duration = seconds > 0.0f ? static_cast<f64>(seconds)
                                  : static_cast<f64>(song_->DurationSeconds(-1));
    if (duration <= 0.0) return clip;
    duration = std::min(duration, static_cast<f64>(maxSeconds));

    const int frames = static_cast<int>(std::llround(duration * sampleRate));
    if (frames <= 0) return clip;
    int fade = static_cast<int>(std::llround(static_cast<f64>(loopFadeSeconds) * sampleRate));
    fade = ClampI(fade, 0, std::max(0, frames / 4));

    ChipPlayer local;
    local.SetSong(*song_);
    local.SetSampleRate(sampleRate);
    local.SetLoop(true);
    local.SetMasterVolume(impl_->masterVolume);
    local.Play();

    std::vector<f32> buf;
    local.RenderTo(&buf, frames + fade);
    if (static_cast<int>(buf.size()) < (frames + fade) * 2)
        buf.resize(static_cast<usize>(frames + fade) * 2, 0.0f);

    // Кроссфейдим начало клипа с продолжением лупа: последний семпл перетекает
    // в первый точно так же, как это было в непрерывном рендере, поэтому клип
    // можно проигрывать встык без щелчка
    // (и без паузы на стыке).
    for (int i = 0; i < fade; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(fade);
        for (int c = 0; c < 2; ++c) {
            const usize a = static_cast<usize>(i) * 2 + static_cast<usize>(c);
            const usize b = static_cast<usize>(frames + i) * 2 + static_cast<usize>(c);
            buf[a] = buf[a] * t + buf[b] * (1.0f - t);
        }
    }
    buf.resize(static_cast<usize>(frames) * 2);

    std::vector<u8> wav = EncodeWav16(buf.data(), frames, 2, sampleRate);
    if (!clip.LoadFromMemory(wav.data(), wav.size(), AudioFormat::Wav)) {
        ENG_LOGE("chiptune", "Bake: failed to build the clip for '%s'", song_->title.c_str());
    }
    return clip;
}

void ChipPlayer::RenderTo(std::vector<f32>* out, int frames) {
    if (!out) return;
    if (frames <= 0) {
        out->clear();
        return;
    }
    out->assign(static_cast<usize>(frames) * 2, 0.0f);
    RenderAdd(out->data(), frames);
}

// ---------------------------------------------------------------------------
// Хелперы нот
// ---------------------------------------------------------------------------
std::string ChipPlayer::NoteName(int semitone) {
    if (semitone == -1) return "---";
    if (semitone == -2) return "OFF";
    if (semitone < 0) semitone = 0;
    static const char* kNames[12] = {"C-", "C#", "D-", "D#", "E-", "F-",
                                     "F#", "G-", "G#", "A-", "A#", "B-"};
    const int octave = semitone / 12;
    const int idx = semitone % 12;
    return std::string(kNames[idx]) + std::to_string(octave);
}

f32 ChipPlayer::NoteFrequency(int semitone) {
    if (semitone < 0) return 0.0f;
    return static_cast<f32>(SemitoneToFreq(static_cast<f64>(semitone)));
}

// ---------------------------------------------------------------------------
// Хелперы построения песен
// ---------------------------------------------------------------------------
namespace {

struct Chord {
    int root;      // басовая тоника, октавы 1-3
    int fifth;     // басовая квинта, октавы 2-3
    int arp[4];    // аккомпанирующее арпеджио (октавы 3-5)
    int pad[3];    // тянущиеся ноты пэда для 16-битной песни
};

struct Mel {
    int row;
    int note;
    int len;
};

void PutNote(ChipPattern& p, int row, int ch, int semi, int vol = 0xFF,
             int fx = ChipFxNone, int param = 0, int arp = 0xFF) {
    if (row < 0 || row >= p.rowCount || ch < 0 || ch >= p.channelCount) return;
    ChipNote& n = p.At(row, ch);
    n.semitone = static_cast<i16>(semi);
    n.instrument = 0;
    n.volume = static_cast<u8>(ClampI(vol, 0, 255));
    n.effect = static_cast<u8>(ClampI(fx, 0, 255));
    n.param = static_cast<u8>(ClampI(param, 0, 255));
    n.arp = static_cast<u8>(ClampI(arp, 0, 255));
}

void PutOff(ChipPattern& p, int row, int ch) {
    if (row < 0 || row >= p.rowCount || ch < 0 || ch >= p.channelCount) return;
    ChipNote& n = p.At(row, ch);
    n.semitone = -2;
    n.volume = 0xFF;
    n.effect = 0;
    n.param = 0;
    n.arp = 0xFF;
}

void PutMelody(ChipPattern& p, int ch, const Mel* notes, int count, int vol) {
    for (int i = 0; i < count; ++i) {
        PutNote(p, notes[i].row, ch, notes[i].note, vol);
        const int end = notes[i].row + notes[i].len;
        if (end > 0 && end < p.rowCount && p.At(end, ch).semitone == -1) PutOff(p, end, ch);
    }
}

ChipChannelDef MakeChannel(ChipWave wave, int volume, int attack, int decay, int sustain,
                           int release, int pan = 0, int detune = 0, int dutySweep = 0,
                           int pitchSweep = 0, bool arpeggio = false, int echoVolume = 0) {
    ChipChannelDef c;
    c.wave = wave;
    c.volume = ClampI(volume, 0, 15);
    c.attack = attack;
    c.decay = decay;
    c.sustain = sustain;
    c.release = release;
    c.pan = ClampI(pan, -8, 8);
    c.detune = detune;
    c.dutySweep = dutySweep;
    c.pitchSweep = pitchSweep;
    c.arpeggio = arpeggio;
    c.echoVolume = echoVolume;
    return c;
}

ChipPattern NewPattern(const char* name, int rows, int channels) {
    ChipPattern p;
    p.name = name;
    p.Resize(rows, channels);
    return p;
}

// Перкуссия: периоды шума NES адресуются номером ноты (0 = самый высокий).
// 15 = гул бочки, 10 = снейр, 2 = яркий хэт.
void AddDrums(ChipPattern& p, int ch, int bar, int variant, int hatVol) {
    const int b = bar * 16;
    PutNote(p, b + 0, ch, 15, 15);
    PutNote(p, b + 2, ch, 2, hatVol, ChipFxNoteCut, 1);
    PutNote(p, b + 4, ch, 10, 12);
    PutNote(p, b + 6, ch, 2, hatVol, ChipFxNoteCut, 1);
    PutNote(p, b + 8, ch, 15, 15);
    PutNote(p, b + 10, ch, 2, hatVol, ChipFxNoteCut, 1);
    PutNote(p, b + 12, ch, 10, 12);
    PutNote(p, b + 14, ch, 2, hatVol, ChipFxNoteCut, 1);
    if (variant == 1) {
        PutNote(p, b + 11, ch, 15, 10, ChipFxNoteCut, 2);
    } else if (variant == 2) {
        PutNote(p, b + 12, ch, 12, 13);
        PutNote(p, b + 13, ch, 10, 13);
        PutNote(p, b + 14, ch, 8, 14);
        PutNote(p, b + 15, ch, 6, 15);
    }
}

// Аккомпанемент в духе NES: арпеджио гармонии + бас из тоники/квинты на треугольнике.
void AddNesBar(ChipPattern& p, int bar, const Chord& c, bool arp, bool drums, int variant,
               int hatVol) {
    const int b = bar * 16;
    PutNote(p, b + 0, 2, c.root, 15);
    PutNote(p, b + 2, 2, c.root + 12, 11);
    PutNote(p, b + 4, 2, c.fifth, 13);
    PutNote(p, b + 6, 2, c.root + 12, 11);
    PutNote(p, b + 8, 2, c.root, 15);
    PutNote(p, b + 10, 2, c.root + 12, 11);
    PutNote(p, b + 12, 2, c.fifth, 13);
    PutNote(p, b + 14, 2, c.root + 12, 11);
    if (arp) {
        for (int i = 0; i < 8; ++i) PutNote(p, b + i * 2, 1, c.arp[i & 3], 9);
    }
    if (drums) AddDrums(p, 3, bar, variant, hatVol);
}

// ---------------------------------------------------------------------------
// Детерминированные встроенные песни
// ---------------------------------------------------------------------------

// "8bit": NES 2A03, ля минор, Am - F - C - G - Dm - E.
ChipSong Build8Bit(u64 seed) {
    Random rng(seed);
    ChipSong s;
    s.title = "Neon Cavern";
    s.chip = ChipType::Nes2A03;
    s.channelCount = 4;
    s.framesPerSecond = 50;
    s.rowsPerBeat = 4;
    s.ticksPerRow = 6;                                        // 125 BPM
    s.beatsPerMinute = s.framesPerSecond * 60 / (s.ticksPerRow * s.rowsPerBeat);
    s.loopOrder = 0;
    s.channels = {
        MakeChannel(ChipWave::Pulse50, 13, 0, 2, 13, 5),      // соло
        MakeChannel(ChipWave::Pulse25, 9, 0, 4, 9, 6),        // гармония
        MakeChannel(ChipWave::Triangle, 15, 0, 0, 15, 2),     // бас
        MakeChannel(ChipWave::Noise, 12, 0, 4, 0, 2),         // перкуссия
    };

    static const Chord kChords[6] = {
        /* Am */ {33, 40, {57, 60, 64, 60}, {0, 0, 0}},
        /* F  */ {29, 36, {53, 57, 60, 57}, {0, 0, 0}},
        /* C  */ {36, 43, {60, 64, 67, 64}, {0, 0, 0}},
        /* G  */ {31, 38, {55, 59, 62, 59}, {0, 0, 0}},
        /* Dm */ {38, 45, {50, 53, 57, 53}, {0, 0, 0}},
        /* E  */ {28, 35, {52, 55, 59, 55}, {0, 0, 0}},
    };
    // Паттерн -> (аккорд такта 0, аккорд такта 1)
    static const int kProg[6][2] = {{0, 0}, {0, 1}, {2, 3}, {0, 1}, {4, 5}, {0, 3}};

    // Солирующая мелодия, по записи на (ряд, нота, длина в рядах).
    static const Mel kLead0[] = {
        {0, 76, 6}, {8, 69, 6}, {16, 76, 6}, {24, 72, 6},
    };
    static const Mel kLead1[] = {
        {0, 76, 4}, {4, 74, 2}, {6, 72, 2}, {8, 69, 4}, {12, 72, 2},
        {14, 74, 2}, {16, 77, 6}, {24, 76, 2}, {26, 74, 2}, {28, 72, 2}, {30, 71, 1},
    };
    static const Mel kLead2[] = {
        {0, 72, 2}, {2, 76, 2}, {4, 79, 4}, {8, 76, 2}, {10, 74, 2},
        {12, 72, 4}, {16, 74, 4}, {20, 71, 2}, {22, 74, 2}, {24, 67, 8},
    };
    static const Mel kLead4[] = {
        {0, 74, 2}, {2, 77, 2}, {4, 81, 4}, {8, 79, 2}, {10, 77, 2},
        {12, 76, 4}, {16, 76, 2}, {20, 80, 2}, {22, 76, 2}, {24, 71, 8},
    };
    static const Mel kLead5[] = {
        {0, 72, 2}, {2, 74, 2}, {4, 76, 4}, {8, 81, 4}, {12, 79, 2},
        {14, 76, 2}, {16, 74, 4}, {20, 71, 2}, {22, 74, 2}, {24, 67, 6}, {30, 71, 2},
    };
    // Форма AABA: тема «Rise» возвращается дословно как паттерн 3 (отличается
    // только сбивка барабанов) — именно это делает мотив запоминающимся.
    static const Mel* kLeads[6] = {kLead0, kLead1, kLead2, kLead1, kLead4, kLead5};
    static const int kLeadCounts[6] = {4, 11, 10, 11, 10, 11};

    static const char* kNames[6] = {"Intro", "Rise", "Answer", "Drift", "Siege", "Turn"};
    for (int pi = 0; pi < 6; ++pi) {
        ChipPattern p = NewPattern(kNames[pi], 32, 4);
        const int hatVol = 5 + static_cast<int>(rng.NextU32() % 3u);
        for (int bar = 0; bar < 2; ++bar) {
            const Chord& c = kChords[kProg[pi][bar]];
            const bool drums = !(pi == 0 && bar == 0);
            int variant = 0;
            if (bar == 1 && rng.Chance(0.35f)) variant = 1;
            if (pi == 5 && bar == 1) variant = 2;
            AddNesBar(p, bar, c, true, drums, variant, hatVol);
        }
        PutMelody(p, 0, kLeads[pi], kLeadCounts[pi], 13);
        s.AddPattern(p);
        s.order.push_back(pi);
    }
    return s;
}

// "16bit": SNES SPC, ре мажор, D - Bm - G - A - Bm - G, 8 семпл-голосов, эхо.
ChipSong Build16Bit(u64 seed) {
    Random rng(seed);
    ChipSong s;
    s.title = "Glass Skyline";
    s.chip = ChipType::SnesSpc;
    s.channelCount = 8;
    s.framesPerSecond = 60;
    s.rowsPerBeat = 4;
    s.ticksPerRow = 9;                                        // 100 BPM
    s.beatsPerMinute = s.framesPerSecond * 60 / (s.ticksPerRow * s.rowsPerBeat);
    s.loopOrder = 0;
    s.echoEnabled = true;
    s.echoDelayMs = 180;
    s.echoFeedback = 150;
    s.echoVolume = 110;
    s.lowPass = true;
    s.lowPassCutoff = 7000;
    s.channels = {
        MakeChannel(ChipWave::Square, 10, 6, 8, 10, 11, -3, 0, 0, 0, false, 12),   // пэд низкий
        MakeChannel(ChipWave::Triangle, 9, 8, 8, 8, 12, 3, 0, 0, 0, false, 10),    // пэд высокий
        MakeChannel(ChipWave::Sine, 12, 0, 7, 4, 11, 0, 0, 0, 0, false, 13),       // колокольное соло
        MakeChannel(ChipWave::Saw, 11, 0, 3, 12, 3, 0, 0, 0, 0, false, 0),         // бас
        MakeChannel(ChipWave::Noise, 10, 0, 4, 0, 3, 0, 0, 0, 0, false, 4),        // барабаны
        MakeChannel(ChipWave::Sample, 8, 2, 6, 6, 9, -6, 4, 6, 12, true, 10),      // арпеджио
        MakeChannel(ChipWave::Triangle, 8, 4, 6, 9, 9, 6, -4, 0, 0, false, 9),     // гармония
        MakeChannel(ChipWave::Square, 7, 0, 5, 2, 9, 8, 0, 0, 0, false, 8),        // блёстки
    };

    static const Chord kChords[6] = {
        /* D   */ {26, 33, {62, 66, 69, 66}, {50, 54, 57}},
        /* Bm  */ {23, 30, {59, 62, 66, 62}, {47, 50, 54}},
        /* G   */ {31, 38, {55, 59, 62, 59}, {43, 47, 50}},
        /* A   */ {33, 40, {57, 61, 64, 61}, {45, 49, 52}},
        /* Em  */ {28, 35, {52, 55, 59, 55}, {40, 43, 47}},
        /* F#m */ {30, 37, {54, 57, 61, 57}, {42, 45, 49}},
    };
    static const int kProg[6][2] = {{0, 0}, {0, 1}, {2, 3}, {0, 1}, {1, 2}, {0, 3}};

    static const Mel kBell0[] = {
        {0, 62, 8}, {16, 69, 8},
    };
    static const Mel kBell1[] = {
        {0, 69, 4}, {4, 71, 4}, {8, 74, 6}, {16, 71, 4}, {20, 69, 2},
        {22, 66, 2}, {24, 62, 8},
    };
    static const Mel kBell2[] = {
        {0, 67, 4}, {4, 69, 4}, {8, 71, 6}, {16, 73, 4}, {20, 71, 2},
        {22, 69, 2}, {24, 66, 6}, {30, 64, 2},
    };
    static const Mel kBell4[] = {
        {0, 66, 4}, {4, 69, 4}, {8, 71, 6}, {16, 67, 4}, {20, 71, 2},
        {22, 74, 2}, {24, 71, 8},
    };
    static const Mel kBell5[] = {
        {0, 62, 2}, {2, 66, 2}, {4, 69, 2}, {6, 74, 4}, {12, 73, 2}, {14, 71, 2},
        {16, 69, 6}, {24, 66, 2}, {26, 64, 2}, {28, 62, 4},
    };
    // Снова форма AABA: тема «Skyline» возвращается как паттерн 3.
    static const Mel* kBells[6] = {kBell0, kBell1, kBell2, kBell1, kBell4, kBell5};
    static const int kBellCounts[6] = {2, 7, 8, 7, 7, 10};

    static const char* kNames[6] = {"Prelude", "Skyline", "Ascent", "Chrome", "Drift", "Return"};
    for (int pi = 0; pi < 6; ++pi) {
        ChipPattern p = NewPattern(kNames[pi], 32, 8);
        const int hatVol = 4 + static_cast<int>(rng.NextU32() % 3u);
        for (int bar = 0; bar < 2; ++bar) {
            const Chord& c = kChords[kProg[pi][bar]];
            const int b = bar * 16;
            // Пэды: тоника + квинта, по ноте на такт.
            PutNote(p, b + 0, 0, c.pad[0], 10);
            PutNote(p, b + 0, 1, c.pad[2], 9);
            PutOff(p, b + 14, 0);
            PutOff(p, b + 14, 1);
            PutNote(p, b + 0, 6, c.pad[1], 8);
            PutOff(p, b + 14, 6);
            // Бас: тоника на 1 и 3, октава на 2 и 4.
            PutNote(p, b + 0, 3, c.root, 11);
            PutNote(p, b + 4, 3, c.root + 12, 9);
            PutNote(p, b + 8, 3, c.root, 11);
            PutNote(p, b + 12, 3, c.root + 12, 9);
            // Голос арпеджио (голос 5) и блёстки (голос 7).
            for (int i = 0; i < 8; ++i) {
                PutNote(p, b + i * 2, 5, c.arp[(i + bar) & 3], 8);
                if ((i & 1) == 0 && bar == 1)
                    PutNote(p, b + i * 2 + 1, 7, c.arp[(i + 1) & 3] + 12, 6);
            }
            if (!(pi == 0 && bar == 0)) {
                int variant = (bar == 1 && rng.Chance(0.35f)) ? 1 : 0;
                if (pi == 5 && bar == 1) variant = 2;
                AddDrums(p, 4, bar, variant, hatVol);
            }
        }
        PutMelody(p, 2, kBells[pi], kBellCounts[pi], 12);
        s.AddPattern(p);
        s.order.push_back(pi);
    }
    return s;
}

// "boss": быстро, минор, агрессивные арпеджио шестнадцатыми и шумовые удары.
ChipSong BuildBoss(u64 seed) {
    Random rng(seed);
    ChipSong s;
    s.title = "Iron Warden";
    s.chip = ChipType::Nes2A03;
    s.channelCount = 4;
    s.framesPerSecond = 60;
    s.rowsPerBeat = 4;
    s.ticksPerRow = 6;                                        // 150 BPM
    s.beatsPerMinute = s.framesPerSecond * 60 / (s.ticksPerRow * s.rowsPerBeat);
    s.loopOrder = 0;
    s.channels = {
        MakeChannel(ChipWave::Pulse50, 13, 0, 1, 12, 3),
        MakeChannel(ChipWave::Pulse12, 10, 0, 2, 8, 4),
        MakeChannel(ChipWave::Triangle, 15, 0, 0, 15, 1),
        MakeChannel(ChipWave::Noise, 13, 0, 3, 0, 2),
    };

    static const Chord kChords[4] = {
        /* Dm  */ {26, 33, {62, 65, 69, 65}, {0, 0, 0}},
        /* Bb  */ {22, 29, {58, 62, 65, 62}, {0, 0, 0}},
        /* Gm  */ {19, 26, {55, 58, 62, 58}, {0, 0, 0}},
        /* A   */ {21, 28, {57, 61, 64, 61}, {0, 0, 0}},
    };
    static const int kProg[5][2] = {{0, 1}, {2, 3}, {0, 0}, {1, 3}, {0, 3}};

    // Соло: короткие агрессивные уколы в ре миноре.
    static const Mel kLead0[] = {
        {0, 74, 2}, {4, 77, 2}, {8, 81, 4}, {16, 79, 2}, {20, 77, 2}, {24, 74, 4},
    };
    static const Mel kLead1[] = {
        {0, 79, 4}, {8, 77, 2}, {12, 74, 2}, {16, 72, 4}, {24, 74, 4},
    };
    static const Mel kLead2[] = {
        {0, 86, 2}, {2, 84, 2}, {4, 81, 2}, {6, 79, 2}, {8, 77, 4},
        {16, 81, 2}, {18, 79, 2}, {20, 77, 2}, {22, 74, 2}, {24, 69, 6},
    };
    static const Mel kLead3[] = {
        {0, 77, 4}, {8, 82, 4}, {16, 79, 4}, {24, 74, 6},
    };
    static const Mel kLead4[] = {
        {0, 74, 2}, {4, 79, 2}, {8, 81, 2}, {12, 82, 4}, {20, 81, 2},
        {22, 79, 2}, {24, 74, 6}, {30, 76, 1},
    };
    static const Mel* kLeads[5] = {kLead0, kLead1, kLead2, kLead3, kLead4};
    static const int kLeadCounts[5] = {6, 5, 10, 4, 8};

    static const char* kNames[5] = {"Alarm", "Chase", "Fangs", "Gate", "Warden"};
    for (int pi = 0; pi < 5; ++pi) {
        ChipPattern p = NewPattern(kNames[pi], 32, 4);
        const int hatVol = 4 + static_cast<int>(rng.NextU32() % 3u);
        for (int bar = 0; bar < 2; ++bar) {
            const Chord& c = kChords[kProg[pi][bar]];
            const int b = bar * 16;
            // Драйвовый бас восьмыми на треугольнике.
            for (int i = 0; i < 16; ++i) {
                const int n = (i & 1) ? c.root + 12 : c.root;
                PutNote(p, b + i, 2, n, (i & 3) == 0 ? 15 : 12);
            }
            // Неутихающее арпеджио шестнадцатыми.
            for (int i = 0; i < 32; ++i) PutNote(p, b + i, 1, c.arp[(i / 2) & 3], 9);
            AddDrums(p, 3, bar, (pi >= 3 && bar == 1) ? 2 : 0, hatVol);
            PutNote(p, b + 6, 3, 15, 11, ChipFxNoteCut, 2);
            PutNote(p, b + 14, 3, 12, 12, ChipFxNoteCut, 2);
        }
        PutMelody(p, 0, kLeads[pi], kLeadCounts[pi], 14);
        s.AddPattern(p);
        s.order.push_back(pi);
    }
    return s;
}

// "dance": бочка на каждую долю, бас между долями, арпеджио-хук (125 BPM).
ChipSong BuildDance(u64 seed) {
    Random rng(seed);
    ChipSong s;
    s.title = "Circuit Bloom";
    s.chip = ChipType::Nes2A03;
    s.channelCount = 4;
    s.framesPerSecond = 50;
    s.rowsPerBeat = 4;
    s.ticksPerRow = 6;                                        // 125 BPM
    s.beatsPerMinute = s.framesPerSecond * 60 / (s.ticksPerRow * s.rowsPerBeat);
    s.loopOrder = 0;
    s.channels = {
        MakeChannel(ChipWave::Pulse50, 13, 0, 2, 11, 3),      // хук
        MakeChannel(ChipWave::Pulse25, 10, 0, 3, 8, 4),       // аккорды
        MakeChannel(ChipWave::Triangle, 15, 0, 0, 15, 1),     // бочка + бас
        MakeChannel(ChipWave::Noise, 12, 0, 3, 0, 2),         // хэты + хлопки
    };

    static const Chord kChords[4] = {
        /* Am */ {33, 40, {57, 60, 64, 60}, {0, 0, 0}},
        /* F  */ {29, 36, {53, 57, 60, 57}, {0, 0, 0}},
        /* C  */ {36, 43, {60, 64, 67, 64}, {0, 0, 0}},
        /* G  */ {31, 38, {55, 59, 62, 59}, {0, 0, 0}},
    };
    static const int kProg[6][2] = {{0, 0}, {0, 1}, {2, 3}, {0, 1}, {2, 3}, {0, 3}};

    static const Mel kHook0[] = {
        {0, 81, 2}, {2, 76, 2}, {4, 72, 2}, {6, 76, 2}, {8, 81, 2},
        {10, 84, 2}, {12, 81, 2}, {14, 76, 2},
    };
    static const Mel kHook1[] = {
        {0, 81, 2}, {2, 76, 2}, {4, 72, 2}, {6, 76, 2}, {8, 84, 2},
        {10, 81, 2}, {12, 79, 2}, {14, 76, 2}, {16, 77, 2}, {18, 81, 2},
        {20, 84, 2}, {22, 81, 2},
    };
    static const Mel kHook2[] = {
        {0, 84, 2}, {2, 79, 2}, {4, 76, 2}, {6, 79, 2}, {8, 84, 2},
        {10, 88, 2}, {12, 84, 2}, {14, 79, 2}, {16, 83, 2}, {18, 79, 2},
        {20, 74, 2}, {22, 79, 2},
    };
    static const Mel kHook3[] = {
        {0, 81, 2}, {2, 77, 2}, {4, 72, 2}, {6, 77, 2}, {8, 81, 2},
        {10, 86, 2}, {12, 81, 2}, {14, 77, 2},
    };
    static const Mel kHook4[] = {
        {0, 79, 2}, {2, 83, 2}, {4, 86, 2}, {6, 83, 2}, {8, 79, 2},
        {10, 74, 2}, {12, 79, 2}, {14, 83, 2}, {16, 84, 2}, {18, 81, 2},
        {20, 76, 2}, {22, 72, 2},
    };
    static const Mel kHook5[] = {
        {0, 81, 4}, {6, 79, 2}, {8, 76, 4}, {14, 72, 2},
        {16, 74, 4}, {22, 76, 2}, {24, 79, 6}, {30, 81, 1},
    };
    static const Mel* kHooks[6] = {kHook0, kHook1, kHook2, kHook3, kHook4, kHook5};
    static const int kHookCounts[6] = {8, 12, 12, 8, 12, 8};

    static const char* kNames[6] = {"Pulse", "Bloom", "Drive", "Glow", "Surge", "Apex"};
    for (int pi = 0; pi < 6; ++pi) {
        ChipPattern p = NewPattern(kNames[pi], 32, 4);
        const int hatVol = 5 + static_cast<int>(rng.NextU32() % 3u);
        for (int bar = 0; bar < 2; ++bar) {
            const Chord& c = kChords[kProg[pi][bar]];
            const int b = bar * 16;
            // Глухой удар на каждую долю на треугольнике, бас между долями.
            for (int beat = 0; beat < 4; ++beat) {
                PutNote(p, b + beat * 4, 2, 14, 15, ChipFxNoteCut, 2);
                PutNote(p, b + beat * 4 + 2, 2, c.root + 12, 12);
            }
            for (int i = 0; i < 8; ++i)
                PutNote(p, b + i * 2, 1, c.arp[i & 3], 9);
            // Хэты на каждую слабую восьмую, хлопки на 2 и 4.
            for (int i = 0; i < 8; ++i)
                PutNote(p, b + i * 2 + 1, 3, 2, hatVol, ChipFxNoteCut, 1);
            PutNote(p, b + 4, 3, 10, 12);
            PutNote(p, b + 12, 3, 10, 12);
            if (rng.Chance(0.3f)) PutNote(p, b + 14, 3, 13, 9, ChipFxNoteCut, 2);
            if (pi == 5 && bar == 1) {
                PutNote(p, b + 12, 3, 12, 13);
                PutNote(p, b + 13, 3, 10, 13);
                PutNote(p, b + 14, 3, 8, 14);
                PutNote(p, b + 15, 3, 6, 15);
            }
        }
        PutMelody(p, 0, kHooks[pi], kHookCounts[pi], 14);
        s.AddPattern(p);
        s.order.push_back(pi);
    }
    return s;
}

// "title": медленно, мелодично, до мажор.
ChipSong BuildTitle(u64 seed) {
    Random rng(seed);
    ChipSong s;
    s.title = "Quiet Horizon";
    s.chip = ChipType::Nes2A03;
    s.channelCount = 4;
    s.framesPerSecond = 60;
    s.rowsPerBeat = 4;
    s.ticksPerRow = 10;                                       // 90 BPM
    s.beatsPerMinute = s.framesPerSecond * 60 / (s.ticksPerRow * s.rowsPerBeat);
    s.loopOrder = 0;
    s.channels = {
        MakeChannel(ChipWave::Pulse50, 12, 1, 3, 12, 8),
        MakeChannel(ChipWave::Pulse25, 8, 1, 5, 8, 9),
        MakeChannel(ChipWave::Triangle, 15, 0, 0, 15, 3),
        MakeChannel(ChipWave::Noise, 9, 0, 5, 0, 4),
    };

    static const Chord kChords[6] = {
        /* C  */ {24, 31, {60, 64, 67, 64}, {0, 0, 0}},
        /* Am */ {33, 40, {57, 60, 64, 60}, {0, 0, 0}},
        /* F  */ {29, 36, {53, 57, 60, 57}, {0, 0, 0}},
        /* G  */ {31, 38, {55, 59, 62, 59}, {0, 0, 0}},
        /* Dm */ {26, 33, {50, 53, 57, 53}, {0, 0, 0}},
        /* Em */ {28, 35, {52, 55, 59, 55}, {0, 0, 0}},
    };
    static const int kProg[6][2] = {{0, 1}, {2, 3}, {0, 4}, {5, 2}, {3, 0}, {1, 3}};

    static const Mel kLead0[] = {
        {0, 72, 8}, {8, 76, 8}, {16, 79, 12}, {28, 76, 4},
    };
    static const Mel kLead1[] = {
        {0, 77, 8}, {8, 76, 6}, {16, 72, 8}, {24, 74, 8},
    };
    static const Mel kLead2[] = {
        {0, 76, 6}, {8, 79, 6}, {16, 72, 8}, {24, 74, 8},
    };
    static const Mel kLead3[] = {
        {0, 74, 8}, {8, 72, 8}, {16, 69, 6}, {24, 72, 8},
    };
    static const Mel kLead4[] = {
        {0, 81, 8}, {8, 79, 6}, {16, 77, 8}, {24, 76, 8},
    };
    static const Mel kLead5[] = {
        {0, 79, 8}, {8, 76, 6}, {16, 72, 8}, {26, 74, 6},
    };
    static const Mel* kLeads[6] = {kLead0, kLead1, kLead2, kLead3, kLead4, kLead5};

    static const char* kNames[6] = {"Dawn", "Fields", "Rain", "Hearth", "Stars", "Home"};
    for (int pi = 0; pi < 6; ++pi) {
        ChipPattern p = NewPattern(kNames[pi], 32, 4);
        const int hatVol = 3 + static_cast<int>(rng.NextU32() % 2u);
        for (int bar = 0; bar < 2; ++bar) {
            const Chord& c = kChords[kProg[pi][bar]];
            const int b = bar * 16;
            PutNote(p, b + 0, 2, c.root, 15);
            PutNote(p, b + 8, 2, c.fifth, 12);
            PutNote(p, b + 12, 2, c.root + 12, 10);
            for (int i = 0; i < 4; ++i) PutNote(p, b + i * 4, 1, c.arp[i], 8);
            // Редкая перкуссия: мягкая бочка на 1, щётка на 3.
            PutNote(p, b + 0, 3, 15, 8, ChipFxNoteCut, 3);
            PutNote(p, b + 8, 3, 9, 6, ChipFxNoteCut, 4);
            if (bar == 1) PutNote(p, b + 14, 3, 2, hatVol, ChipFxNoteCut, 2);
        }
        PutMelody(p, 0, kLeads[pi], 4, 12);
        s.AddPattern(p);
        s.order.push_back(pi);
    }
    return s;
}

}  // namespace

ChipSong ChipPlayer::Make8BitSong(u64 seed) { return Build8Bit(seed ? seed : 1); }
ChipSong ChipPlayer::Make16BitSong(u64 seed) { return Build16Bit(seed ? seed : 2); }
ChipSong ChipPlayer::MakeBossSong(u64 seed) { return BuildBoss(seed ? seed : 3); }
ChipSong ChipPlayer::MakeDanceSong(u64 seed) { return BuildDance(seed ? seed : 4); }
ChipSong ChipPlayer::MakeTitleSong(u64 seed) { return BuildTitle(seed ? seed : 5); }

std::vector<std::string> ChipPlayer::BuiltinNames() {
    return {"8bit", "16bit", "boss", "dance", "title"};
}

ChipSong ChipPlayer::MakeNamed(const std::string& name, u64 seed) {
    std::string lower;
    lower.reserve(name.size());
    for (char c : name)
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (lower == "16bit" || lower == "16-bit" || lower == "snes" || lower == "spc")
        return Make16BitSong(seed ? seed : 2);
    if (lower == "boss") return MakeBossSong(seed ? seed : 3);
    if (lower == "dance") return MakeDanceSong(seed ? seed : 4);
    if (lower == "title") return MakeTitleSong(seed ? seed : 5);
    return Make8BitSong(seed ? seed : 1);
}

// ---------------------------------------------------------------------------
// Сериализация
// ---------------------------------------------------------------------------
namespace {

const char* ChipTypeName(ChipType t) {
    switch (t) {
        case ChipType::Nes2A03: return "Nes2A03";
        case ChipType::GameBoy: return "GameBoy";
        case ChipType::SnesSpc: return "SnesSpc";
        case ChipType::SegaYM: return "SegaYM";
        case ChipType::PcSpeaker: return "PcSpeaker";
        default: return "Nes2A03";
    }
}

bool ParseChipType(const std::string& s, ChipType* out) {
    if (s == "Nes2A03" || s == "nes" || s == "nes2a03") *out = ChipType::Nes2A03;
    else if (s == "GameBoy" || s == "gb" || s == "gameboy") *out = ChipType::GameBoy;
    else if (s == "SnesSpc" || s == "spc" || s == "snes") *out = ChipType::SnesSpc;
    else if (s == "SegaYM" || s == "ym" || s == "sega") *out = ChipType::SegaYM;
    else if (s == "PcSpeaker" || s == "pc" || s == "beep") *out = ChipType::PcSpeaker;
    else return false;
    return true;
}

const char* ChipWaveName(ChipWave w) {
    switch (w) {
        case ChipWave::Pulse12: return "Pulse12";
        case ChipWave::Pulse25: return "Pulse25";
        case ChipWave::Pulse50: return "Pulse50";
        case ChipWave::Pulse75: return "Pulse75";
        case ChipWave::Triangle: return "Triangle";
        case ChipWave::Saw: return "Saw";
        case ChipWave::Sine: return "Sine";
        case ChipWave::Square: return "Square";
        case ChipWave::Noise: return "Noise";
        case ChipWave::Sample: return "Sample";
        default: return "Pulse50";
    }
}

bool ParseChipWave(const std::string& s, ChipWave* out) {
    if (s == "Pulse12") *out = ChipWave::Pulse12;
    else if (s == "Pulse25") *out = ChipWave::Pulse25;
    else if (s == "Pulse50") *out = ChipWave::Pulse50;
    else if (s == "Pulse75") *out = ChipWave::Pulse75;
    else if (s == "Triangle") *out = ChipWave::Triangle;
    else if (s == "Saw") *out = ChipWave::Saw;
    else if (s == "Sine") *out = ChipWave::Sine;
    else if (s == "Square") *out = ChipWave::Square;
    else if (s == "Noise") *out = ChipWave::Noise;
    else if (s == "Sample") *out = ChipWave::Sample;
    else return false;
    return true;
}

std::string Sanitize(const std::string& s, char bad, char rep) {
    std::string out = s;
    for (char& c : out) {
        if (c == bad || c == '\n' || c == '\r' || c == '=' || c == '"') c = rep;
    }
    return out;
}

bool ParseInt(const std::string& s, int* out) {
    if (s.empty()) return false;
    usize i = 0;
    bool neg = false;
    if (s[0] == '-' || s[0] == '+') {
        neg = s[0] == '-';
        i = 1;
    }
    if (i >= s.size()) return false;
    long v = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (s[i] - '0');
        if (v > 100000000L) return false;
    }
    *out = static_cast<int>(neg ? -v : v);
    return true;
}

std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

std::string Trim(const std::string& s) {
    usize a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

bool ParseNoteName(const std::string& s, int* out) {
    if (s == "---") {
        *out = -1;
        return true;
    }
    if (s == "OFF" || s == "off") {
        *out = -2;
        return true;
    }
    if (s.size() < 3) return false;
    int base = -1;
    switch (s[0]) {
        case 'C': base = 0; break;
        case 'D': base = 2; break;
        case 'E': base = 4; break;
        case 'F': base = 5; break;
        case 'G': base = 7; break;
        case 'A': base = 9; break;
        case 'B': base = 11; break;
        default: return false;
    }
    if (s[1] == '#') base += 1;
    else if (s[1] != '-') return false;
    int octave = 0;
    if (!ParseInt(s.substr(2), &octave)) return false;
    const int semi = octave * 12 + base;
    if (semi < -2 || semi > 127) return false;
    *out = semi;
    return true;
}

std::string FormatCell(const ChipNote& n) {
    if (n.semitone == -1 && n.instrument == 0 && n.volume == 0xFF && n.effect == 0 &&
        n.param == 0 && n.arp == 0xFF)
        return "...";
    std::string out = ChipPlayer::NoteName(n.semitone);
    out += ',';
    out += std::to_string(n.instrument);
    out += ',';
    out += std::to_string(n.volume);
    out += ',';
    out += std::to_string(n.effect);
    out += ',';
    out += std::to_string(n.param);
    out += ',';
    out += std::to_string(n.arp);
    return out;
}

bool ParseCell(const std::string& text, ChipNote* out, std::string* error) {
    if (text == "...") {
        *out = ChipNote{};
        return true;
    }
    std::vector<std::string> f = Split(text, ',');
    if (f.size() != 6) {
        if (error) *error = "row cell needs 6 comma separated fields: '" + text + "'";
        return false;
    }
    int semi = -1, inst = 0, vol = 255, fx = 0, param = 0, arp = 255;
    if (!ParseNoteName(f[0], &semi)) {
        if (error) *error = "bad note name '" + f[0] + "'";
        return false;
    }
    if (!ParseInt(f[1], &inst) || !ParseInt(f[2], &vol) || !ParseInt(f[3], &fx) ||
        !ParseInt(f[4], &param) || !ParseInt(f[5], &arp)) {
        if (error) *error = "bad numeric cell field in '" + text + "'";
        return false;
    }
    if (inst < 0 || inst > 255 || vol < 0 || vol > 255 || fx < 0 || fx > 255 || param < 0 ||
        param > 255 || arp < 0 || arp > 255) {
        if (error) *error = "cell field out of range in '" + text + "'";
        return false;
    }
    out->semitone = static_cast<i16>(semi);
    out->instrument = static_cast<u8>(inst);
    out->volume = static_cast<u8>(vol);
    out->effect = static_cast<u8>(fx);
    out->param = static_cast<u8>(param);
    out->arp = static_cast<u8>(arp);
    return true;
}

}  // namespace

std::string ChipPlayer::Serialize(const ChipSong& song) {
    std::string out;
    out += "#GE-CHIPTUNE 1\n";
    out += "title=" + Sanitize(song.title, ' ', ' ') + "\n";
    out += "author=" + Sanitize(song.author, ' ', ' ') + "\n";
    out += std::string("chip=") + ChipTypeName(song.chip) + "\n";
    out += "channels=" + std::to_string(song.channels.size()) + "\n";
    out += "speed=" + std::to_string(song.ticksPerRow) + "\n";
    out += "fps=" + std::to_string(song.framesPerSecond) + "\n";
    out += "rowsPerBeat=" + std::to_string(song.rowsPerBeat) + "\n";
    out += "bpm=" + std::to_string(song.beatsPerMinute) + "\n";
    out += "loopOrder=" + std::to_string(song.loopOrder) + "\n";
    out += std::string("echoEnabled=") + (song.echoEnabled ? "1" : "0") + "\n";
    out += "echoDelayMs=" + std::to_string(song.echoDelayMs) + "\n";
    out += "echoFeedback=" + std::to_string(song.echoFeedback) + "\n";
    out += "echoVolume=" + std::to_string(song.echoVolume) + "\n";
    out += std::string("lowPass=") + (song.lowPass ? "1" : "0") + "\n";
    out += "lowPassCutoff=" + std::to_string(song.lowPassCutoff) + "\n";
    for (usize i = 0; i < song.channels.size(); ++i) {
        const ChipChannelDef& c = song.channels[i];
        out += "channel";
        out += " wave=" + std::string(ChipWaveName(c.wave));
        out += " volume=" + std::to_string(c.volume);
        out += " dutySweep=" + std::to_string(c.dutySweep);
        out += " pitchSweep=" + std::to_string(c.pitchSweep);
        out += " arpeggio=" + std::to_string(c.arpeggio ? 1 : 0);
        out += " echoVolume=" + std::to_string(c.echoVolume);
        out += " attack=" + std::to_string(c.attack);
        out += " decay=" + std::to_string(c.decay);
        out += " sustain=" + std::to_string(c.sustain);
        out += " release=" + std::to_string(c.release);
        out += " pan=" + std::to_string(c.pan);
        out += " detune=" + std::to_string(c.detune);
        out += "\n";
    }
    for (usize pi = 0; pi < song.patterns.size(); ++pi) {
        const ChipPattern& p = song.patterns[pi];
        out += "pattern=" + Sanitize(p.name, ',', '_') + "," + std::to_string(p.rowCount) + "," +
               std::to_string(p.channelCount) + "\n";
        for (int r = 0; r < p.rowCount; ++r) {
            out += "row";
            for (int ch = 0; ch < p.channelCount; ++ch) {
                out += ' ';
                out += FormatCell(p.At(r, ch));
            }
            out += "\n";
        }
    }
    out += "order=";
    for (usize i = 0; i < song.order.size(); ++i) {
        if (i) out += ',';
        out += std::to_string(song.order[i]);
    }
    out += "\n";
    return out;
}

bool ChipPlayer::Deserialize(const std::string& text, ChipSong* out, std::string* error) {
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };
    if (error) error->clear();
    if (!out) return fail("chiptune: no output song");
    *out = ChipSong{};

    // Разбиваем на строки.
    std::vector<std::string> lines;
    {
        std::string cur;
        for (char c : text) {
            if (c == '\n') {
                lines.push_back(cur);
                cur.clear();
            } else if (c != '\r') {
                cur.push_back(c);
            }
        }
        if (!cur.empty()) lines.push_back(cur);
    }
    if (lines.empty()) return fail("chiptune: empty text");

    usize li = 0;
    {
        const std::string first = Trim(lines[0]);
        if (first.rfind("#GE-CHIPTUNE", 0) != 0)
            return fail("chiptune: missing #GE-CHIPTUNE header");
        ++li;
    }

    int declaredChannels = -1;
    std::vector<int> pendingPatterns;

    for (; li < lines.size(); ++li) {
        const std::string line = Trim(lines[li]);
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("row", 0) == 0 &&
            (line.size() == 3 || line[3] == ' ')) {
            // Строка row вне паттерна.
            if (pendingPatterns.empty()) continue;
            ChipPattern& p = out->patterns.back();
            const int idx = pendingPatterns.back();
            const std::string rest = Trim(line.substr(3));
            std::vector<std::string> cells;
            {
                std::string cur;
                for (char c : rest) {
                    if (c == ' ' || c == '\t') {
                        if (!cur.empty()) {
                            cells.push_back(cur);
                            cur.clear();
                        }
                    } else {
                        cur.push_back(c);
                    }
                }
                if (!cur.empty()) cells.push_back(cur);
            }
            if (static_cast<int>(cells.size()) != p.channelCount)
                return fail("chiptune: row " + std::to_string(idx) + " has " +
                            std::to_string(cells.size()) + " cells, expected " +
                            std::to_string(p.channelCount));
            if (idx < 0 || idx >= p.rowCount) return fail("chiptune: too many rows");
            for (int ch = 0; ch < p.channelCount; ++ch) {
                ChipNote n;
                if (!ParseCell(cells[static_cast<usize>(ch)], &n, error)) return false;
                p.At(idx, ch) = n;
            }
            pendingPatterns.back() = idx + 1;
            continue;
        }

        const usize eq = line.find('=');
        if (eq == std::string::npos) return fail("chiptune: malformed line '" + line + "'");
        const std::string key = Trim(line.substr(0, eq));
        const std::string val = Trim(line.substr(eq + 1));
        int iv = 0;

        if (line.rfind("channel", 0) == 0 && (line.size() == 7 || line[7] == ' ')) {
            ChipChannelDef c;
            std::vector<std::string> toks;
            {
                std::string cur;
                for (usize k = 7; k < line.size(); ++k) {
                    const char ch2 = line[k];
                    if (ch2 == ' ' || ch2 == '\t') {
                        if (!cur.empty()) {
                            toks.push_back(cur);
                            cur.clear();
                        }
                    } else {
                        cur.push_back(ch2);
                    }
                }
                if (!cur.empty()) toks.push_back(cur);
            }
            for (const std::string& t : toks) {
                const usize e2 = t.find('=');
                if (e2 == std::string::npos) return fail("chiptune: bad channel token '" + t + "'");
                const std::string k2 = t.substr(0, e2);
                const std::string v2 = t.substr(e2 + 1);
                int x = 0;
                if (k2 == "wave") {
                    ChipWave w = ChipWave::Pulse50;
                    if (!ParseChipWave(v2, &w)) return fail("chiptune: unknown wave '" + v2 + "'");
                    c.wave = w;
                } else if (k2 == "volume") {
                    if (!ParseInt(v2, &x) || x < 0 || x > 15) return fail("chiptune: bad volume");
                    c.volume = x;
                } else if (k2 == "dutySweep") {
                    if (!ParseInt(v2, &x) || x < -1000 || x > 1000)
                        return fail("chiptune: bad dutySweep");
                    c.dutySweep = x;
                } else if (k2 == "pitchSweep") {
                    if (!ParseInt(v2, &x) || x < -1000 || x > 1000)
                        return fail("chiptune: bad pitchSweep");
                    c.pitchSweep = x;
                } else if (k2 == "arpeggio") {
                    c.arpeggio = (v2 == "1" || v2 == "true");
                } else if (k2 == "echoVolume") {
                    if (!ParseInt(v2, &x) || x < 0 || x > 15) return fail("chiptune: bad echoVolume");
                    c.echoVolume = x;
                } else if (k2 == "attack") {
                    if (!ParseInt(v2, &x) || x < 0 || x > 15) return fail("chiptune: bad attack");
                    c.attack = x;
                } else if (k2 == "decay") {
                    if (!ParseInt(v2, &x) || x < 0 || x > 15) return fail("chiptune: bad decay");
                    c.decay = x;
                } else if (k2 == "sustain") {
                    if (!ParseInt(v2, &x) || x < 0 || x > 15) return fail("chiptune: bad sustain");
                    c.sustain = x;
                } else if (k2 == "release") {
                    if (!ParseInt(v2, &x) || x < 0 || x > 15) return fail("chiptune: bad release");
                    c.release = x;
                } else if (k2 == "pan") {
                    if (!ParseInt(v2, &x) || x < -8 || x > 8) return fail("chiptune: bad pan");
                    c.pan = x;
                } else if (k2 == "detune") {
                    if (!ParseInt(v2, &x) || x < -1200 || x > 1200)
                        return fail("chiptune: bad detune");
                    c.detune = x;
                } else {
                    ENG_LOGT("chiptune", "ignoring unknown channel key '%s'", k2.c_str());
                }
            }
            if (static_cast<int>(out->channels.size()) >= kMaxChannels)
                return fail("chiptune: too many channels");
            out->channels.push_back(c);
            continue;
        }

        if (key == "title") {
            out->title = val;
        } else if (key == "author") {
            out->author = val;
        } else if (key == "chip") {
            ChipType t = ChipType::Nes2A03;
            if (!ParseChipType(val, &t)) return fail("chiptune: unknown chip '" + val + "'");
            out->chip = t;
        } else if (key == "channels") {
            if (!ParseInt(val, &iv) || iv < 1 || iv > kMaxChannels)
                return fail("chiptune: bad channel count '" + val + "'");
            declaredChannels = iv;
        } else if (key == "speed") {
            if (!ParseInt(val, &iv) || iv < 1 || iv > 64) return fail("chiptune: bad speed");
            out->ticksPerRow = iv;
        } else if (key == "fps") {
            if (!ParseInt(val, &iv) || iv < 1 || iv > 1000) return fail("chiptune: bad fps");
            out->framesPerSecond = iv;
        } else if (key == "rowsPerBeat") {
            if (!ParseInt(val, &iv) || iv < 1 || iv > 64) return fail("chiptune: bad rowsPerBeat");
            out->rowsPerBeat = iv;
        } else if (key == "bpm") {
            if (!ParseInt(val, &iv) || iv < 0 || iv > 1000) return fail("chiptune: bad bpm");
            out->beatsPerMinute = iv;
        } else if (key == "loopOrder") {
            if (!ParseInt(val, &iv) || iv < -1 || iv > kMaxPatterns)
                return fail("chiptune: bad loopOrder");
            out->loopOrder = iv;
        } else if (key == "echoEnabled") {
            out->echoEnabled = val == "1" || val == "true";
        } else if (key == "echoDelayMs") {
            if (!ParseInt(val, &iv) || iv < 1 || iv > 1000) return fail("chiptune: bad echoDelayMs");
            out->echoDelayMs = iv;
        } else if (key == "echoFeedback") {
            if (!ParseInt(val, &iv) || iv < 0 || iv > 255) return fail("chiptune: bad echoFeedback");
            out->echoFeedback = iv;
        } else if (key == "echoVolume") {
            if (!ParseInt(val, &iv) || iv < 0 || iv > 255) return fail("chiptune: bad echoVolume");
            out->echoVolume = iv;
        } else if (key == "lowPass") {
            out->lowPass = val == "1" || val == "true";
        } else if (key == "lowPassCutoff") {
            if (!ParseInt(val, &iv) || iv < 100 || iv > 96000)
                return fail("chiptune: bad lowPassCutoff");
            out->lowPassCutoff = iv;
        } else if (key == "channel") {
            // Строка "channel ..." обрабатывается до общего разбора
            // ключ/значение; попадание сюда означает, что строка некорректна.
            return fail("chiptune: malformed channel line");
        } else if (key == "pattern") {
            std::vector<std::string> f = Split(val, ',');
            if (f.size() != 3) return fail("chiptune: pattern needs name,rows,channels");
            int rows = 0, chans = 0;
            if (!ParseInt(f[1], &rows) || rows < 1 || rows > kMaxRows)
                return fail("chiptune: bad pattern row count '" + f[1] + "'");
            if (!ParseInt(f[2], &chans) || chans < 1 || chans > kMaxChannels)
                return fail("chiptune: bad pattern channel count '" + f[2] + "'");
            if (static_cast<int>(out->patterns.size()) >= kMaxPatterns)
                return fail("chiptune: too many patterns");
            if (!pendingPatterns.empty() && pendingPatterns.back() < out->patterns.back().rowCount)
                return fail("chiptune: pattern ended early");
            ChipPattern p;
            p.name = f[0];
            p.Resize(rows, chans);
            out->patterns.push_back(p);
            pendingPatterns.push_back(0);
        } else if (key == "order") {
            out->order.clear();
            if (!val.empty()) {
                for (const std::string& t : Split(val, ',')) {
                    if (!ParseInt(t, &iv) || iv < 0 || iv >= kMaxPatterns)
                        return fail("chiptune: bad order entry '" + t + "'");
                    out->order.push_back(iv);
                }
            }
        } else {
            ENG_LOGT("chiptune", "ignoring unknown key '%s'", key.c_str());
        }
    }

    if (!pendingPatterns.empty() && pendingPatterns.back() < out->patterns.back().rowCount)
        return fail("chiptune: truncated pattern data");
    if (out->patterns.empty()) return fail("chiptune: no patterns");
    if (out->order.empty()) return fail("chiptune: no order list");
    for (int o : out->order)
        if (o < 0 || o >= static_cast<int>(out->patterns.size()))
            return fail("chiptune: order references a missing pattern");
    if (declaredChannels > 0 && static_cast<int>(out->channels.size()) != declaredChannels)
        return fail("chiptune: channel count does not match the channel list");
    if (out->channels.empty()) {
        out->channels.assign(static_cast<usize>(declaredChannels > 0 ? declaredChannels : 4),
                             ChipChannelDef{});
    }
    out->channelCount = static_cast<int>(out->channels.size());
    if (out->loopOrder >= static_cast<int>(out->order.size())) out->loopOrder = -1;
    if (error) error->clear();
    return true;
}

bool ChipPlayer::SaveToFile(const ChipSong& song, const std::string& path) {
    const std::string text = Serialize(song);
    if (!WriteTextFile(path, text)) {
        ENG_LOGE("chiptune", "SaveToFile failed: %s", path.c_str());
        return false;
    }
    return true;
}

bool ChipPlayer::LoadFromFile(const std::string& path, ChipSong* out, std::string* error) {
    const std::string text = ReadTextFile(path);
    if (text.empty()) {
        if (error) *error = "chiptune: cannot read '" + path + "'";
        return false;
    }
    return Deserialize(text, out, error);
}

// ---------------------------------------------------------------------------
// MakeSingleCycleWaveform (публичная)
// ---------------------------------------------------------------------------
std::vector<f32> MakeSingleCycleWaveform(ChipWave wave, int samples, f32 phase) {
    if (samples <= 0) return {};
    return BuildWaveform(wave, samples, phase);
}

}  // namespace crossrender
