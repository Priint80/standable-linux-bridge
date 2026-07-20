#pragma once

#include "relay_tracker.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

// SteamVR can surface DebugRequest output outside developer logs. Keep the
// original provider's tracker identity while avoiding bridge implementation
// names, remote indices, serials, and arbitrary request text in that response.
// The translation unit's normal headers are pre-included above so this macro
// reaches only the existing DebugRequest snprintf call.
#define snprintf(buffer, response_size, format, ...) \
    snprintf(buffer, response_size, "Standable tracker ready")
