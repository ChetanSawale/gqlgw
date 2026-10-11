-- gqlgw.exec: executes a plan produced by libgql.
--
-- Phase 1 (resolution): walk the plan over the raw JSON data. Fields carrying
-- a @fetch route are queued; when nothing can progress locally, one "round"
-- dispatches every queued fetch at once (gqlgw.fetch: cache + batching +
-- parallel subrequests) and resolution resumes on the results. The number of
-- rounds is the depth of the fetch chain, not the size of the lists.
--
-- Phase 2 (completion): walk the resolved tree in plan order, coerce leaf
-- values, propagate nulls and write the JSON response with ordered keys.
local ffi = require "ffi"
local cjson = require "cjson.safe"
local gffi = require "gqlgw.ffi"

local ffi_string = ffi.string
local type, setmetatable, getmetatable, tonumber, tostring = type, setmetatable, getmetatable, tonumber, tostring
local string_format, math_floor, math_huge = string.format, math.floor, math.huge
local table_concat = table.concat
local null = cjson.null
local array_mt = cjson.array_mt

local PF_LEAF, PF_OBJECT, PF_ABSTRACT, PF_RAW = gffi.PF_LEAF, gffi.PF_OBJECT, gffi.PF_ABSTRACT, gffi.PF_RAW
local GK_ENUM = gffi.GK_ENUM
local SK_INT, SK_FLOAT, SK_STRING, SK_BOOLEAN, SK_ID = gffi.SK_INT, gffi.SK_FLOAT, gffi.SK_STRING,
    gffi.SK_BOOLEAN, gffi.SK_ID

local json = cjson.new()
json.encode_escape_forward_slash(false)
json.encode_empty_table_as_object(true)
local args_decoder = cjson.new()
args_decoder.decode_array_with_array_mt(true)

local _M = {}

-- markers
local ERR_MT = { __name = "gqlgw.error" }
local NODE_MT = { __name = "gqlgw.node" }
local LIST_MT = { __name = "gqlgw.list" }
local TYPENAME = setmetatable({}, { __name = "gqlgw.typename" })
local PENDING = setmetatable({}, { __name = "gqlgw.pending" })
_M.PENDING = PENDING

local function field_error(msg)
    return setmetatable({ msg = msg }, ERR_MT)
end
_M.field_error = field_error

local function is_error(v)
    return type(v) == "table" and getmetatable(v) == ERR_MT
end
_M.is_error = is_error

local function quote(s)
    return json.encode(s)
end

local function inspect(v)
    if type(v) == "string" then
        return quote(v)
    end
    if type(v) == "table" then
        local s = json.encode(v)
        return s or "[object]"
    end
    if v == null then
        return "null"
    end
    return tostring(v)
end

-- JSON number text (shortest round-trip representation)
local function num_json(n)
    if n ~= n or n == math_huge or n == -math_huge then
        return nil
    end
    if n == math_floor(n) and n > -1e15 and n < 1e15 then
        return string_format("%d", n)
    end
    for p = 15, 17 do
        local s = string_format("%." .. p .. "g", n)
        if tonumber(s) == n then
            return s
        end
    end
    return string_format("%.17g", n)
end
_M.num_json = num_json

local function to_number(v)
    if type(v) == "number" then
        return v
    end
    if type(v) == "boolean" then
        return v and 1 or 0
    end
    if type(v) == "string" and v ~= "" then
        return tonumber(v)
    end
    return nil
end

