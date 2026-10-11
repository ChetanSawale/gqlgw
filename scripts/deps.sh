#!/usr/bin/env bash
# Installe les dépendances hors apt dans .deps/ (sans sudo) :
#   - OpenResty (compilé depuis les sources)
#   - Test::Nginx (cpanm --local-lib)
#   - graphql-js (oracle de test, npm)
#   - corpus toml-test (fichiers uniquement, pas besoin de Go)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEPS="$ROOT/.deps"
OR_VER="${OR_VER:-1.31.1.1}"
TOML_TEST_VER="${TOML_TEST_VER:-2.2.0}"
JOBS="$(nproc)"

mkdir -p "$DEPS/src"

if [ ! -x "$DEPS/openresty/nginx/sbin/nginx" ]; then
    echo ">> OpenResty $OR_VER"
    cd "$DEPS/src"
    [ -f "openresty-$OR_VER.tar.gz" ] || curl -fsSLO "https://openresty.org/download/openresty-$OR_VER.tar.gz"
    rm -rf "openresty-$OR_VER"
    tar xzf "openresty-$OR_VER.tar.gz"
    cd "openresty-$OR_VER"
    ./configure --prefix="$DEPS/openresty" --with-pcre-jit --with-http_v2_module -j"$JOBS" > "$DEPS/openresty-configure.log"
    make -j"$JOBS" > "$DEPS/openresty-build.log" 2>&1
    make install >> "$DEPS/openresty-build.log" 2>&1
fi
"$DEPS/openresty/nginx/sbin/nginx" -V 2>&1 | head -1

if ! PERL5LIB="$DEPS/perl/lib/perl5" perl -MTest::Nginx::Socket -e1 2>/dev/null; then
    echo ">> Test::Nginx"
    cpanm --notest --quiet --local-lib="$DEPS/perl" Test::Nginx
fi
echo "Test::Nginx OK"

if [ ! -d "$ROOT/tests/oracle/node_modules/graphql" ]; then
    echo ">> graphql-js"
    (cd "$ROOT/tests/oracle" && npm install --no-audit --no-fund --silent)
fi
node -e "console.log('graphql-js', require('$ROOT/tests/oracle/node_modules/graphql/package.json').version)"

if [ ! -f "$DEPS/toml-test/tests/files-toml-1.1.0" ]; then
    echo ">> toml-test $TOML_TEST_VER"
    cd "$DEPS/src"
    curl -fsSL -o "toml-test-$TOML_TEST_VER.tar.gz" "https://github.com/toml-lang/toml-test/archive/refs/tags/v$TOML_TEST_VER.tar.gz"
    tar xzf "toml-test-$TOML_TEST_VER.tar.gz"
    ln -sfn "src/toml-test-$TOML_TEST_VER" "$DEPS/toml-test"
fi
echo "toml-test $(basename "$(readlink "$DEPS/toml-test")")"
