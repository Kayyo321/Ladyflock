/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <ExtensionVerifier/ConnectionFromClient.h>
#include <ExtensionVerifier/XpiVerifier.h>
#include <LibCore/EventLoop.h>

namespace ExtensionVerifier {

ConnectionFromClient::ConnectionFromClient(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionFromClient<ExtensionVerifierClientEndpoint, ExtensionVerifierServerEndpoint>(*this, move(transport), 0)
{
}

ConnectionFromClient::~ConnectionFromClient() = default;

void ConnectionFromClient::die()
{
    Core::EventLoop::current().quit(0);
}

Messages::ExtensionVerifierServer::VerifyXpiResponse ConnectionFromClient::verify_xpi(
    ByteBuffer xpi_bytes,
    Optional<String> expected_addon_id,
    Vector<String> trusted_root_pems,
    Vector<String> revoked_addon_ids,
    Vector<String> revoked_sha256_hexes)
{
    Vector<String> file_names;
    Vector<ByteBuffer> file_contents;
    auto result = verify_xpi_bytes(
        xpi_bytes.bytes(),
        expected_addon_id,
        trusted_root_pems,
        revoked_addon_ids,
        revoked_sha256_hexes,
        file_names,
        file_contents);
    if (!result.ok)
        return { move(result), {}, {} };
    return { move(result), move(file_names), move(file_contents) };
}

}
