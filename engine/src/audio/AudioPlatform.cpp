// Платформенный бэкенд аудиовывода.
//
// На каждой платформе компилируется один реальный бэкенд:
//   macOS / iOS : CoreAudio (AudioToolbox AudioUnit / RemoteIO)
//   Windows     : WASAPI, render-клиент в общем режиме
//   Linux       : ALSA через dlopen("libasound.so.2") - без зависимости линковки
//   Android     : плеер на очереди буферов OpenSL ES
//   WASM        : мост Emscripten WebAudio ScriptProcessor
//
// Везде остальных (или когда бэкенд не поднялся) движок откатывается к
// переносимому null-бэкенду: PlatformAudioInit() возвращает false, Audio::Init()
// оставляет silent_ = true, и микшером управляют Audio::Update() /
// Audio::PumpSilent(). Установка ENG_AUDIO_NULL=1 принудительно включает тихий
// путь — удобно для CI и запусков без окружения.
//
// Колбэк рендера вызывает только Audio::Mix(); он никогда не выделяет память.
//
// NOTE: crossrender/core/Math.h использует std::vector в Random::Shuffle, но не включает
// <vector>; подключаем его первым, чтобы замороженный публичный заголовок компилировался автономно.
#include <vector>

#include "crossrender/audio/Audio.h"

#include "crossrender/core/Log.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

// Системные заголовки платформ подключаются вне `namespace crossrender` (macOS MacTypes.h
// определяет глобальный struct Rect, который иначе столкнулся бы с crossrender::Rect).
#if defined(ENG_PLATFORM_MACOS) || defined(ENG_PLATFORM_IOS)
#include <AudioUnit/AudioUnit.h>
#include <AudioToolbox/AudioToolbox.h>
#elif defined(ENG_PLATFORM_WINDOWS)
#include <thread>
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#elif defined(ENG_PLATFORM_LINUX)
#include <thread>
#include <dlfcn.h>
#elif defined(ENG_PLATFORM_ANDROID)
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#elif defined(ENG_PLATFORM_WASM)
#include <emscripten/emscripten.h>
#endif

