/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/Array.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/Math.h>
#include <AK/QuickSort.h>
#include <Hub/Application.h>
#include <Hub/ConnectionFromUI.h>
#include <Hub/Proxies.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibHTTP/HeaderList.h>
#include <LibImageDecoderClient/Client.h>
#include <LibPrivacy/Identity/Types.h>
#include <LibRequests/RequestClient.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/Loader/UserAgent.h>
#include <LibWebCommon/Permissions/Permissions.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/CrashReportStore.h>
#include <LibWebView/DefaultBrowser.h>
#include <LibWebView/Extensions/ExtensionManager.h>
#include <LibWebView/GamepadManager.h>
#include <LibWebView/IdentityManager.h>
#include <LibWebView/ImageDecoderManager.h>
#include <LibWebView/MediaSessionCoordinator.h>
#include <LibWebView/MicrophoneAuthorizer.h>
#include <LibWebView/PageTranslation.h>
#include <LibWebView/PermissionManager.h>
#include <LibWebView/PrivacyActivity.h>
#include <LibWebView/PrivacyPolicyCoordinator.h>
#include <LibWebView/ProcessSnapshot.h>
#include <LibWebView/ReportSubmission.h>
#include <LibWebView/RequestServerManager.h>
#include <LibWebView/Settings.h>
#include <LibWebView/TabPerformanceMonitor.h>
#include <LibWebView/UpdateCheck.h>
#include <LibWebView/WebCompatReport.h>
#include <LibWebView/WebUI/ApplicationWebUI.h>

