-- gqlgw.metrics: business metrics in the Prometheus text format.
--
-- Each worker aggregates into a Lua table; a timer adds it to the shared dict
-- every second, and the metrics handler renders the shared dict (after
-- flushing its own worker). Dict keys: family \t suffix \t labels \t le.
-- Histogram buckets are stored per bucket and made cumulative at rendering.
local ngx = ngx
local re_find = ngx.re.find
local pairs, ipairs, tostring, tonumber = pairs, ipairs, tostring, tonumber
local table_concat, table_sort = table.concat, table.sort
local str_format = string.format

local _M = {}

local FAMILIES = {
    gqlgw_requests_total = { "counter",
        "GraphQL requests by operation name, operation type and result (ok, partial, request_error, http_error)." },
    gqlgw_request_duration_seconds = { "histogram", "GraphQL request duration by operation name." },
    gqlgw_errors_total = { "counter", "GraphQL errors by kind (document, operation, field)." },
    gqlgw_field_requests_total = { "counter", "GraphQL requests selecting a field, by schema coordinate." },
    gqlgw_upstream_requests_total = { "counter", "Upstream requests by source and status (2xx, 3xx, 404, 4xx, 5xx)." },
    gqlgw_upstream_duration_seconds = { "histogram", "Upstream response time (until headers) by source." },
    gqlgw_batches_total = { "counter", "Batched upstream requests by field coordinate." },
    gqlgw_batch_size = { "histogram", "Keys per batched upstream request, by field coordinate." },
    gqlgw_cache_total = { "counter", "Resource cache lookups by source and result (l1, l2, l3, miss)." },
    gqlgw_memcached_errors_total = { "counter", "memcached failures (memcached is then skipped for a while)." },
    gqlgw_rounds = { "histogram", "Fetch rounds per GraphQL request." },
    gqlgw_doc_cache_total = { "counter", "Validated document cache lookups by result (hit, miss)." },
    gqlgw_info = { "gauge", "gqlgw version and schema hash." },
}

local SIZE_BUCKETS = { 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000 }
local ROUND_BUCKETS = { 0, 1, 2, 3, 4, 6, 8, 12, 16, 32 }

local conf          -- { dict, exclude, buckets, info }
local buckets_of = {}
local acc, nacc = {}, 0
local timer_started = false
local op_labels, n_op_labels = {}, 0

function _M.enabled()
    return conf ~= nil
end

local function num(v)
    if v == math.floor(v) and v > -1e15 and v < 1e15 then
        return str_format("%d", v)
    end
    return str_format("%.15g", v)
end

-- label value escaping (text format 0.0.4)
local function esc(v)
    v = tostring(v)
    if v:find('[\\"\n]') then
        v = v:gsub("\\", "\\\\"):gsub('"', '\\"'):gsub("\n", "\\n")
    end
    return v
end
_M.esc = esc

