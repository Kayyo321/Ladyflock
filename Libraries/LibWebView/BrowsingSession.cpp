/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/BlobURLStore.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/CookieJar.h>
#include <LibWebView/Extensions/ExtensionManager.h>
#include <LibWebView/HSTSStore.h>
#include <LibWebView/PermissionManager.h>
#include <LibWebView/StorageJar.h>

namespace WebView {

static u64 s_next_id { 1 };

BrowsingSession::BrowsingSession(IsPrivate is_private, Privacy::IdentityDescriptor identity)
    : m_id(s_next_id++)
    , m_is_private(is_private)
    , m_identity(move(identity))
{
}

BrowsingSession::~BrowsingSession() = default;

NonnullRefPtr<BrowsingSession> BrowsingSession::create(IsPrivate is_private)
{
    auto identity = is_private == IsPrivate::Yes
        ? Privacy::IdentityDescriptor::private_ephemeral()
        : Privacy::IdentityDescriptor::default_persistent();
    return create_for_identity(move(identity));
}

NonnullRefPtr<BrowsingSession> BrowsingSession::create_for_identity(Privacy::IdentityDescriptor identity)
{
    auto is_private = identity.is_ephemeral() ? IsPrivate::Yes : IsPrivate::No;
    auto session = adopt_ref(*new BrowsingSession(is_private, move(identity)));

    if (session->is_ephemeral()) {
        session->cookie_jar = CookieJar::create(IsPrivate::Yes);
        session->storage_jar = StorageJar::create();
        session->blob_url_store = make<BlobURLStore>();
        session->hsts_store = HSTSStore::create();
    }

    return session;
}

}
