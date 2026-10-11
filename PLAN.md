# gqlgw — SDL-first GraphQL gateway on OpenResty

Plan validated on 2026-10-11. This document is the design reference; it is updated as the
milestones progress. v2 (distribution, TOML configuration, metrics): section 9. Progress:
section 10. Documentation and CI: section 11.

## 1. Goal

Serve a GraphQL API described in SDL, whose fields are resolved by REST services behind nginx,
removing the N+1 problem with **batching** (dataloader style) and a **per-resource cache** (local
shm + memcached).

```
          offline                                      runtime (OpenResty)
schema.graphqls ─► gqlc (C99) ─► schema.gqlb ──mmap──► libgql.so (C99, through FFI)
  + directives              └──► schema.routes.lua ─► gqlgw/*.lua
                                                        │
POST /graphql ─► content_by_lua ─► libgql: parse → validate → normalized plan
                                ─► Lua executor, in "rounds"
                                     ├─ L2: lua_shared_dict (per node)
                                     ├─ L3: memcached (ONE endpoint, possibly a mcrouter)
                                     └─ misses ─► capture_multi ─► location /_src/<source>/ ─► proxy_pass
                                ─► completion + ordered JSON
```

**C / Lua boundary**

- C = everything that only depends on (schema, query, variables): parsing, validation, variable
  coercion, normalization (fragments, `@skip/@include`, concrete types), introspection. Pure, no
  I/O, fuzzable.
- Lua = everything that depends on data: subrequests, cache, JSON → GraphQL mapping, result
  coercion, `null` propagation, serialization.

One C code base (GNU C99, no dependency) built as `gqlc` (CLI) and `libgql.so` (FFI).

## 2. Validated decisions

| # | Decision |
|---|---|
| 1 | Parsing + validation + plan in C99 through FFI (same code as `gqlc`). |
| 2 | No nginx C module. Dropped on 2026-10-11: it would bring nothing over FFI. |
| 3 | Routing declared in the SDL; convention `source: "x"` → `location /_src/x/`. |
| 4 | Cache key = REST resource (`source + path`), not the GraphQL selection. |
| 5 | **Memcached = one endpoint** (can be a mcrouter); clustering is not our problem. |
| 6 | Out of scope for v1: subscriptions, `@defer/@stream`, uploads, federation, pagination; custom scalars passed through. |
| 7 | Introspection on by default, can be turned off. |

Target spec: GraphQL **September 2025** (including `@oneOf`, schema coordinates), GraphQL-over-HTTP (draft).

## 3. Gateway directives

Declared implicitly by `gqlc` (no need to write them), hidden from introspection.

```graphql
directive @fetch(source: String!, path: String!, method: String! = "GET",
                 body: String, select: String) on FIELD_DEFINITION
directive @batch(path: String!, match: String!, max: Int! = 100, select: String) on FIELD_DEFINITION
directive @cache(ttl: Int!, scope: String! = "PUBLIC") on FIELD_DEFINITION | OBJECT | INTERFACE | UNION
directive @from(key: String!) on FIELD_DEFINITION
directive @discriminator(key: String!) on INTERFACE | UNION
```

- Field without `@fetch`: value read from the parent JSON object (key = field name, or `@from(key:)`).
- Templates: `{args.x}`, `{args.x.y}`, `{parent.field}` (GraphQL name, translated to the raw JSON
  key by `gqlc`), `{keys}` (batch only), `{args}` (body: every argument as JSON).
  In `path` values are URL-encoded; in `body` they are JSON-encoded. A brace that is not followed
  by `args`, `parent` or `keys` is literal (JSON bodies).
- `@batch`: requires a GET `@fetch` with exactly one `{parent.*}` (the key). Missing keys go in one
  `path` call (`{keys}` = keys joined with `,`), response = array (or `select`), matched by the
  JSON key `match`. Each item is cached under the key of its single fetch.
- `@cache`: on the field, otherwise on the returned type. Without `@cache`, no cache. Only GETs are
  cached. `scope: "PRIVATE"` adds the identity (configurable header) to the key.
