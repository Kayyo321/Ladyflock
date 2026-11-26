/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Privacy {

enum class NetworkMode : u8;
enum class CachePolicy : u8;
enum class DNSRoutePolicy : u8;
enum class WebRTCPolicy : u8;
enum class PolicyDecision : u8;
enum class PolicyReason : u8;

struct NetworkPolicy;
struct PolicySnapshot;
struct PolicyInstallAck;
struct PolicyInput;
struct PolicyCompilationResult;
struct EvaluationResult;
struct InstallOutcome;

class NetworkGate;

}
