# Security policy

## Supported versions

gqlgw is in 0.x: security fixes go to the latest release only.

| Version | Supported |
|---|---|
| 0.2.x | yes |
| older | no |

## Reporting a vulnerability

Please **do not open a public issue**. Report it privately through GitHub:
[Security → Report a vulnerability](https://github.com/abusi/gqlgw/security/advisories/new).

Include what you can: the version, the configuration and schema involved, a request that
reproduces the problem, and its impact. You will get an answer as soon as possible. Once a fix is
released, the advisory is published with credit to you, unless you prefer otherwise.

Typical areas: anything that lets a client reach an unintended upstream path or header, leak
another user's cached data, crash or exhaust a worker (the C parser, validation and planning are
fuzzed, but bugs happen), or read the rendered configuration.
