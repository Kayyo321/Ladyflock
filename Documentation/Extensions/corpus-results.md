# Starting the Most-Used Firefox Extensions

These are the results of starting, in Ladyflock, each of the 100 most-used extensions on Mozilla Add-ons that passes its install check, plus 1Password. The list is from 26 November 2026. [Firefox Extension Compatibility](Compatibility.md) explains the method, the APIs and the gaps.

Each extension started in a fresh profile with one browser window. The harness then started its background page, loaded a page with a sign-in form in that window's tab, and opened its toolbar popup. Every request went through a proxy on the loopback interface, which served only the test page and refused everything else. The results come from the errors and warnings each extension reported to its consoles.

**"No errors" means the extension started without reporting an error. It does not mean the extension works.** Nothing here signed in, synced, blocked a real ad, or used an extension's features past its first screen.

| Result | Extensions |
|---|---|
| No errors | 33, five of which register header changes that have no effect |
| Only expected errors | 9 |
| Gaps or errors not yet explained | 21 |
| Refused at install | 1 (1Password) |

"Only expected errors" are errors Firefox would also give under this harness:

- Calls to the extension's servers, which the proxy refuses.
- A missing native host.
- Errors Firefox gives itself, such as `Managed storage manifest not found`, or `Receiving end does not exist` for a message to a tab with no content script or to another extension.
- An injection into a tab the extension may not access. Dark Reader injects into every tab at startup, and the window's tab may still be `about:blank` at that moment. Firefox refuses such an injection too.

"Header changes have no effect" marks an extension that registered a blocking `webRequest.onBeforeSendHeaders` listener, which its console then warned about. The extension starts, but its changes to request headers aren't made.

The run varies a little with timing. In one earlier full run, Firefox Relay's popup found no `document.body` and Proton VPN found no browser window; started alone, neither did.

