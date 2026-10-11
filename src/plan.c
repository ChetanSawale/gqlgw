/* plan.c - operation selection, variable coercion, CollectFields and the flat
 * execution plan consumed by the Lua executor.
 *
 * The plan holds, for every (concrete object type, merged selection sets)
 * pair, the ordered list of response fields with their coerced arguments.
 * Abstract fields get one selection per possible object type ("cases").
 * Selections are memoized, so the plan is a DAG and fragment-heavy queries
 * stay small.
 */
#include "plan_int.h"
#include "errors.h"
#include "json.h"
#include "parser.h"
#include "values.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- collector --------------------------------------------------------------- */

const gql_str *gql_collector_var(gql_collector *c, gql_str name)
{
    uint32_t i;
    for (i = 0; i < c->nvars; i++)
        if (gql_str_eq(c->vars[i].name, name))
            return &c->vars[i].json;
    return NULL;
}

static const gql_str *vctx_var(void *ud, gql_str name)
{
    return gql_collector_var(ud, name);
}

static int value_bool(gql_collector *c, const ast_value *v, int dflt)
{
    if (!v)
        return dflt;
    if (v->kind == AV_BOOLEAN)
        return v->boolean;
    if (v->kind == AV_VARIABLE) {
        const gql_str *js = gql_collector_var(c, v->s);
        if (!js)
            return dflt;
        return js->n == 4 && memcmp(js->p, "true", 4) == 0;
    }
    return dflt;
}

static const ast_value *arg_value(const ast_arg *args, const char *name)
{
    for (; args; args = args->next)
        if (gql_str_eqz(args->name, name))
            return args->value;
    return NULL;
}

int gql_arg_bool(gql_collector *c, const ast_selection *node, const char *name, int dflt)
{
    return value_bool(c, arg_value(node->args, name), dflt);
}

int gql_arg_string(gql_collector *c, const ast_selection *node, const char *name, gql_str *out, gql_buf *tmp)
{
    const ast_value *v = arg_value(node->args, name);
    if (!v)
        return 0;
    if (v->kind == AV_STRING) {
        *out = v->s;
        return 1;
    }
    if (v->kind == AV_VARIABLE) {
        const gql_str *js = gql_collector_var(c, v->s);
        gql_json *j;
        char err[64];
        if (!js)
            return 0;
        j = gql_json_parse(js->p, js->n, c->arena, 4, err, sizeof err);
        if (!j || j->kind != JS_STRING)
            return 0;
        *out = j->s;
        (void)tmp;
        return 1;
    }
    return 0;
}

static int skipped(gql_collector *c, const ast_directive *d)
{
    for (; d; d = d->next) {
        if (gql_str_eqz(d->name, "skip") && value_bool(c, arg_value(d->args, "if"), 0))
            return 1;
        if (gql_str_eqz(d->name, "include") && !value_bool(c, arg_value(d->args, "if"), 1))
            return 1;
    }
    return 0;
}

static int applies(gql_collector *c, uint32_t objtype, gql_str cond)
{
    uint32_t t;
    if (!cond.n)
        return 1;
    t = gql_type_by_name(c->s, cond.p, cond.n);
    if (t == GQL_NONE)
        return 0;
    return t == objtype || gql_is_possible(c->s, t, objtype);
}

typedef struct {
    gql_hmap gmap;       /* response key -> group */
    gql_vec keys;        /* gql_str per group */
    gql_vec gnodes;      /* gnode */
    gql_vec visited;     /* gql_str fragment names */
} CC;

typedef struct {
    uint32_t g;
    const ast_selection *node;
} gnode;

