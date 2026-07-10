/* Copyright (C) 2026  Paige Julianne Sullivan
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/* ============================================================================
 * squish.c — libsquish implementation
 *
 * SQUISH is a context-mixing compressor. Per bit:
 *   1. Ten models predict P(next bit = 1):
 *        - order-0..order-6 byte contexts (hashed adaptive counters)
 *        - word model (hash of current alphanumeric word — text)
 *        - record model (auto-detected record length R: byte one row up,
 *          and column-position context — tables/spreadsheets/rasters)
 *        - match model (6-byte rolling hash -> longest recent repeat)
 *   2. A logistic mixer (1024 context-selected weight sets, trained online
 *      by gradient descent) fuses the predictions in the log-odds domain.
 *   3. Two chained APM/SSE stages recalibrate the mixed probability.
 *   4. A carryless 32-bit binary arithmetic coder codes the bit.
 * Decompression runs the identical model pipeline in lockstep.
 *
 * Container format "SQ02" (see docs/FORMAT.md):
 *   [0..3]  magic "SQ02"
 *   [4..11] original size, u64 little-endian
 *   [12]    mode: 0 = arithmetic-coded, 1 = stored (incompressible fallback)
 *   [13..]  payload
 *   [last4] FNV-1a 64 of the original data, folded to 32 bits, LE
 *
 * All state lives in a heap-allocated squish_ctx => no global mutable state,
 * safe for concurrent use from multiple threads.
 * ==========================================================================*/
#include "squish.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t  i32;
typedef int64_t  i64;

/* ---------------- tunables (fixed: they define the stream format) -------- */
#define TBITS     22            /* per-model hash table: 4M counters (16MB) */
#define TSIZE     (1u<<TBITS)
#define TMASK     (TSIZE-1)
#define MHBITS    22            /* match-model hash table */
#define MHSIZE    (1u<<MHBITS)
#define MHMASK    (MHSIZE-1)
#define NTAB      8             /* hashed models: o1 o2 o3 o4 o6 word recA recB */
#define NIN       11            /* mixer inputs: bias + o0 + 8 tables + match */
#define NSETS     1024          /* mixer weight sets */
#define MIX_SHIFT 11            /* mixer learning-rate shift */
#define WCLAMP    (1<<24)
#define CNT_CAP   255           /* counter adaptation cap */
#define MINMATCH  6
#define RECWIN    2048          /* record-length vote window */

#define HDR_SIZE  13            /* magic + size + mode byte */
#define CKS_SIZE  4
#define OVERHEAD  (HDR_SIZE + CKS_SIZE)

static const u8 MAGIC[4] = { 'S', 'Q', '0', '2' };
enum { MODE_CM = 0, MODE_STORED = 1 };

/* multi-block container "SQ01" (see docs/FORMAT.md):
 *   [0..3]   magic "SQ01"
 *   [4..11]  total original size, u64 little-endian
 *   [12]     mode: 2 = multi-block
 *   [13..16] chunk count k, u32 little-endian
 *   [17..]   k * u32 LE: compressed size of each chunk
 *   then     k chunks, each a complete SQ02 stream (own checksum/fallback)
 * Chunks share no model state, so they compress and decompress in parallel. */
static const u8 MAGIC_MB[4] = { 'S', 'Q', '0', '1' };
enum { MODE_MB = 2 };
#define MB_HDR_SIZE   17
#define MB_MAX_CHUNKS 65536u    /* SQUISH_MAX_INPUT / SQUISH_MIN_CHUNK */

/* ---------------- context: every bit of mutable state -------------------- */
typedef struct squish_ctx {
    /* logistic tables (deterministic, built per context: no global state) */
    int   sq[4096];             /* squash: d(-2048..2047) -> p(0..4095) */
    short st[4096];             /* stretch: p(0..4095) -> d(-2047..2047) */
    u16   rate[CNT_CAP+1];      /* counter learning rates */

    /* models */
    u32  *tab[NTAB];            /* hashed counter tables */
    u32   o0[256];              /* order-0 direct table  */
    u32   msm[64];              /* match counters: bucket*2 + expected_bit */
    u32  *mt;                   /* match hash -> position */
    i32   w[NSETS*NIN];         /* mixer weights */
    u16  *apm1, *apm2;          /* SSE tables: 256*33 and 65536*33 */

    /* history */
    u8   *buf;                  /* processed bytes so far (window = whole file) */
    int   buf_write;            /* 0 when buf aliases the caller's src */
    u64   pos;
    u64   c8;                   /* last 8 bytes */
    u32   c0;                   /* partial byte with leading-1 sentinel */
    int   bitn;
    u32   b1;                   /* previous byte */

    u32   cxt[NTAB];
    int   have[NTAB];
    u32   hword;                /* rolling word hash */

    u32   lastpos[256];         /* record-length detection */
    u32   rvote[RECWIN+1];
    u32   R, rcd;

    u64   mptr; u32 mlen;       /* match model */
    int   mvalid; u8 mbyte;

    /* per-bit scratch */
    int   stin[NIN];
    u32   tidx[NTAB];
    int   msmidx;
    u32   mixsel;
    int   pmix;
    u32   a1idx, a2idx;

    /* arithmetic coder + buffer I/O */
    u32   x1, x2, x;
    const u8 *in; size_t in_pos, in_end;      /* decode source */
    size_t starved;                           /* filler bytes read past end */
    u8   *out; size_t out_pos, out_cap;       /* encode sink   */
    int   overflow;
} Ctx;

/* ---------------- small helpers ------------------------------------------ */
static inline int squash(const Ctx *S, int d) {
    if (d < -2047) d = -2047;
    if (d >  2047) d =  2047;
    return S->sq[d+2048];
}
static inline int cnt_p(u32 c) { return (int)(c >> 16); }
static inline u32 cnt_upd(const Ctx *S, u32 c, int y) {
    int p = (int)(c >> 16), n = (int)(c & 0xffff);
    p += ((y ? 65535 : 0) - p) * S->rate[n] >> 16;
    if (n < CNT_CAP) n++;
    return ((u32)p << 16) | (u32)n;
}
static inline u32 hh(u64 x, u64 salt) {
    x = (x + salt) * 0x9E3779B97F4A7C15ull;
    x ^= x >> 29; x *= 0xBF58476D1CE4E5B9ull; x ^= x >> 32;
    return (u32)x;
}
static u64 fnv1a64(const u8 *p, size_t n) {
    u64 h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}

/* ---------------- buffer-backed arithmetic coder ------------------------- */
static inline void sink_put(Ctx *S, u8 b) {
    if (S->out_pos < S->out_cap) S->out[S->out_pos++] = b;
    else S->overflow = 1;
}
static inline int src_get(Ctx *S) {           /* 255 past end, like EOF */
    if (S->in_pos < S->in_end) return S->in[S->in_pos++];
    S->starved++;
    return 255;
}
static void enc_init(Ctx *S) { S->x1 = 0; S->x2 = 0xffffffff; }
static inline void enc_bit(Ctx *S, int y, int p16) {
    u32 r = S->x2 - S->x1;
    u32 xmid = S->x1 + (r>>16)*(u32)p16 + (((r&0xffff)*(u32)p16)>>16);
    if (y) S->x2 = xmid; else S->x1 = xmid + 1;
    while (((S->x1 ^ S->x2) & 0xff000000u) == 0) {
        sink_put(S, (u8)(S->x2 >> 24));
        S->x1 <<= 8; S->x2 = (S->x2 << 8) | 255;
    }
}
static void enc_flush(Ctx *S) {
    for (int i = 0; i < 4; i++) { sink_put(S, (u8)(S->x1 >> 24)); S->x1 <<= 8; }
}
static void dec_init(Ctx *S) {
    S->x1 = 0; S->x2 = 0xffffffff; S->x = 0;
    for (int i = 0; i < 4; i++) S->x = (S->x << 8) | (u32)src_get(S);
}
static inline int dec_bit(Ctx *S, int p16) {
    u32 r = S->x2 - S->x1;
    u32 xmid = S->x1 + (r>>16)*(u32)p16 + (((r&0xffff)*(u32)p16)>>16);
    int y = (S->x <= xmid);
    if (y) S->x2 = xmid; else S->x1 = xmid + 1;
    while (((S->x1 ^ S->x2) & 0xff000000u) == 0) {
        S->x1 <<= 8; S->x2 = (S->x2 << 8) | 255;
        S->x = (S->x << 8) | (u32)src_get(S);
    }
    return y;
}

/* ---------------- APM / SSE stage ---------------------------------------- */
static int apm_alloc(Ctx *S, u16 **t, u32 n) {
    *t = (u16*)malloc((size_t)n * 33 * sizeof(u16));
    if (!*t) return 0;
    for (u32 c = 0; c < n; c++)
        for (u32 j = 0; j < 33; j++)
            (*t)[c*33+j] = (u16)(squash(S, ((int)j - 16) * 128) << 4);
    return 1;
}
static inline int apm_pp(const Ctx *S, u16 *t, u32 cx, int pr, u32 *save) {
    int s = S->st[pr] + 2048;               /* 1..4095 */
    int j = s >> 7, wg = s & 127;
    u32 i = cx * 33 + (u32)j;
    *save = i + (u32)((wg >> 6) & 1);
    return (t[i]*(128-wg) + t[i+1]*wg) >> 11;
}
static inline void apm_upd(u16 *t, u32 idx, int y) {
    int v = t[idx];
    v += ((y ? 65535 : 0) - v) >> 7;
    t[idx] = (u16)v;
}

