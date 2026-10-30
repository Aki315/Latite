#include "pch.h"
#include "ScreenViewHooks.h"
#include "PacketHooks.h"
#include "client/event/Eventing.h"
#include "client/event/events/RenderLayerEvent.h"
#include "client/event/events/RenderGameEvent.h"
#include "mc/common/client/gui/controls/VisualTree.h"
#include "mc/common/client/gui/controls/UIControl.h"
#include "../Hooks.h"
#include <client/script/PluginManager.h>

namespace {
    std::shared_ptr<Hook> setupAndRenderHook;
}

void __fastcall ScreenViewHooks::setupAndRender(SDK::ScreenView* view, void* ctx) {
    LATITE_TRACE_HOOK("ScreenViewHooks::setupAndRender");
    setupAndRenderHook->oFunc<decltype(&setupAndRender)>()(view, ctx);
    LATITE_TRACE_HOOK("setupAndRender: original returned");
    RenderLayerEvent ev { view, reinterpret_cast<SDK::MinecraftUIRenderContext*>(ctx) };
    Eventing::get().dispatch(ev);
    LATITE_TRACE_HOOK("setupAndRender: RenderLayerEvent dispatched");

    if (view->visualTree->rootControl->name == "debug_screen") {
        PluginManager::Event ev { L"render2d", {}, false };
        Latite::getPluginManager().dispatchEvent(ev);
    }

    static bool hasInitPacketSender = false;
    if (!hasInitPacketSender) {
        if (SDK::ClientInstance::get()->getLocalPlayer()) {
            Latite::getHooks().get<PacketHooks>().initPacketSender(
                SDK::ClientInstance::get()->getLocalPlayer()->packetSender);
            hasInitPacketSender = true;
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
