# A gateway image: your schema and configuration on top of gqlgw.
#
#   docker build -t my-gateway .
#   docker run -p 8080:8080 -p 8191:8191 \
#       -e USERS_URL=http://users:8080 -e POSTS_URL=http://posts:8080 my-gateway
ARG GQLGW_IMAGE=gqlgw:latest
FROM ${GQLGW_IMAGE}
COPY schema.graphqls gqlgw.toml /etc/gqlgw/
# the schema is compiled, and the configuration checked, when the image is built
RUN gqlc compile -c /etc/gqlgw/gqlgw.toml /etc/gqlgw/schema.graphqls
