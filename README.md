# gqlgw — passerelle GraphQL SDL-first pour OpenResty

gqlgw expose une API GraphQL décrite en SDL dont les champs sont résolus par des services REST
derrière nginx. Le N+1 est éliminé par **batching** (façon dataloader, par « rounds ») et par un
**cache de ressources** à trois niveaux (lrucache par worker, `lua_shared_dict`, memcached).

```
schema.graphqls ──gqlc compile (build)──► schema.gqlb + schema.routes.lua
gqlgw.toml ───────gqlc render (démarrage)─► gqlgw.conf (nginx) + gqlgw.lua (runtime)
                                               │
POST /graphql ─► libgql.so (FFI) : parse → validation → plan
              ─► exécuteur Lua : rounds de fetchs ─► capture_multi ─► /_src/<source>/ ─► upstream
              ─► réponse JSON (ordre des clés de la requête, propagation des null)
```

Conception détaillée et état des jalons : [PLAN.md](PLAN.md). Mesures : [bench/RESULTS.md](bench/RESULTS.md).
Licence : BSD-2-Clause.

## Démarrage rapide (Docker)

```bash
make image                   # image de base gqlgw:<version> et gqlgw:latest
cd examples/docker
docker compose up --build    # passerelle d'exemple + memcached + backend mock
curl -s localhost:8080/graphql -H 'Content-Type: application/json' \
     -d '{"query":"{ posts(limit: 3) { title author { name } } }"}'
curl -s localhost:8191/gqlgw_metrics
```

Une passerelle, c'est une image construite `FROM gqlgw` avec votre schéma et votre configuration :

```dockerfile
FROM gqlgw:0.2.0
COPY schema.graphqls gqlgw.toml /etc/gqlgw/
RUN gqlc compile -c /etc/gqlgw/gqlgw.toml /etc/gqlgw/schema.graphqls
```

```bash
docker build -t my-gateway .
docker run --read-only --tmpfs /tmp -p 8080:8080 -p 8191:8191 \
    -e USERS_URL=http://users:8080 -e POSTS_URL=http://posts:8080 my-gateway
```

- **Au build**, `gqlc compile -c` compile le SDL et vérifie la configuration (syntaxe, clés,
  types, chaque `@fetch(source:)` a sa `[sources.x]`). Un schéma invalide casse le `docker build`.
- **Au démarrage**, l'entrypoint lance `gqlc render` (interpolation de l'environnement, validation
  des valeurs), puis `openresty -t` (qui charge le schéma), puis `openresty`. Une variable
  manquante empêche le démarrage avec un message clair.

L'image tourne en utilisateur `gqlgw` (uid 10001) et n'écrit que dans `/tmp/gqlgw` : elle
fonctionne avec `--read-only --tmpfs /tmp`. Ports exposés : 8080 (GraphQL), 8191 (métriques et
purge). Points d'extension : `/etc/gqlgw/conf.d/*.conf` (niveau `http`), `/etc/gqlgw/server.d/*.conf`
(dans le `server` gqlgw), `/etc/gqlgw/sources.d/<source>/*.conf` (dans la location d'une source).

## Installation classique (nginx/OpenResty existant)

```bash
make && sudo make install   # PREFIX=/usr/local, OPENRESTY_PREFIX=/usr/local/openresty
gqlc compile -c /etc/gqlgw/gqlgw.toml /etc/gqlgw/schema.graphqls
gqlc render -c /etc/gqlgw/gqlgw.toml -o /etc/gqlgw/run
```

puis, dans le bloc `http {}` de nginx : `include /etc/gqlgw/run/gqlgw.conf;` et `nginx -s reload`.
Avec `gqlc render --locations-only`, deux fichiers : `gqlgw.http.conf` (dans `http {}`) et
`gqlgw.locations.conf` (dans un `server {}` existant, pour partager son TLS et son domaine).
Les modules Lua sont installés dans `site/lualib` d'OpenResty : pas de `lua_package_path` à écrire.
Si vous avez déjà un `init_by_lua`, mettez `[nginx] init = false` et appelez
`require("gqlgw").init("/etc/gqlgw/run/gqlgw.lua")` dans le vôtre.

## Développement

```bash
make deps        # OpenResty 1.31.1.1, Test::Nginx, graphql-js, toml-test (dans .deps/, sans sudo)
make             # build/libgql.so + build/gqlc
make test        # C (ASan/UBSan), toml-test, config, oracle graphql-js, Test::Nginx
make docker-test # image + passerelle d'exemple + memcached + mock
make bench       # wrk : naïf vs @batch vs @batch + @cache
make fuzz        # libFuzzer (FUZZ_TIME=60 par cible)
```

Dépendances système : `build-essential`, `libpcre2-dev libssl-dev zlib1g-dev` (pour OpenResty),
`memcached`, `cpanminus`, `clang llvm libclang-rt-dev` (fuzzing), `wrk`, `node` (oracle),
`python3` (toml-test), `docker.io docker-buildx docker-compose-v2` (image).

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
| `@fetch(source, path, method = "GET", body, select)` | Résout le champ par une requête vers la source (`[sources.<source>]`). `select: "data.items"` extrait une sous-valeur de la réponse. |
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

## `gqlgw.toml`

TOML 1.1 (parseur maison, 100 % du corpus `toml-test`). Toute valeur string peut utiliser
`${VAR}` (erreur si absente), `${VAR:-défaut}`, `${VAR:?message}` ; `$${` donne `${` littéral.
Un champ entier ou booléen peut s'écrire `"${PORT}"`. Les chemins relatifs (`schema`, `routes`,
`lib`, `ca`, `socket`) partent du dossier du fichier. Une clé inconnue est une erreur (ligne:colonne).

