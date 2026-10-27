#pragma once
// Reads the enchantments (which enchantment, which level) of an item out of its NBT "user data" tag, so the armor
// estimate can use the Sharpness level of the weapon you are actually swinging instead of assuming Sharpness V.
//
// No game code in here, so it is unit tested on its own (tests/EnchantNbtTest.cpp).
//
// The layout of the game's CompoundTag is NOT known for sure (the SDK only has an empty CompoundTag class), so this
// does not trust one exact layout: it follows the Windows x64 (MSVC) layout it expects, checks every step, and gives
// up with Status::Unreadable when something does not look right, in which case the caller falls back to assuming a
// level. Levels are only returned when all of this checks out: a key "ench" whose value is a list of compounds, each
// with "id" and "lvl" numbers in a sane range.
//
// What it expects (Windows x64, MSVC):
//   CompoundTag      vtable (8), then a std::map<std::string, CompoundTagVariant>: head node pointer, size.
//   map node         left (0), parent (8), right (16), color (24), isnil (25), then the pair at 32:
//                    std::string key (32 bytes), then the variant (the tag itself, stored inline, starts with its vtable).
//   ListTag          vtable (8), then a std::vector of owning pointers to the elements: begin, end, capacity.
//   ShortTag/IntTag  vtable (8), then the number (little endian: a short is read, which is also right for a small int).
//   enchantment      a compound with "id" and "lvl".
//
// Every read of game memory goes through the Reader you pass in (bool read(const void* from, void* to, size_t bytes)),
// so the caller decides how to do it safely (the game side uses a reader that survives an access violation). Nothing
// here dereferences a pointer itself, and every loop is bounded.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace EnchantNbt {
    constexpr int kSharpness = 9; // Bedrock enchantment id (Protection is 0, Smite 10, Bane of Arthropods 11).
    constexpr int kMaxLevel = 255;
    constexpr size_t kMaxNodes = 64;
    constexpr size_t kMaxEnchants = 32;

    // How much damage Sharpness adds in Blurry's Java PvP: 0.5 per level plus 0.5 (Sharpness V is +3), none without it.
    inline float sharpnessBonus(int level) { return level > 0 ? 0.5f * static_cast<float>(level) + 0.5f : 0.f; }

    enum class Status {
        Unreadable, // The tag was not understood. Nothing is known about the enchantments.
        Parsed,     // The enchantments are known (possibly none, for an item with no tag at all).
    };

    struct Enchants {
        Status status = Status::Unreadable;
        std::vector<std::pair<int, int>> list; // (enchantment id, level)

        // The level of one enchantment, 0 when the item does not have it. Only meaningful when status is Parsed.
        int levelOf(int id) const {
            int best = 0;
            for (auto const& e : list) {
                if (e.first == id && e.second > best) best = e.second;
            }
            return best;
        }
    };

    namespace detail {
        constexpr size_t kNodeBytes = 0x50;
        constexpr size_t kKeyOffset = 0x20;
        constexpr size_t kValueOffset = 0x40;

        inline bool looksLikeAddress(uint64_t p) { return p >= 0x10000 && p <= 0x00007FFFFFFFFFFFull; }

        inline uint64_t qword(const unsigned char* block, size_t off) {
            uint64_t v = 0;
            std::memcpy(&v, block + off, sizeof v);
            return v;
        }

        // The text of the std::string that starts at raw[0..32): 1 to 32 printable characters.
        template <class Reader>
        bool stringAt(const unsigned char* raw, Reader&& read, std::string& out) {
            const uint64_t size = qword(raw, 16), capacity = qword(raw, 24);
            if (size < 1 || size > 32 || capacity < size || capacity > 4096) return false;
            char text[33];
            if (capacity <= 15) {
                std::memcpy(text, raw, static_cast<size_t>(size));
            } else {
                const uint64_t pointer = qword(raw, 0);
                if (!looksLikeAddress(pointer)) return false;
                if (!read(reinterpret_cast<const void*>(static_cast<uintptr_t>(pointer)), text,
                          static_cast<size_t>(size))) {
                    return false;
                }
            }
            for (size_t i = 0; i < size; ++i) {
                if (text[i] < 32 || text[i] > 126) return false;
            }
            out.assign(text, static_cast<size_t>(size));
            return true;
        }

        // One entry of a compound: its key and where its value (the tag, starting with its vtable) is.
        struct Entry {
            std::string key;
            uintptr_t value;
        };

        // Walks the map of the compound tag at `compound` and returns its entries. `ok` is false when the map was not
        // recognised at all (no sentinel head node found), as opposed to being recognised and empty.
        template <class Reader>
        std::vector<Entry> entriesOf(uintptr_t compound, Reader&& read, bool& ok) {
            ok = false;
            std::vector<Entry> entries;
            unsigned char head[0x28];
            unsigned char self[0x28];
            if (!looksLikeAddress(compound) || !read(reinterpret_cast<const void*>(compound), self, sizeof self)) {
                return entries;
            }

            // The head pointer of the map follows the vtable. Look at the first few fields rather than only one, and
            // accept the one that points at a sentinel node (isnil set, pointers that are addresses or null).
            uintptr_t headAddress = 0;
            for (size_t off = 8; off <= 0x20 && !headAddress; off += 8) {
                const uint64_t candidate = qword(self, off);
                if (!looksLikeAddress(candidate)) continue;
                if (!read(reinterpret_cast<const void*>(static_cast<uintptr_t>(candidate)), head, sizeof head)) continue;
                const bool isNil = head[0x19] == 1;
                const uint64_t left = qword(head, 0), parent = qword(head, 8), right = qword(head, 16);
                auto pointerOrNull = [](uint64_t p) { return p == 0 || looksLikeAddress(p); };
                if (isNil && pointerOrNull(left) && pointerOrNull(parent) && pointerOrNull(right)) {
                    headAddress = static_cast<uintptr_t>(candidate);
                }
            }
            if (!headAddress) return entries;
            ok = true;

            std::vector<uintptr_t> pending;
            std::unordered_set<uintptr_t> seen;
            seen.insert(headAddress);
            const uint64_t root = qword(head, 8);
            if (looksLikeAddress(root)) pending.push_back(static_cast<uintptr_t>(root));

            unsigned char node[kNodeBytes];
            size_t visited = 0;
            while (!pending.empty() && visited < kMaxNodes) {
                const uintptr_t at = pending.back();
                pending.pop_back();
                if (!seen.insert(at).second) continue; // Already seen: a loop, or a pointer back to the head.
                if (!read(reinterpret_cast<const void*>(at), node, sizeof node)) continue;
                ++visited;
                if (node[0x19] == 1) continue; // A sentinel.

                for (size_t off : { size_t(0), size_t(16) }) { // left, right
                    const uint64_t child = qword(node, off);
                    if (looksLikeAddress(child)) pending.push_back(static_cast<uintptr_t>(child));
                }
                std::string key;
                if (stringAt(node + kKeyOffset, read, key)) entries.push_back({ key, at + kValueOffset });
            }
            return entries;
        }

        // The number stored in a ShortTag / IntTag at `tag`.
        template <class Reader>
        bool numberAt(uintptr_t tag, Reader&& read, int& out) {
            unsigned char raw[16];
            if (!looksLikeAddress(tag) || !read(reinterpret_cast<const void*>(tag), raw, sizeof raw)) return false;
            int16_t value = 0;
            std::memcpy(&value, raw + 8, sizeof value);
            out = value;
            return true;
        }

        inline bool findEntry(std::vector<Entry> const& entries, std::string_view key, uintptr_t& value) {
            for (auto const& e : entries) {
                if (e.key == key) {
                    value = e.value;
                    return true;
                }
            }
            return false;
        }

        // The elements of the ListTag at `list`: the addresses of the tags it owns.
        template <class Reader>
        std::vector<uintptr_t> listElements(uintptr_t list, Reader&& read) {
            std::vector<uintptr_t> elements;
            unsigned char raw[0x30];
            if (!looksLikeAddress(list) || !read(reinterpret_cast<const void*>(list), raw, sizeof raw)) return elements;
            // The vector follows the vtable (look one field further as well, in case something sits in between).
            for (size_t off = 8; off <= 16 && elements.empty(); off += 8) {
                const uint64_t begin = qword(raw, off), end = qword(raw, off + 8), cap = qword(raw, off + 16);
                if (!looksLikeAddress(begin) || end < begin || cap < end || (end - begin) % 8 != 0) continue;
                const size_t count = static_cast<size_t>((end - begin) / 8);
                if (count == 0 || count > kMaxEnchants) continue;
                std::vector<uintptr_t> candidate;
                for (size_t i = 0; i < count; ++i) {
                    uint64_t element = 0;
                    if (!read(reinterpret_cast<const void*>(static_cast<uintptr_t>(begin) + i * 8), &element,
                              sizeof element) ||
                        !looksLikeAddress(element)) {
                        candidate.clear();
                        break;
                    }
                    candidate.push_back(static_cast<uintptr_t>(element));
                }
                if (!candidate.empty()) elements = std::move(candidate);
            }
            return elements;
        }
    } // namespace detail

    // The enchantments of an item, from its user-data tag (ItemStackBase::tag). A null tag is an item with no tag data,
    // so it has no enchantments.
    template <class Reader>
    Enchants readEnchants(const void* tag, Reader&& read) {
        Enchants result;
        if (!tag) {
            result.status = Status::Parsed;
            return result;
        }

        bool ok = false;
        auto entries = detail::entriesOf(reinterpret_cast<uintptr_t>(tag), read, ok);
        uintptr_t list = 0;
        if (!ok || !detail::findEntry(entries, "ench", list)) return result;

        for (uintptr_t element : detail::listElements(list, read)) {
            bool elementOk = false;
            auto fields = detail::entriesOf(element, read, elementOk);
            uintptr_t idTag = 0, levelTag = 0;
            if (!elementOk || !detail::findEntry(fields, "id", idTag) ||
                !detail::findEntry(fields, "lvl", levelTag)) {
                continue;
            }
            int id = 0, level = 0;
            if (!detail::numberAt(idTag, read, id) || !detail::numberAt(levelTag, read, level)) continue;
            if (id < 0 || id > 120 || level < 1 || level > kMaxLevel) continue; // Not an enchantment as we know it.
            result.list.emplace_back(id, level);
        }

        if (!result.list.empty()) result.status = Status::Parsed;
        return result;
    }
}
