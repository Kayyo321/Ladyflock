# Firefox Extension Compatibility

This document records which Firefox extensions Ladyflock can run, how that was measured, what each API does here, and what is still missing. Everything here was checked against the 100 most-used extensions on Mozilla Add-ons as of 26 November 2026, downloaded as their signed packages.

**Bottom line.** 63 of the 100 now pass Ladyflock's install check. Before this work about 15 did, by a script that applied the earlier rules; the earlier check itself wasn't run. All 63 load, and 32 of them start with no error at all. 8 more report only errors that Firefox would give under the same test, mostly because the test refuses network access. The other 23 run into a gap listed in section 5, or an error not yet explained.

**Starting without an error does not mean an extension works.** Nothing here signed in, synced, or used an extension's features past its first screen.

| | Before | After |
|---|---|---|
| Pass the install check (manifest and packaged rulesets) | about 15 | 63 |
| Start with no error (background, popup, a page with a sign-in form) | not measured | 32 of 63 |
| Start with only the errors Firefox would also give here | not measured | 8 more |
| Crash the browser or the tab when started | not measured | 0 (two crashes found and fixed, section 3) |

## 1. How Ladyflock decides to install an extension

`SupportMatrix::evaluate_manifest` (`Libraries/LibWebCommon/Extensions/SupportMatrix.cpp`) follows Firefox's own rules:

- **A key or permission Firefox doesn't know is ignored with a warning**, as Firefox ignores it. Examples are Chrome's `key`, `update_url`, `externally_connectable`, `minimum_chrome_version`, `version_name`, and the `background` and `offscreen` permissions. Ignoring them grants nothing.
- **A key or permission Firefox acts on, and Ladyflock doesn't implement, refuses the install**, with the reason shown in `about:addons`. The current list is in section 4.
- **A content security policy that would let an extension page run scripts from where Firefox does not refuses the install**, as it does in Firefox. Only `script-src`, its `-elem` and `-attr` variants, `worker-src` (falling back to `child-src`), and, in Manifest V2, `object-src` are checked. `connect-src`, `img-src`, `form-action` and the rest are the extension's own business. 1Password's policy, which names local `http://127.0.0.1` ports in `connect-src`, was refused before this change.
- **Packaged declarative rulesets must parse**, or the install is refused. Rules may name any of Firefox's resource types. Types Ladyflock never issues, such as `csp_report`, match nothing.
- **Match patterns** accept `ws:`, `wss:` and `ftp:` schemes, and hosts with a port, as Firefox does. Patterns for `file:`, `ws:`, `wss:` and `ftp:` grant no access.

## 2. What each new API does

Each row is covered by a test in `Tests/LibWebView/TestExtensionCompat.cpp`, `TestExtensionRuntime.cpp` or `Tests/LibWeb/TestWebExtensions.cpp`.

