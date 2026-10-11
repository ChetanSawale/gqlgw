-- gqlgw.cache: three level resource cache.
--
--   L1: lua-resty-lrucache per worker, holding decoded values (no JSON decode)
--   L2: lua_shared_dict (per node, short TTL)
--   L3: memcached, a single endpoint (it may be a mcrouter; clustering is not
--       our concern). Reads are one multi-get per round, writes are pipelined
--       from a timer so they never delay the response. Fail-open: when
--       memcached is unavailable the gateway keeps serving from upstream.
--
-- Keys are REST resources ("<source>:<path>"), not GraphQL selections, so all
-- queries share entries. A per-source generation counter is part of the key:
-- purging a source is a single increment.
local memcached = require "resty.memcached"
local lrucache = require "resty.lrucache"
local metrics = require "gqlgw.metrics"
local cjson = require "cjson.safe"

local ngx = ngx
local ngx_md5 = ngx.md5
local ipairs, tostring, tonumber = ipairs, tostring, tonumber

local decoder = cjson.new()
decoder.decode_array_with_array_mt(true)
local l1                 -- per worker lrucache, created lazily

local _M = {}

local conf = {
    shm = nil,               -- ngx.shared dict
    l2_ttl = 2,              -- seconds, upper bound for L2 entries
    gen_ttl = 1,             -- seconds a generation number is cached in L2
    memcached = nil,         -- { host, port, timeout, pool_size, keepalive, prefix } or nil
    private_header = "authorization",
    prefix = "gqlgw:",
    down_ttl = 5,            -- seconds memcached is skipped after a failure
}

function _M.init(c)
    c = c or {}
    if c.shm then
        local dict = ngx.shared[c.shm]
        if not dict then
            error("gqlgw: lua_shared_dict \"" .. c.shm .. "\" is not defined")
        end
        conf.shm = dict
    end
    conf.l2_ttl = c.l2_ttl or conf.l2_ttl
    conf.gen_ttl = c.gen_ttl or conf.gen_ttl
    conf.memcached = c.memcached
    conf.private_header = (c.private_header or conf.private_header):lower():gsub("-", "_")
    conf.prefix = c.prefix or conf.prefix
    conf.down_ttl = c.down_ttl or conf.down_ttl
    conf.l1_size = c.l1_size or 10000
    conf.l1_ttl = c.l1_ttl or 1
    _M.conf = conf
end

local function get_l1()
    if not l1 and conf.l1_size > 0 then
        l1 = assert(lrucache.new(conf.l1_size))
    end
    return l1
end

-- "memcached is down" is remembered per worker, and per node when a shm is configured
local down_until = 0

local function memc_down()
    if ngx.now() < down_until then
        return true
    end
    return conf.shm and conf.shm:get("gqlgw:memc_down")
end

local function mark_down(err)
    ngx.log(ngx.WARN, "gqlgw: memcached unavailable, skipping it for ", conf.down_ttl, "s: ", err)
    if metrics.enabled() then
        metrics.inc("gqlgw_memcached_errors_total", "")
    end
    down_until = ngx.now() + conf.down_ttl
    if conf.shm then
        conf.shm:set("gqlgw:memc_down", true, conf.down_ttl)
    end
end

local function connect()
    local m = conf.memcached
    local memc, err = memcached:new({ key_transform = { function(k) return k end, function(k) return k end } })
    if not memc then
        return nil, err
    end
    memc:set_timeout(m.timeout or 100)
    local ok
    if m.socket then
        ok, err = memc:connect("unix:" .. m.socket)
    else
        ok, err = memc:connect(m.host or "127.0.0.1", m.port or 11211)
    end
    if not ok then
        return nil, err
    end
    return memc
end

local function release(memc)
    local m = conf.memcached
    memc:set_keepalive(m.keepalive or 60000, m.pool_size or 64)
end

-- current generation of a source (memcached is the source of truth)
local function generation(source)
    local gk = conf.prefix .. "gen:" .. source
    local shm = conf.shm
    local g = shm and shm:get(gk)
    if g then
        return g
    end
    g = "0"
    if conf.memcached and not memc_down() then
        local memc, err = connect()
        if memc then
            local v
            v, _, err = memc:get(gk)
            if v then
                g = v
            elseif err then
                mark_down(err)
            end
            release(memc)
        else
            mark_down(err)
        end
    end
    if shm then
        shm:set(gk, g, conf.gen_ttl)
    end
    return g
end

