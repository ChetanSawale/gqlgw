/* sdl.c - SDL compiler: AST -> flat schema model, type system validation,
 * gateway directives (@fetch, @batch, @cache, @from, @discriminator) -> routes.
 */
#include "sdl.h"
#include "parser.h"
#include "values.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "prelude.inc"

typedef struct {
    const ast_def *def;
    uint32_t next;
} tdef_link;

struct gql_compiled {
    gql_schema s;
    gql_arena arena;
    gql_errors *errs, *warns;
    ast_doc *user, *prelude;

    gql_vec types, fields, ivals, evals, trefs, idx, dirs;
    gql_buf str;
    gql_hmap strmap, typemap, dirmap, trefmap;

    gql_vec t_head;       /* uint32 per type */
    gql_vec t_tail;       /* uint32 per type */
    gql_vec t_prelude;    /* uint8 per type: defined by the prelude */
    gql_vec tdefs;        /* tdef_link */
    gql_vec iv_ast;       /* const ast_inputval * per ival */
    gql_vec iv_owner;     /* iv_owner per ival */
    gql_vec iv_state;     /* uint8 per ival: 0 todo, 1 in progress, 2 done */
    gql_vec f_ast;        /* const ast_fielddef * per field */
    gql_vec ev_ast;       /* const ast_enumval * per eval */
    gql_vec d_ast;        /* const ast_def * per directive */
    const ast_def *schema_def;
    gql_vec schema_exts;  /* const ast_def * */
    gql_vec pending_exts; /* const ast_def * */

    gql_vec routes;       /* gql_route */
    gql_vec from;         /* gql_str per field */
    gql_vec discr;        /* gql_str per type */
    int failed;
};

typedef struct gql_compiled C;

enum { IV_FIELD_ARG, IV_DIR_ARG, IV_INPUT_FIELD, IV_META };
typedef struct {
    uint32_t kind, owner;   /* owner: field idx / directive idx / input type idx */
} iv_owner;

#define TYPE(c, i)  VEC_AT(&(c)->types, gql_type_rec, i)
#define FIELD(c, i) VEC_AT(&(c)->fields, gql_field_rec, i)
#define IVAL(c, i)  VEC_AT(&(c)->ivals, gql_ival_rec, i)
#define EVAL(c, i)  VEC_AT(&(c)->evals, gql_eval_rec, i)
#define TREF(c, i)  VEC_AT(&(c)->trefs, gql_tref_rec, i)
#define DIR(c, i)   VEC_AT(&(c)->dirs, gql_dir_rec, i)
#define U32(v, i)   VEC_AT(v, uint32_t, i)
#define SN(c, r)    ((c)->str.p + (r).off)


/* ---- diagnostics ------------------------------------------------------ */

static void err_at(C *c, const gql_loc *l, const char *fmt, ...) GQL_PRINTF(3, 4);
static void err_at(C *c, const gql_loc *l, const char *fmt, ...)
{
    va_list ap;
    c->failed = 1;
    va_start(ap, fmt);
    errs_addv(c->errs, l, (l && l->line) ? 1 : 0, fmt, ap);
    va_end(ap);
}

static void warn_at(C *c, const gql_loc *l, const char *fmt, ...) GQL_PRINTF(3, 4);
static void warn_at(C *c, const gql_loc *l, const char *fmt, ...)
{
    va_list ap;
    if (!c->warns)
        return;
    va_start(ap, fmt);
    errs_addv(c->warns, l, (l && l->line) ? 1 : 0, fmt, ap);
    va_end(ap);
}

/* ---- strings / type refs ------------------------------------------------ */

static void sync_view(C *c);

static gql_sref str_add(C *c, const char *p, uint32_t n)
{
    gql_sref r;
    r.off = (uint32_t)c->str.len;
    r.len = n;
    buf_put(&c->str, p, n);
    buf_putc(&c->str, 0);
    c->s.str = c->str.p;
    c->s.nstr = (uint32_t)c->str.len;
    return r;
}

/* interns a string whose bytes are stable (source text or arena) */
static gql_sref str_intern(C *c, gql_str s)
{
    gql_sref r = { 0, 0 };
    uint32_t off;
    if (!s.n)
        return r;
    off = hm_get(&c->strmap, 0, s.p, s.n);
    if (off != GQL_NONE) {
        r.off = off;
        r.len = s.n;
        return r;
    }
    r = str_add(c, s.p, s.n);
    hm_put(&c->strmap, 0, s.p, s.n, r.off);
    return r;
}

static uint32_t tref_get(C *c, uint32_t kind, uint32_t of)
{
    uint32_t key[2], *k, i;
    gql_tref_rec *r;
    key[0] = kind;
    key[1] = of;
    i = hm_get(&c->trefmap, 0, (const char *)key, sizeof key);
    if (i != GQL_NONE)
        return i;
    k = arena_alloc(&c->arena, sizeof key);
    r = vec_push(&c->trefs);
    if (!k || !r) {
        err_at(c, NULL, "out of memory");
        return GQL_NONE;
    }
    k[0] = kind;
    k[1] = of;
    r->kind = kind;
    r->of = of;
    i = c->trefs.len - 1;
    hm_put(&c->trefmap, 0, (const char *)k, sizeof key, i);
    return i;
}

static uint32_t tref_ast(C *c, const ast_type *t)
{
    uint32_t of;
    switch (t->kind) {
    case AT_NAMED:
        of = hm_get(&c->typemap, 0, t->name.p, t->name.n);
        if (of == GQL_NONE) {
            err_at(c, &t->loc, "Unknown type \"%.*s\".", (int)t->name.n, t->name.p);
            return GQL_NONE;
        }
        TYPE(c, of).flags |= TF_REFERENCED;
        return tref_get(c, TR_NAMED, of);
    case AT_LIST:
        of = tref_ast(c, t->of);
        return of == GQL_NONE ? GQL_NONE : tref_get(c, TR_LIST, of);
    case AT_NONNULL:
        of = tref_ast(c, t->of);
        return of == GQL_NONE ? GQL_NONE : tref_get(c, TR_NONNULL, of);
    }
    return GQL_NONE;
}

static uint32_t tref_named_c(C *c, uint32_t t)
{
    while (TREF(c, t).kind != TR_NAMED)
        t = TREF(c, t).of;
    return TREF(c, t).of;
}

static const char *tref_str(C *c, gql_buf *b, uint32_t t)
{
    buf_reset(b);
    sync_view(c);
    gql_print_tref(b, &c->s, t);
    return buf_cstr(b);
}

static void sync_view(C *c)
{
    gql_schema *s = &c->s;
    s->types = c->types.p;   s->ntypes = c->types.len;
    s->fields = c->fields.p; s->nfields = c->fields.len;
    s->ivals = c->ivals.p;   s->nivals = c->ivals.len;
    s->evals = c->evals.p;   s->nevals = c->evals.len;
    s->trefs = c->trefs.p;   s->ntrefs = c->trefs.len;
    s->idx = c->idx.p;       s->nidx = c->idx.len;
    s->dirs = c->dirs.p;     s->ndirs = c->dirs.len;
    s->str = c->str.p;       s->nstr = (uint32_t)c->str.len;
}

static void push_u32(gql_vec *v, uint32_t x)
{
    uint32_t *p = vec_push(v);
    if (p)
        *p = x;
}

static void push_ptr(gql_vec *v, const void *x)
{
    const void **p = vec_push(v);
    if (p)
        *p = x;
}

static void push_u8(gql_vec *v, uint8_t x)
{
    uint8_t *p = vec_push(v);
    if (p)
        *p = x;
}

/* ---- AST helpers -------------------------------------------------------- */

static const ast_directive *find_dir(const ast_directive *d, const char *name)
{
    for (; d; d = d->next)
        if (gql_str_eqz(d->name, name))
            return d;
    return NULL;
}

static const ast_value *dir_arg(const ast_directive *d, const char *name)
{
    const ast_arg *a;
    if (!d)
        return NULL;
    for (a = d->args; a; a = a->next)
        if (gql_str_eqz(a->name, name))
            return a->value;
    return NULL;
}

static int arg_string(const ast_directive *d, const char *name, gql_str *out)
{
    const ast_value *v = dir_arg(d, name);
    if (!v || v->kind != AV_STRING)
        return 0;
    *out = v->s;
    return 1;
}

static int arg_int(const ast_directive *d, const char *name, long *out)
{
    const ast_value *v = dir_arg(d, name);
    if (!v || v->kind != AV_INT)
        return 0;
    *out = strtol(v->s.p, NULL, 10);
    return 1;
}

static void check_name(C *c, gql_str name, const gql_loc *l)
{
    if (name.n >= 2 && name.p[0] == '_' && name.p[1] == '_')
        err_at(c, l, "Name \"%.*s\" must not begin with \"__\", which is reserved by GraphQL introspection.",
               (int)name.n, name.p);
}

static uint32_t def_kind_to_type_kind(ast_def_kind k)
{
    switch (k) {
    case DEF_SCALAR: return GK_SCALAR;
    case DEF_OBJECT: return GK_OBJECT;
    case DEF_INTERFACE: return GK_INTERFACE;
    case DEF_UNION: return GK_UNION;
    case DEF_ENUM: return GK_ENUM;
    case DEF_INPUT: return GK_INPUT;
    default: return GQL_NONE;
    }
}

static const char *ext_kind_word(ast_def_kind k)
{
    switch (k) {
    case DEF_SCALAR: return "scalar";
    case DEF_OBJECT: return "object";
    case DEF_INTERFACE: return "interface";
    case DEF_UNION: return "union";
    case DEF_ENUM: return "enum";
    case DEF_INPUT: return "input object";
    default: return "?";
    }
}

