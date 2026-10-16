#pragma once
#include <algorithm>
#include <vector>
#include <unordered_map>
#include <mc/common/world/Attribute.h>
#include <optional>
#include <stdexcept>

namespace SDK {
    class BaseAttributeMap {
        std::vector<uint32_t> ids;
        std::vector<AttributeInstance> instances;
        char pad[0x20];

        struct AttributeResult { // Made up
            std::vector<uint32_t>::iterator id;
            std::vector<AttributeInstance>::iterator instance;
        };

    public:
        AttributeInstance* getInstance(unsigned int id) {
            if (this->ids.size() != this->instances.size()) return nullptr;

            for (size_t i = 0; i < this->ids.size(); i++) {
                if (this->ids[i] == id) return &this->instances[i];
            }

            return nullptr;
        }

        // Diagnostics / lenient lookup (used by HealthIndicator).
        // getInstance() above gives up entirely when the two vectors differ in size; these don't.
        [[nodiscard]] size_t idCount() const { return ids.size(); }
        [[nodiscard]] size_t instanceCount() const { return instances.size(); }
        [[nodiscard]] uint32_t idAt(size_t i) const { return i < ids.size() ? ids[i] : 0xFFFFFFFFu; }

        // For maps where `instances` has entries that `ids` doesn't list (e.g. ids = 1..5,7..18 but 18 instances).
        // If the instances form one contiguous run of ids (size == max - min + 1), the instance for an id sits at
        // index (id - min). Returns nullptr whenever that assumption doesn't hold.
        AttributeInstance* getInstanceDense(unsigned int id) {
            if (this->ids.empty() || this->ids.size() > 4096 || this->instances.empty()) return nullptr;
            auto [mn, mx] = std::minmax_element(this->ids.begin(), this->ids.end());
            size_t span = static_cast<size_t>(*mx - *mn) + 1;
            if (span != this->instances.size()) return nullptr;
            if (id < *mn || id > *mx) return nullptr;
            return &this->instances[id - *mn];
        }

        // Stride-aware lookup. `instances` is a std::vector<AttributeInstance>, so its reported size is
        // (last - first) / sizeof(AttributeInstance). If the game's real AttributeInstance is bigger than our
        // `pad[0x80]` (e.g. 0x88), the reported count is wrong (17 real entries -> 18) and every index lands on
        // the wrong memory. Here we read the raw begin/end pointers (MSVC layout: first, last, end), derive the
        // real stride from ids.size(), and index with that instead.
        [[nodiscard]] size_t realStride() const {
            auto* raw = reinterpret_cast<char* const*>(&instances);
            char* first = raw[0];
            char* last = raw[1];
            if (!first || last <= first || ids.empty()) return 0;
            size_t bytes = static_cast<size_t>(last - first);
            if (bytes % ids.size() != 0) return 0;
            size_t stride = bytes / ids.size();
            return (stride >= 0x80 && stride <= 0x200) ? stride : 0;
        }

        AttributeInstance* getInstanceByStride(unsigned int id) {
            size_t stride = realStride();
            if (!stride) return nullptr;
            char* first = reinterpret_cast<char* const*>(&instances)[0];
            for (size_t i = 0; i < ids.size(); i++) {
                if (ids[i] == id) return reinterpret_cast<AttributeInstance*>(first + i * stride);
            }
            return nullptr;
        }

        [[nodiscard]] uint32_t lastId() const { return ids.empty() ? 0xFFFFFFFFu : ids.back(); }

        AttributeInstance* getInstanceLenient(unsigned int id) {
            size_t n = std::min(this->ids.size(), this->instances.size());
            for (size_t i = 0; i < n; i++) {
                if (this->ids[i] == id) return &this->instances[i];
            }
            return nullptr;
        }
    };

    struct AttributesComponent : IEntityComponent {
        static constexpr uint32_t type_hash = 0xFD3B0613;

        BaseAttributeMap baseAttributes {};
    };
}
