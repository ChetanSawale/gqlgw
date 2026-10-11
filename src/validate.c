/* validate.c - validation of executable documents.
 *
 * Implements the rules of the GraphQL spec (section 5) with the messages of
 * graphql-js 17 (suggestions hidden), plus MaxIntrospectionDepth.
 */
#include "validate.h"
#include "parser.h"
#include "values.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const ast_value *var;
    gql_tv loc;
    int loc_has_default;
    uint32_t parent_input;
} var_use;

typedef struct {
    const ast_def *def;
    uint32_t type;          /* composite type condition, GQL_NONE if unknown */
    gql_vec uses;           /* var_use */
    gql_vec spreads;        /* const ast_selection * */
    uint8_t used;
} frag_t;

/* ---- overlapping fields -------------------------------------------------- */

typedef struct {
    uint32_t parent;               /* parent type (GQL_NONE if unknown) */
    const ast_selection *node;
    uint32_t def;                  /* field def (GQL_NONE if unknown) */
} fentry;

typedef struct {
    gql_str name;
    uint32_t first, count;
} fgroup;

typedef struct {
    uint32_t id;
    fentry *entries;
    fgroup *groups;
    uint32_t ngroups;
    gql_str *spreads;
    uint32_t nspreads;
} fieldmap;

typedef struct {
    gql_str name;
    const char *reason;
    gql_loc locs[GQL_ERR_MAXLOCS];
    int nlocs;
} conflict;

typedef struct V V;
struct V {
    gql_vcheck vc;               /* must be first */
    const gql_schema *s;
    const ast_doc *doc;
    gql_errors *errs;
    int introspection;
    gql_arena arena;
    gql_hmap frags;              /* name -> index in fragv of first definition */
    gql_vec fragv;               /* frag_t, one per fragment definition */
    gql_vec *uses;               /* current sink */
    gql_vec *spreads;            /* current sink */
    gql_buf b1, b2;
    /* overlap state */
    gql_hmap fm_cache;           /* selset pointer -> fieldmap index */
    gql_vec fms;                 /* fieldmap * */
    gql_hmap fm_groups;          /* scope = fieldmap id + 1, name -> group */
    gql_hmap cmp_ff;             /* compared (fieldmap, fragment, exclusive) */
    gql_hmap cmp_frag;           /* compared fragment pairs */
    uint32_t comparisons, max_comparisons;
    int too_complex;
    uint32_t intro_steps;
};

static void verr(V *v, const gql_loc *locs, int n, const char *fmt, ...) GQL_PRINTF(4, 5);
static void verr(V *v, const gql_loc *locs, int n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    errs_addv(v->errs, locs, n, fmt, ap);
    va_end(ap);
}

static void vc_error(gql_vcheck *vc, const gql_loc *locs, int nlocs, const char *msg)
{
    V *v = (V *)vc;
    errs_add(v->errs, locs, nlocs, "%s", msg);
}

static void vc_var(gql_vcheck *vc, const ast_value *var, gql_tv loc, int loc_has_default, uint32_t parent_input)
{
    V *v = (V *)vc;
    var_use *u;
    if (!v->uses)
        return;
    u = vec_push(v->uses);
    if (!u)
        return;
    u->var = var;
    u->loc = loc;
    u->loc_has_default = loc_has_default;
    u->parent_input = parent_input;
}

#define TNAME(v, t) GQL_S((v)->s, (v)->s->types[t].name)

static const char *tref_s(V *v, gql_buf *b, uint32_t t)
{
    buf_reset(b);
    gql_print_tref(b, v->s, t);
    return buf_cstr(b);
}

static frag_t *frag_by_name(V *v, gql_str name)
{
    uint32_t i = hm_get(&v->frags, 0, name.p, name.n);
    return i == GQL_NONE ? NULL : &VEC_AT(&v->fragv, frag_t, i);
}

/* ---- directives ----------------------------------------------------------- */

static void check_args_unique(V *v, const ast_arg *args)
{
    const ast_arg *a, *b;
    for (a = args; a; a = a->next) {
        gql_loc locs[GQL_ERR_MAXLOCS];
        int n = 0, earlier = 0;
        for (b = args; b != a; b = b->next)
            if (gql_str_eq(a->name, b->name))
                earlier = 1;
        if (earlier)
            continue;
        locs[n++] = a->loc;
        for (b = a->next; b; b = b->next)
            if (gql_str_eq(a->name, b->name) && n < GQL_ERR_MAXLOCS)
                locs[n++] = b->loc;
        if (n > 1)
            verr(v, locs, n, "There can be only one argument named \"%.*s\".", (int)a->name.n, a->name.p);
    }
}

static void check_directives(V *v, const ast_directive *dirs, uint32_t loc)
{
    const gql_schema *s = v->s;
    const ast_directive *d, *e;
    for (d = dirs; d; d = d->next) {
        uint32_t di = gql_dir_by_name(s, d->name.p, d->name.n), k;
        const gql_dir_rec *dr;
        const ast_arg *a;
        if (di != GQL_NONE && (s->dirs[di].flags & DF_HIDDEN))
            di = GQL_NONE;
        if (di == GQL_NONE) {
            verr(v, &d->loc, 1, "Unknown directive \"@%.*s\".", (int)d->name.n, d->name.p);
            for (a = d->args; a; a = a->next)
                gql_check_literal(&v->vc, tv_none(), a->value, 0, GQL_NONE);
            continue;
        }
        dr = &s->dirs[di];
        if (!(dr->locmask & (1u << loc)))
            verr(v, &d->loc, 1, "Directive \"@%.*s\" may not be used on %s.", (int)d->name.n, d->name.p,
                 gql_dirloc_names[loc]);
        if (!(dr->flags & DF_REPEATABLE)) {
            for (e = dirs; e != d; e = e->next)
                if (gql_str_eq(e->name, d->name)) {
                    gql_loc ls[2];
                    ls[0] = e->loc;
                    ls[1] = d->loc;
                    verr(v, ls, 2, "The directive \"@%.*s\" can only be used once at this location.",
                         (int)d->name.n, d->name.p);
                    break;
                }
        }
        for (a = d->args; a; a = a->next) {
            uint32_t ai = gql_arg_lookup(s, dr->args_first, dr->args_count, a->name.p, a->name.n);
            if (ai == GQL_NONE) {
                verr(v, &a->loc, 1, "Unknown argument \"%.*s\" on directive \"@%.*s\".", (int)a->name.n, a->name.p,
                     (int)d->name.n, d->name.p);
                gql_check_literal(&v->vc, tv_none(), a->value, 0, GQL_NONE);
                continue;
            }
            gql_check_literal(&v->vc, tv_tref(s->ivals[ai].type), a->value,
                              (s->ivals[ai].flags & MF_HAS_DEFAULT) != 0, GQL_NONE);
        }
        check_args_unique(v, d->args);
        for (k = 0; k < dr->args_count; k++) {
            const gql_ival_rec *ar = &s->ivals[dr->args_first + k];
            if (!gql_tref_nonnull(s, ar->type) || (ar->flags & MF_HAS_DEFAULT))
                continue;
            for (a = d->args; a; a = a->next)
                if (a->name.n == ar->name.len && memcmp(a->name.p, GQL_S(s, ar->name), a->name.n) == 0)
                    break;
            if (!a)
                verr(v, &d->loc, 1, "Argument \"@%.*s(%s:)\" of type \"%s\" is required, but it was not provided.",
                     (int)d->name.n, d->name.p, GQL_S(s, ar->name), tref_s(v, &v->b1, ar->type));
        }
    }
}

