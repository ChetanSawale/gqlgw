/* util.h - arena, growable buffers, hashing, hash map. GNU C99, no deps. */
#ifndef GQL_UTIL_H
#define GQL_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

#define GQL_NONE UINT32_MAX

#if defined(__GNUC__)
#define GQL_PRINTF(a, b) __attribute__((format(printf, a, b)))
#define GQL_UNUSED __attribute__((unused))
#else
#define GQL_PRINTF(a, b)
#define GQL_UNUSED
#endif

/* ---- arena ------------------------------------------------------------ */

typedef struct gql_arena_chunk {
    struct gql_arena_chunk *next;
    size_t cap, used;
    /* data follows, 16-byte aligned */
} gql_arena_chunk;

typedef struct {
    gql_arena_chunk *head;
    size_t total;   /* bytes reserved so far */
    size_t limit;   /* 0 = unlimited */
    int oom;        /* set once an allocation failed */
} gql_arena;

void  arena_init(gql_arena *a, size_t limit);
void *arena_alloc(gql_arena *a, size_t n);              /* zeroed, NULL on failure */
char *arena_strndup(gql_arena *a, const char *s, size_t n);
void  arena_free(gql_arena *a);

/* ---- growable byte buffer --------------------------------------------- */

typedef struct {
    char *p;
    size_t len, cap;
    int oom;
} gql_buf;

void buf_init(gql_buf *b);
void buf_free(gql_buf *b);
void buf_reset(gql_buf *b);
int  buf_reserve(gql_buf *b, size_t extra);
void buf_put(gql_buf *b, const void *p, size_t n);
void buf_puts(gql_buf *b, const char *s);
void buf_putc(gql_buf *b, char c);
void buf_printf(gql_buf *b, const char *fmt, ...) GQL_PRINTF(2, 3);
void buf_vprintf(gql_buf *b, const char *fmt, va_list ap);
/* JSON string literal with quotes; input is assumed to be valid UTF-8 */
void buf_json_str(gql_buf *b, const char *s, size_t n);
/* GraphQL string literal (as printed by graphql-js) */
void buf_gql_str(gql_buf *b, const char *s, size_t n);
const char *buf_cstr(gql_buf *b);                       /* NUL-terminates */

/* ---- generic growable array ------------------------------------------- */

typedef struct {
    void *p;
    uint32_t len, cap;
    uint32_t elem;
    int oom;
} gql_vec;

void  vec_init(gql_vec *v, uint32_t elem);
void *vec_push(gql_vec *v);                             /* zeroed slot or NULL */
void  vec_free(gql_vec *v);
#define VEC_AT(v, T, i) (((T *)(v)->p)[i])

/* ---- hashing ---------------------------------------------------------- */

uint64_t fnv1a64(const void *p, size_t n, uint64_t h);
#define FNV64_INIT 0xcbf29ce484222325ULL

/* ---- hash map: (scope u32, bytes) -> u32 ------------------------------- */

typedef struct {
    const char *key;     /* not owned */
    uint32_t len;
    uint32_t scope;
    uint32_t val;
    uint32_t hash;       /* 0 = empty slot (hash forced non-zero) */
} gql_hslot;

typedef struct {
    gql_hslot *slots;
    uint32_t cap, count;
    int oom;
    /* when set, slot keys are offsets relative to *base (a relocatable pool) */
    const char *const *base;
} gql_hmap;

void     hm_init(gql_hmap *m);
void     hm_free(gql_hmap *m);
/* returns 0 if inserted, 1 if key already present (value untouched), -1 oom */
int      hm_put(gql_hmap *m, uint32_t scope, const char *k, uint32_t n, uint32_t val);
uint32_t hm_get(const gql_hmap *m, uint32_t scope, const char *k, uint32_t n);
/* insert a key stored as an offset into *m->base (m->base must be set) */
int      hm_put_off(gql_hmap *m, uint32_t scope, uint32_t off, uint32_t n, uint32_t val);

/* ---- UTF-8 ------------------------------------------------------------ */

/* encode code point, returns bytes written (1..4) */
int  utf8_encode(uint32_t cp, char out[4]);
/* length of a valid UTF-8 sequence at p (<= n), 0 if invalid */
int  utf8_valid_seq(const unsigned char *p, size_t n);

#endif
