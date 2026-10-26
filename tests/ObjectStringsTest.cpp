// Tests for src/util/ObjectStrings.h (finding a name inside a game object). No game dependencies, so it builds anywhere:
//
//   g++ -std=c++20 -Wall -Wextra -Isrc tests/ObjectStringsTest.cpp -o object_strings_test && ./object_strings_test
//
// The objects here are made-up buffers laid out like Windows x64 std::string (16 bytes of text or a pointer, size,
// capacity). They test the searching, not reading real game memory.

#include "util/ObjectStrings.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(cond)) {                                                                                                 \
            ++failures;                                                                                                \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                                                        \
        }                                                                                                              \
    } while (0)

// The memory a test has set up. The reader below only reads inside it and refuses everything else, like the game's
// reader refuses addresses that are not mapped (the scan follows plenty of pointers that lead nowhere).
static std::vector<std::pair<uintptr_t, uintptr_t>> validRanges;

static void allow(const void* at, size_t bytes) {
    const auto begin = reinterpret_cast<uintptr_t>(at);
    validRanges.emplace_back(begin, begin + bytes);
}

static bool readAny(const void* from, void* to, size_t bytes) {
    const auto begin = reinterpret_cast<uintptr_t>(from);
    for (auto const& range : validRanges) {
        if (begin >= range.first && begin + bytes <= range.second) {
            std::memcpy(to, from, bytes);
            return true;
        }
    }
    return false;
}

// Reads nothing: as if every address were unreadable.
static bool readNothing(const void*, void*, size_t) { return false; }

// Writes a std::string-shaped field at dest (32 bytes). Short text goes inline, long text goes to heapText.
static void putString(unsigned char* dest, std::string const& text, std::vector<std::string>& heap) {
    std::memset(dest, 0, ObjectStrings::kStringBytes);
    uint64_t size = text.size();
    uint64_t capacity = text.size() <= 15 ? 15 : text.size() | 15;
    if (text.size() <= 15) {
        std::memcpy(dest, text.data(), text.size());
    } else {
        heap.push_back(text);
        const char* p = heap.back().c_str();
        allow(p, heap.back().size() + 1);
        std::memcpy(dest, &p, sizeof p);
    }
    std::memcpy(dest + 16, &size, 8);
    std::memcpy(dest + 24, &capacity, 8);
}

static bool contains(std::vector<std::string> const& v, std::string const& s) {
    for (auto const& x : v) {
        if (x == s) return true;
    }
    return false;
}

static void testLongNameInObject() {
    validRanges.clear();
    std::vector<std::string> heap;
    heap.reserve(8); // the pointers must stay valid
    alignas(8) unsigned char object[0x200] = {};
    allow(object, sizeof object);
    putString(object + 0x40, "controller.render.bf_shield", heap);

    auto found = ObjectStrings::findStrings(object, "shield", readAny, sizeof object);
    CHECK(found.size() == 1);
    CHECK(contains(found, "controller.render.bf_shield"));
}

static void testShortNameInline() {
    validRanges.clear();
    std::vector<std::string> heap;
    alignas(8) unsigned char object[0x100] = {};
    allow(object, sizeof object);
    putString(object + 0x20, "my_shield", heap); // 9 characters: stored inline

    auto found = ObjectStrings::findStrings(object, "shield", readAny, sizeof object);
    CHECK(contains(found, "my_shield"));
}

static void testNameBehindPointer() {
    validRanges.clear();
    std::vector<std::string> heap;
    heap.reserve(8);
    alignas(8) unsigned char inner[0x100] = {};
    allow(inner, sizeof inner);
    putString(inner + 0x18, "controller.render.bf_shield", heap);

    // The outer object only holds a pointer to the inner one.
    alignas(8) unsigned char outer[0x100] = {};
    allow(outer, sizeof outer);
    const unsigned char* innerPtr = inner;
    std::memcpy(outer + 0x28, &innerPtr, sizeof innerPtr);

    auto found = ObjectStrings::findStrings(outer, "shield", readAny, sizeof outer, sizeof inner);
    CHECK(contains(found, "controller.render.bf_shield"));
}

