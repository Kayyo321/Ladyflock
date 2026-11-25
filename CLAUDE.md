# Claude Code in Ladyflock

@AGENTS.md

The imported file is the shared project guidance. Keep general project rules there and use this file for Claude Code workflow. If the import is not active in your client, read `AGENTS.md` explicitly before editing.

## Start a change

- Work from a Ladyflock checkout. Inspect Git status and confirm which files and branch the request covers. Check the nearest directory instructions, then trace the code from its real entry point to the policy owner and the test that can observe it.
- Be explicit about the current feature boundary. For example, “a local unpacked fixture runs under declared host permissions” is a useful milestone; “supports Firefox extensions” is too broad until tested.
- Keep the task small enough to verify. For a cross-process feature, first describe the browser-side owner, the minimum data sent across IPC, the permission check at use time, and the expected behavior when the document or permission goes away.

## Use subagents selectively

- Use read-only exploration or review in parallel when it will save time. Give each worker a concrete question and ask for file and test references. For workers making edits, give each a separate worktree or disjoint files and name the expected handoff.
- The lead owns shared APIs and IPC changes, integrates the work, and runs the combined test. Treat each worker's report as a lead until verified in the integrated diff. Avoid having multiple workers independently change the same API or test expectation.

## Before handing back

- Check the final diff for unrelated changes and verify the smallest runnable test. A page-global shortcut, disabled permission check, or skipped job is not a passing result.
- If the real browser test cannot run, state the actual blocker and what remains unverified. Do not quietly replace it with a mock and claim end-to-end coverage.
- Summarize behavior changed, validation and its gaps, and commit/remote state when relevant. Don't promise Firefox compatibility beyond the manifest and API behavior actually exercised.
