#!/bin/sh
# gqlgw container entrypoint.
#
# Without arguments (or with nginx flags): renders /etc/gqlgw/gqlgw.toml with
# the environment (gqlc render), checks the result (openresty -t, which also
# loads the compiled schema), then runs OpenResty in the foreground.
# Any other command is executed as is (docker run IMAGE gqlc ..., sh, ...).
set -e
CONF="${GQLGW_CONFIG:-/etc/gqlgw/gqlgw.toml}"
RUN=/tmp/gqlgw

if [ "$#" -gt 0 ] && [ "${1#-}" = "$1" ]; then
    exec "$@"
fi
if [ ! -f "$CONF" ]; then
    echo "gqlgw: $CONF not found: build an image FROM this one with your schema and gqlgw.toml" >&2
    echo "gqlgw: (see /usr/local/share/gqlgw/Dockerfile.example)" >&2
    exit 1
fi
umask 077
mkdir -p "$RUN/tmp"
gqlc render -q -c "$CONF" -o "$RUN"
openresty -e stderr -q -t
exec openresty -e stderr -g 'daemon off;' "$@"