/* ---------------- predictor ---------------------------------------------- */
static void set_contexts(Ctx *S) {
    S->cxt[0] = hh(S->c8 & 0xffull,           0x11);
    S->cxt[1] = hh(S->c8 & 0xffffull,         0x22);
    S->cxt[2] = hh(S->c8 & 0xffffffull,       0x33);
    S->cxt[3] = hh(S->c8 & 0xffffffffull,     0x44);
    S->cxt[4] = hh(S->c8 & 0xffffffffffffull, 0x66);
    S->have[0]=S->have[1]=S->have[2]=S->have[3]=S->have[4]=1;
    if (S->hword) { S->cxt[5] = hh(S->hword, 0x77); S->have[5] = 1; }
    else S->have[5] = 0;
    if (S->R >= 2 && S->pos >= 2*(u64)S->R) {
        u8 up1 = S->buf[S->pos - S->R], up2 = S->buf[S->pos - 2*(u64)S->R];
        S->cxt[6] = hh(((u64)S->R << 8) | up1, 0x88);
        S->cxt[7] = hh(((u64)(S->pos % S->R) << 16) | ((u64)up1 << 8) | up2,
                       ((u64)S->R << 8) ^ 0x99);
        S->have[6] = S->have[7] = 1;
    } else S->have[6] = S->have[7] = 0;
    if (S->mlen) { S->mvalid = 1; S->mbyte = S->buf[S->mptr]; }
    else S->mvalid = 0;
    S->mixsel = ((S->mlen ? 1u : 0u) << 9) | ((S->R >= 2 ? 1u : 0u) << 8) | S->b1;
}

