// Tests for src/util/ShieldState.h (shield up / ready / broken). It has no game dependencies, so it builds anywhere:
//
//   g++ -std=c++20 -Wall -Wextra -Isrc tests/ShieldStateTest.cpp -o shield_state_test && ./shield_state_test
//
// These test the rules and the bookkeeping. They do NOT test the parts that need a running Minecraft: reading the
// sound packet, the status flags, or drawing.

#include "util/ShieldState.h"

#include <cstdio>

using namespace ShieldState;
using namespace std::chrono_literals;

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

static const Clock::time_point T0 = Clock::time_point(std::chrono::seconds(1000));

static void testSoundFilter() {
    CHECK(isShieldBreakSound("random.break", 1.2f));
    CHECK(isShieldBreakSound("minecraft:random.break", 1.2f));
    CHECK(isShieldBreakSound("random.break", 1.19f));
    // A shield that wears out plays the same sound at the normal pitch (and only to its owner): not a disable.
    CHECK(!isShieldBreakSound("random.break", 1.0f));
    CHECK(!isShieldBreakSound("item.shield.block", 1.2f));
    CHECK(!isShieldBreakSound("random.breakx", 1.2f));
    CHECK(!isShieldBreakSound("", 1.2f));
}

static void testAttribute() {
    std::vector<Candidate> players = { { 1, 10.f, 64.f, 20.f }, { 2, 13.f, 64.f, 20.f } };

    // The packet stores the position times 8.
    CHECK(attribute(players, 80, 512, 160) == std::optional<uint64_t>(1));
    CHECK(attribute(players, 104, 512, 160) == std::optional<uint64_t>(2));
    // Quantised to 1/8 of a block: a little off still matches.
    CHECK(attribute(players, 81, 513, 159) == std::optional<uint64_t>(1));

    // If the layout turns out to hold plain block positions, that reading is used.
    CHECK(attribute(players, 10, 64, 20) == std::optional<uint64_t>(1));
    CHECK(attribute(players, 13, 64, 20) == std::optional<uint64_t>(2));

    // Nobody close: no guess.
    CHECK(!attribute(players, 800, 512, 160).has_value());
    CHECK(!attribute({}, 80, 512, 160).has_value());

    // Two players about equally close: no guess either (a wrong red is worse than none).
    std::vector<Candidate> close = { { 1, 10.f, 64.f, 20.f }, { 2, 10.5f, 64.f, 20.f } };
    CHECK(!attribute(close, 82, 512, 160).has_value()); // 10.25: both within the tie margin
    // But clearly closer to one: fine.
    CHECK(attribute(close, 80, 512, 160) == std::optional<uint64_t>(1));
}

static void testTrackerBasics() {
    Tracker tr;
    // Someone who never held a shield has no entry and no state.
    tr.observe(1, false, false, T0);
    CHECK(tr.size() == 0 && tr.state(1, T0) == State::None);

    tr.observe(1, true, false, T0);
    CHECK(tr.state(1, T0) == State::Ready);
    tr.observe(1, true, true, T0 + 100ms);
    CHECK(tr.state(1, T0 + 100ms) == State::Blocking);
    tr.observe(1, true, false, T0 + 200ms);
    CHECK(tr.state(1, T0 + 200ms) == State::Ready);
    tr.observe(1, false, false, T0 + 300ms); // put it away
    CHECK(tr.state(1, T0 + 300ms) == State::None);
}

static void testBreakAndCooldown() {
    Tracker tr;
    tr.observe(1, true, false, T0);
    tr.observe(1, true, true, T0 + 100ms); // blocking
    tr.noteBroken(1, T0 + 200ms, 5.f);
    tr.observe(1, true, true, T0 + 210ms); // still holding right click through the break
    CHECK(tr.state(1, T0 + 210ms) == State::Broken);
    CHECK(tr.secondsLeft(1, T0 + 210ms) > 4.9f && tr.secondsLeft(1, T0 + 210ms) <= 5.f);

    // Cooldown over, still holding right click: the addon's shield is "down", so NOT green.
    tr.observe(1, true, true, T0 + 5300ms);
    CHECK(tr.state(1, T0 + 5300ms) == State::Ready);
    CHECK(tr.secondsLeft(1, T0 + 5300ms) == 0.f);

    // Release and press again: up.
    tr.observe(1, true, false, T0 + 5400ms);
    tr.observe(1, true, true, T0 + 5500ms);
    CHECK(tr.state(1, T0 + 5500ms) == State::Blocking);
}

static void testPressDuringCooldownDoesNotCount() {
    Tracker tr;
    tr.observe(1, true, true, T0); // already blocking
    tr.noteBroken(1, T0 + 100ms, 5.f);
    tr.observe(1, true, false, T0 + 500ms);  // lets go
    tr.observe(1, true, true, T0 + 1000ms);  // presses during the cooldown: the addon ignores it
    CHECK(tr.state(1, T0 + 1000ms) == State::Broken);
    tr.observe(1, true, true, T0 + 6000ms);  // cooldown over, held since the early press
    CHECK(tr.state(1, T0 + 6000ms) == State::Ready); // must press again
    tr.observe(1, true, false, T0 + 6100ms);
    tr.observe(1, true, true, T0 + 6200ms);
    CHECK(tr.state(1, T0 + 6200ms) == State::Blocking);
}

static void testShieldAwayWhileBroken() {
    Tracker tr;
    tr.observe(1, true, false, T0);
    tr.noteBroken(1, T0 + 100ms, 5.f);
    tr.observe(1, false, false, T0 + 200ms); // swapped to another item
    CHECK(tr.state(1, T0 + 200ms) == State::None);
    tr.observe(1, true, false, T0 + 300ms); // back: the cooldown is still running
    CHECK(tr.state(1, T0 + 300ms) == State::Broken);
}

static void testPlayersAreIndependent() {
    Tracker tr;
    tr.observe(1, true, true, T0);
    tr.observe(2, true, true, T0);
    tr.noteBroken(1, T0 + 50ms, 5.f);
    tr.observe(1, true, true, T0 + 60ms);
    tr.observe(2, true, true, T0 + 60ms);
    CHECK(tr.state(1, T0 + 60ms) == State::Broken);
    CHECK(tr.state(2, T0 + 60ms) == State::Blocking);
}

static void testBreakBeforeFirstSighting() {
    // The break sound can arrive in the same frame the shield is first seen.
    Tracker tr;
    tr.noteBroken(7, T0, 5.f);
    tr.observe(7, true, true, T0 + 10ms);
    CHECK(tr.state(7, T0 + 10ms) == State::Broken);
}

static void testCooldownLength() {
    Tracker tr;
    tr.observe(1, true, false, T0);
    tr.noteBroken(1, T0, 2.f);
    CHECK(tr.state(1, T0 + 1900ms) == State::Broken);
    CHECK(tr.state(1, T0 + 2100ms) == State::Ready);
}

static void testPrune() {
    Tracker tr;
    tr.observe(1, true, false, T0);
    tr.observe(2, true, false, T0 + 40s);
    tr.prune(T0 + 45s);
    CHECK(tr.size() == 1);
    CHECK(tr.state(1, T0 + 45s) == State::None);
    CHECK(tr.state(2, T0 + 45s) == State::Ready);
}

int main() {
    testSoundFilter();
    testAttribute();
    testTrackerBasics();
    testBreakAndCooldown();
    testPressDuringCooldownDoesNotCount();
    testShieldAwayWhileBroken();
    testPlayersAreIndependent();
    testBreakBeforeFirstSighting();
    testCooldownLength();
    testPrune();

    std::printf("%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
