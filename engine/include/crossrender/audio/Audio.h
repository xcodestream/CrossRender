//
// CrossRender
// Автор: xcodestream <xcodestream@gmail.com>
// Лицензия: MIT
// Дата: 15.05.2026
// Описание: звуковой движок: декодирование WAV/OGG/MP3 в PCM, программный микшер и 3D-звук.
//
#pragma once

#include "crossrender/core/Base.h"
#include "crossrender/core/Math.h"

#include <memory>
#include <string>
#include <vector>

namespace crossrender {

enum class AudioFormat : u8 { Unknown, Wav, Ogg, Mp3 };

// Декодированный неизменяемый PCM-буфер (16 бит или float, с чередованием каналов).
class AudioClip {
public:
    AudioClip() = default;
    ~AudioClip();
    AudioClip(AudioClip&&) noexcept;
    AudioClip& operator=(AudioClip&&) noexcept;
    AudioClip(const AudioClip&) = delete;
    AudioClip& operator=(const AudioClip&) = delete;

    bool LoadFromFile(const std::string& path);
    bool LoadFromMemory(const void* data, usize size, AudioFormat hint = AudioFormat::Unknown);
    void Destroy();

    [[nodiscard]] bool Valid() const { return !samples_.empty(); }
    [[nodiscard]] const std::vector<f32>& Samples() const { return samples_; }
    [[nodiscard]] int Channels() const { return channels_; }
    [[nodiscard]] int SampleRate() const { return sampleRate_; }
    [[nodiscard]] f32 Duration() const {
        return sampleRate_ > 0 ? static_cast<f32>(frameCount()) / static_cast<f32>(sampleRate_) : 0.0f;
    }
    [[nodiscard]] usize frameCount() const {
        return channels_ > 0 ? samples_.size() / static_cast<usize>(channels_) : 0;
    }
    [[nodiscard]] const std::string& SourcePath() const { return path_; }
    [[nodiscard]] AudioFormat Format() const { return format_; }
    // Метаданные области зацикливания из WAV-чанка `smpl` (0 при отсутствии).
    [[nodiscard]] u32 LoopStart() const { return loopStart_; }
    [[nodiscard]] u32 LoopEnd() const { return loopEnd_; }

    // Декодеры (статические, также вызываются тестами напрямую).
    static bool DecodeWav(const void* data, usize size, std::vector<f32>* out, int* channels,
                          int* sampleRate, std::string* error);
    static bool DecodeOgg(const void* data, usize size, std::vector<f32>* out, int* channels,
                          int* sampleRate, std::string* error);
    static bool DecodeMp3(const void* data, usize size, std::vector<f32>* out, int* channels,
                          int* sampleRate, std::string* error);
    static AudioFormat DetectFormat(const void* data, usize size);
    // Генерирует процедурный тон (тесты / звук-заглушка без ассетов).
    static AudioClip MakeTone(f32 frequency, f32 durationSeconds, f32 sampleRate = 44100.0f,
                              int channels = 2);
    static AudioClip MakeNoise(f32 durationSeconds, f32 sampleRate = 44100.0f, int channels = 2);
    static AudioClip MakeSilence(f32 durationSeconds, f32 sampleRate = 44100.0f, int channels = 2);

private:
    std::vector<f32> samples_;
    int channels_ = 0, sampleRate_ = 0;
    u32 loopStart_ = 0, loopEnd_ = 0;
    AudioFormat format_ = AudioFormat::Unknown;
    std::string path_;
};

using VoiceId = u32;
constexpr VoiceId kInvalidVoice = 0;

struct PlayParams {
    f32 volume = 1.0f;
    f32 pitch = 1.0f;
    f32 pan = 0.0f;         // -1 влево, +1 вправо (2D)
    bool looping = false;
    bool stream = false;    // длинная музыка: стриминг с диска вместо ОЗУ
    f64 startTimeOffset = 0.0;
    f32 fadeIn = 0.0f;
    // 3D
    bool spatial = false;
    Vec3 position{0, 0, 0};
    f32 minDistance = 1.0f;
    f32 maxDistance = 30.0f;
    int rolloff = 1;  // 0 нет, 1 обратная, 2 линейная, 3 экспоненциальная
    int bus = 0;
};

struct ListenerState {
    Vec3 position{0, 0, 0};
    Vec3 forward{0, 0, -1};
    Vec3 up{0, 1, 0};
    Vec3 velocity{0, 0, 0};
};

// Аудиошина для группового управления громкостью (master / music / sfx / ui).
struct AudioBus {
    std::string name;
    f32 volume = 1.0f;
    bool muted = false;
    int parent = -1;
};

class Audio {
public:
    static Audio& Get();

