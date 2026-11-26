/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/QuickSort.h>
#include <LibPrivacy/Identity/Types.h>
#include <LibURL/URL.h>
#include <LibWebView/Application.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/PrivacyActivity.h>
#include <LibWebView/PrivacyPolicyCoordinator.h>
#include <LibWebView/Settings.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebUI/PrivacyUI.h>

namespace WebView {

void PrivacyUI::register_interfaces()
{
    register_interface("loadStatus"sv, [this](auto const&) { load_status(); });
    register_interface("loadCapture"sv, [this](auto const& data) { load_capture(data); });
    register_interface("startCapture"sv, [this](auto const& data) { start_capture(data); });
    register_interface("stopCapture"sv, [this](auto const& data) { stop_capture(data); });
}

static StringView capture_kind_to_string(PrivacyActivity::CaptureEventKind kind)
{
    switch (kind) {
    case PrivacyActivity::CaptureEventKind::Request:
        return "request"sv;
    case PrivacyActivity::CaptureEventKind::Blocked:
        return "blocked"sv;
    }
    VERIFY_NOT_REACHED();
}

void PrivacyUI::load_status()
{
    auto& session = client().session();
    auto& coordinator = Application::privacy_policy_coordinator();
    auto const& snapshot = coordinator.snapshot_for_session(session);

    JsonObject identity;
    identity.set("id"sv, session.identity().id);
    identity.set("name"sv, session.identity().display_name);
    identity.set("kind"sv, Privacy::identity_kind_to_string(session.identity().kind));

    // The route goes out only as its redacted description: never its credentials or isolation secret.
    JsonObject network;
    network.set("mode"sv, Privacy::network_mode_to_string(coordinator.network_mode(session)));
    network.set("epoch"sv, coordinator.committed_epoch(session));
    if (snapshot.network.route.has_value())
        network.set("route"sv, MUST(String::from_byte_string(snapshot.network.route->redacted_description())));
    else
        network.set("route"sv, JsonValue {});
    network.set("localDNS"sv, coordinator.allows_dns_resolution(session));
    network.set("webRTC"sv, coordinator.allows_webrtc_network(session));

    auto counters = Application::privacy_activity().counters_for(session);
    JsonObject counts;
    counts.set("navigationsSanitized"sv, counters.navigations_sanitized);
    counts.set("trackingParametersRemoved"sv, counters.tracking_parameters_removed);
    counts.set("navigationsWithScriptingBlocked"sv, counters.navigations_with_scripting_blocked);

    JsonArray scripting_disabled_sites;
    auto sites = Application::settings().scripting_disabled_sites().values();
    quick_sort(sites);
    for (auto const& site : sites)
        scripting_disabled_sites.must_append(site);
    JsonObject controls;
    controls.set("stripTrackingParameters"sv, Application::browser_options().privacy_strip_tracking_parameters);
    controls.set("scriptingDisabledSites"sv, move(scripting_disabled_sites));

    JsonArray tabs;
    ViewImplementation::for_each_view([&](ViewImplementation& view) {
        if (&view.session() != &session)
            return IterationDecision::Continue;
        JsonObject tab;
        tab.set("id"sv, view.view_id());
        tab.set("title"sv, view.title().to_utf8());
        tab.set("host"sv, PrivacyActivity::host_for_capture(view.url()));
        tabs.must_append(move(tab));
        return IterationDecision::Continue;
    });

    JsonObject status;
    status.set("identity"sv, move(identity));
    status.set("network"sv, move(network));
    status.set("counters"sv, move(counts));
    status.set("controls"sv, move(controls));
    status.set("tabs"sv, move(tabs));
    async_send_message("loadStatus"sv, move(status));
}

ViewImplementation* PrivacyUI::tab_for(JsonValue const& data) const
{
    if (!data.is_object())
        return nullptr;
    auto id = data.as_object().get_u64("tabId"sv);
    if (!id.has_value())
        return nullptr;
    ViewImplementation* found = nullptr;
    ViewImplementation::for_each_view([&](ViewImplementation& view) {
        if (view.view_id() == *id && &view.session() == &client().session()) {
            found = &view;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    return found;
}

void PrivacyUI::start_capture(JsonValue const& data)
{
    // A tab of another identity, or one that is gone, gets the same answer as a tab without a capture.
    auto* tab = tab_for(data);
    if (!tab) {
        load_capture(data);
        return;
    }
    auto seconds = data.as_object().get_u64("seconds"sv).value_or(60);
    Application::privacy_activity().start_capture(*tab, AK::Duration::from_seconds(static_cast<i64>(min<u64>(seconds, 3600))));
    load_capture(data);
}

void PrivacyUI::stop_capture(JsonValue const& data)
{
    if (auto* tab = tab_for(data))
        Application::privacy_activity().stop_capture(tab->view_id());
    load_capture(data);
}

void PrivacyUI::load_capture(JsonValue const& data)
{
    JsonObject result;
    auto* tab = tab_for(data);
    result.set("tabId"sv, tab ? JsonValue { tab->view_id() } : JsonValue {});

    Optional<PrivacyActivity::Capture const&> capture;
    if (tab)
        capture = Application::privacy_activity().capture_for(tab->view_id(), client().session());
    result.set("active"sv, capture.has_value());

    JsonArray entries;
    if (capture.has_value()) {
        auto now = MonotonicTime::now();
        result.set("secondsLeft"sv, max<i64>(0, (capture->expires_at - now).to_seconds()));
        result.set("dropped"sv, capture->dropped_entries);
        for (auto const& entry : capture->entries) {
            JsonObject row;
            row.set("host"sv, entry.host);
            row.set("kind"sv, capture_kind_to_string(entry.kind));
            row.set("secondsAgo"sv, max<i64>(0, (now - entry.time).to_seconds()));
            entries.must_append(move(row));
        }
    }
    result.set("entries"sv, move(entries));
    async_send_message("loadCapture"sv, move(result));
}

}
