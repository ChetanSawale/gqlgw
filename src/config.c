/* config.c - gqlgw.toml loading: defaults, ${VAR} interpolation, validation. */
#define _GNU_SOURCE
#include "config.h"

#include <arpa/inet.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "toml.h"

#ifndef GQLGW_DEFAULT_LIB
#define GQLGW_DEFAULT_LIB "/usr/local/lib/gqlgw/libgql.so"
#endif

typedef struct {
    gqlgw_config *cfg;
    const toml_doc *doc;
    gqlgw_cfg_mode mode;
    gqlgw_getenv_fn env;
    void *ud;
    const char *resolv_conf;
    gql_errors *errs;
    const char *section;            /* for messages: "gateway", "sources.users" */
} C;

static void err(C *c, const gql_loc *loc, const char *fmt, ...) GQL_PRINTF(3, 4);

static void err(C *c, const gql_loc *loc, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    errs_addv(c->errs, loc, loc && loc->line ? 1 : 0, fmt, ap);
    va_end(ap);
}

static char *dup(C *c, const char *s, size_t n)
{
    char *r = arena_strndup(&c->cfg->arena, s, n);
    return r ? r : (char *)"";
}

static char *dupz(C *c, const char *s)
{
    return dup(c, s, strlen(s));
}

static char *fmt(C *c, const char *f, ...) GQL_PRINTF(2, 3);

static char *fmt(C *c, const char *f, ...)
{
    char tmp[1024];
    va_list ap;
    va_start(ap, f);
    vsnprintf(tmp, sizeof tmp, f, ap);
    va_end(ap);
    return dupz(c, tmp);
}

/* ---- ${VAR} interpolation ---------------------------------------------- */

static const char *default_getenv(const char *name, void *ud)
{
    (void)ud;
    return getenv(name);
}

