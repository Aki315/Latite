#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace SDK {
    struct EffectData {
        int id;
        int duration; // ticks left, <= 0 means infinite / no counter
        int amplifier; // 0 = level I
    };

    // The game keeps the active effects in a std::vector<MobEffectInstance>. The size of one MobEffectInstance
    // changes between game versions, so instead of declaring the struct we read the vector's raw begin/end
    // pointers and work out the element size by checking that every element looks like a real effect.
    //
    // type_hash is FNV-1a 32 of "MobEffectsComponent", the same way every other component hash in this folder
    // is derived from its name.
    struct MobEffectsComponent : IEntityComponent {
        static constexpr uint32_t type_hash = 0xE6A1B550;

        char* first;
        char* last;
        char* end;

        [[nodiscard]] size_t byteSize() const {
            if (!first || last <= first) return 0;
            return static_cast<size_t>(last - first);
        }

        // Where the amplifier lives inside one element depends on the layout of that game version.
        static size_t amplifierOffset(size_t stride) { return stride == 0x80 ? 0x14 : 0x20; }

        [[nodiscard]] bool plausible(const char* p, size_t stride) const {
            const int id = *reinterpret_cast<const int*>(p);
            const int duration = *reinterpret_cast<const int*>(p + 4);
            const int amp = *reinterpret_cast<const int*>(p + amplifierOffset(stride));
            return id >= 1 && id <= 64 && duration >= -1 && duration < 1000000000 && amp >= 0 && amp <= 255;
        }

        // Returns 0 when no element size makes every element look valid.
        [[nodiscard]] size_t detectStride() const {
            const size_t bytes = byteSize();
            if (bytes == 0 || bytes > 64 * 0x200) return 0;

            static constexpr size_t candidates[] = { 0x88, 0x80, 0x90, 0x98, 0xA0, 0xA8, 0xB0, 0x78, 0x70 };
            for (size_t stride : candidates) {
                if (bytes % stride != 0) continue;
                bool ok = true;
                for (size_t off = 0; off < bytes && ok; off += stride) {
                    ok = plausible(first + off, stride);
                }
                if (ok) return stride;
            }
            return 0;
        }

        [[nodiscard]] std::vector<EffectData> read() const {
            std::vector<EffectData> result;
            const size_t stride = detectStride();
            if (!stride) return result;

            for (size_t off = 0; off < byteSize(); off += stride) {
                const char* p = first + off;
                result.push_back({ *reinterpret_cast<const int*>(p), *reinterpret_cast<const int*>(p + 4),
                                   *reinterpret_cast<const int*>(p + amplifierOffset(stride)) });
            }
            return result;
        }

        // Debug: the n-th 32-bit value of the first element.
        [[nodiscard]] int rawInt(size_t n) const {
            if (byteSize() < (n + 1) * 4) return 0;
            return reinterpret_cast<const int*>(first)[n];
        }
    };
}
