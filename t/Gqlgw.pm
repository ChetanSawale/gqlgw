# Shared Test::Nginx configuration for the gqlgw suite.
package t::Gqlgw;

use Test::Nginx::Socket -Base;

our $HttpConfig = <<'_EOC_';
    lua_package_path "$TEST_NGINX_ROOT/lua/?.lua;$TEST_NGINX_ROOT/lua/?/init.lua;$TEST_NGINX_ROOT/t/lib/?.lua;;";
    lua_shared_dict gqlgw_cache 10m;
    lua_shared_dict mock 1m;
    init_by_lua_block {
        require("gqlgw").init({
            schema = "$TEST_NGINX_ROOT/t/schemas/blog.gqlb",
            lib = "$TEST_NGINX_ROOT/build/libgql.so",
            cache = { shm = "gqlgw_cache", l2_ttl = 60 },
            debug_headers = true,
            __GQLGW_EXTRA__
        })
    }
    upstream mock {
        server 127.0.0.1:$TEST_NGINX_SERVER_PORT;
        keepalive 16;
    }
_EOC_

our $Config = <<'_EOC_';
    location = /graphql {
        set $gqlgw_path "";
        content_by_lua_block { require("gqlgw").serve() }
    }
    location = /purge {
        content_by_lua_block { require("gqlgw").purge() }
    }
    location /_src/users/ {
        internal;
        proxy_http_version 1.1;
        proxy_set_header Connection "";
        proxy_set_header Accept-Encoding "";
        proxy_pass http://mock/__mock$gqlgw_path;
    }
    location /_src/posts/ {
        internal;
        proxy_http_version 1.1;
        proxy_set_header Connection "";
        proxy_set_header Accept-Encoding "";
        proxy_pass http://mock/__mock$gqlgw_path;
    }
    location /_src/echo/ {
        internal;
        proxy_http_version 1.1;
        proxy_set_header Connection "";
        proxy_set_header Accept-Encoding "";
        proxy_pass http://mock/__mock$gqlgw_path;
    }
    location /__mock/ {
        content_by_lua_block { require("mock").handle() }
    }
_EOC_

add_block_preprocessor(sub {
    my $block = shift;
    my $extra = $block->gqlgw_extra // '';
    if (!defined $block->http_config) {
        (my $hc = $HttpConfig) =~ s/__GQLGW_EXTRA__/$extra/;
        $block->set_value("http_config", $hc);
    }
    if (!defined $block->config) {
        $block->set_value("config", $Config);
    }
    if (!defined $block->no_error_log && !defined $block->error_log) {
        $block->set_value("no_error_log", "[error]\n[alert]\n[crit]");
    }
});

1;