static void link_def(C *c, uint32_t ti, const ast_def *d)
{
    tdef_link *l = vec_push(&c->tdefs);
    uint32_t li = c->tdefs.len - 1;
    if (!l)
        return;
    l->def = d;
    l->next = GQL_NONE;
    if (U32(&c->t_head, ti) == GQL_NONE)
        U32(&c->t_head, ti) = li;
    else
        VEC_AT(&c->tdefs, tdef_link, U32(&c->t_tail, ti)).next = li;
    U32(&c->t_tail, ti) = li;
}

#define FOR_DEFS(c, ti, l) \
    for (uint32_t _li = U32(&(c)->t_head, ti); _li != GQL_NONE && ((l) = VEC_AT(&(c)->tdefs, tdef_link, _li).def, 1); \
         _li = VEC_AT(&(c)->tdefs, tdef_link, _li).next)

static const ast_def *base_def(C *c, uint32_t ti)
{
    const ast_def *d;
    FOR_DEFS(c, ti, d)
        if (!d->is_ext)
            return d;
    return NULL;
}

static int deprecation(C *c, const ast_directive *dirs, gql_sref *out)
{
    const ast_directive *d = find_dir(dirs, "deprecated");
    gql_str reason;
    if (!d)
        return 0;
    if (arg_string(d, "reason", &reason)) {
        *out = str_intern(c, reason);
    } else {
        static const char dflt[] = "No longer supported";
        gql_str s;
        s.p = dflt;
        s.n = sizeof dflt - 1;
        *out = str_intern(c, s);
    }
    return 1;
}

/* ---- phase A: collect definitions -------------------------------------- */

static int is_gateway_dir(gql_str n)
{
    return gql_str_eqz(n, "fetch") || gql_str_eqz(n, "batch") || gql_str_eqz(n, "cache") ||
           gql_str_eqz(n, "from") || gql_str_eqz(n, "discriminator");
}

static uint32_t builtin_scalar_kind(gql_str n)
{
    if (gql_str_eqz(n, "Int")) return SK_INT;
    if (gql_str_eqz(n, "Float")) return SK_FLOAT;
    if (gql_str_eqz(n, "String")) return SK_STRING;
    if (gql_str_eqz(n, "Boolean")) return SK_BOOLEAN;
    if (gql_str_eqz(n, "ID")) return SK_ID;
    return SK_CUSTOM;
}

static void new_type(C *c, const ast_def *d, int prelude)
{
    gql_type_rec *t = vec_push(&c->types);
    uint32_t ti = c->types.len - 1;
    if (!t)
        return;
    t->kind = def_kind_to_type_kind(d->kind);
    t->name = str_intern(c, d->name);
    t->desc = str_intern(c, d->desc);
    if (prelude) {
        if (d->name.n >= 2 && d->name.p[0] == '_')
            t->flags |= TF_INTROSPECTION;
        else if (d->kind == DEF_SCALAR) {
            t->flags |= TF_BUILTIN;
            t->scalar = builtin_scalar_kind(d->name);
        }
    }
    push_u32(&c->t_head, GQL_NONE);
    push_u32(&c->t_tail, GQL_NONE);
    push_u8(&c->t_prelude, (uint8_t)prelude);
    hm_put(&c->typemap, 0, d->name.p, d->name.n, ti);
    link_def(c, ti, d);
}

static void collect(C *c, const ast_doc *doc, int prelude)
{
    const ast_def *d;
    for (d = doc->defs; d; d = d->next) {
        uint32_t i;
        switch (d->kind) {
        case DEF_OPERATION:
        case DEF_FRAGMENT:
            err_at(c, &d->loc, "Executable definitions (operations, fragments) are not allowed in a schema.");
            break;
        case DEF_SCHEMA:
            if (d->is_ext)
                push_ptr(&c->schema_exts, d);
            else if (c->schema_def)
                err_at(c, &d->loc, "Must provide only one schema definition.");
            else
                c->schema_def = d;
            break;
        case DEF_DIRECTIVE:
            i = hm_get(&c->dirmap, 0, d->name.p, d->name.n);
            if (i != GQL_NONE) {
                const ast_def *u = VEC_AT(&c->d_ast, const ast_def *, i);
                if (prelude)
                    err_at(c, &u->name_loc, "Directive \"@%.*s\" is built-in%s and cannot be redefined.",
                           (int)d->name.n, d->name.p, is_gateway_dir(d->name) ? " (gqlgw)" : "");
                else
                    err_at(c, &d->name_loc, "There can be only one directive named \"@%.*s\".",
                           (int)d->name.n, d->name.p);
                break;
            }
            if (!prelude)
                check_name(c, d->name, &d->name_loc);
            {
                gql_dir_rec *r = vec_push(&c->dirs);
                if (!r)
                    break;
                r->name = str_intern(c, d->name);
                r->desc = str_intern(c, d->desc);
                if (prelude)
                    r->flags |= DF_BUILTIN | (is_gateway_dir(d->name) ? DF_HIDDEN : 0);
                if (d->repeatable)
                    r->flags |= DF_REPEATABLE;
                hm_put(&c->dirmap, 0, d->name.p, d->name.n, c->dirs.len - 1);
                push_ptr(&c->d_ast, d);
            }
            break;
        default:
            if (d->is_ext) {
                push_ptr(&c->pending_exts, d);
                break;
            }
            i = hm_get(&c->typemap, 0, d->name.p, d->name.n);
            if (i != GQL_NONE) {
                const ast_def *u = base_def(c, i);
                if (prelude) {
                    if (u && u->kind == DEF_SCALAR && d->kind == DEF_SCALAR &&
                        builtin_scalar_kind(d->name) != SK_CUSTOM) {
                        /* user re-declared a spec scalar: use the standard definition */
                        TYPE(c, i).flags |= TF_BUILTIN;
                        TYPE(c, i).scalar = builtin_scalar_kind(d->name);
                        TYPE(c, i).desc = str_intern(c, d->desc);
                        VEC_AT(&c->t_prelude, uint8_t, i) = 1;
                    } else {
                        err_at(c, u ? &u->name_loc : NULL,
                               "Type \"%.*s\" is reserved by GraphQL and cannot be redefined.",
                               (int)d->name.n, d->name.p);
                    }
                } else {
                    err_at(c, &d->name_loc, "There can be only one type named \"%.*s\".",
                           (int)d->name.n, d->name.p);
                }
                break;
            }
            if (!prelude)
                check_name(c, d->name, &d->name_loc);
            new_type(c, d, prelude);
        }
    }
}

static void apply_extensions(C *c)
{
    uint32_t k;
    for (k = 0; k < c->pending_exts.len; k++) {
        const ast_def *d = VEC_AT(&c->pending_exts, const ast_def *, k);
        uint32_t i = hm_get(&c->typemap, 0, d->name.p, d->name.n);
        if (i == GQL_NONE) {
            err_at(c, &d->name_loc, "Cannot extend type \"%.*s\" because it is not defined.",
                   (int)d->name.n, d->name.p);
            continue;
        }
        if (TYPE(c, i).kind != def_kind_to_type_kind(d->kind)) {
            err_at(c, &d->name_loc, "Cannot extend non-%s type \"%.*s\".", ext_kind_word(d->kind),
                   (int)d->name.n, d->name.p);
            continue;
        }
        link_def(c, i, d);
    }
}

/* ---- phase B/C: records --------------------------------------------------- */

/* name of a type from its AST (stable pointer, unlike the string pool) */
static gql_str tname(C *c, uint32_t ti)
{
    gql_str s;
    const ast_def *d = base_def(c, ti);
    if (d)
        return d->name;
    s.p = SN(c, TYPE(c, ti).name);
    s.n = TYPE(c, ti).name.len;
    return s;
}

/* schema coordinate of an input value: Type.field(arg:), @dir(arg:), Input.field */
static const char *iv_coord(C *c, gql_buf *b, uint32_t iv)
{
    iv_owner o = VEC_AT(&c->iv_owner, iv_owner, iv);
    gql_sref an = IVAL(c, iv).name;
    buf_reset(b);
    switch (o.kind) {
    case IV_FIELD_ARG: {
        const gql_field_rec *f = &FIELD(c, o.owner);
        if (f->owner != GQL_NONE)
            buf_printf(b, "%s.", SN(c, TYPE(c, f->owner).name));
        buf_printf(b, "%s(%s:)", SN(c, f->name), SN(c, an));
        break;
    }
    case IV_DIR_ARG:
        buf_printf(b, "@%s(%s:)", SN(c, DIR(c, o.owner).name), SN(c, an));
        break;
    case IV_INPUT_FIELD:
        buf_printf(b, "%s.%s", SN(c, TYPE(c, o.owner).name), SN(c, an));
        break;
    default:
        buf_puts(b, SN(c, an));
    }
    return buf_cstr(b);
}

static uint32_t new_ival(C *c, const ast_inputval *a, uint32_t okind, uint32_t owner, int user)
{
    gql_ival_rec *r;
    iv_owner *o;
    uint32_t t, iv;
    if (user)
        check_name(c, a->name, &a->loc);
    t = tref_ast(c, a->type);
    r = vec_push(&c->ivals);
    o = vec_push(&c->iv_owner);
    if (!r || !o)
        return GQL_NONE;
    iv = c->ivals.len - 1;
    o->kind = okind;
    o->owner = owner;
    IVAL(c, iv).name = str_intern(c, a->name);
    IVAL(c, iv).desc = str_intern(c, a->desc);
    IVAL(c, iv).type = t;
    if (deprecation(c, a->dirs, &IVAL(c, iv).deprecation))
        IVAL(c, iv).flags |= MF_DEPRECATED;
    push_ptr(&c->iv_ast, a);
    push_u8(&c->iv_state, 0);
    if (t != GQL_NONE && !gql_kind_is_input(TYPE(c, tref_named_c(c, t)).kind)) {
        gql_buf b1, b2;
        buf_init(&b1);
        buf_init(&b2);
        err_at(c, &a->loc, "The type of %s must be Input Type but got: %s.", iv_coord(c, &b1, iv),
               tref_str(c, &b2, t));
        buf_free(&b1);
        buf_free(&b2);
    }
    return iv;
}

