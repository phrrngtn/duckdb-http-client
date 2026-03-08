# duckdb-http-client

A DuckDB extension providing HTTP client functions as composable SQL primitives.
Built on the [DuckDB C Extension API](https://github.com/duckdb/extension-template-c)
for binary compatibility across DuckDB versions.

Inspired by Alex Garcia's excellent [sqlite-http](https://github.com/asg017/sqlite-http)
(`http0`) extension for SQLite, which demonstrated how natural and powerful
HTTP-in-SQL can be when done as explicit table-valued and scalar functions
rather than as a transparent filesystem layer.

## Loading

```sql
LOAD 'path/to/http_client.duckdb_extension';
```

Or, if loading an unsigned extension:

```bash
duckdb -unsigned -cmd "LOAD 'build/release/http_client.duckdb_extension';"
```

## HTTP Functions

### Table functions: `http_get`, `http_post`, `http_put`, `http_delete`, `http_patch`, `http_head`, `http_options`

Each returns a single-row table with request and response details.

```sql
-- Simple GET
SELECT response_status_code, response_body
FROM http_get('https://httpbin.org/get');
```

```sql
-- GET with query parameters
SELECT json_extract_string(response_body, '$.args.q') AS search_term
FROM http_get('https://httpbin.org/get', params := MAP {'q': 'duckdb', 'page': '1'});
```

```sql
-- GET with custom headers
SELECT json_extract_string(response_body, '$.headers.X-Api-Key') AS echoed_key
FROM http_get('https://httpbin.org/get', headers := MAP {'X-Api-Key': 'secret123'});
```

```sql
-- POST with JSON body
SELECT response_status_code,
       json_extract_string(response_body, '$.json.name') AS name
FROM http_post('https://httpbin.org/post', body := '{"name": "duckdb"}');
```

```sql
-- PUT with explicit content type
SELECT response_status_code
FROM http_put('https://httpbin.org/put',
    body := '<item><name>test</name></item>',
    content_type := 'application/xml');
```

```sql
-- Disable SSL verification (for self-signed certs during testing)
SELECT response_status_code
FROM http_get('https://localhost:8443/health', verify_ssl := false);
```

```sql
-- Custom timeout (seconds)
SELECT response_status_code
FROM http_get('https://httpbin.org/delay/2', timeout := 5);
```

### `http_do(method, url, ...)`

Generic method for any HTTP verb.

```sql
SELECT response_status_code
FROM http_do('DELETE', 'https://httpbin.org/delete');

SELECT response_status_code
FROM http_do('PATCH', 'https://httpbin.org/patch', body := '{"patched": true}');
```

### Output columns

All table functions return the same schema:

| Column | Type | Description |
|--------|------|-------------|
| `request_url` | VARCHAR | The URL as sent |
| `request_method` | VARCHAR | HTTP method used |
| `request_headers` | VARCHAR (JSON) | Headers sent, as a JSON object |
| `request_body` | VARCHAR | Request body, if any |
| `response_status_code` | INTEGER | HTTP status code (200, 404, etc.) |
| `response_status` | VARCHAR | Status line (e.g. `HTTP/1.1 200 OK`) |
| `response_headers` | VARCHAR (JSON) | Response headers as a JSON object |
| `response_body` | VARCHAR | Response body |
| `response_url` | VARCHAR | Final URL after redirects |
| `elapsed` | DOUBLE | Request duration in seconds |
| `redirect_count` | INTEGER | Number of redirects followed |

### Named parameters

| Parameter | Type | Applies to | Description |
|-----------|------|------------|-------------|
| `headers` | MAP(VARCHAR, VARCHAR) | all | Custom request headers |
| `params` | MAP(VARCHAR, VARCHAR) | all | URL query parameters (URL-encoded) |
| `body` | VARCHAR | POST, PUT, PATCH, DO | Request body |
| `content_type` | VARCHAR | POST, PUT, PATCH, DO | Content-Type header (defaults to `application/json` if body is set) |
| `timeout` | INTEGER | all | Request timeout in seconds (overrides config) |
| `verify_ssl` | BOOLEAN | all | SSL certificate verification (overrides config) |

### Scalar function: `http_request(method, url, headers_json, body, content_type)`

Returns a JSON string containing the full request/response envelope. This is
the function to use for data-driven workflows where URLs come from a query.

```sql
-- Basic usage
SELECT json_extract(
    http_request('GET', 'https://httpbin.org/get', NULL, NULL, NULL),
    '$.response_status_code'
)::INTEGER AS status;
```

```sql
-- Data-driven: fetch from a list of URLs
SELECT
    url,
    json_extract(http_request('GET', url, NULL, NULL, NULL),
        '$.response_status_code')::INTEGER AS status
FROM (VALUES
    ('https://httpbin.org/get'),
    ('https://httpbin.org/ip')
) AS t(url)
ORDER BY url;
```

```sql
-- With headers as a JSON string
SELECT json_extract_string(
    http_request('GET', 'https://httpbin.org/get',
        '{"Authorization": "Bearer my-token"}', NULL, NULL),
    '$.response_body')
AS body;
```

```sql
-- Batch API calls driven by table data
SELECT
    e.endpoint_url,
    json_extract(
        http_request('GET', e.endpoint_url, NULL, NULL, NULL),
        '$.response_status_code')::INTEGER AS status
FROM endpoints AS e
LEFT OUTER JOIN health_checks AS h ON h.url = e.endpoint_url
WHERE h.url IS NULL;
```

## Configuration

Configuration is managed via a DuckDB variable (`http_config`) containing a
`MAP(VARCHAR, VARCHAR)`. Keys are URL prefixes (scopes); values are JSON
objects with configuration fields. The longest matching prefix wins, with
`'default'` as the fallback.

```sql
SET VARIABLE http_config = MAP {
    'default': '{"timeout": 30, "rate_limit": "20/s"}',
    'https://api.example.com/': '{"auth_type": "bearer", "bearer_token": "sk-abc123", "rate_limit": "5/s"}',
    'https://internal.corp.com/': '{"auth_type": "negotiate", "verify_ssl": false}'
};
```

### Configuration fields

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `timeout` | integer | 30 | Request timeout in seconds |
| `rate_limit` | string | `"20/s"` | Rate limit (`"10/s"`, `"100/m"`, `"3600/h"`) |
| `burst` | number | 5.0 | Burst capacity for rate limiter |
| `verify_ssl` | boolean | true | Verify SSL certificates |
| `proxy` | string | | HTTP/HTTPS proxy URL |
| `ca_bundle` | string | | Path to CA certificate bundle |
| `auth_type` | string | | `"negotiate"` or `"bearer"` |
| `bearer_token` | string | | Token for Bearer authentication |

### How configuration flows

The user-facing functions (`http_get`, `http_post`, etc.) are SQL macros that
read `http_config` from the caller's connection via `getvariable()`, then pass
it to the underlying C functions. This means configuration set via
`SET VARIABLE` is correctly visible during function execution. Per-call
parameters (`timeout :=`, `verify_ssl :=`) override the resolved config.

### Scope resolution example

```sql
SET VARIABLE http_config = MAP {
    'default':                    '{"timeout": 30}',
    'https://api.example.com/':   '{"bearer_token": "abc", "rate_limit": "5/s"}',
    'https://api.example.com/v2/':'{"bearer_token": "xyz"}'
};

-- Uses default config (timeout=30, no auth)
SELECT * FROM http_get('https://other-site.com/data');

-- Matches 'https://api.example.com/' scope (bearer_token=abc, rate_limit=5/s)
SELECT * FROM http_get('https://api.example.com/v1/users');

-- Matches 'https://api.example.com/v2/' scope (bearer_token=xyz)
-- Also inherits timeout=30 from default
SELECT * FROM http_get('https://api.example.com/v2/users');
```

### Rate limiting

Rate limiting uses the GCRA (Generic Cell Rate Algorithm) and is applied
per-host automatically. The default is 20 requests/second with a burst of 5.
Override per-scope via configuration:

```sql
SET VARIABLE http_config = MAP {
    'default': '{"rate_limit": "20/s"}',
    'https://rate-limited-api.com/': '{"rate_limit": "2/s"}'
};
```

## Negotiate (SPNEGO/Kerberos) Authentication

### `negotiate_auth_header(url)`

Returns the `Authorization` header value for SPNEGO/Kerberos authentication.
Requires a valid Kerberos ticket (`kinit` or OS-level SSO). The URL must be
HTTPS.

```sql
SELECT negotiate_auth_header('https://intranet.example.com/api/data');
-- Returns: 'Negotiate YIIGhgYJKoZI...'
```

Use it to authenticate HTTP requests to Kerberos-protected services:

```sql
SELECT response_status_code, response_body
FROM http_get('https://intranet.example.com/api/data',
    headers := MAP {
        'Authorization': negotiate_auth_header('https://intranet.example.com/api/data')
    });
```

Or configure it globally so all requests to a host auto-authenticate:

```sql
SET VARIABLE http_config = MAP {
    'https://intranet.example.com/': '{"auth_type": "negotiate"}'
};

-- No explicit headers needed — the token is generated and injected automatically
SELECT * FROM http_get('https://intranet.example.com/api/data');
```

### `negotiate_auth_header_json(url)`

Returns a JSON object with the token and debugging metadata about the
authentication process. Useful for diagnosing Kerberos issues.

```sql
SELECT negotiate_auth_header_json('https://intranet.example.com/api/data');
```

Returns:

```json
{
    "token": "YIIGhgYJKoZIhvcSAQICAQBuggY...",
    "header": "Negotiate YIIGhgYJKoZIhvcSAQICAQBuggY...",
    "url": "https://intranet.example.com/api/data",
    "hostname": "intranet.example.com",
    "spn": "HTTP@intranet.example.com",
    "provider": "GSS-API",
    "library": "/System/Library/Frameworks/GSS.framework/Versions/Current/GSS"
}
```

The fields:

| Field | Description |
|-------|-------------|
| `token` | Base64-encoded SPNEGO token |
| `header` | Complete `Authorization` header value (`Negotiate <token>`) |
| `url` | The URL the token was generated for |
| `hostname` | Hostname extracted from the URL |
| `spn` | Service Principal Name used (`HTTP@hostname`) |
| `provider` | Authentication provider (`GSS-API` on macOS/Linux, `SSPI` on Windows) |
| `library` | Path to the loaded security library |

This is particularly useful for verifying that the correct SPN is being
constructed, that the right security library is loaded, and that the hostname
extraction is working as expected.

## Building

```bash
make
```

This configures cmake, builds the extension, and stamps the metadata for
DuckDB to load it.

## Testing

### Automated tests

```bash
make test_release
```

### Manual testing with the Flask negotiate server

A test server is included for end-to-end Negotiate authentication testing:

```bash
# Start the test server (auto-generates self-signed cert)
python3 test/flask_negotiate_server.py

# In another terminal
duckdb -unsigned -cmd "LOAD 'build/release/http_client.duckdb_extension';" -c "
    -- Health check (no auth required)
    SELECT response_status_code
    FROM http_get('https://localhost:8443/health', verify_ssl := false);

    -- Authenticated request
    SELECT response_status_code, response_body
    FROM http_get('https://localhost:8443/data.json',
        headers := MAP {
            'Authorization': negotiate_auth_header('https://localhost:8443/data.json')
        },
        verify_ssl := false);
"
```

## Note on authorship

The code and documentation in this repository were generated entirely by
Claude Opus 4.6 (Anthropic), under close human supervision. The project is a
vehicle for experimentation with designs, implementation techniques, and the
boundaries of AI-assisted software development — not a production-ready
artifact. Architectural decisions, API shape, and overall direction were
guided by the human; implementation was performed by the model.

## License

MIT

## Acknowledgments

- [sqlite-http](https://github.com/asg017/sqlite-http) by Alex Garcia — the
  model for HTTP-as-SQL-functions
- [pyspnego](https://github.com/jborean93/pyspnego) — used to validate the
  pre-flight Negotiate technique
- Richard E. Silverman — for suggesting the pre-flight Negotiate approach
