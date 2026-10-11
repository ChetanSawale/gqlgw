/* toml.h - TOML 1.1.0 parser (into an arena). GNU C99, no deps.
 *
 * The whole document is validated (syntax, UTF-8, table definition rules);
 * parsing stops at the first error. Tables keep their keys in document order.
 */
#ifndef GQL_TOML_H
#define GQL_TOML_H

#include <stddef.h>
#include <stdint.h>

#include "ast.h"
#include "util.h"

typedef enum {
    TOML_STRING, TOML_INTEGER, TOML_FLOAT, TOML_BOOL,
    TOML_DATETIME,          /* offset date-time */
    TOML_DATETIME_LOCAL, TOML_DATE_LOCAL, TOML_TIME_LOCAL,
    TOML_ARRAY, TOML_TABLE
} toml_kind;

typedef struct toml_value toml_value;
struct toml_value {
    toml_kind kind;
    uint32_t flags;         /* internal: definition state */
    uint32_t sect;          /* internal: section whose dotted keys defined this table */
    uint32_t id;            /* tables: unique id (key lookup scope) */
    gql_loc loc;            /* value, or table header / first definition */
    gql_str key;            /* member name when inside a table */
    gql_loc key_loc;
    gql_str s;              /* STRING: decoded text; date/time kinds: normalized RFC 3339 text */
    int64_t i;              /* INTEGER, BOOL */
    double f;               /* FLOAT */
    toml_value *first;      /* ARRAY items / TABLE members, in document order */
    toml_value *last;
    toml_value *next;
    uint32_t count;
};

typedef struct {
    gql_arena arena;
    gql_hmap keys;          /* (table id, key) -> member index */
    gql_vec members;        /* toml_value* */
    toml_value *root;
    char err[256];
    gql_loc err_loc;
} toml_doc;

/* Parses src (n bytes). Returns 0 on success (doc->root set), -1 on error
 * (doc->err / doc->err_loc set). toml_free must be called in both cases. */
int  toml_parse(toml_doc *doc, const char *src, size_t n);
void toml_free(toml_doc *doc);

const toml_value *toml_get(const toml_doc *doc, const toml_value *table, const char *key);
/* toml-test tagged JSON of a value */
void toml_to_tagged_json(const toml_value *v, gql_buf *b);
const char *toml_kind_name(toml_kind k);

#endif