/* ---- MaxIntrospectionDepth --------------------------------------------- */

#define MAX_LISTS_DEPTH 3

static int intro_depth(V *v, const ast_selset *ss, int depth, const ast_def **stack, int nstack)
{
    const ast_selection *sel;
    if (!ss)
        return 0;
    for (sel = ss->first; sel; sel = sel->next) {
        if (++v->intro_steps > 100000)
            return 1;
        if (sel->kind == SEL_SPREAD) {
            frag_t *f = frag_by_name(v, sel->name);
            int i, seen = 0;
            if (!f)
                continue;
            for (i = 0; i < nstack; i++)
                if (stack[i] == f->def)
                    seen = 1;
            if (seen || nstack >= 64)
                continue;
            stack[nstack] = f->def;
            if (intro_depth(v, f->def->selset, depth, stack, nstack + 1))
                return 1;
        } else if (sel->kind == SEL_FIELD) {
            int d = depth;
            if (gql_str_eqz(sel->name, "fields") || gql_str_eqz(sel->name, "interfaces") ||
                gql_str_eqz(sel->name, "possibleTypes") || gql_str_eqz(sel->name, "inputFields")) {
                if (++d >= MAX_LISTS_DEPTH)
                    return 1;
            }
            if (intro_depth(v, sel->selset, d, stack, nstack))
                return 1;
        } else {
            if (intro_depth(v, sel->selset, depth, stack, nstack))
                return 1;
        }
    }
    return 0;
}

/* ---- OverlappingFieldsCanBeMerged ---------------------------------------- */

static void print_sorted(gql_buf *b, const ast_value *v)
{
    const ast_value *it;
    if (v->kind == AV_LIST) {
        buf_putc(b, '[');
        for (it = v->items; it; it = it->next) {
            print_sorted(b, it);
            if (it->next)
                buf_puts(b, ", ");
        }
        buf_putc(b, ']');
    } else if (v->kind == AV_OBJECT) {
        /* fields sorted by name (selection sort over a small list) */
        const ast_objfield *f, *best, *last = NULL;
        int first = 1;
        buf_putc(b, '{');
        for (;;) {
            best = NULL;
            for (f = v->fields; f; f = f->next) {
                int cmp_last, cmp_best;
                if (last) {
                    cmp_last = memcmp(f->name.p, last->name.p, f->name.n < last->name.n ? f->name.n : last->name.n);
                    if (cmp_last < 0 || (cmp_last == 0 && f->name.n <= last->name.n))
                        continue;
                }
                if (best) {
                    cmp_best = memcmp(f->name.p, best->name.p, f->name.n < best->name.n ? f->name.n : best->name.n);
                    if (cmp_best > 0 || (cmp_best == 0 && f->name.n >= best->name.n))
                        continue;
                }
                best = f;
            }
            if (!best)
                break;
            if (!first)
                buf_puts(b, ", ");
            first = 0;
            buf_put(b, best->name.p, best->name.n);
            buf_puts(b, ": ");
            print_sorted(b, best->value);
            last = best;
        }
        buf_putc(b, '}');
    } else {
        gql_print_value(b, v);
    }
}

static int same_arguments(V *v, const ast_arg *a1, const ast_arg *a2)
{
    const ast_arg *a, *b;
    int n1 = 0, n2 = 0, eq = 1;
    gql_buf p1, p2;
    for (a = a1; a; a = a->next)
        n1++;
    for (b = a2; b; b = b->next)
        n2++;
    if (n1 != n2)
        return 0;
    buf_init(&p1);
    buf_init(&p2);
    for (a = a1; a && eq; a = a->next) {
        for (b = a2; b; b = b->next)
            if (gql_str_eq(a->name, b->name))
                break;
        if (!b) {
            eq = 0;
            break;
        }
        buf_reset(&p1);
        buf_reset(&p2);
        print_sorted(&p1, a->value);
        print_sorted(&p2, b->value);
        eq = p1.len == p2.len && (!p1.len || memcmp(p1.p, p2.p, p1.len) == 0);
    }
    buf_free(&p1);
    buf_free(&p2);
    (void)v;
    return eq;
}

static int types_conflict(const gql_schema *s, uint32_t t1, uint32_t t2)
{
    uint32_t k1 = s->trefs[t1].kind, k2 = s->trefs[t2].kind, n1, n2;
    if (k1 == TR_LIST)
        return k2 == TR_LIST ? types_conflict(s, s->trefs[t1].of, s->trefs[t2].of) : 1;
    if (k2 == TR_LIST)
        return 1;
    if (k1 == TR_NONNULL)
        return k2 == TR_NONNULL ? types_conflict(s, s->trefs[t1].of, s->trefs[t2].of) : 1;
    if (k2 == TR_NONNULL)
        return 1;
    n1 = s->trefs[t1].of;
    n2 = s->trefs[t2].of;
    if (gql_kind_is_leaf(s->types[n1].kind) || gql_kind_is_leaf(s->types[n2].kind))
        return n1 != n2;
    return 0;
}

