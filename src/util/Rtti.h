#pragma once
// Reads the C++ class name of a game object from its RTTI, and uses it to re-find fields after a game update
// moved them. Everything here is guarded: a bad pointer returns false instead of crashing.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <Windows.h>

namespace rtti {
    inline uintptr_t imageBase() {
        static const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
        return base;
    }

    inline uintptr_t imageSize() {
        static const uintptr_t size = [] {
            const auto base = imageBase();
            const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            return static_cast<uintptr_t>(nt->OptionalHeader.SizeOfImage);
        }();
        return size;
    }

    inline bool inImage(uintptr_t p) {
        return p >= imageBase() && p < imageBase() + imageSize();
    }

    inline bool plausiblePtr(uintptr_t p) {
        return p >= 0x10000 && p < 0x00007FFFFFFFFFFFull && (p & 7) == 0;
    }

    // Number of guarded probes running on this thread. The debug exception logger ignores access violations raised
    // while it is above zero, since those are expected (that is what a probe is for).
    inline thread_local int probeDepth = 0;

    inline bool readPtr(const void* addr, void** out) {
        bool ok = false;
        ++probeDepth;
        __try {
            *out = *reinterpret_cast<void* const*>(addr);
            ok = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            *out = nullptr;
        }
        --probeDepth;
        return ok;
    }

    inline bool readBytes(const void* addr, void* out, size_t count) {
        bool ok = false;
        ++probeDepth;
        __try {
            std::memcpy(out, addr, count);
            ok = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        --probeDepth;
        return ok;
    }

    inline bool readByte(const void* addr, uint8_t* out) {
        bool ok = false;
        ++probeDepth;
        __try {
            *out = *reinterpret_cast<const uint8_t*>(addr);
            ok = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            *out = 0;
        }
        --probeDepth;
        return ok;
    }

    // Class name of the polymorphic object at obj, e.g. "LocalPlayer". False if obj isn't such an object.
    // (The game has no RTTI for most of its classes, so this mostly says false.)
    inline bool className(const void* obj, char* out, size_t cap) {
        if (cap == 0) return false;
        out[0] = 0;
        bool ok = false;
        ++probeDepth;
        __try {
            do {
                const auto o = reinterpret_cast<uintptr_t>(obj);
                if (!plausiblePtr(o)) break;
                const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(o);
                if (!inImage(vt) || (vt & 7)) break;
                const uintptr_t col = *reinterpret_cast<const uintptr_t*>(vt - 8);
                if (!inImage(col)) break;
                if (*reinterpret_cast<const uint32_t*>(col) != 1) break; // COL signature (x64)
                const uintptr_t td = imageBase() + *reinterpret_cast<const uint32_t*>(col + 12);
                if (!inImage(td)) break;
                const char* n = reinterpret_cast<const char*>(td + 0x10);
                if (n[0] != '.' || n[1] != '?' || n[2] != 'A') break;
                n += 4;
                size_t i = 0;
                while (n[i] && n[i] != '@' && i + 1 < cap && i < 200) {
                    out[i] = n[i];
                    i++;
                }
                out[i] = 0;
                ok = i > 0;
            } while (false);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            out[0] = 0;
            ok = false;
        }
        --probeDepth;
        return ok;
    }

    // needle starting with '=' must equal the class name, otherwise it only has to be contained in it.
    inline bool nameMatches(const char* name, const char* needle) {
        if (needle[0] == 0) return false;
        if (needle[0] == '=') return std::strcmp(name, needle + 1) == 0;
        return std::strstr(name, needle) != nullptr;
    }

    // Finds the pointer field of base (searching outwards from defaultOff, within +-window bytes) that points at
    // an object whose class matches one of the needles, or equals exactPtr. Returns 0 when nothing matches.
    inline size_t findPointerField(const void* base, size_t defaultOff, size_t window, const char* const* needles,
                                   size_t needleCount, const void* exactPtr = nullptr) {
        for (size_t d = 0; d <= window; d += 8) {
            for (int sign = 0; sign < 2; sign++) {
                if (sign == 1 && d == 0) continue;
                size_t off;
                if (sign == 0) {
                    off = defaultOff + d;
                } else {
                    if (d > defaultOff) continue;
                    off = defaultOff - d;
                }
                void* p = nullptr;
                if (!readPtr(reinterpret_cast<const char*>(base) + off, &p) || !p) continue;
                if (exactPtr && p == exactPtr) return off;
                char name[128];
                if (!className(p, name, sizeof(name))) continue;
                for (size_t i = 0; i < needleCount; i++) {
                    if (nameMatches(name, needles[i])) return off;
                }
            }
        }
        return 0;
    }

    inline size_t findPointerField(const void* base, size_t defaultOff, size_t window, const char* needle,
                                   const void* exactPtr = nullptr) {
        return findPointerField(base, defaultOff, window, &needle, 1, exactPtr);
    }
}