static int is_name_start(int ch) { return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_'; }
static int is_name_char(int ch) { return is_name_start(ch) || (ch >= '0' && ch <= '9'); }

/* Returns the interpolated string (arena), or NULL after reporting an error.
 * In CHECK mode, *deferred is set when the string uses ${VAR}. */
static char *interp(C *c, const toml_value *v, int *deferred)
{
    const char *s = v->s.p, *e = s + v->s.n;
    gql_buf b;
    char *r = NULL;
    *deferred = 0;
    if (memchr(s, 0, v->s.n)) {
        err(c, &v->loc, "NUL characters are not allowed in configuration values.");
        return NULL;
    }
    buf_init(&b);
    while (s < e) {
        const char *name, *q, *arg = NULL, *close;
        char op = 0, var[256];
        const char *val;
        if (*s == '$' && s + 2 < e && s[1] == '$' && s[2] == '{') {
            buf_put(&b, "${", 2);
            s += 3;
            continue;
        }
        if (!(*s == '$' && s + 1 < e && s[1] == '{')) {
            buf_putc(&b, *s++);
            continue;
        }
        name = q = s + 2;
        while (q < e && is_name_char((unsigned char)*q))
            q++;
        if (q == name || !is_name_start((unsigned char)*name) || q - name >= (long)sizeof var) {
            err(c, &v->loc, "Invalid environment variable reference \"%.*s\": expected ${NAME}, ${NAME:-default} or ${NAME:?message}.",
                (int)(e - s > 40 ? 40 : e - s), s);
            goto out;
        }
        if (q < e && *q == '}') {
            close = q;
        } else if (q + 1 < e && q[0] == ':' && (q[1] == '-' || q[1] == '?')) {
            op = q[1];
            arg = q + 2;
            close = memchr(arg, '}', (size_t)(e - arg));
            if (!close) {
                err(c, &v->loc, "Unterminated environment variable reference \"%.*s\".", (int)(e - s > 40 ? 40 : e - s), s);
                goto out;
            }
        } else {
            err(c, &v->loc, "Invalid environment variable reference \"%.*s\": expected ${NAME}, ${NAME:-default} or ${NAME:?message}.",
                (int)(e - s > 40 ? 40 : e - s), s);
            goto out;
        }
        memcpy(var, name, (size_t)(q - name));
        var[q - name] = 0;
        s = close + 1;
        if (c->mode == CFG_CHECK) {
            *deferred = 1;
            continue;
        }
        val = c->env(var, c->ud);
        if (op == '-' && (!val || !*val)) {
            buf_put(&b, arg, (size_t)(close - arg));
            continue;
        }
        if (op == '?' && (!val || !*val)) {
            if (close > arg)
                err(c, &v->loc, "%s: %.*s", var, (int)(close - arg), arg);
            else
                err(c, &v->loc, "Environment variable %s is required.", var);
            goto out;
        }
        if (!val) {
            err(c, &v->loc, "Environment variable %s is not set (use ${%s:-default} to give a default value).", var, var);
            goto out;
        }
        buf_puts(&b, val);
    }
    if (b.oom)
        err(c, &v->loc, "Out of memory.");
    else
        r = dup(c, b.p ? b.p : "", b.len);
out:
    buf_free(&b);
    return r;
}

/* ---- scalar parsers ---------------------------------------------------- */

static int parse_int(const char *s, int64_t *out)
{
    char *end;
    long long v;
    if (!*s)
        return -1;
    v = strtoll(s, &end, 10);
    if (*end)
        return -1;
    *out = v;
    return 0;
}

static int parse_size(const char *s, int64_t *out)
{
    int64_t v = 0, mul = 1;
    const char *p = s;
    if (*p < '0' || *p > '9')
        return -1;
    for (; *p >= '0' && *p <= '9'; p++) {
        if (v > (INT64_MAX - 9) / 10)
            return -1;
        v = v * 10 + (*p - '0');
    }
    switch (*p) {
    case 0: break;
    case 'k': case 'K': mul = 1024; p++; break;
    case 'm': case 'M': mul = 1024 * 1024; p++; break;
    case 'g': case 'G': mul = 1024 * 1024 * 1024; p++; break;
    default: return -1;
    }
    if (*p || v > INT64_MAX / mul)
        return -1;
    *out = v * mul;
    return 0;
}

/* nginx time: groups of digits with an optional unit (ms s m h d w M y) */
static int valid_duration(const char *s)
{
    if (!*s)
        return 0;
    while (*s) {
        if (*s < '0' || *s > '9')
            return 0;
        while (*s >= '0' && *s <= '9')
            s++;
        if (s[0] == 'm' && s[1] == 's')
            s += 2;
        else if (*s && strchr("smhdwMy", *s))
            s++;
    }
    return 1;
}

static int has_ctl(const char *s)
{
    for (; *s; s++)
        if (((unsigned char)*s < 0x20 && *s != '\t') || *s == 0x7f)
            return 1;
    return 0;
}

/* ---- validators -------------------------------------------------------- */

typedef int (*check_fn)(C *c, const char *s, const gql_loc *loc, const char *key);

static int chk_nonempty(C *c, const char *s, const gql_loc *loc, const char *key)
{
    if (!*s || has_ctl(s)) {
        err(c, loc, "[%s] %s must be a non-empty string without control characters.", c->section, key);
        return -1;
    }
    return 0;
}

/* a single nginx token: listen, server_name */
static int chk_word(C *c, const char *s, const gql_loc *loc, const char *key)
{
    const char *p;
    for (p = s; *p; p++)
        if ((unsigned char)*p <= 0x20 || *p == 0x7f || strchr(";{}\"'\\$#", *p))
            break;
    if (!*s || *p) {
        err(c, loc, "[%s] %s: invalid value \"%s\" (no spaces, quotes or ;{}$#).", c->section, key, s);
        return -1;
    }
    return 0;
}

/* nginx parameters separated by spaces: listen ("8443 ssl"), server_name */
static int chk_words(C *c, const char *s, const gql_loc *loc, const char *key)
{
    const char *p;
    for (p = s; *p; p++)
        if ((*p != ' ' && (unsigned char)*p <= 0x20) || *p == 0x7f || strchr(";{}\"'\\$#", *p))
            break;
    if (!*s || *s == ' ' || *p) {
        err(c, loc, "[%s] %s: invalid value \"%s\" (no quotes, tabs or ;{}$#).", c->section, key, s);
        return -1;
    }
    return 0;
}

/* nginx location path */
static int chk_location(C *c, const char *s, const gql_loc *loc, const char *key)
{
    const char *p;
    for (p = s; *p; p++)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
              strchr("/-._~%!*'()+,=:@", *p)))
            break;
    if (s[0] != '/' || *p) {
        err(c, loc, "[%s] %s: \"%s\" must be a URI path starting with '/' (letters, digits, /-._~%%!*'()+,=:@).",
            c->section, key, s);
        return -1;
    }
    return 0;
}

/* file paths: written quoted in nginx and Lua; '$' would be an nginx variable */
static int chk_file(C *c, const char *s, const gql_loc *loc, const char *key)
{
    if (!*s || has_ctl(s) || strchr(s, '$')) {
        err(c, loc, "[%s] %s: invalid path \"%s\" (empty, control characters or '$').", c->section, key, s);
        return -1;
    }
    return 0;
}

static int chk_header_name(C *c, const char *s, const gql_loc *loc, const char *key)
{
    const char *p;
    for (p = s; *p; p++)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-'))
            break;
    if (!*s || *p) {
        err(c, loc, "[%s] %s: invalid header name \"%s\" (letters, digits and '-').", c->section, key, s);
        return -1;
    }
    return 0;
}

/* headers managed by gqlgw itself */
static int chk_forward_header(C *c, const char *s, const gql_loc *loc, const char *key)
{
    static const char *const RESERVED[] = {
        "host", "connection", "content-length", "transfer-encoding", "accept-encoding", "content-type",
        "te", "keep-alive", "upgrade", "expect", "proxy-connection"
    };
    size_t i;
    if (chk_header_name(c, s, loc, key))
        return -1;
    for (i = 0; i < sizeof RESERVED / sizeof RESERVED[0]; i++)
        if (!strcasecmp(s, RESERVED[i])) {
            err(c, loc, "[%s] %s: the %s header is managed by gqlgw%s.", c->section, key, s,
                i == 0 ? " (see host_header)" : "");
            return -1;
        }
    return 0;
}

static int chk_header_value(C *c, const char *s, const gql_loc *loc, const char *key)
{
    if (has_ctl(s)) {
        err(c, loc, "[%s] %s: header values cannot contain control characters.", c->section, key);
        return -1;
    }
    return 0;
}

