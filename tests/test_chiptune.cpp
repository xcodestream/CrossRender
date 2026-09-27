// Тесты модуля chiptune: генерация волновых форм, помощники нот,
// детерминированные встроенные песни, секвенсор трекера (тайминг, прыжки по order,
// зацикливание, мьют по каналам), аддитивный рендер, характер каждой волны, эхо-
// хвост SPC, Bake(), сериализация туда-обратно и производительность рендера.
#include <vector>

#include "crossrender/audio/Chiptune.h"

#include "crossrender/core/File.h"
#include "crossrender/core/Time.h"
#include "crossrender/test/Test.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

using namespace crossrender;

namespace {

// ---------------------------------------------------------------------------
// Статистика сигнала
// ---------------------------------------------------------------------------
struct Stats {
    f64 peak = 0.0;
    f64 rms = 0.0;
    f64 zcr = 0.0;         // пересечений нуля на сэмпл (канал 0)
    f64 brightness = 0.0;  // rms(first difference) / rms  (примерно спектральный центроид)
    bool finite = true;
    usize frames = 0;
};

Stats Analyse(const std::vector<f32>& interleaved, int channels = 2) {
    Stats st;
    if (interleaved.empty() || channels <= 0) return st;
    const usize frames = interleaved.size() / static_cast<usize>(channels);
    st.frames = frames;
    double sum2 = 0.0, diff2 = 0.0;
    usize crossings = 0;
    f64 prev = 0.0;
    bool havePrev = false;
    for (usize f = 0; f < frames; ++f) {
        const f32 v = interleaved[f * static_cast<usize>(channels)];
        if (!std::isfinite(v)) st.finite = false;
        st.peak = std::max(st.peak, std::fabs(static_cast<f64>(v)));
        sum2 += static_cast<double>(v) * static_cast<double>(v);
        if (havePrev) {
            const double d = static_cast<double>(v) - prev;
            diff2 += d * d;
            if ((v >= 0.0f) != (prev >= 0.0)) ++crossings;
        }
        prev = static_cast<f64>(v);
        havePrev = true;
    }
    if (frames > 0) {
        st.rms = std::sqrt(sum2 / static_cast<double>(frames));
        st.zcr = static_cast<f64>(crossings) / static_cast<f64>(frames);
        const double drms = std::sqrt(diff2 / static_cast<double>(frames));
        st.brightness = st.rms > 1e-9 ? drms / st.rms : 0.0;
    }
    return st;
}

// ---------------------------------------------------------------------------
// Помощники рендеринга
// ---------------------------------------------------------------------------
std::vector<f32> RenderSong(const ChipSong& song, f32 seconds, int sampleRate = 44100,
                            int muteChannel = -2, bool allowLoop = true) {
    ChipPlayer p;
    p.SetSong(song);
    p.SetSampleRate(sampleRate);
    if (!allowLoop) p.SetLoop(false);
    if (muteChannel >= 0) p.SetChannelMute(muteChannel, true);
    p.Play();
    std::vector<f32> buf;
    p.RenderTo(&buf, static_cast<int>(std::lround(static_cast<f64>(seconds) * sampleRate)));
    return buf;
}

f64 WindowRms(const std::vector<f32>& s, f32 fromSeconds, f32 toSeconds, int sampleRate = 44100) {
    const usize a = static_cast<usize>(std::max(0.0, static_cast<double>(fromSeconds) * sampleRate));
    usize b = static_cast<usize>(std::max(0.0, static_cast<double>(toSeconds) * sampleRate)) * 2;
    if (b > s.size()) b = s.size();
    usize a2 = a * 2;
    if (a2 >= b) return 0.0;
    double sum2 = 0.0;
    usize n = 0;
    for (usize i = a2; i < b; ++i) {
        sum2 += static_cast<double>(s[i]) * static_cast<double>(s[i]);
        ++n;
    }
    return n ? std::sqrt(sum2 / static_cast<double>(n)) : 0.0;
}

// Минимальная одноканальная песня для тестов генератора/секвенсора.
ChipSong MakeOneChannelSong(ChipType chip, ChipWave wave, int note = 57, int rows = 16,
                            int tpr = 6, int fps = 60) {
    ChipSong s;
    s.title = "One";
    s.chip = chip;
    s.channelCount = 1;
    s.framesPerSecond = fps;
    s.rowsPerBeat = 4;
    s.ticksPerRow = tpr;
    s.beatsPerMinute = fps * 60 / (tpr * 4);
    s.loopOrder = 0;
    ChipChannelDef c;
    c.wave = wave;
    c.volume = 12;
    c.attack = 0;
    c.decay = 2;
    c.sustain = 12;
    c.release = 5;
    s.channels.push_back(c);
    ChipPattern p;
    p.name = "W";
    p.Resize(rows, 1);
    p.At(0, 0).semitone = static_cast<i16>(note);
    s.AddPattern(p);
    s.order.push_back(0);
    return s;
}

ChipSong MakeTinySong(int rows, int channels, int tpr, int fps, int patterns = 1,
                      int loopOrder = 0) {
    ChipSong s;
    s.title = "Tiny";
    s.chip = ChipType::Nes2A03;
    s.channelCount = channels;
    s.framesPerSecond = fps;
    s.rowsPerBeat = 4;
    s.ticksPerRow = tpr;
    s.beatsPerMinute = fps * 60 / (tpr * 4);
    s.loopOrder = loopOrder;
    for (int i = 0; i < channels; ++i) {
        ChipChannelDef c;
        c.wave = (i == 0) ? ChipWave::Pulse50 : (i == 1 ? ChipWave::Pulse25 : ChipWave::Triangle);
        c.volume = 12;
        c.attack = 0;
        c.decay = 2;
        c.sustain = 12;
        c.release = 3;
        s.channels.push_back(c);
    }
    for (int pi = 0; pi < patterns; ++pi) {
        ChipPattern p;
        p.name = "P" + std::to_string(pi);
        p.Resize(rows, channels);
        for (int r = 0; r < rows; ++r) {
            for (int ch = 0; ch < channels; ++ch) {
                ChipNote& n = p.At(r, ch);
                n.semitone = static_cast<i16>(45 + ch * 7 + (r % 5) + pi * 2);
                n.volume = 0xFF;
                n.arp = 0xFF;
            }
        }
        s.AddPattern(p);
        s.order.push_back(pi);
    }
    return s;
}

// ---------------------------------------------------------------------------
// Обвязка колбэка строк
// ---------------------------------------------------------------------------
int g_rowCalls = 0;
int g_userCalls = 0;
int g_lastOrder = -1;
int g_lastRow = -1;

void RowCb(int order, int row, void* user) {
    ++g_rowCalls;
    g_lastOrder = order;
    g_lastRow = row;
    if (user) ++(*static_cast<int*>(user));
}

}  // namespace

