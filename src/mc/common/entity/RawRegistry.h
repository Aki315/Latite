#pragma once
// Finds a component by walking the game's entt registry in memory, the way the game's own inlined lookup does.
// entt::basic_registry::try_get from the SDK's copy of entt assumes a container layout that is not the one the game
// was built with, so on 1.26.60 it asserts or reads garbage. Layout (checked against a client that reads 1.26.60
// from outside the process):
//   registry + 0x48/0x50  vector of hash-bucket heads (count is a power of two)
//   registry + 0x68/0x70  vector of 0x20-byte nodes { u64 next; u32 typeHash; u32 pad; Storage* storage; ... }
//   storage  + 0x08/0x10  vector of sparse pages (2048 u32 slots each, indexed by the entity's low 18 bits)
//   storage  + 0x50       array of component pages (128 components each)
// A slot holds the entity's high bits xor'ed with its packed position.
#include <Windows.h>
#include <cstdint>
#include <cstring>

namespace rawregistry {
    inline bool plausible(uintptr_t p) {
        return p >= 0x10000 && p < 0x00007FFFFFFFFFFFull;
    }

    template<typename T>
    inline bool rd(uintptr_t addr, T& out) {
        if (!plausible(addr)) return false;
        std::memcpy(&out, reinterpret_cast<const void*>(addr), sizeof(T));
        return true;
    }

    inline uintptr_t findStorage(uintptr_t registry, uint32_t hash) {
        uintptr_t buckets = 0, bucketsEnd = 0, nodes = 0, nodesEnd = 0;
        if (!plausible(registry) || !rd(registry + 0x48, buckets) || !rd(registry + 0x50, bucketsEnd) ||
            !rd(registry + 0x68, nodes) || !rd(registry + 0x70, nodesEnd) || !plausible(buckets) ||
            bucketsEnd <= buckets || !plausible(nodes) || nodesEnd < nodes)
            return 0;
        const uint64_t count = (bucketsEnd - buckets) / 8;
        if (!count || count > (1u << 24) || (count & (count - 1))) return 0;
        uint64_t cur = 0;
        if (!rd(buckets + (hash & (count - 1)) * 8, cur)) return 0;
        for (int guard = 0; guard < 2048; ++guard) {
            if (cur == ~0ull || cur > (1u << 24)) return 0;
            const uintptr_t node = nodes + cur * 0x20;
            if (node >= nodesEnd) return 0;
            struct Raw {
                uint64_t next;
                uint32_t key, pad;
                uintptr_t storage;
            } raw;
            if (!rd(node, raw)) return 0;
            if (raw.key == hash) return plausible(raw.storage) ? raw.storage : 0;
            cur = raw.next;
        }
        return 0;
    }

    // Calls fn(typeHash) for every component storage the registry has (at most `limit` of them).
    template<typename Fn>
    inline void forEachStorageHash(uintptr_t registry, Fn&& fn, size_t limit = 4096) {
        uintptr_t nodes = 0, nodesEnd = 0;
        if (!plausible(registry) || !rd(registry + 0x68, nodes) || !rd(registry + 0x70, nodesEnd) || !plausible(nodes) ||
            nodesEnd <= nodes)
            return;
        size_t n = 0;
        for (uintptr_t node = nodes; node + 0x20 <= nodesEnd && n < limit; node += 0x20, ++n) {
            uint32_t key = 0;
            if (!rd(node + 8, key)) return;
            fn(key);
        }
    }

    // Address of the component with the given type hash and size for the entity, or 0 if it has none.
    inline uintptr_t findComponent(uintptr_t registry, uint32_t entity, uint32_t hash, size_t size) {
        const uintptr_t storage = findStorage(registry, hash);
        if (!storage) return 0;
        const uint32_t index = entity & 0x3FFFFu;
        if (index == 0x3FFFFu) return 0;
        uintptr_t pages = 0, pagesEnd = 0, page = 0, comps = 0, compPage = 0;
        uint32_t packed = 0;
        if (!rd(storage + 0x08, pages) || !rd(storage + 0x10, pagesEnd) || !plausible(pages) || pagesEnd < pages) return 0;
        if ((uint64_t)(index >> 11) >= (pagesEnd - pages) / 8) return 0;
        if (!rd(pages + (uintptr_t)(index >> 11) * 8, page) || !plausible(page) ||
            !rd(page + (uintptr_t)(index & 0x7FF) * 4, packed))
            return 0;
        if (((entity & 0xFFFC0000u) ^ packed) >= 0x3FFFFu) return 0; // the entity does not have this component
        if (!rd(storage + 0x50, comps) || !plausible(comps) ||
            !rd(comps + (uintptr_t)((packed & 0x3FF80u) >> 4), compPage) || !plausible(compPage))
            return 0;
        return compPage + (uintptr_t)(packed & 0x7Fu) * size;
    }
}
