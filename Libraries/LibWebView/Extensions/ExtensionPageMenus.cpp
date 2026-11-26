/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibWebView/CanonicalNavigable.h>
#include <LibWebView/Extensions/ExtensionManager.h>

namespace WebView::Extensions {

static bool supported_page_menu_properties(JsonObject const& properties, bool creating)
{
    bool supported = true;
    properties.for_each_member([&](auto const& key, auto const&) {
        if (!(creating && key == "id"sv) && !key.is_one_of("title"sv, "contexts"sv, "type"sv, "documentUrlPatterns"sv, "targetUrlPatterns"sv))
            supported = false;
    });
    if (properties.has("type"sv)) {
        auto type = properties.get_string("type"sv);
        if (!type.has_value() || *type != "normal"sv)
            return false;
    }
    return supported;
}

ErrorOr<void> ExtensionManager::change_page_menu(String const& extension_id, StringView method, JsonArray const& args)
{
    auto& items = m_page_menus.ensure(extension_id);
    if (method == "removeAll"sv && args.is_empty()) {
        items.clear();
        return {};
    }
    if (method == "remove"sv && args.size() == 1 && args.at(0).is_string()) {
        if (!items.remove_all_matching([&](auto const& item) { return item.id == args.at(0).as_string(); }))
            return Error::from_string_literal("Unknown menu item");
        return {};
    }

    bool creating = method == "create"sv;
    if (!(creating && args.size() == 1) && !(method == "update"sv && args.size() == 2 && args.at(0).is_string()))
        return Error::from_string_literal("Unsupported menu operation");
    auto const& properties = args.at(creating ? 0 : 1);
    if (!properties.is_object() || !supported_page_menu_properties(properties.as_object(), creating))
        return Error::from_string_literal("Only normal page menu items are supported");
    auto const& details = properties.as_object();
    auto title = details.get_string("title"sv);
    if ((creating || details.has("title"sv)) && (!title.has_value() || title->is_empty() || title->byte_count() > 512))
        return Error::from_string_literal("Expected a menu title of at most 512 bytes");

    MenuRegistration next {};
    size_t index = 0;
    if (creating) {
        auto id = details.get_string("id"sv);
        if (!id.has_value() || id->is_empty() || id->byte_count() > 256)
            return Error::from_string_literal("Expected a menu ID of at most 256 bytes");
        if (items.size() >= 64 || any_of(items, [&](auto const& item) { return item.id == *id; }))
            return Error::from_string_literal("Duplicate menu item ID or menu limit reached");
        next.id = *id;
    } else {
        while (index < items.size() && items[index].id != args.at(0).as_string())
            ++index;
        if (index == items.size())
            return Error::from_string_literal("Unknown menu item");
        next = items[index];
    }
    if (title.has_value())
        next.title = *title;
    if (details.has("contexts"sv)) {
        auto contexts = details.get_array("contexts"sv);
        if (!contexts.has_value() || contexts->is_empty() || contexts->size() > 16)
            return Error::from_string_literal("Expected a nonempty menu context list");
        next.contexts.clear();
        for (auto const& value : contexts->values()) {
            if (!value.is_string() || !value.as_string().is_one_of("all"sv, "page"sv, "frame"sv, "editable"sv, "link"sv, "image"sv, "video"sv, "audio"sv))
                return Error::from_string_literal("Unsupported menu context");
            next.contexts.append(value.as_string());
        }
    }
    for (auto key : { "documentUrlPatterns"sv, "targetUrlPatterns"sv }) {
        if (!details.has(key))
            continue;
        auto list = details.get_array(key);
        if (!list.has_value() || list->is_empty() || list->size() > 128)
            return Error::from_string_literal("Expected a nonempty menu URL pattern list");
        bool targets = key == "targetUrlPatterns"sv;
        auto& patterns = targets ? next.targets : next.documents;
        patterns.clear();
        if (targets)
            next.matches_abp_links = false;
        for (auto const& entry : list->values()) {
            if (!entry.is_string() || entry.as_string().byte_count() > 4096)
                return Error::from_string_literal("Invalid menu URL pattern");
            if (targets && entry.as_string() == "abp:*"sv) {
                next.matches_abp_links = true;
                continue;
            }
            auto pattern = Web::Extensions::MatchPattern::parse(entry.as_string());
            if (pattern.is_error())
                return Error::from_string_literal("Unsupported menu URL pattern");
            patterns.append(pattern.release_value());
        }
    }
    next.token = m_next_menu_token++;
    if (creating)
        items.append(move(next));
    else
        items[index] = move(next);
    return {};
}

bool ExtensionManager::MenuRegistration::matches(CanonicalNavigable const& frame, ExtensionMenuTarget const& target) const
{
    if (!documents.is_empty() && !any_of(documents, [&](auto const& pattern) { return pattern.matches(frame.active_document_url()); }))
        return false;
    if (!targets.is_empty() || matches_abp_links) {
        bool matches_target = any_of(targets, [&](auto const& pattern) {
            return (target.link_url.has_value() && pattern.matches(*target.link_url)) || (target.source_url.has_value() && pattern.matches(*target.source_url));
        });
        if (!matches_target && !(matches_abp_links && target.link_url.has_value() && target.link_url->scheme() == "abp"sv))
            return false;
    }
    return any_of(contexts, [&](auto const& context) {
        if (context == "all"sv)
            return true;
        if (context == "frame"sv)
            return frame.parent() != nullptr;
        if (context == "link"sv)
            return target.link_url.has_value();
        if (context == "editable"sv)
            return target.editable;
        if (context == "page"sv)
            return !frame.parent() && !target.link_url.has_value() && !target.media_type.has_value() && !target.editable && !target.has_selection;
        return target.media_type == context;
    });
}

}
