// App mode CoreGraphics and UIImage: bitmap contexts, a CgBI/standard PNG decoder (iOS ships byte-swapped,
// premultiplied "CgBI" PNGs that Android's decoder rejects) and JPEG through Android's AImageDecoder, plus the
// CGGeometry helpers, color spaces, data providers and gradients a 2-D iOS game touches.
#include "app.hpp"

#include <android/imagedecoder.h>
#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <map>

// the host's text engine (android_app.cpp); absent in the command-line host
extern "C" int host_text_measure(const uint16_t* text, int len, float size, float* width, float* ascent, float* descent, float* leading) __attribute__((weak));
extern "C" int host_text_draw(const uint16_t* text, int len, float size, uint32_t rgba, uint8_t* dst, int w, int h, float x, float y) __attribute__((weak));

namespace app {

bool color_components(id c, double rgba[4]);   // app_uikit.cpp

namespace {

struct Bitmap {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;   // premultiplied RGBA8, top-down
    int scale = 1;
};

// ---- PNG (standard + Apple CgBI) ----
uint32_t rd_be(const uint8_t* p) { return static_cast<uint32_t>(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

int paeth(int a, int b, int c) {
    int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}
void unfilter(std::vector<uint8_t>& raw, int w, int h, int bpp, int stride) {
    for (int y = 0; y < h; ++y) {
        uint8_t* row = raw.data() + y * (stride + 1);
        uint8_t f = row[0];
        uint8_t* cur = row + 1;
        uint8_t* prev = y ? row - stride : nullptr;
        for (int x = 0; x < stride; ++x) {
            int a = x >= bpp ? cur[x - bpp] : 0;
            int b = prev ? prev[x] : 0;
            int c = (prev && x >= bpp) ? prev[x - bpp] : 0;
            int v = cur[x];
            switch (f) {
                case 1: v += a; break;
                case 2: v += b; break;
                case 3: v += (a + b) / 2; break;
                case 4: v += paeth(a, b, c); break;
                default: break;
            }
            cur[x] = static_cast<uint8_t>(v);
        }
    }
}

bool decode_png(const uint8_t* p, size_t n, Bitmap& out) {
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (n < 8 || std::memcmp(p, sig, 8) != 0) return false;
    size_t i = 8;
    int w = 0, h = 0, depth = 0, color = 0;
    bool cgbi = false, interlaced = false;
    std::vector<uint8_t> idat;
    std::vector<uint8_t> palette, trns;
    while (i + 8 <= n) {
        uint32_t len = rd_be(p + i);
        const uint8_t* type = p + i + 4;
        const uint8_t* body = p + i + 8;
        if (i + 12 + len > n) break;
        if (!std::memcmp(type, "CgBI", 4)) cgbi = true;
        else if (!std::memcmp(type, "IHDR", 4)) {
            w = static_cast<int>(rd_be(body)); h = static_cast<int>(rd_be(body + 4));
            depth = body[8]; color = body[9]; interlaced = body[12] != 0;
        } else if (!std::memcmp(type, "PLTE", 4)) {
            palette.assign(body, body + len);
        } else if (!std::memcmp(type, "tRNS", 4)) {
            trns.assign(body, body + len);
        } else if (!std::memcmp(type, "IDAT", 4)) {
            idat.insert(idat.end(), body, body + len);
        } else if (!std::memcmp(type, "IEND", 4)) {
            break;
        }
        i += 12 + len;
    }
    if (w <= 0 || h <= 0 || depth != 8 || interlaced) return false;   // decode_image falls back to the platform decoder
    int channels = color == 2 ? 3 : color == 6 ? 4 : color == 0 ? 1 : color == 4 ? 2 : color == 3 ? 1 : 0;
    if (!channels) return false;
    int stride = w * channels;
    std::vector<uint8_t> raw(static_cast<size_t>(stride + 1) * h);
    z_stream zs{};
    // CgBI stores raw deflate (no zlib header); standard PNG has the 2-byte header
    if (inflateInit2(&zs, cgbi ? -15 : 15) != Z_OK) return false;
    zs.next_in = const_cast<uint8_t*>(idat.data());
    zs.avail_in = static_cast<uInt>(idat.size());
    zs.next_out = raw.data();
    zs.avail_out = static_cast<uInt>(raw.size());
    int zr = inflate(&zs, Z_FINISH);
    inflateEnd(&zs);
    if (zr != Z_STREAM_END && zs.total_out != raw.size()) { log("PNG: inflate failed (%d)", zr); return false; }
    unfilter(raw, w, h, channels, stride);
    out.w = w;
    out.h = h;
    out.rgba.resize(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = raw.data() + y * (stride + 1) + 1;
        uint8_t* dst = out.rgba.data() + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x) {
            uint8_t r, g, b, a = 255;
            if (color == 2) { r = row[x * 3]; g = row[x * 3 + 1]; b = row[x * 3 + 2]; }
            else if (color == 6) { r = row[x * 4]; g = row[x * 4 + 1]; b = row[x * 4 + 2]; a = row[x * 4 + 3]; }
            else if (color == 0) { r = g = b = row[x]; }
            else if (color == 4) { r = g = b = row[x * 2]; a = row[x * 2 + 1]; }
            else {   // palette
                int idx = row[x];
                r = idx * 3 < static_cast<int>(palette.size()) ? palette[idx * 3] : 0;
                g = idx * 3 + 1 < static_cast<int>(palette.size()) ? palette[idx * 3 + 1] : 0;
                b = idx * 3 + 2 < static_cast<int>(palette.size()) ? palette[idx * 3 + 2] : 0;
                a = idx < static_cast<int>(trns.size()) ? trns[idx] : 255;
            }
            if (cgbi) std::swap(r, b);   // CgBI is BGRA; it is also already premultiplied
            dst[x * 4] = r;
            dst[x * 4 + 1] = g;
            dst[x * 4 + 2] = b;
            dst[x * 4 + 3] = a;
        }
    }
    return true;
}

bool decode_android(const uint8_t* p, size_t n, Bitmap& out) {
    AImageDecoder* dec = nullptr;
    if (AImageDecoder_createFromBuffer(p, n, &dec) != ANDROID_IMAGE_DECODER_SUCCESS) return false;
    const AImageDecoderHeaderInfo* info = AImageDecoder_getHeaderInfo(dec);
    out.w = AImageDecoderHeaderInfo_getWidth(info);
    out.h = AImageDecoderHeaderInfo_getHeight(info);
    AImageDecoder_setAndroidBitmapFormat(dec, 1 /*RGBA_8888*/);
    size_t stride = AImageDecoder_getMinimumStride(dec);
    out.rgba.resize(stride * out.h);
    int r = AImageDecoder_decodeImage(dec, out.rgba.data(), stride, out.rgba.size());
    AImageDecoder_delete(dec);
    if (r != ANDROID_IMAGE_DECODER_SUCCESS) return false;
    if (stride != static_cast<size_t>(out.w) * 4) {
        std::vector<uint8_t> packed(static_cast<size_t>(out.w) * out.h * 4);
        for (int y = 0; y < out.h; ++y) std::memcpy(packed.data() + static_cast<size_t>(y) * out.w * 4, out.rgba.data() + y * stride, static_cast<size_t>(out.w) * 4);
        out.rgba.swap(packed);
    }
    return true;
}

bool decode_image(const uint8_t* p, size_t n, Bitmap& out) {
    // decode_png is there for Apple's CgBI variant and reads plain 8-bit PNGs on the way; palette depths below 8,
    // 16-bit and interlaced files go to the platform decoder like every other format
    if (n >= 8 && p[0] == 137 && p[1] == 'P' && decode_png(p, n, out)) return true;
    return decode_android(p, n, out);
}

// ---- object storage ----
Class C_UIImage, C_CGImageRef, C_CGColorSpace, C_CGContext, C_CGDataProvider, C_CGColor, C_CGFont, C_CGGradient, C_CGPath;
struct ImageExt : Ext { std::shared_ptr<Bitmap> bmp; int orientation = 0; double scale = 1; id cg = nullptr; ~ImageExt() override { release(cg); } };
struct CGImageExt : Ext { std::shared_ptr<Bitmap> bmp; };
struct ContextExt : Ext {
    Bitmap bmp;
    int bpp = 4;
    bool gray = false;
    std::vector<CGAffineTransform> stack;
    CGAffineTransform ctm{1, 0, 0, 1, 0, 0};
    uint8_t* user = nullptr;      // the caller's backing store (CGBitmapContextCreate data), kept in sync with bmp
    size_t user_bpr = 0;
    int order = 0;                // byte order of a pixel in the backing store, from the bitmap info (kPixelOrder)
    std::vector<uint8_t> own;     // the backing store of a context made without one, when its order is not RGBA
    double fill[4] = {0, 0, 0, 1};
    CGPoint text_pos{0, 0};
};
// Drawing goes to bmp; a context created over the caller's memory mirrors it there, where the caller reads it.
// Where R, G, B and A sit in a pixel: RGBA (alpha last), BGRA (alpha first, 32-bit little endian: the native iOS
// layout), ARGB (alpha first), ABGR (alpha last, little endian). bmp itself is always RGBA.
const uint8_t kPixelOrder[4][4] = {{0, 1, 2, 3}, {2, 1, 0, 3}, {1, 2, 3, 0}, {3, 2, 1, 0}};
// The other way, before drawing or snapshotting: the app also writes pixels into its buffer directly.
void load_user(ContextExt* e) {
    if (!e->user) return;
    const uint8_t* o = kPixelOrder[e->order];
    for (int y = 0; y < e->bmp.h; ++y) {
        uint8_t* d = e->bmp.rgba.data() + static_cast<size_t>(y) * e->bmp.w * 4;
        const uint8_t* s = e->user + static_cast<size_t>(y) * e->user_bpr;
        if (!e->order) { std::memcpy(d, s, static_cast<size_t>(e->bmp.w) * 4); continue; }
        for (int x = 0; x < e->bmp.w; ++x, d += 4, s += 4) { d[0] = s[o[0]]; d[1] = s[o[1]]; d[2] = s[o[2]]; d[3] = s[o[3]]; }
    }
}
void sync_user(ContextExt* e) {
    if (!e->user) return;
    const uint8_t* o = kPixelOrder[e->order];
    for (int y = 0; y < e->bmp.h; ++y) {
        uint8_t* d = e->user + static_cast<size_t>(y) * e->user_bpr;
        const uint8_t* s = e->bmp.rgba.data() + static_cast<size_t>(y) * e->bmp.w * 4;
        if (!e->order) { std::memcpy(d, s, static_cast<size_t>(e->bmp.w) * 4); continue; }
        for (int x = 0; x < e->bmp.w; ++x, d += 4, s += 4) { d[o[0]] = s[0]; d[o[1]] = s[1]; d[o[2]] = s[2]; d[o[3]] = s[3]; }
    }
}
struct ProviderExt : Ext {
    id data = nullptr;
    // CGDataProviderCreateWithData: the caller's buffer and the callback that gives it back
    void (*free_cb)(void*, const void*, size_t) = nullptr;
    void* free_info = nullptr;
    const void* free_data = nullptr;
    size_t free_size = 0;
    ~ProviderExt() override {
        release(data);
        if (free_cb) free_cb(free_info, free_data, free_size);
    }
};

std::shared_ptr<Bitmap> image_bitmap(id img);

id make_cgimage(std::shared_ptr<Bitmap> b) {
    if (!b) return nullptr;
    id c = alloc(C_CGImageRef);
    ext_get<CGImageExt>(c)->bmp = b;
    return autorelease(c);
}
id make_uiimage(std::shared_ptr<Bitmap> b, double scale) {
    if (!b) return nullptr;
    id img = alloc(C_UIImage);
    auto* e = ext_get<ImageExt>(img);
    e->bmp = b;
    e->scale = scale;
    return autorelease(img);
}

std::map<std::string, std::weak_ptr<Bitmap>> g_image_cache;
std::mutex g_cache_mu;

std::shared_ptr<Bitmap> load_bitmap_file(const std::string& path) {
    {
        std::lock_guard<std::mutex> l(g_cache_mu);
        auto it = g_image_cache.find(path);
        if (it != g_image_cache.end()) if (auto sp = it->second.lock()) return sp;
    }
    std::string data;
    if (!read_file(path, data)) return nullptr;
    auto b = std::make_shared<Bitmap>();
    if (!decode_image(reinterpret_cast<const uint8_t*>(data.data()), data.size(), *b)) { log("image decode failed: %s", path.c_str()); return nullptr; }
    std::lock_guard<std::mutex> l(g_cache_mu);
    g_image_cache[path] = b;
    return b;
}

// imageNamed: tries @2x/@3x and the device idiom suffix, like UIKit
id image_named(const std::string& name) {
    std::string base = bundle_dir() + "/";
    std::string stem = name, ext;
    size_t dot = name.rfind('.'), slash = name.rfind('/');
    // an extension is a dot in the last path component ("foo.png", "ui/foo.png", but not "ui.v2/foo")
    if (dot != std::string::npos && (slash == std::string::npos || slash < dot)) { stem = name.substr(0, dot); ext = name.substr(dot); }
    if (ext.empty()) ext = ".png";
    double scale = 1;
    std::vector<std::pair<std::string, double>> tries = {{stem + "@2x~iphone" + ext, 2}, {stem + "@2x" + ext, 2},
                                                         {stem + "~iphone" + ext, 1}, {stem + ext, 1}};
    for (auto& [cand, sc] : tries) {
        auto b = load_bitmap_file(base + cand);
        if (b) { scale = sc; return make_uiimage(b, scale); }
    }
    return nullptr;
}

std::shared_ptr<Bitmap> image_bitmap(id img) {
    if (ImageExt* e = ext_as<ImageExt>(img)) return e->bmp;
    if (CGImageExt* e = ext_as<CGImageExt>(img)) return e->bmp;
    return nullptr;
}

}  // namespace

bool uiimage_bitmap(id img, const uint8_t** rgba, int* w, int* h) {
    auto b = image_bitmap(img);
    if (!b) return false;
    if (rgba) *rgba = b->rgba.data();
    if (w) *w = b->w;
    if (h) *h = b->h;
    return true;
}

}  // namespace app

using namespace app;

// ---------------------------------------------------------------------------------------------------------------
// CGGeometry (C)
// ---------------------------------------------------------------------------------------------------------------
extern "C" {
static CGAffineTransform cg_concat(CGAffineTransform a, CGAffineTransform b) {
    return {a.a * b.a + a.b * b.c, a.a * b.b + a.b * b.d, a.c * b.a + a.d * b.c, a.c * b.b + a.d * b.d, a.tx * b.a + a.ty * b.c + b.tx, a.tx * b.b + a.ty * b.d + b.ty};
}
static CGAffineTransform cg_make_rotation(double r) { return {std::cos(r), std::sin(r), -std::sin(r), std::cos(r), 0, 0}; }
static CGAffineTransform cg_make_scale(double sx, double sy) { return {sx, 0, 0, sy, 0, 0}; }
static CGAffineTransform cg_make_translation(double tx, double ty) { return {1, 0, 0, 1, tx, ty}; }
static CGAffineTransform cg_rotate(CGAffineTransform t, double r) { return cg_concat(cg_make_rotation(r), t); }
static CGAffineTransform cg_scale(CGAffineTransform t, double sx, double sy) { return cg_concat(cg_make_scale(sx, sy), t); }
static CGAffineTransform cg_translate(CGAffineTransform t, double tx, double ty) { return cg_concat(cg_make_translation(tx, ty), t); }
static CGAffineTransform cg_invert(CGAffineTransform t) {
    double det = t.a * t.d - t.b * t.c;
    if (std::fabs(det) < 1e-12) return t;
    double id = 1 / det;
    return {t.d * id, -t.b * id, -t.c * id, t.a * id, (t.c * t.ty - t.d * t.tx) * id, (t.b * t.tx - t.a * t.ty) * id};
}
static CGPoint cg_apply_point(CGPoint p, CGAffineTransform t) { return {p.x * t.a + p.y * t.c + t.tx, p.x * t.b + p.y * t.d + t.ty}; }
static bool cg_equal_transform(CGAffineTransform a, CGAffineTransform b) { return a.a == b.a && a.b == b.b && a.c == b.c && a.d == b.d && a.tx == b.tx && a.ty == b.ty; }
static CGSize cg_apply_size(CGSize s, CGAffineTransform t) { return {s.width * t.a + s.height * t.c, s.width * t.b + s.height * t.d}; }
static CGRect cg_apply_rect(CGRect r, CGAffineTransform t) {
    CGPoint c[4] = {cg_apply_point({r.origin.x, r.origin.y}, t), cg_apply_point({r.origin.x + r.size.width, r.origin.y}, t),
                    cg_apply_point({r.origin.x, r.origin.y + r.size.height}, t), cg_apply_point({r.origin.x + r.size.width, r.origin.y + r.size.height}, t)};
    double minx = c[0].x, miny = c[0].y, maxx = c[0].x, maxy = c[0].y;
    for (int i = 1; i < 4; ++i) { minx = std::min(minx, c[i].x); miny = std::min(miny, c[i].y); maxx = std::max(maxx, c[i].x); maxy = std::max(maxy, c[i].y); }
    return {{minx, miny}, {maxx - minx, maxy - miny}};
}
static CGRect cg_rect_standardize(CGRect r) {
    if (r.size.width < 0) { r.origin.x += r.size.width; r.size.width = -r.size.width; }
    if (r.size.height < 0) { r.origin.y += r.size.height; r.size.height = -r.size.height; }
    return r;
}
static CGRect cg_rect_integral(CGRect r) {
    r = cg_rect_standardize(r);
    double x = std::floor(r.origin.x), y = std::floor(r.origin.y);
    return {{x, y}, {std::ceil(r.origin.x + r.size.width) - x, std::ceil(r.origin.y + r.size.height) - y}};
}
static CGRect cg_rect_inset(CGRect r, double dx, double dy) { return {{r.origin.x + dx, r.origin.y + dy}, {r.size.width - 2 * dx, r.size.height - 2 * dy}}; }
static CGRect cg_rect_offset(CGRect r, double dx, double dy) { return {{r.origin.x + dx, r.origin.y + dy}, r.size}; }
// CGRectNull: origin at infinity. The union with it is the other rectangle, and no intersection gives it back.
static bool cg_rect_is_null(CGRect r) { return std::isinf(r.origin.x) || std::isinf(r.origin.y); }
static CGRect cg_rect_union(CGRect a, CGRect b) {
    if (cg_rect_is_null(a)) return b;
    if (cg_rect_is_null(b)) return a;
    a = cg_rect_standardize(a); b = cg_rect_standardize(b);
    double minx = std::min(a.origin.x, b.origin.x), miny = std::min(a.origin.y, b.origin.y);
    double maxx = std::max(a.origin.x + a.size.width, b.origin.x + b.size.width), maxy = std::max(a.origin.y + a.size.height, b.origin.y + b.size.height);
    return {{minx, miny}, {maxx - minx, maxy - miny}};
}
static CGRect cg_rect_intersection(CGRect a, CGRect b) {
    a = cg_rect_standardize(a); b = cg_rect_standardize(b);
    double x = std::max(a.origin.x, b.origin.x), y = std::max(a.origin.y, b.origin.y);
    double r = std::min(a.origin.x + a.size.width, b.origin.x + b.size.width), bot = std::min(a.origin.y + a.size.height, b.origin.y + b.size.height);
    if (cg_rect_is_null(a) || cg_rect_is_null(b) || r < x || bot < y) return {{INFINITY, INFINITY}, {0, 0}};   // CGRectNull
    return {{x, y}, {r - x, bot - y}};
}
static bool cg_rect_contains_point(CGRect r, CGPoint p) { r = cg_rect_standardize(r); return p.x >= r.origin.x && p.x < r.origin.x + r.size.width && p.y >= r.origin.y && p.y < r.origin.y + r.size.height; }
static bool cg_rect_contains_rect(CGRect a, CGRect b) { return cg_equal_transform({a.origin.x, a.origin.y, a.size.width, a.size.height, 0, 0}, {a.origin.x, a.origin.y, a.size.width, a.size.height, 0, 0}) && cg_rect_contains_point(a, b.origin) && cg_rect_contains_point(a, {b.origin.x + b.size.width - 0.001, b.origin.y + b.size.height - 0.001}); }
static bool cg_rect_intersects(CGRect a, CGRect b) { CGRect i = cg_rect_intersection(a, b); return i.size.width > 0 && i.size.height > 0; }
static bool cg_rect_is_empty(CGRect r) { return r.size.width <= 0 || r.size.height <= 0; }
static bool cg_rect_equal(CGRect a, CGRect b) { return a.origin.x == b.origin.x && a.origin.y == b.origin.y && a.size.width == b.size.width && a.size.height == b.size.height; }
static bool cg_point_equal(CGPoint a, CGPoint b) { return a.x == b.x && a.y == b.y; }
static bool cg_size_equal(CGSize a, CGSize b) { return a.width == b.width && a.height == b.height; }
static double cg_rect_get_min_x(CGRect r) { return std::min(r.origin.x, r.origin.x + r.size.width); }
static double cg_rect_get_min_y(CGRect r) { return std::min(r.origin.y, r.origin.y + r.size.height); }
static double cg_rect_get_max_x(CGRect r) { return std::max(r.origin.x, r.origin.x + r.size.width); }
static double cg_rect_get_max_y(CGRect r) { return std::max(r.origin.y, r.origin.y + r.size.height); }
static double cg_rect_get_mid_x(CGRect r) { return r.origin.x + r.size.width / 2; }
static double cg_rect_get_mid_y(CGRect r) { return r.origin.y + r.size.height / 2; }
static double cg_rect_get_width(CGRect r) { return std::fabs(r.size.width); }
static double cg_rect_get_height(CGRect r) { return std::fabs(r.size.height); }
static int cg_point_from_dict(id, CGPoint*) { return 0; }
static int cg_rect_from_dict(id, CGRect*) { return 0; }
}

// ---------------------------------------------------------------------------------------------------------------
// CoreGraphics objects
// ---------------------------------------------------------------------------------------------------------------
extern "C" {
static void* cg_colorspace_rgb() { return reinterpret_cast<void*>(1); }
static void* cg_colorspace_gray() { return reinterpret_cast<void*>(2); }
static void cg_release_noop(void*) {}
static void* cg_retain_id(void* o) { return retain(static_cast<id>(o)); }
static long cg_colorspace_model(void* cs) { return cs == reinterpret_cast<void*>(2) ? 0 : 1; }
static size_t cg_colorspace_components(void* cs) { return cs == reinterpret_cast<void*>(2) ? 1 : 3; }

static id cg_bitmap_context_create(void* data, size_t w, size_t h, size_t bpc, size_t bpr, void* cs, uint32_t info) {
    id c = alloc(C_CGContext);
    auto* e = ext_get<ContextExt>(c);
    e->bmp.w = static_cast<int>(w);
    e->bmp.h = static_cast<int>(h);
    e->gray = cs == reinterpret_cast<void*>(2);
    e->bmp.rgba.assign(static_cast<size_t>(w) * h * 4, 0);
    // kCGImageAlphaPremultipliedFirst / First / NoneSkipFirst put alpha in front; kCGBitmapByteOrder32Little reverses the bytes
    const uint32_t alpha = info & 0x1f;
    const bool alpha_first = alpha == 2 || alpha == 4 || alpha == 6, little = (info & 0x7000) == 0x2000;
    e->order = e->gray ? 0 : alpha_first ? (little ? 1 : 2) : (little ? 3 : 0);
    if (data && bpc == 8 && bpr >= w * 4 && !e->gray) {   // 8-bit color over the caller's buffer: start from its contents
        e->user = static_cast<uint8_t*>(data);
        e->user_bpr = bpr;
        load_user(e);
    } else if (!data && e->order) {   // CoreGraphics allocates the store: the app reads it through CGBitmapContextGetData
        e->own.assign(static_cast<size_t>(w) * h * 4, 0);
        e->user = e->own.data();
        e->user_bpr = w * 4;
    }
    return c;   // +1 (CoreGraphics create rule)
}
static id cg_bitmap_context_create_image(id ctx) {
    auto* e = ext_as<ContextExt>(ctx);
    if (!e) return nullptr;
    load_user(e);
    auto b = std::make_shared<Bitmap>(e->bmp);
    id img = alloc(C_CGImageRef);
    ext_get<CGImageExt>(img)->bmp = b;
    return img;   // +1
}
static void* cg_bitmap_context_get_data(id ctx) { auto* e = ext_as<ContextExt>(ctx); return !e ? nullptr : e->user ? e->user : e->bmp.rgba.data(); }
static size_t cg_bitmap_context_get_width(id ctx) { auto* e = ext_as<ContextExt>(ctx); return e ? e->bmp.w : 0; }
static size_t cg_bitmap_context_get_height(id ctx) { auto* e = ext_as<ContextExt>(ctx); return e ? e->bmp.h : 0; }
static size_t cg_bitmap_context_get_bpr(id ctx) { auto* e = ext_as<ContextExt>(ctx); return !e ? 0 : e->user ? e->user_bpr : static_cast<size_t>(e->bmp.w) * 4; }

static id cg_provider_with_data(void* info, const void* data, size_t size, void (*release_data)(void*, const void*, size_t)) {
    id p = alloc(C_CGDataProvider);
    auto* e = ext_get<ProviderExt>(p);
    e->data = retain(data_new(data, size));
    e->free_cb = release_data;
    e->free_info = info;
    e->free_data = data;
    e->free_size = size;
    return p;
}
static id cg_provider_with_cfdata(id cfdata) {
    id p = alloc(C_CGDataProvider);
    ext_get<ProviderExt>(p)->data = retain(cfdata);
    return p;
}
static id cg_provider_with_file(const char* path) {
    std::string s;
    if (!read_file(remap_path(path ? path : ""), s)) return nullptr;
    id p = alloc(C_CGDataProvider);
    ext_get<ProviderExt>(p)->data = retain(data_new(s.data(), s.size()));
    return p;
}
static id cg_image_from_png(id provider, void*, bool, int) {
    auto* pe = ext_as<ProviderExt>(provider);
    if (!pe) return nullptr;
    auto b = std::make_shared<Bitmap>();
    if (!decode_png(data_bytes(pe->data), data_len(pe->data), *b)) return nullptr;
    id img = alloc(C_CGImageRef);
    ext_get<CGImageExt>(img)->bmp = b;
    return img;
}
static id cg_image_from_jpeg(id provider, void*, bool, int) {
    auto* pe = ext_as<ProviderExt>(provider);
    if (!pe) return nullptr;
    auto b = std::make_shared<Bitmap>();
    if (!decode_android(data_bytes(pe->data), data_len(pe->data), *b)) return nullptr;
    id img = alloc(C_CGImageRef);
    ext_get<CGImageExt>(img)->bmp = b;
    return img;
}
static size_t cg_image_get_width(id img) { auto b = image_bitmap(img); return b ? b->w : 0; }
static size_t cg_image_get_height(id img) { auto b = image_bitmap(img); return b ? b->h : 0; }
static size_t cg_image_get_bpc(id) { return 8; }
static size_t cg_image_get_bpp(id) { return 32; }
static size_t cg_image_get_bpr(id img) { auto b = image_bitmap(img); return b ? static_cast<size_t>(b->w) * 4 : 0; }
static void* cg_image_get_colorspace(id) { return reinterpret_cast<void*>(1); }
static uint32_t cg_image_get_alpha(id) { return 1; }   // kCGImageAlphaPremultipliedLast
static uint32_t cg_image_get_bitmapinfo(id) { return 1; }
static id cg_image_create_copy(id img) { auto b = image_bitmap(img); if (!b) return nullptr; id c = alloc(C_CGImageRef); ext_get<CGImageExt>(c)->bmp = b; return c; }
static id cg_image_create_with_image_in_rect(id img, CGRect r) {
    auto b = image_bitmap(img);
    if (!b) return nullptr;
    int x = std::max(0, static_cast<int>(r.origin.x)), y = std::max(0, static_cast<int>(r.origin.y));
    int w = std::min(static_cast<int>(r.size.width), b->w - x), h = std::min(static_cast<int>(r.size.height), b->h - y);
    if (w <= 0 || h <= 0) return nullptr;
    auto sub = std::make_shared<Bitmap>();
    sub->w = w; sub->h = h; sub->rgba.resize(static_cast<size_t>(w) * h * 4);
    for (int yy = 0; yy < h; ++yy)
        std::memcpy(sub->rgba.data() + static_cast<size_t>(yy) * w * 4, b->rgba.data() + (static_cast<size_t>(y + yy) * b->w + x) * 4, static_cast<size_t>(w) * 4);
    id c = alloc(C_CGImageRef);
    ext_get<CGImageExt>(c)->bmp = sub;
    return c;
}
static void* cg_color_create(void*, const double* comps) { (void)comps; return reinterpret_cast<void*>(0x3); }
static void* cg_color_create_generic_rgb(double, double, double, double) { return reinterpret_cast<void*>(0x3); }
static const double* cg_color_get_components(void*) { static double c[4] = {0, 0, 0, 1}; return c; }
static size_t cg_color_get_count(void*) { return 4; }
static void* cg_font_create(const char*) { return reinterpret_cast<void*>(0x5); }
static void cg_noop() {}
static void cg_context_noop(id, ...) {}
static void cg_draw_image(id ctx, CGRect r, id img) {
    auto* ce = ext_as<ContextExt>(ctx);
    auto b = image_bitmap(img);
    if (!ce || !b) return;
    load_user(ce);
    int dx = static_cast<int>(r.origin.x), dy = static_cast<int>(r.origin.y), dw = static_cast<int>(r.size.width), dh = static_cast<int>(r.size.height);
    for (int y = 0; y < dh; ++y) {
        int ty = dy + y;
        if (ty < 0 || ty >= ce->bmp.h) continue;
        int sy = b->h - 1 - (y * b->h / std::max(dh, 1));   // CoreGraphics y is bottom-up
        for (int x = 0; x < dw; ++x) {
            int tx = dx + x;
            if (tx < 0 || tx >= ce->bmp.w) continue;
            int sx = x * b->w / std::max(dw, 1);
            const uint8_t* s = b->rgba.data() + (static_cast<size_t>(sy) * b->w + sx) * 4;
            uint8_t* d = ce->bmp.rgba.data() + (static_cast<size_t>(ty) * ce->bmp.w + tx) * 4;
            int a = s[3];
            for (int k = 0; k < 3; ++k) d[k] = static_cast<uint8_t>((s[k] * a + d[k] * (255 - a)) / 255);
            d[3] = static_cast<uint8_t>(a + d[3] * (255 - a) / 255);
        }
    }
    sync_user(ce);
}

// CGContextClearRect: transparent black. Texture loaders clear the buffer they malloc'ed before drawing into it;
// without this the transparent parts of the image keep whatever the heap had there.
static void cg_clear_rect(id ctx, CGRect r) {
    auto* e = ext_as<ContextExt>(ctx);
    if (!e) return;
    load_user(e);
    r = cg_rect_standardize(r);
    const double w = e->bmp.w, h = e->bmp.h;
    const int x0 = static_cast<int>(std::clamp(std::floor(r.origin.x), 0.0, w)), x1 = static_cast<int>(std::clamp(std::ceil(r.origin.x + r.size.width), 0.0, w));
    // CoreGraphics y runs up from the bottom row
    const int y0 = static_cast<int>(std::clamp(h - std::ceil(r.origin.y + r.size.height), 0.0, h)), y1 = static_cast<int>(std::clamp(h - std::floor(r.origin.y), 0.0, h));
    for (int y = y0; y < y1 && x1 > x0; ++y) std::memset(e->bmp.rgba.data() + (static_cast<size_t>(y) * e->bmp.w + x0) * 4, 0, static_cast<size_t>(x1 - x0) * 4);
    sync_user(e);
}

// ---- CoreText: CTFont is UIFont (toll-free on iOS), CTLine a run of text; glyphs come from the host's text engine ----
Class C_CTLine;
struct LineExt : Ext { std::u16string text; double size = 12; bool has_color = false; double rgba[4] = {0, 0, 0, 1}; };

id attr_any(id attrs, std::initializer_list<const char*> keys) {
    for (const char* k : keys) if (id v = dict_get(attrs, k)) return v;
    return nullptr;
}

id ct_font_create_ui(uint32_t, double size, id) { return retain(send_cls(class_named("UIFont"), "systemFontOfSize:", size > 0 ? size : 12.0)); }
id ct_font_create_with_name(id name, double size, const void*) { return retain(send_cls(class_named("UIFont"), "fontWithName:size:", name, size > 0 ? size : 12.0)); }
double ct_font_get_size(id font) { return send<double>(font, "pointSize"); }

id ct_line_create(id as) {
    id line = alloc(C_CTLine);
    auto* e = ext_get<LineExt>(line);
    e->text = u16(send(as, "string"));
    id attrs = send(as, "attributesAtIndex:effectiveRange:", static_cast<NSUInteger>(0), static_cast<NSRange*>(nullptr));
    e->has_color = true;   // CoreText's default foreground is black
    if (attrs) {
        if (id font = attr_any(attrs, {"kCTFontAttributeName", "NSFont", "NSFontAttributeName"})) e->size = send<double>(font, "pointSize");
        if (attr_any(attrs, {"kCTForegroundColorFromContextAttributeName", "CTForegroundColorFromContext"})) e->has_color = false;   // the fill color
        else if (id color = attr_any(attrs, {"kCTForegroundColorAttributeName", "CTForegroundColor", "NSColor", "NSForegroundColorAttributeName"})) color_components(color, e->rgba);
    }
    return line;   // +1 (create rule)
}

void line_metrics(const LineExt* e, double& w, double& ascent, double& descent, double& leading) {
    float fw, fa, fd, fl;
    if (host_text_measure && host_text_measure(reinterpret_cast<const uint16_t*>(e->text.data()), static_cast<int>(e->text.size()), static_cast<float>(e->size), &fw, &fa, &fd, &fl)) {
        w = fw; ascent = fa; descent = fd; leading = fl;
        return;
    }
    w = 0.55 * e->size * static_cast<double>(e->text.size());   // no text engine (command-line host): an estimate
    ascent = 0.8 * e->size;
    descent = 0.2 * e->size;
    leading = 0;
}
double ct_line_typographic_bounds(id line, double* ascent, double* descent, double* leading) {
    auto* e = ext_as<LineExt>(line);
    double w = 0, a = 0, d = 0, l = 0;
    if (e) line_metrics(e, w, a, d, l);
    if (ascent) *ascent = a;
    if (descent) *descent = d;
    if (leading) *leading = l;
    return w;
}

void ct_line_draw(id line, id ctx) {
    auto* le = ext_as<LineExt>(line);
    auto* ce = ext_as<ContextExt>(ctx);
    if (!le || !ce || le->text.empty() || !host_text_draw) return;
    load_user(ce);
    const double* c = le->has_color ? le->rgba : ce->fill;
    auto byte = [](double v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255)); };
    uint32_t rgba = byte(c[0]) << 24 | byte(c[1]) << 16 | byte(c[2]) << 8 | byte(c[3]);
    // the text position is in CoreGraphics space (origin bottom-left, through the CTM); bitmap row 0 is the top
    const CGAffineTransform& m = ce->ctm;
    double x = m.a * ce->text_pos.x + m.c * ce->text_pos.y + m.tx;
    double y = m.b * ce->text_pos.x + m.d * ce->text_pos.y + m.ty;
    host_text_draw(reinterpret_cast<const uint16_t*>(le->text.data()), static_cast<int>(le->text.size()), static_cast<float>(le->size), rgba,
                   ce->bmp.rgba.data(), ce->bmp.w, ce->bmp.h, static_cast<float>(x), static_cast<float>(ce->bmp.h - y));
    double w, a, d, l;
    line_metrics(le, w, a, d, l);
    ce->text_pos.x += w;   // drawing advances the text position
    sync_user(ce);
}