static void collect_fm(V *v, uint32_t parent, const ast_selset *ss, gql_vec *ents, gql_vec *names, gql_vec *spreads)
{
    const gql_schema *s = v->s;
    const ast_selection *sel;
    for (sel = ss->first; sel; sel = sel->next) {
        if (sel->kind == SEL_FIELD) {
            fentry *e = vec_push(ents);
            gql_str *nm = vec_push(names);
            if (!e || !nm)
                return;
            e->parent = parent;
            e->node = sel;
            e->def = GQL_NONE;
            /* like graphql-js: getFields() only, meta fields have no definition here */
            if (parent != GQL_NONE && (s->types[parent].kind == GK_OBJECT || s->types[parent].kind == GK_INTERFACE))
                e->def = gql_member(s, parent, sel->name.p, sel->name.n);
            *nm = sel->alias.n ? sel->alias : sel->name;
        } else if (sel->kind == SEL_SPREAD) {
            uint32_t k;
            for (k = 0; k < spreads->len; k++)
                if (gql_str_eq(VEC_AT(spreads, gql_str, k), sel->name))
                    break;
            if (k == spreads->len) {
                gql_str *sp = vec_push(spreads);
                if (sp)
                    *sp = sel->name;
            }
        } else {
            uint32_t tc = parent;
            if (sel->name.n) {
                tc = gql_type_by_name(s, sel->name.p, sel->name.n);
            }
            collect_fm(v, tc, sel->selset, ents, names, spreads);
        }
    }
}

static fieldmap *get_fieldmap(V *v, uint32_t parent, const ast_selset *ss)
{
    uint32_t i = hm_get(&v->fm_cache, 0, (const char *)&ss, sizeof ss), k, ng = 0;
    gql_vec ents, names, spreads;
    fieldmap *fm;
    uint32_t *gidx, *pos;
    const ast_selset **key;

    if (i != GQL_NONE)
        return VEC_AT(&v->fms, fieldmap *, i);
    vec_init(&ents, sizeof(fentry));
    vec_init(&names, sizeof(gql_str));
    vec_init(&spreads, sizeof(gql_str));
    collect_fm(v, parent, ss, &ents, &names, &spreads);

    fm = arena_alloc(&v->arena, sizeof *fm);
    key = arena_alloc(&v->arena, sizeof *key);
    gidx = malloc((ents.len + 1) * sizeof *gidx);
    if (!fm || !key || !gidx) {
        free(gidx);
        vec_free(&ents);
        vec_free(&names);
        vec_free(&spreads);
        return NULL;
    }
    fm->id = v->fms.len;
    /* group by response name, preserving first-appearance order */
    fm->groups = arena_alloc(&v->arena, (ents.len + 1) * sizeof(fgroup));
    fm->entries = arena_alloc(&v->arena, (ents.len + 1) * sizeof(fentry));
    for (k = 0; k < ents.len; k++) {
        gql_str nm = VEC_AT(&names, gql_str, k);
        uint32_t g = hm_get(&v->fm_groups, fm->id + 1, nm.p, nm.n);
        if (g == GQL_NONE) {
            g = ng++;
            hm_put(&v->fm_groups, fm->id + 1, nm.p, nm.n, g);
            fm->groups[g].name = nm;
            fm->groups[g].count = 0;
        }
        fm->groups[g].count++;
        gidx[k] = g;
    }
    fm->ngroups = ng;
    pos = calloc(ng + 1, sizeof *pos);
    if (pos && fm->groups && fm->entries) {
        uint32_t acc = 0;
        for (k = 0; k < ng; k++) {
            fm->groups[k].first = acc;
            pos[k] = acc;
            acc += fm->groups[k].count;
        }
        for (k = 0; k < ents.len; k++)
            fm->entries[pos[gidx[k]]++] = VEC_AT(&ents, fentry, k);
    } else {
        fm->ngroups = 0;
    }
    free(pos);
    free(gidx);
    fm->nspreads = spreads.len;
    fm->spreads = arena_alloc(&v->arena, (spreads.len + 1) * sizeof(gql_str));
    if (fm->spreads && spreads.len)
        memcpy(fm->spreads, spreads.p, spreads.len * sizeof(gql_str));
    else if (!fm->spreads)
        fm->nspreads = 0;
    vec_free(&ents);
    vec_free(&names);
    vec_free(&spreads);
    *key = ss;
    {
        fieldmap **slot = vec_push(&v->fms);
        if (slot)
            *slot = fm;
    }
    hm_put(&v->fm_cache, 0, (const char *)key, sizeof *key, fm->id);
    return fm;
}

static fieldmap *frag_fieldmap(V *v, gql_str name)
{
    frag_t *f = frag_by_name(v, name);
    if (!f)
        return NULL;
    return get_fieldmap(v, f->type, f->def->selset);
}

static void find_between_subsel(V *v, int excl, uint32_t p1, const ast_selset *ss1, uint32_t p2,
                                const ast_selset *ss2, gql_vec *out);

static int find_conflict(V *v, int parent_excl, gql_str name, const fentry *f1, const fentry *f2, conflict *out)
{
    const gql_schema *s = v->s;
    const ast_selection *n1 = f1->node, *n2 = f2->node;
    int excl;
    uint32_t t1, t2;
    gql_buf b;

    if (++v->comparisons > v->max_comparisons) {
        v->too_complex = 1;
        return 0;
    }
    excl = parent_excl || (f1->parent != f2->parent && f1->parent != GQL_NONE && f2->parent != GQL_NONE &&
                           s->types[f1->parent].kind == GK_OBJECT && s->types[f2->parent].kind == GK_OBJECT);
    out->name = name;
    out->nlocs = 2;
    out->locs[0] = n1->loc;
    out->locs[1] = n2->loc;
    buf_init(&b);
    if (!excl) {
        if (!gql_str_eq(n1->name, n2->name)) {
            buf_printf(&b, "\"%.*s\" and \"%.*s\" are different fields", (int)n1->name.n, n1->name.p,
                       (int)n2->name.n, n2->name.p);
            goto conflict;
        }
        if (!same_arguments(v, n1->args, n2->args)) {
            buf_puts(&b, "they have differing arguments");
            goto conflict;
        }
    }
    t1 = f1->def != GQL_NONE ? s->fields[f1->def].type : GQL_NONE;
    t2 = f2->def != GQL_NONE ? s->fields[f2->def].type : GQL_NONE;
    if (t1 != GQL_NONE && t2 != GQL_NONE && types_conflict(s, t1, t2)) {
        buf_puts(&b, "they return conflicting types \"");
        gql_print_tref(&b, s, t1);
        buf_puts(&b, "\" and \"");
        gql_print_tref(&b, s, t2);
        buf_putc(&b, '"');
        goto conflict;
    }
    if (n1->selset && n2->selset) {
        gql_vec sub;
        uint32_t k;
        vec_init(&sub, sizeof(conflict));
        find_between_subsel(v, excl, t1 != GQL_NONE ? gql_tref_named(s, t1) : GQL_NONE, n1->selset,
                            t2 != GQL_NONE ? gql_tref_named(s, t2) : GQL_NONE, n2->selset, &sub);
        if (sub.len) {
            for (k = 0; k < sub.len; k++) {
                conflict *c = &VEC_AT(&sub, conflict, k);
                if (k)
                    buf_puts(&b, " and ");
                buf_printf(&b, "subfields \"%.*s\" conflict because %s", (int)c->name.n, c->name.p, c->reason);
                if (out->nlocs + 2 <= GQL_ERR_MAXLOCS && k == 0) {
                    out->locs[out->nlocs++] = c->locs[0];
                    out->locs[out->nlocs++] = c->locs[1];
                }
            }
            /* graphql-js orders nodes as [node1, ...sub1, node2, ...sub2] */
            if (out->nlocs == 4) {
                gql_loc tmp = out->locs[1];
                out->locs[1] = out->locs[2];
                out->locs[2] = tmp;
            }
            vec_free(&sub);
            goto conflict;
        }
        vec_free(&sub);
    }
    buf_free(&b);
    return 0;
conflict:
    out->reason = arena_strndup(&v->arena, b.p ? b.p : "", b.len);
    buf_free(&b);
    return out->reason != NULL;
}

