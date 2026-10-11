# gqlgw — passerelle GraphQL SDL-first sur OpenResty

Plan validé le 2026-10-11. Ce document est la référence de conception ; il est mis à jour au fil des jalons.
v2 (distribution, configuration TOML, métriques) : section 9. État d'avancement : section 10.

## 1. Objectif

Exposer une API GraphQL décrite en SDL, dont les champs sont résolus par des services REST
derrière nginx, en éliminant le problème N+1 par **batching** (façon dataloader) et **cache par
ressource** (shm local + memcached).

```
          hors ligne                                   runtime (OpenResty)
schema.graphqls ─► gqlc (C99) ─► schema.gqlb ──mmap──► libgql.so (C99, via FFI)
  + directives              └──► schema.routes.lua ─► gqlgw/*.lua
                                                        │
POST /graphql ─► content_by_lua ─► libgql : parse → valide → plan normalisé
                                ─► exécuteur Lua par « rounds »
                                     ├─ L2 : lua_shared_dict (par nœud)
                                     ├─ L3 : memcached (UN endpoint, éventuellement mcrouter)
                                     └─ misses ─► capture_multi ─► location /_src/<source>/ ─► proxy_pass
                                ─► complétion + JSON ordonné
```

**Frontière C / Lua**

- C = tout ce qui ne dépend que de (schéma, requête, variables) : parsing, validation, coercion des
  variables, normalisation (fragments, `@skip/@include`, types concrets), introspection. Pur, sans I/O, fuzzable.
- Lua = tout ce qui dépend des données : sous-requêtes, cache, mapping JSON → GraphQL, coercion des
  résultats, propagation des `null`, sérialisation.

Un seul code C (GNU C99, zéro dépendance) compilé en `gqlc` (CLI) et `libgql.so` (FFI).

## 2. Décisions validées

| # | Décision |
|---|---|
| 1 | Parsing + validation + plan en C99 via FFI (même code que `gqlc`). |
| 2 | Pas de module C nginx. Abandonné le 2026-10-11 : il n'apporterait rien par rapport au FFI. |
| 3 | Routage déclaré dans le SDL ; convention `source: "x"` → `location /_src/x/`. |
| 4 | Clé de cache = ressource REST (`source + chemin`), pas la sélection GraphQL. |
| 5 | **Memcached = un seul endpoint** (peut être un mcrouter) ; le clustering n'est pas notre problème. |
| 6 | Hors périmètre v1 : subscriptions, `@defer/@stream`, uploads, fédération, pagination ; scalars custom en passthrough. |
| 7 | Introspection activée par défaut, désactivable. |

Spec visée : GraphQL **September 2025** (dont `@oneOf`, schema coordinates), GraphQL-over-HTTP (draft).

## 3. Directives de passerelle

Déclarées implicitement par `gqlc` (pas besoin de les écrire), invisibles à l'introspection.

```graphql
directive @fetch(source: String!, path: String!, method: String! = "GET",
                 body: String, select: String) on FIELD_DEFINITION
directive @batch(path: String!, match: String!, max: Int! = 100, select: String) on FIELD_DEFINITION
directive @cache(ttl: Int!, scope: String! = "PUBLIC") on FIELD_DEFINITION | OBJECT | INTERFACE | UNION
directive @from(key: String!) on FIELD_DEFINITION
directive @discriminator(key: String!) on INTERFACE | UNION
```

- Champ sans `@fetch` : valeur lue dans l'objet JSON parent (clé = nom du champ, ou `@from(key:)`).
- Templates : `{args.x}`, `{args.x.y}`, `{parent.champ}` (nom GraphQL, traduit en clé JSON brute par
  `gqlc`), `{keys}` (batch uniquement), `{args}` (body : tous les arguments en JSON).
  Dans `path` les valeurs sont URL-encodées ; dans `body` elles sont encodées en JSON. Une accolade
  non suivie de `args`, `parent` ou `keys` est littérale (bodies JSON).
- `@batch` : exige `@fetch` GET avec exactement un `{parent.*}` (la clé). Les clés manquantes partent en
  un appel `path` (`{keys}` = clés jointes par `,`), réponse = tableau (ou `select`), rattachement par
  la clé JSON `match`. Chaque élément est mis en cache sous la clé de son fetch unitaire.