static void byte_update(Ctx *S, u8 b) {
    if (S->buf_write) S->buf[S->pos] = b;
    S->pos++;
    S->b1 = b;
    S->c8 = (S->c8 << 8) | b;

    /* word hash: letters, digits, non-ASCII continue a word */
    if ((b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') ||
        (b >= '0' && b <= '9') || b >= 128)
        S->hword = (S->hword + (u32)(b | 32) + 1) * 0x9E3779B1u;
    else S->hword = 0;

    /* record-length voting */
    {
        u32 d = (u32)(S->pos - S->lastpos[b]);
        if (d >= 2 && d <= RECWIN) S->rvote[d]++;
        S->lastpos[b] = (u32)S->pos;
        if (--S->rcd == 0) {
            S->rcd = RECWIN;
            u32 vmax = 0, dmax = 0;
            for (u32 d2 = 2; d2 <= RECWIN; d2++) {
                if (S->rvote[d2] > vmax) { vmax = S->rvote[d2]; dmax = d2; }
                S->rvote[d2] >>= 1;
            }
            if (vmax >= 600) S->R = dmax;
            else if (vmax < 300) S->R = 0;
        }
    }

    /* match model: extend or re-anchor */
    if (S->mlen) {
        if (S->buf[S->mptr] == b) { S->mptr++; if (S->mlen < 65535) S->mlen++; }
        else S->mlen = 0;
    }
    if (S->pos >= MINMATCH) {
        u32 mh = hh(S->c8 & 0xffffffffffffull, 0xABCD) & MHMASK;
        if (!S->mlen) {
            u32 cand = S->mt[mh];
            if (cand && cand < S->pos && cand >= 3 &&
                S->buf[cand-1] == S->buf[S->pos-1] &&
                S->buf[cand-2] == S->buf[S->pos-2] &&
                S->buf[cand-3] == S->buf[S->pos-3]) {
                S->mptr = cand; S->mlen = MINMATCH;
            }
        }
        S->mt[mh] = (u32)S->pos;
    }
    set_contexts(S);
}

/* returns 12-bit P(bit=1), 1..4094 */
static inline int predict(Ctx *S) {
    S->stin[0] = 128;                                /* mixer bias */
    S->stin[1] = S->st[cnt_p(S->o0[S->c0]) >> 4];
    for (int k = 0; k < NTAB; k++) {
        if (S->have[k]) {
            S->tidx[k] = (S->cxt[k] ^ (S->c0 * 2654435761u)) & TMASK;
            S->stin[2+k] = S->st[cnt_p(S->tab[k][S->tidx[k]]) >> 4];
        } else S->stin[2+k] = 0;
    }
    if (S->mvalid) {
        int expbit = (S->mbyte >> (7 - S->bitn)) & 1;
        int bucket = S->mlen < 32 ? (int)S->mlen : 31;
        S->msmidx = bucket * 2 + expbit;
        S->stin[10] = S->st[cnt_p(S->msm[S->msmidx]) >> 4];
    } else { S->msmidx = -1; S->stin[10] = 0; }

    i64 dot = 0;
    const i32 *wp = &S->w[S->mixsel * NIN];
    for (int i = 0; i < NIN; i++) dot += (i64)wp[i] * S->stin[i];
    int d = (int)(dot >> 16);
    if (d < -2047) d = -2047;
    if (d >  2047) d =  2047;
    S->pmix = S->sq[d + 2048];

    int pr = S->pmix;
    pr = (pr + 3 * apm_pp(S, S->apm1, S->c0, pr, &S->a1idx)) >> 2;
    pr = (pr + 3 * apm_pp(S, S->apm2, (S->b1 << 8) | S->c0, pr, &S->a2idx)) >> 2;
    if (pr < 1)    pr = 1;
    if (pr > 4094) pr = 4094;
    return pr;
}

static inline void update(Ctx *S, int y) {
    int err = (y << 12) - S->pmix;
    i32 *wp = &S->w[S->mixsel * NIN];
    for (int i = 0; i < NIN; i++) {
        i32 w = wp[i] + ((S->stin[i] * err) >> MIX_SHIFT);
        if (w < -WCLAMP) w = -WCLAMP;
        if (w >  WCLAMP) w =  WCLAMP;
        wp[i] = w;
    }
    S->o0[S->c0] = cnt_upd(S, S->o0[S->c0], y);
    for (int k = 0; k < NTAB; k++)
        if (S->have[k]) S->tab[k][S->tidx[k]] = cnt_upd(S, S->tab[k][S->tidx[k]], y);
    if (S->msmidx >= 0) S->msm[S->msmidx] = cnt_upd(S, S->msm[S->msmidx], y);
    apm_upd(S->apm1, S->a1idx, y);
    apm_upd(S->apm2, S->a2idx, y);

    if (S->mvalid) {
        int expbit = (S->mbyte >> (7 - S->bitn)) & 1;
        if (y != expbit) S->mvalid = 0;
    }
    S->c0 = (S->c0 << 1) | (u32)y;
    if (++S->bitn == 8) {
        u8 b = (u8)(S->c0 & 0xff);
        S->c0 = 1; S->bitn = 0;
        byte_update(S, b);
    }
}

/* ---------------- context lifecycle -------------------------------------- */
static void ctx_free(Ctx *S) {
    if (!S) return;
    for (int k = 0; k < NTAB; k++) free(S->tab[k]);
    free(S->mt); free(S->apm1); free(S->apm2);
    free(S);
}

/* buf: history window (caller's src for compress, dst for decompress) */
static Ctx *ctx_new(u8 *buf, int buf_write) {
    Ctx *S = (Ctx*)calloc(1, sizeof(Ctx));
    if (!S) return NULL;

    for (int d = -2048; d < 2048; d++) {
        double v = 4096.0 / (1.0 + exp(-d / 256.0));
        int r = (int)v; if (r > 4095) r = 4095; if (r < 0) r = 0;
        S->sq[d+2048] = r;
    }
    int pi = 0;
    for (int d = -2047; d <= 2047; d++) {
        int p = S->sq[d+2048];
        while (pi <= p && pi < 4096) S->st[pi++] = (short)d;
    }
    while (pi < 4096) S->st[pi++] = 2047;
    for (int n = 0; n <= CNT_CAP; n++) S->rate[n] = (u16)(65536 / (n + 2));

    for (int k = 0; k < NTAB; k++) {
        S->tab[k] = (u32*)malloc((size_t)TSIZE * sizeof(u32));
        if (!S->tab[k]) { ctx_free(S); return NULL; }
        for (u32 i = 0; i < TSIZE; i++) S->tab[k][i] = 32768u << 16;
    }
    S->mt = (u32*)calloc(MHSIZE, sizeof(u32));
    if (!S->mt) { ctx_free(S); return NULL; }
    for (int i = 0; i < 256; i++) S->o0[i] = 32768u << 16;
    for (int b = 0; b < 32; b++) {
        S->msm[b*2+0] = 12000u << 16;     /* expected bit 0 -> P(1) low  */
        S->msm[b*2+1] = 53536u << 16;     /* expected bit 1 -> P(1) high */
    }
    for (int i = 0; i < NSETS*NIN; i++) S->w[i] = 65536 / NIN;
    if (!apm_alloc(S, &S->apm1, 256) || !apm_alloc(S, &S->apm2, 65536)) {
        ctx_free(S); return NULL;
    }
    S->buf = buf; S->buf_write = buf_write;
    S->c0 = 1; S->rcd = RECWIN;
    set_contexts(S);
    return S;
}

/* ---------------- container helpers -------------------------------------- */
static void put_le(u8 *p, u64 v, int nb) {
    for (int i = 0; i < nb; i++) p[i] = (u8)(v >> (8*i));
}
static u64 get_le(const u8 *p, int nb) {
    u64 v = 0;
    for (int i = 0; i < nb; i++) v |= (u64)p[i] << (8*i);
    return v;
}

/* ---------------- thread portability shim -------------------------------- */
#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
typedef HANDLE            sq_thread;
typedef CRITICAL_SECTION  sq_mutex;
typedef DWORD             sq_thread_ret;
#  define SQ_THREAD_CALL WINAPI
#  define SQ_THREAD_DONE 0
static void sq_mutex_init(sq_mutex *m)    { InitializeCriticalSection(m); }
static void sq_mutex_destroy(sq_mutex *m) { DeleteCriticalSection(m); }
static void sq_mutex_lock(sq_mutex *m)    { EnterCriticalSection(m); }
static void sq_mutex_unlock(sq_mutex *m)  { LeaveCriticalSection(m); }
typedef sq_thread_ret (SQ_THREAD_CALL *sq_thread_fn)(void *);
static int sq_thread_start(sq_thread *t, sq_thread_fn fn, void *arg) {
    *t = CreateThread(NULL, 0, fn, arg, 0, NULL);
    return *t != NULL;
}
static void sq_thread_join(sq_thread t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}
static int sq_ncpu(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 1;
}
#else
#  include <pthread.h>
#  include <unistd.h>
typedef pthread_t        sq_thread;
typedef pthread_mutex_t  sq_mutex;
typedef void *           sq_thread_ret;
#  define SQ_THREAD_CALL
#  define SQ_THREAD_DONE NULL
static void sq_mutex_init(sq_mutex *m)    { pthread_mutex_init(m, NULL); }
static void sq_mutex_destroy(sq_mutex *m) { pthread_mutex_destroy(m); }
static void sq_mutex_lock(sq_mutex *m)    { pthread_mutex_lock(m); }
static void sq_mutex_unlock(sq_mutex *m)  { pthread_mutex_unlock(m); }
typedef sq_thread_ret (*sq_thread_fn)(void *);
static int sq_thread_start(sq_thread *t, sq_thread_fn fn, void *arg) {
    return pthread_create(t, NULL, fn, arg) == 0;
}
static void sq_thread_join(sq_thread t) { pthread_join(t, NULL); }
static int sq_ncpu(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}
#endif

/* ---------------- public API ---------------------------------------------*/
SQUISH_API const char *squish_version(void) { return SQUISH_VERSION_STRING; }

SQUISH_API const char *squish_strerror(int code) {
    switch (code) {
    case SQUISH_OK:          return "success";
    case SQUISH_E_PARAM:     return "invalid argument";
    case SQUISH_E_NOMEM:     return "out of memory";
    case SQUISH_E_FORMAT:    return "not a SQUISH stream or stream corrupt";
    case SQUISH_E_DSTSIZE:   return "destination buffer too small";
    case SQUISH_E_TOOBIG:    return "input exceeds 4 GiB limit";
    case SQUISH_E_IO:        return "file I/O error";
    case SQUISH_E_CHECKSUM:  return "integrity check failed";
    default:                 return "unknown error";
    }
}

SQUISH_API size_t squish_compress_bound(size_t src_len) {
    return src_len + OVERHEAD;      /* stored-mode fallback caps expansion */
}

SQUISH_API int squish_decompressed_size(const void *src, size_t src_len,
                                        uint64_t *out_size) {
    if (!src || !out_size) return SQUISH_E_PARAM;
    if (src_len < 12 ||
        (memcmp(src, MAGIC, 4) != 0 && memcmp(src, MAGIC_MB, 4) != 0))
        return SQUISH_E_FORMAT;
    u64 n = get_le((const u8*)src + 4, 8);
    if (n >= SQUISH_MAX_INPUT) return SQUISH_E_FORMAT;
    *out_size = n;
    return SQUISH_OK;
}

static int compress_ex(const void *src, size_t src_len,
                       void *dst, size_t *dst_len,
                       squish_progress_fn cb, void *user) {
    if ((!src && src_len) || !dst || !dst_len) return SQUISH_E_PARAM;
    if ((u64)src_len >= SQUISH_MAX_INPUT)      return SQUISH_E_TOOBIG;
    size_t cap = *dst_len;
    size_t stored_size = OVERHEAD + src_len;
    if (cap < OVERHEAD) return SQUISH_E_DSTSIZE;

    u8 *d = (u8*)dst;
    memcpy(d, MAGIC, 4);
    put_le(d + 4, (u64)src_len, 8);
    u64 cks = fnv1a64((const u8*)src, src_len);

    /* try context-mixing; stop early once it can't beat stored mode */
    Ctx *S = ctx_new((u8*)(uintptr_t)src, /*buf_write=*/0);
    if (!S) return SQUISH_E_NOMEM;
    S->out = d + HDR_SIZE;
    size_t cm_cap = (cap < stored_size ? cap : stored_size) - OVERHEAD;
    S->out_cap = cm_cap;
    enc_init(S);
    for (size_t j = 0; j < src_len && !S->overflow; j++) {
        if (cb && (j & 0xFFFFu) == 0) cb(j, src_len, user);
        u8 b = ((const u8*)src)[j];
        for (int i = 7; i >= 0; i--) {
            int y = (b >> i) & 1;
            int pr = predict(S);
            enc_bit(S, y, (pr << 4) | 8);
            update(S, y);
        }
    }
    if (!S->overflow) enc_flush(S);
    int overflow = S->overflow;
    size_t coded = S->out_pos;
    ctx_free(S);

    if (!overflow) {
        d[12] = MODE_CM;
        put_le(d + HDR_SIZE + coded, cks & 0xffffffff, 4);
        *dst_len = HDR_SIZE + coded + CKS_SIZE;
        if (cb) cb(src_len, src_len, user);
        return SQUISH_OK;
    }
    /* incompressible: store raw if there is room */
    if (cap < stored_size) return SQUISH_E_DSTSIZE;
    d[12] = MODE_STORED;
    if (src_len) memcpy(d + HDR_SIZE, src, src_len);
    put_le(d + HDR_SIZE + src_len, cks & 0xffffffff, 4);
    *dst_len = stored_size;
    if (cb) cb(src_len, src_len, user);
    return SQUISH_OK;
}

SQUISH_API int squish_compress(const void *src, size_t src_len,
                               void *dst, size_t *dst_len) {
    return compress_ex(src, src_len, dst, dst_len, NULL, NULL);
}

static int decompress_mb_ex(const void *src, size_t src_len,
                            void *dst, size_t *dst_len,
                            int nthreads, squish_progress_fn cb, void *user);

static int decompress_ex(const void *src, size_t src_len,
                         void *dst, size_t *dst_len,
                         squish_progress_fn cb, void *user) {
    if (!src || !dst_len || (!dst && *dst_len)) return SQUISH_E_PARAM;
    if (src_len >= 4 && memcmp(src, MAGIC_MB, 4) == 0)
        return decompress_mb_ex(src, src_len, dst, dst_len, 1, cb, user);
    if (src_len < OVERHEAD || memcmp(src, MAGIC, 4) != 0)
        return SQUISH_E_FORMAT;
    const u8 *s = (const u8*)src;
    u64 n = get_le(s + 4, 8);
    if (n >= SQUISH_MAX_INPUT) return SQUISH_E_FORMAT;
    int mode = s[12];
    if (mode != MODE_CM && mode != MODE_STORED) return SQUISH_E_FORMAT;
    if (*dst_len < n) { *dst_len = (size_t)n; return SQUISH_E_DSTSIZE; }
    u32 want = (u32)get_le(s + src_len - CKS_SIZE, 4);

    if (mode == MODE_STORED) {
        if (src_len != OVERHEAD + n) return SQUISH_E_FORMAT;
        if (n) memcpy(dst, s + HDR_SIZE, (size_t)n);
    } else {
        Ctx *S = ctx_new((u8*)dst, /*buf_write=*/1);
        if (!S) return SQUISH_E_NOMEM;
        S->in = s; S->in_pos = HDR_SIZE; S->in_end = src_len - CKS_SIZE;
        dec_init(S);
        for (u64 j = 0; j < n; j++) {
            /* The decoder of a well-formed stream consumes exactly the bytes
             * the encoder emitted, never the past-end filler. Sustained
             * filler reads mean the stream is truncated or forged; bail
             * instead of decoding up to 4 GiB of noise (a ~20-byte forged
             * header would otherwise cost hours of CPU before the checksum
             * finally rejects it). */
            if (S->starved > 64) { ctx_free(S); return SQUISH_E_FORMAT; }
            if (cb && (j & 0xFFFFu) == 0) cb(j, n, user);
            for (int i = 7; i >= 0; i--) {
                int pr = predict(S);
                int y = dec_bit(S, (pr << 4) | 8);
                update(S, y);
            }
        }
        ctx_free(S);
    }
    if ((u32)(fnv1a64((const u8*)dst, (size_t)n) & 0xffffffff) != want)
        return SQUISH_E_CHECKSUM;
    *dst_len = (size_t)n;
    if (cb) cb(n, n, user);
    return SQUISH_OK;
}

SQUISH_API int squish_decompress(const void *src, size_t src_len,
                                 void *dst, size_t *dst_len) {
    return decompress_ex(src, src_len, dst, dst_len, NULL, NULL);
}

/* ---------------- multi-block engine -------------------------------------- */

/* Fans per-chunk progress into one monotonic (processed, total) sequence;
 * the mutex also serializes calls into the user's callback. */
typedef struct {
    squish_progress_fn cb; void *user;
    u64 total, agg;
    u64 *cdone;                 /* per-chunk bytes reported so far */
    sq_mutex mu;
} mt_prog;
typedef struct { mt_prog *p; u32 idx; } chunk_prog;

static void mt_prog_cb(u64 done, u64 total, void *user) {
    chunk_prog *cp = (chunk_prog*)user;
    mt_prog *p = cp->p;
    (void)total;
    sq_mutex_lock(&p->mu);
    p->agg += done - p->cdone[cp->idx];
    p->cdone[cp->idx] = done;
    p->cb(p->agg, p->total, p->user);
    sq_mutex_unlock(&p->mu);
}

static int mt_prog_init(mt_prog *p, squish_progress_fn cb, void *user,
                        u64 total, u32 nchunks, chunk_prog **cps) {
    *cps = NULL;
    if (!cb) return 1;
    p->cb = cb; p->user = user; p->total = total; p->agg = 0;
    p->cdone = (u64*)calloc(nchunks, sizeof(u64));
    *cps = (chunk_prog*)malloc((size_t)nchunks * sizeof(chunk_prog));
    if (!p->cdone || !*cps) {
        free(p->cdone); free(*cps); *cps = NULL;
        return 0;
    }
    for (u32 i = 0; i < nchunks; i++) { (*cps)[i].p = p; (*cps)[i].idx = i; }
    sq_mutex_init(&p->mu);
    return 1;
}
static void mt_prog_destroy(mt_prog *p, chunk_prog *cps) {
    if (!cps) return;
    sq_mutex_destroy(&p->mu);
    free(p->cdone); free(cps);
}

/* Workers take chunks i = tid, tid + T, ...: equal-sized chunks make static
 * striding balance as well as a shared queue, with no synchronization. */
typedef struct {
    /* compress */
    const u8 *src; u64 src_len, chunk;
    u8 **bufs; size_t *lens;
    /* decompress */
    const u8 *s; u8 *dst;
    const u64 *coff, *doff, *olen;
    const u32 *clen;
    /* shared */
    int *rcs; chunk_prog *cps;
    u32 nchunks, tid, nthreads;
} mt_args;

static sq_thread_ret SQ_THREAD_CALL compress_worker(void *arg) {
    mt_args *a = (mt_args*)arg;
    for (u32 i = a->tid; i < a->nchunks; i += a->nthreads) {
        u64 off = (u64)i * a->chunk;
        size_t len = (size_t)(a->chunk <= a->src_len - off ? a->chunk
                                                           : a->src_len - off);
        size_t outn = len + OVERHEAD;
        a->bufs[i] = (u8*)malloc(outn);
        if (!a->bufs[i]) { a->rcs[i] = SQUISH_E_NOMEM; continue; }
        a->rcs[i] = compress_ex(a->src + off, len, a->bufs[i], &outn,
                                a->cps ? mt_prog_cb : NULL,
                                a->cps ? (void*)&a->cps[i] : NULL);
        a->lens[i] = outn;
    }
    return SQ_THREAD_DONE;
}

static sq_thread_ret SQ_THREAD_CALL decompress_worker(void *arg) {
    mt_args *a = (mt_args*)arg;
    for (u32 i = a->tid; i < a->nchunks; i += a->nthreads) {
        size_t dn = (size_t)a->olen[i];
        a->rcs[i] = decompress_ex(a->s + a->coff[i], a->clen[i],
                                  a->dst + a->doff[i], &dn,
                                  a->cps ? mt_prog_cb : NULL,
                                  a->cps ? (void*)&a->cps[i] : NULL);
    }
    return SQ_THREAD_DONE;
}

/* Run `fn` over `nchunks` chunks on `nthreads` threads (worker 0 runs on the
 * calling thread; a failed spawn folds that stripe into the caller too). */
static void mt_run(sq_thread_fn fn, mt_args *proto, u32 nthreads,
                   mt_args *args, sq_thread *tids) {
    u32 started = 0;
    for (u32 t = 1; t < nthreads; t++) {
        args[t] = *proto;
        args[t].tid = t;
        args[t].nthreads = nthreads;
        if (sq_thread_start(&tids[started], fn, &args[t])) started++;
        else { args[t].nthreads = 0; }          /* mark: run inline below */
    }
    args[0] = *proto;
    args[0].tid = 0;
    args[0].nthreads = nthreads;
    fn(&args[0]);
    for (u32 t = 1; t < nthreads; t++)
        if (args[t].nthreads == 0) { args[t].nthreads = nthreads; fn(&args[t]); }
    for (u32 t = 0; t < started; t++) sq_thread_join(tids[t]);
}

static int compress_mt_ex(const void *src, size_t src_len,
                          void *dst, size_t *dst_len,
                          int nthreads, size_t chunk_size,
                          squish_progress_fn cb, void *user) {
    if ((!src && src_len) || !dst || !dst_len) return SQUISH_E_PARAM;
    if ((u64)src_len >= SQUISH_MAX_INPUT)      return SQUISH_E_TOOBIG;
    u64 chunk = chunk_size ? chunk_size : SQUISH_DEFAULT_CHUNK;
    if (chunk < SQUISH_MIN_CHUNK) chunk = SQUISH_MIN_CHUNK;
    if ((u64)src_len <= chunk)
        return compress_ex(src, src_len, dst, dst_len, cb, user);

    u32 k = (u32)(((u64)src_len + chunk - 1) / chunk);
    int T = nthreads > 0 ? nthreads : sq_ncpu();
    if ((u32)T > k) T = (int)k;

    u8    **bufs = (u8**)calloc(k, sizeof(u8*));
    size_t *lens = (size_t*)calloc(k, sizeof(size_t));
    int    *rcs  = (int*)calloc(k, sizeof(int));
    mt_args *args = (mt_args*)calloc(T, sizeof(mt_args));
    sq_thread *tids = (sq_thread*)calloc(T, sizeof(sq_thread));
    mt_prog prog; chunk_prog *cps = NULL;
    int rc = SQUISH_OK;
    if (!bufs || !lens || !rcs || !args || !tids ||
        !mt_prog_init(&prog, cb, user, src_len, k, &cps)) {
        rc = SQUISH_E_NOMEM;
        goto done;
    }

    {
        mt_args proto;
        memset(&proto, 0, sizeof proto);
        proto.src = (const u8*)src; proto.src_len = src_len;
        proto.chunk = chunk;
        proto.bufs = bufs; proto.lens = lens;
        proto.rcs = rcs; proto.cps = cps; proto.nchunks = k;
        mt_run(compress_worker, &proto, (u32)T, args, tids);
    }
    for (u32 i = 0; i < k && rc == SQUISH_OK; i++) rc = rcs[i];
    if (rc != SQUISH_OK) goto done;

    {
        size_t cap = *dst_len, stored_size = OVERHEAD + src_len;
        u64 total = MB_HDR_SIZE + 4ull * k;
        for (u32 i = 0; i < k; i++) total += lens[i];
        u8 *d = (u8*)dst;
        if (total < stored_size && total <= cap) {
            memcpy(d, MAGIC_MB, 4);
            put_le(d + 4, (u64)src_len, 8);
            d[12] = MODE_MB;
            put_le(d + 13, k, 4);
            size_t w = MB_HDR_SIZE;
            for (u32 i = 0; i < k; i++, w += 4) put_le(d + w, lens[i], 4);
            for (u32 i = 0; i < k; i++) {
                memcpy(d + w, bufs[i], lens[i]);
                w += lens[i];
            }
            *dst_len = w;
        } else if (stored_size <= cap) {
            memcpy(d, MAGIC, 4);
            put_le(d + 4, (u64)src_len, 8);
            d[12] = MODE_STORED;
            if (src_len) memcpy(d + HDR_SIZE, src, src_len);
            put_le(d + HDR_SIZE + src_len,
                   fnv1a64((const u8*)src, src_len) & 0xffffffff, 4);
            *dst_len = stored_size;
        } else rc = SQUISH_E_DSTSIZE;
    }
    if (rc == SQUISH_OK && cb) cb(src_len, src_len, user);

done:
    if (bufs) for (u32 i = 0; i < k; i++) free(bufs[i]);
    mt_prog_destroy(&prog, cps);
    free(bufs); free(lens); free(rcs); free(args); free(tids);
    return rc;
}

static int decompress_mb_ex(const void *src, size_t src_len,
                            void *dst, size_t *dst_len,
                            int nthreads, squish_progress_fn cb, void *user) {
    if (!src || !dst_len || (!dst && *dst_len)) return SQUISH_E_PARAM;
    const u8 *s = (const u8*)src;
    if (src_len < MB_HDR_SIZE + 4 || memcmp(s, MAGIC_MB, 4) != 0 ||
        s[12] != MODE_MB)
        return SQUISH_E_FORMAT;
    u64 n = get_le(s + 4, 8);
    u32 k = (u32)get_le(s + 13, 4);
    if (n >= SQUISH_MAX_INPUT || k < 1 || k > MB_MAX_CHUNKS) return SQUISH_E_FORMAT;
    u64 table_end = MB_HDR_SIZE + 4ull * k;
    if (src_len < table_end) return SQUISH_E_FORMAT;
    if (*dst_len < n) { *dst_len = (size_t)n; return SQUISH_E_DSTSIZE; }

    u32 *clen = (u32*)malloc((size_t)k * sizeof(u32));
    u64 *coff = (u64*)malloc((size_t)k * sizeof(u64));
    u64 *doff = (u64*)malloc((size_t)k * sizeof(u64));
    u64 *olen = (u64*)malloc((size_t)k * sizeof(u64));
    int *rcs  = (int*)calloc(k, sizeof(int));
    mt_args *args = NULL; sq_thread *tids = NULL;
    mt_prog prog; chunk_prog *cps = NULL;
    int rc = SQUISH_OK;
    if (!clen || !coff || !doff || !olen || !rcs) { rc = SQUISH_E_NOMEM; goto done; }

    {   /* chunk table: offsets must tile the payload and the output exactly;
         * chunks must themselves be SQ02 (no recursive containers) */
        u64 co = table_end, dof = 0;
        for (u32 i = 0; i < k; i++) {
            clen[i] = (u32)get_le(s + MB_HDR_SIZE + 4ull*i, 4);
            coff[i] = co; doff[i] = dof;
            if (clen[i] < OVERHEAD || clen[i] > src_len - co) {
                rc = SQUISH_E_FORMAT; goto done;
            }
            if (memcmp(s + co, MAGIC, 4) != 0) { rc = SQUISH_E_FORMAT; goto done; }
            olen[i] = get_le(s + co + 4, 8);
            /* no encoder emits empty chunks; rejecting them stops a forged
             * table from demanding 64k pointless model setups (~150 MB of
             * table init each) for zero output */
            if (olen[i] == 0 || olen[i] > n - dof) { rc = SQUISH_E_FORMAT; goto done; }
            co += clen[i]; dof += olen[i];
        }
        if (co != src_len || dof != n) { rc = SQUISH_E_FORMAT; goto done; }
    }

    {
        int T = nthreads > 0 ? nthreads : sq_ncpu();
        if ((u32)T > k) T = (int)k;
        args = (mt_args*)calloc(T, sizeof(mt_args));
        tids = (sq_thread*)calloc(T, sizeof(sq_thread));
        if (!args || !tids || !mt_prog_init(&prog, cb, user, n, k, &cps)) {
            rc = SQUISH_E_NOMEM; goto done;
        }
        mt_args proto;
        memset(&proto, 0, sizeof proto);
        proto.s = s; proto.dst = (u8*)dst;
        proto.coff = coff; proto.doff = doff;
        proto.olen = olen; proto.clen = clen;
        proto.rcs = rcs; proto.cps = cps; proto.nchunks = k;
        mt_run(decompress_worker, &proto, (u32)T, args, tids);
    }
    for (u32 i = 0; i < k && rc == SQUISH_OK; i++) rc = rcs[i];
    if (rc == SQUISH_OK) {
        *dst_len = (size_t)n;
        if (cb) cb(n, n, user);
    }

done:
    mt_prog_destroy(&prog, cps);
    free(clen); free(coff); free(doff); free(olen); free(rcs);
    free(args); free(tids);
    return rc;
}

SQUISH_API int squish_threads(void) { return sq_ncpu(); }

SQUISH_API int squish_compress_mt(const void *src, size_t src_len,
                                  void *dst, size_t *dst_len,
                                  int nthreads, size_t chunk_size,
                                  squish_progress_fn progress, void *user) {
    return compress_mt_ex(src, src_len, dst, dst_len,
                          nthreads, chunk_size, progress, user);
}

SQUISH_API int squish_decompress_mt(const void *src, size_t src_len,
                                    void *dst, size_t *dst_len,
                                    int nthreads,
                                    squish_progress_fn progress, void *user) {
    if (src && src_len >= 4 && memcmp(src, MAGIC_MB, 4) == 0)
        return decompress_mb_ex(src, src_len, dst, dst_len,
                                nthreads, progress, user);
    return decompress_ex(src, src_len, dst, dst_len, progress, user);
}

static int compress_alloc_ex(const void *src, size_t src_len,
                             void **dst, size_t *dst_len,
                             squish_progress_fn cb, void *user) {
    if (!dst || !dst_len) return SQUISH_E_PARAM;
    *dst = NULL; *dst_len = 0;
    size_t cap = squish_compress_bound(src_len);
    u8 *buf = (u8*)malloc(cap);
    if (!buf) return SQUISH_E_NOMEM;
    size_t out = cap;
    int rc = compress_ex(src, src_len, buf, &out, cb, user);
    if (rc != SQUISH_OK) { free(buf); return rc; }
    u8 *trim = (u8*)realloc(buf, out ? out : 1);
    *dst = trim ? trim : buf;
    *dst_len = out;
    return SQUISH_OK;
}

SQUISH_API int squish_compress_alloc(const void *src, size_t src_len,
                                     void **dst, size_t *dst_len) {
    return compress_alloc_ex(src, src_len, dst, dst_len, NULL, NULL);
}

static int decompress_alloc_ex(const void *src, size_t src_len,
                               void **dst, size_t *dst_len,
                               squish_progress_fn cb, void *user) {
    if (!dst || !dst_len) return SQUISH_E_PARAM;
    *dst = NULL; *dst_len = 0;
    u64 n;
    int rc = squish_decompressed_size(src, src_len, &n);
    if (rc != SQUISH_OK) return rc;
    if (n >= SQUISH_MAX_INPUT) return SQUISH_E_FORMAT;
    u8 *buf = (u8*)malloc(n ? (size_t)n : 1);
    if (!buf) return SQUISH_E_NOMEM;
    size_t out = (size_t)n;
    rc = decompress_ex(src, src_len, buf, &out, cb, user);
    if (rc != SQUISH_OK) { free(buf); return rc; }
    *dst = buf; *dst_len = out;
    return SQUISH_OK;
}

SQUISH_API int squish_decompress_alloc(const void *src, size_t src_len,
                                       void **dst, size_t *dst_len) {
    return decompress_alloc_ex(src, src_len, dst, dst_len, NULL, NULL);
}

SQUISH_API int squish_compress_alloc_mt(const void *src, size_t src_len,
                                        void **dst, size_t *dst_len,
                                        int nthreads, size_t chunk_size,
                                        squish_progress_fn progress,
                                        void *user) {
    if (!dst || !dst_len) return SQUISH_E_PARAM;
    *dst = NULL; *dst_len = 0;
    size_t cap = squish_compress_bound(src_len);
    u8 *buf = (u8*)malloc(cap);
    if (!buf) return SQUISH_E_NOMEM;
    size_t out = cap;
    int rc = compress_mt_ex(src, src_len, buf, &out,
                            nthreads, chunk_size, progress, user);
    if (rc != SQUISH_OK) { free(buf); return rc; }
    u8 *trim = (u8*)realloc(buf, out ? out : 1);
    *dst = trim ? trim : buf;
    *dst_len = out;
    return SQUISH_OK;
}

SQUISH_API int squish_decompress_alloc_mt(const void *src, size_t src_len,
                                          void **dst, size_t *dst_len,
                                          int nthreads,
                                          squish_progress_fn progress,
                                          void *user) {
    if (!dst || !dst_len) return SQUISH_E_PARAM;
    *dst = NULL; *dst_len = 0;
    u64 n;
    int rc = squish_decompressed_size(src, src_len, &n);
    if (rc != SQUISH_OK) return rc;
    if (n >= SQUISH_MAX_INPUT) return SQUISH_E_FORMAT;
    u8 *buf = (u8*)malloc(n ? (size_t)n : 1);
    if (!buf) return SQUISH_E_NOMEM;
    size_t out = (size_t)n;
    rc = squish_decompress_mt(src, src_len, buf, &out,
                              nthreads, progress, user);
    if (rc != SQUISH_OK) { free(buf); return rc; }
    *dst = buf; *dst_len = out;
    return SQUISH_OK;
}

SQUISH_API void squish_free(void *p) { free(p); }

/* ---------------- file helpers ------------------------------------------- */

/* 64-bit stream offsets: plain ftell returns long, which is 32-bit on
 * Windows — a >=2 GiB input would be mis-sized (and on a 32-bit size_t the
 * cast below could silently truncate what gets compressed). */
#if defined(_WIN32)
#  define sq_ftell64(f) _ftelli64(f)
#else
#  define sq_ftell64(f) ftello(f)
#endif

static int read_whole(const char *path, u8 **data, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return SQUISH_E_IO;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return SQUISH_E_IO; }
    long long n = sq_ftell64(f);
    if (n < 0) { fclose(f); return SQUISH_E_IO; }
    if ((unsigned long long)n > (size_t)-1) { fclose(f); return SQUISH_E_TOOBIG; }
    rewind(f);
    u8 *buf = (u8*)malloc(n ? (size_t)n : 1);
    if (!buf) { fclose(f); return SQUISH_E_NOMEM; }
    if (n && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf); fclose(f); return SQUISH_E_IO;
    }
    fclose(f);
    *data = buf; *len = (size_t)n;
    return SQUISH_OK;
}
static int write_whole(const char *path, const u8 *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return SQUISH_E_IO;
    if (len && fwrite(data, 1, len, f) != len) { fclose(f); return SQUISH_E_IO; }
    return fclose(f) == 0 ? SQUISH_OK : SQUISH_E_IO;
}

