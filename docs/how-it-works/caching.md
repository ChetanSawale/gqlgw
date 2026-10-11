# Resource cache

The cache stores **REST resources**, not GraphQL responses. The key of a cached value is the
request that produced it (`users:/users/42`), so every query that needs user 42 shares the entry,
whatever its selection.

## Three levels

```mermaid
flowchart LR
    subgraph worker ["nginx worker"]
        L1["L1 · lua-resty-lrucache<br/>decoded Lua values<br/>l1_size entries, l1_ttl (1 s)"]
    end
    subgraph node ["node (all workers)"]
        L2["L2 · lua_shared_dict gqlgw_cache<br/>JSON text<br/>shm_size, l2_ttl (2 s)"]
    end
    subgraph shared ["every gateway"]
        L3["L3 · memcached (one endpoint)<br/>JSON text<br/>@cache(ttl)"]
    end
    L1 -- miss --> L2 -- miss --> L3 -- miss --> up[(upstream)]
```

| Level | Scope | Holds | TTL of an entry | Optional |
|---|---|---|---|---|
| L1 | one worker | decoded values: no JSON decoding on a hit | `min(ttl, l1_ttl)` | `l1_size = 0` disables it |
| L2 | one node | JSON text | `min(ttl, l2_ttl)` | `shm_size = 0` disables it |
| L3 | all gateways | JSON text | `ttl` from `@cache` | no `[cache.memcached]` section, no L3 |

L1 and L2 are short-lived on purpose. They absorb bursts: the same entity read by many requests
in the same second. memcached holds the real TTL and is shared, so a gateway that just started
benefits from what the others fetched. Without memcached, L2 alone behaves as a per-node cache
capped at `l2_ttl`.

## Read and write paths

```mermaid
sequenceDiagram
    participant R as round (fetch.dispatch)
    participant L1 as L1 (worker)
    participant L2 as L2 (shm)
    participant M as memcached
    participant U as upstream
    R->>L1: get k1 … kn
    L1-->>R: hits
    R->>L2: get the L1 misses
    L2-->>R: hits (decoded, copied to L1)
    R->>M: ONE multi-get for the L2 misses
    M-->>R: hits (copied to L2 and L1)
    R->>U: subrequests for the remaining misses
    U-->>R: responses
    R->>L1: set (decoded)
    R->>L2: set (JSON)
    R--)M: set, pipelined from a timer (after the response)
```

memcached writes never delay the response: they are queued to an `ngx.timer` and sent as one
pipeline.

## What gets cached

- Only `GET` routes with `@cache`, on the field or on the type it returns. The field wins.
- `404` responses and missing batch items: a negative entry, for `min(ttl, negative_ttl)` (5 s).
- Batch items, each under the key of its **single** fetch.
- Never: mutations, failed responses (`5xx`, invalid JSON, timeouts).

```graphql
type User @cache(ttl: 300) { id: ID! name: String! }          # every field returning a User

type Query {
  me: User @fetch(source: "users", path: "/me") @cache(ttl: 30, scope: "PRIVATE")   # overrides
}
```

## Keys

```mermaid
flowchart LR
    p["prefix<br/>gqlgw:"] --- g["generation<br/>of the source"] --- r["source:path<br/>users:/users/42"] --- i["| md5(identity)<br/>PRIVATE only"]
```

`gqlgw:3:users:/users/42`, or `gqlgw:3:users:/me|5d41402a…` with `scope: "PRIVATE"`. A key longer
than 200 bytes is replaced by its MD5 (memcached accepts 250 bytes).

!!! warning "Per-user data needs the PRIVATE scope"
    The key is the upstream request. If `/me` answers differently for each caller, a `PUBLIC`
    entry would serve the first caller's data to everyone. `PRIVATE` adds a hash of the
    `private_header` value (default `Authorization`) to the key. See
    [Auth context forwarding](../examples/auth.md).

## Purge: generations, not deletions

Purging a source increments its **generation**, which is part of every key. Old entries are not
deleted: they are never read again and expire on their own.

```mermaid
sequenceDiagram
    participant O as operator
    participant A as gateway A (admin port)
    participant M as memcached
    participant B as gateway B
    O->>A: POST /gqlgw_purge?source=users
    A->>M: incr gqlgw:gen:users (3 → 4)
    A-->>O: {"source":"users","generation":4}
    Note over A: its L2 knows generation 4 at once
    Note over B: B caches the generation for gen_ttl (1 s)
    B->>M: get gqlgw:gen:users (after gen_ttl)
    M-->>B: 4
    Note over A,B: keys now start with gqlgw:4:users: — the old entries are orphaned
```

Without memcached, the generation lives in the node's L2, so a purge only applies to that node.

## When memcached fails

The cache is **fail-open**. A connection error or a timeout (`[cache.memcached] timeout`, 100 ms by
default) marks memcached as down for `down_ttl` (5 s), for every worker of the node. During that time the
gateway uses L1 and L2 and goes to the upstreams; `gqlgw_memcached_errors_total` counts the
failures. Then it tries again.
