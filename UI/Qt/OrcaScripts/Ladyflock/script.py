"""Custom Orca script for Ladyflock browser."""

from __future__ import annotations

from orca import structural_navigator
from orca.ax_utilities import AXUtilities
from orca.scripts import default
from orca.scripts import web

from .script_utilities import Utilities

try:
    from orca import document_presenter
except ImportError:
    document_presenter = None

try:
    from orca.enums import StructuralNavigationMode
except ImportError:
    from orca.structural_navigator import NavigationMode as StructuralNavigationMode

try:
    from orca.scripts.toolkits.Qt.script import Script as _QtToolkitScript
except ImportError:
    _QtToolkitScript = None

if _QtToolkitScript is not None:

    class Script(web.Script, _QtToolkitScript):
        pass
else:

    class Script(web.Script):
        pass


class Script(Script):
    def get_toolkit_name(self):
        return "Ladyflock"

    def get_utilities(self) -> Utilities:
        return Utilities(self)

    def get_app_key_bindings(self):
        return web.Script.get_app_key_bindings(self)

    def activate(self):
        # Put the structural navigator into DOCUMENT mode at script activation. web.Script only sets this when
        # on_focused_changed reaches its tail (focus transitioning from outside the document to a focusable element
        # inside it) — which doesn't fire for Ladybird's document-root focus event (web.Script short-circuits on
        # source == document). Without this, H/K/L bindings stay unregistered until you navigate into a focusable
        # in-page element; the suspension state still controls whether the bindings fire as commands — so typing in
        # the URL bar is unaffected.
        super().activate()
        structural_navigator.get_navigator().set_mode(self, StructuralNavigationMode.DOCUMENT)

    def _suspend_document_navigators(self, suspended, reason):
        if document_presenter is not None:
            document_presenter.get_presenter().suspend_navigators(self, suspended, reason)
            return

        # Orca 49.7 exposes the navigators on the script, before the document presenter was introduced.
        self.get_caret_navigator().suspend_commands(self, suspended, reason)
        self.get_structural_navigator().suspend_commands(self, suspended, reason)

    # Orca 50 renamed the focus event handler.
    def _on_focused_changed(self, event):
        return self._handle_focus_change(event, super()._on_focused_changed, default.Script._on_focused_changed)

    def on_focused_changed(self, event):
        return self._handle_focus_change(event, super().on_focused_changed, default.Script.on_focused_changed)

    def _handle_focus_change(self, event, web_focus, default_focus):
        # The web script returns early on document-root focus, which Qt emits when the web view gains focus and
        # after page loads. Enable document navigation immediately so H/K/L work before focus moves into the page.
        if self.utilities.in_document_content(event.source):
            reason = "Ladyflock: focus in document content"
            structural_navigator.get_navigator().set_mode(self, StructuralNavigationMode.DOCUMENT)
            self._suspend_document_navigators(False, reason)
        if web_focus(event):
            return True

        # Let the default script announce browser chrome and enter focus mode for editable widgets.
        if (AXUtilities.is_entry(event.source) or AXUtilities.is_editable(event.source)) and event.detail1:
            reason = "Ladyflock: focus on editable chrome widget"
            self._suspend_document_navigators(True, reason)
        return default_focus(self, event)
