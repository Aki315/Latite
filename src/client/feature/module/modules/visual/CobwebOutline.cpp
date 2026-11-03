#include "pch.h"
#include "CobwebOutline.h"

#include <util/DrawUtil3D.h>
#include "client/event/events/ClickEvent.h"
#include "client/event/events/PacketReceiveEvent.h"
#include "mc/common/client/game/ClientInstance.h"
#include "mc/common/network/Packet.h"
#include "mc/common/client/renderer/MaterialPtr.h"
#include "mc/common/world/level/BlockSource.h"
#include "mc/common/world/level/block/Block.h"
#include "mc/common/world/level/block/BlockLegacy.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>

namespace {
    using Quad = std::array<Vec3, 4>;

    struct ShadedQuad {
        Quad quad;
        float shade; // How bright the face is, the way the game lights the faces of a block.
        int face;    // axis * 2 + side, the bit in a cobweb's face mask.
    };

    // The frame the Java pack puts on every face of the block: on each of the six faces, a border of width t made of
    // four flat strips (bottom, top, left, right). Coordinates are in blocks, relative to the block's lower corner.
    // The strips sit a hair outside the block, so they never flicker against the faces of neighbouring blocks.
    std::vector<ShadedQuad> frameQuads(float t) {
        constexpr float out = 0.002f;
        std::vector<ShadedQuad> quads;
        quads.reserve(24);

        for (int axis = 0; axis < 3; ++axis) {
            for (int side = 0; side < 2; ++side) {
                const float plane = side == 0 ? -out : 1.f + out;
                // Same brightness the game gives the faces of a block: top 1, east/west 0.6, north/south 0.8, bottom 0.5.
                const float shade = axis == 1 ? (side == 1 ? 1.f : 0.5f) : (axis == 0 ? 0.6f : 0.8f);

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
                    quads.push_back({ { point(u0, v0), point(u1, v0), point(u1, v1), point(u0, v1) }, shade, axis * 2 + side });
                };

                strip(0.f, 0.f, 1.f, t);           // bottom
                strip(0.f, 1.f - t, 1.f, 1.f);     // top
                strip(0.f, t, t, 1.f - t);         // left
                strip(1.f - t, t, 1.f, 1.f - t);   // right
            }
        }
        return quads;
    }

    // A web's position as one number, for looking up whether a neighbour is a web too. x and z within +-33 million,
    // y within -1024..3071.
    uint64_t keyOf(int x, int y, int z) {
        return (static_cast<uint64_t>(x + (1 << 25)) << 38) | (static_cast<uint64_t>(z + (1 << 25)) << 12) |
               static_cast<uint64_t>(y + 1024);
    }

    // The search works for at most this long each frame (checked every few blocks), however big the range is. It is
    // time, not a block count, so a slow computer doesn't stutter and a fast one finishes in fewer frames.
    constexpr std::chrono::microseconds kScanBudget { 900 };
    constexpr size_t kTimeCheckEvery = 128;

    // Packets that mean a block (or a whole chunk) changed: UpdateBlock 21, LevelChunk 58, UpdateBlockSynced 110,
    // UpdateSubChunkBlocks 172. Any of them only asks for an early search, so a wrong id costs one extra search.
    bool isBlockChangePacket(uint8_t id) { return id == 21 || id == 58 || id == 110 || id == 172; }
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
    addSliderSetting("maxOutlines", LocalizeString::get("client.module.cobwebOutline.maxOutlines.name"),
                     LocalizeString::get("client.module.cobwebOutline.maxOutlines.desc"), maxOutlines, FloatValue(25.f),
                     FloatValue(1500.f), FloatValue(25.f));

    addSetting("onlyInView", LocalizeString::get("client.module.cobwebOutline.onlyInView.name"),
               LocalizeString::get("client.module.cobwebOutline.onlyInView.desc"), onlyInView);
    addSliderSetting("viewAngle", LocalizeString::get("client.module.cobwebOutline.viewAngle.name"),
                     LocalizeString::get("client.module.cobwebOutline.viewAngle.desc"), viewAngle, FloatValue(60.f),
                     FloatValue(180.f), FloatValue(5.f), "onlyInView"_istrue);

    Eventing::get().listen<RenderLevelEvent, &CobwebOutline::onRenderLevel>(this);
    // A block that changes (a web placed or broken, by you or anyone else) asks for an early search instead of waiting for
    // the next timed one: this is what made new webs show up late.
    listen<PacketReceiveEvent>((EventListenerFunc)&CobwebOutline::onBlockChangePacket);
    listen<ClickEvent>((EventListenerFunc)&CobwebOutline::onClick);
}

