#pragma once
#include "util/Logger.h"
#include "util/Rtti.h"

namespace rtti {
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