// ---------------------------------------------------------------------------
// MakeSingleCycleWaveform
// ---------------------------------------------------------------------------
ENG_TEST(Chiptune, WaveformShapes) {
    const ChipWave periodic[] = {ChipWave::Pulse12, ChipWave::Pulse25, ChipWave::Pulse50,
                                 ChipWave::Pulse75, ChipWave::Triangle, ChipWave::Saw,
                                 ChipWave::Sine,    ChipWave::Square,   ChipWave::Sample};
    const int n = 128;

    for (ChipWave w : periodic) {
        const std::vector<f32> a = MakeSingleCycleWaveform(w, n, 0.0f);
        ENG_CHECK_EQ(static_cast<int>(a.size()), n);

        f64 mean = 0.0, peak = 0.0;
        for (f32 v : a) {
            ENG_CHECK(std::isfinite(v));
            mean += v;
            peak = std::max(peak, std::fabs(static_cast<f64>(v)));
        }
        mean /= static_cast<f64>(a.size());
        ENG_CHECK_MSG(std::fabs(mean) < 1e-3, "waveform must be DC free");
        ENG_CHECK_MSG(peak <= 1.0 + 1e-4, "waveform peak must not exceed 1");
        ENG_CHECK_MSG(peak > 0.9, "waveform should be normalised to a peak of 1");

        // Период: та же волна, построенная с вдвое большим числом сэмплов, должна
        // содержать исходные значения на каждом чётном индексе, то есть одна запись
        // таблицы - ровно один период.
        const std::vector<f32> b = MakeSingleCycleWaveform(w, n * 2, 0.0f);
        ENG_CHECK_EQ(static_cast<int>(b.size()), n * 2);
        for (int i = 0; i < n; ++i)
            ENG_CHECK_NEAR(b[static_cast<usize>(i) * 2], a[static_cast<usize>(i)], 1e-4);
    }

    // Шум намеренно непериодичен, но должен оставаться корректным.
    const std::vector<f32> noise = MakeSingleCycleWaveform(ChipWave::Noise, 128, 0.0f);
    ENG_CHECK_EQ(static_cast<int>(noise.size()), 128);
    f64 nmean = 0.0, npeak = 0.0;
    for (f32 v : noise) {
        ENG_CHECK(std::isfinite(v));
        nmean += v;
        npeak = std::max(npeak, std::fabs(static_cast<f64>(v)));
    }
    nmean /= 128.0;
    ENG_CHECK(std::fabs(nmean) < 1e-3);
    ENG_CHECK(npeak <= 1.0 + 1e-4);
    ENG_CHECK(npeak > 0.9);

    // Фаза сдвигает волну, и у каждой волны своя форма.
    const std::vector<f32> sine0 = MakeSingleCycleWaveform(ChipWave::Sine, 64, 0.0f);
    const std::vector<f32> sineQ = MakeSingleCycleWaveform(ChipWave::Sine, 64, 0.25f);
    f64 diff = 0.0;
    for (usize i = 0; i < sine0.size(); ++i) diff += std::fabs(sine0[i] - sineQ[i]);
    ENG_CHECK_MSG(diff > 10.0, "phase must shift the waveform");

    // Вырожденные входы не должны приводить к падению.
    ENG_CHECK(MakeSingleCycleWaveform(ChipWave::Sine, 0, 0.0f).empty());
    ENG_CHECK(MakeSingleCycleWaveform(ChipWave::Sine, -4, 0.0f).empty());
    const std::vector<f32> single = MakeSingleCycleWaveform(ChipWave::Sine, 2, 0.0f);
    ENG_CHECK_EQ(static_cast<int>(single.size()), 2);
}

// ---------------------------------------------------------------------------
// Помощники нот
// ---------------------------------------------------------------------------
ENG_TEST(Chiptune, NoteHelpers) {
    ENG_CHECK(ChipPlayer::NoteFrequency(57) == 440.0f);
    ENG_CHECK_NEAR(ChipPlayer::NoteFrequency(69), 880.0, 0.01);
    ENG_CHECK_NEAR(ChipPlayer::NoteFrequency(45), 220.0, 0.01);
    ENG_CHECK_NEAR(ChipPlayer::NoteFrequency(48), 261.6256, 0.01);
    ENG_CHECK_NEAR(ChipPlayer::NoteFrequency(0), 16.3516, 0.01);
    ENG_CHECK(ChipPlayer::NoteFrequency(-1) == 0.0f);

    ENG_CHECK_STR_EQ(ChipPlayer::NoteName(0), "C-0");
    ENG_CHECK_STR_EQ(ChipPlayer::NoteName(48), "C-4");
    ENG_CHECK_STR_EQ(ChipPlayer::NoteName(57), "A-4");
    ENG_CHECK_STR_EQ(ChipPlayer::NoteName(46), "A#3");
    ENG_CHECK_STR_EQ(ChipPlayer::NoteName(61), "C#5");
    ENG_CHECK_STR_EQ(ChipPlayer::NoteName(-1), "---");
    ENG_CHECK_STR_EQ(ChipPlayer::NoteName(-2), "OFF");
    ENG_CHECK_STR_EQ(ChipPlayer::NoteName(127), "G-10");

    // Прогоняем каждый полутон через публичный текстовый формат (это единственный
    // публичный парсер имён нот).
    ChipSong s;
    s.title = "Names";
    s.chip = ChipType::Nes2A03;
    s.channelCount = 1;
    s.framesPerSecond = 60;
    s.rowsPerBeat = 4;
    s.ticksPerRow = 6;
    s.beatsPerMinute = 150;
    s.loopOrder = 0;
    s.channels.push_back(ChipChannelDef{});
    ChipPattern p;
    p.name = "Names";
    p.Resize(128, 1);
    for (int i = 0; i < 128; ++i) {
        p.At(i, 0).semitone = static_cast<i16>(i);
        p.At(i, 0).volume = 0xFF;
        p.At(i, 0).arp = 0xFF;
    }
    s.AddPattern(p);
    s.order.push_back(0);

    const std::string text = ChipPlayer::Serialize(s);
    ChipSong back;
    std::string err;
    ENG_CHECK_MSG(ChipPlayer::Deserialize(text, &back, &err), err.c_str());
    ENG_CHECK_EQ(back.PatternCount(), 1);
    ENG_CHECK_EQ(back.patterns[0].rowCount, 128);
    for (int i = 0; i < 128; ++i)
        ENG_CHECK_EQ(static_cast<int>(back.patterns[0].At(i, 0).semitone), i);
}

