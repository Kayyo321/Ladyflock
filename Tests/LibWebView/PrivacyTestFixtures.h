/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/Base64.h>
#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/ScopeGuard.h>
#include <AK/StringBuilder.h>
#include <AK/Vector.h>
#include <LibCore/File.h>
#include <LibCrypto/Hash/Hash.h>
#include <LibThreading/Thread.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

namespace PrivacyTest {

// EC/prime256v1 test CA ("Certificate Exception Test CA") + leaf cert for DNS:device.test (valid 2020-2050).
inline constexpr StringView test_ca_certificate_pem = R"PEM(-----BEGIN CERTIFICATE-----
MIIBdDCCARmgAwIBAgIBATAKBggqhkjOPQQDAjAoMSYwJAYDVQQDDB1DZXJ0aWZp
Y2F0ZSBFeGNlcHRpb24gVGVzdCBDQTAgFw0yMDAxMDEwMDAwMDBaGA8yMDUwMDEw
MTAwMDAwMFowKDEmMCQGA1UEAwwdQ2VydGlmaWNhdGUgRXhjZXB0aW9uIFRlc3Qg
Q0EwWTATBgcqhkjOPQIBBggqhkjOPQMBBwNCAATEEi1yk5a7Sxn3oAC7KR8FPPAJ
qfOQXFxopx97KL3+llTlqWixgpxYcNm+WHryfAgTGo52y0OGKtAMqQp8xiLdozIw
MDAdBgNVHQ4EFgQU2xKqM4jS50/iBg0ClmBTVoKav7owDwYDVR0TAQH/BAUwAwEB
/zAKBggqhkjOPQQDAgNJADBGAiEAoYypwtjKWGMJWq3owgdvmMkRFUfxyFkq6Rb6
Ki145MkCIQDl+r81A2QTv68DQXITfh0KweDR/su9BcHjfPcoN6Kp7w==
-----END CERTIFICATE-----)PEM"sv;

inline constexpr StringView device_test_certificate_pem = R"PEM(-----BEGIN CERTIFICATE-----
MIIBVjCB/aADAgECAgECMAoGCCqGSM49BAMCMCgxJjAkBgNVBAMMHUNlcnRpZmlj
YXRlIEV4Y2VwdGlvbiBUZXN0IENBMCAXDTIwMDEwMTAwMDAwMFoYDzIwNTAwMTAx
MDAwMDAwWjAWMRQwEgYDVQQDDAtkZXZpY2UudGVzdDBZMBMGByqGSM49AgEGCCqG
SM49AwEHA0IABBp8Gr7JlmkYUyIg8blVKdDebqE2af/OjjAUFjFv1Y8Mswi8gDW3
6JRWhCV4wxPWhJ000b7WVfv9GkRTOKhp52OjKDAmMBYGA1UdEQQPMA2CC2Rldmlj
ZS50ZXN0MAwGA1UdEwEB/wQCMAAwCgYIKoZIzj0EAwIDSAAwRQIhAJLsK1rTUs3o
zg5qBMBnPx2PR9W88Vvb1VIiJR3L7FHaAiBER0MVsgsJagepf7V27sQpU8gZPGKy
SsZNJ2jsOiQ40g==
-----END CERTIFICATE-----)PEM"sv;

inline constexpr StringView device_test_key_pem = R"PEM(-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgTS9Xd7fs6qsAyqCu
7g6UKt+UqB8/PmOuaFLEzDmDM/ihRANCAAQafBq+yZZpGFMiIPG5VSnQ3m6hNmn/
zo4wFBYxb9WPDLMIvIA1t+iUVoQleMMT1oSdNNG+1lX7/RpEUzioaedj
-----END PRIVATE KEY-----)PEM"sv;

inline ByteString write_test_ca_file(ByteString const& directory)
{
    auto path = ByteString::formatted("{}/test-ca.pem", directory);
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Write));
    MUST(file->write_until_depleted(test_ca_certificate_pem.bytes()));
    return path;
}