static int chk_cidr(C *c, const char *s, const gql_loc *loc, const char *key)
{
    char addr[64];
    unsigned char buf[16];
    const char *slash = strchr(s, '/');
    size_t n = slash ? (size_t)(slash - s) : strlen(s);
    int v6 = memchr(s, ':', n) != NULL, max = v6 ? 128 : 32;
    if (!strcmp(s, "all"))
        return 0;
    if (n && n < sizeof addr) {
        memcpy(addr, s, n);
        addr[n] = 0;
        if (inet_pton(v6 ? AF_INET6 : AF_INET, addr, buf) == 1) {
            int64_t len;
            if (!slash || (parse_int(slash + 1, &len) == 0 && len >= 0 && len <= max))
                return 0;
        }
    }
    err(c, loc, "[%s] %s: \"%s\" is not an IP address or CIDR range.", c->section, key, s);
    return -1;
}

static int chk_resolver(C *c, const char *s, const gql_loc *loc, const char *key)
{
    if (!strcmp(s, "auto"))
        return 0;
    return chk_word(c, s, loc, key);
}

/* memcached key prefix */
static int chk_prefix(C *c, const char *s, const gql_loc *loc, const char *key)
{
    const char *p;
    for (p = s; *p; p++)
        if ((unsigned char)*p <= 0x20 || *p == 0x7f)
            break;
    if (*p || strlen(s) > 32) {
        err(c, loc, "[%s] %s: at most 32 characters, without spaces or control characters.", c->section, key);
        return -1;
    }
    return 0;
}

/* ---- field descriptors ------------------------------------------------- */

enum { T_BOOL, T_INT, T_NUM, T_STR, T_SIZE, T_DUR, T_LISTEN, T_LIST, T_MAP, T_NUMLIST, T_SUB };

typedef struct {
    const char *key;
    int type;
    size_t off;
    double min, max;
    const char *def;
    check_fn check;
} fdesc;

#define G(m) offsetof(gqlgw_config, m)
#define S(m) offsetof(gqlgw_source, m)
#define BIG 1e15

static const fdesc GATEWAY[] = {
    { "listen", T_LISTEN, G(gw.listen), 0, 0, "8080", chk_words },
    { "server_name", T_STR, G(gw.server_name), 0, 0, "_", chk_words },
    { "path", T_STR, G(gw.path), 0, 0, "/graphql", chk_location },
    { "schema", T_STR, G(gw.schema), 0, 0, "schema.gqlb", chk_file },
    { "routes", T_STR, G(gw.routes), 0, 0, NULL, chk_file },
    { "lib", T_STR, G(gw.lib), 0, 0, GQLGW_DEFAULT_LIB, chk_file },
    { "access_log", T_STR, G(gw.access_log), 0, 0, NULL, chk_file },
    { "introspection", T_BOOL, G(gw.introspection), 0, 0, "true", NULL },
    { "strict_path_args", T_BOOL, G(gw.strict_path_args), 0, 0, "true", NULL },
    { "debug_headers", T_BOOL, G(gw.debug_headers), 0, 0, "false", NULL },
    { "max_body", T_SIZE, G(gw.max_body), 1, 1 << 30, "1m", NULL },
};

static const fdesc LIMITS[] = {
    { "query_bytes", T_INT, G(lim.query_bytes), 1, 1 << 26, "65536", NULL },
    { "tokens", T_INT, G(lim.tokens), 1, 1 << 24, "20000", NULL },
    { "depth", T_INT, G(lim.depth), 1, 1000, "32", NULL },
    { "errors", T_INT, G(lim.errors), 1, 100000, "100", NULL },
    { "plan_fields", T_INT, G(lim.plan_fields), 1, 1 << 24, "20000", NULL },
    { "memory", T_SIZE, G(lim.memory), 1 << 16, 1u << 31, "16m", NULL },
    { "rounds", T_INT, G(lim.rounds), 1, 1000, "16", NULL },
    { "fetches", T_INT, G(lim.fetches), 1, 1000000, "1000", NULL },
    { "concurrency", T_INT, G(lim.concurrency), 1, 10000, "64", NULL },
    { "response_bytes", T_SIZE, G(lim.response_bytes), 1, 1u << 31, "16m", NULL },
};

static const fdesc CACHE[] = {
    { "shm_size", T_SIZE, G(cache.shm_size), 0, BIG, "64m", NULL },
    { "l1_size", T_INT, G(cache.l1_size), 0, 10000000, "10000", NULL },
    { "l1_ttl", T_NUM, G(cache.l1_ttl), 0, 86400, "1", NULL },
    { "l2_ttl", T_NUM, G(cache.l2_ttl), 0, 86400 * 30, "2", NULL },
    { "gen_ttl", T_NUM, G(cache.gen_ttl), 0, 3600, "1", NULL },
    { "negative_ttl", T_NUM, G(cache.negative_ttl), 0, 86400, "5", NULL },
    { "down_ttl", T_NUM, G(cache.down_ttl), 0, 3600, "5", NULL },
    { "private_header", T_STR, G(cache.private_header), 0, 0, "Authorization", chk_header_name },
    { "prefix", T_STR, G(cache.prefix), 0, 0, "gqlgw:", chk_prefix },
    { "memcached", T_SUB, 0, 0, 0, NULL, NULL },
};

