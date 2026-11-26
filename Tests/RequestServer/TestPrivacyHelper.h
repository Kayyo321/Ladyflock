/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibPrivacy/Policy/Compiler.h>
#include <RequestServer/PrivacyPolicy.h>

namespace RequestServerTest {

inline void install_allow_privacy_policy(u64 session_id = 1)
{
    RequestServer::reset_privacy_network_gate();
    auto compiled = Privacy::PolicyCompiler::compile(session_id, 1, { .user_preference = Privacy::NetworkPolicy::allow_default() });
    VERIFY(!compiled.is_error());
    auto outcome = RequestServer::privacy_network_gate().install(compiled.value().snapshot);
    VERIFY(outcome.ack.accepted);
}

}
