// AudioUnit RemoteIO emulation on AAudio.
//
// The guest (FMOD inside Unity) configures one output unit with an AudioStreamBasicDescription and a render
// callback, then starts it. Here the unit opens an AAudio stream in the guest's own format (AAudio resamples), and
// AAudio's real-time data callback calls the guest's render callback with an AudioBufferList pointing straight at
// the output buffer (interleaved) or at scratch planes that are then interleaved (non-interleaved formats).
// Without an AAudio device, or with LOADER_AUDIO=0, a timed thread drives the callback and discards the samples.
// LOADER_AUDIO_WAV=<path> additionally records what the guest rendered.
#include <aaudio/AAudio.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <set>
#include <thread>
#include <vector>


extern "C" void loader_bind_thread();   // android_loader.cpp

namespace {

struct RenderCallback { int32_t (*proc)(void*, uint32_t*, const void*, uint32_t, uint32_t, void*); void* refcon; };

// PCM layout of an AudioStreamBasicDescription, as far as converting samples needs it
struct Fmt {
    double rate = 44100.0;
    uint32_t channels = 2, bits = 32, flags = 0x9;
    uint32_t bps() const { return bits / 8 ? bits / 8 : 4; }
    bool planar() const { return (flags & 0x20) != 0; }   // kAudioFormatFlagIsNonInterleaved
};

// One input of a mixer unit: its own callback and format, gain/pan/pitch, and the resampler's pending input
// (stereo float frames pulled but not yet consumed).
struct Bus {
    RenderCallback cb{nullptr, nullptr};
    Fmt fmt;
    float volume = 1.f, pan = 0.f, playback = 1.f;
    bool enabled = true;
    std::vector<float> in;
    double pos = 0, sample_time = 0;
    std::vector<uint8_t> raw, abl;
};

struct Unit;
// where a mixer's output goes: an output unit, or an input bus of another mixer
struct Dest { Unit* unit = nullptr; int bus = -1; };

struct Unit {
    RenderCallback cb{nullptr, nullptr};
    double rate = 44100.0;
    uint32_t channels = 2, bits = 32, flags = 0x9;    // default: interleaved float32 (IsFloat | IsPacked)
    bool non_interleaved = false, is_float = true;
    AAudioStream* stream = nullptr;
    std::atomic<bool> running{false};
    std::thread pump;
    double sample_time = 0;
    std::vector<uint8_t> planes, abl;
    FILE* wav = nullptr;
    uint32_t wav_bytes = 0;
    uint64_t callbacks = 0;
    double level_since = 0;   // output level report
    int level_peak = 0, level_silent = 0, level_buffers = 0;
    // mixer units (MultiChannelMixer, 3D mixer; converters and effects pass through as a one-bus mixer)
    bool mixer = false, mixer3d = false;
    std::vector<Bus> buses;
    uint32_t bus_count = 0;
    float out_volume = 1.f;
    Fmt out_fmt{44100.0, 2, 32, 0x9};
    Dest dest;
    std::vector<float> mix;
};

uint32_t bytes_per_sample(const Unit* u) { return u->bits / 8 ? u->bits / 8 : 4; }

// live units, so the host can pause every output stream when the app goes to the background
std::mutex g_units_mu;
std::set<Unit*> g_units;
std::atomic<bool> g_paused{false};

void wav_header(FILE* f, const Unit* u, uint32_t data_bytes) {
    uint32_t rate = static_cast<uint32_t>(u->rate), bps = bytes_per_sample(u), block = u->channels * bps;
    uint16_t fmt = u->is_float ? 3 : 1, ch = static_cast<uint16_t>(u->channels), bits = static_cast<uint16_t>(u->bits), blk = static_cast<uint16_t>(block);
    uint32_t byte_rate = rate * block, riff = 36 + data_bytes, sixteen = 16;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&sixteen, 4, 1, f);
    fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&byte_rate, 4, 1, f);
    fwrite(&blk, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data_bytes, 4, 1, f);
    fseek(f, 0, SEEK_END);
}

