#pragma once

#include <cstdio>

// SteamVR can surface DebugRequest output outside developer logs. Keep the
// original provider's tracker identity while avoiding bridge implementation
// names, remote indices, serials, and arbitrary request text in that response.
#define snprintf(buffer, response_size, format, ...) \
    snprintf(buffer, response_size, "Standable tracker ready")
