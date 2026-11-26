# Ladyflock

Ladyflock is an independent browser project based on [Ladybird](https://github.com/LadybirdBrowser/ladybird). Its initial goal is to run existing Firefox WebExtensions on Ladybird's engine, starting with a small, explicitly supported set of APIs.

## Project status

Ladyflock is a development browser. The executable, build commands, and artwork still use the upstream Ladybird name and assets.

## Firefox extensions

Choose **Extensions → Manage extensions** in the toolbar or visit `about:addons` to manage extensions, review permissions, and install a signed XPI from disk. The Extensions toolbar button also lets you run an extension action, pin it, or open its sidebar.

Launch with `--enable-amo-catalog` to try search and installation from Mozilla Add-ons. Use `--load-extension /path/to/unpacked-extension` in a development profile to load a local unpacked extension.

The Add-ons page lists supported manifest keys, permissions, and APIs. The **Tested in Ladyflock** label applies to a specific package version and hash. For uBlock Origin and uBlock Origin Lite, the test covers signed installation from Mozilla Add-ons and default network ad blocking. Cosmetic filtering and advanced rule modes have limited support. Extensions run in regular profiles.

To repeat the live check with a built browser, run `python3 Tests/LibWebView/test-amo-ublock.py /path/to/Ladybird`. It uses separate disposable profiles and a loopback HTTP server.

## Build from source

Follow the [inherited build instructions](Documentation/BuildInstructionsLadybird.md). They still refer to Ladybird. See [Documentation](Documentation/) for the upstream engine and browser architecture.

## Upstream and license

See [Updating from Ladybird](UPSTREAM.md) to review newer upstream revisions without importing upstream branches or tags.

Ladyflock is a separate project and is not affiliated with the Ladybird Browser Initiative. This repository preserves the upstream Git history and copyright notices. The inherited source is distributed under the [BSD 2-Clause license](LICENSE), with individual notices where applicable.
