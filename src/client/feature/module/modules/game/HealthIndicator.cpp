#include "pch.h"
#include "HealthIndicator.h"

#include "util/WorldToScreen.h"
#include "mc/common/entity/component/AttributesComponent.h"

#include <algorithm>
#include <cmath>

namespace {
    constexpr auto kFont = Renderer::FontSelection::PrimaryRegular;
    constexpr int kHeartsPerRow = 10;
    constexpr int kMaxHeartsShown = 20;

    // 7.0 -> "7", 7.5 -> "7.5" (health is tracked in half-heart steps)
    std::wstring formatHalf(float v) {
        float rounded = std::round(v * 2.f) / 2.f;
        if (rounded == std::floor(rounded)) {
            return std::to_wstring(static_cast<int>(rounded));
        }
        return std::format(L"{:.1f}", rounded);
    }

    d2d::Color healthGradient(float fraction) {
        fraction = std::clamp(fraction, 0.f, 1.f);
        float r = fraction < 0.5f ? 1.f : 2.f * (1.f - fraction);
        float g = fraction > 0.5f ? 1.f : 2.f * fraction;
        return d2d::Color(r, g, 0.f, 1.f);
    }

    struct Target {
        Vec2 screen;
        float distance;
        float health;
        float maxHealth;
        std::wstring label; // only set in debug mode
    };
}

HealthIndicator::HealthIndicator()
    : Module("HealthIndicator", LocalizeString::get("client.module.healthIndicator.name"),
             LocalizeString::get("client.module.healthIndicator.desc"), GAME, nokeybind) {
    style.addEntry(EnumEntry(StyleNumber, LocalizeString::get("client.module.healthIndicator.style.number.name"),
                             LocalizeString::get("client.module.healthIndicator.style.number.desc")));
    style.addEntry(EnumEntry(StyleHearts, LocalizeString::get("client.module.healthIndicator.style.hearts.name"),
                             LocalizeString::get("client.module.healthIndicator.style.hearts.desc")));

    numberFormat.addEntry(EnumEntry(FormatHearts,
                                    LocalizeString::get("client.module.healthIndicator.numberFormat.hearts.name"),
                                    LocalizeString::get("client.module.healthIndicator.numberFormat.hearts.desc")));
    numberFormat.addEntry(EnumEntry(FormatPoints,
                                    LocalizeString::get("client.module.healthIndicator.numberFormat.points.name"),
                                    LocalizeString::get("client.module.healthIndicator.numberFormat.points.desc")));

    addEnumSetting("style", LocalizeString::get("client.module.healthIndicator.style.name"),
                   LocalizeString::get("client.module.healthIndicator.style.desc"), style);
    addEnumSetting("numberFormat", LocalizeString::get("client.module.healthIndicator.numberFormat.name"),
                   LocalizeString::get("client.module.healthIndicator.numberFormat.desc"), numberFormat,
                   Setting::Condition("style", Setting::Condition::EQUALS, { StyleNumber }));
    addSetting("colorByHealth", LocalizeString::get("client.module.healthIndicator.colorByHealth.name"),
               LocalizeString::get("client.module.healthIndicator.colorByHealth.desc"), colorByHealth,
               Setting::Condition("style", Setting::Condition::EQUALS, { StyleNumber }));
    addSetting("textColor", LocalizeString::get("client.module.healthIndicator.textColor.name"),
               LocalizeString::get("client.module.healthIndicator.textColor.desc"), textColor,
               "colorByHealth"_isfalse);
    addSetting("showBackground", LocalizeString::get("client.module.healthIndicator.showBackground.name"),
               LocalizeString::get("client.module.healthIndicator.showBackground.desc"), showBackground);
    addSetting("playersOnly", LocalizeString::get("client.module.healthIndicator.playersOnly.name"),
               LocalizeString::get("client.module.healthIndicator.playersOnly.desc"), playersOnly);
    addSetting("debug", LocalizeString::get("client.module.healthIndicator.debug.name"),
               LocalizeString::get("client.module.healthIndicator.debug.desc"), debug);
    addSliderSetting("textSize",LocalizeString::get("client.module.healthIndicator.textSize.name"),
                     LocalizeString::get("client.module.healthIndicator.textSize.desc"), textSize, FloatValue(16.f),
                     FloatValue(48.f), FloatValue(1.f));
    addSliderSetting("maxDistance", LocalizeString::get("client.module.healthIndicator.maxDistance.name"),
                     LocalizeString::get("client.module.healthIndicator.maxDistance.desc"), maxDistance,
                     FloatValue(8.f), FloatValue(128.f), FloatValue(1.f));
    addSliderSetting("heightOffset", LocalizeString::get("client.module.healthIndicator.heightOffset.name"),
                     LocalizeString::get("client.module.healthIndicator.heightOffset.desc"), heightOffset,
                     FloatValue(0.f), FloatValue(2.f), FloatValue(0.05f));

    listen<RenderOverlayEvent>(&HealthIndicator::onRenderOverlay);
}

