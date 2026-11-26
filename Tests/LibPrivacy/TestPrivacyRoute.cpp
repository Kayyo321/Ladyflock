/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <AK/Queue.h>
#include <LibIPC/Attachment.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibPrivacy/Hooks/RequestServerHooks.h>
#include <LibPrivacy/Policy/Compiler.h>
#include <LibPrivacy/Policy/NetworkGate.h>
#include <LibPrivacy/Policy/Route.h>
#include <LibPrivacy/Policy/Types.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>

static URL::URL parse_url(StringView input)
{
    return URL::Parser::basic_parse(input).release_value();
}

static Privacy::ProxyRoute route(StringView string)
{
    return Privacy::ProxyRoute::parse(string).release_value();
}

static Privacy::ProxyRoute isolated_route(StringView secret, u64 generation = 1)
{
    auto result = route("socks5h://127.0.0.1:9050"sv);
    result.isolation = Privacy::StreamIsolation::PerIdentityAndSite;
    result.isolation_secret = secret;
    result.isolation_generation = generation;
    return result;
}

TEST_CASE(routes_are_pinned_to_ip_literals_that_resolve_names_remotely)
{
    auto socks = route("socks5h://127.0.0.1:9050"sv);
    EXPECT_EQ(socks.type, Privacy::RouteProxyType::SOCKS5Hostname);
    EXPECT_EQ(socks.host, "127.0.0.1"sv);
    EXPECT_EQ(socks.port, 9050);

    auto http = route("HTTP://user:pass@[::1]:8080/"sv);
    EXPECT_EQ(http.type, Privacy::RouteProxyType::HTTP);
    EXPECT_EQ(http.host, "::1"sv);
    EXPECT_EQ(http.credentials, "user:pass"sv);
    EXPECT_EQ(http.redacted_description(), "http://[::1]:8080"sv);

    EXPECT(Privacy::ProxyRoute::parse("socks4a://10.0.0.1:1080"sv).has_value());
    EXPECT(Privacy::ProxyRoute::parse("https://192.0.2.1:443"sv).has_value());

    // Types that resolve destination names locally would leak them to the local resolver.
    EXPECT(!Privacy::ProxyRoute::parse("socks5://127.0.0.1:9050"sv).has_value());
    EXPECT(!Privacy::ProxyRoute::parse("socks4://127.0.0.1:9050"sv).has_value());
    // Reaching a named proxy needs a lookup outside the route.
    EXPECT(!Privacy::ProxyRoute::parse("socks5h://localhost:9050"sv).has_value());
    EXPECT(!Privacy::ProxyRoute::parse("socks5h://proxy.example:9050"sv).has_value());
    // A route names its port and nothing else.
    EXPECT(!Privacy::ProxyRoute::parse("socks5h://127.0.0.1"sv).has_value());
    EXPECT(!Privacy::ProxyRoute::parse("socks5h://127.0.0.1:0"sv).has_value());
    EXPECT(!Privacy::ProxyRoute::parse("socks5h://127.0.0.1:9050/path"sv).has_value());
    EXPECT(!Privacy::ProxyRoute::parse("127.0.0.1:9050"sv).has_value());
    EXPECT(!Privacy::ProxyRoute::parse("socks5h://[fe80::1%25en0]:9050"sv).has_value());

    // Isolation needs SOCKS5 credentials of its own and a secret to derive them from.
    auto isolated = isolated_route("0123456789abcdef0123456789abcdef"sv);
    EXPECT(isolated.is_valid());
    auto with_credentials = isolated;
    with_credentials.credentials = "user:pass";
    EXPECT(!with_credentials.is_valid());
    auto http_isolated = isolated;
    http_isolated.type = Privacy::RouteProxyType::HTTP;
    EXPECT(!http_isolated.is_valid());
    auto short_secret = isolated;
    short_secret.isolation_secret = "short";
    EXPECT(!short_secret.is_valid());
    auto stray_secret = socks;
    stray_secret.isolation_secret = "0123456789abcdef0123456789abcdef";
    EXPECT(!stray_secret.is_valid());
}