static void build_directives(C *c)
{
    uint32_t i, k;
    for (i = 0; i < c->dirs.len; i++) {
        const ast_def *d = VEC_AT(&c->d_ast, const ast_def *, i);
        const ast_inputval *a, *b;
        const ast_name *l;
        int user = !(DIR(c, i).flags & DF_BUILTIN);
        uint32_t first = c->ivals.len;
        for (a = d->inputs; a; a = a->next) {
            for (b = d->inputs; b != a; b = b->next)
                if (gql_str_eq(a->name, b->name))
                    break;
            if (b != a) {
                err_at(c, &a->loc, "Argument \"@%.*s(%.*s:)\" can only be defined once.",
                       (int)d->name.n, d->name.p, (int)a->name.n, a->name.p);
                continue;
            }
            new_ival(c, a, IV_DIR_ARG, i, user);
        }
        DIR(c, i).args_first = first;
        DIR(c, i).args_count = c->ivals.len - first;
        DIR(c, i).locs_first = c->idx.len;
        for (l = d->names; l; l = l->next) {
            for (k = 0; k < DL_COUNT; k++)
                if (gql_str_eqz(l->name, gql_dirloc_names[k]))
                    break;
            if (k < DL_COUNT && !(DIR(c, i).locmask & (1u << k))) {
                DIR(c, i).locmask |= 1u << k;
                push_u32(&c->idx, k);
            }
        }
        DIR(c, i).locs_count = c->idx.len - DIR(c, i).locs_first;
        if (deprecation(c, d->dirs, &DIR(c, i).deprecation))
            DIR(c, i).flags |= DF_DEPRECATED;
    }
}

static int sref_eq(C *c, gql_sref r, gql_str s)
{
    return r.len == s.n && memcmp(SN(c, r), s.p, s.n) == 0;
}

static void build_object_like(C *c, uint32_t ti, int user)
{
    const ast_def *d, *base = base_def(c, ti);
    gql_str tn = tname(c, ti);
    uint32_t ffirst = c->fields.len, ifirst, k;

    FOR_DEFS(c, ti, d) {
        const ast_fielddef *f;
        for (f = d->fields; f; f = f->next) {
            const ast_inputval *a, *b;
            uint32_t t, fi, afirst;
            gql_field_rec *r;
            for (k = ffirst; k < c->fields.len; k++)
                if (sref_eq(c, FIELD(c, k).name, f->name))
                    break;
            if (k < c->fields.len) {
                err_at(c, &f->loc, "Field \"%.*s.%.*s\" can only be defined once.",
                       (int)tn.n, tn.p, (int)f->name.n, f->name.p);
                continue;
            }
            if (user)
                check_name(c, f->name, &f->loc);
            t = tref_ast(c, f->type);
            r = vec_push(&c->fields);
            if (!r)
                return;
            fi = c->fields.len - 1;
            push_ptr(&c->f_ast, f);
            FIELD(c, fi).name = str_intern(c, f->name);
            FIELD(c, fi).desc = str_intern(c, f->desc);
            FIELD(c, fi).owner = ti;
            FIELD(c, fi).type = t;
            if (deprecation(c, f->dirs, &FIELD(c, fi).deprecation))
                FIELD(c, fi).flags |= MF_DEPRECATED;
            if (t != GQL_NONE && !gql_kind_is_output(TYPE(c, tref_named_c(c, t)).kind)) {
                gql_buf tb;
                buf_init(&tb);
                err_at(c, &f->loc, "The type of %.*s.%.*s must be Output Type but got: %s.",
                       (int)tn.n, tn.p, (int)f->name.n, f->name.p, tref_str(c, &tb, t));
                buf_free(&tb);
            }
            afirst = c->ivals.len;
            for (a = f->args; a; a = a->next) {
                for (b = f->args; b != a; b = b->next)
                    if (gql_str_eq(a->name, b->name))
                        break;
                if (b != a) {
                    err_at(c, &a->loc, "Argument \"%.*s.%.*s(%.*s:)\" can only be defined once.",
                           (int)tn.n, tn.p, (int)f->name.n, f->name.p, (int)a->name.n, a->name.p);
                    continue;
                }
                new_ival(c, a, IV_FIELD_ARG, fi, user);
            }
            FIELD(c, fi).args_first = afirst;
            FIELD(c, fi).args_count = c->ivals.len - afirst;
        }
    }
    TYPE(c, ti).fields_first = ffirst;
    TYPE(c, ti).fields_count = c->fields.len - ffirst;
    if (!TYPE(c, ti).fields_count)
        err_at(c, base ? &base->name_loc : NULL, "Type %.*s must define one or more fields.", (int)tn.n, tn.p);

    ifirst = c->idx.len;
    FOR_DEFS(c, ti, d) {
        const ast_name *n;
        for (n = d->names; n; n = n->next) {
            uint32_t ii = hm_get(&c->typemap, 0, n->name.p, n->name.n);
            if (ii == GQL_NONE) {
                err_at(c, &n->loc, "Unknown type \"%.*s\".", (int)n->name.n, n->name.p);
                continue;
            }
            if (ii == ti) {
                err_at(c, &n->loc, "Type %.*s cannot implement itself because it would create a circular reference.",
                       (int)tn.n, tn.p);
                continue;
            }
            if (TYPE(c, ii).kind != GK_INTERFACE) {
                err_at(c, &n->loc, "Type %.*s must only implement Interface types, it cannot implement %.*s.",
                       (int)tn.n, tn.p, (int)n->name.n, n->name.p);
                continue;
            }
            for (k = ifirst; k < c->idx.len; k++)
                if (U32(&c->idx, k) == ii)
                    break;
            if (k < c->idx.len) {
                err_at(c, &n->loc, "Type %.*s can only implement %.*s once.", (int)tn.n, tn.p,
                       (int)n->name.n, n->name.p);
                continue;
            }
            TYPE(c, ii).flags |= TF_REFERENCED;
            push_u32(&c->idx, ii);
        }
    }
    TYPE(c, ti).ifaces_first = ifirst;
    TYPE(c, ti).ifaces_count = c->idx.len - ifirst;
}

static void build_union(C *c, uint32_t ti)
{
    const ast_def *d, *base = base_def(c, ti);
    gql_str tn = tname(c, ti);
    uint32_t first = c->idx.len, k, declared = 0;
    FOR_DEFS(c, ti, d) {
        const ast_name *n;
        for (n = d->names; n; n = n->next) {
            declared++;
            uint32_t mi = hm_get(&c->typemap, 0, n->name.p, n->name.n);
            if (mi == GQL_NONE) {
                err_at(c, &n->loc, "Unknown type \"%.*s\".", (int)n->name.n, n->name.p);
                continue;
            }
            if (TYPE(c, mi).kind != GK_OBJECT) {
                err_at(c, &n->loc, "Union type %.*s can only include Object types, it cannot include %.*s.",
                       (int)tn.n, tn.p, (int)n->name.n, n->name.p);
                continue;
            }
            for (k = first; k < c->idx.len; k++)
                if (U32(&c->idx, k) == mi)
                    break;
            if (k < c->idx.len) {
                err_at(c, &n->loc, "Union type %.*s can only include type %.*s once.", (int)tn.n, tn.p,
                       (int)n->name.n, n->name.p);
                continue;
            }
            TYPE(c, mi).flags |= TF_REFERENCED;
            push_u32(&c->idx, mi);
        }
    }
    TYPE(c, ti).possible_first = first;
    TYPE(c, ti).possible_count = c->idx.len - first;
    if (!declared)
        err_at(c, base ? &base->name_loc : NULL, "Union type %.*s must define one or more member types.",
               (int)tn.n, tn.p);
}

static void build_enum(C *c, uint32_t ti, int user)
{
    const ast_def *d, *base = base_def(c, ti);
    gql_str tn = tname(c, ti);
    uint32_t first = c->evals.len, k;
    FOR_DEFS(c, ti, d) {
        const ast_enumval *v;
        for (v = d->values; v; v = v->next) {
            gql_eval_rec *r;
            uint32_t ei;
            for (k = first; k < c->evals.len; k++)
                if (sref_eq(c, EVAL(c, k).name, v->name))
                    break;
            if (k < c->evals.len) {
                err_at(c, &v->loc, "Enum value \"%.*s.%.*s\" can only be defined once.", (int)tn.n, tn.p,
                       (int)v->name.n, v->name.p);
                continue;
            }
            if (user)
                check_name(c, v->name, &v->loc);
            r = vec_push(&c->evals);
            if (!r)
                return;
            ei = c->evals.len - 1;
            push_ptr(&c->ev_ast, v);
            EVAL(c, ei).name = str_intern(c, v->name);
            EVAL(c, ei).desc = str_intern(c, v->desc);
            EVAL(c, ei).owner = ti;
            if (deprecation(c, v->dirs, &EVAL(c, ei).deprecation))
                EVAL(c, ei).flags |= MF_DEPRECATED;
        }
    }
    TYPE(c, ti).evals_first = first;
    TYPE(c, ti).evals_count = c->evals.len - first;
    if (!TYPE(c, ti).evals_count)
        err_at(c, base ? &base->name_loc : NULL, "Enum type %.*s must define one or more values.", (int)tn.n, tn.p);
}

static void build_input(C *c, uint32_t ti, int user)
{
    const ast_def *d, *base = base_def(c, ti);
    gql_str tn = tname(c, ti);
    uint32_t first = c->ivals.len, k;
    FOR_DEFS(c, ti, d) {
        const ast_inputval *a;
        if (find_dir(d->dirs, "oneOf"))
            TYPE(c, ti).flags |= TF_ONEOF;
        for (a = d->inputs; a; a = a->next) {
            for (k = first; k < c->ivals.len; k++)
                if (sref_eq(c, IVAL(c, k).name, a->name))
                    break;
            if (k < c->ivals.len) {
                err_at(c, &a->loc, "Field \"%.*s.%.*s\" can only be defined once.", (int)tn.n, tn.p,
                       (int)a->name.n, a->name.p);
                continue;
            }
            new_ival(c, a, IV_INPUT_FIELD, ti, user);
        }
    }
    TYPE(c, ti).fields_first = first;
    TYPE(c, ti).fields_count = c->ivals.len - first;
    if (!TYPE(c, ti).fields_count)
        err_at(c, base ? &base->name_loc : NULL, "Input Object type %.*s must define one or more fields.",
               (int)tn.n, tn.p);
}