SQUISH_API int squish_compress_file2(const char *src_path,
                                     const char *dst_path,
                                     squish_progress_fn progress, void *user) {
    if (!src_path || !dst_path) return SQUISH_E_PARAM;
    u8 *in; size_t n;
    int rc = read_whole(src_path, &in, &n);
    if (rc != SQUISH_OK) return rc;
    void *out; size_t outn;
    rc = compress_alloc_ex(in, n, &out, &outn, progress, user);
    free(in);
    if (rc != SQUISH_OK) return rc;
    rc = write_whole(dst_path, (const u8*)out, outn);
    squish_free(out);
    return rc;
}

SQUISH_API int squish_compress_file(const char *src_path,
                                    const char *dst_path) {
    return squish_compress_file2(src_path, dst_path, NULL, NULL);
}

SQUISH_API int squish_decompress_file2(const char *src_path,
                                       const char *dst_path,
                                       squish_progress_fn progress,
                                       void *user) {
    if (!src_path || !dst_path) return SQUISH_E_PARAM;
    u8 *in; size_t n;
    int rc = read_whole(src_path, &in, &n);
    if (rc != SQUISH_OK) return rc;
    void *out; size_t outn;
    rc = decompress_alloc_ex(in, n, &out, &outn, progress, user);
    free(in);
    if (rc != SQUISH_OK) return rc;
    rc = write_whole(dst_path, (const u8*)out, outn);
    squish_free(out);
    return rc;
}

