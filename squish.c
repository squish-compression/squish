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
 * There is one on-disk format, the SQUISH archive (magic "SQSH", see
 * docs/FORMAT.md). Its atom is a coded *block*:
 *   [0]     mode: 0 = arithmetic-coded, 1 = stored (incompressible fallback)
 *   [1..]   payload
 *   [last4] FNV-1a 64 of the block's original bytes, folded to 32 bits, LE
 * A block's original and compressed sizes are recorded by the enclosing
 * archive index, so the block itself carries no magic or length. A member's
 * data is one or more blocks (chunked for parallelism); the archive header,
 * member blocks, and a compressed index make up the file.
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

/* A coded block: 1 mode byte + payload + 4-byte checksum. Its original and
 * compressed sizes come from the archive index, not the block. */
#define BLK_MODE   1
#define CKS_SIZE   4
#define BLK_OVER   (BLK_MODE + CKS_SIZE)   /* fixed per-block overhead = 5 */

enum { MODE_CM = 0, MODE_STORED = 1 };

/* Largest number of blocks a single member can hold (MAX_INPUT / MIN_CHUNK),
 * used to bound forged index allocations. */
#define MAX_BLOCKS 65536u

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

/* ---------------- public API: introspection ------------------------------*/
SQUISH_API const char *squish_version(void) { return SQUISH_VERSION_STRING; }

SQUISH_API const char *squish_strerror(int code) {
    switch (code) {
    case SQUISH_OK:          return "success";
    case SQUISH_E_PARAM:     return "invalid argument";
    case SQUISH_E_NOMEM:     return "out of memory";
    case SQUISH_E_FORMAT:    return "not a SQUISH archive or archive corrupt";
    case SQUISH_E_DSTSIZE:   return "destination buffer too small";
    case SQUISH_E_TOOBIG:    return "input exceeds 4 GiB limit";
    case SQUISH_E_IO:        return "file I/O error";
    case SQUISH_E_CHECKSUM:  return "integrity check failed";
    default:                 return "unknown error";
    }
}

SQUISH_API int squish_threads(void) { return sq_ncpu(); }

SQUISH_API void squish_free(void *p) { free(p); }

/* Number of coded blocks a member of `orig` bytes uses at block size `chunk`
 * (chunk >= 1): 0 for an empty member, else ceil(orig/chunk). */
static u64 member_blocks(u64 orig, u64 chunk) {
    return orig ? (orig + chunk - 1) / chunk : 0;
}

/* ---------------- coded block: the compression atom ----------------------- *
 * A block is [mode u8][payload][checksum u32]; its original and compressed
 * sizes live in the enclosing index. Stored mode caps a block at n+BLK_OVER. */

static int block_compress(const u8 *src, size_t n, u8 *dst, size_t cap,
                          size_t *outlen, squish_progress_fn cb, void *user) {
    if ((u64)n >= SQUISH_MAX_INPUT) return SQUISH_E_TOOBIG;
    size_t stored_size = BLK_OVER + n;
    if (cap < BLK_OVER) return SQUISH_E_DSTSIZE;
    u64 cks = fnv1a64(src, n);

    Ctx *S = ctx_new((u8*)(uintptr_t)src, /*buf_write=*/0);
    if (!S) return SQUISH_E_NOMEM;
    S->out = dst + BLK_MODE;
    S->out_cap = (cap < stored_size ? cap : stored_size) - BLK_OVER;
    enc_init(S);
    for (size_t j = 0; j < n && !S->overflow; j++) {
        if (cb && (j & 0xFFFFu) == 0) cb(j, n, user);
        u8 b = src[j];
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
        dst[0] = MODE_CM;
        put_le(dst + BLK_MODE + coded, cks & 0xffffffff, 4);
        *outlen = BLK_MODE + coded + CKS_SIZE;
        if (cb) cb(n, n, user);
        return SQUISH_OK;
    }
    if (cap < stored_size) return SQUISH_E_DSTSIZE;
    dst[0] = MODE_STORED;
    if (n) memcpy(dst + BLK_MODE, src, n);
    put_le(dst + BLK_MODE + n, cks & 0xffffffff, 4);
    *outlen = stored_size;
    if (cb) cb(n, n, user);
    return SQUISH_OK;
}

