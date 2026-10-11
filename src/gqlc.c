/* gqlc - gqlgw schema compiler, configuration renderer and debugging tool.
 *
 *   gqlc compile [-c gqlgw.toml] [-o out.gqlb] [-r out.routes.lua] schema.graphqls
 *   gqlc render  -c gqlgw.toml [-o dir] [--locations-only]
 *   gqlc check   schema.gqlb query.graphql
 *   gqlc plan    schema.gqlb query.graphql [variables.json] [operationName]
 *   gqlc introspect schema.gqlb
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "gql.h"
#include "gqlb.h"
#include "sdl.h"

static char *read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    char *p = NULL;
    long len;
    if (!f) {
        fprintf(stderr, "gqlc: %s: %s\n", path, strerror(errno));
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) == 0 && (len = ftell(f)) >= 0 && fseek(f, 0, SEEK_SET) == 0) {
        p = malloc((size_t)len + 1);
        if (p && fread(p, 1, (size_t)len, f) == (size_t)len) {
            p[len] = 0;
            *n = (size_t)len;
        } else {
            free(p);
            p = NULL;
        }
    }
    fclose(f);
    if (!p)
        fprintf(stderr, "gqlc: %s: read error\n", path);
    return p;
}

static char *replace_ext(const char *path, const char *ext)
{
    size_t n = strlen(path), e = strlen(ext);
    const char *dot = strrchr(path, '.'), *slash = strrchr(path, '/');
    char *out;
    if (dot && (!slash || dot > slash))
        n = (size_t)(dot - path);
    out = malloc(n + e + 1);
    if (!out)
        return NULL;
    memcpy(out, path, n);
    memcpy(out + n, ext, e + 1);
    return out;
}

/* loads gqlgw.toml; prints the errors */
static int load_config(gqlgw_config *cfg, const char *path, gqlgw_cfg_mode mode, const char *resolv_conf)
{
    size_t n;
    char *src = read_file(path, &n);
    gql_errors errs;
    gql_buf b;
    int rc;
    if (!src)
        return -1;
    errs_init(&errs, 100);
    buf_init(&b);
    rc = gqlgw_config_load(cfg, path, src, n, mode, NULL, NULL, resolv_conf, &errs);
    errs_to_text(&errs, path, &b);
    if (b.len)
        fprintf(stderr, "%s", buf_cstr(&b));
    buf_free(&b);
    errs_free(&errs);
    free(src);
    return rc;
}

/* every @fetch source must be configured in [sources] */
static int check_sources(const gql_compiled *c, const gqlgw_config *cfg, const char *cfg_path, int quiet)
{
    const gql_schema *s = gql_compiled_schema(c);
    uint32_t i, k, n = gql_compiled_nroutes(c);
    int rc = 0;
    for (i = 0; i < n; i++) {
        const gql_route *r = gql_compiled_route(c, i);
        const gql_field_rec *f = &s->fields[r->field];
        for (k = 0; k < i; k++)
            if (gql_compiled_route(c, k)->source.n == r->source.n &&
                !memcmp(gql_compiled_route(c, k)->source.p, r->source.p, r->source.n))
                break;
        if (k < i || gqlgw_config_source(cfg, r->source.p, r->source.n))
            continue;
        fprintf(stderr, "%s: @fetch(source: \"%.*s\") on %s.%s: no [sources.%.*s] in the configuration\n", cfg_path,
                (int)r->source.n, r->source.p, GQL_S(s, s->types[f->owner].name), GQL_S(s, f->name),
                (int)r->source.n, r->source.p);
        rc = 1;
    }
    for (k = 0; k < cfg->nsources && !quiet; k++) {
        for (i = 0; i < n; i++) {
            const gql_route *r = gql_compiled_route(c, i);
            if (strlen(cfg->sources[k].name) == r->source.n && !memcmp(cfg->sources[k].name, r->source.p, r->source.n))
                break;
        }
        if (i == n)
            fprintf(stderr, "%s:%u:%u: warning: source \"%s\" is not used by the schema\n", cfg_path,
                    cfg->sources[k].loc.line, cfg->sources[k].loc.col, cfg->sources[k].name);
    }
    return rc;
}

