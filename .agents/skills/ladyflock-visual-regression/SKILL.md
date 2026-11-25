---
name: ladyflock-visual-regression
description: Verify Ladyflock visual fidelity and regressions with reproducible before-and-after captures, native Qt and Ladyflock WebDriver tests, and a platform, theme, scale, and interaction matrix.
---

# Verify the real UI

Run for any user-visible appearance or interaction change. This workflow collects evidence; it is not a replacement for CI, a visible test run, or human review.

## Pick two useful baselines

- Use the pre-change Ladyflock revision for regression. For Ladybird parity, use the relevant upstream revision or the closest unchanged Ladybird component, and record that reference separately. Do not pull in unrelated upstream UI changes or declare an internet screenshot canonical.
- On each test platform, match the build options, OS/window backend, fonts, appearance/accent, scale, window size, screenshot bounds, fixture data, and focus/interaction state. Record the exact revisions and environment. Use a fresh isolated profile and synthetic tabs or forms. Do not capture personal browsing data.
- Wait for the UI state or rendering signal, not a blind long sleep. Take before and after images of the same area. Inspect both at native scale and side by side; use a diff or overlay for same-environment captures when useful. Investigate pixel differences; do not hide clipping, missing icons, broken focus or misalignment behind a loose tolerance.

## Choose the actual test surface

| Change | Prefer | Proof you still need |
| --- | --- | --- |
| Qt chrome, toolbar, tab, menu or popover | Focused `TestUIQt` suite in `Tests/UI/Qt`; run the real desktop app | Native screenshots for affected themes, DPI and platform-specific window behavior. |
| `about:` or browser settings page | Ladyflock WebDriver or real app, with its own isolated profile | Ladyflock-rendered screenshot and a behavioral assertion for changed controls. |
| Web renderer | `Tests/LibWeb` Layout/Ref/Screenshot test where appropriate | Inspect actual expected/actual/diff artifacts; use a platform-specific baseline only when the renderer legitimately differs. |

For an already configured Ladyflock development build with Qt, a focused example is:

```sh
cmake --build Build/ladyflock --target TestUIQt
Build/ladyflock/bin/TestUIQt --suite TestPermissionPromptPopover
```

Pick the suite from the actual test files. Read `Documentation/Testing.md` and `Documentation/WebDriver.md` before running broader suites. The repository's offscreen Qt tests can assert geometry and state; they cannot certify native painting or window interaction. A widget capture can also omit separately composed areas; capture the real window or desktop target for those surfaces.

## Required coverage for the changed surface

- **Appearance:** light and dark; check system/accent and private-window variants where that surface uses them.
- **Geometry:** normal and smallest supported layout; at least 1× and HiDPI when available, and fractional scale on affected Linux paths when available. Check long strings, translations or RTL if alignment or text layout changed.
- **Interaction:** normal, hover, focus via keyboard, pressed/selected, disabled, and opened/closed where applicable. Verify Escape/click-away, focus return, and reduced-motion if affected.
- **Platforms:** test every platform directly affected by a platform-specific change; for shared desktop chrome, report macOS and Linux separately rather than inferring parity from one.

## Readiness rule

Treat unexplained differences and failing checks as blockers. Keep intentional baseline updates narrowly scoped and inspect the changed images. If a needed platform, GUI, or test runner is unavailable, report **not verified** for that row; do not claim visual parity or readiness on it. Deliver the tested revision, reference, actual commands and outcomes, before/after evidence, and any remaining gap. A skipped Actions job is not a pass.
