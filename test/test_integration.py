"""Integration tests for duckdb-http-client extension.

Requires the Flask concurrency server running on localhost:8444.
Start it before running:  python test/flask_concurrency_server.py
"""

import json
import urllib.request

import duckdb
import pytest

EXTENSION_PATH = "build/release/http_client.duckdb_extension"
SERVER = "http://localhost:8444"


@pytest.fixture
def con():
    c = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    c.execute(f"LOAD '{EXTENSION_PATH}'")
    return c


@pytest.fixture(autouse=True)
def reset_server():
    """Reset Flask server counters before each test."""
    try:
        urllib.request.urlopen(f"{SERVER}/reset")
    except Exception:
        pass
    yield


# ---------------------------------------------------------------------------
# Basic HTTP verbs
# ---------------------------------------------------------------------------

def test_get(con):
    r = con.execute(
        f"SELECT r.response_status_code FROM (SELECT http_get('{SERVER}/fast') AS r)"
    ).fetchone()
    assert r[0] == 200


def test_get_with_headers(con):
    r = con.execute(
        f"SELECT r.response_status_code FROM ("
        f"SELECT http_get('{SERVER}/fast', headers := MAP {{'X-Test': 'ci'}}) AS r)"
    ).fetchone()
    assert r[0] == 200


def test_response_headers_are_map(con):
    r = con.execute(
        f"SELECT r.response_headers['Content-Type'] FROM ("
        f"SELECT http_get('{SERVER}/fast') AS r)"
    ).fetchone()
    assert r[0] is not None


def test_post(con):
    r = con.execute(
        f"""SELECT r.response_status_code FROM (
            SELECT http_post('{SERVER}/fast',
                body := '{{"k": "v"}}',
                content_type := 'application/json') AS r)"""
    ).fetchone()
    assert r[0] in (200, 405)


def test_generic_request(con):
    r = con.execute(
        f"SELECT r.response_status_code FROM ("
        f"SELECT http_request('GET', '{SERVER}/fast') AS r)"
    ).fetchone()
    assert r[0] == 200


# ---------------------------------------------------------------------------
# Batch / data-driven
# ---------------------------------------------------------------------------

def test_batch_urls(con):
    rows = con.execute(
        f"""SELECT r.response_status_code AS status FROM (
            SELECT http_get(url) AS r
            FROM (VALUES ('{SERVER}/fast'), ('{SERVER}/health')) AS t(url))"""
    ).fetchall()
    assert len(rows) == 2
    assert all(r[0] == 200 for r in rows)


# ---------------------------------------------------------------------------
# Parallel execution
# ---------------------------------------------------------------------------

def test_parallel_execution(con):
    con.execute(
        f"""SELECT r.response_status_code FROM (
            SELECT http_get('{SERVER}/slow/' || id::VARCHAR || '?delay=0.2') AS r
            FROM range(5) AS t(id))"""
    ).fetchall()
    stats = json.loads(urllib.request.urlopen(f"{SERVER}/stats").read())
    assert stats["total_requests"] == 5
    assert stats["peak_concurrent_connections"] > 1


# ---------------------------------------------------------------------------
# Rate limit stats
# ---------------------------------------------------------------------------

def test_rate_limit_stats(con):
    con.execute(
        f"SELECT http_get('{SERVER}/fast') FROM range(3) AS t(id)"
    ).fetchall()
    rows = con.execute("SELECT * FROM http_rate_limit_stats()").fetchall()
    assert len(rows) > 0


# ---------------------------------------------------------------------------
# Negotiate auth error cases (no network needed)
# ---------------------------------------------------------------------------

def test_negotiate_requires_https(con):
    with pytest.raises(duckdb.Error, match="HTTPS"):
        con.execute("SELECT negotiate_auth_header('http://example.com')")


def test_negotiate_requires_valid_url(con):
    with pytest.raises(duckdb.Error, match="Invalid URL"):
        con.execute("SELECT negotiate_auth_header('not-a-url')")


# ---------------------------------------------------------------------------
# Config helpers
# ---------------------------------------------------------------------------

def test_config_set_and_get(con):
    con.execute(
        """SET VARIABLE http_config = http_config_set(
            'https://api.example.com/',
            json_object('auth_type', 'bearer', 'bearer_token', 'tok123'))"""
    )
    r = con.execute(
        "SELECT json_extract_string(http_config_get('https://api.example.com/'), '$.bearer_token')"
    ).fetchone()
    assert r[0] == "tok123"


def test_config_set_preserves_other_scopes(con):
    con.execute(
        "SET VARIABLE http_config = http_config_set('https://a.com/', json_object('timeout', 10))"
    )
    con.execute(
        "SET VARIABLE http_config = http_config_set('https://b.com/', json_object('timeout', 20))"
    )
    a = con.execute("SELECT http_config_get('https://a.com/')").fetchone()
    b = con.execute("SELECT http_config_get('https://b.com/')").fetchone()
    assert a[0] is not None
    assert b[0] is not None


def test_config_remove(con):
    con.execute(
        "SET VARIABLE http_config = http_config_set('https://a.com/', json_object('timeout', 10))"
    )
    con.execute(
        "SET VARIABLE http_config = http_config_set('https://b.com/', json_object('timeout', 20))"
    )
    con.execute("SET VARIABLE http_config = http_config_remove('https://a.com/')")
    a = con.execute("SELECT http_config_get('https://a.com/')").fetchone()
    b = con.execute("SELECT http_config_get('https://b.com/')").fetchone()
    assert a[0] is None
    assert b[0] is not None


def test_config_set_bearer(con):
    con.execute(
        "SET VARIABLE http_config = http_config_set_bearer('https://v.com/', 'eyJtoken', expires_at := 1741564800)"
    )
    r = con.execute("SELECT http_config_get('https://v.com/')").fetchone()
    cfg = json.loads(r[0])
    assert cfg["auth_type"] == "bearer"
    assert cfg["bearer_token"] == "eyJtoken"
    assert cfg["bearer_token_expires_at"] == 1741564800


def test_config_set_bearer_without_expiry(con):
    con.execute(
        "SET VARIABLE http_config = http_config_set_bearer('https://v.com/', 'simple')"
    )
    r = con.execute("SELECT http_config_get('https://v.com/')").fetchone()
    cfg = json.loads(r[0])
    assert cfg["bearer_token"] == "simple"
    assert "bearer_token_expires_at" not in cfg


# ---------------------------------------------------------------------------
# Bearer token expiry
# ---------------------------------------------------------------------------

def test_expired_bearer_token_raises_error(con):
    # epoch 1 = 1970-01-01T00:00:01Z — definitely expired
    con.execute(
        "SET VARIABLE http_config = http_config_set_bearer('http://localhost:9999/', 'expired', expires_at := 1)"
    )
    with pytest.raises(duckdb.Error, match="Bearer token for localhost expired at"):
        con.execute(
            "SELECT r.response_status_code FROM (SELECT http_get('http://localhost:9999/anything') AS r)"
        )
