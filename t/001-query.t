# Basic execution: fetch, @from, arguments, lists, errors.
use lib '.';
use t::Gqlgw;

plan 'no_plan';
no_long_string();
run_tests();

__DATA__

=== TEST 1: single fetch with @from mapping
--- request
POST /graphql
{"query": "{ user(id: \"3\") { id name createdAt } }"}
--- more_headers
Content-Type: application/json
--- response_body chomp
{"data":{"user":{"id":"3","name":"User 3","createdAt":"2020-01-04"}}}
--- response_headers
X-GQL-Rounds: 1
X-GQL-Subrequests: 1



=== TEST 2: aliases, __typename, missing resource (404) is null
--- request
POST /graphql
{"query": "{ a: user(id: \"1\") { __typename name } b: user(id: \"404\") { name } }"}
--- more_headers
Content-Type: application/json
--- response_body chomp
{"data":{"a":{"__typename":"User","name":"User 1"},"b":null}}



=== TEST 3: N+1 is batched: 20 posts and their authors in 2 subrequests
--- request
POST /graphql
{"query": "{ posts(limit: 20) { id author { id } } }"}
--- more_headers
Content-Type: application/json
--- response_headers
X-GQL-Rounds: 2
X-GQL-Subrequests: 2
X-GQL-Batches: 1
--- response_body_like chomp
^\{"data":\{"posts":\[\{"id":"1","author":\{"id":"2"\}\},\{"id":"2","author":\{"id":"3"\}\},.*\{"id":"20","author":\{"id":"7"\}\}\]\}\}$



=== TEST 4: without @batch the same query costs 1 + distinct authors subrequests
--- request
POST /graphql
{"query": "{ posts(limit: 20) { id unbatchedAuthor { id } } }"}
--- more_headers
Content-Type: application/json
--- response_headers
X-GQL-Rounds: 2
X-GQL-Subrequests: 8
--- response_body_like chomp
"unbatchedAuthor":\{"id":"7"\}



=== TEST 5: variables, defaults and list arguments
--- request
POST /graphql
{"query": "query($ids: [ID!]!) { users(ids: $ids) { id } }", "variables": {"ids": ["5", "6", "nope"]}}
--- more_headers
Content-Type: application/json
--- response_body chomp
{"data":{"users":[{"id":"5"},{"id":"6"}]}}



=== TEST 6: abstract types resolved with @discriminator
--- request
POST /graphql
{"query": "{ search(q: \"x\") { __typename ... on User { name } ... on Post { title author { name } } } }"}
--- more_headers
Content-Type: application/json
--- response_body chomp
{"errors":[{"message":"Abstract type \"SearchResult\" must resolve to an Object type at runtime for field \"Query.search\": the value has no valid \"kind\" (got \"Robot\").","locations":[{"line":1,"column":3}],"path":["search",2]}],"data":null}



=== TEST 7: nested fetches per parent (User.posts)
--- request
POST /graphql
{"query": "{ user(id: \"2\") { posts(limit: 2) { id title } } }"}
--- more_headers
Content-Type: application/json
--- response_body chomp
{"data":{"user":{"posts":[{"id":"1","title":"Post 1"},{"id":"8","title":"Post 8"}]}}}



=== TEST 8: non-null violation propagates to the nearest nullable parent
--- request
POST /graphql
{"query": "{ posts(limit: 60) { id strictAuthor { id } } }"}
--- more_headers
Content-Type: application/json
--- response_body_like chomp
^\{"errors":\[\{"message":"Cannot return null for non-nullable field Post.strictAuthor.","locations":\[\{"line":1,"column":25\}\],"path":\["posts",59,"strictAuthor"\]\}\],"data":null\}$



=== TEST 9: upstream errors become field errors
--- request
POST /graphql
{"query": "{ a: user(id: \"500\") { id } b: user(id: \"bad\") { id } c: user(id: \"1\") { id } }"}
--- more_headers
Content-Type: application/json
--- response_body chomp
{"errors":[{"message":"Upstream \"users\" returned HTTP 500.","locations":[{"line":1,"column":3}],"path":["a"]},{"message":"Upstream \"users\" returned invalid JSON.","locations":[{"line":1,"column":29}],"path":["b"]}],"data":{"a":null,"b":null,"c":{"id":"1"}}}



=== TEST 10: leaf coercion: nullable body, lists of strings
--- request
POST /graphql
{"query": "{ posts(limit: 5, tag: \"even\") { id body tags } }"}
--- more_headers
Content-Type: application/json
--- response_body chomp
{"data":{"posts":[{"id":"2","body":"Body 2","tags":["even"]},{"id":"4","body":"Body 4","tags":["even"]},{"id":"6","body":"Body 6","tags":["even"]},{"id":"8","body":"Body 8","tags":["even"]},{"id":"10","body":null,"tags":["even"]}]}}
