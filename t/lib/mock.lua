-- Mock REST backends for the Test::Nginx suite. Every request is counted in
-- the "mock" shared dict: GET /__mock/hits returns the counters, DELETE resets.
local cjson = require "cjson.safe"
cjson.encode_empty_table_as_object(false)

local _M = {}

local users, posts = {}, {}
for i = 1, 100 do
    users[tostring(i)] = { id = tostring(i), name = "User " .. i, created_at = "2020-01-" .. string.format("%02d", (i % 28) + 1),
                           email = "u" .. i .. "@example.com" }
end
for i = 1, 60 do
    posts[i] = { id = tostring(i), title = "Post " .. i, body = (i % 5 == 0) and cjson.null or ("Body " .. i),
                 tags = (i % 2 == 0) and { "even" } or setmetatable({}, cjson.array_mt), author_id = tostring((i % 7) + 1) }
end
-- a post whose author does not exist (404 / missing in batch)
posts[60].author_id = "999"

local function count(key)
    local d = ngx.shared.mock
    d:incr("hits", 1, 0)
    d:incr("hits:" .. key, 1, 0)
end

local function send(status, v)
    ngx.status = status
    ngx.header["Content-Type"] = "application/json"
    if v ~= nil then
        ngx.print(type(v) == "string" and v or cjson.encode(v))
    end
    return ngx.exit(status)
end

function _M.handle()
    local uri = ngx.var.uri:gsub("^/__mock", "")
    local method = ngx.req.get_method()
    local args = ngx.req.get_uri_args()

    if uri == "/hits" then
        local d = ngx.shared.mock
        if method == "DELETE" then
            d:flush_all()
            return send(200, { ok = true })
        end
        local out = {}
        for _, k in ipairs(d:get_keys(0)) do
            out[k] = d:get(k)
        end
        return send(200, out)
    end

    -- debugging aid: echoes what the gateway sent upstream
    if uri:find("^/echo") then
        count("echo")
        ngx.req.read_body()
        return send(200, { method = method, uri = ngx.var.request_uri, body = ngx.req.get_body_data() or cjson.null,
                           accept_encoding = ngx.var.http_accept_encoding or cjson.null,
                           authorization = ngx.var.http_authorization or cjson.null,
                           content_length = ngx.var.http_content_length or cjson.null })
    end

    local id = uri:match("^/users/(%w+)$")
    if id then
        count("users/" .. id)
        if method == "PATCH" then
            ngx.req.read_body()
            local body = cjson.decode(ngx.req.get_body_data() or "")
            local u = users[id]
            if not u then
                return send(404, { error = "not found" })
            end
            local copy = {}
            for k, v in pairs(u) do copy[k] = v end
            if body and body.name then copy.name = body.name end
            return send(200, copy)
        end
        if id == "500" then
            return send(500, { error = "boom" })
        end
        if id == "slow" then
            ngx.sleep(0.5)
            return send(200, users["1"])
        end
        if id == "bad" then
            return send(200, "{not json")
        end
        if not users[id] then
            return send(404, { error = "not found" })
        end
        return send(200, users[id])
    end
    if uri == "/users" then
        count("users?ids")
        local out = setmetatable({}, cjson.array_mt)
        for k in tostring(args.ids or ""):gmatch("[^,]+") do
            if users[k] then out[#out + 1] = users[k] end
        end
        return send(200, out)
    end
    if uri == "/posts" then
        if method == "POST" then
            count("posts:create")
            ngx.req.read_body()
            local body = cjson.decode(ngx.req.get_body_data() or "") or {}
            body.id = "61"
            body.author_id = "1"
            return send(201, body)
        end
        count("posts")
        local limit = tonumber(args.limit) or 20
        local out = setmetatable({}, cjson.array_mt)
        for _, p in ipairs(posts) do
            if #out >= limit then break end
            local ok = true
            if args.tag and args.tag ~= "" then
                ok = false
                for _, t in ipairs(p.tags) do if t == args.tag then ok = true end end
            end
            if args.author and args.author ~= "" and p.author_id ~= args.author then ok = false end
            if ok then out[#out + 1] = p end
        end
        return send(200, out)
    end
    if uri == "/search" then
        count("search")
        return send(200, { results = {
            { kind = "User", id = "1", name = "User 1" },
            { kind = "Post", id = "2", title = "Post 2", author_id = "3", tags = setmetatable({}, cjson.array_mt) },
            { kind = "Robot", id = "x" },
        } })
    end
    return send(404, { error = "no route " .. uri })
end

return _M
