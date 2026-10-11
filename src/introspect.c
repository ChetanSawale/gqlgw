/* introspect.c - renders __schema / __type selections straight to JSON.
 *
 * Introspection only depends on the schema and the query, so it is computed
 * while planning and handed to Lua as raw JSON.
 */
#include "plan_int.h"

#include <stdlib.h>
#include <string.h>

/* a value of one of the introspection types */
typedef enum { IO_SCHEMA, IO_TYPE, IO_TREF, IO_FIELD, IO_IVAL, IO_EVAL, IO_DIR } io_kind;

typedef struct {
    io_kind kind;
    uint32_t idx;
} io_obj;

typedef struct {
    gql_collector *c;
    const gql_schema *s;
    gql_buf *out;
    uint8_t *visible;          /* per type: listed in __schema.types */
    int failed;
} IC;

static void render_obj(IC *ic, io_obj o, const ast_selset *const *sets, uint32_t nsets);

static const char *meta_type_name(io_kind k)
{
    switch (k) {
    case IO_SCHEMA: return "__Schema";
    case IO_TYPE: case IO_TREF: return "__Type";
    case IO_FIELD: return "__Field";
    case IO_IVAL: return "__InputValue";
    case IO_EVAL: return "__EnumValue";
    case IO_DIR: return "__Directive";
    }
    return "?";
}

static void jstr(IC *ic, gql_sref r)
{
    buf_json_str(ic->out, GQL_S(ic->s, r), r.len);
}

static void jstr_or_null(IC *ic, gql_sref r)
{
    if (r.len)
        jstr(ic, r);
    else
        buf_puts(ic->out, "null");
}

static void jbool(IC *ic, int b)
{
    buf_puts(ic->out, b ? "true" : "false");
}

/* renders a list field: items are produced by the iterator callback */
static void list_begin(IC *ic) { buf_putc(ic->out, '['); }
static void list_end(IC *ic) { buf_putc(ic->out, ']'); }

static void child(IC *ic, const gql_group *g, io_obj o)
{
    const ast_selset **sets = arena_alloc(ic->c->arena, (g->n + 1) * sizeof *sets);
    uint32_t k, n = 0;
    if (!sets) {
        ic->failed = 1;
        buf_puts(ic->out, "null");
        return;
    }
    for (k = 0; k < g->n; k++)
        if (g->nodes[k]->selset)
            sets[n++] = g->nodes[k]->selset;
    render_obj(ic, o, sets, n);
}

static void item(IC *ic, const gql_group *g, io_obj o, int *first)
{
    if (!*first)
        buf_putc(ic->out, ',');
    *first = 0;
    child(ic, g, o);
}

static io_obj mk(io_kind k, uint32_t i)
{
    io_obj o;
    o.kind = k;
    o.idx = i;
    return o;
}

/* type reference as a __Type: wrappers become LIST / NON_NULL */
static io_obj tref_obj(const gql_schema *s, uint32_t tref)
{
    if (s->trefs[tref].kind == TR_NAMED)
        return mk(IO_TYPE, s->trefs[tref].of);
    return mk(IO_TREF, tref);
}

static const char *kind_name(uint32_t k)
{
    switch (k) {
    case GK_SCALAR: return "SCALAR";
    case GK_OBJECT: return "OBJECT";
    case GK_INTERFACE: return "INTERFACE";
    case GK_UNION: return "UNION";
    case GK_ENUM: return "ENUM";
    case GK_INPUT: return "INPUT_OBJECT";
    }
    return "SCALAR";
}

static int dir_visible(const gql_schema *s, uint32_t d)
{
    return !(s->dirs[d].flags & DF_HIDDEN);
}

static void ivals_list(IC *ic, const gql_group *g, uint32_t first, uint32_t count, int incl_depr)
{
    uint32_t k;
    int f = 1;
    list_begin(ic);
    for (k = 0; k < count; k++) {
        const gql_ival_rec *a = &ic->s->ivals[first + k];
        if ((a->flags & MF_DEPRECATED) && !incl_depr)
            continue;
        item(ic, g, mk(IO_IVAL, first + k), &f);
    }
    list_end(ic);
}

