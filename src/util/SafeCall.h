#pragma once
#include <Windows.h>
#include <type_traits>

// Calls into the game that may be wrong on a new Minecraft version run through here: a crash inside returns false
// instead of killing the game (C++ exceptions are left alone).
namespace latite_safe {
    inline bool invokeRaw(void (*fn)(void*), void* ctx) {
        __try {
            fn(ctx);
            return true;
        } __except (GetExceptionCode() == 0xE06D7363 ? EXCEPTION_CONTINUE_SEARCH : EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    template<typename F>
    inline bool run(F&& f) {
        using Fn = std::remove_reference_t<F>;
        return invokeRaw(
            [](void* c) {
                (*static_cast<Fn*>(c))();
            },
            &f);
    }
}
