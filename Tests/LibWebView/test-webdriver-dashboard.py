#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladyflock developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import importlib
import json
import os
import subprocess
import tempfile
import time
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

            # 1. Verify Settings offers Dashboard on Qt and preserves Blank as default.
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
            assert mode_state["mode"] == "blank", mode_state
            assert mode_state["url"] == "about:newtab", mode_state

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

            # 3. Enter Customize mode: the dot field appears, and a tile dragged by its face snaps to it.
            command(port, session, "execute/sync", {"script": "document.querySelector('#btn-customize').click();", "args": []})
            wait_for(port, session, DOT_PIXELS)

            before = command(port, session, "execute/sync", {"script": PLACEMENTS, "args": []})
            clock = next(p for p in before if p["type"] == "world-clock")
            assert (clock["col"], clock["row"]) == (0, 0), before
            geometry = command(
                port,
                session,
                "execute/sync",
                {
                    "script": """
                    const tile = document.querySelector(".dash-card[data-widget-type='world-clock']").getBoundingClientRect();
                    const tasks = document.querySelector(".dash-card[data-widget-type='quick-tasks']").getBoundingClientRect();
                    const columns = Number(document.querySelector(".dash-card[data-widget-type='quick-tasks']").dataset.col);
                    return { x: tile.left + tile.width / 2, y: tile.top + tile.height / 2, unitX: (tasks.left - tile.left) / columns, height: tile.height };
                    """,
                    "args": [],
                },
            )
            # Carry it four rows down and a little over one column across: it lands on the nearest peg.
            unit_y = (geometry["height"] + 14) / clock["h"]
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
                    {"type": "pointerMove", "origin": "viewport", "x": end_x, "y": end_y, "duration": 100},
                ],
            )
            lifted = wait_for(
                port,
                session,
                "return document.querySelector(\".dash-card[data-widget-type='world-clock']\").classList.contains('is-dragging');",
            )
            assert lifted
            pointer(port, session, [{"type": "pointerUp", "button": 0}])
            moved = wait_for(
                port,
                session,
                """
                const c = document.querySelector(".dash-card[data-widget-type='world-clock']");
                return c.dataset.row === '4' && !c.classList.contains('is-dragging') ? [Number(c.dataset.col), Number(c.dataset.row)] : false;
                """,
            )
            assert moved == [1, 4], moved
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
            assert (clock["col"], clock["row"]) == (1, 4), after_reload
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
                    document.querySelector('#btn-done-edit').click();
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
                if (document.documentElement.classList.contains('is-editing')) return false;
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
            print(
                "PASS: Graphical Qt dashboard starter, dot-field drag, shelf add, reload persistence, "
                "and Settings after a preset verified"
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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("webdriver_binary")
    parser.add_argument("--graphical", action="store_true")
    args = parser.parse_args()
    test_headless_fallback_and_settings(args.webdriver_binary)
    if args.graphical:
        test_graphical_dashboard_end_to_end(args.webdriver_binary)


if __name__ == "__main__":
    main()
