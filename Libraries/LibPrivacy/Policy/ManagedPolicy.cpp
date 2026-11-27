/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/LexicalPath.h>
#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibPrivacy/Policy/ManagedPolicy.h>
#include <LibPrivacy/Policy/Route.h>

#include <fcntl.h>
#include <sys/stat.h>

namespace Privacy {

static constexpr i64 MANAGED_POLICY_VERSION = 1;
#if !defined(AK_OS_WINDOWS)
static constexpr size_t MAX_MANAGED_POLICY_SIZE = 64 * KiB;
#endif

StringView managed_policy_status_to_string(ManagedPolicyStatus status)
{
    switch (status) {
    case ManagedPolicyStatus::Absent:
        return "absent"sv;
    case ManagedPolicyStatus::Active:
        return "active"sv;
    case ManagedPolicyStatus::Rejected:
        return "rejected"sv;
    }
    VERIFY_NOT_REACHED();
}

ManagedPolicy ManagedPolicyLoadResult::effective_policy() const
{
    switch (status) {
    case ManagedPolicyStatus::Absent:
        return {};
    case ManagedPolicyStatus::Active:
        return policy;
    case ManagedPolicyStatus::Rejected:
        return { .network_ceiling = NetworkPolicy::deny_all(), .strip_tracking_parameters = false };
    }
    VERIFY_NOT_REACHED();
}

ErrorOr<void> validate_managed_policy_metadata(ManagedPolicyFileMetadata const& file, ManagedPolicyFileMetadata const& directory, uid_t required_owner)
{
    if (!file.is_regular_file)
        return Error::from_string_literal("The policy is not a regular file");
    if (!directory.is_directory)
        return Error::from_string_literal("The policy's directory is not a directory");
    if (file.owner != required_owner)
        return Error::from_string_literal("The policy file is not owned by the administrator");
    if (directory.owner != required_owner)
        return Error::from_string_literal("The policy's directory is not owned by the administrator");
    if ((file.mode & (S_IWGRP | S_IWOTH)) != 0)
        return Error::from_string_literal("The policy file is writable by other accounts");
    if ((directory.mode & (S_IWGRP | S_IWOTH)) != 0)
        return Error::from_string_literal("The policy's directory is writable by other accounts");
    return {};
}

static ErrorOr<void> reject_unknown_keys(JsonObject const& object, ReadonlySpan<StringView> known_keys)
{
    ErrorOr<void> result;
    object.for_each_member([&](auto const& key, auto const&) {
        if (!result.is_error() && !known_keys.contains_slow(key.bytes_as_string_view()))
            result = Error::from_string_literal("The policy has a field this version does not support");
    });
    return result;
}

ErrorOr<ManagedPolicy> parse_managed_policy(StringView json)
{
    auto value = TRY(JsonValue::from_string(json));
    if (!value.is_object())
        return Error::from_string_literal("The policy is not a JSON object");
    auto const& object = value.as_object();

    static constexpr Array known_keys { "version"sv, "network"sv, "stripTrackingParameters"sv };
    TRY(reject_unknown_keys(object, known_keys));

    auto version = object.get_integer<i64>("version"sv);
    if (version != MANAGED_POLICY_VERSION)
        return Error::from_string_literal("The policy has an unsupported version");

    ManagedPolicy policy;

    if (auto strip = object.get("stripTrackingParameters"sv); strip.has_value()) {
        if (!strip->is_bool())
            return Error::from_string_literal("stripTrackingParameters is not a boolean");
        policy.strip_tracking_parameters = strip->as_bool();
    }

    if (auto network = object.get("network"sv); network.has_value()) {
        if (!network->is_object())
            return Error::from_string_literal("network is not an object");
        auto const& network_object = network->as_object();

        static constexpr Array known_network_keys { "mode"sv, "proxy"sv };
        TRY(reject_unknown_keys(network_object, known_network_keys));

        auto mode_name = network_object.get_string("mode"sv);
        if (!mode_name.has_value())
            return Error::from_string_literal("network.mode is missing");
        auto mode = network_mode_from_string(*mode_name);
        if (!mode.has_value())
            return Error::from_string_literal("network.mode is not a known mode");

        auto proxy = network_object.get_string("proxy"sv);
        if (proxy.has_value() != (*mode == NetworkMode::ProxyOnly))
            return Error::from_string_literal("network.proxy must be given exactly when network.mode is proxy-only");

        switch (*mode) {
        case NetworkMode::Allow:
            break;
        case NetworkMode::Offline:
            policy.network_ceiling = NetworkPolicy::offline();
            break;
        case NetworkMode::ProxyOnly: {
            auto route = ProxyRoute::parse(*proxy);
            if (!route.has_value())
                return Error::from_string_literal("network.proxy is not a usable route");
            policy.network_ceiling = NetworkPolicy::proxy_only(route.release_value());
            break;
        }
        }
    }

    return policy;
}

#if !defined(AK_OS_WINDOWS)
static ManagedPolicyFileMetadata metadata_from_stat(struct stat const& st)
{
    return {
        .is_regular_file = S_ISREG(st.st_mode),
        .is_directory = S_ISDIR(st.st_mode),
        .owner = st.st_uid,
        .mode = st.st_mode,
    };
}
#endif

ManagedPolicyLoadResult load_managed_policy(ByteString const& path, uid_t required_owner)
{
    auto rejected = [](ByteString reason) {
        warnln("Managed privacy policy rejected: {}. No identity has network access until it is fixed.", reason);
        return ManagedPolicyLoadResult { .status = ManagedPolicyStatus::Rejected, .policy = {}, .reason = move(reason) };
    };

#if defined(AK_OS_WINDOWS)
    // There is no protected location or ownership check on Windows yet, so a policy found there can't be trusted.
    (void)required_owner;
    if (Core::System::stat(path).is_error())
        return {};
    return rejected("Managed policies are not supported on this platform");
#else

    // A symbolic link is never followed: it is examined as itself, and is not a regular file.
    auto file_stat = Core::System::lstat(path);
    if (file_stat.is_error()) {
        if (file_stat.error().is_errno() && file_stat.error().code() == ENOENT)
            return {};
        return rejected(ByteString::formatted("{}", file_stat.error()));
    }

    auto directory_stat = Core::System::stat(LexicalPath { path }.dirname());
    if (directory_stat.is_error())
        return rejected(ByteString::formatted("{}", directory_stat.error()));

    if (auto valid = validate_managed_policy_metadata(metadata_from_stat(file_stat.value()), metadata_from_stat(directory_stat.value()), required_owner); valid.is_error())
        return rejected(ByteString::formatted("{}", valid.error()));

    auto contents = [&]() -> ErrorOr<ByteBuffer> {
        auto fd = TRY(Core::System::open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
        auto file = TRY(Core::File::adopt_fd(fd, Core::File::OpenMode::Read));
        // The file read is the one that was checked, not one swapped in after the check.
        auto opened = TRY(Core::System::fstat(fd));
        if (opened.st_ino != file_stat.value().st_ino || opened.st_dev != file_stat.value().st_dev)
            return Error::from_string_literal("The policy file changed while it was read");
        if (static_cast<size_t>(opened.st_size) > MAX_MANAGED_POLICY_SIZE)
            return Error::from_string_literal("The policy file is too large");
        return file->read_until_eof();
    }();
    if (contents.is_error())
        return rejected(ByteString::formatted("{}", contents.error()));

    auto policy = parse_managed_policy(StringView { contents.value() });
    if (policy.is_error())
        return rejected(ByteString::formatted("{}", policy.error()));

    return { .status = ManagedPolicyStatus::Active, .policy = policy.release_value(), .reason = {} };
#endif
}

Optional<ByteString> default_managed_policy_path()
{
#if defined(AK_OS_MACOS)
    return "/Library/Managed Preferences/org.ladyflock.browser/policy.json"sv;
#elif defined(AK_OS_LINUX) || defined(AK_OS_BSD_GENERIC)
    return "/etc/ladyflock/policies/policy.json"sv;
#else
    return {};
#endif
}

}
