/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Time.h>
#include <LibCore/Timer.h>
#include <LibDNS/Resolver.h>
#include <LibHTTP/Cache/DiskCacheSettings.h>
#include <LibHTTP/Forward.h>
#include <LibIPC/ConnectionFromClient.h>
#include <RequestServer/CURL.h>
#include <RequestServer/ConnectionFromClient.h>
#include <RequestServer/Forward.h>
#include <RequestServer/RequestServerControlClientEndpoint.h>
#include <RequestServer/RequestServerControlEndpoint.h>

namespace RequestServer {

// The control plane of RequestServer. Only the process that launched RequestServer speaks this endpoint, over the
// initial socket. Everything that applies to the process as a whole lives here; data connections cannot reach it.
class ControlConnectionFromClient final
    : public IPC::ConnectionFromClient<RequestServerControlClientEndpoint, RequestServerControlEndpoint> {
    C_OBJECT(ControlConnectionFromClient);

public:
    ~ControlConnectionFromClient() override;

    virtual void die() override;

    static Optional<ControlConnectionFromClient&> the();

    // Resolves a name through the system resolver of the process that launched RequestServer.
    DNS::Resolver::CancelRemoteSystemResolution resolve_host_with_system_resolver(ByteString const& host, Core::Socket::AddressFamily, Function<void(DNS::Resolver::SystemResolution)>);

    bool should_check_web_request(URL::URL const&) const;
    void check_web_request_before_request(int client_id, URL::URL const&, ByteString const& method, Function<void(bool cancel)> callback);

private:
    ControlConnectionFromClient(NonnullOwnPtr<IPC::Transport>, RequestServer::ConnectionFromClient::ConnectionMap&, RequestServer::ConnectionFromClient::RequestTransferLeaseMap&, Optional<HTTP::DiskCache&>);

    virtual Messages::RequestServerControl::ConnectNewClientResponse connect_new_client(IsPrivate, SiteBinding) override;
    virtual Messages::RequestServerControl::ConnectNewClientsResponse connect_new_clients(size_t count, IsPrivate, SiteBinding) override;
    virtual void bind_client_to_site(int client_id, Utf16String top_level_site, Optional<Utf16String> frame_site) override;

    virtual void designate_request_adopter(u64 designation_id, int source_client_id, u64 source_request_id, int adopter_client_id) override;

    virtual void set_disk_cache_settings(HTTP::DiskCacheSettings) override;

    virtual void set_dns_server(ByteString host_or_address, u16 port, bool use_tls, bool validate_dnssec_locally) override;
    virtual void set_use_system_dns() override;

    virtual void set_proxy_configuration(HTTP::ProxyConfiguration) override;

    virtual void trust_certificate(ByteString host, u16 port, ByteString fingerprint) override;

    virtual void set_performance_monitor_enabled(bool) override;

    virtual void estimate_cache_size_accessed_since(u64 cache_size_estimation_id, UnixDateTime since) override;
    virtual void remove_cache_entries_accessed_since(u64 clear_cache_request_id, UnixDateTime since) override;
    virtual void retrieve_cache_access_profile(u64 access_profile_request_id, AK::Duration granularity) override;
    virtual void remove_cache_entries_accessed_before(UnixDateTime before) override;

    virtual void retrieved_http_cookie(int client_id, u64 request_id, RequestType request_type, u64 cookie_request_id, String cookie) override;
    virtual void stored_response_cookies_and_hsts_policy(int client_id, u64 request_id, u64 store_request_id) override;

    virtual void resolved_host_with_system_resolver(u64 resolution_id, Optional<Vector<ByteString>> addresses, Optional<ByteString> canonical_name) override;

    virtual void set_web_request_watch_filter(bool enabled, bool match_all_urls, Vector<String> exact_hosts, Vector<String> suffix_hosts) override;
    virtual void did_check_web_request_before_request(u64 check_id, bool cancel) override;

    virtual Messages::RequestServerControl::InstallPrivacyPolicyResponse install_privacy_policy(Privacy::PolicySnapshot snapshot) override;

    void quiesce_active_network_work();

    struct ClientSocket {
        IPC::TransportHandle handle;
        int client_id { -1 };
    };
    ErrorOr<ClientSocket> create_client_socket(IsPrivate, SiteBinding);
    void push_network_usage();

    RequestServer::ConnectionFromClient::ConnectionMap& m_connections;
    RequestServer::ConnectionFromClient::RequestTransferLeaseMap& m_request_transfer_leases;
    Optional<HTTP::DiskCache&> m_disk_cache;

    NonnullRefPtr<Resolver> m_resolver;

    RefPtr<Core::Timer> m_disk_cache_size_report_timer;

    RefPtr<Core::Timer> m_performance_timer;
    Optional<MonotonicTime> m_last_performance_push;

    HashMap<u64, Function<void(DNS::Resolver::SystemResolution)>> m_pending_system_resolutions;
    HashTable<u64> m_cancelled_system_resolutions;
    u64 m_next_system_resolution_id { 0 };

    bool m_web_request_watch_enabled { false };
    bool m_web_request_match_all_urls { false };
    Vector<String> m_web_request_exact_hosts;
    Vector<String> m_web_request_suffix_hosts;
    HashMap<u64, Function<void(bool cancel)>> m_pending_web_request_checks;
    u64 m_next_web_request_check_id { 1 };
};

}
