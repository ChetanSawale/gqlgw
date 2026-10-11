# Configuration rendered by `gqlc render --locations-only` from t/config/gqlgw.toml:
# headers sent to each source (allowlist, denylist, set_headers), request bodies,
# TLS verification, DNS resolution of upstreams, cache, ${VAR} interpolation.
use lib '.';
use t::GqlgwConfig;

plan 'no_plan';
no_long_string();
no_shuffle();
run_tests();

__DATA__

=== TEST 1: query through the generated locations
--- request
POST /graphql
{"query":"{ user(id: \"3\") { id name } }"}
--- more_headers
Content-Type: application/json
--- response_body chomp
{"data":{"user":{"id":"3","name":"User 3"}}}
--- response_headers
X-GQL-Subrequests: 1



=== TEST 2: allowlist (default): only Authorization is forwarded
--- request
POST /graphql
{"query":"{ usersHeaders { method uri authorization cookie acceptLanguage accept host contentType acceptEncoding userAgent xTrace } }"}
--- more_headers
Content-Type: application/json
Authorization: Bearer abc
Cookie: session=1
Accept-Language: fr
User-Agent: test-agent
X-Trace: t1
Accept-Encoding: gzip
--- response_body chomp
{"data":{"usersHeaders":{"method":"GET","uri":"/__mock/headers","authorization":"Bearer abc","cookie":null,"acceptLanguage":null,"accept":"application/json","host":"127.0.0.1:1984","contentType":null,"acceptEncoding":null,"userAgent":null,"xTrace":null}}}



=== TEST 3: denylist: everything but Cookie and X-Trace
--- request
POST /graphql
{"query":"{ postsHeaders { uri authorization cookie acceptLanguage accept contentType acceptEncoding userAgent xTrace } }"}
--- more_headers
Content-Type: application/json
Authorization: Bearer abc
Cookie: session=1
Accept-Language: fr
User-Agent: test-agent
X-Trace: t1
Accept-Encoding: gzip
--- response_body chomp
{"data":{"postsHeaders":{"uri":"/__mock/headers","authorization":"Bearer abc","cookie":null,"acceptLanguage":"fr","accept":"application/json","contentType":null,"acceptEncoding":null,"userAgent":"test-agent","xTrace":null}}}



=== TEST 4: allowlist with set_headers ('$' kept literally, ${VAR:-default} interpolated)
--- request
POST /graphql
{"query":"{ echoHeaders { authorization cookie acceptLanguage xTrace xApiKey } }"}
--- more_headers
Content-Type: application/json
Authorization: Bearer abc
Cookie: session=1
Accept-Language: fr
X-Trace: t1
--- response_body chomp
{"data":{"echoHeaders":{"authorization":null,"cookie":null,"acceptLanguage":"fr","xTrace":"t1","xApiKey":"k$y-secret"}}}



=== TEST 5: mutation body: Content-Type set by the gateway in allowlist mode
--- request
POST /graphql
{"query":"mutation { echoPost(input: \"hi\") { method contentType contentLength body } }"}
--- more_headers
Content-Type: application/json
--- response_body chomp
{"data":{"echoPost":{"method":"POST","contentType":"application/json","contentLength":"15","body":"{\"input\": \"hi\"}"}}}



=== TEST 6: GET subrequests carry no Content-Type even when the client sent one
--- request
POST /graphql
{"query":"{ postsHeaders { contentType contentLength } }"}
--- more_headers
Content-Type: application/json
--- response_body chomp
{"data":{"postsHeaders":{"contentType":null,"contentLength":null}}}



=== TEST 7: TLS upstream resolved by name, certificate verified
--- request
POST /graphql
{"query":"{ secureHeaders { host uri } }"}
--- more_headers
Content-Type: application/json
--- response_body chomp
{"data":{"secureHeaders":{"host":"localhost:19843","uri":"/__mock/headers"}}}



=== TEST 8: TLS verification failure is a field error
--- request
POST /graphql
{"query":"{ badTls { host } }"}
--- more_headers
Content-Type: application/json
--- response_body chomp
{"errors":[{"message":"Upstream \"badtls\" returned HTTP 502.","locations":[{"line":1,"column":3}],"path":["badTls"]}],"data":{"badTls":null}}
--- error_log
upstream SSL certificate verify error



=== TEST 9: cache (L2) with the rendered configuration
--- config
    include $TEST_NGINX_ROOT/t/config/run/gqlgw.locations.conf;
    location /__mock/ {
        content_by_lua_block { require("mock").handle() }
    }
    location = /t {
        content_by_lua_block {
            local cjson = require "cjson"
            ngx.req.set_header("Content-Type", "application/json")
            for i = 1, 2 do
                local res = ngx.location.capture("/graphql", { method = ngx.HTTP_POST,
                    body = cjson.encode({ query = '{ cachedUser(id: "7") { name } }' }) })
                ngx.say(res.body, " sub=", res.header["X-GQL-Subrequests"])
            end
        }
    }
--- request
GET /t
--- response_body
{"data":{"cachedUser":{"name":"User 7"}}} sub=1
{"data":{"cachedUser":{"name":"User 7"}}} sub=0



=== TEST 10: purge on the admin server
--- request
POST /admin/gqlgw_purge?source=users
--- response_body chomp
{"source":"users","generation":1}



=== TEST 11: the admin server answers 404 elsewhere
--- request
GET /admin/graphql
--- error_code: 404
--- response_body_like: 404 Not Found
