// Тесты аудио-модуля: декодирование WAV, сгенерированные тоны, поведение микшера
// (громкость/панорама/питч/зацикливание/шины/3D), детерминизм и тихий fallback.
//
// NOTE: crossrender/core/Math.h использует std::vector в Random::Shuffle, но не включает
// <vector>; подключаем его первым, чтобы замороженный публичный заголовок компилировался сам.
#include <vector>

#include "crossrender/core/File.h"
#include "crossrender/test/Test.h"
#include "crossrender/audio/Audio.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <cstdlib>
#include <cstring>
#include <algorithm>

using namespace crossrender;

namespace {

// ---------------------------------------------------------------------------
// Вспомогательные функции
// ---------------------------------------------------------------------------
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
void PutU64(std::vector<u8>& v, u64 x) {
    PutU32(v, static_cast<u32>(x & 0xFFFFFFFFu));
    PutU32(v, static_cast<u32>(x >> 32));
}
void PutTag(std::vector<u8>& v, const char* t) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<u8>(t[i]));
}
void PutChunk(std::vector<u8>& out, const char* id, const std::vector<u8>& payload) {
    PutTag(out, id);
    PutU32(out, static_cast<u32>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    if (payload.size() & 1u) out.push_back(0);  // чанки RIFF выровнены по слову
}

// Детерминированный, точно представимый тестовый сигнал: -0.5 .. +0.484375.
f32 Pattern(usize frame, int channel) { return static_cast<f32>((frame + static_cast<usize>(channel) * 7u) % 64u) / 64.0f - 0.5f; }

void EncodeSample(std::vector<u8>& out, f32 v, int bits, bool floatFmt) {
    if (floatFmt) {
        if (bits == 32) {
            u32 raw = 0;
            std::memcpy(&raw, &v, sizeof(raw));
            PutU32(out, raw);
        } else {
            f64 d = static_cast<f64>(v);
            u64 raw = 0;
            std::memcpy(&raw, &d, sizeof(raw));
            PutU64(out, raw);
        }
        return;
    }
    switch (bits) {
        case 8:
            out.push_back(static_cast<u8>(static_cast<int>(std::lround(v * 128.0f + 128.0f))));
            break;
        case 16:
            PutU16(out, static_cast<u16>(static_cast<i16>(std::lround(v * 32768.0f))));
            break;
        case 24: {
            i32 s = static_cast<i32>(std::lround(v * 8388608.0f));
            out.push_back(static_cast<u8>(s & 0xFF));
            out.push_back(static_cast<u8>((s >> 8) & 0xFF));
            out.push_back(static_cast<u8>((s >> 16) & 0xFF));
            break;
        }
        case 32:
        default:
            PutU32(out, static_cast<u32>(static_cast<i32>(std::llround(static_cast<f64>(v) * 2147483647.0))));
            break;
    }
}

struct WavSpec {
    int bits = 16;
    bool floatFmt = false;
    bool extensible = false;
    int channels = 2;
    int sampleRate = 44100;
    int frames = 256;
    bool withSmpl = false;
    u32 loopStart = 0;
    u32 loopEndInclusive = 0;
};

// Минимальный WAV-энкодер для проверки декодера (все поддерживаемые глубины).
std::vector<u8> BuildWav(const WavSpec& spec) {
    std::vector<u8> fmt;
    const u16 tag = spec.extensible ? 0xFFFE
                                   : static_cast<u16>(spec.floatFmt ? 3 : 1);
    const u16 blockAlign = static_cast<u16>(spec.bits / 8 * spec.channels);
    PutU16(fmt, tag);
    PutU16(fmt, static_cast<u16>(spec.channels));
    PutU32(fmt, static_cast<u32>(spec.sampleRate));
    PutU32(fmt, static_cast<u32>(spec.sampleRate * blockAlign));
    PutU16(fmt, blockAlign);
    PutU16(fmt, static_cast<u16>(spec.bits));
    if (spec.extensible) {
        PutU16(fmt, 22);                                    // cbSize
        PutU16(fmt, static_cast<u16>(spec.bits));           // валидные биты
        PutU32(fmt, spec.channels == 2 ? 0x3u : 0x4u);      // маска каналов
        // SubFormat GUID: {00000001|3}-0000-0010-8000-00aa00389b71
        PutU32(fmt, spec.floatFmt ? 3u : 1u);
        PutU16(fmt, 0);
        PutU16(fmt, 0x0010);
        const u8 tail[8] = {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
        fmt.insert(fmt.end(), tail, tail + 8);
    }

    std::vector<u8> data;
    for (int f = 0; f < spec.frames; ++f)
        for (int c = 0; c < spec.channels; ++c)
            EncodeSample(data, Pattern(static_cast<usize>(f), c), spec.bits, spec.floatFmt);

    std::vector<u8> body;
    PutChunk(body, "fmt ", fmt);
    PutChunk(body, "data", data);
    if (spec.withSmpl) {
        std::vector<u8> smpl;
        PutU32(smpl, 0);                       // производитель
        PutU32(smpl, 0);                       // продукт
        PutU32(smpl, 1000000000u / 44100u);    // период сэмпла
        PutU32(smpl, 60);                      // MIDI-нота единичной высоты
        PutU32(smpl, 0);                       // дробная часть питча
        PutU32(smpl, 0);                       // формат SMPTE
        PutU32(smpl, 0);                       // смещение SMPTE
        PutU32(smpl, 1);                       // один цикл
        PutU32(smpl, 0);                       // данные сэмплера
        PutU32(smpl, 0);                       // id cue-точки
        PutU32(smpl, 0);                       // тип: forward
        PutU32(smpl, spec.loopStart);
        PutU32(smpl, spec.loopEndInclusive);
        PutU32(smpl, 0);                       // дробная часть
        PutU32(smpl, 0);                       // число воспроизведений
        PutChunk(body, "smpl", smpl);
    }

    std::vector<u8> out;
    PutTag(out, "RIFF");
    PutU32(out, static_cast<u32>(4 + body.size()));
    PutTag(out, "WAVE");
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

f32 PeakOf(const std::vector<f32>& buffer) {
    f32 peak = 0.0f;
    for (f32 v : buffer) peak = std::max(peak, std::fabs(v));
    return peak;
}

f32 PeakOfChannel(const std::vector<f32>& buffer, int channel, int channels) {
    f32 peak = 0.0f;
    for (usize f = 0; f * static_cast<usize>(channels) + static_cast<usize>(channel) < buffer.size(); ++f)
        peak = std::max(peak, std::fabs(buffer[f * static_cast<usize>(channels) + static_cast<usize>(channel)]));
    return peak;
}

int ZeroCrossings(const std::vector<f32>& buffer, int channel, int channels) {
    int crossings = 0;
    f32 prev = 0.0f;
    bool first = true;
    for (usize f = 0; f * static_cast<usize>(channels) + static_cast<usize>(channel) < buffer.size(); ++f) {
        f32 v = buffer[f * static_cast<usize>(channels) + static_cast<usize>(channel)];
        if (!first && ((prev <= 0.0f && v > 0.0f) || (prev >= 0.0f && v < 0.0f))) ++crossings;
        prev = v;
        first = false;
    }
    return crossings;
}

void ResetAudio() {
    Audio& a = Audio::Get();
    a.Shutdown();  // останавливает устройство и сбрасывает все голоса
    a.StopAll();
    a.SetMasterVolume(1.0f);
    ListenerState listener;
    a.SetListener(listener);
}

// Смешивает frames кадров текущего состояния в свежий стерео-буфер.
std::vector<f32> MixFrames(Audio& a, int frames) {
    std::vector<f32> buffer(static_cast<usize>(frames) * 2, 0.0f);
    a.Mix(buffer.data(), frames);
    return buffer;
}

}  // namespace

// ---------------------------------------------------------------------------
// Определение формата / ошибки декодирования
// ---------------------------------------------------------------------------
ENG_TEST(Audio, DetectFormatMagic) {
    const std::vector<u8> wav = BuildWav(WavSpec{});
    ENG_CHECK(AudioClip::DetectFormat(wav.data(), wav.size()) == AudioFormat::Wav);

    const u8 ogg[32] = {'O', 'g', 'g', 'S', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    ENG_CHECK(AudioClip::DetectFormat(ogg, sizeof(ogg)) == AudioFormat::Ogg);

    const u8 id3[16] = {'I', 'D', '3', 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    ENG_CHECK(AudioClip::DetectFormat(id3, sizeof(id3)) == AudioFormat::Mp3);

    const u8 frameSync[8] = {0xFF, 0xFB, 0x90, 0x00, 0, 0, 0, 0};
    ENG_CHECK(AudioClip::DetectFormat(frameSync, sizeof(frameSync)) == AudioFormat::Mp3);

    const u8 garbage[16] = {'N', 'O', 'P', 'E', 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    ENG_CHECK(AudioClip::DetectFormat(garbage, sizeof(garbage)) == AudioFormat::Unknown);
    ENG_CHECK(AudioClip::DetectFormat(nullptr, 0) == AudioFormat::Unknown);
    ENG_CHECK(AudioClip::DetectFormat(garbage, 3) == AudioFormat::Unknown);
}

ENG_TEST(Audio, GarbageDecodersFailSafely) {
    std::vector<u8> garbage(512);
    for (usize i = 0; i < garbage.size(); ++i) garbage[i] = static_cast<u8>((i * 37u + 11u) & 0xFFu);

    std::vector<f32> samples;
    int channels = 0, rate = 0;

    std::string oggError;
    ENG_CHECK(!AudioClip::DecodeOgg(garbage.data(), garbage.size(), &samples, &channels, &rate, &oggError));
    ENG_CHECK(!oggError.empty());

    std::string mp3Error;
    samples.clear();
    ENG_CHECK(!AudioClip::DecodeMp3(garbage.data(), garbage.size(), &samples, &channels, &rate, &mp3Error));
    ENG_CHECK(!mp3Error.empty());

    std::string wavError;
    samples.clear();
    ENG_CHECK(!AudioClip::DecodeWav(garbage.data(), garbage.size(), &samples, &channels, &rate, &wavError));
    ENG_CHECK(!wavError.empty());

    // Пустой ввод тоже должен отвергаться и не должен приводить к падению.
    ENG_CHECK(!AudioClip::DecodeOgg(nullptr, 0, &samples, &channels, &rate, &oggError));
    ENG_CHECK(!AudioClip::DecodeMp3(nullptr, 0, &samples, &channels, &rate, &mp3Error));
    ENG_CHECK(!AudioClip::DecodeWav(nullptr, 0, &samples, &channels, &rate, &wavError));

    // Клип, собранный из мусора, остаётся невалидным.
    AudioClip clip;
    ENG_CHECK(!clip.LoadFromMemory(garbage.data(), garbage.size()));
    ENG_CHECK(!clip.Valid());
}

// ---------------------------------------------------------------------------
// Декодирование WAV
// ---------------------------------------------------------------------------
ENG_TEST(Audio, WavDecodeAllBitDepths) {
    const struct {
        int bits;
        bool floatFmt;
        bool extensible;
        f32 tolerance;
    } cases[] = {
        {8, false, false, 0.01f},   {16, false, false, 1e-4f}, {24, false, false, 1e-6f},
        {32, false, false, 1e-6f},  {32, true, false, 1e-6f},  {64, true, false, 1e-6f},
        {16, false, true, 1e-4f},   {24, false, true, 1e-6f},  {32, true, true, 1e-6f},
    };

    for (const auto& c : cases) {
        WavSpec spec;
        spec.bits = c.bits;
        spec.floatFmt = c.floatFmt;
        spec.extensible = c.extensible;
        spec.channels = 2;
        spec.sampleRate = 22050;
        spec.frames = 200;
        const std::vector<u8> bytes = BuildWav(spec);

        std::vector<f32> samples;
        int channels = 0, rate = 0;
        std::string error;
        ENG_CHECK_MSG(AudioClip::DecodeWav(bytes.data(), bytes.size(), &samples, &channels, &rate, &error),
                      error);
        if (samples.empty()) continue;
        ENG_CHECK_EQ(channels, 2);
        ENG_CHECK_EQ(rate, 22050);
        ENG_CHECK_EQ(samples.size(), static_cast<usize>(200 * 2));
        for (int f = 0; f < 200; ++f) {
            for (int ch = 0; ch < 2; ++ch) {
                const f32 expected = Pattern(static_cast<usize>(f), ch);
                const f32 got = samples[static_cast<usize>(f) * 2 + static_cast<usize>(ch)];
                ENG_CHECK_NEAR(got, expected, c.tolerance);
            }
        }

        // Обёртка-клип должна нести те же метаданные.
        AudioClip clip;
        ENG_CHECK(clip.LoadFromMemory(bytes.data(), bytes.size()));
        ENG_CHECK_EQ(clip.Channels(), 2);
        ENG_CHECK_EQ(clip.SampleRate(), 22050);
        ENG_CHECK_EQ(clip.frameCount(), static_cast<usize>(200));
        ENG_CHECK(clip.Format() == AudioFormat::Wav);
    }
}

ENG_TEST(Audio, WavDecodeMonoAndFileRoundTrip) {
    WavSpec spec;
    spec.bits = 16;
    spec.channels = 1;
    spec.sampleRate = 44100;
    spec.frames = 128;
    const std::vector<u8> bytes = BuildWav(spec);

    // TempFilePath() лежит в пользовательском каталоге, который некоторые песочницы
    // делают read-only; перед сдачей откатываемся на обычный временный путь.
    std::string path = test::TempFilePath("eng_audio_roundtrip.wav");
    if (!WriteBinaryFile(path, bytes.data(), bytes.size())) {
#if !defined(_WIN32)
        path = "/tmp/eng_audio_roundtrip.wav";
        if (!WriteBinaryFile(path, bytes.data(), bytes.size()))
#endif
            ENG_SKIP("no writable temp directory available");
    }
    ENG_CHECK(FileExists(path));

    AudioClip clip;
    ENG_CHECK(clip.LoadFromFile(path));
    ENG_CHECK(clip.Valid());
    ENG_CHECK_EQ(clip.Channels(), 1);
    ENG_CHECK_EQ(clip.SampleRate(), 44100);
    ENG_CHECK_EQ(clip.frameCount(), static_cast<usize>(128));
    ENG_CHECK_STR_EQ(clip.SourcePath(), path);
    ENG_CHECK(clip.Format() == AudioFormat::Wav);
    ENG_CHECK_NEAR(clip.Duration(), 128.0f / 44100.0f, 1e-6f);
    ENG_CHECK_NEAR(clip.Samples()[5], Pattern(5, 0), 1e-4f);

    AudioClip missing;
    ENG_CHECK(!missing.LoadFromFile(path + ".does-not-exist"));
    std::remove(path.c_str());
}

ENG_TEST(Audio, WavSmplLoopPoints) {
    WavSpec spec;
    spec.bits = 16;
    spec.channels = 2;
    spec.sampleRate = 44100;
    spec.frames = 512;
    spec.withSmpl = true;
    spec.loopStart = 10;
    spec.loopEndInclusive = 99;
    const std::vector<u8> bytes = BuildWav(spec);

    AudioClip clip;
    ENG_CHECK(clip.LoadFromMemory(bytes.data(), bytes.size()));
    // конец в smpl включителен, движок отдаёт полуоткрытый диапазон [start, end).
    ENG_CHECK_EQ(clip.LoopStart(), 10u);
    ENG_CHECK_EQ(clip.LoopEnd(), 100u);

    // Без чанка smpl область цикла остаётся пустой.
    WavSpec plain;
    plain.frames = 64;
    const std::vector<u8> plainBytes = BuildWav(plain);
    AudioClip plainClip;
    ENG_CHECK(plainClip.LoadFromMemory(plainBytes.data(), plainBytes.size()));
    ENG_CHECK_EQ(plainClip.LoopStart(), 0u);
    ENG_CHECK_EQ(plainClip.LoopEnd(), 0u);
}

// ---------------------------------------------------------------------------
// Сгенерированные клипы
// ---------------------------------------------------------------------------
ENG_TEST(Audio, MakeToneFrameCountAndEnergy) {
    const AudioClip tone = AudioClip::MakeTone(440.0f, 0.25f, 44100.0f, 2);
    ENG_CHECK(tone.Valid());
    ENG_CHECK_EQ(tone.Channels(), 2);
    ENG_CHECK_EQ(tone.SampleRate(), 44100);
    ENG_CHECK_EQ(tone.frameCount(), static_cast<usize>(11025));
    ENG_CHECK_NEAR(tone.Duration(), 0.25f, 1e-4f);

    const AudioClip mono = AudioClip::MakeTone(440.0f, 0.5f, 48000.0f, 1);
    ENG_CHECK_EQ(mono.frameCount(), static_cast<usize>(24000));
    ENG_CHECK_EQ(mono.Channels(), 1);

    // Проверка энергии (RMS синуса полной амплитуды ~0.707).
    f64 sumSquares = 0.0;
    for (f32 v : tone.Samples()) sumSquares += static_cast<f64>(v) * v;
    const f64 rms = std::sqrt(sumSquares / static_cast<f64>(tone.Samples().size()));
    ENG_CHECK_GT(rms, 0.6);
    ENG_CHECK_GT(PeakOf(tone.Samples()), 0.99f);

    // Синус 440 Гц за 0.25 с имеет 110 периодов -> ~220 пересечений нуля.
    const int crossings = ZeroCrossings(tone.Samples(), 0, 2);
    ENG_CHECK_GT(crossings, 210);
    ENG_CHECK(crossings < 230);

    const AudioClip silence = AudioClip::MakeSilence(0.1f, 44100.0f, 2);
    ENG_CHECK_EQ(silence.frameCount(), static_cast<usize>(4410));
    ENG_CHECK_NEAR(PeakOf(silence.Samples()), 0.0f, 1e-9f);

    // MakeNoise детерминирован при фиксированном seed и остаётся в диапазоне.
    const AudioClip n1 = AudioClip::MakeNoise(0.05f, 44100.0f, 2);
    const AudioClip n2 = AudioClip::MakeNoise(0.05f, 44100.0f, 2);
    ENG_CHECK_EQ(n1.frameCount(), n2.frameCount());
    ENG_CHECK(n1.Samples() == n2.Samples());
    ENG_CHECK_GT(PeakOf(n1.Samples()), 0.5f);
    ENG_CHECK(PeakOf(n1.Samples()) <= 1.0f);
}

// ---------------------------------------------------------------------------
// Микшер
// ---------------------------------------------------------------------------
ENG_TEST(Audio, MixVolumeScalesPeak) {
    ResetAudio();
    Audio& a = Audio::Get();
    const AudioClip tone = AudioClip::MakeTone(440.0f, 0.5f, 44100.0f, 2);

    PlayParams loud;
    loud.volume = 1.0f;
    const VoiceId idLoud = a.Play(tone, loud);
    ENG_CHECK(idLoud != kInvalidVoice);
    const std::vector<f32> loudOut = MixFrames(a, 1024);
    const f32 loudPeak = PeakOf(loudOut);

    a.StopAll();
    PlayParams quiet;
    quiet.volume = 0.5f;
    ENG_CHECK(a.Play(tone, quiet) != kInvalidVoice);
    const std::vector<f32> quietOut = MixFrames(a, 1024);
    const f32 quietPeak = PeakOf(quietOut);

    ENG_CHECK_GT(loudPeak, 0.5f);
    ENG_CHECK_NEAR(quietPeak, loudPeak * 0.5f, 1e-3f);

    // Общая громкость умножает всё.
    a.SetMasterVolume(0.25f);
    a.StopAll();
    ENG_CHECK(a.Play(tone, loud) != kInvalidVoice);
    const f32 masterPeak = PeakOf(MixFrames(a, 1024));
    ENG_CHECK_NEAR(masterPeak, loudPeak * 0.25f, 1e-3f);
    a.SetMasterVolume(1.0f);
    a.StopAll();
}

ENG_TEST(Audio, MixPanSeparatesChannels) {
    ResetAudio();
    Audio& a = Audio::Get();
    const AudioClip tone = AudioClip::MakeTone(440.0f, 0.5f, 44100.0f, 2);

    PlayParams left;
    left.pan = -1.0f;
    left.volume = 0.8f;
    ENG_CHECK(a.Play(tone, left) != kInvalidVoice);
    const std::vector<f32> leftOut = MixFrames(a, 1024);
    ENG_CHECK_GT(PeakOfChannel(leftOut, 0, 2), 0.5f);
    ENG_CHECK_NEAR(PeakOfChannel(leftOut, 1, 2), 0.0f, 1e-5f);
    ENG_CHECK_GT(PeakOfChannel(leftOut, 0, 2), PeakOfChannel(leftOut, 1, 2));

    a.StopAll();
    PlayParams right;
    right.pan = 1.0f;
    right.volume = 0.8f;
    ENG_CHECK(a.Play(tone, right) != kInvalidVoice);
    const std::vector<f32> rightOut = MixFrames(a, 1024);
    ENG_CHECK_NEAR(PeakOfChannel(rightOut, 0, 2), 0.0f, 1e-5f);
    ENG_CHECK_GT(PeakOfChannel(rightOut, 1, 2), 0.5f);

    a.StopAll();
    PlayParams centre;
    centre.volume = 0.8f;
    ENG_CHECK(a.Play(tone, centre) != kInvalidVoice);
    const std::vector<f32> centreOut = MixFrames(a, 1024);
    ENG_CHECK_NEAR(PeakOfChannel(centreOut, 0, 2), PeakOfChannel(centreOut, 1, 2), 1e-6f);
    a.StopAll();
}

ENG_TEST(Audio, MixPitchChangesRate) {
    ResetAudio();
    Audio& a = Audio::Get();
    const AudioClip tone = AudioClip::MakeTone(440.0f, 0.5f, 44100.0f, 2);

    PlayParams normal;
    normal.volume = 0.8f;
    ENG_CHECK(a.Play(tone, normal) != kInvalidVoice);
    const std::vector<f32> normalOut = MixFrames(a, 2048);
    const int normalCrossings = ZeroCrossings(normalOut, 0, 2);

    a.StopAll();
    PlayParams fast = normal;
    fast.pitch = 2.0f;
    ENG_CHECK(a.Play(tone, fast) != kInvalidVoice);
    const std::vector<f32> fastOut = MixFrames(a, 2048);
    const int fastCrossings = ZeroCrossings(fastOut, 0, 2);

    ENG_CHECK_GT(normalCrossings, 20);
    ENG_CHECK_GT(fastCrossings, normalCrossings * 3 / 2);
    ENG_CHECK(fastOut != normalOut);
    a.StopAll();
}

ENG_TEST(Audio, LoopKeepsVoiceAlive) {
    ResetAudio();
    Audio& a = Audio::Get();
    const AudioClip shortClip = AudioClip::MakeTone(220.0f, 0.01f, 44100.0f, 2);  // 441 frames

    PlayParams loop;
    loop.looping = true;
    loop.volume = 0.8f;
    const VoiceId loopId = a.Play(shortClip, loop);
    ENG_CHECK(loopId != kInvalidVoice);
    const std::vector<f32> loopOut = MixFrames(a, 4096);  // ~9x длины клипа
    ENG_CHECK(a.IsPlaying(loopId));
    ENG_CHECK_EQ(a.ActiveVoices(), 1);
    ENG_CHECK_GT(PeakOf(loopOut), 0.1f);
    ENG_CHECK_GT(a.VoiceTime(loopId), 0.0f);
    a.Stop(loopId);
    ENG_CHECK(!a.IsPlaying(loopId));

    PlayParams once;
    once.volume = 0.8f;
    const VoiceId onceId = a.Play(shortClip, once);
    ENG_CHECK(onceId != kInvalidVoice);
    MixFrames(a, 4096);
    ENG_CHECK(!a.IsPlaying(onceId));
    ENG_CHECK_EQ(a.ActiveVoices(), 0);
}

ENG_TEST(Audio, StopFreesVoiceAndFades) {
    ResetAudio();
    Audio& a = Audio::Get();
    const AudioClip tone = AudioClip::MakeTone(440.0f, 1.0f, 44100.0f, 2);

    const VoiceId id = a.Play(tone);
    ENG_CHECK(id != kInvalidVoice);
    ENG_CHECK(a.IsPlaying(id));
    ENG_CHECK_EQ(a.ActiveVoices(), 1);
    MixFrames(a, 256);
    ENG_CHECK(a.Stop(id));
    ENG_CHECK(!a.IsPlaying(id));
    ENG_CHECK_EQ(a.ActiveVoices(), 0);
    ENG_CHECK(!a.Stop(id));  // повторная остановка - no-op

    // Затухание держит голос живым, пока фейд не завершится.
    const VoiceId faded = a.Play(tone);
    ENG_CHECK(faded != kInvalidVoice);
    ENG_CHECK(a.Stop(faded, 0.05f));  // 2205 frames
    ENG_CHECK(a.IsPlaying(faded));
    MixFrames(a, 512);
    ENG_CHECK(a.IsPlaying(faded));
    MixFrames(a, 4096);
    ENG_CHECK(!a.IsPlaying(faded));
    ENG_CHECK_EQ(a.ActiveVoices(), 0);

    // Нарастание плавно поднимает голос за запрошенное время.
    a.StopAll();
    PlayParams fadeIn;
    fadeIn.fadeIn = 0.05f;  // 2205 frames
    fadeIn.looping = true;
    ENG_CHECK(a.Play(tone, fadeIn) != kInvalidVoice);
    const f32 earlyPeak = PeakOf(MixFrames(a, 128));
    const f32 laterPeak = PeakOf(MixFrames(a, 4096));
    ENG_CHECK_GT(laterPeak, 0.6f);
    ENG_CHECK(earlyPeak < laterPeak * 0.2f);
    a.StopAll();

    // Pause/Resume не теряет голос.
    const VoiceId paused = a.Play(tone);
    a.Pause(paused);
    MixFrames(a, 256);
    ENG_CHECK(a.IsPlaying(paused));
    a.Resume(paused);
    MixFrames(a, 64);
    ENG_CHECK(a.IsPlaying(paused));
    a.StopAll();
    ENG_CHECK_EQ(a.ActiveVoices(), 0);
}

ENG_TEST(Audio, BusVolumeAffectsOutput) {
    ResetAudio();
    Audio& a = Audio::Get();
    const int sfx = a.AddBus("sfx", 0);
    ENG_CHECK_GT(sfx, 0);
    ENG_CHECK_EQ(a.AddBus("sfx", 0), sfx);  // идемпотентно
    ENG_CHECK_NEAR(a.BusVolume("sfx"), 1.0f, 1e-6f);

    const AudioClip tone = AudioClip::MakeTone(440.0f, 0.5f, 44100.0f, 2);
    PlayParams params;
    params.volume = 1.0f;
    params.bus = sfx;

    ENG_CHECK(a.Play(tone, params) != kInvalidVoice);
    const f32 full = PeakOf(MixFrames(a, 1024));

    a.StopAll();
    a.SetBusVolume("sfx", 0.5f);
    ENG_CHECK_NEAR(a.BusVolume("sfx"), 0.5f, 1e-6f);
    ENG_CHECK(a.Play(tone, params) != kInvalidVoice);
    const f32 half = PeakOf(MixFrames(a, 1024));
    ENG_CHECK_NEAR(half, full * 0.5f, 1e-3f);

    a.StopAll();
    a.SetBusMuted("sfx", true);
    ENG_CHECK(a.Play(tone, params) != kInvalidVoice);
    ENG_CHECK_NEAR(PeakOf(MixFrames(a, 1024)), 0.0f, 1e-6f);

    // Заглушенный родитель глушит и дочернюю шину.
    a.SetBusMuted("sfx", false);
    const int ui = a.AddBus("sfx.ui", sfx);
    ENG_CHECK_GT(ui, 0);
    a.SetBusVolume("sfx", 0.25f);
    PlayParams child = params;
    child.bus = ui;
    a.StopAll();
    ENG_CHECK(a.Play(tone, child) != kInvalidVoice);
    const f32 childPeak = PeakOf(MixFrames(a, 1024));
    ENG_CHECK_NEAR(childPeak, full * 0.25f, 1e-3f);

    a.SetBusMuted("sfx", true);
    a.StopAll();
    ENG_CHECK(a.Play(tone, child) != kInvalidVoice);
    ENG_CHECK_NEAR(PeakOf(MixFrames(a, 1024)), 0.0f, 1e-6f);

    // StopBus затрагивает только голоса, идущие через неё.
    a.SetBusMuted("sfx", false);
    a.SetBusVolume("sfx", 1.0f);
    a.StopAll();
    PlayParams flat;
    flat.bus = 0;
    const VoiceId childVoice = a.Play(tone, child);
    const VoiceId masterVoice = a.Play(tone, flat);
    ENG_CHECK(childVoice != kInvalidVoice);
    ENG_CHECK(masterVoice != kInvalidVoice);
    a.StopBus("sfx");
    ENG_CHECK(!a.IsPlaying(childVoice));
    ENG_CHECK(a.IsPlaying(masterVoice));
    a.StopAll();
    a.SetBusVolume("sfx", 1.0f);
    a.SetBusMuted("sfx", false);
}

ENG_TEST(Audio, SpatialDistanceAttenuation) {
    ResetAudio();
    Audio& a = Audio::Get();
    const AudioClip tone = AudioClip::MakeTone(440.0f, 0.5f, 44100.0f, 2);

    PlayParams params;
    params.spatial = true;
    params.volume = 1.0f;
    params.minDistance = 1.0f;
    params.maxDistance = 30.0f;
    params.rolloff = 1;  // обратная
    params.looping = true;

    params.position = Vec3{0, 0, -1};  // прямо впереди, на minDistance
    ENG_CHECK(a.Play(tone, params) != kInvalidVoice);
    const std::vector<f32> nearOut = MixFrames(a, 1024);
    const f32 nearPeak = PeakOf(nearOut);
    ENG_CHECK_GT(nearPeak, 0.6f);
    // Прямо впереди остаётся по центру.
    ENG_CHECK_NEAR(PeakOfChannel(nearOut, 0, 2), PeakOfChannel(nearOut, 1, 2), 1e-5f);

    a.StopAll();
    params.position = Vec3{0, 0, -10};
    ENG_CHECK(a.Play(tone, params) != kInvalidVoice);
    const f32 farPeak = PeakOf(MixFrames(a, 1024));
    ENG_CHECK_GT(nearPeak, farPeak * 2.0f);
    ENG_CHECK_NEAR(farPeak, nearPeak * 0.1f, 0.02f);

    // За maxDistance голос неслышен.
    a.StopAll();
    params.position = Vec3{0, 0, -100};
    ENG_CHECK(a.Play(tone, params) != kInvalidVoice);
    ENG_CHECK_NEAR(PeakOf(MixFrames(a, 1024)), 0.0f, 1e-6f);

    // Источник справа панорамируется вправо (вперёд у слушателя -Z, вверх +Y).
    a.StopAll();
    params.position = Vec3{1, 0, -1};
    ENG_CHECK(a.Play(tone, params) != kInvalidVoice);
    const std::vector<f32> sideOut = MixFrames(a, 1024);
    ENG_CHECK_GT(PeakOfChannel(sideOut, 1, 2), PeakOfChannel(sideOut, 0, 2));

    // Rolloff 0 игнорирует расстояние (до maxDistance).
    a.StopAll();
    params.rolloff = 0;
    params.position = Vec3{0, 0, -10};
    ENG_CHECK(a.Play(tone, params) != kInvalidVoice);
    ENG_CHECK_NEAR(PeakOf(MixFrames(a, 1024)), nearPeak, 0.02f);

    // Линейный rolloff в середине даёт ~половину громкости.
    a.StopAll();
    params.rolloff = 2;
    params.position = Vec3{0, 0, -15.5f};
    ENG_CHECK(a.Play(tone, params) != kInvalidVoice);
    const f32 midPeak = PeakOf(MixFrames(a, 1024));
    ENG_CHECK_GT(midPeak, nearPeak * 0.3f);
    ENG_CHECK(midPeak < nearPeak * 0.7f);
    a.StopAll();
}

ENG_TEST(Audio, MixIsDeterministic) {
    const AudioClip tone = AudioClip::MakeTone(440.0f, 0.25f, 44100.0f, 2);
    const AudioClip noise = AudioClip::MakeNoise(0.25f, 44100.0f, 2);

    auto run = [&](std::vector<f32>* out) {
        ResetAudio();
        Audio& a = Audio::Get();
        PlayParams p;
        p.volume = 0.7f;
        p.pan = -0.35f;
        p.looping = true;
        a.Play(tone, p);
        PlayParams spatial;
        spatial.spatial = true;
        spatial.position = Vec3{2, 0, -4};
        spatial.rolloff = 1;
        spatial.volume = 0.4f;
        spatial.looping = true;
        a.Play(noise, spatial);
        a.SetMasterVolume(0.9f);
        *out = MixFrames(a, 512);
        a.StopAll();
    };

    std::vector<f32> first, second;
    run(&first);
    run(&second);
    ENG_CHECK_EQ(first.size(), second.size());
    ENG_CHECK(first == second);

    // Тот же буфер, смешанный дважды подряд, не должен меняться (без аллокаций,
    // без скрытого состояния кроме позиции воспроизведения).
    std::vector<f32> third;
    run(&third);
    ENG_CHECK(first == third);
}

ENG_TEST(Audio, LoudMixDoesNotWrap) {
    ResetAudio();
    Audio& a = Audio::Get();
    const AudioClip tone = AudioClip::MakeTone(440.0f, 0.5f, 44100.0f, 2);

    // 32 одинаковых голоса полной амплитуды по центру: лимитер должен удержать
    // результат внутри [-1, 1] вместо заворачивания.
    PlayParams params;
    params.volume = 4.0f;
    for (int i = 0; i < 32; ++i) ENG_CHECK(a.Play(tone, params) != kInvalidVoice);
    const std::vector<f32> out = MixFrames(a, 512);
    f32 peak = 0.0f;
    bool finite = true;
    for (f32 v : out) {
        peak = std::max(peak, std::fabs(v));
        if (!std::isfinite(v)) finite = false;
    }
    ENG_CHECK(finite);
    ENG_CHECK(peak <= 1.0f);
    ENG_CHECK_GT(peak, 0.9f);
    ENG_CHECK_GT(a.GetStats().mixedFrames, 0u);
    ENG_CHECK_EQ(a.GetStats().maxVoices, 64);
    a.StopAll();
}

// ---------------------------------------------------------------------------
// Тихий / независимый от устройства путь
// ---------------------------------------------------------------------------
ENG_TEST(Audio, SilentFallbackInitAndUpdate) {
    ResetAudio();
    Audio& a = Audio::Get();

    const bool ok = a.Init(44100, 2, 512);
    ENG_CHECK(ok);
    ENG_CHECK(a.Initialised());
    ENG_CHECK_EQ(a.SampleRate(), 44100);
    ENG_CHECK_EQ(a.Channels(), 2);
    // Когда бэкенд не скомпилирован, движок должен быть тихим; когда есть,
    // устройство могло как подняться, так и нет (headless CI).
    if (!Audio::PlatformAudioAvailable()) ENG_CHECK(a.Silent());

    const AudioClip tone = AudioClip::MakeTone(440.0f, 0.5f, 44100.0f, 2);
    const VoiceId id = a.Play(tone);
    ENG_CHECK(id != kInvalidVoice);

    // PumpSilent смешивает в черновой буфер, поэтому голоса идут вперёд без устройства.
    a.PumpSilent(256);
    const f32 t0 = a.VoiceTime(id);
    ENG_CHECK_GT(t0, 0.0f);
    a.PumpSilent(256);
    ENG_CHECK_GT(a.VoiceTime(id), t0);

    // Update() гоняет микшер только когда реальное устройство не работает.
    const f32 before = a.VoiceTime(id);
    a.Update(0.05f);  // 2205 frames
    if (a.Silent()) ENG_CHECK_GT(a.VoiceTime(id), before);

    a.Update(-1.0f);  // должно игнорироваться
    a.StopAll();
    a.Shutdown();
    ENG_CHECK(!a.Initialised());
    ENG_CHECK(a.Silent());
    ENG_CHECK_EQ(a.ActiveVoices(), 0);
}

ENG_TEST(Audio, SilentModeForcedByEnv) {
    ResetAudio();
#if !defined(_WIN32)
    // ENG_AUDIO_NULL принудительно включает переносимый null-бэкенд, чтобы CI оставался headless.
    Audio& a = Audio::Get();
    setenv("ENG_AUDIO_NULL", "1", 1);
    ENG_CHECK(a.Init(44100, 2, 256));
    ENG_CHECK(a.Initialised());
    ENG_CHECK(a.Silent());
    const AudioClip tone = AudioClip::MakeTone(440.0f, 0.1f, 44100.0f, 2);
    ENG_CHECK(a.Play(tone) != kInvalidVoice);
    a.Update(0.02f);
    ENG_CHECK_GT(a.GetStats().mixedFrames, 0u);
    a.StopAll();
    a.Shutdown();
    unsetenv("ENG_AUDIO_NULL");
#endif
}

// Конверсия частоты дискретизации устройство/клип: клип 44100 Гц на устройстве
// 22050 Гц должен расходоваться вдвое быстрее в выходных кадрах.
ENG_TEST(Audio, DeviceRateConversion) {
    ResetAudio();
#if !defined(_WIN32)
    setenv("ENG_AUDIO_NULL", "1", 1);
#endif
    Audio& a = Audio::Get();
    ENG_CHECK(a.Init(22050, 2, 512));
    ENG_CHECK_EQ(a.SampleRate(), 22050);

    const AudioClip tone = AudioClip::MakeTone(440.0f, 0.25f, 44100.0f, 2);  // 11025 frames
    PlayParams params;
    params.volume = 0.8f;
    const VoiceId id = a.Play(tone, params);
    ENG_CHECK(id != kInvalidVoice);
    MixFrames(a, 4096);  // 2 clip frames per output frame -> 8192 of 11025
    ENG_CHECK(a.IsPlaying(id));
    MixFrames(a, 2048);  // 12288 > 11025
    ENG_CHECK(!a.IsPlaying(id));

    // Тон 440 Гц всё ещё пересекает ноль ~88 раз за 0.1 с времени устройства.
    const VoiceId id2 = a.Play(tone, params);
    ENG_CHECK(id2 != kInvalidVoice);
    const std::vector<f32> out = MixFrames(a, 2205);
    const int crossings = ZeroCrossings(out, 0, 2);
    ENG_CHECK_GT(crossings, 80);
    ENG_CHECK(crossings < 96);
    ENG_CHECK_GT(PeakOf(out), 0.5f);
    a.StopAll();

    // Оставляем синглтон на формате устройства по умолчанию для других наборов тестов.
    a.Shutdown();
#if !defined(_WIN32)
    a.Init(44100, 2, 1024);
    unsetenv("ENG_AUDIO_NULL");
#endif
    a.Shutdown();
}
