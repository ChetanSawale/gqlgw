/* libFuzzer target: gqlgw.toml loading (RENDER mode, fake environment) and rendering. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "config.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static const char *fake_env(const char *name, void *ud)
{
    (void)ud;
    if (!strcmp(name, "EMPTY"))
        return "";
    if (!strcmp(name, "UNSET"))
        return NULL;
    if (!strcmp(name, "URL"))
        return "http://users.svc:8080/api";
    return "8080";
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    gqlgw_config cfg;
    gql_errors errs;
    gql_buf a, b, c;
    errs_init(&errs, 100);
    buf_init(&a);
    buf_init(&b);
    buf_init(&c);
    gqlgw_cfg_mode mode = size & 1 ? CFG_RENDER : CFG_CHECK;
    if (gqlgw_config_load(&cfg, "fuzz.toml", (const char *)data, size, mode, fake_env, NULL, "/nonexistent", &errs) == 0 &&
        mode == CFG_RENDER) {
        gqlgw_render_lua(&cfg, "fuzz.toml", &a);
        gqlgw_render_nginx(&cfg, "/run/gqlgw/gqlgw.lua", "fuzz.toml", &b, size & 2 ? &c : NULL);
    }
    errs_to_text(&errs, "fuzz.toml", &a);
    gqlgw_config_free(&cfg);
    errs_free(&errs);
    buf_free(&a);
    buf_free(&b);
    buf_free(&c);
    return 0;
}
