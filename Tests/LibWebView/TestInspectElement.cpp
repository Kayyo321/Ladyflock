/*
 * Copyright (c) 2026, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HeadlessTest.h"
#include <AK/LexicalPath.h>
#include <LibCore/Timer.h>
#include <LibGfx/SystemTheme.h>
#include <LibTest/TestCase.h>
#include <LibURL/InternalURLs.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/InspectorSession.h>
#include <LibWebView/Settings.h>
#include <LibWebView/Utilities.h>

namespace {

static void wait_for(Function<bool()> condition)
{
    bool timed_out = false;
    auto timer = Core::Timer::create_single_shot(AK::Duration::from_seconds(10), [&] { timed_out = true; });
    timer->start();
    Core::EventLoop::current().spin_until([&] { return timed_out || condition(); });
    timer->stop();
    VERIFY(!timed_out);
}

static auto make_view(WebView::IsPrivate is_private = WebView::IsPrivate::No)
{
    auto theme = MUST(Gfx::load_system_theme(LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv).string()));
    return WebView::HeadlessWebView::create(move(theme), { 800, 600 }, is_private);
}

static void load_frontend(WebView::HeadlessWebView& frontend)
{
    bool loaded = false;
    frontend.on_load_finish = [&](auto const& url) { loaded = url == URL::about_inspector(); };
    frontend.load_from_browser_ui(URL::about_inspector());
    wait_for([&] { return loaded; });
    frontend.on_load_finish = nullptr;
}

static JsonObject command(WebView::HeadlessWebView& frontend, StringView name, JsonObject params = {}, StringView follow_event = {})
{
    static u64 request_id = 1000000;
    auto id = request_id++;
    auto prefix = ByteString::formatted("RESULT-{}:", id);
    auto script = MUST(String::formatted(R"JS(
        (() => {{
            const params = {};
            const followEvent = "{}";
            const handler = event => {{
                const {{name, data}} = event.detail;
                if (name === "inspectorReply" && data.requestId === {}) {{
                    if (followEvent && data.ok) return;
                }} else if (name !== followEvent || !followEvent) return;
                if (name === "domNodeProperties" && (data.nodeId !== params.nodeId || data.type !== params.type)) return;
                document.removeEventListener("WebUIMessage", handler);
                document.title = "{}" + JSON.stringify(data);
            }};
            document.addEventListener("WebUIMessage", handler);
            const send = () => ladybird.sendMessage("inspectorCommand", {{ requestId: {}, command: "{}", params }});
            if (window.ladybird) send();
            else document.addEventListener("WebUILoaded", send, {{ once: true }});
        }})();
    )JS",
        params.serialized(), follow_event, id, prefix, id, name));
    frontend.run_javascript(script);
    wait_for([&] { return frontend.title().to_byte_string().starts_with(prefix); });
    auto title = frontend.title().to_byte_string();
    auto value = MUST(JsonValue::from_string(title.substring_view(prefix.length())));
    VERIFY(value.is_object());
    return value.as_object();
}

static JsonObject result(WebView::HeadlessWebView& frontend, StringView name, JsonObject params = {})
{
    auto reply = command(frontend, name, move(params));
    if (!reply.get_bool("ok"sv).value_or(false))
        warnln("Inspector command {} failed: {}", name, reply.serialized());
    VERIFY(reply.get_bool("ok"sv).value_or(false));
    return *reply.get_object("result"sv);
}

static u64 search(WebView::HeadlessWebView& frontend, StringView query)
{
    JsonObject params;
    params.set("query"sv, query);
    auto response = result(frontend, "searchDom"sv, move(params));
    auto matches = response.get_array("matches"sv);
    VERIFY(matches.has_value() && !matches->is_empty());
    return matches->at(0).as_object().get_u64("nodeId"sv).value();
}

static JsonObject node_properties(WebView::HeadlessWebView& frontend, u64 node_id, StringView type)
{
    JsonObject params;
    params.set("nodeId"sv, node_id);
    params.set("type"sv, type);
    auto response = command(frontend, "inspectNode"sv, move(params), "domNodeProperties"sv);
    VERIFY(response.get_u64("nodeId"sv) == node_id);
    return response;
}

static JsonObject margin_rule(WebView::HeadlessWebView& frontend, u64 node_id)
{
    auto response = node_properties(frontend, node_id, "appliedStyleRules"sv);
    for (auto const& item : response.get_array("properties"sv)->values()) {
        auto rule = item.as_object().get_object("rule"sv);
        if (rule.has_value() && rule->get_string("authoredText"sv).value_or(String {}).contains("z-index"sv))
            return *rule;
    }
    VERIFY_NOT_REACHED();
}

}

TEST_CASE(inspector_attachment_styles_history_and_detach)
{
    Test::HeadlessTestEnvironment environment { "TestInspectElement"sv };
    auto app = TRY_OR_FAIL(environment.create_application());
    auto target = make_view();
    target->load_html(R"HTML(<!doctype html><title>Inspector fixture</title>
        <style>
          @layer early, late;
          @layer early { #subject { color: rgb(1, 2, 3) !important; } }
          @layer late { #subject { color: rgb(4, 5, 6) !important; } }
          #subject { z-index: 5; position: static; margin-left: 2px; }
          #subject:hover { margin-left: 7px; }
        </style>
        <main style="display: flex"><button id=subject>Distinctive fixture text</button></main>
        <div id=shadow-host></div>
        <script>
            const source = ':host { color: rgb(11, 12, 13); }';
            const pageStyle = document.createElement('style');
            pageStyle.textContent = source;
            document.head.append(pageStyle);
            const shadowStyle = document.createElement('style');
            shadowStyle.textContent = source;
            document.getElementById('shadow-host').attachShadow({ mode: 'open' }).append(shadowStyle);
        </script>
    )HTML"sv);
    wait_for([&] { return target->title() == "Inspector fixture"_utf16; });

    auto unbound = make_view();
    load_frontend(*unbound);
    auto state = command(*unbound, "getSessionState"sv, {}, "inspectorState"sv);
    EXPECT_EQ(state.get_bool("bound"sv), Optional<bool> { false });
    unbound->run_javascript("document.title = getComputedStyle(document.querySelector('#workspace')).display === 'none' && getComputedStyle(document.querySelector('#unbound-state')).display !== 'none' ? 'Detached' : 'Attached';"_string);
    wait_for([&] { return unbound->title() == "Detached"_utf16; });
    EXPECT_EQ(command(*unbound, "getDomTree"sv).get_bool("ok"sv), Optional<bool> { false });
    EXPECT(!WebView::InspectorSession::find_for_frontend_view(unbound->view_id()));

    auto frontend = make_view();
    auto session = WebView::InspectorSession::authorize_frontend_view(*target, frontend->view_id());
    VERIFY(session);
    load_frontend(*frontend);
    wait_for([&] { return session->is_attached(); });
    state = command(*frontend, "getSessionState"sv, {}, "inspectorState"sv);
    EXPECT_EQ(state.get_bool("bound"sv), Optional<bool> { true });
    EXPECT_EQ(state.get_u64("targetViewId"sv), target->view_id());

    auto tree = result(*frontend, "getDomTree"sv);
    auto document = tree.get_object("tree"sv);
    VERIFY(document.has_value());
    auto children = document->get_array("children"sv);
    VERIFY(children.has_value() && !children->is_empty());
    EXPECT_EQ(children->at(0).as_object().get_string("type"sv), "doctype"sv);

    // The page and the shadow root have the same sheet, which they share compiled. Only the shadow root's copy can
    // style the host, through :host, so that is the copy the rule names, and editing it restyles the host.
    auto host_id = search(*frontend, "#shadow-host"sv);
    auto host_styles = node_properties(*frontend, host_id, "appliedStyleRules"sv);
    Optional<JsonObject> host_rule;
    for (auto const& item : host_styles.get_array("properties"sv)->values()) {
        auto rule = item.as_object().get_object("rule"sv);
        if (rule.has_value() && rule->get_string("cssText"sv).value_or(String {}).starts_with_bytes(":host"sv))
            host_rule = *rule;
    }
    VERIFY(host_rule.has_value());
    VERIFY(host_rule->has("sheetId"sv));
    JsonObject host_edit;
    host_edit.set("nodeId"sv, host_id);
    host_edit.set("sheetId"sv, host_rule->get_string("sheetId"sv).value());
    host_edit.set("ruleIdentity"sv, host_rule->get_string("ruleIdentity"sv).value());
    host_edit.set("baseRevision"sv, host_rule->get_u64("sheetRevision"sv).value());
    host_edit.set("property"sv, "color"sv);
    host_edit.set("value"sv, "rgb(21, 22, 23)"sv);
    EXPECT(!result(*frontend, "editStyleDeclaration"sv, host_edit).get_bool("conflict"sv).value());
    auto host_computed = node_properties(*frontend, host_id, "computedStyle"sv).get_object("properties"sv).value();
    EXPECT_EQ(host_computed.get_string("color"sv), "rgb(21, 22, 23)"sv);

    auto node_id = search(*frontend, "#subject"sv);
    EXPECT_EQ(search(*frontend, "//*[@id='subject']"sv), node_id);
    EXPECT(search(*frontend, "Distinctive fixture text"sv) != 0);

    auto computed = node_properties(*frontend, node_id, "computedStyle"sv).get_object("properties"sv).value();
    EXPECT_EQ(computed.get_string("color"sv), "rgb(1, 2, 3)"sv);
    EXPECT_EQ(computed.get_string("margin-left"sv), "2px"sv);

    auto applied = node_properties(*frontend, node_id, "appliedStyleRules"sv);
    bool saw_layer_winner = false;
    bool saw_static_flex_z_index = false;
    for (auto const& item : applied.get_array("properties"sv)->values()) {
        auto rule = item.as_object().get_object("rule"sv);
        if (!rule.has_value())
            continue;
        for (auto const& value : rule->get_array("declarations"sv)->values()) {
            auto const& declaration = value.as_object();
            auto property = declaration.get_string("name"sv);
            if (property == "color"sv && declaration.get_bool("cascadeWinner"sv).value_or(false)) {
                EXPECT_EQ(rule->get_string("layerName"sv), "early"sv);
                EXPECT(declaration.get_object("cascadeStory"sv).has_value());
                saw_layer_winner = true;
            }
            if (property == "z-index"sv && declaration.get_bool("cascadeWinner"sv).value_or(false)) {
                EXPECT(!declaration.has("inactiveReason"sv));
                saw_static_flex_z_index = true;
            }
        }
    }
    EXPECT(saw_layer_winner);
    EXPECT(saw_static_flex_z_index);

    auto editable_rule = margin_rule(*frontend, node_id);

    JsonObject edit;
    edit.set("nodeId"sv, node_id);
    edit.set("sheetId"sv, editable_rule.get_string("sheetId"sv).value());
    edit.set("ruleIdentity"sv, editable_rule.get_string("ruleIdentity"sv).value());
    edit.set("baseRevision"sv, editable_rule.get_u64("sheetRevision"sv).value());
    edit.set("property"sv, "margin-left"sv);
    edit.set("value"sv, "3px"sv);
    EXPECT(!result(*frontend, "editStyleDeclaration"sv, edit).get_bool("conflict"sv).value());
    computed = node_properties(*frontend, node_id, "computedStyle"sv).get_object("properties"sv).value();
    EXPECT_EQ(computed.get_string("margin-left"sv), "3px"sv);
    EXPECT_EQ(session->edit_journal().size(), 2u);
    auto entry_id = session->edit_journal().last().id;
    JsonObject undo;
    undo.set("id"sv, entry_id);
    result(*frontend, "undoEdit"sv, move(undo));
    EXPECT(session->edit_journal().last().undone);
    computed = node_properties(*frontend, node_id, "computedStyle"sv).get_object("properties"sv).value();
    EXPECT_EQ(computed.get_string("margin-left"sv), "2px"sv);

    // Refresh after undo so that only the page's CSSOM mutation can invalidate this revision.
    auto revision_after_undo = margin_rule(*frontend, node_id).get_u64("sheetRevision"sv).value();
    edit.set("baseRevision"sv, revision_after_undo);
    target->run_javascript("document.styleSheets[0].cssRules[3].style.setProperty('margin-left', '4px'); document.title = 'CSS changed';"_string);
    wait_for([&] { return target->title() == "CSS changed"_utf16; });
    auto conflict = result(*frontend, "editStyleDeclaration"sv, edit);
    EXPECT_EQ(conflict.get_bool("conflict"sv), Optional<bool> { true });
    EXPECT_EQ(conflict.get_u64("expectedRevision"sv), revision_after_undo);
    auto revision_after_page_edit = margin_rule(*frontend, node_id).get_u64("sheetRevision"sv).value();
    EXPECT_NE(revision_after_page_edit, revision_after_undo);
    EXPECT_EQ(conflict.get_u64("currentRevision"sv), revision_after_page_edit);
    EXPECT_EQ(session->edit_journal().size(), 2u);

    JsonObject force;
    force.set("nodeId"sv, node_id);
    force.set("state"sv, "hover"sv);
    force.set("enabled"sv, true);
    result(*frontend, "setForcedPseudoState"sv, move(force));
    computed = node_properties(*frontend, node_id, "computedStyle"sv).get_object("properties"sv).value();
    EXPECT_EQ(computed.get_string("margin-left"sv), "7px"sv);

    JsonObject text;
    text.set("nodeId"sv, search(*frontend, "Distinctive fixture text"sv));
    text.set("text"sv, "Edited fixture text"sv);
    result(*frontend, "setNodeText"sv, move(text));
    EXPECT_EQ(session->edit_journal().last().after_text, "Edited fixture text"sv);
    target->run_javascript("document.title = document.getElementById('subject').textContent;"_string);
    wait_for([&] { return target->title() == "Edited fixture text"_utf16; });

    target->did_connect_devtools_client();
    session->detach_frontend();
    EXPECT(!session->is_attached());
    state = command(*frontend, "getSessionState"sv, {}, "inspectorState"sv);
    EXPECT_EQ(state.get_bool("bound"sv), Optional<bool> { false });
    EXPECT_EQ(command(*frontend, "getDomTree"sv).get_bool("ok"sv), Optional<bool> { false });
    edit.set("baseRevision"sv, revision_after_page_edit);
    EXPECT_EQ(command(*frontend, "editStyleDeclaration"sv, edit).get_bool("ok"sv), Optional<bool> { false });
    target->run_javascript("document.title = getComputedStyle(document.getElementById('subject')).marginLeft;"_string);
    wait_for([&] { return target->title() == "4px"_utf16; });

    Optional<JsonObject> remote_tree;
    target->on_received_dom_tree = [&](JsonObject dom) { remote_tree = move(dom); };
    target->inspect_dom_tree();
    wait_for([&] { return remote_tree.has_value(); });
    target->did_disconnect_devtools_client();
}

TEST_CASE(inspector_cosmetic_filters_preserve_other_profile_settings)
{
    Test::HeadlessTestEnvironment environment { "TestInspectElementFilters"sv };
    auto app = TRY_OR_FAIL(environment.create_application());
    auto target = make_view();
    auto session = WebView::InspectorSession::ensure_for_target_view(*target);
    auto& settings = WebView::Application::settings();
    settings.set_custom_content_blocker_filters("example.test##.existing\n"_string);

    auto saved = session->save_cosmetic_filter_rule("example.test"_string, ".fixture"_string);
    EXPECT(saved.get_bool("saved"sv).value_or(false));
    EXPECT(session->save_cosmetic_filter_rule("example.test"_string, ".fixture"_string).get_bool("duplicate"sv).value_or(false));
    settings.set_custom_content_blocker_filters("example.test##.existing\nexample.test##.fixture\nexample.test##.concurrent\n"_string);
    auto undone = session->undo_cosmetic_filter_rule(saved.get_u64("filterJournalId"sv).value());
    EXPECT(undone.get_bool("undone"sv).value_or(false));
    EXPECT_EQ(settings.custom_content_blocker_filters(), "example.test##.existing\nexample.test##.concurrent\n"sv);

    auto private_view = make_view(WebView::IsPrivate::Yes);
    auto private_session = WebView::InspectorSession::ensure_for_target_view(*private_view);
    EXPECT(!private_session->save_cosmetic_filter_rule("example.test"_string, ".private"_string).get_bool("saved"sv).value_or(true));
    EXPECT_EQ(settings.custom_content_blocker_filters(), "example.test##.existing\nexample.test##.concurrent\n"sv);

    constexpr size_t filter_limit = 4 * MiB;
    constexpr auto rule_line = "example.test##.at-limit\n"sv;
    auto comment = TRY_OR_FAIL(String::repeated('!', filter_limit - rule_line.length() - 1));
    settings.set_custom_content_blocker_filters(comment);
    EXPECT_EQ(session->save_cosmetic_filter_rule("example.test"_string, ".at-limit"_string).get_bool("saved"sv), Optional<bool> { true });
    auto at_limit = settings.custom_content_blocker_filters();
    EXPECT_EQ(at_limit.bytes_as_string_view().length(), filter_limit);
    EXPECT(at_limit.ends_with_bytes(rule_line));
    EXPECT_EQ(session->save_cosmetic_filter_rule("example.test"_string, ".over-limit"_string).get_bool("saved"sv), Optional<bool> { false });
    EXPECT(settings.custom_content_blocker_filters() == at_limit);
}
