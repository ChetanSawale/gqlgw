/* config.h - gqlgw.toml: typed gateway configuration (gqlc only).
 *
 * Two modes:
 *   CFG_CHECK  (gqlc compile -c, at image build): environment not available;
 *              ${VAR} syntax is checked and values using it are validated later.
 *   CFG_RENDER (gqlc render, at container start): ${VAR} interpolated from the
 *              environment, everything validated.
 */
#ifndef GQL_CONFIG_H
#define GQL_CONFIG_H

#include <stdint.h>

#include "errors.h"
#include "util.h"

typedef enum { CFG_CHECK, CFG_RENDER } gqlgw_cfg_mode;

/* environment lookup (getenv by default); returns NULL when unset */
typedef const char *(*gqlgw_getenv_fn)(const char *name, void *ud);

typedef struct { char *text; int64_t bytes; } cfg_size;
typedef struct { char **v; uint32_t n; } cfg_list;
typedef struct { char **k, **v; uint32_t n; } cfg_map;

enum { HDR_ALLOW = 1, HDR_DENY = 2 };

typedef struct {
    char *name;
    gql_loc loc;
    int deferred;                   /* CHECK mode: url uses ${VAR} */
    char *url;
    char *scheme;                   /* "http" / "https" */
    char *host;                     /* name, IPv4 or [IPv6] */
    int port;
    int host_is_ip;
    char *prefix;                   /* path prefix without trailing slash ("" if none) */
    char *connect_timeout, *send_timeout, *read_timeout, *keepalive_timeout;
    int64_t keepalive;
    int headers_mode;               /* HDR_ALLOW / HDR_DENY */
    cfg_list forward_headers, drop_headers;
    cfg_map set_headers;
    int tls_verify;
    char *ca, *tls_name, *host_header;
} gqlgw_source;

typedef struct {
    gql_arena arena;
    char *file;                     /* path of the TOML file */
    char *dir;                      /* its directory (absolute when possible) */
    struct {
        char *listen, *server_name, *path, *schema, *routes, *lib, *access_log;
        int introspection, strict_path_args, debug_headers;
        cfg_size max_body;
    } gw;
    struct {
        int64_t query_bytes, tokens, depth, errors, plan_fields, rounds, fetches, concurrency;
        cfg_size memory, response_bytes;
    } lim;
    struct {
        cfg_size shm_size;
        int64_t l1_size;
        double l1_ttl, l2_ttl, gen_ttl, negative_ttl, down_ttl;
        char *private_header, *prefix;
        int memcached;              /* [cache.memcached] present */
        char *host, *socket;
        int64_t port, timeout, pool_size, keepalive;
    } cache;
    struct {
        int init;
        cfg_list resolver;          /* "auto" expanded in RENDER mode */
        char *resolver_valid;
        int resolver_ipv6;
    } ngx;
    struct {
        int enabled;
        char *listen, *path;
        cfg_list allow, exclude_operations;
        cfg_size shm_size;
        double *buckets;
        uint32_t nbuckets;
    } metrics;
    struct {
        int enabled;
        char *path;
    } purge;
    gqlgw_source *sources;
    uint32_t nsources;
} gqlgw_config;

/* Loads src (the TOML text of path). Returns 0 when there is no error.
 * env may be NULL (getenv). resolv_conf: path used for resolver = "auto"
 * (NULL: /etc/resolv.conf). */
int gqlgw_config_load(gqlgw_config *c, const char *path, const char *src, size_t n, gqlgw_cfg_mode mode,
                      gqlgw_getenv_fn env, void *ud, const char *resolv_conf, gql_errors *errs);
void gqlgw_config_free(gqlgw_config *c);
const gqlgw_source *gqlgw_config_source(const gqlgw_config *c, const char *name, uint32_t n);

/* writers (render.c) */
void gqlgw_render_lua(const gqlgw_config *c, const char *src_name, gql_buf *out);
/* nginx configuration: whole block for http {} (locations == NULL), or the
 * http part and the locations to include in an existing server {} */
void gqlgw_render_nginx(const gqlgw_config *c, const char *lua_path, const char *src_name,
                        gql_buf *http, gql_buf *locations);

#endif
