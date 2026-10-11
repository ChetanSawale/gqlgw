#!/bin/sh
# Self-signed certificates for the TLS configuration tests:
#   cert.pem / key.pem   for "localhost" (served by the TLS mock backend)
#   other.pem            an unrelated certificate (verification must fail)
set -e
dir="$1"
mkdir -p "$dir"
[ -f "$dir/cert.pem" ] && [ -f "$dir/other.pem" ] && exit 0
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 3650 \
    -subj /CN=localhost -addext subjectAltName=DNS:localhost \
    -keyout "$dir/key.pem" -out "$dir/cert.pem" 2>/dev/null
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 3650 \
    -subj /CN=other -addext subjectAltName=DNS:localhost \
    -keyout "$dir/otherkey.pem" -out "$dir/other.pem" 2>/dev/null
