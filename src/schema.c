/* schema.c - lookups and type helpers on the flat schema model. */
#include "schema.h"

#include <string.h>

const char *const gql_dirloc_names[DL_COUNT] = {
    "QUERY", "MUTATION", "SUBSCRIPTION", "FIELD", "FRAGMENT_DEFINITION", "FRAGMENT_SPREAD",
    "INLINE_FRAGMENT", "VARIABLE_DEFINITION", "FRAGMENT_VARIABLE_DEFINITION", "SCHEMA",
    "SCALAR", "OBJECT", "FIELD_DEFINITION", "ARGUMENT_DEFINITION", "INTERFACE", "UNION",
    "ENUM", "ENUM_VALUE", "INPUT_OBJECT", "INPUT_FIELD_DEFINITION", "DIRECTIVE_DEFINITION"
};

void gql_schema_unindex(gql_schema *s)
{
    hm_free(&s->types_by_name);
    hm_free(&s->members);
    hm_free(&s->possible);
    hm_free(&s->dirs_by_name);
}

int gql_schema_index(gql_schema *s)
{
    uint32_t i, j;
    gql_schema_unindex(s);
    s->types_by_name.base = &s->str;
    s->members.base = &s->str;
    s->possible.base = &s->str;
    s->dirs_by_name.base = &s->str;
    for (i = 0; i < s->ntypes; i++) {
        const gql_type_rec *t = &s->types[i];
        if (hm_put_off(&s->types_by_name, 0, t->name.off, t->name.len, i) < 0)
            return -1;
        if (t->kind == GK_OBJECT || t->kind == GK_INTERFACE) {
            for (j = 0; j < t->fields_count; j++) {
                const gql_field_rec *f = &s->fields[t->fields_first + j];
                if (hm_put_off(&s->members, i + 1, f->name.off, f->name.len, t->fields_first + j) < 0)
                    return -1;
            }
        } else if (t->kind == GK_INPUT) {
            for (j = 0; j < t->fields_count; j++) {
                const gql_ival_rec *f = &s->ivals[t->fields_first + j];
                if (hm_put_off(&s->members, i + 1, f->name.off, f->name.len, t->fields_first + j) < 0)
                    return -1;
            }
        } else if (t->kind == GK_ENUM) {
            for (j = 0; j < t->evals_count; j++) {
                const gql_eval_rec *v = &s->evals[t->evals_first + j];
                if (hm_put_off(&s->members, i + 1, v->name.off, v->name.len, t->evals_first + j) < 0)
                    return -1;
            }
        }
        if (gql_kind_is_abstract(t->kind)) {
            for (j = 0; j < t->possible_count; j++) {
                uint32_t o = s->idx[t->possible_first + j];
                if (hm_put_off(&s->possible, i + 1, s->types[o].name.off, s->types[o].name.len, o) < 0)
                    return -1;
            }
        }
    }
    for (i = 0; i < s->ndirs; i++)
        if (hm_put_off(&s->dirs_by_name, 0, s->dirs[i].name.off, s->dirs[i].name.len, i) < 0)
            return -1;
    s->t_string = gql_type_by_name(s, "String", 6);
    s->t_boolean = gql_type_by_name(s, "Boolean", 7);
    s->t_schema = gql_type_by_name(s, "__Schema", 8);
    s->t_type = gql_type_by_name(s, "__Type", 6);
    return 0;
}

uint32_t gql_type_by_name(const gql_schema *s, const char *name, uint32_t n)
{
    return hm_get(&s->types_by_name, 0, name, n);
}

uint32_t gql_member(const gql_schema *s, uint32_t type, const char *name, uint32_t n)
{
    return hm_get(&s->members, type + 1, name, n);
}

uint32_t gql_dir_by_name(const gql_schema *s, const char *name, uint32_t n)
{
    return hm_get(&s->dirs_by_name, 0, name, n);
}

