/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/LexicalPath.h>
#include <AK/Math.h>
#include <AK/NeverDestroyed.h>
#include <AK/Random.h>
#include <AK/StringBuilder.h>
#include <AK/Time.h>
#include <LibURL/InternalURLs.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/MimeSniff/Resource.h>
#include <LibWebView/HistoryStore.h>
#include <LibWebView/SearchEngine.h>
#include <LibWebView/Settings.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/BrowserWindow.h>
#include <UI/Qt/DashboardPage.h>
#include <UI/Qt/HubConnection.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/Tab.h>
#include <UI/Qt/Theme.h>

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QIcon>
#include <QPixmap>
#include <QRegularExpression>
#include <QXmlStreamReader>

namespace Ladybird {

namespace {

struct DashboardCoordinator {
    OwnPtr<DashboardStore> persistent_store;
    HashMap<u64, NonnullOwnPtr<DashboardStore>> ephemeral_stores;
    Vector<DashboardPage*> live_pages;
    HashMap<String, size_t> active_fetches_per_scope;
    // Boards whose Customize button was hidden until the browser quits.
    HashTable<String> customize_hidden_for_session;
    u64 next_nonce { 1 };

    DashboardStore& store_for(Hub::WebUIPageContext const& ctx)
    {
        bool use_persistent = ctx.is_private == WebView::IsPrivate::No
            && !ctx.is_ephemeral
            && (ctx.identity_id.is_empty() || ctx.identity_id == "default"sv)
            && !Application::the().profile_is_temporary();

        if (use_persistent) {
            if (!persistent_store) {
                persistent_store = DashboardStore::open_persistent(
                    Application::the().data_directory(),
                    Application::the().uses_sql_database());
            }
            return *persistent_store;
        }

        auto session_key = ctx.browsing_session_id;
        if (auto it = ephemeral_stores.find(session_key); it != ephemeral_stores.end())
            return *it->value;

        auto created = DashboardStore::create_ephemeral();
        auto& ref = *created;
        ephemeral_stores.set(session_key, move(created));
        return ref;
    }

    void maybe_release_ephemeral_store(u64 browsing_session_id)
    {
        for (auto* page : live_pages) {
            if (page->context().browsing_session_id == browsing_session_id)
                return;
        }
        ephemeral_stores.remove(browsing_session_id);
    }