static void collect_rec(gql_collector *c, CC *cc, uint32_t objtype, const ast_selset *ss)
{
    const ast_selection *sel;
    for (sel = ss->first; sel; sel = sel->next) {
        if (skipped(c, sel->dirs))
            continue;
        if (sel->kind == SEL_FIELD) {
            gql_str key = sel->alias.n ? sel->alias : sel->name;
            uint32_t g = hm_get(&cc->gmap, 0, key.p, key.n);
            gnode *gn;
            if (g == GQL_NONE) {
                gql_str *k = vec_push(&cc->keys);
                if (!k)
                    return;
                *k = key;
                g = cc->keys.len - 1;
                hm_put(&cc->gmap, 0, key.p, key.n, g);
            }
            gn = vec_push(&cc->gnodes);
            if (gn) {
                gn->g = g;
                gn->node = sel;
            }
        } else if (sel->kind == SEL_SPREAD) {
            uint32_t k, fi;
            const ast_def *fd;
            for (k = 0; k < cc->visited.len; k++)
                if (gql_str_eq(VEC_AT(&cc->visited, gql_str, k), sel->name))
                    break;
            if (k < cc->visited.len)
                continue;
            {
                gql_str *slot = vec_push(&cc->visited);
                if (slot)
                    *slot = sel->name;
            }
            fi = c->frags ? hm_get(c->frags, 0, sel->name.p, sel->name.n) : GQL_NONE;
            if (fi == GQL_NONE)
                continue;
            fd = c->fragdefs[fi];
            if (!applies(c, objtype, fd->type_cond))
                continue;
            collect_rec(c, cc, objtype, fd->selset);
        } else {
            if (!applies(c, objtype, sel->name))
                continue;
            collect_rec(c, cc, objtype, sel->selset);
        }
    }
}

int gql_collect(gql_collector *c, uint32_t objtype, const ast_selset *const *sets, uint32_t nsets,
                gql_group **out, uint32_t *nout)
{
    CC cc;
    uint32_t i, ng;
    gql_group *groups;
    uint32_t *fill;
    int rc = 0;

    hm_init(&cc.gmap);
    vec_init(&cc.keys, sizeof(gql_str));
    vec_init(&cc.gnodes, sizeof(gnode));
    vec_init(&cc.visited, sizeof(gql_str));
    for (i = 0; i < nsets; i++)
        collect_rec(c, &cc, objtype, sets[i]);
    ng = cc.keys.len;
    groups = arena_alloc(c->arena, (ng + 1) * sizeof *groups);
    fill = calloc(ng + 1, sizeof *fill);
    if (!groups || !fill || cc.keys.oom || cc.gnodes.oom || cc.gmap.oom) {
        rc = -1;
        goto out;
    }
    for (i = 0; i < cc.gnodes.len; i++)
        groups[VEC_AT(&cc.gnodes, gnode, i).g].n++;
    for (i = 0; i < ng; i++) {
        groups[i].key = VEC_AT(&cc.keys, gql_str, i);
        groups[i].nodes = arena_alloc(c->arena, groups[i].n * sizeof(ast_selection *));
        if (!groups[i].nodes) {
            rc = -1;
            goto out;
        }
    }
    for (i = 0; i < cc.gnodes.len; i++) {
        gnode *gn = &VEC_AT(&cc.gnodes, gnode, i);
        groups[gn->g].nodes[fill[gn->g]++] = gn->node;
    }
    *out = groups;
    *nout = ng;
out:
    free(fill);
    hm_free(&cc.gmap);
    vec_free(&cc.keys);
    vec_free(&cc.gnodes);
    vec_free(&cc.visited);
    return rc;
}

/* ---- plan --------------------------------------------------------------------- */

struct gql_plan {
    gql_arena arena;
    gql_vec sels, fields, cases;
    gql_buf str;
    gql_plan_view view;
    gql_errors errs;
    gql_buf errjson;
    int ok;
    gql_hmap memo;
    uint32_t max_fields;
    gql_collector col;
};

