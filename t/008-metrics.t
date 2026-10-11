# Prometheus metrics on the admin server (t/config/gqlgw.toml: [metrics] port,
# exclude_operations = ["^Excluded"]).
use lib '.';
use t::GqlgwConfig;

plan 'no_plan';
no_long_string();
run_tests();

__DATA__

=== TEST 1: request, field, upstream, cache and batch metrics
--- request eval
[
"POST /graphql
{\"query\":\"query GetUser { user(id: \\\"1\\\") { name } }\"}",
"POST /graphql
{\"query\":\"query GetUser { user(id: \\\"2\\\") { id name } }\"}",
"POST /graphql
{\"query\":\"query ExcludedOp { user(id: \\\"3\\\") { id } }\"}",
"POST /graphql
{\"query\":\"{ cachedUser(id: \\\"4\\\") { id } }\"}",
"POST /graphql
{\"query\":\"{ cachedUser(id: \\\"4\\\") { id } }\"}",
"POST /graphql
{\"query\":\"query Broken { nope }\",\"operationName\":\"Broken\"}",
"POST /graphql
{\"query\":\"query Partial { badTls { host } }\"}",
"POST /graphql
{\"query\":\"query Posts { posts { title author { name } } }\"}",
"GET /graphql?query=%7B__typename%7D",
"POST /check_metrics
# TYPE gqlgw_requests_total counter
gqlgw_requests_total{operation=\"GetUser\",type=\"query\",result=\"ok\"} 2
gqlgw_requests_total{operation=\"__excluded\",type=\"query\",result=\"ok\"} 1
gqlgw_requests_total{operation=\"__anonymous\",type=\"query\",result=\"ok\"} 3
gqlgw_requests_total{operation=\"Broken\",type=\"unknown\",result=\"request_error\"} 1
gqlgw_requests_total{operation=\"Partial\",type=\"query\",result=\"partial\"} 1
gqlgw_requests_total{operation=\"Posts\",type=\"query\",result=\"ok\"} 1
gqlgw_errors_total{kind=\"document\"} 1
gqlgw_errors_total{kind=\"field\"} 1
gqlgw_field_requests_total{coordinate=\"Query.user\"} 3
gqlgw_field_requests_total{coordinate=\"User.name\"} 3
gqlgw_field_requests_total{coordinate=\"Post.author\"} 1
gqlgw_doc_cache_total{result=\"hit\"} 1
gqlgw_doc_cache_total{result=\"miss\"} 8
gqlgw_cache_total{source=\"users\",level=\"miss\"} 1
gqlgw_cache_total{source=\"users\",level=\"l1\"} 1
gqlgw_upstream_requests_total{source=\"users\",status=\"2xx\"} 5
gqlgw_upstream_requests_total{source=\"posts\",status=\"2xx\"} 1
gqlgw_upstream_requests_total{source=\"badtls\",status=\"5xx\"} 1
gqlgw_upstream_duration_seconds_count{source=\"posts\"} 1
gqlgw_batches_total{coordinate=\"Post.author\"} 1
gqlgw_batch_size_bucket{coordinate=\"Post.author\",le=\"2\"} 0
gqlgw_batch_size_bucket{coordinate=\"Post.author\",le=\"5\"} 1
gqlgw_batch_size_count{coordinate=\"Post.author\"} 1
# TYPE gqlgw_request_duration_seconds histogram
gqlgw_request_duration_seconds_bucket{operation=\"GetUser\",le=\"+Inf\"} 2
gqlgw_request_duration_seconds_count{operation=\"GetUser\"} 2
gqlgw_rounds_bucket{le=\"2\"} 8
gqlgw_rounds_count 8
"
]
--- more_headers
Content-Type: application/json
--- response_body_like eval
[
qr/\{"data":\{"user":\{"name":"User 1"\}\}\}/,
qr/"User 2"/,
qr/"id":"3"/,
qr/"id":"4"/,
qr/"id":"4"/,
qr/Cannot query field/,
qr/Upstream \\"badtls\\" returned HTTP 502/,
qr/"author":\{"name":"User/,
qr/"__typename":"Query"/,
qr/^done\n\z/,
]
--- error_code eval
[200, 200, 200, 200, 200, 400, 200, 200, 200, 200]
--- no_error_log
[alert]
[crit]



=== TEST 2: gqlgw_info and the exposition format
--- request
GET /admin/gqlgw_metrics
--- response_headers
Content-Type: text/plain; version=0.0.4; charset=utf-8
--- response_body_like
^# HELP gqlgw_info gqlgw version and schema hash\.
# TYPE gqlgw_info gauge
gqlgw_info\{version="[0-9.]+",schema_hash="[0-9a-f]{16}"\} 1



=== TEST 3: requests rejected before parsing are counted as http_error
--- request eval
[
"POST /graphql
{\"query\":\"{ __typename }\"}",
"POST /check_metrics
gqlgw_requests_total{operation=\"__unknown\",type=\"unknown\",result=\"http_error\"} 1
"
]
--- more_headers
Content-Type: text/plain
--- error_code eval
[415, 200]
--- response_body_like eval
[
qr/Unsupported Content-Type/,
qr/^done\n\z/,
]
