# Identities, Partitioned Storage, and Navigation Handoffs (Milestone 2)

This document describes Ladyflock's **Milestone 2 (Identities and Storage)** architecture, SQLite schema migrations and quarantine rules, cross-identity navigation handoff contract, and verified test results. See also [NetworkEnforcement.md](NetworkEnforcement.md) for Milestones 0 and 1.

## 1. Architecture Overview

### 1.1 Pure Identity, Partitioning, and Handoff Core (`Libraries/LibPrivacy/`)
- **Identity Descriptors and Domain Rules (`Identity/Types.{h,cpp}`)**:
  - `DEFAULT_IDENTITY_ID = "default"`, `PRIVATE_IDENTITY_ID = "private"`.
  - `IdentityKind`: `Persistent`, `Temporary`, `Disposable`, `Offline`, `TorRouted`, `DirectNetwork`.
  - `is_valid_identity_id(StringView)`: validates canonical identity IDs (ASCII lowercase alphanumeric plus `-`/`_`, bounded length, no path traversal characters).
  - `IdentityDescriptor` and `DomainAssignmentRule` (`pattern` supporting exact hosts and `*.domain` wildcards) with strict IPC codecs rejecting out-of-range enum values.
- **Canonical Storage Partition Keys (`Storage/PartitionKey.{h,cpp}`)**:
  - Format: `p1|<serialized_origin>|<serialized_top_level_site>|<0|1>`, where the trailing bit encodes `has_cross_site_ancestor` (separating `A -> B -> A` nested contexts from first-party `A`).
  - `StoragePartitionKey::from_context(origin, top_level_origin, has_cross_site_ancestor)` excludes opaque origins (including ordinary `file://` opaque origins and sandboxed iframes without `allow-same-origin`) and missing/opaque top-level origins.
  - `classify_legacy_storage_key(raw_storage_key)` classifies pre-partitioning rows into `AlreadyPartitioned`, `MigrateToFirstParty` (valid `http`/`https` tuple origins), or `Quarantine` (`file://`, `"null"`, opaque, or unparseable keys).
- **Pre-DNS Navigation Handoff Evaluator (`Identity/NavigationHandoff.{h,cpp}`)**:
  - Pure decision engine (`NavigationHandoffEvaluator::evaluate`) executed in Hub before DNS or network dispatch in the source identity.
  - Computes route restrictiveness tiers (`route_restrictiveness_rank`: `Offline` > `TorRouted` > proxy/restricted > `Allow`/`DirectNetwork`).
  - Crossing from a more restrictive route to a less restrictive route returns `HandoffAction::RequiresConfirmation` until explicitly confirmed, always populating `displayed_destination_url` (because URL-embedded tokens survive handoffs).
  - Every cross-identity handoff (`HandoffAction::HandoffToNewContext`) mandates `open_fresh_context = true`, `drop_opener = true`, `strip_referrer = true`, `strip_credentials = true`, and `replay_post_body = false`.

### 1.2 Hub Session & Identity Ownership (`Libraries/LibWebView/`)
- **`BrowsingSession` (`BrowsingSession.{h,cpp}`)**:
  - Carries a `Privacy::IdentityDescriptor` (`identity()`, `identity_id()`, `is_ephemeral()`, `persists_history()`).
  - Each identity owns its own `CookieJar`, `StorageJar`, `PermissionManager`, `BlobURLStore`, `HSTSStore`, `WebContent` processes, and `RequestServer` instances.