static void build_types(C *c)
{
    uint32_t ti, n = c->types.len;
    for (ti = 0; ti < n; ti++) {
        int user = !VEC_AT(&c->t_prelude, uint8_t, ti);
        const ast_def *d;
        switch (TYPE(c, ti).kind) {
        case GK_SCALAR:
            FOR_DEFS(c, ti, d) {
                gql_str url;
                if (arg_string(find_dir(d->dirs, "specifiedBy"), "url", &url))
                    TYPE(c, ti).specified_by = str_intern(c, url);
            }
            break;
        case GK_OBJECT:
        case GK_INTERFACE:
            build_object_like(c, ti, user);
            break;
        case GK_UNION:
            build_union(c, ti);
            break;
        case GK_ENUM:
            build_enum(c, ti, user);
            break;
        case GK_INPUT:
            build_input(c, ti, user);
            break;
        }
    }
    /* possible types of interfaces: implementing object types, in type order */
    for (ti = 0; ti < n; ti++) {
        uint32_t first, o, k;
        if (TYPE(c, ti).kind != GK_INTERFACE)
            continue;
        first = c->idx.len;
        for (o = 0; o < n; o++) {
            if (TYPE(c, o).kind != GK_OBJECT)
                continue;
            for (k = 0; k < TYPE(c, o).ifaces_count; k++)
                if (U32(&c->idx, TYPE(c, o).ifaces_first + k) == ti) {
                    push_u32(&c->idx, o);
                    break;
                }
        }
        TYPE(c, ti).possible_first = first;
        TYPE(c, ti).possible_count = c->idx.len - first;
    }
}

/* ---- phase D: meta fields, roots, index -------------------------------- */

static uint32_t named_tref(C *c, const char *name, int nonnull)
{
    uint32_t ti = hm_get(&c->typemap, 0, name, (uint32_t)strlen(name)), t;
    if (ti == GQL_NONE)
        return GQL_NONE;
    t = tref_get(c, TR_NAMED, ti);
    return nonnull ? tref_get(c, TR_NONNULL, t) : t;
}

static uint32_t meta_field(C *c, const char *name, uint32_t type)
{
    gql_field_rec *f = vec_push(&c->fields);
    uint32_t fi = c->fields.len - 1;
    if (!f)
        return GQL_NONE;
    push_ptr(&c->f_ast, NULL);
    FIELD(c, fi).name = str_add(c, name, (uint32_t)strlen(name));
    FIELD(c, fi).owner = GQL_NONE;
    FIELD(c, fi).type = type;
    FIELD(c, fi).args_first = c->ivals.len;
    return fi;
}

static void build_meta(C *c)
{
    gql_ival_rec *a;
    iv_owner *o;
    c->s.f_typename = meta_field(c, "__typename", named_tref(c, "String", 1));
    c->s.f_schema = meta_field(c, "__schema", named_tref(c, "__Schema", 1));
    c->s.f_type = meta_field(c, "__type", named_tref(c, "__Type", 0));
    a = vec_push(&c->ivals);
    o = vec_push(&c->iv_owner);
    if (!a || !o)
        return;
    IVAL(c, c->ivals.len - 1).name = str_add(c, "name", 4);
    IVAL(c, c->ivals.len - 1).type = named_tref(c, "String", 1);
    o->kind = IV_META;
    o->owner = c->s.f_type;
    push_ptr(&c->iv_ast, NULL);
    push_u8(&c->iv_state, 2);
    FIELD(c, c->s.f_type).args_first = c->ivals.len - 1;
    FIELD(c, c->s.f_type).args_count = 1;
}

static void build_roots(C *c)
{
    uint32_t roots[3] = { GQL_NONE, GQL_NONE, GQL_NONE };
    gql_loc locs[3] = { { 0, 0 }, { 0, 0 }, { 0, 0 } };
    static const char *const opname[3] = { "Query", "Mutation", "Subscription" };
    int i, j;

    if (c->schema_def) {
        uint32_t k;
        for (k = 0; k <= c->schema_exts.len; k++) {
            const ast_def *d = k == 0 ? c->schema_def : VEC_AT(&c->schema_exts, const ast_def *, k - 1);
            const ast_optype *o;
            for (o = d->roots; o; o = o->next) {
                uint32_t ti = hm_get(&c->typemap, 0, o->type.p, o->type.n);
                if (roots[o->op] != GQL_NONE || locs[o->op].line) {
                    err_at(c, &o->loc, "There can be only one %s type in schema.", gql_op_name(o->op));
                    continue;
                }
                locs[o->op] = o->loc;
                if (ti == GQL_NONE) {
                    err_at(c, &o->loc, "Unknown type \"%.*s\".", (int)o->type.n, o->type.p);
                    continue;
                }
                roots[o->op] = ti;
            }
        }
        c->s.desc = str_intern(c, c->schema_def->desc);
    } else {
        for (i = 0; i < 3; i++)
            roots[i] = hm_get(&c->typemap, 0, opname[i], (uint32_t)strlen(opname[i]));
    }
    for (i = 0; i < 3; i++) {
        if (roots[i] == GQL_NONE)
            continue;
        if (TYPE(c, roots[i]).kind != GK_OBJECT) {
            gql_str n = tname(c, roots[i]);
            if (i == 0)
                err_at(c, &locs[i], "%s root type must be Object type, it cannot be %.*s.", opname[i], (int)n.n, n.p);
            else
                err_at(c, &locs[i], "%s root type must be Object type if provided, it cannot be %.*s.", opname[i],
                       (int)n.n, n.p);
            roots[i] = GQL_NONE;
            continue;
        }
        TYPE(c, roots[i]).flags |= TF_REFERENCED;
    }
    if (roots[0] == GQL_NONE && !c->failed)
        err_at(c, NULL, "Query root type must be provided.");
    for (i = 0; i < 3; i++)
        for (j = i + 1; j < 3; j++)
            if (roots[i] != GQL_NONE && roots[i] == roots[j]) {
                gql_str n = tname(c, roots[i]);
                err_at(c, &locs[j], "All root types must be different, \"%.*s\" type is used as %s and %s root types.",
                       (int)n.n, n.p, gql_op_name((gql_op_type)i), gql_op_name((gql_op_type)j));
            }
    if (roots[2] != GQL_NONE) {
        warn_at(c, &locs[2], "Subscriptions are not supported by gqlgw: the subscription root type is ignored.");
        roots[2] = GQL_NONE;
    }
    c->s.query_type = roots[0];
    c->s.mutation_type = roots[1];
    c->s.subscription_type = roots[2];
}

static int reindex(C *c)
{
    sync_view(c);
    if (gql_schema_index(&c->s)) {
        err_at(c, NULL, "out of memory");
        return -1;
    }
    return 0;
}

/* ---- phase E: semantic validation -------------------------------------- */

static void validate_implementations(C *c)
{
    const gql_schema *s = &c->s;
    uint32_t ti, k, j, m;
    gql_buf b1, b2;
    buf_init(&b1);
    buf_init(&b2);
    for (ti = 0; ti < s->ntypes; ti++) {
        const gql_type_rec *t = &s->types[ti];
        const ast_def *base = base_def(c, ti);
        const gql_loc *tl = base ? &base->name_loc : NULL;
        const char *tn = GQL_S(s, t->name);
        if (t->kind != GK_OBJECT && t->kind != GK_INTERFACE)
            continue;
        for (k = 0; k < t->ifaces_count; k++) {
            uint32_t ii = s->idx[t->ifaces_first + k];
            const gql_type_rec *it = &s->types[ii];
            const char *in = GQL_S(s, it->name);
            /* transitive interfaces */
            for (j = 0; j < it->ifaces_count; j++) {
                uint32_t tr = s->idx[it->ifaces_first + j];
                for (m = 0; m < t->ifaces_count; m++)
                    if (s->idx[t->ifaces_first + m] == tr)
                        break;
                if (m == t->ifaces_count) {
                    if (tr == ti)
                        err_at(c, tl, "Type %s cannot implement %s because it would create a circular reference.",
                               tn, in);
                    else
                        err_at(c, tl, "Type %s must implement %s because it is implemented by %s.", tn,
                               GQL_S(s, s->types[tr].name), in);
                }
            }
            for (j = 0; j < it->fields_count; j++) {
                const gql_field_rec *iff = &s->fields[it->fields_first + j];
                const char *fn = GQL_S(s, iff->name);
                uint32_t tfi = gql_member(s, ti, fn, iff->name.len);
                const gql_field_rec *tf;
                const ast_fielddef *tfa;
                if (tfi == GQL_NONE) {
                    err_at(c, tl, "Interface field %s.%s expected but %s does not provide it.", in, fn, tn);
                    continue;
                }
                tf = &s->fields[tfi];
                tfa = VEC_AT(&c->f_ast, const ast_fielddef *, tfi);
                if (tf->type != GQL_NONE && iff->type != GQL_NONE && !gql_tref_is_subtype(s, tf->type, iff->type)) {
                    buf_reset(&b1);
                    gql_print_tref(&b1, s, iff->type);
                    buf_reset(&b2);
                    gql_print_tref(&b2, s, tf->type);
                    err_at(c, tfa ? &tfa->type->loc : tl, "Interface field %s.%s expects type %s but %s.%s is type %s.",
                           in, fn, buf_cstr(&b1), tn, fn, buf_cstr(&b2));
                }
                for (m = 0; m < iff->args_count; m++) {
                    const gql_ival_rec *ia = &s->ivals[iff->args_first + m];
                    const char *an = GQL_S(s, ia->name);
                    uint32_t ta = gql_arg_lookup(s, tf->args_first, tf->args_count, an, ia->name.len);
                    if (ta == GQL_NONE) {
                        err_at(c, tfa ? &tfa->loc : tl,
                               "Interface field argument %s.%s(%s:) expected but %s.%s does not provide it.",
                               in, fn, an, tn, fn);
                        continue;
                    }
                    if (s->ivals[ta].type != ia->type) {
                        buf_reset(&b1);
                        gql_print_tref(&b1, s, ia->type);
                        buf_reset(&b2);
                        gql_print_tref(&b2, s, s->ivals[ta].type);
                        err_at(c, tfa ? &tfa->loc : tl,
                               "Interface field argument %s.%s(%s:) expects type %s but %s.%s(%s:) is type %s.",
                               in, fn, an, buf_cstr(&b1), tn, fn, an, buf_cstr(&b2));
                    }
                }
                for (m = 0; m < tf->args_count; m++) {
                    const gql_ival_rec *ta = &s->ivals[tf->args_first + m];
                    const char *an = GQL_S(s, ta->name);
                    if (gql_arg_lookup(s, iff->args_first, iff->args_count, an, ta->name.len) != GQL_NONE)
                        continue;
                    if (gql_tref_nonnull(s, ta->type) && !(ta->flags & MF_HAS_DEFAULT) &&
                        !VEC_AT(&c->iv_ast, const ast_inputval *, tf->args_first + m)->defval) {
                        buf_reset(&b1);
                        gql_print_tref(&b1, s, ta->type);
                        err_at(c, tfa ? &tfa->loc : tl,
                               "Argument \"%s.%s(%s:)\" must not be required type \"%s\" if not provided by the "
                               "Interface field \"%s.%s\".", tn, fn, an, buf_cstr(&b1), in, fn);
                    }
                }
                if ((tf->flags & MF_DEPRECATED) && !(iff->flags & MF_DEPRECATED))
                    err_at(c, tfa ? &tfa->loc : tl,
                           "Interface field %s.%s is not deprecated, so implementation field %s.%s must not be "
                           "deprecated.", in, fn, tn, fn);
            }
        }
    }
    buf_free(&b1);
    buf_free(&b2);
}

