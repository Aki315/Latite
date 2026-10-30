#pragma once
#include <atomic>
#include <cstdint>
#include "util/Logger.h"

// Debug breadcrumbs: the first call of each hook is written to latest.log. When the game dies without any error
// message, the last [Trace] line shows what ran last.
#ifdef LATITE_DEBUG
#define LATITE_TRACE_HOOK(name)                                                                                        \
    do {                                                                                                               \
        static std::atomic<bool> latiteTraced_ { false };                                                              \
        if (!latiteTraced_.exchange(true)) Logger::Info("[Trace] first call: {}", name);                              \
    } while (0)
// Logs the 1st call, then every "interval"th call (shows how long the game kept running).
#define LATITE_TRACE_EVERY(name, interval)                                                                             \
    do {                                                                                                               \
        static std::atomic<uint64_t> latiteCount_ { 0 };                                                               \
        const uint64_t latiteN_ = ++latiteCount_;                                                                      \
        if (latiteN_ == 1 || latiteN_ % (interval) == 0) Logger::Info("[Trace] {} call #{}", name, latiteN_);          \
    } while (0)
#else
#define LATITE_TRACE_HOOK(name) ((void)0)
#define LATITE_TRACE_EVERY(name, interval) ((void)0)
#endif
