/* plan_int.h - internals shared by plan.c and introspect.c */
#ifndef GQL_PLAN_INT_H
#define GQL_PLAN_INT_H

#include "ast.h"
#include "gql.h"
#include "schema.h"
#include "util.h"

typedef struct {
    gql_str name;
    gql_str json;       /* coerced value */
} gql_varval;

typedef struct {
    const gql_schema *s;
    const ast_doc *doc;
    const gql_hmap *frags;  /* fragment name -> index in fragdefs */
    const ast_def **fragdefs;
    uint32_t nfrags;
    gql_varval *vars;
    uint32_t nvars;
    gql_arena *arena;
} gql_collector;

typedef struct {
    gql_str key;
    const ast_selection **nodes;
    uint32_t n;
} gql_group;

/* CollectFields(objectType, selectionSets) -> ordered groups (arena allocated) */
int gql_collect(gql_collector *c, uint32_t objtype, const ast_selset *const *sets, uint32_t nsets,
                gql_group **out, uint32_t *nout);
/* coerced JSON of a variable, NULL when not provided */
const gql_str *gql_collector_var(gql_collector *c, gql_str name);
/* evaluates a Boolean / String argument of a (meta) field node */
int gql_arg_bool(gql_collector *c, const ast_selection *node, const char *name, int dflt);
int gql_arg_string(gql_collector *c, const ast_selection *node, const char *name, gql_str *out, gql_buf *tmp);

/* renders __schema / __type for a group of field nodes into out (JSON value) */
int gql_introspect(gql_collector *c, uint32_t meta_field, const gql_group *g, gql_buf *out);

#endif
