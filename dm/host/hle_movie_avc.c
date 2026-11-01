// H.264 (AVC) decoding for the movie decoder (hle_movie.c), on Apple's VideoToolbox (the hardware decoder on the Mac and the iPhone) and on Android's MediaCodec.
// Super Mario Odyssey's 44 movies are MP4 / H.264 (the hint videos in its tutorial boxes, the kingdom travel videos): 41 High or Main profile with B-frames, 3 Baseline.
// The SDK's demuxer hands over one access unit per packet with Annex B start codes; SPS / PPS come in Configure's csd-0 / csd-1 (as with Android's MediaCodec) and may also appear in the stream.
// VideoToolbox wants a format description built from SPS / PPS and NAL units with 4-byte length prefixes. Decoding is synchronous and yields pictures in decode order; MediaCodec yields them
// in presentation order, so pictures wait in a queue sorted by presentation time and leave it once more than `depth` are queued, or all at the end of the stream.
// SPS pic_order_cnt_type 2 has decode order equal to output order: even High profile needs no reorder queue in that case
#include "hle.h"
#if !defined(__APPLE__) && !defined(__ANDROID__)
// No H.264 decoder on this platform: decoding fails, so the movie decoder reports the error and the game goes on without the pictures
#include "hle_movie_avc.h"
struct mavc { int red; };
mavc *mavc_new(void) { mavc *a = calloc(1, sizeof *a); if (!a) die("movie: AVC decoder allocation failed"); return a; }
void mavc_free(mavc *a) { free(a); }
void mavc_param_sets(mavc *a, const uint8_t *p, size_t n) { (void)a; (void)p; (void)n; }
void mavc_avcc(mavc *a, const uint8_t *p, size_t n) { (void)a; (void)p; (void)n; }
int mavc_decode(mavc *a, const uint8_t *p, int n, int64_t pts_us) {
    (void)p; (void)n; (void)pts_us;
    if (!a->red++) LOG("[RED] movie: H.264 is not decoded on Android yet");
    return -1;
}
int mavc_take(mavc *a, int flush, uint8_t *nv12, int w, int h, int64_t *pts_us) { (void)a; (void)flush; (void)nv12; (void)w; (void)h; (void)pts_us; return 0; }
int mavc_queued(const mavc *a) { (void)a; return 0; }
void mavc_set_rgba(mavc *a) { (void)a; }
void mavc_drop(mavc *a) { (void)a; }
#else
#include "hle_movie_avc.h"
#define QMAX 16
#ifdef __APPLE__
#include <VideoToolbox/VideoToolbox.h>
#include <arm_neon.h>
struct mavc {
    uint8_t sps[512], pps[512]; int nsps, npps, dirty;
    CMVideoFormatDescriptionRef fd; VTDecompressionSessionRef vs;
    struct { CVPixelBufferRef pb; int64_t pts; } q[QMAX]; int nq, depth;
    uint8_t *au; size_t aucap;   // length-prefixed copy of the access unit being decoded
    int red, rgba;               // rgba: pictures as 4-byte RGBA (native texture mode, Monster Hunter Rise) instead of NV12
};
#define AU_ANNEXB 0
static void close_session(mavc *a) {
    if (a->vs) { VTDecompressionSessionWaitForAsynchronousFrames(a->vs); VTDecompressionSessionInvalidate(a->vs); CFRelease(a->vs); a->vs = NULL; }
    if (a->fd) { CFRelease(a->fd); a->fd = NULL; }
}
void mavc_drop(mavc *a) { for (int i = 0; i < a->nq; i++) CVPixelBufferRelease(a->q[i].pb); a->nq = 0; }
#else
// Android: the hardware decoder through MediaCodec (NDK), synchronous, with byte-buffer output in a flexible YUV 4:2:0 layout (planar or semi-planar, read from the output
// format); access units go in with Annex B start codes and SPS / PPS as csd-0 / csd-1. MediaCodec hands pictures out in presentation order, converted at once into
// NV12 or RGBA copies of the decoded size; the end of the stream is sent when every queued picture is asked for
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <unistd.h>
struct mavc {
    uint8_t sps[512], pps[512]; int nsps, npps, dirty;
    AMediaCodec *mc; int cf, ow, oh, ostride, oslice, eos_in, eos_out;   // output colour format, size, row stride and plane height
    struct { uint8_t *px; int w, h; int64_t pts; } q[QMAX]; int nq, depth;
    uint8_t *au; size_t aucap;   // Annex B copy of the access unit being decoded
    int red, rgba;
};
#define AU_ANNEXB 1
static void close_session(mavc *a) { if (a->mc) { AMediaCodec_stop(a->mc); AMediaCodec_delete(a->mc); a->mc = NULL; } a->eos_in = a->eos_out = 0; }
void mavc_drop(mavc *a) { for (int i = 0; i < a->nq; i++) free(a->q[i].px); a->nq = 0; if (a->mc) { AMediaCodec_flush(a->mc); a->eos_in = a->eos_out = 0; } }
#endif
mavc *mavc_new(void) { mavc *a = calloc(1, sizeof *a); if (!a) die("movie: AVC decoder allocation failed"); return a; }
void mavc_free(mavc *a) { if (!a) return; close_session(a); mavc_drop(a); free(a->au); free(a); }
int mavc_queued(const mavc *a) { return a->nq; }

