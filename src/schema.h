/* schema.h - flat, index-based schema model.
 *
 * The same structures are filled by the SDL compiler (sdl.c) and memory-mapped
 * from a .gqlb file at runtime (gqlb.c). Records only contain integers: strings
 * are (offset, length) references into a NUL-terminated string pool, and all
 * cross references are indices, so the arrays can be dumped / mapped as is.
 */
#ifndef GQL_SCHEMA_H
#define GQL_SCHEMA_H

#include <stdint.h>

#include "util.h"

typedef struct {
    uint32_t off, len;
} gql_sref;

enum { GK_SCALAR = 0, GK_OBJECT, GK_INTERFACE, GK_UNION, GK_ENUM, GK_INPUT };
enum { TR_NAMED = 0, TR_LIST, TR_NONNULL };
enum { SK_CUSTOM = 0, SK_INT, SK_FLOAT, SK_STRING, SK_BOOLEAN, SK_ID };

/* type flags */
#define TF_BUILTIN        0x01u   /* spec scalar */
#define TF_INTROSPECTION  0x02u   /* __Schema & co */
#define TF_ONEOF          0x04u
#define TF_REFERENCED     0x08u   /* referenced by some type reference */

/* member flags (fields, input values, enum values) */
#define MF_DEPRECATED     0x01u
#define MF_HAS_DEFAULT    0x02u

/* directive flags */
#define DF_REPEATABLE     0x01u
#define DF_HIDDEN         0x02u   /* gateway directive: not exposed by introspection */
#define DF_BUILTIN        0x04u
#define DF_DEPRECATED     0x08u

/* directive locations, in __DirectiveLocation order (graphql-js 17) */
enum {
    DL_QUERY, DL_MUTATION, DL_SUBSCRIPTION, DL_FIELD, DL_FRAGMENT_DEFINITION, DL_FRAGMENT_SPREAD,
    DL_INLINE_FRAGMENT, DL_VARIABLE_DEFINITION, DL_FRAGMENT_VARIABLE_DEFINITION, DL_SCHEMA,
    DL_SCALAR, DL_OBJECT, DL_FIELD_DEFINITION, DL_ARGUMENT_DEFINITION, DL_INTERFACE, DL_UNION,
    DL_ENUM, DL_ENUM_VALUE, DL_INPUT_OBJECT, DL_INPUT_FIELD_DEFINITION, DL_DIRECTIVE_DEFINITION,
    DL_COUNT
};
extern const char *const gql_dirloc_names[DL_COUNT];

typedef struct {
    uint32_t kind, flags, scalar;
    gql_sref name, desc, specified_by;
    uint32_t fields_first, fields_count;      /* OBJECT/INTERFACE: fields[]; INPUT: ivals[] */
    uint32_t ifaces_first, ifaces_count;      /* idx[]: implemented interfaces */
    uint32_t possible_first, possible_count;  /* idx[]: possible object types (UNION/INTERFACE) */
    uint32_t evals_first, evals_count;        /* ENUM */
    uint32_t pad;
} gql_type_rec;

typedef struct {
    gql_sref name, desc, deprecation;
    uint32_t flags, owner, type;              /* owner: type idx (GQL_NONE for meta fields) */
    uint32_t args_first, args_count;          /* ivals[] */
    uint32_t pad;
} gql_field_rec;

typedef struct {
    gql_sref name, desc, deprecation;
    gql_sref defval;                          /* default printed as a GraphQL literal */
    gql_sref defjson;                         /* default coerced, as JSON */
    uint32_t flags, type;
} gql_ival_rec;

typedef struct {
    gql_sref name, desc, deprecation;
    uint32_t flags, owner;
} gql_eval_rec;

typedef struct {
    uint32_t kind, of;                        /* NAMED: of = type idx; else of = tref idx */
} gql_tref_rec;

typedef struct {
    gql_sref name, desc, deprecation;
    uint32_t locmask, flags;
    uint32_t args_first, args_count;          /* ivals[] */
    uint32_t locs_first, locs_count;          /* idx[]: location codes, declaration order */
} gql_dir_rec;

