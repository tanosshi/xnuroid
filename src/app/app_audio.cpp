// App mode audio: OpenAL 1.1 as a small software mixer on an AAudio stream, Audio File Services for CAF/WAV/AIFF
// PCM files, System Sound Services (vibration, short sounds) and the old Audio Session C API.
// LOADER_AUDIO=0 keeps the mixer running on a timer thread without an output device.
#include "app.hpp"

#include <aaudio/AAudio.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <map>
#include <thread>

extern "C" void host_vibrate(int ms, int amplitude);   // haptics.cpp

namespace app {

namespace {

// ---------------------------------------------------------------------------------------------------------------
// OpenAL state
// ---------------------------------------------------------------------------------------------------------------
enum : int {
    AL_NONE = 0, AL_SOURCE_RELATIVE = 0x202, AL_CONE_INNER_ANGLE = 0x1001, AL_CONE_OUTER_ANGLE = 0x1002, AL_PITCH = 0x1003, AL_POSITION = 0x1004,
    AL_DIRECTION = 0x1005, AL_VELOCITY = 0x1006, AL_LOOPING = 0x1007, AL_BUFFER = 0x1009, AL_GAIN = 0x100A, AL_MIN_GAIN = 0x100D,
    AL_MAX_GAIN = 0x100E, AL_ORIENTATION = 0x100F, AL_SOURCE_STATE = 0x1010, AL_INITIAL = 0x1011, AL_PLAYING = 0x1012, AL_PAUSED = 0x1013,
    AL_STOPPED = 0x1014, AL_BUFFERS_QUEUED = 0x1015, AL_BUFFERS_PROCESSED = 0x1016, AL_REFERENCE_DISTANCE = 0x1020, AL_ROLLOFF_FACTOR = 0x1021,
    AL_CONE_OUTER_GAIN = 0x1022, AL_MAX_DISTANCE = 0x1023, AL_SEC_OFFSET = 0x1024, AL_SAMPLE_OFFSET = 0x1025, AL_BYTE_OFFSET = 0x1026,
    AL_SOURCE_TYPE = 0x1027, AL_STATIC = 0x1028, AL_STREAMING = 0x1029, AL_UNDETERMINED = 0x1030,
    AL_FORMAT_MONO8 = 0x1100, AL_FORMAT_MONO16 = 0x1101, AL_FORMAT_STEREO8 = 0x1102, AL_FORMAT_STEREO16 = 0x1103,
    AL_FREQUENCY = 0x2001, AL_BITS = 0x2002, AL_CHANNELS = 0x2003, AL_SIZE = 0x2004,
    AL_INVALID_NAME = 0xA001, AL_INVALID_ENUM = 0xA002, AL_INVALID_VALUE = 0xA003, AL_INVALID_OPERATION = 0xA004,
    AL_VENDOR = 0xB001, AL_VERSION = 0xB002, AL_RENDERER = 0xB003, AL_EXTENSIONS = 0xB004,
};

struct Buffer {
    std::vector<int16_t> pcm;        // interleaved 16-bit
    const int16_t* ext = nullptr;    // alBufferDataStatic storage (guest-owned)
    size_t frames = 0;
    int channels = 1, freq = 44100, bits = 16;
    const int16_t* data() const { return ext ? ext : pcm.data(); }
};
struct Source {
    int state = AL_INITIAL;
    unsigned buffer = 0;
    std::vector<unsigned> queue;
    size_t queue_pos = 0;            // index of the buffer playing within queue
    size_t processed = 0;
    double cursor = 0;               // frame position within the current buffer
    float gain = 1, pitch = 1, min_gain = 0, max_gain = 1;
    bool looping = false, relative = false;
    float pos[3] = {0, 0, 0}, vel[3] = {0, 0, 0}, dir[3] = {0, 0, 0};
    float ref_dist = 1, rolloff = 1, max_dist = 1e30f;
    bool streaming() const { return !queue.empty(); }
};

std::mutex g_al;
std::map<unsigned, Buffer> g_buffers;
std::map<unsigned, Source> g_sources;
unsigned g_next_buffer = 1, g_next_source = 1;
float g_listener_gain = 1;
float g_listener_pos[3] = {0, 0, 0};
std::atomic<int> g_error{0};
int g_out_rate = 44100;
AAudioStream* g_stream = nullptr;
bool g_mixer_started = false;

void set_error(int e) { int z = 0; g_error.compare_exchange_strong(z, e); }

const Buffer* current_buffer(Source& s) {
    unsigned id = s.streaming() ? (s.queue_pos < s.queue.size() ? s.queue[s.queue_pos] : 0) : s.buffer;
    auto it = g_buffers.find(id);
    return it == g_buffers.end() ? nullptr : &it->second;
}

// mixes `frames` stereo float frames into out (called with g_al held)
void mix(float* out, int frames) {
    std::memset(out, 0, sizeof(float) * frames * 2);
    for (auto& [id, s] : g_sources) {
        if (s.state != AL_PLAYING) continue;
        float g = std::min(std::max(s.gain, s.min_gain), s.max_gain) * g_listener_gain;
        int i = 0;
        while (i < frames && s.state == AL_PLAYING) {
            const Buffer* b = current_buffer(s);
            if (!b || b->frames == 0) {
                if (s.streaming() && s.queue_pos + 1 < s.queue.size()) { s.queue_pos++; s.processed++; s.cursor = 0; continue; }
                s.state = AL_STOPPED;
                break;
            }
            const int16_t* d = b->data();
            double step = b->freq * static_cast<double>(s.pitch) / g_out_rate;
            if (step <= 0) step = 1;
            for (; i < frames; ++i) {
                size_t f0 = static_cast<size_t>(s.cursor);
                if (f0 >= b->frames) break;
                double frac = s.cursor - f0;
                size_t f1 = f0 + 1 < b->frames ? f0 + 1 : (s.looping && !s.streaming() ? 0 : f0);
                float l, r;
                if (b->channels == 2) {
                    l = static_cast<float>(d[f0 * 2] + (d[f1 * 2] - d[f0 * 2]) * frac);
                    r = static_cast<float>(d[f0 * 2 + 1] + (d[f1 * 2 + 1] - d[f0 * 2 + 1]) * frac);
                } else {
                    l = r = static_cast<float>(d[f0] + (d[f1] - d[f0]) * frac);
                }
                out[i * 2] += l * g * (1.0f / 32768.0f);
                out[i * 2 + 1] += r * g * (1.0f / 32768.0f);
                s.cursor += step;
            }
            if (s.cursor >= b->frames) {
                if (s.streaming()) {
                    s.cursor -= b->frames;
                    if (s.queue_pos + 1 < s.queue.size()) { s.queue_pos++; s.processed++; }
                    else if (s.looping) { s.queue_pos = 0; s.processed = 0; }
                    else { s.processed = s.queue.size(); s.queue_pos = s.queue.size(); s.state = AL_STOPPED; s.cursor = 0; }
                } else if (s.looping) {
                    s.cursor = std::fmod(s.cursor, static_cast<double>(b->frames));
                } else {
                    s.state = AL_STOPPED;
                    s.cursor = 0;
                }
            }
        }
    }
    for (int i = 0; i < frames * 2; ++i) {   // soft clip
        float v = out[i];
        out[i] = v > 1 ? 1 : v < -1 ? -1 : v;
    }
}

aaudio_data_callback_result_t on_audio(AAudioStream*, void*, void* data, int32_t frames) {
    std::lock_guard<std::mutex> l(g_al);
    mix(static_cast<float*>(data), frames);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void start_mixer() {
    if (g_mixer_started) return;
    g_mixer_started = true;
    const char* env = getenv("LOADER_AUDIO");
    if (!(env && std::strcmp(env, "0") == 0)) {
        AAudioStreamBuilder* b = nullptr;
        if (AAudio_createStreamBuilder(&b) == AAUDIO_OK) {
            AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
            AAudioStreamBuilder_setChannelCount(b, 2);
            AAudioStreamBuilder_setSampleRate(b, 44100);
            AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
            AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
            AAudioStreamBuilder_setDataCallback(b, on_audio, nullptr);
            if (AAudioStreamBuilder_openStream(b, &g_stream) == AAUDIO_OK) {
                g_out_rate = AAudioStream_getSampleRate(g_stream);
                AAudioStream_requestStart(g_stream);
                log("OpenAL mixer on AAudio, %d Hz", g_out_rate);
            } else {
                g_stream = nullptr;
            }
            AAudioStreamBuilder_delete(b);
        }
    }
    if (!g_stream) {
        log("OpenAL mixer without an output device");
        std::thread([] {
            std::vector<float> scratch(512 * 2);
            for (;;) {
                usleep(11600);
                std::lock_guard<std::mutex> l(g_al);
                mix(scratch.data(), 512);
            }
        }).detach();
    }
}

}  // namespace

}  // namespace app

using namespace app;

// ---------------------------------------------------------------------------------------------------------------
// OpenAL API
// ---------------------------------------------------------------------------------------------------------------
extern "C" {

static void* al_alcOpenDevice(const char*) { start_mixer(); static int dev; return &dev; }
static bool al_alcCloseDevice(void*) { return true; }
static void* al_alcCreateContext(void*, const int*) { static int ctx; return &ctx; }
static bool al_alcMakeContextCurrent(void*) { return true; }
static void al_alcDestroyContext(void*) {}
static void* al_alcGetCurrentContext() { static int ctx; return &ctx; }
static void* al_alcGetContextsDevice(void*) { static int dev; return &dev; }
static void al_alcProcessContext(void*) {}
static void al_alcSuspendContext(void*) {}
static int al_alcGetError(void*) { return 0; }
static const char* al_alcGetString(void*, int p) { return p == 0x1005 ? "Default" : p == 0x1006 ? "" : "xnuroid OpenAL"; }
static void al_alcGetIntegerv(void*, int p, int n, int* v) {
    if (!v || n < 1) return;
    if (p == 0x1007) *v = g_out_rate;      // ALC_FREQUENCY
    else if (p == 0x1000) *v = 1;          // ALC_MAJOR_VERSION
    else if (p == 0x1001) *v = 1;          // ALC_MINOR_VERSION
    else *v = 0;
}
static bool al_alcIsExtensionPresent(void*, const char*) { return false; }
static void al_alBufferDataStatic(unsigned b, int format, const void* data, int size, int freq);
static void al_mixer_output_rate(double) {}
static void* al_alcGetProcAddress(void*, const char* name) {
    if (!name) return nullptr;
    if (!std::strcmp(name, "alBufferDataStatic") || !std::strcmp(name, "alBufferDataStaticProc")) return reinterpret_cast<void*>(&al_alBufferDataStatic);
    if (!std::strcmp(name, "alcMacOSXMixerOutputRate")) return reinterpret_cast<void*>(&al_mixer_output_rate);
    log("alcGetProcAddress(%s) -> NULL", name);
    return nullptr;
}

static int al_alGetError() { return g_error.exchange(0); }
static void al_alGenBuffers(int n, unsigned* out) {
    std::lock_guard<std::mutex> l(g_al);
    for (int i = 0; i < n; ++i) { out[i] = g_next_buffer++; g_buffers[out[i]]; }
}
static void al_alDeleteBuffers(int n, const unsigned* ids) {
    std::lock_guard<std::mutex> l(g_al);
    for (int i = 0; i < n; ++i) {
        for (auto& [sid, s] : g_sources)
            if (s.buffer == ids[i]) { s.state = AL_STOPPED; s.buffer = 0; }
        g_buffers.erase(ids[i]);
    }
}
static bool al_alIsBuffer(unsigned b) { std::lock_guard<std::mutex> l(g_al); return g_buffers.count(b) != 0; }
static void fill_buffer(Buffer& buf, int format, const void* data, int size, int freq) {
    buf.ext = nullptr;
    buf.freq = freq > 0 ? freq : 44100;
    buf.channels = (format == AL_FORMAT_STEREO8 || format == AL_FORMAT_STEREO16) ? 2 : 1;
    buf.bits = (format == AL_FORMAT_MONO8 || format == AL_FORMAT_STEREO8) ? 8 : 16;
    if (buf.bits == 8) {
        buf.pcm.resize(size);
        auto* p = static_cast<const uint8_t*>(data);
        for (int i = 0; i < size; ++i) buf.pcm[i] = static_cast<int16_t>((p[i] - 128) << 8);
    } else {
        buf.pcm.assign(static_cast<const int16_t*>(data), static_cast<const int16_t*>(data) + size / 2);
    }
    buf.frames = buf.pcm.size() / buf.channels;
}
static void al_alBufferData(unsigned b, int format, const void* data, int size, int freq) {
    std::lock_guard<std::mutex> l(g_al);
    auto it = g_buffers.find(b);
    if (it == g_buffers.end()) { set_error(AL_INVALID_NAME); return; }
    fill_buffer(it->second, format, data, size, freq);
}
static void al_alBufferDataStatic(unsigned b, int format, const void* data, int size, int freq) {
    std::lock_guard<std::mutex> l(g_al);
    auto it = g_buffers.find(b);
    if (it == g_buffers.end()) { set_error(AL_INVALID_NAME); return; }
    Buffer& buf = it->second;
    if (format == AL_FORMAT_MONO16 || format == AL_FORMAT_STEREO16) {
        buf.pcm.clear();
        buf.ext = static_cast<const int16_t*>(data);
        buf.freq = freq > 0 ? freq : 44100;
        buf.channels = format == AL_FORMAT_STEREO16 ? 2 : 1;
        buf.bits = 16;
        buf.frames = static_cast<size_t>(size) / 2 / buf.channels;
    } else {
        fill_buffer(buf, format, data, size, freq);
    }
}
static void al_alGetBufferi(unsigned b, int p, int* v) {
    std::lock_guard<std::mutex> l(g_al);
    auto it = g_buffers.find(b);
    if (it == g_buffers.end() || !v) { set_error(AL_INVALID_NAME); return; }
    const Buffer& buf = it->second;
    switch (p) {
        case AL_FREQUENCY: *v = buf.freq; break;
        case AL_BITS: *v = buf.bits; break;
        case AL_CHANNELS: *v = buf.channels; break;
        case AL_SIZE: *v = static_cast<int>(buf.frames * buf.channels * 2); break;
        default: set_error(AL_INVALID_ENUM);
    }
}

static void al_alGenSources(int n, unsigned* out) {
    start_mixer();
    std::lock_guard<std::mutex> l(g_al);
    for (int i = 0; i < n; ++i) { out[i] = g_next_source++; g_sources[out[i]]; }
}
static void al_alDeleteSources(int n, const unsigned* ids) {
    std::lock_guard<std::mutex> l(g_al);
    for (int i = 0; i < n; ++i) g_sources.erase(ids[i]);
}
static bool al_alIsSource(unsigned s) { std::lock_guard<std::mutex> l(g_al); return g_sources.count(s) != 0; }
static Source* src(unsigned id) {
    auto it = g_sources.find(id);
    if (it == g_sources.end()) { set_error(AL_INVALID_NAME); return nullptr; }
    return &it->second;
}
static void al_alSourcef(unsigned id, int p, float v) {
    std::lock_guard<std::mutex> l(g_al);
    Source* s = src(id);
    if (!s) return;
    switch (p) {
        case AL_GAIN: s->gain = v; break;
        case AL_PITCH: s->pitch = v; break;
        case AL_MIN_GAIN: s->min_gain = v; break;
        case AL_MAX_GAIN: s->max_gain = v; break;
        case AL_REFERENCE_DISTANCE: s->ref_dist = v; break;
        case AL_ROLLOFF_FACTOR: s->rolloff = v; break;
        case AL_MAX_DISTANCE: s->max_dist = v; break;
        case AL_SEC_OFFSET: if (const Buffer* b = current_buffer(*s)) s->cursor = v * b->freq; break;
        case AL_SAMPLE_OFFSET: s->cursor = v; break;
        default: break;
    }
}
static void al_alSource3f(unsigned id, int p, float x, float y, float z) {
    std::lock_guard<std::mutex> l(g_al);
    Source* s = src(id);
    if (!s) return;
    float* t = p == AL_POSITION ? s->pos : p == AL_VELOCITY ? s->vel : p == AL_DIRECTION ? s->dir : nullptr;
    if (t) { t[0] = x; t[1] = y; t[2] = z; }
}
static void al_alSourcefv(unsigned id, int p, const float* v) {
    if (!v) return;
    if (p == AL_POSITION || p == AL_VELOCITY || p == AL_DIRECTION) al_alSource3f(id, p, v[0], v[1], v[2]);
    else al_alSourcef(id, p, v[0]);
}
static void al_alSourcei(unsigned id, int p, int v) {
    std::lock_guard<std::mutex> l(g_al);
    Source* s = src(id);
    if (!s) return;
    switch (p) {
        case AL_BUFFER:
            if (v && !g_buffers.count(static_cast<unsigned>(v))) { set_error(AL_INVALID_VALUE); return; }
            s->buffer = static_cast<unsigned>(v);
            s->queue.clear();
            s->queue_pos = s->processed = 0;
            s->cursor = 0;
            if (s->state == AL_STOPPED) s->state = AL_INITIAL;
            break;
        case AL_LOOPING: s->looping = v != 0; break;
        case AL_SOURCE_RELATIVE: s->relative = v != 0; break;
        case AL_SAMPLE_OFFSET: s->cursor = v; break;
        case AL_BYTE_OFFSET: if (const Buffer* b = current_buffer(*s)) s->cursor = static_cast<double>(v) / (2 * b->channels); break;
        case AL_SEC_OFFSET: if (const Buffer* b = current_buffer(*s)) s->cursor = static_cast<double>(v) * b->freq; break;
        case AL_GAIN: s->gain = static_cast<float>(v); break;
        case AL_REFERENCE_DISTANCE: s->ref_dist = static_cast<float>(v); break;
        case AL_ROLLOFF_FACTOR: s->rolloff = static_cast<float>(v); break;
        case AL_MAX_DISTANCE: s->max_dist = static_cast<float>(v); break;
        default: break;
    }
}
static void al_alSource3i(unsigned id, int p, int x, int y, int z) { al_alSource3f(id, p, static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)); }
static void al_alSourceiv(unsigned id, int p, const int* v) { if (v) al_alSourcei(id, p, v[0]); }
static void al_alGetSourcef(unsigned id, int p, float* v) {
    std::lock_guard<std::mutex> l(g_al);
    Source* s = src(id);
    if (!s || !v) return;
    switch (p) {
        case AL_GAIN: *v = s->gain; break;
        case AL_PITCH: *v = s->pitch; break;
        case AL_MIN_GAIN: *v = s->min_gain; break;
        case AL_MAX_GAIN: *v = s->max_gain; break;
        case AL_SEC_OFFSET: { const Buffer* b = current_buffer(*s); *v = b ? static_cast<float>(s->cursor / b->freq) : 0; break; }
        case AL_SAMPLE_OFFSET: *v = static_cast<float>(s->cursor); break;
        case AL_REFERENCE_DISTANCE: *v = s->ref_dist; break;
        case AL_ROLLOFF_FACTOR: *v = s->rolloff; break;
        case AL_MAX_DISTANCE: *v = s->max_dist; break;
        default: *v = 0; break;
    }
}
static void al_alGetSource3f(unsigned id, int p, float* x, float* y, float* z) {
    std::lock_guard<std::mutex> l(g_al);
    Source* s = src(id);
    if (!s) return;
    const float* t = p == AL_POSITION ? s->pos : p == AL_VELOCITY ? s->vel : s->dir;
    if (x) *x = t[0];
    if (y) *y = t[1];
    if (z) *z = t[2];
}
static void al_alGetSourcefv(unsigned id, int p, float* v) {
    if (!v) return;
    if (p == AL_POSITION || p == AL_VELOCITY || p == AL_DIRECTION) al_alGetSource3f(id, p, v, v + 1, v + 2);
    else al_alGetSourcef(id, p, v);
}
static void al_alGetSourcei(unsigned id, int p, int* v) {
    std::lock_guard<std::mutex> l(g_al);
    Source* s = src(id);
    if (!s || !v) return;
    switch (p) {
        case AL_SOURCE_STATE: *v = s->state; break;
        case AL_BUFFER: *v = static_cast<int>(s->buffer); break;
        case AL_LOOPING: *v = s->looping; break;
        case AL_SOURCE_RELATIVE: *v = s->relative; break;
        case AL_BUFFERS_QUEUED: *v = static_cast<int>(s->queue.size()); break;
        case AL_BUFFERS_PROCESSED: *v = static_cast<int>(std::min(s->processed, s->queue.size())); break;
        case AL_SAMPLE_OFFSET: *v = static_cast<int>(s->cursor); break;
        case AL_BYTE_OFFSET: { const Buffer* b = current_buffer(*s); *v = b ? static_cast<int>(s->cursor) * 2 * b->channels : 0; break; }
        case AL_SEC_OFFSET: { const Buffer* b = current_buffer(*s); *v = b ? static_cast<int>(s->cursor / b->freq) : 0; break; }
        case AL_SOURCE_TYPE: *v = s->streaming() ? AL_STREAMING : s->buffer ? AL_STATIC : AL_UNDETERMINED; break;
        default: *v = 0; break;
    }
}
static void al_alGetSourceiv(unsigned id, int p, int* v) { al_alGetSourcei(id, p, v); }
static void al_play(unsigned id) {
    Source* s = src(id);
    if (!s) return;
    if (s->state == AL_PAUSED) { s->state = AL_PLAYING; return; }
    s->cursor = 0;
    s->queue_pos = 0;
    s->processed = 0;
    s->state = AL_PLAYING;
}
static void al_alSourcePlay(unsigned id) { std::lock_guard<std::mutex> l(g_al); al_play(id); }
static void al_alSourcePlayv(int n, const unsigned* ids) { std::lock_guard<std::mutex> l(g_al); for (int i = 0; i < n; ++i) al_play(ids[i]); }
static void al_stop(unsigned id) {
    Source* s = src(id);
    if (!s) return;
    if (s->state != AL_INITIAL) s->state = AL_STOPPED;
    s->cursor = 0;
    if (s->streaming()) { s->processed = s->queue.size(); s->queue_pos = s->queue.size(); }
}
static void al_alSourceStop(unsigned id) { std::lock_guard<std::mutex> l(g_al); al_stop(id); }
static void al_alSourceStopv(int n, const unsigned* ids) { std::lock_guard<std::mutex> l(g_al); for (int i = 0; i < n; ++i) al_stop(ids[i]); }
static void al_alSourcePause(unsigned id) { std::lock_guard<std::mutex> l(g_al); if (Source* s = src(id)) if (s->state == AL_PLAYING) s->state = AL_PAUSED; }
static void al_alSourcePausev(int n, const unsigned* ids) { for (int i = 0; i < n; ++i) al_alSourcePause(ids[i]); }
static void al_alSourceRewind(unsigned id) { std::lock_guard<std::mutex> l(g_al); if (Source* s = src(id)) { s->state = AL_INITIAL; s->cursor = 0; s->queue_pos = s->processed = 0; } }
static void al_alSourceRewindv(int n, const unsigned* ids) { for (int i = 0; i < n; ++i) al_alSourceRewind(ids[i]); }
static void al_alSourceQueueBuffers(unsigned id, int n, const unsigned* bufs) {
    std::lock_guard<std::mutex> l(g_al);
    Source* s = src(id);
    if (!s) return;
    if (s->buffer && s->queue.empty()) { s->queue.push_back(s->buffer); s->buffer = 0; }
    bool was_drained = s->streaming() && s->queue_pos >= s->queue.size();
    for (int i = 0; i < n; ++i) s->queue.push_back(bufs[i]);
    if (was_drained) s->cursor = 0;
}
static void al_alSourceUnqueueBuffers(unsigned id, int n, unsigned* out) {
    std::lock_guard<std::mutex> l(g_al);
    Source* s = src(id);
    if (!s) return;
    size_t done = std::min(s->processed, s->queue.size());
    if (static_cast<size_t>(n) > done) { set_error(AL_INVALID_VALUE); return; }
    for (int i = 0; i < n; ++i) out[i] = s->queue[i];
    s->queue.erase(s->queue.begin(), s->queue.begin() + n);
    s->processed -= n;
    s->queue_pos = s->queue_pos >= static_cast<size_t>(n) ? s->queue_pos - n : 0;
}
static void al_alListenerf(int p, float v) { if (p == AL_GAIN) { std::lock_guard<std::mutex> l(g_al); g_listener_gain = v; } }
static void al_alListener3f(int p, float x, float y, float z) {
    if (p == AL_POSITION) { std::lock_guard<std::mutex> l(g_al); g_listener_pos[0] = x; g_listener_pos[1] = y; g_listener_pos[2] = z; }
}
static void al_alListenerfv(int p, const float* v) { if (!v) return; if (p == AL_GAIN) al_alListenerf(p, v[0]); else if (p == AL_POSITION) al_alListener3f(p, v[0], v[1], v[2]); }
static void al_alListeneri(int, int) {}
static void al_alGetListenerf(int p, float* v) { if (v) *v = p == AL_GAIN ? g_listener_gain : 0; }
static void al_alGetListener3f(int, float* x, float* y, float* z) { if (x) *x = g_listener_pos[0]; if (y) *y = g_listener_pos[1]; if (z) *z = g_listener_pos[2]; }
static void al_alGetListenerfv(int p, float* v) { if (!v) return; if (p == AL_GAIN) *v = g_listener_gain; else for (int i = 0; i < 3; ++i) v[i] = g_listener_pos[i]; }
static void al_alDistanceModel(int) {}
static void al_alDopplerFactor(float) {}
static void al_alDopplerVelocity(float) {}
static void al_alSpeedOfSound(float) {}
static void al_alEnable(int) {}
static void al_alDisable(int) {}
static bool al_alIsEnabled(int) { return false; }
static const char* al_alGetString(int p) {
    switch (p) {
        case AL_VENDOR: return "xnuroid";
        case AL_VERSION: return "1.1";
        case AL_RENDERER: return "software mixer";
        case AL_EXTENSIONS: return "";
        default: return "";
    }
}
static bool al_alIsExtensionPresent(const char*) { return false; }
static int al_alGetEnumValue(const char*) { return 0; }
static void* al_alGetProcAddress(const char* name) { return al_alcGetProcAddress(nullptr, name); }
static int al_alGetInteger(int) { return 0; }
static float al_alGetFloat(int) { return 0; }

// ---------------------------------------------------------------------------------------------------------------
// Audio File Services (PCM only)
// ---------------------------------------------------------------------------------------------------------------
struct AudioFile {
    double rate = 44100;
    uint32_t channels = 1, bits = 16, flags = 12, bytes_per_frame = 2;
    bool float_fmt = false;
    std::string data;   // PCM payload, little endian
    double duration() const { return bytes_per_frame ? data.size() / bytes_per_frame / rate : 0; }
};
static uint32_t be32(const uint8_t* p) { return static_cast<uint32_t>(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t* p) { return static_cast<uint64_t>(be32(p)) << 32 | be32(p + 4); }
static uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }
static uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }

