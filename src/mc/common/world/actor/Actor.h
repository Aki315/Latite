#pragma once
#include "mc/Util.h"
#include "mc/common/entity/EntityContext.h"
#include "mc/common/util/MolangVariableMap.h"
#include <memory>
#include <atomic>
#include <typeinfo>
#include "util/SafeCall.h"
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
        CLASS_FIELD(StateVectorComponent*, stateVector, 0x218);
        CLASS_FIELD(AABBShapeComponent*, aabbShape, 0x220);
        CLASS_FIELD(ActorRotationComponent*, actorRotation, 0x228);
        // @dump-wbds Actor::getMolangVariables, 3
        CLASS_FIELD(MolangVariableMap, molangVariableMap, 0x370);

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

        // After a game update the registry layout can be wrong; a failed lookup returns nullptr, and after a few
        // failures component lookups are switched off instead of crashing the game.
        template<typename T>
        T* tryGetComponent() {
            static std::atomic<int> failures { 0 };
            if (failures.load() >= 3) return nullptr;
            T* out = nullptr;
            if (!latite_safe::run([&] { out = entityContext.enttRegistry.try_get<T>(entityContext.entity); })) {
                if (++failures >= 3) {
                    Logger::Warn("[Entity] component lookup for {} keeps failing (EntityContext layout changed?); "
                                 "switched off",
                                 typeid(T).name());
                }
                return nullptr;
            }
            return out;
        }
    };
}