static void perr(gql_plan *p, const gql_loc *l, const char *fmt, ...) GQL_PRINTF(3, 4);
static void perr(gql_plan *p, const gql_loc *l, const char *fmt, ...)
{
    va_list ap;
    p->ok = 0;
    va_start(ap, fmt);
    errs_addv(&p->errs, l, l ? 1 : 0, fmt, ap);
    va_end(ap);
}

static uint32_t put_str(gql_plan *p, const char *s, size_t n)
{
    uint32_t off = (uint32_t)p->str.len;
    buf_put(&p->str, s, n);
    return off;
}

static int coerce_args(gql_plan *p, const gql_field_rec *f, const ast_selection *node, gql_buf *out)
{
    const gql_schema *s = p->col.s;
    gql_vctx vx;
    uint32_t k;
    int first = 1;
    vx.s = s;
    vx.var = vctx_var;
    vx.ud = &p->col;
    buf_putc(out, '{');
    for (k = 0; k < f->args_count; k++) {
        const gql_ival_rec *a = &s->ivals[f->args_first + k];
        const ast_arg *an;
        size_t mark = out->len, vstart;
        for (an = node->args; an; an = an->next)
            if (an->name.n == a->name.len && memcmp(an->name.p, GQL_S(s, a->name), an->name.n) == 0)
                break;
        if (!first)
            buf_putc(out, ',');
        buf_json_str(out, GQL_S(s, a->name), a->name.len);
        buf_putc(out, ':');
        vstart = out->len;
        if (an && gql_literal_to_json(&vx, tv_tref(a->type), an->value, out)) {
            if (gql_tref_nonnull(s, a->type) && out->len - vstart == 4 && memcmp(out->p + vstart, "null", 4) == 0) {
                gql_buf tb;
                buf_init(&tb);
                gql_print_tref(&tb, s, a->type);
                perr(p, &an->loc, "Argument \"%s\" of non-null type \"%s\" must not be null.", GQL_S(s, a->name),
                     buf_cstr(&tb));
                buf_free(&tb);
                return -1;
            }
            first = 0;
            continue;
        }
        out->len = vstart;
        if (a->flags & MF_HAS_DEFAULT) {
            buf_put(out, GQL_S(s, a->defjson), a->defjson.len);
            first = 0;
            continue;
        }
        if (gql_tref_nonnull(s, a->type)) {
            gql_buf tb;
            buf_init(&tb);
            gql_print_tref(&tb, s, a->type);
            perr(p, &node->loc, "Argument \"%s\" of required type \"%s\" was not provided.", GQL_S(s, a->name),
                 buf_cstr(&tb));
            buf_free(&tb);
            return -1;
        }
        out->len = mark;
    }
    buf_putc(out, '}');
    return 0;
}

static uint32_t build_sel(gql_plan *p, uint32_t objtype, const ast_selset *const *sets, uint32_t nsets);

static int subsets(gql_plan *p, const gql_group *g, const ast_selset ***out, uint32_t *n)
{
    uint32_t k, m = 0;
    const ast_selset **arr = arena_alloc(&p->arena, (g->n + 1) * sizeof *arr);
    if (!arr)
        return -1;
    for (k = 0; k < g->n; k++)
        if (g->nodes[k]->selset)
            arr[m++] = g->nodes[k]->selset;
    *out = arr;
    *n = m;
    return 0;
}

