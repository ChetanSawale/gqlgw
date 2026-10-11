# Mutations, request bodies and what the backends receive.
use lib '.';
use t::Gqlgw;

plan 'no_plan';
no_long_string();
run_tests();

__DATA__

=== TEST 1: PATCH with a JSON body template
--- request
POST /graphql
{"query": "mutation { renameUser(id: \"4\", name: \"Neo \\\"the one\\\"\") { id name } }"}
--- more_headers
Content-Type: application/json
--- response_body chomp
{"data":{"renameUser":{"id":"4","name":"Neo \"the one\""}}}



=== TEST 2: input object body with defaults applied
--- request
POST /graphql
{"query": "mutation { createPost(input: {title: \"Hi\"}) { id title tags author { name } } }"}
--- more_headers
Content-Type: application/json
--- response_body chomp
{"data":{"createPost":{"id":"61","title":"Hi","tags":[],"author":{"name":"User 1"}}}}



=== TEST 3: root mutation fields run serially, in order
--- request
POST /graphql
{"query": "mutation { a: renameUser(id: \"1\", name: \"A\") { name } b: renameUser(id: \"2\", name: \"B\") { name } }"}
--- more_headers
Content-Type: application/json
--- response_body chomp
{"data":{"a":{"name":"A"},"b":{"name":"B"}}}
--- response_headers
X-GQL-Rounds: 2



=== TEST 4: what a GET subrequest sends upstream from a POST request
--- request
POST /graphql
{"query": "{ echo(path: \"x y\") { method uri body accept_encoding authorization content_length } }"}
--- more_headers
Content-Type: application/json
Accept-Encoding: gzip
Authorization: Bearer secret
--- response_body chomp
{"data":{"echo":{"method":"GET","uri":"/__mock/echo/x%20y","body":null,"accept_encoding":null,"authorization":"Bearer secret","content_length":null}}}



=== TEST 5: what a POST subrequest sends upstream ({args} body)
--- request
POST /graphql
{"query": "mutation { echoPost(path: \"p\", input: {title: \"t\"}) { method body content_length } }"}
--- more_headers
Content-Type: application/json
--- response_body chomp
{"data":{"echoPost":{"method":"POST","body":"{\"path\":\"p\",\"input\":{\"title\":\"t\",\"tags\":[]}}","content_length":"44"}}}



=== TEST 6: arguments cannot alter the upstream path (strict mode, default)
--- request
POST /graphql
{"query": "{ a: echo(path: \"../admin\") { uri } b: user(id: \"1/../../echo\") { id } c: echo(path: \"a b?c\") { uri } }"}
--- more_headers
Content-Type: application/json
--- response_body chomp
{"errors":[{"message":"Invalid value for an upstream path segment (contains \"/\" or is \".\" / \"..\").","locations":[{"line":1,"column":3}],"path":["a"]},{"message":"Invalid value for an upstream path segment (contains \"/\" or is \".\" / \"..\").","locations":[{"line":1,"column":37}],"path":["b"]}],"data":{"a":null,"b":null,"c":{"uri":"/__mock/echo/a%20b%3Fc"}}}



=== TEST 7: without strict mode, "/" stays encoded (%2F) in the upstream path
--- gqlgw_extra
strict_path_args = false,
--- request
POST /graphql
{"query": "{ a: echo(path: \"x/y\") { uri } }"}
--- more_headers
Content-Type: application/json
--- response_body chomp
{"data":{"a":{"uri":"/__mock/echo/x%2Fy"}}}
