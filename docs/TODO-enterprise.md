# Enterprise Feature Ideas

Ideas that might be worth pursuing. These tend toward the "enterprisey" end of
the spectrum — governance, observability, operational controls — and are
recorded here so they don't get lost.

None of these are committed work. They're notes on what the architecture could
support if the need arises.

## Telemetry sink

Push request/response statistics to an external metrics collector (OTEL
collector, Prometheus pushgateway, Splunk HEC, or any HTTP endpoint) so that
DuckDB HTTP activity can participate in existing dashboards, alerts, and
capacity planning.

The extension already has:
- Per-host and global counters (requests, bytes, elapsed, errors, 429s, pacing)
- An HTTP client capable of POSTing JSON

So the implementation would use the extension's own HTTP machinery to push its
own statistics — which means telemetry requests must be excluded from the
counters they report, or you get infinite regress.

Configuration would fit naturally into `http_config`:

```sql
SET VARIABLE http_config = MAP {
    'default': '{"telemetry_sink": "http://otel-collector:4318/v1/metrics"}'
};
```

Push triggers (in order of complexity):
1. Explicit `http_flush_stats(sink_url)` — user controls when
2. On extension unload — automatic, best-effort, no threads
3. Every N requests or T seconds — needs a timer or piggyback mechanism

Start with (1) and (2). If someone wants continuous push, they can call
`http_flush_stats()` on an interval from SQL.

## Bandwidth limiting

The extension controls request *count* but not bytes-on-the-wire. Options:

- **In-extension**: cpr exposes `LimitRate{downrate, uprate}` (maps to
  `CURLOPT_MAX_RECV_SPEED_LARGE`). Per-connection, so aggregate =
  `max_concurrent * per_connection_limit`. Approximate but functional.
- **Proxy-based** (preferred): delegate bandwidth control to a throttling
  proxy (Squid `delay_pools`, Charles Proxy). The extension already supports
  `proxy` in config. This delegates both control and responsibility to IT.
- **OS-level**: macOS `dnctl`/`pfctl`, Linux `tc`. Requires root, system-wide.

The proxy approach is architecturally cleaner: the extension owns request-rate
governance, infrastructure owns bandwidth governance.

## Per-query request ceiling

A `max_requests_per_query` config field that hard-caps the total number of HTTP
requests a single query can make, regardless of rate. Guards against
`FROM range(1000000)` scenarios where rate limiting merely slows the avalanche
rather than stopping it.

## Request audit log

Write a structured log (JSON lines or Parquet) of every HTTP request: URL,
method, status code, elapsed, response size, timestamp. Useful for compliance
and post-incident forensics. The sink could be a local file, an S3 path, or
an HTTP endpoint (same telemetry sink pattern).

## Circuit breaker

If a host returns N consecutive errors (5xx or timeout), stop sending requests
to that host for a cooldown period. Prevents the extension from hammering a
service that's already in trouble. Classic Hystrix/resilience4j pattern.

## Retry with backoff

Configurable retry for transient failures (429, 503, network errors) with
exponential backoff and jitter. The rate limiter already handles 429 feedback
by pushing the TAT forward; explicit retry would complement this for cases
where the request should actually be re-sent.

## Mutual TLS (mTLS)

Client certificate authentication for zero-trust environments. The `ca_bundle`
config field exists; adding `client_cert` and `client_key` fields would
complete the picture. cpr supports this via `SslOptions`.

## Request tagging / correlation IDs

Inject a configurable header (e.g. `X-Request-ID`, `X-Correlation-ID`) into
every outbound request, with a value that traces back to the DuckDB query.
Helps service owners correlate their logs with the DuckDB workload that
generated the traffic.
