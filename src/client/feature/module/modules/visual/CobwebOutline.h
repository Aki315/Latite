#pragma once
#include "../../Module.h"

#include <chrono>
#include <cstdint>
#include <unordered_set>
#include <unordered_map>
#include <vector>

// Draws an outline around every cobweb near you, like the Java "Cobweb Outline" resource pack.
//
// That pack changes the cobweb's model: besides the two crossed planes of the web it adds the six faces of the block,
// each with a texture that is a 1-pixel frame (1/16 of a block wide). Bedrock can't change the model of a vanilla block
// from a resource pack, so the client draws the same shapes itself, over the web.
class CobwebOutline final : public Module {
public:
    CobwebOutline();
    ~CobwebOutline() override = default;

    void onRenderLevel(RenderLevelEvent& event);

private:
    static constexpr int StyleFrame = 0; // The Java look: a frame on each of the six faces.
    static constexpr int StyleLines = 1; // Thin lines along the edges of the block.

    // The world is searched a slice at a time (a few thousand blocks per frame) instead of all at once, so a large range
    // never causes a stutter, and the result only replaces the old list when the whole search is done.
    void startScan(int radius, BlockPos center);
    void stepScan();
    void finishScan();

    EnumData style;
    ValueType outlineColor = ColorValue(0.89f, 0.91f, 0.91f, 1.f);
    ValueType thickness = FloatValue(0.0625f); // In blocks. 1/16, like the Java pack.
    ValueType range = FloatValue(12.f);
    ValueType maxOutlines = FloatValue(300.f); // Only this many cobwebs (the nearest) are drawn.

    // The cobwebs of the last finished search, nearest first up to the limit, and for each the faces that get a frame:
    // bit (axis * 2 + side). Where two cobwebs touch, the face between them is drawn once, not twice.
    std::vector<BlockPos> webs;
    std::vector<uint8_t> faceMask;
    bool hasScanned = false;
    int lastRadius = 0;
    BlockPos lastCenter { 0, 0, 0 };
    std::chrono::steady_clock::time_point lastScanDone {};

    // The search in progress.
    bool scanning = false;
    int scanRadius = 0;
    BlockPos scanCenter { 0, 0, 0 };
    size_t scanIndex = 0;
    std::vector<BlockPos> building;

    // Whether a block type is a cobweb, so the name is only compared once per block type.
    std::unordered_map<const void*, bool> isWebCache;
};
