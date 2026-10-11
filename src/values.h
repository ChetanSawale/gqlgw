/* values.h - input values: literal validation, literal -> JSON, JSON coercion. */
#ifndef GQL_VALUES_H
#define GQL_VALUES_H

#include "ast.h"
#include "json.h"
#include "schema.h"

/* A type view: either a schema type reference or an AST type (variable definition). */
typedef struct {
    const ast_type *ast;     /* non-NULL: AST type */
    uint32_t tref;           /* schema tref when ast == NULL; GQL_NONE = unknown type */
} gql_tv;

static inline gql_tv tv_tref(uint32_t t) { gql_tv v; v.ast = NULL; v.tref = t; return v; }
static inline gql_tv tv_ast(const ast_type *a) { gql_tv v; v.ast = a; v.tref = GQL_NONE; return v; }
static inline gql_tv tv_none(void) { return tv_tref(GQL_NONE); }
static inline int tv_valid(gql_tv t) { return t.ast || t.tref != GQL_NONE; }

int      tv_kind(const gql_schema *s, gql_tv t);          /* TR_NAMED / TR_LIST / TR_NONNULL */
gql_tv   tv_of(const gql_schema *s, gql_tv t);
uint32_t tv_named(const gql_schema *s, gql_tv t);         /* named type idx, GQL_NONE if unknown */
void     tv_print(gql_buf *b, const gql_schema *s, gql_tv t);
/* is the (possibly AST) type `sub` a subtype of schema type `super` */
int      tv_is_subtype(const gql_schema *s, gql_tv sub, gql_tv super);

/* ---- literal validation ------------------------------------------------ */

typedef struct gql_vcheck gql_vcheck;
struct gql_vcheck {
    const gql_schema *s;
    gql_buf *path;           /* optional: current path ("[1].field") while checking */
    void (*on_error)(gql_vcheck *c, const gql_loc *locs, int nlocs, const char *msg);
    /* every variable occurrence; loc may be invalid (inside a custom scalar) */
    void (*on_var)(gql_vcheck *c, const ast_value *var, gql_tv loc, int loc_has_default,
                   uint32_t parent_input);
    void *ud;
};

/* ValuesOfCorrectType + UniqueInputFieldNames for a literal (variables are not resolved). */
void gql_check_literal(gql_vcheck *c, gql_tv type, const ast_value *v, int has_default,
                       uint32_t parent_input);

/* ---- literal -> JSON ----------------------------------------------------- */

typedef struct {
    const gql_schema *s;
    /* coerced JSON of a variable, or NULL when the variable was not provided */
    const gql_str *(*var)(void *ud, gql_str name);
    void *ud;
} gql_vctx;

/* Writes the coerced value of a (valid) literal. Returns 0 when the value is
   absent (unprovided variable), 1 otherwise. */
int gql_literal_to_json(const gql_vctx *c, gql_tv type, const ast_value *v, gql_buf *out);

/* ---- JSON input coercion (variables) ----------------------------------- */

/* Coerces JSON input `v` (NULL = undefined) to `type`. On failure returns -1 and
   writes "<path>: <message>" style text in err (path may be empty). */
int gql_coerce_json(const gql_schema *s, gql_tv type, const gql_json *v, gql_buf *out,
                    gql_buf *err_path, gql_buf *err_msg);

/* inspect(): graphql-js style rendering of a JSON value for messages */
void gql_json_inspect(gql_buf *b, const gql_json *v, int depth);

#endif