static int block_decompress(const u8 *src, size_t csize, u8 *dst, size_t n,
                            squish_progress_fn cb, void *user) {
    if (csize < BLK_OVER) return SQUISH_E_FORMAT;
    int mode = src[0];
    if (mode != MODE_CM && mode != MODE_STORED) return SQUISH_E_FORMAT;
    u32 want = (u32)get_le(src + csize - CKS_SIZE, 4);

    if (mode == MODE_STORED) {
        if (csize != BLK_OVER + n) return SQUISH_E_FORMAT;
        if (n) memcpy(dst, src + BLK_MODE, n);
    } else {
        Ctx *S = ctx_new(dst, /*buf_write=*/1);
        if (!S) return SQUISH_E_NOMEM;
        S->in = src; S->in_pos = BLK_MODE; S->in_end = csize - CKS_SIZE;
        dec_init(S);
        for (u64 j = 0; j < n; j++) {
            /* A well-formed block consumes exactly the encoder's bytes, never
             * the past-end filler. Sustained filler reads mean a truncated or
             * forged block; bail instead of decoding up to 4 GiB of noise. */
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
    if ((u32)(fnv1a64(dst, n) & 0xffffffff) != want) return SQUISH_E_CHECKSUM;
    if (cb) cb(n, n, user);
    return SQUISH_OK;
}

SQUISH_API size_t squish_compress_bound(size_t src_len) {
    /* header + member blocks (stored worst case) + a one-entry index block */
    size_t k = src_len / SQUISH_MIN_CHUNK + 1;      /* max blocks             */
    return 64 + src_len + BLK_OVER * k              /* member data region     */
         + (25 + 8 * k + BLK_OVER)                  /* index: 1 entry, stored */
         + 64;                                      /* slack                  */
}

/* ---------------- multi-block progress fan-in ----------------------------- */
typedef struct {
    squish_progress_fn cb; void *user;
    u64 total, agg;
    u64 *cdone;                 /* per-block bytes reported so far */
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
    if (!cb || nchunks == 0) return 1;
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

/* Workers take blocks i = tid, tid + T, ...: equal-sized blocks make static
 * striding balance as well as a shared queue, with no synchronization. */
typedef struct {
    /* compress */
    const u8 *src; u64 src_len, chunk;
    u8 **bufs; size_t *lens;
    /* decompress */
    const u8 *s; u8 *dst;
    const u64 *coff, *doff, *olen, *clen;
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
        size_t outn = len + BLK_OVER;
        a->bufs[i] = (u8*)malloc(outn);
        if (!a->bufs[i]) { a->rcs[i] = SQUISH_E_NOMEM; continue; }
        a->rcs[i] = block_compress(a->src + off, len, a->bufs[i], outn, &outn,
                                   a->cps ? mt_prog_cb : NULL,
                                   a->cps ? (void*)&a->cps[i] : NULL);
        a->lens[i] = outn;
    }
    return SQ_THREAD_DONE;
}

static sq_thread_ret SQ_THREAD_CALL decompress_worker(void *arg) {
    mt_args *a = (mt_args*)arg;
    for (u32 i = a->tid; i < a->nchunks; i += a->nthreads) {
        a->rcs[i] = block_decompress(a->s + a->coff[i], (size_t)a->clen[i],
                                     a->dst + a->doff[i], (size_t)a->olen[i],
                                     a->cps ? mt_prog_cb : NULL,
                                     a->cps ? (void*)&a->cps[i] : NULL);
    }
    return SQ_THREAD_DONE;
}

/* Run `fn` over `nchunks` blocks on `nthreads` threads (worker 0 runs on the
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

/* Compress a member's bytes into k = ceil(src_len/chunk) coded blocks (0 for
 * an empty member). On success returns malloc'd arrays *bufs (each a block)
 * and *lens, and *k; the caller frees every *bufs[i] plus *bufs and *lens.
 * `chunk` is the effective (>= MIN, non-zero) block size actually used. */
static int compress_member(const u8 *src, size_t src_len, int nthreads,
                           u64 chunk, u8 ***bufs_out, size_t **lens_out,
                           u32 *k_out, squish_progress_fn cb, void *user) {
    *bufs_out = NULL; *lens_out = NULL; *k_out = 0;
    if ((u64)src_len >= SQUISH_MAX_INPUT) return SQUISH_E_TOOBIG;
    if (src_len == 0) return SQUISH_OK;                 /* empty: no blocks */

    u32 k = (u32)member_blocks(src_len, chunk);
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
        rc = SQUISH_E_NOMEM; goto done;
    }
    {
        mt_args proto;
        memset(&proto, 0, sizeof proto);
        proto.src = src; proto.src_len = src_len; proto.chunk = chunk;
        proto.bufs = bufs; proto.lens = lens;
        proto.rcs = rcs; proto.cps = cps; proto.nchunks = k;
        mt_run(compress_worker, &proto, (u32)T, args, tids);
    }
    for (u32 i = 0; i < k && rc == SQUISH_OK; i++) rc = rcs[i];

done:
    mt_prog_destroy(&prog, cps);
    free(rcs); free(args); free(tids);
    if (rc != SQUISH_OK) {
        if (bufs) for (u32 i = 0; i < k; i++) free(bufs[i]);
        free(bufs); free(lens);
        return rc;
    }
    *bufs_out = bufs; *lens_out = lens; *k_out = k;
    return SQUISH_OK;
}

/* Decode a member: `data` points at its first block; `clen[i]` is each block's
 * compressed length (blocks are contiguous). Writes `orig` bytes to dst. */
static int decompress_member(const u8 *data, u64 data_len, const u64 *clen,
                             u32 k, u64 orig, u64 chunk, u8 *dst, int nthreads,
                             squish_progress_fn cb, void *user) {
    if (k == 0) {
        if (orig != 0) return SQUISH_E_FORMAT;
        if (cb) cb(0, 0, user);
        return SQUISH_OK;
    }
    u64 *coff = (u64*)malloc((size_t)k * sizeof(u64));
    u64 *doff = (u64*)malloc((size_t)k * sizeof(u64));
    u64 *olen = (u64*)malloc((size_t)k * sizeof(u64));
    int *rcs  = (int*)calloc(k, sizeof(int));
    mt_args *args = NULL; sq_thread *tids = NULL;
    mt_prog prog; chunk_prog *cps = NULL;
    int rc = SQUISH_OK;
    if (!coff || !doff || !olen || !rcs) { rc = SQUISH_E_NOMEM; goto done; }

    {   /* block table: contiguous compressed spans tiling [0, data_len);
         * per-block original sizes derived from the uniform chunk. */
        u64 co = 0, dof = 0;
        for (u32 i = 0; i < k; i++) {
            coff[i] = co; doff[i] = dof;
            if (clen[i] < BLK_OVER || clen[i] > data_len - co) {
                rc = SQUISH_E_FORMAT; goto done;
            }
            olen[i] = chunk <= orig - dof ? chunk : orig - dof;
            co += clen[i]; dof += olen[i];
        }
        if (co != data_len || dof != orig) { rc = SQUISH_E_FORMAT; goto done; }
    }
    {
        int T = nthreads > 0 ? nthreads : sq_ncpu();
        if ((u32)T > k) T = (int)k;
        args = (mt_args*)calloc(T, sizeof(mt_args));
        tids = (sq_thread*)calloc(T, sizeof(sq_thread));
        if (!args || !tids || !mt_prog_init(&prog, cb, user, orig, k, &cps)) {
            rc = SQUISH_E_NOMEM; goto done;
        }
        mt_args proto;
        memset(&proto, 0, sizeof proto);
        proto.s = data; proto.dst = dst;
        proto.coff = coff; proto.doff = doff; proto.olen = olen; proto.clen = clen;
        proto.rcs = rcs; proto.cps = cps; proto.nchunks = k;
        mt_run(decompress_worker, &proto, (u32)T, args, tids);
    }
    for (u32 i = 0; i < k && rc == SQUISH_OK; i++) rc = rcs[i];
    if (rc == SQUISH_OK && cb) cb(orig, orig, user);

done:
    mt_prog_destroy(&prog, cps);
    free(coff); free(doff); free(olen); free(rcs); free(args); free(tids);
    return rc;
}

/* ============================================================================
 * SQUISH archive: the one on-disk format (see docs/FORMAT.md)
 *
 * A header, one independently-compressed member per file (each a list of
 * coded blocks), and a compressed index of (path, mode, size, block layout).
 * A reader inflates the small index at open time, then reaches any single
 * member by seeking to its blocks — the rest is never touched. LE integers.
 *
 *   0  8  magic "SQSH\r\n\x1a\n"     16  8  entry_count u64
 *   8  4  version u32 (1)            24  8  total_size u64 (sum of file sizes)
 *   12 4  flags   u32 (0)            32  8  chunk_size u64
 *   40 8  index_offset u64           48  8  index_size u64 (compressed)
 *   56 8  index_orig_size u64        64  ... member blocks, then index block
 *
 * Index entry: type u8 | mode u32 | orig u64 | data_off u64 |
 *              (ceil(orig/chunk)) x (csize u64) | plen u32 | path[plen]
 * (a directory carries orig/data_off = 0 and no block sizes.)
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

static const u8 ARC_MAGIC[8] = { 'S','Q','S','H', 0x0D, 0x0A, 0x1A, 0x0A };
#define ARC_VERSION   1u
#define ARC_FLAG_SINGLE 1u         /* archive is one file/blob, not a tree   */
#define ARC_HDR       64u          /* fixed container header size            */
#define ARC_ENT_FIXED 25u          /* type1+mode4+orig8+doff8+plen4          */
#define ARC_MAX_PATH  65535u

#if defined(_WIN32)
#  define sq_fseek64(f,o) _fseeki64((f),(long long)(o),SEEK_SET)
#  define sq_ftell64(f)   _ftelli64(f)
#else
#  define sq_fseek64(f,o) fseeko((f),(off_t)(o),SEEK_SET)
#  define sq_ftell64(f)   ftello(f)
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
static char *path_join(const char *a, const char *b) {
    size_t la = a ? strlen(a) : 0, lb = strlen(b);
    char *r = (char*)malloc(la + 1 + lb + 1);
    if (!r) return NULL;
    if (la) { memcpy(r, a, la); r[la] = '/'; memcpy(r + la + 1, b, lb + 1); }
    else    { memcpy(r, b, lb + 1); }
    return r;
}
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
 * outside the extraction root. The empty path is reserved for the unnamed
 * member of a buffer/blob archive and is handled separately by callers. */
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
static int make_dir(const char *p) {
#if defined(_WIN32)
    if (_mkdir(p) == 0) return 0;
#else
    if (mkdir(p, 0777) == 0) return 0;
#endif
    return errno == EEXIST ? 0 : -1;
}
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

/* ---------------- file slurp/spew ---------------------------------------- */
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

/* ---------------- entry list (shared by writer and reader) --------------- *
 * Each entry carries the member's block layout: data_off and one compressed
 * size per block (nblk = ceil(orig/chunk), 0 for a dir or empty file). */
typedef struct {
    u8   type; u32 mode; u64 orig, data_off;
    u64 *csz; u32 nblk;
    char *path;
} arc_ent;
typedef struct { arc_ent *e; size_t n, cap; } arc_list;

static void arc_list_free(arc_list *L) {
    for (size_t i = 0; i < L->n; i++) { free(L->e[i].path); free(L->e[i].csz); }
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
    en->data_off = 0; en->csz = NULL; en->nblk = 0; en->path = path;
    return 0;
}

/* ---------------- effective chunk ---------------------------------------- */
static u64 eff_chunk(size_t chunk_size) {
    u64 c = chunk_size ? (u64)chunk_size : (u64)SQUISH_DEFAULT_CHUNK;
    if (c < SQUISH_MIN_CHUNK) c = SQUISH_MIN_CHUNK;
    return c;
}

/* ---------------- index (de)serialization -------------------------------- */
static int arc_build_index(const arc_list *L, gbuf *idx) {
    for (size_t i = 0; i < L->n; i++) {
        const arc_ent *e = &L->e[i];
        size_t plen = strlen(e->path);
        if (gbuf_u8(idx, e->type) || gbuf_u32(idx, e->mode) ||
            gbuf_u64(idx, e->orig) || gbuf_u64(idx, e->data_off) ||
            gbuf_u32(idx, (u32)plen))
            return SQUISH_E_NOMEM;
        for (u32 b = 0; b < e->nblk; b++)
            if (gbuf_u64(idx, e->csz[b])) return SQUISH_E_NOMEM;
        if (gbuf_put(idx, e->path, plen)) return SQUISH_E_NOMEM;
    }
    return SQUISH_OK;
}

/* Compress `idx` (may be empty) into a single coded block, appended to *out.
 * Sets *comp_len and *orig_len. */
static int arc_write_index(const gbuf *idx, gbuf *out,
                           u64 *comp_len, u64 *orig_len) {
    size_t cap = idx->len + BLK_OVER, cl = cap;
    u8 dummy = 0;
    u8 *blk = (u8*)malloc(cap);
    if (!blk) return SQUISH_E_NOMEM;
    int rc = block_compress(idx->len ? idx->p : &dummy, idx->len, blk, cap, &cl,
                            NULL, NULL);
    if (rc == SQUISH_OK && gbuf_put(out, blk, cl)) rc = SQUISH_E_NOMEM;
    free(blk);
    if (rc != SQUISH_OK) return rc;
    *comp_len = cl; *orig_len = idx->len;
    return SQUISH_OK;
}

/* ---------------- writer core -------------------------------------------- *
 * Layout an arc_list (dirs metadata-only; files compressed into blocks) into
 * a growable buffer: header, member blocks, index block. Returns the archive
 * in *out (caller frees out->p). Reads file bytes via read_whole when the
 * entry has a filesystem source; the single-blob case pre-populates blocks. */

/* Emit header into the first ARC_HDR bytes of buf. */
static void arc_put_header(u8 *h, u32 flags, u64 entry_count, u64 total,
                           u64 chunk, u64 index_off, u64 index_comp,
                           u64 index_orig) {
    memset(h, 0, ARC_HDR);
    memcpy(h, ARC_MAGIC, 8);
    put_le(h + 8,  ARC_VERSION, 4);
    put_le(h + 12, flags, 4);
    put_le(h + 16, entry_count, 8);
    put_le(h + 24, total, 8);
    put_le(h + 32, chunk, 8);
    put_le(h + 40, index_off, 8);
    put_le(h + 48, index_comp, 8);
    put_le(h + 56, index_orig, 8);
}

/* ---------------- in-memory buffer -> one-member archive ----------------- */
SQUISH_API int squish_compress_alloc(const void *src, size_t src_len,
                                     void **dst, size_t *dst_len,
                                     int nthreads, size_t chunk_size,
                                     squish_progress_fn progress, void *user) {
    if (!dst || !dst_len || (!src && src_len)) return SQUISH_E_PARAM;
    *dst = NULL; *dst_len = 0;
    if ((u64)src_len >= SQUISH_MAX_INPUT) return SQUISH_E_TOOBIG;
    u64 chunk = eff_chunk(chunk_size);

    u8 **bufs; size_t *lens; u32 k;
    int rc = compress_member((const u8*)src, src_len, nthreads, chunk,
                             &bufs, &lens, &k, progress, user);
    if (rc != SQUISH_OK) return rc;

    gbuf out = { NULL, 0, 0 };
    u8 hdr[ARC_HDR];
    memset(hdr, 0, sizeof hdr);
    if (gbuf_put(&out, hdr, ARC_HDR)) { rc = SQUISH_E_NOMEM; goto done; }
    u64 data_off = ARC_HDR;
    for (u32 i = 0; i < k; i++)
        if (gbuf_put(&out, bufs[i], lens[i])) { rc = SQUISH_E_NOMEM; goto done; }

    {   /* one file entry, empty path (the unnamed blob) */
        arc_list L = { NULL, 0, 0 };
        char *empty = (char*)malloc(1);
        if (!empty) { rc = SQUISH_E_NOMEM; goto done; }
        empty[0] = '\0';
        if (arc_list_add(&L, 0, 0, src_len, empty)) { rc = SQUISH_E_NOMEM; goto done; }
        L.e[0].data_off = src_len ? data_off : 0;
        L.e[0].nblk = k;
        L.e[0].csz = (u64*)malloc(k ? k * sizeof(u64) : 1);
        if (!L.e[0].csz) { arc_list_free(&L); rc = SQUISH_E_NOMEM; goto done; }
        for (u32 i = 0; i < k; i++) L.e[0].csz[i] = lens[i];

        gbuf idx = { NULL, 0, 0 };
        rc = arc_build_index(&L, &idx);
        arc_list_free(&L);
        if (rc != SQUISH_OK) { free(idx.p); goto done; }
        u64 index_off = out.len, icl = 0, iorig = 0;
        rc = arc_write_index(&idx, &out, &icl, &iorig);
        free(idx.p);
        if (rc != SQUISH_OK) goto done;
        arc_put_header(out.p, ARC_FLAG_SINGLE, 1, src_len, chunk,
                       index_off, icl, iorig);
    }

done:
    for (u32 i = 0; i < k; i++) free(bufs[i]);
    free(bufs); free(lens);
    if (rc != SQUISH_OK) { free(out.p); return rc; }
    u8 *trim = (u8*)realloc(out.p, out.len ? out.len : 1);
    *dst = trim ? trim : out.p;
    *dst_len = out.len;
    return SQUISH_OK;
}

/* ---------------- reader -------------------------------------------------- */
struct squish_archive {
    FILE     *f;                 /* file-backed; NULL when memory-backed   */
    const u8 *mem; size_t mem_len;
    u64       filesize;
    u32       version, flags;
    u64       total_size, chunk, index_off;
    arc_ent  *ents; u64 nents;
};

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
    return p && len >= 12 && memcmp(p, ARC_MAGIC, 8) == 0 &&
           get_le(p + 8, 4) == ARC_VERSION;
}

