-- gqlgw.http: GraphQL-over-HTTP request parsing and response writing.
local cjson = require "cjson.safe"

local ngx = ngx
local type = type
local str_find, str_lower = string.find, string.lower

local _M = {}

local MT_GQL = "application/graphql-response+json"
local MT_JSON = "application/json"

local decoder = cjson.new()
decoder.decode_array_with_array_mt(true)
local encoder = cjson.new()
encoder.encode_escape_forward_slash(false)
encoder.encode_number_precision(16)

-- response media type from the Accept header (nil = not acceptable)
local function negotiate()
    local accept = ngx.var.http_accept
    if not accept or accept == "" then
        return MT_GQL
    end
    accept = str_lower(accept)
    if str_find(accept, MT_GQL, 1, true) then
        return MT_GQL
    end
    if str_find(accept, MT_JSON, 1, true) or str_find(accept, "*/*", 1, true) or
       str_find(accept, "application/*", 1, true) then
        return MT_JSON
    end
    return nil
end

local function errors_body(msg)
    return '{"errors":[{"message":' .. encoder.encode(msg) .. '}]}'
end

function _M.send(status, media, body, extra_headers)
    ngx.status = status
    ngx.header["Content-Type"] = (media or MT_JSON) .. "; charset=utf-8"
    if extra_headers then
        for k, v in pairs(extra_headers) do
            ngx.header[k] = v
        end
    end
    ngx.print(body)
    return ngx.exit(status)
end

function _M.fail(status, media, msg, extra_headers)
    return _M.send(status, media, errors_body(msg), extra_headers)
end

local function read_body(max)
    ngx.req.read_body()
    local body = ngx.req.get_body_data()
    if body then
        return body
    end
    local file = ngx.req.get_body_file()
    if not file then
        return ""
    end
    local f = io.open(file, "rb")
    if not f then
        return nil
    end
    body = f:read(max + 1)
    f:close()
    return body
end

-- returns req = { query, operation_name, variables (JSON text or nil), method, media }
-- or calls ngx.exit with a 4xx response
function _M.read_request(conf)
    local media = negotiate()
    if not media then
        return _M.fail(406, MT_JSON, "Unsupported Accept header: expected " .. MT_GQL .. " or " .. MT_JSON .. ".")
    end
    local method = ngx.req.get_method()
    local req = { method = method, media = media }
    local max = conf.max_body_bytes or 1048576

    if method == "GET" then
        local args = ngx.req.get_uri_args(32)
        local q = args.query
        if type(q) ~= "string" or q == "" then
            return _M.fail(400, media, "Missing query parameter \"query\".")
        end
        req.query = q
        local op = args.operationName
        if type(op) == "string" and op ~= "" then
            req.operation_name = op
        end
        local vars = args.variables
        if type(vars) == "string" and vars ~= "" then
            req.variables = vars
        end
        return req
    end
    if method ~= "POST" then
        return _M.fail(405, media, "Method " .. method .. " is not allowed: use GET or POST.", { Allow = "GET, POST" })
    end
    local ctype = ngx.var.content_type
    if not ctype or not str_find(str_lower(ctype), "application/json", 1, true) then
        return _M.fail(415, media, "Unsupported Content-Type: expected application/json.")
    end
    local body = read_body(max)
    if not body then
        return _M.fail(400, media, "Cannot read the request body.")
    end
    if #body > max then
        return _M.fail(413, media, "Request body exceeds " .. max .. " bytes.")
    end
    local params = decoder.decode(body)
    if type(params) ~= "table" or getmetatable(params) == cjson.array_mt then
        return _M.fail(400, media, "The request body must be a JSON object.")
    end
    local q = params.query
    if type(q) ~= "string" then
        return _M.fail(400, media, "The \"query\" parameter must be a string.")
    end
    req.query = q
    local op = params.operationName
    if op ~= nil and op ~= cjson.null then
        if type(op) ~= "string" then
            return _M.fail(400, media, "The \"operationName\" parameter must be a string.")
        end
        req.operation_name = op
    end
    local vars = params.variables
    if vars ~= nil and vars ~= cjson.null then
        if type(vars) ~= "table" or (getmetatable(vars) == cjson.array_mt and next(vars) ~= nil) then
            return _M.fail(400, media, "The \"variables\" parameter must be an object.")
        end
        req.variables = encoder.encode(vars)
        if req.variables == "[]" then
            req.variables = "{}"
        end
    end
    return req
end

_M.MT_GQL = MT_GQL
_M.MT_JSON = MT_JSON

return _M
