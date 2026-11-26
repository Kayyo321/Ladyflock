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
#include <AK/HashTable.h>
#include <AK/JsonObject.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <AK/Weakable.h>
#include <LibCore/Timer.h>
#include <LibDatabase/Forward.h>
#include <LibURL/Origin.h>
#include <LibURL/URL.h>
#include <LibWebCommon/Extensions/ExtensionManifest.h>
#include <LibWebCommon/Extensions/ExtensionTypes.h>
#include <LibWebCommon/HTML/CrossProcessId.h>
#include <LibWebCommon/Page/PageId.h>
#include <LibWebView/ContextMenuRequest.h>
#include <LibWebView/Export.h>
#include <LibWebView/Extensions/CatalogClient.h>
#include <LibWebView/Extensions/DeclarativeRequestRules.h>
#include <LibWebView/Extensions/ExtensionActionController.h>
#include <LibWebView/Extensions/ExtensionAlarms.h>
#include <LibWebView/Extensions/ExtensionInstaller.h>
#include <LibWebView/Extensions/ExtensionSidebarController.h>
#include <LibWebView/Extensions/ExtensionStorage.h>
#include <LibWebView/Extensions/RevocationList.h>
#include <LibWebView/Extensions/WebRequestCoordinator.h>
#include <LibWebView/Forward.h>

namespace Requests {

class RequestControlClient;

}

namespace WebView::Extensions {

struct RegisteredExtensionContentScript {
    String id;
    Web::Extensions::ContentScriptRule rule;
    JsonObject descriptor;
    bool persist_across_sessions { true };
    bool run_in_main_world { false };
    struct Owner {
        WeakPtr<WebContentClient> renderer;
        Web::PageId page_id;
        Web::HTML::CrossProcessId document_id;
    };
    Optional<Owner> legacy_owner {};
    Vector<Web::Extensions::NamedSource> legacy_js {};
    Vector<Web::Extensions::NamedSource> legacy_css {};
};

struct WEBVIEW_API InstalledExtension {
    String extension_id;
    String uuid;
    Web::Extensions::InstallSource source { Web::Extensions::InstallSource::UnpackedDev };
    ByteString install_directory;
    Web::Extensions::ExtensionManifest manifest;
    Web::Extensions::CompatibilityStatus compatibility_status { Web::Extensions::CompatibilityStatus::ManifestCompatibleUntested };
    String sha256_hex;
    bool verified_signature { false };
    bool enabled { true };
    u64 grant_generation { 1 };
    bool allow_all_sites { true };
    HashTable<String> granted_extra_hosts;
    HashTable<String> revoked_hosts;
    HashTable<u64> active_tab_ids;
    bool from_catalog { false };
    Optional<bool> network_prediction_setting {};
    Vector<DeclarativeRuleset> declarative_rulesets {};
    MutableDeclarativeRuleset dynamic_declarative_rules {};
    MutableDeclarativeRuleset session_declarative_rules {};
    Vector<RegisteredExtensionContentScript> registered_content_scripts {};
};

// Central Hub authority for extensions within one ordinary BrowsingSession.
// Never created for private browsing sessions.
class WEBVIEW_API ExtensionManager : public Weakable<ExtensionManager> {
    AK_MAKE_NONCOPYABLE(ExtensionManager);

public:
    AK_ALLOC_WITH_KMALLOC;

    static ErrorOr<NonnullOwnPtr<ExtensionManager>> create(ByteString profile_data_root, Database::Database* shared_database = nullptr);
    ~ExtensionManager();

    bool extensions_enabled() const { return m_extensions_enabled; }
    void set_extensions_enabled(bool enabled) { m_extensions_enabled = enabled; }

    ExtensionStorage& storage() { return *m_storage; }
    ExtensionStorage const& storage() const { return *m_storage; }
    bool network_prediction_enabled() const;
    void global_privacy_control_changed();
    ExtensionActionController& action_controller() { return m_action_controller; }
    ExtensionActionController const& action_controller() const { return m_action_controller; }
    ExtensionSidebarController& sidebar_controller() { return m_sidebar_controller; }
    ExtensionSidebarController const& sidebar_controller() const { return m_sidebar_controller; }
    RevocationList& revocation_list() { return m_revocation_list; }
    RevocationList const& revocation_list() const { return m_revocation_list; }
    CatalogClient& catalog_client() { return m_catalog_client; }
    CatalogClient const& catalog_client() const { return m_catalog_client; }
    ExtensionInstaller& installer() { return m_installer; }
    ExtensionInstaller const& installer() const { return m_installer; }
    WebRequestCoordinator& web_request_coordinator() { return m_web_request_coordinator; }
    WebRequestCoordinator const& web_request_coordinator() const { return m_web_request_coordinator; }

