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
    constexpr float kOuterPad = 8.f;
    constexpr float kHeadBox = 54.f;     // The framed square around the head.
    constexpr float kHeadInset = 4.f;    // Space between that frame and the head image.
    constexpr float kDividerGap = 7.f;   // Space on each side of the thin divider lines.
    constexpr float kLine = 2.f;         // Thickness of frames and divider lines.
    constexpr float kRowGap = 5.f;
    constexpr float kSlotBox = 32.f;     // An armor / offhand slot.
    constexpr float kSlotIcon = 26.f;    // The item drawn inside a slot.
    constexpr float kSlotSpacing = 4.f;
    constexpr float kOffhandGap = 8.f;
    constexpr float kBarHeight = 16.f;
    constexpr float kBarMinWidth = 150.f;
    constexpr float kBarGap = 1.f;       // Space between bar segments.
    constexpr float kNumberGap = 8.f;    // Space between the bar and the health number.
    constexpr int kMaxSegments = 20;
    constexpr float kMinWidth = 260.f;
    constexpr float kChamfer = 3.f;

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

    d2d::Color withAlpha(d2d::Color const& c, float factor) { return d2d::Color(c.r, c.g, c.b, c.a * factor); }

    // A rectangle outline of thickness t, drawn inside r.
    void fillFrame(DrawUtil& dc, d2d::Rect const& r, float t, d2d::Color const& color) {
        dc.fillRectangle({ r.left, r.top, r.right, r.top + t }, color);
        dc.fillRectangle({ r.left, r.bottom - t, r.right, r.bottom }, color);
        dc.fillRectangle({ r.left, r.top + t, r.left + t, r.bottom - t }, color);
        dc.fillRectangle({ r.right - t, r.top + t, r.right, r.bottom - t }, color);
    }

    int heartCount(float points) { return std::max(0, static_cast<int>(std::ceil(points / 2.f))); }
    int rowsFor(int hearts) { return (hearts + kHeartsPerRow - 1) / kHeartsPerRow; }
} // namespace