// Calls the guest render callback for `frames` frames and leaves interleaved samples in `out`.
void render(Unit* u, void* out, uint32_t frames) {
    const uint32_t bps = bytes_per_sample(u), frame_bytes = u->channels * bps;
    struct Buffer { uint32_t channels, bytes; void* data; };
    const uint32_t nbufs = u->non_interleaved ? u->channels : 1;
    u->abl.resize(8 + sizeof(Buffer) * nbufs);
    auto* count = reinterpret_cast<uint32_t*>(u->abl.data());
    auto* bufs = reinterpret_cast<Buffer*>(u->abl.data() + 8);
    *count = nbufs;
    if (u->non_interleaved) {
        u->planes.resize(static_cast<size_t>(frames) * frame_bytes);
        for (uint32_t c = 0; c < nbufs; ++c) bufs[c] = {1, frames * bps, u->planes.data() + static_cast<size_t>(c) * frames * bps};
    } else {
        bufs[0] = {u->channels, frames * frame_bytes, out};
    }
    std::memset(u->non_interleaved ? u->planes.data() : static_cast<uint8_t*>(out), 0, static_cast<size_t>(frames) * frame_bytes);
    alignas(8) uint8_t ts[64] = {};   // AudioTimeStamp: mSampleTime first, flags at +56
    std::memcpy(ts, &u->sample_time, 8);
    uint32_t ts_flags = 1;   // kAudioTimeStampSampleTimeValid
    std::memcpy(ts + 56, &ts_flags, 4);
    uint32_t action = 0;
    if (u->cb.proc) u->cb.proc(u->cb.refcon, &action, ts, 0, frames, u->abl.data());
    if (u->callbacks++ == 0) fprintf(stderr, "[AUDIO] first render callback returned (%u frames)\n", frames);
    u->sample_time += frames;
    // LOADER_AUDIO_LEVELS=1: level report every 5 s (16-bit output), to tell "the app renders silence" from "the output path drops it"
    static const bool levels = getenv("LOADER_AUDIO_LEVELS") != nullptr;
    if (levels && !u->is_float && bps == 2) {
        const auto* s = static_cast<const int16_t*>(u->non_interleaved ? static_cast<void*>(u->planes.data()) : out);
        int peak = 0;
        for (size_t i = 0, n = static_cast<size_t>(frames) * u->channels; i < n; ++i) peak = std::max(peak, std::abs(static_cast<int>(s[i])));
        u->level_peak = std::max(u->level_peak, peak);
        u->level_silent += peak == 0;
        ++u->level_buffers;
        if (u->sample_time - u->level_since >= u->rate * 5) {
            fprintf(stderr, "[AUDIO] last 5 s: peak %d, %d of %d buffers silent%s\n", u->level_peak, u->level_silent, u->level_buffers,
                    action & 16 ? " (callback flagged silence)" : "");
            u->level_since = u->sample_time;
            u->level_peak = u->level_silent = u->level_buffers = 0;
        }
    }
    if (u->non_interleaved) {   // planes -> interleaved
        auto* o = static_cast<uint8_t*>(out);
        for (uint32_t f = 0; f < frames; ++f)
            for (uint32_t c = 0; c < u->channels; ++c)
                std::memcpy(o + (static_cast<size_t>(f) * u->channels + c) * bps, u->planes.data() + (static_cast<size_t>(c) * frames + f) * bps, bps);
    }
    if (u->wav) {
        fwrite(out, frame_bytes, frames, u->wav);
        u->wav_bytes += frames * frame_bytes;
        if ((u->callbacks & 63) == 0) wav_header(u->wav, u, u->wav_bytes);
    }
}

aaudio_data_callback_result_t on_audio(AAudioStream*, void* user, void* data, int32_t frames) {
    auto* u = static_cast<Unit*>(user);
    static thread_local bool bound = false;
    if (!bound) { loader_bind_thread(); bound = true; }   // AAudio's thread runs guest code (the render callback)
    if (!u->running || g_paused) {
        std::memset(data, 0, static_cast<size_t>(frames) * u->channels * bytes_per_sample(u));
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }
    render(u, data, static_cast<uint32_t>(frames));
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

bool open_aaudio(Unit* u);
void run_pump(Unit* u);

// The output device went away (headphones unplugged, Bluetooth route change): the stream is dead and its data
// callback is not called again. A new stream is opened on the current default device, from a thread of its own
// because AAudio does not allow closing a stream inside its callbacks.
void reopen_after_disconnect(Unit* u, AAudioStream* dead) {
    {
        std::lock_guard<std::mutex> l(g_units_mu);
        if (!g_units.count(u) || u->stream != dead) return;   // stopped or disposed in the meantime
        u->stream = nullptr;
    }
    AAudioStream_requestStop(dead);
    AAudioStream_close(dead);   // outside the lock, like native_au_stop
    std::lock_guard<std::mutex> l(g_units_mu);
    if (!g_units.count(u) || !u->running || u->stream) return;
    if (open_aaudio(u) && (g_paused || AAudioStream_requestStart(u->stream) == AAUDIO_OK)) return;
    if (u->stream) { AAudioStream_close(u->stream); u->stream = nullptr; }
    fprintf(stderr, "[AUDIO] output not reopened after the route change: rendering into the void\n");
    if (!u->pump.joinable()) u->pump = std::thread(run_pump, u);
}

void on_error(AAudioStream* stream, void* user, aaudio_result_t err) {
    fprintf(stderr, "[AUDIO] AAudio stream error %d\n", err);
    if (err == AAUDIO_ERROR_DISCONNECTED) std::thread(reopen_after_disconnect, static_cast<Unit*>(user), stream).detach();
}

bool open_aaudio(Unit* u) {
    const char* e = getenv("LOADER_AUDIO");
    if (e && e[0] == '0') return false;
    if (u->is_float ? u->bits != 32 : u->bits != 16) return false;   // AAudio takes I16 or 32-bit float
    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return false;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(b, static_cast<int32_t>(u->rate));
    AAudioStreamBuilder_setChannelCount(b, static_cast<int32_t>(u->channels));
    AAudioStreamBuilder_setFormat(b, u->is_float ? AAUDIO_FORMAT_PCM_FLOAT : AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_MUSIC);   // not SONIFICATION: muted with UI sounds on some devices
    AAudioStreamBuilder_setDataCallback(b, on_audio, u);
    AAudioStreamBuilder_setErrorCallback(b, on_error, u);
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &u->stream);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) {
        fprintf(stderr, "[AUDIO] AAudio open failed: %s\n", AAudio_convertResultToText(r));
        u->stream = nullptr;
        return false;
    }
    fprintf(stderr, "[AUDIO] AAudio stream: %d Hz, %d ch, burst %d frames\n", AAudioStream_getSampleRate(u->stream),
            AAudioStream_getChannelCount(u->stream), AAudioStream_getFramesPerBurst(u->stream));
    return true;
}

