-- gqlgw: SDL-first GraphQL gateway for OpenResty.
--
--   init_by_lua_block    { require("gqlgw").init("/run/gqlgw/gqlgw.lua") }      -- written by gqlc render
--                     or { require("gqlgw").init({ schema = "/path/schema.gqlb", ... }) }
--   location = /graphql  { set $gqlgw_path ""; content_by_lua_block { require("gqlgw").serve() } }
--   location /_src/users/ { internal; proxy_pass http://users_svc$gqlgw_path; }
--   log_by_lua_block     { require("gqlgw").log() }          -- optional
--
-- See https://abusi.github.io/gqlgw/ and conf/nginx.conf.example.
local ffi = require "ffi"
local gffi = require "gqlgw.ffi"
local schema = require "gqlgw.schema"
local http = require "gqlgw.http"
local exec = require "gqlgw.exec"
local fetch = require "gqlgw.fetch"
local cache = require "gqlgw.cache"
local metrics = require "gqlgw.metrics"
local lrucache = require "resty.lrucache"

local ngx = ngx
local ffi_string = ffi.string
local tostring = tostring

local _M = { _VERSION = "0.2.0" }

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

-- configuration file written by gqlc render (a Lua chunk returning a table)
local function load_conf(path)
    local chunk, err = loadfile(path)
    if not chunk then
        error("gqlgw.init: cannot load " .. path .. ": " .. tostring(err), 3)
    end
    setfenv(chunk, {})
    local ok, t = pcall(chunk)
    if not ok or type(t) ~= "table" then
        error("gqlgw.init: " .. path .. " does not return a table: " .. tostring(t), 3)
    end
    t.config_path = path
    return t
end

-- every @fetch source of the schema needs a /_src/<source>/ location: when the
-- configuration lists its sources (gqlc render), check them now
local function check_sources(S_, sources)
    local known = {}
    for _, name in ipairs(sources) do
        known[name] = true
    end
    for _, f in pairs(S_.fields) do
        local src = f.route and f.route.fetch and f.route.fetch.source
        if src and not known[src] then
            error(("gqlgw.init: @fetch(source: %q) on %s has no [sources.%s] in the configuration")
                  :format(src, f.coord or f.name, src), 3)
        end
    end
end

-- opts: path of the file written by gqlc render, or a table:
-- schema (path to .gqlb), routes (default: <schema>.routes.lua),
-- lib (path to libgql.so), cache = { shm, memcached = {...}, l2_ttl, ... } and limits (see DEFAULTS)
function _M.init(opts)
    if type(opts) == "string" then
        opts = load_conf(opts)
    end
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
    if opts.sources then
        check_sources(S, opts.sources)
    end
    limits = ffi.new("gql_limits")
    limits.max_bytes = conf.max_query_bytes
    limits.max_tokens = conf.max_tokens
    limits.max_depth = conf.max_depth
    limits.max_errors = conf.max_errors
    limits.max_fields = conf.max_plan_fields
    limits.max_memory = conf.max_memory
    limits.introspection = conf.introspection and 1 or 0
    cache.init(conf.cache)
    metrics.init(conf.metrics, { version = _M._VERSION, schema_hash = S.hash })
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

-- ---- metrics (recorded in the log phase, see _M.log) --------------------------

local function count_errors(json)
    local _, n = json:gsub('"message":', "")
    return n
end

-- operation label when the document could not be planned: the client's
-- operationName, if it is a GraphQL name
local function client_op_label(name)
    if not name or name == "" then
        return metrics.op_label(nil)
    end
    if #name <= 128 and name:find("^[_A-Za-z][_0-9A-Za-z]*$") then
        return metrics.op_label(name)
    end
    return "__invalid"
end

-- every schema coordinate selected by the plan, once per request
local function count_fields(view)
    local seen = {}
    local fields = S.fields
    for i = 0, view.nfields - 1 do
        local fi = tonumber(view.fields[i].field)
        if not seen[fi] then
            seen[fi] = true
            local finfo = fields[fi]
            if finfo and finfo.coord then
                local l = finfo.mlabel
                if not l then
                    l = 'coordinate="' .. finfo.coord .. '"'
                    finfo.mlabel = l
                end
                metrics.inc("gqlgw_field_requests_total", l)
            end
        end
    end
end

function _M.serve()
    if not S then
        return http.fail(500, http.MT_JSON, "gqlgw is not initialized (call gqlgw.init in init_by_lua).")
    end
    local m = metrics.enabled() and { op = "__unknown", type = "unknown", result = "http_error" } or nil
    ngx.ctx.gqlgw_m = m
    local req = http.read_request(conf)
    local media = req.media
    local request_error_status = media == http.MT_GQL and 400 or 200

    local d, doc_hit = get_doc(req.query)
    if not d then
        return http.fail(500, media, "Out of memory.")
    end
    if m then
        m.op = client_op_label(req.operation_name)
        metrics.inc("gqlgw_doc_cache_total", doc_hit and 'result="hit"' or 'result="miss"')
    end
    if C.gql_doc_ok(d) == 0 then
        local e = ffi_string(C.gql_doc_errors(d, size_t_out), size_t_out[0])
        if m then
            m.result = "request_error"
            m.err_kind, m.nerr = "document", count_errors(e)
        end
        return http.send(request_error_status, media, '{"errors":' .. e .. '}')
    end
    local op = req.operation_name or ""
    local vars = req.variables
    local p = C.gql_plan_new(S.handle, d, op, #op, vars, vars and #vars or 0, limits)
    if p == nil then
        return http.fail(500, media, "Out of memory.")
    end
    p = ffi.gc(p, C.gql_plan_free)
    if C.gql_plan_ok(p) == 0 then
        local e = ffi_string(C.gql_plan_errors(p, size_t_out), size_t_out[0])
        if m then
            m.result = "request_error"
            m.err_kind, m.nerr = "operation", count_errors(e)
        end
        return http.send(request_error_status, media, '{"errors":' .. e .. '}')
    end
    local view = C.gql_plan_get_view(p)
    if m then
        m.op = metrics.op_label(ffi_string(C.gql_plan_operation_name(p, size_t_out), size_t_out[0]))
        m.type = view.op == 1 and "mutation" or "query"
    end
    if view.op == 1 and req.method == "GET" then
        return http.fail(405, media, "Mutations cannot be executed with GET: use POST.", { Allow = "POST" })
    end
    if m then
        count_fields(view)
    end
    local ctx = exec.new(S, view, fetch.dispatch, conf)
    ctx.plan = p    -- keep the plan alive while executing
    local body = ctx:execute()
    ngx.ctx.gqlgw = ctx.stats
    if m then
        local nerr = #ctx.errors
        m.result = nerr > 0 and "partial" or "ok"
        m.err_kind, m.nerr = "field", nerr
        m.rounds = ctx.stats.rounds
    end
    if conf.debug_headers then
        debug_headers(ctx, doc_hit)
    end
    return http.send(200, media, body)
end

-- /gqlgw_metrics (admin server)
function _M.metrics()
    if not metrics.enabled() then
        return http.fail(404, http.MT_JSON, "metrics are disabled")
    end
    local body = metrics.render()
    ngx.header["Content-Type"] = "text/plain; version=0.0.4; charset=utf-8"
    ngx.print(body)
    return ngx.exit(200)
end

-- log_by_lua: exposes per request metrics as nginx variables when declared
-- (set $gql_rounds ""; set $gql_subrequests ""; set $gql_cache "";)
function _M.log()
    local m = ngx.ctx.gqlgw_m
    if m then
        local op = 'operation="' .. metrics.esc(m.op) .. '"'
        metrics.inc("gqlgw_requests_total", op .. ',type="' .. m.type .. '",result="' .. m.result .. '"')
        metrics.observe("gqlgw_request_duration_seconds", op, ngx.now() - ngx.req.start_time())
        if m.nerr and m.nerr > 0 then
            metrics.inc("gqlgw_errors_total", 'kind="' .. m.err_kind .. '"', m.nerr)
        end
        if m.rounds then
            metrics.observe("gqlgw_rounds", "", m.rounds)
        end
    end
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
