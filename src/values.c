/* values.c - input value validation and coercion. */
#include "values.h"
#include "parser.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- type views ------------------------------------------------------- */

int tv_kind(const gql_schema *s, gql_tv t)
{
    if (t.ast)
        return t.ast->kind == AT_NAMED ? TR_NAMED : t.ast->kind == AT_LIST ? TR_LIST : TR_NONNULL;
    return (int)s->trefs[t.tref].kind;
}

gql_tv tv_of(const gql_schema *s, gql_tv t)
{
    if (t.ast)
        return tv_ast(t.ast->of);
    return tv_tref(s->trefs[t.tref].of);
}

uint32_t tv_named(const gql_schema *s, gql_tv t)
{
    if (t.ast) {
        const ast_type *a = t.ast;
        while (a->kind != AT_NAMED)
            a = a->of;
        return gql_type_by_name(s, a->name.p, a->name.n);
    }
    if (t.tref == GQL_NONE)
        return GQL_NONE;
    return gql_tref_named(s, t.tref);
}

void tv_print(gql_buf *b, const gql_schema *s, gql_tv t)
{
    if (t.ast)
        gql_print_type(b, t.ast);
    else if (t.tref != GQL_NONE)
        gql_print_tref(b, s, t.tref);
}

int tv_is_subtype(const gql_schema *s, gql_tv sub, gql_tv super)
{
    int ks = tv_kind(s, sub), kp = tv_kind(s, super);
    uint32_t a, b;
    if (kp == TR_NONNULL) {
        if (ks == TR_NONNULL)
            return tv_is_subtype(s, tv_of(s, sub), tv_of(s, super));
        return 0;
    }
    if (ks == TR_NONNULL)
        return tv_is_subtype(s, tv_of(s, sub), super);
    if (kp == TR_LIST) {
        if (ks == TR_LIST)
            return tv_is_subtype(s, tv_of(s, sub), tv_of(s, super));
        return 0;
    }
    if (ks == TR_LIST)
        return 0;
    a = tv_named(s, sub);
    b = tv_named(s, super);
    if (a == GQL_NONE || b == GQL_NONE)
        return 0;
    return gql_named_is_subtype(s, a, b);
}

/* ---- literal validation ---------------------------------------------- */

static void verr(gql_vcheck *c, const ast_value *at, const char *fmt, ...) GQL_PRINTF(3, 4);
static void verr(gql_vcheck *c, const ast_value *at, const char *fmt, ...)
{
    gql_buf b;
    va_list ap;
    buf_init(&b);
    va_start(ap, fmt);
    buf_vprintf(&b, fmt, ap);
    va_end(ap);
    c->on_error(c, &at->loc, 1, buf_cstr(&b));
    buf_free(&b);
}

static const char *printed(gql_buf *b, const ast_value *v)
{
    buf_reset(b);
    gql_print_value(b, v);
    return buf_cstr(b);
}

static const char *tvname(gql_buf *b, const gql_schema *s, gql_tv t)
{
    buf_reset(b);
    tv_print(b, s, t);
    return buf_cstr(b);
}

static int int32_literal(gql_str raw, long long *out)
{
    char tmp[32];
    char *end;
    long long v;
    if (raw.n >= sizeof tmp)
        return 0;
    memcpy(tmp, raw.p, raw.n);
    tmp[raw.n] = 0;
    errno = 0;
    v = strtoll(tmp, &end, 10);
    if (errno || *end || v < -2147483648LL || v > 2147483647LL)
        return 0;
    *out = v;
    return 1;
}