static void push_conflict(gql_vec *out, const conflict *c)
{
    conflict *slot = vec_push(out);
    if (slot)
        *slot = *c;
}

static void collect_within(V *v, fieldmap *fm, gql_vec *out)
{
    uint32_t g, i, j;
    for (g = 0; g < fm->ngroups && !v->too_complex; g++) {
        fgroup *gr = &fm->groups[g];
        for (i = 0; i < gr->count; i++)
            for (j = i + 1; j < gr->count; j++) {
                conflict c;
                if (find_conflict(v, 0, gr->name, &fm->entries[gr->first + i], &fm->entries[gr->first + j], &c))
                    push_conflict(out, &c);
            }
    }
}

static void collect_between(V *v, int excl, fieldmap *fm1, fieldmap *fm2, gql_vec *out)
{
    uint32_t g, i, j;
    for (g = 0; g < fm1->ngroups && !v->too_complex; g++) {
        fgroup *g1 = &fm1->groups[g], *g2;
        uint32_t k = hm_get(&v->fm_groups, fm2->id + 1, g1->name.p, g1->name.n);
        if (k == GQL_NONE)
            continue;
        g2 = &fm2->groups[k];
        for (i = 0; i < g1->count; i++)
            for (j = 0; j < g2->count; j++) {
                conflict c;
                if (find_conflict(v, excl, g1->name, &fm1->entries[g1->first + i], &fm2->entries[g2->first + j], &c))
                    push_conflict(out, &c);
            }
    }
}

/* sets keyed by strings built in the arena */
static int pairset_check_add(V *v, gql_hmap *m, uint32_t scope, const char *a, uint32_t an, const char *b,
                             uint32_t bn, int excl)
{
    /* key = a \0 b ; "exclusive" presence (scope*2+1) is implied by non-exclusive presence (scope*2) */
    char *key;
    uint32_t n = an + 1 + bn;
    key = arena_alloc(&v->arena, n);
    if (!key)
        return 1;
    memcpy(key, a, an);
    key[an] = 0;
    memcpy(key + an + 1, b, bn);
    if (hm_get(m, scope * 2, key, n) != GQL_NONE)
        return 1;
    if (excl && hm_get(m, scope * 2 + 1, key, n) != GQL_NONE)
        return 1;
    hm_put(m, scope * 2 + (excl ? 1 : 0), key, n, 1);
    return 0;
}

static void collect_fields_and_fragment(V *v, int excl, fieldmap *fm, gql_str frag, gql_vec *out)
{
    fieldmap *fm2;
    uint32_t i;
    if (v->too_complex || pairset_check_add(v, &v->cmp_ff, fm->id, frag.p, frag.n, "", 0, excl))
        return;
    fm2 = frag_fieldmap(v, frag);
    if (!fm2 || fm2 == fm)
        return;
    collect_between(v, excl, fm, fm2, out);
    for (i = 0; i < fm2->nspreads; i++)
        collect_fields_and_fragment(v, excl, fm, fm2->spreads[i], out);
}

static void collect_between_fragments(V *v, int excl, gql_str f1, gql_str f2, gql_vec *out)
{
    fieldmap *fm1, *fm2;
    uint32_t i;
    gql_str a = f1, b = f2;
    if (v->too_complex || gql_str_eq(f1, f2))
        return;
    /* unordered pair */
    if (memcmp(a.p, b.p, a.n < b.n ? a.n : b.n) > 0 || (memcmp(a.p, b.p, a.n < b.n ? a.n : b.n) == 0 && a.n > b.n)) {
        gql_str t = a;
        a = b;
        b = t;
    }
    if (pairset_check_add(v, &v->cmp_frag, 0, a.p, a.n, b.p, b.n, excl))
        return;
    fm1 = frag_fieldmap(v, f1);
    fm2 = frag_fieldmap(v, f2);
    if (!fm1 || !fm2)
        return;
    collect_between(v, excl, fm1, fm2, out);
    for (i = 0; i < fm2->nspreads; i++)
        collect_between_fragments(v, excl, f1, fm2->spreads[i], out);
    for (i = 0; i < fm1->nspreads; i++)
        collect_between_fragments(v, excl, fm1->spreads[i], f2, out);
}

static void find_between_subsel(V *v, int excl, uint32_t p1, const ast_selset *ss1, uint32_t p2,
                                const ast_selset *ss2, gql_vec *out)
{
    fieldmap *fm1 = get_fieldmap(v, p1, ss1), *fm2 = get_fieldmap(v, p2, ss2);
    uint32_t i, j;
    if (!fm1 || !fm2)
        return;
    collect_between(v, excl, fm1, fm2, out);
    for (i = 0; i < fm2->nspreads; i++)
        collect_fields_and_fragment(v, excl, fm1, fm2->spreads[i], out);
    for (i = 0; i < fm1->nspreads; i++)
        collect_fields_and_fragment(v, excl, fm2, fm1->spreads[i], out);
    for (i = 0; i < fm1->nspreads; i++)
        for (j = 0; j < fm2->nspreads; j++)
            collect_between_fragments(v, excl, fm1->spreads[i], fm2->spreads[j], out);
}

