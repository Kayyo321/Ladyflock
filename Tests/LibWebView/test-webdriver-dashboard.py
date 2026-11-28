#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladyflock developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import base64
import importlib
import json
import os
import struct
import subprocess
import tempfile
import time
import zlib
from pathlib import Path

webdriver = importlib.import_module("test-webdriver-delete-session")


def command(port, session_id, path, body):
    status, payload, raw = webdriver.request(port, "POST", f"/session/{session_id}/{path}", body)
    assert status == 200, raw
    return payload["value"]


def wait_for(port, session_id, predicate_js, timeout=15.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        val = command(port, session_id, "execute/sync", {"script": predicate_js, "args": []})
        if val:
            return val
        time.sleep(0.1)
    raise AssertionError(f"Timed out waiting for condition: {predicate_js}")


def pointer(port, session_id, actions):
    status, _payload, raw = webdriver.request(
        port,
        "POST",
        f"/session/{session_id}/actions",
        {"actions": [{"type": "pointer", "id": "mouse", "parameters": {"pointerType": "mouse"}, "actions": actions}]},
    )
    assert status == 200, raw


PLACEMENTS = """
return [...document.querySelectorAll('.dash-card')].map(c => ({
    type: c.dataset.widgetType,
    col: Number(c.dataset.col), row: Number(c.dataset.row), w: Number(c.dataset.w), h: Number(c.dataset.h),
}));
"""

# The number of dots the pegboard canvas is currently showing: 0 when it is blank, more once it has drawn.
DOT_PIXELS = """
const canvas = document.querySelector('#dashboard-dots');
if (!canvas.width || !canvas.height) return 0;
const data = canvas.getContext('2d').getImageData(0, 0, canvas.width, canvas.height).data;
let painted = 0;
for (let i = 3; i < data.length; i += 4) if (data[i] > 8) ++painted;
return painted;
"""

DOTS_COVER_VIEWPORT = """
const canvas = document.querySelector('#dashboard-dots');
const rect = canvas.getBoundingClientRect();
if (!canvas.width || !canvas.height || !rect.height) return false;
const scale = canvas.height / rect.height;
const pixels = canvas.getContext('2d').getImageData(0, 0, canvas.width, canvas.height).data;
const painted = [0, 0, 0];
for (let i = 3; i < pixels.length; i += 4) {
    if (pixels[i] <= 8) continue;
    const y = rect.top + Math.floor(i / (4 * canvas.width)) / scale;
    const band = Math.floor(y * painted.length / document.documentElement.clientHeight);
    if (band >= 0 && band < painted.length) ++painted[band];
}
return painted.every(count => count > 0);
"""


def assert_no_overlap(placements):
    for i, a in enumerate(placements):
        for b in placements[i + 1:]:
            apart = (
                a["col"] + a["w"] <= b["col"]
                or b["col"] + b["w"] <= a["col"]
                or a["row"] + a["h"] <= b["row"]
                or b["row"] + b["h"] <= a["row"]
            )
            assert apart, (a, b)


def test_headless_fallback_and_settings(webdriver_binary):
    """On headless frontend (no dashboard factory), Settings omits Dashboard and about:dashboard shows a clean fallback."""
    with tempfile.TemporaryDirectory() as directory:
        process, port = webdriver.launch_webdriver(webdriver_binary, "--headless", "--profile-path", directory)
        session = None
        try:
            session = webdriver.create_session(port)

            # 1. Settings must omit Dashboard option when frontend has no dashboard factory.
            command(port, session, "url", {"url": "about:settings#browsing"})
            wait_for(port, session, "return document.querySelector('#new-tab-page-mode') !== null;")
            dashboard_opt_disabled = wait_for(
                port,
                session,
                """
                const opt = document.querySelector('#new-tab-page-mode-dashboard');
                return opt && (opt.hidden || opt.disabled);
                """,
            )
            assert dashboard_opt_disabled, "Headless frontend must hide/disable Dashboard choice in Settings"
            # A new profile starts on the dashboard, which this frontend can't show, so Settings presents Blank.
            headless_mode = wait_for(port, session, "return document.querySelector('#new-tab-page-mode').value;")
            assert headless_mode == "blank", headless_mode

            # 2. Direct visit to about:dashboard on headless frontend shows intelligible fallback.
            command(port, session, "url", {"url": "about:dashboard"})
            fallback_visible = wait_for(
                port,
                session,
                """
                const fb = document.querySelector('#dashboard-fallback');
                return fb && !fb.classList.contains('hidden');
                """,
                timeout=8.0,
            )
            assert fallback_visible, "Headless about:dashboard must display fallback card"
            print("PASS: Headless Settings omission and about:dashboard fallback verified")
        finally:
            if session:
                webdriver.request(port, "DELETE", f"/session/{session}")
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


def test_graphical_dashboard_end_to_end(webdriver_binary):
    """On the Qt frontend, about:dashboard renders the starter tiles, lets a tile be dragged onto the dot grid and a widget be added from the shelf, and persists both across reload."""
    with tempfile.TemporaryDirectory() as directory:
        env = os.environ.copy()
        if os.uname().sysname != "Darwin":
            env.setdefault("QT_QPA_PLATFORM", "offscreen")
        process, port = webdriver.launch_webdriver(webdriver_binary, "--profile-path", directory, env=env)
        session = None
        try:
            status, payload, raw = webdriver.request(
                port, "POST", "/session", {"capabilities": {"alwaysMatch": {"ladybird:headless": False}}}
            )
            assert status == 200, raw
            session = payload["value"]["sessionId"]

            command(port, session, "window/rect", {"width": 1240, "height": 864})

            # 1. Verify Settings offers Dashboard on Qt, and that a new profile opens new tabs on it.
            command(port, session, "url", {"url": "about:settings#browsing"})
            mode_state = wait_for(
                port,
                session,
                """
                const sel = document.querySelector('#new-tab-page-mode');
                const opt = document.querySelector('#new-tab-page-mode-dashboard');
                const urlInput = document.querySelector('#new-tab-page-url');
                if (!sel || !opt || opt.disabled || opt.hidden || !urlInput || !urlInput.value) return false;
                return { mode: sel.value, url: urlInput.value };
                """,
            )
            assert mode_state["mode"] == "dashboard", mode_state
            assert mode_state["url"] == "about:dashboard", mode_state

            # 2. Open about:dashboard and verify starter 3 cards (World Clock, Quick Tasks, Scratchpad).
            command(port, session, "url", {"url": "about:dashboard"})
            starter_types = wait_for(
                port,
                session,
                """
                const cards = [...document.querySelectorAll('.dash-card')];
                if (cards.length !== 3) return false;
                return cards.map(c => c.dataset.widgetType);
                """,
            )
            assert starter_types == ["world-clock", "quick-tasks", "scratchpad"], starter_types

            # The page must not take focus on load: a new tab keeps it in the address field.
            focused = command(
                port, session, "execute/sync", {"script": "return document.activeElement === document.body;", "args": []}
            )
            assert focused, "about:dashboard must not move focus into the page on load"

            # Outside edit mode the pegboard is out of sight and tile content is live.
            assert command(port, session, "execute/sync", {"script": DOT_PIXELS, "args": []}) == 0
            assert not command(
                port, session, "execute/sync", {"script": "return document.querySelector('.dash-card-face').inert;", "args": []}
            )
            normal_layout = command(
                port,
                session,
                "execute/sync",
                {
                    "script": """
                    const board = document.querySelector('#dashboard-board').getBoundingClientRect();
                    const customize = document.querySelector('#customize-wrap').getBoundingClientRect();
                    const viewport = document.documentElement;
                    return {
                        fillsViewport: board.left === 0 && board.top === 0
                            && board.width === viewport.clientWidth && board.height === viewport.clientHeight,
                        hasHeading: document.querySelector('.dashboard-heading') !== null,
                        customizeInCorner: Math.abs(viewport.clientWidth - customize.right - 18) < 1
                            && Math.abs(viewport.clientHeight - customize.bottom - 18) < 1,
                        menuVisible: [...document.querySelectorAll('.dash-card-menu-btn')]
                            .some(button => button.getBoundingClientRect().height > 0),
                    };
                    """,
                    "args": [],
                },
            )
            assert normal_layout == {
                "fillsViewport": True, "hasHeading": False, "customizeInCorner": True, "menuVisible": False,
            }, normal_layout

            # 3. Enter Customize mode and test fine resizing and center guides.
            command(port, session, "execute/sync", {"script": "document.querySelector('#btn-customize').click();", "args": []})
            wait_for(port, session, DOTS_COVER_VIEWPORT)
            assert command(port, session, "execute/sync", {
                "script": "return document.querySelector('.dash-card-menu-btn').getBoundingClientRect().height > 0;", "args": [],
            })

            before = command(port, session, "execute/sync", {"script": PLACEMENTS, "args": []})
            clock = next(p for p in before if p["type"] == "world-clock")
            assert (clock["col"], clock["row"]) == (0, -clock["h"] // 2), before
            geometry = command(
                port,
                session,
                "execute/sync",
                {
                    "script": """
                    const clock = document.querySelector(".dash-card[data-widget-type='world-clock']");
                    const tile = clock.getBoundingClientRect();
                    const resize = document.querySelector(".dash-card[data-widget-type='scratchpad'] .dash-card-resize").getBoundingClientRect();
                    const gap = parseFloat(document.querySelector('#dashboard-board').style.getPropertyValue('--tile-gap'));
                    return {
                        x: tile.left + tile.width / 2,
                        y: tile.top + tile.height / 2,
                        viewportCenterX: document.documentElement.clientWidth / 2,
                        viewportCenterY: document.documentElement.clientHeight / 2,
                        unitX: (tile.width + gap) / Number(clock.dataset.w),
                        height: tile.height,
                        gap,
                        resizeX: resize.left + resize.width / 2,
                        resizeY: resize.top + resize.height / 2,
                    };
                    """,
                    "args": [],
                },
            )
            assert abs(geometry["y"] - geometry["viewportCenterY"]) < 0.1, geometry
            # Shrink by two fine steps while preserving the vertical center shared by the other cards.
            scratchpad = next(p for p in before if p["type"] == "scratchpad")
            resize_x, resize_y = round(geometry["resizeX"]), round(geometry["resizeY"])
            pointer(
                port,
                session,
                [
                    {"type": "pointerMove", "origin": "viewport", "x": resize_x, "y": resize_y},
                    {"type": "pointerDown", "button": 0},
                    {
                        "type": "pointerMove",
                        "origin": "viewport",
                        "x": round(resize_x - geometry["unitX"] * 2),
                        "y": resize_y,
                        "duration": 100,
                    },
                ],
            )
            wait_for(
                port,
                session,
                """
                return document.querySelector(".dash-card[data-widget-type='scratchpad']").classList.contains('is-resizing')
                    && !document.querySelector('.dash-guide-horizontal').hidden;
                """,
            )
            pointer(port, session, [{"type": "pointerUp", "button": 0}])
            wait_for(
                port,
                session,
                f"""
                return document.querySelector(".dash-card[data-widget-type='scratchpad']").dataset.w === '{scratchpad["w"] - 2}'
                    && document.querySelector('.dash-guide-horizontal').hidden;
                """,
            )

            # Carry the clock onto the viewport center before dropping one fine column and four fine rows away.
            unit_y = (geometry["height"] + geometry["gap"]) / clock["h"]
            start_x, start_y = round(geometry["x"]), round(geometry["y"])
            end_x = round(geometry["x"] + geometry["unitX"] * 1.2)
            end_y = round(geometry["y"] + unit_y * 4.1)
            pointer(
                port,
                session,
                [
                    {"type": "pointerMove", "origin": "viewport", "x": start_x, "y": start_y},
                    {"type": "pointerDown", "button": 0},
                    {"type": "pointerMove", "origin": "viewport", "x": start_x + 12, "y": start_y + 12, "duration": 50},
                    {
                        "type": "pointerMove",
                        "origin": "viewport",
                        "x": round(geometry["viewportCenterX"]),
                        "y": round(geometry["viewportCenterY"]),
                        "duration": 100,
                    },
                ],
            )
            wait_for(
                port,
                session,
                """
                const guides = [...document.querySelectorAll('.dash-guide')];
                if (guides.length !== 2 || guides.some(guide => guide.hidden || !guide.classList.contains('is-viewport')))
                    return false;
                const rect = document.querySelector('#dashboard-landing').getBoundingClientRect();
                return Math.abs(rect.left + rect.width / 2 - document.documentElement.clientWidth / 2) < 0.1
                    && Math.abs(rect.top + rect.height / 2 - document.documentElement.clientHeight / 2) < 0.1;
                """,
            )
            assert command(port, session, "execute/sync", {"script": DOTS_COVER_VIEWPORT, "args": []})
            pointer(
                port,
                session,
                [
                    {"type": "pointerMove", "origin": "viewport", "x": end_x, "y": end_y, "duration": 100},
                    {"type": "pointerUp", "button": 0},
                ],
            )
            moved = wait_for(
                port,
                session,
                f"""
                const c = document.querySelector(".dash-card[data-widget-type='world-clock']");
                const guidesHidden = [...document.querySelectorAll('.dash-guide')].every(guide => guide.hidden);
                return c.dataset.row === '{clock["row"] + 4}' && guidesHidden && !c.classList.contains('is-dragging')
                    ? [Number(c.dataset.col), Number(c.dataset.row)]
                    : false;
                """,
            )
            assert moved == [1, clock["row"] + 4], moved
            assert_no_overlap(command(port, session, "execute/sync", {"script": PLACEMENTS, "args": []}))

            # Add Calculator from the shelf, verify 4 cards and zero overlaps.
            command(port, session, "execute/sync", {"script": "document.querySelector('#btn-add-widget').click();", "args": []})
            wait_for(
                port,
                session,
                """
                const btns = [...document.querySelectorAll('#picker-body .picker-item')];
                const calcItem = btns.find(el => el.textContent.includes('Calculator and Units'));
                if (!calcItem) return false;
                calcItem.querySelector('button').click();
                return true;
                """,
            )
            types_after_add = wait_for(
                port,
                session,
                """
                const cards = [...document.querySelectorAll('.dash-card')];
                if (cards.length !== 4) return false;
                return cards.map(c => c.dataset.widgetType);
                """,
            )
            assert "calculator" in types_after_add, types_after_add
            assert_no_overlap(command(port, session, "execute/sync", {"script": PLACEMENTS, "args": []}))

            # 4. Reload and confirm persistence (4 cards survive reload).
            command(port, session, "refresh", {})
            types_after_reload = wait_for(
                port,
                session,
                """
                const cards = [...document.querySelectorAll('.dash-card')];
                if (cards.length !== 4) return false;
                return cards.map(c => c.dataset.widgetType);
                """,
            )
            assert "calculator" in types_after_reload, types_after_reload
            after_reload = command(port, session, "execute/sync", {"script": PLACEMENTS, "args": []})
            clock = next(p for p in after_reload if p["type"] == "world-clock")
            assert [clock["col"], clock["row"]] == moved, after_reload
            resized = next(p for p in after_reload if p["type"] == "scratchpad")
            assert resized["w"] == scratchpad["w"] - 2, after_reload
            assert_no_overlap(after_reload)

            # 5. Apply a preset through its confirmation, then open a widget's Settings. The dialog has
            #    to be laid out on screen: it was once left open but invisible after a preset.
            command(
                port,
                session,
                "execute/sync",
                {
                    "script": """
                    document.querySelector('#btn-customize').click();
                    document.querySelector('#btn-presets').click();
                    """,
                    "args": [],
                },
            )
            wait_for(
                port,
                session,
                """
                const preset = [...document.querySelectorAll('#picker-body .picker-preset')]
                    .find(el => el.querySelector('.picker-item-title').textContent === 'Focus');
                if (!preset) return false;
                preset.querySelector('button').click();
                return true;
                """,
            )
            wait_for(port, session, "return document.querySelector('#confirm-dialog').open;")
            command(
                port,
                session,
                "execute/sync",
                {"script": "document.querySelector('#confirm-dialog-accept').click();", "args": []},
            )
            wait_for(
                port,
                session,
                """
                const cards = [...document.querySelectorAll('.dash-card:not(.is-leaving)')];
                return cards.length === 6 && cards.some(c => c.dataset.widgetType === 'focus-timer');
                """,
            )
            confirm_height = command(
                port,
                session,
                "execute/sync",
                {
                    "script": """
                    document.querySelector('#picker-close').click();
                    return document.querySelector('#confirm-dialog').getBoundingClientRect().height;
                    """,
                    "args": [],
                },
            )
            assert confirm_height == 0, confirm_height
            wait_for(
                port,
                session,
                """
                if (!document.documentElement.classList.contains('is-editing')) return false;
                const settings = [...document.querySelectorAll('.dash-card-menu .dash-menu-item')]
                    .find(item => item.textContent.includes('Settings'));
                if (settings) {
                    settings.click();
                    return true;
                }
                const menuBtn = document.querySelector(".dash-card[data-widget-type='world-clock'] .dash-card-menu-btn");
                if (menuBtn && menuBtn.getAttribute('aria-expanded') !== 'true') menuBtn.click();
                return false;
                """,
            )
            settings_dialog = wait_for(
                port,
                session,
                """
                const dialog = document.querySelector('#config-dialog');
                if (!dialog.open) return false;
                const rect = dialog.getBoundingClientRect();
                return { width: rect.width, height: rect.height };
                """,
            )
            assert settings_dialog["width"] > 200 and settings_dialog["height"] > 100, settings_dialog

            # The first widget on an empty dashboard belongs at the actual center.
            command(port, session, "execute/sync", {
                "script": """
                document.querySelector('#config-dialog [data-close-dialog]').click();
                document.querySelector('#btn-presets').click();
                """,
                "args": [],
            })
            wait_for(port, session, """
                const empty = [...document.querySelectorAll('.picker-preset .picker-item-title')]
                    .find(title => title.textContent === 'Empty');
                if (!empty) return false;
                empty.closest('.picker-preset').querySelector('button').click();
                return true;
            """)
            wait_for(port, session, "return document.querySelector('#confirm-dialog').open;")
            command(port, session, "execute/sync", {
                "script": "document.querySelector('#confirm-dialog-accept').click();", "args": [],
            })
            wait_for(port, session, "return !document.querySelector('.dash-card');")
            command(port, session, "execute/sync", {
                "script": "document.querySelector('#btn-add-widget').click();", "args": [],
            })
            wait_for(port, session, """
                const search = [...document.querySelectorAll('.picker-item')]
                    .find(item => item.querySelector('.picker-item-title').textContent === 'Search Box');
                if (!search) return false;
                search.querySelector('button').click();
                return true;
            """)
            wait_for(port, session, "return !!document.querySelector('.dash-card[data-widget-type=search-box]');")
            command(port, session, "execute/sync", {
                "script": "document.querySelector('#picker-close').click(); document.querySelector('#btn-done-edit').click();",
                "args": [],
            })
            wait_for(port, session, """
                const tile = document.querySelector('.dash-card[data-widget-type=search-box]').getBoundingClientRect();
                const viewport = document.documentElement;
                return Math.abs(tile.left + tile.width / 2 - viewport.clientWidth / 2) < 0.1
                    && Math.abs(tile.top + tile.height / 2 - viewport.clientHeight / 2) < 0.1;
            """)
            print(
                "PASS: Graphical Qt full-viewport dashboard, edit-only menus, dot coverage, fine drag and resize, "
                "center guides, shelf add, reload persistence, widget settings and centered Search Box verified"
            )
        finally:
            if session:
                webdriver.request(port, "DELETE", f"/session/{session}")
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


def test_graphical_image_widget(webdriver_binary):
    def png(width, height, pixel):
        def chunk(kind, data):
            return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

        header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
        rows = [b"\0" + b"".join(bytes(pixel(x, y)) for x in range(width)) for y in range(height)]
        scanlines = b"".join(rows)
        return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(scanlines)) + chunk(b"IEND", b"")

    def data_url(image):
        return "data:image/png;base64," + base64.b64encode(image).decode("ascii")

    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        local_file = root / "local image.png"
        url_file = root / "remote-image.png"
        bad_file = root / "malformed.png"
        # The two brightness bands must produce different font intensities in ASCII mode.
        local_image = png(40, 20, lambda x, y: (10, 10, 10, 255) if x < 14 else (230, 91, 45, 255))
        # The transparent top and translucent vertical stripe expose stretching, crop offsets and lost alpha.
        url_image = png(20, 40, lambda x, y: (45, 149, 230, 0 if y < 8 else 96 if 4 <= x < 8 else 255))
        local_file.write_bytes(local_image)
        url_file.write_bytes(url_image)
        bad_file.write_bytes(b"\x89PNG\r\n\x1a\nnot an image")
        image_url = "https://images.example/dashboard.png"
        resource_map = root / "resources.json"
        resource_map.write_text(json.dumps({"substitutions": [
            {"url": image_url, "file": str(url_file), "content_type": "image/png"},
        ]}), encoding="utf-8")
        profile = root / "profile"
        profile.mkdir()
        env = os.environ.copy()
        if os.uname().sysname != "Darwin":
            env.setdefault("QT_QPA_PLATFORM", "offscreen")
        process, port = webdriver.launch_webdriver(
            webdriver_binary, "--profile-path", str(profile), "--resource-map", str(resource_map), env=env,
        )
        session = None
        try:
            status, payload, raw = webdriver.request(
                port, "POST", "/session", {"capabilities": {"alwaysMatch": {"ladybird:headless": False}}},
            )
            assert status == 200, raw
            session = payload["value"]["sessionId"]

            def execute(script):
                return command(port, session, "execute/sync", {"script": script, "args": []})

            def wait(script):
                return wait_for(port, session, script)

            def interact(selector, action, body):
                element = command(port, session, "element", {"using": "css selector", "value": selector})
                element_id = element["element-6066-11e4-a52e-4f735466cecf"]
                return command(port, session, f"element/{element_id}/{action}", body)

            def press_key(key):
                command(port, session, "actions", {"actions": [{
                    "type": "key", "id": "keyboard",
                    "actions": [{"type": "keyDown", "value": key}, {"type": "keyUp", "value": key}],
                }]})

            def open_advanced(keyboard=False):
                assert execute("""
                    const advanced = document.querySelector('.image-advanced-settings');
                    return !advanced.open && advanced.querySelector('summary').textContent === 'Advanced Settings…'
                        && !document.querySelector('#image-shader').getBoundingClientRect().width
                        && document.querySelector('.image-resize-mode').getBoundingClientRect().width > 0;
                """)
                if keyboard:
                    execute("document.querySelector('.image-advanced-settings > summary').focus();")
                    press_key("\ue007")  # Enter
                else:
                    interact(".image-advanced-settings > summary", "click", {})
                wait("return document.querySelector('#image-shader').getBoundingClientRect().width > 0;")

            def add_image_widget():
                execute("document.querySelector('#btn-add-widget').click();")
                wait("""
                    const add = document.querySelector('#picker-body [data-widget-type="image"] .picker-add');
                    if (!add) return false;
                    add.click();
                    return true;
                """)
                wait("return document.querySelector('#config-dialog').open && !!document.querySelector('#image-file');")

            def open_image_settings(selector=".dash-card[data-widget-type=image]"):
                execute(f"""
                    const card = document.querySelector({json.dumps(selector)});
                    card.querySelector('.dash-card-menu-btn').click();
                    [...document.querySelectorAll('.dash-menu-item')]
                        .find(item => item.textContent.includes('Settings')).click();
                """)
                wait("return document.querySelector('#config-dialog').open && !!document.querySelector('#image-file');")

            def wait_for_image(expected, width):
                wait(f"""
                    const image = document.querySelector('.dashboard-image');
                    return image?.naturalWidth === {width} && image.getAttribute('src') === {json.dumps(data_url(expected))};
                """)

            command(port, session, "url", {"url": "about:dashboard"})
            wait("return document.documentElement.classList.contains('is-ready');")
            execute("document.querySelector('#btn-customize').click();")
            add_image_widget()
            open_advanced(keyboard=True)
            wait("return document.querySelector('#image-shader')?.value === 'default';")
            assert execute("""return document.querySelector('.image-shader-mode [data-mode=blit]')
                .getAttribute('aria-pressed') === 'true';""")
            assert not execute("return !!document.querySelector('#image-ascii-density')?.getBoundingClientRect().width;")
            interact("#image-file", "value", {"text": str(local_file)})
            wait_for_image(local_image, 40)
            wait("return !document.querySelector('#config-dialog').open;")
            local_file.unlink()
            command(port, session, "refresh", {})
            wait_for_image(local_image, 40)

            assert execute("""
                const image = document.querySelector('.dashboard-image');
                const face = image.closest('.dash-card-face');
                const style = getComputedStyle(face);
                return getComputedStyle(image).objectFit === 'contain' && !face.querySelector('.dash-card-head')
                    && style.boxShadow === 'none' && style.backgroundColor === 'rgba(0, 0, 0, 0)';
            """)

            execute("document.querySelector('#btn-customize').click();")
            open_image_settings()
            interact("#image-file", "value", {"text": str(bad_file)})
            wait("return !!document.querySelector('#config-dialog .form-error:not(.hidden)');")
            wait_for_image(local_image, 40)
            interact(".image-resize-mode [data-mode='crop']", "click", {})
            wait("return getComputedStyle(document.querySelector('.dashboard-image')).objectFit === 'cover';")
            interact("#image-url", "value", {"text": image_url})
            interact(".image-import-url", "click", {})
            wait_for_image(url_image, 20)
            wait("return !document.querySelector('#config-dialog').open;")
            url_file.unlink()
            command(port, session, "refresh", {})
            wait_for_image(url_image, 20)
            assert execute("return getComputedStyle(document.querySelector('.dashboard-image')).objectFit === 'cover';")

            # Upload a shader against the blue URL image, then reuse its saved copy on the orange local image.
            execute("document.querySelector('#btn-customize').click();")
            open_image_settings()
            shader_file = root / "Invert.frag"
            open_advanced()
            shader_file.write_text("vec4 shade(vec2 uv, vec4 color, float time) { return vec4(1.0 - color.rgb, color.a); }")
            interact("#image-shader-file", "value", {"text": str(shader_file)})
            shader_id = wait("""
                const id = document.querySelector('#image-shader').value;
                return id && id !== 'default' && id !== 'breeze' && id;
            """)
            shader_file.unlink()
            first_id = execute("return document.querySelector('.dash-card[data-widget-type=image]').dataset.instanceId;")
            first = f".dash-card[data-instance-id='{first_id}']"

            def check_blit(card, expected):
                wait(f"""
                    const canvas = document.querySelector({json.dumps(card + " .dashboard-image-shader")});
                    if (!canvas || !canvas.getBoundingClientRect().width || !canvas.width || !canvas.height) return false;
                    const rgb = canvas.getContext('2d').getImageData(canvas.width >> 1, canvas.height >> 1, 1, 1).data;
                    return rgb[3] === 255 && {json.dumps(expected)}.every((v, i) => Math.abs(rgb[i] - v) <= 2);
                """)

            def check_resize_mode(mode):
                interact(f".image-resize-mode [data-mode='{mode}']", "click", {})
                wait(f"""
                    const c = document.querySelector({json.dumps(first + " .dashboard-image-shader")});
                    const img = c.parentElement.querySelector('img');
                    if (img.dataset.mode !== {json.dumps(mode)} || !c.width || !c.height) return false;
                    const reference = document.createElement('canvas');
                    reference.width = c.width;
                    reference.height = c.height;
                    const ctx = reference.getContext('2d');
                    const scale = Math.{"min" if mode == "resize" else "max"}(c.width / img.naturalWidth, c.height / img.naturalHeight);
                    const w = img.naturalWidth * scale, h = img.naturalHeight * scale;
                    ctx.drawImage(img, (c.width - w) / 2, (c.height - h) / 2, w, h);
                    const expected = ctx.getImageData(0, 0, c.width, c.height).data;
                    const actual = c.getContext('2d').getImageData(0, 0, c.width, c.height).data;
                    const alphas = new Set();
                    for (let y = 1; y < 16; ++y) {{
                        for (let x = 1; x < 16; ++x) {{
                            const i = 4 * (Math.floor(y * c.height / 16) * c.width + Math.floor(x * c.width / 16)) + 3;
                            if (![0, 96, 255].includes(expected[i])) continue;
                            if (Math.abs(actual[i] - expected[i]) > 3) return false;
                            if (expected[i] === 96 && [210, 106, 25].some((v, k) => Math.abs(actual[i - 3 + k] - v) > 6))
                                return false;
                            alphas.add(expected[i]);
                        }}
                    }}
                    return alphas.has(96) && alphas.has(255);
                """)

            check_blit(first, [210, 106, 25])
            check_resize_mode("resize")
            check_resize_mode("crop")
            shader_file.write_text("vec4 shade(vec2 uv, vec4 color, float time) { return ; }")
            interact("#image-shader-file", "value", {"text": str(shader_file)})
            wait("return !!document.querySelector('#config-dialog .form-error:not(.hidden)');")
            assert execute("return document.querySelector('#image-shader').value;") == shader_id
            check_blit(first, [210, 106, 25])
            interact("#config-dialog [data-close-dialog]", "click", {})
            add_image_widget()
            open_advanced()
            assert execute("return document.querySelector('#image-shader').value;") == "default"
            local_file.write_bytes(local_image)
            interact("#image-file", "value", {"text": str(local_file)})
            wait("return !document.querySelector('#config-dialog').open && document.querySelectorAll('.dashboard-image').length === 2;")
            second_id = execute(f"""return [...document.querySelectorAll('.dash-card[data-widget-type=image]')]
                .find(c => c.dataset.instanceId !== {json.dumps(first_id)}).dataset.instanceId;""")
            second = f".dash-card[data-instance-id='{second_id}']"
            open_image_settings(second)
            open_advanced()
            wait(f"""return [...document.querySelector('#image-shader').options].some(o => o.value === {json.dumps(shader_id)});""")
            # Clicking select options through WebDriver fires the same input/change events as a user selection.
            interact(f"#image-shader option[value='{shader_id}']", "click", {})
            check_blit(second, [25, 164, 210])
            interact(".image-shader-mode [data-mode=ascii]", "value", {"text": " "})
            wait("""return document.querySelector('.image-shader-mode [data-mode=ascii]').getAttribute('aria-pressed') === 'true';""")

            def check_ascii(inverted):
                wait(f"""
                    const c = document.querySelector({json.dumps(second + " .dashboard-image-shader")});
                    if (!c?.getBoundingClientRect().width || c.width < 4 || c.height < 4) return false;
                    const fittedWidth = Math.min(c.width, 2 * c.height);
                    function strokeBrightness(fraction) {{
                        const x = (c.width - fittedWidth) / 2 + fittedWidth * fraction;
                        const pixels = c.getContext('2d').getImageData(x, c.height / 2, fittedWidth / 8, 18).data;
                        let min = 255, max = 0;
                        for (let i = 0; i < pixels.length; i += 4) {{
                            const light = pixels[i];
                            if (pixels[i + 1] !== light || pixels[i + 2] !== light) return null;
                            min = Math.min(min, light);
                            max = Math.max(max, light);
                        }}
                        return max - min > 4 ? max : null;
                    }}
                    const left = strokeBrightness(0.1), right = strokeBrightness(0.6);
                    return left !== null && right !== null && {"left - right" if inverted else "right - left"} > 16;
                """)

            check_ascii(inverted=True)
            assert execute("""
                const range = document.querySelector('#image-ascii-density');
                return [range.type, range.min, range.max, range.step, range.value];
            """) == ["range", "80", "560", "10", "330"]
            # Count the glyph strokes along a row within a single flat tone. This verifies the rendered
            # resolution changes, even if the slider value and its readout update ahead of the draw.
            glyph_runs = f"""(() => {{
                const c = document.querySelector({json.dumps(second + " .dashboard-image-shader")});
                const w = Math.min(c.width, 2 * c.height);
                const x = Math.floor((c.width - w) / 2 + 0.55 * w), y = Math.floor(c.height / 2);
                const area = c.getContext('2d').getImageData(x, y, Math.floor(w / 3), 32);
                let most = 0;
                for (let row = 0; row < area.height; ++row) {{
                    let runs = 0, inStroke = false;
                    for (let col = 0; col < area.width; ++col) {{
                        const lit = area.data[4 * (row * area.width + col)] > 50;
                        if (lit && !inStroke) ++runs;
                        inStroke = lit;
                    }}
                    most = Math.max(most, runs);
                }}
                return most;
            }})()"""
            initial_runs = execute(f"return {glyph_runs};")
            assert initial_runs > 0
            def step_density(key, values):
                execute("document.querySelector('#image-ascii-density').focus();")
                for value in values:
                    press_key(key)
                    wait(f"""const range = document.querySelector('#image-ascii-density');
                        const output = document.querySelector('output[for=image-ascii-density]');
                        return range.value === '{value}' && !range.disabled && output.value === '{value} chars/in²';""")

            step_density("\ue00f", [230, 130, 80])  # Page Down
            coarse_runs = wait(f"const runs = {glyph_runs}; return runs > 0 && runs < {0.8 * initial_runs} && runs;")
            step_density("\ue00e", [180, 280, 380, 480, 560])  # Page Up
            wait(f"return {glyph_runs} > {1.6 * coarse_runs};")
            command(port, session, "refresh", {})
            check_blit(first, [210, 106, 25])
            check_ascii(inverted=True)
            execute("document.querySelector('#btn-customize').click();")
            open_image_settings(second)
            open_advanced()
            assert execute("return document.querySelector('#image-shader').value;") == shader_id
            assert execute("""return document.querySelector('.image-shader-mode [data-mode=ascii]')
                .getAttribute('aria-pressed') === 'true';""")
            assert execute("return document.querySelector('#image-ascii-density').value;") == "560"
            interact(".image-remove-shader", "click", {})
            interact(".image-confirm-remove", "click", {})
            wait("return document.querySelector('#image-shader').value === 'default';")
            check_ascii(inverted=False)
            wait(f"""const img = document.querySelector({json.dumps(first + " .dashboard-image")});
                return img?.getBoundingClientRect().width && !document.querySelector({json.dumps(first + " .dashboard-image-shader")})?.getBoundingClientRect().width;""")
            interact("#image-shader option[value=breeze]", "click", {})
            wait("return document.querySelector('#image-shader').value === 'breeze' && !document.querySelector('#image-shader').disabled;")
            interact("#config-dialog [data-close-dialog]", "click", {})
            open_image_settings(first)
            open_advanced()
            assert execute("return document.querySelector('#image-shader').value;") == "default"
            assert not execute(f"""return [...document.querySelector('#image-shader').options].some(o => o.value === {json.dumps(shader_id)});""")
            interact(".image-shader-mode [data-mode=ascii]", "click", {})
            wait("return document.querySelector('#image-ascii-density')?.getBoundingClientRect().width;")
            assert execute("return document.querySelector('#image-ascii-density').value;") == "330"
            interact(".image-remove", "click", {})
            wait(f"return !!document.querySelector({json.dumps(first + ' .image-add')});")
            command(port, session, "refresh", {})
            wait(f"return !!document.querySelector({json.dumps(first + ' .image-add')});")
            assert not execute(f"return !!document.querySelector({json.dumps(first + ' .dashboard-image')});")
            check_ascii(inverted=False)
            print("PASS: Qt image import, fit/crop, shared shaders, Blit RGB, ASCII brightness and density, reload and removal verified")
        finally:
            if session:
                webdriver.request(port, "DELETE", f"/session/{session}")
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("webdriver_binary")
    parser.add_argument("--graphical", action="store_true")
    args = parser.parse_args()
    test_headless_fallback_and_settings(args.webdriver_binary)
    if args.graphical:
        test_graphical_dashboard_end_to_end(args.webdriver_binary)
        test_graphical_image_widget(args.webdriver_binary)


if __name__ == "__main__":
    main()
