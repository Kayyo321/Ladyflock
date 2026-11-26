/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/StdLibExtras.h>
#include <LibHTTP/HeaderList.h>
#include <LibRequests/Request.h>
#include <LibRequests/RequestClient.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/Extensions/CatalogClient.h>

namespace WebView::Extensions {

static String url_encode_query(StringView input)
{
    StringBuilder builder;
    for (u8 byte : input.bytes()) {
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9')
            || byte == '-' || byte == '_' || byte == '.' || byte == '~') {
            builder.append(static_cast<char>(byte));
        } else {
            builder.appendff("%{:02X}", byte);
        }
    }
    return MUST(builder.to_string());
}

static String extract_localized_string(JsonValue const& value)
{
    if (value.is_string())
        return value.as_string();
    if (value.is_object()) {
        auto const& obj = value.as_object();
        if (auto en_us = obj.get_string("en-US"sv); en_us.has_value())
            return *en_us;
        if (auto en = obj.get_string("en"sv); en.has_value())
            return *en;
        String first;
        obj.for_each_member([&](String const&, JsonValue const& member) {
            if (first.is_empty() && member.is_string())
                first = member.as_string();
        });
        return first;
    }
    return {};
}

static bool is_amo_url(URL::URL const& url, bool package)
{
    if (url.scheme() != "https"sv || url.port().has_value() || url.includes_credentials() || url.fragment().has_value())
        return false;
    auto path = url.serialize_path();
    if (!package)
        return url.serialized_host() == "addons.mozilla.org"sv && path.starts_with("/api/v5/addons/"sv);
    if (url.serialized_host() == "addons.mozilla.org"sv)
        return path.starts_with("/firefox/downloads/file/"sv) && path.ends_with(".xpi"sv);
    return url.serialized_host() == "addons.cdn.mozilla.net"sv && path.ends_with(".xpi"sv);
}

static CatalogEntry parse_entry(JsonObject const& obj)
{
    CatalogEntry entry;
    entry.addon_id = obj.get_string("guid"sv).value_or({});
    entry.slug = obj.get_string("slug"sv).value_or({});
    if (auto name = obj.get("name"sv); name.has_value())
        entry.name = extract_localized_string(*name);
    if (auto summary = obj.get("summary"sv); summary.has_value())
        entry.summary = extract_localized_string(*summary);
    if (auto url = obj.get_string("url"sv); url.has_value()) {
        auto parsed_url = URL::Parser::basic_parse(*url);
        if (parsed_url.has_value() && parsed_url->scheme() == "https"sv && parsed_url->serialized_host() == "addons.mozilla.org"sv
            && !parsed_url->includes_credentials() && !parsed_url->port().has_value())
            entry.listing_url = *url;
    }
    if (auto authors = obj.get_array("authors"sv); authors.has_value() && !authors->is_empty() && authors->at(0).is_object())
        entry.author = authors->at(0).as_object().get_string("name"sv).value_or({});

    if (auto current_version = obj.get_object("current_version"sv); current_version.has_value()) {
        entry.version = current_version->get_string("version"sv).value_or({});
        if (auto file = current_version->get_object("file"sv); file.has_value()) {
            if (file->get_string("status"sv).value_or({}) == "public"sv) {
                if (auto url = file->get_string("url"sv); url.has_value())
                    entry.download_url = *url;
            }
            if (auto hash = file->get_string("hash"sv); hash.has_value()) {
                auto hash_text = hash->bytes_as_string_view();
                if (hash_text.starts_with("sha256:"sv) && hash_text.length() == 71 && all_of(hash_text.substring_view(7), is_ascii_hex_digit))
                    entry.sha256_hex = MUST(String::from_utf8(hash_text.substring_view(7)));
            }
            if (auto permissions = file->get_array("permissions"sv); permissions.has_value()) {
                for (auto const& permission : permissions->values()) {
                    if (permission.is_string() && permission.as_string() != "<all_urls>"sv && !permission.as_string().contains("://"sv))
                        entry.permissions.append(permission.as_string());
                }
            }
        }
    }

    auto evaluation = Web::Extensions::SupportMatrix::evaluate_catalog_metadata(
        entry.addon_id.bytes_as_string_view(), entry.version.bytes_as_string_view(),
        entry.sha256_hex.map([](auto const& hash) { return hash.bytes_as_string_view(); }), entry.permissions.span());
    entry.compatibility_status = evaluation.status;
    entry.compatibility_reasons = move(evaluation.unsupported_reasons);
    return entry;
}

