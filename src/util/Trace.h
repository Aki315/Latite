#pragma once
#include <atomic>
#include <cstdint>
#include <Windows.h>
#include <cstring>
#include <mutex>
#include <set>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include "util/Logger.h"
#include "util/ExceptionHandler.h"

// Written by the hooks; read by the hang watchdog (ExceptionHandler.cpp) to notice when the game stops making progress.
inline std::atomic<uint64_t> latiteHeartbeat { 0 };
inline std::atomic<const char*> latiteLastHook { nullptr };
inline std::atomic<DWORD> latiteLastHookThread { 0 };

// Logs the first time a given event listener runs, so a hang inside a listener can be pinned down.
template<typename Ev, typename Fn>
inline bool latiteTraceListener(const Ev& ev, const Fn& fn) {
    static std::mutex mtx;
    static std::set<std::pair<uint64_t, uint64_t>> seen;
    uint64_t raw[2] = {};
    std::memcpy(raw, &fn, sizeof(fn) < sizeof(raw) ? sizeof(fn) : sizeof(raw));
    {
        std::lock_guard lock { mtx };
        if (!seen.insert({ raw[0], raw[1] }).second) return false;
    }
    Logger::Info("[Trace] first call of listener {}", DebugExceptionHandler::DescribeAddress(raw[0]));
    return true;
}

// Runs f; in the debug build an access violation inside it is logged once and skipped instead of ending the game.
template<typename F>
inline void latiteGuarded(F&& f, const char* why) {
#ifdef LATITE_CRASH_REPORTING
    using Fn = std::remove_reference_t<F>;
    DebugExceptionHandler::RunVoidWithSehGuard(
        [](void* c) {
            (*static_cast<Fn*>(c))();
        },
        &f, why);
#else
    f();
#endif
}

// Debug breadcrumbs: the first call of each hook is written to latest.log. When the game dies without any error
// message, the last [Trace] line shows what ran last.
#ifdef LATITE_DEBUG
#define LATITE_BEAT(name)                                                                                              \
    do {                                                                                                               \
        latiteHeartbeat.fetch_add(1, std::memory_order_relaxed);                                                       \
        latiteLastHook.store(name, std::memory_order_relaxed);                                                         \
        latiteLastHookThread.store(GetCurrentThreadId(), std::memory_order_relaxed);                                   \
    } while (0)
#define LATITE_TRACE_HOOK(name)                                                                                        \
    do {                                                                                                               \
        LATITE_BEAT(name);                                                                                             \
        static std::atomic<bool> latiteTraced_ { false };                                                              \
        if (!latiteTraced_.exchange(true)) Logger::Info("[Trace] first call: {}", name);                              \
    } while (0)
// Logs the 1st call, then every "interval"th call (shows how long the game kept running).
#define LATITE_TRACE_EVERY(name, interval)                                                                             \
    do {                                                                                                               \
        LATITE_BEAT(name);                                                                                             \
        static std::atomic<uint64_t> latiteCount_ { 0 };                                                               \
        const uint64_t latiteN_ = ++latiteCount_;                                                                      \
        if (latiteN_ == 1 || latiteN_ % (interval) == 0) Logger::Info("[Trace] {} call #{}", name, latiteN_);          \
    } while (0)
#else
#define LATITE_BEAT(name) ((void)0)
#define LATITE_TRACE_HOOK(name) ((void)0)
#define LATITE_TRACE_EVERY(name, interval) ((void)0)
#endif