static const fdesc MEMCACHED[] = {
    { "host", T_STR, G(cache.host), 0, 0, "127.0.0.1", chk_word },
    { "port", T_INT, G(cache.port), 1, 65535, "11211", NULL },
    { "socket", T_STR, G(cache.socket), 0, 0, NULL, chk_file },
    { "timeout", T_INT, G(cache.timeout), 1, 60000, "100", NULL },
    { "pool_size", T_INT, G(cache.pool_size), 1, 10000, "64", NULL },
    { "keepalive", T_INT, G(cache.keepalive), 0, 3600000, "60000", NULL },
};

static const fdesc NGINX[] = {
    { "init", T_BOOL, G(ngx.init), 0, 0, "true", NULL },
    { "resolver", T_LIST, G(ngx.resolver), 0, 0, "auto", chk_resolver },
    { "resolver_valid", T_DUR, G(ngx.resolver_valid), 0, 0, "10s", NULL },
    { "resolver_ipv6", T_BOOL, G(ngx.resolver_ipv6), 0, 0, "true", NULL },
};

static const fdesc METRICS[] = {
    { "enabled", T_BOOL, G(metrics.enabled), 0, 0, "true", NULL },
    { "port", T_LISTEN, G(metrics.listen), 0, 0, "8191", chk_words },
    { "path", T_STR, G(metrics.path), 0, 0, "/gqlgw_metrics", chk_location },
    { "allow", T_LIST, G(metrics.allow), 0, 0, NULL, chk_cidr },
    { "exclude_operations", T_LIST, G(metrics.exclude_operations), 0, 0, NULL, chk_nonempty },
    { "shm_size", T_SIZE, G(metrics.shm_size), 1 << 16, 1u << 31, "8m", NULL },
    { "buckets", T_NUMLIST, G(metrics.buckets), 0, BIG, NULL, NULL },
};

static const fdesc PURGE[] = {
    { "enabled", T_BOOL, G(purge.enabled), 0, 0, "true", NULL },
    { "path", T_STR, G(purge.path), 0, 0, "/gqlgw_purge", chk_location },
};

static const fdesc SOURCE[] = {
    { "url", T_STR, S(url), 0, 0, NULL, chk_nonempty },
    { "connect_timeout", T_DUR, S(connect_timeout), 0, 0, "1s", NULL },
    { "send_timeout", T_DUR, S(send_timeout), 0, 0, "5s", NULL },
    { "read_timeout", T_DUR, S(read_timeout), 0, 0, "5s", NULL },
    { "keepalive", T_INT, S(keepalive), 0, 100000, "64", NULL },
    { "keepalive_timeout", T_DUR, S(keepalive_timeout), 0, 0, "60s", NULL },
    { "forward_headers", T_LIST, S(forward_headers), 0, 0, NULL, chk_forward_header },
    { "drop_headers", T_LIST, S(drop_headers), 0, 0, NULL, chk_forward_header },
    { "set_headers", T_MAP, S(set_headers), 0, 0, NULL, chk_header_value },
    { "tls_verify", T_BOOL, S(tls_verify), 0, 0, "true", NULL },
    { "ca", T_STR, S(ca), 0, 0, "/etc/ssl/certs/ca-certificates.crt", chk_file },
    { "tls_name", T_STR, S(tls_name), 0, 0, NULL, chk_word },
    { "host_header", T_STR, S(host_header), 0, 0, NULL, chk_word },
};

#define NELEMS(a) (sizeof(a) / sizeof((a)[0]))

static const double DEFAULT_BUCKETS[] = { 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1, 2.5, 5, 10 };

/* ---- field setters ----------------------------------------------------- */

static void set_scalar_text(C *c, const fdesc *d, char *base, const char *s, const gql_loc *loc)
{
    void *dst = base + d->off;
    int64_t i;
    double f;
    char *end;
    switch (d->type) {
    case T_BOOL:
        if (!strcmp(s, "true") || !strcmp(s, "false"))
            *(int *)dst = s[0] == 't';
        else
            err(c, loc, "[%s] %s must be true or false.", c->section, d->key);
        return;
    case T_INT:
        if (parse_int(s, &i) || i < d->min || i > d->max)
            err(c, loc, "[%s] %s must be an integer between %.0f and %.0f.", c->section, d->key, d->min, d->max);
        else
            *(int64_t *)dst = i;
        return;
    case T_NUM:
        f = strtod(s, &end);
        if (!*s || *end || !(f >= d->min && f <= d->max))
            err(c, loc, "[%s] %s must be a number between %g and %g.", c->section, d->key, d->min, d->max);
        else
            *(double *)dst = f;
        return;
    case T_SIZE:
        if (parse_size(s, &i) || i < d->min || i > d->max) {
            err(c, loc, "[%s] %s: invalid size \"%s\" (bytes, or a number followed by k, m or g).", c->section, d->key, s);
        } else {
            ((cfg_size *)dst)->text = dupz(c, s);
            ((cfg_size *)dst)->bytes = i;
        }
        return;
    case T_DUR:
        if (!valid_duration(s))
            err(c, loc, "[%s] %s: invalid duration \"%s\" (for example 500ms, 3s, 1m).", c->section, d->key, s);
        else
            *(char **)dst = dupz(c, s);
        return;
    case T_STR:
    case T_LISTEN:
        if (!d->check || d->check(c, s, loc, d->key) == 0)
            *(char **)dst = dupz(c, s);
        return;
    case T_LIST:
        if (!d->check || d->check(c, s, loc, d->key) == 0) {
            cfg_list *l = dst;
            l->v = arena_alloc(&c->cfg->arena, sizeof(char *));
            if (l->v) {
                l->v[0] = dupz(c, s);
                l->n = 1;
            }
        }
        return;
    }
}

