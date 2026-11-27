# Identity Purge, Managed Policy and Download Routing (Milestone 5, first increments)

This document covers the first increments of Ladyflock's **Milestone 5 (Hardening and optional systems)**:

- an emergency purge of a browsing identity, with recovery after an interruption;
- a managed policy that an administrator can use to bind every identity's network;
- downloads that use the network of the identity that started them.

See [NetworkEnforcement.md](NetworkEnforcement.md), [IdentitiesAndStorage.md](IdentitiesAndStorage.md), [ProxyRouting.md](ProxyRouting.md) and [SiteControls.md](SiteControls.md) for Milestones 0–4. [PlanStatus.md](PlanStatus.md) compares the whole privacy plan with the code.

**Milestone 5 is not complete.** Its gate asks each feature to state its platform scope and failure behavior, encrypted profiles to have recovery tests, and third-party code to have capability, revocation and rollback tests before it is loaded. The features below state theirs. Encrypted stores, module signing, encrypted DNS variants, fingerprint noise and remote enterprise distribution are not implemented (section 5). Ladyflock loads no third-party privacy code.

## 1. Purging an identity

`IdentityManager::purge_identity(id)` deletes a named identity and everything Ladyflock stored for it. There is no menu item or settings page for it yet; it is called from code.

| Step | What happens | If it fails |
|---|---|---|
| Journal | The identity is added to `<data>/IdentityPurges.json`, written to a temporary file, synced and renamed into place, before anything is deleted. | Nothing is deleted and the purge returns an error. |
| Stop the identity | Its views are force-closed. Its workers, RequestServers and downloads stop. Its cookie jar and storage jar are replaced by memory-only ones, which makes the old ones flush first, and its permission manager stops writing to disk. | — |
| Cookies, web storage | Its rows are deleted from the browsing database (`Ladybird.db`). | The step is reported and the purge continues. |
| History | Its rows are deleted from the UI's `History.db` (history and address-bar engagements), through a separate SQLite connection. | Reported, purge continues. |
| Configuration | `<config>/Identities/<id>/`, which holds its permissions, is removed. | Reported, purge continues. |
| Caches | `<cache>/Identities/<id>/`, its HTTP caches including the per-site ones, is removed. | Reported, purge continues. |
| Compaction | Both databases are rewritten with `VACUUM`, and their write-ahead logs are emptied with `PRAGMA wal_checkpoint(TRUNCATE)`. Without this, deleted rows stay readable on disk: removing the step makes the test fail. | Reported, purge continues. |
| Clear the journal | The identity leaves the journal only if every step succeeded. | It stays in the journal. |

**Recovery.** While an identity is in the journal, `create_identity` refuses it, so it cannot reattach to whatever is left. At the next start, `Application` runs every journaled purge again before any identity can be created. A journal that exists but cannot be read keeps every named identity closed and is left in place for repair. The default and private identities cannot be purged.

**What it does not reach:**
- Copies outside the browser's own stores: OS backups, snapshots, swap, and files the user saved. The stores are not encrypted, so this is deletion, not cryptographic erasure.
- Favicons, which are not stored per identity.
- What the UI process still holds in memory for the identity, until it restarts.
- Downloads the user had already saved. Ones in progress are cancelled.
- The Qt UI's saved session (`Sessions.db`), which has one store for ordinary windows and one for private ones and does not record a tab's identity. A persistent named identity's open tabs may be saved there. The headless tests cannot exercise this.

**Evidence (`TestLibWebView --suite TestIdentityPurge`, 3 tests).** Markers are looked for in the raw bytes of `Ladybird.db`, `History.db` and their `-wal`, `-shm` and `-journal` files, and after the purge in every file under the test's profile, not through the stores that wrote them.

- **Positive controls:** before the purge, each identity's markers are on disk, and so are its permissions file and cache.
- **Purge:**
  - Afterwards, the purged identity's markers, directories and journal entry are gone. The check runs again after its session and pages have gone, which is when a store still attached to disk would have written back.
  - The other identity keeps its cookies, storage, history and permissions.
  - An identity created again under the same name starts empty.
