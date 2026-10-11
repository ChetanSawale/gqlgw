# Test::Nginx configuration for the tests of the configuration rendered by
# `gqlc render` (t/config/*.toml, rendered by `make test-t`).
package t::GqlgwConfig;

use Test::Nginx::Socket -Base;

our $TLSServer = <<'_EOC_';
    lua_shared_dict mock 1m;
    # TLS mock backend (self-signed certificate for "localhost")
    server {
        listen $TEST_NGINX_GQLGW_TLS_PORT ssl;
        server_name localhost;
        ssl_certificate $TEST_NGINX_ROOT/t/config/run/tls/cert.pem;
        ssl_certificate_key $TEST_NGINX_ROOT/t/config/run/tls/key.pem;
        location /__mock/ {
            content_by_lua_block { require("mock").handle() }
        }
    }
_EOC_

# --locations-only: http part here, locations in the Test::Nginx server
our $HttpConfig = <<'_EOC_';
    lua_package_path "$TEST_NGINX_ROOT/lua/?.lua;$TEST_NGINX_ROOT/lua/?/init.lua;$TEST_NGINX_ROOT/t/lib/?.lua;;";
    include $TEST_NGINX_ROOT/t/config/run/gqlgw.http.conf;
_EOC_

our $Config = <<'_EOC_';
    include $TEST_NGINX_ROOT/t/config/run/gqlgw.locations.conf;
    location /__mock/ {
        content_by_lua_block { require("mock").handle() }
    }
    location /admin/ {
        proxy_pass http://127.0.0.1:$TEST_NGINX_GQLGW_ADMIN_PORT/;
    }
    # POST the expected metric lines: prints the missing ones, then "done"
    location = /check_metrics {
        content_by_lua_block {
            ngx.req.read_body()
            local res = ngx.location.capture("/admin/gqlgw_metrics")
            local have = {}
            for line in res.body:gmatch("[^\n]+") do
                have[line] = true
            end
            for line in (ngx.req.get_body_data() or ""):gmatch("[^\n]+") do
                if not have[line] then
                    ngx.say("missing: ", line)
                end
            end
            ngx.say("done")
        }
    }
_EOC_

# default mode: a gqlgw server {} of its own (t/config/server.toml)
our $ServerHttpConfig = <<'_EOC_';
    lua_package_path "$TEST_NGINX_ROOT/lua/?.lua;$TEST_NGINX_ROOT/lua/?/init.lua;$TEST_NGINX_ROOT/t/lib/?.lua;;";
    include $TEST_NGINX_ROOT/t/config/run-server/gqlgw.conf;
_EOC_

our $ServerConfig = <<'_EOC_';
    location /__mock/ {
        content_by_lua_block { require("mock").handle() }
    }
    location / {
        proxy_pass http://127.0.0.1:$TEST_NGINX_GQLGW_PORT;
    }
_EOC_

add_block_preprocessor(sub {
    my $block = shift;
    my $server = defined $block->gqlgw_server;
    if (!defined $block->http_config) {
        $block->set_value("http_config", ($server ? $ServerHttpConfig : $HttpConfig) . $TLSServer);
    }
    if (!defined $block->config) {
        $block->set_value("config", $server ? $ServerConfig : $Config);
    }
    if (!defined $block->no_error_log && !defined $block->error_log) {
        $block->set_value("no_error_log", "[error]\n[alert]\n[crit]");
    }
});

1;