static bool parse_audio(const std::string& raw, AudioFile& af) {
    const auto* p = reinterpret_cast<const uint8_t*>(raw.data());
    size_t n = raw.size();
    if (n >= 8 && std::memcmp(p, "caff", 4) == 0) {
        size_t i = 8;
        bool big_endian = true;
        while (i + 12 <= n) {
            uint32_t type = be32(p + i);
            int64_t size = static_cast<int64_t>(be64(p + i + 4));
            size_t body = i + 12;
            if (type == 0x64657363) {   // 'desc'
                uint64_t r = be64(p + body);
                std::memcpy(&af.rate, &r, 8);
                uint32_t fmt = be32(p + body + 8), flags = be32(p + body + 12);
                af.bytes_per_frame = be32(p + body + 16);
                af.channels = be32(p + body + 24);
                af.bits = be32(p + body + 28);
                if (fmt != 0x6c70636d) { log("CAF format %.4s not supported", reinterpret_cast<const char*>(p + body + 8)); return false; }
                af.float_fmt = flags & 1;
                big_endian = !(flags & 2);
            } else if (type == 0x64617461) {   // 'data' (4-byte edit count first)
                size_t len = size < 0 ? n - body - 4 : static_cast<size_t>(size) - 4;
                if (body + 4 + len > n) len = n - body - 4;
                af.data.assign(raw, body + 4, len);
                if (big_endian && af.bits == 16)
                    for (size_t k = 0; k + 1 < af.data.size(); k += 2) std::swap(af.data[k], af.data[k + 1]);
            }
            if (size < 0) break;
            i = body + static_cast<size_t>(size);
        }
    } else if (n >= 12 && std::memcmp(p, "RIFF", 4) == 0 && std::memcmp(p + 8, "WAVE", 4) == 0) {
        size_t i = 12;
        while (i + 8 <= n) {
            uint32_t size = le32(p + i + 4);
            if (std::memcmp(p + i, "fmt ", 4) == 0) {
                uint16_t fmt = le16(p + i + 8);
                af.channels = le16(p + i + 10);
                af.rate = le32(p + i + 12);
                af.bytes_per_frame = le16(p + i + 20);
                af.bits = le16(p + i + 22);
                af.float_fmt = fmt == 3;
                if (fmt != 1 && fmt != 3 && fmt != 0xfffe) { log("WAV format %u not supported", fmt); return false; }
            } else if (std::memcmp(p + i, "data", 4) == 0) {
                af.data.assign(raw, i + 8, std::min<size_t>(size, n - i - 8));
            }
            i += 8 + size + (size & 1);
        }
    } else if (n >= 12 && std::memcmp(p, "FORM", 4) == 0 && (std::memcmp(p + 8, "AIFF", 4) == 0 || std::memcmp(p + 8, "AIFC", 4) == 0)) {
        size_t i = 12;
        while (i + 8 <= n) {
            uint32_t size = be32(p + i + 4);
            if (std::memcmp(p + i, "COMM", 4) == 0) {
                af.channels = static_cast<uint32_t>(p[i + 8] << 8 | p[i + 9]);
                af.bits = static_cast<uint32_t>(p[i + 14] << 8 | p[i + 15]);
                const uint8_t* e = p + i + 16;   // 80-bit extended sample rate
                int exp = ((e[0] & 0x7f) << 8 | e[1]) - 16383;
                uint64_t mant = be64(e + 2);
                af.rate = std::ldexp(static_cast<double>(mant), exp - 63);
                af.bytes_per_frame = af.channels * af.bits / 8;
            } else if (std::memcmp(p + i, "SSND", 4) == 0) {
                uint32_t off = be32(p + i + 8);
                af.data.assign(raw, i + 16 + off, size - 8 - off);
                if (af.bits == 16) for (size_t k = 0; k + 1 < af.data.size(); k += 2) std::swap(af.data[k], af.data[k + 1]);
            }
            i += 8 + size + (size & 1);
        }
    } else {
        return false;
    }
    af.flags = af.float_fmt ? 9 : 12;   // IsFloat|IsPacked or IsSignedInteger|IsPacked
    if (!af.bytes_per_frame) af.bytes_per_frame = af.channels * af.bits / 8;
    return !af.data.empty() || true;
}

