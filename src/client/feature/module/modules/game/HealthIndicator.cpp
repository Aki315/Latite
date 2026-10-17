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

    // Vanilla-style heart, drawn from rectangles (no font glyph, no image file).
    // O = outline, R = body, W = shine, . = empty.
    constexpr int kHeartCols = 9;
    constexpr int kHeartRows = 8;
    constexpr const char* kHeart[kHeartRows] = {
        ".OOO.OOO.", "OWRRORRRO", "OWRRRRRRO", "ORRRRRRRO", ".ORRRRRO.", "..ORRRO..", "...ORO...", "....O....",
    };

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
        SDK::AttributeInstance* inst = nullptr;
        const wchar_t* via = L"";
        if (attrs) {
            auto& map = attrs->baseAttributes;
            const int healthId = SDK::Attributes::Health.mIDValue;
            inst = map.getInstance(healthId);
            if (!inst) {
                inst = map.getInstanceByStride(healthId);
                via = L" (stride)";
            }
            if (!inst) {
                // Players: 17 ids vs 18 instances (id 6 missing from `ids`), so the strict lookup refuses to
                // work. Instances are one contiguous run of ids, so index them by (id - first id).
                inst = map.getInstanceDense(healthId);
                via = L" (dense)";
            }
            if (!inst) {
                inst = map.getInstanceLenient(healthId);
                via = L" (lenient)";
            }
            // Reject garbage so a bad lookup can never show nonsense.
            if (inst && !(inst->maxValue > 0.f && inst->maxValue <= 2048.f && inst->value >= 0.f &&
                          inst->value <= inst->maxValue + 1024.f)) {
                inst = nullptr;
            }
        }

        std::wstring label;
        if (debugMode) {
            // Shows, for every entity, exactly why it would or wouldn't get an indicator.
            label = std::format(L"type={} {} ", actor->getEntityTypeID(), actor->isPlayer() ? L"PLAYER" : L"mob");
            if (!attrs) {
                label += L"NO ATTR COMPONENT";
            } else if (!inst) {
                auto& map = attrs->baseAttributes;
                label += std::format(L"NO HEALTH ATTR ids={} inst={} last={} [", map.idCount(), map.instanceCount(),
                                     map.lastId());
                label += std::format(L"stride=0x{:X} ", map.realStride());
                size_t shown = std::min<size_t>(map.idCount(), 14);
                for (size_t i = 0; i < shown; ++i) {
                    label += std::format(L"{}{}", i ? L"," : L"", map.idAt(i));
                }
                label += map.idCount() > shown ? L",...]" : L"]";
            } else {
                label += std::format(L"hp={:.1f}/{:.1f}{}", inst->value, inst->maxValue, via);
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

    // One heart pixel is a whole number of screen pixels, so the rectangles line up without seams and the
    // hearts stay crisp. Neighbouring hearts share their outline column, like the vanilla health bar.
    const float size = std::get<FloatValue>(textSize) * scale;
    const float px = std::max(1.f, std::round(size * 0.9f / kHeartCols));
    const float heartH = kHeartRows * px;
    const float pitchX = (kHeartCols - 1) * px;
    const float pitchY = (kHeartRows + 1) * px;
    const float pad = std::max(2.f, std::round(px * 1.5f));
    const int rows = (totalHearts + kHeartsPerRow - 1) / kHeartsPerRow;
    const int widest = std::min(totalHearts, kHeartsPerRow);

    if (std::get<BoolValue>(showBackground)) {
        const float totalW = pitchX * widest + px;
        d2d::Rect bg { std::round(anchor.x - totalW * 0.5f) - pad, std::round(anchor.y - pitchY * rows - pad * 2.f),
                       std::round(anchor.x - totalW * 0.5f) + totalW + pad, anchor.y };
        dc.fillRoundedRectangle(bg, d2d::Color(0.f, 0.f, 0.f, 0.45f), size * 0.25f);
    }

    const d2d::Color outline = d2d::Color::RGB(25, 8, 10);
    const d2d::Color filled = d2d::Color::RGB(226, 55, 65);
    const d2d::Color shine = d2d::Color::RGB(255, 170, 175);
    const d2d::Color empty = d2d::Color::RGB(70, 70, 70);

    for (int i = 0; i < totalHearts; ++i) {
        const int row = i / kHeartsPerRow;
        const int col = i % kHeartsPerRow;
        const int inRow = std::min(kHeartsPerRow, totalHearts - row * kHeartsPerRow);

        // Row 0 sits closest to the head; extra rows stack upward.
        const float left = std::round(anchor.x - (pitchX * inRow + px) * 0.5f + pitchX * col);
        const float bottom = std::round(anchor.y - pad - pitchY * row);
        const float top = bottom - heartH;

        const bool isFull = i < fullHearts;
        const bool isHalf = hasHalf && i == fullHearts;
        // A half heart is red on its left side only.
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
                d2d::Rect rc { left + c * px, top + r * px, left + end * px, top + (r + 1) * px };
                dc.fillRectangle(rc, color);
                c = end;
            }
        }
    }
}
