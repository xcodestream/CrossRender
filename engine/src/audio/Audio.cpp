// Реализация аудиодвижка: декодирование клипов, программный микшер, шины и
// 3D-пространственная панорама.
//
// Декодирование
//   WAV : собственный RIFF-парсер (PCM 8/16/24/32 бит, IEEE float 32/64,
//         WAVE_FORMAT_EXTENSIBLE и точки лупа `smpl`).
//   OGG : stb_vorbis_decode_memory (engine/third_party/stb_vorbis.c).
//   MP3 : minimp3 (engine/third_party/minimp3*.h, реализация в этой единице трансляции).
//
// Потоки выполнения
//   Любая мутация состояния голоса/шины выполняется под Impl::mutex. Колбэк
//   вывода платформы вызывает Mix() из аудиопотока; Mix() не выделяет память и
//   берёт мьютекс лишь на время одного буфера.
//
// Время жизни
//   Голос ссылается на AudioClip, переданный в Play(); клип должен оставаться
//   живым (и не перемещаться), пока голос не остановится.
// NOTE: crossrender/core/Math.h использует std::vector в Random::Shuffle, но не включает
// <vector>; подключаем его первым, чтобы замороженный публичный заголовок компилировался автономно.
#include <vector>

#include "crossrender/audio/Audio.h"

#include "crossrender/core/Log.h"
#include "crossrender/core/File.h"

#include <cmath>
#include <mutex>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <algorithm>

// Реализация minimp3 живёт здесь (ровно одна единица трансляции определяет её).
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_STDIO
// Vendored-заголовок чинить не наша задача; глушим предупреждения Apple Clang
// 64->32, возникающие внутри него. У других компиляторов такой группы диагностик нет.
#if defined(__APPLE__) && defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wshorten-64-to-32"
#endif
#include "minimp3_ex.h"
#if defined(__APPLE__) && defined(__clang__)
#pragma clang diagnostic pop
#endif

// stb_vorbis компилируется сборкой как C (engine/third_party/stb_vorbis.c).
extern "C" {
int stb_vorbis_decode_memory(const unsigned char* mem, int len, int* channels, int* sample_rate,
                             short** output);
}

namespace crossrender {
namespace {

constexpr int kMaxVoices = 64;
constexpr int kMaxBuses = 32;
constexpr f32 kSoftKnee = 0.75f;
constexpr f64 kTauD = 6.28318530717958647692;

// ---------------------------------------------------------------------------
// Небольшие вспомогательные функции
// ---------------------------------------------------------------------------
inline u16 RdU16(const u8* p) {
    return static_cast<u16>(static_cast<u16>(p[0]) | (static_cast<u16>(p[1]) << 8));
}
inline u32 RdU32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}
inline f32 RdF32(const u8* p) {
    u32 bits = RdU32(p);
    f32 v = 0.0f;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}
inline f64 RdF64(const u8* p) {
    u64 bits = static_cast<u64>(RdU32(p)) | (static_cast<u64>(RdU32(p + 4)) << 32);
    f64 v = 0.0;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}
inline bool FourCC(const u8* p, const char* id) { return std::memcmp(p, id, 4) == 0; }

// Мягкий лимитер: линеен ниже «колена», асимптотически стремится к +/-1 выше
// него, чтобы громкий микс сжимался, а не заворачивался по фазе.
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
    pan = Clamp(pan, -1.0f, 1.0f);
    f32 a = (pan + 1.0f) * 0.25f * kPi;
    *left = std::cos(a);
    *right = std::sin(a);
}

// Громкость как функция расстояния для четырёх поддерживаемых моделей затухания.
inline f32 DistanceGain(int model, f32 dist, f32 minD, f32 maxD) {
    if (minD < 1e-3f) minD = 1e-3f;
    if (maxD <= minD) maxD = minD + 1e-3f;
    if (dist <= minD) return 1.0f;
    if (dist >= maxD) return 0.0f;
    switch (model) {
        case 0:  // нет
            return 1.0f;
        case 2:  // линейный
            return Clamp(1.0f - (dist - minD) / (maxD - minD), 0.0f, 1.0f);
        case 3: {  // экспоненциальный
            f32 r = minD / dist;
            return Clamp(r * r, 0.0f, 1.0f);
        }
        case 1:  // обратный
        default:
            return Clamp(minD / dist, 0.0f, 1.0f);
    }
}

// ---------------------------------------------------------------------------
// Voice
// ---------------------------------------------------------------------------
struct Voice {
    bool active = false;
    bool paused = false;
    u32 generation = 0;  // увеличивается при каждом повторном использовании слота
    u32 serial = 0;      // порядок выделения, используется при вытеснении голосов
    const AudioClip* clip = nullptr;

    f64 playhead = 0.0;       // в фреймах клипа
    f64 playedSeconds = 0.0;  // отрендеренное реальное время, питает VoiceTime()

    f32 volume = 1.0f;
    f32 pitch = 1.0f;
    f32 pan = 0.0f;
    bool looping = false;
    int bus = 0;