static void check_leaf(gql_vcheck *c, uint32_t ti, const ast_value *v)
{
    const gql_schema *s = c->s;
    const gql_type_rec *t = &s->types[ti];
    gql_buf pb;
    long long iv;

    buf_init(&pb);
    if (t->kind == GK_ENUM) {
        if (v->kind != AV_ENUM)
            verr(c, v, "Enum \"%s\" cannot represent non-enum value: %s.", GQL_S(s, t->name), printed(&pb, v));
        else if (gql_member(s, ti, v->s.p, v->s.n) == GQL_NONE)
            verr(c, v, "Value \"%.*s\" does not exist in \"%s\" enum.", (int)v->s.n, v->s.p, GQL_S(s, t->name));
        buf_free(&pb);
        return;
    }
    switch (t->scalar) {
    case SK_INT:
        if (v->kind != AV_INT)
            verr(c, v, "Int cannot represent non-integer value: %s", printed(&pb, v));
        else if (!int32_literal(v->s, &iv))
            verr(c, v, "Int cannot represent non 32-bit signed integer value: %.*s", (int)v->s.n, v->s.p);
        break;
    case SK_FLOAT:
        if (v->kind != AV_INT && v->kind != AV_FLOAT)
            verr(c, v, "Float cannot represent non numeric value: %s", printed(&pb, v));
        break;
    case SK_STRING:
        if (v->kind != AV_STRING)
            verr(c, v, "String cannot represent a non string value: %s", printed(&pb, v));
        break;
    case SK_BOOLEAN:
        if (v->kind != AV_BOOLEAN)
            verr(c, v, "Boolean cannot represent a non boolean value: %s", printed(&pb, v));
        break;
    case SK_ID:
        if (v->kind != AV_STRING && v->kind != AV_INT)
            verr(c, v, "ID cannot represent a non-string and non-integer value: %s", printed(&pb, v));
        break;
    default:
        break;   /* custom scalars accept any literal */
    }
    buf_free(&pb);
}

/* report variables nested in a custom scalar literal (no type information) */
static void untyped_vars(gql_vcheck *c, const ast_value *v)
{
    const ast_value *it;
    const ast_objfield *f;
    if (v->kind == AV_VARIABLE) {
        if (c->on_var)
            c->on_var(c, v, tv_none(), 0, GQL_NONE);
    } else if (v->kind == AV_LIST) {
        for (it = v->items; it; it = it->next)
            untyped_vars(c, it);
    } else if (v->kind == AV_OBJECT) {
        for (f = v->fields; f; f = f->next)
            untyped_vars(c, f->value);
    }
}

void gql_check_literal(gql_vcheck *c, gql_tv type, const ast_value *v, int has_default,
                       uint32_t parent_input)
{
    const gql_schema *s = c->s;
    gql_buf tb, pb;
    uint32_t ti;
    int k;

    if (!tv_valid(type)) {
        untyped_vars(c, v);
        return;
    }
    if (v->kind == AV_VARIABLE) {
        if (c->on_var)
            c->on_var(c, v, type, has_default, parent_input);
        return;
    }
    buf_init(&tb);
    buf_init(&pb);
    k = tv_kind(s, type);
    if (k == TR_NONNULL) {
        if (v->kind == AV_NULL)
            verr(c, v, "Expected value of non-null type \"%s\" not to be null.", tvname(&tb, s, type));
        else
            gql_check_literal(c, tv_of(s, type), v, 0, parent_input);
        goto out;
    }
    if (v->kind == AV_NULL)
        goto out;
    if (k == TR_LIST) {
        gql_tv it = tv_of(s, type);
        const ast_value *item;
        if (v->kind != AV_LIST) {
            gql_check_literal(c, it, v, 0, GQL_NONE);
        } else {
            uint32_t i = 0;
            for (item = v->items; item; item = item->next, i++) {
                size_t mark = c->path ? c->path->len : 0;
                if (c->path)
                    buf_printf(c->path, "[%u]", i);
                gql_check_literal(c, it, item, 0, GQL_NONE);
                if (c->path)
                    c->path->len = mark;
            }
        }
        goto out;
    }
    ti = tv_named(s, type);
    if (ti == GQL_NONE) {
        untyped_vars(c, v);
        goto out;
    }
    if (s->types[ti].kind == GK_INPUT) {
        const gql_type_rec *t = &s->types[ti];
        const ast_objfield *f, *g;
        uint32_t i, known = 0;
        const ast_objfield *last_known = NULL;
        if (v->kind != AV_OBJECT) {
            verr(c, v, "Expected value of type \"%s\" to be an object, found: %s.",
                 GQL_S(s, t->name), printed(&pb, v));
            goto out;
        }
        for (i = 0; i < t->fields_count; i++) {
            const gql_ival_rec *fd = &s->ivals[t->fields_first + i];
            const ast_objfield *node = NULL;
            for (f = v->fields; f; f = f->next)
                if (f->name.n == fd->name.len && memcmp(f->name.p, GQL_S(s, fd->name), f->name.n) == 0)
                    node = f;   /* keyMap: last wins */
            if (!node) {
                if (gql_tref_nonnull(s, fd->type) && !(fd->flags & MF_HAS_DEFAULT))
                    verr(c, v, "Expected value of type \"%s\" to include required field \"%s\", found: %s.",
                         GQL_S(s, t->name), GQL_S(s, fd->name), printed(&pb, v));
            } else {
                size_t mark = c->path ? c->path->len : 0;
                if (c->path) {
                    buf_putc(c->path, '.');
                    buf_put(c->path, GQL_S(s, fd->name), fd->name.len);
                }
                gql_check_literal(c, tv_tref(fd->type), node->value, (fd->flags & MF_HAS_DEFAULT) != 0, ti);
                if (c->path)
                    c->path->len = mark;
            }
        }
        for (f = v->fields; f; f = f->next) {
            if (gql_member(s, ti, f->name.p, f->name.n) == GQL_NONE) {
                gql_loc l = f->loc;
                buf_reset(&pb);
                gql_print_value(&pb, v);
                {
                    gql_buf m;
                    buf_init(&m);
                    buf_printf(&m, "Expected value of type \"%s\" not to include unknown field \"%.*s\", found: %s.",
                               GQL_S(s, t->name), (int)f->name.n, f->name.p, buf_cstr(&pb));
                    c->on_error(c, &l, 1, buf_cstr(&m));
                    buf_free(&m);
                }
            } else {
                known++;
                last_known = f;
            }
            for (g = f->next; g; g = g->next) {
                if (gql_str_eq(f->name, g->name)) {
                    gql_loc ls[2];
                    gql_buf m;
                    ls[0] = f->loc;
                    ls[1] = g->loc;
                    buf_init(&m);
                    buf_printf(&m, "There can be only one input field named \"%.*s\".", (int)f->name.n, f->name.p);
                    c->on_error(c, ls, 2, buf_cstr(&m));
                    buf_free(&m);
                    break;
                }
            }
        }
        if (t->flags & TF_ONEOF) {
            if (known != 1 || last_known->value->kind == AV_NULL)
                verr(c, v, "Within OneOf Input Object type \"%s\", exactly one field must be specified, "
                     "and the value for that field must be non-null.", GQL_S(s, t->name));
        }
        goto out;
    }
    check_leaf(c, ti, v);
    if (s->types[ti].kind == GK_SCALAR && s->types[ti].scalar == SK_CUSTOM)
        untyped_vars(c, v);
out:
    buf_free(&tb);
    buf_free(&pb);
}

