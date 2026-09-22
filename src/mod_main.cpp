// mod_main.cpp — the Dusklight mod entry point.
//
// Per Dusklight's native mod ABI (docs/modding.md): a mod exports
// mod_initialize(), mod_update(), and mod_shutdown(), which Dusklight
// dynamically links and calls on the game's main thread. This file wires
// those lifecycle functions to a background TCP command server and an
// effect dispatcher.
//
// Check these function signatures against the sdk/ headers in your own
// Dusklight checkout before building - some versions pass extra arguments,
// such as a handle to the service registry. Dusklight's starter project
// (TwilitRealm/mod-template) is where to begin; copy this file's logic
// into the project it generates.

#include "command_server.h"
#include "effects.h"

#include <chrono>
#include <memory>

namespace chickencontrol {

namespace {
std::unique_ptr<CommandServer> g_server;
std::unique_ptr<EffectRunner> g_effects;
std::chrono::steady_clock::time_point g_last_tick;

constexpr const char* kListenHost = "127.0.0.1";
constexpr uint16_t kListenPort = 47201; // must match launcher/config.py's default mod_port
} // namespace

extern "C" void mod_initialize() {
    g_server = std::make_unique<CommandServer>(kListenHost, kListenPort);
    g_server->start();

    g_effects = std::make_unique<EffectRunner>();
    g_last_tick = std::chrono::steady_clock::now();
}

extern "C" void mod_update() {
    if (!g_server || !g_effects) return;

    auto now = std::chrono::steady_clock::now();
    double delta = std::chrono::duration<double>(now - g_last_tick).count();
    g_last_tick = now;

    // Drain and dispatch every command received since the last tick. This
    // runs on the main thread (mod_update is called from it), so handlers
    // are free to touch game state directly.
    for (const EffectCommand& cmd : g_server->drain_commands()) {
        g_effects->dispatch(cmd);
    }

    g_effects->tick(delta);
}

extern "C" void mod_shutdown() {
    // Make sure nothing is left frozen or inverted.
    if (g_effects) g_effects->endEverything();
    if (g_server) g_server->stop();
    g_server.reset();
    g_effects.reset();
}

} // namespace chickencontrol
