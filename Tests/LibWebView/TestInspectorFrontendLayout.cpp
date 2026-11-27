/*
 * Copyright (c) 2026, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// The Inspector frontend as Ladyflock renders it, bound to a real target page: layout invariants a screenshot review
// found broken, and the console helpers its panes depend on. Set LADYFLOCK_INSPECTOR_SCREENSHOTS to a directory to
// also save a screenshot of each state the test visits.

#include "HeadlessTest.h"
#include <AK/LexicalPath.h>
#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibCore/Timer.h>
#include <LibGfx/SystemTheme.h>
#include <LibTest/TestCase.h>
#include <LibURL/InternalURLs.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/InspectorSession.h>
#include <LibWebView/Utilities.h>

namespace {

static void wait_for(Function<bool()> condition, unsigned seconds = 10)
{
    bool timed_out = false;
    auto timer = Core::Timer::create_single_shot(AK::Duration::from_seconds(seconds), [&] { timed_out = true; });
    timer->start();
    Core::EventLoop::current().spin_until([&] { return timed_out || condition(); });
    timer->stop();
    VERIFY(!timed_out);
}

static auto make_view(Web::DevicePixelSize size)
{
    auto theme = MUST(Gfx::load_system_theme(LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv).string()));
    return WebView::HeadlessWebView::create(move(theme), size);
}

// Runs an async function body in the frontend and returns what it resolves to.
static JsonValue evaluate(WebView::HeadlessWebView& view, StringView body)
{
    static u64 next_id = 1;
    auto prefix = ByteString::formatted("EVAL-{}:", next_id++);
    auto script = MUST(String::formatted(R"JS(
        (async () => {{
            const until = async (condition, ms = 8000) => {{
                const start = Date.now();
                while (!condition()) {{
                    if (Date.now() - start > ms)
                        throw new Error("Timed out waiting for " + condition);
                    await new Promise(resolve => setTimeout(resolve, 25));
                }}
            }};
            const frames = () => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
            const app = window.ladyflockInspector;
            const findElement = predicate => [...app.elementsTree.nodesById.values()].find(n => n.type === "element" && predicate(n));
            const select = async (predicate, sidebarReady = () => true) => {{
                const node = findElement(predicate);
                app.elementsTree.selectNode(node.id, null, {{ reveal: true, focusRow: true }});
                await until(sidebarReady);
                await frames();
                return node;
            }};
            try {{
                const value = await (async () => {{ {} }})();
                document.title = "{}" + JSON.stringify({{ ok: true, value }});
            }} catch (error) {{
                document.title = "{}" + JSON.stringify({{ ok: false, error: String(error && error.stack || error) }});
            }}
        }})();
    )JS",
        body, prefix, prefix));
    view.run_javascript(script);
    wait_for([&] { return view.title().to_byte_string().starts_with(prefix); }, 20);
    auto title = view.title().to_byte_string();
    auto result = MUST(JsonValue::from_string(title.substring_view(prefix.length())));
    auto const& object = result.as_object();
    if (!object.get_bool("ok"sv).value_or(false)) {
        warnln("Frontend script failed: {}", object.get_string("error"sv).value_or({}));
        VERIFY_NOT_REACHED();
    }
    return object.get("value"sv).value_or(JsonValue {});
}

// Evaluates a check that returns null when it holds, or a description of what is wrong.
static void expect_holds(WebView::HeadlessWebView& view, StringView description, StringView body)
{
    auto problem = evaluate(view, body);
    if (!problem.is_null())
        warnln("{}: {}", description, problem.serialized());
    EXPECT(problem.is_null());
}

static void capture(WebView::HeadlessWebView& view, StringView name)
{
    auto const* directory = getenv("LADYFLOCK_INSPECTOR_SCREENSHOTS");
    if (!directory)
        return;
    evaluate(view, "await frames(); await new Promise(resolve => setTimeout(resolve, 150)); await frames();"sv);
    bool done = false;
    Optional<LexicalPath> path;
    view.take_screenshot(WebView::ViewImplementation::ScreenshotType::Visible)
        ->when_resolved([&](auto const& saved) {
            path = saved;
            done = true;
        })
        .when_rejected([&](auto& error) {
            warnln("Screenshot failed: {}", error);
            done = true;
        });
    wait_for([&] { return done; });
    if (path.has_value())
        MUST(Core::System::rename(path->string(), ByteString::formatted("{}/{}.png", directory, name)));
}

constexpr auto fixture_html = R"HTML(<!doctype html><title>Inspector layout fixture</title>
<style>
  .pay { color: #30343a; background-color: rgb(240, 240, 250); padding: 6px 12px; border-radius: 6px; }
  .pay { color: green !important; }
  .grid { display: grid; grid-template-columns: repeat(3, minmax(80px, 1fr)) [end-of-a-very-long-line-name-that-wraps]; gap: 8px; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif, "Apple Color Emoji"; }
  .row { display: flex; justify-content: space-between; align-items: center; }
  body { margin: 0 auto; max-width: 720px; font: 15px/1.5 system-ui, sans-serif; }
  .checkout-form { display: flex; flex-direction: column; gap: 6px; padding: 12px; border: 1px solid #ccc; }
</style>
<header class="row site-header" data-test="header" aria-label="Site header">
  <h1 id="title" class="headline main-title">Checkout</h1>
  <nav><a href="#a">Home</a> <a href="#b" aria-current="page">Cart</a></nav>
</header>
<form id="checkout" class="checkout-form">
  <label for="email">Email</label>
  <input id="email" type="email" placeholder="you@example.com">
  <button class="pay primary-action extra-class-one extra-class-two" type="button">Pay now</button>
</form>
<section class="grid">
  <div class="cell">One</div><div class="cell">Two</div><div class="cell">Three</div>
  <div class="cell" style="color: blue; margin: 4px 2px 0 1px;">Four</div>
</section>
<p id="ticker">0</p>
<!-- a comment node -->
<div id="host"></div>
<ul>
  <li>Item 1</li><li>Item 2</li><li>Item 3</li><li>Item 4</li><li>Item 5</li><li>Item 6</li><li>Item 7</li><li>Item 8</li>
  <li>Item 9</li><li>Item 10</li><li>Item 11</li><li>Item 12</li><li>Item 13</li><li>Item 14</li><li>Item 15</li><li>Item 16</li>
</ul>
<script>
  var values = "page global";
  document.querySelector(".pay").addEventListener("click", function onPayClick() {}, { passive: true });
  document.getElementById("host").attachShadow({ mode: "open" }).innerHTML = "<span>shadow content</span>";
  document.title = "Inspector layout fixture ready";
</script>
)HTML"sv;

struct Fixture {
    NonnullOwnPtr<WebView::HeadlessWebView> target;
    NonnullOwnPtr<WebView::HeadlessWebView> frontend;
    RefPtr<WebView::InspectorSession> session;
};

struct FixturePage {
    StringView html;
    StringView ready_title;
    size_t minimum_tree_size { 20 };
};

constexpr FixturePage layout_fixture { fixture_html, "Inspector layout fixture ready"sv };

static Fixture open_inspector(Web::DevicePixelSize frontend_size, FixturePage const& page = layout_fixture)
{
    auto target = make_view({ 800, 600 });
    target->load_html(page.html);
    wait_for([&] { return target->title().to_byte_string() == page.ready_title; });

    auto frontend = make_view(frontend_size);
    auto session = WebView::InspectorSession::authorize_frontend_view(*target, frontend->view_id());
    VERIFY(session);
    bool loaded = false;
    frontend->on_load_finish = [&](auto const& url) { loaded = url == URL::about_inspector(); };
    frontend->load_from_browser_ui(URL::about_inspector());
    wait_for([&] { return loaded; });
    frontend->on_load_finish = nullptr;
    wait_for([&] { return session->is_attached(); });

    evaluate(*frontend, MUST(String::formatted(R"JS(
        window.cspViolations = [];
        document.addEventListener("securitypolicyviolation", e => window.cspViolations.push(`${{e.violatedDirective}}: ${{e.target?.outerHTML?.slice(0, 120)}}`), true);
        await until(() => app.elementsTree.nodesById.size > {});
    )JS", page.minimum_tree_size)));
    return { move(target), move(frontend), move(session) };
}

constexpr auto select_pay_button = R"JS(await select(n => String(n.attributes?.class || "").includes("pay"), () => document.querySelectorAll("#sidebar-styles .decl-row").length > 3);)JS"sv;

}

TEST_CASE(inspector_frontend_layout)
{
    Test::HeadlessTestEnvironment environment { "TestInspectorFrontendLayout"sv };
    Vector<StringView> arguments;
    ByteString screenshot_path;
    if (auto const* directory = getenv("LADYFLOCK_INSPECTOR_SCREENSHOTS")) {
        screenshot_path = ByteString::formatted("{}/current.png", directory);
        arguments.append("--screenshot-path"sv);
        arguments.append(screenshot_path);
    }
    auto app = TRY_OR_FAIL(environment.create_application(arguments));
    auto fixture = open_inspector({ 1000, 620 });
    auto& frontend = *fixture.frontend;

    evaluate(frontend, select_pay_button);
    capture(frontend, "01-elements-styles"sv);

    // A tab strip scrolls sideways only; a strip that is taller inside than out scrolls vertically under a trackpad.
    expect_holds(frontend, "Strips scroll only sideways"sv, R"JS(
        app.drawers.open("console");
        await frames();
        const bad = [];
        for (const id of ["panel-tabs", "sidebar-tabs", "drawer-tabs", "dom-breadcrumbs"]) {
            const strip = document.getElementById(id);
            if (strip.scrollHeight > strip.clientHeight || getComputedStyle(strip).overflowY !== "hidden")
                bad.push({ id, scrollHeight: strip.scrollHeight, clientHeight: strip.clientHeight, overflowY: getComputedStyle(strip).overflowY });
            const selected = strip.querySelector('[aria-selected="true"]');
            if (selected && parseFloat(getComputedStyle(selected, "::after").bottom) < 0)
                bad.push({ id, underlineBottom: getComputedStyle(selected, "::after").bottom });
        }
        app.drawers.close();
        return bad.length ? bad : null;
    )JS"sv);

    // A declaration's name, colon, value and semicolon are one line of text, and its checkbox sits on that line.
    expect_holds(frontend, "Declarations read as one line"sv, R"JS(
        const bad = [];
        for (const row of document.querySelectorAll("#sidebar-styles .decl-row")) {
            const name = row.querySelector(".decl-prop").getBoundingClientRect();
            const value = row.querySelector(".decl-val").getClientRects()[0];
            const toggle = row.querySelector(".decl-toggle").getBoundingClientRect();
            const gap = value.left - name.right;
            if (Math.abs(name.top - value.top) > 1 || Math.abs((toggle.top + toggle.bottom) / 2 - (name.top + name.bottom) / 2) > 2 || gap > 30)
                bad.push({ text: row.textContent, nameTop: name.top, valueTop: value.top, gap });
        }
        return bad.length ? bad : null;
    )JS"sv);

    evaluate(frontend, R"JS(await select(n => n.attributes?.class === "grid", () => [...document.querySelectorAll("#sidebar-styles .decl-prop")].some(p => p.textContent === "font-family"));)JS"sv);
    capture(frontend, "02-styles-wrapped-values"sv);
    expect_holds(frontend, "A long value wraps under its name"sv, R"JS(
        const row = [...document.querySelectorAll("#sidebar-styles .decl-row")].find(r => r.querySelector(".decl-prop").textContent === "font-family");
        const name = row.querySelector(".decl-prop").getBoundingClientRect();
        const lines = row.querySelector(".decl-val").getClientRects();
        return lines.length > 1 && Math.abs(lines[0].top - name.top) <= 1 ? null : { lines: lines.length, nameTop: name.top, firstValueTop: lines[0]?.top };
    )JS"sv);

    // The element menu is taller than a short bottom dock; it stays inside the frontend and scrolls.
    frontend.reset_viewport_size({ 1200, 300 });
    evaluate(frontend, "await new Promise(resolve => setTimeout(resolve, 300)); await frames();"sv);
    expect_holds(frontend, "The context menu stays on screen"sv, R"JS(
        const row = document.querySelector("#dom-tree .dom-row.selected");
        const r = row.getBoundingClientRect();
        row.dispatchEvent(new MouseEvent("contextmenu", { bubbles: true, clientX: r.x + 40, clientY: r.y + 5 }));
        await frames();
        const menu = document.getElementById("context-menu").getBoundingClientRect();
        return menu.top >= 0 && menu.bottom <= innerHeight && menu.left >= 0 && menu.right <= innerWidth ? null : { menu: [menu.top, menu.bottom], height: innerHeight };
    )JS"sv);
    capture(frontend, "03-bottom-dock-context-menu"sv);
    expect_holds(frontend, "Arrow keys move through the context menu, Escape closes it"sv, R"JS(
        const menu = document.getElementById("context-menu");
        const first = document.activeElement;
        first.dispatchEvent(new KeyboardEvent("keydown", { key: "ArrowDown", bubbles: true }));
        const moved = document.activeElement !== first && menu.contains(document.activeElement);
        document.activeElement.dispatchEvent(new KeyboardEvent("keydown", { key: "Escape", bubbles: true }));
        await frames();
        return moved && menu.classList.contains("hidden") ? null : { moved, hidden: menu.classList.contains("hidden") };
    )JS"sv);
    frontend.reset_viewport_size({ 1000, 620 });
    evaluate(frontend, "await new Promise(resolve => setTimeout(resolve, 300)); await frames();"sv);

    expect_holds(frontend, "Escape closes an open dropdown before toggling the drawer"sv, R"JS(
        const drawerWasOpen = app.drawers.isOpen;
        document.getElementById("btn-dock").click();
        await frames();
        document.body.dispatchEvent(new KeyboardEvent("keydown", { key: "Escape", bubbles: true }));
        await frames();
        const menuHidden = document.getElementById("dock-menu").classList.contains("hidden");
        return menuHidden && app.drawers.isOpen === drawerWasOpen ? null : { menuHidden, drawerOpen: app.drawers.isOpen };
    )JS"sv);

    expect_holds(frontend, "Dock menu labels line up whether or not they are chosen"sv, R"JS(
        document.getElementById("btn-dock").click();
        await frames();
        const lefts = [...document.querySelectorAll("#dock-menu .menu-item")].map(item => {
            const range = document.createRange();
            range.selectNodeContents(item);
            return Math.round(range.getClientRects()[0]?.left ?? -1);
        });
        document.body.click();
        return new Set(lefts).size === 1 ? null : lefts;
    )JS"sv);

    expect_holds(frontend, "Enter runs the command menu's active command"sv, R"JS(
        app.openCommandMenu();
        const input = document.getElementById("command-menu-input");
        input.value = "drawer: changes";
        input.dispatchEvent(new Event("input"));
        input.dispatchEvent(new KeyboardEvent("keydown", { key: "Enter", bubbles: true }));
        await frames();
        const ran = app.drawers.isOpen && app.drawers.activeDrawer === "changes";
        app.drawers.close();
        return ran && !document.getElementById("command-menu-dialog").open ? null : { open: app.drawers.isOpen, drawer: app.drawers.activeDrawer };
    )JS"sv);

    expect_holds(frontend, "Void elements have no closing tag, and user-agent shadow roots are hidden"sv, R"JS(
        await select(n => n.attributes?.id === "email");
        const bad = [...document.querySelectorAll("#dom-tree .dom-row")].map(row => row.textContent).filter(text => text.includes("</input>") || text.includes("user-agent"));
        app.selectSidebar("layout");
        const flexRows = [...document.querySelectorAll("#sidebar-layout .overlay-row")].map(row => row.textContent.trim());
        app.selectSidebar("styles");
        return bad.length || flexRows.includes("div") ? { bad, flexRows } : null;
    )JS"sv);

    evaluate(frontend, select_pay_button);
    for (auto sidebar : { "computed"sv, "layout"sv, "listeners"sv, "properties"sv, "accessibility"sv }) {
        evaluate(frontend, MUST(String::formatted(R"JS(app.selectSidebar("{}"); await new Promise(resolve => setTimeout(resolve, 400)); await frames();)JS", sidebar)));
        capture(frontend, ByteString::formatted("04-sidebar-{}", sidebar));
    }
    expect_holds(frontend, "Computed groups its properties when asked"sv, R"JS(
        app.selectSidebar("computed");
        await until(() => document.querySelectorAll("#sidebar-computed .computed-table td.decl-prop").length > 5);
        app.sidebars.computedGrouped = true;
        app.sidebars.renderComputed();
        const groups = [...document.querySelectorAll("#sidebar-computed tr.computed-group")].map(row => row.textContent);
        app.sidebars.computedGrouped = false;
        app.selectSidebar("styles");
        return groups.includes("Layout") && groups.includes("Text") ? null : groups;
    )JS"sv);

    for (auto drawer : { "console"sv, "changes"sv, "engine-lens"sv, "rendering"sv, "sensors"sv }) {
        evaluate(frontend, MUST(String::formatted(R"JS(app.drawers.open("{}"); await new Promise(resolve => setTimeout(resolve, 400)); await frames();)JS", drawer)));
        capture(frontend, ByteString::formatted("05-drawer-{}", drawer));
    }
    evaluate(frontend, "app.drawers.close(); await frames();"sv);
    for (auto panel : { "sources"sv, "application"sv, "audits"sv }) {
        evaluate(frontend, MUST(String::formatted(R"JS(app.selectPanel("{}"); await new Promise(resolve => setTimeout(resolve, 400)); await frames();)JS", panel)));
        capture(frontend, ByteString::formatted("06-panel-{}", panel));
    }

    // An inline script comes with its whole document, so its first line is line 1 of the page.
    expect_holds(frontend, "Sources numbers a document from line 1"sv, R"JS(
        app.selectPanel("sources");
        await until(() => document.querySelector("#panel-sources .code-line"));
        const first = document.querySelector("#panel-sources .code-line");
        const problem = first.dataset.line === "1" && first.textContent.includes("<!doctype html>") ? null : first.textContent;
        app.selectPanel("elements");
        return problem;
    )JS"sv);

    // A vertical wheel or two-finger swipe over a strip of tabs scrolls it sideways, and nothing else.
    expect_holds(frontend, "A vertical wheel scrolls a tab strip sideways"sv, R"JS(
        const strip = document.getElementById("sidebar-tabs");
        strip.scrollLeft = 0;
        const wheel = new WheelEvent("wheel", { deltaY: 120, bubbles: true, cancelable: true });
        strip.dispatchEvent(wheel);
        await frames();
        const result = strip.scrollLeft > 0 && wheel.defaultPrevented ? null : { scrollLeft: strip.scrollLeft, prevented: wheel.defaultPrevented };
        strip.scrollLeft = 0;
        return result;
    )JS"sv);

    expect_holds(frontend, "The Elements splitter resizes by keyboard and by dragging"sv, R"JS(
        const splitter = document.getElementById("elements-splitter");
        const tree = document.getElementById("elements-tree-pane");
        const before = tree.getBoundingClientRect().width;
        splitter.focus();
        splitter.dispatchEvent(new KeyboardEvent("keydown", { key: "ArrowLeft", bubbles: true }));
        await frames();
        const afterKey = tree.getBoundingClientRect().width;
        const split = document.getElementById("elements-split").getBoundingClientRect();
        splitter.dispatchEvent(new PointerEvent("pointerdown", { pointerId: 7, button: 0, clientX: split.left + split.width * 0.3, bubbles: true }));
        splitter.dispatchEvent(new PointerEvent("pointermove", { pointerId: 7, clientX: split.left + split.width * 0.3, bubbles: true }));
        splitter.dispatchEvent(new PointerEvent("pointerup", { pointerId: 7, bubbles: true }));
        await frames();
        const afterDrag = tree.getBoundingClientRect().width;
        const dragged = Math.abs(afterDrag - split.width * 0.3) < 3;
        return afterKey < before && dragged && !document.body.classList.contains("splitter-dragging") ? null : { before, afterKey, afterDrag, expected: split.width * 0.3 };
    )JS"sv);

    frontend.reset_viewport_size({ 1200, 300 });
    evaluate(frontend, "await new Promise(resolve => setTimeout(resolve, 300)); await frames();"sv);
    expect_holds(frontend, "The drawer leaves the panel room in a short bottom dock"sv, R"JS(
        app.drawers.open("console");
        await frames();
        const panel = document.getElementById("panel-host").getBoundingClientRect().height;
        const drawer = document.getElementById("inspector-drawer").getBoundingClientRect().height;
        app.drawers.close();
        return panel >= 100 && drawer >= 100 ? null : { panel, drawer };
    )JS"sv);

    frontend.reset_viewport_size({ 460, 700 });
    evaluate(frontend, "await new Promise(resolve => setTimeout(resolve, 300)); await frames();"sv);
    capture(frontend, "07-narrow"sv);
    expect_holds(frontend, "Hidden tabs are hinted at with a fade"sv, R"JS(
        const strip = document.getElementById("panel-tabs");
        return strip.classList.contains("tabs-fade-end") ? null : strip.className;
    )JS"sv);

    expect_holds(frontend, "Every style the frontend sets is allowed by its CSP"sv, "return window.cspViolations.length ? window.cspViolations : null;"sv);
}

TEST_CASE(inspector_frontend_edits)
{
    Test::HeadlessTestEnvironment environment { "TestInspectorFrontendEdits"sv };
    auto app = TRY_OR_FAIL(environment.create_application());
    auto fixture = open_inspector({ 1000, 620 });
    auto& frontend = *fixture.frontend;
    evaluate(frontend, select_pay_button);

    // Editing one declaration leaves the rule's shorthands as written, not expanded into longhands.
    expect_holds(frontend, "An edited rule keeps its shorthands"sv, R"JS(
        const rule = app.stylesPane.rules.find(r => (r.declarations || []).some(d => d.name === "padding"));
        const decl = rule.declarations.find(d => d.name === "border-radius");
        await app.stylesPane.commitDeclarationEdit(rule, decl, rule.declarations.indexOf(decl), "border-radius", "7px");
        await until(() => app.stylesPane.rules.some(r => (r.declarations || []).some(d => d.name === "border-radius" && d.value === "7px")));
        const names = app.stylesPane.rules.find(r => (r.declarations || []).some(d => d.name === "border-radius")).declarations.map(d => d.name);
        return names.includes("padding") && !names.includes("padding-top") ? null : names;
    )JS"sv);

    // Turning a declaration off keeps its row, unchecked, so it can be turned on again.
    expect_holds(frontend, "A declaration turned off can be turned on again"sv, R"JS(
        const findRow = () => [...document.querySelectorAll("#sidebar-styles .decl-row")].find(row => row.querySelector(".decl-prop")?.textContent === "border-radius");
        const toggle = findRow().querySelector(".decl-toggle");
        toggle.checked = false;
        toggle.dispatchEvent(new Event("change"));
        await until(() => findRow()?.classList.contains("disabled"));
        await until(() => app.state.lastComputedStyle?.["border-top-left-radius"] === "0px");
        const offChecked = findRow().querySelector(".decl-toggle").checked;
        const onToggle = findRow().querySelector(".decl-toggle");
        onToggle.checked = true;
        onToggle.dispatchEvent(new Event("change"));
        await until(() => findRow() && !findRow().classList.contains("disabled"));
        await until(() => app.state.lastComputedStyle?.["border-top-left-radius"] === "7px");
        return offChecked ? { offChecked } : null;
    )JS"sv);

    // A page that keeps changing does not take away an attribute being edited, and Enter returns to the tree.
    expect_holds(frontend, "An inline edit survives page mutations"sv, R"JS(
        const value = [...document.querySelectorAll("#dom-tree .dom-attr-val")].find(span => span.textContent === "checkout-form");
        value.dispatchEvent(new MouseEvent("dblclick", { bubbles: true }));
        const input = document.querySelector("#dom-tree input.dom-inline-input");
        input.value = "checkout-form edited";
        app.elementsTree.handleMutation({ documentGeneration: app.state.documentGeneration, removed: [] });
        await new Promise(resolve => setTimeout(resolve, 400));
        const survived = input.isConnected && input.value === "checkout-form edited";
        input.dispatchEvent(new KeyboardEvent("keydown", { key: "Enter", bubbles: true }));
        await until(() => [...document.querySelectorAll("#dom-tree .dom-attr-val")].some(span => span.textContent === "checkout-form edited"));
        return survived && document.activeElement === document.getElementById("dom-tree") ? null : { survived, focus: document.activeElement?.id };
    )JS"sv);
}

// A page that keeps changing re-renders the tree many times a second; that must not move or interrupt anything.
TEST_CASE(inspector_frontend_on_a_living_page)
{
    Test::HeadlessTestEnvironment environment { "TestInspectorFrontendLivingPage"sv };
    auto app = TRY_OR_FAIL(environment.create_application());
    auto fixture = open_inspector({ 1000, 300 });
    auto& frontend = *fixture.frontend;
    evaluate(frontend, select_pay_button);
    evaluate(frontend, R"JS(
        for (const li of [...app.elementsTree.nodesById.values()].filter(n => n.name?.toLowerCase() === "ul"))
            app.elementsTree.expanded.add(String(li.id));
        app.elementsTree.render();
        await frames();
    )JS"sv);
    fixture.target->run_javascript("setInterval(() => { document.getElementById('ticker').textContent = String(Date.now()); }, 40);"_string);

    expect_holds(frontend, "The tree keeps its scroll position while the page mutates"sv, R"JS(
        const tree = document.getElementById("dom-tree");
        tree.scrollTop = 120;
        await new Promise(resolve => setTimeout(resolve, 600));
        return tree.scrollTop === 120 ? null : { scrollTop: tree.scrollTop, scrollHeight: tree.scrollHeight, clientHeight: tree.clientHeight };
    )JS"sv);

    expect_holds(frontend, "A context menu opened on another row stays open"sv, R"JS(
        const styles = document.getElementById("sidebar-styles");
        styles.scrollTop = styles.scrollHeight;
        const rows = [...document.querySelectorAll("#dom-tree .dom-row")].filter(row => !row.classList.contains("selected") && row.getAttribute("role") === "treeitem");
        const row = rows[rows.length - 1];
        const r = row.getBoundingClientRect();
        row.dispatchEvent(new MouseEvent("contextmenu", { bubbles: true, clientX: r.x + 40, clientY: r.y + 5 }));
        await new Promise(resolve => setTimeout(resolve, 500));
        const open = !document.getElementById("context-menu").classList.contains("hidden");
        document.body.click();
        return open ? null : "closed";
    )JS"sv);
}

// Reloading the frontend keeps it bound to its tab.
TEST_CASE(inspector_frontend_reload)
{
    Test::HeadlessTestEnvironment environment { "TestInspectorFrontendReload"sv };
    auto app = TRY_OR_FAIL(environment.create_application());
    auto fixture = open_inspector({ 1000, 620 });
    auto& frontend = *fixture.frontend;
    bool reloaded = false;
    frontend.on_load_finish = [&](auto const& url) { reloaded = url == URL::about_inspector(); };
    frontend.reload();
    wait_for([&] { return reloaded; });
    frontend.on_load_finish = nullptr;
    expect_holds(frontend, "A reloaded Inspector is still bound"sv, R"JS(
        await until(() => app.state.bound && app.elementsTree.nodesById.size > 20);
        return document.getElementById("workspace").classList.contains("hidden") ? "unbound" : null;
    )JS"sv);
    EXPECT(fixture.session->has_current_frontend());
}

TEST_CASE(inspector_console_helpers)
{
    Test::HeadlessTestEnvironment environment { "TestInspectorConsoleHelpers"sv };
    auto app = TRY_OR_FAIL(environment.create_application());
    auto fixture = open_inspector({ 1000, 620 });
    auto& frontend = *fixture.frontend;
    evaluate(frontend, select_pay_button);

    auto results = evaluate(frontend, R"JS(
        const { sendCommand } = await import("resource://ladybird/about-pages/inspector/bridge.js");
        const run = async expression => {
            try {
                return await Promise.race([sendCommand("consoleEvaluate", { expression }), new Promise((_, reject) => setTimeout(() => reject(new Error("no reply")), 3000))]);
            } catch (error) {
                return { error: error.message };
            }
        };
        return {
            selected: await run("$0.className"),
            query: await run("$('#title').textContent"),
            pageGlobalWins: await run("values"),
            thrown: await run("nope.boom"),
            afterThrow: await run("1 + 1"),
        };
    )JS"sv);
    outln("Console helper results: {}", results.serialized());
    auto const& object = results.as_object();
    EXPECT_EQ(object.get_object("selected"sv)->get_string("value"sv), "pay primary-action extra-class-one extra-class-two"sv);
    EXPECT_EQ(object.get_object("query"sv)->get_string("value"sv), "Checkout"sv);
    EXPECT_EQ(object.get_object("pageGlobalWins"sv)->get_string("value"sv), "page global"sv);
    EXPECT_EQ(object.get_object("thrown"sv)->get_bool("thrown"sv), Optional<bool> { true });
    EXPECT_EQ(object.get_object("afterThrow"sv)->get_double_with_precision_loss("value"sv), Optional<double> { 2 });

    // The pane reads $0 too, and a DOM element's properties are accessors on its prototypes.
    expect_holds(frontend, "Properties lists the selected element's prototype chain"sv, R"JS(
        app.selectSidebar("properties");
        await until(() => document.querySelectorAll("#sidebar-properties tr.computed-group").length > 2);
        const sections = [...document.querySelectorAll("#sidebar-properties tr.computed-group")].map(row => row.textContent);
        const names = [...document.querySelectorAll("#sidebar-properties td.decl-prop")].map(cell => cell.textContent);
        return sections.includes("HTMLButtonElement") && sections.includes("Element") && names.includes("disabled") ? null : { sections, names: names.slice(0, 10) };
    )JS"sv);
}

// Waits for the target page's title to become the given one, after running a script that sets it.
static void run_in_target(WebView::HeadlessWebView& target, StringView script, StringView expected_title_prefix)
{
    target.run_javascript(MUST(String::from_utf8(script)));
    wait_for([&] { return target.title().to_byte_string().starts_with(expected_title_prefix); });
}

constexpr auto handlers_html = R"HTML(<!doctype html><title>Handlers</title>
<button id="attr" onclick="window.attributeClicks = (window.attributeClicks || 0) + 1">Attribute handler</button>
<button id="prop">Property handler</button>
<script>
  document.getElementById("prop").onclick = function propertyHandler() {};
  document.title = "Handlers ready";
</script>
)HTML"sv;

// An event handler's listener is named after the handler, and removing it clears the handler, which can then be set again.
TEST_CASE(inspector_event_handler_listeners)
{
    Test::HeadlessTestEnvironment environment { "TestInspectorEventHandlerListeners"sv };
    auto app = TRY_OR_FAIL(environment.create_application());
    auto fixture = open_inspector({ 1000, 620 }, { handlers_html, "Handlers ready"sv, 5 });
    auto& frontend = *fixture.frontend;

    for (auto id : { "attr"sv, "prop"sv }) {
        evaluate(frontend, MUST(String::formatted(R"JS(
            await select(n => n.attributes?.id === "{}");
            app.selectSidebar("listeners");
            await until(() => document.querySelectorAll("#sidebar-listeners .listener-row").length > 0);
            const row = [...document.querySelectorAll("#sidebar-listeners .listener-row")].find(r => r.querySelector(".listener-desc").textContent.includes("#{}"));
            if (!row)
                throw new Error("No listener row for #{}: " + document.getElementById("sidebar-listeners").textContent);
            if (!row.querySelector(".listener-desc").textContent.includes("onclick"))
                throw new Error("The handler's listener is not named onclick: " + row.textContent);
            [...row.querySelectorAll("button")].find(b => b.textContent === "Remove").click();
            await until(() => ![...document.querySelectorAll("#sidebar-listeners .listener-row")].some(r => r.querySelector(".listener-desc").textContent.includes("#{}")));
        )JS", id, id, id, id)));
    }

    run_in_target(*fixture.target, R"JS(
        const results = [];
        for (const id of ["attr", "prop"]) {
            const button = document.getElementById(id);
            const cleared = button.onclick === null;
            let later = 0;
            button.onclick = () => { later++; };
            button.click();
            results.push(`${id}:${cleared}:${later}`);
        }
        document.title = "handlers:" + results.join(",") + ":" + (window.attributeClicks || 0);
    )JS"sv, "handlers:"sv);
    EXPECT_EQ(fixture.target->title().to_byte_string(), "handlers:attr:true:1,prop:true:1:0"sv);
}

constexpr auto navigation_html = R"HTML(<!doctype html><title>First</title>
<a id="javascript-link" href="javascript:void(0)">Does nothing</a>
<p id="first">First page</p>
<script>document.title = "First ready";</script>
)HTML"sv;

// A load that never replaces the document leaves the Inspector as it was, and a new document is shown as soon as it
// replaces the old one, never the old one under the new document's generation.
TEST_CASE(inspector_resets_when_a_document_replaces_another)
{
    Test::HeadlessTestEnvironment environment { "TestInspectorDocumentReplacement"sv };
    auto app = TRY_OR_FAIL(environment.create_application());
    auto fixture = open_inspector({ 1000, 620 }, { navigation_html, "First ready"sv, 5 });
    auto& frontend = *fixture.frontend;
    auto& target = *fixture.target;

    evaluate(frontend, R"JS(
        await select(n => n.attributes?.id === "first");
        window.treesSet = [];
        const setTree = app.elementsTree.setTree.bind(app.elementsTree);
        app.elementsTree.setTree = tree => {
            const ids = [];
            const walk = node => {
                if (!node || typeof node !== "object")
                    return;
                if (node.id !== undefined)
                    ids.push(node.id);
                for (const value of Object.values(node))
                    if (value && typeof value === "object")
                        walk(value);
            };
            walk(tree);
            const text = JSON.stringify(tree);
            window.treesSet.push({ generation: app.state.documentGeneration, ids, first: text.includes('"first"'), second: text.includes('"second"') });
            return setTree(tree);
        };
        window.firstGeneration = app.state.documentGeneration;
        window.previousIds = new Set([...app.elementsTree.nodesById.keys()]);
    )JS"sv);

    run_in_target(target, R"JS(
        document.getElementById("javascript-link").click();
        setTimeout(() => { document.title = "clicked"; }, 300);
    )JS"sv, "clicked"sv);
    expect_holds(frontend, "A javascript: link does not reset the Inspector"sv, R"JS(
        await new Promise(resolve => setTimeout(resolve, 300));
        const selected = app.state.selectedNode?.attributes?.id;
        return app.state.documentGeneration === window.firstGeneration && selected === "first" ? null : { generation: app.state.documentGeneration, first: window.firstGeneration, selected };
    )JS"sv);

    // NB: A tree may come before the new document has parsed its body, but none may hold the old document's nodes:
    //     its marker element, or for a reload of the same page, its node ids. Ids are only unique within a WebContent
    //     process, and these pages are loaded in different ones.
    auto expect_new_document = [&](StringView description, StringView expected, StringView previous) {
        expect_holds(frontend, description, MUST(String::formatted(R"JS(
            await until(() => app.state.documentGeneration > window.firstGeneration && findElement(n => n.attributes?.id === "{}"));
            const previous = "{}";
            const stale = window.treesSet.filter(t => t.generation > window.firstGeneration && (previous ? t[previous] : t.ids.some(id => window.previousIds.has(id))));
            window.firstGeneration = app.state.documentGeneration;
            window.previousIds = new Set([...app.elementsTree.nodesById.keys()]);
            window.treesSet = [];
            return stale.length ? stale.map(t => t.generation) : null;
        )JS", expected, previous)));
    };

    // NB: A file, not load_html(), whose navigation to about:srcdoc would replace the first page's history entry.
    auto second_path = LexicalPath::join(environment.directory(), "second.html"sv).string();
    auto second_page = TRY_OR_FAIL(Core::File::open(second_path, Core::File::OpenMode::Write));
    TRY_OR_FAIL(second_page->write_until_depleted("<!doctype html><title>Second</title><p id=second>Second page</p>"sv.bytes()));
    second_page->close();
    target.load_from_browser_ui(URL::create_with_file_scheme(second_path).release_value());
    expect_new_document("A navigation shows the new document"sv, "second"sv, "first"sv);

    target.reload();
    expect_new_document("A reload shows the reloaded document"sv, "second"sv, ""sv);

    target.traverse_the_history_by_delta(-1);
    expect_new_document("Going back shows the previous document"sv, "first"sv, "second"sv);
}