-- cache key of a resource; ctx caches the generations and the identity per request
function _M.key(ctx, route, rkey)
    local source = route.fetch.source
    local gens = ctx.gens
    if not gens then
        gens = {}
        ctx.gens = gens
    end
    local g = gens[source]
    if not g then
        g = generation(source)
        gens[source] = g
    end
    local k = conf.prefix .. g .. ":" .. rkey
    if route.cache.scope == "PRIVATE" then
        local id = ctx.identity
        if id == nil then
            local h = ngx.var["http_" .. conf.private_header]
            id = h and ngx_md5(h) or ""
            ctx.identity = id
        end
        k = k .. "|" .. id
    end
    -- memcached keys: at most 250 bytes; resource keys never contain spaces or
    -- control characters (paths are URI-escaped, sources are [A-Za-z0-9_-]+)
    if #k > 200 then
        k = conf.prefix .. "h:" .. ngx_md5(k)
    end
    return k
end

-- returns { [key] = decoded value } for the keys found, and { [key] = "l1" | "l2" | "l3" }
function _M.get_multi(ctx, keys)
    local found, misses, levels = {}, {}, {}
    local stats = ctx.stats
    local shm = conf.shm
    local c1 = get_l1()
    for _, k in ipairs(keys) do
        local v = c1 and c1:get(k)
        if v ~= nil then
            found[k] = v
            levels[k] = "l1"
            stats.hits_l1 = stats.hits_l1 + 1
        else
            local js = shm and shm:get(k)
            if js ~= nil then
                v = decoder.decode(js)
                found[k] = v
                levels[k] = "l2"
                stats.hits_l2 = stats.hits_l2 + 1
                if c1 then
                    c1:set(k, v, conf.l1_ttl)
                end
            else
                misses[#misses + 1] = k
            end
        end
    end
    if #misses == 0 or not conf.memcached or memc_down() then
        stats.misses = (stats.misses or 0) + #misses
        return found, levels
    end
    local memc, err = connect()
    if not memc then
        mark_down(err)
        stats.misses = (stats.misses or 0) + #misses
        return found, levels
    end
    local res
    res, err = memc:get(misses)
    if not res then
        mark_down(err)
        memc:close()
        stats.misses = (stats.misses or 0) + #misses
        return found, levels
    end
    release(memc)
    local nmiss = 0
    for _, k in ipairs(misses) do
        local r = res[k]
        if r then
            local v = decoder.decode(r[1])
            found[k] = v
            levels[k] = "l3"
            stats.hits_l3 = stats.hits_l3 + 1
            if shm then
                shm:set(k, r[1], conf.l2_ttl)
            end
            if c1 then
                c1:set(k, v, conf.l1_ttl)
            end
        else
            nmiss = nmiss + 1
        end
    end
    stats.misses = (stats.misses or 0) + nmiss
    return found, levels
end

local function write_memcached(premature, items)
    if premature then
        return
    end
    local memc, err = connect()
    if not memc then
        mark_down(err)
        return
    end
    memc:init_pipeline(#items)
    for _, it in ipairs(items) do
        memc:set(it.key, it.value, it.ttl)
    end
    local results
    results, err = memc:commit_pipeline()
    if not results then
        mark_down(err)
        memc:close()
        return
    end
    release(memc)
end

-- items: { { key, value (JSON text), ttl, decoded (optional) } }
function _M.set_multi(ctx, items)
    local shm = conf.shm
    local c1 = get_l1()
    if c1 then
        for _, it in ipairs(items) do
            if it.decoded ~= nil then
                c1:set(it.key, it.decoded, it.ttl < conf.l1_ttl and it.ttl or conf.l1_ttl)
            end
        end
    end
    if shm then
        for _, it in ipairs(items) do
            local ttl = it.ttl
            if ttl > conf.l2_ttl then
                ttl = conf.l2_ttl
            end
            if ttl > 0 then
                shm:set(it.key, it.value, ttl)
            end
        end
    end
    if conf.memcached and not memc_down() then
        local ok, err = ngx.timer.at(0, write_memcached, items)
        if not ok then
            ngx.log(ngx.WARN, "gqlgw: cannot schedule memcached write: ", err)
        end
    end
end

-- purges every cached resource of a source (all nodes: memcached generation;
-- this node: L2 generation). Returns the new generation.
function _M.purge_source(source)
    local gk = conf.prefix .. "gen:" .. source
    local g
    if conf.memcached then
        local memc, err = connect()
        if not memc then
            return nil, err
        end
        local v
        v, err = memc:incr(gk, 1)
        if not v then
            -- first purge: initialize the counter
            local ok
            ok, err = memc:add(gk, "1")
            v = ok and "1" or nil
            if not v then
                v, err = memc:incr(gk, 1)
            end
        end
        release(memc)
        if not v then
            return nil, err
        end
        g = tostring(v)
    else
        local shm = conf.shm
        g = tostring((tonumber(shm and shm:get(gk)) or 0) + 1)
    end
    if conf.shm then
        conf.shm:set(gk, g, conf.memcached and conf.gen_ttl or 0)
    end
    return g
end

-- note: keys embed the generation, so purged L1 entries are simply never read again

return _M
