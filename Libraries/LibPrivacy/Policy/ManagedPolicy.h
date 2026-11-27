/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <LibPrivacy/Export.h>
#include <LibPrivacy/Policy/Types.h>

#include <sys/types.h>

namespace Privacy {

// An administrator's policy, read once at startup from a file only the system administrator can write. Its network
// policy becomes the administrative ceiling of every identity: command-line options, an identity's own mode and the
// user's preferences can narrow it but never widen it.
struct PRIVACY_API ManagedPolicy {
    // The widest network policy any identity may have. Nothing means the policy does not restrict the network.
    Optional<NetworkPolicy> network_ceiling;
    // Tracking-parameter stripping is on, whatever the command line says.
    bool strip_tracking_parameters { false };

    bool operator==(ManagedPolicy const&) const = default;
};

enum class ManagedPolicyStatus : u8 {
    // There is no policy file.
    Absent,
    Active,
    // There is a policy file that can't be trusted or understood. The browser fails closed: no identity has network
    // access, because the administrator's restriction can't be known.
    Rejected,
};

PRIVACY_API StringView managed_policy_status_to_string(ManagedPolicyStatus);

struct PRIVACY_API ManagedPolicyLoadResult {
    ManagedPolicyStatus status { ManagedPolicyStatus::Absent };
    ManagedPolicy policy;
    // Why a policy was rejected.
    ByteString reason;

    // What the browser enforces: the policy itself, or no network at all for a rejected one.
    ManagedPolicy effective_policy() const;
};

// What the loader needs to know about a file or directory, so the ownership rules can be checked without root.
struct PRIVACY_API ManagedPolicyFileMetadata {
    bool is_regular_file { false };
    bool is_directory { false };
    uid_t owner { 0 };
    mode_t mode { 0 };
};

// The policy file and its directory must belong to the required owner (root, outside tests), and neither may be
// writable by anyone else, so no other account can have written or replaced the policy.
PRIVACY_API ErrorOr<void> validate_managed_policy_metadata(ManagedPolicyFileMetadata const& file, ManagedPolicyFileMetadata const& directory, uid_t required_owner);

// Parses version 1 of the policy format. A field this version doesn't know is an error, not something to skip, as it
// may be a restriction the administrator expects to be enforced.
PRIVACY_API ErrorOr<ManagedPolicy> parse_managed_policy(StringView json);

// Reads and checks the policy at the given path.
PRIVACY_API ManagedPolicyLoadResult load_managed_policy(ByteString const& path, uid_t required_owner = 0);

// Where this platform looks for the policy, or nothing on a platform without a protected location.
PRIVACY_API Optional<ByteString> default_managed_policy_path();

}