void run_pump(Unit* u) {
    const uint32_t frames = 512;
    std::vector<uint8_t> out(static_cast<size_t>(frames) * u->channels * bytes_per_sample(u));
    timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    while (u->running) {
        if (!g_paused) render(u, out.data(), frames);
        next.tv_nsec += static_cast<long>(1e9 * frames / u->rate);
        while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr);
    }
}

// ---- mixer units. Each input bus is pulled through its render callback in its own format, converted to stereo
// float, resampled to the destination rate (times the bus's playback rate: 3D mixer pitch), panned, scaled and summed;
// the sum is written in whatever format the destination (output unit or another mixer's bus) takes.

struct AudioBuffer { uint32_t channels, bytes; void* data; };

float load_sample(const uint8_t* p, const Fmt& f) {
    if (f.flags & 1) {   // kAudioFormatFlagIsFloat
        if (f.bits == 64) { double d; std::memcpy(&d, p, 8); return static_cast<float>(d); }
        float v; std::memcpy(&v, p, 4); return v;
    }
    switch (f.bits) {
        case 16: { int16_t v; std::memcpy(&v, p, 2); return v / 32768.f; }
        case 32: {   // plain 32-bit, or iOS's canonical 8.24 fixed point (fraction bits in flags 7..12)
            int32_t v; std::memcpy(&v, p, 4);
            uint32_t frac = (f.flags >> 7) & 63;
            return frac ? static_cast<float>(v) / static_cast<float>(1u << frac) : v / 2147483648.f;
        }
        case 8: return (f.flags & 4) ? static_cast<int8_t>(*p) / 128.f : (static_cast<int>(*p) - 128) / 128.f;
        default: return 0.f;
    }
}

void store_sample(uint8_t* p, const Fmt& f, float v) {
    if (f.flags & 1) {
        if (f.bits == 64) { double d = v; std::memcpy(p, &d, 8); } else std::memcpy(p, &v, 4);
        return;
    }
    v = std::max(-1.f, std::min(1.f, v));
    if (f.bits == 16) { int16_t s = static_cast<int16_t>(v * 32767.f); std::memcpy(p, &s, 2); }
    else if (f.bits == 32) {
        uint32_t frac = (f.flags >> 7) & 63;
        int32_t s = frac ? static_cast<int32_t>(v * static_cast<float>(1u << frac)) : static_cast<int32_t>(v * 2147483520.f);
        std::memcpy(p, &s, 4);
    }
}

// address of channel c of frame i in an AudioBufferList laid out as f
uint8_t* sample_ptr(void* abl, const Fmt& f, uint32_t i, uint32_t c) {
    auto* bufs = reinterpret_cast<AudioBuffer*>(static_cast<uint8_t*>(abl) + 8);
    uint32_t n = *static_cast<uint32_t*>(abl);
    if (f.planar()) return c < n && bufs[c].data ? static_cast<uint8_t*>(bufs[c].data) + static_cast<size_t>(i) * f.bps() : nullptr;
    return n && bufs[0].data ? static_cast<uint8_t*>(bufs[0].data) + (static_cast<size_t>(i) * f.channels + c) * f.bps() : nullptr;
}

