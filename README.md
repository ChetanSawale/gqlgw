# gqlgw — passerelle GraphQL SDL-first pour OpenResty

gqlgw expose une API GraphQL décrite en SDL dont les champs sont résolus par des services REST
derrière nginx. Le N+1 est éliminé par **batching** (façon dataloader, par « rounds ») et par un
**cache de ressources** à trois niveaux (lrucache par worker, `lua_shared_dict`, memcached).

```
schema.graphqls ──gqlc (C99)──► schema.gqlb + schema.routes.lua
                                      │
POST /graphql ─► libgql.so (FFI) : parse → validation → plan
              ─► exécuteur Lua : rounds de fetchs ─► capture_multi ─► /_src/<source>/ ─► proxy_pass
              ─► réponse JSON (ordre des clés de la requête, propagation des null)
```

Conception détaillée et état des jalons : [PLAN.md](PLAN.md). Mesures : [bench/RESULTS.md](bench/RESULTS.md).

## Démarrage rapide

```bash
make deps        # OpenResty 1.31.1.1, Test::Nginx, graphql-js (dans .deps/, sans sudo)
make             # build/libgql.so + build/gqlc
build/gqlc compile -o build/blog.gqlb examples/blog.graphqls
make test        # tests C (ASan/UBSan) + oracle graphql-js + Test::Nginx
make bench       # wrk : naïf vs @batch vs @batch + @cache
make fuzz        # libFuzzer (FUZZ_TIME=60 par cible)
```

Dépendances système : `build-essential`, `libpcre2-dev libssl-dev zlib1g-dev` (pour OpenResty),
`memcached`, `cpanminus`, `clang llvm libclang-rt-dev` (fuzzing), `wrk`, `node` (oracle).

## Le SDL

Les directives de passerelle sont prédéclarées par `gqlc` (inutile de les écrire) et invisibles à
l'introspection.

```graphql
type Query {
  user(id: ID!): User @fetch(source: "users", path: "/users/{args.id}")
  posts(limit: Int = 20): [Post!]! @fetch(source: "posts", path: "/posts?limit={args.limit}")
}

type Post {
  id: ID!
  authorId: ID! @from(key: "author_id")             # clé JSON brute
  author: User
    @fetch(source: "users", path: "/users/{parent.authorId}")
    @batch(path: "/users?ids={keys}", match: "id", max: 50)
}

type User @cache(ttl: 300) { id: ID! name: String! }

type Mutation {
  renameUser(id: ID!, name: String!): User
    @fetch(source: "users", method: "PATCH", path: "/users/{args.id}", body: "{\"name\": {args.name}}")
}

union SearchResult @discriminator(key: "kind") = User | Post
```

| Directive | Rôle |
|---|---|
| `@fetch(source, path, method = "GET", body, select)` | Résout le champ par une requête vers `location /_src/<source>/`. `select: "data.items"` extrait une sous-valeur de la réponse. |
| `@batch(path, match, max = 100, select)` | Sur un `@fetch` GET avec exactement un `{parent.*}` : les clés manquantes d'un round partent en un appel (`{keys}` = clés séparées par `,`), rattachées par la clé JSON `match`. |
| `@cache(ttl, scope = "PUBLIC")` | Sur le champ ou sur le type retourné. GET uniquement. `PRIVATE` ajoute l'identité (`private_header`) à la clé. |
| `@from(key)` | Nom de la clé JSON brute d'un champ (défaut : le nom du champ). |
| `@discriminator(key)` | Clé JSON qui porte le nom du type concret d'une union / interface (défaut `__typename`). |

Templates : `{args.x}`, `{args.x.y}`, `{parent.champ}` (nom GraphQL, traduit en clé brute),
`{keys}` (batch), `{args}` (body : tous les arguments en JSON). Dans un chemin, les valeurs sont
URL-encodées ; dans un body, encodées en JSON. Une accolade non suivie de `args`, `parent` ou
`keys` est littérale (`"{\"name\": {args.name}}"`).

Réponses upstream : 2xx → JSON (204 / corps vide → `null`) ; 404 → `null` ; autre statut, JSON
invalide ou réponse tronquée → erreur de champ (`"Upstream \"users\" returned HTTP 502."`).