// ---------------------------------------------------------------------------
// Встроенные песни
// ---------------------------------------------------------------------------
ENG_TEST(Chiptune, BuiltinSongs) {
    const std::vector<std::string> names = ChipPlayer::BuiltinNames();
    ENG_CHECK_EQ(static_cast<int>(names.size()), 5);
    ENG_CHECK_STR_EQ(names[0], "8bit");
    ENG_CHECK_STR_EQ(names[1], "16bit");
    ENG_CHECK_STR_EQ(names[2], "boss");
    ENG_CHECK_STR_EQ(names[3], "dance");
    ENG_CHECK_STR_EQ(names[4], "title");

    for (const std::string& name : names) {
        const ChipSong a = ChipPlayer::MakeNamed(name, 7);
        const ChipSong b = ChipPlayer::MakeNamed(name, 7);
        ENG_CHECK_MSG(a.PatternCount() > 0, name.c_str());
        ENG_CHECK_MSG(a.channelCount >= 3, name.c_str());
        ENG_CHECK_MSG(static_cast<int>(a.channels.size()) >= 3, name.c_str());
        ENG_CHECK_MSG(!a.order.empty(), name.c_str());
        ENG_CHECK_MSG(a.TotalRows() > 0, name.c_str());
        const f32 dur = a.DurationSeconds(-1);
        ENG_CHECK_MSG(dur >= 8.0f && dur <= 60.0f, name.c_str());
        // Каждый паттерн полностью размечен, у каждого канала есть аппаратная громкость.
        for (const ChipPattern& p : a.patterns) {
            ENG_CHECK_EQ(static_cast<int>(p.rows.size()), p.rowCount * p.channelCount);
            ENG_CHECK(p.rowCount > 0);
            ENG_CHECK(p.channelCount > 0);
        }
        for (const ChipChannelDef& c : a.channels)
            ENG_CHECK(c.volume >= 0 && c.volume <= 15);
        // Детерминировано при фиксированном seed.
        ENG_CHECK_STR_EQ(ChipPlayer::Serialize(a), ChipPlayer::Serialize(b));
        // DurationSeconds одного элемента order - префикс всей песни.
        if (a.order.size() > 1) {
            const f32 first = a.DurationSeconds(0);
            const f32 second = a.DurationSeconds(1);
            ENG_CHECK(first > second);
            const f64 firstRows =
                static_cast<f64>(a.patterns[static_cast<usize>(a.order[0])].rowCount);
            ENG_CHECK_NEAR(static_cast<f64>(first) - static_cast<f64>(second),
                           firstRows * static_cast<f64>(a.ticksPerRow) /
                               static_cast<f64>(a.framesPerSecond),
                           1e-3);
        }
    }

    // Поиск без учёта регистра и документированный 8-bit fallback.
    ENG_CHECK_STR_EQ(ChipPlayer::Serialize(ChipPlayer::MakeNamed("8BIT", 3)),
                     ChipPlayer::Serialize(ChipPlayer::Make8BitSong(3)));
    ENG_CHECK_STR_EQ(ChipPlayer::Serialize(ChipPlayer::MakeNamed("16Bit", 3)),
                     ChipPlayer::Serialize(ChipPlayer::Make16BitSong(3)));
    ENG_CHECK_STR_EQ(ChipPlayer::Serialize(ChipPlayer::MakeNamed("BOSS", 3)),
                     ChipPlayer::Serialize(ChipPlayer::MakeBossSong(3)));
    ENG_CHECK_STR_EQ(ChipPlayer::Serialize(ChipPlayer::MakeNamed("Dance", 3)),
                     ChipPlayer::Serialize(ChipPlayer::MakeDanceSong(3)));
    ENG_CHECK_STR_EQ(ChipPlayer::Serialize(ChipPlayer::MakeNamed("Title", 3)),
                     ChipPlayer::Serialize(ChipPlayer::MakeTitleSong(3)));
    ENG_CHECK_STR_EQ(ChipPlayer::Serialize(ChipPlayer::MakeNamed("nonsense", 3)),
                     ChipPlayer::Serialize(ChipPlayer::Make8BitSong(3)));

    // Другой seed меняет исполнение, но остаётся детерминированным.
    ENG_CHECK(ChipPlayer::Serialize(ChipPlayer::Make8BitSong(1)) !=
              ChipPlayer::Serialize(ChipPlayer::Make8BitSong(2)));

    // Тематические ожидания из контракта.
    ENG_CHECK(ChipPlayer::Make8BitSong().chip == ChipType::Nes2A03);
    ENG_CHECK(ChipPlayer::Make8BitSong().channelCount == 4);
    ENG_CHECK(ChipPlayer::MakeBossSong().beatsPerMinute >= 140);
    ENG_CHECK(ChipPlayer::MakeDanceSong().beatsPerMinute >= 120 &&
              ChipPlayer::MakeDanceSong().beatsPerMinute <= 128);
    ENG_CHECK(ChipPlayer::MakeTitleSong().beatsPerMinute <= 100);
    const ChipSong s16 = ChipPlayer::Make16BitSong();
    ENG_CHECK(s16.chip == ChipType::SnesSpc);
    ENG_CHECK_EQ(s16.channelCount, 8);
    ENG_CHECK(s16.echoEnabled);
    ENG_CHECK(s16.lowPass);
}

