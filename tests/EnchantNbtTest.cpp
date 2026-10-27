// Tests for src/util/EnchantNbt.h (reading an item's enchantment levels out of its NBT tag). No game dependencies, so it
// builds anywhere:
//
//   g++ -std=c++20 -Wall -Wextra -Isrc tests/EnchantNbtTest.cpp -o enchant_nbt_test && ./enchant_nbt_test
//
// The "game memory" here is made up: compound tags, map nodes, strings and lists laid out the way the reader expects
// them (Windows x64 / MSVC). They test the walking and the checks, NOT that the real game's tags look like this: that
// part can only be checked in game (the Debug Log setting says what was read).

#include "util/EnchantNbt.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
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

// All the memory a test builds. The reader only reads inside it and refuses everything else, like the game's reader
// refuses unmapped addresses.
struct Arena {
    std::vector<std::unique_ptr<std::vector<unsigned char>>> blocks;

    unsigned char* make(size_t bytes) {
        blocks.push_back(std::make_unique<std::vector<unsigned char>>(bytes + 64, 0));
        return blocks.back()->data();
    }

    bool read(const void* from, void* to, size_t bytes) const {
        const auto begin = reinterpret_cast<uintptr_t>(from);
        for (auto const& block : blocks) {
            const auto start = reinterpret_cast<uintptr_t>(block->data());
            if (begin >= start && begin + bytes <= start + block->size()) {
                std::memcpy(to, from, bytes);
                return true;
            }
        }
        return false;
    }
};

static uint64_t addr(const void* p) { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p)); }
static void put64(unsigned char* at, uint64_t v) { std::memcpy(at, &v, 8); }

static void putString(Arena& a, unsigned char* dest, std::string const& text) {
    std::memset(dest, 0, 32);
    const uint64_t size = text.size();
    const uint64_t capacity = text.size() <= 15 ? 15 : (text.size() | 15);
    if (text.size() <= 15) {
        std::memcpy(dest, text.data(), text.size());
    } else {
        unsigned char* heap = a.make(text.size() + 1);
        std::memcpy(heap, text.data(), text.size());
        put64(dest, addr(heap));
    }
    std::memcpy(dest + 16, &size, 8);
    std::memcpy(dest + 24, &capacity, 8);
}

// A map node: left 0, parent 8, right 16, color 24, isnil 25, key at 32, value (a tag, starting with its vtable) at 64.
struct Node {
    unsigned char* raw;
};

static Node makeNode(Arena& a, std::string const& key, unsigned char* value /* copied into the node, 16 bytes */) {
    Node n { a.make(0x60) };
    putString(a, n.raw + 0x20, key);
    if (value) std::memcpy(n.raw + 0x40, value, 16);
    return n;
}

// A ShortTag: a vtable-looking pointer, then the number.
static std::array<unsigned char, 16> shortTag(int16_t v) {
    std::array<unsigned char, 16> t {};
    put64(t.data(), 0x7FF600001000ull);
    std::memcpy(t.data() + 8, &v, 2);
    return t;
}

// A compound tag whose map holds the given nodes. The nodes are chained into a (degenerate) tree: each one is the right
// child of the one before it, under a sentinel head.
static unsigned char* makeCompound(Arena& a, std::vector<Node> const& nodes, size_t headOffsetInCompound = 8) {
    unsigned char* compound = a.make(0x40);
    put64(compound, 0x7FF600002000ull); // vtable
    unsigned char* head = a.make(0x60);
    head[0x19] = 1; // isnil
    put64(head + 0, addr(head));
    put64(head + 16, addr(head));
    if (nodes.empty()) {
        put64(head + 8, addr(head));
    } else {
        put64(head + 8, addr(nodes[0].raw)); // parent of the head = the root
        for (size_t i = 0; i < nodes.size(); ++i) {
            put64(nodes[i].raw + 0, addr(head));
            put64(nodes[i].raw + 8, i ? addr(nodes[i - 1].raw) : addr(head));
            put64(nodes[i].raw + 16, i + 1 < nodes.size() ? addr(nodes[i + 1].raw) : addr(head));
        }
    }
    put64(compound + headOffsetInCompound, addr(head));
    put64(compound + headOffsetInCompound + 8, nodes.size());
    return compound;
}

// A compound tag standing for one enchantment: {id: short, lvl: short}.
static unsigned char* makeEnchant(Arena& a, int16_t id, int16_t level) {
    auto idTag = shortTag(id), levelTag = shortTag(level);
    return makeCompound(a, { makeNode(a, "id", idTag.data()), makeNode(a, "lvl", levelTag.data()) });
}

