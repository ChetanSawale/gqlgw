/* parser.h - GraphQL parser (executable + type system documents). */
#ifndef GQL_PARSER_H
#define GQL_PARSER_H

#include "ast.h"
#include "errors.h"
#include "util.h"

typedef struct {
    uint32_t max_tokens;     /* 0 = unlimited */
    uint32_t max_depth;      /* selection set nesting, 0 = default (512) */
} gql_parse_opts;

/* Parses a document. Returns NULL and records exactly one error on failure.
   All nodes and decoded strings live in `a`; names point into `src`, which must
   outlive the AST. */
ast_doc *gql_parse(const char *src, size_t n, gql_arena *a,
                   const gql_parse_opts *opts, gql_errors *errs);

/* Parses a single constant value (used for SDL default values printed in .gqlb). */
ast_value *gql_parse_const_value(const char *src, size_t n, gql_arena *a, gql_errors *errs);

/* Prints a value as graphql-js would (used for introspection defaultValue). */
void gql_print_value(gql_buf *b, const ast_value *v);
/* Prints a type reference, e.g. [Int!]! */
void gql_print_type(gql_buf *b, const ast_type *t);

const char *gql_op_name(gql_op_type op);

#endif
