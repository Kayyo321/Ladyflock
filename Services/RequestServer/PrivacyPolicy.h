/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibPrivacy/Policy/NetworkGate.h>

namespace RequestServer {

// Process-wide network policy gate owned by RequestServer's control plane.
// Starts default-closed until Hub installs a valid PolicySnapshot.
Privacy::NetworkGate& privacy_network_gate();

// Resets the gate back to its default-closed Uninitialized state when a new
// ControlConnectionFromClient is established.
void reset_privacy_network_gate();

}
