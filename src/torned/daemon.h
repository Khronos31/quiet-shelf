// SPDX-License-Identifier: GPL-2.0-only
#ifndef TORNE_DAEMON_DAEMON_H
#define TORNE_DAEMON_DAEMON_H

#include "torned/backend.h"
#include "torned/config.h"

#include <cstdio>

namespace torne::daemon {

// Runs the parsed command. A null backend is the normal framework build and
// must fail explicitly; a fake backend exists only in tests.
int run_daemon(const DaemonConfig& config, Backend* backend,
               std::FILE* output, std::FILE* error) noexcept;

}  // namespace torne::daemon

#endif  // TORNE_DAEMON_DAEMON_H
