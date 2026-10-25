#pragma once
// Whether another player's shield is working or disabled, as far as the client can tell.
//
// The server (Blurry's Java PvP addon) keeps the shield state to itself: a shield you hit with an axe while they
// block is "broken" for a few seconds (5 by default), and nothing in the game tells the client. What the client can
// see is the hit itself: you swing an axe at a player who is holding a shield up, and their health does not drop
// (the server cancels a blocked hit). So a swing counts as disabling their shield when
//   - you swung an axe at them,
//   - they hold a shield and had it raised (using an item), and
//   - their health (with golden hearts) did not go down within a moment afterwards.
// The shield then counts as disabled for the cooldown, counted from the swing. It only knows about shields YOU
// disabled; a shield broken by someone else looks ready. A swing too weak to do damage (the server cancels spam
// swings too) can look like a block, so it can show a disabled shield that wasn't.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace ShieldStatus {
    using Clock = std::chrono::steady_clock;

    inline bool isShield(std::string const& id) { return id.find("shield") != std::string::npos; }
    inline bool isAxe(std::string const& id) { return id.find("_axe") != std::string::npos; }

    enum class State { Ready, Disabled };

    class Tracker {
    public:
        // You just swung at this player. axe: you hold an axe. raised: they hold a shield and are using it.
        // health: their health with golden hearts right now, < 0 if unknown.
        void noteSwing(uint64_t id, bool axe, bool raised, float health, Clock::time_point now) {
            if (!axe || !raised || health < 0.f) return;
            auto& entry = entries[id];
            entry.pending = true;
            entry.healthAtSwing = health;
            entry.swungAt = now;
        }

        // Call every frame for the player on the HUD. health: their health with golden hearts, < 0 if unknown.
        void update(uint64_t id, float health, float cooldownSeconds, Clock::time_point now) {
            auto it = entries.find(id);
            if (it == entries.end()) return;
            auto& entry = it->second;

            if (health >= 0.f && health <= 0.05f) { // Dead: the next life starts with a working shield.
                entries.erase(it);
                return;
            }
            if (!entry.pending) return;

            if (health >= 0.f && health < entry.healthAtSwing - 0.05f) {
                entry.pending = false; // The hit hurt them, so it was not blocked.
            } else if (now - entry.swungAt >= std::chrono::milliseconds(300)) {
                entry.pending = false; // Nothing happened to their health: blocked, and the shield is down.
                entry.disabledUntil = entry.swungAt + std::chrono::milliseconds(static_cast<int>(cooldownSeconds * 1000.f));
            }
        }

        State state(uint64_t id, Clock::time_point now) const {
            auto it = entries.find(id);
            return it != entries.end() && now < it->second.disabledUntil ? State::Disabled : State::Ready;
        }

        float secondsLeft(uint64_t id, Clock::time_point now) const {
            auto it = entries.find(id);
            if (it == entries.end() || now >= it->second.disabledUntil) return 0.f;
            return std::chrono::duration<float>(it->second.disabledUntil - now).count();
        }

        void clear() { entries.clear(); }

    private:
        struct Entry {
            bool pending = false;
            float healthAtSwing = -1.f;
            Clock::time_point swungAt {};
            Clock::time_point disabledUntil {};
        };
        std::unordered_map<uint64_t, Entry> entries;
    };
}
