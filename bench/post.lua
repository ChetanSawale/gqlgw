-- wrk script: POST the query given in the QUERY environment variable
wrk.method = "POST"
wrk.headers["Content-Type"] = "application/json"
wrk.body = '{"query": "' .. os.getenv("QUERY"):gsub('"', '\\"') .. '"}'
