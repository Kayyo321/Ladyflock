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
#include <LibPrivacy/Policy/Types.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>

static URL::URL parse_url(StringView input)
{
    return URL::Parser::basic_parse(input).release_value();
}

TEST_CASE(default_closed_before_policy_install)
{
    Privacy::NetworkGate gate;
    EXPECT_EQ(gate.state(), Privacy::NetworkGate::State::Uninitialized);
    EXPECT_EQ(gate.committed_epoch(), 0u);
    EXPECT(!gate.bound_session_id().has_value());
    EXPECT(!gate.is_epoch_current(0));
    EXPECT(!gate.is_epoch_current(1));

    auto http_result = Privacy::Hooks::on_before_request(gate, parse_url("http://example.test/"sv));
    EXPECT(http_result.is_denied());
    EXPECT_EQ(http_result.reason, Privacy::PolicyReason::UninitializedClosed);

    auto https_result = Privacy::Hooks::on_before_request(gate, parse_url("https://example.test/"sv));
    EXPECT(https_result.is_denied());
    EXPECT_EQ(https_result.reason, Privacy::PolicyReason::UninitializedClosed);

    auto loopback_result = Privacy::Hooks::on_before_request(gate, parse_url("http://127.0.0.1:8080/"sv));
    EXPECT(loopback_result.is_denied());
    EXPECT_EQ(loopback_result.reason, Privacy::PolicyReason::UninitializedClosed);

    auto dns_result = Privacy::Hooks::on_before_dns_lookup(gate, "example.test"sv, false);
    EXPECT(dns_result.is_denied());
    EXPECT_EQ(dns_result.reason, Privacy::PolicyReason::UninitializedClosed);

    auto sys_dns_result = Privacy::Hooks::on_before_dns_lookup(gate, "example.test"sv, true);
    EXPECT(sys_dns_result.is_denied());
    EXPECT_EQ(sys_dns_result.reason, Privacy::PolicyReason::UninitializedClosed);

    auto ws_result = Privacy::Hooks::on_before_websocket_connect(gate, parse_url("wss://example.test/ws"sv));
    EXPECT(ws_result.is_denied());
    EXPECT_EQ(ws_result.reason, Privacy::PolicyReason::UninitializedClosed);

    auto aia_result = Privacy::Hooks::on_before_aia_fetch(gate, parse_url("http://ca.example.test/inter.cer"sv));
    EXPECT(aia_result.is_denied());
    EXPECT_EQ(aia_result.reason, Privacy::PolicyReason::UninitializedClosed);

    auto cache_result = Privacy::Hooks::on_before_fresh_cache_read(gate);
    EXPECT(cache_result.is_denied());
    EXPECT_EQ(cache_result.reason, Privacy::PolicyReason::UninitializedClosed);

    EXPECT(gate.evaluate_webrtc_network().is_denied());
    EXPECT(gate.evaluate_background_service("update-check"sv).is_denied());
}

TEST_CASE(policy_compiler_precedence_and_validation)
{
    // Standard allow policy compiles cleanly.
    {
        Privacy::PolicyInput input {
            .user_preference = Privacy::NetworkPolicy::allow_default(),
        };
        auto compiled = Privacy::PolicyCompiler::compile(1, 1, input);
        EXPECT(!compiled.is_error());
        EXPECT(compiled.value().snapshot.is_valid());
        EXPECT(compiled.value().snapshot.network.allows_network_egress());
        EXPECT(compiled.value().diagnostics.is_empty());
    }

    // Offline identity with an administrator-configured proxy keeps networking closed and reports conflict.
    {
        Privacy::PolicyInput input {
            .identity_requirement = Privacy::NetworkPolicy::offline(),
            .user_preference = Privacy::NetworkPolicy::allow_default(),
            .has_configured_proxy = true,
        };
        auto compiled = Privacy::PolicyCompiler::compile(42, 1, input);
        EXPECT(!compiled.is_error());
        EXPECT(!compiled.value().snapshot.network.allows_network_egress());
        EXPECT(!compiled.value().snapshot.network.allows_dns_resolution());
        EXPECT(!compiled.value().snapshot.network.allows_webrtc_network());
        EXPECT(compiled.value().snapshot.network.allows_fresh_cache_without_revalidation());
        EXPECT(!compiled.value().diagnostics.is_empty());
    }

    // Administrative Offline ceiling cannot be widened by user preference.
    {
        Privacy::PolicyInput input {
            .administrative_ceiling = Privacy::NetworkPolicy::offline(),
            .user_preference = Privacy::NetworkPolicy::allow_default(),
        };
        auto compiled = Privacy::PolicyCompiler::compile(7, 2, input);
        EXPECT(!compiled.is_error());
        EXPECT(!compiled.value().snapshot.network.allows_network_egress());
        EXPECT(!compiled.value().diagnostics.is_empty());
    }

    // Unsupported schema version is rejected.
    {
        Privacy::PolicyInput input {
            .schema_version = Privacy::CURRENT_POLICY_SCHEMA_VERSION + 1,
            .user_preference = Privacy::NetworkPolicy::allow_default(),
        };
        auto compiled = Privacy::PolicyCompiler::compile(1, 1, input);
        EXPECT(compiled.is_error());
        EXPECT_EQ(compiled.error(), Privacy::PolicyReason::UnsupportedSchemaVersion);
    }

    // Contradictory Offline policy (claiming open WebSockets or DNS) is rejected.
    {
        auto contradictory = Privacy::NetworkPolicy::offline();
        contradictory.allow_websockets = true;
        EXPECT(!contradictory.is_consistent());

        Privacy::PolicyInput input {
            .user_preference = contradictory,
        };
        auto compiled = Privacy::PolicyCompiler::compile(1, 1, input);
        EXPECT(compiled.is_error());
        EXPECT_EQ(compiled.error(), Privacy::PolicyReason::InvalidPolicy);
    }

    // Zero session ID or zero epoch is rejected.
    {
        Privacy::PolicyInput input {};
        EXPECT(Privacy::PolicyCompiler::compile(0, 1, input).is_error());
        EXPECT(Privacy::PolicyCompiler::compile(1, 0, input).is_error());
    }
}