- Abstract types: the JSON must carry `__typename`, or the key named by `@discriminator`.
- Upstream errors: 404 → `null`; other non-2xx / invalid JSON → field error + `null`.

## 4. Components

### C99 — `src/`

| File | Role |
|---|---|
| `arena.*`, `util.*` | arena, buffers, FNV-1a hash, hash tables |
| `lexer.*`, `parser.*`, `ast.h` | complete GraphQL lexer/parser (SDL + executable), hard limits, positions |
| `schema.*` | **flat** schema model (indexed arrays, no pointers) + lookups |
| `sdl.c` | builds the model from the SDL, validates the type system and the gateway directives |
| `gqlb.c` | writes / mmaps the `.gqlb` binary (the model arrays as they are + a string table) |
| `routes.c` | writes `schema.routes.lua` |
| `validate.c` | query validation (section 5 of the spec, every rule) |
| `json.*` | JSON parser (variables) + JSON writer |
| `plan.c` | operation selection, variable/argument coercion, normalization → flat plan |
| `introspect.c` | direct JSON rendering of `__schema` / `__type` |
| `gql.h`, `api.c` | public API (FFI) |
| `gqlc.c` | CLI: `compile`, `check`, `plan` |

FFI API (simplified):

```c
gql_schema *gql_schema_open(const char *path, gql_error *err);
gql_doc    *gql_doc_parse_validate(const gql_schema*, const char *q, size_t n,
                                   const gql_limits*, gql_error *err);
gql_plan   *gql_plan_build(const gql_schema*, const gql_doc*, const char *op, size_t opn,
                           const char *vars, size_t varsn, const gql_limits*, gql_error *err);
```

The plan is a set of `int32` arrays (selections per concrete type, fields, cases of abstract
types) read from Lua without copying. Validated documents are cached (per-worker LRU, key = query
text).

### Lua — `lua/gqlgw/`

| Module | Role |
|---|---|
| `init.lua` | `init(opts)` (init_by_lua), `serve()` (content_by_lua), `log()`, `purge()` |
| `ffi.lua`, `schema.lua` | FFI binding, Lua tables derived from the schema + routes |
| `http.lua` | GraphQL-over-HTTP (POST JSON, GET queries, `application/graphql-response+json`, status codes) |
| `exec.lua` | resolution by rounds, completion, `null` propagation, ordered serialization |
| `fetch.lua` | templates, deduplication, batching, `capture_multi`, response mapping |
| `cache.lua` | L2 shm + L3 memcached (multi-key `get`, async writes, fail-open, generations, purge) |

Libraries used: only those bundled with OpenResty (cjson, lrucache, memcached, resty.string,
string.buffer).

## 5. Milestones

| # | Deliverable | Exit criterion |
|---|---|---|
| M0 | Skeleton, Makefile, OpenResty 1.31.1.1 in `.deps/`, local memcached, Test::Nginx | `make test` green |
| M1 | Lexer + parser, arena, limits | corpus parsed; ASan/UBSan fuzzing without crash |
| M2 | `gqlc`: model, SDL + directive validation, `.gqlb` + `routes.lua` | invalid SDL rejected with line/col |
| M3 | Query validation | same verdicts as graphql-js (corpus + differential fuzzing) |
| M4 | Variables, plan, introspection, FFI API | introspection equivalent to graphql-js |
| M5 | Lua executor without I/O, serialization, errors | execution conformance tests |
| M6 | `@fetch` / `@batch` through `capture_multi`, mutations | 50 posts + authors = 2 upstream calls |
| M7 | shm + memcached cache, purge, negative cache | 2nd query = 0 upstream call |
| M8 | Hardening: limits, timeouts, upstream errors, `wrk` bench vs naive | bench report |
| **v2** | **Distribution and configuration (section 9)** | |
| M9 | TOML 1.1 parser in C99 (`src/toml.*`, linked into `gqlc` only, the FFI ABI does not change) | 100 % of the `toml-test` v2.2.0 corpus (`files-toml-1.1.0` list, valid + invalid); ASan/UBSan fuzzing without crash |
| M10 | `gqlgw.toml`: typed model, `gqlc compile -c` (SDL ↔ sources cross-checks), `gqlc render` (interpolation, `gqlgw.lua`), `gqlgw.init` reads the rendered configuration | golden files; errors with line/col; missing variable = refusal to start |
| M11 | nginx generation: default mode and `--locations-only`, `resolve` upstreams, allow/deny headers, TLS, `sources.d` | `openresty -t` on every golden configuration; Test::Nginx on the generated configuration (headers received by the mock, self-signed TLS accepted/refused) |
| M12 | Prometheus business metrics (port 8191, `/gqlgw_metrics`) | format checked by the tests; overhead < 5 % on the `author` bench |
| M13 | Docker image, entrypoint, `make install`, BSD-2 LICENSE, `examples/docker/`, README | `make image` and `make docker-test` green: query, batch, memcached cache, metrics, purge, refusal to start without a variable |

