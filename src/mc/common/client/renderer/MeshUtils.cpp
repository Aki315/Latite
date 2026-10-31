#include "pch.h"
#include "MeshUtils.h"
#include "UiDrawGate.h"
#include "mc/Addresses.h"
#include "util/SafeCall.h"

void SDK::MeshHelpers::renderMeshImmediately(class ScreenContext* scn, class Tessellator* tess,
                                             class MaterialPtr* material) {
    if (!SDK::uidraw::meshReady() || !scn || !tess) return;
    char pad[0x58] {};
    using func_t = void (*)(ScreenContext*, Tessellator*, MaterialPtr*, char*);
    if (!latite_safe::run([&] {
            reinterpret_cast<func_t>(Signatures::MeshHelpers_renderMeshImmediately.result)(scn, tess, material, pad);
        })) {
        if (!SDK::uidraw::meshBroken.exchange(true)) {
            Logger::Warn("[UI] MeshHelpers::renderMeshImmediately crashed inside the game; mesh drawing is off");
        }
    }
}
