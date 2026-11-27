# Privacy Plan Status

This compares the privacy plan, which is kept as a local design proposal and is not in Git, with the code on branch `privacy-milestone-5` (based on `ladyflock` `529923ae0b2`). It was checked against the source and the tests, not the other documents in this directory.

**Verdict: the plan is not fully implemented.** Ladyflock has a tested network-enforcement core (Milestones 0–3), three of the six Milestone 4 controls, and three Milestone 5 increments. Most of the 20 feature domains in Part III are partial or absent. The plan itself calls Part III a backlog and gives each domain its own release decision.

How each status was established:

- **Tested:** a test exercises the behavior and passed on this revision (macOS 26.3.1, Apple Silicon). The full `TestLibWebView`, `TestLibWeb`, `TestLibPrivacy` and `TestUIQt` suites pass. Of `TestRequestServer`, `TestPrivacyNetworkGate` passes when run on its own. The full run stops in `TestProxyHostResolution`, which also failed before Milestone 3, so the suites after it were not run.
- **Partial:** some of the domain exists. The gaps are named.
- **Absent:** no implementation was found.
- **Upstream:** inherited from Ladybird, not added by Ladyflock.

## 1. Roadmap exit evidence (Part IV)

| Milestone | Exit evidence | Status |
|---|---|---|
| **First target** | Sandboxed Linux build, allowed and denied runs with independent observers | **Not met on Linux.** The controls pass on macOS (`TestPrivacyMilestone0`). Nothing has been built or run on Linux, and no test confirms the OS sandbox itself refused a socket. |
| **0 · First request** | Positive and negative controls, HTTP and HTTPS, fresh RequestServer | Tested (`TestPrivacyMilestone0`, 2 cases). |
| **1 · Offline** | Startup, OS DNS, IPv4/IPv6, redirects, browser services, live revocation | Tested (`TestPrivacyOffline`, 5 cases). |
| | RTC | Partial: the WebRTC helper's flags are checked, not its traffic. |
| | Retries, observation outside the browser, repeat per platform and updater configuration | Absent. The DNS and HTTP observers are loopback servers in the test process. |
| **2 · Identities and storage** | Partitioned embed, migration, workers, shared RequestServer | Tested (`TestPrivacyMilestone2`, 5 cases). |
| **3 · Proxy and Tor** | Proxy loss, direct-route exception, reused connection, helper restart, SOCKS isolation parameters | Tested (`TestPrivacyProxyRoute`). |
| | The Tor backend's circuit sharing | **Not tested.** The Tor endpoint in the tests is a mock SOCKS5 server. A real test needs `tor` or Arti installed and the live Tor network. |
| **4 · Site protections** | Permitted, denied, compatibility and cost for each control; redaction; capture expiry | Tested for tracking-parameter stripping, per-site JavaScript and the dashboard (`TestPrivacySiteControls`, `TestPrivacyDashboard`, `TestExtensionRuntime`). |
| | Native blocking extensions, permissions, fingerprint profiles | Absent. |
| **5 · Hardening** | Platform scope and failure behavior for each feature | Stated for identity purge, managed policy and download routing ([Hardening.md](Hardening.md)). |
| | Recovery tests for encrypted profiles | Absent: there are no encrypted profiles. The purge's recovery is tested (`TestIdentityPurge`). |
| | Capability, revocation and rollback tests before third-party code | No third-party code is loaded, and there is no module system. |

## 2. Part II, core architecture

| Section | Status |
|---|---|
| §1 Topology and enforcement | Tested: Hub compiles and binds session, schema and epoch; helpers start closed; coordinator loss closes admissions; tightening quiesces and terminates a helper that does not acknowledge. The UI can only display the committed mode: no IPC lets it submit policy edits. |
| §2 Precedence | Tested for administrative ceiling ∩ identity requirement ∩ preference. The ceiling is now set by the managed policy. The finer layers (tab grant, site, identity preference, preset, global default) and temporary grants are absent. |
| §3 Module interface | Absent. |
| §4 Hook strategy | Partial. RequestServer request and resolver hooks and LibWeb's per-site scripting check exist. Fetch policy, fingerprinting, JIT/Wasm, cookie-lifetime and permission hooks are absent. |

## 3. Part III domains

