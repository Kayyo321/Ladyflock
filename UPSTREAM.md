# Updating Ladyflock from Ladybird

Ladyflock keeps Ladybird’s Git history. The first imported Ladybird commit was `be5e3ce8585ddb907c17fbdafcea11027a32ae6f`. Later updates can follow Ladybird beyond that revision or its release stage. `ladyflock` is the fork’s only permanent branch. `upstream/master` is a local reference to Ladybird’s source. Importing source in Git and shipping an update to installed browsers are separate operations.

## Pin the inputs

Use a POSIX shell on a macOS or Linux development host. Run the blocks in order in the same shell. Stop if a command fails and investigate. The examples use a dedicated clone so work in progress, builds and profiles in development checkouts stay separate.

```sh
sync_root=$(mktemp -d "${TMPDIR:-/tmp}/ladyflock-sync.XXXXXXXX")
git clone --no-tags --single-branch --branch ladyflock https://github.com/Kayyo321/Ladyflock.git "$sync_root/candidate"
cd "$sync_root/candidate"
git remote add -t master --no-tags upstream https://github.com/LadybirdBrowser/ladybird.git
```

When reusing an existing standalone sync clone, start at its root, set `sync_root` to a fresh temporary directory with the first command above, and inspect `git status --short --branch` for local changes. Add `upstream` only if absent. Verify the fetch URLs identify the two repositories above and the push URL for `origin` identifies Ladyflock. Stop on any mismatch. Fetch just the named branches.

```sh
candidate_src=$PWD
git remote get-url upstream
git remote get-url origin
git remote get-url --push origin
git fetch --no-tags origin refs/heads/ladyflock:refs/remotes/origin/ladyflock
git fetch --no-tags upstream refs/heads/master:refs/remotes/upstream/master
downstream=$(git rev-parse origin/ladyflock)
upstream_tip=$(git rev-parse upstream/master)
target=$upstream_tip
```

For a particular rendering fix, select its reviewed full upstream commit SHA as `target` instead. Use an integrated upstream revision that contains the change and its dependencies. LibGfx, LibWeb, the compositor, IPC, generated bindings and dependency pins can change together. Merge the history through that revision so Git can identify the next incoming range. Copying a library directory or cherry-picking its final patch does not establish that ancestry.

```sh
git merge-base --is-ancestor "$target" "$upstream_tip"
git merge-base --all "$downstream" "$target"
```

If `git merge-base --is-ancestor "$target" "$downstream"` succeeds, stop here. The selected commit is already in Ladyflock’s history. If its behavior appears absent, inspect later changes and reverts. For a new update, expect one common base. Compare it with the last verified upstream SHA in the previous sync record (the import SHA for the first sync). Investigate missing history, a different base, or multiple bases before selecting a range. In a shallow checkout, obtain full history first.

```sh
previous=$(git merge-base "$downstream" "$target")
git log --oneline "$previous..$target"
git diff --stat "$previous" "$target"
git diff --name-status "$previous" "$downstream"
```

Review the incoming changes beside Ladyflock’s own delta. Pay particular attention to shared files, renamed paths, deleted tests, build tools, dependency pins, generated inputs and helpers. Changes can affect callers in other directories even when Git reports no conflicts.

## Integrate on a temporary branch

Start from the pinned downstream commit. Make a second checkout of that same commit for the comparison build. The local clone below has its own build and vcpkg directories. Keep the candidate shell at the root of the first clone.

```sh
topic="sync/ladybird-$(git rev-parse --short "$target")"
git switch -c "$topic" "$downstream"
git clone --no-tags --single-branch --branch "$topic" --no-checkout . "$sync_root/baseline"
git -C "$sync_root/baseline" switch --detach "$downstream"
git merge --no-ff --no-commit "$target"
```

