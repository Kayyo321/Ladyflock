# Security Policy

## Reporting a vulnerability

Send suspected security issues through [Ladyflock’s private vulnerability report form](https://github.com/Kayyo321/Ladyflock/security/advisories/new). Reports go directly to the repository maintainers. Keep security details private while a fix is being prepared.

You may also report privately a security issue you’ve found in another web browser or in the upstream Ladybird engine
that you believe affects Ladyflock.

A useful report names the Ladyflock version or commit you tested, and your platform, and includes a minimal test case
that reproduces the issue. If you found the issue with a fuzzer, please include the input that triggers it, and the
build you ran it against. Please make sure the issue reproduces on the latest build of the `ladyflock` branch.

## Supported versions

Security fixes land on the `ladyflock` branch. Because Ladyflock is built on the upstream Ladybird engine, engine
security fixes may also be coordinated with upstream Ladybird.

## What happens after you report

The maintainers triage each report and coordinate fixes. If an issue originates in the upstream Ladybird engine or a
third-party library, disclosure may be coordinated with upstream maintainers so affected projects have time to resolve
it.

The maintainers reserve the right to reject reports that aren’t security issues, or that are outside the scope of
Ladyflock. A report determined not to be a security issue should be filed as a
[GitHub issue](https://github.com/Kayyo321/Ladyflock/issues/new?template=bug_report.yml) instead.

Ladyflock doesn’t offer bug bounties for security issues at this time.

## Scope

Examples of issues in scope:

- Memory-safety bugs (use-after-free, out-of-bounds reads and writes, type confusion) that web content or extensions can
  trigger, in any Ladyflock process
- Escapes from the sandbox of the WebContent process or of another helper process
- Bypasses of the same-origin policy, site isolation, extension isolation, Content Security Policy, or iframe sandboxing
- Spoofing of the address bar or of other browser UI
- Flaws in validating IPC messages that let a compromised helper process act on the browser UI or on another process

Some security features of the web platform aren’t implemented yet. Reports about a missing feature (rather than a flaw
in an implemented one) may be redirected to regular issues. Vulnerabilities in websites themselves, such as cross-site
scripting or cross-site request forgery on a particular site, aren’t Ladyflock issues.

A controlled crash of the sandboxed WebContent process that corrupts no memory (a failed assertion, a null-pointer
dereference, or a stack overflow that hits the guard page) isn’t a security issue. Please report it as a
[GitHub issue](https://github.com/Kayyo321/Ladyflock/issues/new?template=bug_report.yml). A crash in any other process,
or one that involves memory corruption, is in scope.

Significant portions of the browser depend on the upstream Ladybird engine and third-party libraries. If a security
issue affects how a library or engine component is integrated into Ladyflock, report it privately the same way as other
security issues.

## Known limitations

Process isolation is still being hardened: the browser process and helper processes don’t yet treat every request from
the WebContent process as untrusted. Please still report such flaws privately, the same way as other security issues.

## Responsible disclosure

Please allow maintainers time to investigate and coordinate fixes before public disclosure, especially when an issue
also affects upstream Ladybird or other projects.