SQUISH_API int squish_decompress_file(const char *src_path,
                                      const char *dst_path) {
    return squish_decompress_file2(src_path, dst_path, NULL, NULL);
}

SQUISH_API int squish_compress_file_mt(const char *src_path,
                                       const char *dst_path,
                                       int nthreads, size_t chunk_size,
                                       squish_progress_fn progress,
                                       void *user) {
    if (!src_path || !dst_path) return SQUISH_E_PARAM;
    u8 *in; size_t n;
    int rc = read_whole(src_path, &in, &n);
    if (rc != SQUISH_OK) return rc;
    void *out; size_t outn;
    rc = squish_compress_alloc_mt(in, n, &out, &outn,
                                  nthreads, chunk_size, progress, user);
    free(in);
    if (rc != SQUISH_OK) return rc;
    rc = write_whole(dst_path, (const u8*)out, outn);
    squish_free(out);
    return rc;
}

SQUISH_API int squish_decompress_file_mt(const char *src_path,
                                         const char *dst_path,
                                         int nthreads,
                                         squish_progress_fn progress,
                                         void *user) {
    if (!src_path || !dst_path) return SQUISH_E_PARAM;
    u8 *in; size_t n;
    int rc = read_whole(src_path, &in, &n);
    if (rc != SQUISH_OK) return rc;
    void *out; size_t outn;
    rc = squish_decompress_alloc_mt(in, n, &out, &outn,
                                    nthreads, progress, user);
    free(in);
    if (rc != SQUISH_OK) return rc;
    rc = write_whole(dst_path, (const u8*)out, outn);
    squish_free(out);
    return rc;
}

