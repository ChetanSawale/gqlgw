/* sdl.h - SDL compiler: builds the flat schema model and the gateway routes. */
#ifndef GQL_SDL_H
#define GQL_SDL_H

#include "errors.h"
#include "schema.h"

/* template parts of @fetch / @batch paths and bodies */
enum { TP_LIT, TP_ARGS, TP_ARGS_ALL, TP_PARENT, TP_KEYS };

#define TP_MAXSEGS 8

typedef struct gql_tpl_part {
    int kind;
    gql_str lit;                    /* TP_LIT text, TP_PARENT raw JSON key */
    gql_str segs[TP_MAXSEGS];       /* TP_ARGS path */
    int nsegs;
    struct gql_tpl_part *next;
} gql_tpl_part;

typedef struct {
    uint32_t field;                 /* field idx */
    gql_str source, method, select;
    gql_tpl_part *path, *body;
    int has_batch;
    gql_tpl_part *batch_path;
    gql_str batch_match, batch_select;
    uint32_t batch_max;
    int cache_ttl;                  /* -1: not cached */
    gql_str cache_scope;
} gql_route;

typedef struct gql_compiled gql_compiled;

/* Compiles one SDL document (plus the built-in prelude). Returns NULL on error. */
gql_compiled *gql_sdl_compile(const char *src, size_t n, gql_errors *errs, gql_errors *warnings);
void gql_compiled_free(gql_compiled *c);

const gql_schema *gql_compiled_schema(const gql_compiled *c);
uint32_t gql_compiled_nroutes(const gql_compiled *c);
const gql_route *gql_compiled_route(const gql_compiled *c, uint32_t i);
/* raw JSON key of a field (@from), or NULL if it is the field name */
const gql_str *gql_compiled_from(const gql_compiled *c, uint32_t field);
/* @discriminator key of an abstract type, or NULL */
const gql_str *gql_compiled_discriminator(const gql_compiled *c, uint32_t type);

/* writers (gqlb.c, routes.c) */
int gql_gqlb_write(const gql_schema *s, const char *path, char *err, size_t errn);
int gql_routes_write(const gql_compiled *c, const char *path, const char *sdl_name, char *err, size_t errn);

#endif