TEST_CASE(isolation_credentials_separate_identities_sites_and_generations)
{
    auto work = isolated_route("0123456789abcdef0123456789abcdef"sv);
    auto personal = isolated_route("fedcba9876543210fedcba9876543210"sv);

    auto work_a = Privacy::isolation_credentials_for(work, "https://a.example"sv).release_value();
    auto work_a_again = Privacy::isolation_credentials_for(work, "https://a.example"sv).release_value();
    auto work_b = Privacy::isolation_credentials_for(work, "https://b.example"sv).release_value();
    auto work_browser = Privacy::isolation_credentials_for(work, ""sv).release_value();
    auto personal_a = Privacy::isolation_credentials_for(personal, "https://a.example"sv).release_value();

    // Streams for one site of one identity share credentials, and so may share a circuit.
    EXPECT_EQ(work_a, work_a_again);
    // Sites of one identity share the identity's username but not its password.
    EXPECT_EQ(work_a.username, work_b.username);
    EXPECT_NE(work_a.password, work_b.password);
    EXPECT_NE(work_a.password, work_browser.password);
    // Identities share neither, even for the same site.
    EXPECT_NE(work_a.username, personal_a.username);
    EXPECT_NE(work_a.password, personal_a.password);

    // Credentials name neither the site nor the secret.
    EXPECT(!work_a.password.contains("example"sv));
    EXPECT(!work_a.username.contains(work.isolation_secret));
    EXPECT_EQ(work_a.username.length(), 32u);
    EXPECT_EQ(work_a.password.length(), 32u);

    // A new generation renews every credential.
    auto renewed = isolated_route("0123456789abcdef0123456789abcdef"sv, 2);
    auto renewed_a = Privacy::isolation_credentials_for(renewed, "https://a.example"sv).release_value();
    EXPECT_NE(renewed_a.username, work_a.username);
    EXPECT_NE(renewed_a.password, work_a.password);

    // A route without isolation has none to derive.
    EXPECT(!Privacy::isolation_credentials_for(route("socks5h://127.0.0.1:9050"sv), "https://a.example"sv).has_value());
}

TEST_CASE(proxy_only_policy_denies_local_resolution_speculation_and_webrtc)
{
    auto policy = Privacy::NetworkPolicy::proxy_only(route("socks5h://127.0.0.1:9050"sv));
    EXPECT(policy.is_consistent());
    EXPECT(policy.allows_network_egress());
    EXPECT(policy.requires_mandatory_route());
    EXPECT(!policy.allows_direct_egress());
    EXPECT(!policy.allows_dns_resolution());
    EXPECT(!policy.allows_webrtc_network());
    EXPECT(!policy.allow_speculative_connections);

    // A ProxyOnly policy must carry its route, and only ProxyOnly may carry one.
    auto without_route = policy;
    without_route.route.clear();
    EXPECT(!without_route.is_consistent());
    auto allow_with_route = Privacy::NetworkPolicy::allow_default();
    allow_with_route.route = route("socks5h://127.0.0.1:9050"sv);
    EXPECT(!allow_with_route.is_consistent());
    auto with_dns = policy;
    with_dns.dns_policy = Privacy::DNSRoutePolicy::Default;
    EXPECT(!with_dns.is_consistent());
    auto with_speculation = policy;
    with_speculation.allow_speculative_connections = true;
    EXPECT(!with_speculation.is_consistent());

    auto snapshot = MUST(Privacy::PolicyCompiler::compile(7, 1, { .user_preference = policy })).snapshot;
    Privacy::NetworkGate gate;
    EXPECT(!gate.mandatory_route().has_value());
    EXPECT(gate.install(snapshot).ack.accepted);
    EXPECT_EQ(gate.mandatory_route()->port, 9050);

    EXPECT(Privacy::Hooks::on_before_request(gate, parse_url("https://a.example/"sv)).is_allowed());
    EXPECT(Privacy::Hooks::on_before_websocket_connect(gate, parse_url("wss://a.example/"sv)).is_allowed());
    auto dns = Privacy::Hooks::on_before_dns_lookup(gate, "a.example"sv);
    EXPECT(dns.is_denied());
    EXPECT_EQ(dns.reason, Privacy::PolicyReason::MandatoryRouteUnavailable);
    EXPECT(Privacy::Hooks::on_before_dns_lookup(gate, "a.example"sv, true).is_denied());
    EXPECT(Privacy::Hooks::on_before_request(gate, parse_url("https://a.example/"sv), false, true).is_denied());
    EXPECT(gate.evaluate_webrtc_network().is_denied());

    // A lost coordinator takes the route with it, and with it every admission.
    gate.mark_coordinator_lost();
    EXPECT(!gate.mandatory_route().has_value());
    EXPECT(Privacy::Hooks::on_before_request(gate, parse_url("https://a.example/"sv)).is_denied());
}

