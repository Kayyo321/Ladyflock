# Working on Ladyflock

Ladyflock is a standalone downstream browser based on Ladybird. Its first goal is to run a small, tested subset of **unmodified Firefox WebExtensions** on Ladybird's engine. Extension support is not implemented at the initial baseline. Read the current code and tests before assuming any milestone in the [README](README.md) has shipped.

## Start here

1. Run `git status --short` and identify the current branch and changed files. Preserve existing user edits. Do not reset, clean, amend, or force-push to make a checkout look tidy.
2. Read the closest code, tests, and applicable directory instructions before editing. Identify the process that owns the behavior and the smallest test that proves it.
3. Keep changes scoped and reviewable. Use the project's [coding style](Documentation/CodingStyle.md) and follow the local C++23, Rust, or generated-binding conventions. Do not move code between languages or reformat unrelated files as part of a feature.
4. Keep inherited copyright notices and the [license](LICENSE). Leave logos, product naming inside the browser, release infrastructure, and upstream metadata alone unless the task explicitly covers them.

## Where work belongs

| Area | Start in | What to verify |
| --- | --- | --- |
| DOM, navigation, frame or script lifecycle | `Libraries/LibWeb/`, `Tests/LibWeb/` | Standard web behavior, script timing, frame identity and navigation cleanup. |
| JavaScript realms, wrappers and built-ins | `Libraries/LibJS/` and LibWeb bindings | Realm identity, object ownership and GC lifetime. Keep browser extension APIs out of ordinary page globals. |
| Browser-side coordination, tabs, profiles and renderer IPC | `Libraries/LibWebView/`, `Services/WebContent/`, `Libraries/LibIPC/` | Which process owns policy, and which messages or handles cross the boundary. |
| Browser UI | `UI/Qt/` and `Libraries/LibWebView/` | Put shared behavior in the browser layer; keep frontend code focused on presentation and interaction. |
| Fetch, network service and native content blocking | `Libraries/LibWeb/Fetch/`, `Services/RequestServer/`, `Libraries/LibWeb/ContentBlocker/` | Whether the intended requests are handled before they leave the browser, including applicable redirects and cache paths. Native filtering is not a WebExtensions API. |
| Real browser and helper-process integration | `Tests/LibWebView/` | Use fresh test configuration and local fixtures; see `HeadlessTest.*` and `TestFavicon.cpp`. |

For the process model, use [Porting](Documentation/Porting.md) and [Process Architecture](Documentation/ProcessArchitecture.md) alongside the current code. Historical documentation may lag the implementation.

## WebExtensions: scope and boundaries

- **First milestone:** load a small local, unpacked add-on without altering its manifest or script; run its declared content script only on explicitly matching, permitted pages. Compare the same fixture with Firefox before claiming parity. A working loader is not XPI signing, Mozilla Add-ons integration, general API support, or uBlock Origin compatibility.
- Use a separate extension realm with an explicit DOM bridge. Page JS must not see extension globals or call privileged APIs. Extension resources must not become accessible to arbitrary pages just because they exist in the package. Do not substitute page-world script injection for isolated execution.
- Enforce permissions at the privileged operation, using browser-owned extension, profile, document, and frame identity. Treat renderer messages, page data, manifests, archives, and extension code as untrusted input. Verify payloads and reject missing, stale, or unauthorized context. Never add a bypass to make a demo pass.
- Handle navigation, frame destruction, extension disable/uninstall, and permission revocation. Keep asynchronous results bound to the originating document and profile. Keep private browsing out of scope until there is an explicit policy and test coverage.
- Implement the smallest correct surface for the target add-on. Record supported manifest keys and API semantics, including event ordering and errors. Reject unsupported requirements clearly; do not silently grant privileges or return fake success from a stub. Use Firefox as the behavioral reference when its behavior differs from Chromium.
- Treat package installation, privileged origins, native messaging, network interception, and browser-wide permissions as separate review boundaries. Avoid dispatching network requests, opening external pages, or using any real user profile in test fixtures.

### First compatibility test

Serve two local pages: one whose URL matches the fixture manifest and one that does not. Check that only the permitted page receives a visible DOM marker and that page JavaScript cannot read a plain global set inside the content-script realm. Run the same manifest and script on Firefox. As capabilities expand, add negative coverage for denied hosts, different frames or profiles, navigation and revocation. Assert the behavior, not just that an install or IPC call returned success.

## Build and verify

Read the [platform prerequisites](Documentation/BuildInstructionsLadybird.md), [build options](Documentation/AdvancedBuildInstructions.md), and [test guide](Documentation/Testing.md). Inherited target names still use Ladybird. Use a clean build directory and an isolated test profile when launching a development browser.

For a Ladyflock development build, disable the inherited Ladybird release checker at configure time; this also disables Sparkle. This is separate from Git source sync.

```sh
cmake --preset Release -B Build/ladyflock -DLADYBIRD_ENABLE_UPDATE_CHECK=OFF
cmake --build Build/ladyflock --target TestLibWebView
LADYBIRD_SOURCE_DIR="$PWD" Build/ladyflock/bin/TestLibWebView --suite TestFavicon
```

- Use a focused LibWeb regression for web-facing behavior, a focused runtime test for JS semantics, and a real LibWebView or WebDriver case when the change crosses process boundaries. The favicon suite is a smoke test, not extension coverage.
- For a docs-only change, run `git diff --check` and check relative links and commands against the tree. For code, run the smallest relevant test first and expand coverage with the impact of the change.
- A skipped inherited GitHub Actions job is not build evidence. The baseline upstream CI targets upstream branches and repository names. Cite the exact commands, tested revision, platform, results, and blockers; distinguish not run from passed.

## Git and upstream sync

- The only permanent branch is `ladyflock`. `origin` is Ladyflock; `upstream` is Ladybird. Verify the destination before pushing. Do not push to upstream or import its branches, tags, releases, issues, or PRs.
- Use [UPSTREAM.md](UPSTREAM.md) for source updates. Review a pinned revision in a temporary sync branch; keep real merge ancestry, not squash or rebase. Audit inherited CI, updater, and release changes before accepting them. Do not automatically sync or release.
- Keep upstream merges separate from product changes. Do not introduce unrequested commits, PRs, deployments, or other remote changes. When a push is requested, verify the remote branch and report the commit and validation that actually completed.