    // Startup & shutdown: clean stale staging, reload persisted extensions, and tear down background views.
    ErrorOr<void> initialize_persisted_extensions();
    void shutdown();
    ErrorOr<InstalledExtension> load_unpacked(ByteString const& directory, bool enabled = true);

    ErrorOr<StagedInstall> stage_xpi(ByteString const& xpi_path, Optional<String> const& expected_addon_id = {}, InstallDiagnostic* = nullptr);
    ErrorOr<StagedInstall> stage_catalog_package(CatalogEntry const&, ByteBuffer, bool is_update, InstallDiagnostic* = nullptr);
    ErrorOr<InstalledExtension> approve_install(StringView token);
    ErrorOr<void> cancel_install(StringView token);
    ErrorOr<StagedInstall> stage_update(StringView addon_id, ByteString const& xpi_path, InstallDiagnostic* = nullptr);
    ErrorOr<InstalledExtension> approve_update(StringView token, bool user_consented);

    // Update availability. Ladyflock checks Mozilla Add-ons only when the user asks it to, and remembers what it found
    // for this session, so the toolbar and Add-ons page can show which extensions have a newer version.
    struct UpdateCheckResult {
        size_t checked { 0 };
        Vector<String> failed_extension_ids;
    };
    // Compares Firefox extension versions: negative if left is older than right, zero if equal, positive if newer.
    static int compare_versions(StringView left, StringView right);
    bool can_check_for_update(InstalledExtension const&) const;
    Optional<String> available_update_version(StringView extension_id) const;
    // Records the version Mozilla Add-ons lists for an extension. Returns whether it is newer than the installed one.
    bool record_listed_version(StringView extension_id, StringView listed_version);
    void check_for_updates(Function<void(UpdateCheckResult)> on_complete);

    ErrorOr<void> set_extension_enabled(StringView extension_id, bool enabled);
    ErrorOr<void> set_site_access(StringView extension_id, bool allow_all_sites, Optional<String> toggle_host = {}, bool grant_host = true);
    ErrorOr<void> remove_extension(StringView extension_id);

    Optional<InstalledExtension const&> extension_by_id(StringView extension_id) const;
    Optional<InstalledExtension const&> extension_by_uuid(StringView uuid) const;
    Vector<InstalledExtension const*> all_extensions() const;

    // Resource serving for moz-extension://<uuid>/<path> (with WAR & CSP).
    Web::Extensions::ExtensionResourceResponse serve_resource(URL::URL const&, Optional<URL::Origin> const& initiator_origin, Optional<size_t> size_limit = {}) const;
    static ErrorOr<ByteBuffer> read_package_icon(ByteString const& directory, StringView path);
    String locale_messages_for(InstalledExtension const&) const;
    bool is_extension_document_url(URL::URL const&) const;

    // Permission checks & activeTab.
    void grant_active_tab(StringView extension_id, u64 tab_id);
    void clear_active_tab(u64 tab_id);
    void clear_tab_grants_on_navigation(u64 tab_id);
    bool has_host_permission(StringView extension_id, URL::URL const& url, Optional<u64> tab_id = {}) const;

    // Pre-activation content scripts & world revocation.
    void attach_content_scripts_for_document(WebContentPage&, CanonicalNavigable const&, CanonicalDocument&, URL::URL const& document_url) const;
    void revoke_document_worlds(WebContentPage& page, Web::HTML::CrossProcessId document_id);

    // Tab views (excluding hidden background pages and private views).
    static bool is_extension_background_view(ViewImplementation const&);
    Vector<ViewImplementation*> ordinary_tab_views() const;
    ErrorOr<JsonValue> navigation_frames(StringView method, JsonValue const& arguments) const;
    void dispatch_navigation_event(CanonicalNavigable const&, StringView event, JsonObject extra = {});
    Optional<Web::Extensions::ExtensionTabInfo> tab_info_for_extension(StringView extension_id, ViewImplementation const&) const;
    Optional<u64> tab_id_for_page(WebContentClient const&, Web::PageId) const;
    Optional<u64> view_id_for_document(Web::HTML::CrossProcessId document_id) const;

