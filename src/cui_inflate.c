/* A small DEFLATE (RFC 1951) decoder, used to unpack the embedded fonts and
 * public as cui_inflate (raw or zlib streams, into a fixed buffer or one that
 * grows, optionally stopping when the output is full). Every read and write is
 * bounds checked, so corrupt input fails cleanly. */
#include "cui_internal.h"

#include <stdlib.h>

enum { CUI__INF_GROW = 1, CUI__INF_STOP = 2 };   /* cui__inf.mode bits */

typedef struct {
    const unsigned char *src, *src_end;
    unsigned char *out, *out_end, *out_start;
    unsigned bits;
    int nbits;
    unsigned mode;     /* 0 for cui__inflate: a full output is an error */
    size_t max_out;    /* CUI__INF_GROW: the buffer never grows past this */
    int stopped;       /* CUI__INF_STOP: the output filled up before the end */
} cui__inf;

#define CUI__INF_FAST 9   /* codes up to this many bits decode with one table lookup */

typedef struct {
    unsigned short count[16];
    unsigned short symbol[288];
    unsigned short fast[1 << CUI__INF_FAST];   /* symbol | length << 12, 0 = use the slow path */
} cui__huff;

/* returns -1 past the end of input */
static int cui__inf_bits(cui__inf *s, int need) {
    unsigned v = s->bits;
    while (s->nbits < need) {
        if (s->src >= s->src_end) return -1;
        v |= (unsigned)*s->src++ << s->nbits;
        s->nbits += 8;
    }
    s->bits = need < 32 ? v >> need : 0;
    s->nbits -= need;
    return (int)(v & ((1u << need) - 1));
}

static void cui__inf_build(cui__huff *h, const unsigned char *len, int n) {
    unsigned short offs[16];
    memset(h, 0, sizeof *h);
    for (int i = 0; i < n; i++) h->count[len[i]]++;
    h->count[0] = 0;
    offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = (unsigned short)(offs[i] + h->count[i]);
    for (int i = 0; i < n; i++)
        if (len[i]) h->symbol[offs[len[i]]++] = (unsigned short)i;
    /* canonical codes, bit reversed because DEFLATE packs them LSB first */
    int code = 0, idx = 0;
    for (int l = 1; l <= CUI__INF_FAST; l++) {
        for (int k = 0; k < h->count[l]; k++, code++, idx++) {
            if (code >= (1 << l)) return;          /* oversubscribed: leave it to the slow path */
            int rev = 0;
            for (int b = 0; b < l; b++) rev |= ((code >> b) & 1) << (l - 1 - b);
            for (int f = rev; f < (1 << CUI__INF_FAST); f += 1 << l)
                h->fast[f] = (unsigned short)(h->symbol[idx] | l << 12);
        }
        code <<= 1;
    }
}

static int cui__inf_decode(cui__inf *s, const cui__huff *h) {
    while (s->nbits <= 24 && s->src < s->src_end) {
        s->bits |= (unsigned)*s->src++ << s->nbits;
        s->nbits += 8;
    }
    unsigned e = h->fast[s->bits & ((1u << CUI__INF_FAST) - 1)];
    if (e && (int)(e >> 12) <= s->nbits) {
        s->bits >>= e >> 12;
        s->nbits -= (int)(e >> 12);
        return (int)(e & 0xFFF);
    }
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        int b = cui__inf_bits(s, 1);
        if (b < 0) return -1;
        code |= b;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

/* Makes room for need more bytes of output: 0 when they fit now, 1 when the output is full and the
 * caller should stop (CUI__INF_STOP), -1 when it is an error. */
static int cui__inf_full(cui__inf *s, size_t need) {
    size_t used = (size_t)(s->out - s->out_start), cap = (size_t)(s->out_end - s->out_start);
    if ((s->mode & CUI__INF_GROW) && cap < s->max_out) {
        size_t want = cap < 256 ? 256 : cap * 2;
        if (want < used + need) want = used + need;
        if (want > s->max_out || want < cap) want = s->max_out;
        unsigned char *p = (unsigned char *)realloc(s->out_start, want ? want : 1);
        if (!p) return -1;
        s->out_start = p;
        s->out = p + used;
        s->out_end = p + want;
        if (want - used >= need) return 0;
    }
    if (s->mode & CUI__INF_STOP) { s->stopped = 1; return 1; }
    return -1;
}

/* 0 at the end of the block, 1 stopped with the output full, -1 on an error */
static int cui__inf_codes(cui__inf *s, const cui__huff *lit, const cui__huff *dist) {
    static const unsigned short lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                              35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
    static const unsigned char lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                            3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
    static const unsigned short dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
                                              513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
    static const unsigned char dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7,
                                            8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
    for (;;) {
        int sym = cui__inf_decode(s, lit);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (s->out >= s->out_end) {
                int r = cui__inf_full(s, 1);
                if (r) return r;
            }
            *s->out++ = (unsigned char)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            int e = cui__inf_bits(s, lext[sym]);
            if (e < 0) return -1;
            int len = lbase[sym] + e;
            int ds = cui__inf_decode(s, dist);
            if (ds < 0 || ds >= 30) return -1;
            e = cui__inf_bits(s, dext[ds]);
            if (e < 0) return -1;
            int d = dbase[ds] + e;
            if (d > s->out - s->out_start) return -1;
            int stop = 0;
            if (len > s->out_end - s->out) {
                stop = cui__inf_full(s, (size_t)len);
                if (stop < 0) return -1;
                if (stop) len = (int)(s->out_end - s->out);
            }
            for (int i = 0; i < len; i++, s->out++) *s->out = s->out[-d];
            if (stop) return 1;
        }
    }
}