    void broadcast_to_scope(String const& scope_key, DashboardPage const* exclude, Function<void(DashboardPage&)> const& fn)
    {
        for (auto* page : live_pages) {
            if (page == exclude)
                continue;
            if (page->scope_key() == scope_key)
                fn(*page);
        }
    }
};

DashboardCoordinator& coordinator()
{
    static NeverDestroyed<DashboardCoordinator> s_coordinator;
    return *s_coordinator;
}

String compute_scope_key(Hub::WebUIPageContext const& ctx)
{
    if (ctx.is_private == WebView::IsPrivate::Yes || ctx.is_ephemeral)
        return MUST(String::formatted("ephemeral:{}", ctx.browsing_session_id));
    if (ctx.identity_id.is_empty() || ctx.identity_id == "default"sv)
        return "default"_string;
    return MUST(String::formatted("identity:{}:{}", ctx.identity_id, ctx.browsing_session_id));
}

String generate_opaque_id(StringView prefix)
{
    u64 a = get_random<u64>();
    u64 b = get_random<u64>();
    return MUST(String::formatted("{}-{:016x}{:08x}", prefix, a, static_cast<u32>(b)));
}

String strip_html_tags(QString const& raw)
{
    static QRegularExpression const tag_re(QStringLiteral("<[^>]*>"));
    static QRegularExpression const ws_re(QStringLiteral("\\s+"));
    QString cleaned = raw;
    cleaned.replace(tag_re, QStringLiteral(" "));
    cleaned.replace(ws_re, QStringLiteral(" "));
    cleaned = cleaned.trimmed();
    if (cleaned.size() > 320)
        cleaned = cleaned.left(317) + QStringLiteral("…");
    return ak_string_from_qstring(cleaned);
}

bool is_safe_http_url(URL::URL const& url)
{
    return url.scheme().is_one_of("http"sv, "https"sv)
        && url.username().is_empty()
        && url.password().is_empty()
        && url.host().has_value()
        && !url.host()->is_empty_host();
}

bool is_public_https_url(URL::URL const& url)
{
    if (url.scheme() != "https"sv || !url.username().is_empty() || !url.password().is_empty())
        return false;
    if (!url.host().has_value() || url.host()->is_empty_host())
        return false;
    if (url.host()->is_loopback_or_localhost())
        return false;
    auto serialized = url.serialized_host();
    if (serialized.is_empty() || !serialized.contains('.'))
        return false;
    if (serialized.ends_with(".local"sv, CaseSensitivity::CaseInsensitive)
        || serialized.ends_with(".localhost"sv, CaseSensitivity::CaseInsensitive)
        || serialized.ends_with(".internal"sv, CaseSensitivity::CaseInsensitive)
        || serialized.ends_with(".home.arpa"sv, CaseSensitivity::CaseInsensitive))
        return false;
    return true;
}

String dashboard_image_type(ReadonlyBytes bytes)
{
    auto mime_type = Web::MimeSniff::Resource::sniff(bytes, { .sniffing_context = Web::MimeSniff::SniffingContext::Image });
    auto const& essence = mime_type.essence();
    if (essence.is_one_of("image/png"sv, "image/jpeg"sv, "image/gif"sv, "image/webp"sv, "image/bmp"sv))
        return essence;

    // The image sniffing rules do not include AVIF. Its opening file type box
    // identifies still images and sequences by their major or compatible brand.
    StringView header { bytes };
    if (header.length() >= 16 && header.substring_view(4, 4) == "ftyp"sv) {
        u32 box_size = (static_cast<u32>(bytes[0]) << 24) | (static_cast<u32>(bytes[1]) << 16) | (static_cast<u32>(bytes[2]) << 8) | bytes[3];
        if (box_size >= 16 && box_size <= bytes.size()) {
            if (header.substring_view(8, 4).is_one_of("avif"sv, "avis"sv))
                return "image/avif"_string;
            for (size_t offset = 16; offset + 4 <= box_size; offset += 4) {
                if (header.substring_view(offset, 4).is_one_of("avif"sv, "avis"sv))
                    return "image/avif"_string;
            }
        }
    }
    return {};
}

ErrorOr<JsonObject> dashboard_image_payload(StringView mime_type, ReadonlyBytes bytes)
{
    auto base64 = TRY(encode_base64(bytes));
    JsonObject result;
    result.set("dataUrl"sv, TRY(String::formatted("data:{};base64,{}", mime_type, base64)));
    return result;
}

bool is_allowed_tool_url(URL::URL const& url, bool is_persistent_default)
{
    if (url.scheme() != "about"sv)
        return false;
    auto path = url.serialize_path();
    if (path.is_one_of("settings"sv, "privacy"sv, "support"sv, "version"sv, "crashes"sv, "licenses"sv))
        return true;
    if (is_persistent_default && path.is_one_of("bookmarks"sv, "history"sv, "downloads"sv, "addons"sv))
        return true;
    return false;
}

Optional<JsonObject> parse_feed_xml(ByteString const& raw_xml, String const& source_url)
{
    QXmlStreamReader xml(QByteArray::fromRawData(raw_xml.characters(), static_cast<qsizetype>(raw_xml.length())));
    xml.setEntityExpansionLimit(0);

    String feed_title;
    JsonArray items;
    static constexpr size_t MAX_ITEMS = 20;

    bool in_rss_item = false;
    bool in_atom_entry = false;
    bool in_channel_header = false;
    String cur_title;
    String cur_link;
    String cur_date;
    String cur_excerpt;

    auto flush_item = [&] {
        if (items.size() >= MAX_ITEMS)
            return;
        if (cur_title.is_empty() && cur_link.is_empty())
            return;
        String safe_link;
        if (!cur_link.is_empty()) {
            if (auto parsed = URL::Parser::basic_parse(cur_link); parsed.has_value() && is_safe_http_url(*parsed))
                safe_link = parsed->serialize();
        }
        JsonObject item;
        item.set("title"sv, cur_title.is_empty() ? "(Untitled)"_string : cur_title);
        item.set("url"sv, safe_link);
        item.set("publishedAt"sv, cur_date);
        item.set("excerpt"sv, cur_excerpt);
        items.must_append(move(item));
        cur_title = {};
        cur_link = {};
        cur_date = {};
        cur_excerpt = {};
    };

    while (!xml.atEnd()) {
        auto token = xml.readNext();
        if (token == QXmlStreamReader::DTD)
            return {};

        if (token == QXmlStreamReader::StartElement) {
            auto name = xml.name().toString().toLower();
            if (name == QStringLiteral("channel") || name == QStringLiteral("feed")) {
                in_channel_header = true;
            } else if (name == QStringLiteral("item")) {
                in_rss_item = true;
                in_channel_header = false;
                cur_title = {};
                cur_link = {};
                cur_date = {};
                cur_excerpt = {};
            } else if (name == QStringLiteral("entry")) {
                in_atom_entry = true;
                in_channel_header = false;
                cur_title = {};
                cur_link = {};
                cur_date = {};
                cur_excerpt = {};
            } else if (in_rss_item || in_atom_entry) {
                if (name == QStringLiteral("title") && cur_title.is_empty()) {
                    cur_title = strip_html_tags(xml.readElementText(QXmlStreamReader::IncludeChildElements));
                } else if (name == QStringLiteral("link") && cur_link.is_empty()) {
                    auto href = xml.attributes().value(QStringLiteral("href")).toString().trimmed();
                    if (!href.isEmpty())
                        cur_link = ak_string_from_qstring(href);
                    else
                        cur_link = ak_string_from_qstring(xml.readElementText(QXmlStreamReader::IncludeChildElements).trimmed());
                } else if ((name == QStringLiteral("pubdate") || name == QStringLiteral("updated") || name == QStringLiteral("published") || name == QStringLiteral("date")) && cur_date.is_empty()) {
                    cur_date = ak_string_from_qstring(xml.readElementText(QXmlStreamReader::IncludeChildElements).trimmed());
                } else if ((name == QStringLiteral("description") || name == QStringLiteral("summary") || name == QStringLiteral("content")) && cur_excerpt.is_empty()) {
                    cur_excerpt = strip_html_tags(xml.readElementText(QXmlStreamReader::IncludeChildElements));
                }
            } else if (in_channel_header && name == QStringLiteral("title") && feed_title.is_empty()) {
                feed_title = strip_html_tags(xml.readElementText(QXmlStreamReader::IncludeChildElements));
            }
        } else if (token == QXmlStreamReader::EndElement) {
            auto name = xml.name().toString().toLower();
            if (name == QStringLiteral("item") && in_rss_item) {
                flush_item();
                in_rss_item = false;
            } else if (name == QStringLiteral("entry") && in_atom_entry) {
                flush_item();
                in_atom_entry = false;
            }
        }
    }

    if (xml.hasError() && items.is_empty())
        return {};

    JsonObject out;
    out.set("title"sv, feed_title.is_empty() ? source_url : feed_title);
    out.set("sourceUrl"sv, source_url);
    out.set("items"sv, move(items));
    out.set("fetchedAt"sv, UnixDateTime::now().milliseconds_since_epoch());
    return out;
}

Optional<JsonObject> parse_open_meteo_json(ByteString const& raw_json)
{
    auto parsed = JsonValue::from_string(raw_json);
    if (parsed.is_error() || !parsed.value().is_object())
        return {};
    auto const& root = parsed.value().as_object();
    auto current = root.get_object("current"sv);
    if (!current.has_value())
        return {};

    JsonObject out;
    out.set("provider"sv, "Open-Meteo"_string);
    out.set("attribution"sv, "Weather data by Open-Meteo.com (CC BY 4.0)"_string);
    out.set("fetchedAt"sv, UnixDateTime::now().milliseconds_since_epoch());
    out.set("timezone"sv, root.get_string("timezone"sv).value_or("UTC"_string));

    JsonObject cur;
    if (auto v = current->get_double_with_precision_loss("temperature_2m"sv); v.has_value())
        cur.set("tempC"sv, *v);
    if (auto v = current->get_double_with_precision_loss("apparent_temperature"sv); v.has_value())
        cur.set("feelsLikeC"sv, *v);
    if (auto v = current->get_integer<i64>("weather_code"sv); v.has_value())
        cur.set("weatherCode"sv, *v);
    if (auto v = current->get_double_with_precision_loss("wind_speed_10m"sv); v.has_value())
        cur.set("windKmh"sv, *v);
    if (auto v = current->get_integer<i64>("relative_humidity_2m"sv); v.has_value())
        cur.set("humidity"sv, *v);
    out.set("current"sv, move(cur));

    JsonArray hourly_out;
    if (auto hourly = root.get_object("hourly"sv); hourly.has_value()) {
        auto times = hourly->get_array("time"sv);
        auto temps = hourly->get_array("temperature_2m"sv);
        auto codes = hourly->get_array("weather_code"sv);
        auto precip = hourly->get_array("precipitation_probability"sv);
        if (times.has_value() && temps.has_value()) {
            size_t count = min(times->size(), min(temps->size(), 24uz));
            for (size_t i = 0; i < count; ++i) {
                JsonObject h;
                h.set("time"sv, times->at(i).as_string());
                h.set("tempC"sv, temps->at(i).get_double_with_precision_loss().value_or(0.0));
                if (codes.has_value() && i < codes->size())
                    h.set("weatherCode"sv, codes->at(i).get_integer<i64>().value_or(0));
                if (precip.has_value() && i < precip->size())
                    h.set("precipProb"sv, precip->at(i).get_integer<i64>().value_or(0));
                hourly_out.must_append(move(h));
            }
        }
    }
    out.set("hourly"sv, move(hourly_out));

    JsonArray daily_out;
    if (auto daily = root.get_object("daily"sv); daily.has_value()) {
        auto times = daily->get_array("time"sv);
        auto codes = daily->get_array("weather_code"sv);
        auto maxs = daily->get_array("temperature_2m_max"sv);
        auto mins = daily->get_array("temperature_2m_min"sv);
        auto precip = daily->get_array("precipitation_probability_max"sv);
        if (times.has_value() && maxs.has_value() && mins.has_value()) {
            size_t count = min(times->size(), min(maxs->size(), min(mins->size(), 5uz)));
            for (size_t i = 0; i < count; ++i) {
                JsonObject d;
                d.set("date"sv, times->at(i).as_string());
                d.set("maxC"sv, maxs->at(i).get_double_with_precision_loss().value_or(0.0));
                d.set("minC"sv, mins->at(i).get_double_with_precision_loss().value_or(0.0));
                if (codes.has_value() && i < codes->size())
                    d.set("weatherCode"sv, codes->at(i).get_integer<i64>().value_or(0));
                if (precip.has_value() && i < precip->size())
                    d.set("precipProb"sv, precip->at(i).get_integer<i64>().value_or(0));
                daily_out.must_append(move(d));
            }
        }
    }
    out.set("daily"sv, move(daily_out));
    return out;
}

}

DashboardPage::DashboardPage(Hub::WebUIPageContext const& context)
    : WebUIPage(context.web_ui_id)
    , BookmarkStoreObserver(Application::bookmark_store())
    , m_context(context)
    , m_scope_key(compute_scope_key(context))
    , m_handle_nonce(coordinator().next_nonce++)
{
    coordinator().live_pages.append(this);
}

DashboardPage::~DashboardPage()
{
    m_is_closing = true;
    cancel_in_flight_requests();
    coordinator().live_pages.remove_first_matching([this](auto* p) { return p == this; });
    if (is_ephemeral_scope())
        coordinator().maybe_release_ephemeral_store(m_context.browsing_session_id);
}

void DashboardPage::reset_for_testing()
{
    coordinator().persistent_store.clear();
    coordinator().ephemeral_stores.clear();
    coordinator().active_fetches_per_scope.clear();
}

void DashboardPage::notify_history_cleared()
{
    for (auto* page : coordinator().live_pages) {
        if (!page->is_ephemeral_scope())
            page->push_event("historyCleared"sv, JsonObject {});
    }
}

void DashboardPage::notify_privacy_network_mode_changed()
{
    for (auto* page : coordinator().live_pages) {
        if (Application::the().privacy_network_mode() == Privacy::NetworkMode::Offline)
            page->cancel_in_flight_requests();
        page->push_event("privacyChanged"sv, JsonObject {});
    }
}

void DashboardPage::notify_recently_closed_changed()
{
    for (auto* page : coordinator().live_pages) {
        if (page->is_persistent_default_scope())
            page->push_event("recentlyClosedChanged"sv, JsonObject {});
    }
}

// Every tile, button and mark on the board is colored by the accent, in both of the page's appearances.
static JsonObject accent_json()
{
    auto hex = [](QColor const& color) {
        return MUST(String::formatted("#{:02x}{:02x}{:02x}", color.red(), color.green(), color.blue()));
    };
    auto light = Theme::page_accent(false);
    auto dark = Theme::page_accent(true);
    JsonObject accent;
    accent.set("light"sv, hex(light.accent));
    accent.set("lightFill"sv, hex(light.fill));
    accent.set("dark"sv, hex(dark.accent));
    accent.set("darkFill"sv, hex(dark.fill));
    return accent;
}

void DashboardPage::notify_accent_changed()
{
    auto accent = accent_json();
    for (auto* page : coordinator().live_pages)
        page->push_event("accentChanged"sv, accent);
}

DashboardStore& DashboardPage::store()
{
    return coordinator().store_for(m_context);
}

bool DashboardPage::is_persistent_default_scope() const
{
    return m_context.is_private == WebView::IsPrivate::No
        && !m_context.is_ephemeral
        && (m_context.identity_id.is_empty() || m_context.identity_id == "default"sv);
}

bool DashboardPage::is_ephemeral_scope() const
{
    return m_context.is_private == WebView::IsPrivate::Yes || m_context.is_ephemeral;
}

bool DashboardPage::allows_network_widgets() const
{
    return !is_ephemeral_scope() && !Application::the().profile_is_temporary();
}

BrowserWindow* DashboardPage::owning_window() const
{
    if (m_context.view_id == 0)
        return nullptr;
    return Application::the().window_of_view_id(m_context.view_id);
}

Tab* DashboardPage::owning_tab() const
{
    auto* win = owning_window();
    if (!win)
        return nullptr;
    Tab* found = nullptr;
    win->for_each_tab([&](Tab& tab) {
        if (tab.is_loaded() && tab.view().view_id() == m_context.view_id)
            found = &tab;
    });
    return found;
}

void DashboardPage::reply_ok(i64 request_id, JsonValue data)
{
    if (m_is_closing)
        return;
    JsonObject msg;
    msg.set("requestId"sv, request_id);
    msg.set("documentGeneration"sv, m_context.document_generation);
    msg.set("ok"sv, true);
    msg.set("data"sv, move(data));
    send_message("dashboardReply"_string, move(msg));
}

void DashboardPage::reply_error(i64 request_id, StringView error, Optional<JsonObject> extra)
{
    if (m_is_closing)
        return;
    JsonObject msg = extra.has_value() ? extra.release_value() : JsonObject {};
    msg.set("requestId"sv, request_id);
    msg.set("documentGeneration"sv, m_context.document_generation);
    msg.set("ok"sv, false);
    msg.set("error"sv, MUST(String::from_utf8(error)));
    send_message("dashboardReply"_string, move(msg));
}

void DashboardPage::push_event(StringView event_name, JsonValue data)
{
    if (m_is_closing)
        return;
    JsonObject msg;
    msg.set("event"sv, MUST(String::from_utf8(event_name)));
    msg.set("documentGeneration"sv, m_context.document_generation);
    msg.set("data"sv, move(data));
    send_message("dashboardEvent"_string, move(msg));
}

void DashboardPage::bookmarks_changed()
{
    if (is_persistent_default_scope())
        push_event("bookmarksChanged"sv, JsonObject {});
}

void DashboardPage::download_added(RemoteDownloads::Download const& dl)
{
    if (dl.is_private == m_context.is_private && dl.identity_id == (is_persistent_default_scope() ? String {} : m_context.identity_id))
        push_event("downloadsChanged"sv, JsonObject {});
}

void DashboardPage::download_updated(RemoteDownloads::Download const& dl)
{
    if (dl.is_private == m_context.is_private && dl.identity_id == (is_persistent_default_scope() ? String {} : m_context.identity_id))
        push_event("downloadsChanged"sv, JsonObject {});
}

void DashboardPage::download_removed(u64, WebView::IsPrivate is_private)
{
    if (is_private == m_context.is_private)
        push_event("downloadsChanged"sv, JsonObject {});
}

void DashboardPage::cancel_in_flight_requests()
{
    auto* hub = HubConnection::the_if_connected();
    for (auto const& [req_id, _] : m_in_flight_requests) {
        if (hub)
            hub->stop_request(req_id);
        auto& count = coordinator().active_fetches_per_scope.ensure(m_scope_key);
        if (count > 0)
            --count;
    }
    m_in_flight_requests.clear();
}

void DashboardPage::received_message(String const& name, JsonValue const& data)
{
    if (name != "dashboardRequest"sv || !data.is_object())
        return;
    auto const& obj = data.as_object();
    auto request_id = obj.get_integer<i64>("requestId"sv).value_or(0);
    auto method = obj.get_string("method"sv).value_or(String {});
    auto params = obj.get_object("params"sv).value_or(JsonObject {});

    if (method == "dashboard.init"sv)
        handle_init(request_id);
    else if (method == "dashboard.setCustomizeButton"sv)
        handle_set_customize_button(request_id, params);
    else if (method == "dashboard.commitLayout"sv)
        handle_commit_layout(request_id, params);
    else if (method == "dashboard.deleteAllData"sv)
        handle_delete_all_data(request_id);
    else if (method == "items.list"sv)
        handle_items_list(request_id, params);
    else if (method == "items.save"sv)
        handle_items_save(request_id, params);
    else if (method == "items.delete"sv)
        handle_items_delete(request_id, params);
    else if (method == "items.clearCompleted"sv)
        handle_items_clear_completed(request_id, params);
    else if (method == "images.load"sv)
        handle_image_load(request_id, params);
    else if (method == "images.save"sv)
        handle_image_save(request_id, params);
    else if (method == "images.remove"sv)
        handle_image_remove(request_id, params);
    else if (method == "images.fetchUrl"sv)
        handle_image_fetch_url(request_id, params);
    else if (method == "shaders.list"sv)
        handle_shaders_list(request_id);
    else if (method == "shaders.load"sv)
        handle_shader_load(request_id, params);
    else if (method == "shaders.save"sv)
        handle_shader_save(request_id, params);
    else if (method == "shaders.remove"sv)
        handle_shader_remove(request_id, params);
    else if (method == "topSites.load"sv)
        handle_top_sites_load(request_id);
    else if (method == "search.engines"sv)
        handle_search_engines(request_id);
    else if (method == "search.submit"sv)
        handle_search_submit(request_id, params);
    else if (method == "favicons.lookup"sv)
        handle_favicons_lookup(request_id, params);
    else if (method == "tabs.list"sv)
        handle_tabs_list(request_id);
    else if (method == "tabs.focus"sv)
        handle_tabs_action(request_id, "focus"sv, params);
    else if (method == "tabs.toggleMute"sv)
        handle_tabs_action(request_id, "mute"sv, params);
    else if (method == "tabs.close"sv)
        handle_tabs_action(request_id, "close"sv, params);
    else if (method == "recentlyClosed.list"sv)
        handle_recently_closed_list(request_id);
    else if (method == "recentlyClosed.reopen"sv)
        handle_recently_closed_reopen(request_id, params);
    else if (method == "bookmarks.listFolders"sv)
        handle_bookmarks_folders(request_id);
    else if (method == "bookmarks.listFolder"sv)
        handle_bookmarks_folder(request_id, params);
    else if (method == "history.list"sv)
        handle_history_list(request_id, params);
    else if (method == "history.remove"sv)
        handle_history_remove(request_id, params);
    else if (method == "workspaces.saveFromWindow"sv)
        handle_workspaces_save_window(request_id, params);
    else if (method == "workspaces.open"sv)
        handle_workspaces_open(request_id, params);
    else if (method == "downloads.list"sv)
        handle_downloads_list(request_id);
    else if (method == "downloads.action"sv)
        handle_downloads_action(request_id, params);
    else if (method == "notes.exportMarkdown"sv)
        handle_notes_export(request_id, params);
    else if (method == "shortcuts.list"sv)
        handle_shortcuts_list(request_id);
    else if (method == "navigation.openUrl"sv)
        handle_open_url(request_id, params);
    else if (method == "privacy.summary"sv)
        handle_privacy_summary(request_id);
    else if (method == "extensions.list"sv)
        handle_extensions_list(request_id);
    else if (method == "extensions.setEnabled"sv)
        handle_extensions_set_enabled(request_id, params);
    else if (method == "extensions.openOptions"sv)
        handle_extensions_open_options(request_id, params);
    else if (method == "permissions.list"sv)
        handle_permissions_list(request_id);
    else if (method == "permissions.revoke"sv)
        handle_permissions_revoke(request_id, params);
    else if (method == "profile.summary"sv)
        handle_profile_summary(request_id);
    else if (method == "profile.openPrivateWindow"sv)
        handle_profile_open_private_window(request_id);
    else if (method == "health.snapshot"sv)
        handle_health_snapshot(request_id);
    else if (method == "health.discardTab"sv)
        handle_health_discard_tab(request_id, params);
    else if (method == "media.list"sv)
        handle_media_list(request_id);
    else if (method == "media.action"sv)
        handle_media_action(request_id, params);
    else if (method == "references.list"sv)
        handle_references_list(request_id);
    else if (method == "references.pickFile"sv)
        handle_references_pick_file(request_id);
    else if (method == "references.openFile"sv)
        handle_references_open_file(request_id, params);
    else if (method == "references.removeFile"sv)
        handle_references_remove_file(request_id, params);
    else if (method == "network.listGrants"sv)
        handle_network_list_grants(request_id);
    else if (method == "network.grant"sv)
        handle_network_grant(request_id, params);
    else if (method == "network.revoke"sv)
        handle_network_revoke(request_id, params);
    else if (method == "network.refresh"sv)
        handle_network_refresh(request_id, params);
    else
        reply_error(request_id, "Unknown dashboard method."sv);
}

void DashboardPage::handle_init(i64 request_id)
{
    auto& st = store();
    auto layout = st.load_or_initialize_layout(m_scope_key, is_ephemeral_scope());

    JsonObject ctx;
    ctx.set("documentGeneration"sv, m_context.document_generation);
    ctx.set("isPrivate"sv, m_context.is_private == WebView::IsPrivate::Yes);
    ctx.set("isEphemeral"sv, is_ephemeral_scope());
    ctx.set("isPersistentDefault"sv, is_persistent_default_scope());
    ctx.set("persistsHistory"sv, m_context.persists_history);
    ctx.set("identityId"sv, m_context.identity_id.is_empty() ? "default"_string : m_context.identity_id);
    ctx.set("identityName"sv, m_context.identity_name.is_empty() ? (m_context.is_private == WebView::IsPrivate::Yes ? "Private"_string : "Default"_string) : m_context.identity_name);
    ctx.set("storageStatus"sv, st.storage_status_string());
    ctx.set("allowsNetworkWidgets"sv, allows_network_widgets());
    ctx.set("maxCards"sv, static_cast<i64>(DashboardStore::MAX_CARDS));
    ctx.set("maxGridRows"sv, DashboardStore::MAX_GRID_ROWS);
    ctx.set("maxNoteBytes"sv, static_cast<i64>(DashboardStore::MAX_NOTE_BYTES));
    ctx.set("maxImageBytes"sv, static_cast<i64>(DashboardStore::MAX_IMAGE_BYTES));
    ctx.set("maxShaderBytes"sv, static_cast<i64>(DashboardStore::MAX_SHADER_BYTES));
    ctx.set("accent"sv, accent_json());
    ctx.set("customizeHidden"sv, is_customize_button_hidden());

    JsonObject payload;
    payload.set("context"sv, move(ctx));
    payload.set("layout"sv, DashboardStore::serialize_layout_snapshot(layout));
    reply_ok(request_id, move(payload));
}

// The Customize button can be hidden until the browser quits, or for good. "Forever" is kept in the board's own
// store, so it lasts as long as the board's other data does: a private board forgets it with the rest.
static constexpr auto PREFERENCES_COLLECTION = "dashboard-prefs"sv;
static constexpr auto CUSTOMIZE_BUTTON_ITEM = "customize-button"sv;

bool DashboardPage::is_customize_button_hidden()
{
    if (coordinator().customize_hidden_for_session.contains(m_scope_key))
        return true;
    auto item = store().get_item(m_scope_key, MUST(String::from_utf8(PREFERENCES_COLLECTION)), MUST(String::from_utf8(CUSTOMIZE_BUTTON_ITEM)));
    return item.has_value() && item->value.get_bool("hidden"sv).value_or(false);
}

void DashboardPage::handle_set_customize_button(i64 request_id, JsonObject const& params)
{
    auto visibility = params.get_string("visibility"sv).value_or(String {});
    auto collection = MUST(String::from_utf8(PREFERENCES_COLLECTION));
    auto item_id = MUST(String::from_utf8(CUSTOMIZE_BUTTON_ITEM));
    auto& st = store();

    if (visibility == "session"sv) {
        coordinator().customize_hidden_for_session.set(m_scope_key);
    } else if (visibility == "forever"sv) {
        JsonObject value;
        value.set("hidden"sv, true);
        if (auto res = st.save_item(m_scope_key, collection, item_id, {}, value); !res.ok) {
            reply_error(request_id, res.error);
            return;
        }
        coordinator().customize_hidden_for_session.set(m_scope_key);
    } else if (visibility == "shown"sv) {
        coordinator().customize_hidden_for_session.remove(m_scope_key);
        (void)st.delete_item(m_scope_key, collection, item_id);
    } else {
        reply_error(request_id, "Unknown visibility."sv);
        return;
    }

    auto hidden = is_customize_button_hidden();
    JsonObject out;
    out.set("hidden"sv, hidden);
    reply_ok(request_id, out);
    coordinator().broadcast_to_scope(m_scope_key, this, [&](DashboardPage& peer) {
        peer.push_event("customizeButtonChanged"sv, out);
    });
}

void DashboardPage::handle_commit_layout(i64 request_id, JsonObject const& params)
{
    auto op_id = params.get_string("opId"sv).value_or(String {});
    auto expected_revision = params.get_integer<i64>("expectedRevision"sv).value_or(0);
    auto compact_mode = params.get_bool("compactMode"sv).value_or(false);

    Vector<DashboardStore::WidgetInstance> widgets;
    if (auto arr = params.get_array("widgets"sv); arr.has_value()) {
        for (size_t i = 0; i < arr->size(); ++i) {
            if (!arr->at(i).is_object())
                continue;
            auto const& wo = arr->at(i).as_object();
            DashboardStore::WidgetInstance inst;
            inst.instance_id = wo.get_string("instanceId"sv).value_or(String {});
            inst.widget_type = wo.get_string("widgetType"sv).value_or(String {});
            inst.config_version = wo.get_integer<i64>("configVersion"sv).value_or(1);
            inst.config = wo.get_object("config"sv).value_or(JsonObject {});
            widgets.append(move(inst));
        }
    }

    HashMap<String, Vector<DashboardStore::Placement>> placements;
    if (auto pobj = params.get_object("placements"sv); pobj.has_value()) {
        pobj->for_each_member([&](String const& bp, JsonValue const& val) {
            if (!val.is_array())
                return;
            Vector<DashboardStore::Placement> list;
            auto const& arr = val.as_array();
            for (size_t i = 0; i < arr.size(); ++i) {
                if (!arr.at(i).is_object())
                    continue;
                auto const& po = arr.at(i).as_object();
                list.append({
                    .instance_id = po.get_string("instanceId"sv).value_or(String {}),
                    .col = po.get_integer<i64>("col"sv).value_or(0),
                    .row = po.get_integer<i64>("row"sv).value_or(0),
                    .w = po.get_integer<i64>("w"sv).value_or(1),
                    .h = po.get_integer<i64>("h"sv).value_or(1),
                });
            }
            placements.set(bp, move(list));
        });
    }

    auto& st = store();
    auto res = st.commit_layout(m_scope_key, op_id, expected_revision, compact_mode, widgets, placements);

    if (!res.ok) {
        JsonObject extra;
        extra.set("conflict"sv, res.conflict);
        extra.set("storageStatus"sv, st.storage_status_string());
        extra.set("layout"sv, DashboardStore::serialize_layout_snapshot(res.snapshot));
        reply_error(request_id, res.error, move(extra));
        return;
    }

    // Revoke any network grants that no longer have a placed consumer widget.
    bool has_feed = false;
    bool has_weather = false;
    HashTable<String> referenced_sources;
    for (auto const& w : res.snapshot.widgets) {
        if (w.widget_type == "feed-reader"sv) {
            has_feed = true;
            if (auto sources = w.config.get_array("sourceIds"sv); sources.has_value()) {
                for (size_t i = 0; i < sources->size(); ++i) {
                    if (sources->at(i).is_string())
                        referenced_sources.set(sources->at(i).as_string());
                }
            }
        } else if (w.widget_type == "weather"sv) {
            has_weather = true;
            if (auto sid = w.config.get_string("sourceId"sv); sid.has_value() && !sid->is_empty())
                referenced_sources.set(*sid);
        }
    }
    for (auto const& grant : st.list_network_grants(m_scope_key)) {
        bool orphan = (grant.kind == "feed"sv && !has_feed)
            || (grant.kind == "weather"sv && !has_weather);
        if (orphan) {
            st.revoke_network_grant(m_scope_key, grant.source_id);
            cancel_in_flight_requests();
        }
    }

    auto serialized = DashboardStore::serialize_layout_snapshot(res.snapshot);
    JsonObject out;
    out.set("layout"sv, serialized);
    out.set("duplicate"sv, res.duplicate);
    out.set("storageStatus"sv, st.storage_status_string());
    reply_ok(request_id, move(out));

    if (!res.duplicate) {
        coordinator().broadcast_to_scope(m_scope_key, this, [&](DashboardPage& peer) {
            peer.push_event("layoutUpdated"sv, serialized);
        });
    }
}

void DashboardPage::handle_delete_all_data(i64 request_id)
{
    if (!consume_user_activation()) {
        reply_error(request_id, "User interaction is required."sv);
        return;
    }
    cancel_in_flight_requests();
    coordinator().customize_hidden_for_session.remove(m_scope_key);
    auto& st = store();
    if (!st.delete_scope_data(m_scope_key)) {
        reply_error(request_id, "Couldn’t delete dashboard data."sv);
        return;
    }
    auto fresh = st.load_or_initialize_layout(m_scope_key, is_ephemeral_scope());
    auto serialized = DashboardStore::serialize_layout_snapshot(fresh);
    JsonObject out;
    out.set("layout"sv, serialized);
    reply_ok(request_id, move(out));

    coordinator().broadcast_to_scope(m_scope_key, this, [&](DashboardPage& peer) {
        peer.push_event("dataReset"sv, serialized);
    });
}

void DashboardPage::handle_items_list(i64 request_id, JsonObject const& params)
{
    auto collection = params.get_string("collection"sv).value_or(String {});
    if (collection == DashboardStore::IMAGE_SHADER_COLLECTION) {
        reply_error(request_id, "Use the image shader library."sv);
        return;
    }
    auto limit = params.get_integer<size_t>("limit"sv).value_or(DashboardStore::MAX_COLLECTION_ITEMS);
    auto items = store().list_items(m_scope_key, collection, limit);

    JsonArray arr;
    for (auto const& item : items) {
        JsonObject o;
        o.set("itemId"sv, item.item_id);
        o.set("revision"sv, item.revision);
        o.set("value"sv, item.value);
        o.set("updatedAt"sv, item.updated_at);
        arr.must_append(move(o));
    }
    JsonObject out;
    out.set("collection"sv, collection);
    out.set("items"sv, move(arr));
    reply_ok(request_id, move(out));
}

void DashboardPage::handle_items_save(i64 request_id, JsonObject const& params)
{
    auto collection = params.get_string("collection"sv).value_or(String {});
    if (collection == DashboardStore::IMAGE_SHADER_COLLECTION) {
        reply_error(request_id, "Use the image shader library."sv);
        return;
    }
    auto item_id = params.get_string("itemId"sv).value_or(String {});
    if (item_id.is_empty())
        item_id = generate_opaque_id("item"sv);
    Optional<i64> expected_revision;
    if (params.has("expectedRevision"sv) && !params.get("expectedRevision"sv)->is_null())
        expected_revision = params.get_integer<i64>("expectedRevision"sv);
    auto value = params.get_object("value"sv).value_or(JsonObject {});

    auto& st = store();
    auto res = st.save_item(m_scope_key, collection, item_id, expected_revision, value);
    if (!res.ok) {
        JsonObject extra;
        extra.set("conflict"sv, res.conflict);
        extra.set("currentRevision"sv, res.revision);
        if (res.current_value.has_value())
            extra.set("currentValue"sv, *res.current_value);
        extra.set("storageStatus"sv, st.storage_status_string());
        reply_error(request_id, res.error, move(extra));
        return;
    }

    JsonObject out;
    out.set("collection"sv, collection);
    out.set("itemId"sv, item_id);
    out.set("revision"sv, res.revision);
    out.set("storageStatus"sv, st.storage_status_string());
    reply_ok(request_id, out);

    JsonObject broadcast;
    broadcast.set("collection"sv, collection);
    broadcast.set("itemId"sv, item_id);
    broadcast.set("revision"sv, res.revision);
    broadcast.set("value"sv, value);
    coordinator().broadcast_to_scope(m_scope_key, this, [&](DashboardPage& peer) {
        peer.push_event("itemUpdated"sv, broadcast);
    });
}

void DashboardPage::handle_items_delete(i64 request_id, JsonObject const& params)
{
    auto collection = params.get_string("collection"sv).value_or(String {});
    if (collection == DashboardStore::IMAGE_SHADER_COLLECTION) {
        reply_error(request_id, "Use the image shader library."sv);
        return;
    }
    auto item_id = params.get_string("itemId"sv).value_or(String {});
    if (!store().delete_item(m_scope_key, collection, item_id)) {
        reply_error(request_id, "Couldn’t delete item."sv);
        return;
    }
    JsonObject out;
    out.set("collection"sv, collection);
    out.set("itemId"sv, item_id);
    reply_ok(request_id, out);

    coordinator().broadcast_to_scope(m_scope_key, this, [&](DashboardPage& peer) {
        peer.push_event("itemDeleted"sv, out);
    });
}

void DashboardPage::handle_items_clear_completed(i64 request_id, JsonObject const& params)
{
    auto collection = params.get_string("collection"sv).value_or("tasks"_string);
    if (collection == DashboardStore::IMAGE_SHADER_COLLECTION) {
        reply_error(request_id, "Use the image shader library."sv);
        return;
    }
    Vector<String> ids;
    if (auto arr = params.get_array("itemIds"sv); arr.has_value()) {
        for (size_t i = 0; i < arr->size(); ++i) {
            if (arr->at(i).is_string())
                ids.append(arr->at(i).as_string());
        }
    }
    auto deleted = store().delete_items_by_ids(m_scope_key, collection, ids);
    JsonObject out;
    out.set("collection"sv, collection);
    out.set("deletedCount"sv, static_cast<i64>(deleted));
    reply_ok(request_id, out);

    coordinator().broadcast_to_scope(m_scope_key, this, [&](DashboardPage& peer) {
        peer.push_event("collectionChanged"sv, out);
    });
}

void DashboardPage::handle_image_load(i64 request_id, JsonObject const& params)
{
    auto instance_id = params.get_string("instanceId"sv).value_or(String {});
    auto result = store().load_image(m_scope_key, instance_id);
    if (result.is_error()) {
        reply_error(request_id, "Couldn’t load the saved image."sv);
        return;
    }
    auto image = result.release_value();
    if (!image.has_value()) {
        JsonObject out;
        out.set("dataUrl"sv, JsonValue());
        reply_ok(request_id, move(out));
        return;
    }
    auto payload = dashboard_image_payload(image->mime_type, image->bytes.bytes());
    if (payload.is_error()) {
        reply_error(request_id, "Couldn’t load the saved image."sv);
        return;
    }
    reply_ok(request_id, payload.release_value());
}

void DashboardPage::handle_image_save(i64 request_id, JsonObject const& params)
{
    auto instance_id = params.get_string("instanceId"sv).value_or(String {});
    auto encoded = params.get_string("base64"sv).value_or(String {});
    constexpr size_t max_encoded_bytes = ((DashboardStore::MAX_IMAGE_BYTES + 2) / 3) * 4;
    if (encoded.is_empty() || encoded.bytes().size() > max_encoded_bytes) {
        reply_error(request_id, "Choose an image up to 8 MB."sv);
        return;
    }
    if (store().verify_image_widget(m_scope_key, instance_id).is_error()) {
        reply_error(request_id, "This image widget is no longer on the dashboard."sv);
        return;
    }
    auto bytes = decode_base64(encoded, AK::LastChunkHandling::Strict);
    if (bytes.is_error() || bytes.value().is_empty() || bytes.value().size() > DashboardStore::MAX_IMAGE_BYTES) {
        reply_error(request_id, "Couldn’t read the image. Choose a valid image up to 8 MB."sv);
        return;
    }
    auto mime_type = dashboard_image_type(bytes.value().bytes());
    if (mime_type.is_empty()) {
        reply_error(request_id, "Choose a PNG, JPEG, GIF, WebP, AVIF or BMP image."sv);
        return;
    }

    auto data_url = String::formatted("data:{};base64,{}", mime_type, encoded);
    if (data_url.is_error()) {
        reply_error(request_id, "Couldn’t prepare the image."sv);
        return;
    }
    auto saved = store().save_image(m_scope_key, instance_id, mime_type, bytes.value().bytes());
    if (saved.is_error()) {
        auto message = saved.error().string_literal();
        reply_error(request_id, message.is_empty() ? "Couldn’t save the image."sv : message);
        return;
    }
    JsonObject out;
    out.set("dataUrl"sv, data_url.release_value());
    reply_ok(request_id, move(out));
    broadcast_image_change(instance_id);
}

void DashboardPage::handle_image_remove(i64 request_id, JsonObject const& params)
{
    auto instance_id = params.get_string("instanceId"sv).value_or(String {});
    if (store().remove_image(m_scope_key, instance_id).is_error()) {
        reply_error(request_id, "Couldn’t delete the saved image."sv);
        return;
    }
    reply_ok(request_id);
    broadcast_image_change(instance_id);
}

void DashboardPage::broadcast_image_change(String const& instance_id)
{
    JsonObject event;
    event.set("instanceId"sv, instance_id);
    coordinator().broadcast_to_scope(m_scope_key, this, [&](DashboardPage& peer) {
        peer.push_event("imageChanged"sv, event);
    });
}

void DashboardPage::handle_image_fetch_url(i64 request_id, JsonObject const& params)
{
    if (!allows_network_widgets()) {
        reply_error(request_id, "Import an image from a file in this session."sv);
        return;
    }
    if (Application::the().privacy_network_mode() == Privacy::NetworkMode::Offline) {
        reply_error(request_id, "The browser is in Offline mode."sv);
        return;
    }
    if (!consume_user_activation()) {
        reply_error(request_id, "Click Import to fetch the image."sv);
        return;
    }

    auto instance_id = params.get_string("instanceId"sv).value_or(String {});
    if (store().verify_image_widget(m_scope_key, instance_id).is_error()) {
        reply_error(request_id, "This image widget is no longer on the dashboard."sv);
        return;
    }
    auto url = params.get_string("url"sv).value_or(String {});
    auto parsed = url.bytes().size() <= 4096 ? URL::Parser::basic_parse(url) : Optional<URL::URL> {};
    if (!parsed.has_value() || !is_public_https_url(*parsed)) {
        reply_error(request_id, "Enter a public HTTPS image URL."sv);
        return;
    }
    auto& active_count = coordinator().active_fetches_per_scope.ensure(m_scope_key);
    if (active_count >= 2) {
        reply_error(request_id, "Too many dashboard requests. Try again shortly."sv);
        return;
    }
    auto* hub = HubConnection::the_if_connected();
    if (!hub) {
        reply_error(request_id, "Browser hub is unavailable."sv);
        return;
    }

    ++active_count;
    auto weak_this = make_weak_ptr();
    auto scope_key = m_scope_key;
    auto hub_request_id = hub->fetch_dashboard_resource(web_ui_id(), "image"_string, parsed->serialize(), 0,
        [weak_this, request_id, scope_key, instance_id = move(instance_id)](HubConnection::RequestResponse response) {
            auto& count = coordinator().active_fetches_per_scope.ensure(scope_key);
            if (count > 0)
                --count;
            if (!weak_this || weak_this->m_is_closing)
                return;
            weak_this->m_in_flight_requests.remove_all_matching([&](u64, u64 web_request_id) {
                return web_request_id == static_cast<u64>(request_id);
            });
            if (response.had_network_error || !response.response_code.has_value() || *response.response_code < 200 || *response.response_code >= 300) {
                weak_this->reply_error(request_id, "Couldn’t download the image. Use a direct HTTPS link to an image up to 8 MB."sv);
                return;
            }
            if (weak_this->store().verify_image_widget(scope_key, instance_id).is_error()) {
                weak_this->reply_error(request_id, "This image widget is no longer on the dashboard."sv);
                return;
            }
            auto bytes = response.body.bytes();
            auto mime_type = bytes.size() <= DashboardStore::MAX_IMAGE_BYTES ? dashboard_image_type(bytes) : String {};
            if (mime_type.is_empty()) {
                weak_this->reply_error(request_id, "The URL must lead to a PNG, JPEG, GIF, WebP, AVIF or BMP image up to 8 MB."sv);
                return;
            }
            auto payload = dashboard_image_payload(mime_type, bytes);
            if (payload.is_error()) {
                weak_this->reply_error(request_id, "Couldn’t prepare the image."sv);
                return;
            }
            weak_this->reply_ok(request_id, payload.release_value());
        });
    m_in_flight_requests.set(hub_request_id, static_cast<u64>(request_id));
}

void DashboardPage::handle_shaders_list(i64 request_id)
{
    auto collection = MUST(String::from_utf8(DashboardStore::IMAGE_SHADER_COLLECTION));
    auto items = store().list_items(m_scope_key, collection, DashboardStore::MAX_SHADER_FILES);
    JsonArray shaders;
    for (auto const& item : items) {
        auto name = item.value.get_string("name"sv);
        if (!name.has_value())
            continue;
        JsonObject shader;
        shader.set("id"sv, item.item_id);
        shader.set("name"sv, name.release_value());
        shaders.must_append(move(shader));
    }

    JsonObject out;
    out.set("shaders"sv, move(shaders));
    reply_ok(request_id, move(out));
}

void DashboardPage::handle_shader_load(i64 request_id, JsonObject const& params)
{
    auto shader_id = params.get_string("id"sv).value_or(String {});
    Optional<String> source;
    if (shader_id.bytes_as_string_view().starts_with("shader-"sv)) {
        auto collection = MUST(String::from_utf8(DashboardStore::IMAGE_SHADER_COLLECTION));
        if (auto item = store().get_item(m_scope_key, collection, shader_id); item.has_value()) {
            if (auto saved_source = item->value.get_string("source"sv); saved_source.has_value())
                source = *saved_source;
        }
    }

    JsonObject out;
    out.set("source"sv, source.has_value() ? JsonValue { source.release_value() } : JsonValue {});
    reply_ok(request_id, move(out));
}

void DashboardPage::handle_shader_save(i64 request_id, JsonObject const& params)
{
    auto name = params.get_string("name"sv).value_or(String {});
    auto source = params.get_string("source"sv).value_or(String {});
    JsonObject value;
    value.set("name"sv, name);
    value.set("source"sv, move(source));

    auto collection = MUST(String::from_utf8(DashboardStore::IMAGE_SHADER_COLLECTION));
    auto shader_id = generate_opaque_id("shader"sv);
    auto result = store().save_item(m_scope_key, collection, shader_id, 0, value);
    if (!result.ok) {
        reply_error(request_id, result.error);
        return;
    }

    JsonObject out;
    out.set("id"sv, shader_id);
    out.set("name"sv, move(name));
    reply_ok(request_id, move(out));
    broadcast_shader_change(shader_id);
}

void DashboardPage::handle_shader_remove(i64 request_id, JsonObject const& params)
{
    auto shader_id = params.get_string("id"sv).value_or(String {});
    auto collection = MUST(String::from_utf8(DashboardStore::IMAGE_SHADER_COLLECTION));
    if (!shader_id.bytes_as_string_view().starts_with("shader-"sv) || !store().delete_item(m_scope_key, collection, shader_id)) {
        reply_error(request_id, "Couldn’t remove the saved shader."sv);
        return;
    }

    reply_ok(request_id);
    broadcast_shader_change(shader_id);
}

void DashboardPage::broadcast_shader_change(String const& shader_id)
{
    JsonObject event;
    event.set("id"sv, shader_id);
    coordinator().broadcast_to_scope(m_scope_key, nullptr, [&](DashboardPage& peer) {
        peer.push_event("shadersChanged"sv, event);
    });
}

void DashboardPage::handle_top_sites_load(i64 request_id)
{
    auto& st = store();
    auto pins = st.list_items(m_scope_key, "top-sites-pins"_string, 24);
    auto dismissed_items = st.list_items(m_scope_key, "top-sites-dismissed"_string, 100);
    HashTable<String> dismissed_urls;
    for (auto const& d : dismissed_items) {
        if (auto u = d.value.get_string("url"sv); u.has_value())
            dismissed_urls.set(*u);
    }

    JsonArray pin_arr;
    HashTable<String> pinned_urls;
    for (auto const& p : pins) {
        auto url = p.value.get_string("url"sv).value_or(String {});
        if (url.is_empty())
            continue;
        pinned_urls.set(url);
        JsonObject o;
        o.set("itemId"sv, p.item_id);
        o.set("url"sv, url);
        o.set("title"sv, p.value.get_string("title"sv).value_or(url));
        o.set("pinned"sv, true);
        pin_arr.must_append(move(o));
    }

    JsonArray inferred_arr;
    if (!is_ephemeral_scope() && m_context.persists_history) {
        auto& history = Application::history_store_for_identity(m_context.identity_id, m_context.persists_history);
        for (auto const& entry : history.top_sites(16)) {
            if (pinned_urls.contains(entry.url) || dismissed_urls.contains(entry.url))
                continue;
            String favicon_b64;
            if (entry.favicon_png.has_value()) {
                if (auto enc = encode_base64(entry.favicon_png->bytes()); !enc.is_error())
                    favicon_b64 = enc.release_value();
            }
            JsonObject o;
            o.set("url"sv, entry.url);
            o.set("title"sv, entry.title.value_or(entry.url));
            o.set("faviconBase64Png"sv, move(favicon_b64));
            o.set("visitCount"sv, entry.visit_count);
            o.set("pinned"sv, false);
            inferred_arr.must_append(move(o));
        }
    }

    JsonObject out;
    out.set("pins"sv, move(pin_arr));
    out.set("inferred"sv, move(inferred_arr));
    out.set("historyAvailable"sv, !is_ephemeral_scope() && m_context.persists_history);
    reply_ok(request_id, move(out));
}

void DashboardPage::handle_search_engines(i64 request_id)
{
    auto const& settings = Application::settings();
    JsonArray engines;
    for (auto const& e : WebView::builtin_search_engines()) {
        JsonObject o;
        o.set("name"sv, e.name);
        engines.must_append(move(o));
    }
    for (auto const& e : settings.custom_search_engines()) {
        JsonObject o;
        o.set("name"sv, e.name);
        o.set("custom"sv, true);
        engines.must_append(move(o));
    }
    JsonObject out;
    if (auto current = settings.search_engine_settings().engine; current.has_value())
        out.set("defaultEngine"sv, current->name);
    else
        out.set("defaultEngine"sv, JsonValue {});
    out.set("engines"sv, move(engines));
    reply_ok(request_id, move(out));
}

void DashboardPage::handle_search_submit(i64 request_id, JsonObject const& params)
{
    if (!consume_user_activation()) {
        reply_error(request_id, "User interaction is required."sv);
        return;
    }
    auto query = params.get_string("query"sv).value_or(String {});
    auto trimmed = query.bytes_as_string_view().trim_whitespace();
    if (trimmed.is_empty()) {
        reply_error(request_id, "Enter a search query."sv);
        return;
    }

    auto const& settings = Application::settings();
    Optional<WebView::SearchEngine> engine;
    if (auto requested = params.get_string("engineName"sv); requested.has_value() && !requested->is_empty())
        engine = settings.find_search_engine_by_name(*requested);
    if (!engine.has_value())
        engine = settings.search_engine_settings().engine;
    if (!engine.has_value()) {
        reply_error(request_id, "No search engine configured."sv);
        return;
    }

    auto nav_str = engine->format_search_query_for_navigation(trimmed);
    auto parsed = URL::Parser::basic_parse(nav_str);
    if (!parsed.has_value() || !is_safe_http_url(*parsed)) {
        reply_error(request_id, "Invalid search URL."sv);
        return;
    }

    if (auto* tab = owning_tab()) {
        tab->navigate(*parsed);
        reply_ok(request_id, JsonObject {});
        return;
    }
    reply_error(request_id, "Tab is no longer attached."sv);
}

// A tab's favicon, as the base64 PNG the board shows in place of a site's initial.
static String favicon_base64_png(QIcon const& icon)
{
    if (icon.isNull())
        return {};
    auto pixmap = icon.pixmap(QSize(32, 32));
    if (pixmap.isNull())
        return {};
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly) || !pixmap.save(&buffer, "PNG"))
        return {};
    auto encoded = encode_base64({ reinterpret_cast<u8 const*>(bytes.constData()), static_cast<size_t>(bytes.size()) });
    return encoded.is_error() ? String {} : encoded.release_value();
}

