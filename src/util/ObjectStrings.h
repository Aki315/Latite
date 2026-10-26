#pragma once
// Finds text inside a game object whose layout we do not know. No game code in here, so it is unit tested on its own
// (tests/ObjectStringsTest.cpp). The Shield Indicator uses it to work out which render controller (the thing that draws
// a model, for example "controller.render.bf_shield" for Blurry's shield) it is being called for.
//
// Windows x64 std::string is 32 bytes: 16 bytes that hold the text itself while it is short (up to 15 characters) or a
// pointer to the text once it is longer, then the size, then the capacity. The scan walks an object 8 bytes at a time
// and treats every position as a possible std::string, keeping those that hold sensible printable text.
//
// Every read of game memory goes through the Reader you pass in (bool read(const void* from, void* to, size_t bytes)),
// so the caller decides how to do it safely. Nothing here dereferences a pointer itself.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace ObjectStrings {
    constexpr size_t kStringBytes = 32;
    constexpr size_t kMinLength = 4;
    constexpr size_t kMaxLength = 160;
    constexpr size_t kMaxCapacity = 4096;
    constexpr size_t kInlineCapacity = 15;

    inline bool printable(const char* text, size_t length) {
        for (size_t i = 0; i < length; ++i) {
            if (text[i] < 32 || text[i] > 126) return false;
        }
        return true;
    }

    // Reads the biggest block of at most maxBytes from `at` that the reader allows (the object may end before maxBytes,
    // at the end of a memory page). Returns how many bytes it got, 0 when nothing could be read.
    template <class Reader>
    size_t readBlock(const void* at, size_t maxBytes, std::vector<unsigned char>& block, Reader&& read) {
        for (size_t bytes = maxBytes; bytes >= kStringBytes; bytes /= 2) {
            block.assign(bytes, 0);
            if (read(at, block.data(), bytes)) return bytes;
        }
        block.clear();
        return 0;
    }

    // The text of the std::string that starts at raw[0..32), or nothing when it doesn't look like one.
    template <class Reader>
    bool textOfString(const unsigned char* raw, Reader&& read, std::string& out) {
        uint64_t size = 0, capacity = 0;
        std::memcpy(&size, raw + 16, sizeof size);
        std::memcpy(&capacity, raw + 24, sizeof capacity);
        if (size < kMinLength || size > kMaxLength || capacity < size || capacity > kMaxCapacity) return false;

        char text[kMaxLength + 1];
        if (capacity <= kInlineCapacity) {
            std::memcpy(text, raw, static_cast<size_t>(size));
        } else {
            uint64_t pointer = 0;
            std::memcpy(&pointer, raw, sizeof pointer);
            if (pointer < 0x10000) return false; // Null or a small number, not an address.
            if (!read(reinterpret_cast<const void*>(static_cast<uintptr_t>(pointer)), text, static_cast<size_t>(size))) {
                return false;
            }
        }
        if (!printable(text, static_cast<size_t>(size))) return false;
        out.assign(text, static_cast<size_t>(size));
        return true;
    }

    // Adds every string in the block (8-byte steps) that contains `needle` to `found`, once each.
    template <class Reader>
    void collectFromBlock(const std::vector<unsigned char>& block, size_t bytes, std::string_view needle, Reader&& read,
                          std::vector<std::string>& found, size_t maxFound) {
        std::string text;
        for (size_t off = 0; off + kStringBytes <= bytes && found.size() < maxFound; off += 8) {
            if (!textOfString(block.data() + off, read, text)) continue;
            if (text.find(needle) == std::string::npos) continue;
            bool known = false;
            for (auto const& f : found) known = known || f == text;
            if (!known) found.push_back(text);
        }
    }

    // Looks for text containing `needle` in the object at `object` (its first `objectBytes` bytes), and in the objects its
    // first `maxPointers` pointer-sized fields point to (their first `pointedBytes` bytes): the name is not necessarily
    // stored in the object itself. Finds at most `maxFound` different strings.
    template <class Reader>
    std::vector<std::string> findStrings(const void* object, std::string_view needle, Reader&& read,
                                         size_t objectBytes = 0x400, size_t pointedBytes = 0x100,
                                         size_t maxPointers = 96, size_t maxFound = 8) {
        std::vector<std::string> found;
        std::vector<unsigned char> block;
        const size_t got = readBlock(object, objectBytes, block, read);
        if (got == 0) return found;

        collectFromBlock(block, got, needle, read, found, maxFound);

        size_t followed = 0;
        std::vector<unsigned char> pointed;
        for (size_t off = 0; off + 8 <= got && followed < maxPointers && found.size() < maxFound; off += 8) {
            uint64_t pointer = 0;
            std::memcpy(&pointer, block.data() + off, sizeof pointer);
            if (pointer < 0x10000 || pointer > 0x00007FFFFFFFFFFFull) continue; // Not a user-mode address.
            ++followed;
            const size_t pointedGot =
                readBlock(reinterpret_cast<const void*>(static_cast<uintptr_t>(pointer)), pointedBytes, pointed, read);
            if (pointedGot == 0) continue;
            collectFromBlock(pointed, pointedGot, needle, read, found, maxFound);
        }
        return found;
    }

    // Is this text a name that belongs to a shield's render controller? It has to say "shield" and must not be an
    // animation (the player's controllers can mention shield animations; those are not the shield being drawn).
    inline bool looksLikeShieldName(std::string_view text) {
        if (text.find("shield") == std::string_view::npos) return false;
        constexpr std::string_view animation = "animation.";
        return text.substr(0, animation.size()) != animation;
    }
}
