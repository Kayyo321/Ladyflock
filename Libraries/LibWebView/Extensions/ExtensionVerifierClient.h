/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <ExtensionVerifier/ExtensionVerifierClientEndpoint.h>
#include <ExtensionVerifier/ExtensionVerifierServerEndpoint.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibWebView/ChildIdentity.h>
#include <LibWebView/Export.h>

namespace WebView {

class WEBVIEW_API ExtensionVerifierClient final
    : public IPC::ConnectionToServer<ExtensionVerifierClientEndpoint, ExtensionVerifierServerEndpoint>
    , public ExtensionVerifierClientEndpoint {
    C_OBJECT_ABSTRACT(ExtensionVerifierClient);

public:
    explicit ExtensionVerifierClient(NonnullOwnPtr<IPC::Transport>);
    virtual ~ExtensionVerifierClient() override;

    u64 child_id() const { return m_child_identity.child_id; }
    void set_child_identity(ChildIdentity identity) { m_child_identity = identity; }

private:
    virtual void die() override;

    ChildIdentity m_child_identity;
};

}
