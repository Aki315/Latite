#include "pch.h"
#include "DrawUtil3D.h"
#include "mc/common/client/renderer/MeshUtils.h"
#include <mc/common/client/renderer/Tessellator.h>
#include <mc/common/client/renderer/UiDrawGate.h>

#include <algorithm>
#include <cmath>

namespace {
    // About how wide one screen pixel is, in blocks, per block of distance (roughly a 70 degree fov at 1080p).
    constexpr float kBlocksPerPixelPerBlock = 0.0015f;

    Vec3 cross(Vec3 const& a, Vec3 const& b) {
        return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
    }

    // The four corners of a flat strip along a -> b that faces the camera. Its width is the same number of screen
    // pixels everywhere, so it is scaled with the distance to the camera at each end. Returns false for a line
    // that has no length.
    bool makeStrip(Vec3 const& a, Vec3 const& b, Vec3 const& camera, float pixels, Vec3 (&out)[4]) {
        const Vec3 dir = b - a;
        const float len = dir.magnitude();
        if (len < 1e-5f) return false;

        const Vec3 toCamera = camera - (a + b) * 0.5f;
        Vec3 side = cross(dir, toCamera);
        if (side.magnitude() < 1e-3f * len * std::max(toCamera.magnitude(), 1e-3f)) {
            // The line points straight at the camera, so any direction across it will do.
            const Vec3 helper = std::fabs(dir.y) < 0.9f * len ? Vec3(0.f, 1.f, 0.f) : Vec3(1.f, 0.f, 0.f);
            side = cross(dir, helper);
        }
        side = side.normalized();

        const float halfA = pixels * std::max(a.distance(camera), 0.5f) * kBlocksPerPixelPerBlock * 0.5f;
        const float halfB = pixels * std::max(b.distance(camera), 0.5f) * kBlocksPerPixelPerBlock * 0.5f;

        out[0] = a - side * halfA;
        out[1] = a + side * halfA;
        out[2] = b + side * halfB;
        out[3] = b - side * halfB;
        return true;
    }
} // namespace

MCDrawUtil3D::MCDrawUtil3D(SDK::LevelRenderer* renderer, SDK::ScreenContext* ctx, SDK::MaterialPtr* material)
    : levelRenderer(renderer)
    , screenContext(ctx)
    , material(material) {
    if (!material) {
        this->material = SDK::MaterialPtr::getSelectionBoxMaterial();
    }
}

void MCDrawUtil3D::drawThickLine(Vec3 const& p1, Vec3 const& p2, d2d::Color const& color) {
    Vec3 strip[4];
    const Vec3 camera = levelRenderer->getLevelRendererPlayer()->getOrigin();
    if (!makeStrip(p1, p2, camera, lineThickness, strip)) return;
    fillQuad(strip[0], strip[1], strip[2], strip[3], color);
}

void MCDrawUtil3D::drawLine(Vec3 const& p1, Vec3 const& p2, d2d::Color const& color, bool immediate) {
    if (lineThickness > 1.01f) {
        drawThickLine(p1, p2, color);
        if (immediate) flush();
        return;
    }

    if (!SDK::uidraw::meshReady() || !screenContext || !screenContext->tess) return;
    auto scn = screenContext;
    auto tess = scn->tess;
    *scn->shaderColor = { 1.f, 1.f, 1.f, 1.f };
    tess->begin(SDK::Primitive::LineList, 1);
    tess->color(color);
    auto& origin = levelRenderer->getLevelRendererPlayer()->getOrigin();
    Vec3 a = p1;
    Vec3 b = p2;

    a.x -= origin.x;
    a.y -= origin.y;
    a.z -= origin.z;

    b.x -= origin.x;
    b.y -= origin.y;
    b.z -= origin.z;
    tess->vertex(a.x, a.y, a.z);
    tess->vertex(b.x, b.y, b.z);
    if (immediate) flush();
}