/* graphql-js 17: input objects must be able to hold a finite value */
static uint32_t finite_target(C *c, uint32_t input, uint32_t ftype)
{
    const gql_schema *s = &c->s;
    if (ftype == GQL_NONE)
        return GQL_NONE;
    if (s->types[input].flags & TF_ONEOF) {
        if (s->trefs[ftype].kind == TR_NAMED && s->types[s->trefs[ftype].of].kind == GK_INPUT)
            return s->trefs[ftype].of;
        return GQL_NONE;
    }
    if (s->trefs[ftype].kind == TR_NONNULL) {
        uint32_t o = s->trefs[ftype].of;
        if (s->trefs[o].kind == TR_NAMED && s->types[s->trefs[o].of].kind == GK_INPUT)
            return s->trefs[o].of;
    }
    return GQL_NONE;
}

static void report_cycles(C *c, uint32_t ti, uint8_t *finite, uint8_t *visited, uint32_t *pathidx,
                          gql_vec *path)
{
    const gql_schema *s = &c->s;
    const gql_type_rec *t = &s->types[ti];
    uint32_t k;
    if (visited[ti])
        return;
    visited[ti] = 1;
    pathidx[ti] = path->len;
    for (k = 0; k < t->fields_count; k++) {
        uint32_t iv = t->fields_first + k;
        uint32_t tg = finite_target(c, ti, s->ivals[iv].type);
        if (tg == GQL_NONE || finite[tg])
            continue;
        push_u32(path, iv);
        if (pathidx[tg] == GQL_NONE) {
            report_cycles(c, tg, finite, visited, pathidx, path);
        } else {
            gql_buf b, cb;
            uint32_t m;
            const ast_inputval *a = VEC_AT(&c->iv_ast, const ast_inputval *, U32(path, pathidx[tg]));
            buf_init(&b);
            buf_init(&cb);
            for (m = pathidx[tg]; m < path->len; m++) {
                if (m > pathidx[tg])
                    buf_puts(&b, ", ");
                buf_puts(&b, iv_coord(c, &cb, U32(path, m)));
            }
            err_at(c, a ? &a->loc : NULL,
                   "Input Object %s cannot be provided a finite value because it references itself through fields: %s.",
                   GQL_S(s, s->types[tg].name), buf_cstr(&b));
            buf_free(&b);
            buf_free(&cb);
        }
        path->len--;
    }
    pathidx[ti] = GQL_NONE;
}

static void validate_inputs(C *c)
{
    const gql_schema *s = &c->s;
    uint32_t n = s->ntypes, ti, k, changed;
    uint8_t *finite = calloc(n ? n : 1, 1), *visited = calloc(n ? n : 1, 1);
    uint32_t *pathidx = malloc((n ? n : 1) * sizeof(uint32_t));
    gql_vec path;
    gql_buf b;

    if (!finite || !visited || !pathidx) {
        free(finite);
        free(visited);
        free(pathidx);
        err_at(c, NULL, "out of memory");
        return;
    }
    buf_init(&b);
    for (ti = 0; ti < n; ti++) {
        const gql_type_rec *t = &s->types[ti];
        pathidx[ti] = GQL_NONE;
        if (t->kind != GK_INPUT)
            continue;
        for (k = 0; k < t->fields_count; k++) {
            uint32_t iv = t->fields_first + k;
            const gql_ival_rec *f = &s->ivals[iv];
            const ast_inputval *a = VEC_AT(&c->iv_ast, const ast_inputval *, iv);
            if (t->flags & TF_ONEOF) {
                if (f->type != GQL_NONE && gql_tref_nonnull(s, f->type))
                    err_at(c, a ? &a->type->loc : NULL, "OneOf input field %s.%s must be nullable.",
                           GQL_S(s, t->name), GQL_S(s, f->name));
                if (a && a->defval)
                    err_at(c, &a->loc, "OneOf input field %s.%s cannot have a default value.", GQL_S(s, t->name),
                           GQL_S(s, f->name));
            }
        }
    }
    /* finite value fixpoint */
    for (;;) {
        changed = 0;
        for (ti = 0; ti < n; ti++) {
            const gql_type_rec *t = &s->types[ti];
            uint32_t ntargets = 0, nfinite = 0;
            if (t->kind != GK_INPUT || finite[ti])
                continue;
            for (k = 0; k < t->fields_count; k++) {
                uint32_t tg = finite_target(c, ti, s->ivals[t->fields_first + k].type);
                if (tg == GQL_NONE)
                    continue;
                ntargets++;
                if (finite[tg])
                    nfinite++;
            }
            if ((t->flags & TF_ONEOF) ? (t->fields_count == 0 || ntargets < t->fields_count || nfinite > 0)
                                      : (nfinite == ntargets)) {
                finite[ti] = 1;
                changed = 1;
            }
        }
        if (!changed)
            break;
    }
    vec_init(&path, sizeof(uint32_t));
    for (ti = 0; ti < n; ti++)
        if (s->types[ti].kind == GK_INPUT && !finite[ti])
            report_cycles(c, ti, finite, visited, pathidx, &path);
    vec_free(&path);
    free(finite);
    free(visited);
    free(pathidx);
    buf_free(&b);
}

typedef struct {
    gql_vcheck vc;
    C *c;
    gql_buf first;
    gql_buf path, first_path;
    int n;
} litcheck;

static void lc_first(gql_vcheck *vc, const gql_loc *locs, int nlocs, const char *msg)
{
    litcheck *lc = (litcheck *)vc;
    (void)locs;
    (void)nlocs;
    if (!lc->n++) {
        buf_puts(&lc->first, msg);
        buf_put(&lc->first_path, lc->path.p, lc->path.len);
    }
}

static void lc_report(gql_vcheck *vc, const gql_loc *locs, int nlocs, const char *msg)
{
    litcheck *lc = (litcheck *)vc;
    lc->c->failed = 1;
    errs_add(lc->c->errs, locs, nlocs, "%s", msg);
}

static void ensure_input_defaults(C *c, uint32_t ti);

static void compute_default(C *c, uint32_t iv)
{
    const ast_inputval *a = VEC_AT(&c->iv_ast, const ast_inputval *, iv);
    uint32_t type = IVAL(c, iv).type;
    litcheck lc;
    gql_buf pb, jb, cb;
    gql_vctx vx;

    if (VEC_AT(&c->iv_state, uint8_t, iv))
        return;
    VEC_AT(&c->iv_state, uint8_t, iv) = 1;
    if (!a || !a->defval || type == GQL_NONE) {
        VEC_AT(&c->iv_state, uint8_t, iv) = 2;
        return;
    }
    ensure_input_defaults(c, tref_named_c(c, type));
    sync_view(c);
    memset(&lc, 0, sizeof lc);
    lc.vc.s = &c->s;
    lc.vc.on_error = lc_first;
    lc.c = c;
    buf_init(&lc.first);
    buf_init(&lc.path);
    buf_init(&lc.first_path);
    lc.vc.path = &lc.path;
    gql_check_literal(&lc.vc, tv_tref(type), a->defval, 0, GQL_NONE);
    if (lc.n) {
        buf_init(&cb);
        err_at(c, &a->defval->loc, "%s has invalid default value%s%s: %s", iv_coord(c, &cb, iv),
               lc.first_path.len ? " at " : "", buf_cstr(&lc.first_path), buf_cstr(&lc.first));
        buf_free(&cb);
    } else {
        buf_init(&pb);
        buf_init(&jb);
        gql_print_value(&pb, a->defval);
        vx.s = &c->s;
        vx.var = NULL;
        vx.ud = NULL;
        gql_literal_to_json(&vx, tv_tref(type), a->defval, &jb);
        IVAL(c, iv).defval = str_add(c, pb.p ? pb.p : "", (uint32_t)pb.len);
        IVAL(c, iv).defjson = str_add(c, jb.p ? jb.p : "", (uint32_t)jb.len);
        IVAL(c, iv).flags |= MF_HAS_DEFAULT;
        buf_free(&pb);
        buf_free(&jb);
    }
    buf_free(&lc.first);
    buf_free(&lc.path);
    buf_free(&lc.first_path);
    VEC_AT(&c->iv_state, uint8_t, iv) = 2;
}

