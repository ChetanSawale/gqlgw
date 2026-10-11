/* validate.h - executable document validation (spec section 5, graphql-js 17 rules). */
#ifndef GQL_VALIDATE_H
#define GQL_VALIDATE_H

#include "ast.h"
#include "errors.h"
#include "schema.h"

typedef struct {
    int introspection;          /* 0: reject __schema / __type */
    uint32_t max_comparisons;   /* OverlappingFieldsCanBeMerged work bound, 0 = default */
} gql_validate_opts;

/* returns the number of errors added */
int gql_validate(const gql_schema *s, const ast_doc *doc, const gql_validate_opts *o, gql_errors *errs);

#endif