static String favicon_base64_png(Optional<ByteBuffer> const& png)
{
    if (!png.has_value() || png->is_empty())
        return {};
    auto encoded = encode_base64(png->bytes());
    return encoded.is_error() ? String {} : encoded.release_value();
}

// NB: A URL such as about:blank has no host, and URL::serialized_host() must not be asked for one.
static StringView host_of(URL::URL const& url)
{
    return url.host().has_value() ? url.serialized_host() : StringView {};
}

// The favicon history keeps for a page, or else for the root of its site, or else for any page of the same host.
static String favicon_from_history(WebView::HistoryStore& history, URL::URL const& url)
{
    if (auto entry = history.entry_for_url(url); entry.has_value()) {
        if (auto b64 = favicon_base64_png(entry->favicon_png); !b64.is_empty())
            return b64;
    }
    auto host = host_of(url);
    if (host.is_empty())
        return {};
    if (auto root = URL::Parser::basic_parse(MUST(String::formatted("{}://{}/", url.scheme(), host))); root.has_value()) {
        if (auto entry = history.entry_for_url(*root); entry.has_value()) {
            if (auto b64 = favicon_base64_png(entry->favicon_png); !b64.is_empty())
                return b64;
        }
    }
    for (auto const& entry : history.list_entries(host, 0, 12)) {
        if (!entry.favicon_png.has_value())
            continue;
        auto entry_url = URL::Parser::basic_parse(entry.url);
        if (entry_url.has_value() && host_of(*entry_url) == host)
            return favicon_base64_png(entry.favicon_png);
    }
    return {};
}

