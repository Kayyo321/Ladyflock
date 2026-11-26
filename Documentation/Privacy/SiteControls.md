# Site Protections and the Privacy Dashboard (Milestone 4, first increments)

This document covers the first increments of Ladyflock's **Milestone 4 (Site protections and controls)**:

- tracking-parameter stripping on navigation;
- turning JavaScript off per site;
- a read-only `about:privacy` dashboard with an explicit, expiring tab capture.

See [NetworkEnforcement.md](NetworkEnforcement.md), [IdentitiesAndStorage.md](IdentitiesAndStorage.md) and [ProxyRouting.md](ProxyRouting.md) for Milestones 0–3.

**Milestone 4 is not complete.** Three of its six areas have shipped here. Native-blocking extensions, permission lifetimes and default-deny, and fingerprint profiles are still to do (section 5). Every control below is **off by default**.

## 1. Tracking-parameter stripping

| | |
|---|---|
| Turn on | `--privacy-strip-tracking-parameters` |
| List | `utm_*`, `fbclid`, `gclid`, `dclid`, `gbraid`, `wbraid`, `srsltid`, `msclkid`, `_ga`, `_gl`, `igshid`, `mc_cid`, `mc_eid`, `yclid`. Names compare case-insensitively after percent-decoding. One list (`Libraries/LibPrivacy/Navigation/TrackingParameters.cpp`) serves both this and address-bar autocomplete. Autocomplete also ignores the cache buster `zx`, which navigations keep because a page may depend on it. |
| What changes | Only the matching `name[=value]` segments are removed from the serialized query. Every other byte, separator, order and encoding stays, so a signed URL keeps its signature. The fragment stays. A query left with nothing loses its `?`. Only `http` and `https` URLs are rewritten. |
| Where | Every top-level navigation is covered at one of two points, before the identity handoff, DNS or any request: <br>• **UI process:** `CanonicalNavigable::begin_navigation`, for navigations the browser starts and cross-process ones.<br>• **WebContent:** `LocalNavigable::begin_navigation`, for navigations a page starts in its own process. WebContent reports the count it removed. |
| Not covered | Reloads and history traversals, which do not pass through either point. Redirects that a server sends, which are followed as sent. Subresource URLs. |

Test evidence comes from `TestPrivacySiteControls` (request lines seen by an independent server) and `TestLibPrivacy → TestPrivacyNavigation`:

- **Permitted:** with the control off, tags reach the server. A sign-in callback (`code`, `state`) arrives byte for byte.
- **Denied:** the server receives `/page?id=1` for `/page?utm_source=…&id=1&fbclid=…#top`, and the view shows `#top`. A tracking-only query becomes `/only`. A link the page follows itself arrives as `/target?id=7`.
- **Compatibility:** an S3-style signed URL keeps every signed parameter and its encoding. `utmost`, `utm` and parameter values that merely contain `utm_source` stay. Duplicate keys and empty segments keep their shape.
- **Hyperlink auditing:** Ladyflock does not implement `<a ping>`, and a click sends no ping request. The test records this.
- **Cost:** about 0.8 µs per URL with tracking parameters and 0.4 µs without, over 100,000 iterations on an M-series Mac.

## 2. JavaScript off per site

| | |
|---|---|
| Turn on | `Settings::set_scripting_enabled_for_site(host, false)`, persisted under `siteControls.scriptingDisabledSites` in the profile's settings. There is no settings-page control yet. |
| Scope | A site is a host, canonicalized like content blocking's per-site exceptions. The **top-level** site decides, so a frame follows its top-level document, in or out of process, and a redirect into a listed site lands there without script. Browser (`about:`) pages and the user's extension (`moz-extension:`) pages are never affected, including an extension's own page framed inside a listed site. The list can only turn scripting off: it never turns it back on against `--disable-scripting`. |
| Where | The UI process sends the list to every page (`set_scripting_disabled_sites`) and again on every change. In LibWeb, `is_scripting_enabled()` asks `EnvironmentSettingsObject::is_scripting_allowed_by_site_policy`, which looks up the environment's top-level creation URL. The answer is cached until the page's policy generation changes. Because the spec's own check is used, disabled `<script src>` is never fetched, inline scripts and event-handler attributes do not run, and `<noscript>` content renders. |
| Live changes | A change reaches every page at once. Script that already ran is not undone, and the next document follows the new list. |
| Extensions | Isolated content scripts run through their own realm and keep working on a site with JavaScript off, as in Firefox. Content scripts registered for the `MAIN` world run as page script and follow the page. |

Test evidence comes from `TestPrivacySiteControls` (requests seen by an independent server) and `TestExtensionRuntime`:

- **Permitted:** on a site that is not listed, external, inline and event-handler scripts all fetch their markers, and `<noscript>` stays inert.
- **Denied:** with the site listed:
  - no `/script.js` request, no inline or handler beacon;
  - the `<noscript>` image is requested;
  - a frame from another site inside it runs nothing.
- **Compatibility:**
  - another site still runs its scripts, including its frame from the listed site;
  - removing the site restores scripting for its next document;
  - an extension's isolated content script still runs on a listed site, and the page's own script does not (`isolated_content_scripts_run_on_a_site_with_scripting_turned_off`);
  - the extension's own page framed by a listed site still runs its script. Removing that exemption makes this check fail.
- **Redirect:** a redirect from an unlisted site into a listed one fetches no script and runs no inline script.
- **Cost:** a page with 200 inline scripts and 200 handlers loads in about 14.7 ms. With an unrelated site listed, which forces the lookup path, it took 0.25–0.4 ms longer per load over 40 loads. That difference is within the drift between the A/B/A phases, so treat it as an upper bound. With no sites listed, the check returns before any lookup.

