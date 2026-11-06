#pragma once
// Works out what a Trial Chambers vault ejects for one player, from the level seed, the player's unique actor id and the
// vault position. Only standard C++, so it can be tested without the game.
//
// How it was found (from the game's own code, then checked against real vaults opened in the game):
//   1. A Xoroshiro128++ random is seeded from the level seed, then from the MD5 of "minecraft:vault_loot", then from the
//      player's unique actor id, then from the vault position. The low 32 bits of its first output seed a Mersenne Twister
//      (MT19937, the game's loot random).
//   2. The vault's loot table (reward / reward_ominous) is rolled with that random the way the game rolls any loot table.
// Enchanted items: the item is certain and the loot after it is right, but which enchantments it has isn't known (each
// takes a fixed number of draws that were found from real vaults: 2 for a book, 1 for gear). set_damage (the shield) isn't
// decoded yet: there the prediction stops (complete = false) and whatever would come after it is unknown.
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <random>
#include <string>
#include <vector>

namespace vault_loot {
    struct Stack {
        std::string item;    // "minecraft:arrow"
        int count = 1;
        std::string variant; // potion id of a tipped arrow ("poison"), or the amplifier of an ominous bottle ("3")
        bool details = false; // true: the item is certain but its enchantments / damage are not known
    };

    struct Result {
        std::vector<Stack> items;
        bool complete = true; // false: the loot after the last item could not be worked out
    };

    namespace detail {
        inline uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

        inline uint64_t mix13(uint64_t z) {
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            return z ^ (z >> 31);
        }

        struct Xoro {
            uint64_t s0, s1;
            Xoro(uint64_t a, uint64_t b) : s0(a), s1(b) {
                if ((s0 | s1) == 0) {
                    s0 = 0x9E3779B97F4A7C15ull;
                    s1 = 0x6A09E667F3BCC909ull;
                }
            }
            uint64_t next() {
                const uint64_t a = s0;
                uint64_t b = s1;
                const uint64_t r = rotl(a + b, 17) + a;
                b ^= a;
                s0 = rotl(a, 49) ^ b ^ (b << 21);
                s1 = rotl(b, 28);
                return r;
            }
        };

        // The low 32 bits that seed the loot random of one vault.
        inline uint32_t mtSeed(int64_t levelSeed, int64_t playerId, int x, int y, int z) {
            const uint64_t lo = static_cast<uint64_t>(levelSeed) ^ 0x6A09E667F3BCC909ull;
            const uint64_t hi = lo + 0x9E3779B97F4A7C15ull;
            Xoro g1(mix13(lo), mix13(hi));
            // MD5("minecraft:vault_loot"), as two big-endian 64-bit halves.
            const uint64_t a = g1.next() ^ 0x0cb1349481fe81faull;
            const uint64_t b = g1.next() ^ 0xda2b3a549ec566d4ull;
            Xoro g2(a, b);
            const uint64_t p = static_cast<uint64_t>(playerId);
            const uint64_t c = g2.next() ^ p;
            const uint64_t d = g2.next() ^ p;
            Xoro g3(c, d);
            const uint64_t f = g3.next();
            const uint64_t g = g3.next();
            const uint64_t zz = static_cast<uint64_t>(static_cast<int64_t>(z));
            const uint64_t yy = static_cast<uint64_t>(static_cast<int64_t>(y));
            const int32_t xx = static_cast<int32_t>(static_cast<uint32_t>(3129871u) * static_cast<uint32_t>(x));
            const uint64_t lraw = (116129781ull * zz) ^ yy ^ static_cast<uint64_t>(static_cast<int64_t>(xx));
            const uint64_t pr = lraw * (42317861ull * lraw + 11ull);
            const uint64_t pos = static_cast<uint64_t>(static_cast<int64_t>(pr) >> 16); // arithmetic shift
            Xoro g4(f ^ pos, g);
            return static_cast<uint32_t>(g4.next() & 0xFFFFFFFFull);
        }

        struct Rng {
            std::mt19937 mt; // MT19937 seeded with init_genrand, like the game's
            explicit Rng(uint32_t seed) : mt(seed) {}
            uint32_t raw() { return mt(); }
            int nextInt(int bound) { return bound == 0 ? 0 : static_cast<int>(raw() % static_cast<uint32_t>(bound)); }
            // The game's nextFloat: the raw 32-bit value scaled, NOT signed (confirmed against real vaults).
            float nextFloat() { return static_cast<float>(static_cast<double>(raw()) * 2.328306436538696e-10); }
        };