SQUISH_API int squish_content_size(const void *src, size_t src_len,
                                   uint64_t *out_size) {
    const u8 *p = (const u8*)src;
    if (!src || !out_size) return SQUISH_E_PARAM;
    if (src_len < 32 || memcmp(p, ARC_MAGIC, 8) != 0 ||
        get_le(p + 8, 4) != ARC_VERSION)
        return SQUISH_E_FORMAT;
    *out_size = get_le(p + 24, 8);
    return SQUISH_OK;
}

/* Parse header + decompressed index into a->ents. Returns a status. */
static int arc_parse(squish_archive *a) {
    u8 hdr[ARC_HDR];
    const u8 *hp;
    if (a->mem) {
        if (a->mem_len < ARC_HDR) return SQUISH_E_FORMAT;
        hp = a->mem;
    } else {
        if (a->filesize < ARC_HDR) return SQUISH_E_FORMAT;
        if (sq_fseek64(a->f, 0) != 0 || fread(hdr, 1, ARC_HDR, a->f) != ARC_HDR)
            return SQUISH_E_IO;
        hp = hdr;
    }
    if (memcmp(hp, ARC_MAGIC, 8) != 0 || get_le(hp + 8, 4) != ARC_VERSION)
        return SQUISH_E_FORMAT;
    a->version    = (u32)get_le(hp + 8, 4);
    a->flags      = (u32)get_le(hp + 12, 4);
    u64 count     = get_le(hp + 16, 8);
    a->total_size = get_le(hp + 24, 8);
    a->chunk      = get_le(hp + 32, 8);
    a->index_off  = get_le(hp + 40, 8);
    u64 icl       = get_le(hp + 48, 8);
    u64 iorig     = get_le(hp + 56, 8);

    if (a->chunk < 1) return SQUISH_E_FORMAT;
    /* index block must sit wholly after the header and inside the file */
    if (a->index_off < ARC_HDR || icl > a->filesize - a->index_off)
        return SQUISH_E_FORMAT;
    /* every entry costs at least ARC_ENT_FIXED bytes, so this bounds the
     * index allocation against a forged count */
    if (iorig >= SQUISH_MAX_INPUT || count > iorig / ARC_ENT_FIXED)
        return SQUISH_E_FORMAT;

    const u8 *ip; u8 *iown;
    int rc = arc_range(a, a->index_off, icl, &ip, &iown);
    if (rc != SQUISH_OK) return rc;
    u8 *idx = (u8*)malloc(iorig ? (size_t)iorig : 1);
    if (!idx) { free(iown); return SQUISH_E_NOMEM; }
    rc = block_decompress(ip, (size_t)icl, idx, (size_t)iorig, NULL, NULL);
    free(iown);
    if (rc != SQUISH_OK) { free(idx); return rc == SQUISH_E_CHECKSUM ? SQUISH_E_FORMAT : rc; }

    a->ents = (arc_ent*)calloc(count ? (size_t)count : 1, sizeof(arc_ent));
    if (!a->ents) { free(idx); return SQUISH_E_NOMEM; }

    const u8 *b = idx;
    size_t o = 0, idn = (size_t)iorig;
    rc = SQUISH_OK;
    for (u64 i = 0; i < count; i++) {
        if (idn - o < ARC_ENT_FIXED) { rc = SQUISH_E_FORMAT; break; }
        arc_ent *e = &a->ents[i];
        e->type     = b[o];
        e->mode     = (u32)get_le(b + o + 1, 4);
        e->orig     = get_le(b + o + 5, 8);
        e->data_off = get_le(b + o + 13, 8);
        u32 plen    = (u32)get_le(b + o + 21, 4);
        o += ARC_ENT_FIXED;
        a->nents = i + 1;                       /* so cleanup frees this entry */

        if (e->type == 1) {                    /* directory */
            if (e->orig || e->data_off) { rc = SQUISH_E_FORMAT; break; }
            e->nblk = 0; e->csz = NULL;
        } else if (e->type == 0) {             /* regular file */
            if (e->orig >= SQUISH_MAX_INPUT) { rc = SQUISH_E_FORMAT; break; }
            u64 nb = member_blocks(e->orig, a->chunk);
            if (nb > MAX_BLOCKS) { rc = SQUISH_E_FORMAT; break; }
            e->nblk = (u32)nb;
            if (nb) {
                if ((idn - o) / 8 < nb) { rc = SQUISH_E_FORMAT; break; }
                e->csz = (u64*)malloc((size_t)nb * sizeof(u64));
                if (!e->csz) { rc = SQUISH_E_NOMEM; break; }
                u64 span = 0;
                for (u32 bi = 0; bi < nb; bi++) {
                    e->csz[bi] = get_le(b + o, 8); o += 8;
                    if (e->csz[bi] < BLK_OVER) { rc = SQUISH_E_FORMAT; break; }
                    span += e->csz[bi];
                }
                if (rc != SQUISH_OK) break;
                /* member blocks must lie inside the data region [HDR, index) */
                if (e->data_off < ARC_HDR || span > a->index_off - e->data_off) {
                    rc = SQUISH_E_FORMAT; break;
                }
            } else {
                e->csz = NULL;
                if (e->data_off) { rc = SQUISH_E_FORMAT; break; }
            }
        } else { rc = SQUISH_E_FORMAT; break; }

        if (plen > idn - o) { rc = SQUISH_E_FORMAT; break; }
        /* empty path allowed only for a lone unnamed blob member */
        if (plen == 0) {
            if (!(count == 1 && e->type == 0)) { rc = SQUISH_E_FORMAT; break; }
            e->path = (char*)malloc(1); if (e->path) e->path[0] = '\0';
        } else {
            if (plen > ARC_MAX_PATH ||
                !arc_path_safe((const char*)(b + o), plen)) { rc = SQUISH_E_FORMAT; break; }
            e->path = (char*)malloc((size_t)plen + 1);
            if (e->path) { memcpy(e->path, b + o, plen); e->path[plen] = '\0'; }
        }
        if (!e->path) { rc = SQUISH_E_NOMEM; break; }
        o += plen;
    }
    if (rc == SQUISH_OK && o != idn) rc = SQUISH_E_FORMAT;   /* must tile */
    free(idx);
    return rc;
}

