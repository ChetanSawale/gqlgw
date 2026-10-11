# Contributing to gqlgw

Thanks for your interest! Bug reports, documentation fixes and pull requests are welcome.

## Before you start

- **Bugs**: open an issue with the bug report template: the version, your `gqlgw.toml` and
  schema (reduced to what triggers the bug), the query, and what you expected.
- **Questions and ideas**: use [Discussions](https://github.com/abusi/gqlgw/discussions).
- **Larger changes**: open an issue first to agree on the approach. Some things are out of scope
  by design (see [Conformance and limits](https://abusi.github.io/gqlgw/project/conformance/)):
  subscriptions, federation, several memcached servers, an nginx C module.
- **Security issues**: do not open a public issue, see [SECURITY.md](SECURITY.md).

## Workflow

Contributions come from forks:

1. Fork the repository and create a branch.
2. Make your change, with tests (see below).
3. Open a pull request against `main`.

The CI runs on pull requests once a maintainer has approved the run (every run from a fork needs
an approval). `main` requires the four checks to pass: `make test`, fuzzing, `make docker-test`
and the documentation build. Pull requests are merged by squash, so the title becomes the commit
message: make it a short summary in the imperative mood ("fetch: ...", "docs: ...").

## Development

```bash
make deps        # OpenResty, Test::Nginx, graphql-js, toml-test, in .deps/ (no sudo)
make             # build/libgql.so + build/gqlc
make test        # C (ASan/UBSan), toml-test, config, graphql-js oracle, Test::Nginx
make docker-test # the image end to end
make fuzz        # libFuzzer, FUZZ_TIME=60 per target
```

The system packages are listed in [Development](https://abusi.github.io/gqlgw/project/development/).

## Guidelines

- **C** (`src/`): GNU C99, no dependency, no warning with `-Wall -Wextra`. Anything that parses
  input must keep the hard limits and stay fuzzable; add a regression input under `tests/fuzz/`
  when you fix a crash.
- **Lua** (`lua/gqlgw/`): only the libraries bundled with OpenResty. Keep the hot paths free of
  per-request string building and table churn: the benchmark (`make bench`) is part of the review.
- **Tests**: a bug fix comes with a test that fails without it. Validation changes are checked
  against graphql-js (`make test-oracle`); configuration changes need a case or a golden file in
  `tests/config/`.
- **Documentation** (`docs/`, in English): update the page that describes the behaviour you change.
  Configuration examples live in `examples/` and are tested by `make test-config`. Preview with
  `zensical serve` (see `requirements-docs.txt`).
- Match the style of the surrounding code: naming, comment density, idioms.

## License

By contributing, you agree that your contributions are licensed under the
[BSD-2-Clause license](LICENSE) of the project.