static void ensure_input_defaults(C *c, uint32_t ti)
{
    uint32_t k;
    if (TYPE(c, ti).kind != GK_INPUT)
        return;
    for (k = 0; k < TYPE(c, ti).fields_count; k++)
        compute_default(c, TYPE(c, ti).fields_first + k);
}

static void validate_defaults_and_deprecation(C *c)
{
    uint32_t iv;
    gql_buf cb;
    buf_init(&cb);
    for (iv = 0; iv < c->ivals.len; iv++)
        compute_default(c, iv);
    for (iv = 0; iv < c->ivals.len; iv++) {
        const gql_ival_rec *r = &IVAL(c, iv);
        const ast_inputval *a = VEC_AT(&c->iv_ast, const ast_inputval *, iv);
        iv_owner o = VEC_AT(&c->iv_owner, iv_owner, iv);
        if (!(r->flags & MF_DEPRECATED) || r->type == GQL_NONE)
            continue;
        if (TREF(c, r->type).kind == TR_NONNULL && !(r->flags & MF_HAS_DEFAULT)) {
            if (o.kind == IV_INPUT_FIELD)
                err_at(c, a ? &a->loc : NULL, "Required input field %s cannot be deprecated.", iv_coord(c, &cb, iv));
            else
                err_at(c, a ? &a->loc : NULL, "Required argument %s cannot be deprecated.", iv_coord(c, &cb, iv));
        }
    }
    buf_free(&cb);
}

static void check_dirs(C *c, const ast_directive *dirs, uint32_t loc)
{
    const gql_schema *s = &c->s;
    const ast_directive *d, *e;
    litcheck lc;
    gql_buf tb;

    memset(&lc, 0, sizeof lc);
    lc.vc.s = s;
    lc.vc.on_error = lc_report;
    lc.c = c;
    buf_init(&tb);
    for (d = dirs; d; d = d->next) {
        uint32_t di = gql_dir_by_name(s, d->name.p, d->name.n), k;
        const gql_dir_rec *dr;
        const ast_arg *a, *b;
        if (di == GQL_NONE) {
            err_at(c, &d->loc, "Unknown directive \"@%.*s\".", (int)d->name.n, d->name.p);
            continue;
        }
        dr = &s->dirs[di];
        if (!(dr->locmask & (1u << loc)))
            err_at(c, &d->loc, "Directive \"@%.*s\" may not be used on %s.", (int)d->name.n, d->name.p,
                   gql_dirloc_names[loc]);
        if (!(dr->flags & DF_REPEATABLE))
            for (e = dirs; e != d; e = e->next)
                if (gql_str_eq(e->name, d->name)) {
                    gql_loc ls[2];
                    ls[0] = e->loc;
                    ls[1] = d->loc;
                    c->failed = 1;
                    errs_add(c->errs, ls, 2, "The directive \"@%.*s\" can only be used once at this location.",
                             (int)d->name.n, d->name.p);
                    break;
                }
        for (a = d->args; a; a = a->next) {
            uint32_t ai = gql_arg_lookup(s, dr->args_first, dr->args_count, a->name.p, a->name.n);
            if (ai == GQL_NONE) {
                err_at(c, &a->loc, "Unknown argument \"%.*s\" on directive \"@%.*s\".", (int)a->name.n, a->name.p,
                       (int)d->name.n, d->name.p);
                continue;
            }
            for (b = d->args; b != a; b = b->next)
                if (gql_str_eq(a->name, b->name))
                    break;
            if (b != a) {
                err_at(c, &a->loc, "There can be only one argument named \"%.*s\".", (int)a->name.n, a->name.p);
                continue;
            }
            gql_check_literal(&lc.vc, tv_tref(s->ivals[ai].type), a->value,
                              (s->ivals[ai].flags & MF_HAS_DEFAULT) != 0, GQL_NONE);
        }
        for (k = 0; k < dr->args_count; k++) {
            const gql_ival_rec *ar = &s->ivals[dr->args_first + k];
            if (!gql_tref_nonnull(s, ar->type) || (ar->flags & MF_HAS_DEFAULT))
                continue;
            for (a = d->args; a; a = a->next)
                if (a->name.n == ar->name.len && memcmp(a->name.p, GQL_S(s, ar->name), a->name.n) == 0)
                    break;
            if (!a) {
                buf_reset(&tb);
                gql_print_tref(&tb, s, ar->type);
                err_at(c, &d->loc, "Argument \"@%.*s(%s:)\" of type \"%s\" is required, but it was not provided.",
                       (int)d->name.n, d->name.p, GQL_S(s, ar->name), buf_cstr(&tb));
            }
        }
    }
    buf_free(&tb);
}

static void validate_applied_directives(C *c)
{
    uint32_t ti, k;
    const ast_def *d;
    for (k = 0; k <= c->schema_exts.len; k++) {
        const ast_def *sd = k == 0 ? c->schema_def : VEC_AT(&c->schema_exts, const ast_def *, k - 1);
        if (sd)
            check_dirs(c, sd->dirs, DL_SCHEMA);
    }
    for (k = 0; k < c->dirs.len; k++) {
        const ast_def *dd = VEC_AT(&c->d_ast, const ast_def *, k);
        const ast_inputval *a;
        check_dirs(c, dd->dirs, DL_DIRECTIVE_DEFINITION);
        for (a = dd->inputs; a; a = a->next)
            check_dirs(c, a->dirs, DL_ARGUMENT_DEFINITION);
    }
    for (ti = 0; ti < c->types.len; ti++) {
        FOR_DEFS(c, ti, d) {
            const ast_fielddef *f;
            const ast_inputval *a;
            const ast_enumval *v;
            switch (d->kind) {
            case DEF_SCALAR: check_dirs(c, d->dirs, DL_SCALAR); break;
            case DEF_OBJECT: check_dirs(c, d->dirs, DL_OBJECT); break;
            case DEF_INTERFACE: check_dirs(c, d->dirs, DL_INTERFACE); break;
            case DEF_UNION: check_dirs(c, d->dirs, DL_UNION); break;
            case DEF_ENUM: check_dirs(c, d->dirs, DL_ENUM); break;
            case DEF_INPUT: check_dirs(c, d->dirs, DL_INPUT_OBJECT); break;
            default: break;
            }
            for (f = d->fields; f; f = f->next) {
                check_dirs(c, f->dirs, DL_FIELD_DEFINITION);
                for (a = f->args; a; a = a->next)
                    check_dirs(c, a->dirs, DL_ARGUMENT_DEFINITION);
            }
            for (a = d->inputs; a; a = a->next)
                check_dirs(c, a->dirs, DL_INPUT_FIELD_DEFINITION);
            for (v = d->values; v; v = v->next)
                check_dirs(c, v->dirs, DL_ENUM_VALUE);
        }
    }
}

/* ---- phase F: gateway routes ---------------------------------------------- */

static int valid_source(gql_str s)
{
    uint32_t i;
    if (!s.n)
        return 0;
    for (i = 0; i < s.n; i++)
        if (!(isalnum((unsigned char)s.p[i]) || s.p[i] == '_' || s.p[i] == '-'))
            return 0;
    return 1;
}

typedef struct {
    int nparent, nkeys, nargs_all;
} tpl_stats;

/* length of a placeholder "{args...}", "{parent.x}", "{keys}" starting at p, or 0 */
static size_t placeholder_len(const char *p, const char *e)
{
    const char *q = p + 1;
    static const char *const heads[] = { "args", "parent", "keys", NULL };
    int i;
    if (p >= e || *p != '{')
        return 0;
    for (i = 0; heads[i]; i++) {
        size_t hl = strlen(heads[i]);
        if ((size_t)(e - q) >= hl && memcmp(q, heads[i], hl) == 0 &&
            (q + hl < e && (q[hl] == '}' || q[hl] == '.')))
            break;
    }
    if (!heads[i])
        return 0;
    while (q < e && *q != '}') {
        if (!(isalnum((unsigned char)*q) || *q == '_' || *q == '.'))
            return 0;
        q++;
    }
    return q < e ? (size_t)(q - p + 1) : 0;
}

