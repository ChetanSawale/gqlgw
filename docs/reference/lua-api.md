# Lua API

`gqlc render` generates every call below. Read this page if you write the nginx configuration by
hand, or if you already have an `init_by_lua` (`[nginx] init = false`).

| Function | Phase | |
|---|---|---|
| `require("gqlgw").init(opts)` | `init_by_lua` | loads `libgql.so` and the schema, configures the cache and the metrics |
| `require("gqlgw").serve()` | `content_by_lua` of the GraphQL location | handles one GraphQL request |
| `require("gqlgw").log()` | `log_by_lua` of the GraphQL location | request metrics, `$gql_*` log variables |
| `require("gqlgw").metrics()` | `content_by_lua` | Prometheus text |
| `require("gqlgw").purge()` | `content_by_lua` | `POST ?source=<name>`: purge a source |

## `init(opts)`

`opts` is either the path of the `gqlgw.lua` written by `gqlc render` (recommended), or a table:

```lua
require("gqlgw").init("/tmp/gqlgw/gqlgw.lua")

-- or, by hand:
require("gqlgw").init({
    schema = "/etc/gqlgw/schema.gqlb",        -- routes: /etc/gqlgw/schema.routes.lua
    lib = "/usr/local/lib/gqlgw/libgql.so",
    introspection = true,
    debug_headers = false,
    strict_path_args = true,
    cache = {
        shm = "gqlgw_cache",                  -- a lua_shared_dict; omit for no L2
        l2_ttl = 5,
        memcached = { host = "127.0.0.1", port = 11211, timeout = 50, pool_size = 64 },
        private_header = "Authorization",
    },
    metrics = { shm = "gqlgw_metrics", exclude_operations = { "^IntrospectionQuery$" } },
    max_query_bytes = 65536, max_depth = 32, max_rounds = 16, max_fetches = 1000,
})
```

| Key | Default | `gqlgw.toml` equivalent |
|---|---|---|
| `schema`, `routes`, `lib` | required, `<schema>.routes.lua`, system library | `[gateway]` |
| `introspection`, `debug_headers`, `strict_path_args` | `true`, `false`, `true` | `[gateway]` |
| `max_body_bytes` | `1048576` | `[gateway] max_body` |
| `max_query_bytes`, `max_tokens`, `max_depth`, `max_errors` | 65536, 20000, 32, 100 | `[limits]` |
| `max_plan_fields`, `max_memory` | 20000, 16 MiB | `[limits]` |
| `max_rounds`, `max_fetches`, `concurrency`, `max_response_bytes` | 16, 1000, 64, 16 MiB | `[limits]` |
| `cache_negative_ttl` | 5 | `[cache] negative_ttl` |
| `cache` | none | `[cache]`, `[cache.memcached]` |
| `metrics` | none (no metrics) | `[metrics]` |
| `sources` | not checked | the list of `[sources.*]`: `init` fails if the schema uses another one |
| `sources_prefix` | `"/_src/"` | — |

## Hand-written locations

Without `gqlc render`, you write the locations yourself. The contract:

- the GraphQL location declares `$gqlgw_path` (`set $gqlgw_path "";`);
- one location `/_src/<source>/` per source, which sends `$gqlgw_path` upstream as is;
- these locations must be `internal`, clear `Accept-Encoding`, and choose the client headers
  they forward.

```nginx
--8<-- "conf/nginx.conf.example"
```

The TOML configuration remains the recommended way: it generates these locations with their
safeguards (allowlist, `Host`, TLS, `Content-Type` of bodies, upstream timing).