static void field_value(IC *ic, io_obj o, const gql_group *g)
{
    const gql_schema *s = ic->s;
    const ast_selection *n0 = g->nodes[0];
    gql_str name = n0->name;
    uint32_t k;
    int f = 1;

#define IS(z) gql_str_eqz(name, z)
    if (IS("__typename")) {
        buf_json_str(ic->out, meta_type_name(o.kind), strlen(meta_type_name(o.kind)));
        return;
    }
    switch (o.kind) {
    case IO_SCHEMA:
        if (IS("description")) {
            jstr_or_null(ic, s->desc);
        } else if (IS("types")) {
            list_begin(ic);
            for (k = 0; k < s->ntypes; k++)
                if (ic->visible[k])
                    item(ic, g, mk(IO_TYPE, k), &f);
            list_end(ic);
        } else if (IS("queryType")) {
            child(ic, g, mk(IO_TYPE, s->query_type));
        } else if (IS("mutationType")) {
            if (s->mutation_type == GQL_NONE)
                buf_puts(ic->out, "null");
            else
                child(ic, g, mk(IO_TYPE, s->mutation_type));
        } else if (IS("subscriptionType")) {
            buf_puts(ic->out, "null");
        } else if (IS("directives")) {
            int incl = gql_arg_bool(ic->c, n0, "includeDeprecated", 0);
            list_begin(ic);
            for (k = 0; k < s->ndirs; k++)
                if (dir_visible(s, k) && (incl || !(s->dirs[k].flags & DF_DEPRECATED)))
                    item(ic, g, mk(IO_DIR, k), &f);
            list_end(ic);
        } else {
            buf_puts(ic->out, "null");
        }
        return;
    case IO_TREF: {
        const gql_tref_rec *t = &s->trefs[o.idx];
        if (IS("kind"))
            buf_puts(ic->out, t->kind == TR_LIST ? "\"LIST\"" : "\"NON_NULL\"");
        else if (IS("ofType"))
            child(ic, g, tref_obj(s, t->of));
        else
            buf_puts(ic->out, "null");
        return;
    }
    case IO_TYPE: {
        const gql_type_rec *t = &s->types[o.idx];
        int incl;
        if (IS("kind")) {
            buf_putc(ic->out, '"');
            buf_puts(ic->out, kind_name(t->kind));
            buf_putc(ic->out, '"');
        } else if (IS("name")) {
            jstr(ic, t->name);
        } else if (IS("description")) {
            jstr_or_null(ic, t->desc);
        } else if (IS("specifiedByURL")) {
            if (t->kind == GK_SCALAR)
                jstr_or_null(ic, t->specified_by);
            else
                buf_puts(ic->out, "null");
        } else if (IS("fields")) {
            if (t->kind != GK_OBJECT && t->kind != GK_INTERFACE) {
                buf_puts(ic->out, "null");
                return;
            }
            incl = gql_arg_bool(ic->c, n0, "includeDeprecated", 0);
            list_begin(ic);
            for (k = 0; k < t->fields_count; k++) {
                const gql_field_rec *fr = &s->fields[t->fields_first + k];
                if ((fr->flags & MF_DEPRECATED) && !incl)
                    continue;
                item(ic, g, mk(IO_FIELD, t->fields_first + k), &f);
            }
            list_end(ic);
        } else if (IS("interfaces")) {
            if (t->kind != GK_OBJECT && t->kind != GK_INTERFACE) {
                buf_puts(ic->out, "null");
                return;
            }
            list_begin(ic);
            for (k = 0; k < t->ifaces_count; k++)
                item(ic, g, mk(IO_TYPE, s->idx[t->ifaces_first + k]), &f);
            list_end(ic);
        } else if (IS("possibleTypes")) {
            if (!gql_kind_is_abstract(t->kind)) {
                buf_puts(ic->out, "null");
                return;
            }
            list_begin(ic);
            for (k = 0; k < t->possible_count; k++)
                item(ic, g, mk(IO_TYPE, s->idx[t->possible_first + k]), &f);
            list_end(ic);
        } else if (IS("enumValues")) {
            if (t->kind != GK_ENUM) {
                buf_puts(ic->out, "null");
                return;
            }
            incl = gql_arg_bool(ic->c, n0, "includeDeprecated", 0);
            list_begin(ic);
            for (k = 0; k < t->evals_count; k++) {
                const gql_eval_rec *e = &s->evals[t->evals_first + k];
                if ((e->flags & MF_DEPRECATED) && !incl)
                    continue;
                item(ic, g, mk(IO_EVAL, t->evals_first + k), &f);
            }
            list_end(ic);
        } else if (IS("inputFields")) {
            if (t->kind != GK_INPUT) {
                buf_puts(ic->out, "null");
                return;
            }
            ivals_list(ic, g, t->fields_first, t->fields_count, gql_arg_bool(ic->c, n0, "includeDeprecated", 0));
        } else if (IS("ofType")) {
            buf_puts(ic->out, "null");
        } else if (IS("isOneOf")) {
            if (t->kind == GK_INPUT)
                jbool(ic, (t->flags & TF_ONEOF) != 0);
            else
                buf_puts(ic->out, "null");
        } else {
            buf_puts(ic->out, "null");
        }
        return;
    }
    case IO_FIELD: {
        const gql_field_rec *fr = &s->fields[o.idx];
        if (IS("name"))
            jstr(ic, fr->name);
        else if (IS("description"))
            jstr_or_null(ic, fr->desc);
        else if (IS("args"))
            ivals_list(ic, g, fr->args_first, fr->args_count, gql_arg_bool(ic->c, n0, "includeDeprecated", 0));
        else if (IS("type"))
            child(ic, g, tref_obj(s, fr->type));
        else if (IS("isDeprecated"))
            jbool(ic, (fr->flags & MF_DEPRECATED) != 0);
        else if (IS("deprecationReason"))
            if (fr->flags & MF_DEPRECATED)
                jstr(ic, fr->deprecation);
            else
                buf_puts(ic->out, "null");
        else
            buf_puts(ic->out, "null");
        return;
    }
    case IO_IVAL: {
        const gql_ival_rec *a = &s->ivals[o.idx];
        if (IS("name"))
            jstr(ic, a->name);
        else if (IS("description"))
            jstr_or_null(ic, a->desc);
        else if (IS("type"))
            child(ic, g, tref_obj(s, a->type));
        else if (IS("defaultValue"))
            if (a->flags & MF_HAS_DEFAULT)
                jstr(ic, a->defval);
            else
                buf_puts(ic->out, "null");
        else if (IS("isDeprecated"))
            jbool(ic, (a->flags & MF_DEPRECATED) != 0);
        else if (IS("deprecationReason"))
            if (a->flags & MF_DEPRECATED)
                jstr(ic, a->deprecation);
            else
                buf_puts(ic->out, "null");
        else
            buf_puts(ic->out, "null");
        return;
    }
    case IO_EVAL: {
        const gql_eval_rec *e = &s->evals[o.idx];
        if (IS("name"))
            jstr(ic, e->name);
        else if (IS("description"))
            jstr_or_null(ic, e->desc);
        else if (IS("isDeprecated"))
            jbool(ic, (e->flags & MF_DEPRECATED) != 0);
        else if (IS("deprecationReason"))
            if (e->flags & MF_DEPRECATED)
                jstr(ic, e->deprecation);
            else
                buf_puts(ic->out, "null");
        else
            buf_puts(ic->out, "null");
        return;
    }
    case IO_DIR: {
        const gql_dir_rec *d = &s->dirs[o.idx];
        if (IS("name")) {
            jstr(ic, d->name);
        } else if (IS("description")) {
            jstr_or_null(ic, d->desc);
        } else if (IS("isRepeatable")) {
            jbool(ic, (d->flags & DF_REPEATABLE) != 0);
        } else if (IS("locations")) {
            list_begin(ic);
            for (k = 0; k < d->locs_count; k++) {
                if (k)
                    buf_putc(ic->out, ',');
                buf_putc(ic->out, '"');
                buf_puts(ic->out, gql_dirloc_names[s->idx[d->locs_first + k]]);
                buf_putc(ic->out, '"');
            }
            list_end(ic);
        } else if (IS("args")) {
            ivals_list(ic, g, d->args_first, d->args_count, gql_arg_bool(ic->c, n0, "includeDeprecated", 0));
        } else if (IS("isDeprecated")) {
            jbool(ic, (d->flags & DF_DEPRECATED) != 0);
        } else if (IS("deprecationReason")) {
            if (d->flags & DF_DEPRECATED)
                jstr(ic, d->deprecation);
            else
                buf_puts(ic->out, "null");
        } else {
            buf_puts(ic->out, "null");
        }
        return;
    }
    }
#undef IS
    buf_puts(ic->out, "null");
}