| # | Domain | Status | What exists | Main gaps |
|---|---|---|---|---|
| 1 | Content and tracker blocking | Partial, mostly upstream | The adblock matcher, lists, cosmetic filtering, per-site exceptions (upstream). Ladyflock adds Offline gating of list updates and blocked-request capture. | CNAME uncloaking, scriptlets, element picker, threat lists, enforcement in RequestServer, per-identity rules, fail-closed lists. |
| 2 | Anti-fingerprinting | Absent (upstream compatibility spoofing only) | — | Profiles, noise, canvas/WebGL/audio, fonts, letterboxing, timezone, detection. |
| 3 | Network privacy | Partial, the strongest domain | Offline, proxy-only with one route, SOCKS isolation, Tor-routed identities (external endpoint), WebRTC gating, DNS denial. All tested. | Supervised Tor/Arti, DoH/ODoH, local-network checks, ECH, a test for QUIC/Alt-Svc on the route. |
| 4 | Cookie and storage privacy | Partial | Identity namespaces, partitioning by top-level site for cookies, storage, cache, SharedWorker and BroadcastChannel, migration. All tested. | Third-party cookie blocking, lifetime clamps, Storage Access API, bounce-tracking purge, retention, storage inspection. |
| 5 | Identities | Partial | The identity engine, isolation, pre-DNS handoff, and now purge with recovery. All tested. | No UI to create, switch or purge identities. No code ends a Temporary or Disposable identity: it lasts until something destroys it. Direct-network identities exist only as a name. The Qt session store does not record a tab's identity. |
| 6 | URL and navigation privacy | Partial | Tracking-parameter stripping (tested, command-line only). | Clean-link copy, custom rules, referrer controls, beacon limits, HTTPS-only, IDN lookalikes. |
| 7 | JavaScript and Web API controls | Partial | Per-site JavaScript off (tested, no UI). Global disable (upstream flag). | Third-party JS, eval, JIT/Wasm toggles, worker toggles, CSP overlays. |
| 8 | Permissions | Partial, mostly upstream | One-time grants, embargo and a global Block per capability (upstream). Per-identity managers (tested). | Timed grants, default-deny mode, revocation of unused grants, most capabilities, auditing. |
| 9 | Hardening | Partial | Identity-aware process isolation (tested). Network sandbox switch (only the flag is tested). | Per-platform gating, certificate policy, JIT toggle, LibPrivacy fuzzers, SBOM. |
| 10 | Local data privacy | Partial | Ephemeral identities (tested), crash-report review (upstream), identity purge with recovery (tested). | Encryption at rest, keychain, locking, retention, clear-on-exit, log redaction, secure memory clearing. |
| 11 | Telemetry and independence | Partial | Offline gates for reports, list updates and the add-on catalog (tested). Review-before-send crash reports (upstream). | Update checks are automatic by default and point at Ladybird's feed. No inventory of background endpoints. |
| 12 | Search privacy | Partial, mostly upstream | Custom engines, suggestions off by default (upstream). | **Suggestions for a named identity use the default identity's session (open defect).** Per-identity engines, POST search. |
| 13 | Downloads and files | Partial | Downloads now use their identity's network (tested). Private downloads not persisted (upstream). | Per-identity download history, memory-only mode, checksums, executable warnings, metadata stripping. |
| 14 | `about:privacy` | Partial | Read-only status, counters, redacted route, managed-policy state, tab capture. Tested. | Denial reasons, fingerprinting and permissions views, export, storage inspection. |
| 15 | Privacy modes | Partial | Offline, Tor-routed (network only), per-identity network mode. | The Standard, Enhanced, Strict, Maximum and Compatibility presets; custom modes; timed relaxation; diagnostics. |
| 16 | Modular architecture | Partial | LibPrivacy core types independent of UI. | Modules, SDK, signing. |
| 17 | Extension system | Partial | Extension APIs and isolated content scripts (separate project). Content scripts run on a site with JavaScript off (tested). | Extensions under the network ceiling (untested), per-identity extensions, auditing. |
| 18 | Enterprise administration | Partial | A managed network ceiling and tracking-parameter lock from a protected local file. Tested, except the real root-owned location. | Signatures, freshness, locked-setting UI, allowlists, managed CAs, identities and extensions. |
| 19 | Verification and testing | Partial | Session isolation tested end to end; controls with positive and negative cases. | Observation outside the browser on an isolated network, breakage fixtures, cross-platform runs, export and on-disk audits of the dashboard. |
| 20 | Upstream and maintainability | Partial | `ENABLE_LIBPRIVACY` exists. | Other libraries link LibPrivacy unconditionally, so turning it off is unlikely to build (not tried). `ENABLE_TOR_INTEGRATION` and `ENABLE_PRIVACY_MODULES_WASM` are absent. `SECURITY.md` is unchanged. |

## 4. Defects found by this check

| Defect | State |
|---|---|
| A named identity's download used the default identity's or the private windows' network. A Tor-routed identity's download connected directly. | Fixed on this branch ([Hardening.md](Hardening.md) §3). |
| Persistent-profile tests wrote to the developer profile in the user's home directory. | Fixed on this branch: the test environment now redirects data and cache. |
| The `dns` tool did not link, so a full build failed. | Fixed on this branch. |
| Search suggestions for a named identity go through the default identity's session, with its cookies. | **Open.** `Services/Hub/ConnectionFromUI.cpp`, `fetch_search_suggestions`. |
| In builds with the update checker (`LADYBIRD_ENABLE_UPDATE_CHECK`, on by default in CMake), automatic checks are on by default, against Ladybird's feed. The plan asks for manual or user-scheduled checks. Ladyflock development builds turn the checker off. | **Open.** |
| A Tor-routed (ephemeral) identity's downloads are listed with the private windows' downloads. | **Open**, display only. |
| The Qt session store saves ordinary tabs without their identity, so a persistent named identity's tabs would be restored in the default identity, and a purge does not reach them. | **Open.** |
| `test-privacy-flags-reach-the-browser.py` and `test-webdriver-privacy.py` are not run by CMake or CI. | **Open.** They were run by hand. |