## 6. Tests

- C: `tests/c/` (in-house harness), golden files, libFuzzer + ASan/UBSan (`tests/fuzz/`).
- Oracle: graphql-js through node (`tests/oracle/`, dev tool only).
- Integration: Test::Nginx (`t/`), mock backends = nginx `server {}` blocks that count their hits.
- Bench: `wrk` (`bench/`).

## 7. Dependencies

apt: `memcached libmemcached-tools cpanminus clang llvm libclang-rt-dev wrk` (+ `gcovr valgrind`).
Outside apt, installed by `make deps` in `.deps/` without sudo: OpenResty 1.31.1.1 (sources),
Test::Nginx (cpanm local-lib), graphql-js (npm, dev).

v2: `docker.io docker-buildx docker-compose-v2` (apt). The `toml-test` v2.2.0 corpus is downloaded
by `make deps`: only its files are used, with an in-house runner, so Go is not needed.

## 8. Risks

- Spec validation = the biggest part → graphql-js oracle.
- C bug = worker crash → fuzzing, hard limits, ASan in CI.
- cjson: integers > 2^53, empty arrays (`decode_array_with_array_mt`), key order (in-house serializer).
- Subrequests buffered in memory and inheriting the client headers → size caps, cleaned headers.
- v2: collisions in `http {}` with an existing OpenResty install (single `init_by_lua`, names) →
  everything is prefixed `gqlgw_`, `[nginx] init = false`, no `lua_package_path`.
- v2: interpolated secrets are written into the rendered configuration → `/tmp/gqlgw` (tmpfs), mode 0600.
- v2: with `proxy_pass` to a named upstream, `Host` and SNI would be `gqlgw_<source>` →
  `proxy_set_header Host` and `proxy_ssl_name` are generated from the source URL.

## 9. v2: distribution and configuration

Decided and implemented on 2026-10-11 (M9 to M13, status in section 10).

### Decisions

| # | Decision |
|---|---|
| 8 | Distribution = Docker image built on `openresty/openresty:1.31.1.1-resolute` (glibc; no alpine, musl is too slow). The image is only tagged locally: it is not published to a registry. Docker, not podman. |
| 9 | The SDL is compiled **when the user image is built**. The gateway never starts from an uncompiled `.graphqls`. |
| 10 | Configuration in `gqlgw.toml` (TOML 1.1, in-house C99 parser), which carries as much of the gateway configuration as possible. The SDL keeps `@fetch(source, path)` on the fields: no "everything in configuration" mode. |
| 11 | `${VAR}` is accepted in every string value of the TOML. `gqlc` renders at container start; the Lua side reads the rendered configuration, never the environment. |
| 12 | `gqlc` generates a file to include in `http {}`, with its own `server`, never the whole `nginx.conf`. The `--locations-only` flag produces two files: the http block, and the locations to include in an existing `server`. |
| 13 | One URL per source: load balancing behind it (service, replica set) is not our business. The headers sent to the backends go through an allowlist or a denylist per source. TLS verified by default. Free settings through `sources.d/<source>/*.conf`. |
| 14 | Prometheus business metrics on a dedicated port (8191, `/gqlgw_metrics`). No cap on `operationName`, optional exclusion list with regexes. No `/healthz`. |
| 15 | `make install` now, `.deb` later. BSD-2-Clause license. |

