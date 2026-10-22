// [gta-recomp] Hidden test runs and remote control: see runtime/ps2_control.h.
#include "runtime/ps2_control.h"
#include "runtime/ps2_perf.h"

#include "platform/platform.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
static constexpr SocketHandle kNoSocket = INVALID_SOCKET;
static void closeSocket(SocketHandle s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
static constexpr SocketHandle kNoSocket = -1;
static void closeSocket(SocketHandle s) { close(s); }
#endif

namespace ps2control
{
    namespace
    {
        bool envOn(const char *name)
        {
            const char *v = std::getenv(name);
            return v && *v && *v != '0';
        }

        constexpr int kKeys = 512;
        std::array<std::atomic<bool>, kKeys> g_injected{};
        std::array<bool, kKeys> g_injectedEdge{}; // main thread: went down since the last pump

        struct Command
        {
            std::string line;
            std::promise<std::string> reply;
        };
        std::mutex g_queueMutex;
        std::deque<std::shared_ptr<Command>> g_queue;

        struct Release
        {
            int key;
            std::chrono::steady_clock::time_point at;
            std::shared_ptr<Command> command;
        };
        std::vector<Release> g_releases; // main thread

        std::atomic<bool> g_running{false};
        std::thread g_server;
        SocketHandle g_listen = kNoSocket;

        // platform::Key for a name: letters, digits, F1-F12 and the names below (case-insensitive).
        int keyFromName(std::string name)
        {
            for (char &c : name)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            struct Named { const char *name; int key; };
            static const Named named[] = {
                {"return", platform::KEY_ENTER}, {"enter", platform::KEY_ENTER}, {"start", platform::KEY_ENTER}, {"shift", platform::KEY_LEFT_SHIFT}, {"shiftkey", platform::KEY_LEFT_SHIFT},
                {"cross", platform::KEY_LEFT_SHIFT}, {"space", platform::KEY_SPACE}, {"square", platform::KEY_SPACE}, {"triangle", platform::KEY_F}, {"ctrl", platform::KEY_LEFT_CONTROL},
                {"controlkey", platform::KEY_LEFT_CONTROL}, {"circle", platform::KEY_LEFT_CONTROL}, {"escape", platform::KEY_ESCAPE}, {"esc", platform::KEY_ESCAPE},
                {"backspace", platform::KEY_BACKSPACE}, {"back", platform::KEY_BACKSPACE}, {"select", platform::KEY_BACKSPACE}, {"tab", platform::KEY_TAB}, {"up", platform::KEY_UP},
                {"down", platform::KEY_DOWN}, {"left", platform::KEY_LEFT}, {"right", platform::KEY_RIGHT},
            };
            for (const Named &n : named)
                if (name == n.name)
                    return n.key;
            if (name.size() == 1 && std::isalpha(static_cast<unsigned char>(name[0])))
                return platform::KEY_A + (name[0] - 'a');
            if (name.size() == 1 && std::isdigit(static_cast<unsigned char>(name[0])))
                return platform::KEY_ZERO + (name[0] - '0');
            if (name.size() >= 2 && name[0] == 'f')
            {
                const int n = std::atoi(name.c_str() + 1);
                if (n >= 1 && n <= 12)
                    return platform::KEY_F1 + n - 1;
            }
            return -1;
        }

        void setInjected(int key, bool down)
        {
            if (down && !g_injected[key].load())
                g_injectedEdge[key] = true;
            g_injected[key].store(down);
        }

        void serve(SocketHandle client)
        {
            std::string buffer;
            char chunk[512];
            for (;;)
            {
                const int n = static_cast<int>(recv(client, chunk, sizeof(chunk), 0));
                if (n <= 0)
                    break;
                buffer.append(chunk, static_cast<size_t>(n));
                for (size_t nl; (nl = buffer.find('\n')) != std::string::npos;)
                {
                    std::string line = buffer.substr(0, nl);
                    buffer.erase(0, nl + 1);
                    if (!line.empty() && line.back() == '\r')
                        line.pop_back();
                    auto command = std::make_shared<Command>();
                    command->line = line;
                    std::future<std::string> reply = command->reply.get_future();
                    {
                        std::lock_guard<std::mutex> lock(g_queueMutex);
                        g_queue.push_back(command);
                    }
                    std::string answer = reply.wait_for(std::chrono::seconds(60)) == std::future_status::ready
                                             ? reply.get()
                                             : std::string("error timed out (is the game loop running?)");
                    answer += '\n';
                    send(client, answer.data(), static_cast<int>(answer.size()), 0);
                }
            }
            closeSocket(client);
        }

        void serverLoop()
        {
            while (g_running.load())
            {
                SocketHandle client = accept(g_listen, nullptr, nullptr);
                if (client == kNoSocket)
                    break;
                serve(client);
            }
        }

        void execute(const std::shared_ptr<Command> &command, const std::function<std::string(const std::string &)> &capture,
                     const std::function<void()> &quit)
        {
            std::istringstream in(command->line);
            std::string verb;
            in >> verb;
            if (verb == "ping")
                command->reply.set_value("ok");
            else if (verb == "screenshot")
            {
                std::string path;
                std::getline(in >> std::ws, path);
                if (path.empty())
                {
                    command->reply.set_value("error usage: screenshot <path.png>");
                    return;
                }
                const std::string size = capture(path);
                command->reply.set_value(size.empty() ? "error could not write " + path : "ok " + size);
            }
            else if (verb == "key" || verb == "press")
            {
                std::string name, arg;
                in >> name >> arg;
                const int key = keyFromName(name);
                if (key < 0 || key >= kKeys)
                {
                    command->reply.set_value("error unknown key '" + name + "'");
                    return;
                }
                if (verb == "key")
                {
                    if (arg != "down" && arg != "up")
                    {
                        command->reply.set_value("error usage: key <name> down|up");
                        return;
                    }
                    setInjected(key, arg == "down");
                    command->reply.set_value("ok");
                    return;
                }
                const int ms = arg.empty() ? 150 : std::max(1, std::atoi(arg.c_str()));
                setInjected(key, true);
                g_releases.push_back({key, std::chrono::steady_clock::now() + std::chrono::milliseconds(ms), command});
            }
            else if (verb == "perf")
                command->reply.set_value("ok " + ps2perf::summaryLine());
            else if (verb == "quit")
            {
                command->reply.set_value("ok");
                quit();
            }
            else
                command->reply.set_value("error unknown command '" + verb + "' (ping, screenshot, key, press, perf, quit)");
        }
    }

    bool hiddenMode()
    {
        static const bool hidden = envOn("PS2X_HIDDEN");
        return hidden;
    }

    bool physicalInputOn()
    {
        static const bool on = [] {
            const char *v = std::getenv("PS2X_INPUT");
            return !hiddenMode() && !(v && std::string(v) == "injected");
        }();
        return on;
    }

    bool keyDown(int key)
    {
        if (key >= 0 && key < kKeys && g_injected[key].load(std::memory_order_relaxed))
            return true;
        return physicalInputOn() && platform::keyDown(key);
    }

    bool keyPressed(int key)
    {
        if (key >= 0 && key < kKeys && g_injectedEdge[key])
            return true;
        return physicalInputOn() && platform::keyPressed(key);
    }

    void start()
    {
        if (!envOn("PS2X_CONTROL") && !hiddenMode())
            return;
        const char *portEnv = std::getenv("PS2X_CONTROL_PORT");
        const int port = portEnv && *portEnv ? std::atoi(portEnv) : 47615;
#if defined(_WIN32)
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
            return;
#endif
        g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (g_listen == kNoSocket)
            return;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // this machine only
        if (bind(g_listen, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || listen(g_listen, 4) != 0)
        {
            std::fprintf(stderr, "[control] cannot listen on 127.0.0.1:%d (another game running?)\n", port);
            closeSocket(g_listen);
            g_listen = kNoSocket;
            return;
        }
        g_running.store(true);
        g_server = std::thread(serverLoop);
        std::fprintf(stderr, "[control] listening on 127.0.0.1:%d%s\n", port, hiddenMode() ? " (window hidden, physical input off)" : "");
    }

    void pump(const std::function<std::string(const std::string &path)> &capture, const std::function<void()> &quit)
    {
        g_injectedEdge.fill(false);
        const auto now = std::chrono::steady_clock::now();
        for (size_t i = 0; i < g_releases.size();)
        {
            if (g_releases[i].at > now)
            {
                ++i;
                continue;
            }
            setInjected(g_releases[i].key, false);
            g_releases[i].command->reply.set_value("ok");
            g_releases.erase(g_releases.begin() + static_cast<std::ptrdiff_t>(i));
        }
        std::deque<std::shared_ptr<Command>> commands;
        {
            std::lock_guard<std::mutex> lock(g_queueMutex);
            commands.swap(g_queue);
        }
        for (const auto &command : commands)
            execute(command, capture, quit);
    }

    void stop()
    {
        if (!g_running.exchange(false))
            return;
        closeSocket(g_listen); // unblocks accept()
        g_listen = kNoSocket;
        for (Release &r : g_releases)
            r.command->reply.set_value("ok");
        g_releases.clear();
        {
            std::lock_guard<std::mutex> lock(g_queueMutex);
            for (auto &command : g_queue)
                command->reply.set_value("error game stopping");
            g_queue.clear();
        }
        if (g_server.joinable())
            g_server.detach(); // a connected client may still be reading; the process is ending
    }
}
