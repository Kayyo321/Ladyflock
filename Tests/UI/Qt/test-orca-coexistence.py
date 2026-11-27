#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladyflock developers.
#
# SPDX-License-Identifier: BSD-2-Clause

"""Exercise the two real customization blocks against a stand-in Orca ScriptManager."""

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile
import types

from typing import Any

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
LADYFLOCK_BOOTSTRAP = REPO_ROOT / "UI" / "Qt" / "OrcaScripts" / "orca_customizations_bootstrap.py"

# Unmodified UI/Qt/OrcaScripts/orca_customizations_bootstrap.py from the pre-rebrand
# revision 81f53bea7f52a08be509cf5d726d87ab5f86ed5c.
LADYBIRD_BOOTSTRAP = pathlib.Path(__file__).with_name("OrcaFixtures") / "upstream_ladybird_bootstrap.txt"


def apply_managed_block(existing: str, product: str, body: str) -> str:
    """Mirror UI/Qt/Application.cpp install_orca_customizations_bootstrap()."""
    begin_marker = f"# --- {product}-bootstrap-begin ---"
    end_marker = f"# --- {product}-bootstrap-end ---"
    if not body.endswith("\n"):
        body += "\n"
    managed = f"{begin_marker}\n{body}{end_marker}\n"
    begin_idx = existing.find(begin_marker)
    end_idx = existing.find(end_marker)
    if begin_idx < 0 or end_idx < begin_idx:
        if existing and not existing.endswith("\n"):
            existing += "\n"
        return existing + managed
    end_of_end_line = existing.find("\n", end_idx)
    if end_of_end_line < 0:
        end_of_end_line = len(existing)
    else:
        end_of_end_line += 1
    return existing[:begin_idx] + managed + existing[end_of_end_line:]


def run_order_scenario(order: tuple[str, str]) -> None:
    bootstraps = {
        "Ladybird": LADYBIRD_BOOTSTRAP.read_text(encoding="utf-8"),
        "Ladyflock": LADYFLOCK_BOOTSTRAP.read_text(encoding="utf-8"),
    }
    user_prefix = "# User customization before\nUSER_BEFORE = globals().get('USER_BEFORE', 0) + 1\n"
    user_suffix = "\n# User customization after\nUSER_AFTER = globals().get('USER_AFTER', 0) + 1\n"

    with tempfile.TemporaryDirectory(prefix="ladyflock-orca-coexist-") as tmp:
        orca_dir = pathlib.Path(tmp)
        for name in ("Ladybird", "Ladyflock", "UserScript"):
            package = orca_dir / "orca-scripts" / name
            package.mkdir(parents=True)
            script = (
                "class Script:\n"
                "    def __init__(self, app): self.app = app\n"
                f"    def get_toolkit_name(self): return {name!r}\n"
            )
            if name != "Ladybird":
                script += "\ndef get_script(app):\n    return Script(app)\n"
            (package / "__init__.py").write_text(script, encoding="utf-8")

        content = user_prefix
        for product in order:
            content = apply_managed_block(content, product, bootstraps[product])
        content += user_suffix

        after_reinstall = apply_managed_block(content, "Ladyflock", bootstraps["Ladyflock"])
        assert after_reinstall == content, f"Ladyflock reinstall changed existing content for {order}"
        assert content.count(f"# --- Ladybird-bootstrap-begin ---\n{bootstraps['Ladybird']}") == 1
        assert content.count("# --- Ladyflock-bootstrap-begin ---") == 1

        customizations = orca_dir / "orca-customizations.py"
        customizations.write_text(content, encoding="utf-8")

        for reloads in (3, 0):
            subprocess.run([sys.executable, __file__, str(customizations), str(reloads)], check=True)
        print(f"PASS: {' then '.join(order)} installation, initial load, three reloads, and a fresh process")


def check_dispatch(customizations: pathlib.Path, reloads: int) -> None:
    fake_orca = types.ModuleType("orca")
    fake_sm = types.ModuleType("orca.script_manager")
    fallback_calls = []

    class ScriptManager:
        def _new_named_script(self, app, name) -> Any:
            fallback_calls.append((app, name))
            return ("default-orca-script", app, name)

    fake_sm.__dict__["ScriptManager"] = ScriptManager
    fake_orca.__dict__["script_manager"] = fake_sm
    sys.modules["orca"] = fake_orca
    sys.modules["orca.script_manager"] = fake_sm

    manager = ScriptManager()
    namespace = {"__file__": str(customizations)}
    scripts_dir = str(customizations.parent / "orca-scripts")
    code = compile(customizations.read_bytes(), str(customizations), "exec")
    for executions in range(1, reloads + 2):
        exec(code, namespace)
        assert namespace["USER_BEFORE"] == namespace["USER_AFTER"] == executions
        assert sys.path.count(scripts_dir) == 1

        for name in ("Ladybird", "Ladyflock", "UserScript"):
            app = object()
            script = manager._new_named_script(app, name)
            assert script.get_toolkit_name() == name
            assert script.app is app

        firefox = object()
        assert manager._new_named_script(firefox, "Firefox") == ("default-orca-script", firefox, "Firefox")
        assert fallback_calls == [(firefox, "Firefox")]
        fallback_calls.clear()


if __name__ == "__main__":
    if len(sys.argv) == 3:
        check_dispatch(pathlib.Path(sys.argv[1]), int(sys.argv[2]))
    else:
        run_order_scenario(("Ladybird", "Ladyflock"))
        run_order_scenario(("Ladyflock", "Ladybird"))
