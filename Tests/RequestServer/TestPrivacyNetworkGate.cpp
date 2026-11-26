/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibCore/Socket.h>
#include <LibCore/TCPServer.h>
#include <LibCore/Timer.h>
#include <LibHTTP/Cache/DiskCache.h>
#include <LibIPC/Transport.h>
#include <LibPrivacy/Policy/Compiler.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <RequestServer/CURL.h>
#include <RequestServer/ConnectionFromClient.h>
#include <RequestServer/ControlConnectionFromClient.h>
#include <RequestServer/PrivacyPolicy.h>

namespace {

struct GateTestHarness {
    GateTestHarness()
    {
        static bool libcurl_initialized = [] {
            MUST(RequestServer::initialize_libcurl());
            return true;
        }();
        (void)libcurl_initialized;

        auto control_pair = MUST(IPC::Transport::create_paired());
        control_remote_transport = MUST(control_pair.remote_handle.create_transport());
        control_connection = RequestServer::ControlConnectionFromClient::construct(
            move(control_pair.local), connections, request_transfer_leases, Optional<HTTP::DiskCache&> {});

        auto data_pair = MUST(IPC::Transport::create_paired());
        data_remote_transport = MUST(data_pair.remote_handle.create_transport());
        connection = RequestServer::ConnectionFromClient::construct(
            move(data_pair.local), RequestServer::IsPrivate::No, RequestServer::SiteBinding::Unrestricted,
            connections, request_transfer_leases, Optional<HTTP::DiskCache&> {});
    }

    ~GateTestHarness()
    {
        if (connection && connection->is_open())
            connection->shutdown();
        if (control_connection && control_connection->is_open())
            control_connection->shutdown();
    }

    Privacy::PolicyInstallAck install_policy(Privacy::PolicySnapshot snapshot)
    {
        auto message = make<Messages::RequestServerControl::InstallPrivacyPolicy>(move(snapshot));
        auto response = MUST(static_cast<RequestServerControlEndpoint::Stub&>(*control_connection).handle(move(message)));
        VERIFY(response);
        Queue<IPC::Attachment> attachments;
        auto decoded = MUST(RequestServerControlEndpoint::decode_message(response->data(), attachments));
        VERIFY(decoded->message_id() == Messages::RequestServerControl::InstallPrivacyPolicyResponse::static_message_id());
        return static_cast<Messages::RequestServerControl::InstallPrivacyPolicyResponse&>(*decoded).ack();
    }

    void start_request(u64 request_id, URL::URL url)
    {
        auto message = make<Messages::RequestServer::StartRequest>(
            request_id, ByteString { "GET" }, move(url), Vector<HTTP::Header> {}, ByteBuffer {},
            HTTP::CacheMode::NoStore, Optional<HTTP::NetworkIsolationKey> {}, HTTP::Cookie::IncludeCredentials::No,
            false, Optional<u32> {}, false, 0, HTTP::Cookie::SameSiteContext {}, Requests::ExtensionRequestCheck::NotApplicable);
        VERIFY(!MUST(static_cast<RequestServerEndpoint::Stub&>(*connection).handle(move(message))));
    }

    Optional<Requests::NetworkError> wait_for_request_finished(u64 request_id)
    {
        auto wake_timer = Core::Timer::create_repeating(AK::Duration::from_milliseconds(10), [] { });
        wake_timer->start();

        Optional<Optional<Requests::NetworkError>> network_error;
        event_loop.spin_until([&] {
            (void)data_remote_transport->read_as_many_messages_as_possible_without_blocking([&](auto&& raw_message) {
                auto message = MUST(RequestClientEndpoint::decode_message(raw_message.bytes.bytes(), raw_message.attachments));
                if (message->message_id() != Messages::RequestClient::RequestFinished::static_message_id())
                    return;
                auto& finished = static_cast<Messages::RequestClient::RequestFinished&>(*message);
                if (finished.request_id() == request_id)
                    network_error = finished.network_error();
            });
            return network_error.has_value();
        });
        return *network_error;
    }

    Core::EventLoop event_loop;
    RequestServer::ConnectionFromClient::ConnectionMap connections;
    RequestServer::ConnectionFromClient::RequestTransferLeaseMap request_transfer_leases;
    OwnPtr<IPC::Transport> control_remote_transport;
    RefPtr<RequestServer::ControlConnectionFromClient> control_connection;
    OwnPtr<IPC::Transport> data_remote_transport;
    RefPtr<RequestServer::ConnectionFromClient> connection;
};

}