| Extension | Background starts | Result | What it runs into |
|---|---|---|---|
| 1password-x-password-manager | — | Refused at install | Unsupported declarative request header action |
| 7tv-extension | Yes | No errors |  |
| adblock-for-youtube | Yes | Gaps | DNR `regexFilter` |
| adblocker-ultimate | Yes | No errors | **header changes have no effect** (blocking `onBeforeSendHeaders`) |
| adguard-adblocker | Yes | Gaps | `privacy.network` (also: network refused by the test proxy); **header changes have no effect** (blocking `onBeforeSendHeaders`) |
| betterttv | Yes | Gaps | registering a content script for a host it lacks (Firefox's behaviour unconfirmed) |
| bitwarden-password-manager | Yes | Only expected errors | network refused by the test proxy; an error Firefox gives too |
| cisco-webex-extension | Yes | No errors |  |
| clearurls | Yes | Only expected errors | network refused by the test proxy |
| colorzilla | Yes | Gaps | `extension.getViews`, or `getBackgroundPage` from a popup |
| consent-o-matic | Yes | No errors |  |
| coupert | Yes | Only expected errors | network refused by the test proxy |
| darkreader | Yes | Only expected errors | injecting into a tab it may not access (see above); network refused by the test proxy |
| decentraleyes | Yes | No errors | **header changes have no effect** (blocking `onBeforeSendHeaders`) |
| easyscreenshot | Yes | No errors |  |
| ebates | Yes | No errors |  |
| enhancer-for-youtube | Yes | No errors |  |
| epubreader | Yes | No errors |  |
| firefox-color | Yes | No errors |  |
| fireshot | Yes | Gaps | `extension.getViews`, or `getBackgroundPage` from a popup |
| ghostery | Yes | Gaps | `scripting.executeScript` in the `MAIN` world; errors not yet explained; menu submenus, checkboxes or disabled items (also: network refused by the test proxy); **header changes have no effect** (blocking `onBeforeSendHeaders`) |
| gnome-shell-integration | Yes | Only expected errors | an error Firefox gives too; no native host |
| google-translator-for-firefox | Yes | No errors |  |
| greasemonkey | Yes | No errors | **header changes have no effect** (blocking `onBeforeSendHeaders`) |
| i-dont-care-about-cookies | Yes | No errors |  |
| icloud-passwords | Yes | Gaps | `tabs.connect` |
| istilldontcareaboutcookies | Yes | No errors |  |
| kaspersky-protection-2021 | Yes | No errors |  |
| keepa | Yes | No errors |  |
| keepassxc-browser | Yes | No errors |  |
| languagetool | Yes | Gaps | `windows.update`; a CORS failure (it has no host permission for its server, so Firefox applies CORS too, and the test proxy's refusal has no CORS headers); `import()` in a content script (also: network refused by the test proxy) |
| lastpass-password-manager | Yes | No errors | **header changes have no effect** (blocking `onBeforeSendHeaders`) |
| leechblock-ng | Yes | Gaps | menu submenus, checkboxes or disabled items |
| malwarebytes | Yes | Gaps | errors not yet explained; takes its Chrome path, from the user agent; `document_start` scripts run before `<html>` exists |
| nordpass-password-management | Yes | Gaps | menu submenus, checkboxes or disabled items; `extension.getViews`, or `getBackgroundPage` from a popup |
| norton-password-manager | Yes | Only expected errors | network refused by the test proxy |
| norton-safe-web | Yes | Gaps | errors not yet explained (also: an error Firefox gives too; network refused by the test proxy) |
| ols-f-secure | Yes | Gaps | errors not yet explained |
| onetab | Yes | Gaps | menu submenus, checkboxes or disabled items; errors not yet explained |
| privacy-badger17 | Yes | Only expected errors | network refused by the test proxy; **header changes have no effect** (blocking `onBeforeSendHeaders`) |
| private-relay | Yes | Only expected errors | network refused by the test proxy |
| proton-pass | Yes | No errors | **header changes have no effect** (blocking `onBeforeSendHeaders`) |
| proton-vpn-firefox-extension | Yes | No errors |  |
| savefromnet-helper | Yes | Gaps | errors not yet explained |
| screenshot-capture-annotate | Yes | Gaps | `extension.getViews`, or `getBackgroundPage` from a popup |
| search_by_image | Yes | No errors |  |
| simple-translate | Yes | No errors |  |
| sponsorblock | Yes | Only expected errors | network refused by the test proxy; an error Firefox gives too |
| steam-database | Yes | No errors |  |
| styl-us | Yes | Gaps | `extension.getViews`, or `getBackgroundPage` from a popup |
| to-google-translate | Yes | No errors |  |
| tonec-idm-integration-module | Yes | No errors |  |
| ublock-origin | Yes | No errors |  |
| video-background-play-fix | No background page | No errors |  |
| video-downloader-profession | Yes | No errors |  |
| video-downloadhelper | Yes | Gaps | errors not yet explained; `action.setBadgeTextColor`; menu submenus, checkboxes or disabled items |
| violentmonkey | Yes | Gaps | menu submenus, checkboxes or disabled items |
| wappalyzer | Yes | No errors |  |
| web-threat-shield | Yes | Gaps | errors not yet explained (also: a Chrome-only API) |
| wikibuy-for-firefox | Yes | Gaps | `import()` in a content script; errors not yet explained |
| youtube-high-definition | Yes | No errors |  |
| youtube-nonstop | No background page | No errors |  |
| youtube-recommended-videos | Yes | No errors |  |
| zoom-new-scheduler | Yes | No errors |  |

## Errors not yet explained

These may be Ladyflock defects, or follow from the refused network requests. They need a closer look:

- **ghostery**: `console.error: Error while executing observer:  …`; `console.error: [adblocker] not ready for cosmetic injection …`; `console.error: [adblocker] not ready for network requests blocking`
- **norton-safe-web**: `TypeError: Cannot access property "isInitialized" on undefined object (content/scripts/multiVariateTestingCS.js:1)`; `Cannot access property "isInitialized" on undefined object at content/scripts/multiVariateTestingCS.js:1:2827`; `console.error: Fetching rules from CDN failed.`
- **ols-f-secure**: `TypeError: Cannot access property "MainColor" on undefined object "customization" (common_styles.js:33)`; `Cannot access property "MainColor" on undefined object "customization" at setStyles (common_styles.js:33:36)`
- **onetab**: `Error: core version mismatch (ext-onetab-concatenated-sources-popup.js:59)`; `core version mismatch at s (ext-onetab-concatenated-sources-popup.js:59:48455)`
- **savefromnet-helper**: `Error: 0`; `0`
- **video-downloadhelper**: `console.error: main::purging failed TypeError`
- **web-threat-shield**: `ReferenceError: 'Webroot_Background' is not defined (background_scripts/bk_helper.js:48)`; `TypeError: Cannot access property "responseText" on undefined object "x" (content_scripts/main.js:165)`; `'Webroot_Background' is not defined at onModuleLoaded (background_scripts/bk_helper.js:48:2)`
- **wikibuy-for-firefox**: `TypeError: Not an object of type Node (notifications/pageView.js:1)`; `Not an object of type Node at <unknown>`
- **malwarebytes**: its rules name Chrome-only resource types (`webtransport`, `webbundle`). It leaves those out only when `navigator.userAgent` contains `Firefox/`, and Ladyflock's user agent names Chrome.

To repeat the run, follow section 6 of [Firefox Extension Compatibility](Compatibility.md).
