#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace SDK {
    struct EffectData {
        int id;
        int duration; // ticks left, <= 0 means infinite / no counter
        int amplifier; // 0 = level I
    };

    // The game keeps the effects in a contiguous block of MobEffectInstance objects. The size of one
    // MobEffectInstance changes between game versions, and on newer versions the block is a table that also
    // holds EMPTY slots (id == 0, i.e. the invalid/default effect) next to the active ones. So instead of
    // declaring the struct we read the raw begin/end pointers, work out the element size, and skip empty slots.
    //
    // type_hash is FNV-1a 32 of "MobEffectsComponent", the same way every other component hash in this folder
    // is derived from its name.
    struct MobEffectsComponent : IEntityComponent {
        static constexpr uint32_t type_hash = 0xE6A1B550;

        char* first;
        char* last;
        char* end;

        struct Layout {
            size_t stride = 0;
            size_t ampOffset = 0;
            size_t active = 0; // how many slots look like a real effect
        };

        [[nodiscard]] size_t byteSize() const {
            if (!first || last <= first) return 0;
            return static_cast<size_t>(last - first);
        }

        // An empty slot has id == 0 and no duration/amplifier set in the header part of the element.
        [[nodiscard]] static bool isEmptySlot(const char* p) {
            static constexpr char zeros[0x18] = {};
            return std::memcmp(p, zeros, sizeof(zeros)) == 0;
        }

        [[nodiscard]] static bool plausible(const char* p, size_t ampOffset) {
            int id, duration, amp;
            std::memcpy(&id, p, 4);
            std::memcpy(&duration, p + 4, 4);
            std::memcpy(&amp, p + ampOffset, 4);
            return id >= 1 && id <= 64 && duration >= -1 && duration < 1000000000 && amp >= 0 && amp <= 255;
        }

        // Every slot has to be either empty or a plausible effect. A wrong stride lands in the middle of other
        // fields, which almost never looks like either, so it gets rejected. Returns active == 0 when the layout
        // is unknown OR when there are simply no active effects.
        [[nodiscard]] Layout detectLayout() const {
            Layout best;
            const size_t bytes = byteSize();
            if (bytes == 0 || bytes > 64 * 0x200) return best;

            static constexpr size_t strides[] = { 0x88, 0x80, 0x90, 0x98, 0xA0, 0xA8, 0xB0, 0x78,
                                                  0x70, 0x68, 0x60, 0xB8, 0xC0 };

            for (size_t stride : strides) {
                if (bytes % stride != 0) continue;
                // Both offsets are tried. When both look valid (e.g. the other one happens to hold a 0) the
                // first one wins, so list the one the older versions used for this stride first.
                const size_t preferred = stride == 0x80 ? 0x14 : 0x20;
                const size_t ampOffsets[] = { preferred, preferred == 0x14 ? size_t(0x20) : size_t(0x14) };
                for (size_t ampOff : ampOffsets) {
                    size_t active = 0;
                    bool ok = true;
                    for (size_t off = 0; off < bytes && ok; off += stride) {
                        const char* p = first + off;
                        if (isEmptySlot(p)) continue;
                        if (plausible(p, ampOff)) ++active;
                        else ok = false;
                    }
                    if (ok && active > best.active) best = { stride, ampOff, active };
                }
            }
            return best;
        }

        [[nodiscard]] size_t detectStride() const { return detectLayout().stride; }

        [[nodiscard]] std::vector<EffectData> read() const {
            std::vector<EffectData> result;
            const Layout layout = detectLayout();
            if (!layout.stride || !layout.active) return result;

            for (size_t off = 0; off < byteSize(); off += layout.stride) {
                const char* p = first + off;
                if (isEmptySlot(p)) continue;
                EffectData e;
                std::memcpy(&e.id, p, 4);
                std::memcpy(&e.duration, p + 4, 4);
                std::memcpy(&e.amplifier, p + layout.ampOffset, 4);
                result.push_back(e);
            }
            return result;
        }

        // Debug: the n-th 32-bit value of the whole block.
        [[nodiscard]] int rawInt(size_t n) const {
            if (byteSize() < (n + 1) * 4) return 0;
            int v;
            std::memcpy(&v, first + n * 4, 4);
            return v;
        }

        // Debug: byte offsets (4-byte aligned) that look like "effect id, then a positive duration". When you hold
        // an effect these point at its slot, and the gap between two hits is the element size.
        [[nodiscard]] std::vector<size_t> findCandidates(size_t maxHits) const {
            std::vector<size_t> hits;
            const size_t bytes = byteSize();
            for (size_t off = 0; off + 8 <= bytes && hits.size() < maxHits; off += 4) {
                int id, duration;
                std::memcpy(&id, first + off, 4);
                std::memcpy(&duration, first + off + 4, 4);
                if (id >= 1 && id <= 64 && duration >= 1 && duration < 100000000) hits.push_back(off);
            }
            return hits;
        }
    };
}