void cg_set_fill_color(id ctx, id color) { if (auto* e = ext_as<ContextExt>(ctx)) color_components(color, e->fill); }
void cg_set_rgb_fill(id ctx, double r, double g, double b, double a) { if (auto* e = ext_as<ContextExt>(ctx)) { e->fill[0] = r; e->fill[1] = g; e->fill[2] = b; e->fill[3] = a; } }
void cg_set_gray_fill(id ctx, double v, double a) { cg_set_rgb_fill(ctx, v, v, v, a); }
void cg_set_text_position(id ctx, double x, double y) { if (auto* e = ext_as<ContextExt>(ctx)) e->text_pos = {x, y}; }
CGPoint cg_get_text_position(id ctx) { auto* e = ext_as<ContextExt>(ctx); return e ? e->text_pos : CGPoint{0, 0}; }
}

namespace app {

static id* cg_const(const char* s) { id* slot = new id; *slot = str_new(s); return slot; }

void init_cg() {
    C_CTLine = def_class("CTLine", "NSObject");
    C_UIImage = def_class("UIImage", "NSObject");
    Class I = C_UIImage;
    defc(I, "imageNamed:", +[](id, SEL, id n) -> id { return n ? image_named(utf8(n)) : nullptr; });
    defc(I, "imageWithContentsOfFile:", +[](id, SEL, id path) -> id { auto b = load_bitmap_file(remap_path(utf8(path))); double sc = utf8(path).find("@2x") != std::string::npos ? 2 : 1; return make_uiimage(b, sc); });
    defc(I, "imageWithData:", +[](id, SEL, id d) -> id {
        auto b = std::make_shared<Bitmap>();
        if (!decode_image(data_bytes(d), data_len(d), *b)) return nullptr;
        return make_uiimage(b, 1);
    });
    defc(I, "imageWithData:scale:", +[](id, SEL, id d, double s) -> id {
        auto b = std::make_shared<Bitmap>();
        if (!decode_image(data_bytes(d), data_len(d), *b)) return nullptr;
        return make_uiimage(b, s);
    });
    defc(I, "imageWithCGImage:", +[](id, SEL, id cg) -> id { return make_uiimage(image_bitmap(cg), 1); });
    defc(I, "imageWithCGImage:scale:orientation:", +[](id, SEL, id cg, double s, long) -> id { return make_uiimage(image_bitmap(cg), s); });
    def(I, "initWithContentsOfFile:", +[](id self, SEL, id path) -> id {
        auto b = load_bitmap_file(remap_path(utf8(path)));
        if (!b) { release(self); return nullptr; }
        auto* e = ext_get<ImageExt>(self);
        e->bmp = b;
        e->scale = utf8(path).find("@2x") != std::string::npos ? 2 : 1;
        return self;
    });
    def(I, "initWithData:", +[](id self, SEL, id d) -> id {
        auto b = std::make_shared<Bitmap>();
        if (!decode_image(data_bytes(d), data_len(d), *b)) { release(self); return nullptr; }
        ext_get<ImageExt>(self)->bmp = b;
        return self;
    });
    def(I, "initWithCGImage:", +[](id self, SEL, id cg) -> id { ext_get<ImageExt>(self)->bmp = image_bitmap(cg); return self; });
    def(I, "size", +[](id self, SEL) -> CGSize {
        auto* e = ext_get<ImageExt>(self);
        if (!e->bmp) return {0, 0};
        return {e->bmp->w / e->scale, e->bmp->h / e->scale};
    });
    def(I, "scale", +[](id self, SEL) -> double { return ext_get<ImageExt>(self)->scale; });
    def(I, "imageOrientation", +[](id self, SEL) -> long { return ext_get<ImageExt>(self)->orientation; });
    def(I, "CGImage", +[](id self, SEL) -> id {
        auto* e = ext_get<ImageExt>(self);
        if (!e->cg && e->bmp) e->cg = retain(make_cgimage(e->bmp));
        return e->cg;
    });
    def(I, "stretchableImageWithLeftCapWidth:topCapHeight:", +[](id self, SEL, long, long) -> id { return self; });
    def(I, "resizableImageWithCapInsets:", +[](id self, SEL, CGRect) -> id { return self; });
    def(I, "resizableImageWithCapInsets:resizingMode:", +[](id self, SEL, CGRect, long) -> id { return self; });
    def(I, "imageWithRenderingMode:", +[](id self, SEL, long) -> id { return self; });
    def(I, "imageFlippedForRightToLeftLayoutDirection", +[](id self, SEL) -> id { return self; });
    def(I, "leftCapWidth", +[](id, SEL) -> long { return 0; });
    def(I, "topCapHeight", +[](id, SEL) -> long { return 0; });
    def(I, "drawInRect:", +[](id, SEL, CGRect) {});
    def(I, "drawAtPoint:", +[](id, SEL, CGPoint) {});
    def(I, "drawInRect:blendMode:alpha:", +[](id, SEL, CGRect, long, double) {});
    def(I, "drawAsPatternInRect:", +[](id, SEL, CGRect) {});
    def(I, "copyWithZone:", +[](id self, SEL, void*) -> id { return retain(self); });

    C_CGImageRef = def_class("__CGImage", "NSObject");
    C_CGContext = def_class("__CGContext", "NSObject");
    C_CGDataProvider = def_class("__CGDataProvider", "NSObject");
    C_CGColorSpace = def_class("__CGColorSpace", "NSObject");
    C_CGColor = def_class("__CGColor", "NSObject");
    C_CGFont = def_class("__CGFont", "NSObject");
    C_CGGradient = def_class("__CGGradient", "NSObject");
    C_CGPath = def_class("__CGPath", "NSObject");

    // UIGraphics image context stack
    Class S = class_named("NSString");
    (void)S;
}

void register_cg_symbols(std::unordered_map<std::string, void*>& m) {
    auto r = [&](const char* n, auto f) { m[n] = reinterpret_cast<void*>(f); };
    // CGGeometry
    r("_CGAffineTransformMake", +[](double a, double b, double c, double d, double tx, double ty) -> CGAffineTransform { return {a, b, c, d, tx, ty}; });
    r("_CGAffineTransformConcat", &cg_concat);
    r("_CGAffineTransformInvert", &cg_invert);
    r("_CGAffineTransformMakeRotation", &cg_make_rotation);
    r("_CGAffineTransformMakeScale", &cg_make_scale);
    r("_CGAffineTransformMakeTranslation", &cg_make_translation);
    r("_CGAffineTransformRotate", &cg_rotate);
    r("_CGAffineTransformScale", &cg_scale);
    r("_CGAffineTransformTranslate", &cg_translate);
    r("_CGAffineTransformEqualToTransform", &cg_equal_transform);
    r("_CGPointApplyAffineTransform", &cg_apply_point);
    r("_CGSizeApplyAffineTransform", &cg_apply_size);
    r("_CGRectApplyAffineTransform", &cg_apply_rect);
    r("_CGRectStandardize", &cg_rect_standardize);
    r("_CGRectIntegral", &cg_rect_integral);
    r("_CGRectInset", &cg_rect_inset);
    r("_CGRectOffset", &cg_rect_offset);
    r("_CGRectUnion", &cg_rect_union);
    r("_CGRectIntersection", &cg_rect_intersection);
    r("_CGRectContainsPoint", &cg_rect_contains_point);
    r("_CGRectContainsRect", &cg_rect_contains_rect);
    r("_CGRectIntersectsRect", &cg_rect_intersects);
    r("_CGRectIsEmpty", &cg_rect_is_empty);
    r("_CGRectIsNull", &cg_rect_is_null);
    r("_CGRectEqualToRect", &cg_rect_equal);
    r("_CGPointEqualToPoint", &cg_point_equal);
    r("_CGSizeEqualToSize", &cg_size_equal);
    r("_CGRectGetMinX", &cg_rect_get_min_x);
    r("_CGRectGetMinY", &cg_rect_get_min_y);
    r("_CGRectGetMaxX", &cg_rect_get_max_x);
    r("_CGRectGetMaxY", &cg_rect_get_max_y);
    r("_CGRectGetMidX", &cg_rect_get_mid_x);
    r("_CGRectGetMidY", &cg_rect_get_mid_y);
    r("_CGRectGetWidth", &cg_rect_get_width);
    r("_CGRectGetHeight", &cg_rect_get_height);
    r("_CGPointMakeWithDictionaryRepresentation", &cg_point_from_dict);
    r("_CGRectMakeWithDictionaryRepresentation", &cg_rect_from_dict);
    // color spaces
    r("_CGColorSpaceCreateDeviceRGB", &cg_colorspace_rgb);
    r("_CGColorSpaceCreateDeviceGray", &cg_colorspace_gray);
    r("_CGColorSpaceCreateWithName", +[](id) -> void* { return reinterpret_cast<void*>(1); });
    r("_CGColorSpaceRelease", &cg_release_noop);
    r("_CGColorSpaceRetain", +[](void* c) -> void* { return c; });
    r("_CGColorSpaceGetModel", &cg_colorspace_model);
    r("_CGColorSpaceGetNumberOfComponents", &cg_colorspace_components);
    // bitmap contexts
    r("_CGBitmapContextCreate", &cg_bitmap_context_create);
    r("_CGBitmapContextCreateImage", &cg_bitmap_context_create_image);
    r("_CGBitmapContextGetData", &cg_bitmap_context_get_data);
    r("_CGBitmapContextGetWidth", &cg_bitmap_context_get_width);
    r("_CGBitmapContextGetHeight", &cg_bitmap_context_get_height);
    r("_CGBitmapContextGetBytesPerRow", &cg_bitmap_context_get_bpr);
    r("_CGContextRelease", &cg_release_noop);
    r("_CGContextRetain", &cg_retain_id);
    r("_CGContextDrawImage", &cg_draw_image);
    // images
    r("_CGImageRelease", +[](id img) { release(img); });   // the Create functions and CGImageRetain return +1
    r("_CGImageRetain", &cg_retain_id);
    r("_CGImageCreateCopy", &cg_image_create_copy);
    r("_CGImageCreateWithImageInRect", &cg_image_create_with_image_in_rect);
    r("_CGImageCreateWithPNGDataProvider", &cg_image_from_png);
    r("_CGImageCreateWithJPEGDataProvider", &cg_image_from_jpeg);
    r("_CGImageGetWidth", &cg_image_get_width);
    r("_CGImageGetHeight", &cg_image_get_height);
    r("_CGImageGetBitsPerComponent", &cg_image_get_bpc);
    r("_CGImageGetBitsPerPixel", &cg_image_get_bpp);
    r("_CGImageGetBytesPerRow", &cg_image_get_bpr);
    r("_CGImageGetColorSpace", &cg_image_get_colorspace);
    r("_CGImageGetAlphaInfo", &cg_image_get_alpha);
    r("_CGImageGetBitmapInfo", &cg_image_get_bitmapinfo);
    r("_CGImageIsMask", +[](id) -> bool { return false; });
    // data providers
    r("_CGDataProviderCreateWithData", &cg_provider_with_data);
    r("_CGDataProviderCreateWithCFData", &cg_provider_with_cfdata);
    r("_CGDataProviderCreateWithFilename", &cg_provider_with_file);
    r("_CGDataProviderRelease", +[](id provider) { release(provider); });
    r("_CGDataProviderRetain", &cg_retain_id);
    // colors / fonts / gradients: inert handles (the game's own GL path does the real drawing)
    r("_CGColorCreate", &cg_color_create);
    r("_CGColorCreateGenericRGB", &cg_color_create_generic_rgb);
    r("_CGColorGetComponents", &cg_color_get_components);
    r("_CGColorGetNumberOfComponents", &cg_color_get_count);
    r("_CGColorRelease", &cg_release_noop);
    r("_CGColorRetain", +[](void* c) -> void* { return c; });
    r("_CGFontCreateWithFontName", &cg_font_create);
    r("_CGFontCreateWithDataProvider", +[](id) -> void* { return reinterpret_cast<void*>(0x5); });
    r("_CGFontRelease", &cg_release_noop);
    r("_CGFontCopyTableForTag", +[](void*, uint32_t) -> id { return nullptr; });
    r("_CGFontGetNumberOfGlyphs", +[](void*) -> size_t { return 0; });
    r("_CGGradientCreateWithColors", +[](void*, id, const double*) -> void* { return reinterpret_cast<void*>(0x7); });
    r("_CGGradientCreateWithColorComponents", +[](void*, const double*, const double*, size_t) -> void* { return reinterpret_cast<void*>(0x7); });
    r("_CGGradientRelease", &cg_release_noop);
    // context drawing calls the game makes into an offscreen or UI context: accepted and ignored
    for (const char* n : {"_CGContextSaveGState", "_CGContextRestoreGState", "_CGContextBeginPath", "_CGContextClosePath", "_CGContextFillPath",
                          "_CGContextStrokePath", "_CGContextClip", "_CGContextEOClip", "_CGContextFlush", "_CGContextSynchronize"})
        r(n, &cg_noop);
    for (const char* n : {"_CGContextSetFillColorWithColor", "_CGContextSetStrokeColorWithColor", "_CGContextSetFillColor", "_CGContextSetStrokeColor",
                          "_CGContextSetRGBFillColor", "_CGContextSetRGBStrokeColor", "_CGContextSetGrayFillColor", "_CGContextSetGrayStrokeColor",
                          "_CGContextSetLineWidth", "_CGContextSetLineCap", "_CGContextSetLineJoin", "_CGContextSetAlpha", "_CGContextSetBlendMode",
                          "_CGContextMoveToPoint", "_CGContextAddLineToPoint", "_CGContextAddRect", "_CGContextAddEllipseInRect", "_CGContextFillRect",
                          "_CGContextStrokeRect", "_CGContextClearRect", "_CGContextClipToRect", "_CGContextTranslateCTM", "_CGContextScaleCTM",
                          "_CGContextRotateCTM", "_CGContextConcatCTM", "_CGContextSetShadow", "_CGContextSetShadowWithColor", "_CGContextAddPath",
                          "_CGContextAddArc", "_CGContextAddCurveToPoint", "_CGContextDrawLinearGradient", "_CGContextDrawRadialGradient",
                          "_CGContextSetTextMatrix", "_CGContextSetTextPosition", "_CGContextSetInterpolationQuality", "_CGContextSetShouldAntialias",
                          "_CGContextSetFont", "_CGContextSetFontSize", "_CGContextFillEllipseInRect", "_CGContextStrokeEllipseInRect", "_CGContextSetLineDash",
                          "_CGContextDrawTiledImage", "_CGContextBeginTransparencyLayer", "_CGContextEndTransparencyLayer",
                          "_CGContextAddArcToPoint", "_CGContextAddLines", "_CGContextDrawPath", "_CGContextSetStrokeColorSpace",
                          "_CGContextSetFillColorSpace", "_CGContextStrokeLineSegments", "_CGContextAddQuadCurveToPoint", "_CGContextStrokeRectWithWidth"})
        r(n, reinterpret_cast<void (*)()>(&cg_context_noop));
    r("_CGContextGetCTM", +[](id) -> CGAffineTransform { return {1, 0, 0, 1, 0, 0}; });
    r("_CGContextClearRect", &cg_clear_rect);
    r("_CGContextSetFillColorWithColor", &cg_set_fill_color);
    r("_CGContextSetRGBFillColor", &cg_set_rgb_fill);
    r("_CGContextSetGrayFillColor", &cg_set_gray_fill);
    r("_CGContextSetTextPosition", &cg_set_text_position);
    r("_CGContextGetTextPosition", &cg_get_text_position);
    r("_CGContextRelease", +[](id ctx) { release(ctx); });
    r("_CTFontCreateUIFontForLanguage", &ct_font_create_ui);
    r("_CTFontCreateWithName", &ct_font_create_with_name);
    r("_CTFontGetSize", &ct_font_get_size);
    r("_CTLineCreateWithAttributedString", &ct_line_create);
    r("_CTLineGetTypographicBounds", &ct_line_typographic_bounds);
    r("_CTLineDraw", &ct_line_draw);
    r("_CGPathCreateMutable", +[]() -> void* { return reinterpret_cast<void*>(0x9); });
    r("_CGPathCreateWithRect", +[](CGRect, const void*) -> void* { return reinterpret_cast<void*>(0x9); });
    r("_CGPathCreateWithEllipseInRect", +[](CGRect, const void*) -> void* { return reinterpret_cast<void*>(0x9); });
    r("_CGPathRelease", &cg_release_noop);
    r("_CGPathRetain", +[](void* p) -> void* { return p; });
    for (const char* n : {"_CGPathMoveToPoint", "_CGPathAddLineToPoint", "_CGPathAddRect", "_CGPathCloseSubpath", "_CGPathAddArc", "_CGPathAddCurveToPoint",
                          "_CGPathAddRoundedRect", "_CGPathAddEllipseInRect", "_CGPathAddArcToPoint", "_CGPathAddPath", "_CGPathAddLines",
                          "_CGPathAddQuadCurveToPoint", "_CGPathAddRects"})
        r(n, reinterpret_cast<void (*)()>(&cg_context_noop));
    r("_CGPathIsEmpty", +[](void*) -> bool { return true; });
    // CGRectDivide: cut `amount` off the given edge (MinX, MinY, MaxX, MaxY) into slice; the rest is remainder
    r("_CGRectDivide", +[](CGRect rect, CGRect* slice, CGRect* rem, double amount, uint32_t edge) {
        if (rect.size.width < 0) { rect.origin.x += rect.size.width; rect.size.width = -rect.size.width; }
        if (rect.size.height < 0) { rect.origin.y += rect.size.height; rect.size.height = -rect.size.height; }
        bool horizontal = edge == 0 || edge == 2;
        double dim = horizontal ? rect.size.width : rect.size.height;
        amount = std::max(0.0, std::min(amount, dim));
        CGRect a = rect, b = rect;
        if (horizontal) {
            a.size.width = amount;
            b.size.width = dim - amount;
            if (edge == 0) b.origin.x += amount; else a.origin.x += dim - amount;
        } else {
            a.size.height = amount;
            b.size.height = dim - amount;
            if (edge == 1) b.origin.y += amount; else a.origin.y += dim - amount;
        }
        if (slice) *slice = a;
        if (rem) *rem = b;
    });
    // UIGraphics
    static id ctx_stack_key = nullptr;
    (void)ctx_stack_key;
    r("_UIGraphicsBeginImageContext", +[](CGSize) {});
    r("_UIGraphicsBeginImageContextWithOptions", +[](CGSize, bool, double) {});
    r("_UIGraphicsEndImageContext", +[]() {});
    r("_UIGraphicsGetCurrentContext", +[]() -> id { return nullptr; });
    r("_UIGraphicsGetImageFromCurrentImageContext", +[]() -> id { return nullptr; });
    r("_UIGraphicsPushContext", +[](id) {});
    r("_UIGraphicsPopContext", +[]() {});
    r("_UIImagePNGRepresentation", +[](id) -> id { return nullptr; });
    r("_UIImageJPEGRepresentation", +[](id, double) -> id { return nullptr; });
    r("_UIRectFill", +[](CGRect) {});
    r("_UIRectFrame", +[](CGRect) {});
    r("_UIRectClip", +[](CGRect) {});
    // CGColorSpace/geometry global constants
    static CGRect zero_rect{{0, 0}, {0, 0}};
    static CGRect null_rect{{INFINITY, INFINITY}, {0, 0}};
    static CGRect infinite_rect{{-1e38, -1e38}, {2e38, 2e38}};
    static CGPoint zero_point{0, 0};
    static CGSize zero_size{0, 0};
    static CGAffineTransform ident{1, 0, 0, 1, 0, 0};
    m["_CGRectZero"] = &zero_rect;
    m["_CGRectNull"] = &null_rect;
    m["_CGRectInfinite"] = &infinite_rect;
    m["_CGPointZero"] = &zero_point;
    m["_CGSizeZero"] = &zero_size;
    m["_CGAffineTransformIdentity"] = &ident;
    for (const char* n : {"kCGColorSpaceSRGB", "kCGColorSpaceGenericRGB", "kCGColorSpaceGenericGray", "kCGColorSpaceGenericRGBLinear"})
        m[std::string("_") + n] = cg_const(n);
}

}  // namespace app
