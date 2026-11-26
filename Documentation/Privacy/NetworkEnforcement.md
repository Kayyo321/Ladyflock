# Privacy Network Enforcement (Milestones 0 and 1)

This document describes Ladyflock's network privacy enforcement architecture for **Milestone 0 (First Request)** and **Milestone 1 (Offline)**, its platform boundaries, and the verified test results.

## 1. Architecture Overview

### 1.1 Pure Policy Core (`Libraries/LibPrivacy/`)
`LibPrivacy` is a standalone, UI-free policy core gated by `ENABLE_LIBPRIVACY` (default `ON`):

- **Versioned Policy Types (`Policy/Types.h`, `Policy/Types.cpp`)**:
  - `CURRENT_POLICY_SCHEMA_VERSION = 1`
  - `NetworkMode` (`Allow`, `Offline`), `CachePolicy` (`Normal`, `FreshOnlyNoRevalidation`, `Disabled`), `DNSRoutePolicy` (`Default`, `Denied`), `WebRTCPolicy` (`Allowed`, `Disabled`), `PolicyDecision`, `PolicyReason`.
  - `NetworkPolicy` (fail-closed by default), `PolicySnapshot` (schema version, bound `session_id`, monotonic `epoch`, `NetworkPolicy`), and `PolicyInstallAck`.
  - Strict IPC codecs rejecting out-of-range enum values at decode time.
- **Deterministic Compiler (`Policy/Compiler.h`, `Policy/Compiler.cpp`)**:
  - Compiles `PolicyInput` into an immutable `PolicySnapshot` by intersecting administrative ceiling $\cap$ identity requirement $\cap$ user preference.
  - Rejects unsupported schema versions, invalid session IDs, zero epochs, or contradictory capabilities (for example, `Offline` claiming open DNS or WebSockets).
- **Fail-Closed Gate (`Policy/NetworkGate.h`, `Policy/NetworkGate.cpp`)**:
  - Starts in `State::Uninitialized` (denies all requests, DNS, WebSockets, AIA fetches, speculative connections, WebRTC, and cache reads).
  - Enforces strict schema version checks, monotonic epochs, and session binding once bound.
  - Tracks `State::CoordinatorLost` when the Hub control connection drops, closing all admissions permanently for that helper.
  - Exposes use-time epoch revalidation (`is_epoch_current`) so async DNS/proxy/cookie callbacks and retries cannot act on stale allow decisions after a tightening transition.
- **Thin Hook Adapter (`Hooks/RequestServerHooks.h`)**:
  - Single-point helper checks invoked by `RequestServer`.

### 1.2 Hub Coordinator (`Libraries/LibWebView/PrivacyPolicyCoordinator.{h,cpp}`)
The Hub process is the sole policy coordinator:

- Compiles per-`BrowsingSession` snapshots with monotonically increasing epochs.
- CLI flags in `WebView::Application`:
  - `--offline`: starts in `Privacy::NetworkMode::Offline`.
  - `--privacy-network-mode=<allow|offline>`: selects initial startup mode.
- Synchronously installs `PolicySnapshot` via `RequestServerControl::InstallPrivacyPolicy` inside `RequestServerManager::launch_process()` **before** connecting any data clients (`ui_client` or renderer/worker clients). If installation or acknowledgement fails, the helper is terminated.
- **Live Transitions (`set_network_mode` / `apply_policy_input`)**:
  1. Immediately switches Hub-side admission checks to the tighter snapshot before notifying helpers.
  2. Cancels Hub-initiated background networking (content blocker list updates) and forcefully terminates any live `WebRTCClient` helper in the session whose network access exceeds the new policy.
  3. Synchronously pushes the new `PolicySnapshot` to every `RequestServer` instance in the session and verifies `PolicyInstallAck` (`accepted && committed_epoch == target_snapshot.epoch`). Unresponsive or rejecting helpers are forcefully terminated.
  4. When relaxing `Offline -> Allow`, replaces any `RequestServer` launched under an OS-level `--disable-network` sandbox (`RequestServerManager::restart_network_sandboxed_instances_for_session`) so it regains a network-capable sandbox cleanly.
  5. Propagates default-session mode changes over `HubClient` IPC (`app_privacy_network_mode_changed` and `Hub::InitialState::privacy_network_mode`) so the UI process can gate UI-owned network triggers (such as macOS Sparkle `SoftwareUpdater`).

