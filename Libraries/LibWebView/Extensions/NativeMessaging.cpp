/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <AK/LexicalPath.h>
#include <LibCore/Environment.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibFileSystem/FileSystem.h>
#include <LibWebView/Extensions/NativeMessaging.h>

namespace WebView::Extensions {

static Optional<Vector<ByteString>>& directories_for_testing()
{
    static auto& directories = *new Optional<Vector<ByteString>>;
    return directories;
}

void NativeMessaging::set_manifest_directories_for_testing(Optional<Vector<ByteString>> directories)
{
    directories_for_testing() = move(directories);
}

bool NativeMessaging::is_valid_host_name(StringView name)
{
    if (name.is_empty() || name.length() > 255)
        return false;
    for (auto part : name.split_view('.', SplitBehavior::KeepEmpty)) {
        if (part.is_empty())
            return false;
        for (auto c : part) {
            if (!is_ascii_alphanumeric(c) && c != '_')
                return false;
        }
    }
    return true;
}

Vector<ByteString> NativeMessaging::manifest_directories()
{
    if (directories_for_testing().has_value())
        return *directories_for_testing();

    Vector<ByteString> directories;
    auto home = Core::StandardPaths::home_directory();
#if defined(AK_OS_MACOS)
    directories.append(LexicalPath::join(home, "Library/Application Support/Mozilla/NativeMessagingHosts"sv).string());
    directories.append("/Library/Application Support/Mozilla/NativeMessagingHosts");
#elif !defined(AK_OS_WINDOWS)
    directories.append(LexicalPath::join(home, ".mozilla/native-messaging-hosts"sv).string());
    directories.append("/usr/lib/mozilla/native-messaging-hosts");
    directories.append("/usr/lib64/mozilla/native-messaging-hosts");
#endif
    // Windows lists native hosts in the registry, which Ladyflock does not read.
    return directories;
}

ErrorOr<Optional<NativeMessagingHostManifest>> NativeMessaging::find_host(StringView host_name, StringView extension_id, Optional<Vector<ByteString>> directories)
{
    if (!is_valid_host_name(host_name))
        return Error::from_string_literal("Invalid native application name");

    for (auto const& directory : directories.has_value() ? *directories : manifest_directories()) {
        auto path = LexicalPath::join(directory, ByteString::formatted("{}.json", host_name)).string();
        if (!FileSystem::is_regular_file(path))
            continue;

        auto file = TRY(Core::File::open(path, Core::File::OpenMode::Read));
        auto contents = TRY(file->read_until_eof());
        if (contents.size() > 64 * KiB)
            return Error::from_string_literal("Native manifest is too large");
        auto json = TRY(JsonValue::from_string(contents));
        if (!json.is_object())
            return Error::from_string_literal("Native manifest is not an object");
        auto const& manifest = json.as_object();

        auto name = manifest.get_string("name"sv);
        auto type = manifest.get_string("type"sv);
        auto executable = manifest.get_string("path"sv);
        auto allowed = manifest.get_array("allowed_extensions"sv);
        if (name != host_name || type != "stdio"sv || !executable.has_value() || executable->is_empty() || !allowed.has_value())
            return Error::from_string_literal("Native manifest is not valid");

        // As in Firefox, a host that does not list the extension is as good as absent to it.
        if (!any_of(allowed->values(), [&](auto const& id) { return id.is_string() && id.as_string() == extension_id; }))
            return Optional<NativeMessagingHostManifest> {};

        // A relative path is relative to the manifest, on Windows only; elsewhere the path must be absolute.
        auto executable_path = executable->to_byte_string();
        if (!LexicalPath { executable_path }.is_absolute())
            return Error::from_string_literal("Native manifest path must be absolute");

        return NativeMessagingHostManifest {
            .name = *name,
            .manifest_path = move(path),
            .executable_path = move(executable_path),
        };
    }
    return Optional<NativeMessagingHostManifest> {};
}

}
