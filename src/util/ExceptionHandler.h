#pragma once

#include <cstdint>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <windows.h>

#ifdef LATITE_CRASH_REPORTING

extern __declspec(thread) CONTEXT g_CxxExceptionContext;
extern __declspec(thread) bool g_bHasCxxExceptionContext;
LONG WINAPI VectoredExceptionHandler(PEXCEPTION_POINTERS pExceptionInfo);

namespace DebugExceptionHandler {
    using SehCallback = std::uintptr_t(__cdecl*)(void*);
    using SehVoidCallback = void(__cdecl*)(void*);

    class ErrorBoundaryScope {
    public:
        ErrorBoundaryScope();
        ErrorBoundaryScope(ErrorBoundaryScope const&) = delete;
        ErrorBoundaryScope& operator=(ErrorBoundaryScope const&) = delete;
        ~ErrorBoundaryScope();
    };

    void Install();
    void Uninstall();
    void PrepareErrorBoundary();
    bool IsHandlingCrash();
    [[noreturn]] void AbortProcess();
    std::uintptr_t RunWithSehGuard(SehCallback callback, void* context, char const* reason) noexcept;
    void RunVoidWithSehGuard(SehVoidCallback callback, void* context, char const* reason) noexcept;
    std::string GenerateStackTrace(CONTEXT* contextArg = nullptr, HANDLE threadHandle = nullptr);
    // Debug builds: a background thread that writes every thread's stack to latest.log when the game stops running hooks.
    void StartHangWatchdog();
    std::filesystem::path WriteCrashReport(EXCEPTION_POINTERS* exceptionInfo, std::string_view reason);
    // Logs an access violation (once per faulting address) without a minidump and without ending the process.
    void WriteSurvivedReport(EXCEPTION_POINTERS* exceptionInfo, std::string_view reason);
}

void LogExceptionDetails(const std::exception& e);
void LogUnknownExceptionDetails(std::string_view reason);

#endif