- `@cache` : sur le champ, sinon sur le type retourné. Sans `@cache`, pas de cache. Seuls les GET sont
  cachés. `scope: "PRIVATE"` ajoute l'identité (en-tête configurable) à la clé.
- Types abstraits : le JSON doit porter `__typename`, ou la clé désignée par `@discriminator`.
- Erreurs upstream : 404 → `null` ; autre non-2xx / JSON invalide → erreur de champ + `null`.

## 4. Composants

### C99 — `src/`

| Fichier | Rôle |
|---|---|
| `arena.*`, `util.*` | arène, buffers, hash FNV-1a, tables de hachage |
| `lexer.*`, `parser.*`, `ast.h` | lexer/parser GraphQL complet (SDL + exécutable), limites dures, positions |
| `schema.*` | modèle de schéma **à plat** (tableaux indexés, sans pointeurs) + lookups |
| `sdl.c` | construction du modèle depuis le SDL, validation du type system et des directives de passerelle |
| `gqlb.c` | écriture / mmap du binaire `.gqlb` (les tableaux du modèle tels quels + table de chaînes) |
| `routes.c` | génération de `schema.routes.lua` |
| `validate.c` | validation des requêtes (section 5 de la spec, toutes les règles) |
| `json.*` | parser JSON (variables) + écriture JSON |
| `plan.c` | sélection d'opération, coercion des variables/arguments, normalisation → plan plat |
| `introspect.c` | rendu JSON direct de `__schema` / `__type` |
| `gql.h`, `api.c` | API publique (FFI) |
| `gqlc.c` | CLI : `compile`, `check`, `plan` |

API FFI (simplifiée) :

```c
gql_schema *gql_schema_open(const char *path, gql_error *err);
gql_doc    *gql_doc_parse_validate(const gql_schema*, const char *q, size_t n,
                                   const gql_limits*, gql_error *err);
gql_plan   *gql_plan_build(const gql_schema*, const gql_doc*, const char *op, size_t opn,
                           const char *vars, size_t varsn, const gql_limits*, gql_error *err);
```

Le plan est un ensemble de tableaux d'`int32` (sélections par type concret, champs, cas des types
abstraits) lus sans copie depuis Lua. Les documents validés sont mis en cache (LRU par worker,
clé = texte de la requête).

### Lua — `lua/gqlgw/`

| Module | Rôle |
|---|---|
| `init.lua` | `init(opts)` (init_by_lua), `serve()` (content_by_lua), `log()`, `purge()` |
| `ffi.lua`, `schema.lua` | binding FFI, tables Lua dérivées du schéma + routes |
| `http.lua` | GraphQL-over-HTTP (POST JSON, GET queries, `application/graphql-response+json`, codes) |
| `exec.lua` | résolution par rounds, complétion, propagation des `null`, sérialisation ordonnée |
| `fetch.lua` | templates, dédoublonnage, batching, `capture_multi`, mapping des réponses |
| `cache.lua` | L2 shm + L3 memcached (`get` multi-clés, écritures async, fail-open, générations, purge) |

Bibliothèques utilisées : uniquement celles livrées avec OpenResty (cjson, lrucache, memcached,
resty.string, string.buffer).

## 5. Jalons

