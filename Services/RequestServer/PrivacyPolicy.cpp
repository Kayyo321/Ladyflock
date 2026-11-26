/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <RequestServer/PrivacyPolicy.h>

namespace RequestServer {

static NeverDestroyed<Privacy::NetworkGate> s_privacy_network_gate;

Privacy::NetworkGate& privacy_network_gate()
{
    return *s_privacy_network_gate;
}

void reset_privacy_network_gate()
{
    *s_privacy_network_gate = Privacy::NetworkGate {};
}

}
