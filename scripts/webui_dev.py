#!/usr/bin/env python3
"""Serve the web UI sources live, proxying the API to a running device or simulator.

Edit src/network/html/*.html and reload the browser; no firmware rebuild needed.

  pio run -e simulator_x3 -t run_simulator     # open File Transfer on the simulator (port 8080)
  python3 scripts/webui_dev.py                 # http://127.0.0.1:8000  ->  API on 127.0.0.1:8080
  python3 scripts/webui_dev.py --api http://crosspoint.local --port 8000   # against a real device
"""
import argparse
import http.server
import pathlib
import urllib.error
import urllib.request

HTML = pathlib.Path(__file__).resolve().parent.parent / "src" / "network" / "html"
PAGES = {"/": "AppPage.html", "/settings": "AppPage.html", "/files": "FilesPage.html", "/fonts": "FontsPage.html"}


class Handler(http.server.BaseHTTPRequestHandler):
    api = "http://127.0.0.1:8080"

    def _proxy(self):
        body = self.rfile.read(int(self.headers.get("Content-Length") or 0)) or None
        req = urllib.request.Request(self.api + self.path, data=body, method=self.command)
        for key in ("Content-Type", "If-None-Match"):
            if self.headers.get(key):
                req.add_header(key, self.headers[key])
        try:
            with urllib.request.urlopen(req, timeout=120) as resp:
                status, headers, data = resp.status, resp.headers, resp.read()
        except urllib.error.HTTPError as err:
            status, headers, data = err.code, err.headers, err.read()
        self.send_response(status)
        for key in ("Content-Type", "Content-Encoding", "Content-Disposition"):
            if headers.get(key):
                self.send_header(key, headers[key])
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        page = PAGES.get(self.path.split("?")[0])
        if not page:
            return self._proxy()
        data = (HTML / page).read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    do_POST = do_PUT = do_DELETE = _proxy


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--api", default="http://127.0.0.1:8080", help="device or simulator base URL")
    parser.add_argument("--port", type=int, default=8000)
    args = parser.parse_args()
    Handler.api = args.api.rstrip("/")
    print(f"Web UI dev server: http://127.0.0.1:{args.port}  (API -> {Handler.api})")
    http.server.ThreadingHTTPServer(("127.0.0.1", args.port), Handler).serve_forever()