/* ============================================================================
 * Seekable archive container "SQAR" (see docs/FORMAT.md §12)
 *
 * A directory becomes a header, one independently-compressed §1 stream per
 * file, and a compressed index of (path, mode, size, stream offset/size). A
 * reader inflates the small index at open time, then reaches any single member
 * by seeking to its stream — the rest of the archive is never touched. All
 * integers little-endian.
 *
 *   0   8  magic "SQAR02\n\x1a"
 *   8   4  version u32 (2)          16  8  entry_count u64
 *   12  4  flags   u32 (0)          24  8  total_size  u64 (sum of file sizes)
 *   32  8  index_offset u64         40  8  index_comp_size u64
 *   48  8  index_orig_size u64      56  ... member streams, then index blob
 *
 * Index entry: type u8 | mode u32 | orig u64 | coff u64 | csize u64 |
 *              plen u32 | path[plen]     (dirs carry orig/coff/csize = 0)
 * ==========================================================================*/
#include <errno.h>
#if defined(_WIN32)
#  include <direct.h>
#  include <io.h>
#else
#  include <sys/types.h>
#  include <sys/stat.h>
#  include <dirent.h>
#endif

static const u8 SQAR_MAGIC[8] = { 'S','Q','A','R','0','2','\n','\x1a' };
#define SQAR_VERSION    2u
#define SQAR_HDR        56u        /* fixed container header size            */
#define SQAR_ENT_FIXED  33u        /* type1+mode4+orig8+coff8+csize8+plen4   */
#define SQAR_MAX_PATH   65535u

#if defined(_WIN32)
#  define sq_fseek64(f,o) _fseeki64((f),(long long)(o),SEEK_SET)
#else
#  define sq_fseek64(f,o) fseeko((f),(off_t)(o),SEEK_SET)
#endif

/* ---------------- growable byte buffer ----------------------------------- */
typedef struct { u8 *p; size_t len, cap; } gbuf;

static int gbuf_put(gbuf *b, const void *d, size_t n) {
    if (!n) return 0;
    if (b->len + n < b->len) return -1;                     /* size_t wrap */
    if (b->len + n > b->cap) {
        size_t nc = b->cap ? b->cap : 4096;
        while (nc < b->len + n) {
            if (nc > (size_t)-1 / 2) { nc = b->len + n; break; }
            nc *= 2;
        }
        u8 *np = (u8*)realloc(b->p, nc);
        if (!np) return -1;
        b->p = np; b->cap = nc;
    }
    memcpy(b->p + b->len, d, n);
    b->len += n;
    return 0;
}
static int gbuf_u8 (gbuf *b, u8 v)  { return gbuf_put(b, &v, 1); }
static int gbuf_u32(gbuf *b, u32 v) { u8 t[4]; put_le(t, v, 4); return gbuf_put(b, t, 4); }
static int gbuf_u64(gbuf *b, u64 v) { u8 t[8]; put_le(t, v, 8); return gbuf_put(b, t, 8); }

/* ---------------- path helpers ------------------------------------------- */

/* malloc "<a>/<b>", or a plain copy of b when a is empty/NULL. NULL on OOM. */
static char *path_join(const char *a, const char *b) {
    size_t la = a ? strlen(a) : 0, lb = strlen(b);
    char *r = (char*)malloc(la + 1 + lb + 1);
    if (!r) return NULL;
    if (la) { memcpy(r, a, la); r[la] = '/'; memcpy(r + la + 1, b, lb + 1); }
    else    { memcpy(r, b, lb + 1); }
    return r;
}

/* malloc'd copy of `path` with trailing '/' and '\\' trimmed (never to empty). */
static char *strip_trailing_sep(const char *path) {
    size_t n = strlen(path);
    while (n > 1 && (path[n-1] == '/' || path[n-1] == '\\')) n--;
    char *r = (char*)malloc(n + 1);
    if (!r) return NULL;
    memcpy(r, path, n); r[n] = '\0';
    return r;
}

/* A stored path is safe iff it is relative and every component is non-empty,
 * not "." or "..", and free of ':' and '\\' — so an archive can never write
 * outside the extraction root. */
static int arc_path_safe(const char *p, size_t n) {
    if (n == 0 || p[0] == '/') return 0;
    for (size_t i = 0; i < n; ) {
        size_t j = i;
        while (j < n && p[j] != '/') j++;
        size_t clen = j - i;
        if (clen == 0) return 0;
        if (clen == 1 && p[i] == '.') return 0;
        if (clen == 2 && p[i] == '.' && p[i+1] == '.') return 0;
        for (size_t k = i; k < j; k++)
            if (p[k] == '\\' || p[k] == ':' || p[k] == '\0') return 0;
        i = (j < n) ? j + 1 : j;
    }
    return 1;
}

/* ---------------- filesystem probes -------------------------------------- */
static int path_is_dir(const char *p) {
#if defined(_WIN32)
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat s;
    return stat(p, &s) == 0 && S_ISDIR(s.st_mode);
#endif
}
static int path_is_regular(const char *p) {
#if defined(_WIN32)
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    struct stat s;
    return stat(p, &s) == 0 && S_ISREG(s.st_mode);
#endif
}
static unsigned path_mode(const char *p, unsigned dflt) {
#if defined(_WIN32)
    (void)p; return dflt;
#else
    struct stat s;
    return stat(p, &s) == 0 ? (unsigned)(s.st_mode & 0777) : dflt;
#endif
}
static u64 path_size(const char *p) {
#if defined(_WIN32)
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (GetFileAttributesExA(p, GetFileExInfoStandard, &d))
        return ((u64)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    return 0;
#else
    struct stat s;
    return stat(p, &s) == 0 ? (u64)s.st_size : 0;
#endif
}

/* Create one directory; success if it already exists. */
static int make_dir(const char *p) {
#if defined(_WIN32)
    if (_mkdir(p) == 0) return 0;
#else
    if (mkdir(p, 0777) == 0) return 0;
#endif
    return errno == EEXIST ? 0 : -1;
}

/* Create out_root and every directory along the relative path `rel`. When
 * include_last is 0, stop before rel's final component (create parents only,
 * for a file); when 1, create rel itself too (a directory entry). */
static int make_dirs(const char *out_root, const char *rel, int include_last) {
    if (make_dir(out_root) != 0) return -1;
    size_t rl = strlen(out_root);
    char *w = (char*)malloc(rl + 1 + strlen(rel) + 1);
    if (!w) return -1;
    memcpy(w, out_root, rl); w[rl] = '\0';
    size_t wl = rl;
    int rc = 0;
    for (const char *c = rel; *c; ) {
        const char *slash = strchr(c, '/');
        size_t clen = slash ? (size_t)(slash - c) : strlen(c);
        if (!slash && !include_last) break;
        w[wl++] = '/';
        memcpy(w + wl, c, clen); wl += clen; w[wl] = '\0';
        if (make_dir(w) != 0) { rc = -1; break; }
        if (!slash) break;
        c = slash + 1;
    }
    free(w);
    return rc;
}

/* Write `n` bytes to `path`, applying unix `mode` where supported. */
static int write_file_bytes(const char *path, const u8 *d, size_t n, unsigned mode) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int ok = !(n && fwrite(d, 1, n, f) != n);
    if (fclose(f) != 0) ok = 0;
    if (!ok) { remove(path); return -1; }
#if !defined(_WIN32)
    if (mode) chmod(path, (mode_t)(mode & 0777));
#else
    (void)mode;
#endif
    return 0;
}