### 1.3 Enforcing Processes and Covered Browser IP Paths
- **`RequestServer` (`Services/RequestServer/`)**:
  - **Default-closed startup**: `ControlConnectionFromClient` resets `privacy_network_gate()` on construction; requests before `InstallPrivacyPolicy` fail immediately with `Requests::NetworkError::BlockedByPrivacyPolicy`.
  - **HTTP(S), IPv4/IPv6, redirects, retries, and disk cache**: Checked on initial admission, after cache miss / revalidation detection, before DNS/proxy lookup and in async DNS/proxy callbacks, before and after cookie retrieval, before connect/fetch, and before every retry path. Eligible fresh disk cache entries (`CachePolicy::FreshOnlyNoRevalidation`) are served without network revalidation; stale/no-cache/uncached entries fail closed with zero DNS or TCP traffic.
  - **DNS**: Custom UDP/TCP/DoT DNS (`DNSStreamCurl`), proxy host resolution (`Resolver::resolve_proxy_host`), `ConnectionFromClient::resolve_host`, `ControlConnectionFromClient::resolve_host_with_system_resolver`, and Hub-side `RequestControlClient::resolve_host_with_system_resolver` (checked before dispatching `getaddrinfo` to the worker pool).
  - **WebSockets**: Gated at connect, proxy/DNS callbacks, open, and send; active and pending WebSockets are closed/failed during quiesce.
  - **Speculative connections (`ensure_connection`)**: Gated before DNS lookup or socket creation.
  - **AIA certificate intermediate fetches**: Gated at `fetch_aia_intermediate`, `look_up_aia_host`, and `start_aia_fetch`; active AIA fetches are aborted during quiesce.
  - **Live quiesce**: Tightening aborts active and revalidation requests, tears down open WebSockets and AIA fetches, resets both libcurl multi handles (`reset_curl_multi_handles()`) to close pooled keep-alive sockets immediately, and flushes DNS, proxy-resolution, and Alt-Svc caches (`Resolver::quiesce_for_privacy_policy()`).
  - **Coordinator loss**: `ControlConnectionFromClient::die()` marks `CoordinatorLost` and quiesces all active network work.
  - **OS sandbox defense-in-depth**: `RequestServer --disable-network` applies kernel-level network denial (macOS Seatbelt `Sandbox::NetworkAccess::Denied`; Linux Seccomp omits `allow_network()`) when launched in `Offline` mode while preserving IPC and disk cache access.
- **`WebRTCClient` (`Libraries/LibWebView/HelperProcess.cpp`)**:
  - Launched with `--disable-network` whenever the session policy denies WebRTC network access (in addition to configured proxies).
  - Unrestricted `WebRTCClient` helpers are forcefully terminated during live tightening to `Offline`.
- **Browser Services**:
  - Content blocker list updates (`Application.cpp`), update checker (`UpdateCheck.cpp`), AMO catalog client (`Extensions/CatalogClient.cpp`), report submission (`ReportSubmission.cpp`, `Services/Hub/ConnectionFromUI.cpp`), Hub search suggestions (`Services/Hub/ConnectionFromUI.cpp`), and UI macOS Sparkle `SoftwareUpdater` (`UI/Qt/Application.cpp`, `UI/Qt/SoftwareUpdaterMac.mm`).

## 2. Platform Enforcement Matrix

| Platform | Userspace `LibPrivacy` + Hub + `RequestServer` Gate | `RequestServer --disable-network` OS Sandbox | `WebRTCClient --disable-network` OS Sandbox | Status |
|---|---|---|---|---|
| **macOS (arm64)** | Enforced (default-closed, epoch-bound, live quiesce) | macOS Seatbelt (`Sandbox::NetworkAccess::Denied`) | macOS Seatbelt (`--disable-network`) | **Built and verified** |
| **Linux** | Enforced (shared cross-platform code) | Linux Seccomp (omits `allow_network()`) | Linux Seccomp (`--disable-network`) | **Implemented; not executed on this macOS host** |
| **Windows** | Enforced (shared cross-platform code) | Not implemented (`SandboxUnimplemented.cpp`) | Not implemented | **Untested; userspace-only enforcement** |

### Known Threat-Model Boundaries
- **Userspace-only vs OS-sandboxed enforcement**: On macOS and Linux, launching `RequestServer` or `WebRTCClient` in `Offline` mode adds OS-level kernel network denial on top of userspace policy checks. When transitioning `Allow -> Offline` live, existing `RequestServer` instances enforce userspace quiesce and gate denial while any newly launched or restarted `RequestServer` (and `WebRTCClient`) enters the `--disable-network` OS sandbox; relaxing `Offline -> Allow` restarts sandboxed `RequestServer` instances.
- **External applications / OS URL handlers**: Handing a URL to an external OS application is outside the browser's network stack and requires user confirmation.
- **Sparkle auto-updater**: Gated in `UI/Qt/Application.cpp` and `UI/Qt/SoftwareUpdaterMac.mm`, but `LADYBIRD_ENABLE_UPDATE_CHECK` / `LADYBIRD_ENABLE_SPARKLE` are `OFF` in this build configuration, so Sparkle runtime invocation against a live appcast was not executed here.

## 3. Recorded Verification Results

