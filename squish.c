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
    return S->in_pos < S->in_end ? S->in[S->in_pos++] : 255;
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
    *out_size = get_le((const u8*)src + 4, 8);
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
    memcpy(d + HDR_SIZE, src, src_len);
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
        memcpy(dst, s + HDR_SIZE, (size_t)n);
    } else {
        Ctx *S = ctx_new((u8*)dst, /*buf_write=*/1);
        if (!S) return SQUISH_E_NOMEM;
        S->in = s; S->in_pos = HDR_SIZE; S->in_end = src_len - CKS_SIZE;
        dec_init(S);
        for (u64 j = 0; j < n; j++) {
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
            memcpy(d + HDR_SIZE, src, src_len);
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
            if (olen[i] > n - dof) { rc = SQUISH_E_FORMAT; goto done; }
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
static int read_whole(const char *path, u8 **data, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return SQUISH_E_IO;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return SQUISH_E_IO; }
    long long n = ftell(f);
    if (n < 0) { fclose(f); return SQUISH_E_IO; }
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
