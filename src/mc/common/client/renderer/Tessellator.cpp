#include "pch.h"
#include "Tessellator.h"
#include "UiDrawGate.h"
#include "util/SafeCall.h"

bool SDK::uidraw::meshReady() {
    return !meshBroken.load() && Signatures::Tessellator_begin.result && Signatures::Tessellator_vertex.result &&
           Signatures::Tessellator_color.result && Signatures::MeshHelpers_renderMeshImmediately.result;
}

static void tessFailed(const char* what) {
    if (!SDK::uidraw::meshBroken.exchange(true)) {
        Logger::Warn("[UI] {} crashed inside the game; game-side mesh drawing is switched off for this session", what);
    }
}

void SDK::Tessellator::vertex(float x, float y, float z) {
    if (!uidraw::meshReady() || !this) return;
    if (!latite_safe::run([&] {
            reinterpret_cast<void (*)(Tessellator*, float, float, float)>(Signatures::Tessellator_vertex.result)(this, x,
                                                                                                                  y, z);
        })) {
        tessFailed("Tessellator::vertex");
    }
}

void SDK::Tessellator::begin(Primitive fmt, int numVertices) {
    if (!uidraw::meshReady() || !this) return;
    if (!latite_safe::run([&] {
            reinterpret_cast<void (*)(Tessellator*, int, Primitive, int, bool)>(Signatures::Tessellator_begin.result)(
                this, 0, fmt, numVertices, false); // buildFaceData
        })) {
        tessFailed("Tessellator::begin");
    }
}

void SDK::Tessellator::color(float r, float g, float b, float a) {
    if (!uidraw::meshReady() || !this) return;
    if (!latite_safe::run([&] {
            reinterpret_cast<void (*)(Tessellator*, const Color&)>(Signatures::Tessellator_color.result)(
                this, Color { r, g, b, a });
        })) {
        tessFailed("Tessellator::color");
    }
}

void SDK::Tessellator::color(Color const& col) {
    color(col.r, col.g, col.b, col.a);
}
