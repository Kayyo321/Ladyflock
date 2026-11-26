/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/QuickSort.h>
#include <AK/StringBuilder.h>
#include <LibURL/Parser.h>
#include <LibWebView/Extensions/DeclarativeRequestRules.h>

namespace WebView::Extensions {

using ResourceType = Web::Extensions::WebRequestResourceType;
using Action = DeclarativeRequestRule::Action;
using DomainType = DeclarativeRequestRule::DomainType;

static constexpr size_t max_rulesets = 100;
static constexpr size_t max_enabled_rulesets = 50;
static constexpr size_t max_enabled_rules = 30'000;
static constexpr size_t max_packaged_rules = 100'000;

static bool matches_domain(StringView host, StringView domain)
{
    if (host == domain)
        return true;
    return host.length() > domain.length() && host.ends_with(domain) && host[host.length() - domain.length() - 1] == '.';
}

static bool domain_list_matches(HashTable<String> const& domains, StringView host)
{
    if (domains.is_empty())
        return false;
    while (!host.is_empty()) {
        if (domains.contains(host))
            return true;
        auto dot = host.find('.');
        if (!dot.has_value())
            break;
        host = host.substring_view(*dot + 1);
    }
    return false;
}

static bool is_filter_separator(char byte)
{
    return !is_ascii_alphanumeric(byte) && byte != '_' && byte != '-' && byte != '.' && byte != '%';
}

// Match the DNR wildcard and separator tokens as a glob. Remembering the last
// wildcard avoids recursive backtracking on filters supplied by an extension.
static bool matches_url_pattern(StringView url, StringView pattern, bool at_start, bool at_end)
{
    size_t position = 0;
    size_t token = 0;
    size_t retry_position = 0;
    Optional<size_t> retry_token;
    if (!at_start)
        retry_token = 0;

    for (;;) {
        if (token == pattern.length()) {
            if (!at_end || position == url.length())
                return true;
        } else if (pattern[token] == '*') {
            retry_token = ++token;
            retry_position = position;
            continue;
        } else if (pattern[token] == '^' && token + 1 == pattern.length() && position == url.length()) {
            ++token;
            continue;
        } else if (position < url.length() && (pattern[token] == '^' ? is_filter_separator(url[position]) : pattern[token] == url[position])) {
            ++token;
            ++position;
            continue;
        }
        if (!retry_token.has_value() || retry_position == url.length())
            return false;
        position = ++retry_position;
        token = *retry_token;
    }
}

bool DeclarativeRequestRule::matches(StringView url, StringView host, size_t host_offset, StringView initiator_host, ResourceType type, StringView method, bool first_party) const
{
    if (!types.is_empty() && !types.contains_slow(type))
        return false;
    if (types.is_empty() && excluded_types.is_empty() && type == ResourceType::MainFrame)
        return false;
    if (excluded_types.contains_slow(type))
        return false;
    if ((!methods.is_empty() && !methods.contains_slow(method)) || excluded_methods.contains_slow(method))
        return false;
    if (domain_type != DomainType::Any && first_party != (domain_type == DomainType::FirstParty))
        return false;

    if ((!request_domains.is_empty() && !domain_list_matches(request_domains, host))
        || domain_list_matches(excluded_request_domains, host)
        || (!initiator_domains.is_empty() && !domain_list_matches(initiator_domains, initiator_host))
        || domain_list_matches(excluded_initiator_domains, initiator_host))
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
    case FilterKind::Pattern:
        if (!anchor_domain)
            return matches_url_pattern(url, filter, anchor_start, anchor_end);
        if (matches_url_pattern(url.substring_view(host_offset), filter, true, anchor_end))
            return true;
        for (size_t i = 0; i + 1 < host.length(); ++i) {
            if (host[i] == '.' && matches_url_pattern(url.substring_view(host_offset + i + 1), filter, true, anchor_end))
                return true;
        }
        return false;
    }
    VERIFY_NOT_REACHED();
}

