-- Per-verb scalar macros route to the idempotent or volatile C function
-- based on HTTP method semantics. All return STRUCT.
--
-- Idempotent verbs: safe to deduplicate identical calls within a query.
-- GET, HEAD, OPTIONS are read-only; PUT and DELETE are idempotent by spec.
--
-- Headers are MAP(VARCHAR, VARCHAR) — passed directly to the C function.
-- Config is read from the http_config variable and cast to JSON for the
-- C function, which uses nlohmann to parse per-scope config values.

CREATE OR REPLACE MACRO http_get(url,
    headers := NULL::MAP(VARCHAR, VARCHAR)) AS
    _http_raw_request('GET', url, headers, NULL, NULL,
        CAST(_http_config() AS JSON));

CREATE OR REPLACE MACRO http_head(url,
    headers := NULL::MAP(VARCHAR, VARCHAR)) AS
    _http_raw_request('HEAD', url, headers, NULL, NULL,
        CAST(_http_config() AS JSON));

CREATE OR REPLACE MACRO http_options(url,
    headers := NULL::MAP(VARCHAR, VARCHAR)) AS
    _http_raw_request('OPTIONS', url, headers, NULL, NULL,
        CAST(_http_config() AS JSON));

CREATE OR REPLACE MACRO http_put(url,
    headers := NULL::MAP(VARCHAR, VARCHAR),
    body := NULL::VARCHAR,
    content_type := NULL::VARCHAR) AS
    _http_raw_request('PUT', url, headers, body, content_type,
        CAST(_http_config() AS JSON));

CREATE OR REPLACE MACRO http_delete(url,
    headers := NULL::MAP(VARCHAR, VARCHAR)) AS
    _http_raw_request('DELETE', url, headers, NULL, NULL,
        CAST(_http_config() AS JSON));

-- Non-idempotent verbs: volatile, every call fires.

CREATE OR REPLACE MACRO http_post(url,
    headers := NULL::MAP(VARCHAR, VARCHAR),
    body := NULL::VARCHAR,
    content_type := NULL::VARCHAR) AS
    _http_raw_request_volatile('POST', url, headers, body, content_type,
        CAST(_http_config() AS JSON));

CREATE OR REPLACE MACRO http_patch(url,
    headers := NULL::MAP(VARCHAR, VARCHAR),
    body := NULL::VARCHAR,
    content_type := NULL::VARCHAR) AS
    _http_raw_request_volatile('PATCH', url, headers, body, content_type,
        CAST(_http_config() AS JSON));

-- Generic scalar: method is a runtime parameter, so must be volatile
-- (we can't know at compile time whether it's idempotent).

CREATE OR REPLACE MACRO http_request(method, url,
    headers := NULL::MAP(VARCHAR, VARCHAR),
    body := NULL::VARCHAR,
    content_type := NULL::VARCHAR) AS
    _http_raw_request_volatile(method, url, headers, body, content_type,
        CAST(_http_config() AS JSON));

-- JSON variant: wraps the STRUCT with to_json().

CREATE OR REPLACE MACRO http_request_json(method, url,
    headers := NULL::MAP(VARCHAR, VARCHAR),
    body := NULL::VARCHAR,
    content_type := NULL::VARCHAR) AS
    to_json(_http_raw_request_volatile(method, url, headers, body, content_type,
        CAST(_http_config() AS JSON)));
