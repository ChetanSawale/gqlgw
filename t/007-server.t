# Configuration rendered by `gqlc render` in its default mode (t/config/server.toml):
# gqlgw.conf brings its own server {}, which includes server.d/*.conf.
use lib '.';
use t::GqlgwConfig;

plan 'no_plan';
no_long_string();
no_root_location();
run_tests();

__DATA__

=== TEST 1: query through the gqlgw server
--- gqlgw_server
--- request
POST /graphql
{"query":"{ user(id: \"5\") { id name } }"}
--- more_headers
Content-Type: application/json
--- response_body chomp
{"data":{"user":{"id":"5","name":"User 5"}}}
--- response_headers
X-GQL-Subrequests: 1



=== TEST 2: TLS without verification (IP address: no SNI)
--- gqlgw_server
--- request
POST /graphql
{"query":"{ secureHeaders { host } }"}
--- more_headers
Content-Type: application/json
--- response_body chomp
{"data":{"secureHeaders":{"host":"127.0.0.1:19843"}}}



=== TEST 3: server.d/*.conf is included in the gqlgw server
--- gqlgw_server
--- request
GET /health
--- response_body
ok



=== TEST 4: no cache shm (shm_size = 0): every query goes upstream
--- gqlgw_server
--- request
POST /graphql
{"query":"{ cachedUser(id: \"5\") { id } }"}
--- more_headers
Content-Type: application/json
--- response_headers
X-GQL-Subrequests: 1
--- response_body chomp
{"data":{"cachedUser":{"id":"5"}}}