- **Git revision verified**: `c07675fcfa2` (branch `ladyflock`)
- **Host platform**: macOS 26.3.1 (`25D771280a`), Darwin 25.3.0, `arm64` (`Apple Silicon`)
- **Build configuration**: `Ladyflock/Build/ladyflock` (`RelWithDebInfo`, LLVM `clang++`, `ENABLE_LIBPRIVACY=ON`, `ENABLE_GUI_TARGETS=ON`, `LADYBIRD_ENABLE_UPDATE_CHECK=OFF`, sandboxing enabled)

### Build Command
```sh
cmake --build Ladyflock/Build/ladyflock --target TestLibPrivacy TestRequestServer TestLibWebView RequestServer WebRTCClient Hub ladybird -j$(sysctl -n hw.ncpu)
```
Result: succeeded cleanly (`exit 0`), including `Ladybird.app/Contents/MacOS/Ladybird`, `Hub`, `RequestServer`, and `WebRTCClient`.

### Executed Test Suites (95 / 95 Passed)

| Suite | Command | Passed | Scope Verified |
|---|---|---|---|
| `TestLibPrivacy` | `Ladyflock/Build/ladyflock/bin/TestLibPrivacy` | 4 / 4 | Default-closed gate, compiler precedence/validation, monotonic epochs, session binding, tightening detection, IPC roundtrip & invalid enum rejection |
| `TestRequestServer --suite TestPrivacyNetworkGate` | `Ladyflock/Build/ladyflock/bin/TestRequestServer --suite TestPrivacyNetworkGate` | 2 / 2 | Default-closed startup before `InstallPrivacyPolicy`, bad schema/stale epoch/session mismatch rejection, live tightening, `CoordinatorLost` abort |
| `TestLibWebView --suite TestPrivacyMilestone0` | `LADYBIRD_SOURCE_DIR="$PWD/Ladyflock" Ladyflock/Build/ladyflock/bin/TestLibWebView --suite TestPrivacyMilestone0` | 2 / 2 | HTTP & HTTPS (trusted test CA) positive (allowed) & negative (denied) controls with independent UDP DNS and HTTP/HTTPS observers, local page rendering, `SIGKILL` RequestServer restart, site-specific RequestServer |
| `TestLibWebView --suite TestPrivacyOffline` | `LADYBIRD_SOURCE_DIR="$PWD/Ladyflock" Ladyflock/Build/ladyflock/bin/TestLibWebView --suite TestPrivacyOffline` | 5 / 5 | Startup `--offline` + `--disable-network` sandbox; browser services (content blocker updates, AMO catalog, report submission); IPv4 & IPv6; HTTP 302 redirects; speculative `ensure_connection`; custom UDP DNS & OS `SystemDNS`; WebSockets + live WS teardown; live revocation of open HTTP chunked stream with server-observed EOF; `Offline -> Allow` sandbox restart; fresh disk cache hit without revalidation vs `no-cache`/miss denial; `WebRTCClient` `--disable-network` launch gating & live forceful termination |
| `TestRequestServer` regression suites | `--suite TestSiteBinding`, `TestContentCodingRetry`, `TestNetworkUsage`, `TestCookieResponses`, `TestDiskCacheWaiters`, `TestExportImport`, `TestRequestTimingInfo` | 63 / 63 | No regressions across site binding, retries, traffic accounting, cookies, disk cache waiters, export/import, and timing |
| `TestLibWebView` regression suites | `--suite TestBackgroundNetworking`, `TestRequestServerClientSessions`, `TestRequestServerDiskCaches`, `TestFavicon`, `TestExtensionCore` | 19 / 19 | No regressions across background networking policy, session isolation, site disk caches, favicons, and extensions |

### Untested / Deferred Paths
- **Linux sandboxed build (`SandboxLinux.cpp`)**: Not compiled or executed on this macOS arm64 host.
- **Windows build**: Not compiled or executed on this macOS arm64 host; Windows lacks an OS-level network sandbox.
- **macOS Sparkle (`LADYBIRD_ENABLE_SPARKLE=ON`) & `UpdateChecker` release feed (`LADYBIRD_ENABLE_UPDATE_CHECK=ON`)**: Gated in code, not live-executed in this `LADYBIRD_ENABLE_UPDATE_CHECK=OFF` build.
- **DNS-over-TLS (`DNSOverTLS`) & AIA certificate chain live server tests**: Gated in `DNSStreamCurl.cpp` and `ConnectionFromClient.cpp` (`fetch_aia_intermediate`, `look_up_aia_host`, `start_aia_fetch`), covered by the same `NetworkGate` and quiesce paths, not yet backed by dedicated live DoT/AIA observer fixtures.
- **QUIC / HTTP/3 UDP transport**: libcurl multi handles and `Request` gate block HTTP/3 transfers via `NetworkGate` and `--disable-network` Seatbelt/Seccomp; no dedicated local HTTP/3 observer server was run in this milestone.