DeclarativeRequestMatch match_declarative_request(ReadonlySpan<DeclarativeRuleset> rulesets, Web::Extensions::WebRequestBeforeRequestDetails const& details, Optional<URL::URL> const& initiator, bool has_host_access, ReadonlySpan<DeclarativeRequestRule> dynamic_rules, ReadonlySpan<DeclarativeRequestRule> session_rules)
{
    if (!details.url.scheme().is_one_of("http"sv, "https"sv, "ws"sv, "wss"sv)
        || (initiator.has_value() && !initiator->scheme().is_one_of("http"sv, "https"sv)))
        return {};
    auto host = details.url.serialized_host();
    auto initiator_host = initiator.has_value() ? initiator->serialized_host() : StringView {};
    auto url = details.url.serialize(URL::ExcludeFragment::Yes);
    auto folded = url.to_ascii_lowercase();
    auto method = details.method.to_ascii_lowercase();
    auto host_offset = details.url.to_rust().components.host_start;
    auto first_party = [&] {
        if (!initiator.has_value())
            return false;
        if (host == initiator_host)
            return true;
        auto target = details.url.host()->registrable_domain();
        return target.has_value() && target == initiator->host()->registrable_domain();
    }();

    DeclarativeRequestMatch result;
    auto evaluate_rules = [&](ReadonlySpan<DeclarativeRequestRule> rules) {
        for (auto const& rule : rules) {
            if ((rule.action == Action::Redirect || rule.action == Action::ModifyHeaders) && !has_host_access)
                continue;
            if (rule.action != Action::ModifyHeaders && result.winner
                && (rule.priority < result.winner->priority || (rule.priority == result.winner->priority && rule.action <= result.winner->action)))
                continue;
            if (!rule.matches(rule.case_sensitive ? url : folded, host, host_offset, initiator_host, details.type, method, first_party))
                continue;
            if (rule.action == Action::ModifyHeaders)
                result.response_header_rules.append(&rule);
            else
                result.winner = &rule;
        }
    };
    // Higher precedence rulesets are considered first for equal priority and action.
    evaluate_rules(session_rules);
    evaluate_rules(dynamic_rules);
    for (auto const& ruleset : rulesets) {
        if (ruleset.enabled)
            evaluate_rules(ruleset.rules);
    }
    if (result.winner && result.winner->action == Action::Allow) {
        result.response_header_rules.remove_all_matching([&](auto const* rule) {
            return rule->priority <= result.winner->priority;
        });
    }
    quick_sort(result.response_header_rules, [](auto const* a, auto const* b) { return a->priority > b->priority; });
    return result;
}

Optional<URL::URL> declarative_redirect(DeclarativeRequestRule const& rule, URL::URL const& request_url, StringView extension_uuid)
{
    if (rule.extension_path.has_value())
        return URL::Parser::basic_parse(ByteString::formatted("moz-extension://{}/{}", extension_uuid, *rule.extension_path));

    auto rewritten = request_url;
    if (rule.replacement_query.has_value()) {
        auto query = rule.replacement_query->bytes_as_string_view();
        rewritten.set_query(query.is_empty() ? Optional<StringView> {} : query.substring_view(1));
    } else if (auto query = request_url.query(); query.has_value()) {
        StringBuilder remaining;
        bool found = false;
        bool kept = false;
        for (auto field : query->split_view('&', SplitBehavior::KeepEmpty)) {
            auto key = field.substring_view(0, field.find('=').value_or(field.length()));
            auto name = URL::percent_decode(key);
            if (rule.remove_query_parameters.contains_slow(name.view())) {
                found = true;
                continue;
            }
            if (kept)
                remaining.append('&');
            remaining.append(field);
            kept = true;
        }
        if (found)
            rewritten.set_query(kept ? Optional<StringView> { remaining.string_view() } : Optional<StringView> {});
    }
    if (rewritten != request_url)
        return rewritten;
    return {};
}

static ErrorOr<String> parse_domain(StringView input)
{
    if (input.is_empty() || !input.is_ascii() || input.starts_with('.') || input.contains('%') || input.contains('*') || input.contains(".."sv))
        return Error::from_string_literal("Declarative request rule has an invalid domain");
    auto parsed = URL::Parser::parse_host(input);
    if (!parsed.has_value() || parsed->is_empty_host())
        return Error::from_string_literal("Declarative request rule has an invalid domain");
    return parsed->serialize();
}