static uint32_t build_sel(gql_plan *p, uint32_t objtype, const ast_selset *const *sets, uint32_t nsets)
{
    const gql_schema *s = p->col.s;
    gql_group *groups;
    uint32_t ng, gi, si, first, keylen;
    uintptr_t *key;
    gql_plan_sel *sel;
    gql_buf tmp;

    /* memoization on (type, selection sets) */
    keylen = (nsets + 1) * (uint32_t)sizeof(uintptr_t);
    key = arena_alloc(&p->arena, keylen);
    if (!key)
        goto oom;
    key[0] = objtype;
    memcpy(key + 1, sets, nsets * sizeof(uintptr_t));
    si = hm_get(&p->memo, 0, (const char *)key, keylen);
    if (si != GQL_NONE)
        return si;

    if (gql_collect(&p->col, objtype, sets, nsets, &groups, &ng))
        goto oom;
    if (p->max_fields && p->fields.len + ng > p->max_fields) {
        perr(p, NULL, "Query is too complex: its execution plan exceeds %u fields.", p->max_fields);
        return GQL_NONE;
    }
    sel = vec_push(&p->sels);
    if (!sel)
        goto oom;
    si = p->sels.len - 1;
    first = p->fields.len;
    sel->type = objtype;
    sel->fields_first = first;
    sel->fields_count = ng;
    for (gi = 0; gi < ng; gi++)
        if (!vec_push(&p->fields))
            goto oom;
    hm_put(&p->memo, 0, (const char *)key, keylen, si);

    buf_init(&tmp);
    for (gi = 0; gi < ng && p->ok; gi++) {
        const gql_group *g = &groups[gi];
        const ast_selection *n0 = g->nodes[0];
        uint32_t fdef = gql_field_lookup(s, objtype, n0->name.p, n0->name.n);
        gql_plan_field pf;

        memset(&pf, 0, sizeof pf);
        pf.key_off = put_str(p, g->key.p, g->key.n);
        pf.key_len = g->key.n;
        pf.line = n0->loc.line;
        pf.col = n0->loc.col;
        if (fdef == GQL_NONE) {
            perr(p, &n0->loc, "Cannot query field \"%.*s\" on type \"%s\".", (int)n0->name.n, n0->name.p,
                 GQL_S(s, s->types[objtype].name));
            break;
        }
        pf.field = fdef;
        if (fdef == s->f_typename) {
            pf.kind = PF_LEAF;
        } else if (fdef == s->f_schema || fdef == s->f_type) {
            buf_reset(&tmp);
            if (gql_introspect(&p->col, fdef, g, &tmp)) {
                perr(p, &n0->loc, "Failed to render introspection.");
                break;
            }
            pf.kind = PF_RAW;
            pf.args_off = put_str(p, tmp.p ? tmp.p : "", tmp.len);
            pf.args_len = (uint32_t)tmp.len;
        } else {
            const gql_field_rec *f = &s->fields[fdef];
            uint32_t nt = gql_tref_named(s, f->type);
            const ast_selset **ss;
            uint32_t nss;
            if (f->args_count) {
                buf_reset(&tmp);
                if (coerce_args(p, f, n0, &tmp))
                    break;
                pf.args_off = put_str(p, tmp.p, tmp.len);
                pf.args_len = (uint32_t)tmp.len;
            }
            switch (s->types[nt].kind) {
            case GK_OBJECT:
                pf.kind = PF_OBJECT;
                if (subsets(p, g, &ss, &nss))
                    goto oom2;
                pf.child_first = build_sel(p, nt, ss, nss);
                pf.child_count = 1;
                if (pf.child_first == GQL_NONE)
                    goto done;
                break;
            case GK_INTERFACE:
            case GK_UNION: {
                const gql_type_rec *t = &s->types[nt];
                uint32_t k, cfirst = p->cases.len;
                pf.kind = PF_ABSTRACT;
                if (subsets(p, g, &ss, &nss))
                    goto oom2;
                for (k = 0; k < t->possible_count; k++)
                    if (!vec_push(&p->cases))
                        goto oom2;
                pf.child_first = cfirst;
                pf.child_count = t->possible_count;
                for (k = 0; k < t->possible_count && p->ok; k++) {
                    uint32_t ot = s->idx[t->possible_first + k];
                    uint32_t cs = build_sel(p, ot, ss, nss);
                    if (cs == GQL_NONE)
                        goto done;
                    VEC_AT(&p->cases, gql_plan_case, cfirst + k).type = ot;
                    VEC_AT(&p->cases, gql_plan_case, cfirst + k).sel = cs;
                }
                break;
            }
            default:
                pf.kind = PF_LEAF;
            }
        }
        VEC_AT(&p->fields, gql_plan_field, first + gi) = pf;
    }
done:
    buf_free(&tmp);
    return p->ok ? si : GQL_NONE;
oom2:
    buf_free(&tmp);
oom:
    perr(p, NULL, "Out of memory while planning the query.");
    return GQL_NONE;
}