static std::string url_to_path(id url) {
    if (!url) return "";
    if (is_string(url)) return utf8(url);
    return utf8(send(url, "path"));
}

static int32_t af_open_url(id url, int8_t, uint32_t, AudioFile** out) {
    std::string path = remap_path(url_to_path(url));
    std::string raw;
    if (!read_file(path, raw)) { log("AudioFileOpenURL: cannot read %s", path.c_str()); return -43; }   // fnfErr
    auto* af = new AudioFile();
    if (!parse_audio(raw, *af)) { delete af; log("AudioFileOpenURL: unsupported file %s", path.c_str()); return 0x7479703f; }   // 'typ?'
    *out = af;
    return 0;
}
static int32_t af_close(AudioFile* af) { delete af; return 0; }
static int32_t af_get_property_info(AudioFile*, uint32_t prop, uint32_t* size, uint32_t* writable) {
    if (writable) *writable = 0;
    if (size) *size = prop == 0x64666d74 ? 40 : 8;
    return 0;
}
static int32_t af_get_property(AudioFile* af, uint32_t prop, uint32_t* size, void* out) {
    if (!af || !out) return -50;
    switch (prop) {
        case 0x64666d74: {   // 'dfmt' AudioStreamBasicDescription
            struct ASBD { double rate; uint32_t fmt, flags, bytes_per_packet, frames_per_packet, bytes_per_frame, channels, bits, reserved; } d;
            d.rate = af->rate;
            d.fmt = 0x6c70636d;
            d.flags = af->flags;
            d.bytes_per_packet = d.bytes_per_frame = af->bytes_per_frame;
            d.frames_per_packet = 1;
            d.channels = af->channels;
            d.bits = af->bits;
            d.reserved = 0;
            std::memcpy(out, &d, std::min<uint32_t>(size ? *size : 40, 40));
            if (size) *size = 40;
            return 0;
        }
        case 0x62636e74: { uint64_t v = af->data.size(); std::memcpy(out, &v, 8); if (size) *size = 8; return 0; }   // 'bcnt'
        case 0x70636e74: { uint64_t v = af->bytes_per_frame ? af->data.size() / af->bytes_per_frame : 0; std::memcpy(out, &v, 8); if (size) *size = 8; return 0; }   // 'pcnt'
        case 0x65647572: { double v = af->duration(); std::memcpy(out, &v, 8); if (size) *size = 8; return 0; }   // 'edur'
        case 0x7073697a: case 0x706b7562: { uint32_t v = af->bytes_per_frame; std::memcpy(out, &v, 4); if (size) *size = 4; return 0; }   // 'psze' 'pkub'
        case 0x66666d74: { uint32_t v = 0x63616666; std::memcpy(out, &v, 4); if (size) *size = 4; return 0; }   // 'ffmt'
        default:
            log("AudioFileGetProperty '%c%c%c%c' not supported", prop >> 24, (prop >> 16) & 0xff, (prop >> 8) & 0xff, prop & 0xff);
            return 0x70747970;   // 'pty?'
    }
}
static int32_t af_read_bytes(AudioFile* af, bool, int64_t start, uint32_t* n, void* out) {
    if (!af || !n) return -50;
    if (start < 0 || static_cast<size_t>(start) >= af->data.size()) { *n = 0; return -39; }   // eofErr
    size_t k = std::min<size_t>(*n, af->data.size() - static_cast<size_t>(start));
    std::memcpy(out, af->data.data() + start, k);
    bool short_read = k < *n;
    *n = static_cast<uint32_t>(k);
    return short_read ? -39 : 0;
}
static int32_t af_read_packets(AudioFile* af, bool, uint32_t* out_bytes, void*, int64_t start_packet, uint32_t* packets, void* out) {
    if (!af || !packets) return -50;
    uint32_t bytes = *packets * af->bytes_per_frame;
    int32_t r = af_read_bytes(af, false, start_packet * af->bytes_per_frame, &bytes, out);
    *packets = af->bytes_per_frame ? bytes / af->bytes_per_frame : 0;
    if (out_bytes) *out_bytes = bytes;
    return r == -39 && bytes ? 0 : r;
}
static int32_t af_read_packet_data(AudioFile* af, bool c, uint32_t* io_bytes, void* descs, int64_t start, uint32_t* packets, void* out) {
    return af_read_packets(af, c, io_bytes, descs, start, packets, out);
}