typedef struct gql_schema {
    const gql_type_rec *types;   uint32_t ntypes;
    const gql_field_rec *fields; uint32_t nfields;
    const gql_ival_rec *ivals;   uint32_t nivals;
    const gql_eval_rec *evals;   uint32_t nevals;
    const gql_tref_rec *trefs;   uint32_t ntrefs;
    const uint32_t *idx;         uint32_t nidx;
    const gql_dir_rec *dirs;     uint32_t ndirs;
    const char *str;             uint32_t nstr;

    uint32_t query_type, mutation_type, subscription_type;
    gql_sref desc;
    uint64_t hash;

    /* meta fields (in fields[], owner GQL_NONE) and well-known types */
    uint32_t f_typename, f_schema, f_type;
    uint32_t t_string, t_boolean, t_schema, t_type;

    /* lookups, built by gql_schema_index() */
    gql_hmap types_by_name;
    gql_hmap members;            /* scope = type+1 -> field/ival/eval idx */
    gql_hmap possible;           /* scope = abstract type+1, key = object name -> object idx */
    gql_hmap dirs_by_name;

    /* backing storage */
    void *map;   size_t maplen;  /* mmap'ed .gqlb */
    void *owned;                 /* heap block owned by the schema */
} gql_schema;

#define GQL_S(s, r) ((s)->str + (r).off)

int  gql_schema_index(gql_schema *s);       /* builds lookups; 0 on success */
void gql_schema_unindex(gql_schema *s);

uint32_t gql_type_by_name(const gql_schema *s, const char *name, uint32_t n);
uint32_t gql_member(const gql_schema *s, uint32_t type, const char *name, uint32_t n);
uint32_t gql_dir_by_name(const gql_schema *s, const char *name, uint32_t n);
/* field lookup on a composite type, including __typename / __schema / __type */
uint32_t gql_field_lookup(const gql_schema *s, uint32_t type, const char *name, uint32_t n);
/* argument lookup within an ival range */
uint32_t gql_arg_lookup(const gql_schema *s, uint32_t first, uint32_t count, const char *name, uint32_t n);

uint32_t gql_tref_named(const gql_schema *s, uint32_t tref);   /* named type idx */
static inline int gql_tref_nonnull(const gql_schema *s, uint32_t t) { return s->trefs[t].kind == TR_NONNULL; }
static inline uint32_t gql_tref_nullable(const gql_schema *s, uint32_t t)
{
    return s->trefs[t].kind == TR_NONNULL ? s->trefs[t].of : t;
}
void gql_print_tref(gql_buf *b, const gql_schema *s, uint32_t tref);

static inline int gql_kind_is_leaf(uint32_t k) { return k == GK_SCALAR || k == GK_ENUM; }
static inline int gql_kind_is_composite(uint32_t k) { return k == GK_OBJECT || k == GK_INTERFACE || k == GK_UNION; }
static inline int gql_kind_is_abstract(uint32_t k) { return k == GK_INTERFACE || k == GK_UNION; }
static inline int gql_kind_is_input(uint32_t k) { return k == GK_SCALAR || k == GK_ENUM || k == GK_INPUT; }
static inline int gql_kind_is_output(uint32_t k) { return k != GK_INPUT; }

/* is `object` a possible type of `abstract` (object idx, abstract idx) */
int gql_is_possible(const gql_schema *s, uint32_t abstract, uint32_t object);
/* do the possible types of two composite types overlap */
int gql_types_overlap(const gql_schema *s, uint32_t a, uint32_t b);
/* isTypeSubTypeOf(maybeSub, super) on type references */
int gql_tref_is_subtype(const gql_schema *s, uint32_t sub, uint32_t super);
/* is named type `sub` (object/interface) a subtype of named type `super` */
int gql_named_is_subtype(const gql_schema *s, uint32_t sub, uint32_t super);

#endif
