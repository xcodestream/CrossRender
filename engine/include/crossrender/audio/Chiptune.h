//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: эмуляция 8- и 16-битных звуковых чипов: синтез, трекер и плеер ChipPlayer.
//
#pragma once

#include "crossrender/core/Base.h"
#include "crossrender/core/Math.h"
#include "crossrender/audio/Audio.h"

#include <memory>
#include <string>
#include <vector>

namespace crossrender {

// ---------------------------------------------------------------------------
// Определения чипов
// ---------------------------------------------------------------------------
enum class ChipType : u8 {
    Nes2A03,    // 2 pulse + triangle + noise + DMC (8 бит)
    GameBoy,    // 2 pulse + wave + noise (8 бит)
    SnesSpc,    // 8 сэмпл-голосов + эхо (16 бит)
    SegaYM,     // 6 голосов, близких к FM (16-битный характер)
    PcSpeaker,  // 1-битный пищик (тот самый "8 бит")
    Count,
};

enum class ChipWave : u8 {
    Pulse12, Pulse25, Pulse50, Pulse75,
    Triangle, Saw, Sine, Square,
    Noise, Sample,
    Count,
};

// К какому чипу относится канал.
struct ChipChannelDef {
    ChipWave wave = ChipWave::Pulse50;
    int volume = 12;        // 0..15 аппаратная громкость
    int dutySweep = 0;      // LFO скважности pulse в шагах/сек (0 = выкл)
    int pitchSweep = 0;     // знаковый питч-бенд на строку (NES sweep)
    bool arpeggio = false;  // циклически перебирает приму/терцию/квинту каждый фрейм
    int echoVolume = 0;     // 0..15 (только SPC)
    int attack = 0, decay = 0, sustain = 12, release = 0;  // 0..15 (SPC/сэмпл)
    int pan = 0;            // -8..8
    int detune = 0;         // центы
};

// ---------------------------------------------------------------------------
// Данные трекера
// ---------------------------------------------------------------------------
struct ChipNote {
    i16 semitone = -1;   // -1 = нет ноты, -2 = note off, 0 = C-0
    u8 instrument = 0;
    u8 volume = 0xFF;    // 0xFF = значение по умолчанию у инструмента
    u8 effect = 0;       // см. ChipEffect
    u8 param = 0;
    u8 arp = 0xFF;       // смещение арпеджио в полутонах (0xFF = нет)
};

enum ChipEffect : u8 {
    ChipFxNone = 0,
    ChipFxArpeggio,      // param = смещения в полутонах, упакованные (полубайты x, y)
    ChipFxSlideUp,
    ChipFxSlideDown,
    ChipFxPortamento,
    ChipFxVibrato,
    ChipFxVolumeSlide,
    ChipFxDutyCycle,
    ChipFxJump,          // param = индекс в order
    ChipFxSpeed,         // param = тиков на строку
    ChipFxNoteCut,
    ChipFxRetrigger,
    ChipFxEcho,
    ChipFxDetune,
};

struct ChipPattern {
    std::string name;
    // строки x каналы
    std::vector<ChipNote> rows;
    int rowCount = 0;
    int channelCount = 0;
    [[nodiscard]] const ChipNote& At(int row, int channel) const {
        return rows[static_cast<usize>(row) * static_cast<usize>(channelCount) + static_cast<usize>(channel)];
    }
    ChipNote& At(int row, int channel) {
        return rows[static_cast<usize>(row) * static_cast<usize>(channelCount) + static_cast<usize>(channel)];
    }
    void Resize(int rows_, int channels_) {
        rowCount = rows_;
        channelCount = channels_;
        rows.assign(static_cast<usize>(rows_) * static_cast<usize>(channels_), ChipNote{});
    }
};

// Песня: инструменты, паттерны, список order и настройки чипа по каналам.
struct ChipSong {
    std::string title;
    std::string author = "CrossRender";
    ChipType chip = ChipType::Nes2A03;
    int channelCount = 4;
    int ticksPerRow = 6;         // скорость: строки продвигаются каждые N тиков (фреймов)
    int framesPerSecond = 60;    // частота тиков трекера
    int rowsPerBeat = 4;
    int beatsPerMinute = 125;
    int loopOrder = 0;           // индекс order для зацикливания (-1 = без цикла)
    std::vector<ChipChannelDef> channels;
    std::vector<ChipPattern> patterns;
    std::vector<int> order;      // индексы паттернов, проигрываемые по порядку
    // только для 16 бит
    bool echoEnabled = false;
    int echoDelayMs = 120;
    int echoFeedback = 90;       // 0..255
    int echoVolume = 60;         // 0..255
    bool lowPass = false;
    int lowPassCutoff = 8000;    // Гц

