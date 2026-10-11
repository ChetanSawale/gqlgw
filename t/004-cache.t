# Resource cache: L2 (lua_shared_dict), L3 (memcached), batching with partial
# hits, negative entries, private scope, purge.
use lib '.';
use t::Gqlgw;

plan 'no_plan';
no_long_string();

our $Scenario = <<'_EOC_';
    location = /t {
        content_by_lua_block {
            local cjson = require "cjson"
            local function q(query)
                local res = ngx.location.capture("/graphql", {
                    method = ngx.HTTP_POST, body = cjson.encode({ query = query }) })
                return res
            end
            local function show(label, res)
                ngx.say(label, ": ", res.body, " sub=", res.header["X-GQL-Subrequests"],
                        " cache=", res.header["X-GQL-Cache"])
            end
            local function hits(key)
                local d = ngx.shared.mock
                return d:get("hits:" .. key) or 0
            end
            ngx.ctx.q, ngx.ctx.show, ngx.ctx.hits = q, show, hits
            local scenario = loadstring(ngx.var.scenario)
            setfenv(scenario, setmetatable({ q = q, show = show, hits = hits, cjson = cjson },
                                           { __index = _G }))
            scenario()
        }
    }
_EOC_

add_block_preprocessor(sub {
    my $block = shift;
    if (defined $block->scenario) {
        my $code = $block->scenario;
        $code =~ s/'/\\'/g;
        $code =~ s/\n/ /g;
        $block->set_value("config", $t::Gqlgw::Config . "    set \$scenario '$code';\n" . $Scenario);
        $block->set_value("request", "GET /t");
        $block->set_value("more_headers", "Content-Type: application/json");
    }
});

run_tests();

__DATA__

=== TEST 1: L2 hit on the second request
--- scenario
show("1", q('{ user(id: "1") { name } }'))
show("2", q('{ user(id: "1") { name } }'))
ngx.say("upstream hits: ", hits("users/1"))
--- response_body
1: {"data":{"user":{"name":"User 1"}}} sub=1 cache=l1=0;l2=0;l3=0;miss=1
2: {"data":{"user":{"name":"User 1"}}} sub=0 cache=l1=1;l2=0;l3=0;miss=0
upstream hits: 1



=== TEST 2: batch with partial hits only fetches the misses, in one request
--- scenario
q('{ a: user(id: "2") { id } b: user(id: "3") { id } }')
show("posts", q('{ posts(limit: 14) { author { id } } }'))
show("again", q('{ posts(limit: 14) { author { id } } }'))
ngx.say("batch requests: ", hits("users?ids"))
--- response_body
posts: {"data":{"posts":[{"author":{"id":"2"}},{"author":{"id":"3"}},{"author":{"id":"4"}},{"author":{"id":"5"}},{"author":{"id":"6"}},{"author":{"id":"7"}},{"author":{"id":"1"}},{"author":{"id":"2"}},{"author":{"id":"3"}},{"author":{"id":"4"}},{"author":{"id":"5"}},{"author":{"id":"6"}},{"author":{"id":"7"}},{"author":{"id":"1"}}]}} sub=2 cache=l1=2;l2=0;l3=0;miss=5
again: {"data":{"posts":[{"author":{"id":"2"}},{"author":{"id":"3"}},{"author":{"id":"4"}},{"author":{"id":"5"}},{"author":{"id":"6"}},{"author":{"id":"7"}},{"author":{"id":"1"}},{"author":{"id":"2"}},{"author":{"id":"3"}},{"author":{"id":"4"}},{"author":{"id":"5"}},{"author":{"id":"6"}},{"author":{"id":"7"}},{"author":{"id":"1"}}]}} sub=1 cache=l1=7;l2=0;l3=0;miss=0
batch requests: 1



=== TEST 3: missing resources are cached as negative entries
--- scenario
show("1", q('{ user(id: "404") { id } }'))
show("2", q('{ user(id: "404") { id } }'))
--- response_body
1: {"data":{"user":null}} sub=1 cache=l1=0;l2=0;l3=0;miss=1
2: {"data":{"user":null}} sub=0 cache=l1=1;l2=0;l3=0;miss=0



