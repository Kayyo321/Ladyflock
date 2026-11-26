/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>
#include <LibPrivacy/Policy/NetworkGate.h>
#include <LibURL/URL.h>

namespace Privacy::Hooks {

// Minimal integration adapters called from RequestServer enforcement points.
inline EvaluationResult on_before_request(NetworkGate const& gate, URL::URL const& url, bool is_revalidation = false, bool is_speculative = false)
{
    return gate.evaluate_http_request(url, is_revalidation, is_speculative);
}

inline EvaluationResult on_before_fresh_cache_read(NetworkGate const& gate)
{
    return gate.evaluate_fresh_cache_read();
}

inline EvaluationResult on_before_dns_lookup(NetworkGate const& gate, StringView host, bool is_system_resolver = false)
{
    return gate.evaluate_dns_lookup(host, is_system_resolver);
}

inline EvaluationResult on_before_websocket_connect(NetworkGate const& gate, URL::URL const& url)
{
    return gate.evaluate_websocket(url);
}

inline EvaluationResult on_before_aia_fetch(NetworkGate const& gate, URL::URL const& url)
{
    return gate.evaluate_aia_fetch(url);
}

}
