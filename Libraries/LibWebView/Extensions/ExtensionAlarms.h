/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/Optional.h>
#include <LibCore/Timer.h>
#include <LibWebView/Export.h>

namespace WebView::Extensions {

class WEBVIEW_API ExtensionAlarms {
    AK_MAKE_NONCOPYABLE(ExtensionAlarms);
    AK_MAKE_NONMOVABLE(ExtensionAlarms);

public:
    explicit ExtensionAlarms(Function<void(String const& extension_id, JsonObject)> on_alarm);

    ErrorOr<Optional<JsonValue>> invoke(String const& extension_id, StringView method, JsonArray const& arguments);
    void clear_extension(StringView extension_id);
    void clear() { m_alarms.clear(); }

private:
    struct Alarm {
        NonnullRefPtr<Core::Timer> timer;
        i64 scheduled_time;
        Optional<double> period_in_minutes;

        JsonObject to_json(String const& name) const;
        void schedule(i64 now) const;
    };

    ErrorOr<void> create(String const& extension_id, JsonArray const& arguments);
    bool clear_alarm(StringView extension_id, StringView name);
    void fire(String const& extension_id, String const& name);

    HashMap<String, OrderedHashMap<String, Alarm>> m_alarms;
    Function<void(String const& extension_id, JsonObject)> m_on_alarm;
};

}
