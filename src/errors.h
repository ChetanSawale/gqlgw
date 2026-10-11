/* errors.h - accumulated GraphQL errors (message + locations). */
#ifndef GQL_ERRORS_H
#define GQL_ERRORS_H

#include "ast.h"
#include "util.h"

#define GQL_ERR_MAXLOCS 4

typedef struct {
    char *msg;
    gql_loc locs[GQL_ERR_MAXLOCS];
    int nlocs;
} gql_err_item;

typedef struct {
    gql_vec items;      /* gql_err_item */
    uint32_t max;       /* stop recording after max (0 = unlimited) */
    int truncated;
} gql_errors;

void errs_init(gql_errors *e, uint32_t max);
void errs_free(gql_errors *e);
int  errs_count(const gql_errors *e);
int  errs_full(const gql_errors *e);
void errs_add(gql_errors *e, const gql_loc *locs, int nlocs, const char *fmt, ...) GQL_PRINTF(4, 5);
void errs_addv(gql_errors *e, const gql_loc *locs, int nlocs, const char *fmt, va_list ap);
/* JSON array of {"message":..,"locations":[{"line":..,"column":..}]} */
void errs_to_json(const gql_errors *e, gql_buf *out);
/* human readable: "line:col: message\n" */
void errs_to_text(const gql_errors *e, const char *file, gql_buf *out);

#endif