void CobwebOutline::onBlockChangePacket(Event& evG) {
    auto& ev = reinterpret_cast<PacketReceiveEvent&>(evG);
    auto* pkt = ev.getPacket();
    if (pkt && isBlockChangePacket(static_cast<uint8_t>(pkt->getID()))) dirty = true;
}

// You place or break a block on your own screen before the server confirms it.
void CobwebOutline::onClick(Event& evG) {
    auto& ev = reinterpret_cast<ClickEvent&>(evG);
    if (ev.isDown() && ev.getClickType() != ClickEvent::ClickType::Wheel) dirty = true;
}

void CobwebOutline::startScan(int radius, BlockPos center) {
    scanning = true;
    scanRadius = radius;
    scanCenter = center;
    scanIndex = 0;
    building.clear();
}

// Looks at the next slice of the cube around scanCenter. The cube is walked as one long run of blocks, so the search
// can stop after any block and carry on next frame.
void CobwebOutline::stepScan() {
    auto* ci = SDK::ClientInstance::get();
    auto* region = ci ? ci->getRegion() : nullptr;
    if (!region) {
        scanning = false;
        return;
    }

    const size_t side = static_cast<size_t>(scanRadius) * 2 + 1;
    const size_t total = side * side * side;
    const auto deadline = std::chrono::steady_clock::now() + kScanBudget;
    size_t sinceCheck = 0;

    while (scanIndex < total) {
        if (++sinceCheck >= kTimeCheckEvery) {
            sinceCheck = 0;
            if (std::chrono::steady_clock::now() >= deadline) break;
        }
        const size_t index = scanIndex++;
        const int dx = static_cast<int>(index % side) - scanRadius;
        const int dy = static_cast<int>((index / side) % side) - scanRadius;
        const int dz = static_cast<int>(index / (side * side)) - scanRadius;

        const BlockPos bp { scanCenter.x + dx, scanCenter.y + dy, scanCenter.z + dz };
        SDK::Block* block = region->getBlock(bp);
        if (!block) continue;

        SDK::BlockLegacy* legacy = block->legacyBlock;
        if (!legacy) continue;

        auto cached = isWebCache.find(legacy);
        if (cached == isWebCache.end()) {
            cached = isWebCache.emplace(legacy, legacy->namespacedId.getString() == "minecraft:web").first;
        }
        if (cached->second) building.push_back(bp);
    }

    if (scanIndex >= total) finishScan();
}

// The search is done: keep the nearest webs up to the limit, and work out which faces need a frame.
void CobwebOutline::finishScan() {
    auto distanceSq = [&](BlockPos const& bp) {
        const long long dx = bp.x - scanCenter.x, dy = bp.y - scanCenter.y, dz = bp.z - scanCenter.z;
        return dx * dx + dy * dy + dz * dz;
    };

    const size_t limit = static_cast<size_t>(std::get<FloatValue>(maxOutlines).value);
    if (building.size() > limit) {
        std::nth_element(building.begin(), building.begin() + static_cast<std::ptrdiff_t>(limit), building.end(),
                         [&](BlockPos const& l, BlockPos const& r) { return distanceSq(l) < distanceSq(r); });
        building.resize(limit);
    }

    std::unordered_set<uint64_t> present;
    present.reserve(building.size() * 2);
    for (auto const& bp : building) present.insert(keyOf(bp.x, bp.y, bp.z));

    // A face on the negative side is skipped when the web next to it also has a frame there: the neighbour's
    // positive-side face sits in the same place.
    std::vector<uint8_t> masks;
    masks.reserve(building.size());
    for (auto const& bp : building) {
        uint8_t mask = 0x3F;
        if (present.contains(keyOf(bp.x - 1, bp.y, bp.z))) mask &= ~(1 << 0);
        if (present.contains(keyOf(bp.x, bp.y - 1, bp.z))) mask &= ~(1 << 2);
        if (present.contains(keyOf(bp.x, bp.y, bp.z - 1))) mask &= ~(1 << 4);
        masks.push_back(mask);
    }

    webs.swap(building);
    faceMask.swap(masks);
    building.clear();
    hasScanned = true;
    lastRadius = scanRadius;
    lastCenter = scanCenter;
    lastScanDone = std::chrono::steady_clock::now();
    scanning = false;
}