static int cmd_compile(int argc, char **argv)
{
    const char *in = NULL, *out = NULL, *routes = NULL, *cfg_path = NULL;
    gqlgw_config cfg;
    int have_cfg = 0;
    int quiet = 0;
    char *out_d = NULL, *routes_d = NULL, *src;
    size_t n;
    gql_errors errs, warns;
    gql_compiled *c;
    gql_buf b;
    char err[512];
    int i, rc = 1;

    for (i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc)
            out = argv[++i];
        else if (!strcmp(argv[i], "-r") && i + 1 < argc)
            routes = argv[++i];
        else if (!strcmp(argv[i], "-c") && i + 1 < argc)
            cfg_path = argv[++i];
        else if (!strcmp(argv[i], "-q"))
            quiet = 1;
        else if (argv[i][0] == '-') {
            fprintf(stderr, "gqlc compile: unknown option %s\n", argv[i]);
            return 2;
        } else
            in = argv[i];
    }
    if (!in) {
        fprintf(stderr, "usage: gqlc compile [-q] [-c gqlgw.toml] [-o out.gqlb] [-r out.routes.lua] schema.graphqls\n");
        return 2;
    }
    if (cfg_path) {
        if (load_config(&cfg, cfg_path, CFG_CHECK, NULL)) {
            gqlgw_config_free(&cfg);
            return 1;
        }
        have_cfg = 1;
        if (!out)
            out = cfg.gw.schema;
        if (!routes && cfg.gw.routes)
            routes = cfg.gw.routes;
    }
    if (!out)
        out = out_d = replace_ext(in, ".gqlb");
    if (!routes) {
        char *base = replace_ext(out, "");
        size_t bl = strlen(base);
        routes_d = malloc(bl + 12);
        memcpy(routes_d, base, bl);
        memcpy(routes_d + bl, ".routes.lua", 12);
        free(base);
        routes = routes_d;
    }
    if (!(src = read_file(in, &n)))
        goto done;
    errs_init(&errs, 100);
    errs_init(&warns, 100);
    buf_init(&b);
    c = gql_sdl_compile(src, n, &errs, &warns);
    errs_to_text(&warns, in, &b);
    if (b.len && !quiet)
        fprintf(stderr, "%s", buf_cstr(&b));
    buf_reset(&b);
    if (!c) {
        errs_to_text(&errs, in, &b);
        fprintf(stderr, "%s", buf_cstr(&b));
    } else if (have_cfg && check_sources(c, &cfg, cfg_path, quiet)) {
        /* reported */
    } else if (gql_gqlb_write(gql_compiled_schema(c), out, err, sizeof err) ||
               gql_routes_write(c, routes, in, err, sizeof err)) {
        fprintf(stderr, "gqlc: %s\n", err);
    } else {
        const gql_schema *s = gql_compiled_schema(c);
        if (!quiet)
            fprintf(stderr, "gqlc: %s -> %s, %s (%u types, %u fields, %u routes, hash %016llx)\n", in, out,
                    routes, s->ntypes, s->nfields, gql_compiled_nroutes(c), (unsigned long long)s->hash);
        rc = 0;
    }
    gql_compiled_free(c);
    buf_free(&b);
    errs_free(&errs);
    errs_free(&warns);
    free(src);
done:
    free(out_d);
    free(routes_d);
    if (have_cfg)
        gqlgw_config_free(&cfg);
    return rc;
}

