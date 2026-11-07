#pragma once
// Reads what a vault block entity update contains, out of the printed NBT (the same text the .nbt command shows).
// Only standard C++ in here, so the logic can be tested without the game.
//
// What the vault stores (Bedrock "Vault" block entity, field names from the game's data):
//   config.loot_table, config.override_loot_table_to_display
//   data.display_item         the item spinning in the vault right now (picked at random from the table, NOT a reward)
//   data.items_to_eject       the rewards that were rolled for the key in use (only filled while it is being unlocked)
//   data.total_ejections_needed
// The printed format of the tag isn't known for certain, so this looks for the field names and reads the values next to
// them; the module's Debug Mode writes the raw text so a wrong guess shows up.
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

namespace vault_nbt {
    struct Enchant {
        int id = -1;
        int level = 1;
    };

    struct ItemInfo {
        std::string name; // "minecraft:diamond"
        int count = 1;
        std::vector<Enchant> enchants;
    };

    struct VaultData {
        bool isVault = false;
        std::string lootTable;
        std::string overrideLootTable;
        bool hasDisplay = false;
        ItemInfo display;
        std::vector<ItemInfo> toEject;
        bool hasEjectField = false; // The items_to_eject field was present (even if empty).
    };

    inline bool isWordChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

    // Finds `key` as a whole word (not part of a longer name such as override_loot_table_to_display).
    inline size_t findKey(std::string const& text, std::string const& key, size_t from = 0) {
        size_t at = from;
        while ((at = text.find(key, at)) != std::string::npos) {
            const bool startOk = at == 0 || !isWordChar(text[at - 1]);
            const size_t end = at + key.size();
            const bool endOk = end >= text.size() || !isWordChar(text[end]);
            if (startOk && endOk) return at;
            at += key.size();
        }
        return std::string::npos;
    }

    // Where the value after a key starts. The game prints a tag as lines like
    //     // Name
    //     TAG_String: minecraft:diamond
    // so this skips spaces, new lines, ':', '=' and quotes, and a "TAG_Type:" label if one comes next.
    inline size_t valueStart(std::string const& text, size_t from) {
        size_t i = from;
        auto skip = [&] {
            while (i < text.size() && (text[i] == ':' || text[i] == '=' || text[i] == ' ' || text[i] == '"' ||
                                       text[i] == '\t' || text[i] == '\n' || text[i] == '\r'))
                ++i;
        };
        skip();
        if (text.compare(i, 4, "TAG_") == 0) {
            while (i < text.size() && text[i] != ':' && text[i] != '\n') ++i;
            skip();
        }
        return i;
    }

    // The value after a key as an identifier like minecraft:diamond.
    inline std::string readIdentifier(std::string const& text, size_t from) {
        size_t i = valueStart(text, from);
        size_t end = i;
        while (end < text.size() && (isWordChar(text[end]) || text[end] == ':' || text[end] == '.' || text[end] == '/'))
            ++end;
        return text.substr(i, end - i);
    }

    // The number after a key, or `fallback` when what follows isn't a number.
    inline int readNumber(std::string const& text, size_t from, int fallback) {
        const size_t i = valueStart(text, from);
        if (i >= text.size() || !(std::isdigit(static_cast<unsigned char>(text[i])) || text[i] == '-')) return fallback;
        return std::atoi(text.c_str() + i);
    }

    // Items in a piece of text: Count comes before Name in a printed item (alphabetical), and the enchantments ("ench"
    // with id/lvl pairs) come after the Name and before the next item's Count.
    inline std::vector<ItemInfo> parseItems(std::string const& text) {
        std::vector<ItemInfo> items;
        size_t searchFrom = 0;
        int count = 1;
        while (true) {
            const size_t countAt = findKey(text, "Count", searchFrom);
            const size_t nameAt = findKey(text, "Name", searchFrom);
            if (nameAt == std::string::npos) break;
            if (countAt != std::string::npos && countAt < nameAt) {
                count = readNumber(text, countAt + 5, 1);
                searchFrom = countAt + 5;
                continue;
            }

            ItemInfo item;
            item.name = readIdentifier(text, nameAt + 4);
            item.count = count;
            count = 1;
            searchFrom = nameAt + 4;

            // Enchantments: between this Name and the next item's Count (or the end).
            const size_t regionEnd = [&] {
                const size_t next = findKey(text, "Count", searchFrom);
                return next == std::string::npos ? text.size() : next;
            }();
            const std::string region = text.substr(searchFrom, regionEnd - searchFrom);
            const size_t enchAt = findKey(region, "ench");
            if (enchAt != std::string::npos) {
                size_t pos = enchAt + 4;
                while (true) {
                    const size_t idAt = findKey(region, "id", pos);
                    if (idAt == std::string::npos) break;
                    Enchant e;
                    e.id = readNumber(region, idAt + 2, -1);
                    pos = idAt + 2;
                    const size_t lvlAt = findKey(region, "lvl", pos);
                    if (lvlAt != std::string::npos) {
                        // The level has to come before the next id, or it belongs to the next pair.
                        const size_t nextId = findKey(region, "id", pos);
                        if (nextId == std::string::npos || lvlAt < nextId) {
                            e.level = readNumber(region, lvlAt + 3, 1);
                            pos = lvlAt + 3;
                        }
                    }
                    if (e.id >= 0) item.enchants.push_back(e);
                }
            }
            if (!item.name.empty()) items.push_back(std::move(item));
        }
        return items;
    }