void HealthIndicator::onRenderOverlay(Event&) {
    auto* ci = SDK::ClientInstance::get();
    if (!ci || !ci->minecraft || !ci->minecraftGame) return;

    auto* lp = ci->getLocalPlayer();
    auto* level = ci->minecraft->getLevel();
    if (!lp || !level || !ci->minecraft->timer) return;

    // Only draw while actually playing (hidden in menus / inventory / pause screen).
    if (!ci->minecraftGame->isCursorGrabbed()) return;

    const float alpha = ci->minecraft->timer->alpha;
    const float maxDist = std::get<FloatValue>(maxDistance);
    const float yOffset = std::get<FloatValue>(heightOffset);
    const bool onlyPlayers = std::get<BoolValue>(playersOnly);
    const Vec3 camPos = lp->getPos();

    std::vector<Target> targets;

    const bool debugMode = std::get<BoolValue>(debug);

    for (auto* actor : level->getRuntimeActorList()) {
        if (!actor || actor == lp) continue;
        if (!debugMode && onlyPlayers && !actor->isPlayer()) continue;

        // Read the health attribute directly with null checks instead of Actor::getHealth(), which
        // dereferences the attributes component without checking that the entity actually has one.
        auto* attrs = actor->tryGetComponent<SDK::AttributesComponent>();
        auto* inst = attrs ? attrs->baseAttributes.getInstance(SDK::Attributes::Health.mIDValue) : nullptr;

        std::wstring label;
        if (debugMode) {
            // Shows, for every entity, exactly why it would or wouldn't get an indicator.
            label = std::format(L"type={} {} ", actor->getEntityTypeID(), actor->isPlayer() ? L"PLAYER" : L"mob");
            if (!attrs) {
                label += L"NO ATTR COMPONENT";
            } else if (!inst) {
                label += L"NO HEALTH ATTR";
            } else {
                label += std::format(L"hp={:.1f}/{:.1f}", inst->value, inst->maxValue);
            }
        }

        if (!inst || !(inst->value > 0.f)) {
            if (!debugMode) continue;
        }

        float healthValue = inst ? inst->value : 0.f;
        float maxHealth = inst ? inst->maxValue : 20.f;
        if (!(maxHealth > 0.f)) maxHealth = 20.f;

        Vec3 cur = actor->getPos();
        Vec3 old = actor->getPosOld();
        Vec3 shift = { std::lerp(old.x, cur.x, alpha) - cur.x, std::lerp(old.y, cur.y, alpha) - cur.y,
                       std::lerp(old.z, cur.z, alpha) - cur.z };

        AABB bb = actor->getBoundingBox();
        Vec3 head = { (bb.lower.x + bb.higher.x) * 0.5f + shift.x, bb.higher.y + shift.y + yOffset,
                      (bb.lower.z + bb.higher.z) * 0.5f + shift.z };

        float dx = head.x - camPos.x;
        float dy = head.y - camPos.y;
        float dz = head.z - camPos.z;
        float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (distance > maxDist) continue;

        auto screen = WorldToScreen::convert(head);
        if (!screen.has_value()) continue;

        targets.push_back({ *screen, distance, healthValue, maxHealth, std::move(label) });
    }

    if (targets.empty()) return;

    // Far to near so closer players are drawn on top.
    std::sort(targets.begin(), targets.end(), [](Target const& a, Target const& b) {
        return a.distance > b.distance;
    });

    D2DUtil dc;
    const bool hearts = style.getSelectedKey() == StyleHearts;

    for (auto& t : targets) {
        // Shrink slightly with distance so far players don't clutter the screen.
        float scale = std::clamp(6.f / std::max(t.distance, 1.f), 0.6f, 1.f);

        if (!t.label.empty()) {
            drawLabel(dc, t.screen, scale, t.label, d2d::Colors::YELLOW);
        } else if (hearts && std::ceil(t.maxHealth / 2.f) <= kMaxHeartsShown) {
            drawHearts(dc, t.screen, scale, t.health, t.maxHealth);
        } else {
            // Also the fallback when a player has too many hearts to fit a readable row.
            drawNumber(dc, t.screen, scale, t.health, t.maxHealth);
        }
    }
}