/* writes a file atomically, readable by its owner only (it may hold secrets) */
static int write_private(const char *dir, const char *name, const gql_buf *b)
{
    char path[PATH_MAX], tmp[PATH_MAX];
    int fd;
    size_t off = 0;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    snprintf(tmp, sizeof tmp, "%s/.%s.%ld", dir, name, (long)getpid());
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        fprintf(stderr, "gqlc: %s: %s\n", tmp, strerror(errno));
        return -1;
    }
    while (off < b->len) {
        ssize_t w = write(fd, b->p + off, b->len - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "gqlc: %s: %s\n", tmp, strerror(errno));
            close(fd);
            unlink(tmp);
            return -1;
        }
        off += (size_t)w;
    }
    if (close(fd) || rename(tmp, path)) {
        fprintf(stderr, "gqlc: %s: %s\n", path, strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

static int cmd_render(int argc, char **argv)
{
    const char *cfg_path = NULL, *outdir = ".", *resolv = NULL;
    int locations_only = 0, quiet = 0, i, rc = 1;
    gqlgw_config cfg;
    gql_schema *s;
    char err[512], dir[PATH_MAX], lua_path[PATH_MAX + 16], routes[PATH_MAX];
    gql_buf lua, http, loc;

    for (i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc)
            cfg_path = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc)
            outdir = argv[++i];
        else if (!strcmp(argv[i], "--resolv-conf") && i + 1 < argc)
            resolv = argv[++i];
        else if (!strcmp(argv[i], "--locations-only"))
            locations_only = 1;
        else if (!strcmp(argv[i], "-q"))
            quiet = 1;
        else {
            fprintf(stderr, "gqlc render: unknown argument %s\n", argv[i]);
            return 2;
        }
    }
    if (!cfg_path) {
        fprintf(stderr, "usage: gqlc render -c gqlgw.toml [-o dir] [--locations-only] [--resolv-conf file]\n");
        return 2;
    }
    if (load_config(&cfg, cfg_path, CFG_RENDER, resolv)) {
        gqlgw_config_free(&cfg);
        return 1;
    }
    /* the schema must have been compiled (gqlc compile, at image build time) */
    if (!(s = gql_schema_open(cfg.gw.schema, err, sizeof err))) {
        fprintf(stderr, "gqlc: %s\ngqlc: compile the schema first: gqlc compile -c %s schema.graphqls\n", err, cfg_path);
        gqlgw_config_free(&cfg);
        return 1;
    }
    gql_schema_close(s);
    if (cfg.gw.routes) {
        snprintf(routes, sizeof routes, "%s", cfg.gw.routes);
    } else {
        size_t n = strlen(cfg.gw.schema);
        if (n > 5 && !strcmp(cfg.gw.schema + n - 5, ".gqlb"))
            n -= 5;
        snprintf(routes, sizeof routes, "%.*s.routes.lua", (int)n, cfg.gw.schema);
    }
    if (access(routes, R_OK)) {
        fprintf(stderr, "gqlc: %s: %s (it is written by gqlc compile next to the schema)\n", routes, strerror(errno));
        gqlgw_config_free(&cfg);
        return 1;
    }
    if (mkdir(outdir, 0700) && errno != EEXIST) {
        fprintf(stderr, "gqlc: %s: %s\n", outdir, strerror(errno));
        gqlgw_config_free(&cfg);
        return 1;
    }
    if (!realpath(outdir, dir)) {
        fprintf(stderr, "gqlc: %s: %s\n", outdir, strerror(errno));
        gqlgw_config_free(&cfg);
        return 1;
    }
    snprintf(lua_path, sizeof lua_path, "%s/gqlgw.lua", dir);
    buf_init(&lua);
    buf_init(&http);
    buf_init(&loc);
    gqlgw_render_lua(&cfg, cfg_path, &lua);
    gqlgw_render_nginx(&cfg, lua_path, cfg_path, &http, locations_only ? &loc : NULL);
    if (lua.oom || http.oom || loc.oom) {
        fprintf(stderr, "gqlc: out of memory\n");
    } else if (!write_private(dir, "gqlgw.lua", &lua) &&
               !(locations_only ? write_private(dir, "gqlgw.http.conf", &http) || write_private(dir, "gqlgw.locations.conf", &loc)
                                : write_private(dir, "gqlgw.conf", &http))) {
        if (!quiet)
            fprintf(stderr, "gqlc: %s -> %s/%s, %s (%u sources)\n", cfg_path, dir,
                    locations_only ? "gqlgw.http.conf, gqlgw.locations.conf" : "gqlgw.conf", "gqlgw.lua", cfg.nsources);
        rc = 0;
    }
    buf_free(&lua);
    buf_free(&http);
    buf_free(&loc);
    gqlgw_config_free(&cfg);
    return rc;
}

static void usage(void)
{
    fprintf(stderr,
            "usage:\n"
            "  gqlc compile [-q] [-c gqlgw.toml] [-o out.gqlb] [-r out.routes.lua] schema.graphqls\n"
            "  gqlc render -c gqlgw.toml [-o dir] [--locations-only] [--resolv-conf file]\n"
            "  gqlc check schema.gqlb query.graphql\n"
            "  gqlc plan schema.gqlb query.graphql [variables.json] [operationName]\n"
            "  gqlc introspect schema.gqlb [introspection-query.graphql]\n");
}

static gql_schema *open_schema(const char *path)
{
    char err[512];
    gql_schema *s = gql_schema_open(path, err, sizeof err);
    if (!s)
        fprintf(stderr, "gqlc: %s\n", err);
    return s;
}

static const gql_limits LIMITS = { 0, 0, 0, 100, 0, 0, 1, 0 };