uint32_t gql_field_lookup(const gql_schema *s, uint32_t type, const char *name, uint32_t n)
{
    const gql_type_rec *t = &s->types[type];
    if (n >= 2 && name[0] == '_' && name[1] == '_') {
        if (n == 10 && memcmp(name, "__typename", 10) == 0 && gql_kind_is_composite(t->kind))
            return s->f_typename;
        if (type == s->query_type) {
            if (n == 8 && memcmp(name, "__schema", 8) == 0)
                return s->f_schema;
            if (n == 6 && memcmp(name, "__type", 6) == 0)
                return s->f_type;
        }
    }
    if (t->kind != GK_OBJECT && t->kind != GK_INTERFACE)
        return GQL_NONE;
    return gql_member(s, type, name, n);
}

uint32_t gql_arg_lookup(const gql_schema *s, uint32_t first, uint32_t count, const char *name, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < count; i++) {
        const gql_ival_rec *a = &s->ivals[first + i];
        if (a->name.len == n && memcmp(GQL_S(s, a->name), name, n) == 0)
            return first + i;
    }
    return GQL_NONE;
}

uint32_t gql_tref_named(const gql_schema *s, uint32_t t)
{
    while (s->trefs[t].kind != TR_NAMED)
        t = s->trefs[t].of;
    return s->trefs[t].of;
}

void gql_print_tref(gql_buf *b, const gql_schema *s, uint32_t t)
{
    const gql_tref_rec *r = &s->trefs[t];
    switch (r->kind) {
    case TR_NAMED:
        buf_put(b, GQL_S(s, s->types[r->of].name), s->types[r->of].name.len);
        break;
    case TR_LIST:
        buf_putc(b, '[');
        gql_print_tref(b, s, r->of);
        buf_putc(b, ']');
        break;
    case TR_NONNULL:
        gql_print_tref(b, s, r->of);
        buf_putc(b, '!');
        break;
    }
}

int gql_is_possible(const gql_schema *s, uint32_t abstract, uint32_t object)
{
    if (abstract == object)
        return 1;
    if (!gql_kind_is_abstract(s->types[abstract].kind))
        return 0;
    return hm_get(&s->possible, abstract + 1, GQL_S(s, s->types[object].name),
                  s->types[object].name.len) != GQL_NONE;
}

int gql_types_overlap(const gql_schema *s, uint32_t a, uint32_t b)
{
    const gql_type_rec *ta = &s->types[a], *tb = &s->types[b];
    uint32_t i;
    if (a == b)
        return 1;
    if (gql_kind_is_abstract(ta->kind)) {
        if (gql_kind_is_abstract(tb->kind)) {
            for (i = 0; i < ta->possible_count; i++)
                if (gql_is_possible(s, b, s->idx[ta->possible_first + i]))
                    return 1;
            return 0;
        }
        return gql_is_possible(s, a, b);
    }
    if (gql_kind_is_abstract(tb->kind))
        return gql_is_possible(s, b, a);
    return 0;
}

int gql_named_is_subtype(const gql_schema *s, uint32_t sub, uint32_t super)
{
    const gql_type_rec *ts = &s->types[sub], *tp = &s->types[super];
    uint32_t i;
    if (sub == super)
        return 1;
    if (tp->kind == GK_UNION)
        return ts->kind == GK_OBJECT && gql_is_possible(s, super, sub);
    if (tp->kind == GK_INTERFACE && (ts->kind == GK_OBJECT || ts->kind == GK_INTERFACE)) {
        for (i = 0; i < ts->ifaces_count; i++)
            if (s->idx[ts->ifaces_first + i] == super)
                return 1;
    }
    return 0;
}

int gql_tref_is_subtype(const gql_schema *s, uint32_t sub, uint32_t super)
{
    const gql_tref_rec *a, *b;
    if (sub == super)
        return 1;
    a = &s->trefs[sub];
    b = &s->trefs[super];
    if (b->kind == TR_NONNULL) {
        if (a->kind == TR_NONNULL)
            return gql_tref_is_subtype(s, a->of, b->of);
        return 0;
    }
    if (a->kind == TR_NONNULL)
        return gql_tref_is_subtype(s, a->of, super);
    if (b->kind == TR_LIST) {
        if (a->kind == TR_LIST)
            return gql_tref_is_subtype(s, a->of, b->of);
        return 0;
    }
    if (a->kind == TR_LIST)
        return 0;
    return gql_named_is_subtype(s, a->of, b->of);
}
