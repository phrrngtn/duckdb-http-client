#!/usr/bin/env python3
"""
Minimal HTTP forward proxy that logs every request passing through.

This is NOT a production proxy — it's a test fixture for verifying that the
bhttp extension actually routes requests through a configured proxy.

It handles HTTP CONNECT (tunneling) and plain HTTP proxy requests.
For our test purposes we only need plain HTTP (not HTTPS tunneling),
since the target is a localhost Flask server on plain HTTP.

Usage:
    python test/flask_proxy_server.py          # listens on :8446

Endpoints (proxy control — not proxied):
    GET /proxy/stats    — returns log of proxied requests
    GET /proxy/reset    — clears the log
    GET /proxy/health   — liveness check
"""

import os
import sys
import threading
import time
from datetime import datetime
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.request import urlopen, Request
from urllib.error import URLError, HTTPError
import json


lock = threading.Lock()
proxy_log: list[dict] = []
total_proxied = 0


class ProxyHandler(BaseHTTPRequestHandler):
    """Forward proxy: intercepts requests and forwards them to the origin."""

    def do_GET(self):
        self._proxy_request("GET")

    def do_POST(self):
        self._proxy_request("POST")

    def do_PUT(self):
        self._proxy_request("PUT")

    def do_DELETE(self):
        self._proxy_request("DELETE")

    def do_PATCH(self):
        self._proxy_request("PATCH")

    def do_HEAD(self):
        self._proxy_request("HEAD")

    def _proxy_request(self, method):
        global total_proxied

        # Control endpoints (not proxied)
        if self.path == "/proxy/stats":
            self._send_json(200, {
                "total_proxied": total_proxied,
                "proxy_log": proxy_log,
            })
            return
        if self.path == "/proxy/reset":
            with lock:
                total_proxied = 0
                proxy_log.clear()
            self._send_json(200, {"status": "reset"})
            return
        if self.path == "/proxy/health":
            self._send_json(200, {"status": "ok"})
            return

        # Actual proxying: the path is the full URL when used as a forward proxy
        target_url = self.path
        now = time.time()

        with lock:
            total_proxied += 1
            req_id = total_proxied
            proxy_log.append({
                "id": req_id,
                "method": method,
                "url": target_url,
                "timestamp": round(now, 6),
            })

        print(
            f"[{datetime.now().strftime('%H:%M:%S.%f')[:-3]}] "
            f"PROXY #{req_id:3d} {method} {target_url}",
            file=sys.stderr,
        )

        # Read request body if present
        content_length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(content_length) if content_length > 0 else None

        # Forward the request
        try:
            req = Request(target_url, data=body, method=method)
            # Copy headers (skip proxy-specific ones)
            for key, value in self.headers.items():
                if key.lower() not in ("host", "proxy-connection", "proxy-authorization"):
                    req.add_header(key, value)

            with urlopen(req) as resp:
                resp_body = resp.read()
                self.send_response(resp.status)
                for key, value in resp.getheaders():
                    if key.lower() not in ("transfer-encoding",):
                        self.send_header(key, value)
                self.end_headers()
                self.wfile.write(resp_body)
        except HTTPError as e:
            body_bytes = e.read()
            self.send_response(e.code)
            for key, value in e.headers.items():
                if key.lower() not in ("transfer-encoding",):
                    self.send_header(key, value)
            self.end_headers()
            self.wfile.write(body_bytes)
        except URLError as e:
            self._send_json(502, {"error": str(e.reason)})
        except Exception as e:
            self._send_json(502, {"error": str(e)})

    def _send_json(self, status, obj):
        body = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format, *args):
        # Suppress default access log — we have our own logging
        pass


class ThreadedHTTPServer(HTTPServer):
    """Handle each request in a new thread."""
    def process_request(self, request, client_address):
        t = threading.Thread(target=self._handle, args=(request, client_address))
        t.daemon = True
        t.start()

    def _handle(self, request, client_address):
        try:
            self.finish_request(request, client_address)
        except Exception:
            self.handle_error(request, client_address)
        finally:
            self.shutdown_request(request)


if __name__ == "__main__":
    port = int(os.environ.get("PORT", 8446))
    server = ThreadedHTTPServer(("localhost", port), ProxyHandler)
    print(f"Starting proxy test server on http://localhost:{port}", file=sys.stderr)
    print(f"  Proxies HTTP requests and logs them", file=sys.stderr)
    print(f"  GET /proxy/stats  — proxied request log", file=sys.stderr)
    print(f"  GET /proxy/reset  — clear log", file=sys.stderr)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down proxy server.", file=sys.stderr)
        server.shutdown()