- **Interruption:**
  - A purge whose cookie step fails leaves the cookies on disk, and the identity is refused until the next start finishes the purge.
  - A purge journaled just before the browser stopped is completed at the next start.
- **Unreadable journal:** named identities stay closed.
- **Mutations:** each of these makes the suite fail:
  - leaving out the compaction;
  - leaving the stores attached;
  - letting a pending identity be created again;
  - not resuming at startup.

## 2. Managed policy

An administrator can bind every identity with a JSON file in a protected location. Version 1:

```json
{
    "version": 1,
    "network": { "mode": "offline" },
    "stripTrackingParameters": true
}
```

`network.mode` is `allow`, `offline` or `proxy-only`. For `proxy-only`, `network.proxy` names the route, in the same form as `--privacy-proxy` (an IP literal, such as `socks5h://10.0.0.1:1080`).

| | |
|---|---|
| Location | macOS: `/Library/Managed Preferences/org.ladyflock.browser/policy.json`. Linux and BSD: `/etc/ladyflock/policies/policy.json`. Other platforms have no location, so no policy is read. No option or environment variable can name another file. |
| Trust | The file and its directory must belong to root, and neither may be writable by its group or by others. The file must be a regular file. A symbolic link is examined as itself and refused, and the file read must be the one that was checked. |
| When | Once, at startup, before any policy is compiled. There is no live reload. |
| Effect | `network` becomes the administrative ceiling of every identity: the default one, named ones and Tor-routed ones. Command-line options, an identity's own requirement, `set_network_mode` and `apply_policy_input` (even with a ceiling of its own) can narrow it but never widen it. A managed route and a different route required by an identity (such as Tor) leave that identity with no network. `stripTrackingParameters: true` turns stripping on whatever the command line says. `about:privacy` shows whether a policy is in force. |
| Failure | No file: no managed policy. A file that is untrusted, unreadable, too large (over 64 KiB), malformed, of another version, or has any field version 1 does not know: **every identity has no network**, because the administrator's restriction can't be known. The reason is logged. |

**Scope:**
- The macOS location follows the plan, but macOS management tools deliver configuration profiles as property lists, which this does not read.
- There is no signature check, freshness check or remote distribution.
- The managed route is used without per-site stream isolation. A command-line route with `--privacy-proxy-isolation` alongside it counts as a different route, and closes the network.
- The real protected locations, owned by root, were not exercised: the tests point an in-process test application at a file owned by the account running them.
- Windows has no location, and the loader refuses any policy file there because it has no ownership check yet. None of this has been built on Windows.

**Evidence:**
- **`TestLibPrivacy` (`TestPrivacyManagedPolicy`, 4 tests):**
  - the ownership and permission rules, checked as pure data;
  - the format's rejections: unknown fields, versions and modes, a missing or stray proxy, a route by host name, wrong types and malformed JSON;
  - loading real files, with rejection for a file or directory anyone can write, a truncated file, an unknown field and a symbolic link, each closing the network;
  - the ceiling in the compiler.
- **`TestLibWebView --suite TestPrivacyManagedPolicy` (4 tests), observed by an independent server:**
  - **Positive control:** without a policy, a page's request reaches the server.
  - **Offline policy:** with `--privacy-network-mode=allow` on the command line, neither the default identity nor a named one reaches the server, including after three attempts to open the network through the coordinator.
  - **Managed route:** it carries the page's request, and the destination sees no direct connection.
  - **Untrusted file:** a world-writable file and an unknown field each close the network.
- **Mutations:** each of these makes a test fail:
  - not bounding `apply_policy_input`;
  - accepting a writable file;
  - failing open on a rejected policy.

## 3. Downloads use their identity's network

**The defect.** The browser process made every download's requests through the default identity's RequestServer, or through the private windows' one for a private tab. An ephemeral named identity, such as a Tor-routed one, counts as private, so its downloads went through the private windows' **direct** connection. With the old ownership logic restored, the test sees the destination receive a direct connection from the Tor identity's download.