- **`IdentityManager` (`IdentityManager.{h,cpp}`)**:
  - Owned by `WebView::Application` (`Application::identity_manager()`).
  - Registers the `"default"` session and shared browsing SQLite handle on startup, and creates/destroys named persistent and ephemeral identities on demand.
  - Persistent named identities receive identity-scoped `CookieJar`, `StorageJar`, and per-identity `<config>/Identities/<id>/Permissions.json`.
  - Ephemeral named identities (`Temporary`, `Disposable`, ephemeral `Offline`) receive in-memory `CookieJar`, `StorageJar`, `PermissionManager::create_ephemeral(...)`, and `HTTPDiskCacheMode::Disabled`.
  - `destroy_identity(id)` terminates all workers (`WorkerAgentManager::remove_workers_for_session`), retires all RequestServers (`RequestServerManager::retire_instances_for_session`), clears privacy policy state, and removes domain rules for that identity.
  - Evaluates domain assignment rules (`add_domain_rule`, `matching_domain_rule` with exact-match > longest-wildcard precedence) and delegates to `Privacy::NavigationHandoffEvaluator::evaluate`.
- **Process, Cache, Worker, and BroadcastChannel Isolation**:
  - **Renderer processes (`Application.cpp`, `CanonicalTraversable.cpp`, `CanonicalNavigable.cpp`, `ViewImplementation.cpp`, `HeadlessWebView.cpp`)**:
    - `open_page_for_new_tab(BrowsingSession&)` never reuses the default-session spare `WebContent` process for private or named identities.
    - `launch_child_frame_web_content_process(BrowsingSession&, ...)` binds out-of-process iframes to their parent's exact `BrowsingSession` (not just `IsPrivate`).
    - Closing the last view of a private session immediately clears `Application::m_private_session` so any newly opened private view gets a brand-new `BrowsingSession` (new `id`, empty cookies, empty storage, isolated worker namespace); closing the last view of a named ephemeral identity also tears down its workers and RequestServers immediately.
  - **Partitioned Web Storage (`WebContentPage.cpp`, `LibWeb/StorageAPI/StorageKey.cpp`, `LibWeb/Bindings/Rust/src/storage_api/{storage_key,storage_area_mirror}.rs`)**:
    - Hub computes canonical partitioned keys `(origin, top_level_site, has_cross_site_ancestor)` in `WebContentPage::canonical_storage_key` for both `localStorage` and `sessionStorage`.
    - `WebContent`'s Rust `StorageKey` and `StorageAreaMirror::registry_key_for` also include `(origin, top_level_site, has_cross_site_ancestor)`, preventing same-process `A -> B -> A` iframes from reusing first-party `A`'s in-process storage mirror or receiving first-party `storage` events.
  - **`SharedWorker` & `BroadcastChannel` (`WorkerAgentManager.{h,cpp}`, `WebContentPage.cpp`, `WebContentClient.{h,cpp}`, `WebWorkerClient.cpp`)**:
    - `SharedWorkerKey` partitions by `(session_id, storage_key, url, name, top_level_site, has_cross_site_ancestor)`.
    - `WorkerAgentManager::post_broadcast_channel_message` filters every destination `WebContentClient` and `WebWorkerClient` by `session_id`, `storage_key`, `top_level_site`, and `has_cross_site_ancestor`.
  - **`RequestServer` Caches (`RequestServerManager.{h,cpp}`)**:
    - Named persistent identities namespace their cache roots under `<cache_path>/Identities/<identity_id>/` (and site caches under `<cache_path>/Identities/<identity_id>/RequestServer/Sites/<hmac>`), keeping `"default"` paths unchanged.
    - Ephemeral/private sessions force `HTTPDiskCacheMode::Disabled` and never open or create disk cache directories.
    - Works in both site-specific (`--max-site-request-servers > 0`) and shared (`--max-site-request-servers=0`) RequestServer modes.
  - **UI-Owned History Scoping (`Services/Hub/HubClient.ipc`, `Services/Hub/View.cpp`, `UI/Qt/Application.{h,cpp}`, `UI/Qt/RemoteView.cpp`)**:
    - `Hub::ViewState` carries `identity_id` and `persists_history` from `BrowsingSession`.
    - `Ladybird::Application::history_store_for_identity(identity_id, persists_history)` routes UI history writes (`record_visit`, `update_title`, `update_favicon`) to the identity-scoped `HistoryStore` (or a disabled store when `!persists_history`).
  - **Pre-DNS Cross-Identity Navigation Handoff (`CanonicalNavigable::begin_navigation`)**:
    - Evaluates `IdentityManager::evaluate_navigation_handoff` on top-level navigations **before** any `NavigationLoader`, DNS lookup, or `WebContent` dispatch.
    - Fails closed if `RequiresConfirmation` is returned without confirmation (`ViewImplementation::on_confirm_identity_handoff`), and invokes `ViewImplementation::on_identity_handoff` on `HandoffToNewContext` while cancelling the source navigation immediately.