/* ---- literal -> JSON ---------------------------------------------------- */

static void untyped_literal_json(const gql_vctx *c, const ast_value *v, gql_buf *out)
{
    const ast_value *it;
    const ast_objfield *f;
    const gql_str *js;
    switch (v->kind) {
    case AV_VARIABLE:
        js = c->var ? c->var(c->ud, v->s) : NULL;
        if (js)
            buf_put(out, js->p, js->n);
        else
            buf_puts(out, "null");
        break;
    case AV_INT: case AV_FLOAT:
        buf_put(out, v->s.p, v->s.n);
        break;
    case AV_STRING: case AV_ENUM:
        buf_json_str(out, v->s.p, v->s.n);
        break;
    case AV_BOOLEAN:
        buf_puts(out, v->boolean ? "true" : "false");
        break;
    case AV_NULL:
        buf_puts(out, "null");
        break;
    case AV_LIST:
        buf_putc(out, '[');
        for (it = v->items; it; it = it->next) {
            untyped_literal_json(c, it, out);
            if (it->next)
                buf_putc(out, ',');
        }
        buf_putc(out, ']');
        break;
    case AV_OBJECT:
        buf_putc(out, '{');
        for (f = v->fields; f; f = f->next) {
            buf_json_str(out, f->name.p, f->name.n);
            buf_putc(out, ':');
            untyped_literal_json(c, f->value, out);
            if (f->next)
                buf_putc(out, ',');
        }
        buf_putc(out, '}');
        break;
    }
}