static int coerce_variables(gql_plan *p, const ast_def *op, const gql_json *vars)
{
    const gql_schema *s = p->col.s;
    const ast_vardef *vd;
    uint32_t n = 0;
    gql_buf out, path, msg;
    gql_vctx vx;

    for (vd = op->vars; vd; vd = vd->next)
        n++;
    p->col.vars = arena_alloc(&p->arena, (n + 1) * sizeof(gql_varval));
    p->col.nvars = 0;
    if (!p->col.vars)
        return -1;
    vx.s = s;
    vx.var = NULL;
    vx.ud = NULL;
    buf_init(&out);
    buf_init(&path);
    buf_init(&msg);
    for (vd = op->vars; vd; vd = vd->next) {
        const gql_json *v = vars ? gql_json_get(vars, vd->name.p, vd->name.n) : NULL;
        buf_reset(&out);
        buf_reset(&path);
        buf_reset(&msg);
        if (!v) {
            if (vd->defval) {
                gql_literal_to_json(&vx, tv_ast(vd->type), vd->defval, &out);
            } else if (vd->type->kind == AT_NONNULL) {
                gql_buf tb;
                buf_init(&tb);
                gql_print_type(&tb, vd->type);
                perr(p, &vd->loc, "Variable \"$%.*s\" has invalid value: Expected a value of non-null type \"%s\" "
                     "to be provided.", (int)vd->name.n, vd->name.p, buf_cstr(&tb));
                buf_free(&tb);
                continue;
            } else {
                continue;
            }
        } else if (gql_coerce_json(s, tv_ast(vd->type), v, &out, &path, &msg)) {
            perr(p, &vd->loc, "Variable \"$%.*s\" has invalid value%s%s: %s", (int)vd->name.n, vd->name.p,
                 path.len ? " at " : "", path.len ? buf_cstr(&path) : "", buf_cstr(&msg));
            continue;
        }
        p->col.vars[p->col.nvars].name = vd->name;
        p->col.vars[p->col.nvars].json.p = arena_strndup(&p->arena, out.p ? out.p : "", out.len);
        p->col.vars[p->col.nvars].json.n = (uint32_t)out.len;
        if (!p->col.vars[p->col.nvars].json.p)
            perr(p, NULL, "Out of memory.");
        p->col.nvars++;
    }
    buf_free(&out);
    buf_free(&path);
    buf_free(&msg);
    return p->ok ? 0 : -1;
}

/* entry point used by api.c */
gql_plan *gql_plan_build_internal(const gql_schema *s, const ast_doc *doc, const gql_hmap *frags,
                                  const ast_def **fragdefs, uint32_t nfrags, const char *opname, size_t opn,
                                  const char *vars, size_t varsn, const gql_limits *lim);

