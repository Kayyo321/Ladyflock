#!/usr/bin/env python3

# Renders about:privacy in Ladyflock through WebDriver, in a fresh profile, at a normal and the narrowest width and in
# both themes. Checks its layout and its capture controls through the real WebUI channel, and can save screenshots.

import argparse
import base64
import json
import runpy
import subprocess
import tempfile
import time

from pathlib import Path

helpers = runpy.run_path(str(Path(__file__).with_name("test-webdriver-delete-session.py")))


def run_test(webdriver_binary, theme, width, screenshots):
    with tempfile.TemporaryDirectory(prefix="ladyflock-privacy-") as directory:
        config = Path(directory) / "config"
        config.mkdir()
        (config / "Settings.json").write_text(
            json.dumps(
                {
                    "appearance": {"websiteColorScheme": theme, "interfaceTheme": theme},
                    "siteControls": {
                        "scriptingDisabledSites": ["a-site-with-a-rather-long-host-name-that-has-to-wrap.example"]
                    },
                }
            )
        )
        process, port = helpers["launch_webdriver"](
            webdriver_binary,
            "--headless",
            "--force-cpu-painting",
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
                    (screenshots / f"privacy-{theme}-{width}-{state}.png").write_bytes(
                        base64.b64decode(command("GET", "screenshot"), validate=True)
                    )

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

            def wait_for(source, *args, timeout=10):
                deadline = time.monotonic() + timeout
                while time.monotonic() < deadline:
                    if script(source, *args):
                        return
                    time.sleep(0.1)
                raise AssertionError(f"Timed out waiting for: {source}")

            command("POST", "window/rect", {"width": width, "height": 720})
            command("POST", "url", {"url": "about:privacy"})
            wait_for("return document.querySelector('#network').children.length > 0;")

            assert script("return document.title;") == "Privacy"
            assert script("return innerWidth;") == width
            assert script("return matchMedia('(prefers-color-scheme: dark)').matches;") == (theme == "dark")

            network = script("return document.querySelector('#network').innerText;")
            assert "Direct" in network, network
            assert "None" in network, network
            controls = script("return document.querySelector('#controls').innerText;")
            assert "Kept" in controls, controls
            assert "a-site-with-a-rather-long-host-name-that-has-to-wrap.example" in controls, controls

            # The page never scrolls sideways, even with a long host in the controls.
            assert script("return document.documentElement.scrollWidth <= innerWidth;")
            assert script("return [...document.querySelectorAll('.card')].every(card => card.getBoundingClientRect().right <= innerWidth);")
            screenshot("status")

            # Keyboard: the capture controls are reachable in order and show focus.
            script("document.activeElement?.blur();")
            press_key("")
            assert script("return document.activeElement.id;") == "capture-tab", script("return document.activeElement.outerHTML;")
            press_key("")
            assert script("return document.activeElement.id;") == "capture-duration"
            press_key("")
            assert script("return document.activeElement.id;") == "capture-start"
            # The shared focus ring is a box shadow on :focus-visible.
            assert script("return document.activeElement.matches(':focus-visible') && getComputedStyle(document.activeElement).boxShadow !== 'none';")
            assert script("return document.querySelector('#capture-stop').disabled;")

            # Starting a capture of this tab through the real channel, then stopping it.
            press_key("")
            wait_for("return document.querySelector('#capture-state').textContent.startsWith('Capturing');")
            assert not script("return document.querySelector('#capture-stop').disabled;")
            assert script("return document.documentElement.scrollWidth <= innerWidth;")
            screenshot("capturing")
            script("document.querySelector('#capture-stop').click();")
            wait_for("return document.querySelector('#capture-state').textContent === 'Not capturing';")
            assert script("return document.querySelector('#capture-stop').disabled;")
            print(f"PASS: about:privacy status, site controls, layout, keyboard and capture controls ({theme}, {width}px)")
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
