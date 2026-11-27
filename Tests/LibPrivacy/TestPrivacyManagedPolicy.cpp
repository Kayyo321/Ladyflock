/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibPrivacy/Policy/Compiler.h>
#include <LibPrivacy/Policy/ManagedPolicy.h>
#include <LibPrivacy/Policy/Route.h>
#include <LibTest/TestCase.h>

#include <sys/stat.h>
#include <unistd.h>

using Privacy::ManagedPolicyFileMetadata;

static ManagedPolicyFileMetadata file(uid_t owner, mode_t mode) { return { .is_regular_file = true, .is_directory = false, .owner = owner, .mode = mode }; }
static ManagedPolicyFileMetadata directory(uid_t owner, mode_t mode) { return { .is_regular_file = false, .is_directory = true, .owner = owner, .mode = mode }; }

TEST_CASE(only_a_file_the_administrator_alone_can_write_is_trusted)
{
    EXPECT(!Privacy::validate_managed_policy_metadata(file(0, 0644), directory(0, 0755), 0).is_error());
    EXPECT(!Privacy::validate_managed_policy_metadata(file(0, 0600), directory(0, 0700), 0).is_error());

    // Another owner, for the file or its directory.
    EXPECT(Privacy::validate_managed_policy_metadata(file(501, 0644), directory(0, 0755), 0).is_error());
    EXPECT(Privacy::validate_managed_policy_metadata(file(0, 0644), directory(501, 0755), 0).is_error());
    // Writable by a group or by anyone.
    EXPECT(Privacy::validate_managed_policy_metadata(file(0, 0664), directory(0, 0755), 0).is_error());
    EXPECT(Privacy::validate_managed_policy_metadata(file(0, 0646), directory(0, 0755), 0).is_error());
    EXPECT(Privacy::validate_managed_policy_metadata(file(0, 0644), directory(0, 0775), 0).is_error());
    EXPECT(Privacy::validate_managed_policy_metadata(file(0, 0644), directory(0, 01777), 0).is_error());
    // Not a file, such as a symbolic link examined as itself, or a directory that isn't one.
    EXPECT(Privacy::validate_managed_policy_metadata({ .is_regular_file = false, .is_directory = false, .owner = 0, .mode = 0644 }, directory(0, 0755), 0).is_error());
    EXPECT(Privacy::validate_managed_policy_metadata(file(0, 0644), file(0, 0755), 0).is_error());
}