        enum class Fn { SetCount, SetPotion, Amplifier, EnchantBook, EnchantLevels, Undecoded };
        struct Func {
            Fn kind;
            float min = 0, max = 0;
            const char* text = "";
        };
        struct Entry {
            const char* name; // item id, or the table id for a nested table
            int weight;
            bool table;
            std::vector<Func> fns;
        };
        struct Pool {
            int rollsMin, rollsMax;
            float chance; // < 0: always
            std::vector<Entry> entries;
        };
        using Table = std::vector<Pool>;

        inline Func count(float a, float b) { return { Fn::SetCount, a, b, "" }; }
        inline Func potion(const char* id) { return { Fn::SetPotion, 0, 0, id }; }
        inline Func amplifier(float a, float b) { return { Fn::Amplifier, a, b, "" }; }
        inline Func enchantBook() { return { Fn::EnchantBook, 0, 0, "" }; }
        inline Func enchantLevels() { return { Fn::EnchantLevels, 0, 0, "" }; }
        inline Func undecoded(const char* what) { return { Fn::Undecoded, 0, 0, what }; }
        inline Entry item(const char* n, int w, std::initializer_list<Func> f = {}) { return { n, w, false, f }; }
        inline Entry nested(const char* n, int w) { return { n, w, true, {} }; }

        // The vault loot tables (trial_chambers/reward*.json of the game's behavior pack).
        inline Table const* findTable(std::string const& name) {
            static const Table reward = {
                { 1, 1, -1.f, { nested("reward_common", 8), nested("reward_rare", 2) } },
                { 1, 3, -1.f, { nested("reward_common", 1) } },
                { 1, 1, 0.25f, { nested("reward_unique", 1) } },
            };
            static const Table rewardCommon = {
                { 1, 1, -1.f,
                  {
                      item("minecraft:arrow", 4, { count(2, 8) }),
                      item("minecraft:arrow", 4, { count(2, 8), potion("poison") }),
                      item("minecraft:emerald", 4, { count(2, 4) }),
                      item("minecraft:wind_charge", 3, { count(1, 3) }),
                      item("minecraft:iron_ingot", 3, { count(1, 4) }),
                      item("minecraft:honey_bottle", 3, { count(1, 2) }),
                      item("minecraft:ominous_bottle", 2, { count(1, 1), amplifier(0, 1) }),
                      item("minecraft:wind_charge", 1, { count(4, 12) }),
                      item("minecraft:diamond", 1, { count(1, 2) }),
                  } },
            };
            static const Table rewardRare = {
                { 1, 1, -1.f,
                  {
                      item("minecraft:emerald", 3, { count(2, 4) }),
                      item("minecraft:shield", 3, { undecoded("set_damage") }),
                      item("minecraft:bow", 3, { enchantLevels() }),
                      item("minecraft:crossbow", 2, { enchantLevels() }),
                      item("minecraft:iron_axe", 2, { enchantLevels() }),
                      item("minecraft:iron_chestplate", 2, { enchantLevels() }),
                      item("minecraft:golden_carrot", 2, { count(1, 2) }),
                      item("minecraft:book", 2, { enchantBook() }),
                      item("minecraft:book", 2, { enchantBook() }),
                      item("minecraft:diamond_chestplate", 1, { enchantLevels() }),
                      item("minecraft:diamond_axe", 1, { enchantLevels() }),
                  } },
            };
            static const Table rewardUnique = {
                { 1, 1, -1.f,
                  {
                      item("minecraft:golden_apple", 4),
                      item("minecraft:bolt_armor_trim_smithing_template", 3),
                      item("minecraft:guster_banner_pattern", 2),
                      item("minecraft:music_disc_precipice", 2),
                      item("minecraft:trident", 1),
                  } },
            };
            static const Table ominous = {
                { 1, 1, -1.f, { nested("reward_ominous_common", 2), nested("reward_ominous_rare", 8) } },
                { 1, 3, -1.f, { nested("reward_ominous_common", 1) } },
                { 1, 1, 0.75f, { nested("reward_ominous_unique", 1) } },
            };
            static const Table ominousCommon = {
                { 1, 1, -1.f,
                  {
                      item("minecraft:emerald", 5, { count(4, 10) }),
                      item("minecraft:wind_charge", 4, { count(8, 12) }),
                      item("minecraft:arrow", 3, { count(4, 12), potion("strong_slowness") }),
                      item("minecraft:diamond", 2, { count(2, 3) }),
                      item("minecraft:ominous_bottle", 1, { count(1, 1), amplifier(2, 4) }),
                  } },
            };
            static const Table ominousRare = {
                { 1, 1, -1.f,
                  {
                      item("minecraft:emerald_block", 5),
                      item("minecraft:iron_block", 4),
                      item("minecraft:crossbow", 4, { enchantLevels() }),
                      item("minecraft:golden_apple", 3),
                      item("minecraft:diamond_axe", 3, { enchantLevels() }),
                      item("minecraft:diamond_chestplate", 3, { enchantLevels() }),
                      item("minecraft:book", 2, { enchantBook() }),
                      item("minecraft:book", 2, { enchantBook() }),
                      item("minecraft:book", 2, { enchantBook() }),
                      item("minecraft:diamond_block", 1),
                  } },
            };
            static const Table ominousUnique = {
                { 1, 1, -1.f,
                  {
                      item("minecraft:enchanted_golden_apple", 3),
                      item("minecraft:flow_armor_trim_smithing_template", 3),
                      item("minecraft:flow_banner_pattern", 2),
                      item("minecraft:music_disc_creator", 1),
                      item("minecraft:heavy_core", 1),
                  } },
            };
            if (name == "reward") return &reward;
            if (name == "reward_common") return &rewardCommon;
            if (name == "reward_rare") return &rewardRare;
            if (name == "reward_unique") return &rewardUnique;
            if (name == "reward_ominous") return &ominous;
            if (name == "reward_ominous_common") return &ominousCommon;
            if (name == "reward_ominous_rare") return &ominousRare;
            if (name == "reward_ominous_unique") return &ominousUnique;
            return nullptr;
        }

