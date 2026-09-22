// command_server.h — a tiny localhost-only TCP server that accepts
// newline-delimited JSON effect commands from the Chicken Control launcher
// and hands them off to a thread-safe queue that mod_update() drains on
// the game's main thread.
//
// IMPORTANT: game state must only ever be touched from mod_update(), which
// Dusklight calls on the main thread. The socket accept/recv loop runs on
// its own background thread, so it never calls into game code directly —
// it only pushes parsed EffectCommand structs into effect_queue_.

#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <deque>

#include "mini_json.h"

namespace chickencontrol {

struct EffectCommand {
    std::string id;
    JsonValue params; // object JsonValue; use params.get("key").as_int(...) etc.
};

class CommandServer {
public:
    CommandServer(std::string host, uint16_t port);
    ~CommandServer();

    // Starts the accept-loop thread. Safe to call once.
    void start();

    // Stops the server and joins the background thread.
    void stop();

    // Pops all commands received since the last call. Call this once per
    // mod_update() tick from the main thread only.
    std::deque<EffectCommand> drain_commands();

private:
    void run_accept_loop();
    void handle_client(int client_socket);
    void push_command(const std::string& json_line);

    std::string host_;
    uint16_t port_;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    int listen_socket_ = -1;

    std::mutex queue_mutex_;
    std::deque<EffectCommand> queue_;
};

} // namespace chickencontrol
