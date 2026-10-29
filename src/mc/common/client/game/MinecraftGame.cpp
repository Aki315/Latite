#include "pch.h"
#include "MinecraftGame.h"
#include "mc/Addresses.h"

namespace {
    struct ClientInterface {
        void* clientInterface;
        std::shared_ptr<SDK::ClientInstance> clientInstance;
    };

    static_assert(offsetof(ClientInterface, clientInstance) == 0x8);
    static_assert(sizeof(ClientInterface) == 0x18);
}

bool SDK::MinecraftGame::isCursorGrabbed() {
    return hat::member_at<bool>(this, 0x1E8);
}

SDK::FontRepository* SDK::MinecraftGame::getFontRepository() {
    NonOwnerPointer<const FontRepository> fontRepository;
    memory::callVirtual<void, NonOwnerPointer<const FontRepository>&>(this, 0x3F, fontRepository);
    return const_cast<FontRepository*>(fontRepository.get());
}

namespace {
    // The MinecraftGame layout shifts between game versions, so the client map isn't read from a hardcoded
    // offset blindly: candidates are validated (MSVC std::map node layout, key 0, a ClientInstance whose vtable
    // points into the game image) under SEH. No objects with destructors in these functions, which __try requires.
    constexpr size_t kDefaultMapOffset = 0x970;
    constexpr size_t kScanStart = 0x600;
    constexpr size_t kScanEnd = 0x1400;

    bool inImage(uintptr_t p) {
        static uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
        static uintptr_t size = [] {
            auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            return static_cast<uintptr_t>(nt->OptionalHeader.SizeOfImage);
        }();
        return p >= base && p < base + size;
    }

    // Returns the ClientInstance of the node with key 0 in a map whose object sits at mapObj, or null.
    void* readPrimaryFromMap(uintptr_t mapObj) {
        __try {
            const uintptr_t head = *reinterpret_cast<const uintptr_t*>(mapObj);
            const uintptr_t count = *reinterpret_cast<const uintptr_t*>(mapObj + 8);
            if (head < 0x10000 || (head & 7) || count == 0 || count > 8) return nullptr;
            // _Isnil of the head node
            if (*reinterpret_cast<const uint8_t*>(head + 0x19) != 1) return nullptr;
            uintptr_t node = *reinterpret_cast<const uintptr_t*>(head + 8); // root
            for (int depth = 0; depth < 16; depth++) {
                if (node < 0x10000 || (node & 7)) return nullptr;
                if (*reinterpret_cast<const uint8_t*>(node + 0x19) == 1) return nullptr; // hit nil, no key 0
                const uint8_t key = *reinterpret_cast<const uint8_t*>(node + 0x20);
                if (key == 0) {
                    const uintptr_t inst = *reinterpret_cast<const uintptr_t*>(node + 0x30);
                    const uintptr_t ctrl = *reinterpret_cast<const uintptr_t*>(node + 0x38);
                    if (inst < 0x10000 || (inst & 7) || ctrl < 0x10000) return nullptr;
                    const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(inst);
                    if (!inImage(vt)) return nullptr;
                    return reinterpret_cast<void*>(inst);
                }
                node = *reinterpret_cast<const uintptr_t*>(node + (0 < key ? 0 : 0x10)); // key>0: left, else right
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        return nullptr;
    }
}

SDK::ClientInstance* SDK::MinecraftGame::getPrimaryClientInstance() {
    static size_t foundOffset = 0;
    const auto self = reinterpret_cast<uintptr_t>(this);

    if (foundOffset) {
        return static_cast<ClientInstance*>(readPrimaryFromMap(self + foundOffset));
    }

    void* inst = readPrimaryFromMap(self + kDefaultMapOffset);
    if (inst) {
        foundOffset = kDefaultMapOffset;
        return static_cast<ClientInstance*>(inst);
    }

    for (size_t off = kScanStart; off < kScanEnd; off += 8) {
        if (off == kDefaultMapOffset) continue;
        inst = readPrimaryFromMap(self + off);
        if (inst) {
            foundOffset = off;
            Logger::Info("MinecraftGame client map found at +0x{:X} (expected +0x{:X})", off, kDefaultMapOffset);
            return static_cast<ClientInstance*>(inst);
        }
    }
    return nullptr;
}
