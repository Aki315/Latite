#pragma once
#include "util/Logger.h"
#include "util/Rtti.h"

namespace rtti {
    // Logs the code at the start of virtual function slots [first,last] of the object at obj: where each slot points
    // (exe+RVA) and its first bytes. A game update moves virtual functions around; this shows what is in each slot.
    inline void logVtable(const char* label, const void* obj, size_t first, size_t last) {
        void* vt = nullptr;
        if (!readPtr(obj, &vt) || !inImage(reinterpret_cast<uintptr_t>(vt))) {
            Logger::Info("[Vtable] {}: no vtable in the game image", label);
            return;
        }
        Logger::Info("[Vtable] {} vtable at exe+0x{:X}, slots 0x{:X}-0x{:X}:", label,
                     reinterpret_cast<uintptr_t>(vt) - imageBase(), first, last);
        for (size_t i = first; i <= last; i++) {
            void* fn = nullptr;
            if (!readPtr(reinterpret_cast<char*>(vt) + i * 8, &fn)) break;
            const auto addr = reinterpret_cast<uintptr_t>(fn);
            if (!inImage(addr)) {
                Logger::Info("[Vtable] {} slot 0x{:X}: 0x{:X} (not in the game image)", label, i, addr);
                continue;
            }
            uint8_t b[14] = {};
            readBytes(fn, b, sizeof(b));
            Logger::Info("[Vtable] {} slot 0x{:X}: exe+0x{:X}: {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} "
                         "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                         label, i, addr - imageBase(), b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9],
                         b[10], b[11], b[12], b[13]);
        }
    }

    // Logs `size` bytes of code/data at addr as 32-byte hex lines ("[Code] label exe+0xRVA: ..."), so a function that
    // stopped working after a game update can be read back from latest.log and disassembled.
    inline void logCode(const char* label, const void* addr, size_t size) {
        const auto a = reinterpret_cast<uintptr_t>(addr);
        if (!addr || !inImage(a)) {
            Logger::Info("[Code] {}: 0x{:X} is not in the game image", label, a);
            return;
        }
        Logger::Info("[Code] {} exe+0x{:X} ({} bytes):", label, a - imageBase(), size);
        for (size_t off = 0; off < size; off += 32) {
            uint8_t b[32] = {};
            const size_t n = size - off < 32 ? size - off : 32;
            if (!readBytes(reinterpret_cast<const char*>(addr) + off, b, n)) break;
            std::string hex;
            for (size_t i = 0; i < n; i++) hex += std::format("{:02X} ", b[i]);
            Logger::Info("[Code] {} +0x{:X}: {}", label, off, hex);
        }
    }

    // Logs the raw 8-byte values in [start,end) of the object at base (pointers, flags and numbers alike).
    inline void logQwords(const char* label, const void* base, size_t start, size_t end) {
        Logger::Info("[Raw] {} raw qwords 0x{:X}-0x{:X}:", label, start, end);
        for (size_t off = start; off < end; off += 8) {
            void* p = nullptr;
            if (!readPtr(reinterpret_cast<const char*>(base) + off, &p)) {
                Logger::Info("[Raw]   {}+0x{:X}: <unreadable>", label, off);
                break;
            }
            Logger::Info("[Raw]   {}+0x{:X}: 0x{:016X}", label, off, reinterpret_cast<uintptr_t>(p));
        }
    }

    // Logs every pointer-looking value in [start,end) of the object at base, with the first qword it points at
    // (a vtable for C++ objects, printed as exe+RVA) and the class name if RTTI knows it. Used to see where fields
    // moved after a game update.
    inline void logPointerMap(const char* label, const void* base, size_t start, size_t end) {
        Logger::Info("[Offsets] {} pointer map 0x{:X}-0x{:X}:", label, start, end);
        for (size_t off = start; off < end; off += 8) {
            void* p = nullptr;
            if (!readPtr(reinterpret_cast<const char*>(base) + off, &p) || !p) continue;
            if (!plausiblePtr(reinterpret_cast<uintptr_t>(p))) continue;
            void* first = nullptr;
            if (!readPtr(p, &first)) continue;
            char name[128];
            const bool named = className(p, name, sizeof(name));
            const auto firstValue = reinterpret_cast<uintptr_t>(first);
            if (inImage(firstValue)) {
                Logger::Info("[Offsets]   {}+0x{:X}: 0x{:X} -> vtable exe+0x{:X} {}", label, off,
                             reinterpret_cast<uintptr_t>(p), firstValue - imageBase(), named ? name : "");
            } else if (named) {
                Logger::Info("[Offsets]   {}+0x{:X}: 0x{:X} {}", label, off, reinterpret_cast<uintptr_t>(p), name);
            }
        }
    }
}
