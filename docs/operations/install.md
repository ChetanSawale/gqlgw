# Existing nginx / OpenResty

gqlgw needs OpenResty (LuaJIT, `lua-nginx-module`, `headers-more`, `lua-resty-lrucache`,
`lua-resty-memcached`): a stock OpenResty 1.31 has all of them.

## Install

```bash
make                       # build/libgql.so, build/gqlc (GNU C99, no dependency)
sudo make install          # PREFIX=/usr/local OPENRESTY_PREFIX=/usr/local/openresty
```

| Installed | Where |
|---|---|
| `gqlc` | `$(PREFIX)/bin/gqlc` |
| `libgql.so` | `$(PREFIX)/lib/gqlgw/libgql.so` (the default `[gateway] lib`) |
| Lua modules | `$(OPENRESTY_PREFIX)/site/lualib/gqlgw/` (already in OpenResty's search path) |

`make uninstall` removes them. `DESTDIR` is supported for packaging.

## Configure

```bash
cd /etc/gqlgw
gqlc compile -c gqlgw.toml schema.graphqls
gqlc render  -c gqlgw.toml -o /etc/openresty/gqlgw
```

```nginx
http {
    include /etc/openresty/gqlgw/gqlgw.conf;
}
```

Or with `--locations-only`, see [Locations only](../examples/locations-only.md).

## Reload

```mermaid
flowchart LR
    e["edit schema.graphqls<br/>or gqlgw.toml"] --> c["gqlc compile -c"] --> r["gqlc render"] --> t["nginx -t"] --> rl["nginx -s reload"]
```

`nginx -t` runs `gqlgw.init()`: it loads the schema and checks the sources, so a broken schema
is caught before the reload. New workers start with the new schema; the shared dicts (L2 cache,
metrics) survive the reload.

## Sharing the http block

- **One `init_by_lua` per `http {}`.** If you have one, set `[nginx] init = false` and call
  `require("gqlgw").init("/etc/openresty/gqlgw/gqlgw.lua")` inside yours.
- **Names.** Everything generated starts with `gqlgw_` (shared dicts, upstreams, variables,
  `log_format`) or `/_src/` (locations).
- **Resolver.** The generated resolvers are scoped to the gqlgw upstreams and locations.
- **`lua_package_path`** is never set by the generated files.