TOML 1.1.0 was released on 2025-12-24. We target it rather than 1.0: its multi-line inline tables
are useful for `set_headers`, and `toml-test` v2.2.0 covers it.

### User journey

```dockerfile
FROM gqlgw:0.2.0
COPY schema.graphqls gqlgw.toml /etc/gqlgw/
RUN gqlc compile -c /etc/gqlgw/gqlgw.toml /etc/gqlgw/schema.graphqls
# optional: COPY sources.d/ /etc/gqlgw/sources.d/   and   COPY conf.d/ /etc/gqlgw/conf.d/
```

```bash
docker run -p 8080:8080 -p 8191:8191 \
  -e USERS_URL=http://users:8080/api/v1 -e MEMCACHED_HOST=memcached my-gateway
```

| When | Command | What it does |
|---|---|---|
| build | `gqlc compile -c gqlgw.toml schema.graphqls` | compiles the SDL (`.gqlb` + `routes.lua`). Checks the TOML: syntax, unknown keys (error with line/col), types, `${…}` syntax. Every `@fetch(source:)` needs its `[sources.x]`; an unused source gives a warning |
| start | `gqlc render -c gqlgw.toml [--locations-only] -o /tmp/gqlgw` | interpolates the environment (missing variable without default = refusal to start), checks the values (URL, durations, sizes), writes `gqlgw.conf` (nginx) and `gqlgw.lua` (runtime configuration of the Lua side, mode 0600). The entrypoint then runs `openresty -t` and `exec openresty` |

Outside Docker, the commands are the same: `gqlc render`, then `nginx -s reload`.

### `gqlgw.toml`

```toml
[gateway]
listen = 8080                          # or "0.0.0.0:8080"
server_name = "_"
schema = "schema.gqlb"                 # relative to the TOML file
introspection = true
strict_path_args = true
debug_headers = false
max_body = "1m"
access_log = "/dev/stdout"             # or "off"

[limits]                               # same defaults as the gqlgw.init options
query_bytes = 65536
depth = 32
# tokens, errors, plan_fields, memory, rounds, fetches, concurrency, response_bytes

[cache]
shm_size = "128m"                      # "0": no L2
l1_size = 10000
l1_ttl = 1
l2_ttl = 2
negative_ttl = 5
private_header = "Authorization"

[cache.memcached]                      # no section: no L3
host = "${MEMCACHED_HOST:-memcached}"
port = 11211
timeout = 50                           # ms

[nginx]
init = true                            # false: call gqlgw.init() from your own init_by_lua
resolver = "auto"                      # reads /etc/resolv.conf; or ["10.0.0.2"]
resolver_valid = "10s"

[metrics]
enabled = true
port = 8191
path = "/gqlgw_metrics"
allow = []                             # CIDR; empty: no filtering
exclude_operations = ["^IntrospectionQuery$"]

[purge]                                # served on the metrics port
enabled = true
path = "/gqlgw_purge"

[sources.users]
url = "${USERS_URL:-http://users:8080/api/v1}"   # http(s)://host[:port][/prefix], no {…}
connect_timeout = "1s"
read_timeout = "3s"
keepalive = 64
forward_headers = ["authorization", "accept-language"]  # allowlist (default: ["authorization"])
# drop_headers = ["cookie"]                              # or denylist, exclusive with forward_headers
set_headers = { X-Api-Key = "${USERS_API_KEY}" }
tls_verify = true                      # https: certificate verification + SNI
ca = "/etc/ssl/certs/ca-certificates.crt"
```