static void render_obj(IC *ic, io_obj o, const ast_selset *const *sets, uint32_t nsets)
{
    const char *tn = meta_type_name(o.kind);
    uint32_t mt = gql_type_by_name(ic->s, tn, (uint32_t)strlen(tn)), ng, k;
    gql_group *groups;
    if (mt == GQL_NONE || gql_collect(ic->c, mt, sets, nsets, &groups, &ng)) {
        ic->failed = 1;
        buf_puts(ic->out, "null");
        return;
    }
    buf_putc(ic->out, '{');
    for (k = 0; k < ng; k++) {
        if (k)
            buf_putc(ic->out, ',');
        buf_json_str(ic->out, groups[k].key.p, groups[k].key.n);
        buf_putc(ic->out, ':');
        field_value(ic, o, &groups[k]);
    }
    buf_putc(ic->out, '}');
}

static void mark_tref(const gql_schema *s, uint8_t *visible, uint32_t t)
{
    if (t < s->ntrefs)
        visible[gql_tref_named(s, t)] = 1;
}

/* types listed by __schema.types: user types, introspection types and the
   built-in scalars that are actually referenced (like graphql-js) */
static uint8_t *compute_visible(const gql_schema *s)
{
    uint8_t *v = calloc(s->ntypes ? s->ntypes : 1, 1);
    uint32_t i, k;
    if (!v)
        return NULL;
    for (i = 0; i < s->ntypes; i++)
        if (!(s->types[i].flags & TF_BUILTIN))
            v[i] = 1;
    for (i = 0; i < s->nfields; i++)
        if (s->fields[i].owner != GQL_NONE)
            mark_tref(s, v, s->fields[i].type);
    for (i = 0; i < s->ntypes; i++) {
        const gql_type_rec *t = &s->types[i];
        if (t->kind == GK_OBJECT || t->kind == GK_INTERFACE) {
            for (k = 0; k < t->fields_count; k++) {
                const gql_field_rec *f = &s->fields[t->fields_first + k];
                uint32_t a;
                for (a = 0; a < f->args_count; a++)
                    mark_tref(s, v, s->ivals[f->args_first + a].type);
            }
        } else if (t->kind == GK_INPUT) {
            for (k = 0; k < t->fields_count; k++)
                mark_tref(s, v, s->ivals[t->fields_first + k].type);
        }
    }
    for (i = 0; i < s->ndirs; i++) {
        if (s->dirs[i].flags & DF_HIDDEN)
            continue;
        for (k = 0; k < s->dirs[i].args_count; k++)
            mark_tref(s, v, s->ivals[s->dirs[i].args_first + k].type);
    }
    return v;
}

int gql_introspect(gql_collector *c, uint32_t meta_field, const gql_group *g, gql_buf *out)
{
    IC ic;
    memset(&ic, 0, sizeof ic);
    ic.c = c;
    ic.s = c->s;
    ic.out = out;
    ic.visible = compute_visible(c->s);
    if (!ic.visible)
        return -1;
    if (meta_field == c->s->f_schema) {
        child(&ic, g, mk(IO_SCHEMA, 0));
    } else {
        gql_str name;
        gql_buf tmp;
        uint32_t t;
        buf_init(&tmp);
        if (gql_arg_string(c, g->nodes[0], "name", &name, &tmp) &&
            (t = gql_type_by_name(c->s, name.p, name.n)) != GQL_NONE && ic.visible[t])
            child(&ic, g, mk(IO_TYPE, t));
        else
            buf_puts(out, "null");
        buf_free(&tmp);
    }
    free(ic.visible);
    return ic.failed || out->oom ? -1 : 0;
}
