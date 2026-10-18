#include "pch.h"
#include "TargetHUD.h"

#include "client/event/events/AttackEvent.h"
#include "client/misc/PlayerHeadCache.h"
#include "mc/common/client/game/ClientInstance.h"
#include "mc/common/entity/component/ActorEquipmentComponent.h"
#include "mc/common/entity/component/AttributesComponent.h"
#include "mc/common/world/actor/player/Inventory.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>

namespace {
    // Layout, in the same units the other HUD modules use (a drawn item is 48 wide at scale 1).
    constexpr float kPadding = 12.f;
    constexpr float kHeadSize = 54.f;
    constexpr float kHeadGap = 9.f;
    constexpr float kLineGap = 4.f;
    constexpr float kIconSize = 30.f;
    constexpr float kIconSpacing = 4.f;
    constexpr float kOffhandGap = 10.f;
    constexpr float kMinWidth = 200.f;
    constexpr float kAccentWidth = 3.f;

    // Vanilla-style heart, drawn from rectangles. O = outline, R = body, W = shine, . = empty.
    constexpr int kHeartCols = 9;
    constexpr int kHeartRows = 8;
    constexpr const char* kHeart[kHeartRows] = {
        ".OOO.OOO.", "OWRRORRRO", "OWRRRRRRO", "ORRRRRRRO", ".ORRRRRO.", "..ORRRO..", "...ORO...", "....O....",
    };
    constexpr float kHeartPx = 2.f;
    constexpr float kHeartPitchX = (kHeartCols - 1) * kHeartPx;
    constexpr float kHeartPitchY = (kHeartRows + 1) * kHeartPx;
    constexpr int kHeartsPerRow = 10;
    constexpr int kMaxHearts = 30; // Above this the health is shown as a number instead.

    struct Vitals {
        float health = -1.f;
        float maxHealth = 20.f;
        float absorption = 0.f;
    };

    // Reads health, max health and golden hearts straight from the attribute list.
    Vitals readVitals(SDK::Actor* actor) {
        Vitals vitals;
        auto* attrs = actor->tryGetComponent<SDK::AttributesComponent>();
        if (!attrs) return vitals;

        auto& map = attrs->baseAttributes;
        auto sane = [](SDK::AttributeInstance* inst) {
            return inst && inst->maxValue > 0.f && inst->maxValue <= 2048.f && inst->value >= 0.f &&
                   inst->value <= inst->maxValue + 1024.f;
        };

        const int healthId = SDK::Attributes::Health.mIDValue;
        SDK::AttributeInstance* health = map.getInstance(healthId);
        if (!health) health = map.getInstanceByStride(healthId);
        if (!health) health = map.getInstanceDense(healthId);
        if (!health) health = map.getInstanceLenient(healthId);
        if (sane(health)) {
            vitals.health = health->value;
            vitals.maxHealth = health->maxValue;
        }

        const int absorptionId = SDK::Attributes::Absorption.mIDValue;
        SDK::AttributeInstance* absorption = map.getInstance(absorptionId);
        if (!absorption) absorption = map.getInstanceByStride(absorptionId);
        if (sane(absorption)) vitals.absorption = absorption->value;
        return vitals;
    }

    int heartCount(float points) { return std::max(0, static_cast<int>(std::ceil(points / 2.f))); }
    int rowsFor(int hearts) { return (hearts + kHeartsPerRow - 1) / kHeartsPerRow; }
} // namespace

