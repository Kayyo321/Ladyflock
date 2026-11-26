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
};

struct WEBCOMMON_API TestedExtensionVersion {
    StringView addon_id;
    StringView version;
    StringView sha256;
};

class WEBCOMMON_API SupportMatrix {
public:
    static ReadonlySpan<StringView> supported_manifest_keys();
    static ReadonlySpan<StringView> supported_permissions();
    static ReadonlySpan<StringView> supported_api_methods();
    static ReadonlySpan<TestedExtensionVersion> tested_extension_versions();

    static bool is_manifest_key_supported(StringView key);
    static bool is_permission_supported(StringView permission);

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
