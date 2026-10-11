-- gqlgw.fetch: one round of upstream fetches.
--
-- For every queued field: render the @fetch templates, deduplicate identical
-- requests, look the resources up in the cache (shm, then memcached), group
-- the misses of @batch fields into batch requests, and run all remaining
-- subrequests in parallel (ngx.location.capture_multi through the internal
-- locations /_src/<source>/...).
local cjson = require "cjson.safe"
local exec = require "gqlgw.exec"
local cache = require "gqlgw.cache"

local ngx = ngx
local type, tostring, pairs, ipairs, unpack = type, tostring, pairs, ipairs, unpack
local table_concat = table.concat
local escape_uri = ngx.escape_uri
local capture_multi = ngx.location.capture_multi
local null = cjson.null
local array_mt = cjson.array_mt
local num_json = exec.num_json
local field_error = exec.field_error

local decode_json = cjson.new()
decode_json.decode_array_with_array_mt(true)
local encode_json = cjson.new()
encode_json.encode_escape_forward_slash(false)
encode_json.encode_number_precision(16)

local METHODS = {
    GET = ngx.HTTP_GET, POST = ngx.HTTP_POST, PUT = ngx.HTTP_PUT,
    PATCH = ngx.HTTP_PATCH, DELETE = ngx.HTTP_DELETE,
}

local _M = {}

local function scalar_str(v)
    local t = type(v)
    if t == "string" then
        return v
    elseif t == "number" then
        return num_json(v) or ""
    elseif t == "boolean" then
        return v and "true" or "false"
    end
    return nil
end
_M.scalar_str = scalar_str

-- values interpolated in a path segment must not contain "/" nor be "." / ".."
-- (defense in depth: some backends decode %2F before routing)
local function unsafe_segment(s)
    return s == "." or s == ".." or s:find("/", 1, true) ~= nil or s:find("\\", 1, true) ~= nil
end

local function path_value(v, strict)
    if v == nil or v == null then
        return ""
    end
    if type(v) == "table" then
        local parts = {}
        for j = 1, #v do
            local s = scalar_str(v[j]) or ""
            if strict and unsafe_segment(s) then
                return nil
            end
            parts[j] = escape_uri(s)
        end
        return table_concat(parts, ",")
    end
    local s = scalar_str(v) or ""
    if strict and unsafe_segment(s) then
        return nil
    end
    return escape_uri(s)
end

local function dig(t, part, from)
    local v = t
    for j = from, #part do
        if type(v) ~= "table" then
            return nil
        end
        v = v[part[j]]
    end
    return v
end

-- returns the rendered path, or nil and an error message (strict mode)
local function render_path(tpl, args, parent, keys, strict)
    local out = {}
    local in_query = false
    for j = 1, #tpl do
        local p = tpl[j]
        if type(p) == "string" then
            out[j] = p
            if not in_query and p:find("?", 1, true) then
                in_query = true
            end
        else
            local kind = p[1]
            local s
            if kind == "args" then
                s = path_value(dig(args, p, 2), strict and not in_query)
            elseif kind == "parent" then
                s = path_value(parent and parent[p[2]], strict and not in_query)
            else
                s = keys or ""
            end
            if not s then
                return nil, "Invalid value for an upstream path segment (contains \"/\" or is \".\" / \"..\")."
            end
            out[j] = s
        end
    end
    return table_concat(out)
end
_M.render_path = render_path

-- args_json: the coerced arguments as produced by libgql (ordered keys)
local function render_body(tpl, args, args_json)
    local out = {}
    for j = 1, #tpl do
        local p = tpl[j]
        if type(p) == "string" then
            out[j] = p
        elseif #p == 1 then
            out[j] = args_json or encode_json.encode(args) or "{}"
        else
            local v = dig(args, p, 2)
            out[j] = (v == nil) and "null" or (encode_json.encode(v) or "null")
        end
    end
    return table_concat(out)
end
_M.render_body = render_body

local function select(v, sel)
    if not sel then
        return v
    end
    for j = 1, #sel do
        if type(v) ~= "table" then
            return null
        end
        v = v[sel[j]]
    end
    if v == nil then
        return null
    end
    return v
end

