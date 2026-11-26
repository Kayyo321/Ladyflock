/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/Extensions/ExtensionVerifierClient.h>

namespace WebView {

ExtensionVerifierClient::ExtensionVerifierClient(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<ExtensionVerifierClientEndpoint, ExtensionVerifierServerEndpoint>(*this, move(transport))
{
}

ExtensionVerifierClient::~ExtensionVerifierClient() = default;

void ExtensionVerifierClient::die()
{
}

}