static void check_overlap(V *v, uint32_t parent, const ast_selset *ss)
{
    fieldmap *fm;
    gql_vec out;
    uint32_t i, j;
    if (v->too_complex)
        return;
    fm = get_fieldmap(v, parent, ss);
    if (!fm)
        return;
    vec_init(&out, sizeof(conflict));
    collect_within(v, fm, &out);
    for (i = 0; i < fm->nspreads; i++) {
        collect_fields_and_fragment(v, 0, fm, fm->spreads[i], &out);
        for (j = i + 1; j < fm->nspreads; j++)
            collect_between_fragments(v, 0, fm->spreads[i], fm->spreads[j], &out);
    }
    for (i = 0; i < out.len; i++) {
        conflict *c = &VEC_AT(&out, conflict, i);
        verr(v, c->locs, c->nlocs,
             "Fields \"%.*s\" conflict because %s. Use different aliases on the fields to fetch both if this was "
             "intentional.", (int)c->name.n, c->name.p, c->reason);
    }
    vec_free(&out);
}

/* ---- selections ------------------------------------------------------------ */

static void walk_selset(V *v, uint32_t parent, const ast_selset *ss);

static void walk_field(V *v, uint32_t parent, const ast_selection *sel)
{
    const gql_schema *s = v->s;
    uint32_t fdef = GQL_NONE, child = GQL_NONE, k;
    const ast_arg *a;

    if (parent != GQL_NONE) {
        fdef = gql_field_lookup(s, parent, sel->name.p, sel->name.n);
        if (fdef == GQL_NONE)
            verr(v, &sel->loc, 1, "Cannot query field \"%.*s\" on type \"%s\".", (int)sel->name.n, sel->name.p,
                 TNAME(v, parent));
    }
    if (fdef != GQL_NONE && (fdef == s->f_schema || fdef == s->f_type) && !v->introspection)
        verr(v, &sel->loc, 1, "GraphQL introspection has been disabled, but the requested query contained the "
             "field \"%.*s\".", (int)sel->name.n, sel->name.p);
    if (gql_str_eqz(sel->name, "__schema") || gql_str_eqz(sel->name, "__type")) {
        const ast_def *stack[64];
        v->intro_steps = 0;
        if (intro_depth(v, sel->selset, 0, stack, 0))
            verr(v, &sel->loc, 1, "Maximum introspection depth exceeded");
    }
    for (a = sel->args; a; a = a->next) {
        uint32_t ai = GQL_NONE;
        if (fdef != GQL_NONE) {
            const gql_field_rec *f = &s->fields[fdef];
            ai = gql_arg_lookup(s, f->args_first, f->args_count, a->name.p, a->name.n);
            if (ai == GQL_NONE) {
                if (f->owner != GQL_NONE)
                    verr(v, &a->loc, 1, "Unknown argument \"%.*s\" on field \"%s.%s\".", (int)a->name.n, a->name.p,
                         TNAME(v, f->owner), GQL_S(s, f->name));
                else
                    verr(v, &a->loc, 1, "Unknown argument \"%.*s\" on field \"<meta>.%s\".", (int)a->name.n,
                         a->name.p, GQL_S(s, f->name));
            }
        }
        gql_check_literal(&v->vc, ai != GQL_NONE ? tv_tref(s->ivals[ai].type) : tv_none(), a->value,
                          ai != GQL_NONE && (s->ivals[ai].flags & MF_HAS_DEFAULT), GQL_NONE);
    }
    check_args_unique(v, sel->args);
    if (fdef != GQL_NONE) {
        const gql_field_rec *f = &s->fields[fdef];
        uint32_t nt = gql_tref_named(s, f->type);
        for (k = 0; k < f->args_count; k++) {
            const gql_ival_rec *ar = &s->ivals[f->args_first + k];
            if (!gql_tref_nonnull(s, ar->type) || (ar->flags & MF_HAS_DEFAULT))
                continue;
            for (a = sel->args; a; a = a->next)
                if (a->name.n == ar->name.len && memcmp(a->name.p, GQL_S(s, ar->name), a->name.n) == 0)
                    break;
            if (!a)
                verr(v, &sel->loc, 1, "Argument \"%s.%s(%s:)\" of type \"%s\" is required, but it was not provided.",
                     f->owner != GQL_NONE ? TNAME(v, f->owner) : TNAME(v, parent), GQL_S(s, f->name),
                     GQL_S(s, ar->name), tref_s(v, &v->b1, ar->type));
        }
        if (gql_kind_is_leaf(s->types[nt].kind)) {
            if (sel->selset)
                verr(v, &sel->selset->loc, 1, "Field \"%.*s\" must not have a selection since type \"%s\" has no "
                     "subfields.", (int)sel->name.n, sel->name.p, tref_s(v, &v->b1, f->type));
        } else {
            child = nt;
            if (!sel->selset)
                verr(v, &sel->loc, 1, "Field \"%.*s\" of type \"%s\" must have a selection of subfields. Did you "
                     "mean \"%.*s { ... }\"?", (int)sel->name.n, sel->name.p, tref_s(v, &v->b1, f->type),
                     (int)sel->name.n, sel->name.p);
        }
    }
    check_directives(v, sel->dirs, DL_FIELD);
    if (sel->selset)
        walk_selset(v, child, sel->selset);
}