-- serializes a leaf value; returns JSON text or nil, message
local function serialize_leaf(tinfo, v)
    if tinfo.kind == GK_ENUM then
        if type(v) == "string" and tinfo.enum[v] then
            return quote(v)
        end
        return nil, 'Enum "' .. tinfo.name .. '" cannot represent value: ' .. inspect(v)
    end
    local sk = tinfo.scalar
    if sk == SK_STRING then
        local tv = type(v)
        if tv == "string" then
            return quote(v)
        elseif tv == "number" then
            local s = num_json(v)
            if s then
                return quote(s)
            end
        elseif tv == "boolean" then
            return v and '"true"' or '"false"'
        end
        return nil, "String cannot represent value: " .. inspect(v)
    elseif sk == SK_INT then
        local n = to_number(v)
        if not n or n ~= math_floor(n) then
            return nil, "Int cannot represent non-integer value: " .. inspect(v)
        end
        if n > 2147483647 or n < -2147483648 then
            return nil, "Int cannot represent non 32-bit signed integer value: " .. inspect(v)
        end
        return string_format("%d", n)
    elseif sk == SK_FLOAT then
        local n = to_number(v)
        local s = n and num_json(n)
        if not s then
            return nil, "Float cannot represent non numeric value: " .. inspect(v)
        end
        return s
    elseif sk == SK_BOOLEAN then
        if type(v) == "boolean" then
            return v and "true" or "false"
        end
        if type(v) == "number" and v == v then
            return v ~= 0 and "true" or "false"
        end
        return nil, "Boolean cannot represent a non boolean value: " .. inspect(v)
    elseif sk == SK_ID then
        if type(v) == "string" then
            return quote(v)
        end
        if type(v) == "number" and v == math_floor(v) and v > -9007199254740992 and v < 9007199254740992 then
            return quote(string_format("%d", v))
        end
        return nil, "ID cannot represent value: " .. inspect(v)
    end
    -- custom scalar: pass the JSON value through
    local s = json.encode(v)
    if not s then
        return nil, "Cannot serialize value of custom scalar \"" .. tinfo.name .. "\""
    end
    return s
end
_M.serialize_leaf = serialize_leaf

-- ---------------------------------------------------------------- context

local Ctx = {}
Ctx.__index = Ctx

-- S: schema (gqlgw.schema), view: gql_plan_view*, fetcher: function(ctx, items)
function _M.new(S, view, fetcher, opts)
    return setmetatable({
        S = S,
        v = view,
        fetcher = fetcher,
        opts = opts or {},
        queue = {},
        errors = {},
        keys = {},        -- plan field idx -> quoted response key
        args = {},        -- plan field idx -> decoded arguments
        cases = {},       -- plan field idx -> { [type name] = { sel, type } }
        rounds = 0,
        stats = { rounds = 0, subrequests = 0, batches = 0, hits_l1 = 0, hits_l2 = 0, hits_l3 = 0, misses = 0 },
    }, Ctx)
end

function Ctx:key(i)
    local k = self.keys[i]
    if not k then
        local f = self.v.fields[i]
        k = quote(ffi_string(self.v.str + f.key_off, f.key_len))
        self.keys[i] = k
    end
    return k
end

function Ctx:plain_key(i)
    local f = self.v.fields[i]
    return ffi_string(self.v.str + f.key_off, f.key_len)
end

-- decoded coerced arguments of a plan field ({} when none)
function Ctx:field_args(i)
    local a = self.args[i]
    if not a then
        local f = self.v.fields[i]
        if f.args_len > 0 then
            a = args_decoder.decode(ffi_string(self.v.str + f.args_off, f.args_len)) or {}
        else
            a = {}
        end
        self.args[i] = a
    end
    return a
end

-- coerced arguments as JSON text (key order of the schema)
function Ctx:field_args_json(i)
    local f = self.v.fields[i]
    if f.args_len == 0 then
        return "{}"
    end
    return ffi_string(self.v.str + f.args_off, f.args_len)
end

function Ctx:case_for(i, tname)
    local m = self.cases[i]
    if not m then
        m = {}
        local f = self.v.fields[i]
        for k = 0, f.child_count - 1 do
            local c = self.v.cases[f.child_first + k]
            m[self.S.types[tonumber(c.type)].name] = c
        end
        self.cases[i] = m
    end
    return m[tname]
end

-- ------------------------------------------------------------- resolution

local resolve_node

