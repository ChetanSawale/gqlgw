# Build and deploy pipeline

A gateway goes through five steps. Each one checks what it can, as early as it can.

```mermaid
flowchart LR
    A["1. author<br/>schema.graphqls<br/>gqlgw.toml"] -- "docker build" --> B["2. compile<br/>gqlc compile -c"]
    B -- "container start" --> C["3. render<br/>gqlc render"]
    C --> D["4. check<br/>openresty -t"]
    D --> E["5. serve<br/>openresty"]
```

| Step | When | Command | Reads | Writes |
|---|---|---|---|---|
| compile | image build | `gqlc compile -c gqlgw.toml schema.graphqls` | SDL, TOML (unrendered) | `schema.gqlb`, `schema.routes.lua` |
| render | container start | `gqlc render -c gqlgw.toml -o /tmp/gqlgw` | TOML, environment, `/etc/resolv.conf`, `schema.gqlb` | `gqlgw.conf`, `gqlgw.lua` (mode 0600) |
| check | container start | `openresty -t` | rendered files, `schema.gqlb`, `libgql.so` | — |
| serve | container start | `exec openresty` | same | — |

## 1. Compile: the schema becomes data

```mermaid
flowchart TB
    sdl[schema.graphqls] --> parse["parse<br/>(+ predeclared gateway directives)"]
    parse --> ts["validate the type system<br/>(graphql-js 17 messages)"]
    ts --> gd["validate the gateway directives<br/>@fetch @batch @cache @from @discriminator"]
    gd --> tpl["parse the templates<br/>{args.x} {parent.f} {keys}"]
    toml[gqlgw.toml] --> tcheck["TOML syntax, keys, types,<br/>${...} syntax (no environment)"]
    tpl --> cross{"every @fetch source<br/>has a [sources.x]?"}
    tcheck --> cross
    cross -- yes --> out["schema.gqlb (flat arrays + string table)<br/>schema.routes.lua (parsed routes)"]
    cross -- no --> fail([error, exit 1])
```

`schema.gqlb` is the schema model **as it lives in memory**: arrays of fixed-size records that
point to each other by index, plus a string table. At runtime, `libgql.so` maps it with `mmap`
and checks every bound, without parsing anything. `schema.routes.lua` holds the parsed
`@fetch` / `@batch` / `@cache` arguments, loaded by `gqlgw.init`.

Errors carry a position and stop the build:

```text
schema.graphqls:2:23: @fetch(path:) of Query.user: unknown argument "uid".
schema.graphqls:3:59: @batch on Query.posts requires the @fetch path to reference exactly one {parent.*} key.
gqlgw.toml:2:1: Unknown key "listne" in [gateway].
gqlgw.toml: @fetch(source: "orders") on Query.orders: no [sources.orders] in the configuration
```

A `[sources.x]` that no `@fetch` uses gives a warning, not an error.

## 2. Render: configuration meets environment

```mermaid
flowchart TB
    toml[gqlgw.toml] --> interp["interpolate ${VAR}, ${VAR:-default}, ${VAR:?message}"]
    env([process environment]) --> interp
    interp --> values["check the values<br/>URLs, durations, sizes, CIDRs, header names"]
    resolv(["/etc/resolv.conf"]) --> dns["resolver = auto<br/>→ nameservers"]
    values --> files
    dns --> files
    gqlb[schema.gqlb] --> exists["schema and routes exist"]
    exists --> files["gqlgw.conf (nginx, include in http {})<br/>gqlgw.lua (runtime options of the Lua side)"]
```

- Rendering happens **at start**, so one image runs in every environment. The Lua side never
  reads the environment: it reads `gqlgw.lua`.
- Secrets interpolated into the files (`set_headers = { X-Api-Key = "${KEY}" }`) end up in files
  written with mode `0600`, under `/tmp/gqlgw` in the image (a tmpfs with `--read-only`).
- A missing variable is fatal and names its position:
  `gqlgw.toml:5:7: Environment variable USERS_URL is not set (use ${USERS_URL:-default} to give a default value).`
- With `--locations-only`, `gqlgw.conf` is replaced by `gqlgw.http.conf` and
  `gqlgw.locations.conf` ([example](../examples/locations-only.md)).

## 3. Check, then serve

The image entrypoint chains everything and only starts nginx if every step passed:

```mermaid
sequenceDiagram
    participant D as docker run
    participant E as gqlgw-entrypoint
    participant G as gqlc
    participant N as openresty
    D->>E: start (env: USERS_URL, ...)
    E->>G: gqlc render -c /etc/gqlgw/gqlgw.toml -o /tmp/gqlgw
    alt variable missing or value invalid
        G-->>E: exit 1 + message
        E-->>D: container exits
    end
    E->>N: openresty -t
    Note over N: init_by_lua runs gqlgw.init():<br/>loads libgql.so, maps schema.gqlb,<br/>checks the sources, compiles the metrics regexes
    alt schema or library unusable
        N-->>E: test failed
        E-->>D: container exits
    end
    E->>N: exec openresty in the foreground
    Note over N: workers serve /graphql (8080)<br/>and the admin server (8191)
```

`gqlgw.init()` runs in the nginx master, before the fork. It loads `libgql.so`, maps the schema
and builds the Lua tables, so workers share them copy-on-write.

## Changing things

| What changed | What to do |
|---|---|
| `schema.graphqls` | rebuild the image (`gqlc compile`), redeploy |
| `gqlgw.toml` | rebuild the image (the TOML is checked against the schema at build time) |
| an environment variable | restart the container (render runs again) |
| outside Docker | `gqlc compile` and/or `gqlc render`, then `nginx -s reload` |

Next: [the life of a request](request.md).