namespace crossrender {
namespace {

struct PlatformState {
    bool active = false;
    int sampleRate = 44100;
    int channels = 2;
    int bufferFrames = 1024;
    std::atomic<int> underruns{0};
};

PlatformState g_state;

bool AudioNullRequested() {
    const char* v = std::getenv("ENG_AUDIO_NULL");
    return v != nullptr && v[0] != '\0' && v[0] != '0';
}

}  // namespace

// ===========================================================================
// macOS / iOS - CoreAudio
// ===========================================================================
#if defined(ENG_PLATFORM_MACOS) || defined(ENG_PLATFORM_IOS)

namespace {

constexpr int kMaxCallbackFrames = 8192;
f32 g_scratch[static_cast<usize>(kMaxCallbackFrames) * 2];  // промежуточный буфер с чередованием каналов
AudioUnit g_audioUnit = nullptr;

OSStatus AudioRenderProc(void* refCon, AudioUnitRenderActionFlags* actionFlags,
                         const AudioTimeStamp* timeStamp, UInt32 busNumber, UInt32 frameCount,
                         AudioBufferList* ioData) {
    (void)refCon;
    (void)actionFlags;
    (void)timeStamp;
    (void)busNumber;
    if (!ioData || ioData->mNumberBuffers == 0 || frameCount == 0) return noErr;

    const int channels = g_state.channels;
    int remaining = static_cast<int>(frameCount);
    int offset = 0;
    while (remaining > 0) {
        int chunk = remaining > kMaxCallbackFrames ? kMaxCallbackFrames : remaining;
        if (ioData->mNumberBuffers == 1 &&
            static_cast<int>(ioData->mBuffers[0].mNumberChannels) == channels) {
            // С чередованием: сводим прямо в буфер устройства.
            f32* dst = static_cast<f32*>(ioData->mBuffers[0].mData);
            Audio::Get().Mix(dst + static_cast<usize>(offset) * static_cast<usize>(channels), chunk);
        } else {
            // Без чередования (или иное число каналов): сводим в scratch и
            // раскладываем по буферам.
            Audio::Get().Mix(g_scratch, chunk);
            for (UInt32 b = 0; b < ioData->mNumberBuffers; ++b) {
                const int bufChannels = static_cast<int>(ioData->mBuffers[b].mNumberChannels);
                f32* dst = static_cast<f32*>(ioData->mBuffers[b].mData);
                for (int f = 0; f < chunk; ++f) {
                    f32 acc = 0.0f;
                    for (int c = 0; c < bufChannels; ++c) {
                        const int srcCh = static_cast<int>(b) * bufChannels + c;
                        if (srcCh < channels) acc += g_scratch[static_cast<usize>(f) * channels + srcCh];
                    }
                    dst[static_cast<usize>(offset + f) * bufChannels + 0] = acc;
                }
            }
        }
        remaining -= chunk;
        offset += chunk;
    }
    return noErr;
}

bool BackendInit(int sampleRate, int channels, int bufferFrames) {
    (void)bufferFrames;
    AudioComponentDescription desc;
    std::memset(&desc, 0, sizeof(desc));
    desc.componentType = kAudioUnitType_Output;
#if defined(ENG_PLATFORM_IOS)
    desc.componentSubType = kAudioUnitSubType_RemoteIO;
#else
    desc.componentSubType = kAudioUnitSubType_DefaultOutput;
#endif
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    desc.componentFlags = 0;
    desc.componentFlagsMask = 0;

    AudioComponent component = AudioComponentFindNext(nullptr, &desc);
    if (!component) {
        ENG_LOGW("audio", "CoreAudio: no default output component");
        return false;
    }
    if (AudioComponentInstanceNew(component, &g_audioUnit) != noErr || !g_audioUnit) {
        ENG_LOGW("audio", "CoreAudio: AudioComponentInstanceNew failed");
        g_audioUnit = nullptr;
        return false;
    }

    AudioStreamBasicDescription format;
    std::memset(&format, 0, sizeof(format));
    format.mSampleRate = static_cast<Float64>(sampleRate);
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    format.mChannelsPerFrame = static_cast<UInt32>(channels);
    format.mBitsPerChannel = 32;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = static_cast<UInt32>(sizeof(f32) * channels);
    format.mBytesPerPacket = format.mBytesPerFrame;

    OSStatus status = AudioUnitSetProperty(g_audioUnit, kAudioUnitProperty_StreamFormat,
                                           kAudioUnitScope_Input, 0, &format, sizeof(format));
    if (status != noErr) {
        ENG_LOGW("audio", "CoreAudio: StreamFormat rejected (%d)", static_cast<int>(status));
        AudioComponentInstanceDispose(g_audioUnit);
        g_audioUnit = nullptr;
        return false;
    }

    AURenderCallbackStruct callback;
    callback.inputProc = &AudioRenderProc;
    callback.inputProcRefCon = nullptr;
    status = AudioUnitSetProperty(g_audioUnit, kAudioUnitProperty_SetRenderCallback,
                                  kAudioUnitScope_Input, 0, &callback, sizeof(callback));
    if (status != noErr) {
        ENG_LOGW("audio", "CoreAudio: SetRenderCallback failed (%d)", static_cast<int>(status));
        AudioComponentInstanceDispose(g_audioUnit);
        g_audioUnit = nullptr;
        return false;
    }

    if (AudioUnitInitialize(g_audioUnit) != noErr) {
        ENG_LOGW("audio", "CoreAudio: AudioUnitInitialize failed");
        AudioComponentInstanceDispose(g_audioUnit);
        g_audioUnit = nullptr;
        return false;
    }
    if (AudioOutputUnitStart(g_audioUnit) != noErr) {
        ENG_LOGW("audio", "CoreAudio: AudioOutputUnitStart failed");
        AudioUnitUninitialize(g_audioUnit);
        AudioComponentInstanceDispose(g_audioUnit);
        g_audioUnit = nullptr;
        return false;
    }
    return true;
}

void BackendShutdown() {
    if (!g_audioUnit) return;
    AudioOutputUnitStop(g_audioUnit);
    AudioUnitUninitialize(g_audioUnit);
    AudioComponentInstanceDispose(g_audioUnit);
    g_audioUnit = nullptr;
}

}  // namespace

// ===========================================================================
// Windows - WASAPI (общий режим)
// ===========================================================================
#elif defined(ENG_PLATFORM_WINDOWS)

namespace {

void BackendShutdown();  // forward-объявление: вызывается из BackendInit ниже

// Общий режим всегда выдаёт формат в стиле WAVE_FORMAT_EXTENSIBLE; настоящий
// тег лежит в первом поле SubFormat GUID.
unsigned WasapiFormatTag(const WAVEFORMATEX* format) {
    if (!format) return 0;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        return reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format)->SubFormat.Data1;
    return format->wFormatTag;
}

IMMDeviceEnumerator* g_enumerator = nullptr;
IMMDevice* g_device = nullptr;
IAudioClient* g_client = nullptr;
IAudioRenderClient* g_render = nullptr;
WAVEFORMATEX* g_mixFormat = nullptr;
HANDLE g_event = nullptr;
std::thread g_thread;
std::atomic<bool> g_running{false};
std::vector<f32> g_wasapiScratch;

void WasapiRenderLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    UINT32 bufferFrames = 0;
    if (FAILED(g_client->GetBufferSize(&bufferFrames)) || bufferFrames == 0) {
        g_running.store(false);
        CoUninitialize();
        return;
    }
    while (g_running.load()) {
        const DWORD wait = WaitForSingleObject(g_event, 2000);
        if (!g_running.load()) break;
        if (wait != WAIT_OBJECT_0) {
            ++g_state.underruns;
            continue;
        }
        UINT32 padding = 0;
        if (FAILED(g_client->GetCurrentPadding(&padding))) continue;
        const UINT32 available = bufferFrames - padding;
        if (available == 0) continue;

        BYTE* target = nullptr;
        if (FAILED(g_render->GetBuffer(available, &target))) continue;

        const int channels = g_state.channels;
        const usize samples = static_cast<usize>(available) * static_cast<usize>(channels);
        if (g_wasapiScratch.size() < samples) g_wasapiScratch.resize(samples);
        Audio::Get().Mix(g_wasapiScratch.data(), static_cast<int>(available));

        const bool isFloat = WasapiFormatTag(g_mixFormat) == WAVE_FORMAT_IEEE_FLOAT &&
                             g_mixFormat->wBitsPerSample == 32;
        if (isFloat) {
            std::memcpy(target, g_wasapiScratch.data(), samples * sizeof(f32));
        } else {
            // Целочисленный 16-битный запасной вариант (достижимо, только если
            // float-сведение недоступно в общем режиме).
            i16* dst = reinterpret_cast<i16*>(target);
            for (usize i = 0; i < samples; ++i) {
                f32 v = g_wasapiScratch[i];
                v = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
                dst[i] = static_cast<i16>(v * 32767.0f);
            }
        }
        g_render->ReleaseBuffer(available, 0);
    }
    CoUninitialize();
}