// ---------------------------------------------------------------------------------------------------------------
// System Sound Services / Audio Session (C API)
// ---------------------------------------------------------------------------------------------------------------
static std::mutex g_ss_mu;
static std::map<uint32_t, unsigned> g_system_sounds;   // SystemSoundID -> AL buffer
static uint32_t g_next_ss = 4096;
static unsigned g_ss_source = 0;

static int32_t ss_create(id url, uint32_t* out) {
    AudioFile* af = nullptr;
    if (af_open_url(url, 1, 0, &af) != 0) return -1500;
    unsigned buf;
    al_alGenBuffers(1, &buf);
    int format = af->channels == 2 ? (af->bits == 8 ? AL_FORMAT_STEREO8 : AL_FORMAT_STEREO16) : (af->bits == 8 ? AL_FORMAT_MONO8 : AL_FORMAT_MONO16);
    al_alBufferData(buf, format, af->data.data(), static_cast<int>(af->data.size()), static_cast<int>(af->rate));
    delete af;
    std::lock_guard<std::mutex> l(g_ss_mu);
    uint32_t sid = g_next_ss++;
    g_system_sounds[sid] = buf;
    if (out) *out = sid;
    return 0;
}
static int32_t ss_dispose(uint32_t sid) {
    std::lock_guard<std::mutex> l(g_ss_mu);
    auto it = g_system_sounds.find(sid);
    if (it != g_system_sounds.end()) { al_alDeleteBuffers(1, &it->second); g_system_sounds.erase(it); }
    return 0;
}
static void ss_play(uint32_t sid) {
    if (sid == 0xFFF) { host_vibrate(400, 255); return; }   // kSystemSoundID_Vibrate
    unsigned buf = 0;
    {
        std::lock_guard<std::mutex> l(g_ss_mu);
        auto it = g_system_sounds.find(sid);
        if (it == g_system_sounds.end()) return;
        buf = it->second;
        if (!g_ss_source) al_alGenSources(1, &g_ss_source);
    }
    al_alSourceStop(g_ss_source);
    al_alSourcei(g_ss_source, AL_BUFFER, static_cast<int>(buf));
    al_alSourcePlay(g_ss_source);
}
static void ss_play_completion(uint32_t sid, id) { ss_play(sid); }
static int32_t ss_add_completion(uint32_t, void*, void*, void*, void*) { return 0; }
static void ss_remove_completion(uint32_t) {}
static int32_t ss_get_property(uint32_t, uint32_t, const void*, uint32_t* size, void* out) { if (out && size) std::memset(out, 0, *size); return 0; }
static int32_t ss_set_property(uint32_t, uint32_t, const void*, uint32_t, const void*) { return 0; }
static int32_t session_zero() { return 0; }
static int32_t session_get_property(uint32_t prop, uint32_t* size, void* out) {
    if (!out || !size) return 0;
    std::memset(out, 0, *size);
    if (prop == 0x63687372 && *size >= 8) { double v = 44100; std::memcpy(out, &v, 8); }   // 'chsr' current hardware sample rate
    if (prop == 0x636f6863 && *size >= 4) { uint32_t v = 2; std::memcpy(out, &v, 4); }      // 'choc' output channels
    return 0;
}

}  // extern "C"