// Read only the SPS prefix through pic_order_cnt_type (H.264 7.4.2.1.1, 8.2.1.3).
// CRI Sofdec2 waits for every picture; holding four merely because profile_idc
// is High made the final picture arrive with EOS, which its player discards.
// Other POC modes keep the existing depth; a truncated/unsupported SPS does too.
typedef struct { uint8_t rbsp[512]; size_t n, bit; int bad; } avc_bits;
static unsigned avc_u(avc_bits *b, unsigned n) {
    if (n > 32 || b->bit + n > b->n * 8) { b->bad = 1; return 0; }
    unsigned v = 0;
    while (n--) { v = (v << 1) | ((b->rbsp[b->bit / 8] >> (7 - b->bit % 8)) & 1); b->bit++; }
    return v;
}
static unsigned avc_ue(avc_bits *b) {
    unsigned z = 0;
    while (!b->bad && !avc_u(b, 1)) if (++z > 30) { b->bad = 1; return 0; }
    return b->bad ? 0 : ((1u << z) - 1) + avc_u(b, z);
}
static int avc_reorder_depth(const uint8_t *nal, size_t n) {
    int fallback = n > 1 && nal[1] == 66 ? 0 : 4;
    if (n < 4 || n > 512 || (nal[0] & 31) != 7) return fallback;
    avc_bits b = {0};
    for (size_t i = 1; i < n; i++) {
        if (i >= 3 && nal[i] == 3 && !nal[i - 1] && !nal[i - 2]) continue; // emulation_prevention_three_byte
        b.rbsp[b.n++] = nal[i];
    }
    unsigned profile = avc_u(&b, 8); avc_u(&b, 16); // constraints, level_idc
    if (avc_ue(&b) > 31) return fallback; // seq_parameter_set_id
    switch (profile) {
    case 44: case 83: case 86: case 100: case 110: case 118: case 122:
    case 128: case 134: case 135: case 138: case 139: case 144: case 244: {
        unsigned chroma = avc_ue(&b);
        if (chroma > 3) return fallback;
        if (chroma == 3) avc_u(&b, 1); // separate_colour_plane_flag
        avc_ue(&b); avc_ue(&b); avc_u(&b, 1); // bit depths, transform bypass
        if (avc_u(&b, 1)) for (unsigned i = 0; i < (chroma == 3 ? 12u : 8u) && !b.bad; i++) {
            if (!avc_u(&b, 1)) continue;
            int last = 8, next = 8;
            for (unsigned j = 0; j < (i < 6 ? 16u : 64u) && !b.bad; j++) {
                if (next) { unsigned code = avc_ue(&b); int delta = code & 1 ? (int)((code + 1) / 2) : -(int)(code / 2); next = (last + delta) & 255; }
                if (next) last = next;
            }
        }
        break;
    }
    case 66: case 77: case 88: break;
    default: return fallback;
    }
    if (avc_ue(&b) > 12) return fallback; // log2_max_frame_num_minus4
    unsigned poc = avc_ue(&b);
    return !b.bad && poc == 2 ? 0 : fallback;
}

