#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladyflock developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import hashlib
import json
import runpy
import tempfile

from pathlib import Path

devtools = runpy.run_path(str(Path(__file__).with_name("test-devtools-client-follows-the-tab.py")))


def catalog_resource_map(root):
    package = root / "sample.xpi"
    package.write_bytes(b"This package has no signature.")
    file_url = "https://addons.mozilla.org/firefox/downloads/file/1/sample.xpi"
    catalog_id = "catalog-fixture"
    listing = {
        "guid": catalog_id,
        "type": "extension",
        "name": "Catalog fixture",
        "slug": "catalog-fixture",
        "current_version": {
            "version": "1.0",
            "file": {
                "status": "public",
                "url": file_url,
                "permissions": ["storage"],
                "hash": "sha256:" + hashlib.sha256(package.read_bytes()).hexdigest(),
            },
        },
    }
    search = root / "search.json"
    search.write_text(json.dumps({"results": [listing], "next": None}))
    detail = root / "detail.json"
    detail.write_text(json.dumps(listing))
    substitutions = [
        ("https://addons.mozilla.org/api/v5/addons/search/", search, "application/json"),
        ("https://addons.mozilla.org/api/v5/addons/addon/" + catalog_id + "/", detail, "application/json"),
        (file_url, package, "application/x-xpinstall"),
    ]
    resource_map = root / "resources.json"
    resource_map.write_text(
        json.dumps(
            {
                "substitutions": [
                    {"url": url, "file": str(path), "content_type": content_type}
                    for url, path, content_type in substitutions
                ]
            }
        )
    )
    return resource_map


def run_test(binary):
    with tempfile.TemporaryDirectory(prefix="ladyflock-addons-controls-") as temporary:
        root = Path(temporary)
        profile = root / "profile"
        extension = root / "fixture"
        extension.mkdir()
        addon_id = "controls@example.test"
        (extension / "manifest.json").write_text(
            json.dumps(
                {
                    "manifest_version": 2,
                    "name": "Profile controls fixture",
                    "version": "1.0",
                    "browser_specific_settings": {"gecko": {"id": addon_id}},
                    "permissions": ["https://example.test/*"],
                }
            )
        )
        process, _, client = devtools["launch"](
            binary,
            str(profile),
            "about:addons",
            f"--load-extension={extension}",
            "--enable-amo-catalog",
            f"--resource-map={catalog_resource_map(root)}",
        )
        try:
            devtools["wait_for_page"](client, "about:addons", "Add-ons")
            client.watch_frame_targets()

            def wait_for(description, expression):
                devtools["wait_until"](description, lambda: client.evaluate(expression) is True)

            def click(selector):
                client.evaluate(
                    """(() => {
                    const control = document.querySelector(%s);
                    control.scrollIntoView({block: "nearest"});
                    const rect = control.getBoundingClientRect();
                    internals.clickThroughUIProcess(rect.x + rect.width / 2, rect.y + rect.height / 2);
                    return true;
                })()"""
                    % json.dumps(selector)
                )

            registry = profile / "data" / "extensions" / "registry.json"

            def entries():
                return json.loads(registry.read_text())["extensions"]

            wait_for("the unpacked fixture", "document.querySelector('[data-control=enabled]')?.checked === true")
            click("[data-control=enabled]")
            wait_for(
                "the browser to disable the extension",
                "document.querySelector('[data-control=enabled]')?.checked === false",
            )
            assert entries()[0]["enabled"] is False
            click("[data-control=enabled]")
            wait_for(
                "the browser to enable the extension",
                "document.querySelector('[data-control=enabled]')?.checked === true",
            )
            click("[data-control=access]")
            wait_for("site settings to open", "document.querySelector('details').open")
            click("[data-control=all-sites]")
            wait_for(
                "the browser to restrict site access",
                "document.querySelector('[data-control=all-sites]')?.checked === false",
            )
            assert entries()[0]["allow_all_sites"] is False
            client.evaluate("document.querySelector('details input[type=text]').value = 'EXAMPLE.TEST'; true;")
            click("[data-control=save-site]")
            wait_for("the allowed host", "document.querySelector('details').innerText.includes('Allowed sites')")
            assert entries()[0]["granted_extra_hosts"] == ["example.test"]
            client.evaluate(
                "document.querySelector('details input[type=text]').value = 'example.test'; document.querySelector('details select').value = 'block'; true;"
            )
            click("[data-control=save-site]")
            wait_for("the blocked host", "document.querySelector('details').innerText.includes('Blocked sites')")
            assert entries()[0]["revoked_hosts"] == ["example.test"]
            click("[data-control=remove]")
            wait_for("the review dialog to open", "document.querySelector('#review').open")
            assert entries()[0]["extension_id"] == addon_id
            click("#cancel")
            wait_for("the dialog to close", "!document.querySelector('#review').open")
            assert entries()[0]["extension_id"] == addon_id
            click("[data-control=remove]")
            wait_for("the review dialog to reopen", "document.querySelector('#review').open")
            click("#confirm")
            wait_for(
                "the empty state",
                "document.querySelector('#installed').textContent.includes('No extensions installed.')",
            )
            assert entries() == []
            assert (extension / "manifest.json").is_file()

            client.evaluate("document.querySelector('#query').value = 'catalog'; true;")
            click("#search")
            wait_for(
                "the local catalog response", "document.querySelector('#results h3')?.textContent.trim() === 'Catalog fixture'"
            )
            assert client.evaluate(
                "document.querySelector('#results').textContent.includes('Compatibility pending package review')"
            )
            click("#results button")
            wait_for(
                "the invalid package to be rejected", "document.querySelector('#notice').classList.contains('error')"
            )
            assert not client.evaluate("document.querySelector('#review').open")
            assert entries() == []
            print("PASS: about:addons profile controls, catalog discovery, and package rejection")
        finally:
            client.close()
            devtools["stop"](process)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("ladybird_binary")
    run_test(parser.parse_args().ladybird_binary)