    f32 fadeIn = 0.0f;
    f32 fadeInPos = 0.0f;
    bool fadingOut = false;
    f32 fadeOut = 0.0f;
    f32 fadeOutPos = 0.0f;

    bool spatial = false;
    Vec3 position{0, 0, 0};
    f32 minDistance = 1.0f;
    f32 maxDistance = 30.0f;
    int rolloff = 1;
};

inline VoiceId MakeVoiceId(int index, u32 generation) {
    return (static_cast<VoiceId>(generation & 0xFFFFu) << 16) | static_cast<VoiceId>(index + 1);
}

}  // namespace

// ---------------------------------------------------------------------------
// Audio::Impl
// ---------------------------------------------------------------------------
struct Audio::Impl {
    mutable std::mutex mutex;
    std::mutex scratchMutex;  // охраняет только `scratch` (делает PumpSilent реентерабельно-безопасным)
    Voice voices[kMaxVoices];
    std::vector<AudioBus> buses;  // шина 0 всегда мастер-шина
    std::vector<f32> scratch;     // цель сведения в тихом режиме
    int bufferFrames = 1024;
    u32 serialCounter = 0;
    f64 updateCarry = 0.0;

    Impl() {
        AudioBus master;
        master.name = "master";
        master.volume = 1.0f;
        master.muted = false;
        master.parent = -1;
        buses.push_back(master);
    }

    int FindBus(const std::string& name) const {
        for (usize i = 0; i < buses.size(); ++i)
            if (buses[i].name == name) return static_cast<int>(i);
        return -1;
    }

    // Обходит цепочку родителей с защитой по глубине, чтобы некорректное дерево
    // шин не могло зациклиться.
    f32 BusGain(int index) const {
        f32 g = 1.0f;
        int cur = index;
        int depth = 0;
        while (cur >= 0 && cur < static_cast<int>(buses.size()) && depth < kMaxBuses) {
            const AudioBus& b = buses[static_cast<usize>(cur)];
            if (b.muted) return 0.0f;
            g *= b.volume;
            cur = b.parent;
            ++depth;
        }
        return g;
    }

    bool IsUnderBus(int index, int ancestor) const {
        int cur = index;
        int depth = 0;
        while (cur >= 0 && cur < static_cast<int>(buses.size()) && depth < kMaxBuses) {
            if (cur == ancestor) return true;
            cur = buses[static_cast<usize>(cur)].parent;
            ++depth;
        }
        return false;
    }

    Voice* Find(VoiceId id) {
        if (id == kInvalidVoice) return nullptr;
        int index = static_cast<int>(id & 0xFFFFu) - 1;
        if (index < 0 || index >= kMaxVoices) return nullptr;
        Voice& v = voices[index];
        if (!v.active) return nullptr;
        if ((v.generation & 0xFFFFu) != (id >> 16)) return nullptr;
        return &v;
    }
};

