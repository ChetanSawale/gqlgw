/* ast.h - GraphQL document AST (executable + type system), arena allocated. */
#ifndef GQL_AST_H
#define GQL_AST_H

#include <stdint.h>

typedef struct {
    const char *p;   /* not NUL-terminated in general */
    uint32_t n;
} gql_str;

typedef struct {
    uint32_t line, col;
} gql_loc;

/* ---- values ----------------------------------------------------------- */

typedef enum {
    AV_VARIABLE, AV_INT, AV_FLOAT, AV_STRING, AV_BOOLEAN, AV_NULL, AV_ENUM, AV_LIST, AV_OBJECT
} ast_value_kind;

typedef struct ast_value ast_value;
typedef struct ast_objfield ast_objfield;

struct ast_value {
    ast_value_kind kind;
    gql_loc loc;
    gql_str s;              /* var name, raw number text, decoded string, enum name */
    int boolean;
    int block;              /* string came from a block string */
    ast_value *items;       /* AV_LIST */
    ast_objfield *fields;   /* AV_OBJECT */
    ast_value *next;
};

struct ast_objfield {
    gql_str name;
    gql_loc loc;
    ast_value *value;
    ast_objfield *next;
};

/* ---- types ------------------------------------------------------------ */

typedef enum { AT_NAMED, AT_LIST, AT_NONNULL } ast_type_kind;

typedef struct ast_type {
    ast_type_kind kind;
    gql_loc loc;
    gql_str name;            /* AT_NAMED */
    struct ast_type *of;     /* AT_LIST / AT_NONNULL */
} ast_type;

/* ---- directives / arguments ------------------------------------------- */

typedef struct ast_arg {
    gql_str name;
    gql_loc loc;
    ast_value *value;
    struct ast_arg *next;
} ast_arg;

typedef struct ast_directive {
    gql_str name;
    gql_loc loc;
    ast_arg *args;
    struct ast_directive *next;
} ast_directive;

/* ---- selections ------------------------------------------------------- */

typedef enum { SEL_FIELD, SEL_SPREAD, SEL_INLINE } ast_sel_kind;

typedef struct ast_selset ast_selset;

typedef struct ast_selection {
    ast_sel_kind kind;
    gql_loc loc;
    gql_str alias;          /* field alias (n == 0 if none) */
    gql_str name;           /* field name / fragment name / inline type condition (n==0: none) */
    gql_loc name_loc;
    ast_arg *args;
    ast_directive *dirs;
    ast_selset *selset;     /* NULL for leaf fields / spreads */
    struct ast_selection *next;
} ast_selection;

struct ast_selset {
    gql_loc loc;
    ast_selection *first;
};

/* ---- definitions ------------------------------------------------------ */

typedef enum { OP_QUERY = 0, OP_MUTATION = 1, OP_SUBSCRIPTION = 2 } gql_op_type;

typedef struct ast_vardef {
    gql_str desc;
    gql_str name;
    gql_loc loc;
    gql_loc name_loc;
    ast_type *type;
    ast_value *defval;
    ast_directive *dirs;
    struct ast_vardef *next;
} ast_vardef;

typedef struct ast_inputval {
    gql_str desc;
    gql_str name;
    gql_loc loc;
    ast_type *type;
    ast_value *defval;
    ast_directive *dirs;
    struct ast_inputval *next;
} ast_inputval;

typedef struct ast_fielddef {
    gql_str desc;
    gql_str name;
    gql_loc loc;
    ast_inputval *args;
    ast_type *type;
    ast_directive *dirs;
    struct ast_fielddef *next;
} ast_fielddef;

typedef struct ast_enumval {
    gql_str desc;
    gql_str name;
    gql_loc loc;
    ast_directive *dirs;
    struct ast_enumval *next;
} ast_enumval;

typedef struct ast_name {
    gql_str name;
    gql_loc loc;
    struct ast_name *next;
} ast_name;

typedef struct ast_optype {
    gql_op_type op;
    gql_str type;
    gql_loc loc;
    struct ast_optype *next;
} ast_optype;

typedef enum {
    DEF_OPERATION, DEF_FRAGMENT,
    DEF_SCHEMA, DEF_SCALAR, DEF_OBJECT, DEF_INTERFACE, DEF_UNION, DEF_ENUM, DEF_INPUT, DEF_DIRECTIVE
} ast_def_kind;

typedef struct ast_def {
    ast_def_kind kind;
    int is_ext;              /* "extend ..." */
    gql_loc loc;
    gql_str desc;
    gql_str name;            /* may be empty for anonymous operations / schema */
    gql_loc name_loc;
    gql_op_type op;          /* DEF_OPERATION */
    int shorthand;           /* "{ ... }" query */
    ast_vardef *vars;        /* DEF_OPERATION */
    gql_str type_cond;       /* DEF_FRAGMENT */
    gql_loc type_cond_loc;
    ast_directive *dirs;
    ast_selset *selset;      /* operation / fragment */
    ast_name *names;         /* implements (object/interface), members (union), locations (directive) */
    ast_fielddef *fields;    /* object / interface */
    ast_inputval *inputs;    /* input object fields, directive arguments */
    ast_enumval *values;     /* enum */
    ast_optype *roots;       /* schema */
    int repeatable;          /* directive */
    struct ast_def *next;
} ast_def;

typedef struct {
    ast_def *defs;
    uint32_t ndefs;
    uint32_t ntokens;
} ast_doc;

static inline int gql_str_eq(gql_str a, gql_str b)
{
    return a.n == b.n && (a.n == 0 || __builtin_memcmp(a.p, b.p, a.n) == 0);
}

static inline int gql_str_eqz(gql_str a, const char *z)
{
    uint32_t i;
    for (i = 0; i < a.n; i++)
        if (!z[i] || z[i] != a.p[i])
            return 0;
    return z[a.n] == 0;
}

#endif
