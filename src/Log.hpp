#pragma once

#include <cstdio>

// Trace logging for comparing execution against known-good emulators (mGBA).
// Enabled by defining ENABLE_TRACE_LOG (e.g. cmake -B build -DENABLE_TRACE_LOG=ON).
#ifdef ENABLE_TRACE_LOG
#define TRACE_LOG(fmt, ...) \
    std::fprintf(stderr, "[TRACE] " fmt "\n" __VA_OPT__(, ) __VA_ARGS__)
#else
#define TRACE_LOG(fmt, ...) ((void)0)
#endif
