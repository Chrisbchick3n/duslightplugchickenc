#include "command_server.h"
#include "mini_json.h"

#include <cstring>
#include <iostream>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  using socket_t = SOCKET;
  static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
  #define CLOSESOCK closesocket
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  using socket_t = int;
  static constexpr socket_t kInvalidSocket = -1;
  #define CLOSESOCK close
#endif

namespace chickencontrol {

namespace {
#if defined(_WIN32)
struct WinsockGuard {
    WinsockGuard() {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
    ~WinsockGuard() { WSACleanup(); }
};
static WinsockGuard g_winsock_guard;
#endif
} // namespace

CommandServer::CommandServer(std::string host, uint16_t port)
    : host_(std::move(host)), port_(port) {}

CommandServer::~CommandServer() { stop(); }

void CommandServer::start() {
    if (running_.exchange(true)) return;
    accept_thread_ = std::thread(&CommandServer::run_accept_loop, this);
}

void CommandServer::stop() {
    if (!running_.exchange(false)) return;
    // Don't close listen_socket_ here: run_accept_loop() owns it and closes
    // it itself once its select()-based poll notices running_ is false.
    // Closing it concurrently from this thread while the accept thread may
    // still be inside select()/accept() on the same fd is a data race.
    if (accept_thread_.joinable()) accept_thread_.join();
}

void CommandServer::run_accept_loop() {
    listen_socket_ = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
    if (listen_socket_ == kInvalidSocket) {
        std::cerr << "[chicken-control] failed to create socket\n";
        running_ = false;
        return;
    }

    int reuse = 1;
    setsockopt(listen_socket_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    // Bind to loopback only — this server must never be reachable from
    // outside the streamer's own machine.
    inet_pton(AF_INET, host_.c_str(), &addr.sin_addr);

    if (bind(listen_socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "[chicken-control] failed to bind " << host_ << ":" << port_ << "\n";
        CLOSESOCK(listen_socket_);
        listen_socket_ = kInvalidSocket;
        running_ = false;
        return;
    }

    listen(listen_socket_, 4);
    std::cerr << "[chicken-control] command server listening on " << host_ << ":" << port_ << "\n";

    // accept() blocks indefinitely, and closing listen_socket_ from another
    // thread (as stop() does) is not reliably enough to unblock it on all
    // platforms/timings — a real accept() call was observed to hang past
    // the close in testing. Instead, poll readiness with a short select()
    // timeout so the loop wakes up on its own to re-check running_.
    while (running_) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(listen_socket_, &read_fds);
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 200 * 1000; // 200ms: responsive enough for stop() without busy-looping

        int ready = select(static_cast<int>(listen_socket_) + 1, &read_fds, nullptr, nullptr, &tv);
        if (ready <= 0) continue; // timeout or interrupted — loop back and re-check running_

        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        socket_t client = accept(listen_socket_,
                                  reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client == kInvalidSocket) {
            if (!running_) break;
            continue;
        }
        // One launcher connects at a time in the common case; handle
        // synchronously on a dedicated thread per connection so a slow or
        // dropped launcher never blocks new connections.
        std::thread(&CommandServer::handle_client, this, static_cast<int>(client)).detach();
    }

    if (listen_socket_ != kInvalidSocket) {
        CLOSESOCK(listen_socket_);
        listen_socket_ = kInvalidSocket;
    }
}

void CommandServer::handle_client(int client_socket) {
    std::string buffer;
    char chunk[4096];
    while (running_) {
        int n = recv(client_socket, chunk, sizeof(chunk), 0);
        if (n <= 0) break;
        buffer.append(chunk, n);

        size_t newline;
        while ((newline = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, newline);
            buffer.erase(0, newline + 1);
            if (!line.empty()) push_command(line);
        }
    }
    CLOSESOCK(client_socket);
}

void CommandServer::push_command(const std::string& json_line) {
    try {
        JsonValue root = JsonParser::parse(json_line);
        std::string cmd = root.get("cmd").as_string();
        if (cmd != "effect") return; // only command type defined so far

        EffectCommand ec;
        ec.id = root.get("id").as_string();
        ec.params = root.get("params"); // Null JsonValue if absent; handlers use .get(key).as_*(fallback)

        std::lock_guard<std::mutex> lock(queue_mutex_);
        queue_.push_back(std::move(ec));
    } catch (const std::exception& e) {
        std::cerr << "[chicken-control] failed to parse command: " << e.what()
                   << " (" << json_line << ")\n";
    }
}

std::deque<EffectCommand> CommandServer::drain_commands() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    std::deque<EffectCommand> out;
    out.swap(queue_);
    return out;
}

} // namespace chickencontrol