bool BackendInit(int sampleRate, int channels, int bufferFrames) {
    (void)bufferFrames;
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return false;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&g_enumerator))) ||
        !g_enumerator) {
        CoUninitialize();
        return false;
    }
    if (FAILED(g_enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &g_device)) || !g_device) {
        BackendShutdown();
        return false;
    }
    if (FAILED(g_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(&g_client))) ||
        !g_client) {
        BackendShutdown();
        return false;
    }
    if (FAILED(g_client->GetMixFormat(&g_mixFormat)) || !g_mixFormat) {
        BackendShutdown();
        return false;
    }
    // Общий режим WASAPI принимает только mix-формат устройства, поэтому
    // отклоняем устройство (и позволяем движку работать тихо), а не играем
    // с неверной частотой или в формате, который микшер не выдаёт.
    if (WasapiFormatTag(g_mixFormat) != WAVE_FORMAT_IEEE_FLOAT ||
        g_mixFormat->wBitsPerSample != 32 ||
        g_mixFormat->nChannels != static_cast<WORD>(channels) ||
        g_mixFormat->nSamplesPerSec != static_cast<DWORD>(sampleRate)) {
        ENG_LOGW("audio", "WASAPI: device mix format is %u Hz / %u ch / %u bit, requested %d Hz / %d ch",
                 static_cast<unsigned>(g_mixFormat->nSamplesPerSec),
                 static_cast<unsigned>(g_mixFormat->nChannels),
                 static_cast<unsigned>(g_mixFormat->wBitsPerSample), sampleRate, channels);
        BackendShutdown();
        return false;
    }
    if (FAILED(g_client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 10000000, 0, g_mixFormat,
                                    nullptr))) {
        BackendShutdown();
        return false;
    }
    g_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_event || FAILED(g_client->SetEventHandle(g_event))) {
        BackendShutdown();
        return false;
    }
    if (FAILED(g_client->GetService(__uuidof(IAudioRenderClient),
                                    reinterpret_cast<void**>(&g_render))) ||
        !g_render) {
        BackendShutdown();
        return false;
    }
    UINT32 bufferFrames = 0;
    if (g_client->GetBufferSize(&bufferFrames) == S_OK && bufferFrames > 0)
        g_wasapiScratch.assign(static_cast<usize>(bufferFrames) * static_cast<usize>(channels), 0.0f);
    g_running.store(true);
    g_thread = std::thread(WasapiRenderLoop);
    if (FAILED(g_client->Start())) {
        g_running.store(false);
        if (g_thread.joinable()) g_thread.join();
        BackendShutdown();
        return false;
    }
    return true;
}