TargetHUD::TargetHUD()
    : HUDModule("TargetHUD", LocalizeString::get("client.hudmodule.targetHud.name"),
                LocalizeString::get("client.hudmodule.targetHud.desc"), HUD) {
    showPreview = false;

    style.addEntry(EnumEntry(StyleHearts, LocalizeString::get("client.hudmodule.targetHud.style.hearts.name"),
                             LocalizeString::get("client.hudmodule.targetHud.style.hearts.desc")));
    style.addEntry(EnumEntry(StyleNumber, LocalizeString::get("client.hudmodule.targetHud.style.number.name"),
                             LocalizeString::get("client.hudmodule.targetHud.style.number.desc")));
    addEnumSetting("style", LocalizeString::get("client.hudmodule.targetHud.style.name"),
                   LocalizeString::get("client.hudmodule.targetHud.style.desc"), style);

    addSliderSetting("duration", LocalizeString::get("client.hudmodule.targetHud.duration.name"),
                     LocalizeString::get("client.hudmodule.targetHud.duration.desc"), duration, FloatValue(1.f),
                     FloatValue(15.f), FloatValue(0.5f));
    addSetting("showHead", LocalizeString::get("client.hudmodule.targetHud.showHead.name"),
               LocalizeString::get("client.hudmodule.targetHud.showHead.desc"), showHead);
    addSetting("showArmor", LocalizeString::get("client.hudmodule.targetHud.showArmor.name"),
               LocalizeString::get("client.hudmodule.targetHud.showArmor.desc"), showArmor);
    addSetting("showOffhand", LocalizeString::get("client.hudmodule.targetHud.showOffhand.name"),
               LocalizeString::get("client.hudmodule.targetHud.showOffhand.desc"), showOffhand);
    addSetting("showAbsorption", LocalizeString::get("client.hudmodule.targetHud.showAbsorption.name"),
               LocalizeString::get("client.hudmodule.targetHud.showAbsorption.desc"), showAbsorption);
    addSliderSetting("textSize", LocalizeString::get("client.hudmodule.targetHud.textSize.name"),
                     LocalizeString::get("client.hudmodule.targetHud.textSize.desc"), textSize, FloatValue(16.f),
                     FloatValue(44.f), FloatValue(1.f));
    addSetting("textColor", LocalizeString::get("client.hudmodule.targetHud.textColor.name"),
               LocalizeString::get("client.hudmodule.targetHud.textColor.desc"), textColor);
    addSetting("backgroundColor", LocalizeString::get("client.hudmodule.targetHud.backgroundColor.name"),
               LocalizeString::get("client.hudmodule.targetHud.backgroundColor.desc"), backgroundColor);
    addSetting("accentColor", LocalizeString::get("client.hudmodule.targetHud.accentColor.name"),
               LocalizeString::get("client.hudmodule.targetHud.accentColor.desc"), accentColor);

    rect = { 0.f, 0.f, kMinWidth, 96.f };

    listen<AttackEvent>((EventListenerFunc)&TargetHUD::onAttack);
}

void TargetHUD::afterLoadConfig() {
    bool hasStoredPosition = false;
    if (auto stored = std::get_if<Vec2Value>(&storedPos)) {
        hasStoredPosition = stored->x != 0.f || stored->y != 0.f;
    }

    HUDModule::afterLoadConfig();
    if (hasStoredPosition) return;

    // First start: just below and to the right of the crosshair.
    auto clientInstance = SDK::ClientInstance::get();
    if (!clientInstance || !clientInstance->getGuiData()) return;

    Vec2 screenSize = clientInstance->getGuiData()->screenSize;
    setPos({ screenSize.x * 0.5f + 40.f, screenSize.y * 0.5f + 40.f });
    storePos(screenSize);
}

void TargetHUD::onAttack(Event& evG) {
    auto& ev = reinterpret_cast<AttackEvent&>(evG);
    auto* actor = ev.getActor();
    if (!actor || !actor->isPlayer()) return;

    targetId = actor->getRuntimeID();
    lastHit = std::chrono::steady_clock::now();
    hasTarget = true;
}

SDK::Actor* TargetHUD::findTarget() const {
    auto clientInstance = SDK::ClientInstance::get();
    if (!clientInstance || !clientInstance->minecraft) return nullptr;

    auto level = clientInstance->minecraft->getLevel();
    if (!level) return nullptr;

    for (auto* actor : level->getRuntimeActorList()) {
        if (actor && actor->getRuntimeID() == targetId) return actor;
    }
    return nullptr;
}

