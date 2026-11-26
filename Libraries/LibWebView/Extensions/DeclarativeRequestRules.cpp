/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/JsonObject.h>
#include <LibURL/Parser.h>
#include <LibWebView/Extensions/DeclarativeRequestRules.h>

namespace WebView::Extensions {

using ResourceType = Web::Extensions::WebRequestResourceType;

static bool matches_domain(StringView host, StringView domain)
{
    if (host == domain)
        return true;
    return host.length() > domain.length() && host.ends_with(domain) && host[host.length() - domain.length() - 1] == '.';
}

bool DeclarativeRequestRule::matches(StringView url, StringView host, StringView initiator_host, ResourceType type) const
{
    if (!types.is_empty() && !types.contains_slow(type))
        return false;
    if (types.is_empty() && excluded_types.is_empty() && type == ResourceType::MainFrame)
        return false;
    if (excluded_types.contains_slow(type))
        return false;

    auto match_domains = [](Vector<String> const& domains, StringView host) {
        return any_of(domains, [&](String const& domain) { return matches_domain(host, domain); });
    };
    if ((!request_domains.is_empty() && !match_domains(request_domains, host))
        || match_domains(excluded_request_domains, host)
        || (!initiator_domains.is_empty() && !match_domains(initiator_domains, initiator_host))
        || match_domains(excluded_initiator_domains, initiator_host))
        return false;

    if (filter.is_empty())
        return true;
    switch (filter_kind) {
    case FilterKind::Contains:
        return url.contains(filter);
    case FilterKind::StartsWith:
        return url.starts_with(filter);
    case FilterKind::EndsWith:
        return url.ends_with(filter);
    case FilterKind::Exact:
        return url == filter;
    case FilterKind::Host:
        return matches_domain(host, filter);
    }
    VERIFY_NOT_REACHED();
}

bool should_block_declarative_request(ReadonlySpan<DeclarativeRuleset> rulesets, Web::Extensions::WebRequestBeforeRequestDetails const& details, Optional<URL::URL> const& initiator)
{
    if (!details.url.scheme().is_one_of("http"sv, "https"sv)
        || (initiator.has_value() && !initiator->scheme().is_one_of("http"sv, "https"sv)))
        return false;
    auto host = details.url.serialized_host();
    auto initiator_host = initiator.has_value() ? initiator->serialized_host() : StringView {};
    auto url = details.url.serialize(URL::ExcludeFragment::Yes);
    auto folded = url.to_ascii_lowercase();
    DeclarativeRequestRule const* winner = nullptr;
    for (auto const& ruleset : rulesets) {
        if (!ruleset.enabled)
            continue;
        for (auto const& rule : ruleset.rules) {
            if (winner && (rule.priority < winner->priority || (rule.priority == winner->priority && (!rule.allow || winner->allow))))
                continue;
            if (rule.matches(rule.case_sensitive ? url : folded, host, initiator_host, details.type))
                winner = &rule;
        }
    }
    return winner && !winner->allow;
}

static ErrorOr<String> parse_domain(StringView input)
{
    if (input.is_empty() || !input.is_ascii() || input.starts_with('.') || input.ends_with('.') || input.contains('%') || input.contains('*') || input.contains(".."sv))
        return Error::from_string_literal("Declarative request rule has an invalid domain");
    auto parsed = URL::Parser::parse_host(input);
    if (!parsed.has_value() || parsed->is_empty_host())
        return Error::from_string_literal("Declarative request rule has an invalid domain");
    return parsed->serialize();
}

static ErrorOr<void> parse_url_filter(DeclarativeRequestRule& rule, StringView filter)
{
    if (filter.is_empty() || filter.length() > 512 || !filter.is_ascii())
        return Error::from_string_literal("Declarative request filter must be nonempty ASCII");
    if (filter == "*"sv)
        return {};
    if (filter.starts_with("||"sv) && filter.ends_with('^')) {
        rule.filter_kind = DeclarativeRequestRule::FilterKind::Host;
        rule.filter = TRY(parse_domain(filter.substring_view(2, filter.length() - 3)));
        return {};
    }
    bool starts_with = filter.starts_with('|');
    if (starts_with)
        filter = filter.substring_view(1);
    bool ends_with = filter.ends_with('|');
    if (ends_with)
        filter = filter.substring_view(0, filter.length() - 1);
    if (filter.is_empty() || filter.contains('*') || filter.contains('^') || filter.contains('|'))
        return Error::from_string_literal("Unsupported declarative request URL filter");
    if (starts_with && ends_with)
        rule.filter_kind = DeclarativeRequestRule::FilterKind::Exact;
    else if (starts_with)
        rule.filter_kind = DeclarativeRequestRule::FilterKind::StartsWith;
    else if (ends_with)
        rule.filter_kind = DeclarativeRequestRule::FilterKind::EndsWith;
    rule.filter = TRY(String::from_utf8(filter));
    if (!rule.case_sensitive)
        rule.filter = rule.filter.to_ascii_lowercase();
    return {};
}

static bool keys_are_supported(JsonObject const& object, ReadonlySpan<StringView> supported)
{
    bool valid = true;
    object.for_each_member([&](String const& key, auto const&) { valid &= supported.contains_slow(key.bytes_as_string_view()); });
    return valid;
}

static ErrorOr<void> read_domains(JsonObject const& condition, StringView name, Vector<String>& out)
{
    if (!condition.has(name))
        return {};
    auto array = condition.get_array(name);
    if (!array.has_value() || (array->is_empty() && !name.starts_with("excluded"sv)) || array->size() > 512)
        return Error::from_string_literal("Invalid declarative request domain list");
    for (auto const& value : array->values()) {
        if (!value.is_string())
            return Error::from_string_literal("Invalid declarative request domain");
        out.append(TRY(parse_domain(value.as_string())));
    }
    return {};
}

static ErrorOr<void> read_types(JsonObject const& condition, StringView name, Vector<ResourceType>& out)
{
    if (!condition.has(name))
        return {};
    auto array = condition.get_array(name);
    if (!array.has_value() || array->is_empty() || array->size() > 16)
        return Error::from_string_literal("Invalid declarative request resource types");
    for (auto const& value : array->values()) {
        auto type = value.is_string() ? Web::Extensions::web_request_resource_type_from_string(value.as_string()) : Optional<ResourceType> {};
        if (!type.has_value())
            return Error::from_string_literal("Unsupported declarative request resource type");
        out.append(*type);
    }
    return {};
}

static ErrorOr<DeclarativeRequestRule> parse_rule(JsonObject const& input)
{
    static constexpr Array rule_keys { "id"sv, "priority"sv, "action"sv, "condition"sv };
    static constexpr Array condition_keys { "urlFilter"sv, "isUrlFilterCaseSensitive"sv, "requestDomains"sv,
        "excludedRequestDomains"sv, "initiatorDomains"sv, "excludedInitiatorDomains"sv, "resourceTypes"sv, "excludedResourceTypes"sv };
    auto action = input.get_object("action"sv);
    auto kind = action.has_value() ? action->get_string("type"sv).value_or({}) : ""_string;
    auto condition = input.get_object("condition"sv);
    if (!keys_are_supported(input, rule_keys) || !condition.has_value() || !keys_are_supported(*condition, condition_keys))
        return Error::from_string_literal("Unsupported declarative request rule condition");
    if (!action.has_value() || action->size() != 1 || (kind != "block"sv && kind != "allow"sv))
        return Error::from_string_literal("Only block and allow declarative actions are supported");

    auto priority = input.get_u32("priority"sv).value_or(input.has("priority"sv) ? 0 : 1);
    if (!priority)
        return Error::from_string_literal("Invalid declarative request priority");
    DeclarativeRequestRule rule;
    rule.priority = priority;
    rule.allow = kind == "allow"sv;
    if (condition->has("isUrlFilterCaseSensitive"sv) && !condition->get_bool("isUrlFilterCaseSensitive"sv).has_value())
        return Error::from_string_literal("Invalid declarative request URL case flag");
    rule.case_sensitive = condition->get_bool("isUrlFilterCaseSensitive"sv).value_or(false);
    if (condition->has("urlFilter"sv))
        TRY(parse_url_filter(rule, condition->get_string("urlFilter"sv).value_or({})));

    TRY(read_domains(*condition, "requestDomains"sv, rule.request_domains));
    TRY(read_domains(*condition, "excludedRequestDomains"sv, rule.excluded_request_domains));
    TRY(read_domains(*condition, "initiatorDomains"sv, rule.initiator_domains));
    TRY(read_domains(*condition, "excludedInitiatorDomains"sv, rule.excluded_initiator_domains));
    TRY(read_types(*condition, "resourceTypes"sv, rule.types));
    TRY(read_types(*condition, "excludedResourceTypes"sv, rule.excluded_types));
    if (!rule.types.is_empty() && !rule.excluded_types.is_empty())
        return Error::from_string_literal("Declarative rule cannot include and exclude resource types together");
    return rule;
}

ErrorOr<Vector<DeclarativeRuleset>> load_declarative_rulesets(Web::Extensions::ExtensionManifest const& manifest, Function<ErrorOr<String>(StringView)> read_ruleset)
{
    Vector<DeclarativeRuleset> rulesets;
    auto json = TRY(JsonValue::from_string(manifest.raw_json()));
    auto const& object = json.as_object();
    if (!object.has("declarative_net_request"sv))
        return rulesets;
    auto declaration = object.get_object("declarative_net_request"sv);
    if (!declaration.has_value() || !manifest.has_permission("declarativeNetRequest"sv))
        return Error::from_string_literal("Declarative rules require the declarativeNetRequest permission");
    auto descriptors = declaration->get_array("rule_resources"sv);
    if (!descriptors.has_value() || declaration->size() != 1 || descriptors->size() > 20)
        return Error::from_string_literal("Unsupported declarative rule resources");

    static constexpr size_t max_static_rules = 10'000;
    size_t rule_count = 0;
    HashTable<String> ruleset_ids;
    for (auto const& descriptor : descriptors->values()) {
        if (!descriptor.is_object())
            return Error::from_string_literal("Invalid declarative rule resource");
        auto const& resource = descriptor.as_object();
        auto id = resource.get_string("id"sv);
        auto path = resource.get_string("path"sv);
        auto enabled = resource.get_bool("enabled"sv);
        if (resource.size() != 3 || !id.has_value() || id->is_empty() || id->bytes().size() > 128 || !path.has_value() || !enabled.has_value()
            || ruleset_ids.set(*id) != AK::HashSetResult::InsertedNewEntry)
            return Error::from_string_literal("Invalid or duplicate declarative rule resource");

        auto normalized = TRY(Web::Extensions::ExtensionManifest::normalize_package_relative_path(*path));
        auto text = TRY(read_ruleset(normalized));
        auto rules_json = TRY(JsonValue::from_string(text));
        if (!rules_json.is_array() || rules_json.as_array().size() > max_static_rules - rule_count)
            return Error::from_string_literal("Too many declarative request rules or invalid ruleset format");
        rule_count += rules_json.as_array().size();
        DeclarativeRuleset ruleset { *id, *enabled, {} };
        HashTable<u32> rule_ids;
        for (auto const& item : rules_json.as_array().values()) {
            auto rule_id = item.is_object() ? item.as_object().get_u32("id"sv).value_or(0) : 0;
            if (!rule_id || rule_ids.set(rule_id) != AK::HashSetResult::InsertedNewEntry)
                return Error::from_string_literal("Invalid or duplicate declarative rule ID");
            ruleset.rules.append(TRY(parse_rule(item.as_object())));
        }
        rulesets.append(move(ruleset));
    }
    return rulesets;
}

}
