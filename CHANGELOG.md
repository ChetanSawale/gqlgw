# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[Semantic Versioning](https://semver.org/) (0.x: the configuration and the Lua API can still change).

## [0.2.0] - 2026-10-11

First public release.

### Gateway

- SDL-first GraphQL gateway for OpenResty: the schema is annotated with `@fetch`, `@batch`,
  `@cache`, `@from` and `@discriminator`, predeclared and hidden from introspection.
- C99 core without dependency (`libgql.so`, loaded through LuaJIT FFI, and the `gqlc` CLI):
  parsing, every validation rule of the GraphQL September 2025 spec (graphql-js 17 messages),
  variable coercion, flat execution plan, introspection identical to graphql-js 17.
- Lua executor: resolution by rounds, deduplication of identical fetches, `@batch` (one request
  per round and per chunk of keys), null propagation, ordered serialization, serial mutations.
- Resource cache: L1 per-worker LRU, L2 `lua_shared_dict`, L3 memcached (one endpoint), negative
  entries, `PRIVATE` scope, purge by generation, fail-open when memcached is down.
- GraphQL over HTTP: `POST` and `GET`, `application/graphql-response+json`, status codes of the draft.
- Limits on the document, the plan, the rounds, the fetches and the upstream responses.

### Configuration and distribution

- `gqlgw.toml` (TOML 1.1, in-house parser passing the whole toml-test corpus), with `${VAR}`,
  `${VAR:-default}` and `${VAR:?message}` interpolated at start.
- `gqlc compile -c` checks the schema against the configuration at build time; `gqlc render`
  generates the nginx block (own `server {}`, or `--locations-only`) and the runtime options.
- Generated upstreams with keepalive and DNS re-resolution, TLS verification, header allowlist or
  denylist, `set_headers`, `sources.d/` and `server.d/` extension points.
- Prometheus metrics on an admin port: requests, durations, errors, fields, upstreams, batches,
  cache levels, rounds.
- Docker image on `ghcr.io/abusi/gqlgw` (linux/amd64, linux/arm64), built from
  `openresty/openresty:1.31.1.1-resolute`, non-root, works with `--read-only`; `make install`
  for an existing OpenResty.

### Fixed before the release

- `{parent.*}` in a `@fetch(body:)` template was sent as `null`.
- `make fuzz` and the C unit tests did not build on a fresh clone (generated `src/prelude.inc`).

[0.2.0]: https://github.com/abusi/gqlgw/releases/tag/v0.2.0