namespace Hub {

static ConnectionFromUI* s_the;

ConnectionFromUI::ConnectionFromUI(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionFromClient<HubClientEndpoint, HubServerEndpoint>(*this, move(transport), 1)
{
    s_the = this;

    // NB: The UI reads the gamepads.
    WebView::GamepadManager::the().set_source(make<GamepadSourceInUI>());
    Application::settings().on_persist = [this](JsonValue const& settings) {
        async_settings_changed(settings.serialized().to_byte_string(), m_last_applied_settings_change_id);
    };
    WebView::ApplicationWebUI::set_callbacks({
        .on_open = [this](WebView::ApplicationWebUIContext const& ctx) {
            async_web_ui_opened(Hub::WebUIPageContext {
                .web_ui_id = ctx.web_ui_id,
                .document_generation = ctx.document_generation,
                .view_id = ctx.view_id,
                .browsing_session_id = ctx.browsing_session_id,
                .host = ctx.host,
                .is_private = ctx.is_private,
                .identity_id = ctx.identity_id,
                .identity_name = ctx.identity_name,
                .is_ephemeral = ctx.is_ephemeral,
                .persists_history = ctx.persists_history,
            });
        },
        .on_message = [this](u64 id, String name, JsonValue data) { async_web_ui_message(id, move(name), move(data)); },
        .on_close = [this](u64 id) { async_web_ui_closed(id); },
    });
    (void)WebView::TabPerformanceMonitor::the();

    auto& browser_process = Application::the().browser_process();
    browser_process.on_new_tab = [this](auto const& urls) { async_app_open_new_tab(urls); };
    browser_process.on_new_window = [this](auto const& urls) { async_app_open_new_window(urls); };

    Application::privacy_policy_coordinator().on_policy_committed = [this](WebView::BrowsingSession const& session, Privacy::PolicySnapshot const& snapshot) {
        if (&session == &Application::default_session())
            async_app_privacy_network_mode_changed(snapshot.network.mode, snapshot.epoch);
    };
}

ConnectionFromUI::~ConnectionFromUI()
{
    Application::privacy_policy_coordinator().on_policy_committed = nullptr;
    WebView::ApplicationWebUI::set_callbacks({});
    Application::settings().on_persist = nullptr;
    auto& browser_process = Application::the().browser_process();
    browser_process.on_new_tab = nullptr;
    browser_process.on_new_window = nullptr;
    s_the = nullptr;
}

ConnectionFromUI* ConnectionFromUI::the()
{
    return s_the;
}

void ConnectionFromUI::die()
{
    // The UI is gone, which ends the browser, however that came to be.
    Core::EventLoop::current().quit(0);
}

// What the UI knows an input event by, while the Hub has it. The event carries it through the view's queue, in place of
// the data about the event that only the UI can hold. Once the event is gone, the UI can let go of that data.
struct InputFromUI final : public Web::BrowserInputData {
    InputFromUI(u64 view_id, u64 input_id)
        : view_id(view_id)
        , input_id(input_id)
    {
    }

    virtual ~InputFromUI() override
    {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_release_input(view_id, input_id);
    }

    u64 view_id { 0 };
    u64 input_id { 0 };
};

Optional<u64> ConnectionFromUI::input_id_of(Web::BrowserInputData const* browser_data)
{
    if (auto const* input_from_ui = as_if<InputFromUI>(browser_data))
        return input_from_ui->input_id;
    return {};
}

template<typename EventType>
static void enqueue_input_from_ui(View& view, u64 input_id, EventType event)
{
    event.browser_data = make<InputFromUI>(view.view_id(), input_id);
    view.enqueue_input_event(move(event));
    view.push_state_to_ui();
}

void ConnectionFromUI::view_enqueue_key_event(u64 view_id, u64 input_id, Web::KeyEvent event)
{
    if (auto* view = find_view(view_id))
        enqueue_input_from_ui(*view, input_id, move(event));
}

void ConnectionFromUI::view_enqueue_mouse_event(u64 view_id, u64 input_id, Web::MouseEvent event)
{
    if (auto* view = find_view(view_id))
        enqueue_input_from_ui(*view, input_id, move(event));
}

void ConnectionFromUI::view_enqueue_drag_event(u64 view_id, u64 input_id, Web::DragEvent event)
{
    if (auto* view = find_view(view_id))
        enqueue_input_from_ui(*view, input_id, move(event));
}

void ConnectionFromUI::view_enqueue_pinch_event(u64 view_id, Web::PinchEvent event)
{
    if (auto* view = find_view(view_id)) {
        view->enqueue_input_event(move(event));
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::release_backing_store(u64 page_id, i32 bitmap_id)
{
    // NB: A page can outlive its view, and a view can stop showing a page, so this goes by the page.
    Web::PageId id { page_id };
    auto client = WebView::WebContentClient::client_for_compositor_context_id(Web::compositor_context_id_for_page(id));
    if (!client.has_value())
        return;
    if (auto* page = client->page(id))
        page->release_presented_bitmap(bitmap_id);
}

void ConnectionFromUI::view_answer(u64 view_id, u64 answer_id, bool answer)
{
    if (auto* view = find_view(view_id)) {
        view->did_get_answer(answer_id, answer);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_answer_permission_request(u64 view_id, u64 answer_id, Web::Permissions::PromptOutcome outcome)
{
    if (auto* view = find_view(view_id)) {
        view->did_get_permission_answer(answer_id, outcome);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::active_view_changed(Optional<u64> view_id)
{
    Application::the().set_active_view(view_id);
    if (auto& manager = Application::default_session().extension_manager; manager) {
        auto* view = view_id.has_value() ? find_view(*view_id) : nullptr;
        bool ordinary_window = view && view->is_tab() && view->is_private() == WebView::IsPrivate::No && view->extension_window_id() != 0;
        manager->notify_window_focus_changed(ordinary_window ? Optional<u64> { view->extension_window_id() } : Optional<u64> {});
    }
}

void ConnectionFromUI::answer_clipboard_text(u64 request_id, Utf16String text)
{
    Application::the().did_read_clipboard_text(request_id, move(text));
}

void ConnectionFromUI::answer_clipboard_item(u64 request_id, Web::Clipboard::SystemClipboardItem item)
{
    Application::the().did_read_clipboard_item(request_id, move(item));
}

void ConnectionFromUI::answer_download_path(u64 request_id, Optional<ByteString> path)
{
    Application::the().did_get_download_path(request_id, move(path));
}

static DownloadState state_of_download(WebView::FileDownloader::Download const& download)
{
    DownloadState state;
    state.id = download.id;
    state.is_private = download.is_private;
    state.identity_id = download.identity_id;
    state.url = download.url;
    state.destination = download.destination.string();
    state.status = download.status;
    state.downloaded_size = download.downloaded_size;
    state.total_size = download.total_size;
    state.error = download.error;
    state.can_resume = download.can_resume;
    state.connection_count = download.connection_count;
    state.bytes_per_second = download.bytes_per_second;
    state.is_waiting_to_retry = download.is_waiting_to_retry;
    state.created_time = download.created_time;
    state.segments = Application::the().file_downloader().segment_progress(download.id);
    return state;
}

Vector<DownloadState> ConnectionFromUI::downloads()
{
    Vector<DownloadState> downloads;
    for (auto const& download : Application::the().file_downloader().downloads())
        downloads.append(state_of_download(download));
    return downloads;
}

void ConnectionFromUI::download_added(WebView::FileDownloader::Download const& download)
{
    async_download_added(state_of_download(download));
}

void ConnectionFromUI::download_updated(WebView::FileDownloader::Download const& download)
{
    async_download_updated(state_of_download(download));
}

void ConnectionFromUI::download_removed(u64 id, WebView::IsPrivate is_private)
{
    async_download_removed(id, is_private);
}

void ConnectionFromUI::cancel_download(u64 id)
{
    Application::the().file_downloader().cancel_download(id);
}

void ConnectionFromUI::pause_download(u64 id)
{
    Application::the().file_downloader().pause_download(id);
}

void ConnectionFromUI::resume_download(u64 id)
{
    Application::the().file_downloader().resume_download(id);
}

void ConnectionFromUI::prune_inactive_downloads(WebView::IsPrivate is_private)
{
    (void)Application::the().file_downloader().prune_inactive_downloads(is_private);
}

void ConnectionFromUI::pause_active_downloads()
{
    Application::the().file_downloader().pause_active_downloads();
}

void ConnectionFromUI::cancel_unresumable_downloads()
{
    Application::the().file_downloader().cancel_unresumable_downloads();
}

void ConnectionFromUI::pip_did_resize(u64 view_id, Gfx::IntSize size)
{
    if (auto* window = PictureInPictureWindowInUI::find(view_id))
        window->did_resize(size);
}

void ConnectionFromUI::pip_did_close(u64 view_id, bool video_keeps_playing)
{
    if (auto* window = PictureInPictureWindowInUI::find(view_id))
        window->did_close(video_keeps_playing);
}

void ConnectionFromUI::geolocation_position(u64 id, Core::GeolocationCoordinates coordinates)
{
    if (auto* provider = GeolocationProviderInUI::the())
        provider->did_receive_position(id, move(coordinates));
}

void ConnectionFromUI::geolocation_error(u64 id, Core::GeolocationError error)
{
    if (auto* provider = GeolocationProviderInUI::the())
        provider->did_receive_error(id, move(error));
}

void ConnectionFromUI::geolocation_authorization_changed(Core::GeolocationAuthorization authorization)
{
    if (auto* provider = GeolocationProviderInUI::the())
        provider->did_change_authorization(authorization);
}

void ConnectionFromUI::microphone_authorization_changed(WebView::MicrophoneAuthorization authorization)
{
    if (auto* authorizer = MicrophoneAuthorizerInUI::the())
        authorizer->did_change_authorization(authorization);
}

void ConnectionFromUI::default_browser_status_changed(WebView::DefaultBrowserStatus status)
{
    Application::the().default_browser().did_read_status(status);
}

void ConnectionFromUI::default_browser_make_default_finished(bool failed, Optional<String> system_message)
{
    if (auto* default_browser = DefaultBrowserInUI::the())
        default_browser->did_make_default({ .failed = failed, .system_message = move(system_message) });
}

void ConnectionFromUI::gamepad_connected(Web::Gamepad::GamepadDescription description, Web::Gamepad::GamepadState state)
{
    if (auto* source = GamepadSourceInUI::the(); source && source->on_gamepad_connected)
        source->on_gamepad_connected(move(description), move(state));
}

void ConnectionFromUI::gamepad_disconnected(u32 handle)
{
    if (auto* source = GamepadSourceInUI::the(); source && source->on_gamepad_disconnected)
        source->on_gamepad_disconnected(handle);
}

void ConnectionFromUI::gamepad_states_changed(Vector<Web::Gamepad::GamepadState> states)
{
    if (auto* source = GamepadSourceInUI::the(); source && source->on_gamepad_states_changed)
        source->on_gamepad_states_changed(move(states));
}

void ConnectionFromUI::translator_language_detected(u64 request_id, Optional<String> language)
{
    if (auto* translator = TranslatorInUI::the())
        translator->did_detect_language(request_id, move(language));
}

void ConnectionFromUI::translator_target_languages_ready(u64 request_id, Vector<WebView::Translator::TargetLanguage> target_languages)
{
    if (auto* translator = TranslatorInUI::the())
        translator->did_get_target_languages(request_id, move(target_languages));
}

void ConnectionFromUI::translator_translation_finished(u64 request_id, Optional<Vector<Web::TranslationRun>> runs)
{
    if (auto* translator = TranslatorInUI::the())
        translator->did_translate(request_id, move(runs));
}

void ConnectionFromUI::translator_languages_changed()
{
    if (auto* translator = TranslatorInUI::the(); translator && translator->on_languages_changed)
        translator->on_languages_changed();
}

void ConnectionFromUI::spell_checker_checked(u64 request_id, Vector<Web::SpellingError> errors)
{
    if (auto* spell_checker = SpellCheckerInUI::the())
        spell_checker->did_check(request_id, move(errors));
}

void ConnectionFromUI::spell_checker_guessed(u64 request_id, Vector<Utf16String> guesses)
{
    if (auto* spell_checker = SpellCheckerInUI::the())
        spell_checker->did_guess(request_id, move(guesses));
}

void ConnectionFromUI::spell_checker_suggested(u64 request_id, Optional<Web::WritingSuggestion> suggestion)
{
    if (auto* spell_checker = SpellCheckerInUI::the())
        spell_checker->did_suggest(request_id, move(suggestion));
}

void ConnectionFromUI::system_time_zone_changed()
{
    WebView::WebContentClient::for_each_client([](WebView::WebContentClient& client) {
        client.async_system_time_zone_changed();
        return IterationDecision::Continue;
    });
}

void ConnectionFromUI::media_session_action(Web::Bindings::MediaSessionAction action, Optional<double> seek_offset, Optional<double> seek_time, Optional<bool> fast_seek)
{
    Application::the().media_session_coordinator().handle_action(action, seek_offset, seek_time, fast_seek);
}

void ConnectionFromUI::notification_shown(u64 id)
{
    if (auto* backend = NotificationBackendInUI::the(); backend && backend->on_shown)
        backend->on_shown(id);
}

void ConnectionFromUI::notification_failed(u64 id)
{
    if (auto* backend = NotificationBackendInUI::the(); backend && backend->on_failed)
        backend->on_failed(id);
}

void ConnectionFromUI::notification_clicked(u64 id)
{
    if (auto* backend = NotificationBackendInUI::the(); backend && backend->on_clicked)
        backend->on_clicked(id);
}

void ConnectionFromUI::notification_closed(u64 id, bool by_user)
{
    if (auto* backend = NotificationBackendInUI::the(); backend && backend->on_closed)
        backend->on_closed(id, by_user ? WebView::NotificationBackend::ByUser::Yes : WebView::NotificationBackend::ByUser::No);
}

void ConnectionFromUI::external_url_handler_resolved(u64 request_id, Optional<u64> handler_id, String application_name, bool is_ladybird)
{
    Application::the().did_resolve_external_url_handler(request_id, handler_id, move(application_name), is_ladybird);
}

void ConnectionFromUI::external_url_launched(u64 request_id, bool launched)
{
    Application::the().did_launch_external_url(request_id, launched);
}

void ConnectionFromUI::view_set_site_permission(u64 view_id, URL::Origin origin, Web::Permissions::PermissionName name, Optional<WebView::PermissionDecision> decision)
{
    if (auto* view = find_view(view_id)) {
        view->set_site_permission(origin, name, decision);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_did_show_permission_prompt(u64 view_id, WebView::PermissionPromptRequest request)
{
    if (auto* view = find_view(view_id))
        view->did_show_permission_prompt(request);
}

void ConnectionFromUI::view_load_navigation(u64 view_id, Web::HTML::PreparedNavigationDescriptor navigation)
{
    if (auto* view = find_view(view_id))
        view->load(move(navigation));
}

void ConnectionFromUI::view_open_navigation_in_new_tab(u64 view_id, Web::HTML::PreparedNavigationDescriptor navigation, Web::HTML::ActivateTab activate_tab)
{
    if (auto* view = find_view(view_id))
        view->open_navigation_in_new_tab(move(navigation), activate_tab);
}

void ConnectionFromUI::view_open_navigation_in_new_window(u64 view_id, Web::HTML::PreparedNavigationDescriptor navigation, WebView::IsPrivate is_private)
{
    if (auto* view = find_view(view_id))
        view->open_navigation_in_new_window(move(navigation), is_private);
}

void ConnectionFromUI::view_traverse_the_history_to_step(u64 view_id, i32 step)
{
    if (auto* view = find_view(view_id))
        view->traverse_the_history_to_step(step);
}

void ConnectionFromUI::view_set_reports_session_history(u64 view_id, bool reports_session_history)
{
    if (auto* view = find_view(view_id))
        view->set_reports_session_history(reports_session_history);
}

void ConnectionFromUI::view_set_compositor_layer_hidden(u64 view_id, u64 layer_id, bool hidden)
{
    if (auto* view = find_view(view_id))
        view->set_compositor_layer_hidden(layer_id, hidden);
}

void ConnectionFromUI::view_set_selected_compositor_layers(u64 view_id, Vector<u64> layer_ids)
{
    if (auto* view = find_view(view_id))
        view->set_selected_compositor_layers(move(layer_ids));
}

void ConnectionFromUI::view_set_hovered_compositor_layer(u64 view_id, Optional<u64> layer_id)
{
    if (auto* view = find_view(view_id))
        view->set_hovered_compositor_layer(layer_id);
}

void ConnectionFromUI::view_show_only_compositor_layers(u64 view_id, Vector<u64> layer_ids)
{
    if (auto* view = find_view(view_id))
        view->show_only_compositor_layers(layer_ids);
}

void ConnectionFromUI::view_show_all_compositor_layers(u64 view_id)
{
    if (auto* view = find_view(view_id))
        view->show_all_compositor_layers();
}

void ConnectionFromUI::view_reset_compositor_layer_raster_counts(u64 view_id)
{
    if (auto* view = find_view(view_id))
        view->reset_compositor_layer_raster_counts();
}

void ConnectionFromUI::view_file_picker_closed(u64 view_id, Vector<Web::HTML::SelectedFile> selected_files)
{
    if (auto* view = find_view(view_id))
        view->file_picker_closed(move(selected_files));
}

void ConnectionFromUI::view_save_screenshot(u64 view_id, Gfx::ShareableBitmap bitmap)
{
    auto* view = find_view(view_id);
    if (view && bitmap.is_valid())
        view->save_screenshot(*bitmap.bitmap());
}

void ConnectionFromUI::view_request_selected_text(u64 view_id, u64 request_id)
{
    auto* view = find_view(view_id);
    if (!view) {
        async_view_selected_text(view_id, request_id, {});
        return;
    }
    auto promise = view->selected_text();
    promise->when_resolved([this, view_id, request_id](ByteString& text) {
        async_view_selected_text(view_id, request_id, text);
    });
    promise->when_rejected([this, view_id, request_id](Error const&) {
        async_view_selected_text(view_id, request_id, {});
    });
}

void ConnectionFromUI::view_request_close_preflight(u64 view_id, u64 request_id)
{
    auto* view = find_view(view_id);
    if (!view) {
        async_view_close_preflight_finished(view_id, request_id, true);
        return;
    }
    view->request_close_preflight([this, view_id, request_id](bool may_close) {
        async_view_close_preflight_finished(view_id, request_id, may_close);
    });
}

void ConnectionFromUI::view_request_page_dumps(u64 view_id, u64 request_id)
{
    auto* view = find_view(view_id);
    if (!view) {
        async_view_page_dumps(view_id, request_id, {});
        return;
    }
    auto promise = WebView::WebCompatReport::request_page_dumps(*view);
    promise->when_resolved([this, view_id, request_id](String& dumps) {
        async_view_page_dumps(view_id, request_id, dumps);
    });
    promise->when_rejected([this, view_id, request_id](Error const&) {
        async_view_page_dumps(view_id, request_id, {});
    });
}

void ConnectionFromUI::request_platform_features_description(u64 request_id)
{
    async_platform_features_description(request_id, Application::the().describe_platform_features_for_report({}));
}

void ConnectionFromUI::view_prepare_for_immediate_close(u64 view_id)
{
    auto* view = find_view(view_id);
    if (!view)
        return;
    // NB: The UI looked before it asked, but a page can start to need a beforeunload check at any moment, and the user
    //     still wants the tab gone. A page must not be able to take the Hub down by racing that, and the UI has
    //     already closed the tab, so the page closes without prompting.
    if (view->needs_beforeunload_check()) {
        view->prepare_for_close_without_prompting()();
        return;
    }
    view->prepare_for_immediate_close();
}

void ConnectionFromUI::view_prepare_for_close_without_prompting(u64 view_id, u64 close_id)
{
    if (auto* view = find_view(view_id))
        m_prepared_closes.set(close_id, view->prepare_for_close_without_prompting());
}

void ConnectionFromUI::finish_prepared_close(u64 close_id)
{
    if (auto close = m_prepared_closes.take(close_id); close.has_value())
        (*close)();
}

void ConnectionFromUI::add_web_ui_page(String host, String title)
{
    WebView::WebUI::add_application_page(move(host), move(title));
}

void ConnectionFromUI::send_web_ui_message(u64 web_ui_id, String name, JsonValue data)
{
    if (auto* web_ui = WebView::ApplicationWebUI::find(web_ui_id))
        web_ui->send_to_page(name, data);
}

Messages::HubServer::WebUiConsumeUserActivationResponse ConnectionFromUI::web_ui_consume_user_activation(u64 web_ui_id)
{
    auto* web_ui = WebView::ApplicationWebUI::find(web_ui_id);
    return web_ui && web_ui->consume_user_activation();
}

static bool is_public_https_dashboard_destination(URL::URL const& url)
{
    if (url.scheme() != "https"sv)
        return false;
    if (!url.username().is_empty() || !url.password().is_empty())
        return false;
    if (!url.host().has_value() || url.host()->is_empty_host())
        return false;

    auto const& host = *url.host();
    if (host.is_loopback_or_localhost())
        return false;

    if (host.has<IPv4Address>()) {
        auto ip = host.get<IPv4Address>();
        u8 a = ip[0];
        u8 b = ip[1];
        // Reject 0.0.0.0/8, 10.0.0.0/8, 100.64.0.0/10, 127.0.0.0/8, 169.254.0.0/16,
        // 172.16.0.0/12, 192.168.0.0/16, 224.0.0.0/4 (multicast), 240.0.0.0/4 (reserved).
        if (a == 0 || a == 10 || a == 127 || a >= 224)
            return false;
        if (a == 100 && (b >= 64 && b <= 127))
            return false;
        if (a == 169 && b == 254)
            return false;
        if (a == 172 && (b >= 16 && b <= 31))
            return false;
        if (a == 192 && b == 168)
            return false;
        return true;
    }

    if (host.has<IPv6Address>()) {
        auto ip = host.get<IPv6Address>();
        if (ip.is_zero() || ip == IPv6Address::loopback() || ip.is_ipv4_mapped())
            return false;
        u16 first = ip[0];
        // fc00::/7 ULA, fe80::/10 link-local, ff00::/8 multicast
        if ((first & 0xfe00) == 0xfc00 || (first & 0xffc0) == 0xfe80 || (first & 0xff00) == 0xff00)
            return false;
        return true;
    }

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

Messages::HubServer::WebUiPrivacySummaryResponse ConnectionFromUI::web_ui_privacy_summary(u64 web_ui_id)
{
    JsonObject result;
    auto* web_ui = WebView::ApplicationWebUI::find(web_ui_id);
    auto* session = web_ui ? web_ui->session() : nullptr;
    if (!session) {
        result.set("available"sv, false);
        return JsonValue { move(result) };
    }

    auto& coordinator = Application::privacy_policy_coordinator();
    auto const& snapshot = coordinator.snapshot_for_session(*session);
    auto counters = Application::privacy_activity().counters_for(*session);

    result.set("available"sv, true);
    result.set("identityId"sv, session->identity().id);
    result.set("identityName"sv, session->identity().display_name);
    result.set("identityKind"sv, Privacy::identity_kind_to_string(session->identity().kind));
    result.set("networkMode"sv, Privacy::network_mode_to_string(coordinator.network_mode(*session)));
    result.set("epoch"sv, coordinator.committed_epoch(*session));
    if (snapshot.network.route.has_value())
        result.set("route"sv, MUST(String::from_byte_string(snapshot.network.route->redacted_description())));
    else
        result.set("route"sv, JsonValue {});
    result.set("localDNS"sv, coordinator.allows_dns_resolution(*session));
    result.set("webRTC"sv, coordinator.allows_webrtc_network(*session));
    result.set("allowsEgress"sv, coordinator.allows_network_egress(*session));
    result.set("allowsBackgroundServices"sv, coordinator.allows_background_services(*session));
    result.set("managedPolicy"sv, Privacy::managed_policy_status_to_string(Application::managed_privacy_policy().status));

    JsonObject counts;
    counts.set("navigationsSanitized"sv, counters.navigations_sanitized);
    counts.set("trackingParametersRemoved"sv, counters.tracking_parameters_removed);
    counts.set("navigationsWithScriptingBlocked"sv, counters.navigations_with_scripting_blocked);
    result.set("counters"sv, move(counts));
    return JsonValue { move(result) };
}

Messages::HubServer::WebUiExtensionsSnapshotResponse ConnectionFromUI::web_ui_extensions_snapshot(u64 web_ui_id)
{
    JsonObject result;
    auto* web_ui = WebView::ApplicationWebUI::find(web_ui_id);
    auto* session = web_ui ? web_ui->session() : nullptr;
    if (!session || session->is_private() == WebView::IsPrivate::Yes || session != &Application::default_session() || !session->extension_manager) {
        result.set("available"sv, false);
        result.set("reason"sv, "Extensions are only available in the ordinary browsing session."_string);
        return JsonValue { move(result) };
    }
    result.set("available"sv, true);
    result.set("summary"sv, session->extension_manager->summary_json());
    return JsonValue { move(result) };
}

Messages::HubServer::WebUiSetExtensionEnabledResponse ConnectionFromUI::web_ui_set_extension_enabled(u64 web_ui_id, String extension_id, bool enabled)
{
    JsonObject result;
    auto* web_ui = WebView::ApplicationWebUI::find(web_ui_id);
    auto* session = web_ui ? web_ui->session() : nullptr;
    if (!web_ui || !session || session->is_private() == WebView::IsPrivate::Yes || session != &Application::default_session() || !session->extension_manager) {
        result.set("ok"sv, false);
        result.set("error"sv, "Extensions are unavailable in this context."_string);
        return JsonValue { move(result) };
    }
    if (!web_ui->consume_user_activation()) {
        result.set("ok"sv, false);
        result.set("error"sv, "User interaction is required."_string);
        return JsonValue { move(result) };
    }
    auto ext = session->extension_manager->extension_by_id(extension_id);
    if (!ext.has_value()) {
        result.set("ok"sv, false);
        result.set("stale"sv, true);
        result.set("error"sv, "Extension is no longer installed."_string);
        result.set("summary"sv, session->extension_manager->summary_json());
        return JsonValue { move(result) };
    }
    if (auto err = session->extension_manager->set_extension_enabled(extension_id, enabled); err.is_error()) {
        result.set("ok"sv, false);
        result.set("error"sv, MUST(String::formatted("{}", err.error())));
    } else {
        result.set("ok"sv, true);
    }
    result.set("summary"sv, session->extension_manager->summary_json());
    return JsonValue { move(result) };
}

Messages::HubServer::WebUiOpenExtensionOptionsResponse ConnectionFromUI::web_ui_open_extension_options(u64 web_ui_id, String extension_id)
{
    auto* web_ui = WebView::ApplicationWebUI::find(web_ui_id);
    auto* session = web_ui ? web_ui->session() : nullptr;
    if (!web_ui || !session || session->is_private() == WebView::IsPrivate::Yes || session != &Application::default_session() || !session->extension_manager)
        return false;
    if (!web_ui->consume_user_activation())
        return false;
    auto ext = session->extension_manager->extension_by_id(extension_id);
    if (!ext.has_value() || !ext->enabled || !ext->manifest.options_ui().has_value())
        return false;
    auto url = session->extension_manager->package_page_url(*ext, ext->manifest.options_ui()->page);
    if (!url.has_value())
        return false;
    auto* source_view = web_ui->view_id().has_value() ? find_view(*web_ui->view_id()) : nullptr;
    WebView::Application::the().open_url_in_new_tab(*url, Web::HTML::ActivateTab::Yes, source_view);
    return true;
}

Messages::HubServer::WebUiPermissionsSnapshotResponse ConnectionFromUI::web_ui_permissions_snapshot(u64 web_ui_id)
{
    JsonObject result;
    auto* web_ui = WebView::ApplicationWebUI::find(web_ui_id);
    auto* session = web_ui ? web_ui->session() : nullptr;
    if (!session || !session->permission_manager) {
        result.set("available"sv, false);
        return JsonValue { move(result) };
    }

    auto& pm = *session->permission_manager;
    result.set("available"sv, true);
    result.set("inheritsOrdinary"sv, pm.inherits_ordinary_session());

    JsonArray entries;
    JsonObject defaults;
    for (auto name : Web::Permissions::PERMISSION_NAMES) {
        auto name_str = Web::Permissions::to_string(name);
        auto def = pm.global_default(name);
        defaults.set(name_str, def == Web::Permissions::EffectiveState::Denied ? "block"sv : "ask"sv);
        for (auto const& site : pm.site_decisions(name)) {
            JsonObject row;
            row.set("origin"sv, site.origin.serialize());
            row.set("permission"sv, name_str);
            switch (site.decision) {
            case WebView::PermissionDecision::Allow:
                row.set("decision"sv, "allow"_string);
                break;
            case WebView::PermissionDecision::AllowOnce:
                row.set("decision"sv, "allow-once"_string);
                break;
            case WebView::PermissionDecision::Block:
                row.set("decision"sv, "block"_string);
                break;
            }
            row.set("generation"sv, pm.decision_generation(name, site.origin));
            entries.must_append(move(row));
        }
    }
    result.set("defaults"sv, move(defaults));
    result.set("entries"sv, move(entries));
    return JsonValue { move(result) };
}

Messages::HubServer::WebUiRevokeSitePermissionResponse ConnectionFromUI::web_ui_revoke_site_permission(u64 web_ui_id, String origin_str, String permission_name, u64 expected_generation)
{
    JsonObject result;
    auto* web_ui = WebView::ApplicationWebUI::find(web_ui_id);
    auto* session = web_ui ? web_ui->session() : nullptr;
    if (!web_ui || !session || !session->permission_manager) {
        result.set("ok"sv, false);
        return JsonValue { move(result) };
    }
    if (!web_ui->consume_user_activation()) {
        result.set("ok"sv, false);
        result.set("error"sv, "User interaction is required."_string);
        return JsonValue { move(result) };
    }
    auto parsed_name = Web::Permissions::permission_name_from_string(permission_name);
    auto parsed_url = URL::Parser::basic_parse(origin_str);
    if (!parsed_name.has_value() || !parsed_url.has_value()) {
        result.set("ok"sv, false);
        result.set("error"sv, "Invalid permission target."_string);
        return JsonValue { move(result) };
    }
    auto origin = parsed_url->origin();
    auto& pm = *session->permission_manager;
    auto current_gen = pm.decision_generation(*parsed_name, origin);
    if (current_gen != expected_generation) {
        result.set("ok"sv, false);
        result.set("stale"sv, true);
        result.set("error"sv, "Permission changed since display."_string);
        return JsonValue { move(result) };
    }
    pm.set_decision(*parsed_name, origin, {});
    result.set("ok"sv, true);
    return JsonValue { move(result) };
}

Messages::HubServer::WebUiIdentitiesSnapshotResponse ConnectionFromUI::web_ui_identities_snapshot(u64 web_ui_id)
{
    JsonObject result;
    auto* web_ui = WebView::ApplicationWebUI::find(web_ui_id);
    auto* session = web_ui ? web_ui->session() : nullptr;
    if (!session) {
        result.set("available"sv, false);
        return JsonValue { move(result) };
    }
    result.set("available"sv, true);
    result.set("isPrivate"sv, session->is_private() == WebView::IsPrivate::Yes);
    result.set("isEphemeral"sv, session->is_ephemeral());
    result.set("persistsHistory"sv, session->persists_history());
    result.set("profileIdentifier"sv, Application::profile().identifier());
    result.set("profileIsTemporary"sv, Application::profile().is_temporary());

    JsonObject current;
    current.set("id"sv, session->identity().id);
    current.set("name"sv, session->identity().display_name);
    current.set("kind"sv, Privacy::identity_kind_to_string(session->identity().kind));
    result.set("currentIdentity"sv, move(current));

    JsonArray identities;
    if (session->is_private() == WebView::IsPrivate::No && !session->is_ephemeral()) {
        for (auto const& id : Application::identity_manager().identities()) {
            JsonObject item;
            item.set("id"sv, id.id);
            item.set("name"sv, id.display_name);
            item.set("kind"sv, Privacy::identity_kind_to_string(id.kind));
            item.set("ephemeral"sv, id.is_ephemeral());
            identities.must_append(move(item));
        }
    }
    result.set("identities"sv, move(identities));
    return JsonValue { move(result) };
}

static StringView media_action_to_string(Web::Bindings::MediaSessionAction action)
{
    switch (action) {
    case Web::Bindings::MediaSessionAction::Play:
        return "play"sv;
    case Web::Bindings::MediaSessionAction::Pause:
        return "pause"sv;
    case Web::Bindings::MediaSessionAction::Seekbackward:
        return "seekbackward"sv;
    case Web::Bindings::MediaSessionAction::Seekforward:
        return "seekforward"sv;
    case Web::Bindings::MediaSessionAction::Previoustrack:
        return "previoustrack"sv;
    case Web::Bindings::MediaSessionAction::Nexttrack:
        return "nexttrack"sv;
    case Web::Bindings::MediaSessionAction::Seekto:
        return "seekto"sv;
    case Web::Bindings::MediaSessionAction::Stop:
        return "stop"sv;
    default:
        return ""sv;
    }
}

static Optional<Web::Bindings::MediaSessionAction> media_action_from_string(StringView action)
{
    if (action == "play"sv)
        return Web::Bindings::MediaSessionAction::Play;
    if (action == "pause"sv)
        return Web::Bindings::MediaSessionAction::Pause;
    if (action == "previoustrack"sv)
        return Web::Bindings::MediaSessionAction::Previoustrack;
    if (action == "nexttrack"sv)
        return Web::Bindings::MediaSessionAction::Nexttrack;
    if (action == "seekbackward"sv)
        return Web::Bindings::MediaSessionAction::Seekbackward;
    if (action == "seekforward"sv)
        return Web::Bindings::MediaSessionAction::Seekforward;
    if (action == "seekto"sv)
        return Web::Bindings::MediaSessionAction::Seekto;
    if (action == "stop"sv)
        return Web::Bindings::MediaSessionAction::Stop;
    return {};
}

Messages::HubServer::WebUiMediaSessionsSnapshotResponse ConnectionFromUI::web_ui_media_sessions_snapshot(u64 web_ui_id)
{
    JsonObject result;
    auto* web_ui = WebView::ApplicationWebUI::find(web_ui_id);
    auto* session = web_ui ? web_ui->session() : nullptr;
    if (!session) {
        result.set("sessions"sv, JsonArray {});
        return JsonValue { move(result) };
    }

    auto& coordinator = Application::the().media_session_coordinator();
    JsonArray sessions;
    WebView::ViewImplementation::for_each_view([&](WebView::ViewImplementation& view) {
        if (&view.session() != session)
            return IterationDecision::Continue;
        auto state = coordinator.state_for_view(view.view_id());
        if (!state.has_value())
            return IterationDecision::Continue;

        JsonObject item;
        item.set("viewId"sv, view.view_id());
        item.set("tabTitle"sv, view.title().to_utf8());
        item.set("title"sv, state->title);
        item.set("artist"sv, state->artist);
        item.set("album"sv, state->album);
        item.set("playing"sv, state->playback_state == Web::Bindings::MediaSessionPlaybackState::Playing);
        item.set("activationCounter"sv, state->activation_counter);

        JsonArray actions;
        for (auto action : state->supported_actions) {
            auto s = media_action_to_string(action);
            if (!s.is_empty())
                actions.must_append(MUST(String::from_utf8(s)));
        }
        item.set("supportedActions"sv, move(actions));

        if (state->position.has_value()) {
            JsonObject pos;
            pos.set("duration"sv, state->position->duration);
            pos.set("position"sv, state->position->position);
            pos.set("playbackRate"sv, state->position->playback_rate);
            item.set("position"sv, move(pos));
        }
        sessions.must_append(move(item));
        return IterationDecision::Continue;
    });

    result.set("sessions"sv, move(sessions));
    return JsonValue { move(result) };
}

Messages::HubServer::WebUiMediaSessionActionResponse ConnectionFromUI::web_ui_media_session_action(u64 web_ui_id, u64 target_view_id, String action_name, Optional<double> seek_time)
{
    auto* web_ui = WebView::ApplicationWebUI::find(web_ui_id);
    auto* session = web_ui ? web_ui->session() : nullptr;
    if (!web_ui || !session)
        return false;
    auto action = media_action_from_string(action_name);
    if (!action.has_value())
        return false;

    WebView::ViewImplementation* target_view = nullptr;
    WebView::ViewImplementation::for_each_view([&](WebView::ViewImplementation& view) {
        if (view.view_id() == target_view_id && &view.session() == session) {
            target_view = &view;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    if (!target_view)
        return false;

    auto& coordinator = Application::the().media_session_coordinator();
    if (!coordinator.state_for_view(target_view_id).has_value())
        return false;
    if (!web_ui->consume_user_activation())
        return false;

    coordinator.handle_action_for_view(target_view_id, *action, {}, seek_time, {});
    return true;
}

void ConnectionFromUI::web_ui_dashboard_fetch(u64 web_ui_id, u64 request_id, String kind, String target, u64 expected_policy_epoch)
{
    auto fail = [&] {
        async_request_finished(request_id, true, {}, {}, {});
    };

    auto* web_ui = WebView::ApplicationWebUI::find(web_ui_id);
    auto* session = web_ui ? web_ui->session() : nullptr;
    if (!session || session->is_private() == WebView::IsPrivate::Yes || session->is_ephemeral()) {
        fail();
        return;
    }

    auto& coordinator = Application::privacy_policy_coordinator();
    if (!coordinator.allows_network_egress(*session)) {
        fail();
        return;
    }
    auto current_epoch = coordinator.committed_epoch(*session);
    if (expected_policy_epoch != 0 && current_epoch != expected_policy_epoch) {
        fail();
        return;
    }

    Optional<URL::URL> fetch_url;
    if (kind == "feed"sv) {
        auto parsed = URL::Parser::basic_parse(target);
        if (!parsed.has_value() || !is_public_https_dashboard_destination(*parsed)) {
            fail();
            return;
        }
        fetch_url = parsed.release_value();
    } else if (kind == "weather"sv) {
        auto parts = target.bytes_as_string_view().split_view(',');
        if (parts.size() != 2) {
            fail();
            return;
        }
        auto lat = parts[0].trim_whitespace().to_number<double>();
        auto lon = parts[1].trim_whitespace().to_number<double>();
        if (!lat.has_value() || !lon.has_value() || !isfinite(*lat) || !isfinite(*lon) || *lat < -90.0 || *lat > 90.0 || *lon < -180.0 || *lon > 180.0) {
            fail();
            return;
        }
        double rounded_lat = round(*lat * 100.0) / 100.0;
        double rounded_lon = round(*lon * 100.0) / 100.0;
        auto weather_url_str = ByteString::formatted(
            "https://api.open-meteo.com/v1/forecast?latitude={:.2f}&longitude={:.2f}&current=temperature_2m,apparent_temperature,weather_code,wind_speed_10m,relative_humidity_2m&hourly=temperature_2m,weather_code,precipitation_probability&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max&forecast_days=5&timezone=auto",
            rounded_lat, rounded_lon);
        fetch_url = URL::Parser::basic_parse(weather_url_str);
        if (!fetch_url.has_value() || !is_public_https_dashboard_destination(*fetch_url)) {
            fail();
            return;
        }
    } else {
        fail();
        return;
    }

    auto instance_or_error = Application::request_server_manager().browser_instance(*session);
    if (instance_or_error.is_error()) {
        fail();
        return;
    }

    auto request_headers = HTTP::HeaderList::create();
    request_headers->set({ "User-Agent"sv, Web::default_user_agent });
    request_headers->set({ "Accept"sv, kind == "weather"sv ? "application/json"sv : "application/rss+xml, application/atom+xml, application/xml, text/xml;q=0.9"sv });

    auto request = instance_or_error.value()->ui_client().start_request(
        "GET"sv,
        *fetch_url,
        HTTP::Cookie::SameSiteContext::same_site(),
        *request_headers,
        {},
        HTTP::CacheMode::NoStore,
        HTTP::Cookie::IncludeCredentials::No);
    if (!request) {
        fail();
        return;
    }

    static constexpr size_t MAX_DASHBOARD_RESPONSE_BYTES = 1024 * 1024;
    struct FetchState : public RefCounted<FetchState> {
        ByteBuffer body;
        Vector<HTTP::Header> headers;
        Optional<u32> status_code;
        bool aborted { false };
        bool finished { false };
    };
    auto state = adopt_ref(*new FetchState);

    auto finish_fetch = [this, request_id, web_ui_id, current_epoch](NonnullRefPtr<FetchState> const& st, bool error_override) {
        if (st->finished)
            return;
        st->finished = true;
        if (auto timer = m_request_timers.take(request_id); timer.has_value() && *timer)
            (*timer)->stop();

        bool policy_ok = false;
        if (auto* live_ui = WebView::ApplicationWebUI::find(web_ui_id); live_ui && live_ui->session()) {
            auto& coord = Application::privacy_policy_coordinator();
            policy_ok = coord.allows_network_egress(*live_ui->session()) && coord.committed_epoch(*live_ui->session()) == current_epoch;
        }

        if (error_override || st->aborted || !policy_ok || !st->status_code.has_value() || *st->status_code < 200 || *st->status_code >= 300) {
            async_request_finished(request_id, true, st->status_code, {}, {});
        } else {
            async_request_finished(request_id, false, st->status_code, move(st->headers), ByteString::copy(st->body.bytes()));
        }

        Core::deferred_invoke([this, request_id] {
            m_requests.remove(request_id);
            m_request_timers.remove(request_id);
        });
    };

    auto timer = Core::Timer::create_single_shot(AK::Duration::from_seconds(10), [this, request_id, state, finish_fetch] {
        state->aborted = true;
        if (auto req = m_requests.get(request_id); req.has_value())
            (*req)->stop();
        finish_fetch(state, true);
    });
    timer->start();
    m_request_timers.set(request_id, timer);

    request->set_unbuffered_request_callbacks(
        [this, request_id, state](NonnullRefPtr<HTTP::HeaderList> response_headers, Optional<u32> response_code, Optional<String> const&, Requests::CacheState) {
            state->status_code = response_code;
            // Reject redirects (3xx) and non-2xx responses immediately.
            if (!response_code.has_value() || *response_code < 200 || *response_code >= 300) {
                state->aborted = true;
                if (auto req = m_requests.get(request_id); req.has_value())
                    (*req)->stop();
                return;
            }
            if (auto ct = response_headers->get("Content-Type"sv); ct.has_value())
                state->headers.append({ "Content-Type"sv, ct.release_value() });
        },
        [this, request_id, state](Requests::ResponseData data) {
            if (state->aborted)
                return;
            auto bytes = data.bytes();
            if (state->body.size() + bytes.size() > MAX_DASHBOARD_RESPONSE_BYTES) {
                state->aborted = true;
                if (auto req = m_requests.get(request_id); req.has_value())
                    (*req)->stop();
                return;
            }
            if (state->body.try_append(bytes).is_error()) {
                state->aborted = true;
                if (auto req = m_requests.get(request_id); req.has_value())
                    (*req)->stop();
            }
        },
        [state](Core::ImmutableBytes data) {
            if (state->aborted)
                return;
            if (data.size() > MAX_DASHBOARD_RESPONSE_BYTES || state->body.try_append(data.bytes()).is_error())
                state->aborted = true;
        },
        [state, finish_fetch](u64, Requests::RequestTimingInfo const&, Optional<Requests::NetworkError> network_error) {
            finish_fetch(state, network_error.has_value() || state->aborted);
        });

    m_requests.set(request_id, request.release_nonnull());
}

Messages::HubServer::InitializeResponse ConnectionFromUI::initialize()
{
    auto const& options = Application::browser_options();

    InitialState state;
    state.settings_json = Application::settings().serialize_json().serialized().to_byte_string();
    state.user_agent = Application::the().user_agent_string();
    state.navigator_compatibility_mode = Application::the().navigator_compatibility_mode();
    state.scripting_enabled = Application::the().scripting_enabled();
    state.blocks_pop_ups = Application::the().blocks_pop_ups();
    state.devtools_enabled = Application::the().devtools_enabled();
    state.sandbox_is_disabled = options.disable_sandbox == WebView::DisableSandbox::Yes;
    state.gpu_painting_enabled = Application::the().compositor_gpu_painting_enabled();
    state.downloads = downloads();
    state.urls = options.urls;
    state.raw_urls = options.raw_urls;
    state.crash_report_directory = WebView::CrashReportStore::the().directory();
    state.devtools_port = options.devtools_port;
    auto const& profile = Application::profile();
    state.config_directory = profile.paths().config;
    state.data_directory = profile.paths().data;
    state.persists_sessions = Application::the().persists_sessions();
    state.uses_sql_database = options.disable_sql_database == WebView::DisableSQLDatabase::No;
    state.profile_identifier = profile.identifier();
    state.profile_identity = profile.paths().identity;
    state.profile_is_temporary = profile.is_temporary();
    state.privacy_network_mode = Application::privacy_policy_coordinator().network_mode(Application::default_session());
    return state;
}

Messages::HubServer::CreateViewResponse ConnectionFromUI::create_view(Hub::NewViewRequest request)
{
    auto& view = Application::the().create_view({
        .is_private = request.is_private,
        .device_pixel_ratio = request.device_pixel_ratio,
        .maximum_frames_per_second = request.maximum_frames_per_second,
        .display_id = request.display_id,
        .screen_shows_high_dynamic_range = request.screen_shows_high_dynamic_range,
        .system_theme = move(request.system_theme),
        .screen_rects = move(request.screen_rects),
        .owner_view_id = request.owner_view_id,
        .websites_prefer_dark = request.websites_prefer_dark,
        .is_tab = request.is_tab,
    });
    return { view.view_id(), view.current_state() };
}

Messages::HubServer::ViewDescriptionResponse ConnectionFromUI::view_description(u64 view_id)
{
    // NB: The UI can ask about a view that the Hub has let go of already, and must not take the Hub down by that.
    auto* view = find_view(view_id);
    if (!view)
        return Hub::ViewState {};
    return { view->current_state() };
}

void ConnectionFromUI::crash_reports_changed()
{
    async_crash_reports_changed();
}

static WebView::Extensions::ExtensionManager* extension_manager_for_tab(View* view)
{
    if (!view || !view->is_tab() || view->is_private() == WebView::IsPrivate::Yes)
        return nullptr;
    auto* manager = view->session().extension_manager.ptr();
    return manager && manager->extensions_enabled() ? manager : nullptr;
}

Optional<Gfx::ShareableBitmap> ConnectionFromUI::extension_icon(URL::URL const& url, String const& version, WebView::Extensions::ExtensionManager& manager)
{
    auto key = MUST(String::formatted("{} {}", version, url));
    if (auto cached = m_extension_icons.find(key); cached != m_extension_icons.end())
        return cached->value;
    // Limit the cache and outstanding decodes even when a script continually changes its icon.
    auto host = MUST(String::from_utf8(url.serialized_host()));
    if (m_pending_icon_hosts.contains(host))
        return {};
    auto& recent = m_recent_extension_icons.ensure(host);
    if (recent.size() == 4)
        m_extension_icons.remove(recent.take_first());
    recent.append(key);
    m_extension_icons.set(key, {});
    auto resource = manager.serve_resource(url, {}, 1024 * 1024);
    if (!resource.allowed || resource.data.is_empty())
        return {};
    if (!m_icon_decoder) {
        // The browser's process ID in the process manager is zero. Its decoder is separate from page decoders.
        auto handle = Application::image_decoder_manager().connect_new_client(0, &Application::default_session(), {});
        if (handle.is_error())
            return {};
        auto transport = handle.value().create_transport();
        if (transport.is_error())
            return {};
        m_icon_decoder = make_ref_counted<ImageDecoderClient::Client>(transport.release_value());
        m_icon_decoder->on_death = [this] { m_icon_decoder = nullptr; };
    }
    auto weak_this = make_weak_ptr<ConnectionFromUI>();
    m_pending_icon_hosts.set(host);
    (void)m_icon_decoder->decode_image(resource.data.bytes(), [weak_this, key, host](auto& image) -> ErrorOr<void> {
        auto self = weak_this.strong_ref();
        if (!self)
            return {};
        self->m_pending_icon_hosts.remove(host);
        self->schedule_extension_state_refresh();
        if (image.frames.is_empty())
            return {};
        if (image.session_id && self->m_icon_decoder)
            self->m_icon_decoder->stop_animation_decode(image.session_id);
        auto& bitmap = *image.frames.first().bitmap;
        if (bitmap.width() > 512 || bitmap.height() > 512)
            return {};
        self->m_extension_icons.set(key, bitmap.to_shareable_bitmap());
        return {}; }, [weak_this, host](Error&) {
        if (auto self = weak_this.strong_ref()) {
            self->m_pending_icon_hosts.remove(host);
            self->schedule_extension_state_refresh();
        } }, Gfx::IntSize { 48, 48 }, resource.mime_type.to_byte_string());
    return {};
}

void ConnectionFromUI::populate_extension_menu_icons(Vector<WebView::ExtensionMenuItem>& items, WebView::Extensions::ExtensionManager& manager)
{
    for (auto& item : items) {
        auto extension = manager.extension_by_id(item.extension_id);
        if (!extension.has_value())
            continue;
        auto path = extension->manifest.package_icon_path();
        if (!path.has_value())
            continue;
        if (auto url = manager.package_page_url(*extension, *path); url.has_value())
            item.icon = extension_icon(*url, extension->manifest.version(), manager);
    }
}

void ConnectionFromUI::send_extension_state(View& view)
{
    auto* manager = extension_manager_for_tab(&view);
    if (!manager) {
        async_view_extension_state(view.view_id(), false, {}, 0);
        return;
    }
    Vector<ExtensionItem> items;
    auto window_id = view.extension_window_id();
    auto tab_id = view.view_id();
    for (auto const* ext : manager->all_extensions()) {
        if (!ext->enabled)
            continue;
        auto action = manager->action_controller().resolved_state(ext->extension_id, tab_id, window_id);
        auto sidebar = manager->sidebar_controller().resolved_state(ext->extension_id, tab_id, window_id);
        Optional<URL::URL> sidebar_url;
        if (sidebar.is_open && sidebar.panel_url.has_value())
            sidebar_url = manager->package_page_url(*ext, *sidebar.panel_url);
        auto resolve_icon = [&](Optional<String> const& path) -> Optional<Gfx::ShareableBitmap> {
            if (path.has_value()) {
                if (auto url = manager->package_page_url(*ext, *path); url.has_value())
                    return extension_icon(*url, ext->manifest.version(), *manager);
            }
            return {};
        };
        auto const& config = ext->manifest.action();
        auto icon_path = config.has_value() ? action.icon_path : sidebar.icon_path.value_or_lazy_evaluated_optional([&] { return action.icon_path; });
        auto icon = resolve_icon(icon_path);
        Optional<Gfx::ShareableBitmap> light_icon;
        Optional<Gfx::ShareableBitmap> dark_icon;
        if (config.has_value() && action.use_manifest_icon) {
            light_icon = resolve_icon(config->light_icon);
            dark_icon = resolve_icon(config->dark_icon);
        }
        Optional<Gfx::ShareableBitmap> sidebar_icon;
        if (sidebar_url.has_value())
            sidebar_icon = resolve_icon(sidebar.icon_path.value_or_lazy_evaluated_optional([&] { return ext->manifest.package_icon_path(); }));
        items.append({
            .id = ext->extension_id,
            .name = ext->manifest.name(),
            .title = action.title,
            .badge = action.badge_text,
            .badge_color = action.badge_background_color,
            .icon = move(icon),
            .light_icon = move(light_icon),
            .dark_icon = move(dark_icon),
            .has_action = ext->manifest.action().has_value(),
            .enabled = action.enabled,
            .pinned = action.pinned,
            .has_options = ext->manifest.options_ui().has_value(),
            .has_sidebar = ext->manifest.sidebar_action().has_value(),
            .sidebar_title = sidebar.title,
            .sidebar_icon = move(sidebar_icon),
            .sidebar_url = move(sidebar_url),
            .update_version = manager->available_update_version(ext->extension_id),
        });
    }
    async_view_extension_state(tab_id, true, move(items), manager->sidebar_controller().window_state(window_id).width_px);
}

void ConnectionFromUI::schedule_extension_state_refresh()
{
    if (exchange(m_extension_refresh_pending, true))
        return;
    deferred_invoke([this] {
        m_extension_refresh_pending = false;
        WebView::ViewImplementation::for_each_view([this](auto& implementation) {
            auto* view = find_view(implementation.view_id());
            if (view && view->is_tab() && view->is_current_tab_of_window() && view->is_private() == WebView::IsPrivate::No)
                send_extension_state(*view);
            return IterationDecision::Continue;
        });
    });
}

void ConnectionFromUI::extension_window_closed(u64 window_id)
{
    auto& manager = Application::default_session().extension_manager;
    if (!manager || !manager->extensions_enabled())
        return;
    m_extension_tab_windows.remove_all_matching([&](u64 tab_id, u64 window) {
        if (window != window_id)
            return false;
        manager->notify_tab_removed(tab_id, true);
        return true;
    });
    manager->action_controller().clear_window_state(window_id);
    manager->sidebar_controller().clear_window_state(window_id);
    manager->notify_window_removed(window_id);
}

void ConnectionFromUI::view_set_extension_window(u64 view_id, u64 window_id)
{
    if (auto* view = find_view(view_id); view && view->is_tab()) {
        view->set_extension_window_id(window_id);
        if (auto* manager = extension_manager_for_tab(view)) {
            if (m_extension_tab_windows.set(view_id, window_id) == AK::HashSetResult::InsertedNewEntry)
                manager->notify_tab_created(*view);
        }
        if (view->is_current_tab_of_window())
            send_extension_state(*view);
    }
}

void ConnectionFromUI::view_invoke_extension(u64 view_id, String id)
{
    auto* view = find_view(view_id);
    if (auto* manager = extension_manager_for_tab(view); manager && view->is_current_tab_of_window()) {
        manager->invoke_action_for_tab(id, view_id, view->extension_window_id(), [&](auto const& url) {
            async_view_open_extension_popup(view_id, id, url);
        });
    }
}

void ConnectionFromUI::view_pin_extension(u64 view_id, String id, bool pinned)
{
    auto* view = find_view(view_id);
    if (auto* manager = extension_manager_for_tab(view); manager && view->is_current_tab_of_window())
        if (auto result = manager->set_action_pinned(id, pinned); result.is_error())
            warnln("Could not save extension pin: {}", result.error());
}

void ConnectionFromUI::view_toggle_extension_sidebar(u64 view_id, String id)
{
    auto* view = find_view(view_id);
    if (auto* manager = extension_manager_for_tab(view); manager && view->is_current_tab_of_window()) {
        auto ext = manager->extension_by_id(id);
        if (ext.has_value() && ext->enabled && !manager->sidebar_controller().toggle(id, view->extension_window_id(), true).is_error())
            manager->notify_chrome_changed();
    }
}

void ConnectionFromUI::view_set_extension_sidebar_width(u64 view_id, u32 width)
{
    auto* view = find_view(view_id);
    if (auto* manager = extension_manager_for_tab(view); manager && view->is_current_tab_of_window())
        if (auto result = manager->set_sidebar_width(view->extension_window_id(), width); result.is_error())
            warnln("Could not save extension sidebar width: {}", result.error());
}

void ConnectionFromUI::view_set_sizes_to_content(u64 view_id, bool sizes_to_content)
{
    // A tab fills its window. Only a view that the UI floats over the window, such as an extension popup, takes its
    // size from its document.
    if (auto* view = find_view(view_id); view && !view->is_tab())
        view->set_sizes_to_content(sizes_to_content);
}

void ConnectionFromUI::view_open_extension_options(u64 view_id, String id)
{
    auto* view = find_view(view_id);
    auto* manager = extension_manager_for_tab(view);
    if (!manager || !view->is_current_tab_of_window())
        return;
    auto ext = manager->extension_by_id(id);
    if (ext.has_value() && ext->enabled && ext->manifest.options_ui().has_value()) {
        if (auto url = manager->package_page_url(*ext, ext->manifest.options_ui()->page); url.has_value())
            WebView::Application::the().open_url_in_new_tab(*url, Web::HTML::ActivateTab::Yes, view);
    }
}

void ConnectionFromUI::view_extension_package_picked(u64 view_id, u64 request_id, Optional<ByteString> path)
{
    if (auto* view = find_view(view_id))
        view->did_pick_extension_package(request_id, move(path));
}

void ConnectionFromUI::ui_did_start()
{
    if (auto& manager = Application::default_session().extension_manager; manager) {
        manager->on_chrome_changed = [] {
            if (auto* ui = the())
                ui->schedule_extension_state_refresh();
        };
    }
    Application::the().ui_did_start();
}

// NB: The UI makes no requests of its own choosing. It names a search engine and a query, and the Hub makes the request
//     for that engine's suggestions, as the UI's own settings say.
void ConnectionFromUI::fetch_search_suggestions(u64 request_id, String engine_name, String query, WebView::IsPrivate is_private)
{
    auto session = WebView::Application::session_for_new_view(is_private);
    if (!WebView::Application::privacy_policy_coordinator().allows_network_egress(*session)) {
        async_request_finished(request_id, true, {}, {}, {});
        return;
    }

    Optional<URL::URL> url;
    if (auto engine = Application::settings().find_search_engine_by_name(engine_name); engine.has_value())
        url = engine->suggestions_url_for_query(query);
    if (!url.has_value()) {
        async_request_finished(request_id, true, {}, {}, {});
        return;
    }

    auto request_headers = HTTP::HeaderList::create();
    request_headers->set({ "User-Agent"sv, Web::default_user_agent });

    static constexpr Array response_header_names { "Content-Type"sv };
    start_request(request_id, "GET"sv, *url, request_headers, {}, HTTP::CacheMode::Default, HTTP::Cookie::IncludeCredentials::Yes, is_private, response_header_names);
}

// NB: A report goes only to the report server, with no credentials, and with only the headers that reports carry.
void ConnectionFromUI::post_report(u64 request_id, URL::URL url, Vector<HTTP::Header> headers, ByteString body)
{
    if (!WebView::Application::privacy_policy_coordinator().allows_network_egress(WebView::Application::default_session())
        || !WebView::ReportSubmission::is_report_server_url(url)) {
        async_request_finished(request_id, true, {}, {}, {});
        return;
    }
    static constexpr Array report_header_names { "Content-Type"sv, "X-Ladybird-Challenge"sv, "X-Ladybird-Nonce"sv };
    auto request_headers = HTTP::HeaderList::create();
    for (auto& header : headers) {
        if (any_of(report_header_names, [&](auto name) { return header.name.equals_ignoring_ascii_case(name); }))
            request_headers->append(move(header));
    }
    static constexpr Array response_header_names { "Retry-After"sv };
    start_request(request_id, "POST"sv, url, *request_headers, body, HTTP::CacheMode::NoStore, HTTP::Cookie::IncludeCredentials::No, WebView::IsPrivate::No, response_header_names);
}

void ConnectionFromUI::start_request(u64 request_id, ByteString const& method, URL::URL const& url, HTTP::HeaderList const& headers, ByteString const& body, HTTP::CacheMode cache_mode, HTTP::Cookie::IncludeCredentials include_credentials, WebView::IsPrivate is_private, ReadonlySpan<StringView> response_header_names)
{
    RefPtr<Requests::Request> request;
    if (auto client = WebView::Application::request_server_client(is_private); !client.is_error())
        request = client.value()->start_request(method, url, HTTP::Cookie::SameSiteContext::same_site(), headers, body.bytes(), cache_mode, include_credentials);
    if (!request) {
        async_request_finished(request_id, true, {}, {}, {});
        return;
    }

    request->set_buffered_request_finished_callback(
        [this, request_id, response_header_names](u64, Requests::RequestTimingInfo const&, Optional<Requests::NetworkError> const& network_error,
            HTTP::HeaderList const& response_headers, Optional<u32> response_code, Optional<String> const&,
            Requests::CacheState, Core::ImmutableBytes payload) {
            // NB: The UI gets only the headers that the response is read for.
            Vector<HTTP::Header> headers_for_ui;
            for (auto name : response_header_names) {
                if (auto value = response_headers.get(name); value.has_value())
                    headers_for_ui.append({ name, value.release_value() });
            }
            async_request_finished(request_id, network_error.has_value(), response_code, move(headers_for_ui), ByteString::copy(payload.bytes()));

            // A request cannot be destroyed from inside its own callback.
            Core::deferred_invoke([this, request_id] {
                m_requests.remove(request_id);
            });
        });
    m_requests.set(request_id, request.release_nonnull());
}

void ConnectionFromUI::stop_request(u64 request_id)
{
    if (auto timer = m_request_timers.take(request_id); timer.has_value() && *timer)
        (*timer)->stop();
    if (auto request = m_requests.take(request_id); request.has_value())
        (*request)->stop();
}

void ConnectionFromUI::check_for_updates([[maybe_unused]] WebView::UpdateCheckTrigger trigger)
{
#if defined(LADYBIRD_HAS_UPDATE_CHECK)
    if (auto* update_checker = Application::the().update_checker())
        update_checker->check(trigger);
#endif
}

void ConnectionFromUI::skip_update_version([[maybe_unused]] String version)
{
#if defined(LADYBIRD_HAS_UPDATE_CHECK)
    if (auto* update_checker = Application::the().update_checker())
        update_checker->skip_version(version);
#endif
}

void ConnectionFromUI::toggle_devtools_enabled()
{
    if (auto result = Application::the().toggle_devtools_enabled(); result.is_error())
        static_cast<WebView::Application&>(Application::the()).display_error_dialog(MUST(String::formatted("Unable to start DevTools: {}", result.error())));
}

void ConnectionFromUI::set_preferred_contrast(Web::CSS::PreferredContrast contrast)
{
    Application::the().set_preferred_contrast(contrast);
}

void ConnectionFromUI::set_preferred_motion(Web::CSS::PreferredMotion motion)
{
    Application::the().set_preferred_motion(motion);
}

void ConnectionFromUI::set_user_agent_string(String user_agent)
{
    Application::the().set_user_agent_string(move(user_agent));
}

void ConnectionFromUI::set_navigator_compatibility_mode(Web::NavigatorCompatibilityMode mode)
{
    Application::the().set_navigator_compatibility_mode(mode);
}

void ConnectionFromUI::set_scripting_enabled(bool enabled)
{
    Application::the().set_scripting_enabled(enabled);
}

void ConnectionFromUI::set_blocks_pop_ups(bool blocks)
{
    Application::the().set_blocks_pop_ups(blocks);
}

void ConnectionFromUI::set_shows_line_box_borders(bool shows)
{
    Application::the().set_shows_line_box_borders(shows);
}

void ConnectionFromUI::set_shows_caret_hit_test_debug_overlay(bool shows)
{
    Application::the().set_shows_caret_hit_test_debug_overlay(shows);
}

void ConnectionFromUI::set_shows_compositor_layers(bool shows)
{
    Application::the().set_shows_compositor_layers(shows);
}

void ConnectionFromUI::dump_cookies()
{
    Application::the().dump_cookies();
}

void ConnectionFromUI::crash_compositor_process()
{
    Application::the().crash_compositor_process();
}

void ConnectionFromUI::launch_devtools_client()
{
    if (auto result = Application::the().launch_devtools_client(); result.is_error())
        WebView::Application::the().display_error_dialog(MUST(String::formatted("Unable to launch the DevTools client: {}", result.error())));
}

void ConnectionFromUI::reset_private_browsing_session()
{
    Application::the().reset_private_browsing_session();
}

void ConnectionFromUI::notify_compositor_gpu_presentation_unavailable()
{
    Application::the().notify_compositor_gpu_presentation_unavailable();
}

void ConnectionFromUI::ui_info_changed(Hub::UIInfo info)
{
    Application::the().set_ui_info(move(info));
}

Messages::HubServer::ProcessSnapshotsResponse ConnectionFromUI::process_snapshots()
{
    auto snapshots = WebView::process_snapshots(Application::process_manager());
    for (auto& snapshot : snapshots) {
        if (snapshot.type == WebView::ProcessType::Browser)
            snapshot.type = WebView::ProcessType::Hub;
    }
    return snapshots;
}

Messages::HubServer::TerminateProcessResponse ConnectionFromUI::terminate_process(u64 child_id)
{
    return WebView::end_process(Application::process_manager(), child_id);
}

Messages::HubServer::ViewNeedsBeforeunloadCheckResponse ConnectionFromUI::view_needs_beforeunload_check(u64 view_id)
{
    auto* view = find_view(view_id);
    return view && view->needs_beforeunload_check();
}

Messages::HubServer::ViewSessionHistoryTraversalItemsResponse ConnectionFromUI::view_session_history_traversal_items(u64 view_id, i32 direction)
{
    if (auto* view = find_view(view_id))
        return view->session_history_traversal_items(direction);
    return Vector<WebView::ViewImplementation::SessionHistoryTraversalItem> {};
}

Messages::HubServer::ViewSessionHistorySnapshotResponse ConnectionFromUI::view_session_history_snapshot(u64 view_id)
{
    if (auto* view = find_view(view_id))
        return Optional<WebView::SessionHistorySnapshot> { view->session_history_snapshot() };
    return Optional<WebView::SessionHistorySnapshot> {};
}

Messages::HubServer::ViewRestoreSessionHistoryFromSnapshotResponse ConnectionFromUI::view_restore_session_history_from_snapshot(u64 view_id, WebView::SessionHistorySnapshot snapshot)
{
    auto* view = find_view(view_id);
    return view && !view->restore_session_history_from_snapshot(move(snapshot)).is_error();
}

Messages::HubServer::ViewKeepStateForDiscardingResponse ConnectionFromUI::view_keep_state_for_discarding(u64 view_id)
{
    auto* view = find_view(view_id);
    if (!view)
        return Optional<u64> {};
    return view->keep_state_for_discarding();
}

void ConnectionFromUI::view_restore_state_of_discarded_tab(u64 view_id, u64 discarded_tab_state_id)
{
    auto* view = find_view(view_id);
    if (!view) {
        WebView::ViewImplementation::forget_state_of_discarded_tab(discarded_tab_state_id);
        return;
    }
    view->restore_state_of_discarded_tab(discarded_tab_state_id);
    view->push_state_to_ui();
}

void ConnectionFromUI::view_decline_discard(u64 view_id)
{
    if (auto* view = find_view(view_id))
        view->did_decline_discard();
}

void ConnectionFromUI::forget_state_of_discarded_tab(u64 discarded_tab_state_id)
{
    WebView::ViewImplementation::forget_state_of_discarded_tab(discarded_tab_state_id);
}

Messages::HubServer::ViewHistoryStepAtDeltaResponse ConnectionFromUI::view_history_step_at_delta(u64 view_id, i32 delta)
{
    auto* view = find_view(view_id);
    if (!view)
        return Optional<i32> {};
    auto const& traversable = view->traversable();
    auto used_steps = traversable.session_history().used_steps();
    auto step_index = traversable.effective_current_session_history_step_index() + delta;
    if (step_index >= used_steps.size())
        return Optional<i32> {};
    return Optional<i32> { used_steps[step_index] };
}

Messages::HubServer::ViewLookUpSelectedTextAtResponse ConnectionFromUI::view_look_up_selected_text_at(u64 view_id, Gfx::IntPoint position)
{
    auto* view = find_view(view_id);
    return view && view->look_up_selected_text_at(position);
}

Messages::HubServer::ViewPageDescriptionResponse ConnectionFromUI::view_page_description(u64 view_id)
{
    auto* view = find_view(view_id);
    if (!view)
        return { URL::URL {}, String {}, Gfx::IntSize {}, 1.0, 1.0 };
    auto page = WebView::WebCompatReport::describe_page(*view);
    return { move(page.url), move(page.user_agent), page.viewport_size, page.device_pixel_ratio, page.zoom_level };
}

void ConnectionFromUI::update_settings(u64 change_id, ByteString changes_json)
{
    auto changes = JsonValue::from_string(changes_json);
    if (changes.is_error() || !changes.value().is_object())
        return;

    // NB: This process owns the settings, so what the UI did not change stays as it is here, however old its copy is.
    auto settings = Application::settings().serialize_json();
    WebView::Settings::apply_changes(settings.as_object(), changes.value().as_object());
    m_last_applied_settings_change_id = change_id;
    Application::settings().replace_with_json(settings, WebView::Settings::PersistChange::Yes);
}

void ConnectionFromUI::destroy_view(u64 view_id)
{
    if (m_extension_tab_windows.remove(view_id)) {
        if (auto* manager = extension_manager_for_tab(find_view(view_id)))
            manager->notify_tab_removed(view_id, false);
    }
    Application::the().destroy_view(view_id);
}

void ConnectionFromUI::view_set_viewport(u64 view_id, Web::DevicePixelSize size, double device_pixel_ratio)
{
    if (auto* view = find_view(view_id))
        view->set_viewport(size, device_pixel_ratio);
}

void ConnectionFromUI::view_set_system_theme(u64 view_id, Core::AnonymousBuffer theme)
{
    if (auto* view = find_view(view_id))
        view->set_system_theme(move(theme));
}

void ConnectionFromUI::view_set_screen_rects(u64 view_id, WebView::ViewImplementation::ScreenRects screen_rects)
{
    if (auto* view = find_view(view_id))
        view->set_screen_rects(move(screen_rects));
}

void ConnectionFromUI::view_set_page_background_color_to_system_canvas(u64 view_id, bool dark)
{
    if (auto* view = find_view(view_id)) {
        view->set_page_background_color_to_system_canvas(dark);
        view->push_state_to_ui();
    }
}

View* ConnectionFromUI::find_view(u64 view_id)
{
    return Application::the().find_ui_view(view_id);
}

void ConnectionFromUI::view_load(u64 view_id, URL::URL url, Web::Bindings::NavigationHistoryBehavior history_behavior)
{
    if (auto* view = find_view(view_id)) {
        view->load_from_browser_ui(url, history_behavior);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_load_html(u64 view_id, ByteString html)
{
    if (auto* view = find_view(view_id)) {
        view->load_html(html);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_find_in_page(u64 view_id, Utf16String query, AK::CaseSensitivity case_sensitivity, bool highlight_all_matches)
{
    if (auto* view = find_view(view_id)) {
        view->find_in_page(query, case_sensitivity, highlight_all_matches);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_set_window_position(u64 view_id, Gfx::IntPoint position)
{
    if (auto* view = find_view(view_id)) {
        view->set_window_position(position);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_set_window_size(u64 view_id, Gfx::IntSize size)
{
    if (auto* view = find_view(view_id)) {
        view->set_window_size(size);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_set_system_visibility_state(u64 view_id, Web::HTML::VisibilityState visibility_state)
{
    if (auto* view = find_view(view_id)) {
        view->set_system_visibility_state(visibility_state);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_set_hosts_remote_layer(u64 view_id, bool hosts_remote_layer)
{
    if (auto* view = find_view(view_id))
        view->set_hosts_remote_layer(hosts_remote_layer);
}

void ConnectionFromUI::view_set_wants_frame_notifications(u64 view_id, bool wants_frame_notifications)
{
    if (auto* view = find_view(view_id))
        view->set_wants_frame_notifications(wants_frame_notifications);
}

void ConnectionFromUI::view_take_frame_snapshot(u64 view_id, u64 request_id)
{
    auto* view = find_view(view_id);
    if (!view) {
        async_view_did_take_frame_snapshot(view_id, request_id, {});
        return;
    }
    view->take_frame_snapshot([view_id, request_id](Optional<Gfx::ShareableBitmap> snapshot) {
        if (auto* ui = ConnectionFromUI::the())
            ui->async_view_did_take_frame_snapshot(view_id, request_id, move(snapshot));
    });
}

void ConnectionFromUI::view_begin_synchronized_resize(u64 view_id, u64 token)
{
    if (auto* view = find_view(view_id))
        view->begin_synchronized_resize(token);
}

// The UI waits for the answer on its main thread, so a view that is gone answers as well.
void ConnectionFromUI::view_await_synchronized_resize_frame(u64 view_id, u64 token, Gfx::IntSize size)
{
    if (auto* view = find_view(view_id))
        view->await_synchronized_resize_frame(token, size);
    else
        async_view_did_hold_synchronized_resize_frame(view_id, token);
}

void ConnectionFromUI::view_commit_synchronized_resize(u64 view_id, u64 token)
{
    if (auto* view = find_view(view_id))
        view->commit_synchronized_resize(token);
    else
        async_view_did_commit_synchronized_resize(view_id, token);
}

void ConnectionFromUI::view_set_pinned(u64 view_id, bool pinned)
{
    if (auto* view = find_view(view_id); view && view->is_pinned() != pinned) {
        view->set_pinned(pinned);
        if (auto* manager = extension_manager_for_tab(view))
            manager->notify_tab_updated(view_id, WebView::Extensions::ExtensionManager::TabChange::Pinned);
    }
}

void ConnectionFromUI::view_set_current_tab_of_window(u64 view_id, bool current_tab_of_window)
{
    if (auto* view = find_view(view_id)) {
        auto was_current = view->is_current_tab_of_window();
        view->set_current_tab_of_window(current_tab_of_window);
        if (current_tab_of_window && view->is_tab()) {
            send_extension_state(*view);
            if (auto* manager = extension_manager_for_tab(view); manager && !was_current)
                manager->notify_tab_activated(view_id, view->extension_window_id());
        }
    }
}

void ConnectionFromUI::view_set_presents_from_cpu_memory(u64 view_id, bool presents_from_cpu_memory)
{
    if (auto* view = find_view(view_id)) {
        view->set_presents_from_cpu_memory(presents_from_cpu_memory);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_set_has_system_focus(u64 view_id, bool has_system_focus)
{
    if (auto* view = find_view(view_id)) {
        view->set_has_system_focus(has_system_focus);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_open_url_in_new_tab(u64 view_id, URL::URL url, Web::HTML::ActivateTab activate_tab)
{
    if (auto* view = find_view(view_id)) {
        view->open_url_in_new_tab(url, activate_tab);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_open_url_in_new_window(u64 view_id, URL::URL url, WebView::IsPrivate is_private)
{
    if (auto* view = find_view(view_id)) {
        view->open_url_in_new_window(url, is_private);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_set_next_history_visit_transition(u64 view_id, WebView::HistoryVisitTransition transition)
{
    if (auto* view = find_view(view_id)) {
        view->set_next_history_visit_transition(transition);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_reload(u64 view_id, WebView::BypassCache bypass_cache)
{
    if (auto* view = find_view(view_id)) {
        view->reload(bypass_cache);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_undo(u64 view_id)
{
    if (auto* view = find_view(view_id))
        view->undo();
}

void ConnectionFromUI::view_redo(u64 view_id)
{
    if (auto* view = find_view(view_id))
        view->redo();
}

void ConnectionFromUI::view_cut_selection(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->cut_selected_text()->when_resolved([](auto& text) {
            if (!text.is_empty())
                Application::the().insert_clipboard_entry({ "text/plain"_string, text });
        });
    }
}

void ConnectionFromUI::view_paste(u64 view_id, Web::Clipboard::PasteMode paste_mode)
{
    if (auto* view = find_view(view_id))
        view->paste_from_clipboard(paste_mode);
}

void ConnectionFromUI::view_select_all(u64 view_id)
{
    if (auto* view = find_view(view_id))
        view->select_all();
}

void ConnectionFromUI::view_get_source(u64 view_id)
{
    if (auto* view = find_view(view_id))
        view->get_source();
}

void ConnectionFromUI::view_traverse_the_history_by_delta(u64 view_id, i32 delta)
{
    if (auto* view = find_view(view_id))
        view->traverse_the_history_by_delta(delta);
}

void ConnectionFromUI::view_translate_to(u64 view_id, String target_language)
{
    if (auto* view = find_view(view_id); view && view->has_traversable())
        view->page_translation().translate_to(target_language);
}

void ConnectionFromUI::view_show_original(u64 view_id)
{
    if (auto* view = find_view(view_id); view && view->has_traversable())
        view->page_translation().show_original();
}

void ConnectionFromUI::view_download_translation_languages(u64 view_id, String target_language)
{
    if (auto* view = find_view(view_id); view && view->has_traversable())
        view->page_translation().download_languages_for(target_language);
}

void ConnectionFromUI::view_prefer_higher_quality_translation(u64 view_id, bool prefers_higher_quality)
{
    if (auto* view = find_view(view_id); view && view->has_traversable())
        view->page_translation().prefer_higher_quality_models(prefers_higher_quality);
}

void ConnectionFromUI::view_download_url(u64 view_id, URL::URL url, bool prompt_for_path, Optional<String> image_mime_type)
{
    if (auto* view = find_view(view_id))
        view->download_url(url, prompt_for_path ? WebView::ViewImplementation::PromptForPath::Yes : WebView::ViewImplementation::PromptForPath::No, image_mime_type);
}

void ConnectionFromUI::view_activate_extension_menu_item(u64 view_id, u64 serial, u64 token)
{
    if (auto* view = find_view(view_id))
        view->activate_extension_menu_item(serial, token);
}

void ConnectionFromUI::view_inspect_context_menu_element(u64 view_id)
{
    if (auto* view = find_view(view_id))
        view->inspect_context_menu_element();
}

void ConnectionFromUI::view_replace_context_menu_misspelling(u64 view_id, Utf16String replacement)
{
    if (auto* view = find_view(view_id))
        view->replace_context_menu_misspelling(replacement);
}

void ConnectionFromUI::view_ignore_misspelling(u64 view_id, Utf16String word)
{
    if (auto* view = find_view(view_id))
        view->ignore_misspelling(word);
}

void ConnectionFromUI::view_learn_misspelling(u64 view_id, Utf16String word)
{
    if (auto* view = find_view(view_id))
        view->learn_misspelling(word);
}

void ConnectionFromUI::view_do_context_menu_media_action(u64 view_id, WebView::ContextMenuMediaAction action)
{
    if (auto* view = find_view(view_id))
        view->do_context_menu_media_action(action);
}

void ConnectionFromUI::view_debug_request(u64 view_id, ByteString request, ByteString argument)
{
    if (auto* view = find_view(view_id))
        view->debug_request(request, argument);
}

void ConnectionFromUI::view_dump_gc_graph(u64 view_id)
{
    auto* view = find_view(view_id);
    if (!view)
        return;

    auto gc_graph_path = view->dump_gc_graph();
    if (gc_graph_path.is_error()) {
        warnln("\033[31;1mFailed to dump GC graph: {}\033[0m", gc_graph_path.error());
        return;
    }
    warnln("\033[33;1mDumped GC graph into {}\033[0m", gc_graph_path.value());
    if (auto source_dir = Core::Environment::get("LADYBIRD_SOURCE_DIR"sv); source_dir.has_value())
        warnln("\033[33;1mGC graph explorer: file://{}/Meta/gc-heap-explorer.html?script=file://{}\033[0m", *source_dir, gc_graph_path.value());
}

void ConnectionFromUI::view_stop_loading(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->stop_loading();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_zoom_in(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->zoom_in();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_zoom_out(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->zoom_out();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_reset_zoom(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->reset_zoom();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_set_display_metadata(u64 view_id, Optional<u64> display_id, double maximum_frames_per_second, bool screen_shows_high_dynamic_range)
{
    if (auto* view = find_view(view_id)) {
        view->set_display_metadata(display_id, maximum_frames_per_second, screen_shows_high_dynamic_range);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_set_scales_frames_to_fit(u64 view_id, bool scales_frames_to_fit)
{
    if (auto* view = find_view(view_id)) {
        view->set_scales_frames_to_fit(scales_frames_to_fit);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_enable_picture_in_picture_controls(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->enable_picture_in_picture_controls();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_update_interface_color_scheme(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->update_interface_color_scheme();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_find_in_page_next_match(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->find_in_page_next_match();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_find_in_page_previous_match(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->find_in_page_previous_match();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_find_in_page_end(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->find_in_page_end();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_report_website_issue(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->report_website_issue();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_request_accessibility_tree(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->request_accessibility_tree();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_perform_accessibility_action(u64 view_id, i64 node_id, String action)
{
    if (auto* view = find_view(view_id)) {
        view->perform_accessibility_action(node_id, action);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_clear_node_picker(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->clear_node_picker();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_node_picker_hover(u64 view_id, Web::DevicePixelPoint position)
{
    if (auto* view = find_view(view_id)) {
        view->node_picker_hover(position);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_node_picker_pick(u64 view_id, Web::DevicePixelPoint position)
{
    if (auto* view = find_view(view_id)) {
        view->node_picker_pick(position);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_node_picker_preview(u64 view_id, Web::DevicePixelPoint position)
{
    if (auto* view = find_view(view_id)) {
        view->node_picker_preview(position);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_node_picker_cancel(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->node_picker_cancel();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_alert_closed(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->alert_closed();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_confirm_closed(u64 view_id, bool accepted)
{
    if (auto* view = find_view(view_id)) {
        view->confirm_closed(accepted);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_prompt_closed(u64 view_id, Optional<Utf16String> response)
{
    if (auto* view = find_view(view_id)) {
        view->prompt_closed(response);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_color_picker_update(u64 view_id, Optional<Color> picked_color, Web::HTML::ColorPickerUpdateState state)
{
    if (auto* view = find_view(view_id)) {
        view->color_picker_update(picked_color, state);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_date_picker_closed(u64 view_id, Optional<Utf16String> picked_value)
{
    if (auto* view = find_view(view_id)) {
        view->date_picker_closed(picked_value);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_select_dropdown_closed(u64 view_id, Optional<u32> selected_item_id)
{
    if (auto* view = find_view(view_id)) {
        view->select_dropdown_closed(selected_item_id);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_set_marked_text_from_input_method(u64 view_id, Utf16String text)
{
    if (auto* view = find_view(view_id)) {
        view->set_marked_text_from_input_method(text);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_commit_text_from_input_method(u64 view_id, Utf16String text, i32 replacement_start, i32 replacement_length)
{
    if (auto* view = find_view(view_id)) {
        view->commit_text_from_input_method(text, replacement_start, replacement_length);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_toggle_page_mute_state(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->toggle_page_mute_state();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_take_and_save_screenshot(u64 view_id, WebView::ViewImplementation::ScreenshotType screenshot_type)
{
    if (auto* view = find_view(view_id)) {
        view->take_and_save_screenshot(screenshot_type);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_request_close(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->request_close();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_deny_fullscreen_window(u64 view_id)
{
    if (auto* view = find_view(view_id))
        view->deny_fullscreen_window();
}

void ConnectionFromUI::view_exit_fullscreen(u64 view_id)
{
    if (auto* view = find_view(view_id)) {
        view->exit_fullscreen();
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_set_is_fullscreen(u64 view_id, Web::ViewportIsFullscreen is_fullscreen)
{
    if (auto* view = find_view(view_id)) {
        view->set_is_fullscreen(is_fullscreen);
        view->push_state_to_ui();
    }
}

void ConnectionFromUI::view_reset_site_permissions(u64 view_id, URL::Origin origin)
{
    if (auto* view = find_view(view_id)) {
        view->reset_site_permissions(origin);
        view->push_state_to_ui();
    }
}

}
