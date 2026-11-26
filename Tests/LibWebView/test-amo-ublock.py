#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladyflock developers.
#
# SPDX-License-Identifier: BSD-2-Clause

"""Opt-in live test: python3 test-amo-ublock.py /path/to/Ladybird [--addon origin|lite]."""

import argparse
import collections
import http.server
import itertools
import json
import runpy
import tempfile
import threading
import time
import urllib.parse
from pathlib import Path


TOOLS = runpy.run_path(str(Path(__file__).with_name("test-devtools-client-follows-the-tab.py")))
ADDONS = {
    "origin": "uBlock0@raymondhill.net",
    "lite": "uBOLiteRedux@raymondhill.net",
}


class AdFixture(http.server.ThreadingHTTPServer):
    def __init__(self):
        super().__init__(("127.0.0.1", 0), FixtureHandler)
        self.requests = collections.Counter()
        self.requests_lock = threading.Lock()

    def hits(self, path):
        with self.requests_lock:
            return self.requests[path]


class FixtureHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        request = urllib.parse.urlsplit(self.path)
        with self.server.requests_lock:
            self.server.requests[self.path] += 1

        if request.path == "/fixture":
            self.reply("text/html", "<!doctype html><title>Ad blocking fixture</title><main>Article</main>")
        elif request.path in ("/adspop.js", "/article.js"):
            self.reply("text/javascript", "window.loadedFixtureScripts.push(document.currentScript.src)")
        else:
            self.send_error(404)

    def reply(self, content_type, body):
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body.encode())

    def log_message(self, *args):
        pass


def wait(description, predicate, timeout=45):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.05)
    raise AssertionError(f"Timed out waiting for {description}")


def test_addon(binary, fixture, key):
    addon_id = ADDONS[key]
    base = f"http://127.0.0.1:{fixture.server_port}"
    with tempfile.TemporaryDirectory(prefix=f"ladyflock-amo-ublock-{key}-") as directory:
        profile = Path(directory) / "profile"
        process, _, client = TOOLS["launch"](binary, str(profile), "about:addons", "--enable-amo-catalog")
        try:
            browser_id = client.tab()["browserId"]

            def original_tab():
                # The extension can create a background target or open a tab during installation.
                client.send({"to": "root", "type": "listTabs"})
                tabs = client.receive_matching(lambda packet: packet.get("from") == "root" and "tabs" in packet)["tabs"]
                return next(tab for tab in tabs if tab["browserId"] == browser_id)

            client.tab = original_tab
            TOOLS["wait_for_page"](client, "about:addons", "Add-ons")
            client.watch_frame_targets()
            evaluate = client.evaluate

            def element(selector):
                return f"document.querySelector({json.dumps(selector)})"

            def click(selector):
                evaluate(f"""(() => {{
                    const element = {element(selector)};
                    element.scrollIntoView({{block: 'center'}});
                    const bounds = element.getBoundingClientRect();
                    internals.clickThroughUIProcess(bounds.x + bounds.width / 2, bounds.y + bounds.height / 2);
                    return true;
                }})()""")

            def navigate(url, title):
                client.submit(f"location.href = {json.dumps(url)}; true")
                TOOLS["wait_for_page"](client, url, title)

            def load_scripts(stage):
                url = f"{base}/fixture?{key}-{stage}"
                navigate(url, "Ad blocking fixture")
                ad = f"/adspop.js?{key}-{stage}"
                article = f"/article.js?{key}-{stage}"
                evaluate(f"""(() => {{
                    window.loadedFixtureScripts = [];
                    window.fixtureOutcomes = {{}};
                    for (const [kind, path] of Object.entries({json.dumps({"ad": ad, "article": article})})) {{
                        const script = document.createElement('script');
                        script.src = path;
                        script.onload = () => fixtureOutcomes[kind] = loadedFixtureScripts.includes(script.src)
                            ? 'executed' : 'replaced';
                        script.onerror = () => fixtureOutcomes[kind] = 'blocked';
                        document.body.append(script);
                    }}
                    return true;
                }})()""")
                wait("both requests to resolve", lambda: evaluate("Object.keys(fixtureOutcomes).length === 2"))
                assert evaluate("fixtureOutcomes.article") == "executed"
                assert fixture.hits(article) == 1
                return evaluate("fixtureOutcomes.ad"), fixture.hits(ad)

            assert load_scripts("before") == ("executed", 1)
            navigate("about:addons", "Add-ons")
            wait("the Mozilla catalog search to be enabled", lambda: evaluate("document.querySelector('#search')?.disabled === false"))
            evaluate("document.querySelector('#query').value = 'ublock'; true")
            click("#search")
            listing = f'[data-addon="{addon_id}"] button'
            wait(f"{addon_id} in live Mozilla Add-ons results", lambda: evaluate(f"!!{element(listing)}"))
            click(listing)
            wait("signed package review", lambda: evaluate("document.querySelector('#review').open || document.querySelector('#notice').classList.contains('error')"))
            assert evaluate("document.querySelector('#review').open"), evaluate("document.querySelector('#notice').innerText")
            assert "Verified signature" in evaluate("document.querySelector('#review').innerText")
            click("#confirm")
            installed = f'[data-extension="{addon_id}"]'
            wait("extension installation", lambda: evaluate(f"!!{element(installed)}"))
            registry = json.loads((profile / "data/extensions/registry.json").read_text())
            entry = next(row for row in registry["extensions"] if row["extension_id"] == addon_id)
            assert entry["source"] == "signed_xpi" and entry["verified_signature"] and entry["from_catalog"], entry
            print(f"{addon_id} {entry['version']} SHA-256 {entry['sha256_hex']} installed from Mozilla Add-ons", flush=True)

            # A persistent background can spend time compiling the stock lists on first install.
            deadline = time.monotonic() + 180
            for attempt in itertools.count():
                outcome = load_scripts(f"enabled-{attempt}")
                if outcome == ("blocked", 0):
                    break
                if time.monotonic() >= deadline:
                    raise AssertionError(f"{addon_id} did not block the stock filter's ad request: {outcome!r}")
                time.sleep(2)

            navigate("about:addons", "Add-ons")
            wait("installed add-on controls", lambda: evaluate(f"!!{element(installed)}"))
            click(installed + " .entry-header .switch-label")
            wait("extension to be disabled", lambda: evaluate(f"{element(installed + ' [data-control=enabled]')}?.checked === false"))
            assert load_scripts("disabled") == ("executed", 1)
            print(f"PASS {addon_id}: only the ad request was blocked, and it reached the server after disable", flush=True)
        finally:
            client.close()
            TOOLS["stop"](process)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ladybird", type=lambda value: str(Path(value).resolve()))
    parser.add_argument("--addon", choices=ADDONS)
    args = parser.parse_args()
    with AdFixture() as fixture:
        thread = threading.Thread(target=fixture.serve_forever, daemon=True)
        thread.start()
        try:
            for key in [args.addon] if args.addon else ADDONS:
                test_addon(args.ladybird, fixture, key)
        finally:
            fixture.shutdown()
            thread.join()


if __name__ == "__main__":
    main()