namespace app {

void init_audio() {
    Class S = def_class("AVAudioSession", "NSObject");
    static id session = alloc(S);
    defc(S, "sharedInstance", +[](id, SEL) -> id { return session; });
    def(S, "setCategory:error:", +[](id, SEL, id, id* e) -> bool { if (e) *e = nullptr; return true; });
    def(S, "setCategory:withOptions:error:", +[](id, SEL, id, NSUInteger, id* e) -> bool { if (e) *e = nullptr; return true; });
    def(S, "setCategory:mode:options:error:", +[](id, SEL, id, id, NSUInteger, id* e) -> bool { if (e) *e = nullptr; return true; });
    def(S, "setActive:error:", +[](id, SEL, bool, id* e) -> bool { if (e) *e = nullptr; return true; });
    def(S, "setActive:withOptions:error:", +[](id, SEL, bool, NSUInteger, id* e) -> bool { if (e) *e = nullptr; return true; });
    def(S, "setMode:error:", +[](id, SEL, id, id* e) -> bool { if (e) *e = nullptr; return true; });
    def(S, "setPreferredSampleRate:error:", +[](id, SEL, double, id* e) -> bool { if (e) *e = nullptr; return true; });
    def(S, "setPreferredIOBufferDuration:error:", +[](id, SEL, double, id* e) -> bool { if (e) *e = nullptr; return true; });
    def(S, "setPreferredHardwareSampleRate:error:", +[](id, SEL, double, id* e) -> bool { if (e) *e = nullptr; return true; });
    def(S, "setDelegate:", +[](id, SEL, id) {});
    def(S, "category", +[](id, SEL) -> id { return str("AVAudioSessionCategorySoloAmbient"); });
    def(S, "isOtherAudioPlaying", +[](id, SEL) -> bool { return false; });
    def(S, "otherAudioPlaying", +[](id, SEL) -> bool { return false; });
    def(S, "secondaryAudioShouldBeSilencedHint", +[](id, SEL) -> bool { return false; });
    def(S, "sampleRate", +[](id, SEL) -> double { return g_out_rate; });
    def(S, "currentHardwareSampleRate", +[](id, SEL) -> double { return g_out_rate; });
    def(S, "preferredSampleRate", +[](id, SEL) -> double { return g_out_rate; });
    def(S, "outputVolume", +[](id, SEL) -> float { return 1.0f; });
    def(S, "IOBufferDuration", +[](id, SEL) -> double { return 0.01; });
    def(S, "outputNumberOfChannels", +[](id, SEL) -> long { return 2; });
    def(S, "currentRoute", +[](id, SEL) -> id { return nullptr; });
    def(S, "recordPermission", +[](id, SEL) -> NSUInteger { return 0x64656e79; });
    def(S, "requestRecordPermission:", +[](id, SEL, id) {});
}

void register_audio_symbols(std::unordered_map<std::string, void*>& m) {
    auto r = [&](const char* n, auto f) { m[n] = reinterpret_cast<void*>(f); };
    r("_alcOpenDevice", &al_alcOpenDevice);
    r("_alcCloseDevice", &al_alcCloseDevice);
    r("_alcCreateContext", &al_alcCreateContext);
    r("_alcMakeContextCurrent", &al_alcMakeContextCurrent);
    r("_alcDestroyContext", &al_alcDestroyContext);
    r("_alcGetCurrentContext", &al_alcGetCurrentContext);
    r("_alcGetContextsDevice", &al_alcGetContextsDevice);
    r("_alcProcessContext", &al_alcProcessContext);
    r("_alcSuspendContext", &al_alcSuspendContext);
    r("_alcGetError", &al_alcGetError);
    r("_alcGetString", &al_alcGetString);
    r("_alcGetIntegerv", &al_alcGetIntegerv);
    r("_alcIsExtensionPresent", &al_alcIsExtensionPresent);
    r("_alcGetProcAddress", &al_alcGetProcAddress);
    r("_alGetError", &al_alGetError);
    r("_alGenBuffers", &al_alGenBuffers);
    r("_alDeleteBuffers", &al_alDeleteBuffers);
    r("_alIsBuffer", &al_alIsBuffer);
    r("_alBufferData", &al_alBufferData);
    r("_alGetBufferi", &al_alGetBufferi);
    r("_alGenSources", &al_alGenSources);
    r("_alDeleteSources", &al_alDeleteSources);
    r("_alIsSource", &al_alIsSource);
    r("_alSourcef", &al_alSourcef);
    r("_alSource3f", &al_alSource3f);
    r("_alSourcefv", &al_alSourcefv);
    r("_alSourcei", &al_alSourcei);
    r("_alSource3i", &al_alSource3i);
    r("_alSourceiv", &al_alSourceiv);
    r("_alGetSourcef", &al_alGetSourcef);
    r("_alGetSource3f", &al_alGetSource3f);
    r("_alGetSourcefv", &al_alGetSourcefv);
    r("_alGetSourcei", &al_alGetSourcei);
    r("_alGetSourceiv", &al_alGetSourceiv);
    r("_alSourcePlay", &al_alSourcePlay);
    r("_alSourcePlayv", &al_alSourcePlayv);
    r("_alSourceStop", &al_alSourceStop);
    r("_alSourceStopv", &al_alSourceStopv);
    r("_alSourcePause", &al_alSourcePause);
    r("_alSourcePausev", &al_alSourcePausev);
    r("_alSourceRewind", &al_alSourceRewind);
    r("_alSourceRewindv", &al_alSourceRewindv);
    r("_alSourceQueueBuffers", &al_alSourceQueueBuffers);
    r("_alSourceUnqueueBuffers", &al_alSourceUnqueueBuffers);
    r("_alListenerf", &al_alListenerf);
    r("_alListener3f", &al_alListener3f);
    r("_alListenerfv", &al_alListenerfv);
    r("_alListeneri", &al_alListeneri);
    r("_alGetListenerf", &al_alGetListenerf);
    r("_alGetListener3f", &al_alGetListener3f);
    r("_alGetListenerfv", &al_alGetListenerfv);
    r("_alDistanceModel", &al_alDistanceModel);
    r("_alDopplerFactor", &al_alDopplerFactor);
    r("_alDopplerVelocity", &al_alDopplerVelocity);
    r("_alSpeedOfSound", &al_alSpeedOfSound);
    r("_alEnable", &al_alEnable);
    r("_alDisable", &al_alDisable);
    r("_alIsEnabled", &al_alIsEnabled);
    r("_alGetString", &al_alGetString);
    r("_alIsExtensionPresent", &al_alIsExtensionPresent);
    r("_alGetEnumValue", &al_alGetEnumValue);
    r("_alGetProcAddress", &al_alGetProcAddress);
    r("_alGetInteger", &al_alGetInteger);
    r("_alGetFloat", &al_alGetFloat);
    r("_AudioFileOpenURL", &af_open_url);
    r("_AudioFileClose", &af_close);
    r("_AudioFileGetPropertyInfo", &af_get_property_info);
    r("_AudioFileGetProperty", &af_get_property);
    r("_AudioFileReadBytes", &af_read_bytes);
    r("_AudioFileReadPackets", &af_read_packets);
    r("_AudioFileReadPacketData", &af_read_packet_data);
    r("_AudioServicesCreateSystemSoundID", &ss_create);
    r("_AudioServicesDisposeSystemSoundID", &ss_dispose);
    r("_AudioServicesPlaySystemSound", &ss_play);
    r("_AudioServicesPlayAlertSound", &ss_play);
    r("_AudioServicesPlaySystemSoundWithCompletion", &ss_play_completion);
    r("_AudioServicesAddSystemSoundCompletion", &ss_add_completion);
    r("_AudioServicesRemoveSystemSoundCompletion", &ss_remove_completion);
    r("_AudioServicesGetProperty", &ss_get_property);
    r("_AudioServicesSetProperty", &ss_set_property);
    for (const char* n : {"_AudioSessionInitialize", "_AudioSessionSetActive", "_AudioSessionSetProperty", "_AudioSessionAddPropertyListener",
                          "_AudioSessionRemovePropertyListenerWithUserData", "_AudioSessionSetActiveWithFlags"})
        r(n, &session_zero);
    r("_AudioSessionGetProperty", &session_get_property);
}

}  // namespace app