| Section | Clé | Défaut | |
|---|---|---|---|
| `[gateway]` | `listen` | `8080` | paramètres de `listen` nginx (`"0.0.0.0:8443 ssl"` + certificats dans `server.d`) |
| | `server_name` / `path` | `"_"` / `"/graphql"` | |
| | `schema` / `routes` | `"schema.gqlb"` / à côté | écrits par `gqlc compile` |
| | `lib` | `/usr/local/lib/gqlgw/libgql.so` | |
| | `access_log` | hérité | chemin (format `gqlgw` : rounds, sous-requêtes, cache) ou `"off"` |
| | `introspection` / `debug_headers` | `true` / `false` | |
| | `strict_path_args` | `true` | refuse `/`, `.` et `..` comme valeur d'un segment de chemin |
| | `max_body` | `"1m"` | taille d'une requête GraphQL |
| `[limits]` | `query_bytes` / `tokens` / `depth` / `errors` | 65536 / 20000 / 32 / 100 | document |
| | `plan_fields` / `memory` | 20000 / `"16m"` | plan d'exécution |
| | `rounds` / `fetches` / `concurrency` | 16 / 1000 / 64 | fetchs par requête GraphQL |
| | `response_bytes` | `"16m"` | réponse upstream |
| `[cache]` | `shm_size` | `"64m"` | L2 ; `0` : pas de L2 |
| | `l1_size` / `l1_ttl` / `l2_ttl` | 10000 / 1 / 2 | L1 par worker (valeurs décodées), L2 (s) |
| | `negative_ttl` / `gen_ttl` / `down_ttl` | 5 / 1 / 5 | 404, génération de purge, memcached en panne (s) |
| | `private_header` / `prefix` | `"Authorization"` / `"gqlgw:"` | |
| `[cache.memcached]` | `host` + `port`, ou `socket` | `127.0.0.1:11211` | un seul endpoint (peut être un mcrouter) ; section absente : pas de L3 |
| | `timeout` / `pool_size` / `keepalive` | 100 ms / 64 / 60000 ms | |
| `[nginx]` | `init` | `true` | `false` : appeler `gqlgw.init` dans votre `init_by_lua` |
| | `resolver` / `resolver_valid` / `resolver_ipv6` | `"auto"` / `"10s"` / `true` | `auto` : `/etc/resolv.conf` au démarrage |
| `[metrics]` | `enabled` / `port` / `path` | `true` / 8191 / `"/gqlgw_metrics"` | server d'admin |
| | `allow` | `[]` | IP / CIDR autorisés (vide : pas de filtre) |
| | `exclude_operations` | `[]` | regex PCRE : opérations comptées sous `__excluded` |
| | `shm_size` / `buckets` | `"8m"` / 5 ms…10 s | |
| `[purge]` | `enabled` / `path` | `true` / `"/gqlgw_purge"` | sur le port d'admin : `POST ?source=users` |
| `[sources.<nom>]` | `url` | requis | `http(s)://hôte[:port][/préfixe]` |
| | `connect_timeout` / `send_timeout` / `read_timeout` | `"1s"` / `"5s"` / `"5s"` | |
| | `keepalive` / `keepalive_timeout` | 64 / `"60s"` | connexions par worker |
| | `forward_headers` | `["Authorization"]` | allowlist des en-têtes client transmis |
| | `drop_headers` | — | denylist (tout le reste est transmis) ; exclusif avec `forward_headers` |
| | `set_headers` | `{}` | en-têtes ajoutés (valeurs littérales, `$` compris) |
| | `tls_verify` / `ca` | `true` / bundle système | |
| | `tls_name` / `host_header` | hôte de l'URL | SNI et `Host` |