std::string TargetHUD::getFaceTexturePath(SDK::Player* player) const {
    if (!player) return {};

    auto clientInstance = SDK::ClientInstance::get();
    if (!clientInstance || !clientInstance->minecraft) return {};

    if (auto playerSkin = player->getSkin()) {
        auto texturePath = PlayerHeadCache::getTexturePath(*playerSkin);
        if (!texturePath.empty()) return texturePath;
    }

    if (player->playerName.empty()) return {};

    auto level = clientInstance->minecraft->getLevel();
    if (!level) return {};

    auto playerList = level->getPlayerList();
    if (!playerList) return {};

    for (auto& entryPair : *playerList) {
        auto& entry = entryPair.second;
        if (entry.name != player->playerName) continue;
        return PlayerHeadCache::getTexturePath(entry.skin);
    }
    return {};
}

bool TargetHUD::gatherInfo(bool preview, Info& info) {
    // In the HUD editor show sample data, so there is always something to move around.
    if (preview) {
        info.name = L"Steve";
        info.health = 14.f;
        info.maxHealth = 20.f;
        info.absorption = 4.f;
        return true;
    }

    if (!hasTarget) return false;

    const auto elapsed = std::chrono::steady_clock::now() - lastHit;
    if (elapsed > std::chrono::duration<float>(std::get<FloatValue>(duration).value)) {
        hasTarget = false;
        return false;
    }

    SDK::Actor* actor = findTarget();
    if (!actor || !actor->isPlayer()) return false;

    auto* player = static_cast<SDK::Player*>(actor);
    info.name = player->playerName.empty() ? L"Player" : util::StrToWStr(player->playerName);
    info.faceTexturePath = getFaceTexturePath(player);

    const Vitals vitals = readVitals(actor);
    info.health = vitals.health;
    info.maxHealth = vitals.maxHealth;
    info.absorption = std::get<BoolValue>(showAbsorption) ? vitals.absorption : 0.f;

    // Actor::getArmor() dereferences the equipment component internally, so guard it first.
    if (auto* equipment = actor->tryGetComponent<SDK::ActorEquipmentComponent>()) {
        if (equipment->armorContainer) {
            for (int slot = 0; slot < 4; ++slot) {
                auto* stack = actor->getArmor(slot);
                if (stack && stack->getItem()) info.armor[slot] = stack;
            }
        }
        if (equipment->handContainer) {
            // The hand container holds the main hand in slot 0 and the offhand in slot 1.
            auto* stack = equipment->handContainer->getItem(1);
            if (stack && stack->getItem()) info.offhand = stack;
        }
    }
    return true;
}

// Draws rows of hearts with their top-left corner at (x, y). Golden hearts use a gold palette.
void TargetHUD::drawHeartRows(DrawUtil& dc, float x, float y, int hearts, int full, bool half, bool gold) const {
    const d2d::Color outline = d2d::Color::RGB(25, 8, 10);
    const d2d::Color empty = d2d::Color::RGB(70, 70, 70);
    const d2d::Color filled = gold ? d2d::Color::RGB(247, 190, 25) : d2d::Color::RGB(226, 55, 65);
    const d2d::Color shine = gold ? d2d::Color::RGB(255, 240, 160) : d2d::Color::RGB(255, 170, 175);

    for (int i = 0; i < hearts; ++i) {
        const float left = std::round(x + (i % kHeartsPerRow) * kHeartPitchX);
        const float top = std::round(y + (i / kHeartsPerRow) * kHeartPitchY);
        const bool isFull = i < full;
        const bool isHalf = half && i == full;
        // A half heart is coloured on its left side only.
        auto litAt = [&](int c) { return isFull || (isHalf && c < kHeartCols / 2); };

        for (int r = 0; r < kHeartRows; ++r) {
            int c = 0;
            while (c < kHeartCols) {
                const char ch = kHeart[r][c];
                if (ch == '.') {
                    ++c;
                    continue;
                }

                // Merge neighbouring pixels of the same kind into one rectangle.
                const bool lit = litAt(c);
                int end = c + 1;
                while (end < kHeartCols && kHeart[r][end] == ch && litAt(end) == lit) ++end;

                const d2d::Color color = ch == 'O' ? outline : (ch == 'R' ? (lit ? filled : empty) : (lit ? shine : empty));
                dc.fillRectangle({ left + c * kHeartPx, top + r * kHeartPx, left + end * kHeartPx,
                                   top + (r + 1) * kHeartPx },
                                 color);
                c = end;
            }
        }
    }
}