// A ListTag value (vtable, then begin / end / capacity of the vector of owned element pointers), 16 bytes of it fit in a
// node, so it gets its own block and the node's value field holds the same 16 bytes.
static unsigned char* makeList(Arena& a, std::vector<unsigned char*> const& elements) {
    unsigned char* vec = a.make(elements.size() * 8 + 8);
    for (size_t i = 0; i < elements.size(); ++i) put64(vec + i * 8, addr(elements[i]));
    unsigned char* list = a.make(0x30);
    put64(list, 0x7FF600003000ull); // vtable
    put64(list + 8, addr(vec));
    put64(list + 16, addr(vec) + elements.size() * 8);
    put64(list + 24, addr(vec) + elements.size() * 8);
    return list;
}

// A whole item tag: {Damage: 0, ench: [ ... ]} with the list stored inline in the node's variant (as the game does).
static unsigned char* makeItemTag(Arena& a, std::vector<std::pair<int16_t, int16_t>> const& enchants,
                                  size_t headOffset = 8) {
    std::vector<unsigned char*> elements;
    for (auto const& e : enchants) elements.push_back(makeEnchant(a, e.first, e.second));
    unsigned char* list = makeList(a, elements);

    auto damage = shortTag(7);
    Node damageNode = makeNode(a, "Damage", damage.data());
    // The ListTag object itself is the variant: vtable, vector begin, vector end, capacity. Make room for all 40 bytes
    // by writing straight into the node (nodes have 0x60 bytes; the value starts at 0x40).
    Node enchNode = makeNode(a, "ench", nullptr);
    std::memcpy(enchNode.raw + 0x40, list, 0x20);
    return makeCompound(a, { damageNode, enchNode }, headOffset);
}

static void testSharpnessBonus() {
    CHECK(EnchantNbt::sharpnessBonus(0) == 0.f);
    CHECK(EnchantNbt::sharpnessBonus(1) == 1.f);
    CHECK(EnchantNbt::sharpnessBonus(3) == 2.f);
    CHECK(EnchantNbt::sharpnessBonus(5) == 3.f); // The old "Sharpness V = +3".
    CHECK(EnchantNbt::sharpnessBonus(-2) == 0.f);
}

static void testReadsLevels() {
    Arena a;
    unsigned char* tag = makeItemTag(a, { { 9, 5 }, { 17, 3 } });
    auto r = EnchantNbt::readEnchants(tag, [&](const void* f, void* t, size_t n) { return a.read(f, t, n); });
    CHECK(r.status == EnchantNbt::Status::Parsed);
    CHECK(r.list.size() == 2);
    CHECK(r.levelOf(EnchantNbt::kSharpness) == 5);
    CHECK(r.levelOf(17) == 3);
    CHECK(r.levelOf(13) == 0); // Fire Aspect: not there.
}

static void testOtherSharpnessLevels() {
    for (int level : { 1, 2, 3, 4, 5, 6, 10 }) {
        Arena a;
        unsigned char* tag = makeItemTag(a, { { 9, static_cast<int16_t>(level) } });
        auto r = EnchantNbt::readEnchants(tag, [&](const void* f, void* t, size_t n) { return a.read(f, t, n); });
        CHECK(r.status == EnchantNbt::Status::Parsed && r.levelOf(9) == level);
    }
}

static void testGlowingWithoutSharpness() {
    // A sword with only Fire Aspect glows, but it has no Sharpness: that is known, not unreadable.
    Arena a;
    unsigned char* tag = makeItemTag(a, { { 13, 2 } });
    auto r = EnchantNbt::readEnchants(tag, [&](const void* f, void* t, size_t n) { return a.read(f, t, n); });
    CHECK(r.status == EnchantNbt::Status::Parsed);
    CHECK(r.levelOf(EnchantNbt::kSharpness) == 0);
}

static void testNoTag() {
    auto none = [](const void*, void*, size_t) { return false; };
    auto r = EnchantNbt::readEnchants(nullptr, none);
    CHECK(r.status == EnchantNbt::Status::Parsed && r.list.empty());
}

static void testTagWithoutEnchantList() {
    // Only a Damage tag (a worn but plain sword): the enchant list is not there, so nothing is known from it.
    Arena a;
    auto damage = shortTag(12);
    unsigned char* tag = makeCompound(a, { makeNode(a, "Damage", damage.data()) });
    auto r = EnchantNbt::readEnchants(tag, [&](const void* f, void* t, size_t n) { return a.read(f, t, n); });
    CHECK(r.status == EnchantNbt::Status::Unreadable);
}

