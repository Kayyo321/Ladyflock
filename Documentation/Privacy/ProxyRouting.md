# Mandatory Proxy Route and Stream Isolation (Milestone 3)

This document describes the first slice of Ladyflock's **Milestone 3 (Proxy and Tor routing)**: one mandatory proxy route per session, SOCKS5 stream isolation per identity and top-level site, and Tor-routed identities that never connect directly. See [NetworkEnforcement.md](NetworkEnforcement.md) for Milestones 0 and 1 and [IdentitiesAndStorage.md](IdentitiesAndStorage.md) for Milestone 2.

**Scope.** Ladyflock does not start, supervise or bundle a Tor daemon or Arti. It sends a Tor-routed identity's traffic to a SOCKS endpoint that the user runs and names. Bridges, pluggable transports, DoH/ODoH, named proxy endpoints and per-site circuit renewal are not part of this slice.

## 1. Using it

| Option | Effect |
|---|---|
| `--privacy-network-mode=proxy-only --privacy-proxy=<url>` | Every session without an identity route of its own starts in proxy-only mode on `<url>`. |
| `--privacy-proxy-isolation` | Gives each identity and top-level site its own SOCKS5 credentials on `--privacy-proxy`. The route must be `socks5h://` without credentials. |
| `--privacy-tor-proxy=<url>` | The `socks5h://` endpoint that Tor-routed identities (`IdentityKind::TorRouted`) must use. Their streams are always isolated. |

A route URL must be `http://`, `https://`, `socks4a://` or `socks5h://`, followed by an IPv4 or bracketed IPv6 literal and an explicit port, for example `socks5h://127.0.0.1:9050`. Optional `user:password@` credentials are allowed except with isolation.

Ladyflock rejects these route URLs:

- `socks4://` and `socks5://`, because the browser would resolve destination names locally.
- Host names, including `localhost`, because reaching the proxy would need a lookup outside the route.
- Interface zones, paths, queries and fragments.

Proxy-only mode without `--privacy-proxy` has no network. `PrivacyPolicyCoordinator::set_network_mode(..., ProxyOnly)` without a configured route is refused with `MandatoryRouteUnavailable`, and the current policy stays.

## 2. Architecture

### 2.1 Policy core (`Libraries/LibPrivacy/Policy/`)

- **`Route.{h,cpp}`**. `ProxyRoute` holds the following fields:
  - `RouteProxyType` (`HTTP`, `HTTPS`, `SOCKS4A`, `SOCKS5Hostname`) and the IP-literal host and port.
  - Optional static credentials.
  - `StreamIsolation` (`None` or `PerIdentityAndSite`).
  - `isolation_secret` and `isolation_generation`.

  `ProxyRoute::parse` and `is_valid` enforce the rules in section 1. The IPC decoder rejects routes that fail `is_valid`. `redacted_description()` leaves out credentials and secrets.
- **`Types.{h,cpp}`**. The schema version is **2**.
  - The new `NetworkMode::ProxyOnly` carries `NetworkPolicy::route`, and no other mode may carry one.
  - `NetworkPolicy::proxy_only(route)` denies local DNS, WebRTC and speculative connections. It allows WebSockets, AIA fetches and browser services, which all go through RequestServer.
  - `allows_direct_egress()` is false under a route. WebRTC and the macOS updater check it, because they cannot use the route.
- **Intersection**. Offline wins over any route. A route narrows direct egress. Two different routes intersect to `Offline`, and `PolicyCompiler` reports `Conflicting mandatory routes`. With an environment proxy present, the compiler reports that the route replaces it, including its bypass list.
- **Tightening**. Any route change counts as a tightening, so RequestServer quiesces and resets its curl pools. This includes a change from direct egress to a route, a change of endpoint, and a change of isolation generation.
- **`NetworkGate::mandatory_route()`**. Returns the installed route only while the gate is `Active`. A closed or coordinator-lost gate has no route and admits nothing.

### 2.2 Enforcement in RequestServer