// Keeps only the cobwebs inside the cone you are looking through. A cobweb is a cube, so the cone is widened by the
// angle the cube covers at its distance, and a cobweb right next to you is always kept (it covers most of the view).
void CobwebOutline::pickVisible(Vec3 const& eye, Vec2 const& rot) {
    visible.clear();
    visible.reserve(webs.size());
    if (!std::get<BoolValue>(onlyInView)) {
        for (uint32_t i = 0; i < webs.size(); ++i) visible.push_back(i);
        return;
    }

    constexpr float deg2rad = 3.14159265f / 180.f;
    const float yaw = (rot.y + 90.f) * deg2rad;
    const float pitch = rot.x * -deg2rad;
    const Vec3 dir { std::cos(yaw) * std::cos(pitch), std::sin(pitch), std::sin(yaw) * std::cos(pitch) };
    const float halfCone = std::get<FloatValue>(viewAngle).value * 0.5f * deg2rad;

    for (uint32_t i = 0; i < webs.size(); ++i) {
        const BlockPos& bp = webs[i];
        const Vec3 d { bp.x + 0.5f - eye.x, bp.y + 0.5f - eye.y, bp.z + 0.5f - eye.z };
        const float dist = d.magnitude();
        if (dist < 2.5f) {
            visible.push_back(i);
            continue;
        }
        const float cosAngle = (d.x * dir.x + d.y * dir.y + d.z * dir.z) / dist;
        // Half the angle the cube covers as seen from here (its corners are 0.87 blocks from its center).
        const float margin = std::asin(std::min(1.f, 0.87f / dist));
        const float limit = halfCone + margin;
        if (limit >= 3.14159265f || cosAngle >= std::cos(limit)) visible.push_back(i);
    }
}

void CobwebOutline::onRenderLevel(RenderLevelEvent& event) {
    auto* ci = SDK::ClientInstance::get();
    if (!ci || !ci->getLocalPlayer() || !ci->getRegion()) {
        webs.clear();
        faceMask.clear();
        building.clear();
        hasScanned = false;
        scanning = false;
        return;
    }

    const int radius = static_cast<int>(std::get<FloatValue>(range).value);

    // Start a new search when none is running and it is time: you moved a block from where the last one was centered, the
    // range changed, a block changed, or a quarter of a second has passed. Searches are at least 50 ms apart.
    if (!scanning) {
        const Vec3 pos = ci->getLocalPlayer()->getPos();
        const BlockPos here { static_cast<int>(std::floor(pos.x)), static_cast<int>(std::floor(pos.y)),
                              static_cast<int>(std::floor(pos.z)) };
        const auto sinceDone = std::chrono::steady_clock::now() - lastScanDone;
        const bool movedAway = std::abs(here.x - lastCenter.x) >= 1 || std::abs(here.y - lastCenter.y) >= 1 ||
                               std::abs(here.z - lastCenter.z) >= 1;
        const bool early = sinceDone > std::chrono::milliseconds(50);
        if (!hasScanned || radius != lastRadius || ((movedAway || dirty) && early) ||
            sinceDone > std::chrono::milliseconds(250)) {
            dirty = false;
            startScan(radius, here);
        }
    }
    if (scanning) stepScan();

    if (webs.empty()) return;

    pickVisible(ci->getLocalPlayer()->getPos(), ci->getLocalPlayer()->getRot());
    if (visible.empty()) return;

    auto color = std::get<ColorValue>(outlineColor).getMainColor();

    if (style.getSelectedKey() == StyleLines) {
        auto dc = MCDrawUtil3D(ci->levelRenderer, SDK::ScreenContext::instance3d,
                               SDK::MaterialPtr::getSelectionBoxMaterial());
        constexpr float grow = 0.002f;
        for (uint32_t i : visible) {
            const BlockPos& bp = webs[i];
            AABB bb(Vec3(bp.x - grow, bp.y - grow, bp.z - grow),
                    Vec3(bp.x + 1.f + grow, bp.y + 1.f + grow, bp.z + 1.f + grow));
            dc.drawBox(bb, color);
        }
        dc.flush();
        return;
    }

    // The Java look: a solid frame on every face of every cobweb, lit like the faces of a block. This uses the same
    // material as the thin lines (not the see-through overlay, which blends with whatever is behind it and made the
    // frame look green on grass and brown on dirt).
    auto dc = MCDrawUtil3D(ci->levelRenderer, SDK::ScreenContext::instance3d,
                           SDK::MaterialPtr::getSelectionBoxMaterial());
    const auto quads = frameQuads(std::get<FloatValue>(thickness).value);
    std::vector<d2d::Color> shadedColors;
    shadedColors.reserve(quads.size());
    for (auto const& face : quads) {
        shadedColors.emplace_back(color.r * face.shade, color.g * face.shade, color.b * face.shade, color.a);
    }
    for (uint32_t i : visible) {
        const BlockPos& bp = webs[i];
        const uint8_t mask = faceMask[i];
        const Vec3 origin { static_cast<float>(bp.x), static_cast<float>(bp.y), static_cast<float>(bp.z) };
        for (size_t f = 0; f < quads.size(); ++f) {
            auto const& face = quads[f];
            if (!(mask & (1 << face.face))) continue;
            auto const& q = face.quad;
            dc.fillQuad(origin + q[0], origin + q[1], origin + q[2], origin + q[3], shadedColors[f]);
        }
    }
    dc.flush();
}