static void set_default(C *c, const fdesc *d, char *base)
{
    if (d->def)
        set_scalar_text(c, d, base, d->def, NULL);
}

static const char *type_label(int t)
{
    switch (t) {
    case T_BOOL: return "a boolean";
    case T_INT: return "an integer";
    case T_NUM: return "a number";
    case T_SIZE: return "a size (integer or string such as \"16m\")";
    case T_DUR: return "a duration (string such as \"3s\")";
    case T_LISTEN: return "a port (integer) or an address (string)";
    case T_LIST: return "an array of strings";
    case T_MAP: return "a table of strings";
    case T_NUMLIST: return "an array of numbers";
    default: return "a string";
    }
}

static void set_field(C *c, const fdesc *d, char *base, const toml_value *v)
{
    char num[64];
    int deferred;
    char *s;
    /* the default stays when the value is invalid (an error is reported), so
     * that the configuration is always complete */
    set_default(c, d, base);
    if (!v)
        return;
    switch (v->kind) {
    case TOML_STRING:
        if (d->type == T_MAP || d->type == T_NUMLIST)
            break;
        if (!(s = interp(c, v, &deferred)))
            return;
        if (!deferred)
            set_scalar_text(c, d, base, s, &v->loc);
        return;
    case TOML_BOOL:
        if (d->type != T_BOOL)
            break;
        *(int *)(base + d->off) = (int)v->i;
        return;
    case TOML_INTEGER:
        if (d->type == T_DUR) {
            if (v->i < 0)
                break;
            snprintf(num, sizeof num, "%llds", (long long)v->i);
        } else if (d->type == T_INT || d->type == T_NUM || d->type == T_SIZE || d->type == T_LISTEN) {
            if (d->type == T_LISTEN && (v->i < 1 || v->i > 65535)) {
                err(c, &v->loc, "[%s] %s: port must be between 1 and 65535.", c->section, d->key);
                return;
            }
            snprintf(num, sizeof num, "%lld", (long long)v->i);
        } else {
            break;
        }
        set_scalar_text(c, d, base, num, &v->loc);
        return;
    case TOML_FLOAT:
        if (d->type != T_NUM)
            break;
        if (!(v->f >= d->min && v->f <= d->max))
            err(c, &v->loc, "[%s] %s must be a number between %g and %g.", c->section, d->key, d->min, d->max);
        else
            *(double *)(base + d->off) = v->f;
        return;
    case TOML_ARRAY:
        if (d->type == T_LIST) {
            cfg_list *l = (cfg_list *)(base + d->off);
            const toml_value *it;
            l->n = 0;
            l->v = arena_alloc(&c->cfg->arena, sizeof(char *) * (v->count ? v->count : 1));
            if (!l->v)
                return;
            for (it = v->first; it; it = it->next) {
                if (it->kind != TOML_STRING) {
                    err(c, &it->loc, "[%s] %s must be %s.", c->section, d->key, type_label(d->type));
                    continue;
                }
                if (!(s = interp(c, it, &deferred)) || deferred)
                    continue;
                if (!d->check || d->check(c, s, &it->loc, d->key) == 0)
                    l->v[l->n++] = s;
            }
            return;
        }
        if (d->type == T_NUMLIST) {
            double *out = arena_alloc(&c->cfg->arena, sizeof(double) * (v->count ? v->count : 1));
            const toml_value *it;
            uint32_t n = 0;
            if (!out)
                return;
            for (it = v->first; it; it = it->next) {
                double f = it->kind == TOML_INTEGER ? (double)it->i : it->kind == TOML_FLOAT ? it->f : NAN;
                if (!(f > 0 && f < d->max) || (n && f <= out[n - 1])) {
                    err(c, &it->loc, "[%s] %s must be increasing positive numbers.", c->section, d->key);
                    return;
                }
                out[n++] = f;
            }
            if (!n) {
                err(c, &v->loc, "[%s] %s cannot be empty.", c->section, d->key);
                return;
            }
            *(double **)(base + d->off) = out;
            c->cfg->metrics.nbuckets = n;
            return;
        }
        break;
    case TOML_TABLE:
        if (d->type == T_MAP) {
            cfg_map *m = (cfg_map *)(base + d->off);
            const toml_value *it;
            m->n = 0;
            m->k = arena_alloc(&c->cfg->arena, sizeof(char *) * (v->count ? v->count : 1));
            m->v = arena_alloc(&c->cfg->arena, sizeof(char *) * (v->count ? v->count : 1));
            if (!m->k || !m->v)
                return;
            for (it = v->first; it; it = it->next) {
                char *name = dup(c, it->key.p, it->key.n);
                if (it->kind != TOML_STRING) {
                    err(c, &it->loc, "[%s] %s.%s must be a string.", c->section, d->key, name);
                    continue;
                }
                if (chk_forward_header(c, name, &it->key_loc, d->key))
                    continue;
                if (!(s = interp(c, it, &deferred)) || deferred)
                    continue;
                if (d->check && d->check(c, s, &it->loc, d->key))
                    continue;
                m->k[m->n] = name;
                m->v[m->n++] = s;
            }
            return;
        }
        break;
    default:
        break;
    }
    err(c, &v->loc, "[%s] %s must be %s, not %s.", c->section, d->key, type_label(d->type),
        v->kind == TOML_TABLE ? "a table" : v->kind == TOML_ARRAY ? "an array" : toml_kind_name(v->kind));
}