void MCDrawUtil3D::drawQuad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, d2d::Color const& col) {
    if (lineThickness > 1.01f) {
        drawThickLine(a, b, col);
        drawThickLine(b, c, col);
        drawThickLine(c, d, col);
        drawThickLine(d, a, col);
        return;
    }

    if (!SDK::uidraw::meshReady() || !screenContext || !screenContext->tess) return;
    auto scn = screenContext;
    auto tess = scn->tess;
    *scn->shaderColor = { 1.f, 1.f, 1.f, 1.f };
    tess->begin(SDK::Primitive::LineList, 1);
    tess->color(col);
    auto origin = levelRenderer->getLevelRendererPlayer()->getOrigin();
    a = a - (origin);
    b = b - (origin);
    c = c - (origin);
    d = d - (origin);

    tess->vertex(a.x, a.y, a.z);
    tess->vertex(b.x, b.y, b.z);

    tess->begin(SDK::Primitive::LineList, 1);
    tess->color(col);
    tess->vertex(b.x, b.y, b.z);
    tess->vertex(c.x, c.y, c.z);

    tess->begin(SDK::Primitive::LineList, 1);
    tess->color(col);
    tess->vertex(c.x, c.y, c.z);
    tess->vertex(d.x, d.y, d.z);

    tess->begin(SDK::Primitive::LineList, 1);
    tess->color(col);
    tess->vertex(d.x, d.y, d.z);
    tess->vertex(a.x, a.y, a.z);
}

void MCDrawUtil3D::fillQuad(Vec3 p1, Vec3 p2, Vec3 p3, Vec3 p4, d2d::Color const& color) {
    if (!SDK::uidraw::meshReady() || !screenContext || !screenContext->tess) return;
    auto scn = screenContext;

    auto tess = scn->tess;
    *scn->shaderColor = Color(1.f, 1.f, 1.f, 1.f);
    tess->begin(SDK::Primitive::Quad, 4);
    tess->color(color);
    auto& origin = levelRenderer->getLevelRendererPlayer()->getOrigin();
    Vec3 a = p1;
    Vec3 b = p2;
    Vec3 c = p3;
    Vec3 d = p4;

    a.x -= origin.x;
    a.y -= origin.y;
    a.z -= origin.z;

    b.x -= origin.x;
    b.y -= origin.y;
    b.z -= origin.z;

    c.x -= origin.x;
    c.y -= origin.y;
    c.z -= origin.z;

    d.x -= origin.x;
    d.y -= origin.y;
    d.z -= origin.z;

    tess->vertex(a.x, a.y, a.z);
    tess->vertex(b.x, b.y, b.z);
    tess->vertex(c.x, c.y, c.z);
    tess->vertex(d.x, d.y, d.z);

    // render other side
    tess->vertex(d.x, d.y, d.z);
    tess->vertex(c.x, c.y, c.z);
    tess->vertex(b.x, b.y, b.z);
    tess->vertex(a.x, a.y, a.z);
}

void MCDrawUtil3D::drawBox(AABB const& bb, d2d::Color const& color) {
    auto scn = screenContext;
    auto tess = scn->tess;
    std::vector<std::pair<Vec3, Vec3>> verticies = {
        { bb.lower, Vec3(bb.lower.x, bb.higher.y, bb.lower.z) },
        { Vec3(bb.higher.x, bb.lower.y, bb.lower.z), Vec3(bb.higher.x, bb.higher.y, bb.lower.z) },
        { Vec3(bb.lower.x, bb.lower.y, bb.higher.z), Vec3(bb.lower.x, bb.higher.y, bb.higher.z) },
        { Vec3(bb.higher.x, bb.lower.y, bb.higher.z), Vec3(bb.higher.x, bb.higher.y, bb.higher.z) },
    };
    for (auto& vert : verticies) {
        drawLine(vert.first, vert.second, color);
    }

    drawQuad(Vec3(bb.lower.x, bb.lower.y, bb.lower.z), Vec3(bb.higher.x, bb.lower.y, bb.lower.z),
             Vec3(bb.higher.x, bb.lower.y, bb.higher.z), Vec3(bb.lower.x, bb.lower.y, bb.higher.z), color);

    drawQuad(Vec3(bb.lower.x, bb.higher.y, bb.lower.z), Vec3(bb.higher.x, bb.higher.y, bb.lower.z),
             Vec3(bb.higher.x, bb.higher.y, bb.higher.z), Vec3(bb.lower.x, bb.higher.y, bb.higher.z), color);

    drawQuad(Vec3(bb.lower.x, bb.higher.y, bb.lower.z), Vec3(bb.higher.x, bb.higher.y, bb.lower.z),
             Vec3(bb.higher.x, bb.higher.y, bb.higher.z), Vec3(bb.lower.x, bb.higher.y, bb.higher.z), color);
}

void MCDrawUtil3D::flush() {
    if (!SDK::uidraw::meshReady()) return;
    SDK::MeshHelpers::renderMeshImmediately(screenContext, screenContext->tess, material);
}