// ---------------------------------------------------------------------------
// Секвенсор
// ---------------------------------------------------------------------------
ENG_TEST(Chiptune, Sequencer) {
    const ChipSong song = MakeTinySong(16, 3, 6, 60);

    // Строка = ticksPerRow / framesPerSecond = 0.1 с.
    ChipPlayer p;
    p.SetSong(song);
    p.SetSampleRate(44100);
    ENG_CHECK(p.HasSong());
    p.Play();
    ENG_CHECK(p.IsPlaying());
    ENG_CHECK_EQ(p.CurrentOrder(), 0);
    ENG_CHECK_EQ(p.CurrentRow(), 0);

    p.Advance(0.5f);  // 30 тиков = ровно 5 строк
    ENG_CHECK_EQ(p.CurrentRow(), 5);
    ENG_CHECK_EQ(p.CurrentTick(), 0);
    p.Advance(0.12f);  // ещё 7 тиков
    ENG_CHECK_EQ(p.CurrentRow(), 6);
    ENG_CHECK_EQ(p.CurrentTick(), 1);
    p.Advance(0.28f);  // ещё 17 тиков -> строка 9, тик 0
    ENG_CHECK_EQ(p.CurrentRow(), 9);
    ENG_CHECK_EQ(p.CurrentTick(), 0);

    // SetOrder прыгает к началу элемента order.
    p.SetOrder(0);
    ENG_CHECK_EQ(p.CurrentOrder(), 0);
    ENG_CHECK_EQ(p.CurrentRow(), 0);

    // зацикливается на loopOrder.
    const ChipSong two = MakeTinySong(4, 2, 6, 60, 2, 0);
    ChipPlayer q;
    q.SetSong(two);
    q.SetSampleRate(44100);
    q.Play();
    q.Advance(1.0f);  // 60 тиков: 4+4 строки = 48 тиков, затем 12 тиков внутрь цикла
    ENG_CHECK_EQ(q.CurrentOrder(), 0);
    ENG_CHECK_EQ(q.CurrentRow(), 2);
    ENG_CHECK(q.IsPlaying());

    // С loopOrder = -1 песня играется один раз и останавливается.
    ChipSong once = two;
    once.loopOrder = -1;
    ChipPlayer r;
    r.SetSong(once);
    r.SetSampleRate(44100);
    r.SetLoop(false);
    r.Play();
    std::vector<f32> buf;
    r.RenderTo(&buf, static_cast<int>(44100 * 2.0f));
    ENG_CHECK_MSG(!r.IsPlaying(), "non looping song should stop at the end");
    ENG_CHECK_EQ(r.CurrentOrder(), 1);
    // Рендер после конца ничего не добавляет.
    std::vector<f32> after;
    r.RenderTo(&after, 512);
    const Stats st = Analyse(after);
    ENG_CHECK(st.rms < 1e-6);
    // Play() после конца перезапускает песню с начала.
    r.Play();
    ENG_CHECK(r.IsPlaying());
    ENG_CHECK_EQ(r.CurrentOrder(), 0);
    ENG_CHECK_EQ(r.CurrentRow(), 0);

    // SetSpeed меняет темп строк.
    ChipPlayer sp;
    sp.SetSong(song);
    sp.SetSampleRate(44100);
    sp.Play();
    sp.SetSpeed(12);
    sp.Advance(0.5f);  // 30 тиков / 12 = 2 строки
    ENG_CHECK_EQ(sp.CurrentRow(), 2);

    // Мьют глушит ровно один канал.
    const ChipSong both = MakeTinySong(16, 2, 6, 60);
    const std::vector<f32> all = RenderSong(both, 1.5f);
    const std::vector<f32> m0 = RenderSong(both, 1.5f, 44100, 0);
    const std::vector<f32> m1 = RenderSong(both, 1.5f, 44100, 1);
    const Stats allSt = Analyse(all);
    const Stats s0 = Analyse(m0);
    const Stats s1 = Analyse(m1);
    ENG_CHECK_MSG(allSt.rms > 0.05, "tiny song should be audible");
    ENG_CHECK_MSG(s0.rms < allSt.rms * 0.9, "muting channel 0 must reduce the level");
    ENG_CHECK_MSG(s1.rms < allSt.rms * 0.9, "muting channel 1 must reduce the level");
    ENG_CHECK_MSG(s0.rms > 1e-4, "unmuted channels must keep sounding");
    ENG_CHECK_MSG(s1.rms > 1e-4, "unmuted channels must keep sounding");
    ENG_CHECK_MSG(std::fabs(s0.rms - s1.rms) > 1e-3, "each channel contributes differently");

    std::vector<f32> bothMuted;
    {
        ChipPlayer m;
        m.SetSong(both);
        m.SetSampleRate(44100);
        m.SetChannelMute(0, true);
        ENG_CHECK(m.ChannelMuted(0));
        ENG_CHECK(!m.ChannelMuted(1));
        m.SetChannelMute(1, true);
        m.Play();
        m.RenderTo(&bothMuted, static_cast<int>(44100 * 1.5f));
    }
    ENG_CHECK_MSG(Analyse(bothMuted).rms < 1e-6, "muting every channel must be silent");
    // Мьюты вне диапазона игнорируются, а не фатальны.
    {
        ChipPlayer m;
        m.SetSong(both);
        m.SetChannelMute(-1, true);
        m.SetChannelMute(99, true);
        ENG_CHECK(!m.ChannelMuted(-1));
        ENG_CHECK(!m.ChannelMuted(99));
    }
}

// ---------------------------------------------------------------------------
// Аддитивный рендер
// ---------------------------------------------------------------------------
ENG_TEST(Chiptune, RenderAddIsAdditive) {
    const ChipSong song = MakeTinySong(32, 2, 6, 60);
    const int frames = 20000;
    std::vector<f32> base(static_cast<usize>(frames) * 2, 0.5f);
    std::vector<f32> zero(static_cast<usize>(frames) * 2, 0.0f);

    ChipPlayer pa;
    pa.SetSong(song);
    pa.SetSampleRate(44100);
    pa.Play();
    pa.RenderAdd(base.data(), frames);

    ChipPlayer pb;
    pb.SetSong(song);
    pb.SetSampleRate(44100);
    pb.Play();
    pb.RenderAdd(zero.data(), frames);

    int increased = 0;
    for (usize i = 0; i < base.size(); ++i) {
        ENG_CHECK(std::isfinite(base[i]));
        ENG_CHECK_NEAR(base[i], 0.5 + zero[i], 1e-5);
        if (base[i] > 0.5) ++increased;
    }
    ENG_CHECK_MSG(increased > 100, "RenderAdd must add audible content on top of the buffer");
    ENG_CHECK_MSG(Analyse(zero).rms > 0.01, "the chip output should not be silent");
    ENG_CHECK(Analyse(zero).peak <= 1.0 + 1e-6);
    ENG_CHECK(pa.LastPeak() > 0.01f);

    // Null / пустые вызовы - no-op.
    std::vector<f32> untouched(8, 0.25f);
    ChipPlayer pc;
    pc.SetSong(song);
    pc.SetSampleRate(44100);
    pc.Play();
    pc.RenderAdd(nullptr, 4);
    pc.RenderAdd(untouched.data(), 0);
    for (f32 v : untouched) ENG_CHECK_NEAR(v, 0.25, 1e-9);

    // Каждый чип и каждая волна должны реально давать конечный звук.
    const ChipType chips[] = {ChipType::Nes2A03, ChipType::GameBoy, ChipType::SnesSpc,
                              ChipType::SegaYM, ChipType::PcSpeaker};
    const ChipWave waves[] = {ChipWave::Pulse12, ChipWave::Pulse25, ChipWave::Pulse50,
                              ChipWave::Pulse75, ChipWave::Triangle, ChipWave::Saw,
                              ChipWave::Sine,    ChipWave::Square,   ChipWave::Noise,
                              ChipWave::Sample};
    for (ChipType chip : chips) {
        for (ChipWave wave : waves) {
            const ChipSong s = MakeOneChannelSong(chip, wave);
            const std::vector<f32> buf = RenderSong(s, 0.6f);
            const Stats st = Analyse(buf);
            ENG_CHECK_MSG(st.finite, ChipPlayer::NoteName(static_cast<int>(wave)).c_str());
            ENG_CHECK_MSG(st.peak <= 1.0 + 1e-6, "soft limiter must keep the peak in range");
            ENG_CHECK_MSG(st.rms > 1e-4, "every channel type must make sound");
        }
    }
}