/* applies the descriptors of one section; tbl may be NULL (all defaults) */
static void apply(C *c, const toml_value *tbl, const fdesc *ds, size_t nd, char *base)
{
    size_t i;
    const toml_value *m;
    if (tbl) {
        for (m = tbl->first; m; m = m->next) {
            for (i = 0; i < nd; i++)
                if (strlen(ds[i].key) == m->key.n && !memcmp(ds[i].key, m->key.p, m->key.n))
                    break;
            if (i == nd)
                err(c, &m->key_loc, "Unknown key \"%.*s\" in [%s].", (int)m->key.n, m->key.p, c->section);
        }
    }
    for (i = 0; i < nd; i++)
        if (ds[i].type != T_SUB)
            set_field(c, &ds[i], base, tbl ? toml_get(c->doc, tbl, ds[i].key) : NULL);
}

static const toml_value *section(C *c, const char *name)
{
    const toml_value *t = toml_get(c->doc, c->doc->root, name);
    if (t && t->kind != TOML_TABLE) {
        err(c, &t->key_loc, "\"%s\" must be a table ([%s]).", name, name);
        return NULL;
    }
    return t;
}

/* ---- paths and URLs ---------------------------------------------------- */

static char *resolve_path(C *c, char *p)
{
    if (!p || p[0] == '/' || !c->cfg->dir)
        return p;
    return fmt(c, "%s/%s", c->cfg->dir, p);
}