static void keep_ps(mavc *a, const uint8_t *nal, size_t n) {   // a parameter set NAL unit (without start code)
    int t = nal[0] & 31; uint8_t *dst = t == 7 ? a->sps : a->pps; int *dn = t == 7 ? &a->nsps : &a->npps;
    if (n > sizeof a->sps) { if (!a->red++) LOG("[RED] movie: AVC parameter set of %zu bytes, too long", n); return; }
    if ((size_t)*dn == n && !memcmp(dst, nal, n)) return;
    memcpy(dst, nal, n); *dn = (int)n; a->dirty = 1;
    if (t == 7) a->depth = avc_reorder_depth(nal, n);
}
// walk the NAL units of p (Annex B start codes, or else 4-byte lengths); fn gets each unit without its prefix. Zero bytes before a start code (the first byte of a 4-byte
// start code, trailing_zero_8bits) are not part of the unit
static size_t next_sc(const uint8_t *p, size_t n, size_t i) { for (; i + 3 <= n; i++) if (!p[i] && !p[i + 1] && p[i + 2] == 1) return i; return n; }
static void each_nal(const uint8_t *p, size_t n, void (*fn)(void *, const uint8_t *, size_t), void *ctx) {
    size_t s = next_sc(p, n, 0);
    if (s < n && s <= 1) {
        while (s < n) {
            size_t b = s + 3, e = next_sc(p, n, b), end = e;
            while (end > b && !p[end - 1]) end--;
            if (end > b) fn(ctx, p + b, end - b);
            s = e;
        }
    } else for (size_t i = 0; i + 4 <= n;) {
        size_t len = (size_t)p[i] << 24 | (size_t)p[i + 1] << 16 | (size_t)p[i + 2] << 8 | p[i + 3]; i += 4;
        if (!len || len > n - i) break;
        fn(ctx, p + i, len); i += len;
    }
}
static void ps_only(void *ctx, const uint8_t *nal, size_t n) { int t = nal[0] & 31; if (t == 7 || t == 8) keep_ps(ctx, nal, n); }
void mavc_param_sets(mavc *a, const uint8_t *p, size_t n) { each_nal(p, n, ps_only, a); }
// AVCDecoderConfigurationRecord: version, profile, compatibility, level, NAL length size - 1, SPS count (low 5 bits) and 16-bit-length SPS, PPS count and 16-bit-length PPS
void mavc_avcc(mavc *a, const uint8_t *p, size_t n) {
    if (n < 7 || p[0] != 1) { if (!a->red++) LOG("[RED] movie: unrecognized avcC record (%zu bytes, version %d)", n, n ? p[0] : -1); return; }
    size_t i = 5;
    for (int set = 0; set < 2 && i < n; set++) {
        int cnt = set ? p[i] : p[i] & 31; i++;
        for (int k = 0; k < cnt && i + 2 <= n; k++) { size_t len = (size_t)p[i] << 8 | p[i + 1]; i += 2; if (len > n - i) return; if (len) keep_ps(a, p + i, len); i += len; }
    }
}

