/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <LibWebView/Extensions/ExtensionAlarms.h>
#include <math.h>

namespace WebView::Extensions {

static constexpr i64 milliseconds_per_minute = 60'000;
static constexpr i64 latest_js_time = 8'640'000'000'000'000;
// Keep each individual wait within the range of all event loop backends.
static constexpr i64 maximum_wait_ms = 24 * 60 * milliseconds_per_minute;

ExtensionAlarms::ExtensionAlarms(Function<void(String const& extension_id, JsonObject)> on_alarm)
    : m_on_alarm(move(on_alarm))
{
}

JsonObject ExtensionAlarms::Alarm::to_json(String const& name) const
{
    JsonObject alarm;
    alarm.set("name"sv, name);
    alarm.set("scheduledTime"sv, scheduled_time);
    if (period_in_minutes.has_value())
        alarm.set("periodInMinutes"sv, *period_in_minutes);
    return alarm;
}

void ExtensionAlarms::Alarm::schedule(i64 now) const
{
    timer->restart(AK::Duration::from_milliseconds(clamp<i64>(scheduled_time - now, 0, maximum_wait_ms)));
}

static ErrorOr<Optional<double>> numeric_option(JsonObject const& options, StringView key)
{
    if (!options.has(key))
        return Optional<double> {};
    auto number = options.get_double_with_precision_loss(key);
    if (!number.has_value() || !isfinite(*number))
        return Error::from_string_literal("Alarm times must be finite numbers");
    return number;
}

ErrorOr<void> ExtensionAlarms::create(String const& extension_id, JsonArray const& arguments)
{
    if (arguments.is_empty() || arguments.size() > 2)
        return Error::from_string_literal("alarms.create requires alarm information");

    String name;
    auto const& first = arguments.at(0);
    if (arguments.size() == 2) {
        if (first.is_string())
            name = first.as_string();
        else if (!first.is_null())
            return Error::from_string_literal("Alarm name must be a string");
    }
    auto const& info = arguments.at(arguments.size() - 1);
    if (!info.is_object())
        return Error::from_string_literal("Alarm information must be an object");

    auto when = TRY(numeric_option(info.as_object(), "when"sv));
    auto delay = TRY(numeric_option(info.as_object(), "delayInMinutes"sv));
    auto period = TRY(numeric_option(info.as_object(), "periodInMinutes"sv));
    if (when.has_value() && delay.has_value())
        return Error::from_string_literal("Use either when or delayInMinutes for an alarm");
    if (!when.has_value() && !delay.has_value() && !period.has_value())
        return Error::from_string_literal("An alarm needs a scheduled time, delay, or period");
    if (period.has_value() && (*period <= 0 || *period >= static_cast<double>(latest_js_time / milliseconds_per_minute)))
        return Error::from_string_literal("Alarm period is outside the supported range");

    auto now = AK::UnixDateTime::now().milliseconds_since_epoch();
    auto target = when.value_or(static_cast<double>(now) + delay.value_or(period.value_or(0.0)) * milliseconds_per_minute);
    if (!isfinite(target) || target < -static_cast<double>(latest_js_time) || target > static_cast<double>(latest_js_time))
        return Error::from_string_literal("Alarm time is outside the supported range");

    auto timer = Core::Timer::create_single_shot({}, [this, extension_id, name] { fire(extension_id, name); });
    auto& alarms = m_alarms.ensure(extension_id);
    // Replacing a named alarm puts it at the end of getAll's insertion order.
    alarms.remove(name);
    Alarm alarm { move(timer), static_cast<i64>(ceil(target)), period };
    alarm.schedule(now);
    alarms.set(move(name), move(alarm));
    return {};
}

ErrorOr<Optional<JsonValue>> ExtensionAlarms::invoke(String const& extension_id, StringView method, JsonArray const& arguments)
{
    if (method == "create"sv) {
        TRY(create(extension_id, arguments));
        return Optional<JsonValue> {};
    }
    if (method == "clearAll"sv) {
        clear_extension(extension_id);
        return Optional<JsonValue> {};
    }
    if (method == "getAll"sv) {
        JsonArray result;
        if (auto alarms = m_alarms.get(extension_id); alarms.has_value()) {
            for (auto const& [name, alarm] : *alarms)
                result.must_append(alarm.to_json(name));
        }
        return Optional<JsonValue> { JsonValue(move(result)) };
    }
    if (method != "get"sv && method != "clear"sv)
        return Error::from_string_literal("Unsupported alarms API method");

    String name;
    if (!arguments.is_empty() && !arguments.at(0).is_null()) {
        if (!arguments.at(0).is_string())
            return Error::from_string_literal("Alarm name must be a string");
        name = arguments.at(0).as_string();
    }
    if (method == "clear"sv)
        return Optional<JsonValue> { JsonValue(clear_alarm(extension_id, name)) };
    if (auto alarms = m_alarms.get(extension_id); alarms.has_value()) {
        if (auto alarm = alarms->get(name); alarm.has_value())
            return Optional<JsonValue> { JsonValue(alarm->to_json(name)) };
    }
    return Optional<JsonValue> {};
}

void ExtensionAlarms::clear_extension(StringView extension_id)
{
    m_alarms.remove(extension_id);
}

bool ExtensionAlarms::clear_alarm(StringView extension_id, StringView name)
{
    auto alarms = m_alarms.find(extension_id);
    if (alarms == m_alarms.end() || !alarms->value.remove(name))
        return false;
    if (alarms->value.is_empty())
        m_alarms.remove(alarms);
    return true;
}

void ExtensionAlarms::fire(String const& extension_id, String const& name)
{
    auto alarms = m_alarms.find(extension_id);
    if (alarms == m_alarms.end())
        return;
    auto alarm = alarms->value.find(name);
    if (alarm == alarms->value.end())
        return;

    auto& entry = alarm->value;
    auto now = AK::UnixDateTime::now().milliseconds_since_epoch();
    if (entry.scheduled_time > now) {
        entry.schedule(now);
        return;
    }

    auto details = entry.to_json(name);
    if (entry.period_in_minutes.has_value()) {
        entry.scheduled_time = now + static_cast<i64>(ceil(*entry.period_in_minutes * milliseconds_per_minute));
        entry.schedule(now);
    } else {
        clear_alarm(extension_id, name);
    }
    m_on_alarm(extension_id, move(details));
}

}
