#!/bin/sh
# Starts / stops a private memcached for the test suite: memcached.sh start|stop [port]
set -e
cmd="$1"
port="${2:-21211}"
dir="$(cd "$(dirname "$0")/.." && pwd)/build"
pidfile="$dir/memcached-$port.pid"
mkdir -p "$dir"
case "$cmd" in
start)
    if [ -f "$pidfile" ] && kill -0 "$(cat "$pidfile")" 2>/dev/null; then
        exit 0
    fi
    memcached -d -l 127.0.0.1 -p "$port" -U 0 -m 32 -P "$pidfile"
    sleep 0.2
    echo "memcached listening on 127.0.0.1:$port"
    ;;
stop)
    if [ -f "$pidfile" ]; then
        kill "$(cat "$pidfile")" 2>/dev/null || true
        rm -f "$pidfile"
    fi
    ;;
*)
    echo "usage: $0 start|stop [port]" >&2
    exit 2
    ;;
esac
