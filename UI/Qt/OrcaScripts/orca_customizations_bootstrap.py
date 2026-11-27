# Ladyflock: register custom app-specific Orca script for Orca 49.7+.
#
# Orca 49.7 removed support for the ~/.local/share/orca/orca-scripts/ directory as a means to directly load custom user/
# app-supplied scripts. Ladyflock installs its own script under orca-scripts/Ladyflock/ and hooks Orca's
# ScriptManager._new_named_script using an independent namespace and closure so it can coexist in orca-customizations.py
# alongside upstream Ladybird and user customizations regardless of installation order.


def _ladyflock_install_orca_hook():
    import importlib as _ladyflock_importlib
    import os as _ladyflock_os
    import sys as _ladyflock_sys

    from types import FunctionType

    scripts_dir = _ladyflock_os.path.join(
        _ladyflock_os.path.dirname(_ladyflock_os.path.abspath(__file__)),
        "orca-scripts",
    )
    if scripts_dir not in _ladyflock_sys.path:
        _ladyflock_sys.path.insert(0, scripts_dir)

    try:
        from orca import script_manager as _ladyflock_script_manager
    except ImportError:
        return

    manager = _ladyflock_script_manager.ScriptManager
    current = manager._new_named_script
    if getattr(current, "_ladyflock_hook_installed", False):
        return

    # The legacy Ladybird hook looks up its predecessor in the customization namespace on every call.
    # Re-executing the file overwrites that binding, so an older hook in the call chain would recurse.
    upstream_predecessor = "_ladybird_original_new_named_script"
    if (
        isinstance(current, FunctionType)
        and current is globals().get("_ladybird_patched_new_named_script")
        and current.__globals__ is globals()
        and upstream_predecessor in current.__code__.co_names
    ):
        predecessor = globals()[upstream_predecessor]
        if getattr(predecessor, "_ladyflock_hook_installed", False):
            # If Ladybird runs later in the file, it will wrap this hook again. If it already ran,
            # this hook already holds the Ladybird implementation captured on the first load.
            manager._new_named_script = predecessor
            return

        # Keep a callable copy of Ladybird's hook with its original predecessor bound in private globals.
        current = FunctionType(
            current.__code__, current.__globals__.copy(), current.__name__, current.__defaults__, current.__closure__
        )

    original_new_named_script = current

    def _ladyflock_patched_new_named_script(self, app, name):
        if app and name == "Ladyflock":
            candidate = _ladyflock_os.path.join(scripts_dir, "Ladyflock", "__init__.py")
            if _ladyflock_os.path.exists(candidate):
                try:
                    module = _ladyflock_importlib.import_module("Ladyflock")
                    if hasattr(module, "get_script"):
                        return module.get_script(app)
                    if hasattr(module, "Script"):
                        return module.Script(app)
                except Exception:
                    # Fall back to Orca's default script, but surface why ours failed to load.
                    import traceback as _ladyflock_traceback

                    _ladyflock_traceback.print_exc()
        return original_new_named_script(self, app, name)

    _ladyflock_patched_new_named_script._ladyflock_hook_installed = True
    manager._new_named_script = _ladyflock_patched_new_named_script


_ladyflock_install_orca_hook()
