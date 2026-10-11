#!/bin/sh
# Integration test of the Docker image (make docker-test).
#
# Builds gqlgw:<version> and the example gateway (examples/docker), then runs
# two gateways sharing memcached in front of the mock backend, read-only:
# queries, batching, L3 cache across gateways, metrics, purge, start-up
# failure on a missing variable, non-root user, rendered file permissions.
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
VERSION=$(sed -n 's/^local _M = { _VERSION = "\(.*\)" }/\1/p' "$ROOT/lua/gqlgw/init.lua")
BASE=openresty/openresty:1.31.1.1-resolute
P=gqlgw-test-$$
fails=0

cleanup() {
    docker rm -f "$P-gw1" "$P-gw2" "$P-mc" "$P-be" > /dev/null 2>&1 || true
    docker network rm "$P" > /dev/null 2>&1 || true
}
trap cleanup EXIT

ok() { echo "  ok   $1"; }
ko() { echo "  FAIL $1"; fails=$((fails + 1)); }
check() {   # check <label> <haystack> <needle>
    case "$2" in
    *"$3"*) ok "$1" ;;
    *) ko "$1: expected \"$3\" in: $(printf %s "$2" | head -c 400)" ;;
    esac
}

echo "docker: building gqlgw:$VERSION"
docker build -q -t "gqlgw:$VERSION" -t gqlgw:latest "$ROOT" > /dev/null
docker build -q --build-arg "GQLGW_IMAGE=gqlgw:$VERSION" -t "$P-example" "$ROOT/examples/docker" > /dev/null

docker network create "$P" > /dev/null
docker run -d --name "$P-mc" --network "$P" --network-alias memcached memcached:1.6 > /dev/null
docker run -d --name "$P-be" --network "$P" --network-alias backend \
    -v "$ROOT/examples/docker/mock/nginx.conf:/usr/local/openresty/nginx/conf/nginx.conf:ro" \
    -v "$ROOT/t/lib/mock.lua:/usr/local/openresty/site/lualib/mock.lua:ro" "$BASE" > /dev/null

gateway() {
    docker run -d --name "$1" --network "$P" --read-only --tmpfs /tmp \
        -p 127.0.0.1::8080 -p 127.0.0.1::8191 \
        -e USERS_URL=http://backend:8080 -e POSTS_URL=http://backend:8080 -e GQLGW_DEBUG_HEADERS=true \
        "$P-example" > /dev/null
}
port() { docker port "$1" "$2" | head -1 | sed 's/.*://'; }
wait_up() {
    i=0
    while [ $i -lt 50 ]; do
        curl -s -o /dev/null "http://127.0.0.1:$(port "$1" 8191)/gqlgw_metrics" && return 0
        sleep 0.2
        i=$((i + 1))
    done
    docker logs "$1" 2>&1 | tail -20
    return 1
}
query() {   # query <container> <graphql>: headers and body
    curl -s -i -H 'Content-Type: application/json' -d "{\"query\":\"$2\"}" \
        "http://127.0.0.1:$(port "$1" 8080)/graphql" | tr -d '\r'
}

gateway "$P-gw1"
gateway "$P-gw2"
wait_up "$P-gw1" && wait_up "$P-gw2" || { ko "gateways did not start"; exit 1; }
ok "two read-only gateways started"

Q='{ posts(limit: 5) { title author { name } } }'
r=$(query "$P-gw1" "$Q")
check "query" "$r" '"title":"Post 1","author":{"name":"User 2"}'
check "batching: posts + one batch for the authors" "$r" "X-GQL-Subrequests: 2"
sleep 0.5    # memcached writes are asynchronous
r=$(query "$P-gw2" "$Q")
check "authors from memcached on the other gateway" "$r" "X-GQL-Subrequests: 1"
check "L3 hits" "$r" "l3=5"

sleep 1.2    # every worker flushes its metrics each second
m=$(curl -s "http://127.0.0.1:$(port "$P-gw1" 8191)/gqlgw_metrics")
check "metrics: requests" "$m" 'gqlgw_requests_total{operation="__anonymous",type="query",result="ok"} 1'
check "metrics: batches" "$m" 'gqlgw_batches_total{coordinate="Post.author"} 1'
check "metrics: upstream" "$m" 'gqlgw_upstream_requests_total{source="users",status="2xx"} 1'

r=$(curl -s -X POST "http://127.0.0.1:$(port "$P-gw1" 8191)/gqlgw_purge?source=users")
check "purge" "$r" '{"source":"users","generation":1}'
sleep 1.2    # generation numbers are cached for 1 s per node
r=$(query "$P-gw2" "$Q")
check "after purge: authors fetched again" "$r" "X-GQL-Subrequests: 2"

check "runs as gqlgw (uid 10001)" "$(docker exec "$P-gw1" id -u)" "10001"
check "rendered configuration is private" "$(docker exec "$P-gw1" stat -c %a /tmp/gqlgw/gqlgw.lua /tmp/gqlgw/gqlgw.conf)" "600
600"

out=$(docker run --rm --network "$P" -e POSTS_URL=http://backend:8080 "$P-example" 2>&1) && rc=0 || rc=$?
[ "$rc" != 0 ] && ok "refuses to start without USERS_URL (exit $rc)" || ko "started without USERS_URL"
check "start-up error message" "$out" "set USERS_URL to the users service"

out=$(docker run --rm "$P-example" gqlc 2>&1 || true)
check "commands are passed through (gqlc)" "$out" "usage:"

docker rmi "$P-example" > /dev/null 2>&1 || true
echo "docker: $fails failure(s)"
[ "$fails" = 0 ]