static void parse_url(C *c, gqlgw_source *s, const gql_loc *loc)
{
    const char *u = s->url, *h, *he, *p;
    char host[256];
    unsigned char ab[16];
    int https;
    if (!strncasecmp(u, "http://", 7)) {
        https = 0;
        h = u + 7;
    } else if (!strncasecmp(u, "https://", 8)) {
        https = 1;
        h = u + 8;
    } else {
        err(c, loc, "[%s] url \"%s\" must start with http:// or https://.", c->section, u);
        return;
    }
    s->scheme = https ? "https" : "http";
    s->port = https ? 443 : 80;
    he = h + strcspn(h, "/?#");
    if (*he == '?' || *he == '#' || memchr(h, '@', (size_t)(he - h))) {
        err(c, loc, "[%s] url \"%s\": user info, query strings and fragments are not supported.", c->section, u);
        return;
    }
    p = he;
    if (*h == '[') {
        const char *rb = memchr(h, ']', (size_t)(he - h));
        if (!rb || rb - h - 1 >= (long)sizeof host) {
            err(c, loc, "[%s] url \"%s\": invalid IPv6 address.", c->section, u);
            return;
        }
        memcpy(host, h + 1, (size_t)(rb - h - 1));
        host[rb - h - 1] = 0;
        if (inet_pton(AF_INET6, host, ab) != 1) {
            err(c, loc, "[%s] url \"%s\": invalid IPv6 address.", c->section, u);
            return;
        }
        s->host = dup(c, h, (size_t)(rb - h + 1));
        s->host_is_ip = 1;
        h = rb + 1;
    } else {
        const char *colon = memchr(h, ':', (size_t)(he - h)), *hend = colon ? colon : he, *q;
        if (hend == h || hend - h >= (long)sizeof host) {
            err(c, loc, "[%s] url \"%s\": missing host.", c->section, u);
            return;
        }
        for (q = h; q < hend; q++)
            if (!((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z') || (*q >= '0' && *q <= '9') ||
                  *q == '.' || *q == '-' || *q == '_')) {
                err(c, loc, "[%s] url \"%s\": invalid host name.", c->section, u);
                return;
            }
        memcpy(host, h, (size_t)(hend - h));
        host[hend - h] = 0;
        s->host = dupz(c, host);
        s->host_is_ip = inet_pton(AF_INET, host, ab) == 1;
        h = hend;
    }
    if (h < he) {
        int64_t port;
        char pb[8];
        if (*h != ':' || he - h - 1 < 1 || he - h - 1 > 5) {
            err(c, loc, "[%s] url \"%s\": invalid port.", c->section, u);
            return;
        }
        memcpy(pb, h + 1, (size_t)(he - h - 1));
        pb[he - h - 1] = 0;
        if (parse_int(pb, &port) || port < 1 || port > 65535) {
            err(c, loc, "[%s] url \"%s\": invalid port.", c->section, u);
            return;
        }
        s->port = (int)port;
    }
    {
        const char *q;
        size_t n = strlen(p);
        for (q = p; *q; q++)
            if (!((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z') || (*q >= '0' && *q <= '9') ||
                  strchr("/-._~%!*'()+,=:@&;", *q))) {
                err(c, loc, "[%s] url \"%s\": invalid character '%c' in the path prefix.", c->section, u, *q);
                return;
            }
        while (n && p[n - 1] == '/')
            n--;
        s->prefix = dup(c, p, n);
    }
}

static void load_source(C *c, const toml_value *t, gqlgw_source *s)
{
    const toml_value *url = toml_get(c->doc, t, "url");
    apply(c, t, SOURCE, NELEMS(SOURCE), (char *)s);
    if (!url) {
        err(c, &t->loc, "[%s] url is required.", c->section);
        return;
    }
    if (url->kind == TOML_STRING && c->mode == CFG_CHECK && !s->url) {
        s->deferred = 1;
        return;
    }
    if (!s->url)
        return;
    parse_url(c, s, &url->loc);
    if (!s->host)
        return;
    if (s->forward_headers.n && s->drop_headers.n) {
        const toml_value *dh = toml_get(c->doc, t, "drop_headers");
        err(c, dh ? &dh->loc : &t->loc, "[%s] forward_headers (allowlist) and drop_headers (denylist) cannot be used together.",
            c->section);
    }
    if (toml_get(c->doc, t, "drop_headers")) {
        s->headers_mode = HDR_DENY;
    } else {
        s->headers_mode = HDR_ALLOW;
        if (!toml_get(c->doc, t, "forward_headers")) {
            s->forward_headers.v = arena_alloc(&c->cfg->arena, sizeof(char *));
            if (s->forward_headers.v) {
                s->forward_headers.v[0] = "Authorization";
                s->forward_headers.n = 1;
            }
        }
    }
    if (!s->host_header)
        s->host_header = (s->port == (strcmp(s->scheme, "https") ? 80 : 443)) ? s->host : fmt(c, "%s:%d", s->host, s->port);
    if (!s->tls_name)
        s->tls_name = s->host;
    s->ca = resolve_path(c, s->ca);
}

/* resolver = "auto": nameservers of resolv.conf */
static void auto_resolver(C *c)
{
    cfg_list *l = &c->cfg->ngx.resolver;
    FILE *f = fopen(c->resolv_conf ? c->resolv_conf : "/etc/resolv.conf", "r");
    char line[512];
    l->n = 0;
    if (!f)
        return;
    l->v = arena_alloc(&c->cfg->arena, sizeof(char *) * 8);
    while (l->v && l->n < 8 && fgets(line, sizeof line, f)) {
        char addr[256];
        unsigned char ab[16];
        if (sscanf(line, " nameserver %255s", addr) != 1)
            continue;
        if (inet_pton(AF_INET, addr, ab) == 1)
            l->v[l->n++] = dupz(c, addr);
        else if (inet_pton(AF_INET6, addr, ab) == 1)
            l->v[l->n++] = fmt(c, "[%s]", addr);
        /* scoped IPv6 addresses (fe80::1%eth0) are not supported by nginx */
    }
    fclose(f);
}

/* ---- entry points ------------------------------------------------------ */

static char *dirname_abs(C *c, const char *path)
{
    const char *slash = strrchr(path, '/');
    char *dir = slash ? dup(c, path, (size_t)(slash - path)) : dupz(c, ".");
    char *real;
    if (!*dir)
        dir = dupz(c, "/");
    real = realpath(dir, NULL);
    if (real) {
        dir = dupz(c, real);
        free(real);
    }
    return dir;
}

int gqlgw_config_load(gqlgw_config *cfg, const char *path, const char *src, size_t n, gqlgw_cfg_mode mode,
                      gqlgw_getenv_fn env, void *ud, const char *resolv_conf, gql_errors *errs)
{
    static const char *const TOP[] = { "gateway", "limits", "cache", "nginx", "metrics", "purge", "sources" };
    toml_doc doc;
    C c;
    const toml_value *t, *m;
    int before = errs_count(errs);
    size_t i;

    memset(cfg, 0, sizeof *cfg);
    arena_init(&cfg->arena, 0);
    memset(&c, 0, sizeof c);
    c.cfg = cfg;
    c.doc = &doc;
    c.mode = mode;
    c.env = env ? env : default_getenv;
    c.ud = ud;
    c.resolv_conf = resolv_conf;
    c.errs = errs;
    cfg->file = dupz(&c, path);
    cfg->dir = dirname_abs(&c, path);

    if (toml_parse(&doc, src, n)) {
        errs_add(errs, &doc.err_loc, 1, "%s", doc.err);
        toml_free(&doc);
        return -1;
    }
    for (m = doc.root->first; m; m = m->next) {
        for (i = 0; i < NELEMS(TOP); i++)
            if (strlen(TOP[i]) == m->key.n && !memcmp(TOP[i], m->key.p, m->key.n))
                break;
        if (i == NELEMS(TOP))
            err(&c, &m->key_loc, "Unknown key \"%.*s\": expected [gateway], [limits], [cache], [nginx], [metrics], [purge] or [sources.<name>].",
                (int)m->key.n, m->key.p);
    }

    c.section = "gateway";
    apply(&c, section(&c, "gateway"), GATEWAY, NELEMS(GATEWAY), (char *)cfg);
    c.section = "limits";
    apply(&c, section(&c, "limits"), LIMITS, NELEMS(LIMITS), (char *)cfg);
    c.section = "cache";
    t = section(&c, "cache");
    apply(&c, t, CACHE, NELEMS(CACHE), (char *)cfg);
    if ((m = toml_get(&doc, t, "memcached"))) {
        c.section = "cache.memcached";
        if (m->kind != TOML_TABLE) {
            err(&c, &m->key_loc, "\"memcached\" must be a table ([cache.memcached]).");
        } else {
            cfg->cache.memcached = 1;
            apply(&c, m, MEMCACHED, NELEMS(MEMCACHED), (char *)cfg);
            if (toml_get(&doc, m, "host") && toml_get(&doc, m, "socket"))
                err(&c, &m->loc, "[cache.memcached] host and socket cannot be used together.");
            cfg->cache.socket = resolve_path(&c, cfg->cache.socket);
        }
    }
    c.section = "nginx";
    apply(&c, section(&c, "nginx"), NGINX, NELEMS(NGINX), (char *)cfg);
    c.section = "metrics";
    apply(&c, section(&c, "metrics"), METRICS, NELEMS(METRICS), (char *)cfg);
    if (!cfg->metrics.buckets) {
        cfg->metrics.buckets = (double *)DEFAULT_BUCKETS;
        cfg->metrics.nbuckets = NELEMS(DEFAULT_BUCKETS);
    }
    c.section = "purge";
    apply(&c, section(&c, "purge"), PURGE, NELEMS(PURGE), (char *)cfg);

    c.section = "sources";
    if ((t = section(&c, "sources"))) {
        cfg->sources = arena_alloc(&cfg->arena, sizeof(gqlgw_source) * (t->count ? t->count : 1));
        for (m = t->first; cfg->sources && m; m = m->next) {
            gqlgw_source *s = &cfg->sources[cfg->nsources];
            uint32_t k;
            for (k = 0; k < m->key.n; k++)
                if (!((m->key.p[k] >= 'A' && m->key.p[k] <= 'Z') || (m->key.p[k] >= 'a' && m->key.p[k] <= 'z') ||
                      (m->key.p[k] >= '0' && m->key.p[k] <= '9') || m->key.p[k] == '_' || m->key.p[k] == '-'))
                    break;
            if (!m->key.n || k < m->key.n) {
                err(&c, &m->key_loc, "Invalid source name \"%.*s\": it must match [A-Za-z0-9_-]+ like @fetch(source:).",
                    (int)m->key.n, m->key.p);
                continue;
            }
            if (m->kind != TOML_TABLE) {
                err(&c, &m->key_loc, "Source \"%.*s\" must be a table ([sources.%.*s]).", (int)m->key.n, m->key.p,
                    (int)m->key.n, m->key.p);
                continue;
            }
            s->name = dup(&c, m->key.p, m->key.n);
            s->loc = m->key_loc;
            c.section = fmt(&c, "sources.%s", s->name);
            load_source(&c, m, s);
            cfg->nsources++;
        }
    }

    cfg->gw.schema = resolve_path(&c, cfg->gw.schema);
    cfg->gw.routes = resolve_path(&c, cfg->gw.routes);
    cfg->gw.lib = resolve_path(&c, cfg->gw.lib);
    if (mode == CFG_RENDER && cfg->ngx.resolver.n == 1 && !strcmp(cfg->ngx.resolver.v[0], "auto"))
        auto_resolver(&c);
    if (mode == CFG_RENDER) {
        for (i = 0; i < cfg->nsources; i++) {
            if (cfg->sources[i].host && !cfg->sources[i].host_is_ip && !cfg->ngx.resolver.n) {
                err(&c, &cfg->sources[i].loc,
                    "Source \"%s\" uses a host name but no DNS resolver is known: set [nginx] resolver.", cfg->sources[i].name);
                break;
            }
        }
        if (cfg->cache.memcached && !cfg->cache.socket && !cfg->ngx.resolver.n) {
            unsigned char ab[16];
            if (inet_pton(AF_INET, cfg->cache.host, ab) != 1 && inet_pton(AF_INET6, cfg->cache.host, ab) != 1)
                err(&c, NULL, "[cache.memcached] host \"%s\" is a name but no DNS resolver is known: set [nginx] resolver.",
                    cfg->cache.host);
        }
        if (cfg->metrics.enabled || cfg->purge.enabled) {
            if (!strcmp(cfg->metrics.listen, cfg->gw.listen))
                err(&c, NULL, "[metrics] port must differ from [gateway] listen.");
        }
    }
    if (cfg->arena.oom)
        err(&c, NULL, "Out of memory.");
    toml_free(&doc);
    return errs_count(errs) > before ? -1 : 0;
}

void gqlgw_config_free(gqlgw_config *c)
{
    arena_free(&c->arena);
}

const gqlgw_source *gqlgw_config_source(const gqlgw_config *c, const char *name, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < c->nsources; i++)
        if (strlen(c->sources[i].name) == n && !memcmp(c->sources[i].name, name, n))
            return &c->sources[i];
    return NULL;
}
