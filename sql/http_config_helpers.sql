-- Configuration helper macros for safe, merge-based updates to individual
-- scopes within http_config. They return the new MAP value for use with
-- SET VARIABLE.

-- Merge a scope's JSON config into the existing http_config, preserving
-- all other scopes. Cast config_json to VARCHAR so json_object() return
-- values (JSON type) are compatible with MAP(VARCHAR, VARCHAR) storage.
CREATE OR REPLACE MACRO http_config_set(scope, config_json) AS
    map_concat(
        _http_config(),
        MAP([scope], [CAST(config_json AS VARCHAR)])
    );

-- Remove a scope from http_config. Returns the new MAP.
CREATE OR REPLACE MACRO http_config_remove(scope) AS
    map_from_entries([
        entry FOR entry IN map_entries(_http_config())
        IF entry.key != scope
    ]);

-- Read a single scope's JSON config string (or NULL).
CREATE OR REPLACE MACRO http_config_get(scope) AS
    _http_config()[scope];

-- Convenience for setting a bearer token with optional expiry.
-- Uses json_object() for safe JSON construction.
CREATE OR REPLACE MACRO http_config_set_bearer(
    scope, token, expires_at := 0) AS
    http_config_set(scope, CASE
        WHEN expires_at > 0 THEN json_object(
            'auth_type', 'bearer',
            'bearer_token', token,
            'bearer_token_expires_at', expires_at)
        ELSE json_object(
            'auth_type', 'bearer',
            'bearer_token', token)
    END);
