/* json.h - small JSON parser (into an arena) and writer helpers. */
#ifndef GQL_JSON_H
#define GQL_JSON_H

#include <stddef.h>
#include <stdint.h>

#include "ast.h"
#include "util.h"

typedef enum { JS_NULL, JS_BOOL, JS_NUMBER, JS_STRING, JS_ARRAY, JS_OBJECT } gql_json_kind;

typedef struct gql_json gql_json;
struct gql_json {
    gql_json_kind kind;
    int boolean;
    gql_str s;            /* decoded string, or raw number text */
    gql_str key;          /* member key when inside an object */
    gql_json *first;      /* array items / object members */
    gql_json *next;
    uint32_t count;
};

/* Parses a complete JSON text. Returns NULL on error and writes a message into err. */
gql_json *gql_json_parse(const char *s, size_t n, gql_arena *a, uint32_t max_depth,
                         char *err, size_t errn);
void gql_json_write(gql_buf *b, const gql_json *v);
const gql_json *gql_json_get(const gql_json *obj, const char *key, uint32_t n);

#endif
