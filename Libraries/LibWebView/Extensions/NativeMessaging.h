/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWebView/Export.h>

namespace WebView::Extensions {

// A native messaging host as a native manifest installs it for Firefox.
// https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/Native_manifests
struct NativeMessagingHostManifest {
    String name;
    ByteString manifest_path;
    ByteString executable_path;
};

class WEBVIEW_API NativeMessaging {
public:
    // A host name is dot-separated words of ASCII letters, digits and underscores, as Firefox requires.
    static bool is_valid_host_name(StringView);

    // The folders Firefox searches for native manifests, in its order, for this user and then for the system.
    static Vector<ByteString> manifest_directories();

    // Finds the host's native manifest, and checks that it allows this extension to start the host. Empty if no
    // manifest names the host or none allows the extension; an error if a manifest exists but is not valid.
    static ErrorOr<Optional<NativeMessagingHostManifest>> find_host(StringView host_name, StringView extension_id, Optional<Vector<ByteString>> directories = {});

    // Tests point the search at their own folder instead of the user's and the system's.
    static void set_manifest_directories_for_testing(Optional<Vector<ByteString>>);
};

}
