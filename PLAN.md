# gqlgw — passerelle GraphQL SDL-first sur OpenResty

Plan validé le 2026-10-11. Ce document est la référence de conception ; il est mis à jour au fil des jalons.
État d'avancement : section 9.

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
| 2 | Pas de module C nginx en v1 (M9 optionnel). |
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
| M9 | *(optionnel)* module C nginx (directives vérifiées à `nginx -t`) | — |

## 6. Tests

- C : `tests/c/` (harness maison), golden files, libFuzzer + ASan/UBSan (`tests/fuzz/`).
- Oracle : graphql-js via node (`tests/oracle/`, outil de dev uniquement).
- Intégration : Test::Nginx (`t/`), backends mock = `server {}` nginx qui comptent leurs hits.
- Bench : `wrk` (`bench/`).

## 7. Dépendances

apt : `memcached libmemcached-tools cpanminus clang llvm libclang-rt-dev wrk` (+ `gcovr valgrind`).
Hors apt, installés par `make deps` dans `.deps/` sans sudo : OpenResty 1.31.1.1 (sources),
Test::Nginx (cpanm local-lib), graphql-js (npm, dev).

## 8. Risques

- Validation spec = le plus gros morceau → oracle graphql-js.
- Bug C = crash d'un worker → fuzzing, limites dures, ASan en CI.
- cjson : entiers > 2^53, tableaux vides (`decode_array_with_array_mt`), ordre des clés (sérialiseur maison).
- Sous-requêtes bufferisées en mémoire et héritant des en-têtes client → plafonds de taille, en-têtes nettoyés.

## 9. État d'avancement (2026-10-11)

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
| M9 | non commencé | optionnel |

### Écarts par rapport au plan initial

- **Chemin upstream via `$gqlgw_path`** au lieu de l'URI de la sous-requête : nginx décode les URI de
  sous-requêtes (`%2F` → `/`), ce qui permettait à un argument de modifier le chemin du backend.
  La location devient `proxy_pass http://svc$gqlgw_path;` et la valeur est envoyée telle quelle.
  En plus, `strict_path_args` (défaut) refuse `/`, `.` et `..` dans un segment de chemin.
- **Cache L1 ajouté** (lrucache par worker, valeurs déjà décodées) : sans lui, le chemin « cache »
  était plus lent que le batch seul (décodage JSON de chaque entité, appels non compilés par le JIT).
- **Messages alignés sur graphql-js 17** (pas 16) : `@deprecated` sur les directives,
  `includeDeprecated: Boolean! = false`, `MaxIntrospectionDepth`, « finite value » des input objects.

### Divergences assumées avec graphql-js

- Pas de suggestions « Did you mean » (équivalent de `hideSuggestions: true`).
- Les valeurs des arguments de directives appliquées dans le SDL sont validées (graphql-js ne le fait pas).
- Sur des documents pathologiques (> 100 erreurs, fragments cycliques imbriqués), l'ordre et les doublons
  de certains messages de fusion de champs peuvent différer ; les verdicts restent identiques.