// ---------------------------------------------------------------------------
// Разбор WAV
// ---------------------------------------------------------------------------
namespace {

struct WavHeader {
    u16 formatTag = 0;
    u16 channels = 0;
    u32 sampleRate = 0;
    u16 blockAlign = 0;
    u16 bitsPerSample = 0;
    u16 validBits = 0;
    bool haveFmt = false;
    bool haveData = false;
    const u8* data = nullptr;
    usize dataSize = 0;
    u32 loopStart = 0;
    u32 loopEnd = 0;
};

bool ParseWavHeader(const void* bytes, usize size, WavHeader* out, std::string* error) {
    auto fail = [&](const char* msg) {
        if (error) *error = msg;
        return false;
    };
    if (error) error->clear();
    if (!bytes || !out || size < 12) return fail("wav: file too small");
    const u8* base = static_cast<const u8*>(bytes);
    if (!FourCC(base, "RIFF")) return fail("wav: missing RIFF header");
    if (!FourCC(base + 8, "WAVE")) return fail("wav: not a WAVE file");

    usize off = 12;
    while (off + 8 <= size) {
        const u8* id = base + off;
        u32 chunkSize = RdU32(base + off + 4);
        usize content = off + 8;
        usize avail = size - content;
        usize use = chunkSize <= avail ? static_cast<usize>(chunkSize) : avail;

        if (FourCC(id, "fmt ")) {
            if (use < 16) return fail("wav: truncated fmt chunk");
            const u8* f = base + content;
            out->formatTag = RdU16(f);
            out->channels = RdU16(f + 2);
            out->sampleRate = RdU32(f + 4);
            out->blockAlign = RdU16(f + 12);
            out->bitsPerSample = RdU16(f + 14);
            out->validBits = out->bitsPerSample;
            if (out->formatTag == 0xFFFE) {  // WAVE_FORMAT_EXTENSIBLE
                if (use < 40) return fail("wav: truncated extensible fmt chunk");
                out->validBits = RdU16(f + 18);
                if (out->validBits == 0) out->validBits = out->bitsPerSample;
                out->formatTag = RdU16(f + 24);  // первое поле SubFormat GUID
            }
            out->haveFmt = true;
        } else if (FourCC(id, "data")) {
            if (!out->haveData) {
                out->data = base + content;
                out->dataSize = use;
                out->haveData = true;
            }
        } else if (FourCC(id, "smpl")) {
            // 36-байтовый заголовок, затем 24-байтовые записи лупов.
            if (use >= 36 + 24) {
                const u8* s = base + content;
                u32 loops = RdU32(s + 28);
                if (loops > 0) {
                    out->loopStart = RdU32(s + 44);
                    out->loopEnd = RdU32(s + 48) + 1u;  // хранится как полуоткрытый диапазон
                }
            }
        }

        if (chunkSize > avail) break;  // файл обрезан: прекращаем сканирование
        off = content + static_cast<usize>(chunkSize) + (chunkSize & 1u);
    }

    if (!out->haveFmt) return fail("wav: no fmt chunk");
    if (!out->haveData) return fail("wav: no data chunk");
    if (out->channels == 0 || out->channels > 8) return fail("wav: unsupported channel count");
    if (out->sampleRate == 0) return fail("wav: invalid sample rate");
    if (out->formatTag != 1 && out->formatTag != 3)
        return fail("wav: unsupported format tag (only PCM and IEEE float)");
    return true;
}

bool DecodeWavInternal(const void* bytes, usize size, std::vector<f32>* out, int* channels,
                       int* sampleRate, u32* loopStart, u32* loopEnd, std::string* error) {
    WavHeader h;
    if (!ParseWavHeader(bytes, size, &h, error)) return false;

    const usize bytesPerSample = static_cast<usize>(h.bitsPerSample / 8);
    if (bytesPerSample == 0 || (h.bitsPerSample % 8) != 0) {
        if (error) *error = "wav: unsupported bit depth";
        return false;
    }
    const bool isFloat = h.formatTag == 3;
    if (!isFloat && h.bitsPerSample != 8 && h.bitsPerSample != 16 && h.bitsPerSample != 24 &&
        h.bitsPerSample != 32) {
        if (error) *error = "wav: unsupported PCM bit depth";
        return false;
    }
    if (isFloat && h.bitsPerSample != 32 && h.bitsPerSample != 64) {
        if (error) *error = "wav: unsupported float bit depth";
        return false;
    }

    const usize frameBytes = h.blockAlign > bytesPerSample * static_cast<usize>(h.channels)
                                 ? static_cast<usize>(h.blockAlign)
                                 : bytesPerSample * static_cast<usize>(h.channels);
    if (frameBytes == 0) {
        if (error) *error = "wav: invalid block alignment";
        return false;
    }
    const usize frames = h.dataSize / frameBytes;
    if (frames == 0) {
        if (error) *error = "wav: empty data chunk";
        return false;
    }

    if (out) {
        out->resize(frames * static_cast<usize>(h.channels));
        for (usize f = 0; f < frames; ++f) {
            const u8* frame = h.data + f * frameBytes;
            for (int c = 0; c < static_cast<int>(h.channels); ++c) {
                const u8* p = frame + static_cast<usize>(c) * bytesPerSample;
                f32 v = 0.0f;
                if (isFloat) {
                    v = h.bitsPerSample == 32 ? RdF32(p) : static_cast<f32>(RdF64(p));
                } else {
                    switch (h.bitsPerSample) {
                        case 8:
                            // 8-битный PCM в RIFF беззнаковый.
                            v = (static_cast<f32>(p[0]) - 128.0f) / 128.0f;
                            break;
                        case 16:
                            v = static_cast<f32>(static_cast<i16>(RdU16(p))) / 32768.0f;
                            break;
                        case 24: {
                            i32 s = static_cast<i32>(static_cast<u32>(p[0]) |
                                                     (static_cast<u32>(p[1]) << 8) |
                                                     (static_cast<u32>(p[2]) << 16));
                            s = (s ^ 0x800000) - 0x800000;  // расширение знака
                            v = static_cast<f32>(s) / 8388608.0f;
                            break;
                        }
                        case 32:
                        default:
                            v = static_cast<f32>(static_cast<i32>(RdU32(p))) / 2147483648.0f;
                            break;
                    }
                }
                if (!std::isfinite(v)) v = 0.0f;
                (*out)[f * static_cast<usize>(h.channels) + static_cast<usize>(c)] = v;
            }
        }
    }

    if (channels) *channels = static_cast<int>(h.channels);
    if (sampleRate) *sampleRate = static_cast<int>(h.sampleRate);
    if (loopStart) *loopStart = h.loopStart;
    if (loopEnd) *loopEnd = h.loopEnd;
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// AudioClip
// ---------------------------------------------------------------------------
AudioClip::~AudioClip() = default;
AudioClip::AudioClip(AudioClip&&) noexcept = default;
AudioClip& AudioClip::operator=(AudioClip&&) noexcept = default;

void AudioClip::Destroy() {
    samples_.clear();
    samples_.shrink_to_fit();
    channels_ = 0;
    sampleRate_ = 0;
    loopStart_ = 0;
    loopEnd_ = 0;
    format_ = AudioFormat::Unknown;
    path_.clear();
}

bool AudioClip::DecodeWav(const void* data, usize size, std::vector<f32>* out, int* channels,
                          int* sampleRate, std::string* error) {
    return DecodeWavInternal(data, size, out, channels, sampleRate, nullptr, nullptr, error);
}

bool AudioClip::DecodeOgg(const void* data, usize size, std::vector<f32>* out, int* channels,
                          int* sampleRate, std::string* error) {
    if (error) error->clear();
    if (out) out->clear();
    if (!data || size == 0) {
        if (error) *error = "ogg: empty input";
        return false;
    }
    int ch = 0, rate = 0;
    short* pcm = nullptr;
    const int len = size > 0x7FFFFFFFu ? 0x7FFFFFFF : static_cast<int>(size);
    int frames =
        stb_vorbis_decode_memory(static_cast<const unsigned char*>(data), len, &ch, &rate, &pcm);
    if (frames <= 0 || !pcm || ch <= 0 || rate <= 0) {
        std::free(pcm);
        if (error) *error = "ogg: not a valid Vorbis stream";
        return false;
    }
    if (out) {
        const usize count = static_cast<usize>(frames) * static_cast<usize>(ch);
        out->resize(count);
        for (usize i = 0; i < count; ++i) (*out)[i] = static_cast<f32>(pcm[i]) / 32768.0f;
    }
    std::free(pcm);
    if (channels) *channels = ch;
    if (sampleRate) *sampleRate = rate;
    return true;
}

bool AudioClip::DecodeMp3(const void* data, usize size, std::vector<f32>* out, int* channels,
                          int* sampleRate, std::string* error) {
    if (error) error->clear();
    if (out) out->clear();
    if (!data || size == 0) {
        if (error) *error = "mp3: empty input";
        return false;
    }
    mp3dec_t decoder;
    mp3dec_file_info_t info;
    int rc =
        mp3dec_load_buf(&decoder, static_cast<const uint8_t*>(data), size, &info, nullptr, nullptr);
    if (rc != 0 || !info.buffer || info.samples == 0 || info.channels <= 0 || info.hz <= 0) {
        std::free(info.buffer);
        if (error) *error = "mp3: not a valid MPEG audio stream";
        return false;
    }
    if (out) {
        out->resize(info.samples);
        for (usize i = 0; i < info.samples; ++i)
            (*out)[i] = static_cast<f32>(info.buffer[i]) / 32768.0f;
    }
    std::free(info.buffer);
    if (channels) *channels = info.channels;
    if (sampleRate) *sampleRate = info.hz;
    return true;
}

AudioFormat AudioClip::DetectFormat(const void* data, usize size) {
    if (!data || size < 4) return AudioFormat::Unknown;
    const u8* p = static_cast<const u8*>(data);
    if (size >= 12 && FourCC(p, "RIFF") && FourCC(p + 8, "WAVE")) return AudioFormat::Wav;
    if (FourCC(p, "OggS")) return AudioFormat::Ogg;
    if (std::memcmp(p, "ID3", 3) == 0) return AudioFormat::Mp3;
    // MPEG frame sync аудио (11 установленных бит) - достаточно как подсказка формата.
    if (p[0] == 0xFF && (p[1] & 0xE0) == 0xE0) return AudioFormat::Mp3;
    return AudioFormat::Unknown;
}

bool AudioClip::LoadFromMemory(const void* data, usize size, AudioFormat hint) {
    if (!data || size == 0) return false;

    AudioFormat format = hint != AudioFormat::Unknown ? hint : DetectFormat(data, size);
    std::vector<f32> samples;
    int ch = 0, rate = 0;
    u32 loopStart = 0, loopEnd = 0;
    std::string error;
    bool ok = false;
    switch (format) {
        case AudioFormat::Wav:
            ok = DecodeWavInternal(data, size, &samples, &ch, &rate, &loopStart, &loopEnd, &error);
            break;
        case AudioFormat::Ogg:
            ok = DecodeOgg(data, size, &samples, &ch, &rate, &error);
            break;
        case AudioFormat::Mp3:
            ok = DecodeMp3(data, size, &samples, &ch, &rate, &error);
            break;
        default:
            error = "audio: unrecognised container format";
            break;
    }
    if (!ok) {
        ENG_LOGE("audio", "clip decode failed: %s", error.c_str());
        return false;
    }

    samples_ = std::move(samples);
    channels_ = ch;
    sampleRate_ = rate;
    format_ = format;
    path_.clear();
    // Оставляем область лупа `smpl` только если она осмысленна.
    if (loopEnd > loopStart && loopStart < frameCount()) {
        loopStart_ = loopStart;
        loopEnd_ = loopEnd > frameCount() ? static_cast<u32>(frameCount()) : loopEnd;
    } else {
        loopStart_ = 0;
        loopEnd_ = 0;
    }
    return true;
}

bool AudioClip::LoadFromFile(const std::string& path) {
    ByteBuffer bytes = ReadBinaryFile(path);
    if (bytes.empty()) {
        ENG_LOGE("audio", "cannot read audio file '%s'", path.c_str());
        return false;
    }
    AudioFormat hint = AudioFormat::Unknown;
    std::string ext = PathExt(path);
    if (ext == ".wav" || ext == ".wave") hint = AudioFormat::Wav;
    else if (ext == ".ogg" || ext == ".oga") hint = AudioFormat::Ogg;
    else if (ext == ".mp3") hint = AudioFormat::Mp3;

    if (!LoadFromMemory(bytes.data(), bytes.size(), hint)) {
        ENG_LOGE("audio", "failed to decode '%s'", path.c_str());
        return false;
    }
    path_ = path;
    return true;
}

AudioClip AudioClip::MakeTone(f32 frequency, f32 durationSeconds, f32 sampleRate, int channels) {
    AudioClip clip;
    if (channels < 1) channels = 1;
    if (!(sampleRate > 0.0f)) sampleRate = 44100.0f;
    if (!(durationSeconds > 0.0f)) durationSeconds = 0.0f;
    const usize frames = static_cast<usize>(static_cast<f64>(durationSeconds) * sampleRate + 0.5);
    const usize ch = static_cast<usize>(channels);
    clip.samples_.assign(frames * ch, 0.0f);
    f64 phase = 0.0;
    const f64 inc = static_cast<f64>(frequency) / static_cast<f64>(sampleRate);
    for (usize f = 0; f < frames; ++f) {
        f32 s = static_cast<f32>(std::sin(phase * kTauD));
        for (usize c = 0; c < ch; ++c) clip.samples_[f * ch + c] = s;
        phase += inc;
        if (phase >= 1.0) phase -= std::floor(phase);
    }
    clip.channels_ = channels;
    clip.sampleRate_ = static_cast<int>(sampleRate);
    clip.format_ = AudioFormat::Unknown;
    return clip;
}

AudioClip AudioClip::MakeNoise(f32 durationSeconds, f32 sampleRate, int channels) {
    AudioClip clip;
    if (channels < 1) channels = 1;
    if (!(sampleRate > 0.0f)) sampleRate = 44100.0f;
    if (!(durationSeconds > 0.0f)) durationSeconds = 0.0f;
    const usize frames = static_cast<usize>(static_cast<f64>(durationSeconds) * sampleRate + 0.5);
    const usize ch = static_cast<usize>(channels);
    clip.samples_.resize(frames * ch);
    Random rng(0x5EED1234u);  // детерминированный
    for (usize f = 0; f < frames; ++f)
        for (usize c = 0; c < ch; ++c) clip.samples_[f * ch + c] = rng.Range(-1.0f, 1.0f);
    clip.channels_ = channels;
    clip.sampleRate_ = static_cast<int>(sampleRate);
    clip.format_ = AudioFormat::Unknown;
    return clip;
}

AudioClip AudioClip::MakeSilence(f32 durationSeconds, f32 sampleRate, int channels) {
    AudioClip clip;
    if (channels < 1) channels = 1;
    if (!(sampleRate > 0.0f)) sampleRate = 44100.0f;
    if (!(durationSeconds > 0.0f)) durationSeconds = 0.0f;
    const usize frames = static_cast<usize>(static_cast<f64>(durationSeconds) * sampleRate + 0.5);
    clip.samples_.assign(frames * static_cast<usize>(channels), 0.0f);
    clip.channels_ = channels;
    clip.sampleRate_ = static_cast<int>(sampleRate);
    clip.format_ = AudioFormat::Unknown;
    return clip;
}

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------
Audio::Audio() : impl_(new Impl()) {}
Audio::~Audio() { Shutdown(); }

Audio& Audio::Get() {
    static Audio instance;
    return instance;
}

bool Audio::Init(int sampleRate, int channels, int bufferFrames) {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (initialised_) return true;
        sampleRate_ = sampleRate > 0 ? sampleRate : 44100;
        channels_ = channels == 1 ? 1 : 2;
        impl_->bufferFrames = bufferFrames > 0 ? bufferFrames : 1024;
        impl_->updateCarry = 0.0;
        stats_ = Stats{};
        stats_.maxVoices = kMaxVoices;
    }
    {
        // Отдельный мьютекс: никогда не вкладывать его в `mutex` (Mix берёт `mutex`,
        // пока PumpSilent держит `scratchMutex`).
        std::lock_guard<std::mutex> lock(impl_->scratchMutex);
        impl_->scratch.assign(static_cast<usize>(impl_->bufferFrames) *
                                  static_cast<usize>(channels_),
                              0.0f);
    }

    // Бэкенд может запустить свой колбэк немедленно, поэтому не держим блокировку.
    const bool device = PlatformAudioInit(sampleRate_, channels_, impl_->bufferFrames);
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        silent_ = !device;
        initialised_ = true;
    }
    if (device) {
        ENG_LOGI("audio", "audio device ready (%d Hz, %d ch, %d frames)", sampleRate_, channels_,
                 impl_->bufferFrames);
    } else {
        ENG_LOGW("audio", "no audio device available, mixer runs in silent mode");
    }
    return true;
}

