// AVFoundation playback: AVPlayer (with AVPlayerItem, AVURLAsset and AVPlayerLayer) and AVAudioPlayer play through the
// host's media player (Android MediaPlayer, HostActivity.media* via android_app.cpp). A video plays in a view laid
// over the game, covering the guest screen, while an AVPlayerLayer shows its player. The end of playback posts
// AVPlayerItemDidPlayToEndTimeNotification or calls the AVAudioPlayer delegate, as on iOS: games wait on exactly that
// (intro movies, music sequencing). Without a host player (command-line loader) playback just runs out its duration.
#include "app.hpp"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
int host_media_open(int id, const char* path, int video) __attribute__((weak));
void host_media_play(int id, int left, int top, int width, int height) __attribute__((weak));
void host_media_pause(int id) __attribute__((weak));
void host_media_close(int id) __attribute__((weak));
void host_media_seek(int id, int ms) __attribute__((weak));
void host_media_volume(int id, float volume) __attribute__((weak));
void host_media_loop(int id, int loop) __attribute__((weak));
int host_media_position(int id) __attribute__((weak));
int host_media_duration(const char* path) __attribute__((weak));
void host_media_set_callback(void (*cb)(int id, int event)) __attribute__((weak));
}

namespace app {

void screen_content_rect(int& x, int& y, int& w, int& h, int& sw, int& sh);   // app_uikit.cpp
void post_note(const char* name, id object, id info);                         // app_foundation2.cpp
id block_copy(id b);
void block_release(id b);
void* block_invoke_ptr(id b);

namespace {

struct CMTime { int64_t value; int32_t timescale; uint32_t flags; int64_t epoch; };
CMTime cm_ms(int64_t ms) { return {ms, 1000, 1, 0}; }
int64_t to_ms(const CMTime& t) { return t.timescale > 0 ? t.value * 1000 / t.timescale : 0; }

// LOADER_MEDIA=0: no host player (playback only runs out its duration)
bool host_player() {
    static const bool off = getenv("LOADER_MEDIA") && getenv("LOADER_MEDIA")[0] == '0';
    return !off && host_media_open && host_media_play;
}

std::mutex g_mu;
std::unordered_map<int, id> g_owner;   // session id -> AVPlayerItem / AVAudioPlayer (not retained)
int g_next_id = 0;

void on_media_event(int id, int event);

// A file opened in the host player. The owner (an AVPlayerItem or AVAudioPlayer) hears about its end through
// media_finished.
struct Session {
    int sid = 0;
    std::string path;
    bool video = false, playing = false, ended = false, loop = false;
    float volume = 1;
    int64_t duration = -1;   // ms
    // the clock without a host player
    double clock_at = 0;
    int64_t clock_pos = 0;
    uint64_t end_timer = 0;