/* ---------------- directory listing (sorted, reproducible) --------------- */
static int name_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}
static void free_names(char **names, size_t n) {
    for (size_t i = 0; i < n; i++) free(names[i]);
    free(names);
}
static int add_name(char ***a, size_t *n, size_t *cap, const char *nm) {
    if (*n == *cap) {
        size_t nc = *cap ? *cap * 2 : 16;
        char **np = (char**)realloc(*a, nc * sizeof *np);
        if (!np) return -1;
        *a = np; *cap = nc;
    }
    size_t l = strlen(nm);
    char *copy = (char*)malloc(l + 1);
    if (!copy) return -1;
    memcpy(copy, nm, l + 1);
    (*a)[(*n)++] = copy;
    return 0;
}
static int list_dir(const char *dir, char ***out, size_t *out_n) {
    char **names = NULL; size_t n = 0, cap = 0;
    *out = NULL; *out_n = 0;
#if defined(_WIN32)
    char *pat = path_join(dir, "*");
    if (!pat) return -1;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    free(pat);
    if (h == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : -1;
    do {
        const char *nm = fd.cFileName;
        if (!strcmp(nm, ".") || !strcmp(nm, "..")) continue;
        if (add_name(&names, &n, &cap, nm) != 0) {
            FindClose(h); free_names(names, n); return -1;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *nm = e->d_name;
        if (!strcmp(nm, ".") || !strcmp(nm, "..")) continue;
        if (add_name(&names, &n, &cap, nm) != 0) {
            closedir(d); free_names(names, n); return -1;
        }
    }
    closedir(d);
#endif
    if (n > 1) qsort(names, n, sizeof *names, name_cmp);
    *out = names; *out_n = n;
    return 0;
}

/* ---------------- entry list (shared by writer and reader) --------------- */
typedef struct { u8 type; u32 mode; u64 orig, coff, csize; char *path; } arc_ent;
typedef struct { arc_ent *e; size_t n, cap; } arc_list;

static void arc_list_free(arc_list *L) {
    for (size_t i = 0; i < L->n; i++) free(L->e[i].path);
    free(L->e);
    L->e = NULL; L->n = L->cap = 0;
}
/* Takes ownership of `path` (freed by arc_list_free); frees it on OOM. */
static int arc_list_add(arc_list *L, u8 type, u32 mode, u64 orig, char *path) {
    if (L->n == L->cap) {
        size_t nc = L->cap ? L->cap * 2 : 32;
        arc_ent *ne = (arc_ent*)realloc(L->e, nc * sizeof *ne);
        if (!ne) { free(path); return -1; }
        L->e = ne; L->cap = nc;
    }
    arc_ent *en = &L->e[L->n++];
    en->type = type; en->mode = mode; en->orig = orig;
    en->coff = 0; en->csize = 0; en->path = path;
    return 0;
}

/* ---------------- writer ------------------------------------------------- */

/* Walk fs_dir, appending each member (dirs before their contents, siblings
 * sorted) to L with its archive-relative path arc_pre/<name>; sum file sizes
 * into *total. Does not read file contents. */
static int arc_scan(const char *fs_dir, const char *arc_pre,
                    arc_list *L, u64 *total) {
    char **names; size_t n;
    if (list_dir(fs_dir, &names, &n) != 0) return SQUISH_E_IO;
    int rc = SQUISH_OK;
    for (size_t i = 0; i < n && rc == SQUISH_OK; i++) {
        char *fs  = path_join(fs_dir, names[i]);
        char *arc = path_join(arc_pre, names[i]);
        if (!fs || !arc) { free(fs); free(arc); rc = SQUISH_E_NOMEM; break; }
        if (strlen(arc) > SQAR_MAX_PATH) { free(fs); free(arc); rc = SQUISH_E_FORMAT; break; }
        if (path_is_dir(fs)) {
            if (arc_list_add(L, 1, path_mode(fs, 0755), 0, arc) != 0) {
                rc = SQUISH_E_NOMEM;
            } else {
                rc = arc_scan(fs, L->e[L->n - 1].path, L, total);  /* arc owned by L */
            }
        } else if (path_is_regular(fs)) {
            u64 sz = path_size(fs);
            if (arc_list_add(L, 0, path_mode(fs, 0644), sz, arc) != 0) rc = SQUISH_E_NOMEM;
            else *total += sz;
        } else {
            free(arc);      /* skip sockets/fifos/dangling links */
        }
        free(fs);
    }
    free_names(names, n);
    return rc;
}

/* Serialize L's metadata into a fresh index buffer (caller frees idx->p). */
static int arc_build_index(const arc_list *L, gbuf *idx) {
    for (size_t i = 0; i < L->n; i++) {
        const arc_ent *e = &L->e[i];
        size_t plen = strlen(e->path);
        if (gbuf_u8(idx, e->type) || gbuf_u32(idx, e->mode) ||
            gbuf_u64(idx, e->orig) || gbuf_u64(idx, e->coff) ||
            gbuf_u64(idx, e->csize) || gbuf_u32(idx, (u32)plen) ||
            gbuf_put(idx, e->path, plen))
            return SQUISH_E_NOMEM;
    }
    return SQUISH_OK;
}

SQUISH_API int squish_archive_create(const char *dir_path,
                                     const char *archive_path,
                                     int nthreads, size_t chunk_size,
                                     squish_progress_fn cb, void *user) {
    if (!dir_path || !archive_path) return SQUISH_E_PARAM;
    char *root = strip_trailing_sep(dir_path);
    if (!root) return SQUISH_E_NOMEM;

    arc_list L = { NULL, 0, 0 };
    u64 total = 0;
    int rc = arc_scan(root, "", &L, &total);
    if (rc != SQUISH_OK) { arc_list_free(&L); free(root); return rc; }

    FILE *f = fopen(archive_path, "wb+");
    if (!f) { arc_list_free(&L); free(root); return SQUISH_E_IO; }

    /* header placeholder: magic + version now, counts/offsets patched at end */
    u8 hdr[SQAR_HDR];
    memset(hdr, 0, sizeof hdr);
    memcpy(hdr, SQAR_MAGIC, 8);
    put_le(hdr + 8, SQAR_VERSION, 4);
    if (fwrite(hdr, 1, SQAR_HDR, f) != SQAR_HDR) { rc = SQUISH_E_IO; goto done; }
    u64 off = SQAR_HDR, processed = 0;

    for (size_t i = 0; i < L.n; i++) {
        arc_ent *e = &L.e[i];
        if (e->type != 0) continue;                 /* directories: metadata only */
        char *fs = path_join(root, e->path);
        if (!fs) { rc = SQUISH_E_NOMEM; goto done; }
        u8 *data; size_t dl;
        rc = read_whole(fs, &data, &dl);
        free(fs);
        if (rc != SQUISH_OK) goto done;
        void *comp; size_t cl;
        rc = squish_compress_alloc_mt(data, dl, &comp, &cl,
                                      nthreads, chunk_size, NULL, NULL);
        free(data);
        if (rc != SQUISH_OK) goto done;
        int ok = (cl == 0) || (fwrite(comp, 1, cl, f) == cl);
        squish_free(comp);
        if (!ok) { rc = SQUISH_E_IO; goto done; }
        e->coff = off; e->csize = cl; e->orig = dl;
        off += cl;
        processed += dl;
        if (cb) cb(processed, total, user);
    }

    {   /* index: build, compress, append */
        gbuf idx = { NULL, 0, 0 };
        rc = arc_build_index(&L, &idx);
        if (rc != SQUISH_OK) { free(idx.p); goto done; }
        void *ic; size_t icl;
        u8 empty = 0;
        rc = squish_compress_alloc(idx.len ? idx.p : &empty, idx.len, &ic, &icl);
        u64 idx_orig = idx.len;
        free(idx.p);
        if (rc != SQUISH_OK) goto done;
        int ok = (icl == 0) || (fwrite(ic, 1, icl, f) == icl);
        squish_free(ic);
        if (!ok) { rc = SQUISH_E_IO; goto done; }

        put_le(hdr + 16, (u64)L.n, 8);
        put_le(hdr + 24, total, 8);
        put_le(hdr + 32, off, 8);              /* index offset */
        put_le(hdr + 40, icl, 8);              /* index compressed size */
        put_le(hdr + 48, idx_orig, 8);         /* index original size */
        if (sq_fseek64(f, 0) != 0 || fwrite(hdr, 1, SQAR_HDR, f) != SQAR_HDR)
            rc = SQUISH_E_IO;
    }
    if (rc == SQUISH_OK && cb) cb(total, total, user);

done:
    if (fclose(f) != 0 && rc == SQUISH_OK) rc = SQUISH_E_IO;
    if (rc != SQUISH_OK) remove(archive_path);
    arc_list_free(&L);
    free(root);
    return rc;
}

/* ---------------- reader ------------------------------------------------- */
struct squish_archive {
    FILE     *f;                 /* file-backed; NULL when memory-backed   */
    const u8 *mem; size_t mem_len;
    u64       filesize;
    u32       version, flags;
    u64       total_size, index_off;
    arc_ent  *ents; u64 nents;   /* coff/csize/orig/mode/type/path per member */
};

/* Copy [off,off+len) of the archive into a fresh buffer, or (memory-backed)
 * point directly at it. *owned is set to a malloc'd buffer the caller frees,
 * or NULL when the pointer is borrowed from the mapped image. */
static int arc_range(squish_archive *a, u64 off, u64 len,
                     const u8 **pp, u8 **owned) {
    *owned = NULL;
    if (off > a->filesize || len > a->filesize - off) return SQUISH_E_FORMAT;
    if (a->mem) { *pp = a->mem + off; return SQUISH_OK; }
    if (len > (size_t)-1) return SQUISH_E_NOMEM;
    u8 *buf = (u8*)malloc(len ? (size_t)len : 1);
    if (!buf) return SQUISH_E_NOMEM;
    if (len && (sq_fseek64(a->f, off) != 0 ||
                fread(buf, 1, (size_t)len, a->f) != (size_t)len)) {
        free(buf); return SQUISH_E_IO;
    }
    *pp = buf; *owned = buf;
    return SQUISH_OK;
}

SQUISH_API int squish_archive_probe(const void *data, size_t len) {
    const u8 *p = (const u8*)data;
    return p && len >= 12 && memcmp(p, SQAR_MAGIC, 8) == 0 &&
           get_le(p + 8, 4) == SQAR_VERSION;
}

/* Parse the header + decompressed index into a->ents. Returns a status. */
static int arc_parse(squish_archive *a) {
    u8 hdr[SQAR_HDR];
    const u8 *hp;
    if (a->mem) {
        if (a->mem_len < SQAR_HDR) return SQUISH_E_FORMAT;
        hp = a->mem;
    } else {
        if (a->filesize < SQAR_HDR) return SQUISH_E_FORMAT;
        if (sq_fseek64(a->f, 0) != 0 || fread(hdr, 1, SQAR_HDR, a->f) != SQAR_HDR)
            return SQUISH_E_IO;
        hp = hdr;
    }
    if (memcmp(hp, SQAR_MAGIC, 8) != 0 || get_le(hp + 8, 4) != SQAR_VERSION)
        return SQUISH_E_FORMAT;
    a->version    = (u32)get_le(hp + 8, 4);
    a->flags      = (u32)get_le(hp + 12, 4);
    u64 count     = get_le(hp + 16, 8);
    a->total_size = get_le(hp + 24, 8);
    a->index_off  = get_le(hp + 32, 8);
    u64 icl       = get_le(hp + 40, 8);
    u64 iorig     = get_le(hp + 48, 8);

    /* index blob must sit wholly after the header and inside the file */
    if (a->index_off < SQAR_HDR || icl > a->filesize - a->index_off)
        return SQUISH_E_FORMAT;
    /* every entry costs at least SQAR_ENT_FIXED + 1 bytes, so this bounds the
     * index allocation against a forged count */
    if (iorig >= SQUISH_MAX_INPUT || count > iorig / (SQAR_ENT_FIXED + 1))
        return SQUISH_E_FORMAT;

    const u8 *ip; u8 *iown;
    int rc = arc_range(a, a->index_off, icl, &ip, &iown);
    if (rc != SQUISH_OK) return rc;
    void *idx = NULL; size_t idn = 0;
    rc = squish_decompress_alloc(ip, (size_t)icl, &idx, &idn);
    free(iown);
    if (rc != SQUISH_OK) return rc == SQUISH_E_CHECKSUM ? SQUISH_E_FORMAT : rc;
    if (idn != iorig) { squish_free(idx); return SQUISH_E_FORMAT; }

    a->ents = (arc_ent*)calloc(count ? (size_t)count : 1, sizeof(arc_ent));
    if (!a->ents) { squish_free(idx); return SQUISH_E_NOMEM; }

    const u8 *b = (const u8*)idx;
    size_t o = 0;
    rc = SQUISH_OK;
    for (u64 i = 0; i < count; i++) {
        if (idn - o < SQAR_ENT_FIXED) { rc = SQUISH_E_FORMAT; break; }
        arc_ent *e = &a->ents[i];
        e->type  = b[o];
        e->mode  = (u32)get_le(b + o + 1, 4);
        e->orig  = get_le(b + o + 5, 8);
        e->coff  = get_le(b + o + 13, 8);
        e->csize = get_le(b + o + 21, 8);
        u32 plen = (u32)get_le(b + o + 29, 4);
        o += SQAR_ENT_FIXED;
        if (plen == 0 || plen > SQAR_MAX_PATH || plen > idn - o) { rc = SQUISH_E_FORMAT; break; }
        if (!arc_path_safe((const char*)(b + o), plen)) { rc = SQUISH_E_FORMAT; break; }
        e->path = (char*)malloc((size_t)plen + 1);
        if (!e->path) { rc = SQUISH_E_NOMEM; break; }
        memcpy(e->path, b + o, plen); e->path[plen] = '\0';
        o += plen;
        a->nents = i + 1;                       /* so cleanup frees this path */
        if (e->type == 1) {
            if (e->orig || e->coff || e->csize) { rc = SQUISH_E_FORMAT; break; }
        } else if (e->type == 0) {
            /* member stream must lie inside the data region [HDR, index_off) */
            if (e->coff < SQAR_HDR || e->csize > a->index_off - e->coff ||
                e->csize < OVERHEAD) { rc = SQUISH_E_FORMAT; break; }
        } else { rc = SQUISH_E_FORMAT; break; }
    }
    if (rc == SQUISH_OK && o != idn) rc = SQUISH_E_FORMAT;   /* must tile */
    squish_free(idx);
    return rc;
}

static void arc_reader_free(squish_archive *a) {
    if (!a) return;
    if (a->f) fclose(a->f);
    if (a->ents) for (u64 i = 0; i < a->nents; i++) free(a->ents[i].path);
    free(a->ents);
    free(a);
}

SQUISH_API int squish_archive_open(const char *path, squish_archive **out) {
    if (!path || !out) return SQUISH_E_PARAM;
    *out = NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return SQUISH_E_IO;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return SQUISH_E_IO; }
    long long sz = sq_ftell64(f);
    if (sz < 0) { fclose(f); return SQUISH_E_IO; }
    squish_archive *a = (squish_archive*)calloc(1, sizeof *a);
    if (!a) { fclose(f); return SQUISH_E_NOMEM; }
    a->f = f; a->filesize = (u64)sz;
    int rc = arc_parse(a);
    if (rc != SQUISH_OK) { arc_reader_free(a); return rc; }
    *out = a;
    return SQUISH_OK;
}

SQUISH_API int squish_archive_open_memory(const void *data, size_t len,
                                          squish_archive **out) {
    if (!data || !out) return SQUISH_E_PARAM;
    *out = NULL;
    squish_archive *a = (squish_archive*)calloc(1, sizeof *a);
    if (!a) return SQUISH_E_NOMEM;
    a->mem = (const u8*)data; a->mem_len = len; a->filesize = len;
    int rc = arc_parse(a);
    if (rc != SQUISH_OK) { arc_reader_free(a); return rc; }
    *out = a;
    return SQUISH_OK;
}

SQUISH_API void squish_archive_close(squish_archive *a) { arc_reader_free(a); }

SQUISH_API int squish_archive_info_get(const squish_archive *a,
                                       squish_archive_info *out) {
    if (!a || !out) return SQUISH_E_PARAM;
    out->version = a->version; out->flags = a->flags;
    out->entry_count = a->nents; out->total_size = a->total_size;
    return SQUISH_OK;
}

SQUISH_API uint64_t squish_archive_count(const squish_archive *a) {
    return a ? a->nents : 0;
}

SQUISH_API int squish_archive_stat(const squish_archive *a, uint64_t index,
                                   squish_archive_entry *out) {
    if (!a || !out || index >= a->nents) return SQUISH_E_PARAM;
    const arc_ent *e = &a->ents[index];
    out->path = e->path;
    out->size = e->orig;
    out->stored_size = e->csize;
    out->mode = e->mode;
    out->is_dir = (e->type == 1);
    return SQUISH_OK;
}

SQUISH_API int squish_archive_find(const squish_archive *a,
                                   const char *path, uint64_t *index_out) {
    if (!a || !path) return SQUISH_E_PARAM;
    size_t n = strlen(path);
    while (n > 1 && path[n-1] == '/') n--;              /* tolerate trailing / */
    for (u64 i = 0; i < a->nents; i++)
        if (strlen(a->ents[i].path) == n && memcmp(a->ents[i].path, path, n) == 0) {
            if (index_out) *index_out = i;
            return SQUISH_OK;
        }
    return SQUISH_E_FORMAT;
}

SQUISH_API int squish_archive_extract(squish_archive *a, uint64_t index,
                                      void **out, size_t *out_len) {
    if (!a || !out || !out_len) return SQUISH_E_PARAM;
    *out = NULL; *out_len = 0;
    if (index >= a->nents) return SQUISH_E_PARAM;
    const arc_ent *e = &a->ents[index];
    if (e->type != 0) return SQUISH_E_PARAM;            /* directory */
    const u8 *cp; u8 *own;
    int rc = arc_range(a, e->coff, e->csize, &cp, &own);
    if (rc != SQUISH_OK) return rc;
    void *dec; size_t dn;
    rc = squish_decompress_alloc_mt(cp, (size_t)e->csize, &dec, &dn, 0, NULL, NULL);
    free(own);
    if (rc != SQUISH_OK) return rc;
    if (dn != e->orig) { squish_free(dec); return SQUISH_E_FORMAT; }
    *out = dec; *out_len = dn;
    return SQUISH_OK;
}

SQUISH_API int squish_archive_extract_path(squish_archive *a, const char *path,
                                           void **out, size_t *out_len) {
    if (!a || !path || !out || !out_len) return SQUISH_E_PARAM;
    *out = NULL; *out_len = 0;
    uint64_t i;
    int rc = squish_archive_find(a, path, &i);
    if (rc != SQUISH_OK) return rc;
    return squish_archive_extract(a, i, out, out_len);
}

SQUISH_API int squish_archive_extract_to_file(squish_archive *a,
                                              const char *path,
                                              const char *dst_path) {
    if (!a || !path || !dst_path) return SQUISH_E_PARAM;
    uint64_t i;
    int rc = squish_archive_find(a, path, &i);
    if (rc != SQUISH_OK) return rc;
    if (a->ents[i].type != 0) return SQUISH_E_PARAM;
    void *buf; size_t n;
    rc = squish_archive_extract(a, i, &buf, &n);
    if (rc != SQUISH_OK) return rc;
    rc = write_file_bytes(dst_path, (const u8*)buf, n, a->ents[i].mode) == 0
             ? SQUISH_OK : SQUISH_E_IO;
    squish_free(buf);
    return rc;
}

/* True if member path `p` is `prefix` itself or lies beneath it. */
static int under_prefix(const char *p, const char *prefix, size_t plen) {
    if (plen == 0) return 1;
    if (strncmp(p, prefix, plen) != 0) return 0;
    return p[plen] == '\0' || p[plen] == '/';
}

SQUISH_API int squish_archive_extract_subtree(squish_archive *a,
                                              const char *prefix,
                                              const char *dst_root,
                                              squish_progress_fn cb, void *user) {
    if (!a || !dst_root) return SQUISH_E_PARAM;
    char pbuf[SQAR_MAX_PATH + 1];
    size_t plen = 0;
    if (prefix && *prefix) {
        plen = strlen(prefix);
        while (plen > 1 && prefix[plen-1] == '/') plen--;
        if (plen > SQAR_MAX_PATH) return SQUISH_E_FORMAT;
        memcpy(pbuf, prefix, plen); pbuf[plen] = '\0';
    }
    const char *pfx = plen ? pbuf : "";

    u64 total = 0, processed = 0;
    int matched = 0;
    for (u64 i = 0; i < a->nents; i++)
        if (under_prefix(a->ents[i].path, pfx, plen)) {
            matched = 1;
            if (a->ents[i].type == 0) total += a->ents[i].orig;
        }
    if (!matched && plen) return SQUISH_E_FORMAT;      /* no such member */
    if (make_dir(dst_root) != 0) return SQUISH_E_IO;

    int rc = SQUISH_OK;
    for (u64 i = 0; i < a->nents && rc == SQUISH_OK; i++) {
        const arc_ent *e = &a->ents[i];
        if (!under_prefix(e->path, pfx, plen)) continue;
        if (e->type == 1) {
            if (make_dirs(dst_root, e->path, 1) != 0) rc = SQUISH_E_IO;
        } else {
            char *full = path_join(dst_root, e->path);
            if (!full) { rc = SQUISH_E_NOMEM; break; }
            void *buf; size_t n;
            rc = squish_archive_extract(a, i, &buf, &n);
            if (rc == SQUISH_OK) {
                if (make_dirs(dst_root, e->path, 0) != 0 ||
                    write_file_bytes(full, (const u8*)buf, n, e->mode) != 0)
                    rc = SQUISH_E_IO;
                squish_free(buf);
                processed += e->orig;
                if (rc == SQUISH_OK && cb) cb(processed, total, user);
            }
            free(full);
        }
    }
    return rc;
}