void Audio::Shutdown() {
    PlatformAudioShutdown();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (int i = 0; i < kMaxVoices; ++i) {
        impl_->voices[i].active = false;
        impl_->voices[i].clip = nullptr;
    }
    initialised_ = false;
    silent_ = true;
}

VoiceId Audio::Play(const AudioClip& clip, const PlayParams& params) {
    if (!clip.Valid()) {
        ENG_LOGW("audio", "Play() called with an empty clip");
        return kInvalidVoice;
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    int slot = -1;
    for (int i = 0; i < kMaxVoices; ++i) {
        if (!impl_->voices[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        // Вытеснение голосов: сначала самые тихие, при равенстве — самые старые.
        f32 bestVol = 1e30f;
        u32 bestSerial = 0xFFFFFFFFu;
        for (int i = 0; i < kMaxVoices; ++i) {
            const Voice& v = impl_->voices[i];
            if (v.volume < bestVol || (v.volume == bestVol && v.serial < bestSerial)) {
                bestVol = v.volume;
                bestSerial = v.serial;
                slot = i;
            }
        }
    }
    if (slot < 0) return kInvalidVoice;

    Voice& v = impl_->voices[slot];
    const u32 generation = (v.generation + 1u) & 0xFFFFu;
    v = Voice{};
    v.generation = generation;
    v.serial = ++impl_->serialCounter;
    v.active = true;
    v.clip = &clip;
    v.volume = params.volume < 0.0f ? 0.0f : params.volume;
    v.pitch = params.pitch > 0.0f ? params.pitch : 1.0f;
    v.pan = Clamp(params.pan, -1.0f, 1.0f);
    v.looping = params.looping;
    v.bus = params.bus;
    v.fadeIn = params.fadeIn > 0.0f ? params.fadeIn : 0.0f;
    v.spatial = params.spatial;
    v.position = params.position;
    v.minDistance = params.minDistance;
    v.maxDistance = params.maxDistance;
    v.rolloff = params.rolloff;
    const f64 clipRate = clip.SampleRate() > 0 ? static_cast<f64>(clip.SampleRate()) : 44100.0;
    v.playhead = params.startTimeOffset > 0.0 ? params.startTimeOffset * clipRate : 0.0;

    // `stream` принимается для совместимости с API; клипы уже владеют своим
    // декодированным PCM, поэтому воспроизведение всегда идёт из памяти.
    return MakeVoiceId(slot, generation);
}

VoiceId Audio::PlayMusic(const AudioClip& clip, f32 volume, bool looping) {
    PlayParams p;
    p.volume = volume;
    p.looping = looping;
    p.stream = true;
    p.bus = 0;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const int music = impl_->FindBus("music");
        if (music >= 0) p.bus = music;
    }
    return Play(clip, p);
}

bool Audio::Stop(VoiceId voice, f32 fadeOut) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Voice* v = impl_->Find(voice);
    if (!v) return false;
    if (fadeOut <= 0.0f) {
        v->active = false;
        v->clip = nullptr;
    } else {
        v->fadingOut = true;
        v->fadeOut = fadeOut;
        v->fadeOutPos = 0.0f;
    }
    return true;
}

void Audio::StopAll(f32 fadeOut) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (int i = 0; i < kMaxVoices; ++i) {
        Voice& v = impl_->voices[i];
        if (!v.active) continue;
        if (fadeOut <= 0.0f) {
            v.active = false;
            v.clip = nullptr;
        } else {
            v.fadingOut = true;
            v.fadeOut = fadeOut;
            v.fadeOutPos = 0.0f;
        }
    }
}