void BackendShutdown() {
    if (g_client) g_client->Stop();
    g_running.store(false);
    if (g_event) SetEvent(g_event);
    if (g_thread.joinable()) g_thread.join();
    if (g_render) {
        g_render->Release();
        g_render = nullptr;
    }
    if (g_client) {
        g_client->Release();
        g_client = nullptr;
    }
    if (g_device) {
        g_device->Release();
        g_device = nullptr;
    }
    if (g_enumerator) {
        g_enumerator->Release();
        g_enumerator = nullptr;
    }
    if (g_mixFormat) {
        CoTaskMemFree(g_mixFormat);
        g_mixFormat = nullptr;
    }
    if (g_event) {
        CloseHandle(g_event);
        g_event = nullptr;
    }
}

}  // namespace

// ===========================================================================
// Linux - ALSA через dlopen (без зависимости на этапе сборки)
// ===========================================================================
#elif defined(ENG_PLATFORM_LINUX)

namespace {

void BackendShutdown();  // forward-объявление: вызывается из BackendInit ниже

// Непрозрачные типы ALSA: нужны только указатели, поэтому заголовки не требуются.
struct _snd_pcm;
using snd_pcm_t = struct _snd_pcm;

// Значения из <alsa/pcm.h>.
constexpr int kPcmStreamPlayback = 0;
constexpr int kPcmFormatFloatLE = 14;
constexpr int kPcmAccessRWInterleaved = 3;

using FnPcmOpen = int (*)(snd_pcm_t**, const char*, int, int);
using FnPcmClose = int (*)(snd_pcm_t*);
using FnPcmSetParams = int (*)(snd_pcm_t*, int, int, unsigned int, unsigned int, int, unsigned int);
using FnPcmWritei = long (*)(snd_pcm_t*, const void*, unsigned long);
using FnPcmRecover = int (*)(snd_pcm_t*, int, int);
using FnPcmPrepare = int (*)(snd_pcm_t*);
using FnPcmDrain = int (*)(snd_pcm_t*);

void* g_alsa = nullptr;
snd_pcm_t* g_pcm = nullptr;
FnPcmOpen g_pcmOpen = nullptr;
FnPcmClose g_pcmClose = nullptr;
FnPcmSetParams g_pcmSetParams = nullptr;
FnPcmWritei g_pcmWritei = nullptr;
FnPcmRecover g_pcmRecover = nullptr;
FnPcmPrepare g_pcmPrepare = nullptr;
FnPcmDrain g_pcmDrain = nullptr;
std::thread g_thread;
std::atomic<bool> g_running{false};
std::vector<f32> g_alsaScratch;

template <typename T>
bool LoadSym(T* out, const char* name) {
    *out = reinterpret_cast<T>(dlsym(g_alsa, name));
    return *out != nullptr;
}

void AlsaRenderLoop() {
    const unsigned int periodFrames = static_cast<unsigned int>(g_state.bufferFrames);
    while (g_running.load()) {
        const usize samples =
            static_cast<usize>(periodFrames) * static_cast<usize>(g_state.channels);
        if (g_alsaScratch.size() < samples) g_alsaScratch.resize(samples);
        Audio::Get().Mix(g_alsaScratch.data(), static_cast<int>(periodFrames));

        long written = g_pcmWritei(g_pcm, g_alsaScratch.data(), periodFrames);
        if (written < 0) {
            ++g_state.underruns;
            if (g_pcmRecover(g_pcm, static_cast<int>(written), 1) < 0) break;
            g_pcmWritei(g_pcm, g_alsaScratch.data(), periodFrames);
        }
    }
}

bool BackendInit(int sampleRate, int channels, int bufferFrames) {
    g_alsa = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!g_alsa) g_alsa = dlopen("libasound.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_alsa) {
        ENG_LOGW("audio", "ALSA: libasound not found");
        return false;
    }
    if (!LoadSym(&g_pcmOpen, "snd_pcm_open") || !LoadSym(&g_pcmClose, "snd_pcm_close") ||
        !LoadSym(&g_pcmSetParams, "snd_pcm_set_params") || !LoadSym(&g_pcmWritei, "snd_pcm_writei") ||
        !LoadSym(&g_pcmRecover, "snd_pcm_recover") || !LoadSym(&g_pcmPrepare, "snd_pcm_prepare") ||
        !LoadSym(&g_pcmDrain, "snd_pcm_drain")) {
        ENG_LOGW("audio", "ALSA: missing required symbols");
        BackendShutdown();
        return false;
    }
    if (g_pcmOpen(&g_pcm, "default", kPcmStreamPlayback, 0) < 0 || !g_pcm) {
        ENG_LOGW("audio", "ALSA: cannot open the default PCM device");
        BackendShutdown();
        return false;
    }
    // Задержка 100 мс, мягкий ресемплинг разрешён.
    const unsigned int latency = static_cast<unsigned int>(bufferFrames) * 4u;
    if (g_pcmSetParams(g_pcm, kPcmFormatFloatLE, kPcmAccessRWInterleaved,
                       static_cast<unsigned int>(channels), static_cast<unsigned int>(sampleRate), 1,
                       latency) < 0) {
        ENG_LOGW("audio", "ALSA: unsupported format, staying silent");
        BackendShutdown();
        return false;
    }
    g_state.bufferFrames = bufferFrames;
    g_running.store(true);
    g_thread = std::thread(AlsaRenderLoop);
    return true;
}

