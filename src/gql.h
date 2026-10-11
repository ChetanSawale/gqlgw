/* gql.h - public C API of libgql (used by gqlc and by the LuaJIT FFI binding).
 *
 * Every structure in this header is part of the FFI ABI: lua/gqlgw/ffi.lua
 * declares the same layouts. Keep both in sync (GQL_ABI_VERSION).
 */
#ifndef GQL_H
#define GQL_H

#include <stddef.h>
#include <stdint.h>

#include "schema.h"

#define GQL_ABI_VERSION 1

#if defined(__GNUC__)
#define GQL_API __attribute__((visibility("default")))
#else
#define GQL_API
#endif

typedef struct gql_doc gql_doc;
typedef struct gql_plan gql_plan;

typedef struct {
    uint32_t max_bytes;         /* query text size, 0 = unlimited */
    uint32_t max_tokens;        /* lexer tokens, 0 = unlimited */
    uint32_t max_depth;         /* selection set nesting, 0 = 512 */
    uint32_t max_errors;        /* validation errors reported, 0 = 100 */
    uint32_t max_fields;        /* fields in an execution plan, 0 = unlimited */
    uint32_t max_memory;        /* bytes of arena per document / plan, 0 = unlimited */
    uint32_t introspection;     /* 0 = __schema / __type are rejected */
    uint32_t reserved;
} gql_limits;

/* plan field kinds */
enum { PF_LEAF = 0, PF_OBJECT = 1, PF_ABSTRACT = 2, PF_RAW = 3 };

typedef struct {
    uint32_t key_off, key_len;      /* response key (alias or name), in plan strings */
    uint32_t field;                 /* schema field index (meta fields included) */
    uint32_t kind;                  /* PF_* */
    uint32_t child_first;           /* OBJECT: selection idx; ABSTRACT: first case idx */
    uint32_t child_count;           /* ABSTRACT: number of cases */
    uint32_t args_off, args_len;    /* coerced arguments as a JSON object; PF_RAW: the JSON value */
    uint32_t line, col;             /* location of the field in the query */
} gql_plan_field;

typedef struct {
    uint32_t type;                  /* concrete object type */
    uint32_t fields_first, fields_count;
    uint32_t pad;
} gql_plan_sel;

typedef struct {
    uint32_t type;                  /* concrete object type */
    uint32_t sel;
} gql_plan_case;

typedef struct {
    uint32_t op;                    /* 0 query, 1 mutation */
    uint32_t root_sel;
    const gql_plan_sel *sels;       uint32_t nsels;
    const gql_plan_field *fields;   uint32_t nfields;
    const gql_plan_case *cases;     uint32_t ncases;
    const char *str;                uint32_t nstr;
} gql_plan_view;

/* read-only view of the schema arrays (records are declared in schema.h) */
typedef struct {
    const gql_type_rec *types;   uint32_t ntypes;
    const gql_field_rec *fields; uint32_t nfields;
    const gql_ival_rec *ivals;   uint32_t nivals;
    const gql_eval_rec *evals;   uint32_t nevals;
    const gql_tref_rec *trefs;   uint32_t ntrefs;
    const uint32_t *idx;         uint32_t nidx;
    const char *str;             uint32_t nstr;
    uint32_t query_type, mutation_type;
    uint32_t f_typename, f_schema, f_type;
} gql_schema_view;

/* ---- schema ---------------------------------------------------------- */

GQL_API int gql_abi_version(void);
GQL_API gql_schema *gql_schema_open(const char *path, char *err, size_t errn);
GQL_API void gql_schema_close(gql_schema *s);
GQL_API uint64_t gql_schema_hash(const gql_schema *s);
/* 16 hex digits + NUL */
GQL_API void gql_schema_hash_hex(const gql_schema *s, char out[17]);
GQL_API void gql_schema_get_view(const gql_schema *s, gql_schema_view *out);

/* ---- documents: parse + validate (variable independent, cacheable) ---- */

GQL_API gql_doc *gql_doc_new(const gql_schema *s, const char *q, size_t n, const gql_limits *lim);
GQL_API int gql_doc_ok(const gql_doc *d);
/* JSON array of GraphQL errors (valid until gql_doc_free) */
GQL_API const char *gql_doc_errors(const gql_doc *d, size_t *len);
GQL_API void gql_doc_free(gql_doc *d);

/* ---- plans: operation selection, variables, normalization ------------- */

GQL_API gql_plan *gql_plan_new(const gql_schema *s, const gql_doc *d, const char *op, size_t opn,
                               const char *vars, size_t varsn, const gql_limits *lim);
GQL_API int gql_plan_ok(const gql_plan *p);
GQL_API const char *gql_plan_errors(const gql_plan *p, size_t *len);
GQL_API const gql_plan_view *gql_plan_get_view(const gql_plan *p);
GQL_API void gql_plan_free(gql_plan *p);

#endif