| # | Livrable | Critère de sortie |
|---|---|---|
| M0 | Squelette, Makefile, OpenResty 1.31.1.1 dans `.deps/`, memcached local, Test::Nginx | `make test` vert |
| M1 | Lexer + parser, arène, limites | corpus parsé ; fuzzing ASan/UBSan sans crash |
| M2 | `gqlc` : modèle, validation SDL + directives, `.gqlb` + `routes.lua` | SDL invalides rejetés avec ligne/col |
| M3 | Validation des requêtes | mêmes verdicts que graphql-js (corpus + fuzz différentiel) |
| M4 | Variables, plan, introspection, API FFI | introspection équivalente à graphql-js |
| M5 | Exécuteur Lua sans I/O, sérialisation, erreurs | tests de conformité d'exécution |
| M6 | `@fetch` / `@batch` via `capture_multi`, mutations | 50 posts + auteurs = 2 appels upstream |
| M7 | Cache shm + memcached, purge, cache négatif | 2e requête = 0 appel upstream |
| M8 | Durcissement : limites, timeouts, erreurs upstream, bench `wrk` vs naïf | rapport de bench |
| **v2** | **Distribution et configuration (section 9)** | |
| M9 | Parseur TOML 1.1 en C99 (`src/toml.*`, lié à `gqlc` uniquement, l'ABI FFI ne change pas) | 100 % du corpus `toml-test` v2.2.0 (liste `files-toml-1.1.0`, valid + invalid) ; fuzzing ASan/UBSan sans crash |
| M10 | `gqlgw.toml` : modèle typé, `gqlc compile -c` (vérifications croisées SDL ↔ sources), `gqlc render` (interpolation, `gqlgw.lua`), `gqlgw.init` lit la conf rendue | golden files ; erreurs avec ligne/col ; variable absente = refus de démarrer |
| M11 | Génération nginx : mode par défaut et `--locations-only`, upstreams `resolve`, en-têtes allow/deny, TLS, `sources.d` | `openresty -t` sur toutes les confs golden ; Test::Nginx sur la conf générée (en-têtes reçus par le mock, TLS auto-signé accepté/refusé) |
| M12 | Métriques Prometheus métier (port 8191, `/gqlgw_metrics`) | format vérifié par les tests ; surcoût < 5 % sur le bench `author` |
| M13 | Image Docker, entrypoint, `make install`, LICENSE BSD-2, `examples/docker/`, README | `make image` et `make docker-test` verts : requête, batch, cache memcached, métriques, purge, refus de démarrer sans variable |

## 6. Tests

- C : `tests/c/` (harness maison), golden files, libFuzzer + ASan/UBSan (`tests/fuzz/`).
- Oracle : graphql-js via node (`tests/oracle/`, outil de dev uniquement).
- Intégration : Test::Nginx (`t/`), backends mock = `server {}` nginx qui comptent leurs hits.
- Bench : `wrk` (`bench/`).

## 7. Dépendances

apt : `memcached libmemcached-tools cpanminus clang llvm libclang-rt-dev wrk` (+ `gcovr valgrind`).
Hors apt, installés par `make deps` dans `.deps/` sans sudo : OpenResty 1.31.1.1 (sources),
Test::Nginx (cpanm local-lib), graphql-js (npm, dev).

v2 : `docker.io docker-buildx docker-compose-v2` (apt). Le corpus `toml-test` v2.2.0 est téléchargé par
`make deps` : on n'utilise que ses fichiers, avec un runner maison, donc pas besoin de Go.

## 8. Risques

- Validation spec = le plus gros morceau → oracle graphql-js.
- Bug C = crash d'un worker → fuzzing, limites dures, ASan en CI.
- cjson : entiers > 2^53, tableaux vides (`decode_array_with_array_mt`), ordre des clés (sérialiseur maison).
- Sous-requêtes bufferisées en mémoire et héritant des en-têtes client → plafonds de taille, en-têtes nettoyés.
- v2 : collisions dans `http {}` avec une install OpenResty existante (`init_by_lua` unique, noms) → tout
  est préfixé `gqlgw_`, `[nginx] init = false`, pas de `lua_package_path`.
- v2 : les secrets interpolés sont écrits dans la conf rendue → `/tmp/gqlgw` (tmpfs), mode 0600.
- v2 : avec `proxy_pass` vers un upstream nommé, `Host` et le SNI vaudraient `gqlgw_<source>` →
  `proxy_set_header Host` et `proxy_ssl_name` sont générés depuis l'URL de la source.

## 9. v2 : distribution et configuration

Décidé et implémenté le 2026-10-11 (M9 à M13, état en section 10).

### Décisions

| # | Décision |
|---|---|
| 8 | Distribution = image Docker construite sur `openresty/openresty:1.31.1.1-resolute` (glibc ; pas d'alpine, musl est trop lente). Image taggée en local uniquement, rien n'est publié. Docker, pas podman. |
| 9 | Le SDL est compilé **au build** de l'image utilisateur. Le gateway ne démarre jamais sur un `.graphqls` non compilé. |
| 10 | Config dans `gqlgw.toml` (TOML 1.1, parseur C99 maison), qui porte le maximum de la config du gateway. Le SDL garde `@fetch(source, path)` sur les champs : pas de mode « tout en config ». |
| 11 | `${VAR}` est accepté dans toute valeur string du TOML. `gqlc` fait le rendu au démarrage du conteneur ; le Lua lit la conf rendue, jamais l'environnement. |
| 12 | `gqlc` génère un fichier à inclure dans `http {}`, avec son propre `server`, jamais le `nginx.conf` complet. Le flag `--locations-only` produit deux fichiers : le bloc http, et les locations à inclure dans un `server` existant. |
| 13 | Une seule URL par source : le load balancing derrière (service, replica set) n'est pas notre affaire. Les en-têtes envoyés aux backends passent par une allowlist ou une denylist par source. TLS vérifié par défaut. Réglages libres via `sources.d/<source>/*.conf`. |
| 14 | Métriques Prometheus métier sur un port dédié (8191, `/gqlgw_metrics`). Pas de plafond sur `operationName`, liste d'exclusion optionnelle par regex. Pas de `/healthz`. |
| 15 | `make install` maintenant, `.deb` plus tard. Licence BSD-2-Clause. |

TOML 1.1.0 est sorti le 2025-12-24. On le vise plutôt que 1.0 : ses tables inline multilignes sont
utiles pour `set_headers`, et `toml-test` v2.2.0 le couvre.

### Parcours utilisateur

```dockerfile
FROM gqlgw:0.2.0
COPY schema.graphqls gqlgw.toml /etc/gqlgw/
RUN gqlc compile -c /etc/gqlgw/gqlgw.toml /etc/gqlgw/schema.graphqls
# optionnel : COPY sources.d/ /etc/gqlgw/sources.d/   et   COPY conf.d/ /etc/gqlgw/conf.d/
```

```bash
docker run -p 8080:8080 -p 8191:8191 \
  -e USERS_URL=http://users:8080/api/v1 -e MEMCACHED_HOST=memcached mon-gateway
```

| Moment | Commande | Ce qu'elle fait |
|---|---|---|
| build | `gqlc compile -c gqlgw.toml schema.graphqls` | compile le SDL (`.gqlb` + `routes.lua`). Vérifie le TOML : syntaxe, clés inconnues (erreur avec ligne/col), types, syntaxe des `${…}`. Chaque `@fetch(source:)` doit avoir sa `[sources.x]` ; une source inutilisée donne un avertissement |
| démarrage | `gqlc render -c gqlgw.toml [--locations-only] -o /tmp/gqlgw` | interpole l'environnement (variable absente sans défaut = refus de démarrer), valide les valeurs (URL, durées, tailles), écrit `gqlgw.conf` (nginx) et `gqlgw.lua` (conf runtime du Lua, mode 0600). L'entrypoint lance ensuite `openresty -t` puis `exec openresty` |

Hors Docker, les commandes sont les mêmes : `gqlc render`, puis `nginx -s reload`.

### `gqlgw.toml`

```toml
[gateway]
listen = 8080                          # ou "0.0.0.0:8080"
server_name = "_"
schema = "schema.gqlb"                 # relatif au fichier TOML
introspection = true
strict_path_args = true
debug_headers = false
max_body = "1m"
access_log = "/dev/stdout"             # ou "off"

[limits]                               # mêmes défauts que les options actuelles de gqlgw.init
query_bytes = 65536
depth = 32
# tokens, errors, plan_fields, memory, rounds, fetches, concurrency, response_bytes

[cache]
shm_size = "128m"                      # "0" : pas de L2
l1_size = 10000
l1_ttl = 1
l2_ttl = 2
negative_ttl = 5
private_header = "Authorization"

[cache.memcached]                      # section absente : pas de L3
host = "${MEMCACHED_HOST:-memcached}"
port = 11211
timeout = 50                           # ms

[nginx]
init = true                            # false : appeler gqlgw.init() dans son propre init_by_lua
resolver = "auto"                      # lit /etc/resolv.conf ; ou ["10.0.0.2"]
resolver_valid = "10s"

[metrics]
enabled = true
port = 8191
path = "/gqlgw_metrics"
allow = []                             # CIDR ; vide : pas de filtrage
exclude_operations = ["^IntrospectionQuery$"]

[purge]                                # servie sur le port des métriques
enabled = true
path = "/gqlgw_purge"

[sources.users]
url = "${USERS_URL:-http://users:8080/api/v1}"   # http(s)://hôte[:port][/préfixe], sans {…}
connect_timeout = "1s"
read_timeout = "3s"
keepalive = 64
forward_headers = ["authorization", "accept-language"]  # allowlist (défaut : ["authorization"])
# drop_headers = ["cookie"]                              # ou denylist, exclusive de forward_headers
set_headers = { X-Api-Key = "${USERS_API_KEY}" }
tls_verify = true                      # https : vérification du certificat + SNI
ca = "/etc/ssl/certs/ca-certificates.crt"
```

Interpolation : `${VAR}` (erreur si la variable est absente), `${VAR:-défaut}`, `${VAR:?message}`,
et `$${` pour un `${` littéral. Elle s'applique aux valeurs string. Un champ typé peut s'écrire
`"${PORT}"` : il est converti selon le type attendu.

### Conf nginx générée

Par défaut, `gqlc render` écrit un seul fichier, `gqlgw.conf`, à inclure dans `http {}`. Il contient :

- `lua_shared_dict gqlgw_cache` et `gqlgw_metrics` ; `init_by_lua_block { require("gqlgw").init("/tmp/gqlgw/gqlgw.lua") }`,
  sauf avec `[nginx] init = false` ;
- un `upstream gqlgw_<source>` par source : `zone`, `resolver` (placé dans l'upstream, donc pas de
  conflit avec un resolver global), `server hôte:port resolve`, `keepalive` ;
- le `server` public : `listen`, `location = /graphql` (`set $gqlgw_path ""`, `set $gqlgw_ctype ""`,
  variables de log) et une `location /_src/<source>/` par source. Chaque location `/_src/` porte :
  - `internal` ;
  - les en-têtes : en allowlist, `proxy_pass_request_headers off` puis un `proxy_set_header` par
    en-tête autorisé ; en denylist, `proxy_set_header X ""` pour chaque en-tête refusé. `Accept-Encoding`
    est toujours vidé ; `Content-Type` des bodies vient de `$gqlgw_ctype` ; `Host` est pris dans l'URL ;
  - les timeouts ;
  - le TLS : `proxy_ssl_verify`, `proxy_ssl_server_name`, `proxy_ssl_name`, `proxy_ssl_trusted_certificate` ;
  - `include /etc/gqlgw/sources.d/<source>/*.conf;` ;
  - `proxy_pass http(s)://gqlgw_<source><préfixe>$gqlgw_path` ;
- le `server` d'admin, sur le port des métriques : `/gqlgw_metrics`, `/gqlgw_purge`, `allow` / `deny`.

`--locations-only` écrit deux fichiers à la place : `gqlgw.http.conf` (shm, init, upstreams, `server`
d'admin) et `gqlgw.locations.conf` (à inclure dans un `server` existant).

Il n'y a pas de `lua_package_path` : les modules sont installés dans le `site/lualib` d'OpenResty, déjà
dans son chemin de recherche. Tous les noms sont préfixés `gqlgw_`.

### Métriques

| Métrique | Labels |
|---|---|
| `gqlgw_requests_total` | `operation`, `type` (query, mutation), `result` (ok, partial, request_error) |
| `gqlgw_request_duration_seconds` (histogramme) | `operation` |
| `gqlgw_errors_total` | `kind` (syntax, validation, variables, field, upstream) |
| `gqlgw_field_requests_total` | `coordinate` (`Type.champ`) |
| `gqlgw_upstream_requests_total` | `source`, `status` (2xx, 404, 4xx, 5xx, timeout) |
| `gqlgw_upstream_duration_seconds` (histogramme) | `source` |
| `gqlgw_batches_total`, `gqlgw_batch_size` (histogramme) | `coordinate` |
| `gqlgw_cache_total` | `source`, `level` (l1, l2, l3, miss) |
| `gqlgw_memcached_errors_total` | — |
| `gqlgw_rounds` (histogramme) | — |
| `gqlgw_doc_cache_total` | `result` (hit, miss) |
| `gqlgw_info` | `version`, `schema_hash` |

- `operation` est l'`operationName` tel quel, ou `__anonymous` sans nom. Un nom qui correspond à une
  regex de `exclude_operations` est compté sous `__excluded`, donc les totaux restent justes. Les
  regex sont des PCRE via `ngx.re`, compilées dans l'init : une regex invalide fait échouer
  `openresty -t`. Pas de plafond.
- `coordinate` est borné par la taille du schéma. C'est la bonne métrique pour savoir quand on peut
  retirer un champ `@deprecated`.
- Les compteurs sont agrégés par worker en Lua, puis vidés dans la shm `gqlgw_metrics` par un timer
  d'une seconde, démarré à la première requête du worker (pas d'`init_worker_by_lua` à partager).
  Le format texte Prometheus est écrit par nous, sans bibliothèque tierce.
- Latence par source : `capture_multi` ne la donne pas. Il faut la remonter depuis la location
  `/_src/` (`$upstream_response_time` lu dans un `header_filter_by_lua`, puis transmis au parent par
  un en-tête interne). À valider en M12.

### Image et installation

- `Dockerfile` multi-stage :
  - l'étape de build part de `openresty/openresty:1.31.1.1-resolute`, ajoute gcc et make, et produit
    `libgql.so` et `gqlc` ;
  - l'image finale part de la même base et contient `gqlc` (`/usr/local/bin`), `libgql.so`
    (`/usr/local/lib/gqlgw/`), les modules Lua (`/usr/local/openresty/site/lualib/gqlgw/`), un
    `nginx.conf` minimal (`include /tmp/gqlgw/gqlgw.conf; include /etc/gqlgw/conf.d/*.conf;`) et
    l'entrypoint.
- Entrypoint : `gqlc render`, puis `openresty -t`, puis `exec openresty -g 'daemon off;'`. Logs sur
  stdout et stderr. Compatible `--read-only` avec un tmpfs sur `/tmp`.
- `make image` crée les tags locaux `gqlgw:<version>` et `gqlgw:latest`. Rien n'est publié.
- `make install PREFIX=/usr/local OPENRESTY_PREFIX=/usr/local/openresty DESTDIR=` installe aux mêmes
  emplacements, hors Docker.
- `examples/docker/` contient un Dockerfile utilisateur, un `gqlgw.toml` et un `compose.yaml`
  (gateway, memcached, backend mock). Il sert aussi de test d'intégration (`make docker-test`).

## 10. État d'avancement (2026-10-11)

| # | Statut | Résultat |
|---|---|---|
| M0 | fait | `make deps` (OpenResty 1.31.1.1 compilé dans `.deps/`, Test::Nginx 0.32, graphql-js 17.0.2), `make test` |
| M1 | fait | lexer/parser complets ; fuzzing libFuzzer + ASan/UBSan : **1 bug trouvé et corrigé** (lecture hors buffer sur `\u` tronqué en fin d'entrée, test de non-régression ajouté), puis 7,5 M exécutions sans crash |
| M2 | fait | `gqlc compile` : modèle à plat, validation du type system (messages graphql-js 17), directives de passerelle, `.gqlb` mmap avec vérification de toutes les bornes, `routes.lua` |
| M3 | fait | 46/46 requêtes du corpus identiques à graphql-js ; fuzz différentiel 9 000 documents : 0 écart de verdict, ~99,7 % de messages identiques |
| M4 | fait | variables, plan mémoïsé (DAG), introspection identique à graphql-js, API FFI |
| M5 | fait | exécuteur par rounds, complétion, propagation des null, sérialisation ordonnée |
| M6 | fait | `@fetch` / `@batch` via `capture_multi`, mutations sérielles ; 20 posts + auteurs = 2 sous-requêtes |
| M7 | fait | L1 lrucache (valeurs décodées) + L2 shm + L3 memcached (multi-get, écritures pipelinées en timer, fail-open), cache négatif, scope PRIVATE, purge par génération |
| M8 | fait | limites (taille, tokens, profondeur, plan, rounds, fetchs, réponse), timeouts → erreurs de champ, bench `wrk` (bench/RESULTS.md) |
| M9 | fait | `src/toml.c` (TOML 1.1.0, ~1 000 lignes) : 214/214 valides et 467/467 invalides du corpus `toml-test` v2.2.0 ; fuzzing 6,1 M exécutions sans crash |
| M10 | fait | `src/config.c`, `src/render.c`, `gqlc compile -c` / `gqlc render` : 14 cas (erreurs ligne:colonne, rendus de référence, `openresty -t`) ; fuzzing de la config : **2 bugs trouvés et corrigés** (rendu d'une source non résolue, champ invalide sans valeur par défaut), puis 3,7 M exécutions sans crash |
| M11 | fait | conf générée testée par Test::Nginx (`t/006`, `t/007`) : allowlist, denylist, `set_headers` avec `$`, bodies, TLS vérifié et refusé, résolution DNS, cache, purge, `server.d` |
| M12 | fait | `lua/gqlgw/metrics.lua`, 13 familles (`t/008`) ; surcoût dans le bruit de mesure (−1,5 % / +1,3 % en médiane, bench/RESULTS.md) |
| M13 | fait | Dockerfile multi-stage (279 Mo, base `openresty/openresty:1.31.1.1-resolute`), utilisateur non-root, `--read-only` ; `make install` ; LICENSE BSD-2 ; `examples/docker/` (compose) ; `make docker-test` : 15 vérifications (deux passerelles + memcached + mock) |

### Écarts par rapport au plan initial

- **Chemin upstream via `$gqlgw_path`** au lieu de l'URI de la sous-requête : nginx décode les URI de
  sous-requêtes (`%2F` → `/`), ce qui permettait à un argument de modifier le chemin du backend.
  La location devient `proxy_pass http://svc$gqlgw_path;` et la valeur est envoyée telle quelle.
  En plus, `strict_path_args` (défaut) refuse `/`, `.` et `..` dans un segment de chemin.
- **Cache L1 ajouté** (lrucache par worker, valeurs déjà décodées) : sans lui, le chemin « cache »
  était plus lent que le batch seul (décodage JSON de chaque entité, appels non compilés par le JIT).
- **Messages alignés sur graphql-js 17** (pas 16) : `@deprecated` sur les directives,
  `includeDeprecated: Boolean! = false`, `MaxIntrospectionDepth`, « finite value » des input objects.

### Écarts de la v2 par rapport au plan

- **Répertoire d'exécution `/tmp/gqlgw`** au lieu de `/run/gqlgw` : Docker monte un tmpfs sur
  `/run` en 755 root (inutilisable par l'utilisateur `gqlgw`), et sur `/tmp` en 1777. L'idiome
  `--read-only --tmpfs /tmp` fonctionne donc tel quel.
- **`$${` donne `${` littéral** (au lieu de `$$` → `$`) : un `$` isolé reste littéral, ce qui ne
  casse ni les regex (`^Foo$`) ni les clés d'API.
- **`Accept: application/json`** envoyé aux backends par défaut (sauf `Accept` dans
  `forward_headers` ou `set_headers`), et les en-têtes gérés par gqlgw (`Host`, `Content-Type`,
  `Content-Length`, `Accept-Encoding`, `Connection`…) refusés dans les listes.
- **Temps upstream par source** remonté par `more_set_headers "X-Gqlgw-Upstream-Time:
  $upstream_header_time"` dans les locations générées (headers-more s'applique aux sous-requêtes),
  sans phase Lua supplémentaire.
- **Vérification des sources à l'init Lua** : `gqlgw.init` refuse un schéma dont une source n'a pas
  de `[sources.x]`, donc `openresty -t` aussi (l'entrypoint l'exécute avant de démarrer).
- **Resolver dans la location `/graphql`** quand memcached est désigné par un nom DNS (les cosockets
  résolvent avec le resolver de la location, pas celui des upstreams).
- **`gql_plan_operation_name`** ajouté à l'API FFI (additif, ABI inchangée) pour le label
  `operation` des métriques.

### Divergences assumées avec graphql-js

- Pas de suggestions « Did you mean » (équivalent de `hideSuggestions: true`).
- Les valeurs des arguments de directives appliquées dans le SDL sont validées (graphql-js ne le fait pas).
- Sur des documents pathologiques (> 100 erreurs, fragments cycliques imbriqués), l'ordre et les doublons
  de certains messages de fusion de champs peuvent différer ; les verdicts restent identiques.
