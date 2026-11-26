/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <ExtensionVerifier/ExtensionVerifierClientEndpoint.h>
#include <ExtensionVerifier/ExtensionVerifierServerEndpoint.h>
#include <LibIPC/ConnectionFromClient.h>

namespace ExtensionVerifier {

class ConnectionFromClient final
    : public IPC::ConnectionFromClient<ExtensionVerifierClientEndpoint, ExtensionVerifierServerEndpoint> {
    C_OBJECT(ConnectionFromClient);

public:
    virtual ~ConnectionFromClient() override;

    virtual void die() override;

private:
    explicit ConnectionFromClient(NonnullOwnPtr<IPC::Transport>);

    virtual Messages::ExtensionVerifierServer::VerifyXpiResponse verify_xpi(
        ByteBuffer xpi_bytes,
        Optional<String> expected_addon_id,
        Vector<String> trusted_root_pems,
        Vector<String> revoked_addon_ids,
        Vector<String> revoked_sha256_hexes) override;
};

}