TEST_CASE(the_policy_format_rejects_what_it_does_not_understand)
{
    auto offline = TRY_OR_FAIL(Privacy::parse_managed_policy(R"({"version":1,"network":{"mode":"offline"}})"sv));
    EXPECT(offline.network_ceiling == Privacy::NetworkPolicy::offline());
    EXPECT(!offline.strip_tracking_parameters);

    auto routed = TRY_OR_FAIL(Privacy::parse_managed_policy(R"({"version":1,"network":{"mode":"proxy-only","proxy":"socks5h://10.0.0.1:1080"}})"sv));
    EXPECT(routed.network_ceiling == Privacy::NetworkPolicy::proxy_only(Privacy::ProxyRoute::parse("socks5h://10.0.0.1:1080"sv).release_value()));

    auto open = TRY_OR_FAIL(Privacy::parse_managed_policy(R"({"version":1,"network":{"mode":"allow"},"stripTrackingParameters":true})"sv));
    EXPECT(!open.network_ceiling.has_value());
    EXPECT(open.strip_tracking_parameters);

    EXPECT(TRY_OR_FAIL(Privacy::parse_managed_policy(R"({"version":1})"sv)) == Privacy::ManagedPolicy {});

    // A field it doesn't know may be a restriction the administrator expects, so it isn't skipped.
    EXPECT(Privacy::parse_managed_policy(R"({"version":1,"blockJavaScript":true})"sv).is_error());
    EXPECT(Privacy::parse_managed_policy(R"({"version":1,"network":{"mode":"offline","dns":"none"}})"sv).is_error());
    EXPECT(Privacy::parse_managed_policy(R"({"version":2,"network":{"mode":"offline"}})"sv).is_error());
    EXPECT(Privacy::parse_managed_policy(R"({"network":{"mode":"offline"}})"sv).is_error());
    EXPECT(Privacy::parse_managed_policy(R"({"version":1,"network":{"mode":"tor"}})"sv).is_error());
    EXPECT(Privacy::parse_managed_policy(R"({"version":1,"network":{}})"sv).is_error());
    EXPECT(Privacy::parse_managed_policy(R"({"version":1,"network":{"mode":"proxy-only"}})"sv).is_error());
    EXPECT(Privacy::parse_managed_policy(R"({"version":1,"network":{"mode":"offline","proxy":"socks5h://10.0.0.1:1080"}})"sv).is_error());
    // A route must be an IP literal, so naming the proxy can't leak a lookup.
    EXPECT(Privacy::parse_managed_policy(R"({"version":1,"network":{"mode":"proxy-only","proxy":"socks5h://proxy.example:1080"}})"sv).is_error());
    EXPECT(Privacy::parse_managed_policy(R"({"version":1,"stripTrackingParameters":"yes"})"sv).is_error());
    EXPECT(Privacy::parse_managed_policy(R"({"version":1,"network":{"mode":"offline"})"sv).is_error());
    EXPECT(Privacy::parse_managed_policy(R"([1])"sv).is_error());
}

static ByteString make_policy_directory()
{
    auto path = ByteString::formatted("{}/ladyflock-managed-policy-{}", Core::StandardPaths::tempfile_directory(), getpid());
    (void)FileSystem::remove(path, FileSystem::RecursionMode::Allowed);
    MUST(Core::Directory::create(path, Core::Directory::CreateDirectories::Yes));
    MUST(Core::System::chmod(path, 0755));
    return path;
}

static void write_policy(ByteString const& path, StringView contents, mode_t mode = 0644)
{
    (void)FileSystem::remove(path, FileSystem::RecursionMode::Disallowed);
    auto policy = MUST(Core::File::open(path, Core::File::OpenMode::Write));
    MUST(policy->write_until_depleted(contents));
    MUST(Core::System::chmod(path, mode));
}

TEST_CASE(a_policy_is_loaded_only_from_a_trusted_file_and_a_bad_one_closes_the_network)
{
    // Outside tests the owner must be root. Here it is this account, which owns the files the test writes.
    auto owner = getuid();
    auto directory_path = make_policy_directory();
    auto path = LexicalPath::join(directory_path, "policy.json"sv).string();

    auto absent = Privacy::load_managed_policy(path, owner);
    EXPECT_EQ(absent.status, Privacy::ManagedPolicyStatus::Absent);
    EXPECT(absent.effective_policy() == Privacy::ManagedPolicy {});

    write_policy(path, R"({"version":1,"network":{"mode":"offline"},"stripTrackingParameters":true})"sv);
    auto active = Privacy::load_managed_policy(path, owner);
    EXPECT_EQ(active.status, Privacy::ManagedPolicyStatus::Active);
    EXPECT(active.effective_policy().network_ceiling == Privacy::NetworkPolicy::offline());
    EXPECT(active.effective_policy().strip_tracking_parameters);

    // The same file, required to belong to someone else (root, as outside tests).
    if (owner != 0)
        EXPECT_EQ(Privacy::load_managed_policy(path, 0).status, Privacy::ManagedPolicyStatus::Rejected);

    auto expect_rejected = [&](StringView why) {
        auto result = Privacy::load_managed_policy(path, owner);
        if (result.status != Privacy::ManagedPolicyStatus::Rejected)
            FAIL(ByteString::formatted("Expected rejection: {}", why));
        EXPECT(result.effective_policy().network_ceiling == Privacy::NetworkPolicy::deny_all());
        EXPECT(!result.reason.is_empty());
    };

    write_policy(path, R"({"version":1,"network":{"mode":"offline"}})"sv, 0666);
    expect_rejected("a file anyone can write"sv);

    write_policy(path, R"({"version":1,"network":{"mode":"offline"}})"sv);
    MUST(Core::System::chmod(directory_path, 0777));
    expect_rejected("a directory anyone can write"sv);
    MUST(Core::System::chmod(directory_path, 0755));

    write_policy(path, R"({"version":1,"network":{"mode":"offline"},"futureRestriction":true})"sv);
    expect_rejected("a field it doesn't know"sv);

    write_policy(path, R"({"version":1,"network":{"mode":"off)"sv);
    expect_rejected("a truncated file"sv);

    auto target = LexicalPath::join(directory_path, "elsewhere.json"sv).string();
    write_policy(target, R"({"version":1})"sv);
    MUST(FileSystem::remove(path, FileSystem::RecursionMode::Disallowed));
    MUST(Core::System::symlink(target, path));
    expect_rejected("a symbolic link"sv);

    MUST(FileSystem::remove(directory_path, FileSystem::RecursionMode::Allowed));
}

TEST_CASE(the_managed_ceiling_holds_against_every_other_input)
{
    auto compile = [](Privacy::PolicyInput const& input) {
        return Privacy::PolicyCompiler::compile(1, 1, input).release_value().snapshot.network;
    };

    // The user's open preference stays within an Offline ceiling.
    auto offline = compile({ .administrative_ceiling = Privacy::NetworkPolicy::offline(), .user_preference = Privacy::NetworkPolicy::allow_default() });
    EXPECT(!offline.allows_network_egress());

    // A managed route binds a session that asked for direct access.
    auto managed_route = Privacy::ProxyRoute::parse("socks5h://10.0.0.1:1080"sv).release_value();
    auto routed = compile({ .administrative_ceiling = Privacy::NetworkPolicy::proxy_only(managed_route), .user_preference = Privacy::NetworkPolicy::allow_default() });
    EXPECT(routed.requires_mandatory_route());
    EXPECT(routed.route == managed_route);

    // An identity that requires a different route, such as a Tor-routed one, gets neither: an empty intersection.
    auto tor_route = Privacy::ProxyRoute::parse("socks5h://127.0.0.1:9050"sv).release_value();
    auto conflicting = compile({
        .administrative_ceiling = Privacy::NetworkPolicy::proxy_only(managed_route),
        .identity_requirement = Privacy::NetworkPolicy::proxy_only(tor_route),
        .user_preference = Privacy::NetworkPolicy::allow_default(),
    });
    EXPECT(!conflicting.allows_network_egress());
}
