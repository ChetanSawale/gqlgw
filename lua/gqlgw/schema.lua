-- gqlgw.schema: loads a compiled schema (.gqlb + .routes.lua) and derives the
-- tables used by the executor. Called from init_by_lua: the result is shared
-- by all workers (copy on write).
local ffi = require "ffi"
local gffi = require "gqlgw.ffi"

local NONE = gffi.NONE
local ffi_string = ffi.string

local _M = {}

local function sref(view, r)
    return ffi_string(view.str + r.off, r.len)
end

-- type reference -> array of codes: "!" (non null), "[" (list), then the named type index
local function chain_of(view, tref)
    local chain = {}
    while true do
        local t = view.trefs[tref]
        if t.kind == gffi.TR_NONNULL then
            chain[#chain + 1] = "!"
            tref = t.of
        elseif t.kind == gffi.TR_LIST then
            chain[#chain + 1] = "["
            tref = t.of
        else
            chain[#chain + 1] = tonumber(t.of)
            return chain
        end
    end
end

local function load_routes(path)
    local chunk, err = loadfile(path)
    if not chunk then
        error("gqlgw: cannot load routes " .. path .. ": " .. tostring(err))
    end
    setfenv(chunk, {})
    return chunk()
end

-- returns a schema object:
--   S.handle          cdata gql_schema* (kept alive)
--   S.hash            hex string
--   S.types[i]        { name, kind, scalar, enum = {NAME=true}, discriminator }
--   S.fields[i]       { name, coord, owner, chain, rawkey, route }   (i = schema field index)
--   S.query_type, S.mutation_type, S.f_typename
function _M.load(gqlb_path, routes_path)
    local C = gffi.C
    local errbuf = ffi.new("char[512]")
    local handle = C.gql_schema_open(gqlb_path, errbuf, 512)
    if handle == nil then
        error("gqlgw: cannot open schema: " .. ffi_string(errbuf))
    end
    handle = ffi.gc(handle, C.gql_schema_close)

    local hex = ffi.new("char[17]")
    C.gql_schema_hash_hex(handle, hex)
    local hash = ffi_string(hex)

    routes_path = routes_path or gqlb_path:gsub("%.gqlb$", "") .. ".routes.lua"
    local routes = load_routes(routes_path)
    if routes.hash ~= hash then
        error(("gqlgw: %s (hash %s) does not match %s (hash %s): recompile with gqlc")
              :format(routes_path, tostring(routes.hash), gqlb_path, hash))
    end

    local view = ffi.new("gql_schema_view")
    C.gql_schema_get_view(handle, view)

    local S = {
        handle = handle,
        hash = hash,
        types = {},
        fields = {},
        query_type = tonumber(view.query_type),
        mutation_type = view.mutation_type ~= NONE and tonumber(view.mutation_type) or nil,
        f_typename = tonumber(view.f_typename),
        f_schema = tonumber(view.f_schema),
        f_type = tonumber(view.f_type),
        routes = routes,
    }

    for i = 0, view.ntypes - 1 do
        local t = view.types[i]
        local name = sref(view, t.name)
        local info = { name = name, kind = tonumber(t.kind), scalar = tonumber(t.scalar) }
        if t.kind == gffi.GK_ENUM then
            local enum = {}
            for k = 0, t.evals_count - 1 do
                enum[sref(view, view.evals[t.evals_first + k].name)] = true
            end
            info.enum = enum
        end
        if t.kind == gffi.GK_UNION or t.kind == gffi.GK_INTERFACE then
            info.discriminator = routes.discriminators and routes.discriminators[name] or "__typename"
        end
        S.types[i] = info
    end

    local from = routes.from or {}
    local nroutes = 0
    for i = 0, view.nfields - 1 do
        local f = view.fields[i]
        local name = sref(view, f.name)
        local info = { name = name, chain = chain_of(view, f.type) }
        if f.owner ~= NONE then
            local owner = tonumber(f.owner)
            info.owner = owner
            info.coord = S.types[owner].name .. "." .. name
            info.rawkey = from[info.coord] or name
            local r = routes.fields[info.coord]
            if r then
                info.route = r
                nroutes = nroutes + 1
            end
        else
            info.coord = "<meta>." .. name
            info.rawkey = name
        end
        S.fields[i] = info
    end
    S.nroutes = nroutes
    return S
end

return _M
