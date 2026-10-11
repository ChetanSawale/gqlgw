/* api.c - public API (gql.h). */
#include "gql.h"
#include "errors.h"
#include "gqlb.h"
#include "parser.h"
#include "validate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct gql_doc {
    gql_arena arena;
    ast_doc *ast;
    int ok;
    gql_buf errors;
    gql_hmap frags;
    const ast_def **fragdefs;
    uint32_t nfrags;
};

gql_plan *gql_plan_build_internal(const gql_schema *s, const ast_doc *doc, const gql_hmap *frags,
                                  const ast_def **fragdefs, uint32_t nfrags, const char *opname, size_t opn,
                                  const char *vars, size_t varsn, const gql_limits *lim);

GQL_API int gql_abi_version(void)
{
    return GQL_ABI_VERSION;
}

GQL_API gql_schema *gql_schema_open(const char *path, char *err, size_t errn)
{
    return gqlb_open(path, err, errn);
}

GQL_API void gql_schema_close(gql_schema *s)
{
    gqlb_close(s);
}

GQL_API uint64_t gql_schema_hash(const gql_schema *s)
{
    return s->hash;
}

GQL_API void gql_schema_hash_hex(const gql_schema *s, char out[17])
{
    snprintf(out, 17, "%016llx", (unsigned long long)s->hash);
}

GQL_API void gql_schema_get_view(const gql_schema *s, gql_schema_view *v)
{
    v->types = s->types;   v->ntypes = s->ntypes;
    v->fields = s->fields; v->nfields = s->nfields;
    v->ivals = s->ivals;   v->nivals = s->nivals;
    v->evals = s->evals;   v->nevals = s->nevals;
    v->trefs = s->trefs;   v->ntrefs = s->ntrefs;
    v->idx = s->idx;       v->nidx = s->nidx;
    v->str = s->str;       v->nstr = s->nstr;
    v->query_type = s->query_type;
    v->mutation_type = s->mutation_type;
    v->f_typename = s->f_typename;
    v->f_schema = s->f_schema;
    v->f_type = s->f_type;
}

static void doc_error(gql_doc *d, gql_errors *e)
{
    d->ok = 0;
    buf_reset(&d->errors);
    errs_to_json(e, &d->errors);
}

GQL_API gql_doc *gql_doc_new(const gql_schema *s, const char *q, size_t n, const gql_limits *lim)
{
    gql_doc *d = calloc(1, sizeof *d);
    gql_errors e;
    gql_parse_opts po;
    gql_validate_opts vo;
    char *src;
    const ast_def *def;
    uint32_t k;

    if (!d)
        return NULL;
    arena_init(&d->arena, lim ? lim->max_memory : 0);
    buf_init(&d->errors);
    hm_init(&d->frags);
    errs_init(&e, lim && lim->max_errors ? lim->max_errors : 100);
    d->ok = 1;
    if (lim && lim->max_bytes && n > lim->max_bytes) {
        errs_add(&e, NULL, 0, "Query exceeds the maximum size of %u bytes.", lim->max_bytes);
        doc_error(d, &e);
        goto out;
    }
    src = arena_strndup(&d->arena, q, n);
    if (!src) {
        errs_add(&e, NULL, 0, "Query too large (memory limit reached).");
        doc_error(d, &e);
        goto out;
    }
    memset(&po, 0, sizeof po);
    po.max_tokens = lim ? lim->max_tokens : 0;
    po.max_depth = lim ? lim->max_depth : 0;
    d->ast = gql_parse(src, n, &d->arena, &po, &e);
    if (!d->ast) {
        doc_error(d, &e);
        goto out;
    }
    memset(&vo, 0, sizeof vo);
    vo.introspection = lim ? (int)lim->introspection : 1;
    if (gql_validate(s, d->ast, &vo, &e)) {
        doc_error(d, &e);
        goto out;
    }
    /* fragment table for planning */
    for (def = d->ast->defs; def; def = def->next)
        if (def->kind == DEF_FRAGMENT)
            d->nfrags++;
    d->fragdefs = arena_alloc(&d->arena, (d->nfrags + 1) * sizeof *d->fragdefs);
    if (!d->fragdefs) {
        errs_add(&e, NULL, 0, "Query too large (memory limit reached).");
        doc_error(d, &e);
        goto out;
    }
    k = 0;
    for (def = d->ast->defs; def; def = def->next)
        if (def->kind == DEF_FRAGMENT) {
            d->fragdefs[k] = def;
            hm_put(&d->frags, 0, def->name.p, def->name.n, k);
            k++;
        }
out:
    errs_free(&e);
    return d;
}

GQL_API int gql_doc_ok(const gql_doc *d)
{
    return d && d->ok;
}

GQL_API const char *gql_doc_errors(const gql_doc *d, size_t *len)
{
    if (!d || d->ok) {
        if (len)
            *len = 0;
        return NULL;
    }
    if (len)
        *len = d->errors.len;
    return buf_cstr((gql_buf *)&d->errors);
}

GQL_API void gql_doc_free(gql_doc *d)
{
    if (!d)
        return;
    buf_free(&d->errors);
    hm_free(&d->frags);
    arena_free(&d->arena);
    free(d);
}

const char *gql_plan_opname_internal(const gql_plan *p, size_t *len);

GQL_API const char *gql_plan_operation_name(const gql_plan *p, size_t *len)
{
    size_t n = 0;
    const char *r = p ? gql_plan_opname_internal(p, &n) : "";
    if (len)
        *len = n;
    return r;
}

GQL_API gql_plan *gql_plan_new(const gql_schema *s, const gql_doc *d, const char *op, size_t opn,
                               const char *vars, size_t varsn, const gql_limits *lim)
{
    if (!d || !d->ok)
        return NULL;
    return gql_plan_build_internal(s, d->ast, &d->frags, d->fragdefs, d->nfrags, op, opn, vars, varsn, lim);
}
