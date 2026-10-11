# Full server

The default mode: `gqlc render` writes **one file**, `gqlgw.conf`, which you include in the
`http {}` block of any nginx/OpenResty configuration. It brings its own `server {}` for GraphQL
and a second one for the admin endpoints.

```toml title="examples/full/gqlgw.toml"
--8<-- "examples/full/gqlgw.toml"
```

```bash
cd /etc/gqlgw                                        # gqlgw.toml + schema.graphqls
gqlc compile -c gqlgw.toml schema.graphqls           # schema.gqlb, schema.routes.lua
gqlc render  -c gqlgw.toml -o /etc/nginx/gqlgw       # gqlgw.conf, gqlgw.lua
```

```nginx title="nginx.conf"
http {
    # ... your existing configuration ...
    include /etc/nginx/gqlgw/gqlgw.conf;
}
```

## What the include contains

```mermaid
flowchart TB
    subgraph http ["http {} (your nginx.conf)"]
        shm["lua_shared_dict gqlgw_cache 128m<br/>lua_shared_dict gqlgw_metrics 8m"]
        init["init_by_lua_block<br/>gqlgw.init('/etc/nginx/gqlgw/gqlgw.lua')"]
        lf["log_format gqlgw"]
        u1["upstream gqlgw_users<br/>users.internal:8080 resolve · keepalive 64"]
        u2["upstream gqlgw_posts<br/>posts.internal:443 resolve · keepalive 64"]
        subgraph s1 ["server · listen 0.0.0.0:8080 reuseport · api.example.com"]
            g["location = /graphql<br/>serve() · log()"]
            l1["location /_src/users/ · internal"]
            l2["location /_src/posts/ · internal · TLS verified"]
            sd["include server.d/*.conf"]
        end
        subgraph s2 ["server · listen 8191 · allow 127.0.0.1, 10.0.0.0/8"]
            m["location = /gqlgw_metrics"]
            p["location = /gqlgw_purge"]
        end
    end
    l1 --> u1
    l2 --> u2
```

Excerpt of the generated `gqlgw.conf` (the `/_src/posts/` location):

```nginx
location /_src/posts/ {
    internal;                                   # unreachable from outside
    proxy_http_version 1.1;
    proxy_set_header Connection "";             # keepalive to the upstream
    proxy_set_header Host "posts.internal";     # from the URL, not "gqlgw_posts"
    proxy_set_header Accept-Encoding "";        # the gateway decodes JSON
    proxy_set_header Content-Type $gqlgw_ctype; # set only when there is a body
    proxy_pass_request_headers off;             # allowlist:
    proxy_set_header Authorization $http_authorization;
    proxy_set_header Accept-Language $http_accept_language;
    proxy_set_header Accept "application/json";
    more_set_headers "X-Gqlgw-Upstream-Time: $upstream_header_time";   # metrics
    proxy_connect_timeout 1s;
    proxy_send_timeout 5s;
    proxy_read_timeout 5s;
    proxy_ssl_server_name on;                   # https: SNI + verification
    proxy_ssl_name "posts.internal";
    proxy_ssl_verify on;
    proxy_ssl_verify_depth 4;
    proxy_ssl_trusted_certificate "/etc/ssl/certs/ca-certificates.crt";
    include "/etc/gqlgw/sources.d/posts/*.conf";   # your additions, if any
    proxy_pass "https://gqlgw_posts$gqlgw_path";   # path rendered by the gateway
}
```

Every line is explained in [Generated nginx configuration](../reference/nginx.md).

## Things to note

- `listen`, `server_name`, the metrics `port` and `allow` are passed to nginx as written, so
  `listen = "0.0.0.0:8443 ssl"` works. Put the certificates in `server.d/*.conf`, next to
  `gqlgw.toml`.
- `resolver = "auto"` copies the nameservers of `/etc/resolv.conf` when rendering. Upstreams
  named by DNS are re-resolved at runtime (`server ... resolve`), every `resolver_valid`.
- `access_log` takes a path, or `"off"`. The `gqlgw` format adds `rounds=`, `sub=` and `cache=` to
  the usual fields.
- Two gateways sharing the same memcached share the L3 cache, and a purge on one is seen by the
  other within `gen_ttl`.