void Audio::StopBus(const std::string& bus, f32 fadeOut) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const int index = impl_->FindBus(bus);
    if (index < 0) return;
    for (int i = 0; i < kMaxVoices; ++i) {
        Voice& v = impl_->voices[i];
        if (!v.active) continue;
        if (!impl_->IsUnderBus(v.bus, index)) continue;
        if (fadeOut <= 0.0f) {
            v.active = false;
            v.clip = nullptr;
        } else {
            v.fadingOut = true;
            v.fadeOut = fadeOut;
            v.fadeOutPos = 0.0f;
        }
    }
}

void Audio::Pause(VoiceId voice) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (Voice* v = impl_->Find(voice)) v->paused = true;
}

void Audio::Resume(VoiceId voice) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (Voice* v = impl_->Find(voice)) v->paused = false;
}

void Audio::SetVoiceVolume(VoiceId voice, f32 volume) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (Voice* v = impl_->Find(voice)) v->volume = volume < 0.0f ? 0.0f : volume;
}

void Audio::SetVoicePitch(VoiceId voice, f32 pitch) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (Voice* v = impl_->Find(voice)) v->pitch = Clamp(pitch, 0.0f, 8.0f);
}

void Audio::SetVoicePosition(VoiceId voice, const Vec3& position) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (Voice* v = impl_->Find(voice)) v->position = position;
}

