#!/bin/sh
# gqlgw.toml tests.
#   cases/*.toml   error cases; header comments:
#                    # mode: render | check   (gqlc render, or gqlc compile -c; default render)
#                    # env: NAME=value ...     (environment of gqlc)
#                    # status: 0 | 1           (expected exit status, default 1)
#                    # expect: text            (must appear in the output, one per line)
#   golden/*.toml  valid configurations: the rendered files must match golden/<name>.*
#                  (UPDATE=1 rewrites them), and `openresty -t` must accept them.
#   examples/*/gqlgw.toml  the configurations shown in the documentation: they must
#                  compile with their schema, render, and pass `openresty -t`.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
GQLC="$ROOT/build/gqlc"
NGINX="$ROOT/.deps/openresty/nginx/sbin/nginx"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fails=0
n=0

for f in "$ROOT"/tests/config/cases/*.toml; do
    n=$((n + 1))
    name=$(basename "$f" .toml)
    mode=$(sed -n 's/^# mode: //p' "$f")
    envs=$(sed -n 's/^# env: //p' "$f")
    status=$(sed -n 's/^# status: //p' "$f")
    [ -n "$status" ] || status=1
    if [ "$mode" = check ]; then
        out=$(cd "$TMP" && env -i PATH="$PATH" $envs "$GQLC" compile -q -c "$f" -o "$TMP/s.gqlb" -r "$TMP/s.routes.lua" \
              "$ROOT/t/schemas/gw.graphqls" 2>&1)
    else
        out=$(env -i PATH="$PATH" $envs "$GQLC" render -q -c "$f" -o "$TMP/$name" \
              --resolv-conf "$ROOT/tests/config/resolv.conf" 2>&1)
    fi
    rc=$?
    ok=1
    [ "$rc" = "$status" ] || { echo "FAIL $name: exit status $rc, expected $status"; ok=0; }
    sed -n 's/^# expect: //p' "$f" > "$TMP/expect"
    while IFS= read -r line; do
        case "$out" in
        *"$line"*) ;;
        *) echo "FAIL $name: missing: $line"; ok=0 ;;
        esac
    done < "$TMP/expect"
    if [ $ok = 0 ]; then
        echo "  output: $out" | head -20
        fails=$((fails + 1))
    fi
done

# nginx_check <name> <render dir>: nginx must accept the rendered configuration
# (init_by_lua loads the schema)
nginx_check() {
    mkdir -p "$TMP/prefix-$1/logs"
    {
        echo "events {}"
        echo "http {"
        echo "  lua_package_path \"$ROOT/lua/?.lua;$ROOT/lua/?/init.lua;;\";"
        if [ -f "$2/gqlgw.conf" ]; then
            echo "  include $2/gqlgw.conf;"
        else
            echo "  include $2/gqlgw.http.conf;"
            echo "  server { listen 127.0.0.1:19899; include $2/gqlgw.locations.conf; }"
        fi
        echo "}"
    } > "$TMP/nginx-$1.conf"
    if ! "$NGINX" -t -q -p "$TMP/prefix-$1" -c "$TMP/nginx-$1.conf" -e "$TMP/prefix-$1/logs/error.log" \
            > "$TMP/t.log" 2>&1; then
        echo "FAIL $1: nginx -t"
        cat "$TMP/t.log"
        fails=$((fails + 1))
    fi
}

for f in "$ROOT"/tests/config/golden/*.toml; do
    n=$((n + 1))
    name=$(basename "$f" .toml)
    out="$TMP/golden-$name"
    lo=$(sed -n 's/^# render: //p' "$f")
    if ! env -i PATH="$PATH" GQLGW_TEST_ROOT="$ROOT" "$GQLC" render -q -c "$f" -o "$out" $lo \
            --resolv-conf "$ROOT/tests/config/resolv.conf"; then
        echo "FAIL $name: render failed"
        fails=$((fails + 1))
        continue
    fi
    for file in "$out"/*; do
        base=$(basename "$file")
        sed "s#$out#@OUT@#g; s#$ROOT#@ROOT@#g" "$file" > "$file.norm"
        if [ "${UPDATE:-0}" = 1 ]; then
            cp "$file.norm" "$ROOT/tests/config/golden/$name.$base"
        elif ! diff -u "$ROOT/tests/config/golden/$name.$base" "$file.norm" > "$TMP/diff"; then
            echo "FAIL $name: $base differs from golden/$name.$base"
            head -40 "$TMP/diff"
            fails=$((fails + 1))
        fi
    done
    nginx_check "$name" "$out"
done

# Documentation examples: compiled with their schema (or the blog schema of
# examples/docker), rendered with a fixed environment, checked by nginx. They
# are copied first: gqlc compile writes next to gqlgw.toml.
for f in "$ROOT"/examples/*/gqlgw.toml; do
    n=$((n + 1))
    dir=$(dirname "$f")
    name=example-$(basename "$dir")
    ex="$TMP/$name"
    cp -r "$dir" "$ex"
    schema="$dir/schema.graphqls"
    [ -f "$schema" ] || schema="$ROOT/examples/docker/schema.graphqls"
    # the library of this build, not the installed one
    lib="lib = \"$ROOT/build/libgql.so\""
    if grep -q '^\[gateway\]' "$ex/gqlgw.toml"; then
        sed -i "/^\[gateway\]/a $lib" "$ex/gqlgw.toml"
    else
        printf '\n[gateway]\n%s\n' "$lib" >> "$ex/gqlgw.toml"
    fi
    if ! "$GQLC" compile -q -c "$ex/gqlgw.toml" "$schema" > "$TMP/t.log" 2>&1; then
        echo "FAIL $name: gqlc compile -c"
        cat "$TMP/t.log"
        fails=$((fails + 1))
        continue
    fi
    lo=$(sed -n 's/^# render: \(--[a-z-]*\).*/\1/p' "$f")
    if ! env -i PATH="$PATH" USERS_URL=http://users:8080 POSTS_URL=http://posts:8080 ORDERS_API_KEY=key \
            "$GQLC" render -q -c "$ex/gqlgw.toml" -o "$ex/run" $lo \
            --resolv-conf "$ROOT/tests/config/resolv.conf" > "$TMP/t.log" 2>&1; then
        echo "FAIL $name: gqlc render"
        cat "$TMP/t.log"
        fails=$((fails + 1))
        continue
    fi
    nginx_check "$name" "$ex/run"
done

echo "config: $n cases, $fails failure(s)"
[ $fails = 0 ]