-- the raw key of the single {parent.*} reference of a batched @fetch path
local function batch_parent_key(route)
    local k = route._parent_key
    if k == nil then
        k = false
        for _, p in ipairs(route.fetch.path) do
            if type(p) == "table" and p[1] == "parent" then
                k = p[2]
            end
        end
        route._parent_key = k
    end
    return k
end

local function decode_response(res, source, max_body)
    local status = res.status
    if status == 404 then
        return null, nil, true
    end
    if status < 200 or status >= 300 then
        return nil, 'Upstream "' .. source .. '" returned HTTP ' .. status .. '.'
    end
    if res.truncated then
        return nil, 'Upstream "' .. source .. '" response was truncated.'
    end
    local body = res.body
    if status == 204 or body == nil or body == "" then
        return null
    end
    if max_body and #body > max_body then
        return nil, 'Upstream "' .. source .. '" response exceeds ' .. max_body .. ' bytes.'
    end
    local v = decode_json.decode(body)
    if v == nil then
        return nil, 'Upstream "' .. source .. '" returned invalid JSON.'
    end
    return v
end

local function fill_all(ctx, items, value)
    for j = 1, #items do
        ctx:fill(items[j], value)
    end
end

function _M.dispatch(ctx, items)
    local conf = ctx.opts
    local prefix = conf.sources_prefix or "/_src/"
    local strict = conf.strict_path_args ~= false
    local stats = ctx.stats
    local units, unit_list = {}, {}
    local groups, group_list = {}, {}

    -- 1. render and deduplicate
    for _, item in ipairs(items) do
        local route = item.finfo.route
        local fetch = route.fetch
        local args = ctx:field_args(item.i)
        local parent = item.node.src
        local path, perr = render_path(fetch.path, args, parent, nil, strict)
        local get = fetch.method == "GET"
        local ckey = path and get and (fetch.source .. ":" .. path) or nil
        if not path then
            ctx:fill(item, field_error(perr))
        elseif route.batch then
            local kv = parent[batch_parent_key(route)]
            if kv == nil or kv == null then
                ctx:fill(item, null)
            else
                local ks = scalar_str(kv)
                local bpath = render_path(route.batch.path, args, nil, "\1")
                local gkey = item.finfo.coord .. "\0" .. bpath
                local g = groups[gkey]
                if not g then
                    g = { route = route, args = args, finfo = item.finfo, entries = {}, order = {} }
                    groups[gkey] = g
                    group_list[#group_list + 1] = g
                end
                local e = g.entries[ks]
                if not e then
                    e = { items = {}, ckey = ckey, key = ks }
                    g.entries[ks] = e
                    g.order[#g.order + 1] = ks
                end
                e.items[#e.items + 1] = item
            end
        else
            local u = ckey and units[ckey]
            if not u then
                u = {
                    uri = prefix .. fetch.source .. "/",
                    path = path,
                    method = fetch.method,
                    body = fetch.body and render_body(fetch.body, args, ctx:field_args_json(item.i)) or nil,
                    ckey = ckey, route = route, items = {},
                }
                if ckey then
                    units[ckey] = u
                end
                unit_list[#unit_list + 1] = u
            end
            u.items[#u.items + 1] = item
        end
    end

    -- 2. cache lookups
    local lookups = {}
    for _, u in ipairs(unit_list) do
        if u.ckey and u.route.cache then
            u.cache_key = cache.key(ctx, u.route, u.ckey)
            lookups[#lookups + 1] = u.cache_key
        end
    end
    for _, g in ipairs(group_list) do
        if g.route.cache then
            for _, ks in ipairs(g.order) do
                local e = g.entries[ks]
                e.cache_key = cache.key(ctx, g.route, e.ckey)
                lookups[#lookups + 1] = e.cache_key
            end
        end
    end
    local hits = #lookups > 0 and cache.get_multi(ctx, lookups) or {}
    local stores = {}

    -- 3. subrequests for the misses
    local reqs, handlers = {}, {}
    for _, u in ipairs(unit_list) do
        local cached = u.cache_key and hits[u.cache_key]
        if cached ~= nil then
            fill_all(ctx, u.items, cached)
        else
            reqs[#reqs + 1] = { u.uri, { method = METHODS[u.method], body = u.body, vars = { gqlgw_path = u.path } } }
            handlers[#handlers + 1] = u
        end
    end
    for _, g in ipairs(group_list) do
        local pending = {}
        for _, ks in ipairs(g.order) do
            local e = g.entries[ks]
            local cached = e.cache_key and hits[e.cache_key]
            if cached ~= nil then
                fill_all(ctx, e.items, cached)
            else
                pending[#pending + 1] = ks
            end
        end
        local max = g.route.batch.max or 100
        local source = g.route.fetch.source
        for off = 1, #pending, max do
            local keys, escaped = {}, {}
            for j = off, math.min(off + max - 1, #pending) do
                keys[#keys + 1] = pending[j]
                escaped[#escaped + 1] = escape_uri(pending[j])
            end
            local bpath = render_path(g.route.batch.path, g.args, nil, table_concat(escaped, ","))
            reqs[#reqs + 1] = { prefix .. source .. "/", { method = ngx.HTTP_GET, vars = { gqlgw_path = bpath } } }
            handlers[#handlers + 1] = { group = g, keys = keys }
            stats.batches = stats.batches + 1
        end
    end

    local max_fetches = conf.max_fetches or 1000
    if stats.subrequests + #reqs > max_fetches then
        local msg = "Too many upstream requests for this query (limit " .. max_fetches .. ")."
        for _, h in ipairs(handlers) do
            if h.group then
                for _, ks in ipairs(h.keys) do
                    fill_all(ctx, h.group.entries[ks].items, field_error(msg))
                end
            else
                fill_all(ctx, h.items, field_error(msg))
            end
        end
        return
    end

    local conc = conf.concurrency or 64
    local max_body = conf.max_response_bytes
    local neg_ttl = conf.cache_negative_ttl or 5
    for off = 1, #reqs, conc do
        local last = math.min(off + conc - 1, #reqs)
        local slice = { unpack(reqs, off, last) }
        stats.subrequests = stats.subrequests + #slice
        local results = { capture_multi(slice) }
        for j = 1, #slice do
            local h, res = handlers[off + j - 1], results[j]
            if h.group then
                local g = h.group
                local route = g.route
                local v, err = decode_response(res, route.fetch.source, max_body)
                local list = v ~= nil and select(v, route.batch.select)
                if v ~= nil and (type(list) ~= "table" or (getmetatable(list) ~= array_mt and next(list) ~= nil)) then
                    if list == null then
                        list = {}
                    else
                        err = 'Upstream "' .. route.fetch.source .. '" batch response is not a JSON array.'
                        v = nil
                    end
                end
                if v == nil then
                    for _, ks in ipairs(h.keys) do
                        fill_all(ctx, g.entries[ks].items, field_error(err))
                    end
                else
                    local by_key = {}
                    local match = route.batch.match
                    for _, obj in ipairs(list) do
                        if type(obj) == "table" then
                            local id = scalar_str(obj[match])
                            if id then
                                by_key[id] = obj
                            end
                        end
                    end
                    for _, ks in ipairs(h.keys) do
                        local e = g.entries[ks]
                        local obj = by_key[ks]
                        fill_all(ctx, e.items, obj or null)
                        if e.cache_key then
                            stores[#stores + 1] = {
                                key = e.cache_key,
                                value = obj and encode_json.encode(obj) or "null",
                                decoded = obj or null,
                                ttl = obj and route.cache.ttl or math.min(route.cache.ttl, neg_ttl),
                            }
                        end
                    end
                end
            else
                local u = h
                local v, err, not_found = decode_response(res, u.route.fetch.source, max_body)
                if v == nil then
                    fill_all(ctx, u.items, field_error(err))
                else
                    local value = select(v, u.route.fetch.select)
                    fill_all(ctx, u.items, value)
                    if u.cache_key then
                        stores[#stores + 1] = {
                            key = u.cache_key,
                            value = encode_json.encode(value) or "null",
                            decoded = value,
                            ttl = not_found and math.min(u.route.cache.ttl, neg_ttl) or u.route.cache.ttl,
                        }
                    end
                end
            end
        end
    end
    if #stores > 0 then
        cache.set_multi(ctx, stores)
    end
end

return _M
