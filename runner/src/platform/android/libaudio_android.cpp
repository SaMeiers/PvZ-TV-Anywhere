#include <pvz_tv/dependencies/dependency.h>

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <android/log.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace pvz_tv {

static SLObjectItf g_engineObject = nullptr;
static SLEngineItf g_engineEngine = nullptr;
static SLObjectItf g_outputMixObject = nullptr;
static SLObjectItf g_playerObject = nullptr;
static SLPlayItf g_playerPlay = nullptr;
static SLAndroidSimpleBufferQueueItf g_playerBufferQueue = nullptr;

static std::mutex g_audio_lock;
static std::condition_variable g_audio_cv;
static std::atomic<bool> g_audio_inited{false};
static std::atomic<bool> g_audio_paused{false};
static uint32_t g_bytes_per_sec = 176400;

static constexpr int kNumBuffers = 16;
/* How much audio may sit in the queue waiting to be played. This is the delay
 * between the game deciding to make a sound and the sound coming out, so it is
 * a time, not a count of buffers: the guest hands over 8 KB at a time, which is
 * 46 ms at 44.1 kHz stereo, and a queue six of those deep was a third of a
 * second behind. Two buffers is the floor -- one playing, one ready -- so the
 * mixer never runs dry. */
static constexpr uint32_t kMaxQueuedMs = 90;
static std::vector<uint8_t> g_ring_buffers[kNumBuffers];
static int g_write_index = 0;

static void bqPlayerCallback(SLAndroidSimpleBufferQueueItf bq, void *context) {
    (void)bq;
    (void)context;
    // Wake up writer thread when OpenSL finishes playing a buffer
    g_audio_cv.notify_one();
}

static void ag_audio_init(GuestCall &c) {
    uint32_t sample_rate = c.arg(0);
    uint32_t channels = c.arg(1);
    uint32_t bits = c.arg(2);
    __android_log_print(ANDROID_LOG_INFO, "RunnerAudio", "AGAudioInit(rate=%u, channels=%u, bits=%u)", sample_rate, channels, bits);

    std::lock_guard<std::mutex> lk(g_audio_lock);
    if (!g_audio_inited.load()) {
        uint32_t freq = sample_rate ? sample_rate : 44100;
        uint32_t ch = channels ? channels : 2;
        uint32_t bit_depth = (bits == 16) ? 16 : 8;
        g_bytes_per_sec = freq * ch * (bit_depth / 8);

        SLresult res;
        res = slCreateEngine(&g_engineObject, 0, nullptr, 0, nullptr, nullptr);
        if (res != SL_RESULT_SUCCESS) {
            c.set_result((uint32_t)-1);
            return;
        }
        res = (*g_engineObject)->Realize(g_engineObject, SL_BOOLEAN_FALSE);
        if (res != SL_RESULT_SUCCESS) {
            c.set_result((uint32_t)-1);
            return;
        }
        res = (*g_engineObject)->GetInterface(g_engineObject, SL_IID_ENGINE, &g_engineEngine);
        if (res != SL_RESULT_SUCCESS) {
            c.set_result((uint32_t)-1);
            return;
        }

        res = (*g_engineEngine)->CreateOutputMix(g_engineEngine, &g_outputMixObject, 0, nullptr, nullptr);
        if (res != SL_RESULT_SUCCESS) {
            c.set_result((uint32_t)-1);
            return;
        }
        res = (*g_outputMixObject)->Realize(g_outputMixObject, SL_BOOLEAN_FALSE);
        if (res != SL_RESULT_SUCCESS) {
            c.set_result((uint32_t)-1);
            return;
        }

        SLDataLocator_AndroidSimpleBufferQueue loc_bufq = {
            SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, kNumBuffers
        };

        SLuint32 sl_freq = (SLuint32)freq * 1000;

        SLDataFormat_PCM format_pcm = {
            SL_DATAFORMAT_PCM,
            (SLuint32)ch,
            sl_freq,
            (SLuint32)bit_depth,
            (SLuint32)bit_depth,
            (ch == 2) ? (SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT) : SL_SPEAKER_FRONT_CENTER,
            SL_BYTEORDER_LITTLEENDIAN
        };

        SLDataSource audioSrc = { &loc_bufq, &format_pcm };
        SLDataLocator_OutputMix loc_outmix = { SL_DATALOCATOR_OUTPUTMIX, g_outputMixObject };
        SLDataSink audioSnk = { &loc_outmix, nullptr };

        const SLInterfaceID ids[1] = { SL_IID_ANDROIDSIMPLEBUFFERQUEUE };
        const SLboolean req[1] = { SL_BOOLEAN_TRUE };

        res = (*g_engineEngine)->CreateAudioPlayer(g_engineEngine, &g_playerObject, &audioSrc, &audioSnk, 1, ids, req);
        if (res != SL_RESULT_SUCCESS) {
            c.set_result((uint32_t)-1);
            return;
        }

        res = (*g_playerObject)->Realize(g_playerObject, SL_BOOLEAN_FALSE);
        if (res != SL_RESULT_SUCCESS) {
            c.set_result((uint32_t)-1);
            return;
        }

        (*g_playerObject)->GetInterface(g_playerObject, SL_IID_PLAY, &g_playerPlay);
        (*g_playerObject)->GetInterface(g_playerObject, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &g_playerBufferQueue);

        (*g_playerBufferQueue)->RegisterCallback(g_playerBufferQueue, bqPlayerCallback, nullptr);
        (*g_playerPlay)->SetPlayState(g_playerPlay, SL_PLAYSTATE_PLAYING);

        g_write_index = 0;
        for (int i = 0; i < kNumBuffers; ++i) {
            g_ring_buffers[i].clear();
        }

        g_audio_inited.store(true);
        __android_log_print(ANDROID_LOG_INFO, "RunnerAudio", "OpenSL ES Audio initialized successfully! (rate=%u, bytes/sec=%u)", freq, g_bytes_per_sec);
    }

    c.set_result(0);
}

static void ag_audio_is_paused(GuestCall &c) {
    c.set_result(g_audio_paused.load() ? 1 : 0);
}

// Called when the activity is paused/resumed, so the game is not heard (and
// does not keep an active OpenSL player) while it is in the background.
void android_runner_set_audio_paused(bool paused) {
    g_audio_paused.store(paused);
    if (!g_playerPlay) return;
    (*g_playerPlay)->SetPlayState(g_playerPlay,
                                  paused ? SL_PLAYSTATE_PAUSED : SL_PLAYSTATE_PLAYING);
}

static void ag_audio_write(GuestCall &c) {
    uint32_t buf_ptr = c.arg(0);
    uint32_t size_bytes = c.arg(1);

    if (!buf_ptr || !size_bytes || !c.in_bounds(buf_ptr, size_bytes)) {
        c.set_result(0);
        return;
    }

    std::unique_lock<std::mutex> lk(g_audio_lock);
    if (!g_audio_inited.load() || g_playerBufferQueue == nullptr) {
        c.set_result(0);
        return;
    }

    // Flow control: let the queue grow to kMaxQueuedMs of audio and no further,
    // which paces the guest against real-time playback.
    const uint32_t queued_bytes_max = (g_bytes_per_sec / 1000u) * kMaxQueuedMs;
    uint32_t max_buffers = (size_bytes > 0) ? (queued_bytes_max / size_bytes) : 2u;
    if (max_buffers < 2u) max_buffers = 2u;
    if (max_buffers > (uint32_t)kNumBuffers) max_buffers = (uint32_t)kNumBuffers;

    while (g_audio_inited.load() && g_playerBufferQueue != nullptr) {
        SLAndroidSimpleBufferQueueState state;
        SLresult res = (*g_playerBufferQueue)->GetState(g_playerBufferQueue, &state);
        if (res != SL_RESULT_SUCCESS || state.count < max_buffers) {
            break;
        }
        g_audio_cv.wait_for(lk, std::chrono::milliseconds(15));
    }

    if (!g_audio_inited.load() || g_playerBufferQueue == nullptr) {
        c.set_result(0);
        return;
    }

    // Copy guest samples into ring buffer to avoid data race with guest thread
    auto &ring_buf = g_ring_buffers[g_write_index];
    ring_buf.assign(c.img->mem + buf_ptr, c.img->mem + buf_ptr + size_bytes);

    SLresult res = (*g_playerBufferQueue)->Enqueue(g_playerBufferQueue, ring_buf.data(), size_bytes);
    if (res == SL_RESULT_SUCCESS) {
        g_write_index = (g_write_index + 1) % kNumBuffers;
    } else {
        __android_log_print(ANDROID_LOG_WARN, "RunnerAudio", "Enqueue failed: 0x%04x", (unsigned)res);
    }

    c.set_result(size_bytes);
}

static void ag_audio_uninit(GuestCall &c) {
    {
        std::lock_guard<std::mutex> lk(g_audio_lock);
        g_audio_inited.store(false);
    }
    g_audio_cv.notify_all();

    std::lock_guard<std::mutex> lk(g_audio_lock);
    if (g_playerPlay) {
        (*g_playerPlay)->SetPlayState(g_playerPlay, SL_PLAYSTATE_STOPPED);
    }
    if (g_playerBufferQueue) {
        (*g_playerBufferQueue)->Clear(g_playerBufferQueue);
    }
    if (g_playerObject) {
        (*g_playerObject)->Destroy(g_playerObject);
        g_playerObject = nullptr;
        g_playerPlay = nullptr;
        g_playerBufferQueue = nullptr;
    }
    if (g_outputMixObject) {
        (*g_outputMixObject)->Destroy(g_outputMixObject);
        g_outputMixObject = nullptr;
    }
    if (g_engineObject) {
        (*g_engineObject)->Destroy(g_engineObject);
        g_engineObject = nullptr;
        g_engineEngine = nullptr;
    }

    c.set_result(0);
}

static void ag_audio_set_read_cb(GuestCall &c) {
    c.set_result(0);
}

void register_libaudio(ImportTable &t) {
    t.add("AGAudioInit", ag_audio_init);
    t.add("AGAudioIsPaused", ag_audio_is_paused);
    t.add("AGAudioWrite", ag_audio_write);
    t.add("AGAudioUninit", ag_audio_uninit);
    t.add("AGAudioSetReadCallback", ag_audio_set_read_cb);
}

} // namespace pvz_tv