static void walk_selset(V *v, uint32_t parent, const ast_selset *ss)
{
    const gql_schema *s = v->s;
    const ast_selection *sel;
    if (errs_full(v->errs))
        return;
    check_overlap(v, parent, ss);
    for (sel = ss->first; sel; sel = sel->next) {
        if (sel->kind == SEL_FIELD) {
            walk_field(v, parent, sel);
        } else if (sel->kind == SEL_INLINE) {
            uint32_t tc = parent;
            if (sel->name.n) {
                tc = gql_type_by_name(s, sel->name.p, sel->name.n);
                if (tc == GQL_NONE) {
                    verr(v, &sel->name_loc, 1, "Unknown type \"%.*s\".", (int)sel->name.n, sel->name.p);
                } else if (!gql_kind_is_composite(s->types[tc].kind)) {
                    verr(v, &sel->name_loc, 1, "Fragment cannot condition on non composite type \"%.*s\".",
                         (int)sel->name.n, sel->name.p);
                    tc = GQL_NONE;
                } else if (parent != GQL_NONE && gql_kind_is_composite(s->types[parent].kind) &&
                           !gql_types_overlap(s, parent, tc)) {
                    verr(v, &sel->loc, 1, "Fragment cannot be spread here as objects of type \"%s\" can never be "
                         "of type \"%s\".", TNAME(v, parent), TNAME(v, tc));
                }
            }
            check_directives(v, sel->dirs, DL_INLINE_FRAGMENT);
            walk_selset(v, tc, sel->selset);
        } else {
            frag_t *f = frag_by_name(v, sel->name);
            if (!f) {
                verr(v, &sel->name_loc, 1, "Unknown fragment \"%.*s\".", (int)sel->name.n, sel->name.p);
            } else {
                const ast_selection **slot;
                if (v->spreads && (slot = vec_push(v->spreads)))
                    *slot = sel;
                if (parent != GQL_NONE && f->type != GQL_NONE && gql_kind_is_composite(s->types[parent].kind) &&
                    !gql_types_overlap(s, parent, f->type))
                    verr(v, &sel->loc, 1, "Fragment \"%.*s\" cannot be spread here as objects of type \"%s\" can "
                         "never be of type \"%s\".", (int)sel->name.n, sel->name.p, TNAME(v, parent),
                         TNAME(v, f->type));
            }
            check_directives(v, sel->dirs, DL_FRAGMENT_SPREAD);
        }
    }
}

/* ---- fragments ------------------------------------------------------------- */

/* graphql-js ValidationContext.getFragmentSpreads: LIFO traversal of nested sets */
static void collect_spreads_rec(const ast_selset *ss, gql_vec *out)
{
    gql_vec stack;
    const ast_selection *sel;
    vec_init(&stack, sizeof(const ast_selset *));
    if (ss) {
        const ast_selset **slot = vec_push(&stack);
        if (slot)
            *slot = ss;
    }
    while (stack.len) {
        const ast_selset *set = VEC_AT(&stack, const ast_selset *, stack.len - 1);
        stack.len--;
        for (sel = set->first; sel; sel = sel->next) {
            if (sel->kind == SEL_SPREAD) {
                const ast_selection **slot = vec_push(out);
                if (slot)
                    *slot = sel;
            } else if (sel->selset) {
                const ast_selset **slot = vec_push(&stack);
                if (slot)
                    *slot = sel->selset;
            }
        }
    }
    vec_free(&stack);
}

static void detect_cycles(V *v, frag_t *f, uint8_t *visited, uint32_t *pathidx, gql_vec *path)
{
    uint32_t fi = (uint32_t)(f - (frag_t *)v->fragv.p), k;
    gql_vec spreads;
    if (visited[fi])
        return;
    visited[fi] = 1;
    vec_init(&spreads, sizeof(const ast_selection *));
    collect_spreads_rec(f->def->selset, &spreads);
    if (!spreads.len) {
        vec_free(&spreads);
        return;
    }
    pathidx[fi] = path->len;
    for (k = 0; k < spreads.len; k++) {
        const ast_selection *sp = VEC_AT(&spreads, const ast_selection *, k);
        frag_t *target = frag_by_name(v, sp->name);
        uint32_t ti = target ? (uint32_t)(target - (frag_t *)v->fragv.p) : GQL_NONE;
        uint32_t cyc = ti != GQL_NONE ? pathidx[ti] : GQL_NONE;
        const ast_selection **slot = vec_push(path);
        if (slot)
            *slot = sp;
        if (cyc == GQL_NONE) {
            if (target)
                detect_cycles(v, target, visited, pathidx, path);
        } else {
            gql_buf via;
            gql_loc locs[GQL_ERR_MAXLOCS];
            int n = 0;
            uint32_t m;
            buf_init(&via);
            for (m = cyc; m < path->len; m++) {
                const ast_selection *ps = VEC_AT(path, const ast_selection *, m);
                if (n < GQL_ERR_MAXLOCS)
                    locs[n++] = ps->loc;
                if (m + 1 < path->len) {
                    if (via.len)
                        buf_puts(&via, ", ");
                    buf_printf(&via, "\"%.*s\"", (int)ps->name.n, ps->name.p);
                }
            }
            if (via.len)
                verr(v, locs, n, "Cannot spread fragment \"%.*s\" within itself via %s.", (int)sp->name.n,
                     sp->name.p, buf_cstr(&via));
            else
                verr(v, locs, n, "Cannot spread fragment \"%.*s\" within itself.", (int)sp->name.n, sp->name.p);
            buf_free(&via);
        }
        path->len--;
    }
    pathidx[fi] = GQL_NONE;
    vec_free(&spreads);
}

/* recursively referenced fragments of an operation (sets f->used, appends uses) */
static void reach(V *v, const gql_vec *spreads, uint8_t *seen, gql_vec *uses_out)
{
    uint32_t k, m;
    for (k = 0; k < spreads->len; k++) {
        const ast_selection *sp = VEC_AT(spreads, const ast_selection *, k);
        uint32_t fi = hm_get(&v->frags, 0, sp->name.p, sp->name.n);
        frag_t *f;
        if (fi == GQL_NONE || seen[fi])
            continue;
        seen[fi] = 1;
        f = &VEC_AT(&v->fragv, frag_t, fi);
        f->used = 1;
        for (m = 0; m < f->uses.len; m++) {
            var_use *u = vec_push(uses_out);
            if (u)
                *u = VEC_AT(&f->uses, var_use, m);
        }
        reach(v, &f->spreads, seen, uses_out);
    }
}

/* ---- operations -------------------------------------------------------------- */

static int allowed_usage(V *v, gql_tv vt, const ast_value *var_default, gql_tv loc, int loc_has_default)
{
    const gql_schema *s = v->s;
    if (tv_kind(s, loc) == TR_NONNULL && tv_kind(s, vt) != TR_NONNULL) {
        int has_nn_default = var_default && var_default->kind != AV_NULL;
        if (!has_nn_default && !loc_has_default)
            return 0;
        return tv_is_subtype(s, vt, tv_of(s, loc));
    }
    return tv_is_subtype(s, vt, loc);
}