- **`proxy_for_connection(url, top_level_site)`** (`Services/RequestServer/Resolver.{h,cpp}`) is the single source of every connection's proxy. It covers `Request` (including each redirect hop, which re-enters `handle_dns_lookup_state`), WebSockets and AIA intermediate fetches. Under a route, the result is always the route, regardless of `ProxyConfiguration` and its `no_proxy` list. Previously an empty `proxy_for()` meant a direct connection.
- **Source of the route**. The route comes from the installed `PolicySnapshot`, not from the separate, unacknowledged `set_proxy_configuration` message. `RequestServerManager::launch_process` installs the snapshot synchronously before any client connects. A restarted RequestServer therefore has the route before its first request.
- **No local resolution**. The route types resolve names at the proxy, so `Request` skips local DNS. `ConnectionFromClient::resolve_host` answers nothing while a route is in force. The gate also denies DNS, the Hub-delegated system resolver and speculative `ensure_connection`.
- **Alt-Svc and HTTP/3**. Both stay off on the routed path, as they already were for any proxied request.
- **`CURLOPT_NOPROXY`**. It is set to an empty list on every routed handle. This is defence in depth: helper processes get an allowlisted environment (`Process::helper_process_environment`) without proxy variables, so curl has no `no_proxy` to read today.
- **Proxy loss**. A request whose proxy is unreachable fails with a network error, with no direct retry.

### 2.3 Hub coordinator (`Libraries/LibWebView/PrivacyPolicyCoordinator.{h,cpp}`)

- `PrivacyPolicyCoordinator::Configuration` holds the default mode, the proxy route, the isolation flag and the Tor route.
- Each session's state holds a random 128-bit `isolation_secret`, generated with `fill_with_random`, and an isolation generation. Every identity has its own `BrowsingSession`, and therefore its own secret. The secret goes only into `PolicySnapshot`s for that session's RequestServers. It is not in `HubClient` IPC, renderer snapshots or logs.
- **Tor-routed identities**. `IdentityDescriptor::tor_routed` used to start with direct egress. Its session now gets an identity requirement of `proxy_only(tor_route)` with isolation. Without `--privacy-tor-proxy`, the requirement is `deny_all()`. The identity's route replaces a browser-wide proxy route, so the two do not conflict.
- **`renew_route_isolation(session)`** bumps the generation. Every later stream of the session gets new credentials, and its existing connections close through the tightening quiesce.

## 3. SOCKS5 isolation parameters

These parameters apply to `StreamIsolation::PerIdentityAndSite` routes, which are `socks5h` only.

| Field | Value |
|---|---|
| SOCKS5 method | Username/password (RFC 1929) |
| Username | First 16 bytes, hex, of SHA-256(`"ladyflock-route-isolation-v1"`, `0x00`, secret, generation as u64, site length as u64, empty site) |
| Password | First 16 bytes, hex, of SHA-256(`"ladyflock-route-isolation-v1"`, `0x01`, secret, generation as u64, site length as u64, top-level site) |
| Top-level site | `HTTP::NetworkIsolationKey::top_level_site` of the request, or the site a WebSocket was opened under. Browser-initiated work (no key) and AIA fetches use the empty site. |
| Separates | Identities, through the username (one secret per session). Top-level sites, through the password. Generations, through both. |

Both values are 32 lowercase hex characters, so they need no escaping in the proxy URL and name neither the site nor the secret. Tor's default `IsolateSOCKSAuth` puts streams with different SOCKS credentials on different circuits. Whether a given Tor or Arti build honours this is a property of that backend, and is untested here (section 5).

## 4. Recorded verification results

- **Git revision verified**: `c8ec6e20248` (branch `privacy-milestone-3`, based on `ladyflock` `72aa25e024a`)
- **Host platform**: macOS 26.3.1, Darwin 25.3.0, `arm64`
- **Build configuration**: `Build/ladyflock` (Release preset, `LADYBIRD_ENABLE_UPDATE_CHECK=OFF`, sandboxing enabled)

```sh
cmake --build Build/ladyflock --target TestLibPrivacy TestRequestServer TestLibWebView TestUIQt Ladybird RequestServer WebContent WebRTCClient
Build/ladyflock/bin/TestLibPrivacy
(cd Tests/LibWebView && LADYBIRD_SOURCE_DIR="$PWD/../.." ../../Build/ladyflock/bin/TestLibWebView --suite TestPrivacyProxyRoute)
```

| Suite | Passed | What it checks |
|---|---|---|
| `TestLibPrivacy` → `TestPrivacyRoute` | 6 / 6 | Route parsing and rejection (locally resolving types, names, zones, paths), isolation validity, credential separation by identity/site/generation, ProxyOnly gate decisions, route intersection and conflicts, route-change tightening, IPC round trip and malformed-route rejection |
| `TestLibPrivacy` (all) | 13 / 13 | Milestones 0–2 policy, identity and storage tests still pass under schema version 2 |
| `TestLibWebView --suite TestPrivacyProxyRoute` | 4 / 4 | The end-to-end cases below |