Interpolation: `${VAR}` (error if the variable is missing), `${VAR:-default}`, `${VAR:?message}`,
and `$${` for a literal `${`. It applies to string values. A typed field can be written
`"${PORT}"`: it is converted to the expected type.

### Generated nginx configuration

By default, `gqlc render` writes one file, `gqlgw.conf`, to include in `http {}`. It contains:

- `lua_shared_dict gqlgw_cache` and `gqlgw_metrics`; `init_by_lua_block { require("gqlgw").init("/tmp/gqlgw/gqlgw.lua") }`,
  unless `[nginx] init = false`;
- one `upstream gqlgw_<source>` per source: `zone`, `resolver` (inside the upstream, so no
  conflict with a global resolver), `server host:port resolve`, `keepalive`;
- the public `server`: `listen`, `location = /graphql` (`set $gqlgw_path ""`, `set $gqlgw_ctype ""`,
  log variables) and one `location /_src/<source>/` per source. Each `/_src/` location has:
  - `internal`;
  - the headers: with an allowlist, `proxy_pass_request_headers off` then one `proxy_set_header` per
    allowed header; with a denylist, `proxy_set_header X ""` for each refused header.
    `Accept-Encoding` is always cleared; the `Content-Type` of bodies comes from `$gqlgw_ctype`;
    `Host` is taken from the URL;
  - the timeouts;
  - TLS: `proxy_ssl_verify`, `proxy_ssl_server_name`, `proxy_ssl_name`, `proxy_ssl_trusted_certificate`;
  - `include /etc/gqlgw/sources.d/<source>/*.conf;`;
  - `proxy_pass http(s)://gqlgw_<source><prefix>$gqlgw_path`;
- the admin `server`, on the metrics port: `/gqlgw_metrics`, `/gqlgw_purge`, `allow` / `deny`.

`--locations-only` writes two files instead: `gqlgw.http.conf` (shm, init, upstreams, admin
`server`) and `gqlgw.locations.conf` (to include in an existing `server`).

There is no `lua_package_path`: the modules are installed in OpenResty's `site/lualib`, already in
its search path. Every name is prefixed `gqlgw_`.

### Metrics

| Metric | Labels |
|---|---|
| `gqlgw_requests_total` | `operation`, `type` (query, mutation), `result` (ok, partial, request_error) |
| `gqlgw_request_duration_seconds` (histogram) | `operation` |
| `gqlgw_errors_total` | `kind` (syntax, validation, variables, field, upstream) |
| `gqlgw_field_requests_total` | `coordinate` (`Type.field`) |
| `gqlgw_upstream_requests_total` | `source`, `status` (2xx, 404, 4xx, 5xx, timeout) |
| `gqlgw_upstream_duration_seconds` (histogram) | `source` |
| `gqlgw_batches_total`, `gqlgw_batch_size` (histogram) | `coordinate` |
| `gqlgw_cache_total` | `source`, `level` (l1, l2, l3, miss) |
| `gqlgw_memcached_errors_total` | — |
| `gqlgw_rounds` (histogram) | — |
| `gqlgw_doc_cache_total` | `result` (hit, miss) |
| `gqlgw_info` | `version`, `schema_hash` |

- `operation` is the `operationName` as is, or `__anonymous` without a name. A name that matches a
  regex of `exclude_operations` is counted under `__excluded`, so the totals stay right. The
  regexes are PCRE through `ngx.re`, compiled in the init: an invalid regex makes `openresty -t`
  fail. No cap.
- `coordinate` is bounded by the size of the schema. It is the right metric to know when a
  `@deprecated` field can be removed.
- Counters are aggregated per worker in Lua, then flushed into the `gqlgw_metrics` shm by a
  one-second timer started at the worker's first request (no `init_worker_by_lua` to share). The
  Prometheus text format is written by us, without a third-party library.
- Latency per source: `capture_multi` does not give it. It must come from the `/_src/` location
  (`$upstream_response_time` read in a `header_filter_by_lua`, then passed to the parent through an
  internal header). To be validated in M12.

### Image and installation