// Calls a bus's callback for `frames` frames and appends them to its pending input as stereo float.
void pull_bus(Bus& b, uint32_t index, uint32_t frames) {
    // by value: the guest may set a new format or callback from another thread while this pull is running
    const Fmt f = b.fmt;
    const RenderCallback cb = b.cb;
    const uint32_t bps = f.bps(), nbufs = f.planar() ? f.channels : 1, per_buf = f.planar() ? 1 : f.channels;
    // slack past the buffers, checked after the callback: a callback that writes more than it was given is reported
    // once instead of corrupting the heap
    constexpr size_t kSlack = 4096;
    const size_t bytes = static_cast<size_t>(frames) * f.channels * bps;
    b.raw.assign(bytes + kSlack, 0);
    std::memset(b.raw.data() + bytes, 0xa5, kSlack);
    b.abl.assign(8 + sizeof(AudioBuffer) * nbufs, 0);
    *reinterpret_cast<uint32_t*>(b.abl.data()) = nbufs;
    auto* bufs = reinterpret_cast<AudioBuffer*>(b.abl.data() + 8);
    for (uint32_t i = 0; i < nbufs; ++i) bufs[i] = {per_buf, frames * per_buf * bps, b.raw.data() + static_cast<size_t>(i) * frames * per_buf * bps};
    alignas(8) uint8_t ts[64] = {};
    std::memcpy(ts, &b.sample_time, 8);
    uint32_t ts_flags = 1, action = 0;
    std::memcpy(ts + 56, &ts_flags, 4);
    if (cb.proc) cb.proc(cb.refcon, &action, ts, index, frames, b.abl.data());   // unset meanwhile: silence
    b.sample_time += frames;
    for (size_t i = bytes; i < bytes + kSlack; ++i)
        if (b.raw[i] != 0xa5) {
            static std::atomic<int> reported{0};
            if (reported++ < 4)
                fprintf(stderr, "[AUDIO] mixer bus %u callback wrote past its %zu-byte buffer (%u frames, %u ch, %u bit, flags 0x%x)\n", index,
                        bytes, frames, f.channels, f.bits, f.flags);
            break;
        }
    size_t at = b.in.size();
    b.in.resize(at + static_cast<size_t>(frames) * 2);
    for (uint32_t i = 0; i < frames; ++i) {
        const uint8_t* l = sample_ptr(b.abl.data(), f, i, 0);
        const uint8_t* r = f.channels > 1 ? sample_ptr(b.abl.data(), f, i, 1) : l;
        b.in[at + 2 * i] = l ? load_sample(l, f) : 0.f;
        b.in[at + 2 * i + 1] = r ? load_sample(r, f) : 0.f;
    }
}

const Fmt& dest_fmt(const Unit* m) {
    static const Fmt fallback;
    if (!m->dest.unit) return m->out_fmt;
    if (m->dest.bus >= 0) return m->dest.unit->buses[static_cast<size_t>(m->dest.bus)].fmt;
    return fallback;   // an output unit: its fields, filled in by mixer_render
}

// RenderCallback proc of a mixer: what its destination calls to get the mix
int32_t mixer_render(void* refcon, uint32_t*, const void*, uint32_t, uint32_t frames, void* abl) {
    auto* m = static_cast<Unit*>(refcon);
    Fmt out = dest_fmt(m);
    if (m->dest.unit && m->dest.bus < 0) {
        const Unit* o = m->dest.unit;
        out.rate = o->rate; out.channels = o->channels; out.bits = o->bits; out.flags = o->flags;
    }
    m->mix.assign(static_cast<size_t>(frames) * 2, 0.f);
    for (uint32_t i = 0; i < m->bus_count && i < m->buses.size(); ++i) {
        Bus& b = m->buses[i];
        if (!b.cb.proc || !b.enabled) continue;
        double step = b.fmt.rate / out.rate * b.playback;
        step = std::max(1.0 / 64, std::min(64.0, step));
        size_t need = static_cast<size_t>(b.pos + step * (frames - 1)) + 2;
        while (b.in.size() / 2 < need) pull_bus(b, i, static_cast<uint32_t>(std::min<size_t>(4096, need - b.in.size() / 2)));
        const float gl = b.volume * std::min(1.f, 1.f - b.pan), gr = b.volume * std::min(1.f, 1.f + b.pan);
        for (uint32_t f = 0; f < frames; ++f) {
            double p = b.pos + step * f;
            size_t k = static_cast<size_t>(p);
            float t = static_cast<float>(p - static_cast<double>(k));
            const float* s = b.in.data() + 2 * k;
            m->mix[2 * f] += (s[0] + (s[2] - s[0]) * t) * gl;
            m->mix[2 * f + 1] += (s[1] + (s[3] - s[1]) * t) * gr;
        }
        b.pos += step * frames;
        size_t used = std::min(static_cast<size_t>(b.pos), b.in.size() / 2);
        b.in.erase(b.in.begin(), b.in.begin() + static_cast<std::ptrdiff_t>(used * 2));
        b.pos -= static_cast<double>(used);
    }
    for (uint32_t f = 0; f < frames; ++f)
        for (uint32_t c = 0; c < out.channels; ++c)
            if (uint8_t* p = sample_ptr(abl, out, f, c)) store_sample(p, out, m->mix[2 * f + std::min(c, 1u)] * m->out_volume);
    return 0;
}

