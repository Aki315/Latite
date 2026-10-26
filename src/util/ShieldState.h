#pragma once
// Whether another player's shield is up, ready, or disabled, as far as the client can tell. No game code in here, so
// it is unit tested on its own (tests/ShieldStateTest.cpp). The Shield Indicator module feeds it.
//
// Blurry's Java PvP addon keeps a shield's state (up / down / broken) in server script memory and never sends it to
// clients. What does reach every nearby client:
//   * the "using item" status flag while someone holds right click on the shield (the addon's shield item has a very
//     long use duration), which is what "up" looks like from outside, and
//   * the sound the addon plays when an axe breaks a shield: "random.break" at pitch 1.2, at the player's position.
//     (The sound played when a shield simply wears out is "random.break" at the normal pitch and goes to its owner
//     only, so the pitch also tells the two apart.)
// So: green = up, red = from the break sound until the cooldown ends (5 s by default, the addon's
// custom_shield_cooldown).
//
// How the addon behaves, which this copies: while broken the shield can't go up. When the cooldown ends the shield is
// "down", NOT up, even if the player is still holding right click: it only goes up again on a fresh press. That is
// what `requirePress` is for.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ShieldState {
    using Clock = std::chrono::steady_clock;

    enum class State {
        None,     // Not holding a shield.
        Ready,    // Holding a shield, not raised.
        Blocking, // Raised (green).
        Broken,   // Disabled by an axe, cooling down (red).
    };

    constexpr float kBreakPitch = 1.2f;

    // The sound the addon plays when a shield breaks. Tolerates the "minecraft:" prefix some sound names carry.
    inline bool isShieldBreakSound(std::string_view name, float pitch) {
        constexpr std::string_view prefix = "minecraft:";
        if (name.substr(0, prefix.size()) == prefix) name.remove_prefix(prefix.size());
        return name == "random.break" && std::fabs(pitch - kBreakPitch) < 0.06f;
    }

    // A player who is holding a shield, where their feet are.
    struct Candidate {
        uint64_t id = 0;
        float x = 0.f, y = 0.f, z = 0.f;
    };

    // Which player a sound belongs to. The sound's position is the shield owner's position, but the packet stores it as
    // a block position in eighths of a block (the protocol's x8). Since that layout is not verified in a running game,
    // the position is tried as eighths first and as plain blocks second; the first reading that points at exactly one
    // shield holder wins. Nothing is returned when no one is close enough or when two players are about equally close
    // (a wrong red is worse than none).
    inline std::optional<uint64_t> attribute(std::vector<Candidate> const& players, int rawX, int rawY, int rawZ,
                                             float maxDistance = 1.5f, float tieMargin = 0.25f) {
        for (const float scale : { 1.f / 8.f, 1.f }) {
            const float sx = static_cast<float>(rawX) * scale;
            const float sy = static_cast<float>(rawY) * scale;
            const float sz = static_cast<float>(rawZ) * scale;

            float best = 1e30f, second = 1e30f;
            const Candidate* bestPlayer = nullptr;
            for (auto const& p : players) {
                const float d = std::sqrt((p.x - sx) * (p.x - sx) + (p.y - sy) * (p.y - sy) + (p.z - sz) * (p.z - sz));
                if (d < best) {
                    second = best;
                    best = d;
                    bestPlayer = &p;
                } else if (d < second) {
                    second = d;
                }
            }
            if (!bestPlayer || best > maxDistance) continue; // Not this reading.
            if (second - best < tieMargin) return std::nullopt; // Two players about as close: don't guess.
            return bestPlayer->id;
        }
        return std::nullopt;
    }

    // One player's shield. Call observe() every frame for each player you can see, noteBroken() when a break sound is
    // attributed to them, state() to ask what to draw.
    class Tracker {
    public:
        // The shield of this player just got broken. cooldownSeconds is the server's cooldown.
        void noteBroken(uint64_t id, Clock::time_point now, float cooldownSeconds) {
            auto& e = entries[id];
            e.brokenUntil = now + std::chrono::milliseconds(static_cast<int>(std::max(0.f, cooldownSeconds) * 1000.f));
            e.requirePress = true; // It does not go up again by itself when the cooldown ends.
            e.lastSeen = now;
        }

        // holdsShield: a shield is in either hand. usingItem: the "using item" (or blocking) status flag is set.
        void observe(uint64_t id, bool holdsShield, bool usingItem, Clock::time_point now) {
            auto it = entries.find(id);
            if (it == entries.end()) {
                if (!holdsShield) return; // Nothing to remember about someone with no shield and no history.
                it = entries.emplace(id, Entry {}).first;
            }
            auto& e = it->second;
            e.lastSeen = now;
            // A fresh press (not raised -> raised) after the break is over lets the shield go up again.
            if (usingItem && !e.wasUsing && now >= e.brokenUntil) e.requirePress = false;
            e.wasUsing = usingItem;
            e.holds = holdsShield;
            e.using_ = usingItem;
        }

        State state(uint64_t id, Clock::time_point now) const {
            auto it = entries.find(id);
            if (it == entries.end() || !it->second.holds) return State::None;
            auto const& e = it->second;
            if (now < e.brokenUntil) return State::Broken;
            if (e.using_ && !e.requirePress) return State::Blocking;
            return State::Ready;
        }

        float secondsLeft(uint64_t id, Clock::time_point now) const {
            auto it = entries.find(id);
            if (it == entries.end() || now >= it->second.brokenUntil) return 0.f;
            return std::chrono::duration<float>(it->second.brokenUntil - now).count();
        }

        // Forget players that have not been seen for a while (left, far away).
        void prune(Clock::time_point now, std::chrono::seconds maxAge = std::chrono::seconds(30)) {
            for (auto it = entries.begin(); it != entries.end();) {
                it = now - it->second.lastSeen > maxAge ? entries.erase(it) : std::next(it);
            }
        }

        void clear() { entries.clear(); }
        size_t size() const { return entries.size(); }

    private:
        struct Entry {
            bool holds = false;
            bool using_ = false;
            bool wasUsing = false;
            bool requirePress = false;
            Clock::time_point brokenUntil {};
            Clock::time_point lastSeen {};
        };
        std::unordered_map<uint64_t, Entry> entries;
    };
}
