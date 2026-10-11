#!/bin/sh
# gqlgw.toml tests.
#   cases/*.toml   error cases; header comments:
#                    # mode: render | check   (gqlc render, or gqlc compile -c; default render)
#                    # env: NAME=value ...     (environment of gqlc)
#                    # status: 0 | 1           (expected exit status, default 1)
#                    # expect: text            (must appear in the output, one per line)
#   golden/*.toml  valid configurations: the rendered files must match golden/<name>.*
#                  (UPDATE=1 rewrites them), and `openresty -t` must accept them.
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
    # nginx must accept the rendered configuration (init_by_lua loads the schema)
    mkdir -p "$TMP/prefix-$name/logs"
    {
        echo "events {}"
        echo "http {"
        echo "  lua_package_path \"$ROOT/lua/?.lua;$ROOT/lua/?/init.lua;;\";"
        if [ -f "$out/gqlgw.conf" ]; then
            echo "  include $out/gqlgw.conf;"
        else
            echo "  include $out/gqlgw.http.conf;"
            echo "  server { listen 127.0.0.1:19899; include $out/gqlgw.locations.conf; }"
        fi
        echo "}"
    } > "$TMP/nginx-$name.conf"
    if ! "$NGINX" -t -q -p "$TMP/prefix-$name" -c "$TMP/nginx-$name.conf" -e "$TMP/prefix-$name/logs/error.log" \
            > "$TMP/t.log" 2>&1; then
        echo "FAIL $name: nginx -t"
        cat "$TMP/t.log"
        fails=$((fails + 1))
    fi
done

echo "config: $n cases, $fails failure(s)"
[ $fails = 0 ]