static void check_operation(V *v, const ast_def *op, uint8_t *seen)
{
    const gql_schema *s = v->s;
    uint32_t root = GQL_NONE, k;
    const ast_vardef *vd, *w;
    gql_vec uses, spreads, all;
    gql_buf b;

    switch (op->op) {
    case OP_QUERY: root = s->query_type; break;
    case OP_MUTATION: root = s->mutation_type; break;
    case OP_SUBSCRIPTION: root = s->subscription_type; break;
    }
    if (root == GQL_NONE)
        verr(v, &op->loc, 1, "The %s operation is not supported by the schema.", gql_op_name(op->op));

    vec_init(&uses, sizeof(var_use));
    vec_init(&spreads, sizeof(const ast_selection *));
    vec_init(&all, sizeof(var_use));
    buf_init(&b);
    v->uses = &uses;
    v->spreads = &spreads;

    for (vd = op->vars; vd; vd = vd->next) {
        uint32_t nt;
        const ast_type *t;
        int earlier = 0, n = 0;
        gql_loc ls[GQL_ERR_MAXLOCS];
        for (w = op->vars; w != vd; w = w->next)
            if (gql_str_eq(w->name, vd->name))
                earlier = 1;
        if (!earlier) {
            for (w = vd; w; w = w->next)
                if (gql_str_eq(w->name, vd->name) && n < GQL_ERR_MAXLOCS)
                    ls[n++] = w->name_loc;
            if (n > 1)
                verr(v, ls, n, "There can be only one variable named \"$%.*s\".", (int)vd->name.n, vd->name.p);
        }
        for (t = vd->type; t->kind != AT_NAMED; t = t->of)
            ;
        nt = gql_type_by_name(s, t->name.p, t->name.n);
        if (nt == GQL_NONE) {
            verr(v, &t->loc, 1, "Unknown type \"%.*s\".", (int)t->name.n, t->name.p);
        } else if (!gql_kind_is_input(s->types[nt].kind)) {
            buf_reset(&b);
            gql_print_type(&b, vd->type);
            verr(v, &vd->type->loc, 1, "Variable \"$%.*s\" cannot be non-input type \"%s\".", (int)vd->name.n,
                 vd->name.p, buf_cstr(&b));
        } else if (vd->defval) {
            v->uses = NULL;   /* defaults are constant */
            gql_check_literal(&v->vc, tv_ast(vd->type), vd->defval, 0, GQL_NONE);
            v->uses = &uses;
        }
        check_directives(v, vd->dirs, DL_VARIABLE_DEFINITION);
    }
    check_directives(v, op->dirs, op->op == OP_QUERY ? DL_QUERY : op->op == OP_MUTATION ? DL_MUTATION
                                                                                         : DL_SUBSCRIPTION);
    walk_selset(v, root, op->selset);
    v->uses = NULL;
    v->spreads = NULL;

    /* recursive variable usages */
    for (k = 0; k < uses.len; k++) {
        var_use *u = vec_push(&all);
        if (u)
            *u = VEC_AT(&uses, var_use, k);
    }
    memset(seen, 0, v->fragv.len ? v->fragv.len : 1);
    reach(v, &spreads, seen, &all);

    for (k = 0; k < all.len; k++) {
        var_use *u = &VEC_AT(&all, var_use, k);
        const ast_vardef *def = NULL;
        for (vd = op->vars; vd; vd = vd->next)
            if (gql_str_eq(vd->name, u->var->s))
                def = vd;   /* graphql-js: the last definition wins */
        if (!def) {
            gql_loc ls[2];
            ls[0] = u->var->loc;
            ls[1] = op->loc;
            if (op->name.n)
                verr(v, ls, 2, "Variable \"$%.*s\" is not defined by operation \"%.*s\".", (int)u->var->s.n,
                     u->var->s.p, (int)op->name.n, op->name.p);
            else
                verr(v, ls, 2, "Variable \"$%.*s\" is not defined.", (int)u->var->s.n, u->var->s.p);
            continue;
        }
        if (tv_valid(u->loc)) {
            const ast_type *t;
            gql_tv vt = tv_ast(def->type);
            for (t = def->type; t->kind != AT_NAMED; t = t->of)
                ;
            if (gql_type_by_name(s, t->name.p, t->name.n) == GQL_NONE)
                continue;
            if (!allowed_usage(v, vt, def->defval, u->loc, u->loc_has_default)) {
                gql_loc ls[2];
                gql_buf tb;
                ls[0] = def->loc;
                ls[1] = u->var->loc;
                buf_reset(&b);
                gql_print_type(&b, def->type);
                buf_init(&tb);
                tv_print(&tb, s, u->loc);
                verr(v, ls, 2, "Variable \"$%.*s\" of type \"%s\" used in position expecting type \"%s\".",
                     (int)u->var->s.n, u->var->s.p, buf_cstr(&b), buf_cstr(&tb));
                buf_free(&tb);
            }
            if (u->parent_input != GQL_NONE && (s->types[u->parent_input].flags & TF_ONEOF) &&
                def->type->kind != AT_NONNULL) {
                gql_loc ls[2];
                ls[0] = def->loc;
                ls[1] = u->var->loc;
                buf_reset(&b);
                gql_print_type(&b, def->type);
                verr(v, ls, 2, "Variable \"$%.*s\" is of type \"%s\" but must be non-nullable to be used for "
                     "OneOf Input Object \"%s\".", (int)u->var->s.n, u->var->s.p, buf_cstr(&b),
                     TNAME(v, u->parent_input));
            }
        }
    }
    for (vd = op->vars; vd; vd = vd->next) {
        int used = 0;
        for (k = 0; k < all.len && !used; k++)
            if (gql_str_eq(VEC_AT(&all, var_use, k).var->s, vd->name))
                used = 1;
        if (!used) {
            if (op->name.n)
                verr(v, &vd->loc, 1, "Variable \"$%.*s\" is never used in operation \"%.*s\".", (int)vd->name.n,
                     vd->name.p, (int)op->name.n, op->name.p);
            else
                verr(v, &vd->loc, 1, "Variable \"$%.*s\" is never used.", (int)vd->name.n, vd->name.p);
        }
    }
    vec_free(&uses);
    vec_free(&spreads);
    vec_free(&all);
    buf_free(&b);
}

