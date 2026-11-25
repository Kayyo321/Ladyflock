# Updating from Ladybird

Ladyflock is a separate GitHub repository. It keeps Ladybird's Git history so Git can merge later upstream changes, but `ladyflock` is the only permanent branch here. A local `upstream/master` ref is not a branch published to Ladyflock. The original import starts at Ladybird commit `be5e3ce8585ddb907c17fbdafcea11027a32ae6f`.

## Set up a clone

Run these commands from a Ladyflock clone once, unless the `upstream` remote already exists. If it does, check `git remote get-url upstream` before using it.

```sh
git remote add -t master --no-tags upstream https://github.com/LadybirdBrowser/ladybird.git
```

## Review an update

Start with a clean worktree. The topic branch name below includes the upstream commit so it is clear what the pull request imports.

```sh
git status --short                         # Stop if this prints anything.
git fetch --no-tags origin ladyflock
git fetch --no-tags upstream
target=$(git rev-parse upstream/master)
git switch -c "sync/ladybird-$(git rev-parse --short "$target")" origin/ladyflock
git merge --no-ff --no-commit "$target"
```

- If Git says the branch is already up to date, there is nothing to publish.
- Otherwise, resolve any conflicts. Preserve Ladyflock-specific README and project settings intentionally rather than choosing one side for the whole merge. Use `git merge --abort` if you decide not to import the change.
- Review the staged diff, especially `.github/`, updater/release configuration, branding, build dependencies, and extension-related code. Don't assume upstream automation or release settings are appropriate for this repository.
- Run the relevant tests and record what passed and what you could not check. Use the [source build guide](Documentation/BuildInstructionsLadybird.md) if the update touches the browser or build system.

```sh
git diff --cached --check
git commit -m "Merge Ladybird upstream at $target"
git merge-base --is-ancestor "$target" HEAD
git push -u origin HEAD
```

Open a pull request from the temporary sync branch into `ladyflock`. Use **Create a merge commit**, not **Squash and merge** or **Rebase and merge**, so that the upstream commit remains an ancestor of `ladyflock`. After merging, delete the temporary branch. Do not push upstream branches or tags to the Ladyflock repo.

## Verify the result

```sh
git fetch --no-tags origin ladyflock
git merge-base --is-ancestor "$target" origin/ladyflock
```

Exit status zero confirms the chosen upstream revision is in Ladyflock's history. The next sync starts from that ancestry. There is no automatic merge or release.
