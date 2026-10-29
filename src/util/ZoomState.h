#pragma once

// How much the Zoom module is magnifying the view right now (1 = not zoomed).
// The Zoom module writes it, WorldToScreen reads it so on-screen markers follow the zoomed picture.
namespace ZoomState {
    inline float factor = 1.f;
}