typedef struct { mavc *a; size_t len; } au_ctx;
static void au_add(void *ctx, const uint8_t *nal, size_t n) {
    au_ctx *c = ctx; mavc *a = c->a; int t = nal[0] & 31;
    if (t == 7 || t == 8) { keep_ps(a, nal, n); return; }
    if (t == 9) return;   // access unit delimiter
    if (c->len + 4 + n > a->aucap) { a->aucap = (c->len + 4 + n) * 2; a->au = realloc(a->au, a->aucap); if (!a->au) die("movie: AVC buffer allocation failed"); }
    uint8_t *o = a->au + c->len;
    if (AU_ANNEXB) { o[0] = o[1] = o[2] = 0; o[3] = 1; } else { o[0] = (uint8_t)(n >> 24); o[1] = (uint8_t)(n >> 16); o[2] = (uint8_t)(n >> 8); o[3] = (uint8_t)n; }
    memcpy(o + 4, nal, n); c->len += 4 + n;
}
#ifdef __APPLE__
static void on_frame(void *ref, void *src, OSStatus st, VTDecodeInfoFlags fl, CVImageBufferRef img, CMTime pts, CMTime dur) {
    (void)src; (void)fl; (void)dur; mavc *a = ref;
    if (st || !img) { if (!a->red++) LOG("[RED] movie: AVC decode error (VideoToolbox %d, first case)", (int)st); return; }
    if (a->nq == QMAX) { if (!a->red++) LOG("[RED] movie: AVC reorder queue full, dropping the oldest frame"); CVPixelBufferRelease(a->q[0].pb); memmove(a->q, a->q + 1, --a->nq * sizeof a->q[0]); }
    int64_t t = pts.timescale ? pts.value * 1000000 / pts.timescale : 0;
    int k = a->nq; while (k > 0 && a->q[k - 1].pts > t) { a->q[k] = a->q[k - 1]; k--; }
    a->q[k].pb = CVPixelBufferRetain(img); a->q[k].pts = t; a->nq++;
}
static int open_session(mavc *a) {
    close_session(a);
    if (!a->nsps || !a->npps) { if (!a->red++) LOG("[RED] movie: AVC has no SPS / PPS, outputting black frames"); return 0; }
    const uint8_t *ps[2] = {a->sps, a->pps}; size_t sz[2] = {(size_t)a->nsps, (size_t)a->npps};
    OSStatus st = CMVideoFormatDescriptionCreateFromH264ParameterSets(NULL, 2, ps, sz, 4, &a->fd);
    if (st) { if (!a->red++) LOG("[RED] movie: AVC parameter sets give no format description (%d), outputting black frames", (int)st); return 0; }
    int32_t pf = a->rgba ? kCVPixelFormatType_32BGRA : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;   // NV12, video range: what the SDK's decoder hands out; BGRA (converted by VideoToolbox with the stream's colour matrix) for RGBA output
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberSInt32Type, &pf);
    const void *k[] = {kCVPixelBufferPixelFormatTypeKey}, *v[] = {n};
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, k, v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks); CFRelease(n);
    VTDecompressionOutputCallbackRecord cb = {on_frame, a};
    st = VTDecompressionSessionCreate(NULL, a->fd, NULL, attrs, &cb, &a->vs); CFRelease(attrs);
    if (st) { if (!a->red++) LOG("[RED] movie: VideoToolbox cannot create an AVC decode session (%d), outputting black frames", (int)st); a->vs = NULL; return 0; }
    CMVideoDimensions dm = CMVideoFormatDescriptionGetDimensions(a->fd);
    LOG("[movie] AVC: VideoToolbox decoding %d×%d, profile %d, reorder depth %d", dm.width, dm.height, a->nsps > 1 ? a->sps[1] : 0, a->depth);
    a->dirty = 0; return 1;
}
int mavc_decode(mavc *a, const uint8_t *p, int n, int64_t pts_us) {
    au_ctx c = {a, 0}; each_nal(p, (size_t)n, au_add, &c);
    if (!c.len) return 0;
    if ((a->dirty || !a->vs) && !open_session(a)) return -1;
    CMBlockBufferRef bb = NULL; CMSampleBufferRef sb = NULL;
    OSStatus st = CMBlockBufferCreateWithMemoryBlock(NULL, NULL, c.len, NULL, NULL, 0, c.len, 0, &bb);
    if (!st) st = CMBlockBufferReplaceDataBytes(a->au, bb, 0, c.len);
    CMSampleTimingInfo ti = {kCMTimeInvalid, CMTimeMake(pts_us, 1000000), kCMTimeInvalid}; size_t ssz = c.len;
    if (!st) st = CMSampleBufferCreateReady(NULL, bb, a->fd, 1, 1, &ti, 1, &ssz, &sb);
    if (!st) st = VTDecompressionSessionDecodeFrame(a->vs, sb, 0, NULL, NULL);   // flags 0: synchronous, the picture is queued before this returns
    if (sb) CFRelease(sb); if (bb) CFRelease(bb);
    if (st) { if (!a->red++) LOG("[RED] movie: AVC decode submission failed (%d, first case)", (int)st); return -1; }
    return 0;
}
void mavc_set_rgba(mavc *a) { if (!a->rgba) { a->rgba = 1; a->dirty = 1; } }
// BGRA picture → RGBA rows of w pixels (stride w * 4); a smaller picture is padded with opaque black, a larger one cropped
static void take_rgba(CVPixelBufferRef pb, uint8_t *dst, int w, int h) {
    int pw = (int)CVPixelBufferGetWidth(pb), ph = (int)CVPixelBufferGetHeight(pb), cw = pw < w ? pw : w, chh = ph < h ? ph : h;
    const uint8_t *src = CVPixelBufferGetBaseAddress(pb); size_t ss = CVPixelBufferGetBytesPerRow(pb);
    if (cw < w || chh < h) for (size_t i = 0; i < (size_t)w * h; i++) ((uint32_t *)dst)[i] = 0xff000000u;
    for (int r = 0; r < chh; r++) {
        const uint8_t *s = src + r * ss; uint8_t *o = dst + (size_t)r * w * 4; int x = 0;
        for (; x + 16 <= cw; x += 16) { uint8x16x4_t v = vld4q_u8(s + 4 * x); uint8x16_t t = v.val[0]; v.val[0] = v.val[2]; v.val[2] = t; vst4q_u8(o + 4 * x, v); }
        for (; x < cw; x++) { o[4 * x] = s[4 * x + 2]; o[4 * x + 1] = s[4 * x + 1]; o[4 * x + 2] = s[4 * x]; o[4 * x + 3] = 255; }
    }
}
int mavc_take(mavc *a, int flush, uint8_t *dst, int w, int h, int64_t *pts_us) {
    if (!a->nq || (!flush && a->nq <= a->depth)) return 0;
    CVPixelBufferRef pb = a->q[0].pb; if (pts_us) *pts_us = a->q[0].pts;
    memmove(a->q, a->q + 1, --a->nq * sizeof a->q[0]);
    CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    if (a->rgba) { take_rgba(pb, dst, w, h); CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly); CVPixelBufferRelease(pb); return 1; }
    int pw = (int)CVPixelBufferGetWidthOfPlane(pb, 0), ph = (int)CVPixelBufferGetHeightOfPlane(pb, 0);
    const uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(pb, 0), *uv = CVPixelBufferGetBaseAddressOfPlane(pb, 1);
    size_t ys = CVPixelBufferGetBytesPerRowOfPlane(pb, 0), uvs = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
    int cw = pw < w ? pw : w, chh = ph < h ? ph : h;
    if (cw < w || chh < h) { memset(dst, 16, (size_t)w * h); memset(dst + (size_t)w * h, 128, (size_t)w * h / 2); }
    for (int r = 0; r < chh; r++) memcpy(dst + (size_t)r * w, y + r * ys, (size_t)cw);
    size_t uvb = (size_t)((cw + 1) & ~1); if (uvb > (size_t)w) uvb = (size_t)w;   // interleaved U V: one byte per luma column
    for (int r = 0; r < (chh + 1) / 2 && r < h / 2; r++) memcpy(dst + (size_t)w * h + (size_t)r * w, uv + r * uvs, uvb);
    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    CVPixelBufferRelease(pb);
    return 1;
}
#else   // Android
static int open_session(mavc *a) {
    close_session(a);
    if (!a->nsps || !a->npps) { if (!a->red++) LOG("[RED] movie: AVC has no SPS / PPS, outputting black frames"); return 0; }
    a->mc = AMediaCodec_createDecoderByType("video/avc");
    if (!a->mc) { if (!a->red++) LOG("[RED] movie: no H.264 decoder on this device (MediaCodec), outputting black frames"); return 0; }
    uint8_t c0[516] = {0, 0, 0, 1}, c1[516] = {0, 0, 0, 1}; memcpy(c0 + 4, a->sps, a->nsps); memcpy(c1 + 4, a->pps, a->npps);
    AMediaFormat *f = AMediaFormat_new();
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, 1920); AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, 1080);   // upper bounds: the real size comes from the SPS
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, 0x7F420888);   // COLOR_FormatYUV420Flexible: a linear planar or semi-planar layout, not a vendor tiling
    AMediaFormat_setBuffer(f, "csd-0", c0, (size_t)a->nsps + 4); AMediaFormat_setBuffer(f, "csd-1", c1, (size_t)a->npps + 4);
    media_status_t st = AMediaCodec_configure(a->mc, f, NULL, NULL, 0); AMediaFormat_delete(f);
    if (st == AMEDIA_OK) st = AMediaCodec_start(a->mc);
    if (st != AMEDIA_OK) { if (!a->red++) LOG("[RED] movie: MediaCodec cannot start the H.264 decoder (%d), outputting black frames", (int)st); close_session(a); return 0; }
    LOG("[movie] AVC: MediaCodec decoding, profile %d, level %d", a->nsps > 1 ? a->sps[1] : 0, a->nsps > 3 ? a->sps[3] : 0);
    a->cf = 0; a->ow = a->oh = 0; a->dirty = 0; return 1;
}
static void out_format(mavc *a) {
    AMediaFormat *f = AMediaCodec_getOutputFormat(a->mc); int32_t v;
    a->cf = AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, &v) ? v : 21;
    a->ow = AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_WIDTH, &v) ? v : 0; a->oh = AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_HEIGHT, &v) ? v : 0;
    int32_t l, t, r, b;   // the picture inside the buffer (the buffer may be aligned past it)
    if (AMediaFormat_getRect(f, "crop", &l, &t, &r, &b)) { a->ow = r - l + 1; a->oh = b - t + 1; }
    a->ostride = AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_STRIDE, &v) && v > 0 ? v : a->ow;
    a->oslice = AMediaFormat_getInt32(f, "slice-height", &v) && v > 0 ? v : a->oh;
    if (a->ostride < a->ow) a->ostride = a->ow;
    if (a->oslice < a->oh) a->oslice = a->oh;
    LOG("[movie] AVC: output %dx%d, colour format %#x, stride %d, plane height %d", a->ow, a->oh, a->cf, a->ostride, a->oslice);
    AMediaFormat_delete(f);
}
static inline uint8_t clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }
// one decoded picture → a copy of its own size, NV12 (stride w) or RGBA (BT.709 limited range from 720 lines up, BT.601 below, as SD and HD video are coded)
static void keep_picture(mavc *a, const uint8_t *buf, size_t n, int64_t pts) {
    int w = a->ow, h = a->oh, planar = a->cf == 19;   // 19 COLOR_FormatYUV420Planar (I420), 21 SemiPlanar (NV12); the flexible format itself is read as NV12
    if (a->cf != 19 && a->cf != 21 && a->cf != 0x7F420888) { if (!a->red++) LOG("[RED] movie: MediaCodec colour format %#x is not a YUV 4:2:0 layout read here, the picture is dropped", a->cf); return; }
    size_t ys = (size_t)a->ostride, yplane = ys * (size_t)a->oslice, cs = planar ? ys / 2 : ys;
    // the last byte read: the last chroma row's last V sample (semi-planar: U V pairs from the U plane's start, V one byte after U)
    size_t voff = planar ? yplane + cs * (size_t)(a->oslice / 2) : yplane + 1, need = h ? voff + cs * (size_t)((h + 1) / 2 - 1) + (planar ? (size_t)(w + 1) / 2 : (size_t)(w - 1)) : 0;
    if (!w || !h || n < need) { if (!a->red++) LOG("[RED] movie: MediaCodec output buffer of %zu bytes is short for %dx%d", n, w, h); return; }
    const uint8_t *Y = buf, *U = buf + yplane, *V = buf + voff;
    int step = planar ? 1 : 2;
    uint8_t *px = malloc(a->rgba ? (size_t)w * h * 4 : (size_t)w * h * 3 / 2 + w); if (!px) return;
    if (a->rgba) {
        int hd = h >= 720, cr = hd ? 459 : 409, cgu = hd ? 55 : 100, cgv = hd ? 136 : 208, cb = hd ? 541 : 516;   // ×256: 1.793 / 0.213 / 0.533 / 2.112 (709), 1.596 / 0.391 / 0.813 / 2.018 (601)
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            int yy = (Y[(size_t)y * ys + x] - 16) * 298, u = U[(size_t)(y / 2) * cs + (x / 2) * step] - 128, v = V[(size_t)(y / 2) * cs + (x / 2) * step] - 128;
            uint8_t *o = px + ((size_t)y * w + x) * 4;
            o[0] = clamp8((yy + cr * v + 128) >> 8); o[1] = clamp8((yy - cgu * u - cgv * v + 128) >> 8); o[2] = clamp8((yy + cb * u + 128) >> 8); o[3] = 255;
        }
    } else {
        for (int y = 0; y < h; y++) memcpy(px + (size_t)y * w, Y + (size_t)y * ys, (size_t)w);
        uint8_t *uv = px + (size_t)w * h;
        for (int y = 0; y < h / 2; y++) for (int x = 0; x < w / 2; x++) { uv[(size_t)y * w + 2 * x] = U[(size_t)y * cs + x * step]; uv[(size_t)y * w + 2 * x + 1] = V[(size_t)y * cs + x * step]; }
    }
    if (a->nq == QMAX) { if (!a->red++) LOG("[RED] movie: AVC picture queue full, dropping the oldest picture"); free(a->q[0].px); memmove(a->q, a->q + 1, --a->nq * sizeof a->q[0]); }
    int k = a->nq; while (k > 0 && a->q[k - 1].pts > pts) { a->q[k] = a->q[k - 1]; k--; }
    a->q[k].px = px; a->q[k].w = w; a->q[k].h = h; a->q[k].pts = pts; a->nq++;
}
// take what the decoder has finished; waits up to wait_us for the first picture
static void drain(mavc *a, int64_t wait_us) {
    for (;;) {
        AMediaCodecBufferInfo bi; ssize_t i = AMediaCodec_dequeueOutputBuffer(a->mc, &bi, wait_us);
        if (i == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) { out_format(a); continue; }
        if (i == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
        if (i < 0) return;
        if (!a->cf) out_format(a);
        size_t sz; uint8_t *b = AMediaCodec_getOutputBuffer(a->mc, (size_t)i, &sz);
        if (b && bi.size > 0) keep_picture(a, b + bi.offset, (size_t)bi.size, bi.presentationTimeUs);
        AMediaCodec_releaseOutputBuffer(a->mc, (size_t)i, false);
        if (bi.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) { a->eos_out = 1; return; }
        wait_us = 0;
    }
}
static int feed(mavc *a, const uint8_t *p, size_t n, int64_t pts_us, uint32_t flags) {
    for (int tries = 0; tries < 50; tries++) {   // up to half a second for a free input buffer, taking finished pictures meanwhile
        ssize_t i = AMediaCodec_dequeueInputBuffer(a->mc, 10000);
        if (i >= 0) {
            size_t cap; uint8_t *b = AMediaCodec_getInputBuffer(a->mc, (size_t)i, &cap);
            if (!b || cap < n) { if (!a->red++) LOG("[RED] movie: H.264 access unit of %zu bytes does not fit MediaCodec's %zu-byte buffer", n, cap); AMediaCodec_queueInputBuffer(a->mc, (size_t)i, 0, 0, pts_us, flags); return -1; }
            if (n) memcpy(b, p, n);
            return AMediaCodec_queueInputBuffer(a->mc, (size_t)i, 0, n, (uint64_t)pts_us, flags) == AMEDIA_OK ? 0 : -1;
        }
        drain(a, 0);
    }
    if (!a->red++) LOG("[RED] movie: MediaCodec took no input for half a second");
    return -1;
}
int mavc_decode(mavc *a, const uint8_t *p, int n, int64_t pts_us) {
    au_ctx c = {a, 0}; each_nal(p, (size_t)n, au_add, &c);
    if (!c.len) return 0;
    if ((a->dirty || !a->mc) && !open_session(a)) return -1;
    if (a->eos_in) { AMediaCodec_flush(a->mc); a->eos_in = a->eos_out = 0; }   // decoding again after the end of the stream (a looping movie)
    int r = feed(a, a->au, c.len, pts_us, 0);
    drain(a, 0);
    return r;
}
void mavc_set_rgba(mavc *a) { a->rgba = 1; }
int mavc_take(mavc *a, int flush, uint8_t *dst, int w, int h, int64_t *pts_us) {
    if (a->mc && !a->nq) drain(a, 0);   // what the hardware decoder has finished; a picture still on its way comes out at a later call (waiting here held up the game's frame: Super Mario Odyssey polls its hint videos every frame)
    if (flush && a->mc && !a->nq && !a->eos_out) {   // end of the stream: what the decoder still holds comes out after an end-of-stream input
        if (!a->eos_in) { a->eos_in = 1; feed(a, NULL, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM); }
        for (int i = 0; i < 25 && !a->nq && !a->eos_out; i++) drain(a, 20000);
    }
    if (!a->nq) return 0;
    uint8_t *px = a->q[0].px; int pw = a->q[0].w, ph = a->q[0].h; if (pts_us) *pts_us = a->q[0].pts;
    memmove(a->q, a->q + 1, --a->nq * sizeof a->q[0]);
    int cw = pw < w ? pw : w, chh = ph < h ? ph : h;
    if (a->rgba) {
        if (cw < w || chh < h) for (size_t i = 0; i < (size_t)w * h; i++) ((uint32_t *)dst)[i] = 0xff000000u;
        for (int r = 0; r < chh; r++) memcpy(dst + (size_t)r * w * 4, px + (size_t)r * pw * 4, (size_t)cw * 4);
    } else {
        if (cw < w || chh < h) { memset(dst, 16, (size_t)w * h); memset(dst + (size_t)w * h, 128, (size_t)w * h / 2); }
        for (int r = 0; r < chh; r++) memcpy(dst + (size_t)r * w, px + (size_t)r * pw, (size_t)cw);
        for (int r = 0; r < chh / 2 && r < h / 2; r++) memcpy(dst + (size_t)w * h + (size_t)r * w, px + (size_t)pw * ph + (size_t)r * pw, (size_t)(cw & ~1));
    }
    free(px);
    return 1;
}
#endif
#endif
