/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibIPC/Forward.h>
#include <LibURL/URL.h>
#include <LibWebCommon/Export.h>
#include <LibWebCommon/HTML/CrossProcessId.h>

namespace Web::Extensions {

enum class ManifestVersion : u8 {
    V2 = 2,
    V3 = 3,
};

enum class RunAt : u8 {
    DocumentStart,
    DocumentEnd,
    DocumentIdle,
};

enum class InstallSource : u8 {
    UnpackedDev,
    SignedXpi,
};

enum class CompatibilityStatus : u8 {
    TestedInLadyflock,
    ManifestCompatibleUntested,
    Incompatible,
};

enum class ContextType : u8 {
    ContentScript,
    BackgroundPage,
    ExtensionPage,
    Popup,
    Sidebar,
    OptionsPage,
};

enum class WebRequestResourceType : u8 {
    MainFrame,
    SubFrame,
    Stylesheet,
    Script,
    Image,
    Font,
    Object,
    XmlHttpRequest,
    Media,
    Other,
};

struct WEBCOMMON_API NamedSource {
    String path;
    String content;
};

struct WEBCOMMON_API AttachedContentScript {
    String extension_id;
    String extension_uuid;
    u64 grant_generation { 0 };
    Web::HTML::CrossProcessId document_id;
    RunAt run_at { RunAt::DocumentIdle };
    Vector<NamedSource> js_files;
    Vector<NamedSource> css_files;
    bool all_frames { false };
    bool match_about_blank { false };
};

struct WEBCOMMON_API ExtensionResourceResponse {
    bool allowed { false };
    ByteBuffer data;
    String mime_type;
    Optional<String> content_security_policy;
    Optional<String> error;
};

struct WEBCOMMON_API ExtensionActionState {
    String extension_id;
    String title;
    String badge_text;
    String badge_background_color;
    Optional<String> popup_url;
    Optional<String> icon_path;
    bool use_manifest_icon { true };
    bool enabled { true };
    bool pinned { false };
};

struct WEBCOMMON_API SidebarActionState {
    String extension_id;
    String title;
    Optional<String> panel_url;
    Optional<String> icon_path;
    bool open_at_install { true };
    bool is_open { false };
};

struct WEBCOMMON_API ExtensionTabInfo {
    u64 tab_id { 0 };
    u64 window_id { 0 };
    bool active { false };
    bool pinned { false };
    String status;
    Optional<String> url;
    Optional<String> title;
    Optional<String> fav_icon_url;
};

struct WEBCOMMON_API ExtensionDataCollectionPermissions {
    Vector<String> required;
    Vector<String> optional;
};

struct WEBCOMMON_API ExtensionPermissionSummary {
    Vector<String> permissions;
    Vector<String> host_permissions;
    Vector<String> optional_permissions;
    Vector<String> optional_host_permissions;
    ExtensionDataCollectionPermissions data_collection;
};

struct WEBCOMMON_API WebRequestWatchEntry {
    String extension_id;
    u64 listener_id { 0 };
    u64 grant_generation { 0 };
    Vector<String> url_patterns;
    Vector<WebRequestResourceType> types;
    bool blocking { false };
};

struct WEBCOMMON_API WebRequestWatchSnapshot {
    u64 revision { 0 };
    Vector<WebRequestWatchEntry> entries;
    bool has_declarative_rules { false };

    bool matches(URL::URL const& url, WebRequestResourceType type) const;
};

struct WEBCOMMON_API WebRequestBeforeRequestDetails {
    u64 request_id { 0 };
    URL::URL url;
    String method;
    WebRequestResourceType type { WebRequestResourceType::Other };
    u64 tab_id { 0 };
    u64 frame_id { 0 };
    u64 parent_frame_id { 0 };
    Optional<String> initiator_origin;
    Optional<String> document_url;
    bool is_navigation { false };
    Optional<HTML::CrossProcessId> source_document_id {};
};

struct WEBCOMMON_API WebRequestDecision {
    bool cancel { false };
};

struct WEBCOMMON_API XpiVerificationResult {
    bool ok { false };
    String error;
    String addon_id;
    String version;
    String sha256_hex;
    String manifest_json;
    CompatibilityStatus compatibility_status { CompatibilityStatus::ManifestCompatibleUntested };
    Vector<String> compatibility_reasons;
    bool verified_signature { false };
    String signer_cn;
};

WEBCOMMON_API Optional<RunAt> run_at_from_string(StringView);
WEBCOMMON_API StringView to_string(RunAt);
WEBCOMMON_API Optional<WebRequestResourceType> web_request_resource_type_from_string(StringView);
WEBCOMMON_API StringView to_string(WebRequestResourceType);
WEBCOMMON_API StringView to_string(CompatibilityStatus);
WEBCOMMON_API StringView to_string(ContextType);

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::Extensions::NamedSource const&);
template<>
WEBCOMMON_API ErrorOr<Web::Extensions::NamedSource> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::Extensions::AttachedContentScript const&);
template<>
WEBCOMMON_API ErrorOr<Web::Extensions::AttachedContentScript> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::Extensions::ExtensionResourceResponse const&);
template<>
WEBCOMMON_API ErrorOr<Web::Extensions::ExtensionResourceResponse> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::Extensions::WebRequestWatchEntry const&);
template<>
WEBCOMMON_API ErrorOr<Web::Extensions::WebRequestWatchEntry> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::Extensions::WebRequestWatchSnapshot const&);
template<>
WEBCOMMON_API ErrorOr<Web::Extensions::WebRequestWatchSnapshot> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::Extensions::WebRequestBeforeRequestDetails const&);
template<>
WEBCOMMON_API ErrorOr<Web::Extensions::WebRequestBeforeRequestDetails> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::Extensions::WebRequestDecision const&);
template<>
WEBCOMMON_API ErrorOr<Web::Extensions::WebRequestDecision> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::Extensions::XpiVerificationResult const&);
template<>
WEBCOMMON_API ErrorOr<Web::Extensions::XpiVerificationResult> decode(Decoder&);

}
