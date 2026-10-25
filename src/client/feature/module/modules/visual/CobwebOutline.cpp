#include "pch.h"
#include "CobwebOutline.h"

#include <util/DrawUtil3D.h>
#include "mc/common/client/game/ClientInstance.h"
#include "mc/common/client/renderer/MaterialPtr.h"
#include "mc/common/world/level/BlockSource.h"
#include "mc/common/world/level/block/Block.h"
#include "mc/common/world/level/block/BlockLegacy.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace {
    using Quad = std::array<Vec3, 4>;

    // The frame the Java pack puts on every face of the block: on each of the six faces, a border of width t made of
    // four flat strips (bottom, top, left, right). Coordinates are in blocks, relative to the block's lower corner.
    // The strips sit a hair outside the block, so they never flicker against the faces of neighbouring blocks.
    std::vector<Quad> frameQuads(float t) {
        constexpr float out = 0.002f;
        std::vector<Quad> quads;
        quads.reserve(24);

        for (int axis = 0; axis < 3; ++axis) {
            for (int side = 0; side < 2; ++side) {
                const float plane = side == 0 ? -out : 1.f + out;

                // A point on this face. u and v run along the face: for the x faces (y, z), for the y faces (x, z) and
                // for the z faces (x, y).
                auto point = [&](float u, float v) {
                    switch (axis) {
                    case 0: return Vec3 { plane, u, v };
                    case 1: return Vec3 { u, plane, v };
                    default: return Vec3 { u, v, plane };
                    }
                };
                auto strip = [&](float u0, float v0, float u1, float v1) {
                    quads.push_back({ point(u0, v0), point(u1, v0), point(u1, v1), point(u0, v1) });
                };

                strip(0.f, 0.f, 1.f, t);           // bottom
                strip(0.f, 1.f - t, 1.f, 1.f);     // top
                strip(0.f, t, t, 1.f - t);         // left
                strip(1.f - t, t, 1.f, 1.f - t);   // right
            }
        }
        return quads;
    }
} // namespace

CobwebOutline::CobwebOutline()
    : Module("CobwebOutline", LocalizeString::get("client.module.cobwebOutline.name"),
             LocalizeString::get("client.module.cobwebOutline.desc"), GAME) {
    style.addEntry(EnumEntry(StyleFrame, LocalizeString::get("client.module.cobwebOutline.style.frame.name"),
                             LocalizeString::get("client.module.cobwebOutline.style.frame.desc")));
    style.addEntry(EnumEntry(StyleLines, LocalizeString::get("client.module.cobwebOutline.style.lines.name"),
                             LocalizeString::get("client.module.cobwebOutline.style.lines.desc")));
    addEnumSetting("style", LocalizeString::get("client.module.cobwebOutline.style.name"),
                   LocalizeString::get("client.module.cobwebOutline.style.desc"), style);
    addSetting("outlineColor", LocalizeString::get("client.module.cobwebOutline.outlineColor.name"),
               LocalizeString::get("client.module.cobwebOutline.outlineColor.desc"), outlineColor);
    addSliderSetting("thickness", LocalizeString::get("client.module.cobwebOutline.thickness.name"),
                     LocalizeString::get("client.module.cobwebOutline.thickness.desc"), thickness, FloatValue(0.02f),
                     FloatValue(0.15f), FloatValue(0.005f));
    addSliderSetting("range", LocalizeString::get("client.module.cobwebOutline.range.name"),
                     LocalizeString::get("client.module.cobwebOutline.range.desc"), range, FloatValue(2.f),
                     FloatValue(24.f), FloatValue(1.f));

    Eventing::get().listen<RenderLevelEvent, &CobwebOutline::onRenderLevel>(this);
}

void CobwebOutline::rescan(int radius) {
    auto* ci = SDK::ClientInstance::get();
    auto* region = ci ? ci->getRegion() : nullptr;
    auto* lp = ci ? ci->getLocalPlayer() : nullptr;
    if (!region || !lp) return;

    const Vec3 pos = lp->getPos();
    const BlockPos center { static_cast<int>(std::floor(pos.x)), static_cast<int>(std::floor(pos.y)),
                            static_cast<int>(std::floor(pos.z)) };

    webs.clear();
    for (int dx = -radius; dx <= radius; ++dx) {
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dz = -radius; dz <= radius; ++dz) {
                const BlockPos bp { center.x + dx, center.y + dy, center.z + dz };
                SDK::Block* block = region->getBlock(bp);
                if (!block) continue;

                SDK::BlockLegacy* legacy = block->legacyBlock;
                if (!legacy) continue;

                auto cached = isWebCache.find(legacy);
                if (cached == isWebCache.end()) {
                    cached = isWebCache.emplace(legacy, legacy->namespacedId.getString() == "minecraft:web").first;
                }
                if (cached->second) webs.push_back(bp);
            }
        }
    }

    lastCenter = center;
    hasScanned = true;
    lastScan = std::chrono::steady_clock::now();
}

void CobwebOutline::onRenderLevel(RenderLevelEvent& event) {
    auto* ci = SDK::ClientInstance::get();
    if (!ci || !ci->getLocalPlayer() || !ci->getRegion()) {
        webs.clear();
        hasScanned = false;
        return;
    }

    const int radius = static_cast<int>(std::get<FloatValue>(range).value);

    // Rescan a few times a second, or right away when you step into a new block.
    const Vec3 pos = ci->getLocalPlayer()->getPos();
    const BlockPos here { static_cast<int>(std::floor(pos.x)), static_cast<int>(std::floor(pos.y)),
                          static_cast<int>(std::floor(pos.z)) };
    const bool moved = here.x != lastCenter.x || here.y != lastCenter.y || here.z != lastCenter.z;
    const auto now = std::chrono::steady_clock::now();
    if (!hasScanned || moved || now - lastScan > std::chrono::milliseconds(250)) {
        rescan(radius);
    }
    if (webs.empty()) return;

    auto color = std::get<ColorValue>(outlineColor).getMainColor();

    if (style.getSelectedKey() == StyleLines) {
        auto dc = MCDrawUtil3D(ci->levelRenderer, SDK::ScreenContext::instance3d,
                               SDK::MaterialPtr::getSelectionBoxMaterial());
        constexpr float grow = 0.002f;
        for (auto const& bp : webs) {
            AABB bb(Vec3(bp.x - grow, bp.y - grow, bp.z - grow),
                    Vec3(bp.x + 1.f + grow, bp.y + 1.f + grow, bp.z + 1.f + grow));
            dc.drawBox(bb, color);
        }
        dc.flush();
        return;
    }

    // The Java look: a frame on every face of every cobweb.
    auto dc = MCDrawUtil3D(ci->levelRenderer, SDK::ScreenContext::instance3d,
                           SDK::MaterialPtr::getSelectionOverlayMaterial());
    const auto quads = frameQuads(std::get<FloatValue>(thickness).value);
    for (auto const& bp : webs) {
        const Vec3 origin { static_cast<float>(bp.x), static_cast<float>(bp.y), static_cast<float>(bp.z) };
        for (auto const& quad : quads) {
            dc.fillQuad(origin + quad[0], origin + quad[1], origin + quad[2], origin + quad[3], color);
        }
    }
    dc.flush();
}