**The fix.** A download now records the identity of the tab or page that started it (`FileDownloader::owner_for`). Every request it makes, including redirects, ranged segments and restarts, uses that identity's RequestServer, and so its route.
- If the identity is gone, the download fails rather than use another identity's network.
- A named identity's downloads are not written to the download store, which has no identity column, so they cannot be resumed after a restart over the default identity's network.
- Purging an identity cancels its downloads. Closing the private windows no longer cancels a named identity's downloads.

**Evidence (`TestLibWebView --suite TestPrivacyProxyRoute`, `a_download_uses_the_network_of_the_identity_it_belongs_to`):**
- **Positive control:** a default-identity download reaches the destination directly.
- **Routed:** a Tor-routed identity's download completes over its SOCKS route, which resolves the name. Neither the destination nor the DNS observer sees anything.
- **Orphaned:** a download for a destroyed identity fails without opening a stream.

**Not covered:**
- The test starts its downloads through `download_file`, as saving a link does. Downloads that a page's own response turns into (`adopt_download`) take their identity from the same `owner_for`, but no test exercises that path.
- The download list for a window is still chosen by private or not, so a Tor-routed identity's downloads are listed with the private windows' downloads.
- Download history per identity, a memory-only download mode, checksums, executable warnings and metadata stripping.

## 4. Fixes found on the way

- **Test profiles.** The headless test environment redirected only `XDG_CONFIG_HOME`. Test applications that keep a persistent profile, such as the Milestone 2 identity tests, therefore opened their databases and caches in the developer profile under `~/Library` on macOS (`~/.local/share` and `~/.cache` on Linux). It now redirects `XDG_DATA_HOME` and `XDG_CACHE_HOME` into the test's directory as well. The purge tests check this before they start.
- **The `dns` tool.** It compiles RequestServer's DNS stream connector, which since Milestone 1 checks the privacy gate, but did not link the gate, so a full build failed. It now links the gate and opens it itself, since it runs without a Hub.

## 5. Not done in Milestone 5

| Project | Why not yet |
|---|---|
| Encrypted stores | Needs a key hierarchy (OS keychain or a password-derived key), an encrypted SQLite or per-record AEAD across five stores, locking and recovery tests. The plan requires recovery tests before shipping it. |
| Module signing and third-party modules | No module interface exists yet (plan Part II §3). Ladyflock loads no third-party privacy code, so the capability, revocation and rollback tests the gate requires have nothing to apply to. |
| Encrypted DNS variants | DoT exists. DoH and ODoH are not implemented. |
| Experimental fingerprint noise | The plan makes noise an experiment on top of consistent fingerprint profiles, which are themselves deferred (see [SiteControls.md](SiteControls.md)). |
| Enterprise distribution | Only a local protected file. No signatures, freshness, locked-setting UI, allowlists or management socket. |
| File handling beyond routing | Checksums, executable warnings, drive-by download limits, metadata stripping and sanitized uploads. |

## 6. Verification

- **Revision:** branch `privacy-milestone-5`, based on `ladyflock` `529923ae0b2`. Host: macOS 26.3.1 on Apple Silicon, Release preset with `LADYBIRD_ENABLE_UPDATE_CHECK=OFF`.
- **New and changed suites:**
  - `TestLibPrivacy`: 21/21, including `TestPrivacyManagedPolicy` 4/4.
  - `TestLibWebView --suite TestIdentityPurge`: 3/3.
  - `--suite TestPrivacyManagedPolicy`: 4/4.
  - `--suite TestPrivacyProxyRoute`: 5/5, including the download case.
- **Mutations:** 8 of 8 caught.
- **Regression runs (all passed):**
  - full `TestLibWebView`, run from `Tests/LibWebView`: exit 0;
  - full `TestLibWeb`, run from `Tests/LibWeb`: exit 0;
  - `TestUIQt` offscreen: exit 0, no failures;
  - `test-webdriver-privacy.py`, light at 800 px and dark at 320 px, including the new managed-policy row;
  - `test-privacy-flags-reach-the-browser.py` against the `Ladybird` binary: 3/3.
- **The user's own profile:** before and after each run, the developer profile's data, cache and preferences directories were listed with their modification times. They were unchanged.
- **Platforms:** Linux and Windows were not built or run.
