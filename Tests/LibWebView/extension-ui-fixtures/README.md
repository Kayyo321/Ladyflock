# Extension UI fixtures

Unpacked extensions for checking how Ladyflock presents common Firefox extension UI: toolbar buttons, badges, the
Extensions menu, popups, sidebars and options pages. They use no network and no host permissions.

Load all of them into a disposable profile:

```sh
args=(); for d in Tests/LibWebView/extension-ui-fixtures/*/; do args+=(--load-extension "$PWD/$d"); done
Build/ladyflock/bin/Ladybird.app/Contents/MacOS/Ladybird --profile-path "$(mktemp -d)" "${args[@]}" about:addons
```

| Fixture | What it covers | Firefox behavior to compare against |
| --- | --- | --- |
| `fixed-width-popup` | MV3 `action`, SVG icon, red badge text | Popup is exactly as wide as `body { width: 320px }` and only as tall as its content. |
| `auto-width-popup` | MV2 `browser_action`, PNG icon, no CSS width | Popup shrinks to the content's preferred (max-content) width. |
| `tall-popup` | Content taller than 600px, long extension name | Popup stops at 600px tall and scrolls; the name is elided in menus. |
| `resizing-popup` | uBlock Origin-style `row-reverse` panes, `theme_icons` | Popup hugs the main pane, then grows to the left when **More** shows the side pane. |
| `sidebar-and-options` | `sidebar_action`, `options_ui`, missing icon file | Sidebar fills its pane; the missing icon falls back to a placeholder. |
| `click-action` | `action.onClicked` without a popup, yellow badge | Each click increments the badge; badge text stays legible on a light color. |
| `full-height-popup` | `html, body { height: 100% }` with the body's default margins | Popup settles at one height rather than growing step by step to 600px. |
| `framed-popup` | Same-origin `moz-extension:` iframe | Only the popup document sizes to content; the frame's text wraps at 300px without a sideways scrollbar. |