TEST_CASE(mandatory_routes_intersect_to_the_same_route_or_to_nothing)
{
    auto tor = Privacy::NetworkPolicy::proxy_only(route("socks5h://127.0.0.1:9050"sv));
    auto other = Privacy::NetworkPolicy::proxy_only(route("http://127.0.0.1:8080"sv));

    // An identity's route narrows a direct-egress preference.
    auto narrowed = MUST(Privacy::PolicyCompiler::compile(7, 1, { .identity_requirement = tor, .user_preference = Privacy::NetworkPolicy::allow_default() }));
    EXPECT_EQ(narrowed.snapshot.network.mode, Privacy::NetworkMode::ProxyOnly);
    EXPECT(narrowed.snapshot.network.route == tor.route);
    EXPECT(!narrowed.snapshot.network.allows_dns_resolution());
    EXPECT(!narrowed.snapshot.network.allow_speculative_connections);

    // The same route twice is still that route.
    auto same = MUST(Privacy::PolicyCompiler::compile(7, 2, { .administrative_ceiling = tor, .user_preference = tor }));
    EXPECT(same.snapshot.network.route == tor.route);
    EXPECT(same.diagnostics.is_empty());

    // Two different required routes leave no route, so egress closes and says why.
    auto conflicting = MUST(Privacy::PolicyCompiler::compile(7, 3, { .administrative_ceiling = tor, .identity_requirement = other }));
    EXPECT_EQ(conflicting.snapshot.network.mode, Privacy::NetworkMode::Offline);
    EXPECT(!conflicting.snapshot.network.route.has_value());
    EXPECT(!conflicting.snapshot.network.allows_network_egress());
    EXPECT(any_of(conflicting.diagnostics, [](auto const& line) { return line.contains("Conflicting mandatory routes"sv); }));

    // Offline wins over any route.
    auto offline = MUST(Privacy::PolicyCompiler::compile(7, 4, { .identity_requirement = Privacy::NetworkPolicy::offline(), .user_preference = tor }));
    EXPECT_EQ(offline.snapshot.network.mode, Privacy::NetworkMode::Offline);

    // The environment's proxy and bypass list give way to the route.
    auto with_environment_proxy = MUST(Privacy::PolicyCompiler::compile(7, 5, { .user_preference = tor, .has_configured_proxy = true }));
    EXPECT(any_of(with_environment_proxy.diagnostics, [](auto const& line) { return line.contains("bypass list"sv); }));
}

TEST_CASE(any_route_change_is_a_tightening)
{
    auto direct = Privacy::NetworkPolicy::allow_default();
    auto tor = Privacy::NetworkPolicy::proxy_only(route("socks5h://127.0.0.1:9050"sv));
    auto other_port = Privacy::NetworkPolicy::proxy_only(route("socks5h://127.0.0.1:9150"sv));
    auto isolated = Privacy::NetworkPolicy::proxy_only(isolated_route("0123456789abcdef0123456789abcdef"sv, 1));
    auto renewed = Privacy::NetworkPolicy::proxy_only(isolated_route("0123456789abcdef0123456789abcdef"sv, 2));

    // Pooled direct connections must not outlive a switch to the route.
    EXPECT(tor.is_tightening_relative_to(direct));
    EXPECT(other_port.is_tightening_relative_to(tor));
    EXPECT(isolated.is_tightening_relative_to(tor));
    EXPECT(renewed.is_tightening_relative_to(isolated));
    EXPECT(!tor.is_tightening_relative_to(tor));
    EXPECT(Privacy::NetworkPolicy::offline().is_tightening_relative_to(tor));

    Privacy::NetworkGate gate;
    auto first = MUST(Privacy::PolicyCompiler::compile(7, 1, { .user_preference = direct })).snapshot;
    auto second = MUST(Privacy::PolicyCompiler::compile(7, 2, { .user_preference = tor })).snapshot;
    auto third = MUST(Privacy::PolicyCompiler::compile(7, 3, { .user_preference = other_port })).snapshot;
    EXPECT(!gate.install(first).requires_quiesce);
    EXPECT(gate.install(second).requires_quiesce);
    EXPECT(gate.install(third).requires_quiesce);
}

TEST_CASE(routes_survive_ipc_and_malformed_routes_do_not)
{
    auto policy = Privacy::NetworkPolicy::proxy_only(isolated_route("0123456789abcdef0123456789abcdef"sv, 3));
    auto snapshot = MUST(Privacy::PolicyCompiler::compile(9, 4, { .user_preference = policy })).snapshot;

    {
        IPC::MessageBuffer buffer;
        IPC::Encoder encoder { buffer };
        MUST(encoder.encode(snapshot));

        FixedMemoryStream stream { buffer.data() };
        Queue<IPC::Attachment> attachments;
        IPC::Decoder decoder { stream, attachments };
        EXPECT(MUST(decoder.decode<Privacy::PolicySnapshot>()) == snapshot);
    }

    // A route that names its proxy by host name must not decode.
    {
        IPC::MessageBuffer buffer;
        IPC::Encoder encoder { buffer };
        MUST(encoder.encode(to_underlying(Privacy::RouteProxyType::SOCKS5Hostname)));
        MUST(encoder.encode(ByteString { "proxy.example" }));
        MUST(encoder.encode(static_cast<u16>(9050)));
        MUST(encoder.encode(ByteString {}));
        MUST(encoder.encode(to_underlying(Privacy::StreamIsolation::None)));
        MUST(encoder.encode(ByteString {}));
        MUST(encoder.encode(static_cast<u64>(0)));

        FixedMemoryStream stream { buffer.data() };
        Queue<IPC::Attachment> attachments;
        IPC::Decoder decoder { stream, attachments };
        EXPECT(decoder.decode<Privacy::ProxyRoute>().is_error());
    }

    // Nor may an unknown proxy type.
    {
        IPC::MessageBuffer buffer;
        IPC::Encoder encoder { buffer };
        MUST(encoder.encode(static_cast<u8>(42)));

        FixedMemoryStream stream { buffer.data() };
        Queue<IPC::Attachment> attachments;
        IPC::Decoder decoder { stream, attachments };
        EXPECT(decoder.decode<Privacy::ProxyRoute>().is_error());
    }
}
