/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/JsonObject.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <AK/Weakable.h>
#include <Hub/HubClientEndpoint.h>
#include <UI/Qt/BookmarkStore.h>
#include <UI/Qt/DashboardStore.h>
#include <UI/Qt/RemoteDownloads.h>
#include <UI/Qt/WebUIPage.h>

namespace Ladybird {

class BrowserWindow;
class Tab;

class DashboardPage final
    : public WebUIPage
    , public Weakable<DashboardPage>
    , public BookmarkStoreObserver
    , public RemoteDownloadsObserver {
public:
    explicit DashboardPage(Hub::WebUIPageContext const&);
    virtual ~DashboardPage() override;

    virtual void received_message(String const& name, JsonValue const& data) override;

    static void notify_history_cleared();
    static void notify_privacy_network_mode_changed();
    static void notify_recently_closed_changed();
    static void reset_for_testing();

    Hub::WebUIPageContext const& context() const { return m_context; }
    String const& scope_key() const { return m_scope_key; }

private:
    // ^BookmarkStoreObserver
    virtual void bookmarks_changed() override;

    // ^RemoteDownloadsObserver
    virtual void download_added(RemoteDownloads::Download const&) override;
    virtual void download_updated(RemoteDownloads::Download const&) override;
    virtual void download_removed(u64, WebView::IsPrivate) override;

    DashboardStore& store();
    bool is_persistent_default_scope() const;
    bool is_ephemeral_scope() const;
    bool allows_network_widgets() const;
    BrowserWindow* owning_window() const;
    Tab* owning_tab() const;

    void reply_ok(i64 request_id, JsonValue data = JsonObject {});
    void reply_error(i64 request_id, StringView error, Optional<JsonObject> extra = {});
    void push_event(StringView event_name, JsonValue data);

    // Core & layout
    void handle_init(i64 request_id);
    void handle_commit_layout(i64 request_id, JsonObject const& params);
    void handle_delete_all_data(i64 request_id);

    // Personal items
    void handle_items_list(i64 request_id, JsonObject const& params);
    void handle_items_save(i64 request_id, JsonObject const& params);
    void handle_items_delete(i64 request_id, JsonObject const& params);
    void handle_items_clear_completed(i64 request_id, JsonObject const& params);

    // Phase B: Navigation & History
    void handle_top_sites_load(i64 request_id);
    void handle_search_engines(i64 request_id);
    void handle_search_submit(i64 request_id, JsonObject const& params);
    void handle_tabs_list(i64 request_id);
    void handle_tabs_action(i64 request_id, StringView action, JsonObject const& params);
    void handle_recently_closed_list(i64 request_id);
    void handle_recently_closed_reopen(i64 request_id, JsonObject const& params);
    void handle_bookmarks_folders(i64 request_id);
    void handle_bookmarks_folder(i64 request_id, JsonObject const& params);
    void handle_history_list(i64 request_id, JsonObject const& params);
    void handle_history_remove(i64 request_id, JsonObject const& params);

    // Phase C: Personal & Activity
    void handle_workspaces_save_window(i64 request_id, JsonObject const& params);
    void handle_workspaces_open(i64 request_id, JsonObject const& params);
    void handle_downloads_list(i64 request_id);
    void handle_downloads_action(i64 request_id, JsonObject const& params);
    void handle_notes_export(i64 request_id, JsonObject const& params);
    void handle_shortcuts_list(i64 request_id);
    void handle_open_url(i64 request_id, JsonObject const& params);

    // Phase D: Browser Controls
    void handle_privacy_summary(i64 request_id);
    void handle_extensions_list(i64 request_id);
    void handle_extensions_set_enabled(i64 request_id, JsonObject const& params);
    void handle_extensions_open_options(i64 request_id, JsonObject const& params);
    void handle_permissions_list(i64 request_id);
    void handle_permissions_revoke(i64 request_id, JsonObject const& params);
    void handle_profile_summary(i64 request_id);
    void handle_profile_open_private_window(i64 request_id);
    void handle_health_snapshot(i64 request_id);
    void handle_health_discard_tab(i64 request_id, JsonObject const& params);
    void handle_media_list(i64 request_id);
    void handle_media_action(i64 request_id, JsonObject const& params);
    void handle_references_list(i64 request_id);
    void handle_references_pick_file(i64 request_id);
    void handle_references_open_file(i64 request_id, JsonObject const& params);
    void handle_references_remove_file(i64 request_id, JsonObject const& params);

    // Phase E: Guarded Network (Feed Reader & Weather)
    void handle_network_list_grants(i64 request_id);
    void handle_network_grant(i64 request_id, JsonObject const& params);
    void handle_network_revoke(i64 request_id, JsonObject const& params);
    void handle_network_refresh(i64 request_id, JsonObject const& params);
    void cancel_in_flight_requests();

    Hub::WebUIPageContext m_context;
    String m_scope_key;
    u64 m_handle_nonce { 0 };
    HashMap<String, u64> m_tab_handles;
    HashMap<u64, u64> m_in_flight_requests;
    bool m_is_closing { false };
};

}