## 3. `about:privacy`

A read-only page (`Base/res/ladybird/about-pages/privacy.html`, backend `Libraries/LibWebView/WebUI/PrivacyUI.cpp`) that shows the following:

- the identity's name and kind;
- its network mode, the redacted route, whether local DNS and WebRTC are allowed, and the policy epoch;
- the site controls and their counts (navigations cleaned, parameters removed, navigations to sites with JavaScript off);
- an explicit tab capture.

| Rule | How it holds |
|---|---|
| **Bound to its identity** | Every query is answered for the page's own WebContent session. A dashboard lists only its identity's tabs. A capture request for another identity's tab, or for a closed tab, gets the same "not capturing" answer as an idle tab. |
| **No secrets** | The route is only sent as `ProxyRoute::redacted_description()`: its credentials and isolation secret never reach the page. Hosts and titles come from websites and are inserted as text only. |
| **Counters** | Counts are bounded, saturating and in memory only, per identity session. They go away with the identity. |
| **Capture** | Started by the user for one tab, for 10 s to 10 min. It keeps only each event's host, with IP addresses kept only as "(IPv4 address)" or "(IPv6 address)", plus the event kind (request or blocked) and its time. It never keeps a path, query, port, header, credential or body. It holds at most 500 events and counts the rest as dropped. |
| **Capture expiry** | It lives in memory only. It is discarded within a second of expiring, when its tab closes, when its tab leaves the identity it was started in, or when that identity is destroyed. |

Test evidence comes from `TestPrivacyDashboard` and `test-webdriver-privacy.py`.

- **Redaction:** a proxy-only default identity with marker credentials in its route, and a Tor-routed identity with an isolation secret. Neither the markers nor the secret appear in either dashboard's messages or rendered text, and each dashboard shows its own redacted route.
- **Identity binding:** each dashboard lists only its own tabs, and cannot capture the other identity's tab.
- **Redaction controls:** a positive check confirms the route really carries the marker credentials. A mutation check that adds them to the dashboard's route makes the test fail.
- **Capture:** using an injected clock, nothing is recorded before a capture starts. During one, `localhost` and `(IPv4 address)` are recorded without the path, query, address or port. So are the requests of a cross-site frame in another WebContent page. The capture is discarded once its time is up, goes away with its tab, and stops at 500 events. Discarding a capture when its tab leaves its identity is implemented but has no test.
- **Rendering:** `test-webdriver-privacy.py` renders the page in Ladyflock through WebDriver in light at 800 px and dark at 320 px. It checks the content, that there is no horizontal overflow with a long host, keyboard order and the shared focus ring, and starting and stopping a capture over the real WebUI channel. Screenshots of the status and capturing states were inspected at both sizes.
- **Cost (reasoned, not measured):** with no capture running, each request start costs the UI process one hash-map lookup. A capture holds at most 500 host strings.
- **Not checked visually:** HiDPI rendering and the page inside the real Qt window.

## 4. Verification

- **Revision:** branch `privacy-milestone-4`, based on `ladyflock` `a5ec801a265`. Host: macOS 26.3.1 on Apple Silicon. Build: `Build/ladyflock` (Release preset, `LADYBIRD_ENABLE_UPDATE_CHECK=OFF`).
- **New suites:** `TestLibPrivacy` (17/17, including `TestPrivacyNavigation` 4/4), `TestLibWebView --suite TestPrivacySiteControls` (4/4), `--suite TestPrivacyDashboard` (2/2) and `--suite TestExtensionRuntime` (13/13). `test-webdriver-privacy.py` passed in both configurations.
- **Real binary:** `test-privacy-flags-reach-the-browser.py` launches the `Ladybird` binary itself, not the in-process test application. It shows that each of these reaches the processes that enforce it:
  - `--privacy-strip-tracking-parameters`, for UI-started and page-started navigations;
  - a site's scripting turned off in the profile's `Settings.json`;
  - the Milestone 3 `--privacy-network-mode=proxy-only --privacy-proxy=…` route, where the page's request reaches the route's listener and never the server directly.
- **Regression runs (all passed):**
  - full `TestLibWebView`, run from `Tests/LibWebView`: exit 0;
  - full `TestLibWeb`, run from `Tests/LibWeb`, which needs its `test-inputs`: exit 0;
  - `TestUIQt` offscreen: no failures;
  - `test-webdriver-addons.py` (light at 800 px and dark at 320 px);
  - `test-addons-profile-controls.py`.

## 5. Not done in Milestone 4

- **Fingerprint profiles.** Deferred on purpose. The user agent and languages are process-wide in WebContent, and workers read hardware values directly. A profile that changes `navigator` without workers and HTTP headers would be the inconsistent state the plan warns against.
- **Permissions.** Grant lifetimes (session, timed and expiring), unused-grant revocation and a single default-deny mode are not implemented. A per-capability Block default exists in Settings already.
- **Native blocking extensions.** CNAME uncloaking, list authentication and the element picker are not implemented.
- **Other gaps:**
  - The "navigations without JavaScript" count only covers navigations the UI process sees, so same-process link clicks within a listed site are not counted.
  - No settings-page UI for the per-site JavaScript list or for tracking-parameter stripping.
  - Server-side redirects keep their tracking parameters.
  - `sendBeacon` restrictions are not implemented.
  - The dashboard has no controls of its own beyond starting and stopping a capture.
- **Platforms.** Linux and Windows were not built or run.
