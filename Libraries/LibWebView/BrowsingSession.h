/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/OwnPtr.h>
#include <AK/RefCounted.h>
#include <AK/Time.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <AK/Weakable.h>
#include <LibPrivacy/Identity/Types.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

enum class IsPrivate : u8 {
    No,
    Yes,
};

// The ordinary session lives as long as the application. Named identities own isolated
// sessions managed by IdentityManager, and private/ephemeral sessions are owned by their
// views and content processes and held weakly elsewhere so nothing outliving them can bring one back.
class WEBVIEW_API BrowsingSession
    : public RefCounted<BrowsingSession>
    , public Weakable<BrowsingSession> {
public:
    static NonnullRefPtr<BrowsingSession> create(IsPrivate);
    static NonnullRefPtr<BrowsingSession> create_for_identity(Privacy::IdentityDescriptor);
    ~BrowsingSession();

    IsPrivate is_private() const { return m_is_private; }
    Privacy::IdentityDescriptor const& identity() const { return m_identity; }
    String const& identity_id() const { return m_identity.id; }
    bool is_ephemeral() const { return m_is_private == IsPrivate::Yes || m_identity.is_ephemeral(); }
    bool persists_history() const { return m_is_private == IsPrivate::No && m_identity.persist_history; }

    // No two sessions of the application share an ID.
    u64 id() const { return m_id; }

    // NB: Null for the ordinary session until Application fills these in during startup.
    OwnPtr<CookieJar> cookie_jar;
    OwnPtr<StorageJar> storage_jar;
    OwnPtr<BlobURLStore> blob_url_store;
    OwnPtr<HSTSStore> hsts_store;
    OwnPtr<PermissionManager> permission_manager;
    // Non-null only for the ordinary browsing session; private sessions never host extensions.
    OwnPtr<Extensions::ExtensionManager> extension_manager;

    struct CertificateException {
        ByteString host;
        u16 port { 0 };
        ByteString fingerprint;

        bool operator==(CertificateException const&) const = default;
    };

    // Keep recent rejections until their warnings arrive, so users can only trust certificates actually presented.
    struct RejectedCertificate {
        CertificateException certificate;
        MonotonicTime rejected_at;
    };
    HashMap<ByteString, Vector<RejectedCertificate>> rejected_self_signed_certificates;

    // Every RequestServer of the session accepts these, including those launched after the user trusted them.
    Vector<CertificateException> trusted_certificates;

private:
    explicit BrowsingSession(IsPrivate, Privacy::IdentityDescriptor);

    u64 m_id { 0 };
    IsPrivate m_is_private { IsPrivate::No };
    Privacy::IdentityDescriptor m_identity;
};

}
