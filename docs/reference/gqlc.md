# gqlc

`gqlc` is the schema compiler, the configuration renderer, and a debugging tool. It is built from
the same C sources as `libgql.so`, so it validates and plans exactly like the gateway.

```text
gqlc compile [-q] [-c gqlgw.toml] [-o out.gqlb] [-r out.routes.lua] schema.graphqls
gqlc render -c gqlgw.toml [-o dir] [--locations-only] [--resolv-conf file] [-q]
gqlc check schema.gqlb query.graphql
gqlc plan schema.gqlb query.graphql [variables.json] [operationName]
gqlc introspect schema.gqlb [introspection-query.graphql]
```

Exit status: `0` on success, `1` on an error in the input, `2` on a usage error.

## compile

Compiles the SDL into `schema.gqlb` and `schema.routes.lua`.

| Option | |
|---|---|
| `-c gqlgw.toml` | also check the configuration, and every `@fetch(source:)` against its `[sources.x]` |
| `-o out.gqlb` | output file (default: `[gateway] schema` with `-c`, else the input with `.gqlb`) |
| `-r out.routes.lua` | routes file (default: `[gateway] routes` with `-c`, else `<output>.routes.lua`) |
| `-q` | no summary line |

```console
$ gqlc compile -c gqlgw.toml schema.graphqls
gqlc: schema.graphqls -> schema.gqlb, schema.routes.lua (21 types, 60 fields, 7 routes, hash dd5aa30cab392b8a)
```

The hash identifies the schema: it is exported as `gqlgw_info{schema_hash="…"}`.

## render

Renders `gqlgw.toml` with the environment into the files nginx loads.

| Option | |
|---|---|
| `-c gqlgw.toml` | required |
| `-o dir` | output directory (default `.`; created with mode `0700` if missing) |
| `--locations-only` | write `gqlgw.http.conf` + `gqlgw.locations.conf` instead of `gqlgw.conf` |
| `--resolv-conf file` | the file read by `resolver = "auto"` (default `/etc/resolv.conf`) |
| `-q` | no summary line |

Files are written atomically with mode `0600`: they can hold secrets from the environment. The
compiled schema and its routes must exist (`gqlc compile` first).

| Mode | Files |
|---|---|
| default | `gqlgw.conf` (include in `http {}`), `gqlgw.lua` |
| `--locations-only` | `gqlgw.http.conf` (in `http {}`), `gqlgw.locations.conf` (in a `server {}`), `gqlgw.lua` |

## check

Parses and validates a query against a compiled schema, like the gateway would.

```console
$ gqlc check schema.gqlb query.graphql
query.graphql: [{"message":"Cannot query field \"titel\" on type \"Post\".","locations":[{"line":1,"column":11}]}]
```

## plan

Prints the execution plan as JSON: the operation, the coerced arguments, the selections per
concrete type. Useful to see what a query will fetch.

```console
$ echo '{ posts(limit: 2) { title author { name } } }' > q.graphql
$ gqlc plan schema.gqlb q.graphql
{"op":"query","sels":3,"fields":4,"root":{"type":"Query","fields":[{"key":"posts","field":"Query.posts",
 "kind":"object","args":{"limit":2},"sel":{"type":"Post","fields":[{"key":"title","field":"Post.title",
 "kind":"leaf"},{"key":"author","field":"Post.author","kind":"object","sel":{"type":"User","fields":[
 {"key":"name","field":"User.name","kind":"leaf"}]}}]}}]}}
```

## introspect

Prints the introspection result of the schema (the standard introspection query, or the one
given), as clients will see it: the gateway directives are not in it.

```bash
gqlc introspect schema.gqlb > schema.json     # for client code generators
```

## In the Docker image

`gqlc` is in the `PATH`. Any command given to `docker run` other than nginx flags is run as is:

```bash
docker run --rm my-gateway gqlc introspect /etc/gqlgw/schema.gqlb > schema.json
```