        struct Runner {
            Rng rng;
            Result out;
            bool stopped = false;

            explicit Runner(uint32_t seed) : rng(seed) {}

            // floor(min) + nextInt(floor(max) - floor(min) + 1): one draw even when min == max.
            int rangeValue(float lo, float hi) {
                const int a = static_cast<int>(std::floor(lo));
                const int b = static_cast<int>(std::floor(hi));
                return a <= b ? a + rng.nextInt(b - a + 1) : a;
            }

            void applyItem(Entry const& e) {
                Stack s;
                s.item = e.name;
                for (auto const& fn : e.fns) {
                    switch (fn.kind) {
                    case Fn::SetCount: s.count = rangeValue(fn.min, fn.max); break;
                    case Fn::Amplifier: s.variant = std::to_string(rangeValue(fn.min, fn.max)); break;
                    case Fn::SetPotion: s.variant = fn.text; break;
                    case Fn::EnchantLevels:
                        // Takes exactly ONE draw: found from one real vault (a normal vault's diamond axe), where the
                        // three items rolled after it only match after skipping one. Not yet confirmed on more vaults.
                        (void)rng.raw();
                        s.details = true;
                        break;
                    case Fn::EnchantBook:
                        // Takes exactly two draws (which enchantment, which level; found from a real vault: the loot that
                        // follows only matches after two). Which enchantment it picks isn't known yet.
                        (void)rng.raw();
                        (void)rng.raw();
                        s.details = true;
                        break;
                    case Fn::Undecoded:
                        s.details = true;
                        out.items.push_back(std::move(s));
                        stopped = true; // what is rolled after this can't be known
                        return;
                    }
                }
                if (s.count != 0) out.items.push_back(std::move(s));
            }

            void runTable(Table const& table) {
                for (auto const& pool : table) {
                    if (stopped) return;
                    runPool(pool);
                }
            }

            void runPool(Pool const& pool) {
                if (pool.chance >= 0.f && !(pool.chance > rng.nextFloat())) return;
                int rolls = pool.rollsMin;
                if (pool.rollsMin <= pool.rollsMax) rolls += rng.nextInt(pool.rollsMax - pool.rollsMin + 1);
                (void)rng.nextFloat(); // the bonus rolls are drawn even when there are none
                for (int i = 0; i < rolls && !stopped; ++i) {
                    int total = 0;
                    for (auto const& e : pool.entries) total += e.weight;
                    if (total <= 0) continue;
                    int n = rng.nextInt(total);
                    for (auto const& e : pool.entries) {
                        n -= e.weight;
                        if (n >= 0) continue;
                        if (e.table) {
                            if (auto const* t = findTable(e.name)) runTable(*t);
                            else stopped = true;
                        } else {
                            applyItem(e);
                        }
                        break;
                    }
                }
            }
        };
    } // namespace detail

    // What the vault at (x, y, z) ejects for the player with this unique actor id. `ominous`: the ominous table.
    inline Result predict(int64_t levelSeed, int64_t playerId, int x, int y, int z, bool ominous) {
        detail::Runner run(detail::mtSeed(levelSeed, playerId, x, y, z));
        run.runTable(*detail::findTable(ominous ? "reward_ominous" : "reward"));
        run.out.complete = !run.stopped;
        return std::move(run.out);
    }
} // namespace vault_loot