static gql_doc *load_doc(const gql_schema *s, const char *path, int *ok)
{
    size_t n;
    char *q = read_file(path, &n);
    gql_doc *d;
    *ok = 0;
    if (!q)
        return NULL;
    d = gql_doc_new(s, q, n, &LIMITS);
    free(q);
    if (!d) {
        fprintf(stderr, "gqlc: out of memory\n");
        return NULL;
    }
    *ok = gql_doc_ok(d);
    return d;
}

static int cmd_check(int argc, char **argv)
{
    gql_schema *s;
    gql_doc *d;
    int ok, i, rc = 0;
    if (argc < 2) {
        usage();
        return 2;
    }
    if (!(s = open_schema(argv[0])))
        return 1;
    for (i = 1; i < argc; i++) {
        d = load_doc(s, argv[i], &ok);
        if (!d) {
            rc = 1;
            continue;
        }
        if (ok) {
            printf("%s: ok\n", argv[i]);
        } else {
            size_t n;
            const char *e = gql_doc_errors(d, &n);
            printf("%s: %.*s\n", argv[i], (int)n, e);
            rc = 1;
        }
        gql_doc_free(d);
    }
    gql_schema_close(s);
    return rc;
}

static const char *const KINDS[] = { "leaf", "object", "abstract", "raw" };

static void dump_sel(const gql_schema *s, const gql_plan_view *v, uint32_t si, int depth, gql_buf *b)
{
    const gql_plan_sel *sel = &v->sels[si];
    uint32_t k;
    buf_printf(b, "{\"type\":\"%s\",\"fields\":[", GQL_S(s, s->types[sel->type].name));
    if (depth > 40) {
        buf_puts(b, "\"...\"]}");
        return;
    }
    for (k = 0; k < sel->fields_count; k++) {
        const gql_plan_field *f = &v->fields[sel->fields_first + k];
        const gql_field_rec *fr = &s->fields[f->field];
        if (k)
            buf_putc(b, ',');
        buf_puts(b, "{\"key\":");
        buf_json_str(b, v->str + f->key_off, f->key_len);
        buf_printf(b, ",\"field\":\"%s%s%s\",\"kind\":\"%s\"",
                   fr->owner != GQL_NONE ? GQL_S(s, s->types[fr->owner].name) : "",
                   fr->owner != GQL_NONE ? "." : "", GQL_S(s, fr->name), KINDS[f->kind]);
        if (f->kind == PF_RAW) {
            buf_puts(b, ",\"value\":");
            buf_put(b, v->str + f->args_off, f->args_len);
        } else if (f->args_len) {
            buf_puts(b, ",\"args\":");
            buf_put(b, v->str + f->args_off, f->args_len);
        }
        if (f->kind == PF_OBJECT) {
            buf_puts(b, ",\"sel\":");
            dump_sel(s, v, f->child_first, depth + 1, b);
        } else if (f->kind == PF_ABSTRACT) {
            uint32_t c;
            buf_puts(b, ",\"cases\":{");
            for (c = 0; c < f->child_count; c++) {
                const gql_plan_case *cs = &v->cases[f->child_first + c];
                if (c)
                    buf_putc(b, ',');
                buf_printf(b, "\"%s\":", GQL_S(s, s->types[cs->type].name));
                dump_sel(s, v, cs->sel, depth + 1, b);
            }
            buf_putc(b, '}');
        }
        buf_putc(b, '}');
    }
    buf_puts(b, "]}");
}

static int cmd_plan(int argc, char **argv)
{
    gql_schema *s;
    gql_doc *d;
    gql_plan *p;
    char *vars = NULL;
    size_t vn = 0;
    const char *op = argc > 3 ? argv[3] : "";
    int ok, rc = 1;
    if (argc < 2) {
        usage();
        return 2;
    }
    if (!(s = open_schema(argv[0])))
        return 1;
    d = load_doc(s, argv[1], &ok);
    if (d && !ok) {
        size_t n;
        const char *e = gql_doc_errors(d, &n);
        printf("{\"errors\":%.*s}\n", (int)n, e);
    } else if (d && (argc < 3 || (vars = read_file(argv[2], &vn)))) {
        p = gql_plan_new(s, d, op, strlen(op), vars, vn, &LIMITS);
        if (!p) {
            fprintf(stderr, "gqlc: out of memory\n");
        } else if (!gql_plan_ok(p)) {
            size_t n;
            const char *e = gql_plan_errors(p, &n);
            printf("{\"errors\":%.*s}\n", (int)n, e);
        } else {
            const gql_plan_view *v = gql_plan_get_view(p);
            gql_buf b;
            buf_init(&b);
            buf_printf(&b, "{\"op\":\"%s\",\"sels\":%u,\"fields\":%u,\"root\":", v->op ? "mutation" : "query",
                       v->nsels, v->nfields);
            dump_sel(s, v, v->root_sel, 0, &b);
            buf_puts(&b, "}\n");
            fputs(buf_cstr(&b), stdout);
            buf_free(&b);
            rc = 0;
        }
        gql_plan_free(p);
    }
    free(vars);
    gql_doc_free(d);
    gql_schema_close(s);
    return rc;
}

