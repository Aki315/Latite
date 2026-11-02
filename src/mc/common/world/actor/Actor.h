#pragma once
#include "mc/Util.h"
#include "mc/common/entity/EntityContext.h"
#include "mc/common/util/MolangVariableMap.h"
#include <memory>
#include <atomic>
#include <typeinfo>
#include "util/SafeCall.h"
#include "mc/common/entity/RawRegistry.h"
#include <cstring>
#include "util/Logger.h"

#include "mc/common/entity/component/AABBShapeComponent.h"
#include "mc/common/entity/component/ActorDataFlagComponent.h"
#include "mc/common/entity/component/ActorRotationComponent.h"
#include "mc/common/entity/component/StateVectorComponent.h"

namespace SDK {
    class Actor {
    public:
        Actor() = delete;

        CLASS_FIELD(EntityContext, entityContext, 0x8);
        CLASS_FIELD(int32_t, ticksExisted, 0x198);
        CLASS_FIELD(int32_t, invulnerableTime, 0x19C); // Mob::hurtEffects
        // @dump-wbds Actor::getDimensionConst, 9, -8
        CLASS_FIELD(std::shared_ptr<class Dimension>, dimension, 0x1C8);
        CLASS_FIELD(StateVectorComponent*, stateVector, 0x240); // +0x28 in 1.26.60
        CLASS_FIELD(AABBShapeComponent*, aabbShape, 0x248);
        CLASS_FIELD(ActorRotationComponent*, actorRotation, 0x250);
        // @dump-wbds Actor::getMolangVariables, 3
        CLASS_FIELD(MolangVariableMap, molangVariableMap, 0x398);

        ActorDataFlagComponent* getActorDataFlagsComponent();

        bool getStatusFlag(int flag);
        void setStatusFlag(int flag, bool value);

        AABB& getBoundingBox();
        Vec2& getRot();
        Vec3& getVelocity();
        Vec3& getPos();
        Vec3& getPosOld();

        int getCommandPermissionLevel();
        std::string const& getNameTag() const;
        void setNameTag(std::string* nametag);
        void setUIRendering(bool value);
        void setYHeadRotations(float current, float old);
        void setYBodyRotations(float current, float old);
        uint64_t getRuntimeID();
        uint32_t getEntityTypeID();
        std::string getEntityLocalizationKey();
        std::string getEntityTypeName();
        std::string getEntityNamespace();
        void const* getActorRendererId();
        void const* getActorRendererIdOverride();
        void swing();
        bool isPlayer();
        bool isItem();

        struct AttributesComponent* getAttributesComponent();
        class AttributeInstance* getAttribute(class Attribute& attribute);
        std::optional<float> getHealth();
        std::optional<float> getMaxHealth();
        std::optional<float> getHunger();
        std::optional<float> getSaturation();
        bool isInvisible();
        class ItemStack* getArmor(int armorSlot);

        // Walks the registry in memory (see RawRegistry.h) instead of using entt, whose container layout does not
        // match the game's build. A crash while reading returns nullptr.
        template<typename T>
        T* tryGetComponent() {
            uintptr_t found = 0;
            const uintptr_t registry = reinterpret_cast<uintptr_t>(&entityContext.enttRegistry);
            uint32_t entity = 0;
            std::memcpy(&entity, &entityContext.entity, sizeof(entity));
            if (!latite_safe::run([&] { found = rawregistry::findComponent(registry, entity, T::type_hash, sizeof(T)); })) {
                return nullptr;
            }
            return reinterpret_cast<T*>(found);
        }
    };
}