int gql_validate(const gql_schema *s, const ast_doc *doc, const gql_validate_opts *o, gql_errors *errs)
{
    V vv, *v = &vv;
    const ast_def *d, *e;
    int before = errs_count(errs), nops = 0;
    uint32_t k;
    uint8_t *seen = NULL, *visited = NULL;
    uint32_t *pathidx = NULL;

    memset(v, 0, sizeof *v);
    v->vc.s = s;
    v->vc.on_error = vc_error;
    v->vc.on_var = vc_var;
    v->s = s;
    v->doc = doc;
    v->errs = errs;
    v->introspection = o ? o->introspection : 1;
    v->max_comparisons = (o && o->max_comparisons) ? o->max_comparisons : 200000;
    arena_init(&v->arena, 0);
    hm_init(&v->frags);
    vec_init(&v->fragv, sizeof(frag_t));
    buf_init(&v->b1);
    buf_init(&v->b2);
    hm_init(&v->fm_cache);
    vec_init(&v->fms, sizeof(fieldmap *));
    hm_init(&v->fm_groups);
    hm_init(&v->cmp_ff);
    hm_init(&v->cmp_frag);

    /* ExecutableDefinitions, fragment table, operation names */
    for (d = doc->defs; d; d = d->next) {
        if (d->kind == DEF_OPERATION) {
            nops++;
        } else if (d->kind == DEF_FRAGMENT) {
            frag_t *f = vec_push(&v->fragv);
            if (!f)
                break;
            vec_init(&f->uses, sizeof(var_use));
            vec_init(&f->spreads, sizeof(const ast_selection *));
            f->def = d;
            f->type = GQL_NONE;
            if (hm_put(&v->frags, 0, d->name.p, d->name.n, v->fragv.len - 1) == 1) {
                gql_loc ls[2];
                for (e = doc->defs; e != d; e = e->next)
                    if (e->kind == DEF_FRAGMENT && gql_str_eq(e->name, d->name))
                        break;
                ls[0] = e->name_loc;
                ls[1] = d->name_loc;
                verr(v, ls, 2, "There can be only one fragment named \"%.*s\".", (int)d->name.n, d->name.p);
            }
        } else {
            gql_str nm = d->name;
            if (d->kind == DEF_SCHEMA) {
                nm.p = "schema";
                nm.n = 6;
            }
            verr(v, &d->loc, 1, "The \"%.*s\" definition is not executable.", (int)nm.n, nm.p);
        }
    }
    for (d = doc->defs; d; d = d->next) {
        if (d->kind != DEF_OPERATION)
            continue;
        if (!d->name.n && nops > 1)
            verr(v, &d->loc, 1, "This anonymous operation must be the only defined operation.");
        if (d->name.n)
            for (e = doc->defs; e != d; e = e->next)
                if (e->kind == DEF_OPERATION && gql_str_eq(e->name, d->name)) {
                    gql_loc ls[2];
                    ls[0] = e->name_loc;
                    ls[1] = d->name_loc;
                    verr(v, ls, 2, "There can be only one operation named \"%.*s\".", (int)d->name.n, d->name.p);
                    break;
                }
    }

    /* fragment definitions */
    for (k = 0; k < v->fragv.len; k++) {
        frag_t *f = &VEC_AT(&v->fragv, frag_t, k);
        uint32_t tc = gql_type_by_name(s, f->def->type_cond.p, f->def->type_cond.n);
        if (tc == GQL_NONE)
            verr(v, &f->def->type_cond_loc, 1, "Unknown type \"%.*s\".", (int)f->def->type_cond.n,
                 f->def->type_cond.p);
        else if (!gql_kind_is_composite(s->types[tc].kind))
            verr(v, &f->def->type_cond_loc, 1, "Fragment \"%.*s\" cannot condition on non composite type \"%.*s\".",
                 (int)f->def->name.n, f->def->name.p, (int)f->def->type_cond.n, f->def->type_cond.p);
        else
            f->type = tc;
    }
    for (k = 0; k < v->fragv.len; k++) {
        frag_t *f = &VEC_AT(&v->fragv, frag_t, k);
        v->uses = &f->uses;
        v->spreads = &f->spreads;
        check_directives(v, f->def->dirs, DL_FRAGMENT_DEFINITION);
        walk_selset(v, f->type, f->def->selset);
        v->uses = NULL;
        v->spreads = NULL;
    }

    /* NoFragmentCycles */
    if (v->fragv.len) {
        gql_vec path;
        visited = calloc(v->fragv.len, 1);
        pathidx = malloc(v->fragv.len * sizeof *pathidx);
        vec_init(&path, sizeof(const ast_selection *));
        if (visited && pathidx) {
            for (k = 0; k < v->fragv.len; k++)
                pathidx[k] = GQL_NONE;
            for (k = 0; k < v->fragv.len; k++)
                detect_cycles(v, &VEC_AT(&v->fragv, frag_t, k), visited, pathidx, &path);
        }
        vec_free(&path);
    }

    /* operations */
    seen = calloc(v->fragv.len ? v->fragv.len : 1, 1);
    if (seen)
        for (d = doc->defs; d && !errs_full(errs); d = d->next)
            if (d->kind == DEF_OPERATION)
                check_operation(v, d, seen);

    /* NoUnusedFragments */
    for (k = 0; k < v->fragv.len; k++) {
        frag_t *f = &VEC_AT(&v->fragv, frag_t, k);
        if (!f->used && hm_get(&v->frags, 0, f->def->name.p, f->def->name.n) == k)
            verr(v, &f->def->loc, 1, "Fragment \"%.*s\" is never used.", (int)f->def->name.n, f->def->name.p);
    }
    if (v->too_complex)
        verr(v, NULL, 0, "Query is too complex to validate.");
    if (errs->truncated) {
        uint32_t max = errs->max;
        errs->max = 0;
        verr(v, NULL, 0, "Too many validation errors, error limit reached. Validation aborted.");
        errs->max = max;
    }

    free(seen);
    free(visited);
    free(pathidx);
    for (k = 0; k < v->fragv.len; k++) {
        vec_free(&VEC_AT(&v->fragv, frag_t, k).uses);
        vec_free(&VEC_AT(&v->fragv, frag_t, k).spreads);
    }
    vec_free(&v->fragv);
    hm_free(&v->frags);
    buf_free(&v->b1);
    buf_free(&v->b2);
    hm_free(&v->fm_cache);
    vec_free(&v->fms);
    hm_free(&v->fm_groups);
    hm_free(&v->cmp_ff);
    hm_free(&v->cmp_frag);
    arena_free(&v->arena);
    return errs_count(errs) - before;
}