    [[nodiscard]] int PatternCount() const { return static_cast<int>(patterns.size()); }
    [[nodiscard]] f32 DurationSeconds(int orderIndex = -1) const;
    [[nodiscard]] int TotalRows() const;
    void AddPattern(const ChipPattern& p) { patterns.push_back(p); }
};

// ---------------------------------------------------------------------------
// ChipPlayer: рендерит песню в PCM в реальном времени или офлайн
// ---------------------------------------------------------------------------
class ChipPlayer {
public:
    ChipPlayer();
    ~ChipPlayer();
    ChipPlayer(const ChipPlayer&) = delete;
    ChipPlayer& operator=(const ChipPlayer&) = delete;

    void SetSong(const ChipSong& song);
    [[nodiscard]] const ChipSong& Song() const;
    [[nodiscard]] bool HasSong() const { return song_ != nullptr; }

    void Play();
    void Pause();
    void Stop();
    void SetLoop(bool loop);
    [[nodiscard]] bool IsPlaying() const;
    void SetSpeed(int ticksPerRow);
    void SetOrder(int orderIndex);
    void SetChannelMute(int channel, bool muted);
    [[nodiscard]] bool ChannelMuted(int channel) const;
    void SetMasterVolume(f32 v);
    void SetSampleRate(int hz);

    // Продвигает секвенсор и смешивает `frameCount` стереофреймов в `out`.
    // `out` — стерео-float с чередованием каналов, к нему *добавляется* (не перезаписывается).
    void RenderAdd(f32* out, int frameCount);
    // Продвигает секвенсор на `dt` секунд без генерации звука (используется
    // экранным трекером, чтобы UI оставался синхронным в silent/headless-режиме).
    void Advance(f32 dt);
    // Колбэк на каждый тик, срабатывает при запуске строки (для подсветки в UI).
    using RowCallback = void (*)(int order, int row, void* user);
    void SetRowCallback(RowCallback cb, void* user) {
        rowCb_ = cb;
        rowUser_ = user;
    }

    // ---- офлайн-рендеринг -------------------------------------------------
    // Рендерит всю песню (или `seconds` секунд, если > 0) в клип, который можно
    // проиграть через crossrender::Audio. `loopFadeSeconds` делает кроссфейд хвоста,
    // чтобы клип зацикливался бесшовно.
    AudioClip Bake(int sampleRate = 44100, f32 seconds = 0.0f, f32 loopFadeSeconds = 0.05f,
                   int maxSeconds = 240) const;
    // Рендерит `frames` стереофреймов, начиная с текущей позиции.
    void RenderTo(std::vector<f32>* out, int frames);

    // ---- live-состояние для UI --------------------------------------------
    struct ChannelState {
        bool active = false;
        int note = -1;
        f32 frequency = 0;
        int volume = 0;
        f32 phase = 0;
        int effect = 0;
        int arpStep = 0;
        bool muted = false;
        std::string lastNoteName;
    };
    [[nodiscard]] const std::vector<ChannelState>& ChannelStates() const;
    [[nodiscard]] int CurrentOrder() const;
    [[nodiscard]] int CurrentRow() const;
    [[nodiscard]] int CurrentTick() const;
    [[nodiscard]] f32 CurrentTime() const;
    [[nodiscard]] u64 RenderedFrames() const;
    // Пиковый уровень последнего отрисованного блока (для VU-метров).
    [[nodiscard]] f32 LastPeak() const;

    // ---- помощники построения песни ---------------------------------------
    // Детерминированные готовые к воспроизведению песни для примера и тестов.
    static ChipSong Make8BitSong(u64 seed = 1);
    static ChipSong Make16BitSong(u64 seed = 2);
    static ChipSong MakeBossSong(u64 seed = 3);
    static ChipSong MakeDanceSong(u64 seed = 4);
    static ChipSong MakeTitleSong(u64 seed = 5);
    // Имена встроенных песен: "8bit", "16bit", "boss", "dance", "title".
    static ChipSong MakeNamed(const std::string& name, u64 seed = 0);
    static std::vector<std::string> BuiltinNames();
    // Кодирует/декодирует компактный текстовый формат трекера (одна строка на ряд).
    static std::string Serialize(const ChipSong& song);
    static bool Deserialize(const std::string& text, ChipSong* out, std::string* error = nullptr);
    // Записывает/читает тот же формат через файловую систему движка.
    static bool SaveToFile(const ChipSong& song, const std::string& path);
    static bool LoadFromFile(const std::string& path, ChipSong* out, std::string* error = nullptr);

    // Помощники имён нот ("C-4", "A#3").
    static std::string NoteName(int semitone);
    static f32 NoteFrequency(int semitone);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    std::shared_ptr<const ChipSong> song_;
    RowCallback rowCb_ = nullptr;
    void* rowUser_ = nullptr;
};

// Генератор волны из одного цикла, используется 16-битными (сэмпловыми) каналами
// и отображением формы волны в демо.
std::vector<f32> MakeSingleCycleWaveform(ChipWave wave, int samples, f32 phase = 0.0f);

}  // namespace crossrender
