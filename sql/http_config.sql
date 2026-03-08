-- Internal helper: safely read the http_config variable.
-- Returns an empty MAP if the variable is not set.
CREATE OR REPLACE MACRO _http_config() AS
    IFNULL(TRY_CAST(getvariable('http_config') AS MAP(VARCHAR, VARCHAR)), MAP {});