Ce que génère `gqlc render` pour chaque source : un `upstream gqlgw_<nom>` (`server ... resolve`
pour un nom DNS, keepalive) et une `location /_src/<nom>/` `internal`. Le chemin upstream passe
par `$gqlgw_path`, déjà encodé : nginx l'envoie tel quel (passer par l'URI de la sous-requête
décoderait `%2F`, et un argument pourrait alors modifier le chemin du backend). `Accept-Encoding`
est vidé, `Accept: application/json` ajouté, `Content-Type` posé seulement quand il y a un body.

## Métriques (`/gqlgw_metrics`, format Prometheus)

| Métrique | Labels |
|---|---|
| `gqlgw_requests_total` | `operation`, `type`, `result` (`ok`, `partial`, `request_error`, `http_error`) |
| `gqlgw_request_duration_seconds` (histogramme) | `operation` |
| `gqlgw_errors_total` | `kind` (`document`, `operation`, `field`) |
| `gqlgw_field_requests_total` | `coordinate` (`Type.champ`) : pour savoir quand retirer un champ `@deprecated` |
| `gqlgw_upstream_requests_total` | `source`, `status` (`2xx`, `3xx`, `404`, `4xx`, `5xx`) |
| `gqlgw_upstream_duration_seconds` (histogramme) | `source` (temps jusqu'aux en-têtes de la réponse) |
| `gqlgw_batches_total`, `gqlgw_batch_size` (histogramme) | `coordinate` |
| `gqlgw_cache_total` | `source`, `level` (`l1`, `l2`, `l3`, `miss`) |
| `gqlgw_memcached_errors_total`, `gqlgw_rounds`, `gqlgw_doc_cache_total`, `gqlgw_info` | |

`operation` vaut l'`operationName` exécuté, `__anonymous` sans nom, `__excluded` s'il correspond à
une regex de `exclude_operations`, `__unknown` pour une requête rejetée avant d'être lue. Ce label
n'est pas plafonné : chaque nom d'opération envoyé par un client crée des séries. Les compteurs
sont agrégés par worker et publiés dans une shm chaque seconde.

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
- TOML : 214/214 documents valides et 467/467 invalides du corpus `toml-test` 1.1.0.

## API Lua (sans `gqlc render`)

`require("gqlgw").init(opts)` accepte aussi une table (`schema`, `lib`, `cache = { shm, memcached }`,
`introspection`, `max_*`, `metrics = { shm, exclude_operations }`…) avec des locations écrites à
la main : voir [conf/nginx.conf.example](conf/nginx.conf.example). La configuration TOML est la voie
recommandée : elle génère les locations avec leurs garde-fous (`internal`, `$gqlgw_path`, en-têtes).

## Limites connues

- Pas de subscriptions, `@defer` / `@stream`, uploads, fédération, conventions de pagination.
- Scalars custom : valeurs JSON transmises telles quelles.
- Les erreurs de coercion d'arguments liées aux variables sont des erreurs de requête (pas de champ).
- Nombres JSON > 2^53 : précision perdue (cjson) — utilisez des chaînes pour les grands identifiants.
- `{args.x}` dans un body réordonne les clés des objets (`{args}` entier garde l'ordre du schéma).
- Les métriques de requêtes sont enregistrées dans la phase log : elles demandent le
  `log_by_lua` généré par `gqlc render`.
