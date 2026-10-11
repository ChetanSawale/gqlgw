/* gqlb.c - .gqlb writer and loader. */
#include "gqlb.h"
#include "sdl.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define ALIGN8(x) (((x) + 7u) & ~7u)

int gqlb_serialize(const gql_schema *s, gql_buf *out)
{
    gqlb_header h;
    const void *data[SEC_N];
    uint32_t sizes[SEC_N], counts[SEC_N], i, off;
    static const char zero[8] = { 0 };

    memset(&h, 0, sizeof h);
    memcpy(h.magic, GQLB_MAGIC, 4);
    h.version = GQLB_VERSION;
    h.endian = GQLB_ENDIAN;
    h.header_size = sizeof h;
    h.hash_lo = (uint32_t)s->hash;
    h.hash_hi = (uint32_t)(s->hash >> 32);
    h.query_type = s->query_type;
    h.mutation_type = s->mutation_type;
    h.subscription_type = s->subscription_type;
    h.f_typename = s->f_typename;
    h.f_schema = s->f_schema;
    h.f_type = s->f_type;
    h.desc = s->desc;

    data[SEC_TYPES] = s->types;   counts[SEC_TYPES] = s->ntypes;   sizes[SEC_TYPES] = sizeof(gql_type_rec);
    data[SEC_FIELDS] = s->fields; counts[SEC_FIELDS] = s->nfields; sizes[SEC_FIELDS] = sizeof(gql_field_rec);
    data[SEC_IVALS] = s->ivals;   counts[SEC_IVALS] = s->nivals;   sizes[SEC_IVALS] = sizeof(gql_ival_rec);
    data[SEC_EVALS] = s->evals;   counts[SEC_EVALS] = s->nevals;   sizes[SEC_EVALS] = sizeof(gql_eval_rec);
    data[SEC_TREFS] = s->trefs;   counts[SEC_TREFS] = s->ntrefs;   sizes[SEC_TREFS] = sizeof(gql_tref_rec);
    data[SEC_IDX] = s->idx;       counts[SEC_IDX] = s->nidx;       sizes[SEC_IDX] = sizeof(uint32_t);
    data[SEC_DIRS] = s->dirs;     counts[SEC_DIRS] = s->ndirs;     sizes[SEC_DIRS] = sizeof(gql_dir_rec);
    data[SEC_STR] = s->str;       counts[SEC_STR] = s->nstr;       sizes[SEC_STR] = 1;

    off = ALIGN8((uint32_t)sizeof h);
    for (i = 0; i < SEC_N; i++) {
        h.sec[i].off = off;
        h.sec[i].count = counts[i];
        h.sec[i].recsize = sizes[i];
        off = ALIGN8(off + counts[i] * sizes[i]);
    }
    h.file_size = off;

    buf_put(out, &h, sizeof h);
    for (i = 0; i < SEC_N; i++) {
        buf_put(out, zero, h.sec[i].off - out->len);
        if (counts[i])
            buf_put(out, data[i], (size_t)counts[i] * sizes[i]);
    }
    buf_put(out, zero, h.file_size - out->len);
    return out->oom ? -1 : 0;
}

