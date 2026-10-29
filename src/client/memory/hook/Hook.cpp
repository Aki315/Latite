#include "pch.h"
#include "Hook.h"
#include "util/logger.h"
#include "MinHook.h"
#include "vhook/vtable_hook.h"

Hook::Hook(uintptr_t targetAddress, void* detourFunc, std::string const& hookName, bool tableSwap)
    : funcPtr(nullptr)
    , detour(detourFunc)
    ,
#ifdef LATITE_DEBUG
    funcName(hookName)
    ,
#else
    funcName("")
    ,
#endif
    tableSwapHook(tableSwap)
    , target(targetAddress) {

    if (tableSwap) {
        auto res = vh::hook(reinterpret_cast<LPVOID*>(targetAddress), detourFunc, &this->funcPtr);
        if (res != 0) {
#ifdef LATITE_DEBUG
            Logger::Warn("Creation of hook {} failed with status {}", this->funcName, vh::status_to_string(res));
#endif
        }
        return;
    }

    MH_STATUS res = MH_CreateHook(reinterpret_cast<LPVOID>(targetAddress), detourFunc, &this->funcPtr);
    if (res != MH_OK) {
#ifdef LATITE_DEBUG
        Logger::Warn("Creation of hook {} failed with status {}", this->funcName, MH_StatusToString(res));
#endif
    }
#ifdef LATITE_DEBUG
    else {
        // The address and first bytes make it easy to see whether a hook landed on a real function start.
        uint8_t bytes[8] = {};
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
        std::memcpy(bytes, reinterpret_cast<void*>(targetAddress), sizeof(bytes));
        Logger::Info("[Hook] {} at exe+0x{:X}: {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}", this->funcName,
                     targetAddress - base, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6],
                     bytes[7]);
    }
#endif
}

HookGroup::HookGroup(std::string const& groupName)
#ifdef LATITE_DEBUG
    : groupName(groupName)
#else
    // hopefully the name gets optimized away
    : groupName("")
#endif
{
}

std::shared_ptr<Hook> HookGroup::addHook(uintptr_t ptr, func_ptr_t detour, const char* name) {
#ifdef LATITE_DEBUG
    auto newHook = std::make_shared<Hook>(ptr, detour, name);
#else
    auto newHook = std::make_shared<Hook>(ptr, detour, "");
#endif
    hooks.emplace_back(newHook);
    return newHook;
}

std::shared_ptr<Hook> HookGroup::addTableSwapHook(uintptr_t ptr, func_ptr_t detour, const char* name) {
#ifdef LATITE_DEBUG
    auto newHook = std::make_shared<Hook>(ptr, detour, name, true);
#else
    auto newHook = std::make_shared<Hook>(ptr, detour, "", true);

#endif
    hooks.emplace_back(newHook);
    return newHook;
}