int gql_literal_to_json(const gql_vctx *c, gql_tv type, const ast_value *v, gql_buf *out)
{
    const gql_schema *s = c->s;
    uint32_t ti;
    int k;
    long long iv;

    if (v->kind == AV_VARIABLE) {
        const gql_str *js = c->var ? c->var(c->ud, v->s) : NULL;
        if (!js)
            return 0;
        buf_put(out, js->p, js->n);
        return 1;
    }
    if (v->kind == AV_NULL) {
        buf_puts(out, "null");
        return 1;
    }
    k = tv_kind(s, type);
    if (k == TR_NONNULL)
        return gql_literal_to_json(c, tv_of(s, type), v, out);
    if (k == TR_LIST) {
        gql_tv it = tv_of(s, type);
        if (v->kind == AV_LIST) {
            const ast_value *item;
            buf_putc(out, '[');
            for (item = v->items; item; item = item->next) {
                size_t mark = out->len;
                if (item != v->items)
                    buf_putc(out, ',');
                if (!gql_literal_to_json(c, it, item, out)) {
                    out->len = mark;
                    if (item != v->items)
                        buf_putc(out, ',');
                    buf_puts(out, "null");
                }
            }
            buf_putc(out, ']');
            return 1;
        } else {
            size_t mark = out->len;
            buf_putc(out, '[');
            if (!gql_literal_to_json(c, it, v, out)) {
                out->len = mark;
                return 0;
            }
            buf_putc(out, ']');
            return 1;
        }
    }
    ti = tv_named(s, type);
    if (ti == GQL_NONE) {
        untyped_literal_json(c, v, out);
        return 1;
    }
    switch (s->types[ti].kind) {
    case GK_INPUT: {
        const gql_type_rec *t = &s->types[ti];
        uint32_t i;
        int first = 1;
        buf_putc(out, '{');
        for (i = 0; i < t->fields_count; i++) {
            const gql_ival_rec *fd = &s->ivals[t->fields_first + i];
            const ast_objfield *f, *node = NULL;
            size_t mark = out->len;
            if (v->kind == AV_OBJECT)
                for (f = v->fields; f; f = f->next)
                    if (f->name.n == fd->name.len && memcmp(f->name.p, GQL_S(s, fd->name), f->name.n) == 0)
                        node = f;
            if (!first)
                buf_putc(out, ',');
            buf_json_str(out, GQL_S(s, fd->name), fd->name.len);
            buf_putc(out, ':');
            if (node && gql_literal_to_json(c, tv_tref(fd->type), node->value, out)) {
                first = 0;
                continue;
            }
            if (fd->flags & MF_HAS_DEFAULT) {
                buf_put(out, GQL_S(s, fd->defjson), fd->defjson.len);
                first = 0;
                continue;
            }
            out->len = mark;   /* omit */
        }
        buf_putc(out, '}');
        return 1;
    }
    case GK_ENUM:
        buf_json_str(out, v->s.p, v->s.n);
        return 1;
    case GK_SCALAR:
        switch (s->types[ti].scalar) {
        case SK_INT:
            if (int32_literal(v->s, &iv))
                buf_printf(out, "%lld", iv);
            else
                buf_puts(out, "null");
            return 1;
        case SK_FLOAT:
            buf_put(out, v->s.p, v->s.n);
            return 1;
        case SK_STRING:
        case SK_ID:
            buf_json_str(out, v->s.p, v->s.n);
            return 1;
        case SK_BOOLEAN:
            buf_puts(out, v->boolean ? "true" : "false");
            return 1;
        default:
            untyped_literal_json(c, v, out);
            return 1;
        }
    }
    untyped_literal_json(c, v, out);
    return 1;
}

/* ---- JSON coercion ---------------------------------------------------- */

static int is_ident(const char *p, uint32_t n)
{
    uint32_t i;
    if (!n || !(p[0] == '_' || p[0] == '$' || (p[0] >= 'a' && p[0] <= 'z') || (p[0] >= 'A' && p[0] <= 'Z')))
        return 0;
    for (i = 1; i < n; i++)
        if (!(p[i] == '_' || p[i] == '$' || (p[i] >= 'a' && p[i] <= 'z') || (p[i] >= 'A' && p[i] <= 'Z') ||
              (p[i] >= '0' && p[i] <= '9')))
            return 0;
    return 1;
}

