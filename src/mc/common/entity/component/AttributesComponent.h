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
