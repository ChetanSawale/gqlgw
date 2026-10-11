# Hardening: upstream timeouts, fetch and round limits, plan size, depth.
use lib '.';
use t::Gqlgw;

plan 'no_plan';
no_long_string();
run_tests();

__DATA__

=== TEST 1: upstream timeout becomes a field error (504)
--- config
    location = /graphql {
        set $gqlgw_path "";
        content_by_lua_block { require("gqlgw").serve() }
    }
    location /_src/users/ {
        internal;
        proxy_read_timeout 100ms;
        proxy_http_version 1.1;
        proxy_set_header Connection "";
        proxy_pass http://mock/__mock$gqlgw_path;
    }
    location /__mock/ {
        content_by_lua_block { require("mock").handle() }
    }
--- request
POST /graphql
{"query": "{ a: user(id: \"slow\") { id } b: user(id: \"1\") { id } }"}
--- more_headers
Content-Type: application/json
--- response_body chomp
{"errors":[{"message":"Upstream \"users\" returned HTTP 504.","locations":[{"line":1,"column":3}],"path":["a"]}],"data":{"a":null,"b":{"id":"1"}}}
--- error_log
upstream timed out
--- no_error_log
[crit]



=== TEST 2: max_fetches caps the number of upstream requests of a query
--- gqlgw_extra
max_fetches = 3,
--- request
POST /graphql
{"query": "{ posts(limit: 7) { id unbatchedAuthor { id } } }"}
--- more_headers
Content-Type: application/json
--- response_body_like chomp
^\{"errors":\[\{"message":"Too many upstream requests for this query \(limit 3\).","locations":\[\{"line":1,"column":24\}\],"path":\["posts",0,"unbatchedAuthor"\]\},.*"data":\{"posts":\[\{"id":"1","unbatchedAuthor":null\},



=== TEST 3: max_rounds limits nested fetch chains
--- gqlgw_extra
max_rounds = 2,
--- request
POST /graphql
{"query": "{ user(id: \"1\") { posts(limit: 1) { author { name } } } }"}
--- more_headers
Content-Type: application/json
--- response_body chomp
{"errors":[{"message":"Too many nested fetches (more than 2 rounds).","locations":[{"line":1,"column":37}],"path":["user","posts",0,"author"]}],"data":{"user":{"posts":[{"author":null}]}}}



=== TEST 4: plan size limit
--- gqlgw_extra
max_plan_fields = 4,
--- request
POST /graphql
{"query": "{ user(id: 1) { id name createdAt email } }"}
--- more_headers
Content-Type: application/json
--- error_code: 400
--- response_body chomp
{"errors":[{"message":"Query is too complex: its execution plan exceeds 4 fields."}]}



=== TEST 5: selection depth limit
--- gqlgw_extra
max_depth = 3,
--- request
POST /graphql
{"query": "{ user(id: 1) { posts { author { posts { id } } } } }"}
--- more_headers
Content-Type: application/json
--- error_code: 400
--- response_body chomp
{"errors":[{"message":"Syntax Error: Document exceeds maximum selection depth of 3.","locations":[{"line":1,"column":32}]}]}



=== TEST 6: token limit
--- gqlgw_extra
max_tokens = 10,
--- request
POST /graphql
{"query": "{ a: user(id: 1) { id } b: user(id: 2) { id } }"}
--- more_headers
Content-Type: application/json
--- error_code: 400
--- response_body chomp
{"errors":[{"message":"Syntax Error: Document contains more than 10 tokens. Parsing aborted.","locations":[{"line":1,"column":20}]}]}
