/* util.c - arena, buffers, hashing, hash map. */
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- arena ------------------------------------------------------------ */

#define ARENA_ALIGN 16
#define ARENA_CHUNK (64 * 1024)
#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((size_t)(a) - 1))

void arena_init(gql_arena *a, size_t limit)
{
    a->head = NULL;
    a->total = 0;
    a->limit = limit;
    a->oom = 0;
}

void *arena_alloc(gql_arena *a, size_t n)
{
    size_t hdr = ALIGN_UP(sizeof(gql_arena_chunk), ARENA_ALIGN);
    gql_arena_chunk *c = a->head;
    void *p;

    if (a->oom)
        return NULL;
    n = ALIGN_UP(n ? n : 1, ARENA_ALIGN);
    if (!c || c->used + n > c->cap) {
        size_t cap = n > ARENA_CHUNK ? n : ARENA_CHUNK;
        if (a->limit && a->total + cap > a->limit) {
            a->oom = 1;
            return NULL;
        }
        c = malloc(hdr + cap);
        if (!c) {
            a->oom = 1;
            return NULL;
        }
        c->cap = cap;
        c->used = 0;
        c->next = a->head;
        a->head = c;
        a->total += cap;
    }
    p = (char *)c + hdr + c->used;
    c->used += n;
    memset(p, 0, n);
    return p;
}

char *arena_strndup(gql_arena *a, const char *s, size_t n)
{
    char *p = arena_alloc(a, n + 1);
    if (!p)
        return NULL;
    if (n)
        memcpy(p, s, n);
    p[n] = 0;
    return p;
}

void arena_free(gql_arena *a)
{
    gql_arena_chunk *c = a->head, *nx;
    while (c) {
        nx = c->next;
        free(c);
        c = nx;
    }
    a->head = NULL;
    a->total = 0;
}

/* ---- buffer ----------------------------------------------------------- */

void buf_init(gql_buf *b)
{
    b->p = NULL;
    b->len = b->cap = 0;
    b->oom = 0;
}

void buf_free(gql_buf *b)
{
    free(b->p);
    buf_init(b);
}

void buf_reset(gql_buf *b)
{
    b->len = 0;
}

int buf_reserve(gql_buf *b, size_t extra)
{
    size_t need = b->len + extra + 1, cap;
    char *np;
    if (b->oom)
        return -1;
    if (need <= b->cap)
        return 0;
    cap = b->cap ? b->cap : 256;
    while (cap < need)
        cap *= 2;
    np = realloc(b->p, cap);
    if (!np) {
        b->oom = 1;
        return -1;
    }
    b->p = np;
    b->cap = cap;
    return 0;
}

void buf_put(gql_buf *b, const void *p, size_t n)
{
    if (buf_reserve(b, n))
        return;
    if (n)
        memcpy(b->p + b->len, p, n);
    b->len += n;
}

void buf_puts(gql_buf *b, const char *s)
{
    buf_put(b, s, strlen(s));
}

void buf_putc(gql_buf *b, char c)
{
    if (buf_reserve(b, 1))
        return;
    b->p[b->len++] = c;
}

void buf_vprintf(gql_buf *b, const char *fmt, va_list ap)
{
    va_list ap2;
    int n;
    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0 || buf_reserve(b, (size_t)n))
        return;
    vsnprintf(b->p + b->len, (size_t)n + 1, fmt, ap);
    b->len += (size_t)n;
}

void buf_printf(gql_buf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    buf_vprintf(b, fmt, ap);
    va_end(ap);
}

static const char HEX[] = "0123456789abcdef";

void buf_json_str(gql_buf *b, const char *s, size_t n)
{
    size_t i, start = 0;
    if (buf_reserve(b, n + 2))
        return;
    buf_putc(b, '"');
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        const char *esc = NULL;
        char ubuf[6];
        if (c >= 0x20 && c != '"' && c != '\\')
            continue;
        buf_put(b, s + start, i - start);
        switch (c) {
        case '"': esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\b': esc = "\\b"; break;
        case '\f': esc = "\\f"; break;
        case '\n': esc = "\\n"; break;
        case '\r': esc = "\\r"; break;
        case '\t': esc = "\\t"; break;
        default:
            ubuf[0] = '\\'; ubuf[1] = 'u'; ubuf[2] = '0'; ubuf[3] = '0';
            ubuf[4] = HEX[c >> 4]; ubuf[5] = HEX[c & 15];
            buf_put(b, ubuf, 6);
        }
        if (esc)
            buf_puts(b, esc);
        start = i + 1;
    }
    buf_put(b, s + start, n - start);
    buf_putc(b, '"');
}

void buf_gql_str(gql_buf *b, const char *s, size_t n)
{
    /* graphql-js printString: escapes control chars as \uXXXX except \b \f \n \r \t,
       plus " and \ ; identical to JSON for our purposes, but uses uppercase hex. */
    size_t i;
    buf_putc(b, '"');
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': buf_puts(b, "\\\""); break;
        case '\\': buf_puts(b, "\\\\"); break;
        case '\b': buf_puts(b, "\\b"); break;
        case '\f': buf_puts(b, "\\f"); break;
        case '\n': buf_puts(b, "\\n"); break;
        case '\r': buf_puts(b, "\\r"); break;
        case '\t': buf_puts(b, "\\t"); break;
        default:
            if (c < 0x20 || c == 0x7f)
                buf_printf(b, "\\u%04X", c);
            else
                buf_putc(b, (char)c);
        }
    }
    buf_putc(b, '"');
}

const char *buf_cstr(gql_buf *b)
{
    if (buf_reserve(b, 0))
        return "";
    b->p[b->len] = 0;
    return b->p;
}

