/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWebCommon/Extensions/ExtensionManifest.h>
#include <LibWebCommon/Extensions/ExtensionTypes.h>
#include <LibWebView/Export.h>
#include <LibWebView/Extensions/ExtensionVerifierClient.h>
#include <LibWebView/Extensions/RevocationList.h>

namespace WebView::Extensions {

// Orders two extension versions as Firefox does: negative, zero or positive as the first is older, the same or newer.
WEBVIEW_API int compare_extension_versions(StringView, StringView);

struct WEBVIEW_API InstallDiagnostic {
    String verification_error;
    Vector<String> compatibility_reasons;
};

struct WEBVIEW_API StagedInstall {
    String token;
    String addon_id;
    String name;
    String version;
    String sha256_hex;
    String manifest_json;
    ByteString staged_directory;
    Optional<String> icon_path;
    Web::Extensions::ExtensionPermissionSummary permissions;
    Web::Extensions::CompatibilityStatus compatibility_status { Web::Extensions::CompatibilityStatus::ManifestCompatibleUntested };
    Vector<String> compatibility_reasons;
    bool verified_signature { false };
    String signer_cn;
    bool is_update { false };
    bool requires_permission_prompt { true };
    Vector<String> newly_requested_permissions;
    Optional<String> previous_sha256_hex;
    bool from_catalog { false };
};

struct WEBVIEW_API CommitResult {
    String addon_id;
    String version;
    ByteString install_directory;
    String sha256_hex;
    String manifest_json;
    Web::Extensions::CompatibilityStatus compatibility_status { Web::Extensions::CompatibilityStatus::ManifestCompatibleUntested };
};

// Orchestrates sandboxed XPI verification, staging under <profile>/extensions/staging/,
// permission/update consent checks, atomic commit, cancellation cleanup, and rollback.
class WEBVIEW_API ExtensionInstaller {
    AK_MAKE_NONCOPYABLE(ExtensionInstaller);

public:
    static constexpr size_t max_package_size = 64 * MiB;

    ExtensionInstaller(ByteString extensions_root, RevocationList const&);
    ~ExtensionInstaller();

    void cleanup_stale_staging();
    void disable_verification() { m_verification_disabled = true; }

    void add_trusted_root_pem(String pem) { m_trusted_root_pems.append(move(pem)); }
    void set_fail_next_commit_for_testing(bool fail) { m_fail_next_commit_for_testing = fail; }

    ErrorOr<StagedInstall> stage_xpi(
        ByteString const& xpi_path,
        Optional<String> const& expected_addon_id = {},
        Optional<Web::Extensions::ExtensionManifest> const& current_manifest_for_update = {},
        Optional<String> const& previous_sha256_hex = {},
        InstallDiagnostic* = nullptr);
    ErrorOr<StagedInstall> stage_package(
        ByteBuffer,
        Optional<String> const& expected_addon_id = {},
        Optional<String> const& expected_version = {},
        Optional<String> const& expected_sha256_hex = {},
        Optional<Web::Extensions::ExtensionManifest> const& current_manifest_for_update = {},
        Optional<String> const& previous_sha256_hex = {},
        InstallDiagnostic* = nullptr);

    ErrorOr<void> cancel_install(StringView token);
    void remove_installed_package(ByteString const& payload_directory) const;
    ErrorOr<Web::Extensions::ExtensionManifest> validate_installed_package(ByteString const& payload_directory, String const& addon_id, String const& sha256_hex);

    // Commits staged install/update atomically via temporary backup + rename so failure
    // rolls back to the previous working version cleanly.
    ErrorOr<CommitResult> commit_staged(
        StringView token,
        bool user_consented,
        Optional<ByteString> const& previous_install_directory,
        Function<ErrorOr<void>(CommitResult const&)> const& on_commit_registry);

    Optional<StagedInstall const&> staged_install(StringView token) const;

private:
    struct VerifiedPackage {
        Web::Extensions::XpiVerificationResult metadata;
        Vector<String> file_names;
        Vector<ByteBuffer> file_contents;
    };

    ErrorOr<NonnullRefPtr<ExtensionVerifierClient>> ensure_verifier_client();
    ErrorOr<VerifiedPackage> verify_package(ByteBuffer, Optional<String> const& expected_addon_id, InstallDiagnostic* = nullptr);

    ByteString staging_root() const;
    ByteString installed_root() const;

    ByteString m_extensions_root;
    RevocationList const& m_revocation_list;
    RefPtr<ExtensionVerifierClient> m_verifier_client;
    Vector<String> m_trusted_root_pems;
    HashMap<String, StagedInstall> m_staged;
    bool m_fail_next_commit_for_testing { false };
    bool m_verification_disabled { false };
};

}
