/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebCommon/Extensions/ExtensionTypes.h>
#include <LibWebCommon/Extensions/MatchPattern.h>

namespace Web::Extensions {

Optional<RunAt> run_at_from_string(StringView value)
{
    if (value == "document_start"sv)
        return RunAt::DocumentStart;
    if (value == "document_end"sv)
        return RunAt::DocumentEnd;
    if (value == "document_idle"sv)
        return RunAt::DocumentIdle;
    return {};
}

StringView to_string(RunAt run_at)
{
    switch (run_at) {
    case RunAt::DocumentStart:
        return "document_start"sv;
    case RunAt::DocumentEnd:
        return "document_end"sv;
    case RunAt::DocumentIdle:
        return "document_idle"sv;
    }
    VERIFY_NOT_REACHED();
}

Optional<WebRequestResourceType> web_request_resource_type_from_string(StringView value)
{
    if (value == "main_frame"sv)
        return WebRequestResourceType::MainFrame;
    if (value == "sub_frame"sv)
        return WebRequestResourceType::SubFrame;
    if (value == "stylesheet"sv)
        return WebRequestResourceType::Stylesheet;
    if (value == "script"sv)
        return WebRequestResourceType::Script;
    if (value == "image"sv)
        return WebRequestResourceType::Image;
    if (value == "font"sv)
        return WebRequestResourceType::Font;
    if (value == "object"sv)
        return WebRequestResourceType::Object;
    if (value == "xmlhttprequest"sv)
        return WebRequestResourceType::XmlHttpRequest;
    if (value == "media"sv)
        return WebRequestResourceType::Media;
    if (value == "other"sv)
        return WebRequestResourceType::Other;
    return {};
}

StringView to_string(WebRequestResourceType type)
{
    switch (type) {
    case WebRequestResourceType::MainFrame:
        return "main_frame"sv;
    case WebRequestResourceType::SubFrame:
        return "sub_frame"sv;
    case WebRequestResourceType::Stylesheet:
        return "stylesheet"sv;
    case WebRequestResourceType::Script:
        return "script"sv;
    case WebRequestResourceType::Image:
        return "image"sv;
    case WebRequestResourceType::Font:
        return "font"sv;
    case WebRequestResourceType::Object:
        return "object"sv;
    case WebRequestResourceType::XmlHttpRequest:
        return "xmlhttprequest"sv;
    case WebRequestResourceType::Media:
        return "media"sv;
    case WebRequestResourceType::Other:
        return "other"sv;
    }
    VERIFY_NOT_REACHED();
}

StringView to_string(CompatibilityStatus status)
{
    switch (status) {
    case CompatibilityStatus::TestedInLadyflock:
        return "Tested in Ladyflock"sv;
    case CompatibilityStatus::ManifestCompatibleUntested:
        return "Manifest compatible · Untested"sv;
    case CompatibilityStatus::Incompatible:
        return "Incompatible"sv;
    }
    VERIFY_NOT_REACHED();
}

StringView to_string(ContextType type)
{
    switch (type) {
    case ContextType::ContentScript:
        return "content_script"sv;
    case ContextType::BackgroundPage:
        return "background"sv;
    case ContextType::ExtensionPage:
        return "extension_page"sv;
    case ContextType::Popup:
        return "popup"sv;
    case ContextType::Sidebar:
        return "sidebar"sv;
    case ContextType::OptionsPage:
        return "options_page"sv;
    }
    VERIFY_NOT_REACHED();
}

bool WebRequestWatchSnapshot::matches(URL::URL const& url, WebRequestResourceType type) const
{
    if (url.scheme() != "http"sv && url.scheme() != "https"sv)
        return false;

    if (has_declarative_rules)
        return true;

    for (auto const& entry : entries) {
        if (!entry.types.is_empty() && !entry.types.contains_slow(type))
            continue;
        for (auto const& raw_pattern : entry.url_patterns) {
            auto parsed = MatchPattern::parse(raw_pattern);
            if (!parsed.is_error() && parsed.value().matches(url))
                return true;
        }
    }
    return false;
}

}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::Extensions::NamedSource const& source)
{
    TRY(encoder.encode(source.path));
    TRY(encoder.encode(source.content));
    return {};
}

