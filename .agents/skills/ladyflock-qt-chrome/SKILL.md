---
name: ladyflock-qt-chrome
description: Preserve Ladybird's native Qt browser appearance when implementing or reviewing Ladyflock toolbar, tabs, menus, popovers, dialogs, icons, focus, theme, or desktop-window changes.
---

# Match Ladybird's native chrome

Use this skill for the browser frame and native controls. For HTML-based internal pages use `ladyflock-webui`; for rendered verification use `ladyflock-visual-regression`.

1. **Find the closest shipped control.** Open its implementation, resources, and tests. Cite the reference file and, for appearance changes, capture its state before editing. For a new control without a direct equivalent, use the closest sibling on the same surface, not a mockup or UI from another browser.
2. **Reuse the same building blocks.** Start with the following tracked files; read the actual implementations before choosing APIs.
   - Chrome arrangement and measurements: `UI/Qt/ChromeLayout.h`, `UI/Qt/ChromeStyle.cpp`, `UI/Qt/WindowToolbar.cpp`, `UI/Qt/BrowserWindow.cpp`.
   - Colors, focus and appearance: `UI/Qt/Theme.h`, `UI/Qt/Theme.cpp`, `UI/Qt/SystemAppearance.h`. Reuse chrome versus floating-panel palettes and private-window treatment. Do not hardcode a color already expressed by a theme role.
   - Controls and assets: `UI/Qt/LadybirdButton.cpp`, `UI/Qt/Popover.cpp`, `UI/Qt/TabBar.cpp`, `UI/Qt/Icon.cpp`, `UI/Qt/ladybird.qrc`. Match icon sizes, insets, focus treatments and existing state transitions. Keep the logo unchanged unless specifically asked.
   - For a prompt or settings panel, compare `UI/Qt/PermissionPromptPopover.cpp` or `UI/Qt/SiteSettingsPopover.cpp` before adding another surface.
3. **Use Qt layout and font metrics.** Use logical pixels, layout constraints, size hints and existing elision behavior. Preserve DPR for raster captures and assets. Avoid fixed geometry to fit one screenshot; test the smallest supported window and long text. Verify accessibility names, tab order and native menu keys for touched controls. Reuse `UI/Qt/Motion.h` and the existing platform preference path for any touched animation.
4. **Keep native behavior native.** macOS menu and window chrome need a real macOS check when affected; Linux windowing, decorations and scale require the affected backend. An offscreen Qt test is useful for state and geometry, but not evidence that desktop chrome or interaction looks correct.
5. **Verify before saying ready.** Add or extend the closest `Tests/UI/Qt/` fixture for any new invariant and run the visual-regression skill. Report reference, target revision, states examined, differences and gaps. Do not rebaseline an unexplained difference.

References: [Qt High DPI](https://doc.qt.io/qt-6/highdpi.html), [QPalette](https://doc.qt.io/qt-6/qpalette.html), and the repository's `Documentation/HumanInterfaceGuidelines/Text.md`.
