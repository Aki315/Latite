#include "pch.h"
#include "TargetHUD.h"

#include "client/event/events/AttackEvent.h"
#include "client/misc/PlayerHeadCache.h"
#include "mc/common/client/game/ClientInstance.h"
#include "mc/common/client/renderer/Tessellator.h"
#include "mc/common/entity/component/ActorEquipmentComponent.h"
#include "mc/common/entity/component/AttributesComponent.h"
#include "mc/common/world/actor/player/Inventory.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <vector>

namespace {
    constexpr float kPi = 3.14159265f;

    // ------------------------------------------------------------------------------------------------------------
    // Reading the target
    // ------------------------------------------------------------------------------------------------------------
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

    // ------------------------------------------------------------------------------------------------------------
    // Colors
    // ------------------------------------------------------------------------------------------------------------
    // 14.0 -> "14", 8.5 -> "8.5". At most `places` decimals, trailing zeros removed.
    std::wstring trimmed(float value, int places) {
        std::wstring s = std::format(L"{:.{}f}", value, places);
        if (places > 0) {
            while (!s.empty() && s.back() == L'0') s.pop_back();
            if (!s.empty() && s.back() == L'.') s.pop_back();
        }
        return s;
    }

    d2d::Color alpha(d2d::Color const& c, float a) { return d2d::Color(c.r, c.g, c.b, a); }

