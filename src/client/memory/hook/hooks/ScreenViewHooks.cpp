#include "pch.h"
#include "ScreenViewHooks.h"
#include "PacketHooks.h"
#include "client/event/Eventing.h"
#include "client/event/events/RenderLayerEvent.h"
#include "client/event/events/RenderGameEvent.h"
#include "mc/common/client/gui/controls/VisualTree.h"
#include "mc/common/client/gui/controls/UIControl.h"
#include "../Hooks.h"
#include "util/RttiLog.h"
#include <client/script/PluginManager.h>

namespace {
    std::shared_ptr<Hook> setupAndRenderHook;
}

void __fastcall ScreenViewHooks::setupAndRender(SDK::ScreenView* view, void* ctx) {
    LATITE_TRACE_HOOK("ScreenViewHooks::setupAndRender");
    setupAndRenderHook->oFunc<decltype(&setupAndRender)>()(view, ctx);
    LATITE_TRACE_HOOK("setupAndRender: original returned");
#ifdef LATITE_DEBUG
    {
        static bool dumped = false;
        if (!dumped) {
            dumped = true;
            // What the hook receives: a MinecraftUIRenderContext starts with {vtable, ClientInstance*, ScreenContext*}.
            void* second = nullptr;
            rtti::readPtr(reinterpret_cast<char*>(ctx) + 8, &second);
            Logger::Info("[UI] setupAndRender: view=0x{:X} ctx=0x{:X} ctx+8=0x{:X} (ClientInstance is 0x{:X})",
                         reinterpret_cast<uintptr_t>(view), reinterpret_cast<uintptr_t>(ctx),
                         reinterpret_cast<uintptr_t>(second), reinterpret_cast<uintptr_t>(SDK::ClientInstance::get()));
            rtti::logVtable("UIRenderContext", ctx, 0, 0x30);
            rtti::logPointerMap("ScreenView", view, 0, 0x100);
            rtti::logPointerMap("UIRenderContext", ctx, 0, 0x40);
            // Code and layout dumps so the broken game-side drawing calls can be repaired from the log.
            rtti::logQwords("UIRenderContext", ctx, 0, 0x60);
            void* scn = nullptr;
            rtti::readPtr(reinterpret_cast<char*>(ctx) + 0x10, &scn);
            Logger::Info("[UI] ScreenContext (ctx+0x10) = 0x{:X}", reinterpret_cast<uintptr_t>(scn));
            if (scn) {
                rtti::logQwords("ScreenContext", scn, 0, 0x100);
                rtti::logPointerMap("ScreenContext", scn, 0, 0x100);
            }
            void* vt = nullptr;
            if (rtti::readPtr(ctx, &vt) && vt) {
                for (size_t slot : { 5, 6, 7 }) {
                    void* fn = nullptr;
                    if (rtti::readPtr(reinterpret_cast<char*>(vt) + slot * 8, &fn)) {
                        rtti::logCode(std::format("UIRenderContext.slot{}", slot).c_str(), fn, 0x1C0);
                    }
                }
            }
            rtti::logCode("Tessellator::begin", reinterpret_cast<void*>(Signatures::Tessellator_begin.result), 0x200);
            rtti::logCode("Tessellator::vertex", reinterpret_cast<void*>(Signatures::Tessellator_vertex.result), 0x120);
            rtti::logCode("MeshHelpers::renderMeshImmediately",
                          reinterpret_cast<void*>(Signatures::MeshHelpers_renderMeshImmediately.result), 0x100);
        }
    }
#endif
    RenderLayerEvent ev { view, reinterpret_cast<SDK::MinecraftUIRenderContext*>(ctx) };
    Eventing::get().dispatch(ev);
    LATITE_TRACE_HOOK("setupAndRender: RenderLayerEvent dispatched");

    latiteGuarded(
        [&] {
            if (view->visualTree->rootControl->name == "debug_screen") {
                PluginManager::Event ev { L"render2d", {}, false };
                Latite::getPluginManager().dispatchEvent(ev);
            }
        },
        "Caught SEH exception in setupAndRender (debug_screen check)");

    static bool hasInitPacketSender = false;
    if (!hasInitPacketSender) {
        auto* lp = SDK::ClientInstance::get()->getLocalPlayer();
        if (lp) {
            hasInitPacketSender = true; // one attempt only: a failure here must not repeat every frame
#ifdef LATITE_DEBUG
            latiteGuarded(
                [&] {
                    // Checks of the 1.26.60 Actor/Player layout: +0x10 should be the entt registry, +0x18 the entity id,
                    // and the component found through the registry should be the same one the field points at.
                    auto base = reinterpret_cast<uintptr_t>(lp);
                    void *reg = nullptr, *ent = nullptr, *sv = nullptr, *ab = nullptr, *sup = nullptr;
                    rtti::readPtr(reinterpret_cast<void*>(base + 0x10), &reg);
                    rtti::readPtr(reinterpret_cast<void*>(base + 0x18), &ent);
                    rtti::readPtr(reinterpret_cast<void*>(base + 0x240), &sv);
                    rtti::readPtr(reinterpret_cast<void*>(base + 0x248), &ab);
                    rtti::readPtr(reinterpret_cast<void*>(base + 0x5E0), &sup);
                    Logger::Info("[Player] lp=0x{:X} +0x10=0x{:X} +0x18=0x{:X} stateVector=0x{:X} aabb=0x{:X} "
                                 "supplies=0x{:X}",
                                 base, reinterpret_cast<uintptr_t>(reg), reinterpret_cast<uintptr_t>(ent) & 0xFFFFFFFF,
                                 reinterpret_cast<uintptr_t>(sv), reinterpret_cast<uintptr_t>(ab),
                                 reinterpret_cast<uintptr_t>(sup));
                    Logger::Info("[Player] component lookup: AABBShape=0x{:X} StateVector=0x{:X}",
                                 reinterpret_cast<uintptr_t>(lp->tryGetComponent<SDK::AABBShapeComponent>()),
                                 reinterpret_cast<uintptr_t>(lp->tryGetComponent<SDK::StateVectorComponent>()));
                    rtti::logPointerMap("Player", lp, 0x180, 0x700);
                },
                "Caught SEH exception in the Player layout check");
#endif
            latiteGuarded(
                [&] {
                    auto* sender = lp->packetSender;
                    void* vt = nullptr;
                    if (sender && rtti::readPtr(sender, &vt) && rtti::inImage(reinterpret_cast<uintptr_t>(vt))) {
                        Latite::getHooks().get<PacketHooks>().initPacketSender(sender);
                    } else {
                        Logger::Warn("[Hook] Player.packetSender does not look like a PacketSender; sendToServer hook "
                                     "skipped");
                    }
                },
                "Caught SEH exception in initPacketSender");
        }
    }

    LATITE_TRACE_HOOK("setupAndRender: before RenderGameEvent");
    RenderGameEvent evt {};
    Eventing::get().dispatch(evt);
    LATITE_TRACE_HOOK("setupAndRender: finished");
}

ScreenViewHooks::ScreenViewHooks()
    : HookGroup("ScreenView") {
    setupAndRenderHook =
        addHook(Signatures::ScreenView_setupAndRender.result, setupAndRender, "ScreenView::setupAndRender");
}