void gql_json_inspect(gql_buf *b, const gql_json *v, int depth)
{
    const gql_json *m;
    if (!v) {
        buf_puts(b, "undefined");
        return;
    }
    switch (v->kind) {
    case JS_NULL: buf_puts(b, "null"); break;
    case JS_BOOL: buf_puts(b, v->boolean ? "true" : "false"); break;
    case JS_NUMBER: buf_put(b, v->s.p, v->s.n); break;
    case JS_STRING: buf_json_str(b, v->s.p, v->s.n); break;
    case JS_ARRAY:
        if (!v->count) {
            buf_puts(b, "[]");
            break;
        }
        if (depth >= 2) {
            buf_puts(b, "[Array]");
            break;
        }
        buf_putc(b, '[');
        for (m = v->first; m; m = m->next) {
            gql_json_inspect(b, m, depth + 1);
            if (m->next)
                buf_puts(b, ", ");
        }
        buf_putc(b, ']');
        break;
    case JS_OBJECT:
        if (!v->count) {
            buf_puts(b, "{}");
            break;
        }
        if (depth >= 2) {
            buf_puts(b, "[Object]");
            break;
        }
        buf_puts(b, "{ ");
        for (m = v->first; m; m = m->next) {
            if (is_ident(m->key.p, m->key.n))
                buf_put(b, m->key.p, m->key.n);
            else
                buf_json_str(b, m->key.p, m->key.n);
            buf_puts(b, ": ");
            gql_json_inspect(b, m, depth + 1);
            if (m->next)
                buf_puts(b, ", ");
        }
        buf_puts(b, " }");
        break;
    }
}

typedef struct {
    const gql_schema *s;
    gql_buf *path, *msg;
    int failed;
} CJ;

static int cfail(CJ *c, const char *fmt, ...) GQL_PRINTF(2, 3);
static int cfail(CJ *c, const char *fmt, ...)
{
    va_list ap;
    if (c->failed)
        return -1;
    c->failed = 1;
    va_start(ap, fmt);
    buf_vprintf(c->msg, fmt, ap);
    va_end(ap);
    return -1;
}

static const char *insp(gql_buf *b, const gql_json *v)
{
    buf_reset(b);
    gql_json_inspect(b, v, 0);
    return buf_cstr(b);
}

static int json_number(const gql_json *v, double *out)
{
    char tmp[64];
    char *end;
    if (v->s.n >= sizeof tmp)
        return 0;
    memcpy(tmp, v->s.p, v->s.n);
    tmp[v->s.n] = 0;
    *out = strtod(tmp, &end);
    return *end == 0;
}