static const char INTROSPECTION_QUERY[] =
    "query IntrospectionQuery {\n"
    "  __schema {\n"
    "    description\n"
    "    queryType { name kind }\n"
    "    mutationType { name kind }\n"
    "    subscriptionType { name kind }\n"
    "    types { ...FullType }\n"
    "    directives {\n"
    "      name description isRepeatable locations\n"
    "      args(includeDeprecated: true) { ...InputValue }\n"
    "      isDeprecated deprecationReason\n"
    "    }\n"
    "  }\n"
    "}\n"
    "fragment FullType on __Type {\n"
    "  kind name description specifiedByURL isOneOf\n"
    "  fields(includeDeprecated: true) {\n"
    "    name description\n"
    "    args(includeDeprecated: true) { ...InputValue }\n"
    "    type { ...TypeRef }\n"
    "    isDeprecated deprecationReason\n"
    "  }\n"
    "  inputFields(includeDeprecated: true) { ...InputValue }\n"
    "  interfaces { ...TypeRef }\n"
    "  enumValues(includeDeprecated: true) { name description isDeprecated deprecationReason }\n"
    "  possibleTypes { ...TypeRef }\n"
    "}\n"
    "fragment InputValue on __InputValue {\n"
    "  name description type { ...TypeRef } defaultValue isDeprecated deprecationReason\n"
    "}\n"
    "fragment TypeRef on __Type {\n"
    "  kind name\n"
    "  ofType { kind name ofType { kind name ofType { kind name ofType { kind name\n"
    "    ofType { kind name ofType { kind name ofType { kind name ofType { kind name } } } } } } } }\n"
    "}\n";

static int cmd_introspect(int argc, char **argv)
{
    gql_schema *s;
    gql_doc *d;
    gql_plan *p;
    int rc = 1;
    char *q = NULL;
    size_t qn = 0;
    if (argc < 1) {
        usage();
        return 2;
    }
    if (argc > 1 && !(q = read_file(argv[1], &qn)))
        return 1;
    if (!(s = open_schema(argv[0]))) {
        free(q);
        return 1;
    }
    d = q ? gql_doc_new(s, q, qn, &LIMITS) : gql_doc_new(s, INTROSPECTION_QUERY, sizeof INTROSPECTION_QUERY - 1, &LIMITS);
    free(q);
    if (d && !gql_doc_ok(d)) {
        size_t n;
        const char *e = gql_doc_errors(d, &n);
        fprintf(stderr, "gqlc: %.*s\n", (int)n, e);
    } else if (d) {
        p = gql_plan_new(s, d, "", 0, NULL, 0, &LIMITS);
        if (p && gql_plan_ok(p)) {
            const gql_plan_view *v = gql_plan_get_view(p);
            const gql_plan_field *f = &v->fields[v->sels[v->root_sel].fields_first];
            printf("{\"__schema\":%.*s}\n", (int)f->args_len, v->str + f->args_off);
            rc = 0;
        } else if (p) {
            size_t n;
            const char *e = gql_plan_errors(p, &n);
            fprintf(stderr, "gqlc: %.*s\n", (int)n, e);
        }
        gql_plan_free(p);
    }
    gql_doc_free(d);
    gql_schema_close(s);
    return rc;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 2;
    }
    if (!strcmp(argv[1], "compile"))
        return cmd_compile(argc - 2, argv + 2);
    if (!strcmp(argv[1], "render"))
        return cmd_render(argc - 2, argv + 2);
    if (!strcmp(argv[1], "check"))
        return cmd_check(argc - 2, argv + 2);
    if (!strcmp(argv[1], "plan"))
        return cmd_plan(argc - 2, argv + 2);
    if (!strcmp(argv[1], "introspect"))
        return cmd_introspect(argc - 2, argv + 2);
    usage();
    return 2;
}