static void arc_reader_free(squish_archive *a) {
    if (!a) return;
    if (a->f) fclose(a->f);
    if (a->ents) for (u64 i = 0; i < a->nents; i++) {
        free(a->ents[i].path); free(a->ents[i].csz);
    }
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
    out->chunk_size = a->chunk;
    return SQUISH_OK;
}

SQUISH_API uint64_t squish_archive_count(const squish_archive *a) {
    return a ? a->nents : 0;
}

static u64 ent_stored(const arc_ent *e) {
    u64 s = 0;
    for (u32 i = 0; i < e->nblk; i++) s += e->csz[i];
    return s;
}

SQUISH_API int squish_archive_stat(const squish_archive *a, uint64_t index,
                                   squish_archive_entry *out) {
    if (!a || !out || index >= a->nents) return SQUISH_E_PARAM;
    const arc_ent *e = &a->ents[index];
    out->path = e->path;
    out->size = e->orig;
    out->stored_size = ent_stored(e);
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

/* Extract member `index` into a fresh buffer. Shared by the public extract
 * calls and the single-member decompress helpers. */
static int arc_extract_idx(squish_archive *a, u64 index, int nthreads,
                           void **out, size_t *out_len,
                           squish_progress_fn cb, void *user) {
    *out = NULL; *out_len = 0;
    if (index >= a->nents) return SQUISH_E_PARAM;
    const arc_ent *e = &a->ents[index];
    if (e->type != 0) return SQUISH_E_PARAM;           /* directory */
    u64 span = ent_stored(e);
    const u8 *cp; u8 *own;
    int rc = arc_range(a, e->data_off, span, &cp, &own);
    if (rc != SQUISH_OK) return rc;
    u8 *dec = (u8*)malloc(e->orig ? (size_t)e->orig : 1);
    if (!dec) { free(own); return SQUISH_E_NOMEM; }
    rc = decompress_member(cp, span, e->csz, e->nblk, e->orig, a->chunk, dec,
                           nthreads, cb, user);
    free(own);
    if (rc != SQUISH_OK) { free(dec); return rc; }
    *out = dec; *out_len = (size_t)e->orig;
    return SQUISH_OK;
}

SQUISH_API int squish_archive_extract(squish_archive *a, uint64_t index,
                                      void **out, size_t *out_len) {
    if (!a || !out || !out_len) return SQUISH_E_PARAM;
    return arc_extract_idx(a, index, 0, out, out_len, NULL, NULL);
}

SQUISH_API int squish_archive_extract_path(squish_archive *a, const char *path,
                                           void **out, size_t *out_len) {
    if (!a || !path || !out || !out_len) return SQUISH_E_PARAM;
    *out = NULL; *out_len = 0;
    uint64_t i;
    int rc = squish_archive_find(a, path, &i);
    if (rc != SQUISH_OK) return rc;
    return arc_extract_idx(a, i, 0, out, out_len, NULL, NULL);
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
    rc = arc_extract_idx(a, i, 0, &buf, &n, NULL, NULL);
    if (rc != SQUISH_OK) return rc;
    rc = write_file_bytes(dst_path, (const u8*)buf, n, a->ents[i].mode) == 0
             ? SQUISH_OK : SQUISH_E_IO;
    squish_free(buf);
    return rc;
}

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
    char pbuf[ARC_MAX_PATH + 1];
    size_t plen = 0;
    if (prefix && *prefix) {
        plen = strlen(prefix);
        while (plen > 1 && prefix[plen-1] == '/') plen--;
        if (plen > ARC_MAX_PATH) return SQUISH_E_FORMAT;
        memcpy(pbuf, prefix, plen); pbuf[plen] = '\0';
    }
    const char *pfx = plen ? pbuf : "";

    u64 total = 0, processed = 0;
    int matched = 0;
    for (u64 i = 0; i < a->nents; i++)
        if (a->ents[i].path[0] && under_prefix(a->ents[i].path, pfx, plen)) {
            matched = 1;
            if (a->ents[i].type == 0) total += a->ents[i].orig;
        }
    if (!matched && plen) return SQUISH_E_FORMAT;      /* no such member */
    if (make_dir(dst_root) != 0) return SQUISH_E_IO;

    int rc = SQUISH_OK;
    for (u64 i = 0; i < a->nents && rc == SQUISH_OK; i++) {
        const arc_ent *e = &a->ents[i];
        if (!e->path[0] || !under_prefix(e->path, pfx, plen)) continue;
        if (e->type == 1) {
            if (make_dirs(dst_root, e->path, 1) != 0) rc = SQUISH_E_IO;
        } else {
            char *full = path_join(dst_root, e->path);
            if (!full) { rc = SQUISH_E_NOMEM; break; }
            void *buf; size_t n;
            rc = arc_extract_idx(a, i, 0, &buf, &n, NULL, NULL);
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

/* ---------------- directory -> archive ----------------------------------- */

/* Walk fs_dir, appending each member (dirs before their contents, siblings
 * sorted) to L with archive-relative path arc_pre/<name>; sum file sizes into
 * *total. Does not read file contents. */
static int arc_scan(const char *fs_dir, const char *arc_pre,
                    arc_list *L, u64 *total) {
    char **names; size_t n;
    if (list_dir(fs_dir, &names, &n) != 0) return SQUISH_E_IO;
    int rc = SQUISH_OK;
    for (size_t i = 0; i < n && rc == SQUISH_OK; i++) {
        char *fs  = path_join(fs_dir, names[i]);
        char *arc = path_join(arc_pre, names[i]);
        if (!fs || !arc) { free(fs); free(arc); rc = SQUISH_E_NOMEM; break; }
        if (strlen(arc) > ARC_MAX_PATH) { free(fs); free(arc); rc = SQUISH_E_FORMAT; break; }
        if (path_is_dir(fs)) {
            if (arc_list_add(L, 1, path_mode(fs, 0755), 0, arc) != 0) {
                rc = SQUISH_E_NOMEM;
            } else {
                rc = arc_scan(fs, L->e[L->n - 1].path, L, total);
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

SQUISH_API int squish_archive_create(const char *dir_path,
                                     const char *archive_path,
                                     int nthreads, size_t chunk_size,
                                     squish_progress_fn cb, void *user) {
    if (!dir_path || !archive_path) return SQUISH_E_PARAM;
    char *root = strip_trailing_sep(dir_path);
    if (!root) return SQUISH_E_NOMEM;
    u64 chunk = eff_chunk(chunk_size);

    arc_list L = { NULL, 0, 0 };
    u64 total = 0;
    int rc = arc_scan(root, "", &L, &total);
    if (rc != SQUISH_OK) { arc_list_free(&L); free(root); return rc; }

    FILE *f = fopen(archive_path, "wb+");
    if (!f) { arc_list_free(&L); free(root); return SQUISH_E_IO; }

    u8 hdr[ARC_HDR];
    memset(hdr, 0, sizeof hdr);
    memcpy(hdr, ARC_MAGIC, 8);
    put_le(hdr + 8, ARC_VERSION, 4);
    if (fwrite(hdr, 1, ARC_HDR, f) != ARC_HDR) { rc = SQUISH_E_IO; goto done; }
    u64 off = ARC_HDR, processed = 0;

    for (size_t i = 0; i < L.n; i++) {
        arc_ent *e = &L.e[i];
        if (e->type != 0) continue;                 /* directories: metadata only */
        char *fs = path_join(root, e->path);
        if (!fs) { rc = SQUISH_E_NOMEM; goto done; }
        u8 *data; size_t dl;
        rc = read_whole(fs, &data, &dl);
        free(fs);
        if (rc != SQUISH_OK) goto done;

        u8 **bufs; size_t *lens; u32 k;
        rc = compress_member(data, dl, nthreads, chunk, &bufs, &lens, &k, NULL, NULL);
        free(data);
        if (rc != SQUISH_OK) goto done;

        e->data_off = dl ? off : 0;
        e->orig = dl;
        e->nblk = k;
        e->csz = (u64*)malloc(k ? k * sizeof(u64) : 1);
        if (!e->csz) { for (u32 j=0;j<k;j++) free(bufs[j]); free(bufs); free(lens);
                       rc = SQUISH_E_NOMEM; goto done; }
        int ok = 1;
        for (u32 j = 0; j < k; j++) {
            e->csz[j] = lens[j];
            if (fwrite(bufs[j], 1, lens[j], f) != lens[j]) ok = 0;
            free(bufs[j]);
            off += lens[j];
        }
        free(bufs); free(lens);
        if (!ok) { rc = SQUISH_E_IO; goto done; }
        processed += dl;
        if (cb) cb(processed, total, user);
    }

    {   /* index: build, compress into one block, append */
        gbuf idx = { NULL, 0, 0 };
        rc = arc_build_index(&L, &idx);
        if (rc != SQUISH_OK) { free(idx.p); goto done; }
        gbuf iblk = { NULL, 0, 0 };
        u64 icl = 0, iorig = 0;
        rc = arc_write_index(&idx, &iblk, &icl, &iorig);
        free(idx.p);
        if (rc != SQUISH_OK) { free(iblk.p); goto done; }
        int ok = (iblk.len == 0) || (fwrite(iblk.p, 1, iblk.len, f) == iblk.len);
        free(iblk.p);
        if (!ok) { rc = SQUISH_E_IO; goto done; }

        arc_put_header(hdr, 0, (u64)L.n, total, chunk, off, icl, iorig);
        if (sq_fseek64(f, 0) != 0 || fwrite(hdr, 1, ARC_HDR, f) != ARC_HDR)
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

/* ---------------- in-memory archive -> buffer (single member) ------------ */
SQUISH_API int squish_decompress_alloc(const void *src, size_t src_len,
                                       void **dst, size_t *dst_len,
                                       int nthreads,
                                       squish_progress_fn progress, void *user) {
    if (!dst || !dst_len) return SQUISH_E_PARAM;
    *dst = NULL; *dst_len = 0;
    squish_archive *a;
    int rc = squish_archive_open_memory(src, src_len, &a);
    if (rc != SQUISH_OK) return rc;
    /* exactly one regular-file member */
    u64 fi = 0, nf = 0;
    for (u64 i = 0; i < a->nents; i++)
        if (a->ents[i].type == 0) { fi = i; nf++; }
    if (nf != 1) { squish_archive_close(a); return SQUISH_E_FORMAT; }
    rc = arc_extract_idx(a, fi, nthreads, dst, dst_len, progress, user);
    squish_archive_close(a);
    return rc;
}

/* ---------------- whole-file helpers ------------------------------------- */

/* Last path component, no directory/drive prefix; never empty. */
static const char *base_name(const char *name) {
    const char *b = name;
    for (const char *p = name; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':') b = p + 1;
    return (*b && strcmp(b, ".") && strcmp(b, "..")) ? b : "data";
}

SQUISH_API int squish_compress_file(const char *src_path, const char *dst_path,
                                    int nthreads, size_t chunk_size,
                                    squish_progress_fn progress, void *user) {
    if (!src_path || !dst_path) return SQUISH_E_PARAM;
    u8 *in; size_t n;
    int rc = read_whole(src_path, &in, &n);
    if (rc != SQUISH_OK) return rc;
    u64 chunk = eff_chunk(chunk_size);

    u8 **bufs; size_t *lens; u32 k;
    rc = compress_member(in, n, nthreads, chunk, &bufs, &lens, &k, progress, user);
    free(in);
    if (rc != SQUISH_OK) return rc;

    FILE *f = fopen(dst_path, "wb+");
    if (!f) { for (u32 i=0;i<k;i++) free(bufs[i]); free(bufs); free(lens);
              return SQUISH_E_IO; }

    u8 hdr[ARC_HDR];
    memset(hdr, 0, sizeof hdr);
    memcpy(hdr, ARC_MAGIC, 8); put_le(hdr + 8, ARC_VERSION, 4);
    int ok = fwrite(hdr, 1, ARC_HDR, f) == ARC_HDR;
    u64 data_off = ARC_HDR, off = ARC_HDR;
    for (u32 i = 0; i < k && ok; i++) {
        if (fwrite(bufs[i], 1, lens[i], f) != lens[i]) ok = 0;
        off += lens[i];
    }
    for (u32 i = 0; i < k; i++) free(bufs[i]);
    if (!ok) { free(bufs); free(lens); fclose(f); remove(dst_path); return SQUISH_E_IO; }

    {   /* one named file entry */
        arc_list L = { NULL, 0, 0 };
        const char *bn = base_name(src_path);
        size_t bl = strlen(bn);
        char *nm = (char*)malloc(bl + 1);
        if (!nm) { free(bufs); free(lens); fclose(f); remove(dst_path); return SQUISH_E_NOMEM; }
        memcpy(nm, bn, bl + 1);
        if (arc_list_add(&L, 0, 0644, n, nm)) { free(bufs); free(lens); fclose(f); remove(dst_path); return SQUISH_E_NOMEM; }
        L.e[0].data_off = n ? data_off : 0;
        L.e[0].nblk = k;
        L.e[0].csz = (u64*)malloc(k ? k * sizeof(u64) : 1);
        if (!L.e[0].csz) { arc_list_free(&L); free(bufs); free(lens); fclose(f); remove(dst_path); return SQUISH_E_NOMEM; }
        for (u32 i = 0; i < k; i++) L.e[0].csz[i] = lens[i];
        free(bufs); free(lens);

        gbuf idx = { NULL, 0, 0 };
        rc = arc_build_index(&L, &idx);
        arc_list_free(&L);
        if (rc != SQUISH_OK) { free(idx.p); fclose(f); remove(dst_path); return rc; }
        gbuf iblk = { NULL, 0, 0 };
        u64 icl = 0, iorig = 0;
        rc = arc_write_index(&idx, &iblk, &icl, &iorig);
        free(idx.p);
        if (rc != SQUISH_OK) { free(iblk.p); fclose(f); remove(dst_path); return rc; }
        ok = (iblk.len == 0) || (fwrite(iblk.p, 1, iblk.len, f) == iblk.len);
        free(iblk.p);
        if (ok) {
            arc_put_header(hdr, ARC_FLAG_SINGLE, 1, n, chunk, off, icl, iorig);
            if (sq_fseek64(f, 0) != 0 || fwrite(hdr, 1, ARC_HDR, f) != ARC_HDR) ok = 0;
        }
    }
    if (fclose(f) != 0) ok = 0;
    if (!ok) { remove(dst_path); return SQUISH_E_IO; }
    return SQUISH_OK;
}

SQUISH_API int squish_decompress_file(const char *src_path, const char *dst_path,
                                      int nthreads,
                                      squish_progress_fn progress, void *user) {
    if (!src_path || !dst_path) return SQUISH_E_PARAM;
    squish_archive *a;
    int rc = squish_archive_open(src_path, &a);
    if (rc != SQUISH_OK) return rc;
    u64 fi = 0, nf = 0;
    for (u64 i = 0; i < a->nents; i++)
        if (a->ents[i].type == 0) { fi = i; nf++; }
    if (nf != 1) { squish_archive_close(a); return SQUISH_E_FORMAT; }
    void *buf; size_t n;
    rc = arc_extract_idx(a, fi, nthreads, &buf, &n, progress, user);
    squish_archive_close(a);
    if (rc != SQUISH_OK) return rc;
    rc = write_file_bytes(dst_path, (const u8*)buf, n, 0) == 0
             ? SQUISH_OK : SQUISH_E_IO;
    squish_free(buf);
    return rc;
}