bool Audio::IsPlaying(VoiceId voice) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->Find(voice) != nullptr;
}

f32 Audio::VoiceTime(VoiceId voice) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Voice* v = impl_->Find(voice);
    return v ? static_cast<f32>(v->playedSeconds) : 0.0f;
}

int Audio::AddBus(const std::string& name, int parent) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const int existing = impl_->FindBus(name);
    if (existing >= 0) return existing;
    if (impl_->buses.size() >= static_cast<usize>(kMaxBuses)) {
        ENG_LOGW("audio", "bus limit (%d) reached, ignoring bus '%s'", kMaxBuses, name.c_str());
        return -1;
    }
    AudioBus bus;
    bus.name = name;
    bus.volume = 1.0f;
    bus.muted = false;
    bus.parent = (parent >= 0 && parent < static_cast<int>(impl_->buses.size())) ? parent : 0;
    impl_->buses.push_back(std::move(bus));
    return static_cast<int>(impl_->buses.size()) - 1;
}

void Audio::SetBusVolume(const std::string& name, f32 volume) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const int index = impl_->FindBus(name);
    if (index < 0) {
        ENG_LOGW("audio", "SetBusVolume: unknown bus '%s'", name.c_str());
        return;
    }
    impl_->buses[static_cast<usize>(index)].volume = volume < 0.0f ? 0.0f : volume;
}