static constexpr size_t MAX_FAVICON_LOOKUPS = 48;

// Finds a logo for each http(s) URL a widget shows, from what this board's own profile already knows: its history, and
// the open tabs of its session. Nothing is fetched, and a URL with no known favicon is left out, so the widget keeps
// its initial for it.
void DashboardPage::handle_favicons_lookup(i64 request_id, JsonObject const& params)
{
    JsonObject icons;
    auto urls = params.get_array("urls"sv);

    WebView::HistoryStore* history = nullptr;
    if (!is_ephemeral_scope() && m_context.persists_history)
        history = &Application::history_store_for_identity(m_context.identity_id, m_context.persists_history);

    Optional<HashMap<String, String>> tab_icons_by_host;
    auto tab_icon_for_host = [&](String const& host) -> String {
        if (!tab_icons_by_host.has_value()) {
            tab_icons_by_host = HashMap<String, String> {};
            for (auto* widget : QApplication::topLevelWidgets()) {
                auto* win = as_if<BrowserWindow>(widget);
                if (!win || win->is_private() != m_context.is_private)
                    continue;
                for (int i = 0; i < win->tab_count(); ++i) {
                    auto* tab = win->tab_at(i);
                    if (!tab || tab->favicon().isNull())
                        continue;
                    if (tab->is_loaded()) {
                        auto const& tab_id = tab->view().identity_id();
                        if (!m_context.identity_id.is_empty() && !tab_id.is_empty() && tab_id != m_context.identity_id)
                            continue;
                    }
                    auto tab_host = MUST(String::from_utf8(host_of(tab->url())));
                    if (tab_host.is_empty() || tab_icons_by_host->contains(tab_host))
                        continue;
                    if (auto b64 = favicon_base64_png(tab->favicon()); !b64.is_empty())
                        tab_icons_by_host->set(tab_host, move(b64));
                }
            }
        }
        return tab_icons_by_host->get(host).value_or(String {});
    };

    if (urls.has_value()) {
        for (size_t i = 0; i < min(urls->size(), MAX_FAVICON_LOOKUPS); ++i) {
            if (!urls->at(i).is_string())
                continue;
            auto const& raw = urls->at(i).as_string();
            if (raw.bytes().size() > 2048 || icons.has(raw))
                continue;
            auto url = URL::Parser::basic_parse(raw);
            if (!url.has_value() || !url->scheme().is_one_of("http"sv, "https"sv))
                continue;
            String b64;
            if (history)
                b64 = favicon_from_history(*history, *url);
            if (b64.is_empty())
                if (auto host = host_of(*url); !host.is_empty())
                    b64 = tab_icon_for_host(MUST(String::from_utf8(host)));
            if (!b64.is_empty())
                icons.set(raw, move(b64));
        }
    }

    JsonObject out;
    out.set("icons"sv, move(icons));
    reply_ok(request_id, move(out));
}

