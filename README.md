# Ladyflock

Ladyflock is an independent browser project based on [Ladybird](https://github.com/LadybirdBrowser/ladybird). Its initial goal is to run existing Firefox WebExtensions on Ladybird's engine, starting with a small, explicitly supported set of APIs.

## Project status

This repository is a starting point, not a browser release. Firefox add-ons, XPI installation, and Mozilla Add-ons integration are not implemented. The browser executable, build commands, and artwork still use the upstream Ladybird name and assets.

## First milestones

1. Load a local WebExtension and run its content script on an explicitly permitted site.
2. Add isolated extension pages, message passing, and local extension storage.
3. Test request-blocking APIs for privacy extensions. Full uBlock Origin compatibility is a longer-term test, not a current feature.

## Build from source

Follow the [inherited build instructions](Documentation/BuildInstructionsLadybird.md). They still refer to Ladybird. See [Documentation](Documentation/) for the upstream engine and browser architecture.

## Upstream and license

Ladyflock is a separate project and is not affiliated with the Ladybird Browser Initiative. This repository preserves the upstream Git history and copyright notices. The inherited source is distributed under the [BSD 2-Clause license](LICENSE), with individual notices where applicable.