void HealthIndicator::drawNumber(D2DUtil& dc, Vec2 const& anchor, float scale, float health, float maxHealth) {
    std::wstring text;
    if (numberFormat.getSelectedKey() == FormatHearts) {
        text = formatHalf(health / 2.f) + L"/" + formatHalf(maxHealth / 2.f); // 10/10
    } else {
        text = std::to_wstring(static_cast<int>(std::ceil(health))) + L"/" +
               std::to_wstring(static_cast<int>(std::ceil(maxHealth))); // 20/20
    }

    d2d::Color color = std::get<BoolValue>(colorByHealth)
                           ? healthGradient(health / maxHealth)
                           : d2d::Color(std::get<ColorValue>(textColor).getMainColor());

    drawLabel(dc, anchor, scale, text, color);
}

void HealthIndicator::drawLabel(D2DUtil& dc, Vec2 const& anchor, float scale, std::wstring const& text,
                                d2d::Color const& color) {
    const float size = std::get<FloatValue>(textSize) * scale;
    const float pad = size * 0.2f;
    const Vec2 ts = dc.getTextSize(text, kFont, size, true, false);

    d2d::Rect bg { anchor.x - ts.x * 0.5f - pad, anchor.y - ts.y - pad * 2.f, anchor.x + ts.x * 0.5f + pad, anchor.y };

    if (std::get<BoolValue>(showBackground)) {
        dc.fillRoundedRectangle(bg, d2d::Color(0.f, 0.f, 0.f, 0.45f), size * 0.25f);
    }

    // A little slack on the sides so the layout never wraps.
    d2d::Rect textRect { bg.left - 4.f, bg.top, bg.right + 4.f, bg.bottom };
    dc.drawText(textRect, text, color, kFont, size, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                false);
}

void HealthIndicator::drawHearts(D2DUtil& dc, Vec2 const& anchor, float scale, float health, float maxHealth) {
    const int totalHearts = std::max(1, static_cast<int>(std::ceil(maxHealth / 2.f)));
    const int halfPoints = std::clamp(static_cast<int>(std::ceil(health)), 0, totalHearts * 2);
    const int fullHearts = halfPoints / 2;
    const bool hasHalf = (halfPoints % 2) != 0;

    const float size = std::get<FloatValue>(textSize) * scale;
    const float cell = size * 0.9f;
    const float pad = size * 0.15f;
    const int rows = (totalHearts + kHeartsPerRow - 1) / kHeartsPerRow;
    const int widest = std::min(totalHearts, kHeartsPerRow);

    if (std::get<BoolValue>(showBackground)) {
        d2d::Rect bg { anchor.x - cell * widest * 0.5f - pad, anchor.y - cell * rows - pad * 2.f,
                       anchor.x + cell * widest * 0.5f + pad, anchor.y };
        dc.fillRoundedRectangle(bg, d2d::Color(0.f, 0.f, 0.f, 0.45f), size * 0.25f);
    }

    const d2d::Color filled = d2d::Color::RGB(226, 55, 65);
    const d2d::Color empty = d2d::Color::RGB(70, 70, 70, 210);
    const std::wstring glyph = L"♥";

    for (int i = 0; i < totalHearts; ++i) {
        const int row = i / kHeartsPerRow;
        const int col = i % kHeartsPerRow;
        const int inRow = std::min(kHeartsPerRow, totalHearts - row * kHeartsPerRow);

        // Row 0 sits closest to the head; extra rows stack upward.
        float left = anchor.x - cell * inRow * 0.5f + cell * col;
        float bottom = anchor.y - pad - cell * row;
        d2d::Rect cellRect { left, bottom - cell, left + cell, bottom };

        const bool isFull = i < fullHearts;
        const bool isHalf = hasHalf && i == fullHearts;

        dc.drawText(cellRect, glyph, isFull ? filled : empty, kFont, size, DWRITE_TEXT_ALIGNMENT_CENTER,
                    DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);

        if (isHalf) {
            // Left half of the heart in red on top of the empty heart.
            d2d::Rect halfClip { cellRect.left, cellRect.top, cellRect.left + cell * 0.5f, cellRect.bottom };
            dc.ctx->PushAxisAlignedClip(halfClip.get(), D2D1_ANTIALIAS_MODE_ALIASED);
            dc.drawText(cellRect, glyph, filled, kFont, size, DWRITE_TEXT_ALIGNMENT_CENTER,
                        DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
            dc.ctx->PopAxisAlignedClip();
        }
    }
}