void DashboardPage::handle_tabs_list(i64 request_id)
{
    auto* caller_window = owning_window();
    m_tab_handles.clear();

    JsonArray windows_arr;
    for (auto* widget : QApplication::topLevelWidgets()) {
        auto* win = as_if<BrowserWindow>(widget);
        if (!win)
            continue;
        if (win->is_private() != m_context.is_private)
            continue;

        JsonArray tabs_arr;
        for (int i = 0; i < win->tab_count(); ++i) {
            auto* tab = win->tab_at(i);
            if (!tab)
                continue;
            // Scope check for loaded tabs with named identities.
            if (tab->is_loaded()) {
                auto const& tab_id = tab->view().identity_id();
                if (!m_context.identity_id.is_empty() && !tab_id.is_empty() && tab_id != m_context.identity_id)
                    continue;
            }
            u64 target_view_id = tab->is_loaded() ? tab->view().view_id() : 0;
            String handle = target_view_id != 0
                ? MUST(String::formatted("tab:{}:{}", m_handle_nonce, target_view_id))
                : MUST(String::formatted("deferred:{}:{}", m_handle_nonce, reinterpret_cast<uintptr_t>(tab)));
            if (target_view_id != 0)
                m_tab_handles.set(handle, target_view_id);

            JsonObject to;
            to.set("handle"sv, handle);
            to.set("title"sv, ak_string_from_qstring(tab->title()));
            to.set("url"sv, tab->url().serialize());
            to.set("isCurrent"sv, win->current_tab() == tab);
            // NB: The other side of a shown split view is on display too, and the two sides of a pair share an ID.
            to.set("isVisible"sv, win->tab_widget().page_area().is_displayed(tab) && win->isVisible() && !win->isMinimized());
            if (auto const* pair = win->tab_widget().split_pair_of(tab))
                to.set("splitViewId"sv, static_cast<i64>(pair->id));
            else
                to.set("splitViewId"sv, -1);
            to.set("isSelf"sv, target_view_id != 0 && target_view_id == m_context.view_id);
            to.set("isPinned"sv, tab->is_pinned());
            to.set("isLoaded"sv, tab->is_loaded());
            to.set("isAudible"sv, tab->is_loaded() && tab->view().tab_indicator_visible(WebView::TabIndicator::PlayingAudio));
            to.set("isMuted"sv, tab->is_loaded() && tab->view().page_mute_state() == Web::HTML::MuteState::Muted);
            to.set("faviconBase64Png"sv, favicon_base64_png(tab->favicon()));
            tabs_arr.must_append(move(to));
        }

        if (!tabs_arr.is_empty()) {
            JsonObject wo;
            wo.set("isCurrentWindow"sv, win == caller_window);
            wo.set("tabCount"sv, static_cast<i64>(tabs_arr.size()));
            wo.set("tabs"sv, move(tabs_arr));
            windows_arr.must_append(move(wo));
        }
    }

    JsonObject out;
    out.set("windows"sv, move(windows_arr));
    reply_ok(request_id, move(out));
}

void DashboardPage::handle_tabs_action(i64 request_id, StringView action, JsonObject const& params)
{
    auto handle = params.get_string("handle"sv).value_or(String {});
    auto maybe_view_id = m_tab_handles.get(handle);
    if (!maybe_view_id.has_value()) {
        JsonObject extra;
        extra.set("stale"sv, true);
        reply_error(request_id, "Tab is no longer open."sv, move(extra));
        return;
    }

    auto* target_win = Application::the().window_of_view_id(*maybe_view_id);
    if (!target_win || target_win->is_private() != m_context.is_private) {
        JsonObject extra;
        extra.set("stale"sv, true);
        reply_error(request_id, "Tab is no longer available in this session."sv, move(extra));
        return;
    }

    Tab* target_tab = nullptr;
    for (int i = 0; i < target_win->tab_count(); ++i) {
        auto* tab = target_win->tab_at(i);
        if (tab && tab->is_loaded() && tab->view().view_id() == *maybe_view_id) {
            target_tab = tab;
            break;
        }
    }
    if (!target_tab) {
        JsonObject extra;
        extra.set("stale"sv, true);
        reply_error(request_id, "Tab is no longer open."sv, move(extra));
        return;
    }
    if (!consume_user_activation()) {
        reply_error(request_id, "User interaction is required."sv);
        return;
    }

    if (action == "focus"sv) {
        int idx = target_win->tab_index(target_tab);
        if (idx >= 0) {
            target_win->tab_widget().set_current_index(idx);
            target_win->bring_to_front();
        }
    } else if (action == "mute"sv) {
        target_tab->view().toggle_page_mute_state();
    } else if (action == "close"sv) {
        target_tab->request_close();
    }
    reply_ok(request_id, JsonObject {});
}

void DashboardPage::handle_recently_closed_list(i64 request_id)
{
    JsonObject out;
    JsonArray items;
    if (!is_persistent_default_scope()) {
        out.set("available"sv, false);
        out.set("items"sv, move(items));
        reply_ok(request_id, move(out));
        return;
    }

    out.set("available"sv, true);
    auto& session = Application::session_store(WebView::IsPrivate::No);
    for (auto const& unit : session.recently_closed_units(20)) {
        if (!SessionStore::is_tab_worth_keeping(unit.active_url))
            continue;
        JsonObject o;
        o.set("unitId"sv, unit.id);
        o.set("url"sv, unit.active_url.serialize());
        o.set("tabCount"sv, static_cast<i64>(unit.tab_count));
        o.set("wasWindow"sv, unit.was_window);
        o.set("fromStartupRecovery"sv, unit.from_startup_recovery);
        o.set("closedAt"sv, unit.closed_at.milliseconds_since_epoch());
        items.must_append(move(o));
    }
    out.set("items"sv, move(items));
    reply_ok(request_id, move(out));
}

void DashboardPage::handle_recently_closed_reopen(i64 request_id, JsonObject const& params)
{
    if (!is_persistent_default_scope()) {
        reply_error(request_id, "Recently closed tabs are unavailable in this session."sv);
        return;
    }
    auto unit_id = params.get_integer<SessionWindowId>("unitId"sv);
    if (!unit_id.has_value()) {
        reply_error(request_id, "Missing closed unit ID."sv);
        return;
    }
    if (!consume_user_activation()) {
        reply_error(request_id, "User interaction is required."sv);
        return;
    }

    auto& session = Application::session_store(WebView::IsPrivate::No);
    bool exists = false;
    for (auto const& u : session.recently_closed_units(50)) {
        if (u.id == *unit_id) {
            exists = true;
            break;
        }
    }
    if (!exists) {
        JsonObject extra;
        extra.set("stale"sv, true);
        reply_error(request_id, "That item was already reopened."sv, move(extra));
        return;
    }

    Application::the().reopen_recently_closed_tab(owning_window(), *unit_id, Application::ReopenClosedWindow::InNewWindow);
    notify_recently_closed_changed();
    reply_ok(request_id, JsonObject {});
}

static void collect_bookmark_folders(Vector<BookmarkItem> const& items, String const& prefix, JsonArray& out)
{
    for (auto const& item : items) {
        if (!item.is_folder())
            continue;
        auto title = item.folder().title.value_or("Untitled folder"_string);
        auto path_label = prefix.is_empty() ? title : MUST(String::formatted("{} / {}", prefix, title));
        JsonObject fo;
        fo.set("id"sv, item.id);
        fo.set("title"sv, title);
        fo.set("pathLabel"sv, path_label);
        fo.set("childCount"sv, static_cast<i64>(item.folder().children.size()));
        out.must_append(move(fo));
        collect_bookmark_folders(item.folder().children, path_label, out);
    }
}

void DashboardPage::handle_bookmarks_folders(i64 request_id)
{
    JsonObject out;
    JsonArray folders;
    if (!is_persistent_default_scope()) {
        out.set("available"sv, false);
        out.set("folders"sv, move(folders));
        reply_ok(request_id, move(out));
        return;
    }
    out.set("available"sv, true);
    JsonObject root;
    root.set("id"sv, "root"_string);
    root.set("title"sv, "Bookmarks Bar"_string);
    root.set("pathLabel"sv, "Bookmarks Bar"_string);
    root.set("childCount"sv, static_cast<i64>(Application::bookmark_store().root_items().size()));
    folders.must_append(move(root));
    collect_bookmark_folders(Application::bookmark_store().root_items(), ""_string, folders);
    out.set("folders"sv, move(folders));
    reply_ok(request_id, move(out));
}