-- turns a raw JSON value into the resolved representation for plan field i
local function materialize(ctx, raw, chain, ci, i, finfo)
    if raw == nil or raw == null then
        return null
    end
    local code = chain[ci]
    if code == "!" then
        return materialize(ctx, raw, chain, ci + 1, i, finfo)
    end
    if code == "[" then
        if type(raw) ~= "table" or (getmetatable(raw) ~= array_mt and next(raw) ~= nil) then
            return field_error('Expected Iterable, but did not find one for field "' .. finfo.coord .. '".')
        end
        local n = #raw
        local out = setmetatable({ n = n }, LIST_MT)
        for j = 1, n do
            out[j] = materialize(ctx, raw[j], chain, ci + 1, i, finfo)
        end
        return out
    end
    local f = ctx.v.fields[i]
    local kind = f.kind
    if kind == PF_LEAF then
        return raw
    end
    if type(raw) ~= "table" or getmetatable(raw) == array_mt then
        return field_error('Expected value of type "' .. ctx.S.types[code].name .. '" for field "' ..
                           finfo.coord .. '" to be an object.')
    end
    if kind == PF_OBJECT then
        return resolve_node(ctx, tonumber(f.child_first), raw, code)
    end
    -- abstract: runtime type from the discriminator key
    local tinfo = ctx.S.types[code]
    local tname = raw[tinfo.discriminator]
    local c = type(tname) == "string" and ctx:case_for(i, tname)
    if not c then
        return field_error('Abstract type "' .. tinfo.name .. '" must resolve to an Object type at runtime for field "' ..
                           finfo.coord .. '": the value has no valid "' .. tinfo.discriminator .. '" (got ' ..
                           inspect(tname) .. ').')
    end
    return resolve_node(ctx, tonumber(c.sel), raw, tonumber(c.type))
end
_M.materialize = materialize