## 2. SQLite Schema Migrations and Quarantine

| Store | New Schema Version | Identity Scope & Legacy Migration Behavior |
|---|---|---|
| **`WebStorage` (`StorageJar.{h,cpp}`)** | **v3** (from v2) | Adds `identity_id TEXT NOT NULL DEFAULT 'default'` to primary key and creates `WebStorageLegacyQuarantine`. Atomic SQLite transaction backfills valid HTTP(S) legacy rows into `'default'` first-party keys (`p1|<origin>|<site>|0`), quarantines `"file://"`, `"null"`, opaque, and unparseable keys out of live web partitions with explicit reasons (`quarantined_legacy_entries()`, `clear_quarantined_legacy_entries()`), and rolls back cleanly on interruption (`set_migration_fault_hook_for_testing`). |
| **`Cookies` (`CookieJar.{h,cpp}`)** | **v4** (from v3) | Adds `identity_id TEXT NOT NULL DEFAULT 'default'` to primary key; migrates all legacy rows strictly to `'default'`. Scopes transient cookie-change notifications to views sharing the exact `CookieJar`. |
| **`History` (`HistoryStore.{h,cpp}`)** | **v5** (from v4) | Adds `identity_id TEXT NOT NULL DEFAULT 'default'` to `History` and `OmniboxEngagements` primary keys; migrates all legacy rows strictly to `'default'`. Keeps `referenced_favicon_hashes()` cross-identity so shared favicons are not garbage-collected while another identity references them. |

## 3. Recorded Verification Results

- **Git revision verified**: `b74ab771319` (branch `ladyflock`)
- **Host platform**: macOS 26.3.1 (`25D771280a`), Darwin 25.3.0, `arm64` (`Apple Silicon`)
- **Build configuration**: `Ladyflock/Build/ladyflock` (`RelWithDebInfo`, LLVM `clang++`, `ENABLE_LIBPRIVACY=ON`, `ENABLE_GUI_TARGETS=ON`, `LADYBIRD_ENABLE_UPDATE_CHECK=OFF`, sandboxing enabled)

### Build Command
```sh
cmake --build Ladyflock/Build/ladyflock --target TestLibPrivacy TestRequestServer TestLibWebView RequestServer WebRTCClient WebContent Hub ladybird -j$(sysctl -n hw.ncpu)
```
Result: succeeded cleanly (`exit 0`).

### Executed Test Suites (132 / 132 Passed)