/* ---- vec -------------------------------------------------------------- */

void vec_init(gql_vec *v, uint32_t elem)
{
    v->p = NULL;
    v->len = v->cap = 0;
    v->elem = elem;
    v->oom = 0;
}

void *vec_push(gql_vec *v)
{
    void *slot;
    if (v->oom)
        return NULL;
    if (v->len == v->cap) {
        uint32_t cap = v->cap ? v->cap * 2 : 16;
        void *np = realloc(v->p, (size_t)cap * v->elem);
        if (!np) {
            v->oom = 1;
            return NULL;
        }
        v->p = np;
        v->cap = cap;
    }
    slot = (char *)v->p + (size_t)v->len * v->elem;
    memset(slot, 0, v->elem);
    v->len++;
    return slot;
}

void vec_free(gql_vec *v)
{
    free(v->p);
    v->p = NULL;
    v->len = v->cap = 0;
}

/* ---- hashing ---------------------------------------------------------- */

uint64_t fnv1a64(const void *p, size_t n, uint64_t h)
{
    const unsigned char *s = p;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= s[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

static uint32_t hm_hash(uint32_t scope, const char *k, uint32_t n)
{
    uint64_t h = fnv1a64(&scope, sizeof scope, FNV64_INIT);
    uint32_t r;
    h = fnv1a64(k, n, h);
    r = (uint32_t)(h ^ (h >> 32));
    return r ? r : 1;
}

void hm_init(gql_hmap *m)
{
    m->slots = NULL;
    m->cap = m->count = 0;
    m->oom = 0;
    m->base = NULL;
}

void hm_free(gql_hmap *m)
{
    const char *const *base = m->base;
    free(m->slots);
    hm_init(m);
    m->base = base;
}

static inline const char *hm_key(const gql_hmap *m, const gql_hslot *s)
{
    return m->base ? *m->base + (size_t)(uintptr_t)s->key : s->key;
}

static int hm_grow(gql_hmap *m)
{
    uint32_t ncap = m->cap ? m->cap * 2 : 64, i;
    gql_hslot *ns = calloc(ncap, sizeof *ns);
    if (!ns) {
        m->oom = 1;
        return -1;
    }
    for (i = 0; i < m->cap; i++) {
        gql_hslot *s = &m->slots[i];
        uint32_t j;
        if (!s->hash)
            continue;
        j = s->hash & (ncap - 1);
        while (ns[j].hash)
            j = (j + 1) & (ncap - 1);
        ns[j] = *s;
    }
    free(m->slots);
    m->slots = ns;
    m->cap = ncap;
    return 0;
}

static int hm_put_raw(gql_hmap *m, uint32_t scope, const char *k, uint32_t n, uint32_t val, const char *stored);

int hm_put(gql_hmap *m, uint32_t scope, const char *k, uint32_t n, uint32_t val)
{
    return hm_put_raw(m, scope, k, n, val, k);
}

int hm_put_off(gql_hmap *m, uint32_t scope, uint32_t off, uint32_t n, uint32_t val)
{
    return hm_put_raw(m, scope, *m->base + off, n, val, (const char *)(uintptr_t)off);
}

static int hm_put_raw(gql_hmap *m, uint32_t scope, const char *k, uint32_t n, uint32_t val, const char *stored)
{
    uint32_t h, j;
    if ((m->count + 1) * 4 >= m->cap * 3 && hm_grow(m))
        return -1;
    h = hm_hash(scope, k, n);
    j = h & (m->cap - 1);
    while (m->slots[j].hash) {
        gql_hslot *s = &m->slots[j];
        if (s->hash == h && s->scope == scope && s->len == n && memcmp(hm_key(m, s), k, n) == 0)
            return 1;
        j = (j + 1) & (m->cap - 1);
    }
    m->slots[j].hash = h;
    m->slots[j].scope = scope;
    m->slots[j].key = stored;
    m->slots[j].len = n;
    m->slots[j].val = val;
    m->count++;
    return 0;
}

uint32_t hm_get(const gql_hmap *m, uint32_t scope, const char *k, uint32_t n)
{
    uint32_t h, j;
    if (!m->cap)
        return GQL_NONE;
    h = hm_hash(scope, k, n);
    j = h & (m->cap - 1);
    while (m->slots[j].hash) {
        const gql_hslot *s = &m->slots[j];
        if (s->hash == h && s->scope == scope && s->len == n && memcmp(hm_key(m, s), k, n) == 0)
            return s->val;
        j = (j + 1) & (m->cap - 1);
    }
    return GQL_NONE;
}

/* ---- UTF-8 ------------------------------------------------------------ */

int utf8_encode(uint32_t cp, char out[4])
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

int utf8_valid_seq(const unsigned char *p, size_t n)
{
    unsigned char c = p[0];
    if (c < 0x80)
        return 1;
    if (c >= 0xC2 && c <= 0xDF) {
        if (n < 2 || (p[1] & 0xC0) != 0x80)
            return 0;
        return 2;
    }
    if (c >= 0xE0 && c <= 0xEF) {
        if (n < 3 || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80)
            return 0;
        if (c == 0xE0 && p[1] < 0xA0)
            return 0;   /* overlong */
        if (c == 0xED && p[1] >= 0xA0)
            return 0;   /* surrogates */
        return 3;
    }
    if (c >= 0xF0 && c <= 0xF4) {
        if (n < 4 || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 || (p[3] & 0xC0) != 0x80)
            return 0;
        if (c == 0xF0 && p[1] < 0x90)
            return 0;
        if (c == 0xF4 && p[1] >= 0x90)
            return 0;
        return 4;
    }
    return 0;
}
