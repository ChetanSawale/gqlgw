# Conformance and limits

## Conformance

| Area | Target | How it is checked |
|---|---|---|
| Validation | every rule of the GraphQL spec (September 2025), graphql-js 17 messages | corpus + differential fuzzing against graphql-js: no verdict difference on 9,000 random documents, ~99.7 % identical messages |
| Introspection | identical to graphql-js 17 (`getIntrospectionQuery`) | oracle tests |
| Execution | null propagation, field order, result coercion | Test::Nginx |
| GraphQL over HTTP | the draft: media types, status codes, `GET` for queries only | Test::Nginx |
| TOML | 1.1.0 | 214/214 valid and 467/467 invalid documents of toml-test |
| Robustness | no crash on any input | libFuzzer + ASan/UBSan on the parser, queries, TOML and configuration (millions of executions per target) |

### Assumed divergences from graphql-js

- No "Did you mean …?" suggestions (like `hideSuggestions: true`).
- Directive arguments applied in the SDL are validated (graphql-js does not).
- On pathological documents (over 100 errors, nested cyclic fragments), the order and duplicates
  of some field-merging messages can differ. The verdicts are the same.

## Not supported

- Subscriptions, `@defer` / `@stream`, file uploads, federation, pagination conventions.
- Persisted queries: queries are parsed at runtime (validated documents are cached per worker).
- Several memcached servers: one endpoint, which can be a mcrouter.

## Known limitations

- Custom scalars are passed through as JSON values, without coercion.
- Argument coercion errors that involve variables are request errors, not field errors.
- JSON numbers above 2^53 lose precision (cjson): use strings for large identifiers.
- `{args.x}` in a body re-encodes the value: the keys of an input object can be reordered
  (`{args}` as a whole keeps the schema order).
- Request metrics are recorded in the log phase: they need the generated `log_by_lua`.