| Suite | Command | Passed | Scope Verified |
|---|---|---|---|
| `TestLibPrivacy` | `Ladyflock/Build/ladyflock/bin/TestLibPrivacy` | 7 / 7 | Milestones 0–1 policy gate/compiler/IPC (4) + Milestone 2 identity descriptors, domain rules, `StoragePartitionKey` & legacy classification, `NavigationHandoffEvaluator` confirmation/sanitization (3) |
| `TestRequestServer --suite TestPrivacyNetworkGate` | `Ladyflock/Build/ladyflock/bin/TestRequestServer --suite TestPrivacyNetworkGate` | 2 / 2 | Default-closed startup, epoch monotonicity, live tightening, coordinator loss |
| `TestLibWebView --suite TestPrivacyMilestone0` | `LADYBIRD_SOURCE_DIR="$PWD/Ladyflock" Ladyflock/Build/ladyflock/bin/TestLibWebView --suite TestPrivacyMilestone0` | 2 / 2 | Milestone 0 HTTP & HTTPS allowed/denied with independent DNS and HTTP/HTTPS observers |
| `TestLibWebView --suite TestPrivacyOffline` | `LADYBIRD_SOURCE_DIR="$PWD/Ladyflock" Ladyflock/Build/ladyflock/bin/TestLibWebView --suite TestPrivacyOffline` | 5 / 5 | Milestone 1 Offline mode, IPv4/IPv6/WS/streams/cache/WebRTC/sandbox restart |
| `TestLibWebView --suite TestPrivacyMilestone2` | `LADYBIRD_SOURCE_DIR="$PWD/Ladyflock" Ladyflock/Build/ladyflock/bin/TestLibWebView --suite TestPrivacyMilestone2` | 5 / 5 | (1) Same-origin embed positive & cross-site negative controls, `A -> B -> A` cross-site ancestor isolation, opaque sandboxed iframe & `data:` exclusion, partitioned third-party cookies; (2) Two persistent identities isolated across `WebContent`, `RequestServer` (both site-specific AND `--max-site-request-servers=0` shared mode), HTTP disk caches, cookies, storage, permissions, and history; (3) `SharedWorker` & `BroadcastChannel` isolation by identity + top-level site, plus closing the last private view tearing down session state; (4) Legacy profile (`WebStorage` v2, `Cookies` v3, `History` v4) interrupted-migration atomic rollback, idempotent resume, `'default'`-only ownership, and `"file://"` / `"null"` quarantine + clearing; (5) Pre-DNS cross-identity navigation handoff (`Offline -> Allow`) confirmation denial & confirmed fresh-context handoff with zero source DNS/HTTP |
| `TestLibWebView` regression suites | `--suite TestStorageJar` (17), `TestCookieJar` (7), `TestHistoryStore` (38), `TestPermissionManager` (30), `TestSharedWorkerHost` (1), `TestRequestServerClientSessions` (2), `TestRequestServerDiskCaches` (2), `TestBackgroundNetworking` (5), `TestExtensionCore` (9) | 111 / 111 | No regressions across storage jars, cookies, history, permissions, shared workers, RequestServer sessions/disk caches, background networking, and extensions |

### Known Gaps & Untested Paths
- **Interactive Qt chrome UI for identity creation/switching and handoff confirmation dialogs**: Hub IPC (`Hub::ViewState::identity_id`, `persists_history`), `UI/Qt` history scoping (`Application::history_store_for_identity`), and `ViewImplementation` handoff hooks (`on_confirm_identity_handoff`, `on_identity_handoff`) are wired and verified via headless tests, while dedicated Qt chrome widgets (identity picker/indicator badges, interactive handoff prompt modal, quarantine recovery dialog) are deferred to the UI polish phase.
- **Persistent per-identity HSTS storage & IndexedDB/CacheStorage/ServiceWorker disk partitioning**: Named identities currently use isolated in-memory `HSTSStore` instances so HSTS observations never cross identities; `localStorage` and `sessionStorage` are partitioned end-to-end by `(origin, top_level_site, has_cross_site_ancestor)`, while IndexedDB disk-level partitioning remains scoped to future storage work.
- **Same-process `BroadcastChannel` in `A -> B -> A`**: Cross-identity and cross-site `BroadcastChannel` isolation are enforced in Hub (`WorkerAgentManager::post_broadcast_channel_message`), while intra-process `BroadcastChannel` delivery inside `WebContent` still keys on `obtain_a_storage_key_for_non_storage_purposes` (origin-only) when two frames share a single renderer process.
- **Linux / Windows builds**: Not compiled or executed on this macOS arm64 host.