void DashboardPage::handle_bookmarks_folder(i64 request_id, JsonObject const& params)
{
    JsonObject out;
    if (!is_persistent_default_scope()) {
        out.set("available"sv, false);
        reply_ok(request_id, move(out));
        return;
    }
    auto folder_id = params.get_string("folderId"sv).value_or("root"_string);
    auto limit = min(params.get_integer<size_t>("limit"sv).value_or(60), 100uz);

    auto& bs = Application::bookmark_store();
    Vector<BookmarkItem> const* children = nullptr;
    String folder_title = "Bookmarks Bar"_string;

    if (folder_id == "root"sv || folder_id.is_empty()) {
        children = &bs.root_items();
    } else if (auto item = bs.find_item_by_id(folder_id); item.has_value() && item->is_folder()) {
        children = &item->folder().children;
        folder_title = item->folder().title.value_or("Untitled folder"_string);
    } else {
        out.set("available"sv, true);
        out.set("missing"sv, true);
        out.set("folderId"sv, folder_id);
        out.set("items"sv, JsonArray {});
        reply_ok(request_id, move(out));
        return;
    }

    JsonArray arr;
    for (size_t i = 0; i < children->size() && arr.size() < limit; ++i) {
        auto const& child = children->at(i);
        JsonObject co;
        co.set("id"sv, child.id);
        if (child.is_folder()) {
            co.set("type"sv, "folder"_string);
            co.set("title"sv, child.folder().title.value_or("Untitled folder"_string));
            co.set("childCount"sv, static_cast<i64>(child.folder().children.size()));
        } else {
            auto const& bm = child.bookmark();
            co.set("type"sv, "bookmark"_string);
            co.set("url"sv, bm.url.serialize());
            co.set("title"sv, bm.title.value_or(bm.url.serialize()));
            if (bm.favicon_hash.has_value()) {
                if (auto png = bs.favicon_png(*bm.favicon_hash); png.has_value()) {
                    if (auto b64 = encode_base64(png->bytes()); !b64.is_error())
                        co.set("faviconBase64Png"sv, b64.release_value());
                }
            }
        }
        arr.must_append(move(co));
    }

    out.set("available"sv, true);
    out.set("missing"sv, false);
    out.set("folderId"sv, folder_id);
    out.set("folderTitle"sv, folder_title);
    out.set("items"sv, move(arr));
    reply_ok(request_id, move(out));
}

void DashboardPage::handle_history_list(i64 request_id, JsonObject const& params)
{
    JsonObject out;
    JsonArray arr;
    if (is_ephemeral_scope() || !m_context.persists_history) {
        out.set("available"sv, false);
        out.set("entries"sv, move(arr));
        out.set("hasMore"sv, false);
        reply_ok(request_id, move(out));
        return;
    }

    auto query = params.get_string("query"sv).value_or(String {});
    auto offset = params.get_integer<size_t>("offset"sv).value_or(0);
    auto limit = min(params.get_integer<size_t>("limit"sv).value_or(20), 50uz);

    auto& history = Application::history_store_for_identity(m_context.identity_id, m_context.persists_history);
    auto entries = history.list_entries(query, offset, limit + 1);
    bool has_more = entries.size() > limit;
    if (has_more)
        entries.resize(limit);

    for (auto const& e : entries) {
        String favicon_b64;
        if (e.favicon_png.has_value()) {
            if (auto enc = encode_base64(e.favicon_png->bytes()); !enc.is_error())
                favicon_b64 = enc.release_value();
        }
        JsonObject o;
        o.set("url"sv, e.url);
        o.set("title"sv, e.title.value_or(e.url));
        o.set("faviconBase64Png"sv, move(favicon_b64));
        o.set("visitCount"sv, e.visit_count);
        o.set("lastVisitedTime"sv, e.last_visited_time.milliseconds_since_epoch());
        arr.must_append(move(o));
    }

    out.set("available"sv, true);
    out.set("entries"sv, move(arr));
    out.set("hasMore"sv, has_more);
    reply_ok(request_id, move(out));
}

void DashboardPage::handle_history_remove(i64 request_id, JsonObject const& params)
{
    if (is_ephemeral_scope() || !m_context.persists_history) {
        reply_error(request_id, "History is unavailable in this session."sv);
        return;
    }
    if (!consume_user_activation()) {
        reply_error(request_id, "User interaction is required."sv);
        return;
    }
    auto url_str = params.get_string("url"sv).value_or(String {});
    auto parsed = URL::Parser::basic_parse(url_str);
    if (!parsed.has_value()) {
        reply_error(request_id, "Invalid URL."sv);
        return;
    }
    auto remove_engagements = (is_persistent_default_scope() && Application::bookmark_store().is_bookmarked(*parsed))
        ? WebView::RemoveHistoryEntryEngagements::No
        : WebView::RemoveHistoryEntryEngagements::Yes;
    Application::history_store_for_identity(m_context.identity_id, m_context.persists_history).remove_entry_for_url(*parsed, remove_engagements);
    reply_ok(request_id, JsonObject {});
}

void DashboardPage::handle_workspaces_save_window(i64 request_id, JsonObject const& params)
{
    auto* win = owning_window();
    if (!win) {
        reply_error(request_id, "Window is unavailable."sv);
        return;
    }
    auto name = params.get_string("name"sv).value_or("Saved workspace"_string);
    auto preview_only = params.get_bool("previewOnly"sv).value_or(false);

    JsonArray tabs_arr;
    size_t omitted = 0;
    for (int i = 0; i < win->tab_count(); ++i) {
        auto* tab = win->tab_at(i);
        if (!tab)
            continue;
        auto url = tab->url();
        if (!is_safe_http_url(url)) {
            ++omitted;
            continue;
        }
        if (tabs_arr.size() >= 30) {
            ++omitted;
            continue;
        }
        JsonObject t;
        t.set("url"sv, url.serialize());
        t.set("title"sv, ak_string_from_qstring(tab->title()));
        t.set("pinned"sv, tab->is_pinned());
        tabs_arr.must_append(move(t));
    }

    if (preview_only) {
        JsonObject out;
        out.set("eligibleCount"sv, static_cast<i64>(tabs_arr.size()));
        out.set("omittedCount"sv, static_cast<i64>(omitted));
        out.set("tabs"sv, move(tabs_arr));
        reply_ok(request_id, move(out));
        return;
    }

    if (tabs_arr.is_empty()) {
        reply_error(request_id, "No eligible web tabs to save in this window."sv);
        return;
    }

    auto ws_id = generate_opaque_id("ws"sv);
    JsonObject val;
    val.set("name"sv, name);
    val.set("tabs"sv, move(tabs_arr));
    val.set("createdAt"sv, UnixDateTime::now().milliseconds_since_epoch());
    auto res = store().save_item(m_scope_key, "workspaces"_string, ws_id, {}, val);
    if (!res.ok) {
        reply_error(request_id, res.error);
        return;
    }

    JsonObject out;
    out.set("itemId"sv, ws_id);
    out.set("omittedCount"sv, static_cast<i64>(omitted));
    reply_ok(request_id, move(out));

    JsonObject ev;
    ev.set("collection"sv, "workspaces"_string);
    coordinator().broadcast_to_scope(m_scope_key, this, [&](DashboardPage& peer) {
        peer.push_event("collectionChanged"sv, ev);
    });
}

void DashboardPage::handle_workspaces_open(i64 request_id, JsonObject const& params)
{
    if (!consume_user_activation()) {
        reply_error(request_id, "User interaction is required."sv);
        return;
    }
    auto* win = owning_window();
    if (!win) {
        reply_error(request_id, "Window is unavailable."sv);
        return;
    }
    auto item_id = params.get_string("itemId"sv).value_or(String {});
    auto item = store().get_item(m_scope_key, "workspaces"_string, item_id);
    if (!item.has_value()) {
        reply_error(request_id, "Workspace not found."sv);
        return;
    }

    auto tabs = item->value.get_array("tabs"sv);
    if (!tabs.has_value() || tabs->is_empty()) {
        reply_error(request_id, "Workspace has no tabs."sv);
        return;
    }

    size_t opened = 0;
    size_t skipped = 0;
    size_t cap = min(tabs->size(), 30uz);
    for (size_t i = 0; i < cap; ++i) {
        if (!tabs->at(i).is_object())
            continue;
        auto const& t = tabs->at(i).as_object();
        auto url_str = t.get_string("url"sv).value_or(String {});
        auto parsed = URL::Parser::basic_parse(url_str);
        if (!parsed.has_value() || !is_safe_http_url(*parsed)) {
            ++skipped;
            continue;
        }
        auto& new_tab = win->new_tab_from_url(*parsed, opened == 0 ? Web::HTML::ActivateTab::Yes : Web::HTML::ActivateTab::No, BrowserWindow::TabLocation::end());
        if (t.get_bool("pinned"sv).value_or(false))
            win->pin_tab(new_tab);
        ++opened;
    }

    JsonObject out;
    out.set("opened"sv, static_cast<i64>(opened));
    out.set("skipped"sv, static_cast<i64>(skipped));
    reply_ok(request_id, move(out));
}

static StringView download_status_string(WebView::FileDownloader::DownloadStatus status)
{
    switch (status) {
    case WebView::FileDownloader::DownloadStatus::InProgress:
        return "in-progress"sv;
    case WebView::FileDownloader::DownloadStatus::Paused:
        return "paused"sv;
    case WebView::FileDownloader::DownloadStatus::Completed:
        return "completed"sv;
    case WebView::FileDownloader::DownloadStatus::Canceled:
        return "canceled"sv;
    case WebView::FileDownloader::DownloadStatus::Failed:
        return "failed"sv;
    }
    return "failed"sv;
}

void DashboardPage::handle_downloads_list(i64 request_id)
{
    JsonObject out;
    JsonArray items;
    if (is_ephemeral_scope()) {
        out.set("available"sv, false);
        out.set("downloads"sv, move(items));
        reply_ok(request_id, move(out));
        return;
    }

    String expected_identity = is_persistent_default_scope() ? String {} : m_context.identity_id;
    out.set("available"sv, true);
    for (auto const& dl : RemoteDownloads::the().downloads_of_session(m_context.is_private)) {
        if (dl.identity_id != expected_identity)
            continue;
        JsonObject o;
        o.set("id"sv, dl.id);
        o.set("fileName"sv, String::from_utf8_with_replacement_character(dl.destination.basename(), String::WithBOMHandling::No));
        o.set("status"sv, download_status_string(dl.status));
        o.set("downloadedSize"sv, dl.downloaded_size);
        o.set("totalSize"sv, dl.total_size.has_value() ? JsonValue { *dl.total_size } : JsonValue {});
        o.set("canResume"sv, dl.can_resume);
        o.set("bytesPerSecond"sv, dl.bytes_per_second.has_value() ? JsonValue { *dl.bytes_per_second } : JsonValue {});
        o.set("createdTime"sv, dl.created_time.milliseconds_since_epoch());
        items.must_append(move(o));
    }
    out.set("downloads"sv, move(items));
    reply_ok(request_id, move(out));
}

void DashboardPage::handle_downloads_action(i64 request_id, JsonObject const& params)
{
    if (is_ephemeral_scope()) {
        reply_error(request_id, "Downloads are unavailable in this session."sv);
        return;
    }
    auto id = params.get_u64("id"sv);
    auto action = params.get_string("action"sv).value_or(String {});
    if (!id.has_value()) {
        reply_error(request_id, "Missing download ID."sv);
        return;
    }
    auto dl = RemoteDownloads::the().download(*id);
    String expected_identity = is_persistent_default_scope() ? String {} : m_context.identity_id;
    if (!dl.has_value() || dl->is_private != m_context.is_private || dl->identity_id != expected_identity) {
        JsonObject extra;
        extra.set("stale"sv, true);
        reply_error(request_id, "Download is no longer available."sv, move(extra));
        return;
    }
    if (!consume_user_activation()) {
        reply_error(request_id, "User interaction is required."sv);
        return;
    }

    if (action == "pause"sv && dl->status == WebView::FileDownloader::DownloadStatus::InProgress && dl->can_resume)
        RemoteDownloads::the().pause_download(*id);
    else if (action == "resume"sv && dl->status == WebView::FileDownloader::DownloadStatus::Paused && dl->can_resume)
        RemoteDownloads::the().resume_download(*id);
    else if (action == "cancel"sv && WebView::FileDownloader::status_is_active(dl->status))
        RemoteDownloads::the().cancel_download(*id);
    else if (action == "reveal"sv && dl->status == WebView::FileDownloader::DownloadStatus::Completed)
        RemoteDownloads::the().show_in_folder(*id);
    else {
        reply_error(request_id, "Action is not valid for this download state."sv);
        return;
    }
    reply_ok(request_id, JsonObject {});
}

void DashboardPage::handle_notes_export(i64 request_id, JsonObject const& params)
{
    if (!consume_user_activation()) {
        reply_error(request_id, "User interaction is required."sv);
        return;
    }
    auto title = params.get_string("title"sv).value_or("note"_string);
    auto content = params.get_string("content"sv).value_or(String {});
    if (content.bytes().size() > DashboardStore::MAX_NOTE_BYTES) {
        reply_error(request_id, "Note exceeds export size limit."sv);
        return;
    }

    QString suggested = qstring_from_ak_string(title);
    suggested.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._ -]")), QStringLiteral("_"));
    if (!suggested.endsWith(QStringLiteral(".md"), Qt::CaseInsensitive))
        suggested += QStringLiteral(".md");

    auto weak_this = make_weak_ptr();
    auto path = QFileDialog::getSaveFileName(owning_window(), QStringLiteral("Export note as Markdown"), suggested, QStringLiteral("Markdown (*.md);;All Files (*)"));
    if (!weak_this || m_is_closing)
        return;
    if (path.isEmpty()) {
        JsonObject out;
        out.set("canceled"sv, true);
        reply_ok(request_id, move(out));
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        reply_error(request_id, "Couldn’t write file."sv);
        return;
    }
    file.write(content.bytes_as_string_view().characters_without_null_termination(), static_cast<qint64>(content.bytes().size()));
    file.close();
    JsonObject out;
    out.set("canceled"sv, false);
    reply_ok(request_id, move(out));
}

