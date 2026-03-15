#!/usr/bin/env python3
"""
Flask server for testing rate limiter behavior.

Logs request timestamps so tests can verify that the GCRA pacer actually
spaces requests.  Optionally returns 429 + Retry-After when a caller-
defined threshold is exceeded (per sliding window).

Usage:
    python test/flask_ratelimit_server.py

Endpoints:
    GET  /req/<path>         — fast endpoint, logs timestamp
    GET  /stats              — returns request log with timestamps
    GET  /reset              — clears counters
    POST /configure          — set server-side 429 threshold (JSON body)
    GET  /health             — liveness check
"""

import json
import os
import sys
import threading
import time
from collections import deque
from datetime import datetime

from flask import Flask, jsonify, request

app = Flask(__name__)

lock = threading.Lock()
request_log: list[dict] = []
total_requests = 0

# Server-side 429 enforcement (optional — disabled by default).
# Set via POST /configure {"max_per_second": 5, "retry_after": 1.0}
enforce_429 = False
max_per_second = 0.0
retry_after_seconds = 1.0
window_requests: deque[float] = deque()  # timestamps within the sliding window


@app.route("/req/<path:path>", methods=["GET", "POST", "PUT", "PATCH", "DELETE"])
def req_endpoint(path):
    global total_requests, enforce_429
    now = time.time()

    with lock:
        total_requests += 1
        req_id = total_requests

        # Optional server-side 429 enforcement
        if enforce_429:
            cutoff = now - 1.0
            while window_requests and window_requests[0] < cutoff:
                window_requests.popleft()
            if len(window_requests) >= max_per_second:
                request_log.append({
                    "id": req_id,
                    "path": path,
                    "timestamp": round(now, 6),
                    "status": 429,
                })
                print(
                    f"[{datetime.now().strftime('%H:%M:%S.%f')[:-3]}] "
                    f"REQ #{req_id:3d} 429 | window={len(window_requests)} | path=/{path}",
                    file=sys.stderr,
                )
                return (
                    jsonify({"error": "rate limited", "request_id": req_id}),
                    429,
                    {"Retry-After": str(retry_after_seconds)},
                )
            window_requests.append(now)

        request_log.append({
            "id": req_id,
            "path": path,
            "timestamp": round(now, 6),
            "status": 200,
        })

    print(
        f"[{datetime.now().strftime('%H:%M:%S.%f')[:-3]}] "
        f"REQ #{req_id:3d} 200 | path=/{path}",
        file=sys.stderr,
    )
    return jsonify({"request_id": req_id, "path": path})


@app.route("/stats")
def stats():
    with lock:
        timestamps = [r["timestamp"] for r in request_log if r["status"] == 200]
        gaps = [
            round(timestamps[i] - timestamps[i - 1], 6)
            for i in range(1, len(timestamps))
        ]
        count_429 = sum(1 for r in request_log if r["status"] == 429)
        return jsonify({
            "total_requests": total_requests,
            "count_429": count_429,
            "request_log": request_log,
            "inter_request_gaps": gaps,
        })


@app.route("/configure", methods=["POST"])
def configure():
    global enforce_429, max_per_second, retry_after_seconds, window_requests
    data = request.get_json(force=True)
    with lock:
        if "max_per_second" in data:
            max_per_second = float(data["max_per_second"])
            enforce_429 = max_per_second > 0
        if "retry_after" in data:
            retry_after_seconds = float(data["retry_after"])
        window_requests.clear()
    return jsonify({"status": "configured", "enforce_429": enforce_429,
                     "max_per_second": max_per_second,
                     "retry_after": retry_after_seconds})


@app.route("/reset")
def reset():
    global total_requests, request_log, enforce_429, window_requests, max_per_second
    with lock:
        total_requests = 0
        request_log = []
        enforce_429 = False
        max_per_second = 0.0
        window_requests.clear()
    return jsonify({"status": "reset"})


@app.route("/health")
def health():
    return jsonify({"status": "ok"})


if __name__ == "__main__":
    port = int(os.environ.get("PORT", 8445))
    print(f"Starting rate-limit test server on http://localhost:{port}", file=sys.stderr)
    print(f"  GET  /req/<path>     — logs timestamp, returns 200 (or 429 if configured)", file=sys.stderr)
    print(f"  GET  /stats          — request log with inter-request gaps", file=sys.stderr)
    print(f"  POST /configure      — set 429 threshold", file=sys.stderr)
    print(f"  GET  /reset          — reset counters", file=sys.stderr)
    app.run(host="localhost", port=port, threaded=True)