resolve_node = function(ctx, sel_idx, src, tidx)
    local v, S = ctx.v, ctx.S
    local sel = v.sels[sel_idx]
    local first, count = tonumber(sel.fields_first), tonumber(sel.fields_count)
    local vals = {}
    local node = setmetatable({ sel = sel_idx, src = src, t = tidx, vals = vals }, NODE_MT)
    local f_typename = S.f_typename
    for k = 0, count - 1 do
        local i = first + k
        local f = v.fields[i]
        local fi = tonumber(f.field)
        if fi == f_typename then
            vals[k + 1] = TYPENAME
        elseif f.kind == PF_RAW then
            vals[k + 1] = { raw = ffi_string(v.str + f.args_off, f.args_len) }
        else
            local finfo = S.fields[fi]
            if finfo.route then
                vals[k + 1] = PENDING
                local q = ctx.queue
                q[#q + 1] = { node = node, k = k + 1, i = i, finfo = finfo }
            else
                vals[k + 1] = materialize(ctx, src[finfo.rawkey], finfo.chain, 1, i, finfo)
            end
        end
    end
    return node
end

-- called by the fetcher for every queued item
function Ctx:fill(item, value)
    if is_error(value) then
        item.node.vals[item.k] = value
    else
        item.node.vals[item.k] = materialize(self, value, item.finfo.chain, 1, item.i, item.finfo)
    end
end

function Ctx:run_rounds()
    local max_rounds = self.opts.max_rounds or 16
    while #self.queue > 0 do
        if self.rounds >= max_rounds then
            for _, item in ipairs(self.queue) do
                item.node.vals[item.k] = field_error("Too many nested fetches (more than " .. max_rounds .. " rounds).")
            end
            self.queue = {}
            return
        end
        self.rounds = self.rounds + 1
        self.stats.rounds = self.rounds
        local items = self.queue
        self.queue = {}
        self.fetcher(self, items)
    end
end

-- ------------------------------------------------------------- completion

local function truncate(out, n)
    for j = #out, n + 1, -1 do
        out[j] = nil
    end
end

-- errors are serialized right away with the conventional key order
local function add_error(ctx, i, path, msg)
    local f = ctx.v.fields[i]
    local p = {}
    for j = 1, #path do
        local e = path[j]
        p[j] = type(e) == "number" and string_format("%d", e) or quote(e)
    end
    ctx.errors[#ctx.errors + 1] = '{"message":' .. quote(msg) .. ',"locations":[{"line":' .. tonumber(f.line) ..
        ',"column":' .. tonumber(f.col) .. '}],"path":[' .. table_concat(p, ",") .. ']}'
end

local write_object

-- returns false when a non-null violation must propagate to the parent
local function write_value(ctx, out, val, chain, ci, i, finfo, path)
    local code = chain[ci]
    if code == "!" then
        local mark = #out
        if val == nil or val == null or val == PENDING then
            add_error(ctx, i, path, 'Cannot return null for non-nullable field ' .. finfo.coord .. '.')
            return false
        end
        if is_error(val) then
            add_error(ctx, i, path, val.msg)
            return false
        end
        if not write_value(ctx, out, val, chain, ci + 1, i, finfo, path) then
            truncate(out, mark)
            return false
        end
        return true
    end
    if val == nil or val == null or val == PENDING then
        out[#out + 1] = "null"
        return true
    end
    if is_error(val) then
        add_error(ctx, i, path, val.msg)
        out[#out + 1] = "null"
        return true
    end
    if code == "[" then
        local mark = #out
        out[#out + 1] = "["
        local np = #path + 1
        for j = 1, val.n do
            if j > 1 then
                out[#out + 1] = ","
            end
            path[np] = j - 1
            if not write_value(ctx, out, val[j], chain, ci + 1, i, finfo, path) then
                -- a non-null item failed: the whole list is null
                path[np] = nil
                truncate(out, mark)
                if chain[ci - 1] == "!" then
                    return false
                end
                out[#out + 1] = "null"
                return true
            end
        end
        path[np] = nil
        out[#out + 1] = "]"
        return true
    end
    local kind = ctx.v.fields[i].kind
    if kind == PF_LEAF then
        local s, err = serialize_leaf(ctx.S.types[code], val)
        if not s then
            add_error(ctx, i, path, err)
            if chain[ci - 1] == "!" then
                return false
            end
            out[#out + 1] = "null"
            return true
        end
        out[#out + 1] = s
        return true
    end
    -- object node
    local mark = #out
    if not write_object(ctx, out, val, path) then
        truncate(out, mark)
        if chain[ci - 1] == "!" then
            return false
        end
        out[#out + 1] = "null"
    end
    return true
end

write_object = function(ctx, out, node, path)
    local v, S = ctx.v, ctx.S
    local sel = v.sels[node.sel]
    local first, count = tonumber(sel.fields_first), tonumber(sel.fields_count)
    local vals = node.vals
    local np = #path + 1
    out[#out + 1] = "{"
    for k = 0, count - 1 do
        local i = first + k
        if k > 0 then
            out[#out + 1] = ","
        end
        out[#out + 1] = ctx:key(i)
        out[#out + 1] = ":"
        local val = vals[k + 1]
        if val == TYPENAME then
            out[#out + 1] = quote(S.types[node.t].name)
        elseif type(val) == "table" and val.raw and getmetatable(val) == nil then
            out[#out + 1] = val.raw
        else
            local finfo = S.fields[tonumber(v.fields[i].field)]
            path[np] = ctx:plain_key(i)
            local ok = write_value(ctx, out, val, finfo.chain, 1, i, finfo, path)
            path[np] = nil
            if not ok then
                return false
            end
        end
    end
    out[#out + 1] = "}"
    return true
end

local function errors_json(errors)
    return "[" .. table_concat(errors, ",") .. "]"
end
_M.errors_json = errors_json

-- executes the plan and returns the response body (JSON text)
function Ctx:execute()
    local v, S = self.v, self.S
    local root_sel = tonumber(v.root_sel)
    local root_type = tonumber(v.sels[root_sel].type)
    local root
    if v.op == 1 then
        -- mutation: root fields are executed serially (fetch, then the whole subtree)
        local sel = v.sels[root_sel]
        local first, count = tonumber(sel.fields_first), tonumber(sel.fields_count)
        local vals = {}
        root = setmetatable({ sel = root_sel, src = {}, t = root_type, vals = vals }, NODE_MT)
        for k = 0, count - 1 do
            local i = first + k
            local f = v.fields[i]
            local fi = tonumber(f.field)
            if fi == S.f_typename then
                vals[k + 1] = TYPENAME
            else
                local finfo = S.fields[fi]
                if finfo.route then
                    vals[k + 1] = PENDING
                    self.queue[1] = { node = root, k = k + 1, i = i, finfo = finfo }
                    self:run_rounds()
                else
                    vals[k + 1] = null
                end
            end
        end
    else
        root = resolve_node(self, root_sel, {}, root_type)
        self:run_rounds()
    end
    local out = {}
    local ok = write_object(self, out, root, {})
    local data = ok and table_concat(out) or "null"
    if #self.errors > 0 then
        return '{"errors":' .. errors_json(self.errors) .. ',"data":' .. data .. '}'
    end
    return '{"data":' .. data .. '}'
end

return _M