## nginx

Voir [conf/nginx.conf.example](conf/nginx.conf.example). L'essentiel :

```nginx
location = /graphql {
    set $gqlgw_path "";
    content_by_lua_block { require("gqlgw").serve() }
}
location /_src/users/ {
    internal;
    proxy_http_version 1.1;
    proxy_set_header Connection "";
    proxy_set_header Accept-Encoding "";
    proxy_pass http://users_svc$gqlgw_path;
}
```

Le chemin upstream est passé dans la variable `$gqlgw_path`, **déjà encodé** : nginx l'envoie tel
quel. Passer par l'URI de la sous-requête décoderait `%2F` (nginx normalise les URI de
sous-requêtes), et un argument pourrait alors modifier le chemin du backend.

Les sous-requêtes héritent des en-têtes du client (`Authorization` compris) : nettoyez ce qui ne
doit pas partir (`Accept-Encoding`, cookies…) dans chaque location `/_src/`.

## Options de `gqlgw.init`

| Option | Défaut | |
|---|---|---|
| `schema` | — | chemin du `.gqlb` (le `.routes.lua` est cherché à côté) |
| `routes`, `lib` | auto | chemin des routes, de `libgql.so` |
| `introspection` | `true` | `__schema` / `__type` |
| `max_query_bytes` / `max_tokens` / `max_depth` | 65536 / 20000 / 32 | limites du document |
| `max_plan_fields` | 20000 | taille du plan d'exécution |
| `max_rounds` / `max_fetches` / `concurrency` | 16 / 1000 / 64 | fetchs par requête GraphQL |
| `max_response_bytes` | 16 Mio | taille d'une réponse upstream |
| `strict_path_args` | `true` | refuse `/`, `.` et `..` comme valeur d'un segment de chemin |
| `doc_cache_size` | 512 | documents validés gardés par worker |
| `cache.shm` / `cache.l2_ttl` | — / 2 s | L2 |
| `cache.l1_size` / `cache.l1_ttl` | 10000 / 1 s | L1 (valeurs décodées, par worker) |
| `cache.memcached` | — | `{ host, port \| socket, timeout, pool_size, keepalive }` (un seul endpoint) |
| `cache.private_header` | `Authorization` | identité de `scope: "PRIVATE"` |
| `cache_negative_ttl` | 5 s | TTL max des 404 / éléments absents d'un batch |
| `debug_headers` | `false` | `X-GQL-Rounds`, `X-GQL-Subrequests`, `X-GQL-Batches`, `X-GQL-Cache`, `X-GQL-Doc-Cache` |

Purge d'une source sur tout le cluster : `require("gqlgw").purge()` derrière une location
d'administration (`POST ?source=users`) incrémente la génération de la source dans memcached.

## GraphQL-over-HTTP

`POST` `application/json` (`query`, `operationName`, `variables`) ou `GET` (queries uniquement,
sinon 405). Avec `Accept: application/graphql-response+json` (ou sans `Accept`), les erreurs de
requête (syntaxe, validation, variables) sont en 400 ; avec `application/json`, en 200.

## Conformité

- Validation : toutes les règles de la spec GraphQL September 2025, messages de graphql-js 17
  (suggestions « Did you mean » omises). Fuzzing différentiel contre graphql-js : aucune
  différence de verdict sur 9 000 documents aléatoires, ~99,7 % de messages identiques.
- Introspection : identique à graphql-js 17 (`getIntrospectionQuery`).
- Exécution : propagation des null, ordre des champs, coercion des scalaires de résultat.

## Limites connues (v1)

- Pas de subscriptions, `@defer` / `@stream`, uploads, fédération, conventions de pagination.
- Scalars custom : valeurs JSON transmises telles quelles.
- Les erreurs de coercion d'arguments liées aux variables sont des erreurs de requête (pas de champ).
- Nombres JSON > 2^53 : précision perdue (cjson) — utilisez des chaînes pour les grands identifiants.
- `{args.x}` dans un body réordonne les clés des objets (`{args}` entier garde l'ordre du schéma).
