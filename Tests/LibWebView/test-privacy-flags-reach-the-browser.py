#!/usr/bin/env python3

# Launches the real Ladyflock binary, not the in-process test application, to check that the privacy options given on its
# command line and in its profile reach the processes that enforce them. Everything is observed by servers outside the
# browser on loopback.

import argparse
import json
import runpy
import socket
import tempfile
import threading
import time

from http.server import BaseHTTPRequestHandler
from http.server import ThreadingHTTPServer
from pathlib import Path

devtools = runpy.run_path(str(Path(__file__).with_name("test-devtools-client-follows-the-tab.py")))

PAGES = {
    "/page": """<!doctype html><title>page</title>
<a id="link" href="/next?fbclid=from-link&k=2">next</a>
<script>document.getElementById("link").click();</script>""",
    "/scripted": """<!doctype html><title>scripted</title>
<script src="/script.js"></script>
<script>new Image().src = "/inline-beacon";</script>
<noscript><img src="/noscript-pixel"></noscript>""",
}


class RecordingServer:
    def __init__(self):
        self.paths = []
        self.lock = threading.Lock()
        recorder = self

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                with recorder.lock:
                    recorder.paths.append(self.path)
                body = PAGES.get(self.path.split("?")[0], "ok").encode()
                self.send_response(200)
                self.send_header("Content-Type", "text/html" if self.path.split("?")[0] in PAGES else "text/plain")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, format, *args):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.port = self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def seen(self):
        with self.lock:
            return list(self.paths)

    def wait_for(self, prefix, timeout=15):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for path in self.seen():
                if path.startswith(prefix):
                    return path
            time.sleep(0.05)
        raise AssertionError(f"The server never saw {prefix}; it saw {self.seen()}")

    def close(self):
        self.server.shutdown()


class CountingListener:
    """Stands where the proxy route points, and only counts the connections it gets."""

    def __init__(self):
        self.socket = socket.socket()
        self.socket.bind(("127.0.0.1", 0))
        self.socket.listen(16)
        self.port = self.socket.getsockname()[1]
        self.connections = 0
        threading.Thread(target=self.accept, daemon=True).start()

    def accept(self):
        while True:
            try:
                connection, _ = self.socket.accept()
            except OSError:
                return
            self.connections += 1
            connection.close()

    def close(self):
        self.socket.close()


def run_browser(binary, profile, url, *arguments):
    process, _, client = devtools["launch"](binary, str(profile), url, *arguments)
    return process, client


def test_tracking_parameters(binary, root):
    server = RecordingServer()
    process = None
    try:
        process, _ = run_browser(
            binary, root / "tracking", f"http://127.0.0.1:{server.port}/page?utm_source=news&id=1", "--privacy-strip-tracking-parameters"
        )
        assert server.wait_for("/page") == "/page?id=1", server.seen()
        # A navigation the page starts in its own process is cleaned too.
        assert server.wait_for("/next") == "/next?k=2", server.seen()
        print("PASS: --privacy-strip-tracking-parameters reaches both the UI process and WebContent")
    finally:
        if process:
            devtools["stop"](process)
        server.close()


def test_site_scripting_from_profile(binary, root):
    server = RecordingServer()
    profile = root / "scripting"
    (profile / "config").mkdir(parents=True)
    (profile / "config" / "Settings.json").write_text(json.dumps({"siteControls": {"scriptingDisabledSites": ["127.0.0.1"]}}))
    process = None
    try:
        process, _ = run_browser(binary, profile, f"http://127.0.0.1:{server.port}/scripted")
        server.wait_for("/noscript-pixel")
        time.sleep(0.5)
        paths = server.seen()
        assert not any(path.startswith("/script.js") or path.startswith("/inline-beacon") for path in paths), paths
        print("PASS: a site's scripting turned off in the profile's settings reaches its pages")
    finally:
        if process:
            devtools["stop"](process)
        server.close()


def test_mandatory_route(binary, root):
    server = RecordingServer()
    route = CountingListener()
    process = None
    try:
        process, _ = run_browser(
            binary,
            root / "route",
            f"http://127.0.0.1:{server.port}/routed",
            "--privacy-network-mode=proxy-only",
            f"--privacy-proxy=socks5h://127.0.0.1:{route.port}",
        )
        deadline = time.monotonic() + 15
        while route.connections == 0 and time.monotonic() < deadline:
            time.sleep(0.05)
        time.sleep(1)
        assert route.connections >= 1, "the browser never connected to the proxy route"
        assert not any(path.startswith("/routed") for path in server.seen()), server.seen()
        print("PASS: --privacy-network-mode=proxy-only with --privacy-proxy sends the page over the route, never directly")
    finally:
        if process:
            devtools["stop"](process)
        route.close()
        server.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("ladybird_binary")
    options = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="ladyflock-privacy-flags-") as directory:
        root = Path(directory)
        test_tracking_parameters(options.ladybird_binary, root)
        test_site_scripting_from_profile(options.ladybird_binary, root)
        test_mandatory_route(options.ladybird_binary, root)