void make_mixer(Unit* u, uint32_t subtype) {
    u->mixer = true;
    u->mixer3d = subtype == 0x3364656d;   // '3dem'
    u->buses.resize(256);
    u->bus_count = (subtype == 0x6d636d78 || u->mixer3d) ? 8 : 1;   // 'mcmx' / '3dem' start with 8 inputs
}

void set_parameter(Unit* u, uint32_t id, uint32_t scope, uint32_t element, float v) {
    if (!u->mixer) return;
    if (scope == 2) { if (id == 0) u->out_volume = v; return; }   // output scope: master volume
    if (scope != 1 || element >= u->buses.size()) return;
    Bus& b = u->buses[element];
    if (u->mixer3d) {
        switch (id) {
            case 0: b.pan = std::max(-1.f, std::min(1.f, std::sin(v * 3.14159265f / 180.f))); break;   // azimuth, degrees
            case 3: b.volume = std::pow(10.f, v / 20.f); break;                                         // gain, dB
            case 4: b.playback = v > 0 ? v : 1.f; break;                                                // playback rate
            case 5: b.enabled = v != 0; break;
            default: break;
        }
    } else {
        switch (id) {
            case 0: b.volume = v; break;
            case 1: b.enabled = v != 0; break;
            case 2: b.pan = std::max(-1.f, std::min(1.f, v)); break;
            default: break;
        }
    }
}

Fmt read_asbd(const void* data) {
    Fmt f;
    std::memcpy(&f.rate, data, 8);
    std::memcpy(&f.flags, static_cast<const uint8_t*>(data) + 12, 4);
    std::memcpy(&f.channels, static_cast<const uint8_t*>(data) + 28, 4);
    std::memcpy(&f.bits, static_cast<const uint8_t*>(data) + 32, 4);
    if (f.rate <= 0) f.rate = 44100.0;
    if (!f.channels) f.channels = 2;
    if (!f.bits) f.bits = 32;
    return f;
}

void write_asbd(void* data, const Fmt& f) {
    std::memset(data, 0, 40);
    uint32_t frame = f.planar() ? f.bps() : f.bps() * f.channels, fid = 0x6c70636d /* 'lpcm' */, fpp = 1;
    auto* d = static_cast<uint8_t*>(data);
    std::memcpy(d, &f.rate, 8);
    std::memcpy(d + 8, &fid, 4);
    std::memcpy(d + 12, &f.flags, 4);
    std::memcpy(d + 16, &frame, 4);
    std::memcpy(d + 20, &fpp, 4);
    std::memcpy(d + 24, &frame, 4);
    std::memcpy(d + 28, &f.channels, 4);
    std::memcpy(d + 32, &f.bits, 4);
}

// AudioComponents: one for output units, one for everything that is mixed in software
struct Component { uint32_t subtype; };
Component g_component{0}, g_mix_mcmx{0x6d636d78}, g_mix_3dem{0x3364656d}, g_mix_other{1};

bool is_output_type(uint32_t type) { return type == 0x61756f75; }   // 'auou'

// AUGraph: nodes own their units; a connection makes the destination call the source mixer's render
struct Graph {
    struct Node { uint32_t type, subtype; Unit* unit; };
    std::vector<Node> nodes;   // AUNode n is nodes[n - 1]
    bool running = false;
    Node* node(int32_t n) { return n >= 1 && static_cast<size_t>(n) <= nodes.size() ? &nodes[static_cast<size_t>(n) - 1] : nullptr; }
};

}  // namespace