static ErrorOr<CatalogSearchResult> parse_search(StringView json_text)
{
    auto document = TRY(JsonValue::from_string(json_text));
    if (!document.is_object())
        return Error::from_string_literal("AMO search response must be a JSON object");
    auto const& object = document.as_object();
    auto results = object.get_array("results"sv);
    if (!results.has_value())
        return Error::from_string_literal("AMO search response missing results");
    CatalogSearchResult result;
    result.has_more = object.get_string("next"sv).has_value();
    for (auto const& item : results->values()) {
        if (!item.is_object())
            continue;
        auto entry = parse_entry(item.as_object());
        if (!entry.addon_id.is_empty() && !entry.name.is_empty())
            result.entries.append(move(entry));
    }
    return result;
}

ErrorOr<Vector<CatalogEntry>> CatalogClient::parse_search_response(StringView json_text)
{
    return TRY(parse_search(json_text)).entries;
}

static void fetch_amo(URL::URL const& url, bool package, unsigned redirects_remaining, Function<void(ErrorOr<ByteBuffer>)> callback)
{
    if (!is_amo_url(url, package)) {
        callback(Error::from_string_literal("Mozilla Add-ons URL is outside the allowed provider"));
        return;
    }

    auto client_or_error = Application::request_server_client(IsPrivate::No);
    if (client_or_error.is_error()) {
        callback(client_or_error.release_error());
        return;
    }

    auto headers = HTTP::HeaderList::create();
    headers->set({ "User-Agent"sv, "Ladyflock"sv });
    if (!package)
        headers->set({ "Accept"sv, "application/json"sv });
    auto request = client_or_error.value()->start_request("GET"sv, url, {}, *headers, {}, HTTP::CacheMode::NoStore, HTTP::Cookie::IncludeCredentials::No);
    if (!request) {
        callback(Error::from_string_literal("Failed to start AMO catalog request"));
        return;
    }

    struct CatalogFetchState : public RefCounted<CatalogFetchState> {
        ByteBuffer buffer;
        Optional<u32> status_code;
        Optional<ByteString> location;
        Function<void(ErrorOr<ByteBuffer>)> callback;
        bool exceeds_limit { false };
        bool is_json { false };
    };
    auto state = make_ref_counted<CatalogFetchState>();
    state->callback = move(callback);
    auto append_data = [state, limit = package ? 64 * MiB : 2 * MiB](ReadonlyBytes bytes) {
        // The response pipe can become readable before the headers IPC message.
        if (state->status_code.value_or(200u) != 200u || state->exceeds_limit)
            return;
        if (bytes.size() > limit - state->buffer.size() || state->buffer.try_append(bytes).is_error())
            state->exceeds_limit = true;
    };

    request->set_unbuffered_request_callbacks(
        [state](NonnullRefPtr<HTTP::HeaderList> headers, Optional<u32> status_code, Optional<String> const&, Requests::CacheState) {
            state->status_code = status_code;
            state->location = headers->get("Location"sv);
            if (auto content_type = headers->get("Content-Type"sv); content_type.has_value()) {
                auto type = content_type->view();
                type = type.substring_view(0, type.find(';').value_or(type.length())).trim_whitespace();
                state->is_json = type.equals_ignoring_ascii_case("application/json"sv);
            }
        },
        [append_data](Requests::ResponseData data) { append_data(data.bytes()); },
        [](Core::ImmutableBytes) { },
        [state, request, url, package, redirects_remaining](u64, Requests::RequestTimingInfo const&, Optional<Requests::NetworkError> network_error) {
            if (network_error.has_value()) {
                state->callback(Error::from_string_literal("Mozilla Add-ons request failed"));
                return;
            }
            auto status = state->status_code.value_or(0);
            if (status >= 300 && status < 400 && redirects_remaining > 0 && state->location.has_value()) {
                auto redirect = URL::Parser::basic_parse(*state->location, url);
                if (redirect.has_value()) {
                    fetch_amo(*redirect, package, redirects_remaining - 1, move(state->callback));
                    return;
                }
            }
            if (status != 200) {
                state->callback(Error::from_string_literal("Mozilla Add-ons did not return the requested resource"));
                return;
            }
            if (!package && !state->is_json) {
                state->callback(Error::from_string_literal("Mozilla Add-ons did not return a JSON response"));
                return;
            }
            if (state->exceeds_limit) {
                state->callback(Error::from_string_literal("Mozilla Add-ons response exceeds the size limit"));
                return;
            }
            state->callback(move(state->buffer));
            (void)request;
        });
}