    // Background / non-persistent event page management.
    void set_event_page_idle_timeout_ms(u32 timeout_ms) { m_event_page_idle_timeout_ms = timeout_ms; }
    bool is_background_running(StringView extension_id) const;
    void ensure_background_started(StringView extension_id, Function<void(bool ready)> on_ready);
    void notify_background_document_loaded(StringView extension_id, WebContentPage&);

    // IPC handlers from WebContentClient.
    void handle_api_call(
        WebContentClient& caller,
        Web::PageId page_id,
        u64 call_id,
        String const& extension_id,
        Web::Extensions::ContextType context_type,
        Web::HTML::CrossProcessId document_id,
        u64 grant_generation,
        String const& namespace_name,
        String const& method_name,
        String const& args_json);

    void handle_event_reply(WebContentClient&, Web::PageId, u64 reply_id, bool has_value, String const& result_json);
    void handle_script_execution_finished(WebContentClient&, Web::PageId, u64 execution_id, bool success, String const& result_or_error_json);
    void handle_web_request_check(WebContentClient& caller, Web::PageId page_id, u64 check_id, Web::Extensions::WebRequestBeforeRequestDetails const&);

    void dispatch_event_to_extension(
        String const& extension_id,
        StringView namespace_name,
        StringView event_name,
        String const& args_json,
        Function<void(bool has_value, String const& result_json)> reply_callback = {},
        Optional<Web::HTML::CrossProcessId> source_document_id = {});

    void notify_tab_created(ViewImplementation const&);
    enum class TabChange { Loading,
        Complete,
        Title,
        URL,
        Pinned };
    void notify_tab_updated(u64 tab_id, TabChange);
    void notify_tab_activated(u64 tab_id, u64 window_id);
    void notify_window_focus_changed(Optional<u64> window_id);
    void notify_tab_removed(u64 tab_id, bool window_closing);

    Function<void()> on_chrome_changed;
    void notify_chrome_changed()
    {
        if (on_chrome_changed)
            on_chrome_changed();
    }
    ErrorOr<void> set_action_pinned(String const&, bool);
    ErrorOr<void> set_sidebar_width(u64 window_id, u32 width);
    static Optional<URL::URL> package_page_url(InstalledExtension const&, StringView path);
    u64 current_window_id() const;

    Vector<ExtensionMenuItem> page_menu_items(ViewImplementation const&, Web::HTML::CrossProcessId, ExtensionMenuTarget const& = {}) const;
    void activate_page_menu_item(ViewImplementation const&, Web::HTML::CrossProcessId, u64 token, ExtensionMenuTarget const& = {});

    JsonObject summary_json() const;

    // Invokes action on a tab (popup or action.onClicked + activeTab).
    ExtensionActionController::InvokeOutcome invoke_action_for_tab(StringView extension_id, u64 tab_id, u64 window_id, Function<void(URL::URL const&)> open_popup = {});

    static void push_watch_filter_to_request_server(Requests::RequestControlClient&, Web::Extensions::WebRequestWatchSnapshot const&);

private:
    ExtensionManager(ByteString profile_data_root, NonnullOwnPtr<ExtensionStorage>);

    ErrorOr<void> load_registry();
    ErrorOr<void> save_registry(InstalledExtension const* replacement = nullptr) const;

    void broadcast_world_revocation(StringView extension_id, u64 new_grant_generation, bool include_extension_pages = true);
    void push_watch_snapshot_everywhere(Web::Extensions::WebRequestWatchSnapshot const&);
    void refresh_declarative_watch_state();
    Optional<InstalledExtension const&> network_prediction_owner() const;
    JsonObject network_prediction_setting_for(StringView extension_id) const;
    void notify_network_prediction_changed(bool previously_enabled);