A conflict pauses the merge. Use `git diff --name-only --diff-filter=U`, resolve and stage each path deliberately, or use `git merge --abort` to abandon the attempt before committing. Review the staged result even after a clean merge. Check Ladyflock’s brand and application IDs, profile and IPC isolation, extension and privacy behavior, release/update/report destinations, and `.github/` workflows. Keep feature work separate. Finish the local merge and identify the exact commit to build.

```sh
git diff --cached --stat
git diff --cached --check
git commit -m "Merge Ladybird upstream at $target"
candidate=$(git rev-parse HEAD)
test "$(git rev-parse HEAD^1)" = "$downstream"
test "$(git rev-parse HEAD^2)" = "$target"
```

## Compare the baseline and the candidate

Run each selected check in **both** source roots, first `$sync_root/baseline`, then `$candidate_src`. Use the same host, compiler, build flags, OS appearance, fonts, display scale, graphics backend and fixture data. Each revision uses its own checked-in dependency pins. Run from clean build directories and use fresh disposable profiles for every browser launch. Follow the [build prerequisites](Documentation/BuildInstructionsLadybird.md) and [test guide](Documentation/Testing.md).

The following commands are for a desktop build with Qt on macOS or Linux. For each clone, start at its root with a supported compiler selected and no build environment pointing at another checkout. `vcpkg` bootstraps the revision’s toolchain. Configure with the release checker disabled before launching either build.

```sh
export LADYBIRD_SOURCE_DIR="$PWD"
python3 Meta/ladybird.py vcpkg --preset Release
cmake --preset Release -B Build/release -DBUILD_TESTING=ON -DLADYBIRD_ENABLE_UPDATE_CHECK=OFF
cmake --build Build/release
ctest --test-dir Build/release -N
ctest --test-dir Build/release --show-only=json-v1 > Build/release/test-inventory.json
```

For a browser or engine sync, exercise the fork’s features as well as the changed upstream component. These suites cover headless extension execution, offline policy and stored identities, plus Qt dashboard, session and split-view behavior. Check the listed tests and the number of cases that actually ran. The Qt suites use the offscreen platform plugin. Treat a missing suite or plugin as a coverage gap. [Qt test registration](Tests/UI/Qt/CMakeLists.txt)

```sh
Build/release/bin/TestLibWebView --suite TestExtensionRuntime
Build/release/bin/TestLibWebView --suite TestPrivacyOffline
Build/release/bin/TestLibWebView --suite TestIdentityPurge
Build/release/bin/TestUIQt --suite TestDashboardStore
Build/release/bin/TestUIQt --suite TestSessionStore
Build/release/bin/TestUIQt --suite TestSplitView
```

For the change that motivated the sync, run a fixture that distinguishes the old behavior from the intended new behavior. For example, render the same local SVG on both builds and retain the baseline, candidate and expected images. If the test was introduced upstream, keep a copy of its inputs so the previous build can exercise the same case. Where performance is the reason for the update, compare repeated measurements on the same hardware and backend.

| Change | Additional evidence |
| --- | --- |
| Rendering, painting, fonts, image decoding or compositing | Run `ctest --test-dir Build/release --output-on-failure --no-tests=error -R '^TestLibGfx$'` and the affected LibWeb layout/reference/screenshot tests. For an SVG renderer change, run `Build/release/bin/test-web --temporary-profile -f 'Ref/input/svg/*' --results-dir Build/release/svg-results`. Inspect the executed test list, failed image pairs and diffs. For a broad rendering change, run the complete `LibWeb` CTest and exercise affected hardware rendering on a supported host. |
| JavaScript, WebContent, IPC or networking | Exercise the changed library’s suites and real browser/helper interactions. For JavaScript use `Build/release/bin/test-js`, and for compositor startup use `(cd Tests/LibWebView && ../../Build/release/bin/TestLibWebView --suite TestCompositorStartup)` from the checkout root with `LADYBIRD_SOURCE_DIR` exported as above. The compositor suite reads a video fixture relative to its working directory. Include affected extension, permission, request-routing and WebDriver cases. |
| Browser chrome, platform identity, storage or release integration | Exercise the affected OS and its packaging or registration path. In disposable OS/profile environments, run upstream and downstream side by side and check launch routing, settings isolation, and product-owned update/report behavior. For visible changes capture matching before/after views. Follow the [visual regression instructions](.agents/skills/ladyflock-visual-regression/SKILL.md). |