-- c: { shm = name, exclude_operations = {regex}, buckets = {seconds} }, info: { version, schema_hash }
function _M.init(c, info)
    if not c then
        conf = nil
        return
    end
    local dict = ngx.shared[c.shm or "gqlgw_metrics"]
    if not dict then
        error("gqlgw: lua_shared_dict \"" .. tostring(c.shm) .. "\" is not defined", 2)
    end
    local exclude = {}
    for _, re in ipairs(c.exclude_operations or {}) do
        local _, _, err = re_find("", re, "jo")
        if err then
            error("gqlgw: [metrics] exclude_operations: invalid regex " .. str_format("%q", re) .. ": " .. err, 2)
        end
        exclude[#exclude + 1] = re
    end
    local b = c.buckets or { 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1, 2.5, 5, 10 }
    buckets_of = {
        gqlgw_request_duration_seconds = b,
        gqlgw_upstream_duration_seconds = b,
        gqlgw_batch_size = SIZE_BUCKETS,
        gqlgw_rounds = ROUND_BUCKETS,
    }
    conf = { dict = dict, exclude = exclude, info = info or {} }
end

local function flush()
    if nacc == 0 then
        return
    end
    local a = acc
    acc, nacc = {}, 0
    local dict = conf.dict
    for k, v in pairs(a) do
        local ok, err = dict:incr(k, v, 0)
        if not ok then
            ngx.log(ngx.WARN, "gqlgw: metrics dropped (", err, "): increase [metrics] shm_size")
        end
    end
end

local function on_timer()
    flush()
end

local function add(key, v)
    local cur = acc[key]
    if cur then
        acc[key] = cur + v
    else
        acc[key] = v
        nacc = nacc + 1
        if not timer_started then
            timer_started = true
            local ok, err = ngx.timer.every(1, on_timer)
            if not ok then
                timer_started = false
                ngx.log(ngx.WARN, "gqlgw: cannot start the metrics timer: ", err)
            end
        end
    end
end

-- dict keys are memoized per (family, labels): no string building per call
local keys, nkeys = {}, 0

local function family_keys(family, labels)
    local fk = keys[family]
    if not fk then
        fk = {}
        keys[family] = fk
    end
    local k = fk[labels]
    if not k then
        if nkeys >= 50000 then
            keys, nkeys = {}, 0
            fk = {}
            keys[family] = fk
        end
        nkeys = nkeys + 1
        local list = buckets_of[family]
        if list then
            k = { sum = family .. "\t_sum\t" .. labels .. "\t", count = family .. "\t_count\t" .. labels .. "\t" }
            for i = 1, #list do
                k[i] = family .. "\t_bucket\t" .. labels .. "\t" .. i
            end
        else
            k = family .. "\t\t" .. labels .. "\t"
        end
        fk[labels] = k
    end
    return k
end

-- counters; labels: 'name="value",...' (already escaped)
function _M.inc(family, labels, v)
    add(family_keys(family, labels), v or 1)
end

function _M.observe(family, labels, v)
    local k = family_keys(family, labels)
    local list = buckets_of[family]
    for i = 1, #list do
        if v <= list[i] then
            add(k[i], 1)
            break
        end
    end
    add(k.sum, v)
    add(k.count, 1)
end

-- label of an operation name: "__anonymous", "__excluded" or the name
function _M.op_label(name)
    if not name or name == "" then
        return "__anonymous"
    end
    local l = op_labels[name]
    if l then
        return l
    end
    l = name
    for _, re in ipairs(conf.exclude) do
        if re_find(name, re, "jo") then
            l = "__excluded"
            break
        end
    end
    if n_op_labels >= 10000 then
        op_labels, n_op_labels = {}, 0
    end
    op_labels[name] = l
    n_op_labels = n_op_labels + 1
    return l
end

-- ---- rendering --------------------------------------------------------------

local function series(family, suffix, labels, extra, value, out)
    local l = labels
    if extra then
        l = l ~= "" and (l .. "," .. extra) or extra
    end
    if l ~= "" then
        out[#out + 1] = family .. suffix .. "{" .. l .. "} " .. value .. "\n"
    else
        out[#out + 1] = family .. suffix .. " " .. value .. "\n"
    end
end

function _M.render()
    flush()
    local dict = conf.dict
    local fams = {}
    for _, k in ipairs(dict:get_keys(0)) do
        local family, suffix, labels, le = k:match("^([^\t]*)\t([^\t]*)\t([^\t]*)\t(.*)$")
        local v = family and dict:get(k)
        if v and FAMILIES[family] then
            local f = fams[family]
            if not f then
                f = {}
                fams[family] = f
            end
            local s = f[labels]
            if not s then
                s = { buckets = {} }
                f[labels] = s
            end
            if suffix == "_bucket" then
                s.buckets[tonumber(le)] = v
            elseif suffix == "_sum" then
                s.sum = v
            elseif suffix == "_count" then
                s.count = v
            else
                s.value = v
            end
        end
    end
    local info = conf.info
    fams.gqlgw_info = { [str_format('version="%s",schema_hash="%s"', esc(info.version or ""),
                                    esc(info.schema_hash or ""))] = { value = 1 } }

    local names = {}
    for name in pairs(fams) do
        names[#names + 1] = name
    end
    table_sort(names)
    local out = {}
    for _, name in ipairs(names) do
        local meta = FAMILIES[name]
        out[#out + 1] = "# HELP " .. name .. " " .. meta[2] .. "\n# TYPE " .. name .. " " .. meta[1] .. "\n"
        local f = fams[name]
        local label_sets = {}
        for labels in pairs(f) do
            label_sets[#label_sets + 1] = labels
        end
        table_sort(label_sets)
        for _, labels in ipairs(label_sets) do
            local s = f[labels]
            if meta[1] == "histogram" then
                local list, cum = buckets_of[name], 0
                for i = 1, #list do
                    cum = cum + (s.buckets[i] or 0)
                    series(name, "_bucket", labels, 'le="' .. num(list[i]) .. '"', num(cum), out)
                end
                series(name, "_bucket", labels, 'le="+Inf"', num(s.count or 0), out)
                series(name, "_sum", labels, nil, num(s.sum or 0), out)
                series(name, "_count", labels, nil, num(s.count or 0), out)
            else
                series(name, "", labels, nil, num(s.value or 0), out)
            end
        end
    end
    return table_concat(out)
end

return _M