// Independent UDP DNS observer server on 127.0.0.1 running on its own OS thread.
// Counts every UDP query packet received and answers A -> 127.0.0.1 (and optionally AAAA -> ::1).
class IndependentDNSObserver {
public:
    explicit IndependentDNSObserver(bool answer_aaaa_with_loopback = false)
        : m_answer_aaaa_with_loopback(answer_aaaa_with_loopback)
    {
        m_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        VERIFY(m_fd >= 0);

        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        VERIFY(::bind(m_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

        socklen_t len = sizeof(addr);
        VERIFY(::getsockname(m_fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
        m_port = ntohs(addr.sin_port);

        m_thread = Threading::Thread::construct("PrivacyDNSObserver"sv, [this] {
            run();
            return 0;
        });
        m_thread->start();
    }

    ~IndependentDNSObserver()
    {
        m_stop.store(true);
        if (m_fd >= 0) {
            ::close(m_fd);
            m_fd = -1;
        }
        if (m_thread)
            (void)m_thread->join();
    }

    u16 port() const { return m_port; }
    size_t query_count() const { return m_query_count.load(); }
    void reset_counts() { m_query_count.store(0); }

private:
    void run()
    {
        u8 packet[2048];
        while (!m_stop.load()) {
            pollfd pfd { .fd = m_fd, .events = POLLIN, .revents = 0 };
            int ready = ::poll(&pfd, 1, 25);
            if (ready <= 0)
                continue;

            sockaddr_in from {};
            socklen_t from_len = sizeof(from);
            ssize_t n = ::recvfrom(m_fd, packet, sizeof(packet), 0, reinterpret_cast<sockaddr*>(&from), &from_len);
            if (n < 12)
                continue;

            m_query_count.fetch_add(1);

            auto response = build_response({ packet, static_cast<size_t>(n) });
            if (!response.is_empty())
                (void)::sendto(m_fd, response.data(), response.size(), 0, reinterpret_cast<sockaddr const*>(&from), from_len);
        }
    }

    ByteBuffer build_response(ReadonlyBytes query) const
    {
        if (query.size() < 12)
            return {};

        size_t offset = 12;
        while (offset < query.size() && query[offset] != 0) {
            if ((query[offset] & 0xc0) != 0)
                return {};
            offset += query[offset] + 1;
        }
        if (offset + 5 > query.size())
            return {};

        u16 qtype = static_cast<u16>((query[offset + 1] << 8) | query[offset + 2]);
        size_t question_end = offset + 5;

        auto response = MUST(ByteBuffer::copy(query.slice(0, question_end)));
        response[2] |= 0x80; // QR = response
        response[3] |= 0x80; // RA = recursion available
        response[6] = 0;
        response[7] = 0;
        response[8] = 0;
        response[9] = 0;
        response[10] = 0;
        response[11] = 0;

        if (qtype == 1) {
            // A -> 127.0.0.1
            response[7] = 1;
            u8 const answer[] = {
                0xc0, 0x0c,
                0x00, 0x01,
                0x00, 0x01,
                0x00, 0x00, 0x00, 0x3c,
                0x00, 0x04,
                127, 0, 0, 1,
            };
            response.append(answer, sizeof(answer));
        } else if (qtype == 28 && m_answer_aaaa_with_loopback) {
            // AAAA -> ::1
            response[7] = 1;
            u8 const answer[] = {
                0xc0, 0x0c,
                0x00, 0x1c,
                0x00, 0x01,
                0x00, 0x00, 0x00, 0x3c,
                0x00, 0x10,
                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,
            };
            response.append(answer, sizeof(answer));
        }
        return response;
    }

    int m_fd { -1 };
    u16 m_port { 0 };
    bool m_answer_aaaa_with_loopback { false };
    Atomic<bool> m_stop { false };
    Atomic<size_t> m_query_count { 0 };
    RefPtr<Threading::Thread> m_thread;
};

// Independent HTTP/HTTPS observer server (IPv4 or IPv6) running on its own OS thread.
// Counts every TCP connection and HTTP request received outside the browser.
class IndependentHTTPServer {
public:
    enum class Protocol {
        HTTP,
        HTTPS,
    };

    enum class Family {
        IPv4,
        IPv6,
    };

    using RequestHandler = Function<ByteString(ByteString const& request_head)>;

    IndependentHTTPServer(Protocol protocol, Family family = Family::IPv4, RequestHandler handler = {})
        : m_protocol(protocol)
        , m_family(family)
        , m_handler(move(handler))
    {
        if (m_protocol == Protocol::HTTPS) {
            m_ssl_ctx = SSL_CTX_new(TLS_server_method());
            VERIFY(m_ssl_ctx);
            load_certificate_and_key();
        }

        int domain = m_family == Family::IPv6 ? AF_INET6 : AF_INET;
        m_listen_fd = ::socket(domain, SOCK_STREAM, 0);
        VERIFY(m_listen_fd >= 0);

        int yes = 1;
        (void)::setsockopt(m_listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        if (m_family == Family::IPv6) {
            int v6only = 1;
            (void)::setsockopt(m_listen_fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
            sockaddr_in6 addr {};
            addr.sin6_family = AF_INET6;
            addr.sin6_addr = in6addr_loopback;
            addr.sin6_port = 0;
            VERIFY(::bind(m_listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
            socklen_t len = sizeof(addr);
            VERIFY(::getsockname(m_listen_fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
            m_port = ntohs(addr.sin6_port);
        } else {
            sockaddr_in addr {};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = 0;
            VERIFY(::bind(m_listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
            socklen_t len = sizeof(addr);
            VERIFY(::getsockname(m_listen_fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
            m_port = ntohs(addr.sin_port);
        }

        VERIFY(::listen(m_listen_fd, 32) == 0);

        m_thread = Threading::Thread::construct("PrivacyHTTPObserver"sv, [this] {
            run();
            return 0;
        });
        m_thread->start();
    }

    ~IndependentHTTPServer()
    {
        m_stop.store(true);
        if (m_listen_fd >= 0) {
            ::shutdown(m_listen_fd, SHUT_RDWR);
            ::close(m_listen_fd);
            m_listen_fd = -1;
        }
        if (m_thread)
            (void)m_thread->join();
        if (m_ssl_ctx)
            SSL_CTX_free(m_ssl_ctx);
    }

    u16 port() const { return m_port; }
    size_t connection_count() const { return m_connection_count.load(); }
    size_t request_count() const { return m_request_count.load(); }

    void reset_counts()
    {
        m_connection_count.store(0);
        m_request_count.store(0);
    }

private:
    void load_certificate_and_key()
    {
        BIO* cert_bio = BIO_new_mem_buf(device_test_certificate_pem.characters_without_null_termination(), static_cast<int>(device_test_certificate_pem.length()));
        VERIFY(cert_bio);
        X509* cert = PEM_read_bio_X509(cert_bio, nullptr, nullptr, nullptr);
        BIO_free(cert_bio);
        VERIFY(cert);
        VERIFY(SSL_CTX_use_certificate(m_ssl_ctx, cert) == 1);
        X509_free(cert);

        BIO* key_bio = BIO_new_mem_buf(device_test_key_pem.characters_without_null_termination(), static_cast<int>(device_test_key_pem.length()));
        VERIFY(key_bio);
        EVP_PKEY* key = PEM_read_bio_PrivateKey(key_bio, nullptr, nullptr, nullptr);
        BIO_free(key_bio);
        VERIFY(key);
        VERIFY(SSL_CTX_use_PrivateKey(m_ssl_ctx, key) == 1);
        EVP_PKEY_free(key);
    }

    void run()
    {
        while (!m_stop.load()) {
            pollfd pfd { .fd = m_listen_fd, .events = POLLIN, .revents = 0 };
            int ready = ::poll(&pfd, 1, 25);
            if (ready <= 0)
                continue;

            int client_fd = ::accept(m_listen_fd, nullptr, nullptr);
            if (client_fd < 0)
                continue;

            m_connection_count.fetch_add(1);
            handle_client(client_fd);
            ::close(client_fd);
        }
    }

    void handle_client(int client_fd)
    {
        timeval tv { .tv_sec = 2, .tv_usec = 0 };
        (void)::setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        (void)::setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        SSL* ssl = nullptr;
        ScopeGuard free_ssl = [&] {
            if (ssl) {
                SSL_shutdown(ssl);
                SSL_free(ssl);
            }
        };

        if (m_protocol == Protocol::HTTPS) {
            ssl = SSL_new(m_ssl_ctx);
            VERIFY(ssl);
            SSL_set_fd(ssl, client_fd);
            if (SSL_accept(ssl) != 1)
                return;
        }

        ByteBuffer request_bytes;
        char chunk[4096];
        while (request_bytes.size() < 64 * KiB) {
            ssize_t n = ssl ? SSL_read(ssl, chunk, sizeof(chunk)) : ::recv(client_fd, chunk, sizeof(chunk), 0);
            if (n <= 0)
                break;
            request_bytes.append(chunk, static_cast<size_t>(n));
            if (StringView { request_bytes.bytes() }.contains("\r\n\r\n"sv))
                break;
        }

        if (request_bytes.is_empty())
            return;

        m_request_count.fetch_add(1);
        ByteString request_head { request_bytes.bytes() };
        ByteString response = m_handler
            ? m_handler(request_head)
            : ByteString { "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok" };

        size_t sent = 0;
        while (sent < response.length()) {
            ssize_t n = ssl
                ? SSL_write(ssl, response.characters() + sent, static_cast<int>(response.length() - sent))
                : ::send(client_fd, response.characters() + sent, response.length() - sent, 0);
            if (n <= 0)
                break;
            sent += static_cast<size_t>(n);
        }
    }

    Protocol m_protocol;
    Family m_family;
    RequestHandler m_handler;
    SSL_CTX* m_ssl_ctx { nullptr };
    int m_listen_fd { -1 };
    u16 m_port { 0 };
    Atomic<bool> m_stop { false };
    Atomic<size_t> m_connection_count { 0 };
    Atomic<size_t> m_request_count { 0 };
    RefPtr<Threading::Thread> m_thread;
};

// Independent HTTP server that sends 200 OK headers + a first chunked body chunk, then holds the
// socket open until the client closes/aborts it (recording client disconnect count).
class IndependentStreamingHTTPServer {
public:
    IndependentStreamingHTTPServer()
    {
        m_listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        VERIFY(m_listen_fd >= 0);

        int yes = 1;
        (void)::setsockopt(m_listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        VERIFY(::bind(m_listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        socklen_t len = sizeof(addr);
        VERIFY(::getsockname(m_listen_fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
        m_port = ntohs(addr.sin_port);

        VERIFY(::listen(m_listen_fd, 8) == 0);

        m_thread = Threading::Thread::construct("PrivacyStreamObserver"sv, [this] {
            run();
            return 0;
        });
        m_thread->start();
    }

    ~IndependentStreamingHTTPServer()
    {
        m_stop.store(true);
        if (m_listen_fd >= 0) {
            ::shutdown(m_listen_fd, SHUT_RDWR);
            ::close(m_listen_fd);
            m_listen_fd = -1;
        }
        if (m_thread)
            (void)m_thread->join();
    }

    u16 port() const { return m_port; }
    size_t connection_count() const { return m_connection_count.load(); }
    size_t request_count() const { return m_request_count.load(); }
    size_t client_disconnect_count() const { return m_client_disconnect_count.load(); }
    bool first_chunk_sent() const { return m_first_chunk_sent.load(); }

    void reset_counts()
    {
        m_connection_count.store(0);
        m_request_count.store(0);
        m_client_disconnect_count.store(0);
        m_first_chunk_sent.store(false);
    }

private:
    void run()
    {
        while (!m_stop.load()) {
            pollfd pfd { .fd = m_listen_fd, .events = POLLIN, .revents = 0 };
            if (::poll(&pfd, 1, 25) <= 0)
                continue;

            int client_fd = ::accept(m_listen_fd, nullptr, nullptr);
            if (client_fd < 0)
                continue;

            m_connection_count.fetch_add(1);
            handle_client(client_fd);
            ::close(client_fd);
        }
    }

    void handle_client(int client_fd)
    {
        timeval tv { .tv_sec = 2, .tv_usec = 0 };
        (void)::setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        (void)::setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        ByteBuffer request_bytes;
        char chunk[1024];
        while (request_bytes.size() < 16 * KiB) {
            ssize_t n = ::recv(client_fd, chunk, sizeof(chunk), 0);
            if (n <= 0)
                return;
            request_bytes.append(chunk, static_cast<size_t>(n));
            if (StringView { request_bytes.bytes() }.contains("\r\n\r\n"sv))
                break;
        }

        m_request_count.fetch_add(1);
        static constexpr StringView initial_response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nTransfer-Encoding: chunked\r\nAccess-Control-Allow-Origin: *\r\n\r\n5\r\nhello\r\n"sv;
        (void)::send(client_fd, initial_response.characters_without_null_termination(), initial_response.length(), 0);
        m_first_chunk_sent.store(true);

        // Hold the connection open until the client closes it (or the server stops).
        while (!m_stop.load()) {
            pollfd pfd { .fd = client_fd, .events = POLLIN | POLLHUP | POLLERR, .revents = 0 };
            int ready = ::poll(&pfd, 1, 25);
            if (ready <= 0)
                continue;
            char probe[16];
            ssize_t n = ::recv(client_fd, probe, sizeof(probe), MSG_DONTWAIT);
            if (n <= 0 || (pfd.revents & (POLLHUP | POLLERR))) {
                m_client_disconnect_count.fetch_add(1);
                return;
            }
        }
    }

    int m_listen_fd { -1 };
    u16 m_port { 0 };
    Atomic<bool> m_stop { false };
    Atomic<size_t> m_connection_count { 0 };
    Atomic<size_t> m_request_count { 0 };
    Atomic<size_t> m_client_disconnect_count { 0 };
    Atomic<bool> m_first_chunk_sent { false };
    RefPtr<Threading::Thread> m_thread;
};

// Independent RFC 6455 WebSocket echo observer server on 127.0.0.1 running on its own OS thread.
class IndependentWebSocketServer {
public:
    IndependentWebSocketServer()
    {
        m_listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        VERIFY(m_listen_fd >= 0);

        int yes = 1;
        (void)::setsockopt(m_listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        VERIFY(::bind(m_listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        socklen_t len = sizeof(addr);
        VERIFY(::getsockname(m_listen_fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
        m_port = ntohs(addr.sin_port);

        VERIFY(::listen(m_listen_fd, 8) == 0);

        m_thread = Threading::Thread::construct("PrivacyWSObserver"sv, [this] {
            run();
            return 0;
        });
        m_thread->start();
    }

    ~IndependentWebSocketServer()
    {
        m_stop.store(true);
        if (m_listen_fd >= 0) {
            ::shutdown(m_listen_fd, SHUT_RDWR);
            ::close(m_listen_fd);
            m_listen_fd = -1;
        }
        if (m_thread)
            (void)m_thread->join();
    }

    u16 port() const { return m_port; }
    size_t connection_count() const { return m_connection_count.load(); }
    size_t handshake_count() const { return m_handshake_count.load(); }
    size_t client_disconnect_count() const { return m_client_disconnect_count.load(); }

    void reset_counts()
    {
        m_connection_count.store(0);
        m_handshake_count.store(0);
        m_client_disconnect_count.store(0);
    }

private:
    void run()
    {
        while (!m_stop.load()) {
            pollfd pfd { .fd = m_listen_fd, .events = POLLIN, .revents = 0 };
            if (::poll(&pfd, 1, 25) <= 0)
                continue;

            int client_fd = ::accept(m_listen_fd, nullptr, nullptr);
            if (client_fd < 0)
                continue;

            m_connection_count.fetch_add(1);
            handle_client(client_fd);
            ::close(client_fd);
        }
    }

    static bool read_exact(int fd, u8* buffer, size_t size, Atomic<bool> const& stop)
    {
        size_t total = 0;
        while (total < size && !stop.load()) {
            pollfd pfd { .fd = fd, .events = POLLIN | POLLHUP | POLLERR, .revents = 0 };
            int ready = ::poll(&pfd, 1, 25);
            if (ready < 0)
                return false;
            if (ready == 0)
                continue;
            ssize_t n = ::recv(fd, buffer + total, size - total, 0);
            if (n <= 0)
                return false;
            total += static_cast<size_t>(n);
        }
        return total == size;
    }

    void handle_client(int client_fd)
    {
        ByteBuffer request_bytes;
        char chunk[2048];
        while (!m_stop.load() && request_bytes.size() < 16 * KiB) {
            pollfd pfd { .fd = client_fd, .events = POLLIN, .revents = 0 };
            int ready = ::poll(&pfd, 1, 25);
            if (ready < 0)
                return;
            if (ready == 0)
                continue;
            ssize_t n = ::recv(client_fd, chunk, sizeof(chunk), 0);
            if (n <= 0)
                return;
            request_bytes.append(chunk, static_cast<size_t>(n));
            if (StringView { request_bytes.bytes() }.contains("\r\n\r\n"sv))
                break;
        }

        StringView head { request_bytes.bytes() };
        Optional<StringView> ws_key;
        for (auto line : head.split_view("\r\n"sv)) {
            if (auto colon = line.find(':'); colon.has_value()) {
                auto name = line.substring_view(0, *colon).trim_whitespace();
                if (name.equals_ignoring_ascii_case("Sec-WebSocket-Key"sv)) {
                    ws_key = line.substring_view(*colon + 1).trim_whitespace();
                    break;
                }
            }
        }
        if (!ws_key.has_value())
            return;

        auto accept_input = ByteString::formatted("{}258EAFA5-E914-47DA-95CA-C5AB0DC85B11", *ws_key);
        auto digest = Crypto::Hash::digest(Crypto::Hash::HashKind::SHA1, accept_input.bytes());
        auto accept_header = MUST(encode_base64(digest.bytes()));
        auto handshake_response = ByteString::formatted(
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: {}\r\n\r\n",
            accept_header);

        if (::send(client_fd, handshake_response.characters(), handshake_response.length(), 0) <= 0)
            return;

        m_handshake_count.fetch_add(1);

        // Echo masked text frames back until client disconnects or sends Close.
        while (!m_stop.load()) {
            u8 header[2];
            if (!read_exact(client_fd, header, 2, m_stop)) {
                m_client_disconnect_count.fetch_add(1);
                return;
            }
            u8 opcode = header[0] & 0x0f;
            bool masked = (header[1] & 0x80) != 0;
            u64 payload_len = header[1] & 0x7f;
            if (payload_len == 126) {
                u8 ext[2];
                if (!read_exact(client_fd, ext, 2, m_stop)) {
                    m_client_disconnect_count.fetch_add(1);
                    return;
                }
                payload_len = (static_cast<u64>(ext[0]) << 8) | ext[1];
            } else if (payload_len == 127) {
                m_client_disconnect_count.fetch_add(1);
                return;
            }

            u8 mask[4] {};
            if (masked && !read_exact(client_fd, mask, 4, m_stop)) {
                m_client_disconnect_count.fetch_add(1);
                return;
            }

            auto payload = MUST(ByteBuffer::create_uninitialized(payload_len));
            if (payload_len > 0 && !read_exact(client_fd, payload.data(), payload_len, m_stop)) {
                m_client_disconnect_count.fetch_add(1);
                return;
            }
            if (masked) {
                for (size_t i = 0; i < payload_len; ++i)
                    payload[i] ^= mask[i % 4];
            }

            if (opcode == 0x8) {
                m_client_disconnect_count.fetch_add(1);
                return;
            }
            if (opcode == 0x1 && payload_len < 126) {
                ByteBuffer frame;
                frame.append(static_cast<u8>(0x81));
                frame.append(static_cast<u8>(payload_len));
                frame.append(payload.bytes());
                (void)::send(client_fd, frame.data(), frame.size(), 0);
            }
        }
    }

    int m_listen_fd { -1 };
    u16 m_port { 0 };
    Atomic<bool> m_stop { false };
    Atomic<size_t> m_connection_count { 0 };
    Atomic<size_t> m_handshake_count { 0 };
    Atomic<size_t> m_client_disconnect_count { 0 };
    RefPtr<Threading::Thread> m_thread;
};

// Mock network endpoint for mandatory-route tests, running its own OS threads (one per connection, so keep-alive
// connections stay open while others are served). In DirectHTTP mode it is an HTTP/1.1 keep-alive origin server. In
// SOCKS5 mode it is a SOCKS5 proxy that records each stream's authentication and requested destination, and then
// answers the tunnelled HTTP requests itself, so no destination is ever contacted.
class MockRouteServer {
public:
    enum class Mode {
        DirectHTTP,
        SOCKS5,
    };

    struct SOCKSStream {
        ByteString username;
        ByteString password;
        // 1 = IPv4, 3 = domain name, 4 = IPv6.
        u8 address_type { 0 };
        ByteString host;
        u16 port { 0 };
    };

    explicit MockRouteServer(Mode mode)
        : m_mode(mode)
    {
        pthread_mutex_init(&m_mutex, nullptr);

        m_listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        VERIFY(m_listen_fd >= 0);
        int yes = 1;
        (void)::setsockopt(m_listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        VERIFY(::bind(m_listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        socklen_t len = sizeof(addr);
        VERIFY(::getsockname(m_listen_fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
        m_port = ntohs(addr.sin_port);
        VERIFY(::listen(m_listen_fd, 32) == 0);

        m_thread = Threading::Thread::construct("PrivacyRouteObserver"sv, [this] {
            accept_loop();
            return 0;
        });
        m_thread->start();
    }

    ~MockRouteServer()
    {
        stop();
        pthread_mutex_destroy(&m_mutex);
    }

    // Closes the listening socket and every open connection, as a crashed proxy would.
    void stop()
    {
        if (m_stop.exchange(true))
            return;
        if (m_listen_fd >= 0) {
            ::shutdown(m_listen_fd, SHUT_RDWR);
            ::close(m_listen_fd);
            m_listen_fd = -1;
        }
        if (m_thread)
            (void)m_thread->join();
        Vector<NonnullRefPtr<Threading::Thread>> threads;
        {
            Locker locker { *this };
            for (int fd : m_client_fds)
                ::shutdown(fd, SHUT_RDWR);
            threads = move(m_client_threads);
        }
        for (auto& thread : threads)
            (void)thread->join();
    }

    u16 port() const { return m_port; }
    size_t connection_count() const { return m_connection_count.load(); }
    size_t request_count() const { return m_request_count.load(); }
    // Connections that the client closed.
    size_t client_closed_count() const { return m_client_closed_count.load(); }

    Vector<SOCKSStream> streams()
    {
        Locker locker { *this };
        return m_streams;
    }

    // The request target of every HTTP request served, as the client sent it ("/path?query").
    Vector<ByteString> request_targets()
    {
        Locker locker { *this };
        return m_request_targets;
    }

    // Serves this body as text/html for the given path, instead of the plain-text default.
    void set_html_for_path(ByteString path, ByteString html)
    {
        Locker locker { *this };
        m_html_by_path.set(move(path), move(html));
    }

    void reset_counts()
    {
        Locker locker { *this };
        m_connection_count.store(0);
        m_request_count.store(0);
        m_client_closed_count.store(0);
        m_streams.clear();
        m_request_targets.clear();
    }

private:
    struct Locker {
        explicit Locker(MockRouteServer& server)
            : server(server)
        {
            pthread_mutex_lock(&server.m_mutex);
        }
        ~Locker() { pthread_mutex_unlock(&server.m_mutex); }
        MockRouteServer& server;
    };

    void accept_loop()
    {
        while (!m_stop.load()) {
            pollfd pfd { .fd = m_listen_fd, .events = POLLIN, .revents = 0 };
            if (::poll(&pfd, 1, 25) <= 0)
                continue;
            int client_fd = ::accept(m_listen_fd, nullptr, nullptr);
            if (client_fd < 0)
                continue;
            m_connection_count.fetch_add(1);

            Locker locker { *this };
            m_client_fds.append(client_fd);
            auto thread = Threading::Thread::construct("PrivacyRouteConnection"sv, [this, client_fd] {
                serve(client_fd);
                {
                    Locker locker { *this };
                    m_client_fds.remove_first_matching([&](int fd) { return fd == client_fd; });
                }
                ::close(client_fd);
                return 0;
            });
            thread->start();
            m_client_threads.append(move(thread));
        }
    }

    bool read_exact(int fd, u8* buffer, size_t size)
    {
        size_t total = 0;
        while (total < size && !m_stop.load()) {
            pollfd pfd { .fd = fd, .events = POLLIN, .revents = 0 };
            int ready = ::poll(&pfd, 1, 25);
            if (ready < 0)
                return false;
            if (ready == 0)
                continue;
            ssize_t n = ::recv(fd, buffer + total, size - total, 0);
            if (n <= 0) {
                if (n == 0)
                    m_client_closed_count.fetch_add(1);
                return false;
            }
            total += static_cast<size_t>(n);
        }
        return total == size;
    }

    static bool send_all(int fd, ReadonlyBytes bytes)
    {
        size_t sent = 0;
        while (sent < bytes.size()) {
            ssize_t n = ::send(fd, bytes.data() + sent, bytes.size() - sent, 0);
            if (n <= 0)
                return false;
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    bool socks5_handshake(int fd, ByteString& destination_host)
    {
        u8 greeting[2];
        if (!read_exact(fd, greeting, 2) || greeting[0] != 5)
            return false;
        u8 methods[255];
        if (!read_exact(fd, methods, greeting[1]))
            return false;
        bool offers_password = false;
        bool offers_none = false;
        for (size_t i = 0; i < greeting[1]; ++i) {
            offers_password |= methods[i] == 2;
            offers_none |= methods[i] == 0;
        }

        SOCKSStream stream;
        if (offers_password) {
            u8 const choice[] = { 5, 2 };
            if (!send_all(fd, { choice, 2 }))
                return false;
            u8 header[2];
            if (!read_exact(fd, header, 2) || header[0] != 1)
                return false;
            u8 field[255];
            if (!read_exact(fd, field, header[1]))
                return false;
            stream.username = ByteString { ReadonlyBytes { field, header[1] } };
            u8 password_length = 0;
            if (!read_exact(fd, &password_length, 1) || !read_exact(fd, field, password_length))
                return false;
            stream.password = ByteString { ReadonlyBytes { field, password_length } };
            u8 const accepted[] = { 1, 0 };
            if (!send_all(fd, { accepted, 2 }))
                return false;
        } else if (offers_none) {
            u8 const choice[] = { 5, 0 };
            if (!send_all(fd, { choice, 2 }))
                return false;
        } else {
            return false;
        }

        u8 request[4];
        if (!read_exact(fd, request, 4) || request[0] != 5 || request[1] != 1)
            return false;
        stream.address_type = request[3];
        u8 address[255];
        if (request[3] == 1) {
            if (!read_exact(fd, address, 4))
                return false;
            stream.host = ByteString::formatted("{}.{}.{}.{}", address[0], address[1], address[2], address[3]);
        } else if (request[3] == 3) {
            u8 length = 0;
            if (!read_exact(fd, &length, 1) || !read_exact(fd, address, length))
                return false;
            stream.host = ByteString { ReadonlyBytes { address, length } };
        } else if (request[3] == 4) {
            if (!read_exact(fd, address, 16))
                return false;
            stream.host = "ipv6"sv;
        } else {
            return false;
        }
        u8 port[2];
        if (!read_exact(fd, port, 2))
            return false;
        stream.port = static_cast<u16>((port[0] << 8) | port[1]);

        destination_host = stream.host;
        {
            Locker locker { *this };
            m_streams.append(move(stream));
        }
        u8 const succeeded[] = { 5, 0, 0, 1, 0, 0, 0, 0, 0, 0 };
        return send_all(fd, { succeeded, sizeof(succeeded) });
    }

    void serve(int fd)
    {
        ByteString destination_host = "direct"sv;
        if (m_mode == Mode::SOCKS5 && !socks5_handshake(fd, destination_host))
            return;

        // Serve keep-alive HTTP/1.1 requests until the client closes the connection.
        ByteBuffer pending;
        while (!m_stop.load()) {
            auto head_end = StringView { pending.bytes() }.find("\r\n\r\n"sv);
            if (!head_end.has_value()) {
                u8 byte_chunk[4096];
                pollfd pfd { .fd = fd, .events = POLLIN, .revents = 0 };
                int ready = ::poll(&pfd, 1, 25);
                if (ready < 0)
                    return;
                if (ready == 0)
                    continue;
                ssize_t n = ::recv(fd, byte_chunk, sizeof(byte_chunk), 0);
                if (n <= 0) {
                    if (n == 0)
                        m_client_closed_count.fetch_add(1);
                    return;
                }
                pending.append(byte_chunk, static_cast<size_t>(n));
                continue;
            }

            auto head = ByteString { pending.bytes().slice(0, *head_end) };
            pending = MUST(ByteBuffer::copy(pending.bytes().slice(*head_end + 4)));
            m_request_count.fetch_add(1);

            auto request_line = head.view().find_first_split_view('\r');
            auto parts = request_line.split_view(' ');
            auto target = parts.size() >= 2 ? parts[1] : "/"sv;
            Optional<ByteString> html;
            {
                Locker locker { *this };
                m_request_targets.append(target);
                auto path = target.find_first_split_view('?');
                if (auto page = m_html_by_path.get(path); page.has_value())
                    html = *page;
            }

            ByteString response;
            if (auto redirect = target.find("/redirect?to="sv); redirect.has_value()) {
                auto location = target.substring_view(*redirect + "/redirect?to="sv.length());
                response = ByteString::formatted("HTTP/1.1 302 Found\r\nLocation: {}\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 0\r\n\r\n", location);
            } else if (html.has_value()) {
                response = ByteString::formatted("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nCache-Control: no-store\r\nContent-Length: {}\r\n\r\n{}", html->length(), *html);
            } else {
                auto body = ByteString::formatted("via-{}:{}", m_mode == Mode::SOCKS5 ? "socks"sv : "direct"sv, destination_host);
                response = ByteString::formatted("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-store\r\nContent-Length: {}\r\n\r\n{}", body.length(), body);
            }
            if (!send_all(fd, response.bytes()))
                return;
        }
    }

    Mode m_mode;
    int m_listen_fd { -1 };
    u16 m_port { 0 };
    Atomic<bool> m_stop { false };
    Atomic<size_t> m_connection_count { 0 };
    Atomic<size_t> m_request_count { 0 };
    Atomic<size_t> m_client_closed_count { 0 };
    pthread_mutex_t m_mutex;
    Vector<int> m_client_fds;
    Vector<NonnullRefPtr<Threading::Thread>> m_client_threads;
    Vector<SOCKSStream> m_streams;
    Vector<ByteString> m_request_targets;
    HashMap<ByteString, ByteString> m_html_by_path;
    RefPtr<Threading::Thread> m_thread;
};

}