void TargetHUD::render(DrawUtil& dc, bool isDefault, bool inEditor) {
    if (!dc.isMinecraft()) return;

    Info info;
    if (!gatherInfo(isDefault || inEditor, info)) return;

    auto& mc = static_cast<MCDrawUtil&>(dc);
    const bool cacheText = !isDefault && !inEditor;
    const float nameSize = std::get<FloatValue>(textSize).value;
    const bool head = std::get<BoolValue>(showHead);

    // ---- Measure everything first ----
    const Vec2 nameBox = dc.getTextSize(info.name, Renderer::FontSelection::PrimaryRegular, nameSize, true, cacheText);

    const int healthHearts = heartCount(info.maxHealth);
    const bool hasHealth = info.health >= 0.f;
    const bool asHearts = hasHealth && style.getSelectedKey() == StyleHearts && healthHearts <= kMaxHearts;
    const int fullHearts = std::clamp(static_cast<int>(std::ceil(info.health)), 0, healthHearts * 2) / 2;
    const bool halfHeart = std::clamp(static_cast<int>(std::ceil(info.health)), 0, healthHearts * 2) % 2 != 0;
    const int goldPoints = std::clamp(static_cast<int>(std::ceil(info.absorption)), 0, kMaxHearts * 2);
    const int goldHearts = (goldPoints + 1) / 2;

    std::wstring numberText;
    Vec2 numberBox {};
    float healthW = 0.f;
    float healthH = 0.f;
    if (hasHealth) {
        if (asHearts) {
            const int widest = std::min(std::max(healthHearts, goldHearts), kHeartsPerRow);
            healthW = kHeartPitchX * widest + kHeartPx;
            healthH = kHeartPitchY * (rowsFor(healthHearts) + rowsFor(goldHearts));
        } else {
            numberText = std::format(L"{:.0f} / {:.0f}", std::ceil(info.health), std::ceil(info.maxHealth));
            if (goldPoints > 0) numberText += std::format(L"  +{}", goldPoints);
            numberBox = dc.getTextSize(numberText, Renderer::FontSelection::PrimaryRegular, nameSize * 0.85f, true,
                                       cacheText);
            healthW = numberBox.x;
            healthH = numberBox.y;
        }
    }

    const bool armorOn = std::get<BoolValue>(showArmor);
    const bool offhandOn = std::get<BoolValue>(showOffhand);
    const int slotCount = (armorOn ? 4 : 0) + (offhandOn ? 1 : 0);
    float itemsW = 0.f;
    if (slotCount > 0) {
        itemsW = slotCount * kIconSize + (slotCount - 1) * kIconSpacing;
        if (armorOn && offhandOn) itemsW += kOffhandGap - kIconSpacing;
    }
    const float itemsH = slotCount > 0 ? kIconSize : 0.f;

    float contentW = std::max({ nameBox.x, healthW, itemsW });
    float contentH = nameBox.y;
    if (healthH > 0.f) contentH += kLineGap + healthH;
    if (itemsH > 0.f) contentH += kLineGap + itemsH;

    const float headW = head ? kHeadSize + kHeadGap : 0.f;
    const float width = std::max(kMinWidth, kPadding * 2.f + kAccentWidth + headW + contentW);
    const float height = kPadding * 2.f + std::max(head ? kHeadSize : 0.f, contentH);

    rect.right = rect.left + width;
    rect.bottom = rect.top + height;

    const float headLeft = kPadding + kAccentWidth;
    const float headTop = (height - kHeadSize) * 0.5f;
    const float contentLeft = headLeft + headW;
    const float contentTop = (height - contentH) * 0.5f;

    float y = contentTop;
    const float nameY = y;
    y += nameBox.y + kLineGap;
    const float healthY = y;
    if (healthH > 0.f) y += healthH + kLineGap;
    const float itemsY = y;

    // Item slot positions: armor first, then a slightly larger gap, then the offhand.
    struct Slot {
        float x;
        SDK::ItemStack* stack;
    };
    std::vector<Slot> slots;
    {
        float x = contentLeft;
        if (armorOn) {
            for (int i = 0; i < 4; ++i) {
                slots.push_back({ x, info.armor[i] });
                x += kIconSize + kIconSpacing;
            }
        }
        if (offhandOn) {
            if (armorOn) x += kOffhandGap - kIconSpacing;
            slots.push_back({ x, info.offhand });
        }
    }

    const d2d::Color bg(std::get<ColorValue>(backgroundColor).getMainColor());
    const d2d::Color accent(std::get<ColorValue>(accentColor).getMainColor());
    const d2d::Color text(std::get<ColorValue>(textColor).getMainColor());

    // ---- Shapes: panel, head backdrop, hearts, empty item slots ----
    // Deferred UI meshes must be resolved before the item renderer changes render state.
    mc.flush(false);
    mc.setImmediate(true);

    mc.fillRectangle({ 0.f, 0.f, width, height }, bg);
    mc.fillRectangle({ 0.f, 0.f, kAccentWidth, height }, accent);

    if (head) {
        mc.fillRectangle({ headLeft, headTop, headLeft + kHeadSize, headTop + kHeadSize },
                         d2d::Color::RGB(40, 40, 40, 255));
    }

    if (hasHealth && asHearts) {
        // Golden hearts sit in the rows above the red ones.
        const int goldRows = rowsFor(goldHearts);
        if (goldHearts > 0) {
            drawHeartRows(dc, contentLeft, healthY, goldHearts, goldPoints / 2, goldPoints % 2 != 0, true);
        }
        drawHeartRows(dc, contentLeft, healthY + goldRows * kHeartPitchY, healthHearts, fullHearts, halfHeart, false);
    }

    for (auto const& slot : slots) {
        mc.fillRectangle({ slot.x, itemsY, slot.x + kIconSize, itemsY + kIconSize }, d2d::Color(0.f, 0.f, 0.f, 0.35f));
    }

    mc.setImmediate(false);

    // ---- Head ----
    if (head && !info.faceTexturePath.empty() && mc.renderCtx) {
        SDK::TexturePtr texture {};
        const auto externalTexture = std::filesystem::path(info.faceTexturePath).is_absolute();
        const auto textureFileSystem =
            externalTexture ? SDK::ResourceFileSystem::Raw : SDK::ResourceFileSystem::UserPackage;
        mc.renderCtx->getTexture(&texture, SDK::ResourceLocation(info.faceTexturePath, textureFileSystem),
                                 externalTexture);
        if (texture.textureData) {
            d2d::Rect headRect { headLeft, headTop, headLeft + kHeadSize, headTop + kHeadSize };
            mc.drawImage(texture, headRect.getPos(), headRect.getSize(), { 0.f, 0.f }, { 1.f, 1.f },
                         d2d::Colors::WHITE);
        }
    }

    // ---- Text ----
    dc.drawText({ contentLeft, nameY, contentLeft + contentW + 4.f, nameY + nameBox.y + 1.f }, info.name, text,
                Renderer::FontSelection::PrimaryRegular, nameSize, DWRITE_TEXT_ALIGNMENT_LEADING,
                DWRITE_PARAGRAPH_ALIGNMENT_NEAR, cacheText);

    if (hasHealth && !asHearts) {
        dc.drawText({ contentLeft, healthY, contentLeft + contentW + 4.f, healthY + numberBox.y + 1.f }, numberText,
                    text, Renderer::FontSelection::PrimaryRegular, nameSize * 0.85f, DWRITE_TEXT_ALIGNMENT_LEADING,
                    DWRITE_PARAGRAPH_ALIGNMENT_NEAR, cacheText);
    }

    // ---- Armor and offhand ----
    mc.flush(false);
    for (auto const& slot : slots) {
        if (!slot.stack || !slot.stack->getItem()) continue;

        mc.drawItem(slot.stack, { slot.x, itemsY }, kIconSize / 48.f, 1.f);
        if (slot.stack->itemCount > 1) {
            dc.drawText({ slot.x, itemsY, slot.x + kIconSize, itemsY + kIconSize },
                        std::to_wstring(slot.stack->itemCount), d2d::Colors::WHITE,
                        Renderer::FontSelection::PrimaryRegular, 18.f, DWRITE_TEXT_ALIGNMENT_TRAILING,
                        DWRITE_PARAGRAPH_ALIGNMENT_FAR);
        }
    }

    dc.flush();
}