TEST_CASE(epoch_monotonicity_session_binding_and_tightening)
{
    Privacy::NetworkGate gate;

    auto allow_v1 = MUST(Privacy::PolicyCompiler::compile(10, 1, { .user_preference = Privacy::NetworkPolicy::allow_default() })).snapshot;
    auto outcome1 = gate.install(allow_v1);
    EXPECT(outcome1.ack.accepted);
    EXPECT_EQ(outcome1.ack.committed_epoch, 1u);
    EXPECT(!outcome1.requires_quiesce);
    EXPECT(gate.is_epoch_current(1));
    EXPECT(Privacy::Hooks::on_before_request(gate, parse_url("https://example.test/page"sv)).is_allowed());
    EXPECT(Privacy::Hooks::on_before_dns_lookup(gate, "example.test"sv, true).is_allowed());
    EXPECT(Privacy::Hooks::on_before_websocket_connect(gate, parse_url("wss://example.test/sock"sv)).is_allowed());
    EXPECT(gate.evaluate_webrtc_network().is_allowed());

    // Replaying the same epoch is rejected, keeping committed state intact.
    auto replay = gate.install(allow_v1);
    EXPECT(!replay.ack.accepted);
    EXPECT_EQ(replay.ack.rejection_reason, Privacy::PolicyReason::StaleEpoch);
    EXPECT_EQ(replay.ack.committed_epoch, 1u);

    // Wrong session ID is rejected.
    auto wrong_session = MUST(Privacy::PolicyCompiler::compile(99, 2, { .user_preference = Privacy::NetworkPolicy::offline() })).snapshot;
    auto mismatch = gate.install(wrong_session);
    EXPECT(!mismatch.ack.accepted);
    EXPECT_EQ(mismatch.ack.rejection_reason, Privacy::PolicyReason::SessionMismatch);
    EXPECT_EQ(gate.committed_epoch(), 1u);

    // Unsupported schema version is rejected without dropping last accepted policy.
    auto bad_schema = allow_v1;
    bad_schema.schema_version = 999;
    bad_schema.epoch = 2;
    auto schema_reject = gate.install(bad_schema);
    EXPECT(!schema_reject.ack.accepted);
    EXPECT_EQ(schema_reject.ack.rejection_reason, Privacy::PolicyReason::UnsupportedSchemaVersion);
    EXPECT_EQ(gate.committed_epoch(), 1u);

    // Tightening Allow -> Offline requires quiesce and invalidates epoch 1.
    auto offline_v2 = MUST(Privacy::PolicyCompiler::compile(10, 2, { .user_preference = Privacy::NetworkPolicy::offline() })).snapshot;
    auto outcome2 = gate.install(offline_v2);
    EXPECT(outcome2.ack.accepted);
    EXPECT_EQ(outcome2.ack.committed_epoch, 2u);
    EXPECT(outcome2.requires_quiesce);
    EXPECT(!gate.is_epoch_current(1));
    EXPECT(gate.is_epoch_current(2));

    // Offline denies HTTP, HTTPS, IPv4/IPv6 loopback, DNS, WebSocket, AIA, WebRTC, speculative, and background services.
    EXPECT_EQ(Privacy::Hooks::on_before_request(gate, parse_url("http://example.test/"sv)).reason, Privacy::PolicyReason::OfflineMode);
    EXPECT_EQ(Privacy::Hooks::on_before_request(gate, parse_url("https://example.test/"sv)).reason, Privacy::PolicyReason::OfflineMode);
    EXPECT_EQ(Privacy::Hooks::on_before_request(gate, parse_url("http://127.0.0.1:1234/"sv)).reason, Privacy::PolicyReason::OfflineMode);
    EXPECT_EQ(Privacy::Hooks::on_before_request(gate, parse_url("http://[::1]:1234/"sv)).reason, Privacy::PolicyReason::OfflineMode);
    EXPECT_EQ(Privacy::Hooks::on_before_request(gate, parse_url("http://example.test/"sv), true, false).reason, Privacy::PolicyReason::OfflineMode);
    EXPECT_EQ(Privacy::Hooks::on_before_request(gate, parse_url("http://example.test/"sv), false, true).reason, Privacy::PolicyReason::OfflineMode);
    EXPECT_EQ(Privacy::Hooks::on_before_dns_lookup(gate, "localhost"sv, true).reason, Privacy::PolicyReason::OfflineMode);
    EXPECT_EQ(Privacy::Hooks::on_before_websocket_connect(gate, parse_url("ws://127.0.0.1:9000/"sv)).reason, Privacy::PolicyReason::OfflineMode);
    EXPECT_EQ(Privacy::Hooks::on_before_aia_fetch(gate, parse_url("http://ca.test/cert"sv)).reason, Privacy::PolicyReason::OfflineMode);
    EXPECT_EQ(gate.evaluate_webrtc_network().reason, Privacy::PolicyReason::OfflineMode);
    EXPECT_EQ(gate.evaluate_background_service("content-blocker-update"sv).reason, Privacy::PolicyReason::OfflineMode);

    // Offline still allows eligible fresh cache reads without network revalidation.
    EXPECT(Privacy::Hooks::on_before_fresh_cache_read(gate).is_allowed());

    // Coordinator loss permanently closes everything, including cache reads and future installs.
    gate.mark_coordinator_lost();
    EXPECT_EQ(gate.state(), Privacy::NetworkGate::State::CoordinatorLost);
    EXPECT(!gate.is_epoch_current(2));
    EXPECT_EQ(Privacy::Hooks::on_before_request(gate, parse_url("http://example.test/"sv)).reason, Privacy::PolicyReason::CoordinatorLost);
    EXPECT_EQ(Privacy::Hooks::on_before_fresh_cache_read(gate).reason, Privacy::PolicyReason::CoordinatorLost);

    auto allow_v3 = MUST(Privacy::PolicyCompiler::compile(10, 3, { .user_preference = Privacy::NetworkPolicy::allow_default() })).snapshot;
    auto after_loss = gate.install(allow_v3);
    EXPECT(!after_loss.ack.accepted);
    EXPECT_EQ(after_loss.ack.rejection_reason, Privacy::PolicyReason::CoordinatorLost);
}

