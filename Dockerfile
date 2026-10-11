# gqlgw: SDL-first GraphQL gateway on OpenResty.
#
#   make image        (docker build -t gqlgw:<version> -t gqlgw:latest .)
#
# Gateways are built FROM this image (see examples/docker/Dockerfile):
#   FROM gqlgw:<version>
#   COPY schema.graphqls gqlgw.toml /etc/gqlgw/
#   RUN gqlc compile -c /etc/gqlgw/gqlgw.toml /etc/gqlgw/schema.graphqls
ARG OPENRESTY_IMAGE=openresty/openresty:1.31.1.1-resolute

FROM ${OPENRESTY_IMAGE} AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends gcc make libc6-dev \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY Makefile ./
COPY scripts/embed_prelude.sh scripts/
COPY src/ src/
RUN make -j"$(nproc)" all CFLAGS="-O2" PREFIX=/usr/local \
 && strip build/gqlc build/libgql.so

FROM ${OPENRESTY_IMAGE}
LABEL org.opencontainers.image.title="gqlgw" \
      org.opencontainers.image.description="SDL-first GraphQL gateway on OpenResty" \
      org.opencontainers.image.licenses="BSD-2-Clause"
COPY --from=build /src/build/gqlc /usr/local/bin/gqlc
COPY --from=build /src/build/libgql.so /usr/local/lib/gqlgw/libgql.so
COPY lua/gqlgw/ /usr/local/openresty/site/lualib/gqlgw/
COPY docker/nginx.conf /usr/local/openresty/nginx/conf/nginx.conf
COPY docker/entrypoint.sh /usr/local/bin/gqlgw-entrypoint
COPY examples/docker/Dockerfile /usr/local/share/gqlgw/Dockerfile.example
COPY examples/docker/gqlgw.toml /usr/local/share/gqlgw/gqlgw.toml.example
COPY LICENSE /usr/local/share/gqlgw/LICENSE
RUN groupadd -g 10001 gqlgw \
 && useradd -u 10001 -g gqlgw -M -d /etc/gqlgw -s /usr/sbin/nologin gqlgw \
 && mkdir -p /etc/gqlgw/conf.d /etc/gqlgw/server.d /etc/gqlgw/sources.d \
 && chown gqlgw:gqlgw /etc/gqlgw \
 && rm -f /etc/nginx/conf.d/default.conf
USER gqlgw
WORKDIR /etc/gqlgw
EXPOSE 8080 8191
STOPSIGNAL SIGQUIT
ENTRYPOINT ["gqlgw-entrypoint"]
CMD []
