// AudioUnit RemoteIO emulation on AAudio.
//
// The guest (FMOD inside Unity) configures one output unit with an AudioStreamBasicDescription and a render
// callback, then starts it. Here the unit opens an AAudio stream in the guest's own format (AAudio resamples), and
// AAudio's real-time data callback calls the guest's render callback with an AudioBufferList pointing straight at
// the output buffer (interleaved) or at scratch planes that are then interleaved (non-interleaved formats).
// Without an AAudio device, or with LOADER_AUDIO=0, a timed thread drives the callback and discards the samples.
// LOADER_AUDIO_WAV=<path> additionally records what the guest rendered.
#include <aaudio/AAudio.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>
#include <vector>

namespace {

struct RenderCallback { int32_t (*proc)(void*, uint32_t*, const void*, uint32_t, uint32_t, void*); void* refcon; };

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
};

uint32_t bytes_per_sample(const Unit* u) { return u->bits / 8 ? u->bits / 8 : 4; }

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
    if (!u->running) {
        std::memset(data, 0, static_cast<size_t>(frames) * u->channels * bytes_per_sample(u));
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }
    render(u, data, static_cast<uint32_t>(frames));
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void on_error(AAudioStream*, void*, aaudio_result_t err) { fprintf(stderr, "[AUDIO] AAudio stream error %d\n", err); }

bool open_aaudio(Unit* u) {
    const char* e = getenv("LOADER_AUDIO");
    if (e && e[0] == '0') return false;
    if (!u->is_float && u->bits != 16) return false;   // AAudio takes I16 or float
    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return false;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(b, static_cast<int32_t>(u->rate));
    AAudioStreamBuilder_setChannelCount(b, static_cast<int32_t>(u->channels));
    AAudioStreamBuilder_setFormat(b, u->is_float ? AAUDIO_FORMAT_PCM_FLOAT : AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_SONIFICATION);
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
        render(u, out.data(), frames);
        next.tv_nsec += static_cast<long>(1e9 * frames / u->rate);
        while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr);
    }
}

char g_component;

}  // namespace

extern "C" {

void* native_au_find_next(void* prev, const void*) { return prev ? nullptr : &g_component; }
int32_t native_au_instance_new(void*, void** out) { if (out) *out = new Unit(); return 0; }

int32_t native_au_stop(void* p) {
    auto* u = static_cast<Unit*>(p);
    if (!u || !u->running.exchange(false)) return 0;
    if (u->stream) {
        AAudioStream_requestStop(u->stream);
        AAudioStream_close(u->stream);
        u->stream = nullptr;
    }
    if (u->pump.joinable()) u->pump.join();
    return 0;
}

int32_t native_au_instance_dispose(void* p) {
    auto* u = static_cast<Unit*>(p);
    if (!u) return 0;
    native_au_stop(u);
    if (u->wav) { wav_header(u->wav, u, u->wav_bytes); fclose(u->wav); }
    delete u;
    return 0;
}

int32_t native_au_set_property(void* p, uint32_t prop, uint32_t, uint32_t, const void* data, uint32_t size) {
    auto* u = static_cast<Unit*>(p);
    if (!u || !data) return 0;
    if (prop == 23 && size >= sizeof(RenderCallback)) {   // kAudioUnitProperty_SetRenderCallback
        std::memcpy(&u->cb, data, sizeof(RenderCallback));
    } else if (prop == 8 && size >= 40) {                 // kAudioUnitProperty_StreamFormat
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

int32_t native_au_get_property(void* p, uint32_t prop, uint32_t, uint32_t, void* data, uint32_t* size) {
    auto* u = static_cast<Unit*>(p);
    if (!data || !size) return 0;
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
    if (!u || u->running.exchange(true)) return 0;
    if (const char* path = getenv("LOADER_AUDIO_WAV")) {
        if ((u->wav = fopen(path, "wb"))) wav_header(u->wav, u, 0);
    }
    if (open_aaudio(u) && AAudioStream_requestStart(u->stream) == AAUDIO_OK) return 0;
    if (u->stream) { AAudioStream_close(u->stream); u->stream = nullptr; }
    fprintf(stderr, "[AUDIO] no output device: rendering into the void\n");
    u->pump = std::thread(run_pump, u);
    return 0;
}

}  // extern "C"