`TestPrivacyProxyRoute` runs the real browser against independent observers on their own threads:

- an independent UDP DNS server that answers with loopback;
- a mock SOCKS5 proxy that records each stream's authentication, address type and destination, and answers the tunnelled HTTP itself;
- a trap HTTP server on the port that a direct connection would reach.

It checks these cases:

1. **Bypass lists.** The environment names the trap as `http_proxy` and exempts every destination (`no_proxy=fixture.example,excluded.example,localhost,127.0.0.1`). A page subresource still reaches the proxy as a domain-name (`ATYP 3`) destination. So do a redirect to an exempt host and a `localhost` URL.
2. **Helper restart.** After `SIGKILL` of every RequestServer, the replacement uses the route. So does a newly launched site-specific RequestServer.
3. **Proxy loss.** With the proxy stopped, page fetches and direct requests (including through a redirect) fail visibly.
4. **Reused connection.** In allow mode, two requests share one keep-alive direct connection (positive control). A live `set_network_mode(ProxyOnly)` closes that pooled connection, and the next request goes through the proxy with no new DNS query.
5. **Stream isolation.**
   - Sites `a` and `b` and browser-initiated work share the identity's username but have distinct passwords and separate streams.
   - Every later stream of site `a` carries site `a`'s credentials.
   - A second identity gets a different username.
   - A Tor-routed identity goes to the Tor endpoint, not the browser proxy, with credentials of its own.
   - Renewal closes existing streams and changes site `a`'s credentials.
6. **Tor without an endpoint.** The default identity connects directly (positive control). A Tor-routed identity without `--privacy-tor-proxy` gets `BlockedByPrivacyPolicy`. Proxy-only mode without a route is refused.

Across cases 1–3, 5 and 6, the DNS observer records **zero** queries and the trap records **zero** connections while a route (or a closed Tor identity) is in force.

Mutation checks: removing the route-generation tightening makes case 5's renewal assertion fail. Disabling `CURLOPT_NOPROXY` leaves every test passing, which is consistent with section 2.2: proxy variables never reach RequestServer.

Regression suites on the same build:

- `TestLibWebView`: `TestPrivacyMilestone0` 2/2, `TestPrivacyOffline` 5/5, `TestPrivacyMilestone2` 5/5, `TestBackgroundNetworking` 5/5, `TestRequestServerClientSessions` 2/2, `TestRequestServerDiskCaches` 2/2, `TestFavicon` 1/1, `TestExtensionCore` 11/11 and `TestOmnibox` 79/79.
- `TestUIQt` (offscreen): no failures.

### Failures not caused by this milestone

- **`TestRequestServer --suite TestProxyHostResolution`**. `proxy_given_by_name_is_pinned_to_the_addresses_found` fails and then crashes. Milestone 1 (`4e8635bfa3e`) added a privacy-gate check to `resolve_proxy_host`, and the unit test never installs a policy, so its lookup is denied.
- **`TestRequestServer --suite TestPrivacyNetworkGate`**. It failed with "Unable to connect" in a full `TestRequestServer` run. It passed 4 of 4 times when run alone. The allow-mode connection path it uses is unchanged by this milestone.

## 5. Not verified, and deferred

- **Real Tor circuit sharing.** No `tor` or `arti` binary is installed on this host, and checking circuit separation needs the live Tor network. Not run. Treat the isolation guarantee as "distinct SOCKS credentials per identity and site" until a run against the selected backend confirms distinct circuits.
- **Route types and transports.** Only `socks5h` and plain HTTP destinations were exercised end to end. These parameters are parsed and unit-tested but have no observer fixture:
  - `http://`, `https://` and `socks4a://` routes;
  - HTTPS destinations tunnelled through the route;
  - WebSockets and AIA fetches over the route.
- **Kernel enforcement.** Enforcement is in userspace only. RequestServer's macOS and Linux sandboxes still allow general network access in proxy-only mode; they do not limit it to the proxy endpoint.
- **Per-site renewal.** `renew_route_isolation` renews a whole identity. Renewing a single site's token and closing only that site's connections is not implemented.
- **Out of scope.** Named proxy endpoints and DNS bootstrap, PAC evaluation (still unsupported; PAC never yields a direct route here), supervised Tor or Arti, bridges, and a settings UI for routes (command line only).
- **Other platforms.** Linux and Windows were not built or run on this host.