gql_plan *gql_plan_build_internal(const gql_schema *s, const ast_doc *doc, const gql_hmap *frags,
                                  const ast_def **fragdefs, uint32_t nfrags, const char *opname, size_t opn,
                                  const char *vars, size_t varsn, const gql_limits *lim)
{
    gql_plan *p = calloc(1, sizeof *p);
    const ast_def *d, *op = NULL;
    int nops = 0;
    gql_json *vj = NULL;
    uint32_t root, rs;
    const ast_selset *rootset[1];

    if (!p)
        return NULL;
    p->ok = 1;
    arena_init(&p->arena, lim ? lim->max_memory : 0);
    vec_init(&p->sels, sizeof(gql_plan_sel));
    vec_init(&p->fields, sizeof(gql_plan_field));
    vec_init(&p->cases, sizeof(gql_plan_case));
    buf_init(&p->str);
    buf_init(&p->errjson);
    errs_init(&p->errs, 50);
    hm_init(&p->memo);
    p->max_fields = lim ? lim->max_fields : 0;
    p->col.s = s;
    p->col.doc = doc;
    p->col.frags = frags;
    p->col.fragdefs = fragdefs;
    p->col.nfrags = nfrags;
    p->col.arena = &p->arena;
    buf_putc(&p->str, 0);

    for (d = doc->defs; d; d = d->next) {
        if (d->kind != DEF_OPERATION)
            continue;
        if (opn) {
            if (d->name.n == opn && memcmp(d->name.p, opname, opn) == 0)
                op = d;
        } else {
            nops++;
            op = d;
        }
    }
    if (opn && !op) {
        perr(p, NULL, "Unknown operation named \"%.*s\".", (int)opn, opname);
        goto finish;
    }
    if (!opn && nops > 1) {
        perr(p, NULL, "Must provide operation name if query contains multiple operations.");
        goto finish;
    }
    if (!op) {
        perr(p, NULL, "Must provide an operation.");
        goto finish;
    }
    root = op->op == OP_QUERY ? s->query_type : op->op == OP_MUTATION ? s->mutation_type : GQL_NONE;
    if (root == GQL_NONE) {
        perr(p, &op->loc, "The %s operation is not supported by the schema.", gql_op_name(op->op));
        goto finish;
    }
    if (varsn && !(varsn == 4 && memcmp(vars, "null", 4) == 0)) {
        char err[160];
        vj = gql_json_parse(vars, varsn, &p->arena, 128, err, sizeof err);
        if (!vj) {
            perr(p, NULL, "Variables are not valid JSON: %s.", err);
            goto finish;
        }
        if (vj->kind != JS_OBJECT) {
            perr(p, NULL, "Variables must be provided as an object.");
            goto finish;
        }
    }
    if (coerce_variables(p, op, vj))
        goto finish;
    rootset[0] = op->selset;
    rs = build_sel(p, root, rootset, 1);
    if (rs == GQL_NONE && p->ok)
        perr(p, NULL, "Failed to plan the query.");
    p->view.op = op->op == OP_MUTATION ? 1 : 0;
    p->view.root_sel = rs;

finish:
    if (p->str.oom || p->sels.oom || p->fields.oom || p->cases.oom || p->memo.oom)
        if (p->ok)
            perr(p, NULL, "Out of memory while planning the query.");
    p->view.sels = p->sels.p;
    p->view.nsels = p->sels.len;
    p->view.fields = p->fields.p;
    p->view.nfields = p->fields.len;
    p->view.cases = p->cases.p;
    p->view.ncases = p->cases.len;
    p->view.str = p->str.p;
    p->view.nstr = (uint32_t)p->str.len;
    if (!p->ok)
        errs_to_json(&p->errs, &p->errjson);
    return p;
}

GQL_API int gql_plan_ok(const gql_plan *p)
{
    return p && p->ok;
}

GQL_API const char *gql_plan_errors(const gql_plan *p, size_t *len)
{
    if (!p || p->ok) {
        if (len)
            *len = 0;
        return NULL;
    }
    if (len)
        *len = p->errjson.len;
    return buf_cstr((gql_buf *)&p->errjson);
}

GQL_API const gql_plan_view *gql_plan_get_view(const gql_plan *p)
{
    return p && p->ok ? &p->view : NULL;
}

GQL_API void gql_plan_free(gql_plan *p)
{
    if (!p)
        return;
    vec_free(&p->sels);
    vec_free(&p->fields);
    vec_free(&p->cases);
    buf_free(&p->str);
    buf_free(&p->errjson);
    errs_free(&p->errs);
    hm_free(&p->memo);
    arena_free(&p->arena);
    free(p);
}
