/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/JsonObject.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibWebCommon/Export.h>
#include <LibWebCommon/Extensions/ExtensionManifest.h>
#include <LibWebCommon/Extensions/ExtensionTypes.h>

namespace Web::Extensions {

struct WEBCOMMON_API CompatibilityEvaluation {
    CompatibilityStatus status { CompatibilityStatus::Incompatible };
    Vector<String> unsupported_reasons;
    // What Firefox would also ignore, such as a key or permission only Chrome reads.
    Vector<String> warnings;
};

struct WEBCOMMON_API TestedExtensionVersion {
    StringView addon_id;
    StringView version;
    StringView sha256;
};

class WEBCOMMON_API SupportMatrix {
public:
    // What runtime.getBrowserInfo() reports. The version is the Firefox release whose WebExtensions behavior
    // Ladyflock follows, the one its Firefox user agent names, so that an extension's check of the browser's
    // version compares against the API it is calling. Gecko's build ID has been frozen at this date since 2010.
    static constexpr StringView browser_name = "Ladyflock"sv;
    static constexpr StringView browser_vendor = "Ladyflock"sv;
    static constexpr StringView firefox_api_version = "156.0"sv;
    static constexpr StringView browser_build_id = "20100101"sv;

    static ReadonlySpan<StringView> supported_manifest_keys();
    static ReadonlySpan<StringView> supported_permissions();
    static ReadonlySpan<StringView> supported_api_methods();
    static ReadonlySpan<TestedExtensionVersion> tested_extension_versions();

    static bool is_manifest_key_supported(StringView key);
    static bool is_permission_supported(StringView permission);
    static bool is_permission_known_to_firefox(StringView permission);
    static bool is_manifest_key_known_unsupported(StringView key);

    static CompatibilityEvaluation evaluate_manifest(ExtensionManifest const&, Optional<StringView> sha256 = {}, ReadonlySpan<TestedExtensionVersion> extra_tested_versions = {});
    static CompatibilityEvaluation evaluate_catalog_metadata(
        Optional<StringView> addon_id,
        Optional<StringView> version,
        Optional<StringView> sha256,
        ReadonlySpan<String> permissions,
        bool has_service_worker_only = false);

    static JsonObject to_json();
};

}