void Audio::SetBusMuted(const std::string& name, bool muted) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const int index = impl_->FindBus(name);
    if (index < 0) {
        ENG_LOGW("audio", "SetBusMuted: unknown bus '%s'", name.c_str());
        return;
    }
    impl_->buses[static_cast<usize>(index)].muted = muted;
}

f32 Audio::BusVolume(const std::string& name) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const int index = impl_->FindBus(name);
    return index < 0 ? 0.0f : impl_->buses[static_cast<usize>(index)].volume;
}

int Audio::ActiveVoices() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    int count = 0;
    // Приостановленный голос всё ещё владеет своим слотом, поэтому считается
    // активным (согласуется со Stats::voices, вычисляемым в Mix()).
    for (int i = 0; i < kMaxVoices; ++i)
        if (impl_->voices[i].active) ++count;
    return count;
}

// ---------------------------------------------------------------------------
// Сведение
// ---------------------------------------------------------------------------
void Audio::Mix(f32* out, int frameCount) {
    if (!out || frameCount <= 0) return;

    Impl& im = *impl_;
    std::lock_guard<std::mutex> lock(im.mutex);

    const int devChannels = channels_ == 1 ? 1 : 2;
    const f32 devRate = sampleRate_ > 0 ? static_cast<f32>(sampleRate_) : 44100.0f;
    const f32 invDevRate = 1.0f / devRate;
    const f32 master = masterVolume_;
    const usize sampleCount = static_cast<usize>(frameCount) * static_cast<usize>(devChannels);
    for (usize i = 0; i < sampleCount; ++i) out[i] = 0.0f;

    // Усиление по шинам, вычисляется один раз на буфер (без строк, без аллокаций).
    f32 busGain[kMaxBuses];
    const int busCount =
        im.buses.size() < static_cast<usize>(kMaxBuses) ? static_cast<int>(im.buses.size()) : kMaxBuses;
    for (int i = 0; i < busCount; ++i) busGain[i] = im.BusGain(i);

    const Vec3 listenerPos = listener_.position;
    const Vec3 listenerFwd = Normalize(listener_.forward);
    Vec3 listenerRight = Normalize(Cross(listenerFwd, listener_.up));
    if (LengthSq(listenerRight) < kEpsilon) listenerRight = Vec3{1, 0, 0};

    int activeVoices = 0;
    for (int vi = 0; vi < kMaxVoices; ++vi) {
        Voice& v = im.voices[vi];
        if (!v.active) continue;
        if (!v.clip || !v.clip->Valid()) {
            v.active = false;
            v.clip = nullptr;
            continue;
        }
        ++activeVoices;
        if (v.paused) continue;

        const std::vector<f32>& data = v.clip->Samples();
        const int clipChannels = v.clip->Channels() > 0 ? v.clip->Channels() : 1;
        const usize clipFrames = data.size() / static_cast<usize>(clipChannels);
        if (clipFrames == 0) {
            v.active = false;
            v.clip = nullptr;
            --activeVoices;
            continue;
        }
        const f32* src = data.data();

        // Область лупа: метаданные `smpl`, если есть, иначе весь клип.
        usize loopStart = v.clip->LoopStart();
        usize loopEnd = v.clip->LoopEnd();
        if (loopStart >= clipFrames) loopStart = 0;
        if (loopEnd == 0 || loopEnd > clipFrames) loopEnd = clipFrames;
        if (loopEnd <= loopStart) {
            loopStart = 0;
            loopEnd = clipFrames;
        }

        const f32* srcL = src;
        const f32* srcR = clipChannels > 1 ? src + 1 : src;
        const usize stride = static_cast<usize>(clipChannels);
        const f32 clipRate =
            v.clip->SampleRate() > 0 ? static_cast<f32>(v.clip->SampleRate()) : devRate;
        const f64 step =
            static_cast<f64>(v.pitch) * (static_cast<f64>(clipRate) / static_cast<f64>(devRate));

        // Усиление: голос * дерево шин * мастер * расстояние.
        f32 gain = v.volume * master;
        const int busIndex = (v.bus >= 0 && v.bus < busCount) ? v.bus : 0;
        gain *= busGain[busIndex];

        f32 left = 0.70710678f, right = 0.70710678f;
        if (v.spatial) {
            const Vec3 delta = v.position - listenerPos;
            const f32 dist = Length(delta);
            gain *= DistanceGain(v.rolloff, dist, v.minDistance, v.maxDistance);
            if (dist > kEpsilon) PanGains(Clamp(Dot(delta / dist, listenerRight), -1.0f, 1.0f), &left, &right);
        } else {
            PanGains(v.pan, &left, &right);
        }

        bool finished = false;
        for (int f = 0; f < frameCount && !finished; ++f) {
            if (v.looping) {
                if (v.playhead >= static_cast<f64>(loopEnd)) {
                    const f64 span = static_cast<f64>(loopEnd - loopStart);
                    f64 over = std::fmod(v.playhead - static_cast<f64>(loopStart), span);
                    if (over < 0.0) over += span;
                    v.playhead = static_cast<f64>(loopStart) + over;
                }
            } else if (v.playhead >= static_cast<f64>(clipFrames)) {
                break;  // рендерить больше нечего
            }

            usize i0 = static_cast<usize>(v.playhead);
            if (i0 >= clipFrames) i0 = clipFrames - 1;
            const f32 frac = static_cast<f32>(v.playhead - static_cast<f64>(i0));
            usize i1 = i0 + 1;
            if (v.looping) {
                if (i1 >= loopEnd) i1 = loopStart;
            } else if (i1 >= clipFrames) {
                i1 = i0;
            }

            const f32 l0 = srcL[i0 * stride];
            const f32 l1 = srcL[i1 * stride];
            const f32 l = l0 + (l1 - l0) * frac;
            const f32 r0 = srcR[i0 * stride];
            const f32 r1 = srcR[i1 * stride];
            const f32 r = r0 + (r1 - r0) * frac;

            f32 fade = 1.0f;
            if (v.fadingOut) {
                fade = v.fadeOut > 0.0f ? 1.0f - v.fadeOutPos / v.fadeOut : 0.0f;
                if (fade < 0.0f) fade = 0.0f;
            } else if (v.fadeIn > 0.0f) {
                fade = v.fadeInPos / v.fadeIn;
                if (fade > 1.0f) fade = 1.0f;
            }
            const f32 g = gain * fade;

            if (devChannels == 1) {
                out[f] += (l + r) * 0.5f * g;
            } else {
                out[f * 2] += l * g * left;
                out[f * 2 + 1] += r * g * right;
            }

            v.playhead += step;
            v.playedSeconds += static_cast<f64>(invDevRate);
            if (v.fadingOut) {
                v.fadeOutPos += invDevRate;
                if (v.fadeOutPos >= v.fadeOut) finished = true;
            } else if (v.fadeIn > 0.0f) {
                v.fadeInPos += invDevRate;
            }
        }

        if (finished || (!v.looping && v.playhead >= static_cast<f64>(clipFrames))) {
            v.active = false;
            v.clip = nullptr;
            --activeVoices;
        }
    }

    // Мягкий лимитер: удерживает горячий микс в [-1, 1] без жёсткого клиппинга.
    for (usize i = 0; i < sampleCount; ++i) out[i] = SoftClip(out[i]);

    stats_.voices = activeVoices;
    stats_.maxVoices = kMaxVoices;
    stats_.mixedFrames += static_cast<u64>(frameCount);
    stats_.cpuLoad = static_cast<f32>(activeVoices) / static_cast<f32>(kMaxVoices);
}

void Audio::Update(f32 dt) {
    if (!(dt > 0.0f)) return;
    // С реальным устройством микшер гоняет колбэк; иначе имитируем его.
    if (initialised_ && !silent_) return;
    int whole = 0;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const f64 frames =
            impl_->updateCarry + static_cast<f64>(dt) * static_cast<f64>(sampleRate_);
        whole = static_cast<int>(frames);
        impl_->updateCarry = frames - static_cast<f64>(whole);
    }
    if (whole > 0) PumpSilent(whole);
}

void Audio::PumpSilent(int frames) {
    if (frames <= 0) return;
    const usize channels = channels_ == 1 ? 1u : 2u;
    const usize needed = static_cast<usize>(frames) * channels;
    std::lock_guard<std::mutex> lock(impl_->scratchMutex);
    if (impl_->scratch.size() < needed) impl_->scratch.resize(needed);
    Mix(impl_->scratch.data(), frames);
}

}  // namespace crossrender