static void testOtherNamesAreIgnored() {
    validRanges.clear();
    std::vector<std::string> heap;
    heap.reserve(8);
    alignas(8) unsigned char object[0x200] = {};
    allow(object, sizeof object);
    putString(object + 0x00, "controller.render.player.first_person", heap);
    putString(object + 0x40, "texture.default", heap);

    auto found = ObjectStrings::findStrings(object, "shield", readAny, sizeof object);
    CHECK(found.empty());
}

static void testGarbageIsIgnored() {
    validRanges.clear();
    // Bytes that are not text, and a size that is too big or a capacity that is too small, must not be taken for a name.
    alignas(8) unsigned char object[0x100];
    allow(object, sizeof object);
    for (size_t i = 0; i < sizeof object; ++i) object[i] = static_cast<unsigned char>(i * 37 + 11);
    auto found = ObjectStrings::findStrings(object, "shield", readAny, sizeof object);
    CHECK(found.empty());

    std::vector<std::string> heap;
    heap.reserve(4);
    alignas(8) unsigned char bad[0x100] = {};
    allow(bad, sizeof bad);
    putString(bad, "controller.render.bf_shield", heap);
    uint64_t hugeSize = 100000;
    std::memcpy(bad + 16, &hugeSize, 8);
    CHECK(ObjectStrings::findStrings(bad, "shield", readAny, sizeof bad).empty());

    putString(bad, "controller.render.bf_shield", heap);
    uint64_t smallCapacity = 4; // smaller than the size
    std::memcpy(bad + 24, &smallCapacity, 8);
    CHECK(ObjectStrings::findStrings(bad, "shield", readAny, sizeof bad).empty());
}

static void testUnreadableMemory() {
    validRanges.clear();
    std::vector<std::string> heap;
    heap.reserve(4);
    alignas(8) unsigned char object[0x100] = {};
    allow(object, sizeof object);
    putString(object, "controller.render.bf_shield", heap);
    CHECK(ObjectStrings::findStrings(object, "shield", readNothing, sizeof object).empty());
}

static void testObjectEndsEarly() {
    validRanges.clear();
    // A reader that only allows 0x80 bytes at a time: the scan must shrink its block until the read works.
    std::vector<std::string> heap;
    heap.reserve(4);
    alignas(8) unsigned char object[0x400] = {};
    allow(object, sizeof object);
    putString(object + 0x20, "controller.render.bf_shield", heap);
    auto limited = [](const void* from, void* to, size_t bytes) {
        if (bytes > 0x80) return false;
        return readAny(from, to, bytes);
    };
    auto found = ObjectStrings::findStrings(object, "shield", limited, 0x400);
    CHECK(contains(found, "controller.render.bf_shield"));
}

static void testDuplicatesCountOnce() {
    validRanges.clear();
    std::vector<std::string> heap;
    heap.reserve(4);
    alignas(8) unsigned char object[0x200] = {};
    allow(object, sizeof object);
    putString(object + 0x00, "controller.render.bf_shield", heap);
    putString(object + 0x80, "controller.render.bf_shield", heap);
    auto found = ObjectStrings::findStrings(object, "shield", readAny, sizeof object);
    CHECK(found.size() == 1);
}

static void testShieldNameRule() {
    CHECK(ObjectStrings::looksLikeShieldName("controller.render.bf_shield"));
    CHECK(ObjectStrings::looksLikeShieldName("array.bf_shield_state"));
    CHECK(ObjectStrings::looksLikeShieldName("texture.bf_shield_blocking"));
    CHECK(!ObjectStrings::looksLikeShieldName("animation.player.shield_block"));
    CHECK(!ObjectStrings::looksLikeShieldName("controller.render.item_default"));
    CHECK(!ObjectStrings::looksLikeShieldName("controller.render.player.third_person"));
}

int main() {
    testLongNameInObject();
    testShortNameInline();
    testNameBehindPointer();
    testOtherNamesAreIgnored();
    testGarbageIsIgnored();
    testUnreadableMemory();
    testObjectEndsEarly();
    testDuplicatesCountOnce();
    testShieldNameRule();

    std::printf("%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
