#pragma once

#include <cstdio>

#ifdef ENABLE_TRACE_LOG
#define TRACE_LOG(fmt, ...) \
    std::fprintf(stderr, "[TRACE] " fmt "\n" __VA_OPT__(, ) __VA_ARGS__)
#else
#define TRACE_LOG(fmt, ...) ((void)0)
#endif