static int cui__inf_dynamic(cui__inf *s, cui__huff *lit, cui__huff *dist) {
    static const unsigned char order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    unsigned char lens[320];
    int nlen = cui__inf_bits(s, 5), ndist = cui__inf_bits(s, 5), ncode = cui__inf_bits(s, 4);
    if (nlen < 0 || ndist < 0 || ncode < 0) return -1;
    nlen += 257; ndist += 1; ncode += 4;
    if (nlen > 286 || ndist > 30) return -1;
    memset(lens, 0, sizeof lens);
    for (int i = 0; i < ncode; i++) {
        int v = cui__inf_bits(s, 3);
        if (v < 0) return -1;
        lens[order[i]] = (unsigned char)v;
    }
    cui__inf_build(lit, lens, 19);
    int i = 0;
    while (i < nlen + ndist) {
        int sym = cui__inf_decode(s, lit);
        if (sym < 0) return -1;
        if (sym < 16) { lens[i++] = (unsigned char)sym; continue; }
        int val = 0, rep;
        if (sym == 16) {
            if (i == 0) return -1;
            val = lens[i - 1];
            rep = cui__inf_bits(s, 2); if (rep < 0) return -1; rep += 3;
        } else if (sym == 17) {
            rep = cui__inf_bits(s, 3); if (rep < 0) return -1; rep += 3;
        } else {
            rep = cui__inf_bits(s, 7); if (rep < 0) return -1; rep += 11;
        }
        if (i + rep > nlen + ndist) return -1;
        while (rep--) lens[i++] = (unsigned char)val;
    }
    if (lens[256] == 0) return -1;
    cui__inf_build(lit, lens, nlen);
    cui__inf_build(dist, lens + nlen, ndist);
    return cui__inf_codes(s, lit, dist);
}

/* Decodes blocks until the last one: 0 done, 1 stopped with the output full, -1 on an error.
 * On 0 and 1, whole bytes read ahead into the bit buffer are given back to s->src. */
static int cui__inf_run(cui__inf *s) {
    cui__huff lit, dist;
    int last;
    do {
        last = cui__inf_bits(s, 1);
        int type = cui__inf_bits(s, 2);
        if (last < 0 || type < 0) return -1;
        int err;
        if (type == 0) {
            /* stored blocks start on a byte boundary; give back whole bytes read ahead */
            s->src -= s->nbits >> 3;
            s->bits = 0; s->nbits = 0;
            if (s->src_end - s->src < 4) return -1;
            unsigned len = s->src[0] | (s->src[1] << 8), nlen = s->src[2] | (s->src[3] << 8);
            s->src += 4;
            if (len != (~nlen & 0xFFFFu) || len > (size_t)(s->src_end - s->src)) return -1;
            err = 0;
            if (len > (size_t)(s->out_end - s->out)) {
                err = cui__inf_full(s, len);
                if (err < 0) return -1;
                if (err) len = (unsigned)(s->out_end - s->out);
            }
            memcpy(s->out, s->src, len);
            s->out += len; s->src += len;
        } else if (type == 1) {
            unsigned char lens[320];
            int i = 0;
            for (; i < 144; i++) lens[i] = 8;
            for (; i < 256; i++) lens[i] = 9;
            for (; i < 280; i++) lens[i] = 7;
            for (; i < 288; i++) lens[i] = 8;
            for (; i < 318; i++) lens[i] = 5;
            cui__inf_build(&lit, lens, 288);
            cui__inf_build(&dist, lens + 288, 30);
            err = cui__inf_codes(s, &lit, &dist);
        } else if (type == 2) {
            err = cui__inf_dynamic(s, &lit, &dist);
        } else {
            return -1;
        }
        if (err < 0) return -1;
        if (err > 0) break;
    } while (!last);
    s->src -= s->nbits >> 3;
    s->bits = 0; s->nbits = 0;
    return s->stopped ? 1 : 0;
}