| API | Behaviour in Ladyflock | Limits |
|---|---|---|
| `runtime.getPlatformInfo` | Firefox's names, such as `mac` and `aarch64`. | — |
| `runtime.getBrowserInfo` | `name` and `vendor` are `"Ladyflock"`. `version` is `156.0`, the Firefox release whose WebExtensions behaviour Ladyflock follows. | An extension that branches on `name === "Firefox"` takes its other path. In the corpus, only Privacy Badger does, to choose a help link. See also the user agent in section 5. |
| `runtime.onStartup`, `onSuspend`, `onMessageExternal`, `onConnectExternal`, enums | Events and constants as in Firefox. | Ladyflock has no messaging between extensions, so the external events never fire. |
| `runtime.setUninstallURL`, `runtime.reload` | The URL, which must be http or https, opens when the extension is removed. `reload` disables and re-enables the extension. | — |
| `runtime.sendMessage`, `tabs.sendMessage`, `runtime.connect` | Every form Firefox accepts: with or without an extension ID and options, and with a callback in either namespace. `tabs.sendMessage` with `frameId` reaches that frame. | A message to another extension fails with Firefox's `Could not establish connection. Receiving end does not exist.` Without `frameId`, `tabs.sendMessage` reaches only the top-level frame. |
| `extension.getBackgroundPage`, `runtime.getBackgroundPage` | From the background page, its own window. | **From any other page they throw.** Ladyflock's extension pages run apart from the background page, so they can't reach its window. `extension.getViews` is missing. |
| Cross-origin requests from extension pages | As in Firefox, an extension's own page may read responses from hosts it has access to, without CORS and without a preflight. The browser decides from the current grants, so withdrawing a host takes effect at once. | Content scripts and web pages are unchanged. Mixed-content blocking still applies, so an extension page can't fetch plain `http:` from a remote host. |
| `storage.sync` | Kept on this device, in its own table, under Firefox's sync quotas: 100 KiB in total, 8 KiB per item, 512 items. This is what Firefox does when it isn't signed in. | Never leaves the device. |
| `storage.managed` | Rejects with Firefox's `Managed storage manifest not found`. | No enterprise policy for extensions. |
| `storage.<area>.onChanged` | Each area's own event. | — |
| Content scripts: `runtime.getManifest`, `i18n`, `extension.getURL` | Content scripts now see their real manifest and messages; before, `getManifest()` returned `{}` and `i18n` was missing. | — |
| `content_scripts` `world: "MAIN"` | Runs in the page's own realm, without extension APIs, including at `document_start` before the page's first script. | `scripting.executeScript` with `world: "MAIN"` is still refused. |
| `tabs.executeScript`, `tabs.insertCSS`, `tabs.removeCSS`, `scripting` with `allFrames` | Reaches the top-level frame and each other frame the extension may access. Results come back in frame order; the first frame's error fails the call. | Frames the extension may not access are skipped, as in Firefox. |
| `management.getSelf`, `uninstallSelf`; with the permission, `getAll`, `get` and events | As in Firefox. | `uninstallSelf({showConfirmDialog: true})` declines, as Firefox does when the user cancels, since there is no dialog. |
| `windows.get`, `getAll`, `getLastFocused`, `create`, `onCreated`, `onRemoved` | `create` opens the URL, or the extension's own page, in a new normal window and resolves with it. | Every window is `normal`: `type: "popup"`, size and position are not applied. No `windows.update` or `remove`. |
| `tabs.getCurrent`, `tabs.onReplaced`, `tabs.onHighlighted`, tabs enums | `getCurrent` is the tab an extension page is shown in. Ladyflock highlights only the active tab, so `onHighlighted` fires with `onActivated`. `TabStatus`, `WindowType` and the other enums are Firefox's. | Ladyflock never replaces a tab's page, so `onReplaced` never fires. |
| `webNavigation.onTabReplaced` | Present. | Never fires, for the same reason. |
| `webRequest` options and resource types | The `extraInfoSpec` options Firefox accepts for each event, including `responseHeaders` on `onResponseStarted`, `onBeforeRedirect` and `onCompleted`, which now carry the headers. All of Firefox's resource types and options enums. | `requestBody` is accepted, but the details have no request body. |
| `webRequest.onBeforeSendHeaders` | Fires before each request with its headers, when asked for `requestHeaders`. | **A blocking listener can't change the headers.** What it returns has no effect, and the extension's console says so. Decentraleyes, Ghostery's Global Privacy Control, Greasemonkey, LastPass and Proton Pass listen to it. Their other features start, but these header changes don't happen. |
| `action.setIcon`, `browserAction.setIcon` | A relative path is relative to the calling page, as in Firefox. | The toolbar draws bitmaps from the package only. An SVG path or `imageData` succeeds but leaves the current icon. `setBadgeTextColor` is missing. |
| `notifications` | Shown through the system's notifications, under the extension's name. `onShown`, `onClicked` and `onClosed` (with `byUser`) come from the platform. | Every template shows as `basic`, as in Firefox. No icon yet. |
| `idle` | From the system's idle time and screen lock, on macOS. `onStateChanged` is checked every 5 seconds. | **Elsewhere the system always counts as active**, so an auto-lock that waits for idle or for the screen to lock never fires. |
| `clipboardWrite`, `clipboardRead` | An extension's own pages may write or read the clipboard without a click, as in Firefox. The browser checks the permission again. | — |
| `cookies` | The default profile's cookies, limited to sites within hosts the extension may access. `onChanged` reports additions and removals. Store `firefox-default`. | Partitioned cookies are not listed. Cookies of private windows and named identities are never reachable. |
| `downloads` | Saves into the downloads folder through the browser's download manager, from http or https, or from a blob the extension made. `search`, `cancel`, `pause`, `resume`, and `onCreated`, `onChanged`, `onErased` are provided. | GET only. No `open`, `show` or `erase`. `onDeterminingFilename` is Chrome's and absent, as in Firefox. Only the default profile's downloads are visible. |
| `nativeMessaging` | Finds the native manifest where Firefox does. With none, it fails with Firefox's `No such native application <name>`. | **Ladyflock does not start native hosts**, so desktop integration (1Password, KeePassXC, Bitwarden biometrics, iCloud Passwords, IDM) does not work. |
| `declarativeNetRequestWithHostAccess` | Every rule needs host access, as in Firefox. | — |
| `privacy.services.passwordSavingEnabled` and the autofill settings | Report `false` and `not_controllable`, since Ladyflock saves no passwords and fills no forms itself. Password managers set them at startup. | `privacy.network` is missing. |
| `action.getUserSettings` | Whether the action is pinned. | — |
| `menus`, `contextMenus` | Adds the `selection` context, with `selectionText`, plus separators, `visible` and `icons`. `onShown` fires as a page's menu opens, with the page's details only for pages the extension may access. `onHidden` fires once an item is chosen. | A menu closed without a choice reports `onHidden` only when the next one opens. `refresh` resolves, but an open menu keeps its items. Items for the tab strip, toolbar button, bookmarks and tools menu are kept but not shown. No submenus (`parentId`), checkboxes, radio items or disabled items. |
| `webRequest.onAuthRequired` | Present with `webRequestBlocking` or `webRequestAuthProvider`. | **Never fires**: Ladyflock answers no HTTP authentication challenge (Fetch's prompt step is unimplemented). |
| `theme` | `getCurrent` reports no theme. | `update` and `reset` reject: Ladyflock does not apply extension themes. Dark Reader's optional theme feature fails; Firefox Color does nothing. |
| `omnibox` | The key is accepted, and the API has its events. | The keyword is not bound to the address bar, so the events never fire. |
| `commands` `suggested_key` | Accepted. | No shortcut is bound; `commands.getAll` reports none, as Firefox does for an unbound command. |
| `match_origin_as_fallback` | Reaches `about:` frames. | No content script runs in `blob:` or `data:` frames. |

### Privacy decisions

Extensions run only in the default profile, so none of these APIs reaches a private window or a named identity:

- `cookies` and `downloads` see only the default profile's data.
- Native hosts are not started.
- The CORS exemption applies only to the extension's own documents, which the browser identifies. It never applies to its content scripts. Requests still go through the profile's network routing, including proxy-only mode.
- Request headers can't be changed by extensions.
- `proxy`, `contextualIdentities` and `browsingData` are still refused. Each would interact with identity routing or purge, so each needs its own policy first.

## 3. Defects found by starting the extensions

| Defect | Effect | Fix |
|---|---|---|
| `import()` in a content script dereferenced a missing HTML script record. | **The user's tab crashed** whenever such a content script ran, for example LanguageTool's. | It now rejects with a `TypeError`. Module loading for content scripts is still missing (section 5). |
| `management.getSelf` read `homepage_url` through a reference into a destroyed temporary. | **The browser crashed** for any extension with a homepage that called it, for example AdBlocker for YouTube. | Fixed; the test fixture now has a homepage. |
| Content scripts that threw or failed to parse reported nothing. | Their errors were invisible. | Reported to the console, as in Firefox. |
| API errors reached extensions wrapped in JSON quotes, as `"\"No such …\""`. | Extensions that match error text misread every error. | Plain messages. |
| `idle.onStateChanged` compared the new state with itself. | It never fired. | Found by its test before shipping. |
| The first CORS exemption read the scheme of an opaque origin. | Any `about:blank` or sandboxed page's fetch would have crashed the renderer. | Found by the full `TestLibWebView` run before it was committed. |

## 4. The corpus

Each of the top 100 was downloaded from addons.mozilla.org and its SHA-256 checked against the listing. The packages were kept outside the repository.

### Install check

`Build/ladyflock/bin/extension-compat <xpi>...` runs Ladyflock's install check on each package: its manifest and its packaged rulesets. It prints one JSON object per package and runs none of the extension's code.

Of the 37 refused, the reasons are below. Some extensions have more than one.

| Refused for | Extensions | Examples |
|---|---|---|
| `identity` | 8 | Grammarly, MetaMask, DeepL, Read Aloud |
| `proxy` | 7 | FoxyProxy, NordVPN, Browsec, 1VPN |
| `contextualIdentities` | 5 | Multi-Account Containers, Facebook Container, Cookie AutoDelete, Tampermonkey, Tree Style Tab |
| `devtools_page` | 4 | Adblock Plus, AdBlock, React Developer Tools, Cookie-Editor |
| `history` | 4 | DownThemAll!, Reddit Enhancement Suite |
| `chrome_settings_overrides` | 4 | DuckDuckGo, Ecosia, Startpage |
| `browsingData` | 3 | Cookie AutoDelete, ESET |
| `page_action` | 3 | TWP, Multi-Account Containers, Reddit Enhancement Suite |
| `sessions`, `search`, `tabGroups`, `webRequestFilterResponse` | 2 each | Tree Style Tab, NoScript, Tab Session Manager |
| Changing request headers in a packaged rule | 1 | 1Password |
| `bookmarks`, `browserSettings`, `downloads.open`, `pkcs11`, `user_scripts`, `protocol_handlers`, `menus.overrideContext` | 1 each | iCloud Bookmarks, ESET, DownThemAll!, Belgium eID, Tampermonkey, Tree Style Tab |
| A ruleset without the `declarativeNetRequest` permission | 1 | DeepL |

### Starting each extension

`TestLibWebView --suite TestExtensionCorpus`, with `LADYFLOCK_EXTENSION_CORPUS` set to a folder of unpacked extensions, starts each one in a fresh profile:

1. It opens a browser window, as a running browser has.
2. It starts the extension's background page.
3. It loads a page with a sign-in form in that window's tab, for the content scripts.
4. It opens the toolbar popup.

Tabs and windows the extension opens are created as the UI would create them. Every request goes through a proxy on the loopback interface. The proxy serves only the test page, refuses everything else, and records the hosts each extension tried to reach. No extension reached the network, so **network features and the CORS exemption can't be seen working in this run**; their tests are in `TestExtensionCompat.cpp`.

The harness reports console errors from the background page and popup. Content-script errors appear only in the renderer's log, which the harness marks per extension. A control extension that throws in all three places confirmed that each is heard.

Each error was then sorted as one of these:

- **Expected:** a refused network request, a missing native host, an error Firefox gives too, or a harness artifact.
- **A gap** from section 5.
- **Not yet explained.**

The results, extension by extension, are in [corpus-results.md](corpus-results.md).

### Password managers

| Extension | Installs | Starts | What it runs into |
|---|---|---|---|
| 1Password | No | — | Its packaged rule changes request headers (`modifyHeaders` with `requestHeaders`), which Ladyflock can't do. In an earlier probe with that rule removed, its background page started, and then needed native messaging and `extension.getViews`. |
| Bitwarden | Yes | Yes | Only errors Firefox would give here: its server is refused, and it logs Firefox's `Managed storage manifest not found`. Its server calls fail as network errors, not CORS errors. |
| Proton Pass | Yes | Yes | No errors. What its `onBeforeSendHeaders` listener returns has no effect. |
| LastPass | Yes | Yes | No errors. What its `onBeforeSendHeaders` listener returns has no effect. |
| KeePassXC | Yes | Yes | No errors. It needs its native host, which Ladyflock does not start, so it can't reach KeePassXC. |
| NordPass | Yes | Yes | Its background page starts. Its popup calls `extension.getViews`, which is missing, and its menu uses submenus. |
| Norton Password Manager | Yes | Yes | Only refused network requests, for its rule files. |

**Sign-in was not tested.** None of these was used past its first screen, since that needs an account. Whether sign-in, sync and autofill work also depends on the engine, for example WebAssembly, WebCrypto and IndexedDB in extension pages, and on the gaps in section 5.

## 5. What is missing

Roughly in order of how many of the top 100 each holds back:

1. **Changing request headers.** This covers `declarativeNetRequest` `modifyHeaders` with `requestHeaders`, and what a blocking `webRequest.onBeforeSendHeaders` returns. It blocks 1Password's install and leaves header features inert in five extensions that start. The decision has to reach the request before `ResourceLoader::load` in `Fetching.cpp`.
2. **The user agent names Chrome.** Ladyflock's default user agent ends in `Chrome/… Safari/…`, so an extension that checks `navigator.userAgent` for `Firefox/` takes its Chrome path. Malwarebytes then adds rules with Chrome-only resource types, which fail. Firefox APIs plus a Chrome user agent is a combination no extension is tested against. Whether extension pages should report Firefox's user agent is a product decision still to make.
3. **`identity`** (8 refused): `getRedirectURL` and `launchWebAuthFlow`, for Grammarly, MetaMask and others.
4. **`proxy`** (7 refused, mostly VPNs): deliberately refused until it can be bounded by the managed policy and kept out of Tor identities.
5. **`contextualIdentities`** (5), **`devtools_page`** (4), **`history`** (4), **`chrome_settings_overrides`** (4), **`browsingData`** (3), **`page_action`** (3), then `sessions`, `search`, `tabGroups`, `webRequestFilterResponse` and `bookmarks`.
6. **Reaching the background page from other pages:** `getBackgroundPage` from a popup, and `extension.getViews`. These need the pages of one extension to share a process. ColorZilla, FireShot, Screenshot Capture and Stylus use them.
7. **Menu submenus**, checkboxes, radio items and disabled items. They need UI work. Used by LeechBlock, Ghostery, OneTab, Video DownloadHelper and Violentmonkey.
8. **`import()` in content scripts**, used by LanguageTool and Capital One Shopping.
9. **Starting native messaging hosts.**
10. **Smaller APIs and differences:**
    - DNR `regexFilter` (AdBlocker for YouTube)
    - `scripting.executeScript` in the `MAIN` world (Ghostery)
    - `tabs.connect` (iCloud Passwords)
    - `windows.update` and `remove` (LanguageTool)
    - `action.setBadgeTextColor` (Video DownloadHelper)
    - `privacy.network` (AdGuard)
    - `document_start` scripts run before the `<html>` element exists (Malwarebytes)
    - `runtime.getContexts`, `tabs.captureVisibleTab` and keyboard shortcuts for commands
    - Whether Firefox exempts extension pages from mixed-content blocking is unconfirmed.

## 6. How to repeat this

```sh
# Install check, on downloaded XPIs:
Build/ladyflock/bin/extension-compat /path/to/xpis/*.xpi

# Start each unpacked extension (one per folder) and record errors:
LADYFLOCK_EXTENSION_CORPUS=/path/to/unpacked LADYBIRD_SOURCE_DIR="$PWD" \
    Build/ladyflock/bin/TestLibWebView --suite TestExtensionCorpus 2> corpus.log
```

`LADYFLOCK_EXTENSION_CORPUS_ONLY=<folder>` starts a single extension. Each extension's report is a `REPORT {...}` line on standard error. The renderer's lines that follow `=== corpus extension: <name>` belong to that extension.
