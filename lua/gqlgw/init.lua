-- gqlgw: SDL-first GraphQL gateway for OpenResty.
--
--   init_by_lua_block    { require("gqlgw").init({ schema = "/path/schema.gqlb", ... }) }
--   location = /graphql  { set $gqlgw_path ""; content_by_lua_block { require("gqlgw").serve() } }
--   location /_src/users/ { internal; proxy_pass http://users_svc$gqlgw_path; }
--   log_by_lua_block     { require("gqlgw").log() }          -- optional
--
-- See PLAN.md and conf/nginx.conf.example.
local ffi = require "ffi"
local gffi = require "gqlgw.ffi"
local schema = require "gqlgw.schema"
local http = require "gqlgw.http"
local exec = require "gqlgw.exec"
local fetch = require "gqlgw.fetch"
local cache = require "gqlgw.cache"
local lrucache = require "resty.lrucache"

local ngx = ngx
local ffi_string = ffi.string
local tostring = tostring

local _M = { _VERSION = "0.1.0" }

local S              -- loaded schema
local C              -- libgql
local limits         -- gql_limits cdata
local conf           -- options
local docs           -- per worker LRU: query text -> gql_doc*
local size_t_out = ffi.new("size_t[1]")

local DEFAULTS = {
    sources_prefix = "/_src/",
    introspection = true,
    doc_cache_size = 512,
    max_query_bytes = 65536,
    max_tokens = 20000,
    max_depth = 32,
    max_errors = 100,
    max_plan_fields = 20000,
    max_memory = 16 * 1024 * 1024,
    max_rounds = 16,
    max_fetches = 1000,
    concurrency = 64,
    max_response_bytes = 16 * 1024 * 1024,
    max_body_bytes = 1048576,
    cache_negative_ttl = 5,
    debug_headers = false,
}

-- opts: schema (path to .gqlb), routes (default: <schema>.routes.lua),
-- lib (path to libgql.so), cache = { shm, memcached = {...}, l2_ttl, ... } and limits (see DEFAULTS)
function _M.init(opts)
    if not opts or not opts.schema then
        error("gqlgw.init: opts.schema (path to the compiled .gqlb) is required")
    end
    conf = {}
    for k, v in pairs(DEFAULTS) do
        conf[k] = v
    end
    for k, v in pairs(opts) do
        conf[k] = v
    end
    C = gffi.load(opts.lib)
    S = schema.load(opts.schema, opts.routes)
    limits = ffi.new("gql_limits")
    limits.max_bytes = conf.max_query_bytes
    limits.max_tokens = conf.max_tokens
    limits.max_depth = conf.max_depth
    limits.max_errors = conf.max_errors
    limits.max_fields = conf.max_plan_fields
    limits.max_memory = conf.max_memory
    limits.introspection = conf.introspection and 1 or 0
    cache.init(conf.cache)
    _M.schema = S
    _M.conf = conf
    return S
end

-- per worker document cache (created lazily: init_by_lua runs in the master)
local function doc_cache()
    if not docs then
        docs = assert(lrucache.new(conf.doc_cache_size))
    end
    return docs
end

local function get_doc(query)
    local cache_ = doc_cache()
    local d = cache_:get(query)
    if d then
        return d, true
    end
    d = C.gql_doc_new(S.handle, query, #query, limits)
    if d == nil then
        return nil
    end
    d = ffi.gc(d, C.gql_doc_free)
    cache_:set(query, d)
    return d, false
end

local function debug_headers(ctx, doc_hit)
    local st = ctx.stats
    ngx.header["X-GQL-Rounds"] = st.rounds
    ngx.header["X-GQL-Subrequests"] = st.subrequests
    ngx.header["X-GQL-Batches"] = st.batches
    ngx.header["X-GQL-Cache"] = "l1=" .. st.hits_l1 .. ";l2=" .. st.hits_l2 .. ";l3=" .. st.hits_l3 .. ";miss=" .. st.misses
    ngx.header["X-GQL-Doc-Cache"] = doc_hit and "hit" or "miss"
end

function _M.serve()
    if not S then
        return http.fail(500, http.MT_JSON, "gqlgw is not initialized (call gqlgw.init in init_by_lua).")
    end
    local req = http.read_request(conf)
    local media = req.media
    local request_error_status = media == http.MT_GQL and 400 or 200

    local d, doc_hit = get_doc(req.query)
    if not d then
        return http.fail(500, media, "Out of memory.")
    end
    if C.gql_doc_ok(d) == 0 then
        local e = C.gql_doc_errors(d, size_t_out)
        return http.send(request_error_status, media, '{"errors":' .. ffi_string(e, size_t_out[0]) .. '}')
    end
    local op = req.operation_name or ""
    local vars = req.variables
    local p = C.gql_plan_new(S.handle, d, op, #op, vars, vars and #vars or 0, limits)
    if p == nil then
        return http.fail(500, media, "Out of memory.")
    end
    p = ffi.gc(p, C.gql_plan_free)
    if C.gql_plan_ok(p) == 0 then
        local e = C.gql_plan_errors(p, size_t_out)
        return http.send(request_error_status, media, '{"errors":' .. ffi_string(e, size_t_out[0]) .. '}')
    end
    local view = C.gql_plan_get_view(p)
    if view.op == 1 and req.method == "GET" then
        return http.fail(405, media, "Mutations cannot be executed with GET: use POST.", { Allow = "POST" })
    end
    local ctx = exec.new(S, view, fetch.dispatch, conf)
    ctx.plan = p    -- keep the plan alive while executing
    local body = ctx:execute()
    ngx.ctx.gqlgw = ctx.stats
    if conf.debug_headers then
        debug_headers(ctx, doc_hit)
    end
    return http.send(200, media, body)
end

-- log_by_lua: exposes per request metrics as nginx variables when declared
-- (set $gql_rounds ""; set $gql_subrequests ""; set $gql_cache "";)
function _M.log()
    local st = ngx.ctx.gqlgw
    if not st then
        return
    end
    local v = ngx.var
    pcall(function() v.gql_rounds = st.rounds end)
    pcall(function() v.gql_subrequests = st.subrequests end)
    pcall(function()
        v.gql_cache = "l1=" .. st.hits_l1 .. ";l2=" .. st.hits_l2 .. ";l3=" .. st.hits_l3 .. ";miss=" .. st.misses
    end)
end

-- admin handler: POST ?source=<name> purges every cached resource of a source
function _M.purge()
    local source = ngx.var.arg_source
    if ngx.req.get_method() ~= "POST" or not source or source == "" then
        return http.fail(400, http.MT_JSON, "usage: POST ?source=<name>")
    end
    local g, err = cache.purge_source(source)
    if not g then
        return http.fail(502, http.MT_JSON, "purge failed: " .. tostring(err))
    end
    return http.send(200, http.MT_JSON, '{"source":' .. require("cjson").encode(source) .. ',"generation":' .. g .. '}')
end

return _M
