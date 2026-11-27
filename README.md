# Ladyflock

Ladyflock is an independent browser project based on [Ladybird](https://github.com/LadybirdBrowser/ladybird). Its initial goal is to run existing Firefox WebExtensions on Ladybird's engine, starting with a small, explicitly supported set of APIs.

## Project status

Ladyflock is a development browser built on the Ladybird engine. It ships with its own Ladyflock branding, artwork, executable name, platform application ID (`io.github.kayyo321.Ladyflock`), and separate default profiles so it can run side by side with Ladybird.

### Upgrading from a build branded Ladybird

Ladyflock starts with a fresh profile. Your previous build still opens its existing data under `Ladybird/Profiles/<name>`. The usual profile names are `developer` for development builds and `default` for releases. Named profiles use the name you supplied.

| Platform | Previous settings root | Previous data root |
| --- | --- | --- |
| macOS | `~/Library/Preferences` | `~/Library/Application Support` |
| Linux | `$XDG_CONFIG_HOME` (default `~/.config`) | `$XDG_DATA_HOME` (default `~/.local/share`) |
| Windows | `%APPDATA%` | `%LOCALAPPDATA%` |

Append `Ladybird/Profiles/<name>` to each root to find the old profile. Ladyflock creates the same layout under `Ladyflock`. On macOS, explicit XDG overrides also take precedence. If you previously used `--profile-path`, close the old browser, copy the entire custom profile directory, and pass the copy’s absolute path to Ladyflock. Default profiles split their files across OS roots and need to stay with the previous build.

## Firefox extensions

Choose **Extensions → Manage extensions** in the toolbar or visit `about:addons` to manage extensions, review permissions, and install a signed XPI from disk. The Extensions toolbar button also lets you run an extension action, pin it, or open its sidebar.

Launch with `--enable-amo-catalog` to try search and installation from Mozilla Add-ons. Use `--load-extension /path/to/unpacked-extension` in a development profile to load a local unpacked extension.

The Add-ons page lists supported manifest keys, permissions, and APIs. The **Tested in Ladyflock** label applies to a specific package version and hash. For uBlock Origin and uBlock Origin Lite, the test covers signed installation from Mozilla Add-ons, default network ad blocking, and hiding ad elements with bundled cosmetic filters. The test uses Lite's **Complete** mode for generic cosmetic filtering. Advanced rule modes have limited support. Extensions run in regular profiles.

To repeat the live check with a built browser, run `python3 Tests/LibWebView/test-amo-ublock.py /path/to/Ladyflock`. It uses separate disposable profiles and a loopback HTTP server.

[Firefox Extension Compatibility](Documentation/Extensions/Compatibility.md) covers the rest. It explains how the install check follows Firefox's rules, what each API does here, and its limits. It also has the results of checking and starting the 100 most-used extensions on Mozilla Add-ons, with the gaps that remain.

## Build from source

Follow the [Ladyflock build instructions](Documentation/BuildInstructionsLadybird.md). The build helper retains its upstream name, `Meta/ladybird.py`. See [Documentation](Documentation/) for the upstream engine and browser architecture.

## Upstream and license

See [Updating from Ladybird](UPSTREAM.md) to review newer upstream revisions without importing upstream branches or tags.

Ladyflock is a separate project and is not affiliated with the Ladybird Browser Initiative. This repository preserves the upstream Git history and copyright notices. The inherited source is distributed under the [BSD 2-Clause license](LICENSE), with individual notices where applicable.