    int64_t length() {
        if (duration < 0) duration = host_media_duration ? host_media_duration(path.c_str()) : 0;
        return duration;
    }
    bool open(id owner, bool with_video) {
        if (sid) return true;
        if (path.empty()) return false;
        {
            std::lock_guard<std::mutex> l(g_mu);
            sid = ++g_next_id;
            g_owner[sid] = owner;
        }
        video = with_video;
        log("media %d: %s%s%s", sid, path.c_str(), video ? " (video)" : "", host_player() ? "" : " (no host player: timed only)");
        if (host_player()) {
            if (host_media_set_callback) host_media_set_callback(&on_media_event);
            host_media_open(sid, path.c_str(), video ? 1 : 0);
            if (host_media_volume) host_media_volume(sid, volume);
            if (host_media_loop) host_media_loop(sid, loop ? 1 : 0);
        }
        return true;
    }
    int64_t position() {
        if (!sid) return 0;
        if (ended && !loop) return length();
        if (host_player()) return host_media_position ? host_media_position(sid) : 0;
        return playing ? clock_pos + static_cast<int64_t>((now_s() - clock_at) * 1000) : clock_pos;
    }
    void play(id owner, bool with_video) {
        if (sid && with_video != video) {   // shown in a layer now (or no longer): reopen where it was
            int64_t at = position();
            close();
            open(owner, with_video);
            seek(at);
        }
        open(owner, with_video);
        if (ended) { ended = false; seek(0); }
        playing = true;
        if (host_player()) {
            int x = 0, y = 0, w = 0, h = 0, sw, sh;
            if (video) screen_content_rect(x, y, w, h, sw, sh);
            host_media_play(sid, x, y, w, h);
            return;
        }
        clock_at = now_s();
        int64_t left = std::max<int64_t>(0, length() - clock_pos);
        int session = sid;
        if (end_timer) timer_remove(end_timer);
        end_timer = timer_add(left / 1000.0, 0, [session] { on_media_event(session, 1); });
    }
    void pause() {
        if (!sid || !playing) return;
        if (host_player()) { if (host_media_pause) host_media_pause(sid); }
        else {
            clock_pos = position();
            if (end_timer) { timer_remove(end_timer); end_timer = 0; }
        }
        playing = false;
    }
    void seek(int64_t ms) {
        ms = std::max<int64_t>(0, ms);
        if (!sid) return;
        ended = false;
        if (host_player()) { if (host_media_seek) host_media_seek(sid, static_cast<int>(ms)); return; }
        bool was = playing;
        pause();
        clock_pos = ms;
        if (was) playing = false, play(nullptr, video);
    }
    void set_volume(float v) {
        volume = v;
        if (sid && host_media_volume) host_media_volume(sid, v);
    }
    void set_loop(bool l) {
        loop = l;
        if (sid && host_media_loop) host_media_loop(sid, l ? 1 : 0);
    }
    void close() {
        if (!sid) return;
        if (end_timer) { timer_remove(end_timer); end_timer = 0; }
        if (host_media_close) host_media_close(sid);
        std::lock_guard<std::mutex> l(g_mu);
        g_owner.erase(sid);
        sid = 0;
        playing = false;
    }
};

bool is_video_path(const std::string& p) {
    size_t dot = p.rfind('.');
    std::string ext = dot == std::string::npos ? "" : p.substr(dot + 1);
    for (auto& c : ext) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return ext == "m4v" || ext == "mp4" || ext == "mov" || ext == "3gp" || ext == "mkv" || ext == "webm";
}
std::string url_path(id url) {
    if (!url) return "";
    std::string p = is_kind(url, "NSString") ? utf8(url) : utf8(send(url, "path"));
    return p.empty() ? p : remap_path(p);
}

// ---- KVO on the AV objects: "status" turns ReadyToPlay right after an observer is added, "rate" drops to 0 at the end
struct Observer { id who; std::string key; NSUInteger options; void* ctx; };
struct Observed {
    std::vector<Observer> observers;
    void add(id self, id who, id key, NSUInteger options, void* ctx);
    void remove(id who, id key);
    void notify(id self, const std::string& key, id value);
};
id kvo_value(id self, const std::string& key) {
    if (key == "status") return num_i(1);   // ReadyToPlay
    if (key == "rate") return num_f(send<float>(self, "rate"));
    return nullptr;
}
void Observed::add(id self, id who, id key, NSUInteger options, void* ctx) {
    if (!who || !key) return;
    std::string k = utf8(key);
    if (trace_enabled("av")) log("AV observe %s of %p (options 0x%lx)", k.c_str(), static_cast<void*>(self), static_cast<unsigned long>(options));
    observers.push_back({who, k, options, ctx});
    if (k != "status" && !(options & 4)) return;
    id v = kvo_value(self, k);
    if (!v) return;
    retain(self);
    retain(v);
    post_main([this, self, who, k, v] {   // after the caller finishes setting up, as the real status change would
        bool still = false;
        for (auto& o : observers) still = still || (o.who == who && o.key == k);
        if (still) notify(self, k, v);
        release(v);
        release(self);
    });
}
void Observed::remove(id who, id key) {
    std::string k = key ? utf8(key) : "";
    observers.erase(std::remove_if(observers.begin(), observers.end(),
                                   [&](const Observer& o) { return o.who == who && (k.empty() || o.key == k); }),
                    observers.end());
}
void Observed::notify(id self, const std::string& key, id value) {
    std::vector<Observer> hits;
    for (auto& o : observers) if (o.key == key) hits.push_back(o);
    for (auto& o : hits) {
        id change = dict_new(true);
        dict_set(change, "kind", num_i(1));   // NSKeyValueChangeSetting
        if (value) dict_set(change, "new", value);
        send<void>(o.who, "observeValueForKeyPath:ofObject:change:context:", str(key), self, change, o.ctx);
        release(change);
    }
}

Class C_AVAsset, C_AVURLAsset, C_AVPlayerItem, C_AVPlayer, C_AVPlayerLayer, C_AVAudioPlayer;

struct AssetExt : Ext {
    id url = nullptr;
    std::string path;
    int64_t duration = -1;
    ~AssetExt() override { release(url); }
    int64_t length() {
        if (duration < 0) duration = host_media_duration ? host_media_duration(path.c_str()) : 0;
        return duration;
    }
};

struct ItemExt : Ext {
    Session s;
    id asset = nullptr;
    id player = nullptr;   // the AVPlayer it is current in (not retained)
    Observed kvo;
    ~ItemExt() override { s.close(); release(asset); }
};

struct PlayerExt : Ext {
    id item = nullptr;
    id layer = nullptr;    // an AVPlayerLayer showing this player (not retained)
    float rate = 0, volume = 1;
    bool muted = false;
    long action_at_end = 1;   // AVPlayerActionAtItemEndPause
    std::vector<std::pair<uint64_t, id>> time_observers;   // timer, block
    Observed kvo;
    ~PlayerExt() override {
        for (auto& t : time_observers) { timer_remove(t.first); block_release(t.second); }
        if (item) {
            auto* ie = ext_get<ItemExt>(item);
            ie->s.pause();
            if (ie->s.video) ie->s.close();   // its view goes with the player
            if (ie->player == self_obj) ie->player = nullptr;
            release(item);
        }
    }
    id self_obj = nullptr;
};

// AVPlayerLayer is a CALayer, whose Ext slot UIKit uses: its state lives in a side table, dropped by a dealloc hook
struct LayerState {
    id player = nullptr;
    id gravity = nullptr;
};
std::mutex g_layer_mu;
std::unordered_map<id, LayerState> g_layer_states;
LayerState* layer_state(id layer) {
    std::lock_guard<std::mutex> l(g_layer_mu);
    return &g_layer_states[layer];   // node-based: the pointer stays valid until the entry is erased
}

ItemExt* item_of(id item) { return item ? ext_get<ItemExt>(item) : nullptr; }
PlayerExt* player_of(id p) {
    auto* e = ext_get<PlayerExt>(p);
    e->self_obj = p;
    return e;
}

id make_item(id asset) {
    id item = alloc(C_AVPlayerItem);
    auto* e = ext_get<ItemExt>(item);
    e->asset = retain(asset);
    e->s.path = ext_get<AssetExt>(asset)->path;
    return item;
}
id make_asset(id url) {
    id a = alloc(C_AVURLAsset);
    auto* e = ext_get<AssetExt>(a);
    e->url = retain(url);
    e->path = url_path(url);
    if (trace_enabled("av") || e->path.empty()) log("AVURLAsset %s", e->path.empty() ? "(no path)" : e->path.c_str());
    return a;
}

// what the player's current item plays into: the screen while a layer shows the player and the file has video
bool shows_video(PlayerExt* p, ItemExt* ie) { return p->layer && is_video_path(ie->s.path); }

void player_apply(id player) {
    auto* p = player_of(player);
    ItemExt* ie = item_of(p->item);
    if (!ie) return;
    ie->player = player;
    ie->s.set_volume(p->muted ? 0.f : p->volume);
    if (p->rate > 0) ie->s.play(p->item, shows_video(p, ie));
    else ie->s.pause();
}

void set_rate(id player, float r) {
    auto* p = player_of(player);
    if (trace_enabled("av")) log("AVPlayer %p rate %.1f (item %p, layer %p)", static_cast<void*>(player), r, static_cast<void*>(p->item), static_cast<void*>(p->layer));
    if (p->rate == r) { player_apply(player); return; }
    p->rate = r;
    player_apply(player);
    p->kvo.notify(player, "rate", num_f(r));
}

// A seek on an item whose player still has a rate restarts it: with AVPlayerActionAtItemEndNone the rate stays 1 at the
// end, and seeking back to zero is how games loop a movie.
void item_seek(id item, int64_t ms) {
    ItemExt* ie = item_of(item);
    if (!ie) return;
    ie->s.seek(ms);
    if (ie->player && player_of(ie->player)->item == item && player_of(ie->player)->rate > 0 && !ie->s.playing) player_apply(ie->player);
}

// the layer stopped showing its player: the movie keeps playing, without a picture
void layer_detach(id layer) {
    auto* le = layer_state(layer);
    if (!le->player) return;
    auto* p = player_of(le->player);
    if (p->layer == layer) {
        p->layer = nullptr;
        ItemExt* ie = item_of(p->item);
        if (ie && ie->s.video) {
            bool was = ie->s.playing;
            int64_t at = ie->s.position();
            ie->s.close();
            if (was) { ie->s.play(p->item, false); ie->s.seek(at); }
        }
    }
}

struct AudioExt : Ext {
    Session s;
    id url = nullptr, data = nullptr;
    id delegate = nullptr;   // not retained
    long loops = 0, loops_left = 0;
    std::string temp;        // initWithData: writes the data here
    ~AudioExt() override {
        s.close();
        release(url);
        release(data);
        if (!temp.empty()) unlink(temp.c_str());
    }
};

void item_finished(id item, bool ok) {
    auto* ie = item_of(item);
    ie->s.playing = false;
    ie->s.ended = true;
    if (id player = ie->player) {
        auto* p = player_of(player);
        if (p->item == item && p->action_at_end != 2) {   // AVPlayerActionAtItemEndNone keeps the rate
            p->rate = 0;
            p->kvo.notify(player, "rate", num_f(0));
        }
    }
    post_note(ok ? "AVPlayerItemDidPlayToEndTimeNotification" : "AVPlayerItemFailedToPlayToEndTimeNotification", item, nullptr);
}

void audio_finished(id player, bool ok) {
    auto* e = ext_get<AudioExt>(player);
    if (ok && (e->loops < 0 || e->loops_left > 0)) {   // numberOfLoops: n more times, or forever
        if (e->loops > 0) --e->loops_left;
        e->s.ended = false;
        e->s.seek(0);
        e->s.play(player, false);
        return;
    }
    e->s.playing = false;
    e->s.ended = true;
    if (e->delegate && lookup_noforward(e->delegate, sel("audioPlayerDidFinishPlaying:successfully:")))
        send<void>(e->delegate, "audioPlayerDidFinishPlaying:successfully:", player, ok);
    if (!ok && e->delegate && lookup_noforward(e->delegate, sel("audioPlayerDecodeErrorDidOccur:error:")))
        send<void>(e->delegate, "audioPlayerDecodeErrorDidOccur:error:", player, static_cast<id>(nullptr));
}

// host player events (its UI thread) -> the main thread
void on_media_event(int sid, int event) {
    post_main([sid, event] {
        id owner;
        {
            std::lock_guard<std::mutex> l(g_mu);
            auto it = g_owner.find(sid);
            if (it == g_owner.end()) return;
            owner = retain(it->second);
        }
        if (is_kind(owner, C_AVAudioPlayer)) audio_finished(owner, event == 1);
        else item_finished(owner, event == 1);
        release(owner);
    });
}

}  // namespace

// LOADER_APP_TRACE=av: one line per AVFoundation call the game makes
void av_trace(const char* what, id self) {
    if (trace_enabled("av")) log("AV %s %p", what, static_cast<void*>(self));
}

void init_av() {
    C_AVAsset = def_class("AVAsset", "NSObject");
    C_AVURLAsset = def_class("AVURLAsset", "AVAsset");
    for (Class A : {C_AVAsset, C_AVURLAsset}) {
        defc(A, "assetWithURL:", +[](id, SEL, id url) -> id { return autorelease(make_asset(url)); });
        defc(A, "URLAssetWithURL:options:", +[](id, SEL, id url, id) -> id { return autorelease(make_asset(url)); });
    }
    def(C_AVURLAsset, "initWithURL:options:", +[](id self, SEL, id url, id) -> id {
        auto* e = ext_get<AssetExt>(self);
        e->url = retain(url);
        e->path = url_path(url);
        return self;
    });
    def(C_AVAsset, "URL", +[](id self, SEL) -> id { return ext_get<AssetExt>(self)->url; });
    def(C_AVAsset, "duration", +[](id self, SEL) -> CMTime { return cm_ms(ext_get<AssetExt>(self)->length()); });
    def(C_AVAsset, "isPlayable", +[](id, SEL) -> bool { return true; });
    def(C_AVAsset, "isReadable", +[](id, SEL) -> bool { return true; });
    def(C_AVAsset, "tracks", +[](id self, SEL) -> id { av_trace("asset tracks", self); return arr({}); });
    def(C_AVAsset, "tracksWithMediaType:", +[](id self, SEL, id) -> id { av_trace("asset tracksWithMediaType", self); return arr({}); });
    def(C_AVAsset, "statusOfValueForKey:error:", +[](id self, SEL, id, id* err) -> long {
        av_trace("asset statusOfValueForKey", self);
        if (err) *err = nullptr;
        return 2;   // Loaded
    });
    def(C_AVAsset, "loadValuesAsynchronouslyForKeys:completionHandler:", +[](id self, SEL, id, id handler) {
        av_trace("asset loadValuesAsynchronously", self);
        if (!handler) return;
        id h = block_copy(handler);
        post_main([h] {
            reinterpret_cast<void (*)(id)>(block_invoke_ptr(h))(h);
            block_release(h);
        });
    });

    C_AVPlayerItem = def_class("AVPlayerItem", "NSObject");
    defc(C_AVPlayerItem, "playerItemWithAsset:", +[](id, SEL, id asset) -> id {
        av_trace("playerItemWithAsset", asset);
        return asset ? autorelease(make_item(asset)) : nullptr;
    });
    defc(C_AVPlayerItem, "playerItemWithURL:", +[](id, SEL, id url) -> id {
        id a = make_asset(url);
        id item = make_item(a);
        release(a);
        return autorelease(item);
    });
    def(C_AVPlayerItem, "initWithAsset:", +[](id self, SEL, id asset) -> id {
        auto* e = ext_get<ItemExt>(self);
        e->asset = retain(asset);
        e->s.path = asset ? ext_get<AssetExt>(asset)->path : "";
        return self;
    });
    def(C_AVPlayerItem, "initWithURL:", +[](id self, SEL, id url) -> id {
        auto* e = ext_get<ItemExt>(self);
        e->asset = make_asset(url);
        e->s.path = ext_get<AssetExt>(e->asset)->path;
        return self;
    });
    def(C_AVPlayerItem, "asset", +[](id self, SEL) -> id { return ext_get<ItemExt>(self)->asset; });
    def(C_AVPlayerItem, "status", +[](id, SEL) -> long { return 1; });   // AVPlayerItemStatusReadyToPlay
    def(C_AVPlayerItem, "error", +[](id, SEL) -> id { return nullptr; });
    def(C_AVPlayerItem, "duration", +[](id self, SEL) -> CMTime { return cm_ms(ext_get<ItemExt>(self)->s.length()); });
    def(C_AVPlayerItem, "currentTime", +[](id self, SEL) -> CMTime { return cm_ms(ext_get<ItemExt>(self)->s.position()); });
    def(C_AVPlayerItem, "seekToTime:", +[](id self, SEL, CMTime t) { item_seek(self, to_ms(t)); });
    def(C_AVPlayerItem, "seekToTime:completionHandler:", +[](id self, SEL, CMTime t, id handler) {
        item_seek(self, to_ms(t));
        if (handler) reinterpret_cast<void (*)(id, bool)>(block_invoke_ptr(handler))(handler, true);
    });
    def(C_AVPlayerItem, "isPlaybackLikelyToKeepUp", +[](id, SEL) -> bool { return true; });
    def(C_AVPlayerItem, "isPlaybackBufferFull", +[](id, SEL) -> bool { return true; });
    def(C_AVPlayerItem, "isPlaybackBufferEmpty", +[](id, SEL) -> bool { return false; });
    def(C_AVPlayerItem, "tracks", +[](id, SEL) -> id { return arr({}); });
    def(C_AVPlayerItem, "addObserver:forKeyPath:options:context:", +[](id self, SEL, id who, id key, NSUInteger opt, void* ctx) {
        ext_get<ItemExt>(self)->kvo.add(self, who, key, opt, ctx);
    });
    def(C_AVPlayerItem, "removeObserver:forKeyPath:", +[](id self, SEL, id who, id key) { ext_get<ItemExt>(self)->kvo.remove(who, key); });
    def(C_AVPlayerItem, "removeObserver:forKeyPath:context:", +[](id self, SEL, id who, id key, void*) {
        ext_get<ItemExt>(self)->kvo.remove(who, key);
    });

    C_AVPlayer = def_class("AVPlayer", "NSObject");
    Class Q = def_class("AVQueuePlayer", "AVPlayer");
    defc(C_AVPlayer, "playerWithPlayerItem:", +[](id cls, SEL, id item) -> id {
        return autorelease(send(alloc(reinterpret_cast<Class>(cls)), "initWithPlayerItem:", item));
    });
    defc(C_AVPlayer, "playerWithURL:", +[](id cls, SEL, id url) -> id {
        return autorelease(send(alloc(reinterpret_cast<Class>(cls)), "initWithURL:", url));
    });
    def(C_AVPlayer, "initWithPlayerItem:", +[](id self, SEL, id item) -> id {
        av_trace("player initWithPlayerItem", item);
        auto* p = player_of(self);
        p->item = retain(item);
        if (ItemExt* ie = item_of(item)) ie->player = self;
        return self;
    });
    def(C_AVPlayer, "initWithURL:", +[](id self, SEL, id url) -> id {
        id a = make_asset(url);
        auto* p = player_of(self);
        p->item = make_item(a);
        release(a);
        item_of(p->item)->player = self;
        return self;
    });
    def(Q, "initWithItems:", +[](id self, SEL, id items) -> id {   // the first item only: queues are rare in games
        id first = items && send<NSUInteger>(items, "count") ? send(items, "objectAtIndex:", static_cast<NSUInteger>(0)) : nullptr;
        return send(self, "initWithPlayerItem:", first);
    });
    def(C_AVPlayer, "currentItem", +[](id self, SEL) -> id { return player_of(self)->item; });
    def(C_AVPlayer, "replaceCurrentItemWithPlayerItem:", +[](id self, SEL, id item) {
        auto* p = player_of(self);
        if (item == p->item) return;
        if (ItemExt* old = item_of(p->item)) {
            old->s.pause();
            if (old->s.video) old->s.close();
            if (old->player == self) old->player = nullptr;
        }
        id prev = p->item;
        p->item = retain(item);
        release(prev);
        player_apply(self);
    });
    def(C_AVPlayer, "play", +[](id self, SEL) { set_rate(self, 1.f); });
    def(C_AVPlayer, "pause", +[](id self, SEL) { set_rate(self, 0.f); });
    def(C_AVPlayer, "rate", +[](id self, SEL) -> float { return player_of(self)->rate; });
    def(C_AVPlayer, "setRate:", +[](id self, SEL, float r) { set_rate(self, r); });
    def(C_AVPlayer, "status", +[](id, SEL) -> long { return 1; });
    def(C_AVPlayer, "error", +[](id, SEL) -> id { return nullptr; });
    def(C_AVPlayer, "currentTime", +[](id self, SEL) -> CMTime {
        ItemExt* ie = item_of(player_of(self)->item);
        return cm_ms(ie ? ie->s.position() : 0);
    });
    def(C_AVPlayer, "seekToTime:", +[](id self, SEL, CMTime t) { item_seek(player_of(self)->item, to_ms(t)); });
    def(C_AVPlayer, "seekToTime:toleranceBefore:toleranceAfter:", +[](id self, SEL, CMTime t, CMTime, CMTime) {
        item_seek(player_of(self)->item, to_ms(t));
    });
    def(C_AVPlayer, "seekToTime:completionHandler:", +[](id self, SEL, CMTime t, id handler) {
        item_seek(player_of(self)->item, to_ms(t));
        if (handler) reinterpret_cast<void (*)(id, bool)>(block_invoke_ptr(handler))(handler, true);
    });
    def(C_AVPlayer, "volume", +[](id self, SEL) -> float { return player_of(self)->volume; });
    def(C_AVPlayer, "setVolume:", +[](id self, SEL, float v) { player_of(self)->volume = v; player_apply(self); });
    def(C_AVPlayer, "isMuted", +[](id self, SEL) -> bool { return player_of(self)->muted; });
    def(C_AVPlayer, "setMuted:", +[](id self, SEL, bool m) { player_of(self)->muted = m; player_apply(self); });
    def(C_AVPlayer, "actionAtItemEnd", +[](id self, SEL) -> long { return player_of(self)->action_at_end; });
    def(C_AVPlayer, "setActionAtItemEnd:", +[](id self, SEL, long a) { player_of(self)->action_at_end = a; });
    def(C_AVPlayer, "setAllowsExternalPlayback:", +[](id, SEL, bool) {});
    def(C_AVPlayer, "setUsesExternalPlaybackWhileExternalScreenIsActive:", +[](id, SEL, bool) {});
    def(C_AVPlayer, "setAutomaticallyWaitsToMinimizeStalling:", +[](id, SEL, bool) {});
    // periodic time observers: the block gets the current time every interval, on the main thread
    def(C_AVPlayer, "addPeriodicTimeObserverForInterval:queue:usingBlock:", +[](id self, SEL, CMTime interval, id, id block) -> id {
        if (!block) return nullptr;
        double every = std::max(0.01, to_ms(interval) / 1000.0);
        id b = block_copy(block);
        uint64_t t = timer_add(every, every, [self, b] {
            ItemExt* ie = item_of(player_of(self)->item);
            if (!ie || !ie->s.playing) return;
            reinterpret_cast<void (*)(id, CMTime)>(block_invoke_ptr(b))(b, cm_ms(ie->s.position()));
        });
        player_of(self)->time_observers.push_back({t, b});
        return autorelease(retain(num_u(t)));
    });
    def(C_AVPlayer, "addBoundaryTimeObserverForTimes:queue:usingBlock:", +[](id, SEL, id, id, id) -> id { return autorelease(retain(num_u(0))); });
    def(C_AVPlayer, "removeTimeObserver:", +[](id self, SEL, id token) {
        auto* p = player_of(self);
        uint64_t t = static_cast<uint64_t>(num_ival(token));
        for (auto it = p->time_observers.begin(); it != p->time_observers.end(); ++it)
            if (it->first == t) {
                timer_remove(t);
                block_release(it->second);
                p->time_observers.erase(it);
                return;
            }
    });
    def(C_AVPlayer, "addObserver:forKeyPath:options:context:", +[](id self, SEL, id who, id key, NSUInteger opt, void* ctx) {
        player_of(self)->kvo.add(self, who, key, opt, ctx);
    });
    def(C_AVPlayer, "removeObserver:forKeyPath:", +[](id self, SEL, id who, id key) { player_of(self)->kvo.remove(who, key); });
    def(C_AVPlayer, "removeObserver:forKeyPath:context:", +[](id self, SEL, id who, id key, void*) { player_of(self)->kvo.remove(who, key); });

    // AVPlayerLayer: while it shows a player, that player's video is on screen
    C_AVPlayerLayer = def_class("AVPlayerLayer", "CALayer");
    defc(C_AVPlayerLayer, "playerLayerWithPlayer:", +[](id cls, SEL, id player) -> id {
        id l = send(alloc(reinterpret_cast<Class>(cls)), "init");
        send<void>(l, "setPlayer:", player);
        return autorelease(l);
    });
    def(C_AVPlayerLayer, "player", +[](id self, SEL) -> id { return layer_state(self)->player; });
    def(C_AVPlayerLayer, "setPlayer:", +[](id self, SEL, id player) {
        av_trace("layer setPlayer", player);
        auto* le = layer_state(self);
        if (le->player == player) return;
        layer_detach(self);
        id prev = le->player;
        le->player = retain(player);
        release(prev);
        if (player) {
            player_of(player)->layer = self;
            player_apply(player);
        }
    });
    def(C_AVPlayerLayer, "videoGravity", +[](id self, SEL) -> id { return layer_state(self)->gravity; });
    def(C_AVPlayerLayer, "setVideoGravity:", +[](id self, SEL, id g) {
        auto* le = layer_state(self);
        id prev = le->gravity;
        le->gravity = retain(g);
        release(prev);
    });
    def(C_AVPlayerLayer, "isReadyForDisplay", +[](id, SEL) -> bool { return true; });
    def(C_AVPlayerLayer, "removeFromSuperlayer", +[](id self, SEL) {
        layer_detach(self);
        send_super<void>(self, C_AVPlayerLayer, "removeFromSuperlayer");
    });
    add_dealloc_hook([](id o) {
        {
            std::lock_guard<std::mutex> l(g_layer_mu);
            if (!g_layer_states.count(o)) return;
        }
        layer_detach(o);
        LayerState st;
        {
            std::lock_guard<std::mutex> l(g_layer_mu);
            auto it = g_layer_states.find(o);
            if (it == g_layer_states.end()) return;
            st = it->second;
            g_layer_states.erase(it);
        }
        release(st.player);
        release(st.gravity);
    });

    // AVAudioPlayer
    C_AVAudioPlayer = def_class("AVAudioPlayer", "NSObject");
    def(C_AVAudioPlayer, "initWithContentsOfURL:error:", +[](id self, SEL, id url, id* err) -> id {
        if (err) *err = nullptr;
        auto* e = ext_get<AudioExt>(self);
        e->url = retain(url);
        e->s.path = url_path(url);
        return self;
    });
    def(C_AVAudioPlayer, "initWithContentsOfURL:fileTypeHint:error:", +[](id self, SEL, id url, id, id* err) -> id {
        return send(self, "initWithContentsOfURL:error:", url, err);
    });
    def(C_AVAudioPlayer, "initWithData:error:", +[](id self, SEL, id data, id* err) -> id {
        if (err) *err = nullptr;
        auto* e = ext_get<AudioExt>(self);
        e->data = retain(data);
        // the host player reads files: the data goes to one in the temporary directory
        static std::atomic<int> n{0};
        e->temp = sandbox_dir("tmp") + "/avaudio_" + std::to_string(++n);
        if (FILE* f = fopen(e->temp.c_str(), "wb")) {
            fwrite(send<const void*>(data, "bytes"), 1, send<NSUInteger>(data, "length"), f);
            fclose(f);
        }
        e->s.path = e->temp;
        return self;
    });
    def(C_AVAudioPlayer, "prepareToPlay", +[](id self, SEL) -> bool { return ext_get<AudioExt>(self)->s.open(self, false); });
    def(C_AVAudioPlayer, "play", +[](id self, SEL) -> bool {
        auto* e = ext_get<AudioExt>(self);
        if (!e->s.playing) e->loops_left = e->loops;
        e->s.play(self, false);
        return true;
    });
    def(C_AVAudioPlayer, "playAtTime:", +[](id self, SEL, double) -> bool { return send<bool>(self, "play"); });
    def(C_AVAudioPlayer, "pause", +[](id self, SEL) { ext_get<AudioExt>(self)->s.pause(); });
    def(C_AVAudioPlayer, "stop", +[](id self, SEL) { ext_get<AudioExt>(self)->s.pause(); });   // stop keeps the position, as on iOS
    def(C_AVAudioPlayer, "isPlaying", +[](id self, SEL) -> bool { return ext_get<AudioExt>(self)->s.playing; });
    def(C_AVAudioPlayer, "volume", +[](id self, SEL) -> float { return ext_get<AudioExt>(self)->s.volume; });
    def(C_AVAudioPlayer, "setVolume:", +[](id self, SEL, float v) { ext_get<AudioExt>(self)->s.set_volume(v); });
    def(C_AVAudioPlayer, "setVolume:fadeDuration:", +[](id self, SEL, float v, double) { ext_get<AudioExt>(self)->s.set_volume(v); });
    def(C_AVAudioPlayer, "pan", +[](id, SEL) -> float { return 0.f; });
    def(C_AVAudioPlayer, "setPan:", +[](id, SEL, float) {});
    def(C_AVAudioPlayer, "numberOfLoops", +[](id self, SEL) -> long { return ext_get<AudioExt>(self)->loops; });
    def(C_AVAudioPlayer, "setNumberOfLoops:", +[](id self, SEL, long n) {
        auto* e = ext_get<AudioExt>(self);
        e->loops = n;
        e->loops_left = n;
        e->s.set_loop(n < 0);   // forever: the host player loops seamlessly
    });
    def(C_AVAudioPlayer, "currentTime", +[](id self, SEL) -> double { return ext_get<AudioExt>(self)->s.position() / 1000.0; });
    def(C_AVAudioPlayer, "setCurrentTime:", +[](id self, SEL, double t) { ext_get<AudioExt>(self)->s.seek(static_cast<int64_t>(t * 1000)); });
    def(C_AVAudioPlayer, "duration", +[](id self, SEL) -> double { return ext_get<AudioExt>(self)->s.length() / 1000.0; });
    def(C_AVAudioPlayer, "deviceCurrentTime", +[](id, SEL) -> double { return now_s(); });
    def(C_AVAudioPlayer, "delegate", +[](id self, SEL) -> id { return ext_get<AudioExt>(self)->delegate; });
    def(C_AVAudioPlayer, "setDelegate:", +[](id self, SEL, id d) { ext_get<AudioExt>(self)->delegate = d; });
    def(C_AVAudioPlayer, "url", +[](id self, SEL) -> id { return ext_get<AudioExt>(self)->url; });
    def(C_AVAudioPlayer, "data", +[](id self, SEL) -> id { return ext_get<AudioExt>(self)->data; });
    def(C_AVAudioPlayer, "numberOfChannels", +[](id, SEL) -> NSUInteger { return 2; });
    def(C_AVAudioPlayer, "enableRate", +[](id, SEL) -> bool { return false; });
    def(C_AVAudioPlayer, "setEnableRate:", +[](id, SEL, bool) {});
    def(C_AVAudioPlayer, "rate", +[](id, SEL) -> float { return 1.f; });
    def(C_AVAudioPlayer, "setRate:", +[](id, SEL, float) {});
    def(C_AVAudioPlayer, "isMeteringEnabled", +[](id, SEL) -> bool { return false; });
    def(C_AVAudioPlayer, "setMeteringEnabled:", +[](id, SEL, bool) {});
    def(C_AVAudioPlayer, "updateMeters", +[](id, SEL) {});
    def(C_AVAudioPlayer, "averagePowerForChannel:", +[](id, SEL, NSUInteger) -> float { return -160.f; });
    def(C_AVAudioPlayer, "peakPowerForChannel:", +[](id, SEL, NSUInteger) -> float { return -160.f; });
}

}  // namespace app