    void register_extension_entry(InstalledExtension, bool first_install = false);
    void dispatch_update_available_event(StagedInstall const&);
    void dispatch_installed_event(InstalledExtension const&, Optional<String> previous_version = {});
    void stop_background_page(StringView extension_id);
    void schedule_event_page_idle_timer(StringView extension_id);
    u64 register_event_reply(Function<void(bool, String const&)>);
    void bind_event_reply(Optional<u64>, WebContentPage&);
    struct RuntimePortEndpoint {
        WeakPtr<WebContentClient> renderer;
        Web::PageId page_id;
        Web::HTML::CrossProcessId document_id;
    };
    struct RuntimePort {
        String extension_id;
        RuntimePortEndpoint sender;
        Optional<RuntimePortEndpoint> receiver;
        Vector<String> queued_messages;
        size_t queued_message_bytes { 0 };
        bool connected { false };
    };
    JsonObject extension_message_sender(WebContentClient&, Web::PageId, Web::HTML::CrossProcessId, String const& extension_id) const;
    void send_runtime_port_event(RuntimePortEndpoint const&, String const& extension_id, StringView event, String const& port_id, Optional<String> const& payload = {});
    void close_runtime_port(String const& id, Optional<Web::HTML::CrossProcessId> source_document = {}, Optional<String> const& error = {});
    bool handle_runtime_port_call(WebContentClient&, Web::PageId, Web::HTML::CrossProcessId, String const& extension_id, StringView method, JsonArray const&);

    void handle_scripting_execute_script(
        WebContentClient& caller,
        Web::PageId page_id,
        u64 call_id,
        Web::HTML::CrossProcessId source_document_id,
        InstalledExtension const&,
        JsonObject const& details_obj,
        bool modern_api = true);
    void handle_scripting_css(
        WebContentClient& caller,
        Web::PageId page_id,
        u64 call_id,
        Web::HTML::CrossProcessId source_document_id,
        InstalledExtension const&,
        JsonObject const& details_obj,
        bool remove);
    ErrorOr<JsonValue> handle_registered_content_scripts(InstalledExtension&, StringView method, JsonArray const& arguments);

    struct BackgroundHost {
        AK_ALLOC_WITH_KMALLOC;

        OwnPtr<HeadlessWebView> view;
        bool loaded { false };
        Vector<Function<void(bool)>> pending_ready_callbacks;
        RefPtr<Core::Timer> startup_timer;
        RefPtr<Core::Timer> idle_timer;
    };

    ByteString m_profile_data_root;
    ByteString m_extensions_root;
    Optional<u64> m_focused_window_id;
    bool m_extensions_enabled { false };
    u32 m_event_page_idle_timeout_ms { 30000 };

    NonnullOwnPtr<ExtensionStorage> m_storage;
    NonnullOwnPtr<ExtensionStorage> m_session_storage { ExtensionStorage::create() };
    HashTable<String> m_content_script_session_storage_access;
    ExtensionActionController m_action_controller;
    ExtensionAlarms m_alarms;
    ExtensionSidebarController m_sidebar_controller;
    RevocationList m_revocation_list;
    CatalogClient m_catalog_client;
    // The newer version Mozilla Add-ons listed for an extension at the last check this session, by extension ID.
    HashMap<String, String> m_available_updates;
    ExtensionInstaller m_installer;
    WebRequestCoordinator m_web_request_coordinator;

    ErrorOr<void> change_page_menu(String const& extension_id, StringView method, JsonArray const&);
    struct MenuRegistration {
        bool matches(CanonicalNavigable const&, ExtensionMenuTarget const&) const;
        String id;
        String title;
        u64 token;
        Vector<String> contexts { "page"_string };
        Vector<Web::Extensions::MatchPattern> documents {};
        Vector<Web::Extensions::MatchPattern> targets {};
        bool matches_abp_links { false };
    };
    HashMap<String, Vector<MenuRegistration>> m_page_menus;
    u64 m_next_menu_token { 1 };

    HashMap<String, InstalledExtension> m_extensions_by_id;
    HashMap<String, String> m_id_by_uuid;
    HashMap<String, String> m_persisted_uuid_by_id;

    HashMap<String, NonnullOwnPtr<BackgroundHost>> m_background_hosts;
    HashMap<String, RuntimePort> m_runtime_ports;

    struct PendingEventReply {
        Function<void(bool has_value, String const& result_json)> callback;
        WeakPtr<WebContentClient> renderer;
        Optional<Web::PageId> page_id;
        RefPtr<Core::Timer> timeout;
    };
    HashMap<u64, PendingEventReply> m_pending_event_replies;
    u64 m_next_reply_id { 1 };

    struct PendingScriptExecution {
        WeakPtr<WebContentClient> caller;
        Web::PageId page_id { 0 };
        Web::HTML::CrossProcessId document_id;
        u64 call_id { 0 };
        i64 frame_id { 0 };
        bool expect_array_results { false };
        WeakPtr<WebContentClient> renderer;
        Web::PageId target_page_id { 0 };
    };
    HashMap<u64, PendingScriptExecution> m_pending_script_executions;
    u64 m_next_execution_id { 1 };
};

}