TEST_CASE(request_server_starts_default_closed_until_policy_installed)
{
    auto server = MUST(Core::TCPServer::try_create());
    MUST(server->listen(IPv4Address { 127, 0, 0, 1 }, 0));
    size_t accepted_connections = 0;
    server->on_ready_to_accept = [&] {
        ++accepted_connections;
        auto socket = MUST(server->accept());
        MUST(socket->write_until_depleted("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok"sv.bytes()));
        socket->close();
    };
    auto url = URL::Parser::basic_parse(ByteString::formatted("http://127.0.0.1:{}/probe", *server->local_port())).release_value();

    GateTestHarness harness;

    // Before InstallPrivacyPolicy arrives, RequestServer must start closed and reject HTTP traffic.
    harness.start_request(1, url);
    auto error = harness.wait_for_request_finished(1);
    EXPECT(error == Requests::NetworkError::BlockedByPrivacyPolicy);
    EXPECT_EQ(accepted_connections, 0u);

    // Invalid schema version is rejected and keeps RequestServer closed.
    auto bad_schema = MUST(Privacy::PolicyCompiler::compile(7, 1, { .user_preference = Privacy::NetworkPolicy::allow_default() })).snapshot;
    bad_schema.schema_version = Privacy::CURRENT_POLICY_SCHEMA_VERSION + 42;
    auto bad_ack = harness.install_policy(bad_schema);
    EXPECT(!bad_ack.accepted);
    EXPECT_EQ(bad_ack.rejection_reason, Privacy::PolicyReason::UnsupportedSchemaVersion);

    harness.start_request(2, url);
    EXPECT(harness.wait_for_request_finished(2) == Requests::NetworkError::BlockedByPrivacyPolicy);
    EXPECT_EQ(accepted_connections, 0u);

    // Installing valid Allow policy opens admissions for the bound session.
    auto allow_v1 = MUST(Privacy::PolicyCompiler::compile(7, 1, { .user_preference = Privacy::NetworkPolicy::allow_default() })).snapshot;
    auto allow_ack = harness.install_policy(allow_v1);
    EXPECT(allow_ack.accepted);
    EXPECT_EQ(allow_ack.committed_epoch, 1u);

    harness.start_request(3, url);
    EXPECT(!harness.wait_for_request_finished(3).has_value());
    EXPECT_EQ(accepted_connections, 1u);

    // Stale epoch and wrong session ID are rejected without disturbing epoch 1.
    auto stale_ack = harness.install_policy(allow_v1);
    EXPECT(!stale_ack.accepted);
    EXPECT_EQ(stale_ack.rejection_reason, Privacy::PolicyReason::StaleEpoch);

    auto wrong_session = MUST(Privacy::PolicyCompiler::compile(8, 2, { .user_preference = Privacy::NetworkPolicy::offline() })).snapshot;
    auto mismatch_ack = harness.install_policy(wrong_session);
    EXPECT(!mismatch_ack.accepted);
    EXPECT_EQ(mismatch_ack.rejection_reason, Privacy::PolicyReason::SessionMismatch);

    // Tightening to Offline (epoch 2) denies subsequent requests with zero new connections.
    auto offline_v2 = MUST(Privacy::PolicyCompiler::compile(7, 2, { .user_preference = Privacy::NetworkPolicy::offline() })).snapshot;
    auto offline_ack = harness.install_policy(offline_v2);
    EXPECT(offline_ack.accepted);
    EXPECT_EQ(offline_ack.committed_epoch, 2u);

    harness.start_request(4, url);
    EXPECT(harness.wait_for_request_finished(4) == Requests::NetworkError::BlockedByPrivacyPolicy);
    EXPECT_EQ(accepted_connections, 1u);
}

TEST_CASE(coordinator_loss_closes_admissions_and_aborts_in_flight_request)
{
    auto server = MUST(Core::TCPServer::try_create());
    MUST(server->listen(IPv4Address { 127, 0, 0, 1 }, 0));
    Vector<NonnullOwnPtr<Core::TCPSocket>> held_sockets;
    server->on_ready_to_accept = [&] {
        auto socket = MUST(server->accept());
        // Hold the socket open without answering to simulate an in-flight transfer when Hub dies.
        held_sockets.append(move(socket));
    };
    auto url = URL::Parser::basic_parse(ByteString::formatted("http://127.0.0.1:{}/hang", *server->local_port())).release_value();

    GateTestHarness harness;
    auto allow_v1 = MUST(Privacy::PolicyCompiler::compile(11, 1, { .user_preference = Privacy::NetworkPolicy::allow_default() })).snapshot;
    EXPECT(harness.install_policy(allow_v1).accepted);

    harness.start_request(1, url);
    harness.event_loop.spin_until([&] { return !held_sockets.is_empty(); });

    // Simulate coordinator loss.
    harness.control_connection->die();

    // The in-flight request fails immediately with BlockedByPrivacyPolicy.
    EXPECT(harness.wait_for_request_finished(1) == Requests::NetworkError::BlockedByPrivacyPolicy);

    // Subsequent requests are denied without ever touching the server.
    size_t connections_before = held_sockets.size();
    harness.start_request(2, url);
    EXPECT(harness.wait_for_request_finished(2) == Requests::NetworkError::BlockedByPrivacyPolicy);
    EXPECT_EQ(held_sockets.size(), connections_before);
}