TargetHUD::TargetHUD()
    : HUDModule("TargetHUD", LocalizeString::get("client.hudmodule.targetHud.name"),
                LocalizeString::get("client.hudmodule.targetHud.desc"), HUD) {
    showPreview = false;

    style.addEntry(EnumEntry(StyleBar, LocalizeString::get("client.hudmodule.targetHud.style.bar.name"),
                             LocalizeString::get("client.hudmodule.targetHud.style.bar.desc")));
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
    addSetting("healthColor", LocalizeString::get("client.hudmodule.targetHud.healthColor.name"),
               LocalizeString::get("client.hudmodule.targetHud.healthColor.desc"), healthColor);
    addSetting("backgroundColor", LocalizeString::get("client.hudmodule.targetHud.backgroundColor.name"),
               LocalizeString::get("client.hudmodule.targetHud.backgroundColor.desc"), backgroundColor);
    addSetting("accentColor", LocalizeString::get("client.hudmodule.targetHud.accentColor.name"),
               LocalizeString::get("client.hudmodule.targetHud.accentColor.desc"), accentColor);

    rect = { 0.f, 0.f, kMinWidth, 110.f };

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
    const float numberSize = nameSize * 0.8f;
    const bool head = std::get<BoolValue>(showHead);

    const d2d::Color bg(std::get<ColorValue>(backgroundColor).getMainColor());
    const d2d::Color accent(std::get<ColorValue>(accentColor).getMainColor());
    const d2d::Color accentDim = withAlpha(accent, 0.45f);
    const d2d::Color text(std::get<ColorValue>(textColor).getMainColor());
    const d2d::Color healthCol(std::get<ColorValue>(healthColor).getMainColor());
    const d2d::Color gold = d2d::Color::RGB(247, 190, 25);
    const d2d::Color slotFill = d2d::Color::RGB(8, 12, 22, 235);

    // ---- Measure everything first ----
    const Vec2 nameBox = dc.getTextSize(info.name, Renderer::FontSelection::PrimaryRegular, nameSize, true, cacheText);

    const int healthHearts = heartCount(info.maxHealth);
    const bool hasHealth = info.health >= 0.f;
    const int styleKey = style.getSelectedKey();
    // Too many hearts to draw: show the bar instead.
    const bool asHearts = hasHealth && styleKey == StyleHearts && healthHearts <= kMaxHearts;
    const bool asBar = hasHealth && (styleKey == StyleBar || (styleKey == StyleHearts && !asHearts));
    const bool asNumber = hasHealth && styleKey == StyleNumber;
    const int healthPoints = std::clamp(static_cast<int>(std::ceil(info.health)), 0, healthHearts * 2);
    const int fullHearts = healthPoints / 2;
    const bool halfHeart = healthPoints % 2 != 0;
    const int goldPoints = std::clamp(static_cast<int>(std::ceil(info.absorption)), 0, kMaxHearts * 2);
    const int goldHearts = (goldPoints + 1) / 2;

    // The health number is drawn in the health colour, with the golden hearts after it in gold.
    std::wstring healthText;
    std::wstring goldText;
    Vec2 healthTextBox {};
    Vec2 goldTextBox {};
    if (asBar || asNumber) {
        healthText = std::format(L"{:.0f}/{:.0f}", std::ceil(info.health), std::ceil(info.maxHealth));
        healthTextBox = dc.getTextSize(healthText, Renderer::FontSelection::PrimaryRegular, numberSize, true, cacheText);
        if (goldPoints > 0) {
            goldText = std::format(L" +{}", goldPoints);
            goldTextBox = dc.getTextSize(goldText, Renderer::FontSelection::PrimaryRegular, numberSize, true, cacheText);
        }
    }
    const float numberW = healthTextBox.x + goldTextBox.x;

    float healthW = 0.f;
    float healthH = 0.f;
    if (asBar) {
        healthW = kBarMinWidth + kNumberGap + numberW;
        healthH = kBarHeight;
    } else if (asHearts) {
        const int widest = std::min(std::max(healthHearts, goldHearts), kHeartsPerRow);
        healthW = kHeartPitchX * widest + kHeartPx;
        healthH = kHeartPitchY * (rowsFor(healthHearts) + rowsFor(goldHearts));
    } else if (asNumber) {
        healthW = numberW;
        healthH = healthTextBox.y;
    }

    const bool armorOn = std::get<BoolValue>(showArmor);
    const bool offhandOn = std::get<BoolValue>(showOffhand);
    const bool itemsOn = armorOn || offhandOn;
    float itemsW = 0.f;
    if (armorOn) itemsW += 4 * kSlotBox + 3 * kSlotSpacing;
    if (offhandOn) itemsW += (armorOn ? kOffhandGap * 2 + kLine : 0.f) + kSlotBox;

    float contentW = std::max({ nameBox.x, healthW, itemsW });
    float contentH = nameBox.y;
    if (healthH > 0.f) contentH += kRowGap + healthH;
    if (itemsOn) contentH += kRowGap + kLine + kRowGap + kSlotBox;

    const float leftPart = head ? kHeadBox + kDividerGap + kLine + kDividerGap : 0.f;
    const float contentLeft = kOuterPad + leftPart;
    const float width = std::max(kMinWidth, contentLeft + contentW + kOuterPad);
    const float height = kOuterPad * 2.f + std::max(head ? kHeadBox : 0.f, contentH);
    contentW = width - kOuterPad - contentLeft; // Let the content (and the bar) use the whole width.

    rect.right = rect.left + width;
    rect.bottom = rect.top + height;

    const float contentTop = (height - contentH) * 0.5f;
    const float nameY = contentTop;
    const float healthY = nameY + nameBox.y + kRowGap;
    const float afterHealthY = healthH > 0.f ? healthY + healthH : nameY + nameBox.y;
    const float dividerY = afterHealthY + kRowGap;
    const float itemsY = dividerY + kLine + kRowGap;

    const float headLeft = kOuterPad;
    const float headTop = (height - kHeadBox) * 0.5f;
    const float dividerX = headLeft + kHeadBox + kDividerGap;

    // Slot positions: armor from the left, the offhand against the right edge behind its own divider.
    struct Slot {
        float x;
        SDK::ItemStack* stack;
    };
    std::vector<Slot> slots;
    float offhandDividerX = -1.f;
    if (armorOn) {
        for (int i = 0; i < 4; ++i) {
            slots.push_back({ contentLeft + i * (kSlotBox + kSlotSpacing), info.armor[i] });
        }
    }
    if (offhandOn) {
        const float offhandX = contentLeft + contentW - kSlotBox;
        slots.push_back({ offhandX, info.offhand });
        if (armorOn) offhandDividerX = offhandX - kOffhandGap - kLine;
    }

    // ---- Shapes: panel, frames, dividers, health, slots ----
    // Deferred UI meshes must be resolved before the item renderer changes render state.
    mc.flush(false);
    mc.setImmediate(true);

    // Panel with the four outer corners cut off, and a thin border that follows the same shape.
    mc.fillRectangle({ kChamfer, 0.f, width - kChamfer, height }, bg);
    mc.fillRectangle({ 0.f, kChamfer, kChamfer, height - kChamfer }, bg);
    mc.fillRectangle({ width - kChamfer, kChamfer, width, height - kChamfer }, bg);
    mc.fillRectangle({ kChamfer, 0.f, width - kChamfer, 1.5f }, accentDim);
    mc.fillRectangle({ kChamfer, height - 1.5f, width - kChamfer, height }, accentDim);
    mc.fillRectangle({ 0.f, kChamfer, 1.5f, height - kChamfer }, accentDim);
    mc.fillRectangle({ width - 1.5f, kChamfer, width, height - kChamfer }, accentDim);

    if (head) {
        d2d::Rect headFrame { headLeft, headTop, headLeft + kHeadBox, headTop + kHeadBox };
        mc.fillRectangle(headFrame, slotFill);
        fillFrame(mc, headFrame, kLine, accent);
        mc.fillRectangle({ dividerX, kOuterPad, dividerX + kLine, height - kOuterPad }, accentDim);
    }

    if (asBar) {
        const float barW = std::max(40.f, contentW - kNumberGap - numberW);
        d2d::Rect bar { contentLeft, healthY, contentLeft + barW, healthY + kBarHeight };
        mc.fillRectangle(bar, d2d::Color::RGB(14, 8, 12, 240));
        fillFrame(mc, bar, kLine, accent);

        // Segments: one per health point (at most 20). The last one fills partly.
        const float innerLeft = bar.left + kLine + 2.f;
        const float innerRight = bar.right - kLine - 2.f;
        const float innerTop = bar.top + kLine + 2.f;
        const float innerBottom = bar.bottom - kLine - 2.f;
        const int segments = std::clamp(static_cast<int>(std::ceil(info.maxHealth)), 1, kMaxSegments);
        const float segW = ((innerRight - innerLeft) - (segments - 1) * kBarGap) / segments;
        const float filled = std::clamp(info.health / std::max(info.maxHealth, 1.f), 0.f, 1.f) * segments;
        for (int i = 0; i < segments; ++i) {
            const float x = innerLeft + i * (segW + kBarGap);
            mc.fillRectangle({ x, innerTop, x + segW, innerBottom }, withAlpha(healthCol, 0.2f));
            const float amount = std::clamp(filled - i, 0.f, 1.f);
            if (amount > 0.f) mc.fillRectangle({ x, innerTop, x + segW * amount, innerBottom }, healthCol);
        }

        // Golden hearts: a gold strip along the top of the bar, as long as the share of max health they add.
        if (goldPoints > 0) {
            const float share = std::clamp(info.absorption / std::max(info.maxHealth, 1.f), 0.f, 1.f);
            mc.fillRectangle({ innerLeft, innerTop, innerLeft + (innerRight - innerLeft) * share, innerTop + 3.f },
                             gold);
        }
    } else if (asHearts) {
        // Golden hearts sit in the rows above the red ones.
        const int goldRows = rowsFor(goldHearts);
        if (goldHearts > 0) {
            drawHeartRows(dc, contentLeft, healthY, goldHearts, goldPoints / 2, goldPoints % 2 != 0, true);
        }
        drawHeartRows(dc, contentLeft, healthY + goldRows * kHeartPitchY, healthHearts, fullHearts, halfHeart, false);
    }

    if (itemsOn) {
        mc.fillRectangle({ contentLeft, dividerY, contentLeft + contentW, dividerY + kLine }, accentDim);
        for (auto const& slot : slots) {
            d2d::Rect box { slot.x, itemsY, slot.x + kSlotBox, itemsY + kSlotBox };
            mc.fillRectangle(box, slotFill);
            fillFrame(mc, box, 1.5f, accent);
        }
        if (offhandDividerX >= 0.f) {
            mc.fillRectangle({ offhandDividerX, itemsY, offhandDividerX + kLine, itemsY + kSlotBox }, accentDim);
        }
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
            d2d::Rect headRect { headLeft + kHeadInset, headTop + kHeadInset, headLeft + kHeadBox - kHeadInset,
                                 headTop + kHeadBox - kHeadInset };
            mc.drawImage(texture, headRect.getPos(), headRect.getSize(), { 0.f, 0.f }, { 1.f, 1.f },
                         d2d::Colors::WHITE);
        }
    }

    // ---- Text ----
    dc.drawText({ contentLeft, nameY, contentLeft + contentW, nameY + nameBox.y + 1.f }, info.name, text,
                Renderer::FontSelection::PrimaryRegular, nameSize, DWRITE_TEXT_ALIGNMENT_LEADING,
                DWRITE_PARAGRAPH_ALIGNMENT_NEAR, cacheText);

    if (asBar) {
        // The number sits to the right of the bar, centred on it. Golden hearts follow in gold.
        const float numberX = contentLeft + contentW - numberW;
        dc.drawText({ numberX, healthY, numberX + healthTextBox.x + 4.f, healthY + kBarHeight }, healthText, healthCol,
                    Renderer::FontSelection::PrimaryRegular, numberSize, DWRITE_TEXT_ALIGNMENT_LEADING,
                    DWRITE_PARAGRAPH_ALIGNMENT_CENTER, cacheText);
        if (!goldText.empty()) {
            const float goldX = numberX + healthTextBox.x;
            dc.drawText({ goldX, healthY, goldX + goldTextBox.x + 4.f, healthY + kBarHeight }, goldText, gold,
                        Renderer::FontSelection::PrimaryRegular, numberSize, DWRITE_TEXT_ALIGNMENT_LEADING,
                        DWRITE_PARAGRAPH_ALIGNMENT_CENTER, cacheText);
        }
    } else if (asNumber) {
        dc.drawText({ contentLeft, healthY, contentLeft + healthTextBox.x + 4.f, healthY + healthTextBox.y + 1.f },
                    healthText, healthCol, Renderer::FontSelection::PrimaryRegular, numberSize,
                    DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, cacheText);
        if (!goldText.empty()) {
            const float goldX = contentLeft + healthTextBox.x;
            dc.drawText({ goldX, healthY, goldX + goldTextBox.x + 4.f, healthY + goldTextBox.y + 1.f }, goldText, gold,
                        Renderer::FontSelection::PrimaryRegular, numberSize, DWRITE_TEXT_ALIGNMENT_LEADING,
                        DWRITE_PARAGRAPH_ALIGNMENT_NEAR, cacheText);
        }
    }

    // ---- Armor and offhand ----
    mc.flush(false);
    const float iconInset = (kSlotBox - kSlotIcon) * 0.5f;
    for (auto const& slot : slots) {
        if (!slot.stack || !slot.stack->getItem()) continue;

        mc.drawItem(slot.stack, { slot.x + iconInset, itemsY + iconInset }, kSlotIcon / 48.f, 1.f);
        if (slot.stack->itemCount > 1) {
            dc.drawText({ slot.x, itemsY, slot.x + kSlotBox - 2.f, itemsY + kSlotBox }, std::to_wstring(slot.stack->itemCount),
                        d2d::Colors::WHITE, Renderer::FontSelection::PrimaryRegular, 18.f, DWRITE_TEXT_ALIGNMENT_TRAILING,
                        DWRITE_PARAGRAPH_ALIGNMENT_FAR);
        }
    }

    dc.flush();
}