void BackendShutdown() {
    g_running.store(false);
    if (g_thread.joinable()) g_thread.join();
    if (g_pcm && g_pcmDrain) g_pcmDrain(g_pcm);
    if (g_pcm && g_pcmClose) g_pcmClose(g_pcm);
    g_pcm = nullptr;
    if (g_alsa) {
        dlclose(g_alsa);
        g_alsa = nullptr;
    }
}

}  // namespace

// ===========================================================================
// Android - OpenSL ES
// ===========================================================================
#elif defined(ENG_PLATFORM_ANDROID)

namespace {

constexpr int kQueueBuffers = 3;

SLObjectItf g_engineObject = nullptr;
SLEngineItf g_engine = nullptr;
SLObjectItf g_mixObject = nullptr;
SLObjectItf g_playerObject = nullptr;
SLPlayItf g_player = nullptr;
SLAndroidSimpleBufferQueueItf g_queue = nullptr;
std::vector<f32> g_androidBuffers[kQueueBuffers];
int g_nextBuffer = 0;

void AndroidQueueCallback(SLAndroidSimpleBufferQueueItf, void*) {
    const int frames = g_state.bufferFrames;
    std::vector<f32>& target = g_androidBuffers[g_nextBuffer];
    Audio::Get().Mix(target.data(), frames);
    (*g_queue)->Enqueue(g_queue, target.data(),
                        static_cast<SLuint32>(target.size() * sizeof(f32)));
    g_nextBuffer = (g_nextBuffer + 1) % kQueueBuffers;
}

bool BackendInit(int sampleRate, int channels, int bufferFrames) {
    g_state.bufferFrames = bufferFrames > 0 ? bufferFrames : 1024;
    const usize samples = static_cast<usize>(g_state.bufferFrames) * static_cast<usize>(channels);
    for (int i = 0; i < kQueueBuffers; ++i) g_androidBuffers[i].assign(samples, 0.0f);

    if (slCreateEngine(&g_engineObject, 0, nullptr, 0, nullptr, nullptr) != SL_RESULT_SUCCESS)
        return false;
    if ((*g_engineObject)->Realize(g_engineObject, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS)
        return false;
    if ((*g_engineObject)->GetInterface(g_engineObject, SL_IID_ENGINE, &g_engine) != SL_RESULT_SUCCESS)
        return false;
    if ((*g_engine)->CreateOutputMix(g_engine, &g_mixObject, 0, nullptr, nullptr) !=
        SL_RESULT_SUCCESS)
        return false;
    if ((*g_mixObject)->Realize(g_mixObject, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) return false;

    SLDataLocator_AndroidSimpleBufferQueue queueLocator = {
        SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, kQueueBuffers};
    SLDataFormat_PCM format = {SL_DATAFORMAT_PCM,
                               static_cast<SLuint32>(channels),
                               static_cast<SLuint32>(sampleRate) * 1000u,
                               SL_PCMSAMPLEFORMAT_FIXED_32,
                               SL_PCMSAMPLEFORMAT_FIXED_32,
                               channels == 2 ? (SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT)
                                             : SL_SPEAKER_FRONT_CENTER,
                               SL_BYTEORDER_LITTLEENDIAN};
    SLDataSource source = {&queueLocator, &format};
    SLDataLocator_OutputMix outputLocator = {SL_DATALOCATOR_OUTPUTMIX, g_mixObject};
    SLDataSink sink = {&outputLocator, nullptr};

    const SLInterfaceID ids[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE};
    const SLboolean req[] = {SL_BOOLEAN_TRUE};
    if ((*g_engine)->CreateAudioPlayer(g_engine, &g_playerObject, &source, &sink, 1, ids, req) !=
        SL_RESULT_SUCCESS)
        return false;
    if ((*g_playerObject)->Realize(g_playerObject, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS)
        return false;
    if ((*g_playerObject)->GetInterface(g_playerObject, SL_IID_PLAY, &g_player) != SL_RESULT_SUCCESS)
        return false;
    if ((*g_playerObject)
            ->GetInterface(g_playerObject, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &g_queue) !=
        SL_RESULT_SUCCESS)
        return false;
    if ((*g_queue)->RegisterCallback(g_queue, AndroidQueueCallback, nullptr) != SL_RESULT_SUCCESS)
        return false;

    AndroidQueueCallback(g_queue, nullptr);
    return (*g_player)->SetPlayState(g_player, SL_PLAYSTATE_PLAYING) == SL_RESULT_SUCCESS;
}

void BackendShutdown() {
    if (g_player) (*g_player)->SetPlayState(g_player, SL_PLAYSTATE_STOPPED);
    g_player = nullptr;
    g_queue = nullptr;
    if (g_playerObject) {
        (*g_playerObject)->Destroy(g_playerObject);
        g_playerObject = nullptr;
    }
    if (g_mixObject) {
        (*g_mixObject)->Destroy(g_mixObject);
        g_mixObject = nullptr;
    }
    if (g_engineObject) {
        (*g_engineObject)->Destroy(g_engineObject);
        g_engineObject = nullptr;
    }
    g_engine = nullptr;
}

}  // namespace

// ===========================================================================
// WebAssembly - Emscripten WebAudio
// ===========================================================================
#elif defined(ENG_PLATFORM_WASM)

namespace {

std::vector<f32> g_wasmScratch;

extern "C" {

// Вызывается из JavaScript один раз на render-квант WebAudio.
EMSCRIPTEN_KEEPALIVE
void eng_audio_wasm_fill(float* out, int frames) {
    if (!out || frames <= 0) return;
    const int channels = g_state.channels;
    const usize samples = static_cast<usize>(frames) * static_cast<usize>(channels);
    if (g_wasmScratch.size() < samples) g_wasmScratch.resize(samples);
    Audio::Get().Mix(g_wasmScratch.data(), frames);
    std::memcpy(out, g_wasmScratch.data(), samples * sizeof(f32));
}

EMSCRIPTEN_KEEPALIVE
int eng_audio_wasm_channels() { return g_state.channels; }

EMSCRIPTEN_KEEPALIVE
int eng_audio_wasm_rate() { return g_state.sampleRate; }

}  // extern "C"

// JavaScript-мост владеет AudioContext; эта сторона лишь открывает доступ к микшеру.
EM_JS(int, eng_audio_wasm_start, (int sampleRate, int channels, int bufferFrames), {
    if (typeof window === 'undefined') return 0;
    if (!window.__engAudio) {
        try {
            var ctx = new (window.AudioContext || window.webkitAudioContext)();
            var node = ctx.createScriptProcessor(bufferFrames, 0, channels);
            node.onaudioprocess = function (event) {
                var out = event.outputBuffer;
                var frames = out.length;
                var interleaved = new Float32Array(frames * channels);
                _eng_audio_wasm_fill(interleaved.byteOffset, frames);
                for (var c = 0; c < channels; ++c) {
                    var dst = out.getChannelData(c);
                    for (var f = 0; f < frames; ++f) dst[f] = interleaved[f * channels + c];
                }
            };
            node.connect(ctx.destination);
            window.__engAudio = {ctx: ctx, node: node};
        } catch (e) {
            return 0;
        }
    }
    return 1;
});

EM_JS(void, eng_audio_wasm_stop, (), {
    if (typeof window !== 'undefined' && window.__engAudio) {
        try {
            window.__engAudio.node.disconnect();
            window.__engAudio.ctx.close();
        } catch (e) {}
        window.__engAudio = null;
    }
});

bool BackendInit(int sampleRate, int channels, int bufferFrames) {
    return eng_audio_wasm_start(sampleRate, channels, bufferFrames) != 0;
}

void BackendShutdown() { eng_audio_wasm_stop(); }

}  // namespace

// ===========================================================================
// Переносимый null-бэкенд (нет устройства вывода)
// ===========================================================================
#else

namespace {

bool BackendInit(int, int, int) { return false; }
void BackendShutdown() {}

}  // namespace

#endif

// ===========================================================================
// Платформенные хуки аудио
// ===========================================================================
bool Audio::PlatformAudioInit(int sampleRate, int channels, int bufferFrames) {
    g_state.sampleRate = sampleRate;
    g_state.channels = channels;
    g_state.bufferFrames = bufferFrames;
    g_state.underruns.store(0);

    if (AudioNullRequested()) {
        ENG_LOGI("audio", "ENG_AUDIO_NULL set: using the null audio backend");
        return false;
    }
    const bool ok = BackendInit(sampleRate, channels, bufferFrames);
    g_state.active = ok;
    if (!ok) ENG_LOGW("audio", "platform audio backend unavailable");
    return ok;
}

void Audio::PlatformAudioShutdown() {
    if (!g_state.active) return;
    BackendShutdown();
    g_state.active = false;
}

bool Audio::PlatformAudioAvailable() {
#if defined(ENG_PLATFORM_MACOS) || defined(ENG_PLATFORM_IOS) || defined(ENG_PLATFORM_WINDOWS) || \
    defined(ENG_PLATFORM_LINUX) || defined(ENG_PLATFORM_ANDROID) || defined(ENG_PLATFORM_WASM)
    return true;
#else
    return false;
#endif
}

}  // namespace crossrender
