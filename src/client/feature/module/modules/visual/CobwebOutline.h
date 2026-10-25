#pragma once
#include "../../Module.h"

#include <chrono>
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

    void rescan(int range);

    EnumData style;
    ValueType outlineColor = ColorValue(0.89f, 0.91f, 0.91f, 1.f);
    ValueType thickness = FloatValue(0.0625f); // In blocks. 1/16, like the Java pack.
    ValueType range = FloatValue(12.f);

    // Cobwebs found in the last scan, refreshed a few times per second instead of every frame.
    std::vector<BlockPos> webs;
    BlockPos lastCenter { 0, 0, 0 };
    bool hasScanned = false;
    std::chrono::steady_clock::time_point lastScan {};

    // Whether a block type is a cobweb, so the name is only compared once per block type.
    std::unordered_map<const void*, bool> isWebCache;
};
