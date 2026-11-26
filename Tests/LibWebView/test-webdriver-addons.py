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

            def expect_icon(selector):
                command(
                    "POST",
                    "execute/async",
                    {
                        "script": """
                        const [selector, done] = arguments;
                        const icon = document.querySelector(selector + " .extension-icon");
                        if (icon.querySelector("img")?.naturalWidth) return done();
                        const observer = new MutationObserver(() => {
                            if (icon.querySelector("img")?.naturalWidth) { observer.disconnect(); done(); }
                        });
                        observer.observe(icon, {childList: true});
                        """,
                        "args": [selector],
                    },
                )
                assert script(
                    "const icon = document.querySelector(arguments[0] + ' .extension-icon');"
                    "return icon.getBoundingClientRect().width === 32 && icon.getAttribute('aria-hidden') === 'true';",
                    selector,
                )

            def click(selector):
                script("document.querySelector(arguments[0]).click();", selector)

            def press_key(value):
                command(
                    "POST",
                    "actions",
                    {
                        "actions": [
                            {
                                "type": "key",
                                "id": "keyboard",
                                "actions": [{"type": "keyDown", "value": value}, {"type": "keyUp", "value": value}],
                            }
                        ]
                    },
                )

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
                "icon_key": "package-key",
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
            assert sent() == ["getExtensionIcon", {"icon_key": "package-key"}]
            assert script("return !!document.querySelector('#installed .extension-icon svg');")
            reply("extensionIcon", icon_key="package-key", icon_data_url="https://example.invalid/image.png")
            assert script("return !document.querySelector('#installed .extension-icon img');")
            fixture = Path(__file__).resolve().parents[2] / "Base/res/icons/48x48/app-browser.png"
            png = "data:image/png;base64," + base64.b64encode(fixture.read_bytes()).decode()
            reply("extensionIcon", icon_key="package-key", icon_data_url=png)
            expect_icon("#installed")
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
            expect_icon("#review")
            assert script("return document.activeElement.id;") == "cancel"
            press_key("\ue00c")  # Escape
            assert script("return document.activeElement.dataset.control;") == "remove"
            assert sent()[0] != "removeExtension"
            click('[data-control="options"]')
            assert sent() == ["openExtensionOptions", {"extension_id": ext["extension_id"]}]
            reply("extensionOptionsOpened")
            assert script("""
                const query = document.querySelector("#query");
                query.value = "clear this";
                query.scrollIntoView({block: "center"});
                const clear = internals.getShadowRoot(query).querySelector('button[aria-label="Clear search"]');
                const rect = clear.getBoundingClientRect();
                const field = query.getBoundingClientRect();
                const center = rect.top + rect.height / 2;
                if (Math.abs(center - (field.top + field.height / 2)) > 0.5) return false;
                internals.click(rect.left + rect.width / 2, center);
                return query.value === "" && document.activeElement === query;
            """)
            script("document.querySelector('#query').value='example'; document.querySelector('#search').click();")
            name, request = sent()
            assert name == "searchCatalog" and request["query"] == "example" and request["page"] == 1
            catalog_entry = {
                "addon_id": "catalog@example.test",
                "icon_key": "catalog-key",
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
            assert sent() == ["getExtensionIcon", {"icon_key": "catalog-key"}]
            reply("extensionIcon", icon_key="catalog-key", icon_data_url=png)
            expect_icon("#results")
            screenshot("installed-and-catalog")
            script("document.querySelector('#results').scrollIntoView();")
            screenshot("catalog-icon")
            click("#next")
            assert sent()[1]["page"] == 2
            reply(
                "catalogSearchResults",
                request_id=sent()[1]["request_id"],
                results=[catalog_entry, dict(catalog_entry, addon_id="missing-icon@example.test", icon_key="missing")],
                has_more=False,
            )
            assert sent() == ["getExtensionIcon", {"icon_key": "missing"}]
            reply("extensionIcon", icon_key="missing")
            expect_icon("#results article:first-child")
            assert script("return !!document.querySelector('#results article:last-child .extension-icon svg');")
            requests = script("return window.requests.filter(([name]) => name === 'getExtensionIcon');")
            assert requests.count(["getExtensionIcon", {"icon_key": "catalog-key"}]) == 1
            screenshot("catalog-fallback")
            assert script(
                "return document.querySelector('#next').disabled && !document.querySelector('#previous').disabled;"
            )

            stage = "#results article:first-child button"
            review_label = "Review/Install Extension"
            assert script("return document.querySelector(arguments[0]).textContent;", stage) == review_label
            script("document.querySelector(arguments[0]).focus();", stage)
            press_key("\ue007")  # Enter
            assert sent() == ["stageCatalogInstall", {"extension_id": catalog_entry["addon_id"]}]
            assert script("return document.querySelector(arguments[0]).ariaBusy;", stage) == "true"
            assert script("return document.querySelector('#results article:last-child button').ariaBusy;") is None
            assert script("""
                const button = document.activeElement;
                const spinner = getComputedStyle(button, "::before");
                const reduceMotion = matchMedia("(prefers-reduced-motion: reduce)").matches;
                return button.ariaDisabled === "true" && button.textContent.includes("Preparing review")
                    && spinner.display === "inline-block" && (spinner.animationName === "none") === reduceMotion;
            """)
            assert script("return document.querySelector('#notice[role=status]').textContent;") == (
                "Downloading and verifying the extension…"
            )
            click(stage)
            assert script("return requests.filter(([name]) => name === 'stageCatalogInstall').length;") == 1
            screenshot("staging")
            reply("installStaged", error="Download failed")
            assert script("return document.querySelector('#notice.error').textContent;") == "Download failed"
            assert script(
                """
                const button = document.querySelector(arguments[0]);
                return button === document.activeElement && button.ariaBusy === null && button.ariaDisabled === null;
            """,
                stage,
            )
            click(stage)
            reply("installStaged", cancelled=True)
            assert script("return document.querySelector(arguments[0]).textContent;", stage) == review_label
            click(stage)
            reply("installedExtensions", **summary)
            assert script("return document.querySelector(arguments[0]).ariaBusy;", stage) == "true"
            staged = {
                "token": "opaque-review-token",
                "icon_key": "package-key",
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
            assert script("return document.querySelector(arguments[0]).ariaBusy;", stage) is None
            assert sent()[0] == "stageCatalogInstall"
            expect_icon("#review")
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
            assert script("return document.activeElement === document.querySelector(arguments[0]);", stage)
            reply("installCancelled")
            click("#pick-file")
            assert sent() == ["pickLocalXpi", {}]
            reply(
                "installStaged",
                error="Package incompatible",
                compatibility_reasons=["Unsupported pattern <file>", "Unsupported permission"],
            )
            assert script("return document.querySelector('#notice.error').textContent;") == (
                "Package incompatible\nUnsupported pattern <file>\nUnsupported permission"
            )
            assert not script("return !!document.querySelector('#notice file');")
            click("#pick-file")
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
            reply("updateStaged", **dict(staged, icon_key=None), newly_requested_permissions=["storage"])
            assert script("return !!document.querySelector('#review .extension-icon svg');")
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

            # Updates found by a check: a hint on each icon, a direct action, and one review for all of them.
            first = dict(ext, extension_id="first@example.test", name="First extension", update_version="1.1")
            second = dict(ext, extension_id="second@example.test", name="Second extension", icon_key=None)
            second["update_version"] = "3.0"
            current = dict(ext, extension_id="third@example.test", name="Third extension", icon_key=None)
            update_summary = dict(summary, extensions=[first, second, current])
            reply("installedExtensions", **update_summary)
            assert script("return !document.querySelector('#update-controls').hidden;")
            assert script("return document.querySelector('#update-all').textContent;") == "Update All (2)"
            assert script("return document.querySelectorAll('#installed .update-hint').length;") == 2
            first_entry = '[data-extension="first@example.test"]'
            available = script("return document.querySelector(arguments[0] + ' .update-available').textContent;", first_entry)
            assert available == "Version 1.1 is available"
            assert script(
                "const b = document.querySelector(arguments[0] + ' [data-control=update]');"
                "return b.textContent === 'Update to 1.1' && b.classList.contains('primary-button');",
                first_entry,
            )
            third_update = '[data-extension="third@example.test"] [data-control=update]'
            assert script("return document.querySelector(arguments[0]).textContent;", third_update) == "Check for Update"
            assert script("""
                const hint = document.querySelector('#installed .update-hint').getBoundingClientRect();
                const icon = document.querySelector('#installed .extension-icon').getBoundingClientRect();
                return hint.right > icon.right - 4 && hint.top < icon.top + 4 && hint.width === 14;
            """)
            assert script("return document.documentElement.scrollWidth <= innerWidth;")
            script("document.querySelector('#installed-heading').scrollIntoView();")
            screenshot("updates-available")

            click("#check-updates")
            assert sent() == ["checkForUpdates", {}]
            assert script("return document.querySelector('#update-all').ariaDisabled;") == "true"
            reply("updatesChecked", checked=2, failed=["Third extension"], summary=update_summary)
            assert script("return document.querySelector('#notice').textContent;") == (
                "2 updates are available.\nCouldn’t check Third extension."
            )
            assert script("return document.querySelector('#update-all').ariaDisabled;") is None

            click("#update-all")
            assert sent() == ["stageUpdates", {"extension_ids": ["first@example.test", "second@example.test"]}]
            request_count = script("return requests.length;")
            click("#update-all")
            assert script("return requests.length;") == request_count
            staged_first = dict(
                staged, token="first-token", name="First extension", version="1.1", previous_version="1.0"
            )
            staged_first["newly_requested_permissions"] = []
            staged_second = dict(
                staged, token="second-token", name="Second extension", version="3.0", previous_version="1.0"
            )
            staged_second.update(icon_key=None, newly_requested_permissions=["tabs"])
            unverified = dict(staged, token="unverified-token", name="Unverified", verified_signature=False)
            unverified["newly_requested_permissions"] = []
            staged_reply = {
                "updates": [staged_first, staged_second, unverified],
                "errors": [{"name": "Fourth extension", "error": "Download failed"}],
                "summary": update_summary,
            }
            reply("updatesStaged", **staged_reply)
            assert sent() == ["cancelUpdates", {"tokens": ["unverified-token"]}]
            assert script("return document.querySelector('#review').open;")
            assert script("return document.querySelector('#review-title').textContent;") == "Review 2 updates"
            assert script("return document.querySelector('#confirm').textContent;") == "Update All"
            review = script("return document.querySelector('#review').innerText;")
            for value in (
                "Version 1.0 → 1.1",
                "Version 1.0 → 3.0",
                "New permissions",
                "tabs",
                "Fourth extension: Download failed",
                "Unverified: The package could not be verified for installation.",
            ):
                assert value in review, value
            assert script("return document.querySelectorAll('#review .update-list > li').length;") == 2
            assert script(
                "return document.querySelector('#review').scrollWidth <= document.querySelector('#review').clientWidth;"
            )
            screenshot("bulk-review")
            click("#cancel")
            assert sent() == ["cancelUpdates", {"tokens": ["first-token", "second-token"]}]
            assert script("return document.activeElement.id;") == "update-all"
            reply("updatesCancelled")

            click("#update-all")
            reply("updatesStaged", **staged_reply)
            click("#confirm")
            assert sent() == ["approveUpdates", {"tokens": ["first-token", "second-token"], "consented": True}]
            updated_first = dict(first, version="1.1")
            del updated_first["update_version"]
            reply(
                "updatesCompleted",
                updated=["First extension"],
                errors=[{"name": "Second extension", "error": "Installed extension changed. Check for updates again."}],
                summary=dict(summary, extensions=[updated_first, second, current]),
            )
            assert script("return document.querySelector('#notice.error').textContent;") == (
                "Updated First extension.\nSecond extension: Installed extension changed. Check for updates again."
            )
            assert script("return document.querySelector('#update-all').textContent;") == "Update"
            assert script("return document.querySelectorAll('#installed .update-hint').length;") == 1

            reply("installedExtensions", **summary)
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