/* parses "lit{args.a.b}lit{parent.x}{keys}{args}" */
static gql_tpl_part *parse_tpl(C *c, gql_str t, const gql_loc *loc, uint32_t fi, const char *what,
                               int in_body, int allow_keys, tpl_stats *st)
{
    const gql_schema *s = &c->s;
    const gql_field_rec *f = &s->fields[fi];
    const char *p = t.p, *e = t.p + t.n;
    gql_tpl_part *head = NULL, **tail = &head;
    const char *fname = GQL_S(s, f->name), *tname_ = GQL_S(s, s->types[f->owner].name);

    memset(st, 0, sizeof *st);
    while (p < e) {
        gql_tpl_part *part = arena_alloc(&c->arena, sizeof *part);
        size_t phlen;
        if (!part) {
            err_at(c, loc, "out of memory");
            return NULL;
        }
        if (!(phlen = placeholder_len(p, e))) {
            /* literal text up to the next placeholder */
            const char *q = p + 1;
            while (q < e && !(*q == '{' && placeholder_len(q, e)))
                q++;
            part->kind = TP_LIT;
            part->lit.p = p;
            part->lit.n = (uint32_t)(q - p);
            p = q;
        } else {
            gql_str ph;
            const char *q = p + phlen - 1;   /* closing brace */
            p++;
            ph.p = p;
            ph.n = (uint32_t)(q - p);
            p = q + 1;
            if (gql_str_eqz(ph, "keys")) {
                if (!allow_keys) {
                    err_at(c, loc, "%s of %s.%s: {keys} is only allowed in @batch(path:).", what, tname_, fname);
                    return NULL;
                }
                part->kind = TP_KEYS;
                st->nkeys++;
            } else if (gql_str_eqz(ph, "args")) {
                if (!in_body) {
                    err_at(c, loc, "%s of %s.%s: {args} (all arguments) is only allowed in body templates.",
                           what, tname_, fname);
                    return NULL;
                }
                part->kind = TP_ARGS_ALL;
                st->nargs_all++;
            } else if (ph.n > 5 && memcmp(ph.p, "args.", 5) == 0) {
                uint32_t cur_tref = GQL_NONE, ai;
                const char *r = ph.p + 5, *re = ph.p + ph.n;
                part->kind = TP_ARGS;
                while (r <= re) {
                    const char *dot = r;
                    gql_str sg;
                    while (dot < re && *dot != '.')
                        dot++;
                    sg.p = r;
                    sg.n = (uint32_t)(dot - r);
                    if (!sg.n || part->nsegs >= TP_MAXSEGS) {
                        err_at(c, loc, "%s of %s.%s: invalid placeholder {%.*s}.", what, tname_, fname,
                               (int)ph.n, ph.p);
                        return NULL;
                    }
                    if (part->nsegs == 0) {
                        ai = gql_arg_lookup(s, f->args_first, f->args_count, sg.p, sg.n);
                        if (ai == GQL_NONE) {
                            err_at(c, loc, "%s of %s.%s: unknown argument \"%.*s\".", what, tname_, fname,
                                   (int)sg.n, sg.p);
                            return NULL;
                        }
                        cur_tref = s->ivals[ai].type;
                    } else {
                        uint32_t nt = gql_tref_named(s, cur_tref);
                        uint32_t m;
                        if (s->trefs[gql_tref_nullable(s, cur_tref)].kind != TR_NAMED ||
                            s->types[nt].kind != GK_INPUT ||
                            (m = gql_member(s, nt, sg.p, sg.n)) == GQL_NONE) {
                            err_at(c, loc, "%s of %s.%s: \"%.*s\" is not an input field of the referenced value.",
                                   what, tname_, fname, (int)sg.n, sg.p);
                            return NULL;
                        }
                        cur_tref = s->ivals[m].type;
                    }
                    part->segs[part->nsegs++] = sg;
                    r = dot + 1;
                }
                if (!in_body) {
                    uint32_t nn = gql_tref_nullable(s, cur_tref);
                    if (s->trefs[nn].kind == TR_LIST)
                        nn = gql_tref_nullable(s, s->trefs[nn].of);
                    if (s->trefs[nn].kind != TR_NAMED || !gql_kind_is_leaf(s->types[s->trefs[nn].of].kind)) {
                        err_at(c, loc, "%s of %s.%s: {%.*s} must reference a scalar, an enum or a list of them "
                               "(use a body template for input objects).", what, tname_, fname, (int)ph.n, ph.p);
                        return NULL;
                    }
                }
            } else if (ph.n > 7 && memcmp(ph.p, "parent.", 7) == 0) {
                gql_str pf;
                uint32_t pfi, nn;
                const gql_str *raw;
                pf.p = ph.p + 7;
                pf.n = ph.n - 7;
                pfi = gql_member(s, f->owner, pf.p, pf.n);
                if (pfi == GQL_NONE) {
                    err_at(c, loc, "%s of %s.%s: {parent.%.*s}: %s has no field \"%.*s\".", what, tname_, fname,
                           (int)pf.n, pf.p, tname_, (int)pf.n, pf.p);
                    return NULL;
                }
                nn = gql_tref_nullable(s, s->fields[pfi].type);
                if (s->trefs[nn].kind != TR_NAMED || !gql_kind_is_leaf(s->types[s->trefs[nn].of].kind)) {
                    err_at(c, loc, "%s of %s.%s: {parent.%.*s} must reference a scalar or enum field.", what,
                           tname_, fname, (int)pf.n, pf.p);
                    return NULL;
                }
                if (find_dir(VEC_AT(&c->f_ast, const ast_fielddef *, pfi)->dirs, "fetch")) {
                    err_at(c, loc, "%s of %s.%s: {parent.%.*s} cannot reference a field resolved by @fetch.", what,
                           tname_, fname, (int)pf.n, pf.p);
                    return NULL;
                }
                raw = gql_compiled_from(c, pfi);
                part->kind = TP_PARENT;
                part->lit = raw ? *raw : pf;
                st->nparent++;
            } else {
                err_at(c, loc, "%s of %s.%s: invalid placeholder {%.*s} (expected args.*, parent.*, keys or args).",
                       what, tname_, fname, (int)ph.n, ph.p);
                return NULL;
            }
        }
        *tail = part;
        tail = &part->next;
    }
    return head;
}

static int valid_select(gql_str s)
{
    uint32_t i;
    if (!s.n || s.p[0] == '.' || s.p[s.n - 1] == '.')
        return 0;
    for (i = 1; i < s.n; i++)
        if (s.p[i] == '.' && s.p[i - 1] == '.')
            return 0;
    return 1;
}

static int cache_of(C *c, const ast_directive *dirs, const gql_loc *loc, int *ttl, gql_str *scope)
{
    const ast_directive *d = find_dir(dirs, "cache");
    long t;
    if (!d)
        return 0;
    if (!arg_int(d, "ttl", &t) || t < 0) {
        err_at(c, loc, "@cache(ttl:) must be a non-negative integer (seconds).");
        return 0;
    }
    if (!arg_string(d, "scope", scope)) {
        scope->p = "PUBLIC";
        scope->n = 6;
    }
    if (!gql_str_eqz(*scope, "PUBLIC") && !gql_str_eqz(*scope, "PRIVATE")) {
        err_at(c, loc, "@cache(scope:) must be \"PUBLIC\" or \"PRIVATE\".");
        return 0;
    }
    *ttl = (int)t;
    return 1;
}