template<>
ErrorOr<Web::Extensions::NamedSource> IPC::decode(Decoder& decoder)
{
    auto path = TRY(decoder.decode<String>());
    auto content = TRY(decoder.decode<String>());
    return Web::Extensions::NamedSource { move(path), move(content) };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::Extensions::AttachedContentScript const& script)
{
    TRY(encoder.encode(script.extension_id));
    TRY(encoder.encode(script.extension_uuid));
    TRY(encoder.encode(script.grant_generation));
    TRY(encoder.encode(script.document_id));
    TRY(encoder.encode(script.run_at));
    TRY(encoder.encode(script.js_files));
    TRY(encoder.encode(script.css_files));
    TRY(encoder.encode(script.all_frames));
    TRY(encoder.encode(script.match_about_blank));
    return {};
}

template<>
ErrorOr<Web::Extensions::AttachedContentScript> IPC::decode(Decoder& decoder)
{
    auto extension_id = TRY(decoder.decode<String>());
    auto extension_uuid = TRY(decoder.decode<String>());
    auto grant_generation = TRY(decoder.decode<u64>());
    auto document_id = TRY(decoder.decode<Web::HTML::CrossProcessId>());
    auto run_at = TRY(decoder.decode<Web::Extensions::RunAt>());
    auto js_files = TRY(decoder.decode<Vector<Web::Extensions::NamedSource>>());
    auto css_files = TRY(decoder.decode<Vector<Web::Extensions::NamedSource>>());
    auto all_frames = TRY(decoder.decode<bool>());
    auto match_about_blank = TRY(decoder.decode<bool>());
    return Web::Extensions::AttachedContentScript {
        move(extension_id),
        move(extension_uuid),
        grant_generation,
        document_id,
        run_at,
        move(js_files),
        move(css_files),
        all_frames,
        match_about_blank,
    };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::Extensions::ExtensionResourceResponse const& response)
{
    TRY(encoder.encode(response.allowed));
    TRY(encoder.encode(response.data));
    TRY(encoder.encode(response.mime_type));
    TRY(encoder.encode(response.content_security_policy));
    TRY(encoder.encode(response.error));
    return {};
}

template<>
ErrorOr<Web::Extensions::ExtensionResourceResponse> IPC::decode(Decoder& decoder)
{
    auto allowed = TRY(decoder.decode<bool>());
    auto data = TRY(decoder.decode<ByteBuffer>());
    auto mime_type = TRY(decoder.decode<String>());
    auto content_security_policy = TRY(decoder.decode<Optional<String>>());
    auto error = TRY(decoder.decode<Optional<String>>());
    return Web::Extensions::ExtensionResourceResponse {
        allowed,
        move(data),
        move(mime_type),
        move(content_security_policy),
        move(error),
    };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::Extensions::WebRequestWatchEntry const& entry)
{
    TRY(encoder.encode(entry.extension_id));
    TRY(encoder.encode(entry.listener_id));
    TRY(encoder.encode(entry.grant_generation));
    TRY(encoder.encode(entry.url_patterns));
    TRY(encoder.encode(entry.types));
    TRY(encoder.encode(entry.blocking));
    return {};
}

template<>
ErrorOr<Web::Extensions::WebRequestWatchEntry> IPC::decode(Decoder& decoder)
{
    auto extension_id = TRY(decoder.decode<String>());
    auto listener_id = TRY(decoder.decode<u64>());
    auto grant_generation = TRY(decoder.decode<u64>());
    auto url_patterns = TRY(decoder.decode<Vector<String>>());
    auto types = TRY(decoder.decode<Vector<Web::Extensions::WebRequestResourceType>>());
    auto blocking = TRY(decoder.decode<bool>());
    return Web::Extensions::WebRequestWatchEntry {
        move(extension_id),
        listener_id,
        grant_generation,
        move(url_patterns),
        move(types),
        blocking,
    };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::Extensions::WebRequestWatchSnapshot const& snapshot)
{
    TRY(encoder.encode(snapshot.revision));
    TRY(encoder.encode(snapshot.entries));
    return encoder.encode(snapshot.has_declarative_rules);
}

