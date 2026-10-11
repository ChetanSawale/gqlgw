-- Benchmark backend: every request costs BACKEND_LATENCY seconds.
local cjson = require "cjson"
local _M = {}
local LATENCY = tonumber(os.getenv("BACKEND_LATENCY") or "0.002")
local posts = {}
for i = 1, 100 do
    posts[i] = { id = tostring(i), title = "Post " .. i, author_id = tostring(i) }
end
local function user(id) return { id = id, name = "User " .. id } end

function _M.handle()
    ngx.sleep(LATENCY)
    local uri = ngx.var.uri
    ngx.header["Content-Type"] = "application/json"
    local id = uri:match("^/users/(%d+)$")
    if id then
        return ngx.print(cjson.encode(user(id)))
    end
    if uri == "/users" then
        local out = {}
        for k in (ngx.var.arg_ids or ""):gmatch("[^,]+") do out[#out + 1] = user(k) end
        return ngx.print(cjson.encode(out))
    end
    if uri == "/posts" then
        local n = tonumber(ngx.var.arg_limit) or 100
        local out = {}
        for i = 1, math.min(n, #posts) do out[i] = posts[i] end
        return ngx.print(cjson.encode(out))
    end
    ngx.status = 404
    ngx.print('{"error":"not found"}')
end
return _M
