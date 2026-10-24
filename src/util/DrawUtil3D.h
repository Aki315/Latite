#include "DrawContext.h"

namespace SDK {
    class LevelRenderer;
    class ScreenContext;
    class MaterialPtr;
}

class MCDrawUtil3D {
private:
    SDK::LevelRenderer* levelRenderer;
    SDK::ScreenContext* screenContext;
    SDK::MaterialPtr* material;
    float lineThickness = 1.f;

    void drawThickLine(Vec3 const& pos1, Vec3 const& pos2, d2d::Color const& color);

public:
    MCDrawUtil3D(SDK::LevelRenderer* renderer, SDK::ScreenContext* ctx, SDK::MaterialPtr* material = nullptr);

    void setMaterial(SDK::MaterialPtr* material) { this->material = material; }

    // Width of drawLine / drawQuad / drawBox lines in screen pixels. 1 (default) is the game's own thin line.
    // Anything above that is drawn as a flat strip that always faces the camera.
    void setLineThickness(float pixels) { lineThickness = pixels < 1.f ? 1.f : pixels; }

    void drawLine(Vec3 const& pos1, Vec3 const& pos2, d2d::Color const& color, bool immediate = false);
    void drawQuad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, d2d::Color const& col);
    void fillQuad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, d2d::Color const& color);
    void drawBox(AABB const& box, d2d::Color const& color);
    void flush();
};
