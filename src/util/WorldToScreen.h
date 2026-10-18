#pragma once
#include <optional>
#include "LMath.h"

namespace WorldToScreen {
    // applyZoom: follow the Zoom module's magnification, which the cached game matrices don't include.
    std::optional<Vec2> convert(const Vec3& worldPos, bool applyZoom = true);
};