    // Splits the text at the field names, so each field's value is looked for in its own part only.
    inline VaultData parse(std::string const& text) {
        VaultData out;

        // The block entity id: id: "Vault".
        size_t idAt = 0;
        while ((idAt = findKey(text, "id", idAt)) != std::string::npos) {
            if (readIdentifier(text, idAt + 2) == "Vault") {
                out.isVault = true;
                break;
            }
            idAt += 2;
        }
        // The server's updates for a vault don't always carry the block entity id, so the vault's own fields count too.
        if (!out.isVault) {
            for (const char* key : { "connected_particle_range", "display_item", "items_to_eject",
                                     "total_ejections_needed", "override_loot_table_to_display", "activation_range" }) {
                if (findKey(text, key) != std::string::npos) {
                    out.isVault = true;
                    break;
                }
            }
        }
        if (!out.isVault) return out;

        static const char* const keys[] = { "display_item",
                                            "items_to_eject",
                                            "total_ejections_needed",
                                            "loot_table",
                                            "override_loot_table_to_display",
                                            "config",
                                            "shared_data",
                                            "server_data",
                                            "key_item",
                                            "activation_range",
                                            "deactivation_range",
                                            "connected_particles_range",
                                            "connected_players",
                                            "rewarded_players",
                                            "state_updating_resumes_at",
                                            "next_state",
                                            "vault_state" };
        std::vector<size_t> starts;
        for (const char* key : keys) {
            const size_t at = findKey(text, key);
            if (at != std::string::npos) starts.push_back(at);
        }
        auto sectionOf = [&](const char* key) -> std::string {
            const size_t at = findKey(text, key);
            if (at == std::string::npos) return {};
            size_t end = text.size();
            for (size_t s : starts) {
                if (s > at && s < end) end = s;
            }
            return text.substr(at + std::string(key).size(), end - at - std::string(key).size());
        };

        out.lootTable = readIdentifier(sectionOf("loot_table"), 0);
        out.overrideLootTable = readIdentifier(sectionOf("override_loot_table_to_display"), 0);

        const std::string display = sectionOf("display_item");
        if (!display.empty()) {
            auto items = parseItems(display);
            if (!items.empty()) {
                out.hasDisplay = true;
                out.display = std::move(items.front());
            }
        }
        out.hasEjectField = findKey(text, "items_to_eject") != std::string::npos;
        out.toEject = parseItems(sectionOf("items_to_eject"));
        return out;
    }

    inline bool isOminous(VaultData const& data) {
        auto has = [](std::string const& s) { return s.find("ominous") != std::string::npos; };
        return has(data.lootTable) || has(data.overrideLootTable);
    }

    // Bedrock's numeric enchantment ids (the ones up to 37 are certain; newer ones are shown as a number).
    inline std::string enchantName(int id) {
        static const char* const names[] = {
            "Protection", "Fire Protection", "Feather Falling", "Blast Protection", "Projectile Protection",
            "Thorns", "Respiration", "Depth Strider", "Aqua Affinity", "Sharpness", "Smite", "Bane of Arthropods",
            "Knockback", "Fire Aspect", "Looting", "Efficiency", "Silk Touch", "Unbreaking", "Fortune", "Power",
            "Punch", "Flame", "Infinity", "Luck of the Sea", "Lure", "Frost Walker", "Mending", "Curse of Binding",
            "Curse of Vanishing", "Impaling", "Riptide", "Loyalty", "Channeling", "Multishot", "Piercing",
            "Quick Charge", "Soul Speed", "Swift Sneak"
        };
        if (id >= 0 && id < static_cast<int>(sizeof(names) / sizeof(names[0]))) return names[id];
        return "Enchantment #" + std::to_string(id);
    }

    inline std::string roman(int n) {
        static const char* const numerals[] = { "", "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X" };
        return n >= 1 && n <= 10 ? numerals[n] : std::to_string(n);
    }

    // "minecraft:enchanted_golden_apple" -> "Enchanted Golden Apple". The smithing templates are named by what they are for
    // ("minecraft:bolt_armor_trim_smithing_template" -> "Bolt Armor Trim", "netherite_upgrade_smithing_template" -> "Netherite
    // Upgrade"), because the full names are far too long for a label.
    inline std::string prettyName(std::string id) {
        const size_t colon = id.find(':');
        if (colon != std::string::npos) id = id.substr(colon + 1);
        const std::string tail = "_smithing_template";
        if (id.size() > tail.size() && id.compare(id.size() - tail.size(), tail.size(), tail) == 0) id.erase(id.size() - tail.size());
        std::string out;
        bool up = true;
        for (char c : id) {
            if (c == '_') {
                out += ' ';
                up = true;
            } else {
                out += up ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
                up = false;
            }
        }
        return out;
    }
} // namespace vault_nbt