    d2d::Color mix(d2d::Color const& a, d2d::Color const& b, float t) {
        t = std::clamp(t, 0.f, 1.f);
        return d2d::Color(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
    }

    // Full health is green, half is yellow, almost dead is red.
    d2d::Color healthGradient(float fraction) {
        const d2d::Color green = d2d::Color::RGB(52, 211, 98);
        const d2d::Color yellow = d2d::Color::RGB(250, 204, 21);
        const d2d::Color red = d2d::Color::RGB(239, 68, 68);
        fraction = std::clamp(fraction, 0.f, 1.f);
        return fraction >= 0.5f ? mix(yellow, green, (fraction - 0.5f) * 2.f) : mix(red, yellow, fraction * 2.f);
    }

    // ------------------------------------------------------------------------------------------------------------
    // Everything a design needs to draw itself
    // ------------------------------------------------------------------------------------------------------------
    struct Frame {
        MCDrawUtil* mc = nullptr;
        TargetInfo const* info = nullptr;
        bool cacheText = false;
        float textSize = 20.f;
        bool armorOn = true;
        bool offhandOn = true;

        d2d::Color bg = d2d::Color(0.f, 0.f, 0.f, 1.f);
        d2d::Color text = d2d::Color(1.f, 1.f, 1.f, 1.f);
        d2d::Color dim = d2d::Color(1.f, 1.f, 1.f, 1.f);
        d2d::Color health = d2d::Color(1.f, 1.f, 1.f, 1.f);
        d2d::Color gold = d2d::Color(1.f, 1.f, 1.f, 1.f);
        d2d::Color faint = d2d::Color(1.f, 1.f, 1.f, 1.f);
        d2d::Color track = d2d::Color(1.f, 1.f, 1.f, 1.f);
        float fraction = 1.f;      // health / max health, 0..1
        int healthPoints = 0;      // rounded up
        int maxPoints = 20;        // rounded up
        int goldPoints = 0;        // golden hearts, in points
        int display = 1;           // 1 = the design's own bar or ring, 2 = a row of hearts, 3 = number only
        std::wstring healthNow;    // "14"
        std::wstring healthMax;    // "20"
        std::wstring healthText;   // "14/20"
        std::wstring goldText;     // "+4" (empty without golden hearts)
    };

    // ------------------------------------------------------------------------------------------------------------
    // Text
    // ------------------------------------------------------------------------------------------------------------
    Vec2 measure(Frame& f, std::wstring const& s, float size) {
        return f.mc->getTextSize(s, Renderer::FontSelection::PrimaryRegular, size, true, f.cacheText);
    }

    void putText(Frame& f, float x, float y, float w, float h, std::wstring const& s, d2d::Color const& color,
                 float size, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
                 DWRITE_PARAGRAPH_ALIGNMENT vAlign = DWRITE_PARAGRAPH_ALIGNMENT_CENTER) {
        if (s.empty()) return;
        f.mc->drawText({ x, y, x + w, y + h }, s, color, Renderer::FontSelection::PrimaryRegular, size, align, vAlign,
                       f.cacheText);
    }

    // ------------------------------------------------------------------------------------------------------------
    // Shapes. Coordinates are in HUD units, like everything else here.
    // ------------------------------------------------------------------------------------------------------------
    void fillRound(Frame& f, d2d::Rect const& rc, float radius, d2d::Color const& color) {
        f.mc->fillRoundedRectangle(rc, color, radius);
    }

    void strip(Frame& f, std::vector<Vec2> const& pts, d2d::Color const& color) {
        if (pts.size() < 3) return;
        auto* scn = f.mc->scn;
        auto* tess = scn->tess;
        const float gs = f.mc->guiScale;
        *scn->shaderColor = { 1.f, 1.f, 1.f, 1.f };
        tess->begin(SDK::Primitive::Trianglestrip, static_cast<int>(pts.size()));
        tess->color(color);
        for (auto const& p : pts) tess->vertex(p.x * gs, p.y * gs);
        f.mc->flush(false);
    }

    void fillDisc(Frame& f, Vec2 c, float r, d2d::Color const& color) {
        constexpr int n = 40;
        std::vector<Vec2> pts;
        for (int i = 0; i <= n; ++i) {
            const float a = 2.f * kPi * static_cast<float>(i) / n;
            pts.push_back({ c.x + r * std::cos(a), c.y + r * std::sin(a) });
            pts.push_back(c);
        }
        strip(f, pts, color);
    }

    // A ring segment of thickness t whose middle is on radius r. Angles are in radians, clockwise on screen, 0 = right.
    void fillArcBand(Frame& f, Vec2 c, float r, float t, float a0, float sweep, d2d::Color const& color) {
        const int n = std::max(2, static_cast<int>(std::ceil(std::abs(sweep) / (kPi / 24.f))));
        const float outer = r + t * 0.5f;
        const float inner = r - t * 0.5f;
        std::vector<Vec2> pts;
        for (int i = 0; i <= n; ++i) {
            const float a = a0 + sweep * static_cast<float>(i) / n;
            pts.push_back({ c.x + outer * std::cos(a), c.y + outer * std::sin(a) });
            pts.push_back({ c.x + inner * std::cos(a), c.y + inner * std::sin(a) });
        }
        strip(f, pts, color);
    }

    // The area between a rectangle's corner and the quarter circle that rounds it.
    void fillCornerWedge(Frame& f, Vec2 corner, Vec2 arcCenter, float r, float a0, d2d::Color const& color) {
        constexpr int n = 12;
        std::vector<Vec2> pts;
        for (int i = 0; i <= n; ++i) {
            const float a = a0 + (kPi * 0.5f) * static_cast<float>(i) / n;
            pts.push_back({ arcCenter.x + r * std::cos(a), arcCenter.y + r * std::sin(a) });
            pts.push_back(corner);
        }
        strip(f, pts, color);
    }

    // Rounds the corners of whatever was drawn inside rc, by painting the corners in the panel colour.
    // A radius of half the size turns a square into a circle.
    void roundCorners(Frame& f, d2d::Rect const& rc, float r, d2d::Color const& mask) {
        r = std::min(r, std::min(rc.right - rc.left, rc.bottom - rc.top) * 0.5f);
        fillCornerWedge(f, { rc.left, rc.top }, { rc.left + r, rc.top + r }, r, kPi, mask);
        fillCornerWedge(f, { rc.right, rc.top }, { rc.right - r, rc.top + r }, r, kPi * 1.5f, mask);
        fillCornerWedge(f, { rc.right, rc.bottom }, { rc.right - r, rc.bottom - r }, r, 0.f, mask);
        fillCornerWedge(f, { rc.left, rc.bottom }, { rc.left + r, rc.bottom - r }, r, kPi * 0.5f, mask);
    }

    // The player's face, with rounded corners.
    void drawHead(Frame& f, d2d::Rect const& box, float radius) {
        f.mc->fillRectangle(box, d2d::Color::RGB(30, 36, 52));

        auto& path = f.info->faceTexturePath;
        if (!path.empty() && f.mc->renderCtx) {
            SDK::TexturePtr texture {};
            const auto external = std::filesystem::path(path).is_absolute();
            const auto fileSystem = external ? SDK::ResourceFileSystem::Raw : SDK::ResourceFileSystem::UserPackage;
            f.mc->renderCtx->getTexture(&texture, SDK::ResourceLocation(path, fileSystem), external);
            if (texture.textureData) {
                f.mc->drawImage(texture, box.getPos(), box.getSize(), { 0.f, 0.f }, { 1.f, 1.f }, d2d::Colors::WHITE);
            }
        }
        roundCorners(f, box, radius, alpha(f.bg, 1.f));
    }

    // A smooth pill-shaped bar: dark track, fill in the health colour. Optional quarter ticks.
    void pillBar(Frame& f, float x, float y, float w, float h, bool ticks) {
        fillRound(f, { x, y, x + w, y + h }, h * 0.5f, f.track);
        if (f.fraction > 0.f) {
            fillRound(f, { x, y, x + std::max(h, w * f.fraction), y + h }, h * 0.5f, f.health);
        }
        if (ticks) {
            for (int i = 1; i < 4; ++i) {
                const float tx = x + w * static_cast<float>(i) / 4.f;
                f.mc->fillRectangle({ tx - 0.75f, y, tx + 0.75f, y + h }, alpha(f.bg, 0.85f));
            }
        }
    }

    // ------------------------------------------------------------------------------------------------------------
    // Chips: "14/20 +4" in a tinted pill, or just the golden hearts
    // ------------------------------------------------------------------------------------------------------------
    constexpr float kChipPad = 6.f;

    Vec2 numberChipSize(Frame& f, float size) {
        const Vec2 a = measure(f, f.healthText, size);
        const Vec2 b = f.goldText.empty() ? Vec2 { 0.f, 0.f } : measure(f, f.goldText, size);
        return { a.x + b.x + kChipPad * 2.f, a.y + 4.f };
    }

    void drawNumberChip(Frame& f, float x, float y, float size) {
        const Vec2 a = measure(f, f.healthText, size);
        const Vec2 b = f.goldText.empty() ? Vec2 { 0.f, 0.f } : measure(f, f.goldText, size);
        const Vec2 chip = numberChipSize(f, size);
        fillRound(f, { x, y, x + chip.x, y + chip.y }, chip.y * 0.5f, alpha(f.health, 0.18f));
        putText(f, x + kChipPad, y, a.x + 2.f, chip.y, f.healthText, f.health, size);
        putText(f, x + kChipPad + a.x, y, b.x + 2.f, chip.y, f.goldText, f.gold, size);
    }

    Vec2 goldChipSize(Frame& f, float size) {
        return { measure(f, f.goldText, size).x + kChipPad * 2.f, measure(f, f.goldText, size).y + 4.f };
    }

    void drawGoldChip(Frame& f, float x, float y, float size) {
        const Vec2 chip = goldChipSize(f, size);
        fillRound(f, { x, y, x + chip.x, y + chip.y }, chip.y * 0.5f, alpha(f.gold, 0.18f));
        putText(f, x + kChipPad, y, chip.x, chip.y, f.goldText, f.gold, size);
    }

    // ------------------------------------------------------------------------------------------------------------
    // Armor and offhand
    // ------------------------------------------------------------------------------------------------------------
    struct Slot {
        float x;
        float y;
        SDK::ItemStack* stack;
    };

    struct ItemsRow {
        std::vector<Slot> slots;
        float width = 0.f;
        float dividerX = -1.f; // Centre of the line between the armor and the offhand, -1 = none.
    };

    // Helmet to boots, then a gap with a divider, then the offhand.
    ItemsRow itemsRow(Frame& f, float x, float y, float box, float spacing, float dividerGap) {
        ItemsRow row;
        float cx = x;
        if (f.armorOn) {
            for (int i = 0; i < 4; ++i) {
                row.slots.push_back({ cx, y, f.info->armor[i] });
                cx += box + spacing;
            }
            cx -= spacing;
        }
        if (f.offhandOn) {
            if (f.armorOn) {
                cx += dividerGap;
                row.dividerX = cx;
                cx += dividerGap;
            }
            row.slots.push_back({ cx, y, f.info->offhand });
            cx += box;
        }
        row.width = row.slots.empty() ? 0.f : cx - x;
        return row;
    }

    // Draws the faint boxes (and divider) behind the slots.
    void drawSlotBacks(Frame& f, ItemsRow const& row, float box, float radius) {
        for (auto const& slot : row.slots) {
            fillRound(f, { slot.x, slot.y, slot.x + box, slot.y + box }, radius, f.faint);
        }
        if (row.dividerX >= 0.f && !row.slots.empty()) {
            const float y = row.slots.front().y;
            f.mc->fillRectangle({ row.dividerX - 0.75f, y + 2.f, row.dividerX + 0.75f, y + box - 2.f }, f.track);
        }
    }

    // Draws the items. Call after everything else, the item renderer changes render state.
    void drawSlotItems(Frame& f, ItemsRow const& row, float box, float icon) {
        f.mc->flush(false);
        const float inset = (box - icon) * 0.5f;
        for (auto const& slot : row.slots) {
            if (!slot.stack || !slot.stack->getItem()) continue;

            f.mc->drawItem(slot.stack, { slot.x + inset, slot.y + inset }, icon / 48.f, 1.f);
            if (slot.stack->itemCount > 1) {
                putText(f, slot.x, slot.y, box - 1.f, box, std::to_wstring(slot.stack->itemCount), d2d::Colors::WHITE,
                        13.f, DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_FAR);
            }
        }
    }

    // ------------------------------------------------------------------------------------------------------------
    // Pixel hearts (vanilla style, drawn from rectangles)
    // ------------------------------------------------------------------------------------------------------------
    constexpr int kHeartCols = 9;
    constexpr int kHeartRows = 8;
    constexpr const char* kHeart[kHeartRows] = {
        ".OOO.OOO.", "OWRRORRRO", "OWRRRRRRO", "ORRRRRRRO", ".ORRRRRO.", "..ORRRO..", "...ORO...", "....O....",
    };
    constexpr float kHeartPx = 2.f;
    constexpr float kHeartPitch = (kHeartCols - 1) * kHeartPx;
    constexpr float kHeartHeight = kHeartRows * kHeartPx;
    constexpr int kMaxHeartsInRow = 20;

    // lit: 0 = empty, 1 = half (left side coloured), 2 = full.
    void drawHeart(Frame& f, float x, float y, int lit, bool gold) {
        const d2d::Color outline = d2d::Color::RGB(25, 8, 10);
        const d2d::Color empty = d2d::Color::RGB(58, 62, 78);
        const d2d::Color filled = gold ? d2d::Color::RGB(247, 190, 25) : d2d::Color::RGB(226, 55, 65);
        const d2d::Color shine = gold ? d2d::Color::RGB(255, 240, 160) : d2d::Color::RGB(255, 170, 175);
        const float left = std::round(x);
        const float top = std::round(y);
        auto litAt = [&](int c) { return lit == 2 || (lit == 1 && c < kHeartCols / 2); };

        for (int r = 0; r < kHeartRows; ++r) {
            int c = 0;
            while (c < kHeartCols) {
                const char ch = kHeart[r][c];
                if (ch == '.') {
                    ++c;
                    continue;
                }

                // Merge neighbouring pixels of the same kind into one rectangle.
                const bool on = litAt(c);
                int end = c + 1;
                while (end < kHeartCols && kHeart[r][end] == ch && litAt(end) == on) ++end;

                const d2d::Color color = ch == 'O' ? outline : (ch == 'R' ? (on ? filled : empty) : (on ? shine : empty));
                f.mc->fillRectangle({ left + c * kHeartPx, top + r * kHeartPx, left + end * kHeartPx,
                                      top + (r + 1) * kHeartPx },
                                    color);
                c = end;
            }
        }
    }

    int heartCount(float points) { return std::max(0, static_cast<int>(std::ceil(points / 2.f))); }

    // A row of hearts: the red ones for max health, then the golden ones.
    struct HeartsRow {
        int red = 0;
        int gold = 0;
        bool usable = false; // false when there are too many hearts for one row
        float width = 0.f;
    };

    HeartsRow heartsRow(Frame& f) {
        HeartsRow hr;
        hr.red = heartCount(f.info->maxHealth);
        hr.gold = (f.goldPoints + 1) / 2;
        hr.usable = hr.red + hr.gold <= kMaxHeartsInRow;
        hr.width = hr.usable ? kHeartPitch * static_cast<float>(hr.red + hr.gold) + kHeartPx : 0.f;
        return hr;
    }

    // What to draw for the health: 1 = the design's own bar or ring, 2 = hearts, 3 = number only.
    // Too many hearts for a row fall back to the bar.
    int graphicKind(Frame const& f, HeartsRow const& hr) { return (f.display == 2 && !hr.usable) ? 1 : f.display; }

    void drawHeartsRowAt(Frame& f, float x, float y, HeartsRow const& hr) {
        for (int i = 0; i < hr.red; ++i) {
            const int lit = f.healthPoints >= 2 * i + 2 ? 2 : (f.healthPoints == 2 * i + 1 ? 1 : 0);
            drawHeart(f, x + kHeartPitch * static_cast<float>(i), y, lit, false);
        }
        for (int g = 0; g < hr.gold; ++g) {
            const int lit = f.goldPoints >= 2 * g + 2 ? 2 : 1;
            drawHeart(f, x + kHeartPitch * static_cast<float>(hr.red + g), y, lit, true);
        }
    }

    // One row under the name: a thin bar, a row of hearts, or nothing.
    float graphicRowHeight(int kind, float barH) { return kind == 1 ? barH : (kind == 2 ? kHeartHeight : 0.f); }

    void drawGraphicRow(Frame& f, int kind, float x, float y, float w, float barH, HeartsRow const& hr) {
        if (kind == 1) {
            pillBar(f, x, y, w, barH, false);
        } else if (kind == 2) {
            drawHeartsRowAt(f, x, y, hr);
        }
    }

    // ============================================================================================================
    // Design: Pill. A rounded capsule with a round head, a thin pill bar and a number chip.
    // ============================================================================================================
    Vec2 drawPill(Frame& f) {
        const float nameSize = f.textSize;
        const float numSize = f.textSize * 0.72f;
        constexpr float box = 24.f;
        constexpr float icon = 18.f;
        constexpr float barH = 8.f;
        constexpr float headD = 44.f;
        constexpr float ringT = 3.f;

        const Vec2 nameBox = measure(f, f.info->name, nameSize);
        const Vec2 chip = numberChipSize(f, numSize);
        const ItemsRow probe = itemsRow(f, 0.f, 0.f, box, 5.f, 7.f);
        const HeartsRow hr = heartsRow(f);
        const int kind = graphicKind(f, hr);
        const float graphicH = graphicRowHeight(kind, barH);

        const float topRowH = std::max(nameBox.y, chip.y);
        const float contentW =
            std::max({ 150.f, nameBox.x + 12.f + chip.x, probe.width, kind == 2 ? hr.width : 0.f });
        const float contentH = topRowH + (graphicH > 0.f ? 5.f + graphicH : 0.f) + (probe.slots.empty() ? 0.f : 6.f + box);

        const float ringOuter = headD * 0.5f + 2.f + ringT;
        const float height = std::max(2.f * ringOuter + 10.f, contentH + 16.f);
        const float contentLeft = height * 0.5f + ringOuter + 10.f;
        const float width = contentLeft + contentW + 18.f;
        const float contentTop = (height - contentH) * 0.5f;

        fillRound(f, { 0.f, 0.f, width, height }, height * 0.5f, f.bg);

        // Round head with a ring in the health colour.
        const Vec2 c { height * 0.5f, height * 0.5f };
        drawHead(f, { c.x - headD * 0.5f, c.y - headD * 0.5f, c.x + headD * 0.5f, c.y + headD * 0.5f }, headD * 0.5f);
        fillArcBand(f, c, headD * 0.5f + 2.f + ringT * 0.5f, ringT, 0.f, 2.f * kPi, f.health);

        // Name and number chip.
        putText(f, contentLeft, contentTop, contentW, topRowH, f.info->name, f.text, nameSize);
        drawNumberChip(f, contentLeft + contentW - chip.x, contentTop + (topRowH - chip.y) * 0.5f, numSize);

        // Bar or hearts.
        const float graphicY = contentTop + topRowH + 5.f;
        drawGraphicRow(f, kind, contentLeft, graphicY, contentW, barH, hr);

        // Armor and offhand.
        const float itemsY = contentTop + topRowH + (graphicH > 0.f ? 5.f + graphicH : 0.f) + 6.f;
        const ItemsRow row = itemsRow(f, contentLeft, itemsY, box, 5.f, 7.f);
        drawSlotBacks(f, row, box, 6.f);
        drawSlotItems(f, row, box, icon);
        return { width, height };
    }

    // ============================================================================================================
    // Design: Hearts. Pixel hearts in a row, golden hearts after them, the number in the health colour.
    // ============================================================================================================
    Vec2 drawHeartsDesign(Frame& f) {
        const float nameSize = f.textSize;
        const float numSize = f.textSize * 0.8f;
        constexpr float pad = 10.f;
        constexpr float headS = 48.f;
        constexpr float gap = 12.f;
        constexpr float box = 24.f;
        constexpr float icon = 18.f;
        constexpr float barH = 8.f;

        const Vec2 nameBox = measure(f, f.info->name, nameSize);
        const Vec2 healthBox = measure(f, f.healthText, numSize);
        const Vec2 goldBox = f.goldText.empty() ? Vec2 { 0.f, 0.f } : measure(f, f.goldText, numSize);
        const float numberW = healthBox.x + goldBox.x;

        const HeartsRow hr = heartsRow(f);
        const int kind = graphicKind(f, hr);
        const float graphicH = graphicRowHeight(kind, barH);
        const ItemsRow probe = itemsRow(f, 0.f, 0.f, box, 5.f, 7.f);

        const float topRowH = std::max(nameBox.y, healthBox.y);
        const float contentW =
            std::max({ 150.f, nameBox.x + 12.f + numberW, kind == 2 ? hr.width : 0.f, probe.width });
        const float contentH = topRowH + (graphicH > 0.f ? 5.f + graphicH : 0.f) + (probe.slots.empty() ? 0.f : 5.f + box);
        const float height = std::max(headS, contentH) + pad * 2.f;
        const float contentLeft = pad + headS + gap;
        const float width = contentLeft + contentW + pad;
        const float contentTop = (height - contentH) * 0.5f;

        fillRound(f, { 0.f, 0.f, width, height }, 8.f, f.bg);
        drawHead(f, { pad, (height - headS) * 0.5f, pad + headS, (height + headS) * 0.5f }, 6.f);

        putText(f, contentLeft, contentTop, contentW, topRowH, f.info->name, f.text, nameSize);
        const float numberX = contentLeft + contentW - numberW;
        putText(f, numberX, contentTop, healthBox.x + 2.f, topRowH, f.healthText, f.health, numSize);
        putText(f, numberX + healthBox.x, contentTop, goldBox.x + 2.f, topRowH, f.goldText, f.gold, numSize);

        const float graphicY = contentTop + topRowH + 5.f;
        drawGraphicRow(f, kind, contentLeft, graphicY, contentW, barH, hr);

        const float itemsY = contentTop + topRowH + (graphicH > 0.f ? 5.f + graphicH : 0.f) + 5.f;
        const ItemsRow row = itemsRow(f, contentLeft, itemsY, box, 5.f, 7.f);
        drawSlotBacks(f, row, box, 6.f);
        drawSlotItems(f, row, box, icon);
        return { width, height };
    }

    // ============================================================================================================
    // Design: Ring. A health ring around a round head, a big number and a golden chip.
    // ============================================================================================================
    Vec2 drawRing(Frame& f) {
        const float nameSize = f.textSize * 0.85f;
        const float bigSize = f.textSize * 2.f;
        const float numSize = f.textSize * 0.75f;
        const float chipSize = f.textSize * 0.65f;
        constexpr float pad = 12.f;
        constexpr float headD = 44.f;
        constexpr float ringT = 5.f;
        constexpr float gap = 14.f;
        constexpr float box = 24.f;
        constexpr float icon = 18.f;

        const float ringMid = headD * 0.5f + 3.f + ringT * 0.5f;
        const float ringBox = (ringMid + ringT * 0.5f) * 2.f;

        const std::wstring maxText = L"/" + f.healthMax;
        const Vec2 nameBox = measure(f, f.info->name, nameSize);
        const Vec2 bigBox = measure(f, f.healthNow, bigSize);
        const Vec2 maxBox = measure(f, maxText, numSize);
        const Vec2 chip = f.goldText.empty() ? Vec2 { 0.f, 0.f } : goldChipSize(f, chipSize);
        const ItemsRow probe = itemsRow(f, 0.f, 0.f, box, 5.f, 7.f);
        const HeartsRow hr = heartsRow(f);
        const int kind = graphicKind(f, hr);

        const float numberRowW = bigBox.x + 4.f + maxBox.x + (f.goldText.empty() ? 0.f : 8.f + chip.x);
        const float contentW = std::max({ 110.f, nameBox.x, numberRowW, probe.width, kind == 2 ? hr.width : 0.f });
        const float heartsH = kind == 2 ? 5.f + kHeartHeight : 0.f;
        const float contentH =
            nameBox.y + 2.f + bigBox.y + heartsH + (probe.slots.empty() ? 0.f : 6.f + box);
        const float height = std::max(ringBox, contentH) + pad * 2.f;
        const float contentLeft = pad + ringBox + gap;
        const float width = contentLeft + contentW + pad;
        const float contentTop = (height - contentH) * 0.5f;

        fillRound(f, { 0.f, 0.f, width, height }, 10.f, f.bg);

        // Head in the middle of the ring.
        const Vec2 c { pad + ringBox * 0.5f, height * 0.5f };
        drawHead(f, { c.x - headD * 0.5f, c.y - headD * 0.5f, c.x + headD * 0.5f, c.y + headD * 0.5f }, headD * 0.5f);

        if (kind == 1) {
            // Ring: a dark track, and the health on top, starting at the top and going clockwise.
            fillArcBand(f, c, ringMid, ringT, 0.f, 2.f * kPi, f.track);
            if (f.fraction >= 0.995f) {
                fillArcBand(f, c, ringMid, ringT, 0.f, 2.f * kPi, f.health);
            } else if (f.fraction > 0.f) {
                const float start = -kPi * 0.5f;
                const float sweep = 2.f * kPi * f.fraction;
                fillArcBand(f, c, ringMid, ringT, start, sweep, f.health);
                // Round ends.
                fillDisc(f, { c.x + ringMid * std::cos(start), c.y + ringMid * std::sin(start) }, ringT * 0.5f, f.health);
                fillDisc(f, { c.x + ringMid * std::cos(start + sweep), c.y + ringMid * std::sin(start + sweep) },
                         ringT * 0.5f, f.health);
            }
        } else {
            // Hearts or number only: a thin ring in the health colour around the head.
            fillArcBand(f, c, ringMid, 2.5f, 0.f, 2.f * kPi, f.health);
        }

        // Name, big number, "/20", golden chip.
        putText(f, contentLeft, contentTop, contentW, nameBox.y, f.info->name, f.text, nameSize);
        const float numberY = contentTop + nameBox.y + 2.f;
        putText(f, contentLeft, numberY, bigBox.x + 2.f, bigBox.y, f.healthNow, f.health, bigSize);
        putText(f, contentLeft + bigBox.x + 4.f, numberY, maxBox.x + 2.f, bigBox.y - 4.f, maxText, f.dim, numSize,
                DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_FAR);
        if (!f.goldText.empty()) {
            drawGoldChip(f, contentLeft + bigBox.x + 4.f + maxBox.x + 8.f, numberY + bigBox.y - chip.y - 6.f, chipSize);
        }

        float y = numberY + bigBox.y;
        if (kind == 2) {
            drawHeartsRowAt(f, contentLeft, y + 5.f, hr);
            y += heartsH;
        }

        const ItemsRow row = itemsRow(f, contentLeft, y + 6.f, box, 5.f, 7.f);
        drawSlotBacks(f, row, box, 6.f);
        drawSlotItems(f, row, box, icon);
        return { width, height };
    }

    // ============================================================================================================
    // Design: Tag. One slim line that stays out of the way: head, name, bar with ticks, number, armor.
    // ============================================================================================================
    Vec2 drawTag(Frame& f) {
        const float nameSize = f.textSize * 0.8f;
        const float numSize = f.textSize * 0.7f;
        constexpr float pad = 6.f;
        constexpr float headS = 24.f;
        constexpr float height = 36.f;
        constexpr float barW = 120.f;
        constexpr float barH = 8.f;
        constexpr float box = 22.f;
        constexpr float icon = 17.f;

        const Vec2 nameBox = measure(f, f.info->name, nameSize);
        const Vec2 healthBox = measure(f, f.healthText, numSize);
        const Vec2 goldBox = f.goldText.empty() ? Vec2 { 0.f, 0.f } : measure(f, f.goldText, numSize);
        const ItemsRow probe = itemsRow(f, 0.f, 0.f, box, 3.f, 6.f);
        const bool hasItems = !probe.slots.empty();
        const HeartsRow hr = heartsRow(f);
        const int kind = graphicKind(f, hr);
        const float graphicW = kind == 1 ? barW : (kind == 2 ? hr.width : 0.f);

        float x = pad;
        const float headX = x;
        x += headS + 8.f;
        const float nameX = x;
        x += nameBox.x + 10.f;
        const float graphicX = x;
        if (graphicW > 0.f) x += graphicW + 8.f;
        const float numberX = x;
        x += healthBox.x + goldBox.x;
        float dividerX = -1.f;
        float itemsX = 0.f;
        if (hasItems) {
            x += 8.f;
            dividerX = x;
            x += 1.5f + 8.f;
            itemsX = x;
            x += probe.width;
        }
        const float width = x + pad;

        fillRound(f, { 0.f, 0.f, width, height }, 8.f, f.bg);
        drawHead(f, { headX, (height - headS) * 0.5f, headX + headS, (height + headS) * 0.5f }, 5.f);

        putText(f, nameX, 0.f, nameBox.x + 2.f, height, f.info->name, f.text, nameSize);
        if (kind == 1) {
            pillBar(f, graphicX, (height - barH) * 0.5f, barW, barH, true);
        } else if (kind == 2) {
            drawHeartsRowAt(f, graphicX, (height - kHeartHeight) * 0.5f, hr);
        }
        putText(f, numberX, 0.f, healthBox.x + 2.f, height, f.healthText, f.health, numSize);
        putText(f, numberX + healthBox.x, 0.f, goldBox.x + 2.f, height, f.goldText, f.gold, numSize);

        if (hasItems) {
            f.mc->fillRectangle({ dividerX, 7.f, dividerX + 1.5f, height - 7.f }, f.track);
            const ItemsRow row = itemsRow(f, itemsX, (height - box) * 0.5f, box, 3.f, 6.f);
            drawSlotBacks(f, row, box, 5.f);
            drawSlotItems(f, row, box, icon);
        }
        return { width, height };
    }

    // ============================================================================================================
    // Design: Side bar. A tall bar on the edge, head with name, a big number, armor in a compact grid.
    // ============================================================================================================
    Vec2 drawSideBar(Frame& f) {
        const float nameSize = f.textSize * 0.8f;
        const float bigSize = f.textSize * 2.f;
        const float numSize = f.textSize * 0.7f;
        const float chipSize = f.textSize * 0.65f;
        constexpr float pad = 10.f;
        constexpr float barW = 8.f;
        constexpr float headS = 44.f;
        constexpr float box = 26.f;
        constexpr float icon = 20.f;
        constexpr float spacing = 4.f;

        const HeartsRow hr = heartsRow(f);
        const int kind = graphicKind(f, hr);

        const Vec2 nameBox = measure(f, f.info->name, nameSize);
        const float headColW = std::max(headS, nameBox.x);
        const std::wstring subText = L"/" + f.healthMax + L" health";
        const Vec2 bigBox = measure(f, f.healthNow, bigSize);
        const Vec2 subBox = measure(f, subText, numSize);
        const Vec2 chip = f.goldText.empty() ? Vec2 { 0.f, 0.f } : goldChipSize(f, chipSize);
        const float numberW = std::max({ bigBox.x, subBox.x, chip.x });
        const float numberH = bigBox.y + subBox.y + (f.goldText.empty() ? 0.f : 4.f + chip.y);

        // Armor and offhand go in a grid, three to a row.
        std::vector<SDK::ItemStack*> stacks;
        if (f.armorOn) {
            for (int i = 0; i < 4; ++i) stacks.push_back(f.info->armor[i]);
        }
        if (f.offhandOn) stacks.push_back(f.info->offhand);
        const int count = static_cast<int>(stacks.size());
        const int cols = std::min(count, 3);
        const int rows = (count + 2) / 3;
        const float gridW = count > 0 ? cols * box + (cols - 1) * spacing : 0.f;
        const float gridH = count > 0 ? rows * box + (rows - 1) * spacing : 0.f;

        const float leftColH = headS + 4.f + nameBox.y;
        const float areaH = std::max({ leftColH, numberH, gridH });
        const float heartsH = kind == 2 ? 8.f + kHeartHeight : 0.f; // A row of hearts along the bottom.
        const float height = areaH + heartsH + pad * 2.f;
        const float barArea = kind == 1 ? barW + 10.f : 0.f; // The tall bar only in the bar design.
        const float headColX = pad + barArea;
        const float numberX = headColX + headColW + 14.f;
        const float gridX = numberX + numberW + (count > 0 ? 16.f : 0.f);
        const float width = std::max(gridX + gridW + pad, kind == 2 ? pad * 2.f + hr.width : 0.f);

        fillRound(f, { 0.f, 0.f, width, height }, 8.f, f.bg);

        // Tall bar on the left edge, filling from the bottom.
        if (kind == 1) {
            const float trackH = height - pad * 2.f;
            fillRound(f, { pad, pad, pad + barW, pad + trackH }, barW * 0.5f, f.track);
            if (f.fraction > 0.f) {
                const float fillH = std::max(barW, trackH * f.fraction);
                fillRound(f, { pad, pad + trackH - fillH, pad + barW, pad + trackH }, barW * 0.5f, f.health);
            }
        }

        // Head with the name under it.
        const float leftTop = pad + (areaH - leftColH) * 0.5f;
        const float headX = headColX + (headColW - headS) * 0.5f;
        drawHead(f, { headX, leftTop, headX + headS, leftTop + headS }, 6.f);
        putText(f, headColX, leftTop + headS + 4.f, headColW, nameBox.y, f.info->name, f.text, nameSize,
                DWRITE_TEXT_ALIGNMENT_CENTER);

        // Big number, "/20 health", golden chip.
        const float numberTop = pad + (areaH - numberH) * 0.5f;
        putText(f, numberX, numberTop, bigBox.x + 2.f, bigBox.y, f.healthNow, f.health, bigSize);
        putText(f, numberX, numberTop + bigBox.y, subBox.x + 2.f, subBox.y, subText, f.dim, numSize);
        if (!f.goldText.empty()) drawGoldChip(f, numberX, numberTop + bigBox.y + subBox.y + 4.f, chipSize);

        // Hearts along the bottom.
        if (kind == 2) drawHeartsRowAt(f, pad, pad + areaH + 8.f, hr);

        // Armor grid.
        if (count > 0) {
            ItemsRow row;
            const float gridTop = pad + (areaH - gridH) * 0.5f;
            for (int i = 0; i < count; ++i) {
                row.slots.push_back({ gridX + static_cast<float>(i % 3) * (box + spacing),
                                      gridTop + static_cast<float>(i / 3) * (box + spacing), stacks[i] });
            }
            drawSlotBacks(f, row, box, 6.f);
            drawSlotItems(f, row, box, icon);
        }
        return { width, height };
    }
} // namespace

TargetHUD::TargetHUD()
    : HUDModule("TargetHUD", LocalizeString::get("client.hudmodule.targetHud.name"),
                LocalizeString::get("client.hudmodule.targetHud.desc"), HUD) {
    showPreview = false;

    design.addEntry(EnumEntry(DesignPill, LocalizeString::get("client.hudmodule.targetHud.design.pill.name"),
                              LocalizeString::get("client.hudmodule.targetHud.design.pill.desc")));
    design.addEntry(EnumEntry(DesignHearts, LocalizeString::get("client.hudmodule.targetHud.design.hearts.name"),
                              LocalizeString::get("client.hudmodule.targetHud.design.hearts.desc")));
    design.addEntry(EnumEntry(DesignRing, LocalizeString::get("client.hudmodule.targetHud.design.ring.name"),
                              LocalizeString::get("client.hudmodule.targetHud.design.ring.desc")));
    design.addEntry(EnumEntry(DesignTag, LocalizeString::get("client.hudmodule.targetHud.design.tag.name"),
                              LocalizeString::get("client.hudmodule.targetHud.design.tag.desc")));
    design.addEntry(EnumEntry(DesignSideBar, LocalizeString::get("client.hudmodule.targetHud.design.sideBar.name"),
                              LocalizeString::get("client.hudmodule.targetHud.design.sideBar.desc")));
    addEnumSetting("design", LocalizeString::get("client.hudmodule.targetHud.design.name"),
                   LocalizeString::get("client.hudmodule.targetHud.design.desc"), design);

    healthDisplay.addEntry(EnumEntry(DisplayDefault, LocalizeString::get("client.hudmodule.targetHud.display.default.name"),
                                     LocalizeString::get("client.hudmodule.targetHud.display.default.desc")));
    healthDisplay.addEntry(EnumEntry(DisplayBar, LocalizeString::get("client.hudmodule.targetHud.display.bar.name"),
                                     LocalizeString::get("client.hudmodule.targetHud.display.bar.desc")));
    healthDisplay.addEntry(EnumEntry(DisplayHearts, LocalizeString::get("client.hudmodule.targetHud.display.hearts.name"),
                                     LocalizeString::get("client.hudmodule.targetHud.display.hearts.desc")));
    healthDisplay.addEntry(EnumEntry(DisplayNumber, LocalizeString::get("client.hudmodule.targetHud.display.number.name"),
                                     LocalizeString::get("client.hudmodule.targetHud.display.number.desc")));
    addEnumSetting("healthDisplay", LocalizeString::get("client.hudmodule.targetHud.display.name"),
                   LocalizeString::get("client.hudmodule.targetHud.display.desc"), healthDisplay);

    numberUnit.addEntry(EnumEntry(UnitPoints, LocalizeString::get("client.hudmodule.targetHud.unit.points.name"),
                                  LocalizeString::get("client.hudmodule.targetHud.unit.points.desc")));
    numberUnit.addEntry(EnumEntry(UnitHearts, LocalizeString::get("client.hudmodule.targetHud.unit.hearts.name"),
                                  LocalizeString::get("client.hudmodule.targetHud.unit.hearts.desc")));
    addEnumSetting("numberUnit", LocalizeString::get("client.hudmodule.targetHud.unit.name"),
                   LocalizeString::get("client.hudmodule.targetHud.unit.desc"), numberUnit);
    addSliderSetting("decimals", LocalizeString::get("client.hudmodule.targetHud.decimals.name"),
                     LocalizeString::get("client.hudmodule.targetHud.decimals.desc"), decimals, FloatValue(0.f),
                     FloatValue(2.f), FloatValue(1.f));

    addSliderSetting("duration", LocalizeString::get("client.hudmodule.targetHud.duration.name"),
                     LocalizeString::get("client.hudmodule.targetHud.duration.desc"), duration, FloatValue(1.f),
                     FloatValue(15.f), FloatValue(0.5f));
    addSetting("showArmor", LocalizeString::get("client.hudmodule.targetHud.showArmor.name"),
               LocalizeString::get("client.hudmodule.targetHud.showArmor.desc"), showArmor);
    addSetting("showOffhand", LocalizeString::get("client.hudmodule.targetHud.showOffhand.name"),
               LocalizeString::get("client.hudmodule.targetHud.showOffhand.desc"), showOffhand);
    addSetting("showAbsorption", LocalizeString::get("client.hudmodule.targetHud.showAbsorption.name"),
               LocalizeString::get("client.hudmodule.targetHud.showAbsorption.desc"), showAbsorption);
    addSliderSetting("textSize", LocalizeString::get("client.hudmodule.targetHud.textSize.name"),
                     LocalizeString::get("client.hudmodule.targetHud.textSize.desc"), textSize, FloatValue(12.f),
                     FloatValue(36.f), FloatValue(1.f));
    addSetting("colorByHealth", LocalizeString::get("client.hudmodule.targetHud.colorByHealth.name"),
               LocalizeString::get("client.hudmodule.targetHud.colorByHealth.desc"), colorByHealth);
    addSetting("healthColor", LocalizeString::get("client.hudmodule.targetHud.healthColor.name"),
               LocalizeString::get("client.hudmodule.targetHud.healthColor.desc"), healthColor,
               "colorByHealth"_isfalse);
    addSetting("textColor", LocalizeString::get("client.hudmodule.targetHud.textColor.name"),
               LocalizeString::get("client.hudmodule.targetHud.textColor.desc"), textColor);
    addSetting("backgroundColor", LocalizeString::get("client.hudmodule.targetHud.backgroundColor.name"),
               LocalizeString::get("client.hudmodule.targetHud.backgroundColor.desc"), backgroundColor);

    rect = { 0.f, 0.f, 260.f, 80.f };

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

bool TargetHUD::gatherInfo(bool preview, TargetInfo& info) {
    // In the HUD editor show sample data, so there is always something to move around.
    if (preview) {
        info.name = L"Steve";
        info.health = 14.f;
        info.maxHealth = 20.f;
        info.absorption = std::get<BoolValue>(showAbsorption) ? 4.f : 0.f;
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

void TargetHUD::render(DrawUtil& dc, bool isDefault, bool inEditor) {
    if (!dc.isMinecraft()) return;

    TargetInfo info;
    if (!gatherInfo(isDefault || inEditor, info)) return;

    auto& mc = static_cast<MCDrawUtil&>(dc);

    Frame f;
    f.mc = &mc;
    f.info = &info;
    f.cacheText = !isDefault && !inEditor;
    f.textSize = std::get<FloatValue>(textSize).value;
    f.armorOn = std::get<BoolValue>(showArmor);
    f.offhandOn = std::get<BoolValue>(showOffhand);

    f.bg = d2d::Color(std::get<ColorValue>(backgroundColor).getMainColor());
    f.text = d2d::Color(std::get<ColorValue>(textColor).getMainColor());
    f.dim = d2d::Color::RGB(150, 158, 175);
    f.gold = d2d::Color::RGB(247, 190, 25);
    f.faint = d2d::Color(1.f, 1.f, 1.f, 0.07f);
    f.track = d2d::Color(1.f, 1.f, 1.f, 0.12f);

    // Health, rounded up like the game's own hearts.
    const bool known = info.health >= 0.f;
    f.maxPoints = std::max(1, static_cast<int>(std::ceil(info.maxHealth)));
    f.healthPoints = known ? std::clamp(static_cast<int>(std::ceil(info.health)), 0, f.maxPoints) : 0;
    f.fraction = known ? std::clamp(info.health / std::max(info.maxHealth, 1.f), 0.f, 1.f) : 0.f;
    if (!known) {
        f.health = f.dim;
    } else if (std::get<BoolValue>(colorByHealth)) {
        f.health = healthGradient(f.fraction);
    } else {
        f.health = d2d::Color(std::get<ColorValue>(healthColor).getMainColor());
    }
    f.goldPoints = known ? std::clamp(static_cast<int>(std::ceil(info.absorption)), 0, 40) : 0;

    // The number: in health points (20/20) or hearts (10/10), with up to two decimals (8.5/10). With no decimals
    // it rounds up, like the game's own hearts.
    const bool inHearts = numberUnit.getSelectedKey() == UnitHearts;
    const int places = std::clamp(static_cast<int>(std::lround(std::get<FloatValue>(decimals).value)), 0, 2);
    auto numberText = [&](float points) {
        float value = inHearts ? points / 2.f : points;
        if (places == 0) value = std::ceil(value - 0.0001f);
        return trimmed(value, places);
    };
    f.healthNow = known ? numberText(info.health) : L"?";
    f.healthMax = numberText(info.maxHealth);
    f.healthText = known ? f.healthNow + L"/" + f.healthMax : L"?";
    f.goldText = f.goldPoints > 0 ? L"+" + numberText(info.absorption) : std::wstring();

    // What to draw for the health: the design's own bar or ring, a row of hearts, or just the number.
    const int displayKey = healthDisplay.getSelectedKey();
    if (displayKey == DisplayDefault) {
        f.display = design.getSelectedKey() == DesignHearts ? 2 : 1;
    } else {
        f.display = displayKey; // DisplayBar = 1, DisplayHearts = 2, DisplayNumber = 3
    }

    // Shapes are drawn straight away; the text is queued and drawn by the final flush, on top of everything.
    mc.flush(false);
    mc.setImmediate(true);

    Vec2 size;
    switch (design.getSelectedKey()) {
    case DesignHearts: size = drawHeartsDesign(f); break;
    case DesignRing: size = drawRing(f); break;
    case DesignTag: size = drawTag(f); break;
    case DesignSideBar: size = drawSideBar(f); break;
    default: size = drawPill(f); break;
    }

    rect.right = rect.left + size.x;
    rect.bottom = rect.top + size.y;

    mc.setImmediate(false);
    dc.flush();
}