// ---------------------------------------------------------------------------
// Характер каждой волны
// ---------------------------------------------------------------------------
ENG_TEST(Chiptune, WaveCharactersDiffer) {
    const ChipWave waves[] = {ChipWave::Pulse12, ChipWave::Pulse25, ChipWave::Pulse50,
                              ChipWave::Pulse75, ChipWave::Triangle, ChipWave::Saw,
                              ChipWave::Sine,    ChipWave::Square,   ChipWave::Noise,
                              ChipWave::Sample};
    const int count = static_cast<int>(sizeof(waves) / sizeof(waves[0]));
    std::vector<Stats> stats;
    for (int i = 0; i < count; ++i) {
        // Шумовые каналы трактуют ноту как индекс аппаратного периода, поэтому
        // для генератора шума берём яркий (2), а иначе обычную высоту.
        const int note = waves[i] == ChipWave::Noise ? 2 : 57;
        const ChipSong s = MakeOneChannelSong(ChipType::Nes2A03, waves[i], note, 64, 6, 60);
        const std::vector<f32> buf = RenderSong(s, 0.8f);
        const Stats st = Analyse(buf);
        ENG_CHECK_MSG(st.finite, "wave output must be finite");
        ENG_CHECK_MSG(st.rms > 1e-4, "wave output must be audible");
        stats.push_back(st);
    }

    // Несколько из десяти генераторов намеренно эквивалентны на реальном
    // железе: Square - это pulse с 50%, а импульсы с duty 25%/75% имеют тот же
    // амплитудный спектр (различаются только фазой и DC), что ровно повторяет
    // железо NES. Их таблицы всё же должны различаться.
    const std::vector<f32> p25 = MakeSingleCycleWaveform(ChipWave::Pulse25, 128, 0.0f);
    const std::vector<f32> p75 = MakeSingleCycleWaveform(ChipWave::Pulse75, 128, 0.0f);
    f64 tableDiff = 0.0;
    for (usize i = 0; i < p25.size(); ++i) tableDiff += std::fabs(p25[i] - p75[i]);
    ENG_CHECK_MSG(tableDiff > 10.0, "25% and 75% duty must not be the same table");

    // Различимые отпечатки (rms, brightness): десять чип-волн попадают в
    // восемь слышимых корзин (Pulse50 == Square, Pulse75 == инвертированный Pulse25).
    std::vector<long long> keys;
    for (const Stats& s : stats) {
        const long long k = static_cast<long long>(std::llround(s.rms * 400.0)) * 1000 +
                            static_cast<long long>(std::llround(s.brightness * 100.0));
        if (std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(k);
    }
    ENG_CHECK_MSG(keys.size() >= 7, "the chip waves must sound different");

    auto idx = [&](ChipWave w) {
        for (int i = 0; i < count; ++i)
            if (waves[i] == w) return i;
        return 0;
    };
    // Скважность меняет RMS импульсной волны.
    ENG_CHECK_MSG(std::fabs(stats[static_cast<usize>(idx(ChipWave::Pulse12))].rms -
                            stats[static_cast<usize>(idx(ChipWave::Pulse50))].rms) > 0.05,
                  "12.5% and 50% duty pulses must differ");
    ENG_CHECK_MSG(std::fabs(stats[static_cast<usize>(idx(ChipWave::Pulse12))].rms -
                            stats[static_cast<usize>(idx(ChipWave::Pulse25))].rms) > 0.05,
                  "12.5% and 25% duty pulses must differ");
    // Шум куда ярче (выше частота пересечений нуля), чем синус.
    ENG_CHECK(stats[static_cast<usize>(idx(ChipWave::Noise))].zcr >
              stats[static_cast<usize>(idx(ChipWave::Sine))].zcr * 2.0);
    // Пила ярче синуса; треугольник темнее импульса.
    ENG_CHECK(stats[static_cast<usize>(idx(ChipWave::Saw))].brightness >
              stats[static_cast<usize>(idx(ChipWave::Sine))].brightness);
    ENG_CHECK(stats[static_cast<usize>(idx(ChipWave::Triangle))].brightness <
              stats[static_cast<usize>(idx(ChipWave::Pulse50))].brightness);
    // 1-битный дельта-канал NES (Sample) тише и грубее импульса.
    ENG_CHECK(stats[static_cast<usize>(idx(ChipWave::Sample))].rms <
              stats[static_cast<usize>(idx(ChipWave::Pulse50))].rms);
    ENG_CHECK(stats[static_cast<usize>(idx(ChipWave::Sample))].rms > 1e-3);
}

// ---------------------------------------------------------------------------
// Характер 8-bit против 16-bit: эхо SPC оставляет длинный хвост.
// ---------------------------------------------------------------------------
ENG_TEST(Chiptune, SixteenBitEchoTail) {
    ChipSong s8 = ChipPlayer::Make8BitSong();
    ChipSong s16 = ChipPlayer::Make16BitSong();
    s8.loopOrder = -1;
    s16.loopOrder = -1;

    const f32 d8 = s8.DurationSeconds(-1);
    const f32 d16 = s16.DurationSeconds(-1);
    const f32 extra = 1.5f;

    const std::vector<f32> b8 = RenderSong(s8, d8 + extra, 44100, -2, false);
    const std::vector<f32> b16 = RenderSong(s16, d16 + extra, 44100, -2, false);

    // Короткое окно сразу после последней строки: 8-битная мелодия затихла
    // полностью, а эхо SPC всё ещё повторяется.
    const f64 tail8 = WindowRms(b8, d8 + 0.3f, d8 + 1.2f);
    const f64 tail16 = WindowRms(b16, d16 + 0.3f, d16 + 1.2f);
    ENG_CHECK_MSG(tail8 < 1e-5, "the 8-bit tune must stop dead after its release");
    ENG_CHECK_MSG(tail16 > 1e-4, "the 16-bit song must have an audible echo tail");
    ENG_CHECK_MSG(tail16 > tail8 * 10.0 + 1e-5,
                  "the SPC echo must ring out well past the last 8-bit note");

    // Хвост - это эхо, а не застрявшая нота: он затухает со временем.
    const f64 early = WindowRms(b16, d16 + 0.2f, d16 + 0.5f);
    const f64 late = WindowRms(b16, d16 + 1.0f, d16 + 1.5f);
    ENG_CHECK(early > 1e-3);
    ENG_CHECK(early > late * 4.0);

    // Обе песни различаются и внутри тела мелодии.
    const Stats a = Analyse(b8);
    const Stats b = Analyse(b16);
    ENG_CHECK(a.rms > 0.05 && b.rms > 0.02);
    ENG_CHECK(std::fabs(a.rms - b.rms) > 0.02);
}

// ---------------------------------------------------------------------------
// Bake
// ---------------------------------------------------------------------------
ENG_TEST(Chiptune, BakeProducesClip) {
    for (const std::string& name : ChipPlayer::BuiltinNames()) {
        const ChipSong song = ChipPlayer::MakeNamed(name, 5);
        ChipPlayer player;
        player.SetSong(song);
        const AudioClip clip = player.Bake(44100, 0.0f, 0.05f);
        ENG_CHECK_MSG(clip.Valid(), name.c_str());
        ENG_CHECK_EQ(clip.SampleRate(), 44100);
        ENG_CHECK_EQ(clip.Channels(), 2);
        const f32 expected = song.DurationSeconds(-1);
        ENG_CHECK_MSG(std::fabs(clip.Duration() - expected) <= expected * 0.1f, name.c_str());
        ENG_CHECK(clip.frameCount() > 0);

        const std::vector<f32>& s = clip.Samples();
        f64 peak = 0.0;
        for (f32 v : s) {
            ENG_CHECK(std::isfinite(v));
            peak = std::max(peak, std::fabs(static_cast<f64>(v)));
        }
        ENG_CHECK_MSG(peak <= 1.0 + 1e-6, "baked clip must not clip");
        ENG_CHECK_MSG(peak > 0.05, "baked clip must be audible");
        ENG_CHECK(Analyse(s).rms > 0.01);

        // Бесшовный цикл означает, что хвост кроссфейдом уходит в начало, поэтому
        // последние миллисекунды не должны быть тихими.
        ENG_CHECK(WindowRms(s, clip.Duration() - 0.02f, clip.Duration()) > 1e-4);
    }

    // Явная длительность и кламп по maxSeconds.
    ChipPlayer player;
    player.SetSong(ChipPlayer::Make8BitSong());
    const AudioClip twoSeconds = player.Bake(22050, 2.0f, 0.05f);
    ENG_CHECK(twoSeconds.Valid());
    ENG_CHECK_EQ(twoSeconds.SampleRate(), 22050);
    ENG_CHECK_NEAR(twoSeconds.Duration(), 2.0, 0.05);
    const AudioClip clamped = player.Bake(44100, 100.0f, 0.05f, 3);
    ENG_CHECK(clamped.Valid());
    ENG_CHECK_NEAR(clamped.Duration(), 3.0, 0.1);

    // Запекание без песни даёт невалидный клип вместо падения.
    ChipPlayer empty;
    ENG_CHECK(!empty.Bake(44100).Valid());
    ENG_CHECK(!empty.HasSong());
}

// ---------------------------------------------------------------------------
// Сериализация
// ---------------------------------------------------------------------------
ENG_TEST(Chiptune, SerializationRoundTrip) {
    for (const std::string& name : ChipPlayer::BuiltinNames()) {
        const ChipSong song = ChipPlayer::MakeNamed(name, 11);
        const std::string text = ChipPlayer::Serialize(song);
        ENG_CHECK(text.size() > 100);
        ENG_CHECK(text.rfind("#GE-CHIPTUNE", 0) == 0);

        ChipSong back;
        std::string err = "unset";
        ENG_CHECK_MSG(ChipPlayer::Deserialize(text, &back, &err), err.c_str());
        ENG_CHECK(err.empty());

        // Сравнение по каждому полю туда-обратно.
        ENG_CHECK_STR_EQ(back.title, song.title);
        ENG_CHECK_STR_EQ(back.author, song.author);
        ENG_CHECK(back.chip == song.chip);
        ENG_CHECK_EQ(back.channelCount, song.channelCount);
        ENG_CHECK_EQ(back.ticksPerRow, song.ticksPerRow);
        ENG_CHECK_EQ(back.framesPerSecond, song.framesPerSecond);
        ENG_CHECK_EQ(back.rowsPerBeat, song.rowsPerBeat);
        ENG_CHECK_EQ(back.beatsPerMinute, song.beatsPerMinute);
        ENG_CHECK_EQ(back.loopOrder, song.loopOrder);
        ENG_CHECK_EQ(back.echoEnabled, song.echoEnabled);
        ENG_CHECK_EQ(back.echoDelayMs, song.echoDelayMs);
        ENG_CHECK_EQ(back.echoFeedback, song.echoFeedback);
        ENG_CHECK_EQ(back.echoVolume, song.echoVolume);
        ENG_CHECK_EQ(back.lowPass, song.lowPass);
        ENG_CHECK_EQ(back.lowPassCutoff, song.lowPassCutoff);
        ENG_CHECK_EQ(back.PatternCount(), song.PatternCount());
        ENG_CHECK_EQ(static_cast<int>(back.order.size()), static_cast<int>(song.order.size()));
        for (usize i = 0; i < song.order.size(); ++i)
            ENG_CHECK_EQ(back.order[i], song.order[i]);
        for (usize i = 0; i < song.channels.size(); ++i) {
            ENG_CHECK(back.channels[i].wave == song.channels[i].wave);
            ENG_CHECK_EQ(back.channels[i].volume, song.channels[i].volume);
            ENG_CHECK_EQ(back.channels[i].attack, song.channels[i].attack);
            ENG_CHECK_EQ(back.channels[i].decay, song.channels[i].decay);
            ENG_CHECK_EQ(back.channels[i].sustain, song.channels[i].sustain);
            ENG_CHECK_EQ(back.channels[i].release, song.channels[i].release);
            ENG_CHECK_EQ(back.channels[i].pan, song.channels[i].pan);
            ENG_CHECK_EQ(back.channels[i].detune, song.channels[i].detune);
            ENG_CHECK_EQ(back.channels[i].dutySweep, song.channels[i].dutySweep);
            ENG_CHECK_EQ(back.channels[i].pitchSweep, song.channels[i].pitchSweep);
            ENG_CHECK_EQ(back.channels[i].arpeggio, song.channels[i].arpeggio);
            ENG_CHECK_EQ(back.channels[i].echoVolume, song.channels[i].echoVolume);
        }
        for (usize pi = 0; pi < song.patterns.size(); ++pi) {
            const ChipPattern& a = song.patterns[pi];
            const ChipPattern& b = back.patterns[pi];
            ENG_CHECK_EQ(b.rowCount, a.rowCount);
            ENG_CHECK_EQ(b.channelCount, a.channelCount);
            ENG_CHECK_STR_EQ(b.name, a.name);
            for (int r = 0; r < a.rowCount; ++r) {
                for (int ch = 0; ch < a.channelCount; ++ch) {
                    ENG_CHECK_EQ(static_cast<int>(b.At(r, ch).semitone),
                                 static_cast<int>(a.At(r, ch).semitone));
                    ENG_CHECK_EQ(static_cast<int>(b.At(r, ch).volume),
                                 static_cast<int>(a.At(r, ch).volume));
                    ENG_CHECK_EQ(static_cast<int>(b.At(r, ch).effect),
                                 static_cast<int>(a.At(r, ch).effect));
                    ENG_CHECK_EQ(static_cast<int>(b.At(r, ch).param),
                                 static_cast<int>(a.At(r, ch).param));
                    ENG_CHECK_EQ(static_cast<int>(b.At(r, ch).arp),
                                 static_cast<int>(a.At(r, ch).arp));
                }
            }
        }

        // Serialize -> Deserialize -> Serialize стабильно.
        ENG_CHECK_STR_EQ(ChipPlayer::Serialize(back), text);
    }

    // Каждый эффект переживает прогон туда-обратно.
    ChipSong fx = MakeTinySong(32, 2, 6, 60);
    for (int r = 0; r < 32; ++r) {
        ChipNote& n = fx.patterns[0].At(r, 0);
        n.semitone = static_cast<i16>(50 + r);
        n.effect = static_cast<u8>(r % 14);
        n.param = static_cast<u8>(r * 7 % 256);
        n.volume = static_cast<u8>(r % 16);
        n.arp = static_cast<u8>(r);
    }
    fx.patterns[0].At(0, 0).semitone = -2;
    ChipSong fxBack;
    ENG_CHECK(ChipPlayer::Deserialize(ChipPlayer::Serialize(fx), &fxBack));
    ENG_CHECK_STR_EQ(ChipPlayer::Serialize(fxBack), ChipPlayer::Serialize(fx));

    // Файловый прогон через файловую систему движка.
    const std::string path = test::TempFilePath("chiptune_roundtrip.gct");
    const ChipSong fileSong = ChipPlayer::MakeBossSong(9);
    ENG_CHECK(ChipPlayer::SaveToFile(fileSong, path));
    ChipSong loaded;
    std::string err;
    ENG_CHECK_MSG(ChipPlayer::LoadFromFile(path, &loaded, &err), err.c_str());
    ENG_CHECK_STR_EQ(ChipPlayer::Serialize(loaded), ChipPlayer::Serialize(fileSong));
    std::remove(path.c_str());
    ENG_CHECK(!ChipPlayer::LoadFromFile(test::TempFilePath("chiptune_missing.gct"), &loaded, &err));
}

ENG_TEST(Chiptune, DeserializeRejectsGarbage) {
    ChipSong out;
    std::string err;
    const ChipSong good = ChipPlayer::Make8BitSong(1);
    const std::string goodText = ChipPlayer::Serialize(good);

    const std::string bad[] = {
        "",
        "hello world",
        "this is not a chiptune",
        "#GE-CHIPTUNE 1",
        "#GE-CHIPTUNE 1\nchannels=notanumber\n",
        "#GE-CHIPTUNE 1\nchannels=-3\n",
        "#GE-CHIPTUNE 1\nchannels=99999\n",
        "#GE-CHIPTUNE 1\npattern=P,0,1\n",
        "#GE-CHIPTUNE 1\npattern=P,99999999,1\n",
        "#GE-CHIPTUNE 1\npattern=P,4,99999\n",
        "#GE-CHIPTUNE 1\npattern=P,4,1\nrow\n",
        "#GE-CHIPTUNE 1\npattern=P,4,1\nrow zzz\n",
        "#GE-CHIPTUNE 1\npattern=P,4,1\nrow A-4,0,255,0,0,255\norder=0\n",
        "#GE-CHIPTUNE 1\npattern=P,2,1\nrow A-4,0,255,0,0,255\nrow Q-9,0,255,0,0,255\norder=0\n",
        "#GE-CHIPTUNE 1\norder=7\n",
        "#GE-CHIPTUNE 1\nspeed=bogus\n",
        "GE-CHIPTUNE\nnonsense",
    };
    for (const std::string& t : bad) {
        ChipSong tmp;
        tmp.title = "sentinel";
        std::string e;
        const bool ok = ChipPlayer::Deserialize(t, &tmp, &e);
        ENG_CHECK_MSG(!ok, std::string("should reject: ") + t);
        ENG_CHECK(!e.empty());
    }

    // Обрезанный, но в остальном валидный текст.
    ENG_CHECK(!ChipPlayer::Deserialize(goodText.substr(0, goodText.size() * 2 / 3), &out, &err));
    ENG_CHECK(!ChipPlayer::Deserialize(goodText.substr(0, 1), &out, &err));

    // Бинарный мусор, включая NUL-байты.
    std::string junk;
    for (int i = 0; i < 512; ++i) junk.push_back(static_cast<char>(i * 37 % 256));
    junk.insert(0, "#GE-CHIPTUNE 1\n");
    ChipSong tmp;
    ENG_CHECK(!ChipPlayer::Deserialize(junk, &tmp, &err));

    // Нулевой указатель вывода завершается аккуратной ошибкой.
    ENG_CHECK(!ChipPlayer::Deserialize(goodText, nullptr, &err));
    ENG_CHECK(!err.empty());

    // Валидная песня разбирается без проблем.
    ChipSong ok;
    ENG_CHECK(ChipPlayer::Deserialize(goodText, &ok, &err));
    ENG_CHECK_STR_EQ(ok.title, good.title);
}

// ---------------------------------------------------------------------------
// Живое состояние, пик и колбэк строк
// ---------------------------------------------------------------------------
ENG_TEST(Chiptune, LiveStateAndPeak) {
    const ChipSong song = ChipPlayer::Make8BitSong();
    ChipPlayer p;
    p.SetSong(song);
    p.SetSampleRate(44100);
    ENG_CHECK_EQ(static_cast<int>(p.ChannelStates().size()), song.channelCount);
    for (const auto& st : p.ChannelStates()) ENG_CHECK(st.lastNoteName.empty());

    g_rowCalls = 0;
    g_userCalls = 0;
    g_lastOrder = -1;
    g_lastRow = -1;
    p.SetRowCallback(&RowCb, &g_userCalls);
    p.Play();
    std::vector<f32> buf;
    p.RenderTo(&buf, static_cast<int>(44100 * 2.0f));
    ENG_CHECK_MSG(g_rowCalls > 0, "the row callback must fire");
    ENG_CHECK_EQ(g_rowCalls, g_userCalls);
    ENG_CHECK(g_lastOrder >= 0);
    ENG_CHECK(g_lastRow >= 0);
    ENG_CHECK_EQ(g_lastRow, p.CurrentRow());
    // 2 секунды при 0.12 с на строку - около 16 строк; колбэк срабатывает раз
    // за строку, так что допускаем щедрое окно.
    ENG_CHECK(g_rowCalls >= 15 && g_rowCalls <= 18);

    ENG_CHECK(p.LastPeak() > 0.05f);
    ENG_CHECK(p.LastPeak() <= 1.0f);
    ENG_CHECK(p.RenderedFrames() > 0);
    ENG_CHECK(p.CurrentTime() > 1.9f);

    int active = 0;
    bool named = false;
    for (const auto& st : p.ChannelStates()) {
        if (st.active) ++active;
        if (!st.lastNoteName.empty()) named = true;
        ENG_CHECK(st.volume >= 0 && st.volume <= 15);
        ENG_CHECK(std::isfinite(st.frequency));
        ENG_CHECK(std::isfinite(st.phase));
        ENG_CHECK(st.phase >= -1.0f && st.phase <= 2.0f);
    }
    ENG_CHECK_MSG(named, "channel states must expose note names for the tracker UI");
    ENG_CHECK_MSG(active > 0, "at least one channel should still be sounding");

    // Общая громкость 0 глушит вывод; Stop() перематывает.
    ChipPlayer v;
    v.SetSong(song);
    v.SetSampleRate(44100);
    v.SetMasterVolume(0.0f);
    v.Play();
    std::vector<f32> silent;
    v.RenderTo(&silent, 4096);
    ENG_CHECK(Analyse(silent).rms < 1e-6);
    v.SetMasterVolume(1.0f);
    v.Stop();
    ENG_CHECK(!v.IsPlaying());
    ENG_CHECK_EQ(v.CurrentRow(), 0);
    ENG_CHECK_EQ(v.CurrentOrder(), 0);
    ENG_CHECK(v.LastPeak() == 0.0f);
    v.Pause();
    ENG_CHECK(!v.IsPlaying());
}

// ---------------------------------------------------------------------------
// Производительность
// ---------------------------------------------------------------------------
ENG_TEST(Chiptune, RenderPerformance) {
    ChipPlayer p;
    p.SetSong(ChipPlayer::Make8BitSong());
    p.SetSampleRate(44100);
    p.Play();
    std::vector<f32> buf(static_cast<usize>(44100 * 5) * 2, 0.0f);
    const f64 t0 = NowSeconds();
    p.RenderAdd(buf.data(), 44100 * 5);
    const f64 elapsed = NowSeconds() - t0;
    ENG_CHECK_MSG(elapsed < 0.25, "rendering 5 s of the 8-bit song must take < 250 ms");
    ENG_CHECK(Analyse(buf).rms > 0.05);
    std::printf("      [chiptune] 5 s of 8-bit audio rendered in %.1f ms\n", elapsed * 1000.0);
}

// Ассетный генератор пишет встроенные песни как текст трекера; если они есть,
// файловый формат должен сохранять их побайтово точно.
ENG_TEST(Chiptune, AssetSongsRoundTrip) {
    int found = 0;
    for (const std::string& name : ChipPlayer::BuiltinNames()) {
        const std::string path = PathJoin(GetAssetRoot(), "audio/song_" + name + ".chip");
        if (!FileExists(path)) continue;
        ++found;
        ChipSong loaded;
        std::string err;
        ENG_CHECK_MSG(ChipPlayer::LoadFromFile(path, &loaded, &err), err.c_str());
        ChipSong expected = ChipPlayer::MakeNamed(name, 7);
        ENG_CHECK_EQ(loaded.title, expected.title);
        ENG_CHECK_EQ(loaded.channelCount, expected.channelCount);
        ENG_CHECK_EQ(loaded.PatternCount(), expected.PatternCount());
        ENG_CHECK_EQ(static_cast<int>(loaded.order.size()), static_cast<int>(expected.order.size()));
        ENG_CHECK_EQ(static_cast<int>(loaded.chip), static_cast<int>(expected.chip));
        ENG_CHECK_NEAR(loaded.ticksPerRow, expected.ticksPerRow, 1e-6f);
        ENG_CHECK_NEAR(loaded.DurationSeconds(), expected.DurationSeconds(), 0.05f);
        // Повторная сериализация загруженной песни воспроизводит файл байт в байт.
        ENG_CHECK_EQ(ChipPlayer::Serialize(loaded), ChipPlayer::Serialize(expected));
    }
    if (found == 0) ENG_SKIP("no .chip assets staged (run build.sh assets)");
    ENG_CHECK_EQ(found, 5);
}

// Каждый тип фильтра должен иметь метаданные и пригодные значения по умолчанию.
ENG_TEST(Chiptune, SongsPlayThroughTheMixer) {
    ChipPlayer p;
    p.SetSong(ChipPlayer::MakeNamed("dance", 7));
    p.Play();
    std::vector<f32> out(2048 * 2, 0.0f);
    for (int block = 0; block < 40; ++block) p.RenderAdd(out.data(), 2048);
    f64 energy = 0;
    int nonZero = 0;
    for (f32 v : out) {
        energy += static_cast<f64>(v) * v;
        if (std::fabs(v) > 1e-4f) ++nonZero;
    }
    ENG_CHECK_GT(energy, 1.0);
    ENG_CHECK_GT(nonZero, 100);
    ENG_CHECK_GT(p.CurrentTime(), 0.5f);
}