int gql_gqlb_write(const gql_schema *s, const char *path, char *err, size_t errn)
{
    gql_buf b;
    FILE *f;
    char tmp[4096];
    buf_init(&b);
    if (gqlb_serialize(s, &b)) {
        snprintf(err, errn, "out of memory");
        buf_free(&b);
        return -1;
    }
    snprintf(tmp, sizeof tmp, "%s.tmp.%ld", path, (long)getpid());
    f = fopen(tmp, "wb");
    if (!f) {
        snprintf(err, errn, "%s: %s", tmp, strerror(errno));
        buf_free(&b);
        return -1;
    }
    if (fwrite(b.p, 1, b.len, f) != b.len) {
        snprintf(err, errn, "%s: %s", tmp, strerror(errno));
        fclose(f);
        unlink(tmp);
        buf_free(&b);
        return -1;
    }
    if (fclose(f)) {
        snprintf(err, errn, "%s: %s", tmp, strerror(errno));
        unlink(tmp);
        buf_free(&b);
        return -1;
    }
    buf_free(&b);
    if (rename(tmp, path)) {
        snprintf(err, errn, "%s: %s", path, strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* ---- loading ------------------------------------------------------------- */

typedef struct {
    const gql_schema *s;
    char *err;
    size_t errn;
    int bad;
} V;

static int vfail(V *v, const char *what, uint32_t i)
{
    if (!v->bad)
        snprintf(v->err, v->errn, "corrupted schema file: invalid %s #%u", what, i);
    v->bad = 1;
    return -1;
}

static int sref_ok(const gql_schema *s, gql_sref r)
{
    return (uint64_t)r.off + r.len < s->nstr && s->str[r.off + r.len] == 0;
}

static int range_ok(uint32_t first, uint32_t count, uint32_t n)
{
    return (uint64_t)first + count <= n;
}

static int validate(V *v)
{
    const gql_schema *s = v->s;
    uint32_t i, k;
    if (!s->nstr || s->str[s->nstr - 1] != 0)
        return vfail(v, "string pool", 0);
    for (i = 0; i < s->ntrefs; i++) {
        const gql_tref_rec *t = &s->trefs[i];
        if (t->kind > TR_NONNULL || (t->kind == TR_NAMED ? t->of >= s->ntypes : t->of >= i))
            return vfail(v, "type reference", i);
    }
    for (i = 0; i < s->ntypes; i++) {
        const gql_type_rec *t = &s->types[i];
        if (t->kind > GK_INPUT || t->scalar > SK_ID || !sref_ok(s, t->name) || !sref_ok(s, t->desc) ||
            !sref_ok(s, t->specified_by) ||
            !range_ok(t->fields_first, t->fields_count, t->kind == GK_INPUT ? s->nivals : s->nfields) ||
            !range_ok(t->ifaces_first, t->ifaces_count, s->nidx) ||
            !range_ok(t->possible_first, t->possible_count, s->nidx) ||
            !range_ok(t->evals_first, t->evals_count, s->nevals))
            return vfail(v, "type", i);
        for (k = 0; k < t->ifaces_count; k++)
            if (s->idx[t->ifaces_first + k] >= s->ntypes)
                return vfail(v, "type", i);
        for (k = 0; k < t->possible_count; k++)
            if (s->idx[t->possible_first + k] >= s->ntypes)
                return vfail(v, "type", i);
        if ((t->kind != GK_OBJECT && t->kind != GK_INTERFACE && t->kind != GK_INPUT && t->fields_count) ||
            (t->kind != GK_ENUM && t->evals_count))
            return vfail(v, "type", i);
    }
    for (i = 0; i < s->nfields; i++) {
        const gql_field_rec *f = &s->fields[i];
        if (!sref_ok(s, f->name) || !sref_ok(s, f->desc) || !sref_ok(s, f->deprecation) ||
            (f->owner != GQL_NONE && f->owner >= s->ntypes) || f->type >= s->ntrefs ||
            !range_ok(f->args_first, f->args_count, s->nivals))
            return vfail(v, "field", i);
    }
    for (i = 0; i < s->nivals; i++) {
        const gql_ival_rec *a = &s->ivals[i];
        if (!sref_ok(s, a->name) || !sref_ok(s, a->desc) || !sref_ok(s, a->deprecation) ||
            !sref_ok(s, a->defval) || !sref_ok(s, a->defjson) || a->type >= s->ntrefs)
            return vfail(v, "input value", i);
    }
    for (i = 0; i < s->nevals; i++) {
        const gql_eval_rec *e = &s->evals[i];
        if (!sref_ok(s, e->name) || !sref_ok(s, e->desc) || !sref_ok(s, e->deprecation) || e->owner >= s->ntypes)
            return vfail(v, "enum value", i);
    }
    for (i = 0; i < s->ndirs; i++) {
        const gql_dir_rec *d = &s->dirs[i];
        if (!sref_ok(s, d->name) || !sref_ok(s, d->desc) || !sref_ok(s, d->deprecation) ||
            !range_ok(d->args_first, d->args_count, s->nivals) || !range_ok(d->locs_first, d->locs_count, s->nidx))
            return vfail(v, "directive", i);
        for (k = 0; k < d->locs_count; k++)
            if (s->idx[d->locs_first + k] >= DL_COUNT)
                return vfail(v, "directive", i);
    }
    if (s->query_type >= s->ntypes || s->types[s->query_type].kind != GK_OBJECT)
        return vfail(v, "query root", 0);
    if (s->mutation_type != GQL_NONE && (s->mutation_type >= s->ntypes || s->types[s->mutation_type].kind != GK_OBJECT))
        return vfail(v, "mutation root", 0);
    if (s->subscription_type != GQL_NONE)
        return vfail(v, "subscription root", 0);
    if (s->f_typename >= s->nfields || s->f_schema >= s->nfields || s->f_type >= s->nfields ||
        !sref_ok(s, s->desc))
        return vfail(v, "meta field", 0);
    return 0;
}

static gql_schema *attach(gql_schema *s, const char *base, size_t n, char *err, size_t errn)
{
    const gqlb_header *h = (const gqlb_header *)base;
    static const uint32_t recsizes[SEC_N] = {
        sizeof(gql_type_rec), sizeof(gql_field_rec), sizeof(gql_ival_rec), sizeof(gql_eval_rec),
        sizeof(gql_tref_rec), sizeof(uint32_t), sizeof(gql_dir_rec), 1
    };
    uint32_t i;
    V v;

    if (n < sizeof *h || memcmp(h->magic, GQLB_MAGIC, 4) != 0) {
        snprintf(err, errn, "not a gqlb file");
        return NULL;
    }
    if (h->endian != GQLB_ENDIAN || h->version != GQLB_VERSION || h->header_size != sizeof *h ||
        h->file_size != n) {
        snprintf(err, errn, "unsupported gqlb file (version %u, rebuild it with this gqlc)", h->version);
        return NULL;
    }
    for (i = 0; i < SEC_N; i++) {
        if (h->sec[i].recsize != recsizes[i] || (h->sec[i].off & 7u) ||
            (uint64_t)h->sec[i].off + (uint64_t)h->sec[i].count * recsizes[i] > n) {
            snprintf(err, errn, "corrupted gqlb file (section %u)", i);
            return NULL;
        }
    }
    s->types = (const gql_type_rec *)(base + h->sec[SEC_TYPES].off);   s->ntypes = h->sec[SEC_TYPES].count;
    s->fields = (const gql_field_rec *)(base + h->sec[SEC_FIELDS].off); s->nfields = h->sec[SEC_FIELDS].count;
    s->ivals = (const gql_ival_rec *)(base + h->sec[SEC_IVALS].off);   s->nivals = h->sec[SEC_IVALS].count;
    s->evals = (const gql_eval_rec *)(base + h->sec[SEC_EVALS].off);   s->nevals = h->sec[SEC_EVALS].count;
    s->trefs = (const gql_tref_rec *)(base + h->sec[SEC_TREFS].off);   s->ntrefs = h->sec[SEC_TREFS].count;
    s->idx = (const uint32_t *)(base + h->sec[SEC_IDX].off);           s->nidx = h->sec[SEC_IDX].count;
    s->dirs = (const gql_dir_rec *)(base + h->sec[SEC_DIRS].off);      s->ndirs = h->sec[SEC_DIRS].count;
    s->str = base + h->sec[SEC_STR].off;                               s->nstr = h->sec[SEC_STR].count;
    s->query_type = h->query_type;
    s->mutation_type = h->mutation_type;
    s->subscription_type = h->subscription_type;
    s->f_typename = h->f_typename;
    s->f_schema = h->f_schema;
    s->f_type = h->f_type;
    s->desc = h->desc;
    s->hash = ((uint64_t)h->hash_hi << 32) | h->hash_lo;
    v.s = s;
    v.err = err;
    v.errn = errn;
    v.bad = 0;
    if (validate(&v))
        return NULL;
    if (gql_schema_index(s)) {
        snprintf(err, errn, "out of memory");
        return NULL;
    }
    if (s->t_string == GQL_NONE || s->t_boolean == GQL_NONE || s->t_schema == GQL_NONE || s->t_type == GQL_NONE) {
        snprintf(err, errn, "corrupted gqlb file (missing built-in types)");
        gql_schema_unindex(s);
        return NULL;
    }
    return s;
}

gql_schema *gqlb_open(const char *path, char *err, size_t errn)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    void *m;
    gql_schema *s;

    if (fd < 0) {
        snprintf(err, errn, "%s: %s", path, strerror(errno));
        return NULL;
    }
    if (fstat(fd, &st) || st.st_size <= 0) {
        snprintf(err, errn, "%s: cannot stat or empty file", path);
        close(fd);
        return NULL;
    }
    m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) {
        snprintf(err, errn, "%s: mmap: %s", path, strerror(errno));
        return NULL;
    }
    s = calloc(1, sizeof *s);
    if (!s) {
        munmap(m, (size_t)st.st_size);
        snprintf(err, errn, "out of memory");
        return NULL;
    }
    s->map = m;
    s->maplen = (size_t)st.st_size;
    if (!attach(s, m, (size_t)st.st_size, err, errn)) {
        munmap(m, (size_t)st.st_size);
        free(s);
        return NULL;
    }
    return s;
}

gql_schema *gqlb_load(const void *data, size_t n, char *err, size_t errn)
{
    gql_schema *s = calloc(1, sizeof *s);
    void *copy = malloc(n ? n : 1);
    if (!s || !copy) {
        free(s);
        free(copy);
        snprintf(err, errn, "out of memory");
        return NULL;
    }
    memcpy(copy, data, n);
    s->owned = copy;
    if (!attach(s, copy, n, err, errn)) {
        free(copy);
        free(s);
        return NULL;
    }
    return s;
}

void gqlb_close(gql_schema *s)
{
    if (!s)
        return;
    gql_schema_unindex(s);
    if (s->map)
        munmap(s->map, s->maplen);
    free(s->owned);
    free(s);
}
