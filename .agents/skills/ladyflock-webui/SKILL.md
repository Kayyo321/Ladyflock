---
name: ladyflock-webui
description: Build and review Ladyflock's HTML-based internal about pages, settings, onboarding, and reusable WebUI components without changing Ladybird's visual language or browser privilege boundaries.
---

# Match Ladybird's internal pages

Use this skill for HTML/CSS/JS that the browser ships as its own UI. Do not apply a generic web-app redesign or use a Chromium render as proof of Ladyflock's appearance.

1. **Find the existing page and its closest sibling.** Start at `Base/res/ladybird/about-pages/`. For settings, inspect `settings.html`, the matching module under `settings/`, and shared styles in `webui.css`. For onboarding, compare `welcome.css` and the existing welcome resources. Trace how the page is hosted through `UI/Qt/WebUIPage.cpp` when settings or browser integration is affected.
2. **Extend existing components and tokens.** Reuse the page/card/row/control patterns and the actual CSS variables for text, borders, disabled states and focus. Add a shared token only when there is an actual reuse case. Avoid a new JS/CSS framework, remote assets, global resets, and inline appearance rules that bypass the existing theme.
3. **Test behavior as well as appearance.** Preserve native semantics for inputs, labels and buttons. Check keyboard operation, focus visibility, narrow layouts, zoom/scale, long labels, system/light/dark appearance and relevant empty or error states. Prefer small DOM/layout assertions over a full-page image when a specific geometry invariant is the issue.
4. **Keep the browser boundary intact.** Do not expose internal objects, settings, or privileged messages to regular webpages to facilitate styling or tests. Use existing navigation and settings wiring, and test new interactions via the real browser. For settings integration, inspect `Tests/LibWebView/test-webdriver-settings-changes.py` and the repository's `Documentation/WebDriver.md`.
5. **Render it in Ladyflock.** Use Ladyflock's WebDriver or the real application for internal-page verification. For renderer behavior, consider the repo's LibWeb Text, Layout, Ref or Screenshot suite as appropriate. Run `ladyflock-visual-regression` for visual sign-off. A screenshot produced by a different browser may aid exploration but does not count as a pass.

References: the repository's `Documentation/Testing.md`, `Documentation/WebDriver.md`, and `Documentation/HumanInterfaceGuidelines/Text.md`.