- Multi-stage `Dockerfile`:
  - the build stage starts from `openresty/openresty:1.31.1.1-resolute`, adds gcc and make, and
    produces `libgql.so` and `gqlc`;
  - the final image starts from the same base and contains `gqlc` (`/usr/local/bin`), `libgql.so`
    (`/usr/local/lib/gqlgw/`), the Lua modules (`/usr/local/openresty/site/lualib/gqlgw/`), a
    minimal `nginx.conf` (`include /tmp/gqlgw/gqlgw.conf; include /etc/gqlgw/conf.d/*.conf;`) and
    the entrypoint.
- Entrypoint: `gqlc render`, then `openresty -t`, then `exec openresty -g 'daemon off;'`. Logs on
  stdout and stderr. Works with `--read-only` and a tmpfs on `/tmp`.
- `make image` creates the local tags `gqlgw:<version>` and `gqlgw:latest`. Nothing is pushed.
- `make install PREFIX=/usr/local OPENRESTY_PREFIX=/usr/local/openresty DESTDIR=` installs to the
  same locations, outside Docker.
- `examples/docker/` contains a user Dockerfile, a `gqlgw.toml` and a `compose.yaml` (gateway,
  memcached, mock backend). It is also the integration test (`make docker-test`).

## 10. Progress (2026-10-11)

| # | Status | Result |
|---|---|---|
| M0 | done | `make deps` (OpenResty 1.31.1.1 built in `.deps/`, Test::Nginx 0.32, graphql-js 17.0.2), `make test` |
| M1 | done | complete lexer/parser; libFuzzer + ASan/UBSan fuzzing: **1 bug found and fixed** (read past the buffer on a truncated `\u` at the end of the input, regression test added), then 7.5 M executions without crash |
| M2 | done | `gqlc compile`: flat model, type system validation (graphql-js 17 messages), gateway directives, mmap'ed `.gqlb` with every bound checked, `routes.lua` |
| M3 | done | 46/46 corpus queries identical to graphql-js; differential fuzzing on 9,000 documents: 0 verdict difference, ~99.7 % identical messages |
| M4 | done | variables, memoized plan (DAG), introspection identical to graphql-js, FFI API |
| M5 | done | executor by rounds, completion, `null` propagation, ordered serialization |
| M6 | done | `@fetch` / `@batch` through `capture_multi`, serial mutations; 20 posts + authors = 2 subrequests |
| M7 | done | L1 lrucache (decoded values) + L2 shm + L3 memcached (multi-get, writes pipelined from a timer, fail-open), negative cache, PRIVATE scope, purge by generation |
| M8 | done | limits (size, tokens, depth, plan, rounds, fetches, response), timeouts → field errors, `wrk` bench (bench/RESULTS.md) |
| M9 | done | `src/toml.c` (TOML 1.1.0, ~1,000 lines): 214/214 valid and 467/467 invalid documents of the `toml-test` v2.2.0 corpus; fuzzing 6.1 M executions without crash |
| M10 | done | `src/config.c`, `src/render.c`, `gqlc compile -c` / `gqlc render`: 14 cases (line:column errors, reference renderings, `openresty -t`); configuration fuzzing: **2 bugs found and fixed** (rendering of an unresolved source, invalid field without a default value), then 3.7 M executions without crash |
| M11 | done | generated configuration tested by Test::Nginx (`t/006`, `t/007`): allowlist, denylist, `set_headers` with `$`, bodies, TLS verified and refused, DNS resolution, cache, purge, `server.d` |
| M12 | done | `lua/gqlgw/metrics.lua`, 13 families (`t/008`); overhead within the measurement noise (−1.5 % / +1.3 % in median, bench/RESULTS.md) |
| M13 | done | multi-stage Dockerfile (279 MB, base `openresty/openresty:1.31.1.1-resolute`), non-root user, `--read-only`; `make install`; BSD-2 LICENSE; `examples/docker/` (compose); `make docker-test`: 15 checks (two gateways + memcached + mock) |

### Deviations from the initial plan