size_t cui__inflate(const void *src, size_t src_len, void *dst, size_t dst_cap) {
    cui__inf s;
    memset(&s, 0, sizeof s);
    s.src = (const unsigned char *)src;
    s.src_end = s.src + src_len;
    s.out = s.out_start = (unsigned char *)dst;
    s.out_end = s.out + dst_cap;
    if (cui__inf_run(&s) != 0) return 0;
    return (size_t)(s.out - s.out_start);
}

/* ---- public: cui_inflate, cui_inflate_alloc ---- */

static uint32_t cui__adler32(const unsigned char *p, size_t n) {
    uint32_t a = 1, b = 0;
    while (n) {
        size_t k = n < 5552 ? n : 5552;   /* the most bytes before the sums can overflow 32 bits */
        n -= k;
        while (k--) { a += *p++; b += a; }
        a %= 65521u;
        b %= 65521u;
    }
    return b << 16 | a;
}

static cui_inflate_result cui__inf_public(cui__inf *s, size_t src_len, unsigned flags) {
    cui_inflate_result r = { CUI_INFLATE_ERROR, 0, 0 };
    const unsigned char *start = s->src;
    s->src_end = start + src_len;
    if (flags & CUI_INFLATE_STOP_WHEN_FULL) s->mode |= CUI__INF_STOP;
    if (flags & CUI_INFLATE_ZLIB) {
        /* CMF: method 8 (deflate), window up to 32 KB; FLG: no preset dictionary, header a multiple of 31 */
        if (src_len < 2) return r;
        unsigned cmf = start[0], flg = start[1];
        if ((cmf & 15) != 8 || (cmf >> 4) > 7 || (flg & 0x20) || ((cmf << 8) | flg) % 31) return r;
        s->src += 2;
    }
    int st = cui__inf_run(s);
    r.out_len = (size_t)(s->out - s->out_start);
    if (st == 0 && (flags & CUI_INFLATE_ZLIB)) {
        if (s->src_end - s->src < 4) {
            st = -1;
        } else {
            uint32_t want = (uint32_t)s->src[0] << 24 | (uint32_t)s->src[1] << 16 | (uint32_t)s->src[2] << 8 | s->src[3];
            s->src += 4;
            if (!(flags & CUI_INFLATE_NO_CHECK) && cui__adler32(s->out_start, r.out_len) != want) st = -1;
        }
    }
    r.status = st < 0 ? CUI_INFLATE_ERROR : st ? CUI_INFLATE_FULL : CUI_INFLATE_OK;
    r.in_used = (size_t)(s->src - start);
    return r;
}

cui_inflate_result cui_inflate(const void *src, size_t src_len, void *dst, size_t dst_cap, unsigned flags) {
    cui__inf s;
    memset(&s, 0, sizeof s);
    s.src = (const unsigned char *)src;
    s.out = s.out_start = (unsigned char *)dst;
    s.out_end = s.out + dst_cap;
    return cui__inf_public(&s, src_len, flags);
}

void *cui_inflate_alloc(const void *src, size_t src_len, unsigned flags, size_t size_hint, size_t max_out,
                        cui_inflate_result *result) {
    cui__inf s;
    memset(&s, 0, sizeof s);
    if (!max_out) max_out = (size_t)-1;
    size_t cap = size_hint ? size_hint : (src_len < ((size_t)-1) / 4 ? src_len * 4 : src_len);
    if (cap < 64) cap = 64;
    if (cap > max_out) cap = max_out;
    unsigned char *buf = (unsigned char *)malloc(cap ? cap : 1);
    cui_inflate_result r = { CUI_INFLATE_ERROR, 0, 0 };
    if (buf) {
        s.src = (const unsigned char *)src;
        s.out = s.out_start = buf;
        s.out_end = buf + cap;
        s.mode = CUI__INF_GROW;
        s.max_out = max_out;
        r = cui__inf_public(&s, src_len, flags);
        buf = s.out_start;   /* it may have moved as it grew */
        if (r.status == CUI_INFLATE_ERROR) { free(buf); buf = NULL; }
    }
    if (result) *result = r;
    return buf;
}
