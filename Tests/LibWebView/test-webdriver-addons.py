#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladyflock developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import base64
import json
import runpy
import subprocess
import tempfile

from pathlib import Path

helpers = runpy.run_path(str(Path(__file__).with_name("test-webdriver-delete-session.py")))


def run_test(webdriver_binary, theme, width, screenshots):
    with tempfile.TemporaryDirectory(prefix="ladyflock-addons-") as directory:
        config = Path(directory) / "config"
        config.mkdir()
        (config / "Settings.json").write_text(
            json.dumps({"appearance": {"websiteColorScheme": theme, "interfaceTheme": theme}})
        )
        process, port = helpers["launch_webdriver"](
            webdriver_binary,
            "--headless",
            "--force-cpu-painting",
            "--expose-internals-object",
            "--profile-path",
            directory,
        )
        session = None
        try:
            session = helpers["create_session"](port)

            def command(method, path, data=None):
                status, response, raw = helpers["request"](port, method, f"/session/{session}/{path}", data)
                assert status == 200, raw
                return response["value"]

            def script(source, *args):
                return command("POST", "execute/sync", {"script": source, "args": args})

            def screenshot(state):
                if screenshots:
                    (screenshots / f"addons-{theme}-{width}-{state}.png").write_bytes(
                        base64.b64decode(command("GET", "screenshot"), validate=True)
                    )

            def reply(message_name, **data):
                script(
                    "document.dispatchEvent(new CustomEvent('WebUIMessage', "
                    "{detail: {name: arguments[0], data: arguments[1]}}));",
                    message_name,
                    data,
                )

            def click(selector):
                script("document.querySelector(arguments[0]).click();", selector)

            def sent():
                return script("return window.requests.at(-1);")

            command("POST", "window/rect", {"width": width, "height": 680})
            command("POST", "url", {"url": "about:addons"})
            # Use the real WebUI channel for the initial profile state.
            command(
                "POST",
                "execute/async",
                {
                    "script": """
                    const done = arguments[0];
                    function ready() { return document.querySelector("#installed")?.textContent.includes("No extensions installed."); }
                    if (ready()) return done();
                    const observer = new MutationObserver(() => {
                        if (ready()) { observer.disconnect(); done(); }
                    });
                    observer.observe(document.documentElement, {subtree: true, childList: true});
                """,
                    "args": [],
                },
            )
            assert script("return document.title;") == "Add-ons"
            assert script("return innerWidth;") == width
            assert script("return matchMedia('(prefers-color-scheme: dark)').matches;") == (theme == "dark")
            assert script("return document.documentElement.scrollWidth <= innerWidth;")
            screenshot("empty")

            # Drive the catalog and staged-package replies without contacting AMO or opening a native file picker.
            script("window.requests = []; ladybird.sendMessage = (name, data) => requests.push([name, data]);")
            ext = {
                "extension_id": "fixture@example.test",
                "name": "Example extension with a long name " + "abcdef" * 12,
                "version": "1.0",
                "source": "unpacked",
                "enabled": True,
                "allow_all_sites": True,
                "has_options": True,
                "can_check_update": True,
                "host_permissions": ["https://example.test/*"],
                "compatibility_status": "Manifest compatible · Untested",
            }
            summary = {"extensions": [ext], "extensions_enabled": True, "enable_public_amo_catalog": True}
            reply("installedExtensions", **summary)
            click('[data-control="enabled"]')
            assert sent() == ["setExtensionEnabled", {"extension_id": ext["extension_id"], "enabled": False}]
            assert script("return document.querySelector('[data-control=enabled]').checked;")
            reply("extensionStateUpdated", error="Change denied")
            assert script("return document.querySelector('#notice').textContent;") == "Change denied"
            click('[data-control="access"]')
            click('[data-control="all-sites"]')
            assert sent() == ["setExtensionSiteAccess", {"extension_id": ext["extension_id"], "allow_all_sites": False}]
            reply("extensionSiteAccessUpdated", summary=summary)
            assert script("return document.querySelector('details').open;")

            click('[data-control="remove"]')
            assert script("return document.activeElement.id;") == "cancel"
            command(
                "POST",
                "actions",
                {
                    "actions": [
                        {
                            "type": "key",
                            "id": "keyboard",
                            "actions": [{"type": "keyDown", "value": "\ue00c"}, {"type": "keyUp", "value": "\ue00c"}],
                        }
                    ]
                },
            )
            assert script("return document.activeElement.dataset.control;") == "remove"
            assert sent()[0] != "removeExtension"
            click('[data-control="options"]')
            assert sent() == ["openExtensionOptions", {"extension_id": ext["extension_id"]}]
            reply("extensionOptionsOpened")
            script("document.querySelector('#query').value='example'; document.querySelector('#search').click();")
            name, request = sent()
            assert name == "searchCatalog" and request["query"] == "example" and request["page"] == 1
            catalog_entry = {
                "addon_id": "catalog@example.test",
                "name": "Example catalog entry",
                "version": "2",
                "summary": "<b>plain text</b>",
                "author": "Example author",
                "compatibility_status": "Package review required",
                "listing_url": "https://addons.mozilla.org/en-US/firefox/",
            }
            reply("catalogSearchResults", request_id=request["request_id"] - 1, results=[catalog_entry])
            assert script("return document.querySelector('#results').childElementCount;") == 0
            reply("catalogSearchResults", request_id=request["request_id"], results=[catalog_entry], has_more=True)
            assert script("return !document.querySelector('#results b') && !document.querySelector('#pages').hidden;")
            assert script("return document.documentElement.scrollWidth <= innerWidth;")
            screenshot("installed-and-catalog")
            click("#next")
            assert sent()[1]["page"] == 2
            reply("catalogSearchResults", request_id=sent()[1]["request_id"], results=[catalog_entry], has_more=False)
            assert script(
                "return document.querySelector('#next').disabled && !document.querySelector('#previous').disabled;"
            )

            click("#results button")
            assert sent() == ["stageCatalogInstall", {"extension_id": catalog_entry["addon_id"]}]
            staged = {
                "token": "opaque-review-token",
                "name": catalog_entry["name"],
                "version": "2",
                "verified_signature": True,
                "compatibility_status": ext["compatibility_status"],
                "permissions": ["storage"],
                "host_permissions": ["https://example.test/*"],
                "optional_permissions": ["tabs"],
                "optional_host_permissions": ["https://optional.test/*"],
                "data_collection": {"required": ["technicalAndInteraction"], "optional": ["browsingActivity"]},
                "requires_permission_prompt": True,
            }
            reply("installStaged", **staged)
            assert script("return document.activeElement.id;") == "cancel"
            assert sent()[0] == "stageCatalogInstall"
            review = script("return document.querySelector('#review').textContent;")
            for value in (
                "storage",
                "https://example.test/*",
                "tabs",
                "https://optional.test/*",
                "technicalAndInteraction",
                "browsingActivity",
            ):
                assert value in review, value
            assert script(
                "return document.querySelector('#review').scrollWidth <= document.querySelector('#review').clientWidth;"
            )
            screenshot("review")
            if width == 800:
                script("internals.setDevicePixelRatio(2)")
                assert script("return devicePixelRatio;") == 2
                assert script("return document.documentElement.scrollWidth <= innerWidth;")
                screenshot("review-2x")
                script("internals.setDevicePixelRatio(1); internals.setBrowserZoom(1.5);")
                assert script("return document.documentElement.scrollWidth <= innerWidth;")
                screenshot("review-zoom150")
                script("internals.setBrowserZoom(1)")
            click("#cancel")
            assert sent() == ["cancelInstall", {"token": staged["token"]}]
            reply("installCancelled")
            click("#pick-file")
            assert sent() == ["pickLocalXpi", {}]
            reply("installStaged", **dict(staged, verified_signature=False))
            assert sent() == ["cancelInstall", {"token": staged["token"]}]
            assert not script("return document.querySelector('#review').open;")
            reply("installCancelled")
            click("#pick-file")
            reply("installStaged", **staged)
            click("#confirm")
            assert sent() == ["approveInstall", {"token": staged["token"], "consented": True}]
            reply("installCompleted", summary=summary)
            click('[data-control="update"]')
            assert sent() == ["checkManualUpdate", {"extension_id": ext["extension_id"]}]
            reply("updateStaged", **staged, newly_requested_permissions=["storage"])
            assert script(
                "return document.querySelector('#review').innerText.includes('New permissions in this update');"
            )
            click("#confirm")
            assert sent() == ["approveUpdate", {"token": staged["token"], "consented": True}]
            local_extension = dict(ext, source="signed_xpi", can_check_update=False)
            reply("updateCompleted", summary=dict(summary, extensions=[local_extension]))
            click('[data-control="update-file"]')
            assert sent() == ["pickLocalXpi", {"extension_id": ext["extension_id"]}]
            reply("updateStaged", **staged, newly_requested_permissions=[])
            click("#cancel")
            assert sent()[0] == "cancelInstall"
            reply("installCancelled")
            click('[data-control="remove"]')
            click("#confirm")
            assert sent() == ["removeExtension", {"extension_id": ext["extension_id"]}]
            reply("extensionRemoved", summary=dict(summary, extensions=[]))
            assert script("return document.querySelector('#installed').innerText;") == "No extensions installed."
            print(
                f"PASS: about:addons profile state, controls, catalog pagination and permission review ({theme}, {width}px)"
            )
        finally:
            if session:
                helpers["request"](port, "DELETE", f"/session/{session}")
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("webdriver_binary")
    parser.add_argument("--screenshots", type=Path)
    options = parser.parse_args()
    if options.screenshots:
        options.screenshots.mkdir(parents=True, exist_ok=True)
    for color_scheme, viewport_width in (("light", 800), ("dark", 320)):
        run_test(options.webdriver_binary, color_scheme, viewport_width, options.screenshots)