void CatalogClient::search(StringView query, Function<void(ErrorOr<Vector<CatalogEntry>>)> callback) const
{
    search(query, 1, [callback = move(callback)](ErrorOr<CatalogSearchResult> response) {
        if (response.is_error())
            return callback(response.release_error());
        callback(move(response.value().entries));
    });
}

void CatalogClient::search(StringView query, u32 page, Function<void(ErrorOr<CatalogSearchResult>)> callback) const
{
    if (m_offline_fixture_json.has_value())
        return callback(parse_search(*m_offline_fixture_json));
    if (!m_enable_public_amo_catalog)
        return callback(Error::from_string_literal("Mozilla Add-ons catalog lookup is disabled in this build"));
    if (query.length() > 100 || page < 1 || page > 100)
        return callback(Error::from_string_literal("Invalid Mozilla Add-ons search query or page"));
    auto url = URL::Parser::basic_parse(ByteString::formatted("{}/addons/search/?q={}&app=firefox&type=extension&page={}", m_api_base_url, url_encode_query(query), page));
    if (!url.has_value())
        return callback(Error::from_string_literal("Invalid Mozilla Add-ons API URL"));
    fetch_amo(*url, false, 2, [callback = move(callback)](ErrorOr<ByteBuffer> response) {
        if (response.is_error())
            return callback(response.release_error());
        callback(parse_search(StringView { response.value().bytes() }));
    });
}

void CatalogClient::lookup(StringView addon_id, Function<void(ErrorOr<CatalogEntry>)> callback) const
{
    if (!m_enable_public_amo_catalog)
        return callback(Error::from_string_literal("Mozilla Add-ons catalog lookup is disabled in this build"));
    if (addon_id.is_empty() || addon_id.length() > 255 || addon_id == "."sv || addon_id == ".."sv)
        return callback(Error::from_string_literal("Invalid Mozilla Add-ons ID"));
    auto url = URL::Parser::basic_parse(ByteString::formatted("{}/addons/addon/{}/", m_api_base_url, url_encode_query(addon_id)));
    if (!url.has_value())
        return callback(Error::from_string_literal("Invalid Mozilla Add-ons API URL"));
    auto id = MUST(String::from_utf8(addon_id));
    fetch_amo(*url, false, 2, [callback = move(callback), id = move(id)](ErrorOr<ByteBuffer> response) {
        if (response.is_error())
            return callback(response.release_error());
        auto parsed = JsonValue::from_string(StringView { response.value().bytes() });
        if (parsed.is_error() || !parsed.value().is_object())
            return callback(Error::from_string_literal("Invalid Mozilla Add-ons detail response"));
        auto const& object = parsed.value().as_object();
        if (object.get_string("type"sv).value_or({}) != "extension"sv)
            return callback(Error::from_string_literal("Mozilla Add-ons result is not a browser extension"));
        auto entry = parse_entry(object);
        if (entry.addon_id != id || entry.version.is_empty() || !entry.download_url.has_value() || !entry.sha256_hex.has_value())
            return callback(Error::from_string_literal("Mozilla Add-ons listing has no matching public package"));
        callback(move(entry));
    });
}

void CatalogClient::download(CatalogEntry const& entry, Function<void(ErrorOr<ByteBuffer>)> callback) const
{
    if (!m_enable_public_amo_catalog || !entry.download_url.has_value() || !entry.sha256_hex.has_value())
        return callback(Error::from_string_literal("Mozilla Add-ons package is unavailable"));
    auto url = URL::Parser::basic_parse(*entry.download_url);
    if (!url.has_value())
        return callback(Error::from_string_literal("Invalid Mozilla Add-ons download URL"));
    fetch_amo(*url, true, 3, move(callback));
}

}