    // Инициализирует платформенное устройство вывода. Возвращает false, если оно
    // недоступно (микшер всё равно работает в "тихом" режиме, чтобы тесты проходили).
    bool Init(int sampleRate = 44100, int channels = 2, int bufferFrames = 1024);
    void Shutdown();
    [[nodiscard]] bool Initialised() const { return initialised_; }
    [[nodiscard]] bool Silent() const { return silent_; }
    [[nodiscard]] int SampleRate() const { return sampleRate_; }
    [[nodiscard]] int Channels() const { return channels_; }

    VoiceId Play(const AudioClip& clip, const PlayParams& params = {});
    VoiceId PlayMusic(const AudioClip& clip, f32 volume = 1.0f, bool looping = true);
    bool Stop(VoiceId voice, f32 fadeOut = 0.0f);
    void StopAll(f32 fadeOut = 0.0f);
    void StopBus(const std::string& bus, f32 fadeOut = 0.0f);
    void Pause(VoiceId voice);
    void Resume(VoiceId voice);
    void SetVoiceVolume(VoiceId voice, f32 volume);
    void SetVoicePitch(VoiceId voice, f32 pitch);
    void SetVoicePosition(VoiceId voice, const Vec3& position);
    [[nodiscard]] bool IsPlaying(VoiceId voice) const;
    [[nodiscard]] f32 VoiceTime(VoiceId voice) const;

    // Шины
    int AddBus(const std::string& name, int parent = 0);
    void SetBusVolume(const std::string& name, f32 volume);
    void SetBusMuted(const std::string& name, bool muted);
    [[nodiscard]] f32 BusVolume(const std::string& name) const;

    void SetMasterVolume(f32 v) { masterVolume_ = v; }
    [[nodiscard]] f32 MasterVolume() const { return masterVolume_; }
    void SetListener(const ListenerState& l) { listener_ = l; }
    [[nodiscard]] const ListenerState& Listener() const { return listener_; }

    // Смешивает следующие `frameCount` фреймов в `out` (float с чередованием каналов).
    // Платформенный бэкенд вызывает это из аудиопотока; тесты вызывают напрямую
    // для детерминированной проверки.
    void Mix(f32* out, int frameCount);
    // Продвигает микшер без устройства вывода (тихий режим / тесты).
    void Update(f32 dt);
    // Запускает платформенный колбэк микшера один раз (используется тестами).
    void PumpSilent(int frames = 1024);

    [[nodiscard]] int ActiveVoices() const;
    struct Stats {
        int voices = 0;
        int maxVoices = 0;
        f32 cpuLoad = 0;
        u64 mixedFrames = 0;
        int underruns = 0;
    };
    [[nodiscard]] const Stats& GetStats() const { return stats_; }
    // Платформенные хуки, реализованные в папках под каждую ОС.
    static bool PlatformAudioInit(int sampleRate, int channels, int bufferFrames);
    static void PlatformAudioShutdown();
    static bool PlatformAudioAvailable();

    struct Impl;

private:
    Audio();
    ~Audio();
    std::unique_ptr<Impl> impl_;
    int sampleRate_ = 44100;
    int channels_ = 2;
    f32 masterVolume_ = 1.0f;
    bool initialised_ = false;
    bool silent_ = false;
    ListenerState listener_;
    Stats stats_{};
};

}  // namespace crossrender