static void testUnreadableMemory() {
    Arena a;
    unsigned char* tag = makeItemTag(a, { { 9, 5 } });
    auto refuse = [](const void*, void*, size_t) { return false; };
    CHECK(EnchantNbt::readEnchants(tag, refuse).status == EnchantNbt::Status::Unreadable);

    // Garbage where a tag should be.
    unsigned char* junk = a.make(0x40);
    for (size_t i = 0; i < 0x40; ++i) junk[i] = static_cast<unsigned char>(0xA5 + i);
    CHECK(EnchantNbt::readEnchants(junk, [&](const void* f, void* t, size_t n) { return a.read(f, t, n); }).status ==
          EnchantNbt::Status::Unreadable);
    // A pointer that leads nowhere.
    CHECK(EnchantNbt::readEnchants(reinterpret_cast<void*>(0x1234567800ull),
                                   [&](const void* f, void* t, size_t n) { return a.read(f, t, n); })
              .status == EnchantNbt::Status::Unreadable);
}

static void testImplausibleValuesAreRejected() {
    Arena a;
    {
        unsigned char* tag = makeItemTag(a, { { 9, 999 } }); // Level far too high for a short that is really a level.
        auto r = EnchantNbt::readEnchants(tag, [&](const void* f, void* t, size_t n) { return a.read(f, t, n); });
        CHECK(r.status == EnchantNbt::Status::Unreadable);
    }
    {
        unsigned char* tag = makeItemTag(a, { { 9, 0 } }); // Level 0 is not an enchantment.
        auto r = EnchantNbt::readEnchants(tag, [&](const void* f, void* t, size_t n) { return a.read(f, t, n); });
        CHECK(r.status == EnchantNbt::Status::Unreadable);
    }
    {
        unsigned char* tag = makeItemTag(a, { { -3, 2 }, { 9, 4 } }); // One bad entry is skipped, the rest is used.
        auto r = EnchantNbt::readEnchants(tag, [&](const void* f, void* t, size_t n) { return a.read(f, t, n); });
        CHECK(r.status == EnchantNbt::Status::Parsed && r.levelOf(9) == 4 && r.list.size() == 1);
    }
}

static void testLoopsEnd() {
    // A node that points back at itself must not hang the walk.
    Arena a;
    unsigned char* tag = makeItemTag(a, { { 9, 5 } });
    unsigned char* head = reinterpret_cast<unsigned char*>(static_cast<uintptr_t>(0));
    uint64_t headPtr = 0;
    std::memcpy(&headPtr, tag + 8, 8);
    head = reinterpret_cast<unsigned char*>(static_cast<uintptr_t>(headPtr));
    uint64_t root = 0;
    std::memcpy(&root, head + 8, 8);
    unsigned char* rootNode = reinterpret_cast<unsigned char*>(static_cast<uintptr_t>(root));
    put64(rootNode + 0, addr(rootNode));  // left -> itself
    put64(rootNode + 16, addr(rootNode)); // right -> itself
    auto r = EnchantNbt::readEnchants(tag, [&](const void* f, void* t, size_t n) { return a.read(f, t, n); });
    CHECK(r.status == EnchantNbt::Status::Unreadable); // (and it returned at all)
}

static void testLongKeyAndHeadFurtherIn() {
    // The map head not right after the vtable but one field later: still found.
    Arena a;
    unsigned char* tag = makeItemTag(a, { { 9, 3 } }, 16);
    auto r = EnchantNbt::readEnchants(tag, [&](const void* f, void* t, size_t n) { return a.read(f, t, n); });
    CHECK(r.status == EnchantNbt::Status::Parsed && r.levelOf(9) == 3);

    // A key longer than the inline buffer (stored on the heap) is read too.
    unsigned char* node = a.make(0x60);
    putString(a, node + 0x20, "a_rather_long_key_name");
    unsigned char* head = a.make(0x60);
    head[0x19] = 1;
    put64(head, addr(head));
    put64(head + 8, addr(node));
    put64(head + 16, addr(head));
    put64(node, addr(head));
    put64(node + 8, addr(head));
    put64(node + 16, addr(head));
    unsigned char* compound = a.make(0x40);
    put64(compound + 8, addr(head));
    bool ok = false;
    auto entries = EnchantNbt::detail::entriesOf(addr(compound), [&](const void* f, void* t, size_t n) { return a.read(f, t, n); }, ok);
    CHECK(ok && entries.size() == 1 && entries[0].key == "a_rather_long_key_name");
}

int main() {
    testSharpnessBonus();
    testReadsLevels();
    testOtherSharpnessLevels();
    testGlowingWithoutSharpness();
    testNoTag();
    testTagWithoutEnchantList();
    testUnreadableMemory();
    testImplausibleValuesAreRejected();
    testLoopsEnd();
    testLongKeyAndHeadFurtherIn();

    std::printf("%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
