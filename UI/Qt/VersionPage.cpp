/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <AK/NeverDestroyed.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibCore/BrowserVersion.h>
#include <LibCore/System.h>
#include <LibWebCommon/Loader/UserAgent.h>
#include <LibWebView/BuildInformation.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/Theme.h>
#include <UI/Qt/VersionPage.h>

namespace Ladybird {

static Vector<VersionPage*>& live_version_pages()
{
    static NeverDestroyed<Vector<VersionPage*>> pages;
    return *pages;
}

static JsonObject serialize_mark_palette(Theme::PageMarkColors const& colors)
{
    auto hex = [](QColor const& color) {
        return MUST(String::formatted("#{:02x}{:02x}{:02x}", color.red(), color.green(), color.blue()));
    };
    JsonObject palette;
    palette.set("light"_string, hex(colors.light));
    palette.set("back"_string, hex(colors.back));
    palette.set("mid"_string, hex(colors.mid));
    palette.set("deep"_string, hex(colors.deep));
    palette.set("bird"_string, hex(colors.bird));
    palette.set("shadow"_string, hex(colors.shadow));
    palette.set("solid"_string, hex(colors.solid));
    return palette;
}

static JsonObject mark_colors_json()
{
    JsonObject mark_colors;
    mark_colors.set("light"_string, serialize_mark_palette(Theme::page_mark_colors(false)));
    mark_colors.set("dark"_string, serialize_mark_palette(Theme::page_mark_colors(true)));
    return mark_colors;
}

VersionPage::VersionPage(u64 web_ui_id)
    : WebUIPage(web_ui_id)
{
    live_version_pages().append(this);
}

VersionPage::~VersionPage()
{
    live_version_pages().remove_all_matching([this](VersionPage* page) { return page == this; });
}

void VersionPage::notify_accent_changed()
{
    auto colors = mark_colors_json();
    for (auto* page : live_version_pages())
        page->send_message("markColorChanged"_string, colors);
}

void VersionPage::received_message(String const& name, JsonValue const&)
{
    if (name == "loadVersionInfo"sv)
        load_version_info();
}

void VersionPage::load_version_info()
{
    static NeverDestroyed<String> browser_name = String::from_utf8_without_validation({ BROWSER_BRAND_NAME, __builtin_strlen(BROWSER_BRAND_NAME) });
    static NeverDestroyed<String> engine_name = String::from_utf8_without_validation({ BROWSER_ENGINE_NAME, __builtin_strlen(BROWSER_ENGINE_NAME) });
    static NeverDestroyed<String> engine_version = String::from_utf8_without_validation({ BROWSER_COMPATIBILITY_VERSION, __builtin_strlen(BROWSER_COMPATIBILITY_VERSION) });
    static NeverDestroyed<String> internal_name = String::from_utf8_without_validation({ BROWSER_INTERNAL_NAME, __builtin_strlen(BROWSER_INTERNAL_NAME) });
    static NeverDestroyed<String> browser_version = String::from_utf8_without_validation({ BROWSER_VERSION, __builtin_strlen(BROWSER_VERSION) });
    static NeverDestroyed<String> release_stage = String::from_utf8_without_validation({ BROWSER_RELEASE_STAGE, __builtin_strlen(BROWSER_RELEASE_STAGE) });
    static NeverDestroyed<String> release_channel = String::from_utf8_without_validation({ BROWSER_RELEASE_CHANNEL, __builtin_strlen(BROWSER_RELEASE_CHANNEL) });
    static NeverDestroyed<String> product_name = String::from_utf8_without_validation({ BROWSER_PRODUCT_NAME, __builtin_strlen(BROWSER_PRODUCT_NAME) });
    static NeverDestroyed<String> browser_commit = String::from_utf8_without_validation(WebView::LADYBIRD_COMMIT.bytes());
    static NeverDestroyed<String> arch = String::from_utf8_without_validation({ CPU_STRING, __builtin_strlen(CPU_STRING) });
    static NeverDestroyed<String> platform_name = String::from_utf8_without_validation({ OS_STRING, __builtin_strlen(OS_STRING) });
    static NeverDestroyed<String> command_line = Application::the().command_line();
    static NeverDestroyed<String> executable_path = MUST(String::from_byte_string(MUST(Core::System::current_executable_path())));

    JsonObject version_info;
    version_info.set("browserName"_string, *browser_name);
    version_info.set("engineName"_string, *engine_name);
    version_info.set("engineVersion"_string, *engine_version);
    version_info.set("internalName"_string, *internal_name);
    version_info.set("browserVersion"_string, *browser_version);
    version_info.set("releaseStage"_string, *release_stage);
    version_info.set("releaseChannel"_string, *release_channel);
    version_info.set("isReleaseBuild"_string, static_cast<bool>(BROWSER_IS_RELEASE_BUILD));
    version_info.set("isFinalRelease"_string, static_cast<bool>(BROWSER_IS_FINAL_RELEASE));
    version_info.set("productName"_string, *product_name);
    version_info.set("browserCommit"_string, *browser_commit);
    version_info.set("arch"_string, *arch);
    version_info.set("platformName"_string, *platform_name);
    version_info.set("commandLine"_string, *command_line);
    version_info.set("executablePath"_string, *executable_path);
    version_info.set("markColors"_string, mark_colors_json());

    send_message("renderVersionInfo"_string, move(version_info));
}

}