TEST_CASE(ipc_roundtrip_and_invalid_enum_rejection)
{
    auto snapshot = MUST(Privacy::PolicyCompiler::compile(123, 5, { .user_preference = Privacy::NetworkPolicy::offline() })).snapshot;

    {
        IPC::MessageBuffer buffer;
        IPC::Encoder encoder { buffer };
        MUST(encoder.encode(snapshot));

        FixedMemoryStream stream { buffer.data() };
        Queue<IPC::Attachment> attachments;
        IPC::Decoder decoder { stream, attachments };
        auto decoded = MUST(decoder.decode<Privacy::PolicySnapshot>());
        EXPECT_EQ(decoded, snapshot);
    }

    {
        Privacy::PolicyInstallAck ack { .accepted = true, .committed_epoch = 5, .rejection_reason = Privacy::PolicyReason::None };
        IPC::MessageBuffer buffer;
        IPC::Encoder encoder { buffer };
        MUST(encoder.encode(ack));

        FixedMemoryStream stream { buffer.data() };
        Queue<IPC::Attachment> attachments;
        IPC::Decoder decoder { stream, attachments };
        auto decoded = MUST(decoder.decode<Privacy::PolicyInstallAck>());
        EXPECT_EQ(decoded, ack);
    }

    // Corrupt NetworkMode byte must fail decoding.
    {
        IPC::MessageBuffer buffer;
        IPC::Encoder encoder { buffer };
        MUST(encoder.encode(static_cast<u8>(99)));
        MUST(encoder.encode(static_cast<u8>(0)));
        MUST(encoder.encode(static_cast<u8>(0)));
        MUST(encoder.encode(static_cast<u8>(0)));
        MUST(encoder.encode(false));
        MUST(encoder.encode(false));
        MUST(encoder.encode(false));
        MUST(encoder.encode(false));

        FixedMemoryStream stream { buffer.data() };
        Queue<IPC::Attachment> attachments;
        IPC::Decoder decoder { stream, attachments };
        auto decoded = decoder.decode<Privacy::NetworkPolicy>();
        EXPECT(decoded.is_error());
    }
}