extern "C" {

// desc: AudioComponentDescription {type, subtype, manufacturer, flags, mask}
void* native_au_find_next(void* prev, const void* desc) {
    if (prev) return nullptr;
    uint32_t td[2] = {0x61756f75, 0};
    if (desc) std::memcpy(td, desc, 8);
    if (is_output_type(td[0])) return &g_component;
    return td[1] == g_mix_mcmx.subtype ? &g_mix_mcmx : td[1] == g_mix_3dem.subtype ? &g_mix_3dem : &g_mix_other;
}
int32_t native_au_instance_new(void* component, void** out) {
    auto* u = new Unit();
    auto* c = static_cast<Component*>(component);
    if (c && c != &g_component) make_mixer(u, c->subtype);
    { std::lock_guard<std::mutex> l(g_units_mu); g_units.insert(u); }
    if (out) *out = u;
    return 0;
}

// Background/foreground: pause every AAudio output stream (the guest's render callback stops being called).
void host_audio_units_pause(int paused) {
    g_paused = paused != 0;
    std::lock_guard<std::mutex> l(g_units_mu);
    for (Unit* u : g_units) {
        if (!u->stream || !u->running) continue;
        if (paused) AAudioStream_requestPause(u->stream);
        else AAudioStream_requestStart(u->stream);
    }
}

int32_t native_au_stop(void* p) {
    auto* u = static_cast<Unit*>(p);
    if (!u || !u->running.exchange(false)) return 0;
    // Unpublish under the lock so host_audio_units_pause cannot touch a closed stream; close outside it, since
    // closing waits for the data callback, which runs guest code that may start or stop units itself.
    AAudioStream* s = nullptr;
    { std::lock_guard<std::mutex> l(g_units_mu); std::swap(s, u->stream); }
    if (s) {
        AAudioStream_requestStop(s);
        AAudioStream_close(s);
    }
    if (u->pump.joinable()) u->pump.join();
    return 0;
}

int32_t native_au_instance_dispose(void* p) {
    auto* u = static_cast<Unit*>(p);
    if (!u) return 0;
    native_au_stop(u);
    { std::lock_guard<std::mutex> l(g_units_mu); g_units.erase(u); }
    if (u->wav) { wav_header(u->wav, u, u->wav_bytes); fclose(u->wav); }
    delete u;
    return 0;
}

int32_t native_au_set_property(void* p, uint32_t prop, uint32_t scope, uint32_t element, const void* data, uint32_t size) {
    auto* u = static_cast<Unit*>(p);
    if (!u || !data) return 0;
    if (u->mixer) {   // scope 1 = input (per bus), 2 = output
        if (prop == 11 && scope == 1 && size >= 4) {   // kAudioUnitProperty_ElementCount
            uint32_t n;
            std::memcpy(&n, data, 4);
            u->bus_count = std::min<uint32_t>(n, static_cast<uint32_t>(u->buses.size()));
        } else if (prop == 23 && size >= sizeof(RenderCallback) && element < u->buses.size()) {
            std::memcpy(&u->buses[element].cb, data, sizeof(RenderCallback));
        } else if (prop == 8 && size >= 40) {
            if (scope == 2) u->out_fmt = read_asbd(data);
            else if (element < u->buses.size()) u->buses[element].fmt = read_asbd(data);
        }
        return 0;
    }
    if (prop == 23 && size >= sizeof(RenderCallback)) {   // kAudioUnitProperty_SetRenderCallback
        std::memcpy(&u->cb, data, sizeof(RenderCallback));
    } else if (prop == 8 && size >= 40) {                 // kAudioUnitProperty_StreamFormat
        // Element 1 is the microphone side of a RemoteIO unit, not what gets played. And a stream that is already
        // open keeps its format: render() fills AAudio's buffer by these fields.
        if (element == 1 || u->running) {
            fprintf(stderr, "[AUDIO] stream format for %s ignored\n", element == 1 ? "the input element" : "a running unit");
            return 0;
        }
        double rate;
        uint32_t fl, ch, bits;
        std::memcpy(&rate, data, 8);
        std::memcpy(&fl, static_cast<const uint8_t*>(data) + 12, 4);
        std::memcpy(&ch, static_cast<const uint8_t*>(data) + 28, 4);
        std::memcpy(&bits, static_cast<const uint8_t*>(data) + 32, 4);
        if (rate > 0) u->rate = rate;
        if (ch) u->channels = ch;
        if (bits) u->bits = bits;
        u->flags = fl;
        u->is_float = (fl & 1) != 0;          // kAudioFormatFlagIsFloat
        u->non_interleaved = (fl & 0x20) != 0; // kAudioFormatFlagIsNonInterleaved
        fprintf(stderr, "[AUDIO] stream format: %.0f Hz, %u ch, %u bit %s%s\n", u->rate, u->channels, u->bits, u->is_float ? "float" : "int",
                u->non_interleaved ? " non-interleaved" : "");
    }
    return 0;
}

int32_t native_au_get_property(void* p, uint32_t prop, uint32_t scope, uint32_t element, void* data, uint32_t* size) {
    auto* u = static_cast<Unit*>(p);
    if (!data || !size) return 0;
    if (u && u->mixer) {
        if (prop == 8 && *size >= 40) {
            write_asbd(data, scope == 2 || element >= u->buses.size() ? u->out_fmt : u->buses[element].fmt);
            *size = 40;
            return 0;
        }
        if (prop == 11 && *size >= 4) {
            uint32_t n = scope == 1 ? u->bus_count : 1;
            std::memcpy(data, &n, 4);
            *size = 4;
            return 0;
        }
    }
    if (prop == 8 && *size >= 40 && u) {   // current stream format
        std::memset(data, 0, 40);
        uint32_t bps = bytes_per_sample(u), frame = u->non_interleaved ? bps : bps * u->channels;
        uint32_t fid = 0x6c70636d /* 'lpcm' */, fpp = 1;
        auto* d = static_cast<uint8_t*>(data);
        std::memcpy(d, &u->rate, 8);
        std::memcpy(d + 8, &fid, 4);
        std::memcpy(d + 12, &u->flags, 4);
        std::memcpy(d + 16, &frame, 4);
        std::memcpy(d + 20, &fpp, 4);
        std::memcpy(d + 24, &frame, 4);
        std::memcpy(d + 28, &u->channels, 4);
        std::memcpy(d + 32, &u->bits, 4);
        *size = 40;
    } else if (prop == 14 && *size >= 4) {   // kAudioUnitProperty_MaximumFramesPerSlice
        uint32_t v = 4096;
        std::memcpy(data, &v, 4);
        *size = 4;
    } else {
        std::memset(data, 0, *size);
    }
    return 0;
}

int32_t native_au_zero(void*) { return 0; }

int32_t native_au_start(void* p) {
    auto* u = static_cast<Unit*>(p);
    if (!u || u->mixer || u->running.exchange(true)) return 0;
    if (const char* path = getenv("LOADER_AUDIO_WAV")) {
        if ((u->wav = fopen(path, "wb"))) wav_header(u->wav, u, 0);
    }
    {
        // Under the lock so host_audio_units_pause never sees a half-opened stream. A start while the app is in the
        // background opens the stream and leaves starting it to the resume.
        std::lock_guard<std::mutex> l(g_units_mu);
        if (open_aaudio(u) && (g_paused || AAudioStream_requestStart(u->stream) == AAUDIO_OK)) return 0;
        if (u->stream) { AAudioStream_close(u->stream); u->stream = nullptr; }
    }
    fprintf(stderr, "[AUDIO] no output device: rendering into the void\n");
    u->pump = std::thread(run_pump, u);
    return 0;
}

// AudioUnitSetParameter / GetParameter: mixer volume, enable, pan (3D mixer: azimuth, gain in dB, playback rate)
int32_t native_au_set_parameter(void* p, uint32_t id, uint32_t scope, uint32_t element, float value, uint32_t) {
    if (p) set_parameter(static_cast<Unit*>(p), id, scope, element, value);
    return 0;
}
int32_t native_au_get_parameter(void* p, uint32_t id, uint32_t scope, uint32_t element, float* value) {
    auto* u = static_cast<Unit*>(p);
    if (!value) return 0;
    *value = 0.f;
    if (!u || !u->mixer) return 0;
    if (scope == 2) { if (id == 0) *value = u->out_volume; return 0; }
    if (element >= u->buses.size()) return 0;
    const Bus& b = u->buses[element];
    if (u->mixer3d) *value = id == 3 ? 20.f * std::log10(std::max(b.volume, 1e-6f)) : id == 4 ? b.playback : id == 5 ? (b.enabled ? 1.f : 0.f) : 0.f;
    else *value = id == 0 ? b.volume : id == 1 ? (b.enabled ? 1.f : 0.f) : id == 2 ? b.pan : 0.f;
    return 0;
}
// AudioUnitRender: pulling a mixer by hand
int32_t native_au_render(void* p, uint32_t* flags, const void* ts, uint32_t bus, uint32_t frames, void* abl) {
    auto* u = static_cast<Unit*>(p);
    if (u && u->mixer && abl) return mixer_render(u, flags, ts, bus, frames, abl);
    return 0;
}

int32_t native_augraph_new(void** out) {
    if (out) *out = new Graph();
    return 0;
}
int32_t native_augraph_add_node(void* g, const uint32_t* desc, int32_t* out) {
    auto* graph = static_cast<Graph*>(g);
    if (!graph || !desc) return -50;   // paramErr
    graph->nodes.push_back({desc[0], desc[1], nullptr});
    if (out) *out = static_cast<int32_t>(graph->nodes.size());
    fprintf(stderr, "[AUDIO] AUGraph node %zu: %.4s/%.4s\n", graph->nodes.size(), reinterpret_cast<const char*>(&desc[0]),
            reinterpret_cast<const char*>(&desc[1]));   // four-char codes read backwards (little-endian)
    return 0;
}
// AUGraphOpen instantiates every node's unit; done lazily too, for graphs that ask for a unit first
static Unit* graph_unit(Graph::Node* n) {
    if (n && !n->unit) {
        uint32_t desc[2] = {n->type, n->subtype};
        void* u = nullptr;
        native_au_instance_new(native_au_find_next(nullptr, desc), &u);
        n->unit = static_cast<Unit*>(u);
    }
    return n ? n->unit : nullptr;
}
int32_t native_augraph_open(void* g) {
    if (auto* graph = static_cast<Graph*>(g))
        for (auto& n : graph->nodes) graph_unit(&n);
    return 0;
}
int32_t native_augraph_node_info(void* g, int32_t node, uint32_t* desc, void** unit) {
    auto* graph = static_cast<Graph*>(g);
    Graph::Node* n = graph ? graph->node(node) : nullptr;
    if (!n) return -10866;   // kAUGraphErr_NodeNotFound
    if (desc) { desc[0] = n->type; desc[1] = n->subtype; desc[2] = 0x6170706c; /* 'appl' */ desc[3] = desc[4] = 0; }
    if (unit) *unit = graph_unit(n);
    return 0;
}
int32_t native_augraph_set_node_input_callback(void* g, int32_t node, uint32_t bus, const void* cb) {
    auto* graph = static_cast<Graph*>(g);
    Unit* u = graph ? graph_unit(graph->node(node)) : nullptr;
    if (!u) return -10866;
    native_au_set_property(u, 23, 1, bus, cb, sizeof(RenderCallback));
    return 0;
}
// src's output feeds dst's input `in`: dst pulls src's mix
int32_t native_augraph_connect(void* g, int32_t src_node, uint32_t, int32_t dst_node, uint32_t in) {
    auto* graph = static_cast<Graph*>(g);
    Unit* src = graph ? graph_unit(graph->node(src_node)) : nullptr;
    Unit* dst = graph ? graph_unit(graph->node(dst_node)) : nullptr;
    if (!src || !dst) return -10866;
    if (!src->mixer) return 0;   // only mixers (and pass-through units) are sources
    RenderCallback cb{mixer_render, src};
    if (dst->mixer) {
        if (in >= dst->buses.size()) return -50;
        dst->buses[in].cb = cb;
        dst->buses[in].fmt = Fmt{src->out_fmt.rate, 2, 32, 0x9};
        if (in >= dst->bus_count) dst->bus_count = in + 1;
        src->dest = {dst, static_cast<int>(in)};
    } else {
        dst->cb = cb;
        src->dest = {dst, -1};
    }
    return 0;
}
int32_t native_augraph_disconnect(void* g, int32_t dst_node, uint32_t in) {
    auto* graph = static_cast<Graph*>(g);
    Unit* dst = graph ? graph_unit(graph->node(dst_node)) : nullptr;
    if (!dst) return -10866;
    if (dst->mixer) { if (in < dst->buses.size()) dst->buses[in].cb = {nullptr, nullptr}; }
    else dst->cb = {nullptr, nullptr};
    return 0;
}
int32_t native_augraph_start(void* g) {
    auto* graph = static_cast<Graph*>(g);
    if (!graph) return -50;
    for (auto& n : graph->nodes)
        if (is_output_type(n.type)) native_au_start(graph_unit(&n));
    graph->running = true;
    return 0;
}
int32_t native_augraph_stop(void* g) {
    auto* graph = static_cast<Graph*>(g);
    if (!graph) return -50;
    for (auto& n : graph->nodes)
        if (n.unit && is_output_type(n.type)) native_au_stop(n.unit);
    graph->running = false;
    return 0;
}
int32_t native_augraph_is_running(void* g, uint8_t* out) {
    if (out) *out = g && static_cast<Graph*>(g)->running;
    return 0;
}
int32_t native_augraph_node_count(void* g, uint32_t* out) {
    if (out) *out = g ? static_cast<uint32_t>(static_cast<Graph*>(g)->nodes.size()) : 0;
    return 0;
}
int32_t native_augraph_ind_node(void* g, uint32_t index, int32_t* out) {
    if (out) *out = static_cast<int32_t>(index) + 1;
    return g && index < static_cast<Graph*>(g)->nodes.size() ? 0 : -50;
}
int32_t native_augraph_dispose(void* g) {
    auto* graph = static_cast<Graph*>(g);
    if (!graph) return 0;
    native_augraph_stop(graph);
    for (auto& n : graph->nodes)
        if (n.unit) native_au_instance_dispose(n.unit);
    delete graph;
    return 0;
}
int32_t native_augraph_zero(void*) { return 0; }   // Initialize, Uninitialize, Close, Update
int32_t native_augraph_cpu_load(void*, float* out) {
    if (out) *out = 0.f;
    return 0;
}

}  // extern "C"
