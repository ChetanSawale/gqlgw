# GraphQL-over-HTTP: media types, status codes, GET, request errors.
use lib '.';
use t::Gqlgw;

plan 'no_plan';
no_long_string();
run_tests();

__DATA__

=== TEST 1: validation errors are 400 with application/graphql-response+json
--- request
POST /graphql
{"query": "{ user(id: 1) { nope } }"}
--- more_headers
Content-Type: application/json
Accept: application/graphql-response+json
--- error_code: 400
--- response_headers
Content-Type: application/graphql-response+json; charset=utf-8
--- response_body chomp
{"errors":[{"message":"Cannot query field \"nope\" on type \"User\".","locations":[{"line":1,"column":17}]}]}



=== TEST 2: ... and 200 with application/json
--- request
POST /graphql
{"query": "{ user(id: 1) { nope } }"}
--- more_headers
Content-Type: application/json
Accept: application/json
--- error_code: 200
--- response_headers
Content-Type: application/json; charset=utf-8



=== TEST 3: syntax error
--- request
POST /graphql
{"query": "{ user(id: 1) { name "}
--- more_headers
Content-Type: application/json
--- error_code: 400
--- response_body chomp
{"errors":[{"message":"Syntax Error: Expected Name, found <EOF>.","locations":[{"line":1,"column":22}]}]}



=== TEST 4: GET query with variables and operationName
--- request
GET /graphql?query=query%20A%20%7B%20hello%20%7D%20query%20B(%24id%3A%20ID!)%20%7B%20user(id%3A%20%24id)%20%7B%20name%20%7D%20%7D&operationName=B&variables=%7B%22id%22%3A%227%22%7D
--- response_body chomp
{"data":{"user":{"name":"User 7"}}}



=== TEST 5: mutations are not allowed over GET
--- request
GET /graphql?query=mutation%20%7B%20renameUser(id%3A%201%2C%20name%3A%20%22x%22)%20%7B%20id%20%7D%20%7D
--- error_code: 405
--- response_headers
Allow: POST



=== TEST 6: malformed body
--- request
POST /graphql
{"query": 1
--- more_headers
Content-Type: application/json
--- error_code: 400
--- response_body chomp
{"errors":[{"message":"The request body must be a JSON object."}]}



=== TEST 7: unsupported content type
--- request
POST /graphql
query { hello }
--- more_headers
Content-Type: application/graphql
--- error_code: 415



=== TEST 8: invalid variables are a request error
--- request
POST /graphql
{"query": "query($id: ID!) { user(id: $id) { name } }", "variables": {"id": {"a": 1}}}
--- more_headers
Content-Type: application/json
--- error_code: 400
--- response_body chomp
{"errors":[{"message":"Variable \"$id\" has invalid value: ID cannot represent value: { a: 1 }","locations":[{"line":1,"column":7}]}]}



=== TEST 9: introspection
--- request
POST /graphql
{"query": "{ __schema { queryType { name } mutationType { name } } __type(name: \"Post\") { fields { name } } }"}
--- more_headers
Content-Type: application/json
--- response_body chomp
{"data":{"__schema":{"queryType":{"name":"Query"},"mutationType":{"name":"Mutation"}},"__type":{"fields":[{"name":"id"},{"name":"title"},{"name":"body"},{"name":"tags"},{"name":"authorId"},{"name":"author"},{"name":"strictAuthor"},{"name":"unbatchedAuthor"},{"name":"echoParent"}]}}}



=== TEST 10: gateway directives are not visible
--- request
POST /graphql
{"query": "{ __schema { directives { name } } }"}
--- more_headers
Content-Type: application/json
--- response_body chomp
{"data":{"__schema":{"directives":[{"name":"include"},{"name":"skip"},{"name":"deprecated"},{"name":"specifiedBy"},{"name":"oneOf"}]}}}



=== TEST 11: introspection can be disabled
--- gqlgw_extra
introspection = false,
--- request
POST /graphql
{"query": "{ __schema { queryType { name } } }"}
--- more_headers
Content-Type: application/json
--- error_code: 400
--- response_body chomp
{"errors":[{"message":"GraphQL introspection has been disabled, but the requested query contained the field \"__schema\".","locations":[{"line":1,"column":3}]}]}



=== TEST 12: query size limit
--- gqlgw_extra
max_query_bytes = 20,
--- request
POST /graphql
{"query": "{ user(id: 1) { name } }"}
--- more_headers
Content-Type: application/json
--- error_code: 400
--- response_body chomp
{"errors":[{"message":"Query exceeds the maximum size of 20 bytes."}]}



=== TEST 13: unsupported Accept
--- request
POST /graphql
{"query": "{ hello }"}
--- more_headers
Content-Type: application/json
Accept: text/html
--- error_code: 406



=== TEST 14: documents are cached per worker
--- request eval
["POST /graphql\n{\"query\": \"{ hello }\"}", "POST /graphql\n{\"query\": \"{ hello }\"}"]
--- more_headers
Content-Type: application/json
--- response_headers eval
["X-GQL-Doc-Cache: miss", "X-GQL-Doc-Cache: hit"]
--- response_body eval
["{\"data\":{\"hello\":null}}", "{\"data\":{\"hello\":null}}"]