static int coerce(CJ *c, gql_tv type, const gql_json *v, gql_buf *out)
{
    const gql_schema *s = c->s;
    gql_buf tb, ib;
    int k = tv_kind(s, type), rc = 0;
    uint32_t ti;
    double d;

    buf_init(&tb);
    buf_init(&ib);
    if (k == TR_NONNULL) {
        if (!v)
            rc = cfail(c, "Expected a value of non-null type \"%s\" to be provided.", tvname(&tb, s, type));
        else if (v->kind == JS_NULL)
            rc = cfail(c, "Expected value of non-null type \"%s\" not to be null.", tvname(&tb, s, type));
        else
            rc = coerce(c, tv_of(s, type), v, out);
        goto out;
    }
    if (!v || v->kind == JS_NULL) {
        buf_puts(out, "null");
        goto out;
    }
    if (k == TR_LIST) {
        gql_tv it = tv_of(s, type);
        buf_putc(out, '[');
        if (v->kind == JS_ARRAY) {
            const gql_json *m;
            uint32_t i = 0;
            for (m = v->first; m && !rc; m = m->next, i++) {
                size_t pm = c->path->len;
                if (m != v->first)
                    buf_putc(out, ',');
                buf_printf(c->path, "[%u]", i);
                rc = coerce(c, it, m, out);
                if (!rc)
                    c->path->len = pm;
            }
        } else {
            rc = coerce(c, it, v, out);
        }
        buf_putc(out, ']');
        goto out;
    }
    ti = tv_named(s, type);
    if (ti == GQL_NONE) {
        gql_json_write(out, v);
        goto out;
    }
    switch (s->types[ti].kind) {
    case GK_INPUT: {
        const gql_type_rec *t = &s->types[ti];
        const gql_json *m;
        uint32_t i, nset = 0;
        int first = 1;
        const char *tn = GQL_S(s, t->name);
        if (v->kind != JS_OBJECT) {
            rc = cfail(c, "Expected value of type \"%s\" to be an object, found: %s.", tn, insp(&ib, v));
            goto out;
        }
        buf_putc(out, '{');
        for (i = 0; i < t->fields_count && !rc; i++) {
            const gql_ival_rec *fd = &s->ivals[t->fields_first + i];
            const gql_json *fv = gql_json_get(v, GQL_S(s, fd->name), fd->name.len);
            size_t pm = c->path->len;
            if (!fv) {
                if (fd->flags & MF_HAS_DEFAULT) {
                    if (!first)
                        buf_putc(out, ',');
                    buf_json_str(out, GQL_S(s, fd->name), fd->name.len);
                    buf_putc(out, ':');
                    buf_put(out, GQL_S(s, fd->defjson), fd->defjson.len);
                    first = 0;
                } else if (gql_tref_nonnull(s, fd->type)) {
                    rc = cfail(c, "Expected value of type \"%s\" to include required field \"%s\", found: %s.",
                               tn, GQL_S(s, fd->name), insp(&ib, v));
                }
                continue;
            }
            if (!first)
                buf_putc(out, ',');
            buf_json_str(out, GQL_S(s, fd->name), fd->name.len);
            buf_putc(out, ':');
            first = 0;
            buf_putc(c->path, '.');
            buf_put(c->path, GQL_S(s, fd->name), fd->name.len);
            rc = coerce(c, tv_tref(fd->type), fv, out);
            if (!rc)
                c->path->len = pm;
        }
        buf_putc(out, '}');
        if (rc)
            goto out;
        for (m = v->first; m; m = m->next) {
            if (gql_member(s, ti, m->key.p, m->key.n) == GQL_NONE) {
                rc = cfail(c, "Expected value of type \"%s\" not to include unknown field \"%.*s\", found: %s.",
                           tn, (int)m->key.n, m->key.p, insp(&ib, v));
                goto out;
            }
            nset++;
        }
        if (t->flags & TF_ONEOF) {
            const gql_json *only = NULL;
            for (m = v->first; m; m = m->next)
                only = m;
            if (nset != 1 || only->kind == JS_NULL)
                rc = cfail(c, "Within OneOf Input Object type \"%s\", exactly one field must be specified, "
                           "and the value for that field must be non-null.", tn);
        }
        goto out;
    }
    case GK_ENUM: {
        const char *tn = GQL_S(s, s->types[ti].name);
        if (v->kind != JS_STRING)
            rc = cfail(c, "Enum \"%s\" cannot represent non-string value: %s.", tn, insp(&ib, v));
        else if (gql_member(s, ti, v->s.p, v->s.n) == GQL_NONE)
            rc = cfail(c, "Value \"%.*s\" does not exist in \"%s\" enum.", (int)v->s.n, v->s.p, tn);
        else
            buf_json_str(out, v->s.p, v->s.n);
        goto out;
    }
    default:
        break;
    }
    switch (s->types[ti].scalar) {
    case SK_INT:
        if (v->kind != JS_NUMBER || !json_number(v, &d) || !isfinite(d) || floor(d) != d)
            rc = cfail(c, "Int cannot represent non-integer value: %s", insp(&ib, v));
        else if (d > 2147483647.0 || d < -2147483648.0)
            rc = cfail(c, "Int cannot represent non 32-bit signed integer value: %s", insp(&ib, v));
        else
            buf_printf(out, "%d", (int)d);
        break;
    case SK_FLOAT:
        if (v->kind != JS_NUMBER || !json_number(v, &d) || !isfinite(d))
            rc = cfail(c, "Float cannot represent non numeric value: %s", insp(&ib, v));
        else
            buf_put(out, v->s.p, v->s.n);
        break;
    case SK_STRING:
        if (v->kind != JS_STRING)
            rc = cfail(c, "String cannot represent a non string value: %s", insp(&ib, v));
        else
            buf_json_str(out, v->s.p, v->s.n);
        break;
    case SK_BOOLEAN:
        if (v->kind != JS_BOOL)
            rc = cfail(c, "Boolean cannot represent a non boolean value: %s", insp(&ib, v));
        else
            buf_puts(out, v->boolean ? "true" : "false");
        break;
    case SK_ID:
        if (v->kind == JS_STRING)
            buf_json_str(out, v->s.p, v->s.n);
        else if (v->kind == JS_NUMBER && json_number(v, &d) && isfinite(d) && floor(d) == d &&
                 fabs(d) < 9007199254740992.0)
            buf_printf(out, "\"%.0f\"", d);
        else
            rc = cfail(c, "ID cannot represent value: %s", insp(&ib, v));
        break;
    default:
        gql_json_write(out, v);
    }
out:
    buf_free(&tb);
    buf_free(&ib);
    return rc;
}

int gql_coerce_json(const gql_schema *s, gql_tv type, const gql_json *v, gql_buf *out,
                    gql_buf *err_path, gql_buf *err_msg)
{
    CJ c;
    c.s = s;
    c.path = err_path;
    c.msg = err_msg;
    c.failed = 0;
    return coerce(&c, type, v, out);
}
