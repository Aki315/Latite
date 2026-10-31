#pragma once
// entt asserts when a component registry does not look like an entt registry. That happens when a Minecraft update
// changes the layout the SDK expects, and the default assert() opens a modal dialog that cannot be caught. Raise a
// structured exception instead: the guards around component lookups (Actor::tryGetComponent) catch it.
#include <Windows.h>

constexpr DWORD kLatiteEnttAssertCode = 0xE0A77001;

inline void latiteEnttAssertFail() {
    RaiseException(kLatiteEnttAssertCode, 0, 0, nullptr);
}

#define ENTT_ASSERT(condition, msg) ((condition) ? void(0) : latiteEnttAssertFail())