=== TEST 4: purge a source
--- scenario
q('{ user(id: "5") { name } }')
show("cached", q('{ user(id: "5") { name } }'))
local res = ngx.location.capture("/purge?source=users", { method = ngx.HTTP_POST })
ngx.say("purge: ", res.status, " ", res.body)
show("purged", q('{ user(id: "5") { name } }'))
--- response_body
cached: {"data":{"user":{"name":"User 5"}}} sub=0 cache=l1=1;l2=0;l3=0;miss=0
purge: 200 {"source":"users","generation":1}
purged: {"data":{"user":{"name":"User 5"}}} sub=1 cache=l1=0;l2=0;l3=0;miss=1



=== TEST 5: PRIVATE scope: one entry per identity
--- scenario
ngx.req.set_header("Authorization", "Bearer alice")
show("alice", q('{ me { authorization } }'))
show("alice", q('{ me { authorization } }'))
ngx.req.set_header("Authorization", "Bearer bob")
show("bob", q('{ me { authorization } }'))
--- response_body
alice: {"data":{"me":{"authorization":"Bearer alice"}}} sub=1 cache=l1=0;l2=0;l3=0;miss=1
alice: {"data":{"me":{"authorization":"Bearer alice"}}} sub=0 cache=l1=1;l2=0;l3=0;miss=0
bob: {"data":{"me":{"authorization":"Bearer bob"}}} sub=1 cache=l1=0;l2=0;l3=0;miss=1



=== TEST 6: memcached (L3) shared cache, writes are asynchronous
--- gqlgw_extra
cache = { memcached = { host = "127.0.0.1", port = $TEST_NGINX_MEMCACHED_PORT, timeout = 200 }, l1_size = 0 },
--- scenario
local memcached = require "resty.memcached"
local m = memcached:new()
m:connect("127.0.0.1", tonumber(os.getenv("TEST_NGINX_MEMCACHED_PORT") or 21211))
m:flush_all()
m:close()
show("1", q('{ user(id: "8") { name } }'))
ngx.sleep(0.05)
show("2", q('{ user(id: "8") { name } }'))
--- main_config
env TEST_NGINX_MEMCACHED_PORT;
--- response_body
1: {"data":{"user":{"name":"User 8"}}} sub=1 cache=l1=0;l2=0;l3=0;miss=1
2: {"data":{"user":{"name":"User 8"}}} sub=0 cache=l1=0;l2=0;l3=1;miss=0



=== TEST 7: memcached down: fail-open
--- gqlgw_extra
cache = { memcached = { host = "127.0.0.1", port = 1, timeout = 100 }, l1_size = 0 },
--- scenario
show("1", q('{ user(id: "9") { name } }'))
show("2", q('{ user(id: "9") { name } }'))
--- response_body
1: {"data":{"user":{"name":"User 9"}}} sub=1 cache=l1=0;l2=0;l3=0;miss=1
2: {"data":{"user":{"name":"User 9"}}} sub=1 cache=l1=0;l2=0;l3=0;miss=1
--- error_log
memcached unavailable
--- no_error_log
[crit]
[alert]



=== TEST 8: purge with memcached increments the shared generation
--- gqlgw_extra
cache = { memcached = { host = "127.0.0.1", port = $TEST_NGINX_MEMCACHED_PORT, timeout = 200 }, shm = "gqlgw_cache", gen_ttl = 0.001, l1_size = 0 },
--- scenario
q('{ user(id: "10") { name } }')
ngx.sleep(0.05)
show("cached", q('{ user(id: "10") { name } }'))
local res = ngx.location.capture("/purge?source=users", { method = ngx.HTTP_POST })
ngx.say("purge: ", res.status)
ngx.sleep(0.01)
show("purged", q('{ user(id: "10") { name } }'))
--- main_config
env TEST_NGINX_MEMCACHED_PORT;
--- response_body_like
cached: \{"data":\{"user":\{"name":"User 10"\}\}\} sub=0 cache=l1=0;l2=1;l3=0;miss=0
purge: 200
purged: \{"data":\{"user":\{"name":"User 10"\}\}\} sub=1 cache=l1=0;l2=0;l3=0;miss=1



=== TEST 9: L2 (shm) serves when L1 is disabled
--- gqlgw_extra
cache = { shm = "gqlgw_cache", l2_ttl = 60, l1_size = 0 },
--- scenario
show("1", q('{ user(id: "11") { name } }'))
show("2", q('{ user(id: "11") { name } }'))
--- response_body
1: {"data":{"user":{"name":"User 11"}}} sub=1 cache=l1=0;l2=0;l3=0;miss=1
2: {"data":{"user":{"name":"User 11"}}} sub=0 cache=l1=0;l2=1;l3=0;miss=0