static void build_routes(C *c)
{
    const gql_schema *s = &c->s;
    uint32_t fi, ti;

    for (ti = 0; ti < s->ntypes; ti++) {
        const ast_def *d;
        gql_str *ds = vec_push(&c->discr);
        (void)ds;
        FOR_DEFS(c, ti, d) {
            const ast_directive *dd = find_dir(d->dirs, "discriminator");
            gql_str key;
            if (dd && arg_string(dd, "key", &key)) {
                if (!key.n)
                    err_at(c, &dd->loc, "@discriminator(key:) must not be empty.");
                VEC_AT(&c->discr, gql_str, ti) = key;
            }
        }
    }
    /* @from first: templates translate parent fields to raw keys */
    for (fi = 0; fi < s->nfields; fi++) {
        const ast_fielddef *fa = VEC_AT(&c->f_ast, const ast_fielddef *, fi);
        const ast_directive *d;
        gql_str key;
        gql_str *slot = vec_push(&c->from);
        (void)slot;
        if (!fa || !(d = find_dir(fa->dirs, "from")))
            continue;
        if (s->types[s->fields[fi].owner].kind != GK_OBJECT) {
            err_at(c, &d->loc, "@from must be used on object type fields.");
            continue;
        }
        if (find_dir(fa->dirs, "fetch")) {
            err_at(c, &d->loc, "@from cannot be combined with @fetch on the same field.");
            continue;
        }
        if (arg_string(d, "key", &key)) {
            if (!key.n)
                err_at(c, &d->loc, "@from(key:) must not be empty.");
            VEC_AT(&c->from, gql_str, fi) = key;
        }
    }
    for (fi = 0; fi < s->nfields; fi++) {
        const ast_fielddef *fa = VEC_AT(&c->f_ast, const ast_fielddef *, fi);
        const gql_field_rec *f = &s->fields[fi];
        const ast_directive *fd, *bd, *cd;
        gql_route r;
        gql_str path, body, sel;
        tpl_stats st;
        uint32_t owner_kind;

        if (!fa)
            continue;
        owner_kind = s->types[f->owner].kind;
        fd = find_dir(fa->dirs, "fetch");
        bd = find_dir(fa->dirs, "batch");
        cd = find_dir(fa->dirs, "cache");
        if (!fd) {
            if (bd)
                err_at(c, &bd->loc, "@batch requires @fetch on %s.%s.", GQL_S(s, s->types[f->owner].name),
                       GQL_S(s, f->name));
            if (cd)
                err_at(c, &cd->loc, "@cache on a field requires @fetch on %s.%s (or put @cache on the type).",
                       GQL_S(s, s->types[f->owner].name), GQL_S(s, f->name));
            if ((f->owner == s->query_type || f->owner == s->mutation_type) && owner_kind == GK_OBJECT)
                warn_at(c, &fa->loc, "%s.%s has no @fetch: it resolves from an empty root object and will "
                        "always be null.", GQL_S(s, s->types[f->owner].name), GQL_S(s, f->name));
            continue;
        }
        if (owner_kind != GK_OBJECT) {
            err_at(c, &fd->loc, "@fetch must be declared on object type fields (%s.%s is an interface field).",
                   GQL_S(s, s->types[f->owner].name), GQL_S(s, f->name));
            continue;
        }
        memset(&r, 0, sizeof r);
        r.field = fi;
        r.cache_ttl = -1;
        if (!arg_string(fd, "source", &r.source) || !valid_source(r.source)) {
            err_at(c, &fd->loc, "@fetch(source:) must match [A-Za-z0-9_-]+.");
            continue;
        }
        if (!arg_string(fd, "method", &r.method)) {
            r.method.p = "GET";
            r.method.n = 3;
        }
        if (!gql_str_eqz(r.method, "GET") && !gql_str_eqz(r.method, "POST") && !gql_str_eqz(r.method, "PUT") &&
            !gql_str_eqz(r.method, "PATCH") && !gql_str_eqz(r.method, "DELETE")) {
            err_at(c, &fd->loc, "@fetch(method:) must be one of GET, POST, PUT, PATCH, DELETE.");
            continue;
        }
        arg_string(fd, "path", &path);
        if (!path.n || path.p[0] != '/') {
            err_at(c, &fd->loc, "@fetch(path:) must start with '/'.");
            continue;
        }
        r.path = parse_tpl(c, path, &fd->loc, fi, "@fetch(path:)", 0, 0, &st);
        if (!r.path)
            continue;
        if (st.nparent > 1 && bd) {
            err_at(c, &bd->loc, "@batch on %s.%s requires the @fetch path to reference exactly one {parent.*} key.",
                   GQL_S(s, s->types[f->owner].name), GQL_S(s, f->name));
            continue;
        }
        {
            tpl_stats pst = st;
            if (arg_string(fd, "body", &body)) {
                if (gql_str_eqz(r.method, "GET")) {
                    err_at(c, &fd->loc, "@fetch(body:) is not allowed with method GET.");
                    continue;
                }
                r.body = parse_tpl(c, body, &fd->loc, fi, "@fetch(body:)", 1, 0, &st);
                if (!r.body)
                    continue;
            }
            st = pst;
        }
        if (arg_string(fd, "select", &sel)) {
            if (!valid_select(sel)) {
                err_at(c, &fd->loc, "@fetch(select:) must be a dotted path like \"data.items\".");
                continue;
            }
            r.select = sel;
        }
        if (bd) {
            tpl_stats bst;
            gql_str bpath, match;
            long mx = 100;
            if (!gql_str_eqz(r.method, "GET")) {
                err_at(c, &bd->loc, "@batch requires a GET @fetch.");
                continue;
            }
            if (st.nparent != 1) {
                err_at(c, &bd->loc, "@batch on %s.%s requires the @fetch path to reference exactly one {parent.*} key.",
                       GQL_S(s, s->types[f->owner].name), GQL_S(s, f->name));
                continue;
            }
            arg_string(bd, "path", &bpath);
            if (!bpath.n || bpath.p[0] != '/') {
                err_at(c, &bd->loc, "@batch(path:) must start with '/'.");
                continue;
            }
            r.batch_path = parse_tpl(c, bpath, &bd->loc, fi, "@batch(path:)", 0, 1, &bst);
            if (!r.batch_path)
                continue;
            if (bst.nkeys != 1 || bst.nparent != 0) {
                err_at(c, &bd->loc, "@batch(path:) must contain {keys} exactly once and no {parent.*}.");
                continue;
            }
            if (!arg_string(bd, "match", &match) || !match.n) {
                err_at(c, &bd->loc, "@batch(match:) must name the JSON key identifying returned items.");
                continue;
            }
            arg_int(bd, "max", &mx);
            if (mx < 1 || mx > 10000) {
                err_at(c, &bd->loc, "@batch(max:) must be between 1 and 10000.");
                continue;
            }
            if (arg_string(bd, "select", &sel)) {
                if (!valid_select(sel)) {
                    err_at(c, &bd->loc, "@batch(select:) must be a dotted path like \"data.items\".");
                    continue;
                }
                r.batch_select = sel;
            }
            r.has_batch = 1;
            r.batch_match = match;
            r.batch_max = (uint32_t)mx;
        }
        if (!cache_of(c, fa->dirs, cd ? &cd->loc : &fa->loc, &r.cache_ttl, &r.cache_scope)) {
            uint32_t nt = gql_tref_named(s, f->type);
            const ast_def *td;
            r.cache_ttl = -1;
            FOR_DEFS(c, nt, td) {
                if (find_dir(td->dirs, "cache"))
                    cache_of(c, td->dirs, &td->name_loc, &r.cache_ttl, &r.cache_scope);
            }
        }
        if (r.cache_ttl >= 0 && !gql_str_eqz(r.method, "GET")) {
            if (cd)
                warn_at(c, &cd->loc, "%s.%s: @cache is ignored for %.*s fetches.",
                        GQL_S(s, s->types[f->owner].name), GQL_S(s, f->name), (int)r.method.n, r.method.p);
            r.cache_ttl = -1;
        }
        {
            gql_route *slot = vec_push(&c->routes);
            if (slot)
                *slot = r;
        }
    }
}

/* ---- public API ------------------------------------------------------------ */

const gql_str *gql_compiled_from(const gql_compiled *c, uint32_t field)
{
    const gql_str *s;
    if (field >= c->from.len)
        return NULL;
    s = &VEC_AT(&c->from, gql_str, field);
    return s->n ? s : NULL;
}

const gql_str *gql_compiled_discriminator(const gql_compiled *c, uint32_t type)
{
    const gql_str *s;
    if (type >= c->discr.len)
        return NULL;
    s = &VEC_AT(&c->discr, gql_str, type);
    return s->n ? s : NULL;
}

const gql_schema *gql_compiled_schema(const gql_compiled *c)
{
    return &c->s;
}

uint32_t gql_compiled_nroutes(const gql_compiled *c)
{
    return c->routes.len;
}

const gql_route *gql_compiled_route(const gql_compiled *c, uint32_t i)
{
    return &VEC_AT(&c->routes, gql_route, i);
}

void gql_compiled_free(gql_compiled *c)
{
    if (!c)
        return;
    gql_schema_unindex(&c->s);
    vec_free(&c->types);
    vec_free(&c->fields);
    vec_free(&c->ivals);
    vec_free(&c->evals);
    vec_free(&c->trefs);
    vec_free(&c->idx);
    vec_free(&c->dirs);
    buf_free(&c->str);
    hm_free(&c->strmap);
    hm_free(&c->typemap);
    hm_free(&c->dirmap);
    hm_free(&c->trefmap);
    vec_free(&c->t_head);
    vec_free(&c->t_tail);
    vec_free(&c->t_prelude);
    vec_free(&c->tdefs);
    vec_free(&c->iv_ast);
    vec_free(&c->iv_owner);
    vec_free(&c->iv_state);
    vec_free(&c->f_ast);
    vec_free(&c->ev_ast);
    vec_free(&c->d_ast);
    vec_free(&c->schema_exts);
    vec_free(&c->pending_exts);
    vec_free(&c->routes);
    vec_free(&c->from);
    vec_free(&c->discr);
    arena_free(&c->arena);
    free(c);
}

gql_compiled *gql_sdl_compile(const char *src, size_t n, gql_errors *errs, gql_errors *warnings)
{
    C *c = calloc(1, sizeof *c);
    gql_parse_opts po;
    gql_errors perr;

    if (!c) {
        errs_add(errs, NULL, 0, "out of memory");
        return NULL;
    }
    arena_init(&c->arena, 0);
    c->errs = errs;
    c->warns = warnings;
    vec_init(&c->types, sizeof(gql_type_rec));
    vec_init(&c->fields, sizeof(gql_field_rec));
    vec_init(&c->ivals, sizeof(gql_ival_rec));
    vec_init(&c->evals, sizeof(gql_eval_rec));
    vec_init(&c->trefs, sizeof(gql_tref_rec));
    vec_init(&c->idx, sizeof(uint32_t));
    vec_init(&c->dirs, sizeof(gql_dir_rec));
    buf_init(&c->str);
    hm_init(&c->strmap);
    hm_init(&c->typemap);
    hm_init(&c->dirmap);
    hm_init(&c->trefmap);
    vec_init(&c->t_head, sizeof(uint32_t));
    vec_init(&c->t_tail, sizeof(uint32_t));
    vec_init(&c->t_prelude, sizeof(uint8_t));
    vec_init(&c->tdefs, sizeof(tdef_link));
    vec_init(&c->iv_ast, sizeof(void *));
    vec_init(&c->iv_owner, sizeof(iv_owner));
    vec_init(&c->iv_state, sizeof(uint8_t));
    vec_init(&c->f_ast, sizeof(void *));
    vec_init(&c->ev_ast, sizeof(void *));
    vec_init(&c->d_ast, sizeof(void *));
    vec_init(&c->schema_exts, sizeof(void *));
    vec_init(&c->pending_exts, sizeof(void *));
    vec_init(&c->routes, sizeof(gql_route));
    vec_init(&c->from, sizeof(gql_str));
    vec_init(&c->discr, sizeof(gql_str));
    buf_putc(&c->str, 0);   /* offset 0 = "" */
    c->s.query_type = c->s.mutation_type = c->s.subscription_type = GQL_NONE;

    memset(&po, 0, sizeof po);
    c->user = gql_parse(src, n, &c->arena, &po, errs);
    if (!c->user)
        goto fail;
    errs_init(&perr, 4);
    c->prelude = gql_parse(GQL_PRELUDE, sizeof GQL_PRELUDE - 1, &c->arena, &po, &perr);
    if (!c->prelude) {
        errs_add(errs, NULL, 0, "internal error: prelude does not parse: %s",
                 errs_count(&perr) ? VEC_AT(&perr.items, gql_err_item, 0).msg : "?");
        errs_free(&perr);
        goto fail;
    }
    errs_free(&perr);

    collect(c, c->user, 0);
    collect(c, c->prelude, 1);
    apply_extensions(c);
    build_directives(c);
    build_types(c);
    build_meta(c);
    build_roots(c);
    if (c->failed || reindex(c))
        goto fail;
    c->s.hash = fnv1a64(src, n, fnv1a64(GQL_PRELUDE, sizeof GQL_PRELUDE - 1, FNV64_INIT));
    validate_implementations(c);
    validate_inputs(c);
    validate_defaults_and_deprecation(c);
    if (c->failed || reindex(c))
        goto fail;
    validate_applied_directives(c);
    build_routes(c);
    if (c->failed || reindex(c))
        goto fail;
    return c;
fail:
    gql_compiled_free(c);
    return NULL;
}