void DashboardPage::handle_shortcuts_list(i64 request_id)
{
    auto& app = Application::the();
    struct EntrySpec {
        StringView category;
        StringView label;
        QAction* action;
    };
    auto specs = to_array<EntrySpec>({
        { "Tabs"sv, "New tab"sv, app.new_tab_action() },
        { "Tabs"sv, "Close current tab"sv, app.close_current_tab_action() },
        { "Tabs"sv, "Reopen recently closed tab"sv, app.reopen_recently_closed_tab_action() },
        { "Tabs"sv, "Next tab"sv, app.open_next_tab_action() },
        { "Tabs"sv, "Previous tab"sv, app.open_previous_tab_action() },
        { "Tabs"sv, "Search tabs"sv, app.search_tabs_action() },
        { "Windows"sv, "New window"sv, app.new_window_action() },
        { "Windows"sv, "New private window"sv, app.new_private_window_action() },
        { "Navigation"sv, "Focus address bar"sv, app.open_location_action() },
        { "Navigation"sv, "Back"sv, app.navigate_back_action() },
        { "Navigation"sv, "Forward"sv, app.navigate_forward_action() },
        { "Navigation"sv, "Reload"sv, app.reload_action() },
        { "Navigation"sv, "Reload ignoring cache"sv, app.reload_bypassing_cache_action() },
        { "Navigation"sv, "Stop loading"sv, app.stop_action() },
        { "Find & View"sv, "Find in page"sv, app.find_in_page_action() },
        { "Find & View"sv, "Find next"sv, app.find_next_action() },
        { "Find & View"sv, "Find previous"sv, app.find_previous_action() },
        { "Find & View"sv, "Zoom in"sv, app.zoom_in_action() },
        { "Find & View"sv, "Zoom out"sv, app.zoom_out_action() },
        { "Find & View"sv, "Reset zoom"sv, app.reset_zoom_action() },
        { "View"sv, "Toggle bookmarks bar"sv, app.toggle_bookmark_bar_action() },
        { "View"sv, "Toggle sidebar"sv, app.toggle_sidebar_action() },
        { "Browser"sv, "Open file"sv, app.open_file_action() },
        { "Browser"sv, "Downloads"sv, app.open_downloads_action() },
        { "Browser"sv, "Settings"sv, app.open_settings_action() },
        { "Browser"sv, "Quit"sv, app.quit_action() },
    });

    JsonArray shortcuts;
    for (auto const& spec : specs) {
        if (!spec.action)
            continue;
        auto seq = spec.action->shortcut();
        if (seq.isEmpty())
            continue;
        JsonObject o;
        o.set("category"sv, spec.category);
        o.set("label"sv, spec.label);
        o.set("shortcut"sv, ak_string_from_qstring(seq.toString(QKeySequence::NativeText)));
        shortcuts.must_append(move(o));
    }

    JsonObject out;
    out.set("shortcuts"sv, move(shortcuts));
    reply_ok(request_id, move(out));
}

void DashboardPage::handle_open_url(i64 request_id, JsonObject const& params)
{
    if (!consume_user_activation()) {
        reply_error(request_id, "User interaction is required."sv);
        return;
    }
    auto url_str = params.get_string("url"sv).value_or(String {});
    auto in_new_tab = params.get_bool("newTab"sv).value_or(false);
    auto parsed = URL::Parser::basic_parse(url_str);
    if (!parsed.has_value()) {
        reply_error(request_id, "Invalid URL."sv);
        return;
    }
    if (!is_safe_http_url(*parsed) && !is_allowed_tool_url(*parsed, is_persistent_default_scope())) {
        reply_error(request_id, "Destination URL is not allowed."sv);
        return;
    }

    if (in_new_tab) {
        if (auto* win = owning_window()) {
            win->new_tab_from_url(*parsed, Web::HTML::ActivateTab::Yes, BrowserWindow::TabLocation::after_current_tab());
            reply_ok(request_id, JsonObject {});
            return;
        }
    } else if (auto* tab = owning_tab()) {
        tab->navigate(*parsed);
        reply_ok(request_id, JsonObject {});
        return;
    }
    reply_error(request_id, "Tab is no longer attached."sv);
}

void DashboardPage::handle_privacy_summary(i64 request_id)
{
    auto* hub = HubConnection::the_if_connected();
    if (!hub) {
        reply_error(request_id, "Browser hub is unavailable."sv);
        return;
    }
    reply_ok(request_id, hub->web_ui_privacy_summary(web_ui_id()));
}

void DashboardPage::handle_extensions_list(i64 request_id)
{
    auto* hub = HubConnection::the_if_connected();
    if (!hub) {
        reply_error(request_id, "Browser hub is unavailable."sv);
        return;
    }
    reply_ok(request_id, hub->web_ui_extensions_snapshot(web_ui_id()));
}

void DashboardPage::handle_extensions_set_enabled(i64 request_id, JsonObject const& params)
{
    auto* hub = HubConnection::the_if_connected();
    if (!hub) {
        reply_error(request_id, "Browser hub is unavailable."sv);
        return;
    }
    auto ext_id = params.get_string("extensionId"sv).value_or(String {});
    auto enabled = params.get_bool("enabled"sv).value_or(false);
    reply_ok(request_id, hub->web_ui_set_extension_enabled(web_ui_id(), move(ext_id), enabled));
}

void DashboardPage::handle_extensions_open_options(i64 request_id, JsonObject const& params)
{
    auto* hub = HubConnection::the_if_connected();
    if (!hub) {
        reply_error(request_id, "Browser hub is unavailable."sv);
        return;
    }
    auto ext_id = params.get_string("extensionId"sv).value_or(String {});
    bool opened = hub->web_ui_open_extension_options(web_ui_id(), move(ext_id));
    if (!opened) {
        reply_error(request_id, "Extension options could not be opened."sv);
        return;
    }
    reply_ok(request_id, JsonObject {});
}

void DashboardPage::handle_permissions_list(i64 request_id)
{
    auto* hub = HubConnection::the_if_connected();
    if (!hub) {
        reply_error(request_id, "Browser hub is unavailable."sv);
        return;
    }
    reply_ok(request_id, hub->web_ui_permissions_snapshot(web_ui_id()));
}

void DashboardPage::handle_permissions_revoke(i64 request_id, JsonObject const& params)
{
    auto* hub = HubConnection::the_if_connected();
    if (!hub) {
        reply_error(request_id, "Browser hub is unavailable."sv);
        return;
    }
    auto origin = params.get_string("origin"sv).value_or(String {});
    auto perm = params.get_string("permission"sv).value_or(String {});
    auto expected_gen = params.get_u64("generation"sv).value_or(0);
    reply_ok(request_id, hub->web_ui_revoke_site_permission(web_ui_id(), move(origin), move(perm), expected_gen));
}

void DashboardPage::handle_profile_summary(i64 request_id)
{
    auto* hub = HubConnection::the_if_connected();
    if (!hub) {
        reply_error(request_id, "Browser hub is unavailable."sv);
        return;
    }
    reply_ok(request_id, hub->web_ui_identities_snapshot(web_ui_id()));
}

void DashboardPage::handle_profile_open_private_window(i64 request_id)
{
    if (!consume_user_activation()) {
        reply_error(request_id, "User interaction is required."sv);
        return;
    }
    Application::the().open_new_window(WebView::IsPrivate::Yes);
    reply_ok(request_id, JsonObject {});
}

void DashboardPage::handle_health_snapshot(i64 request_id)
{
    m_tab_handles.clear();
    JsonArray tabs_arr;
    size_t eligible_count = 0;
    size_t loaded_count = 0;

    for (auto* widget : QApplication::topLevelWidgets()) {
        auto* win = as_if<BrowserWindow>(widget);
        if (!win || win->is_private() != m_context.is_private)
            continue;
        for (int i = 0; i < win->tab_count(); ++i) {
            auto* tab = win->tab_at(i);
            if (!tab)
                continue;
            if (tab->is_loaded()) {
                auto const& tab_id = tab->view().identity_id();
                if (!m_context.identity_id.is_empty() && !tab_id.is_empty() && tab_id != m_context.identity_id)
                    continue;
                ++loaded_count;
            }
            ++eligible_count;
            auto const& stats = tab->performance_stats();
            u64 target_view_id = tab->is_loaded() ? tab->view().view_id() : 0;
            String handle = target_view_id != 0
                ? MUST(String::formatted("health:{}:{}", m_handle_nonce, target_view_id))
                : String {};
            if (!handle.is_empty())
                m_tab_handles.set(handle, target_view_id);

            bool is_active = win->current_tab() == tab;
            // NB: A page on display, such as the other side of a shown split view, can't be discarded either.
            bool is_displayed = win->tab_widget().page_area().is_displayed(tab);
            bool is_self = target_view_id != 0 && target_view_id == m_context.view_id;

            JsonObject to;
            to.set("handle"sv, handle);
            to.set("title"sv, ak_string_from_qstring(tab->title()));
            to.set("url"sv, tab->url().serialize());
            to.set("isActive"sv, is_active);
            to.set("isLoaded"sv, tab->is_loaded());
            to.set("isVisible"sv, is_displayed);
            to.set("canDiscard"sv, tab->is_loaded() && !is_active && !is_displayed && !is_self && !tab->is_pinned());
            if (stats.cpu_percent.has_value())
                to.set("cpuPercent"sv, *stats.cpu_percent);
            if (stats.memory_bytes.has_value())
                to.set("memoryBytes"sv, *stats.memory_bytes);
            to.set("sharedWithOtherTabs"sv, stats.other_tabs_sharing_processes);
            to.set("faviconBase64Png"sv, favicon_base64_png(tab->favicon()));
            tabs_arr.must_append(move(to));
        }
    }

    JsonObject out;
    out.set("eligibleTabCount"sv, static_cast<i64>(eligible_count));
    out.set("loadedTabCount"sv, static_cast<i64>(loaded_count));
    out.set("tabs"sv, move(tabs_arr));
    reply_ok(request_id, move(out));
}

void DashboardPage::handle_health_discard_tab(i64 request_id, JsonObject const& params)
{
    auto handle = params.get_string("handle"sv).value_or(String {});
    auto maybe_view_id = m_tab_handles.get(handle);
    if (!maybe_view_id.has_value()) {
        JsonObject extra;
        extra.set("stale"sv, true);
        reply_error(request_id, "Tab is no longer open."sv, move(extra));
        return;
    }
    auto* win = Application::the().window_of_view_id(*maybe_view_id);
    if (!win || win->is_private() != m_context.is_private) {
        reply_error(request_id, "Tab is no longer available."sv);
        return;
    }
    Tab* target_tab = nullptr;
    for (int i = 0; i < win->tab_count(); ++i) {
        auto* t = win->tab_at(i);
        if (t && t->is_loaded() && t->view().view_id() == *maybe_view_id) {
            target_tab = t;
            break;
        }
    }
    if (!target_tab || win->current_tab() == target_tab || target_tab->is_pinned() || *maybe_view_id == m_context.view_id) {
        reply_error(request_id, "Active or pinned tabs cannot be discarded."sv);
        return;
    }
    if (!consume_user_activation()) {
        reply_error(request_id, "User interaction is required."sv);
        return;
    }
    win->discard_tab(*target_tab);
    reply_ok(request_id, JsonObject {});
}

void DashboardPage::handle_media_list(i64 request_id)
{
    auto* hub = HubConnection::the_if_connected();
    if (!hub) {
        reply_error(request_id, "Browser hub is unavailable."sv);
        return;
    }
    auto snapshot = hub->web_ui_media_sessions_snapshot(web_ui_id());

    // The Hub knows each session's tab only by its view; the tab's favicon pictures it.
    if (snapshot.is_object()) {
        if (auto sessions = snapshot.as_object().get_array("sessions"sv); sessions.has_value()) {
            JsonArray pictured;
            for (auto const& value : sessions->values()) {
                if (!value.is_object()) {
                    pictured.must_append(value);
                    continue;
                }
                auto session = value.as_object();
                if (auto view_id = session.get_integer<u64>("viewId"sv); view_id.has_value()) {
                    if (auto* win = Application::the().window_of_view_id(*view_id); win && win->is_private() == m_context.is_private) {
                        win->for_each_tab([&](Tab& tab) {
                            if (tab.is_loaded() && tab.view().view_id() == *view_id)
                                session.set("faviconBase64Png"sv, favicon_base64_png(tab.favicon()));
                        });
                    }
                }
                pictured.must_append(move(session));
            }
            auto out = snapshot.as_object();
            out.set("sessions"sv, move(pictured));
            reply_ok(request_id, move(out));
            return;
        }
    }
    reply_ok(request_id, move(snapshot));
}

void DashboardPage::handle_media_action(i64 request_id, JsonObject const& params)
{
    auto* hub = HubConnection::the_if_connected();
    if (!hub) {
        reply_error(request_id, "Browser hub is unavailable."sv);
        return;
    }
    auto target_view_id = params.get_u64("viewId"sv).value_or(0);
    auto action = params.get_string("action"sv).value_or(String {});
    Optional<double> seek_time;
    if (params.has("seekTime"sv) && !params.get("seekTime"sv)->is_null())
        seek_time = params.get_double_with_precision_loss("seekTime"sv);
    bool ok = hub->web_ui_media_session_action(web_ui_id(), target_view_id, move(action), seek_time);
    if (!ok) {
        JsonObject extra;
        extra.set("stale"sv, true);
        reply_error(request_id, "Media session is no longer active."sv, move(extra));
        return;
    }
    reply_ok(request_id, JsonObject {});
}