For a broad engine or toolchain update, run `ctest --test-dir Build/release --output-on-failure --no-tests=error` on both builds and record affected platform coverage. Inspect added, removed, skipped or newly rebaselined tests separately. Keep known fixtures and previous expectations available to check for lost coverage. Investigate every new failure, crash, timeout, skip or unexpected pixel change. Rerun flakes on both sides and retain the original results. If an upstream change deliberately alters a result, document the upstream change, its expected behavior and the observed difference before accepting it. If an affected platform or required check is unavailable, record the gap and obtain that evidence before claiming it passed. The [inherited CI workflow](.github/workflows/ci.yml) has upstream-specific guards. A skipped job supplies no test evidence. Before adapting a guarded workflow, compare its application paths and archive names against the current build and CPack output. The inherited JS/Wasm artifact job can also target historical Ladybird revisions. Review repository guards, upload destinations, signing and publication separately before enabling jobs for the fork.

For CTest runs, add `--output-junit "$PWD/Build/release/ctest-results.xml"` to retain per-test results in each clone. Give separate selections distinct filenames. Keep the sync branch out of `ladyflock` while a new failure is unexplained or a required comparison is missing. Report “no new regressions observed” only for the configurations and behaviors actually compared.

## Record and land the tested result

Keep this record with the sync review so the next agent can reproduce the comparison.

| Record | What to include |
| --- | --- |
| Revisions and scope | `downstream`, `previous`, `target`, `candidate` (full SHAs), update rationale, changed components and notable conflict resolutions. |
| Environment | OS/architecture, compiler, configure/build commands, dependency changes, graphics backend and fixture/profile setup. |
| Baseline and candidate results | Exact commands, exit codes and case counts on each revision. Identify any failing, skipped or removed cases by name. Attach logs, screenshots or image diffs and document intentional changes, blockers and outstanding platform checks. State which behaviors were exercised. |

Before landing, repeat the fetch of `origin/ladyflock` and run `test "$(git rev-parse origin/ladyflock)" = "$downstream"` in the candidate clone. If the branch advanced, integrate the new tip on the sync branch, use that tip as the comparison baseline, and repeat the affected checks on the new combined candidate. Update the record and the candidate SHA after every code or resolution change. Retain artifacts outside the working trees before cleanup.

When publishing is requested, push only the reviewed topic branch to `origin`, open its pull request into `ladyflock`, and select **Create a merge commit**. Squash and rebase would discard the ancestry established above. Keep upstream branches and tags local. A source sync does not publish a release.

```sh
git push -u origin "$topic"
```

After the PR lands, fetch the fork and verify both the reviewed upstream revision and the tested candidate are ancestors of `ladyflock`.

```sh
git fetch --no-tags origin refs/heads/ladyflock:refs/remotes/origin/ladyflock
git merge-base --is-ancestor "$target" origin/ladyflock
git merge-base --is-ancestor "$candidate" origin/ladyflock
```

Record the landing commit. Compare its tree with the tested candidate using `git diff --exit-code "$candidate" <landing-commit>` (substitute the PR’s actual merge SHA). If the tree differs, inspect and test the combined changes before reporting verification. After acceptance, retire the temporary branch. If a landed sync causes a regression, fix it or revert the reviewed PR merge on a recovery branch using its Ladyflock first parent (`git revert -m 1 <landing-commit>`). Reverting preserves upstream ancestry. A later import must explicitly restore the reverted changes that are needed and repeat the comparison.
