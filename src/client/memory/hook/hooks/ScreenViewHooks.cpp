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