template<>
ErrorOr<Web::Extensions::WebRequestWatchSnapshot> IPC::decode(Decoder& decoder)
{
    auto revision = TRY(decoder.decode<u64>());
    auto entries = TRY(decoder.decode<Vector<Web::Extensions::WebRequestWatchEntry>>());
    auto has_declarative_rules = TRY(decoder.decode<bool>());
    return Web::Extensions::WebRequestWatchSnapshot { revision, move(entries), has_declarative_rules };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::Extensions::WebRequestBeforeRequestDetails const& details)
{
    TRY(encoder.encode(details.request_id));
    TRY(encoder.encode(details.url));
    TRY(encoder.encode(details.method));
    TRY(encoder.encode(details.type));
    TRY(encoder.encode(details.tab_id));
    TRY(encoder.encode(details.frame_id));
    TRY(encoder.encode(details.parent_frame_id));
    TRY(encoder.encode(details.initiator_origin));
    TRY(encoder.encode(details.document_url));
    TRY(encoder.encode(details.is_navigation));
    TRY(encoder.encode(details.source_document_id));
    return {};
}

template<>
ErrorOr<Web::Extensions::WebRequestBeforeRequestDetails> IPC::decode(Decoder& decoder)
{
    auto request_id = TRY(decoder.decode<u64>());
    auto url = TRY(decoder.decode<URL::URL>());
    auto method = TRY(decoder.decode<String>());
    auto type = TRY(decoder.decode<Web::Extensions::WebRequestResourceType>());
    auto tab_id = TRY(decoder.decode<u64>());
    auto frame_id = TRY(decoder.decode<u64>());
    auto parent_frame_id = TRY(decoder.decode<u64>());
    auto initiator_origin = TRY(decoder.decode<Optional<String>>());
    auto document_url = TRY(decoder.decode<Optional<String>>());
    auto is_navigation = TRY(decoder.decode<bool>());
    auto source_document_id = TRY(decoder.decode<Optional<Web::HTML::CrossProcessId>>());
    return Web::Extensions::WebRequestBeforeRequestDetails {
        request_id,
        move(url),
        move(method),
        type,
        tab_id,
        frame_id,
        parent_frame_id,
        move(initiator_origin),
        move(document_url),
        is_navigation,
        source_document_id,
    };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::Extensions::WebRequestDecision const& decision)
{
    TRY(encoder.encode(decision.cancel));
    return {};
}

template<>
ErrorOr<Web::Extensions::WebRequestDecision> IPC::decode(Decoder& decoder)
{
    auto cancel = TRY(decoder.decode<bool>());
    return Web::Extensions::WebRequestDecision { cancel };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::Extensions::XpiVerificationResult const& result)
{
    TRY(encoder.encode(result.ok));
    TRY(encoder.encode(result.error));
    TRY(encoder.encode(result.addon_id));
    TRY(encoder.encode(result.version));
    TRY(encoder.encode(result.sha256_hex));
    TRY(encoder.encode(result.manifest_json));
    TRY(encoder.encode(result.compatibility_status));
    TRY(encoder.encode(result.compatibility_reasons));
    TRY(encoder.encode(result.verified_signature));
    TRY(encoder.encode(result.signer_cn));
    return {};
}

template<>
ErrorOr<Web::Extensions::XpiVerificationResult> IPC::decode(Decoder& decoder)
{
    auto ok = TRY(decoder.decode<bool>());
    auto error = TRY(decoder.decode<String>());
    auto addon_id = TRY(decoder.decode<String>());
    auto version = TRY(decoder.decode<String>());
    auto sha256_hex = TRY(decoder.decode<String>());
    auto manifest_json = TRY(decoder.decode<String>());
    auto compatibility_status = TRY(decoder.decode<Web::Extensions::CompatibilityStatus>());
    auto compatibility_reasons = TRY(decoder.decode<Vector<String>>());
    auto verified_signature = TRY(decoder.decode<bool>());
    auto signer_cn = TRY(decoder.decode<String>());
    return Web::Extensions::XpiVerificationResult {
        ok,
        move(error),
        move(addon_id),
        move(version),
        move(sha256_hex),
        move(manifest_json),
        compatibility_status,
        move(compatibility_reasons),
        verified_signature,
        move(signer_cn),
    };
}