void DashboardPage::handle_references_list(i64 request_id)
{
    JsonArray tools;
    auto append_tool = [&](StringView label, StringView url, StringView desc) {
        JsonObject t;
        t.set("label"sv, label);
        t.set("url"sv, url);
        t.set("description"sv, desc);
        tools.must_append(move(t));
    };

    append_tool("Settings"sv, "about:settings"sv, "Browser preferences"sv);
    append_tool("Privacy"sv, "about:privacy"sv, "Network mode and counters"sv);
    if (is_persistent_default_scope()) {
        append_tool("Bookmarks"sv, "about:bookmarks"sv, "Saved links and folders"sv);
        append_tool("History"sv, "about:history"sv, "Visited pages"sv);
        append_tool("Downloads"sv, "about:downloads"sv, "Recent file transfers"sv);
        append_tool("Add-ons"sv, "about:addons"sv, "Manage extensions"sv);
    }
    append_tool("Support"sv, "about:support"sv, "Diagnostics"sv);
    append_tool("Version"sv, "about:version"sv, "Build details"sv);

    JsonArray files;
    if (is_persistent_default_scope()) {
        for (auto const& item : store().list_items(m_scope_key, "pinned-references"_string, 50)) {
            auto path = item.value.get_string("path"sv).value_or(String {});
            QFileInfo info(qstring_from_ak_string(path));
            JsonObject f;
            f.set("id"sv, item.item_id);
            f.set("label"sv, item.value.get_string("label"sv).value_or(item.value.get_string("basename"sv).value_or("Document"_string)));
            f.set("basename"sv, item.value.get_string("basename"sv).value_or(String {}));
            f.set("missing"sv, !(info.exists() && info.isFile()));
            files.must_append(move(f));
        }
    }

    JsonObject out;
    out.set("tools"sv, move(tools));
    out.set("files"sv, move(files));
    out.set("canPinLocalFiles"sv, is_persistent_default_scope());
    reply_ok(request_id, move(out));
}

void DashboardPage::handle_references_pick_file(i64 request_id)
{
    if (!is_persistent_default_scope()) {
        reply_error(request_id, "Pinning local files is unavailable in ephemeral sessions."sv);
        return;
    }
    if (!consume_user_activation()) {
        reply_error(request_id, "User interaction is required."sv);
        return;
    }

    auto weak_this = make_weak_ptr();
    auto picked = QFileDialog::getOpenFileName(owning_window(), QStringLiteral("Pin a local document"), {}, QStringLiteral("Documents (*.pdf *.html *.htm *.txt *.md *.svg *.png *.jpg);;All Files (*)"));
    if (!weak_this || m_is_closing)
        return;
    if (picked.isEmpty()) {
        JsonObject out;
        out.set("canceled"sv, true);
        reply_ok(request_id, move(out));
        return;
    }

    QFileInfo info(picked);
    if (!info.exists() || !info.isFile()) {
        reply_error(request_id, "Selected item is not a regular file."sv);
        return;
    }

    auto ref_id = generate_opaque_id("ref"sv);
    auto basename = ak_string_from_qstring(info.fileName());
    JsonObject val;
    val.set("path"sv, ak_string_from_qstring(info.canonicalFilePath()));
    val.set("basename"sv, basename);
    val.set("label"sv, basename);

    auto res = store().save_item(m_scope_key, "pinned-references"_string, ref_id, {}, val);
    if (!res.ok) {
        reply_error(request_id, res.error);
        return;
    }

    JsonObject out;
    out.set("canceled"sv, false);
    out.set("id"sv, ref_id);
    out.set("label"sv, basename);
    out.set("basename"sv, basename);
    reply_ok(request_id, move(out));
}

void DashboardPage::handle_references_open_file(i64 request_id, JsonObject const& params)
{
    if (!is_persistent_default_scope()) {
        reply_error(request_id, "Local document pins are unavailable in this session."sv);
        return;
    }
    if (!consume_user_activation()) {
        reply_error(request_id, "User interaction is required."sv);
        return;
    }
    auto ref_id = params.get_string("id"sv).value_or(String {});
    auto item = store().get_item(m_scope_key, "pinned-references"_string, ref_id);
    if (!item.has_value()) {
        reply_error(request_id, "Pinned file was removed."sv);
        return;
    }
    auto path = item->value.get_string("path"sv).value_or(String {});
    QFileInfo info(qstring_from_ak_string(path));
    if (!info.exists() || !info.isFile()) {
        JsonObject extra;
        extra.set("missing"sv, true);
        reply_error(request_id, "File was moved or deleted. Reselect it to reopen."sv, move(extra));
        return;
    }

    auto file_url = URL::create_with_file_scheme(path.to_byte_string());
    if (!file_url.has_value()) {
        reply_error(request_id, "Couldn’t construct file URL."sv);
        return;
    }

    if (auto* tab = owning_tab()) {
        tab->navigate(*file_url);
        reply_ok(request_id, JsonObject {});
        return;
    }
    reply_error(request_id, "Tab is no longer attached."sv);
}

void DashboardPage::handle_references_remove_file(i64 request_id, JsonObject const& params)
{
    if (!is_persistent_default_scope()) {
        reply_error(request_id, "Unavailable in this session."sv);
        return;
    }
    auto ref_id = params.get_string("id"sv).value_or(String {});
    (void)store().delete_item(m_scope_key, "pinned-references"_string, ref_id);
    reply_ok(request_id, JsonObject {});
}

void DashboardPage::handle_network_list_grants(i64 request_id)
{
    JsonObject out;
    JsonArray grants;
    out.set("allowed"sv, allows_network_widgets());
    out.set("offline"sv, Application::the().privacy_network_mode() == Privacy::NetworkMode::Offline);

    if (allows_network_widgets()) {
        for (auto const& g : store().list_network_grants(m_scope_key)) {
            JsonObject go;
            go.set("sourceId"sv, g.source_id);
            go.set("kind"sv, g.kind);
            go.set("consentVersion"sv, g.consent_version);
            go.set("config"sv, g.config);
            go.set("cache"sv, g.cache);
            go.set("expiresAt"sv, g.expires_at);
            go.set("lastSuccessAt"sv, g.last_success_at);
            grants.must_append(move(go));
        }
    }
    out.set("grants"sv, move(grants));
    reply_ok(request_id, move(out));
}

void DashboardPage::handle_network_grant(i64 request_id, JsonObject const& params)
{
    if (!allows_network_widgets()) {
        reply_error(request_id, "Network widgets are disabled in private or temporary sessions."sv);
        return;
    }
    if (!consume_user_activation()) {
        reply_error(request_id, "Explicit consent click is required."sv);
        return;
    }
    auto kind = params.get_string("kind"sv).value_or(String {});
    JsonObject cfg;
    String source_id;

    if (kind == "feed"sv) {
        auto url_str = params.get_string("url"sv).value_or(String {});
        auto parsed = URL::Parser::basic_parse(url_str);
        if (!parsed.has_value() || !is_public_https_url(*parsed)) {
            reply_error(request_id, "Only public HTTPS feed URLs without credentials are allowed."sv);
            return;
        }
        auto normalized = parsed->serialize();
        source_id = MUST(String::formatted("feed:{}", normalized));
        cfg.set("url"sv, normalized);
        cfg.set("label"sv, params.get_string("label"sv).value_or(MUST(String::from_utf8(parsed->serialized_host()))));
        cfg.set("autoRefresh"sv, params.get_bool("autoRefresh"sv).value_or(false));
    } else if (kind == "weather"sv) {
        auto lat = params.get_double_with_precision_loss("lat"sv);
        auto lon = params.get_double_with_precision_loss("lon"sv);
        auto label = params.get_string("label"sv).value_or("Location"_string);
        if (!lat.has_value() || !lon.has_value() || !isfinite(*lat) || !isfinite(*lon) || *lat < -90.0 || *lat > 90.0 || *lon < -180.0 || *lon > 180.0) {
            reply_error(request_id, "Enter valid latitude (-90 to 90) and longitude (-180 to 180)."sv);
            return;
        }
        double rlat = round(*lat * 100.0) / 100.0;
        double rlon = round(*lon * 100.0) / 100.0;
        source_id = MUST(String::formatted("weather:{:.2f},{:.2f}", rlat, rlon));
        cfg.set("lat"sv, rlat);
        cfg.set("lon"sv, rlon);
        cfg.set("label"sv, label);
        cfg.set("provider"sv, "Open-Meteo (api.open-meteo.com)"_string);
        cfg.set("autoRefresh"sv, params.get_bool("autoRefresh"sv).value_or(false));
    } else {
        reply_error(request_id, "Unsupported network source kind."sv);
        return;
    }

    if (!store().upsert_network_grant(m_scope_key, source_id, kind, 1, cfg)) {
        reply_error(request_id, "Couldn’t save network consent."sv);
        return;
    }

    JsonObject out;
    out.set("sourceId"sv, source_id);
    out.set("kind"sv, kind);
    out.set("config"sv, cfg);
    reply_ok(request_id, move(out));

    coordinator().broadcast_to_scope(m_scope_key, this, [&](DashboardPage& peer) {
        peer.push_event("networkGrantsChanged"sv, JsonObject {});
    });
}

void DashboardPage::handle_network_revoke(i64 request_id, JsonObject const& params)
{
    auto source_id = params.get_string("sourceId"sv).value_or(String {});
    cancel_in_flight_requests();
    (void)store().revoke_network_grant(m_scope_key, source_id);
    reply_ok(request_id, JsonObject {});

    coordinator().broadcast_to_scope(m_scope_key, this, [&](DashboardPage& peer) {
        peer.push_event("networkGrantsChanged"sv, JsonObject {});
    });
}

void DashboardPage::handle_network_refresh(i64 request_id, JsonObject const& params)
{
    if (!allows_network_widgets()) {
        reply_error(request_id, "Network refresh is disabled in private or temporary sessions."sv);
        return;
    }
    if (Application::the().privacy_network_mode() == Privacy::NetworkMode::Offline) {
        reply_error(request_id, "Browser is currently in Offline mode."sv);
        return;
    }

    auto source_id = params.get_string("sourceId"sv).value_or(String {});
    auto grant = store().get_network_grant(m_scope_key, source_id);
    if (!grant.has_value()) {
        reply_error(request_id, "Consent has not been granted for this source."sv);
        return;
    }

    auto& active_count = coordinator().active_fetches_per_scope.ensure(m_scope_key);
    if (active_count >= 2) {
        reply_error(request_id, "Too many concurrent refreshes; try again shortly."sv);
        return;
    }

    auto* hub = HubConnection::the_if_connected();
    if (!hub) {
        reply_error(request_id, "Browser hub is unavailable."sv);
        return;
    }

    String target;
    if (grant->kind == "feed"sv) {
        target = grant->config.get_string("url"sv).value_or(String {});
    } else if (grant->kind == "weather"sv) {
        auto lat = grant->config.get_double_with_precision_loss("lat"sv).value_or(0.0);
        auto lon = grant->config.get_double_with_precision_loss("lon"sv).value_or(0.0);
        target = MUST(String::formatted("{:.2f},{:.2f}", lat, lon));
    } else {
        reply_error(request_id, "Unsupported source kind."sv);
        return;
    }

    ++active_count;
    auto weak_this = make_weak_ptr();
    auto kind = grant->kind;
    auto scope_key = m_scope_key;

    auto hub_req_id = hub->fetch_dashboard_resource(
        web_ui_id(),
        kind,
        target,
        0,
        [weak_this, request_id, source_id, kind, target, scope_key](HubConnection::RequestResponse response) {
            auto& cnt = coordinator().active_fetches_per_scope.ensure(scope_key);
            if (cnt > 0)
                --cnt;
            if (!weak_this || weak_this->m_is_closing)
                return;

            // Remove tracking entry
            Vector<u64> done_keys;
            for (auto const& [k, v] : weak_this->m_in_flight_requests) {
                if (v == static_cast<u64>(request_id))
                    done_keys.append(k);
            }
            for (auto k : done_keys)
                weak_this->m_in_flight_requests.remove(k);

            // Reverify grant still exists before committing cache.
            auto current_grant = weak_this->store().get_network_grant(scope_key, source_id);
            if (!current_grant.has_value()) {
                weak_this->reply_error(request_id, "Source consent was revoked."sv);
                return;
            }

            if (response.had_network_error || !response.response_code.has_value() || *response.response_code < 200 || *response.response_code >= 300) {
                weak_this->reply_error(request_id, "Couldn’t fetch update from source."sv);
                return;
            }

            Optional<JsonObject> parsed_data;
            if (kind == "feed"sv)
                parsed_data = parse_feed_xml(response.body, target);
            else if (kind == "weather"sv)
                parsed_data = parse_open_meteo_json(response.body);

            if (!parsed_data.has_value()) {
                weak_this->reply_error(request_id, "Received malformed response from source."sv);
                return;
            }

            i64 now_ms = UnixDateTime::now().milliseconds_since_epoch();
            i64 expires_ms = now_ms + (30ll * 60 * 1000);
            (void)weak_this->store().update_network_cache(scope_key, source_id, *parsed_data, expires_ms, now_ms);

            JsonObject out;
            out.set("sourceId"sv, source_id);
            out.set("cache"sv, *parsed_data);
            out.set("lastSuccessAt"sv, now_ms);
            weak_this->reply_ok(request_id, move(out));

            coordinator().broadcast_to_scope(scope_key, weak_this.ptr(), [&](DashboardPage& peer) {
                peer.push_event("networkGrantsChanged"sv, JsonObject {});
            });
        });

    m_in_flight_requests.set(hub_req_id, static_cast<u64>(request_id));
}

}