static ErrorOr<void> parse_url_filter(DeclarativeRequestRule& rule, StringView filter)
{
    using FilterKind = DeclarativeRequestRule::FilterKind;
    if (filter.is_empty() || filter.length() > 512 || !all_of(filter, [](char byte) { return is_ascii_printable(byte); }))
        return Error::from_string_literal("Declarative request filter must be nonempty printable ASCII");
    if (filter == "*"sv)
        return {};
    if (filter.starts_with("||"sv)) {
        rule.anchor_domain = true;
        filter = filter.substring_view(2);
        if (filter.starts_with('*'))
            return Error::from_string_literal("Wildcard is not allowed after a declarative domain anchor");
    } else if (filter.starts_with('|')) {
        rule.anchor_start = true;
        filter = filter.substring_view(1);
    }
    if (filter.ends_with('|')) {
        rule.anchor_end = true;
        filter = filter.substring_view(0, filter.length() - 1);
    }
    if (filter.is_empty())
        return Error::from_string_literal("Empty declarative request URL filter");

    if (rule.anchor_domain && !rule.anchor_end && filter.ends_with('^') && !rule.case_sensitive) {
        auto host = filter.substring_view(0, filter.length() - 1);
        auto parsed_host = parse_domain(host);
        if (!parsed_host.is_error() && parsed_host.value() == host) {
            rule.filter = parsed_host.release_value();
            rule.filter_kind = FilterKind::Host;
            return {};
        }
    }

    if (rule.anchor_domain || filter.contains('*') || filter.contains('^'))
        rule.filter_kind = FilterKind::Pattern;
    else if (rule.anchor_start && rule.anchor_end)
        rule.filter_kind = FilterKind::Exact;
    else if (rule.anchor_start)
        rule.filter_kind = FilterKind::StartsWith;
    else if (rule.anchor_end)
        rule.filter_kind = FilterKind::EndsWith;
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

static ErrorOr<void> read_domains(JsonObject const& condition, StringView name, HashTable<String>& out)
{
    if (!condition.has(name))
        return {};
    auto array = condition.get_array(name);
    if (!array.has_value() || (array->is_empty() && !name.starts_with("excluded"sv)) || array->size() > 50'000)
        return Error::from_string_literal("Invalid declarative request domain list");
    out.ensure_capacity(array->size());
    for (auto const& value : array->values()) {
        if (!value.is_string())
            return Error::from_string_literal("Invalid declarative request domain");
        out.set(TRY(parse_domain(value.as_string())));
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

static ErrorOr<void> read_methods(JsonObject const& condition, StringView name, Vector<String>& out)
{
    if (!condition.has(name))
        return {};
    auto array = condition.get_array(name);
    if (!array.has_value() || array->is_empty() || array->size() > 16)
        return Error::from_string_literal("Invalid declarative request methods");
    for (auto const& value : array->values()) {
        if (!value.is_string() || !value.as_string().is_one_of("connect"sv, "delete"sv, "get"sv, "head"sv, "options"sv, "patch"sv, "post"sv, "put"sv, "trace"sv))
            return Error::from_string_literal("Unsupported declarative request method");
        out.append(value.as_string());
    }
    return {};
}

static ErrorOr<void> parse_redirect_action(DeclarativeRequestRule& rule, JsonObject const& action)
{
    auto redirect = action.get_object("redirect"sv);
    if (action.size() != 2 || !redirect.has_value() || redirect->size() != 1)
        return Error::from_string_literal("Invalid declarative request redirect action");
    if (auto path = redirect->get_string("extensionPath"sv); path.has_value()) {
        auto path_view = path->bytes_as_string_view();
        if (!path_view.starts_with('/') || path_view.contains('%') || path_view.contains('?') || path_view.contains('#'))
            return Error::from_string_literal("Declarative redirect requires a package-relative extension path");
        rule.extension_path = TRY(Web::Extensions::ExtensionManifest::normalize_package_relative_path(*path));
        return {};
    }
    auto transform = redirect->get_object("transform"sv);
    if (!transform.has_value() || transform->size() != 1)
        return Error::from_string_literal("Unsupported declarative request redirect destination");
    if (auto query = transform->get_string("query"sv); query.has_value()) {
        if (!query->is_empty() && !query->starts_with('?'))
            return Error::from_string_literal("Declarative redirect query must start with a question mark");
        rule.replacement_query = query.release_value();
        return {};
    }
    auto query_transform = transform->get_object("queryTransform"sv);
    auto remove = query_transform.has_value() ? query_transform->get_array("removeParams"sv) : Optional<JsonArray> {};
    if (!remove.has_value() || query_transform->size() != 1 || remove->is_empty() || remove->size() > 1024)
        return Error::from_string_literal("Unsupported declarative request query transform");
    for (auto const& name : remove->values()) {
        if (!name.is_string())
            return Error::from_string_literal("Invalid declarative redirect query parameter");
        rule.remove_query_parameters.append(name.as_string());
    }
    return {};
}

static ErrorOr<void> parse_response_header_action(DeclarativeRequestRule& rule, JsonObject const& action)
{
    auto modifications = action.get_array("responseHeaders"sv);
    if (action.size() != 2 || !modifications.has_value() || modifications->is_empty() || modifications->size() > 16)
        return Error::from_string_literal("Unsupported declarative request header action");
    for (auto const& item : modifications->values()) {
        if (!item.is_object())
            return Error::from_string_literal("Invalid declarative response header modification");
        auto const& object = item.as_object();
        auto name = object.get_string("header"sv).value_or(String {}).to_ascii_lowercase();
        auto operation = object.get_string("operation"sv).value_or(String {});
        if (operation == "append"sv && object.size() == 3 && name == "content-security-policy"sv) {
            auto value = object.get_string("value"sv);
            if (!value.has_value() || value->is_empty() || value->bytes().size() > 4096
                || !all_of(value->bytes_as_string_view(), [](char byte) { return is_ascii_printable(byte); }))
                return Error::from_string_literal("Invalid declarative Content-Security-Policy header value");
            rule.response_headers.append({ move(name), *value });
            continue;
        }
        if (operation == "remove"sv && object.size() == 2 && name.is_one_of("location"sv, "refresh"sv, "set-cookie"sv)) {
            rule.response_headers.append({ move(name), {} });
            continue;
        }
        return Error::from_string_literal("Unsupported declarative response header modification");
    }
    return {};
}

static ErrorOr<DeclarativeRequestRule> parse_rule(JsonObject const& input)
{
    static constexpr Array rule_keys { "id"sv, "priority"sv, "action"sv, "condition"sv };
    static constexpr Array condition_keys { "urlFilter"sv, "isUrlFilterCaseSensitive"sv, "requestDomains"sv,
        "excludedRequestDomains"sv, "initiatorDomains"sv, "excludedInitiatorDomains"sv, "resourceTypes"sv, "excludedResourceTypes"sv,
        "domainType"sv, "requestMethods"sv, "excludedRequestMethods"sv };
    auto action = input.get_object("action"sv);
    auto kind = action.has_value() ? action->get_string("type"sv).value_or({}) : ""_string;
    auto condition = input.get_object("condition"sv);
    if (!keys_are_supported(input, rule_keys) || !condition.has_value() || !keys_are_supported(*condition, condition_keys))
        return Error::from_string_literal("Unsupported declarative request rule condition");
    if (!action.has_value())
        return Error::from_string_literal("Declarative request rule requires an action");

    auto priority = input.get_u32("priority"sv).value_or(input.has("priority"sv) ? 0 : 1);
    if (!priority)
        return Error::from_string_literal("Invalid declarative request priority");
    DeclarativeRequestRule rule;
    rule.priority = priority;
    if (kind == "redirect"sv) {
        rule.action = Action::Redirect;
        TRY(parse_redirect_action(rule, *action));
    } else if (kind == "modifyHeaders"sv) {
        rule.action = Action::ModifyHeaders;
        TRY(parse_response_header_action(rule, *action));
    } else if (action->size() == 1 && kind.is_one_of("allow"sv, "block"sv)) {
        rule.action = kind == "allow"sv ? Action::Allow : Action::Block;
    } else {
        return Error::from_string_literal("Unsupported declarative request action");
    }
    if (condition->has("domainType"sv)) {
        auto domain_type = condition->get_string("domainType"sv).value_or(String {});
        if (!domain_type.is_one_of("firstParty"sv, "thirdParty"sv))
            return Error::from_string_literal("Invalid declarative request domain type");
        rule.domain_type = domain_type == "firstParty"sv ? DomainType::FirstParty : DomainType::ThirdParty;
    }
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
    TRY(read_methods(*condition, "requestMethods"sv, rule.methods));
    TRY(read_methods(*condition, "excludedRequestMethods"sv, rule.excluded_methods));
    if (!rule.methods.is_empty() && !rule.excluded_methods.is_empty())
        return Error::from_string_literal("Declarative rule cannot include and exclude request methods together");
    if (!rule.types.is_empty() && !rule.excluded_types.is_empty())
        return Error::from_string_literal("Declarative rule cannot include and exclude resource types together");
    return rule;
}

static ErrorOr<Vector<DeclarativeRequestRule>> parse_rules(JsonArray const& array)
{
    Vector<DeclarativeRequestRule> rules;
    rules.ensure_capacity(array.size());
    HashTable<u32> rule_ids;
    for (auto const& item : array.values()) {
        auto rule_id = item.is_object() ? item.as_object().get_u32("id"sv).value_or(0) : 0;
        if (!rule_id || rule_ids.set(rule_id) != AK::HashSetResult::InsertedNewEntry)
            return Error::from_string_literal("Invalid or duplicate declarative rule ID");
        rules.append(TRY(parse_rule(item.as_object())));
    }
    return rules;
}

ErrorOr<MutableDeclarativeRuleset> parse_mutable_ruleset(JsonArray array)
{
    if (array.size() > 5000 || array.serialized().byte_count() > 4 * MiB)
        return Error::from_string_literal("Dynamic or session declarative rule limit exceeded");
    auto rules = TRY(parse_rules(array));
    return MutableDeclarativeRuleset { move(array), move(rules) };
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
    if (!descriptors.has_value() || declaration->size() != 1 || descriptors->size() > max_rulesets)
        return Error::from_string_literal("Unsupported declarative rule resources");

    size_t rule_count = 0;
    HashTable<String> ruleset_ids;
    Vector<bool> enabled_rulesets;
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
        if (!rules_json.is_array() || rules_json.as_array().size() > max_packaged_rules - rule_count)
            return Error::from_string_literal("Too many declarative request rules or invalid ruleset format");
        rule_count += rules_json.as_array().size();
        enabled_rulesets.append(*enabled);
        rulesets.append({ *id, *enabled, TRY(parse_rules(rules_json.as_array())) });
    }
    if (!declarative_ruleset_limits_ok(rulesets, enabled_rulesets))
        return Error::from_string_literal("Enabled declarative rulesets exceed the static rule limits");
    return rulesets;
}

bool declarative_ruleset_limits_ok(ReadonlySpan<DeclarativeRuleset> rulesets, ReadonlySpan<bool> enabled)
{
    VERIFY(rulesets.size() == enabled.size());
    size_t enabled_count = 0;
    size_t enabled_rules = 0;
    for (size_t i = 0; i < enabled.size(); ++i) {
        if (enabled[i]) {
            ++enabled_count;
            enabled_rules += rulesets[i].rules.size();
        }
    }
    return enabled_count <= max_enabled_rulesets && enabled_rules <= max_enabled_rules;
}

size_t available_static_declarative_rules(ReadonlySpan<DeclarativeRuleset> rulesets)
{
    size_t enabled_rules = 0;
    for (auto const& ruleset : rulesets) {
        if (ruleset.enabled)
            enabled_rules += ruleset.rules.size();
    }
    return max_enabled_rules - enabled_rules;
}

}