- **Upstream path through `$gqlgw_path`** instead of the subrequest URI: nginx decodes subrequest
  URIs (`%2F` → `/`), which let an argument change the backend path. The location becomes
  `proxy_pass http://svc$gqlgw_path;` and the value is sent as is. On top of that,
  `strict_path_args` (default) refuses `/`, `.` and `..` in a path segment.
- **L1 cache added** (per-worker lrucache, values already decoded): without it, the "cache" path was
  slower than the batch alone (JSON decoding of every entity, calls not compiled by the JIT).
- **Messages aligned on graphql-js 17** (not 16): `@deprecated` on directives,
  `includeDeprecated: Boolean! = false`, `MaxIntrospectionDepth`, "finite value" of input objects.

### v2 deviations from the plan

- **Runtime directory `/tmp/gqlgw`** instead of `/run/gqlgw`: Docker mounts a tmpfs on `/run` with
  mode 755 owned by root (unusable by the `gqlgw` user), and on `/tmp` with mode 1777. The
  `--read-only --tmpfs /tmp` idiom therefore works as is.
- **`$${` gives a literal `${`** (instead of `$$` → `$`): a lone `$` stays literal, which breaks
  neither regexes (`^Foo$`) nor API keys.
- **`Accept: application/json`** sent to the backends by default (unless `Accept` is in
  `forward_headers` or `set_headers`), and the headers managed by gqlgw (`Host`, `Content-Type`,
  `Content-Length`, `Accept-Encoding`, `Connection`…) refused in the lists.
- **Upstream time per source** passed up by `more_set_headers "X-Gqlgw-Upstream-Time:
  $upstream_header_time"` in the generated locations (headers-more applies to subrequests),
  without an extra Lua phase.
- **Source check in the Lua init**: `gqlgw.init` refuses a schema whose source has no
  `[sources.x]`, so `openresty -t` does too (the entrypoint runs it before starting).
- **Resolver in the `/graphql` location** when memcached is given by a DNS name (cosockets resolve
  with the location's resolver, not the upstreams').
- **`gql_plan_operation_name`** added to the FFI API (additive, ABI unchanged) for the `operation`
  label of the metrics.

### Assumed divergences from graphql-js

- No "Did you mean" suggestions (equivalent of `hideSuggestions: true`).
- The values of directive arguments applied in the SDL are validated (graphql-js does not).
- On pathological documents (> 100 errors, nested cyclic fragments), the order and duplicates of
  some field-merging messages can differ; the verdicts stay identical.

## 11. Documentation and CI (2026-10-11)

- **Documentation site** built with [Zensical](https://zensical.org) 0.0.69 (the successor of
  Material for MkDocs, which reaches end of life on 2026-11-05; MkDocs 2.0 is a plugin-less
  pre-release), from `docs/` and `zensical.toml`, published on GitHub Pages
  (<https://abusi.github.io/gqlgw/>) by `.github/workflows/docs.yml` on every push to `main`.
  Diagrams are Mermaid. In English, like every document of the repository.
- **Tested examples**: `examples/{full,locations-only,auth}/` are included in the documentation and
  checked by `make test-config` (compile with the schema, render, `openresty -t`).
- **CI** (`.github/workflows/ci.yml`) on pull requests only: `make test` in the
  `openresty/openresty:1.31.1.1-resolute` image, fuzzing (regressions + 30 s per target),
  `make docker-test`, strict documentation build.
- Writing the documentation found a bug: `{parent.x}` in a `@fetch(body:)` template was accepted
  by `gqlc` but rendered as `null` (looked up in the arguments). Fixed in `fetch.lua`, covered by
  `t/003-mutation.t`.
- Replaying the CI jobs in the image found two portability problems, both fixed: `make fuzz` and
  `build/test_c` did not depend on the generated `src/prelude.inc` (broken on a fresh clone), and
  `t/006-config.t` relied on the system DNS to resolve `localhost` for the nginx resolver. The suite
  now starts its own responder (`scripts/dns.py`, `make dns-start`), like its memcached.
