/*
 * server.cpp
 * Chat Server - CC3064 Proyecto 1
 * Compile: see Makefile
 * Run: ./server <port>
 */

#include <iostream>
#include <string>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <vector>
#include <chrono>
#include <atomic>
#include <csignal>
#include <cstring>
#include <ctime>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "framing.h"

// Generated protobuf headers
#include "common.pb.h"
#include "client-side/register.pb.h"
#include "client-side/message_general.pb.h"
#include "client-side/message_dm.pb.h"
#include "client-side/change_status.pb.h"
#include "client-side/list_users.pb.h"
#include "client-side/get_user_info.pb.h"
#include "client-side/quit.pb.h"
#include "server-side/server_response.pb.h"
#include "server-side/all_users.pb.h"
#include "server-side/for_dm.pb.h"
#include "server-side/broadcast_messages.pb.h"
#include "server-side/get_user_info_response.pb.h"

// ── Timestamp helper ──────────────────────────────────────────────────────────
static std::string now_str() {
    std::time_t t = std::time(nullptr);
    char buf[9]; // "HH:MM:SS\0"
    std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&t));
    return std::string(buf);
}

// ── Inactivity timeout (seconds) ──────────────────────────────────────────────
static constexpr int INACTIVITY_TIMEOUT_SEC = 30;

// ── User session ─────────────────────────────────────────────────────────────
struct UserSession {
    std::string username;
    std::string ip;
    int         fd;
    chat::StatusEnum status;
    std::chrono::steady_clock::time_point last_active;
};

// ── Global state ─────────────────────────────────────────────────────────────
static std::unordered_map<std::string, UserSession> g_users; // key = username
static std::mutex g_users_mutex;
static std::atomic<bool> g_running{true};

// ── Helpers ──────────────────────────────────────────────────────────────────
static void send_server_response(int fd, int code, const std::string& msg, bool ok) {
    chat::ServerResponse resp;
    resp.set_status_code(code);
    resp.set_message(msg);
    resp.set_is_successful(ok);
    std::string payload;
    resp.SerializeToString(&payload);
    send_frame(fd, MSG_SERVER_RESPONSE, payload);
}

static void broadcast_all(const std::string& payload, uint8_t type, int exclude_fd = -1) {
    std::lock_guard<std::mutex> lock(g_users_mutex);
    for (auto& [uname, sess] : g_users) {
        if (sess.fd != exclude_fd) {
            send_frame(sess.fd, type, payload);
        }
    }
}

static std::string remove_user_by_fd(int fd) {
    std::lock_guard<std::mutex> lock(g_users_mutex);
    for (auto it = g_users.begin(); it != g_users.end(); ++it) {
        if (it->second.fd == fd) {
            std::string name = it->first;
            std::cout << "[" << now_str() << "] [SERVER] User disconnected: " << name << std::endl;
            g_users.erase(it);
            return name;
        }
    }
    return "";
}

static void touch_user(const std::string& ip) {
    std::lock_guard<std::mutex> lock(g_users_mutex);
    for (auto& [uname, sess] : g_users) {
        if (sess.ip == ip) {
            sess.last_active = std::chrono::steady_clock::now();
            if (sess.status == chat::INVISIBLE) {
                // keep invisible
            } else {
                sess.status = chat::ACTIVE;
            }
            break;
        }
    }
}

// ── Inactivity monitor thread ─────────────────────────────────────────────────
static void inactivity_monitor() {
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(g_users_mutex);
        for (auto& [uname, sess] : g_users) {
            if (sess.status == chat::INVISIBLE) continue;
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - sess.last_active).count();
            if (elapsed >= INACTIVITY_TIMEOUT_SEC && sess.status != chat::DO_NOT_DISTURB) {
                sess.status = chat::DO_NOT_DISTURB; // INACTIVO mapped to DO_NOT_DISTURB
                std::cout << "[" << now_str() << "] [SERVER] User " << uname << " set to INACTIVE (inactivity)\n";
                // Notify the user
                send_server_response(sess.fd, 200, "Your status was set to INACTIVE due to inactivity.", true);
            }
        }
    }
}

// ── Client handler thread ─────────────────────────────────────────────────────
static void handle_client(int client_fd, std::string client_ip) {
    std::cout << "[" << now_str() << "] [SERVER] New connection from " << client_ip << " fd=" << client_fd << "\n";

    uint8_t     type;
    std::string payload;

    while (g_running) {
        if (!recv_frame(client_fd, type, payload)) {
            // Connection closed
            break;
        }

        // Touch last active
        touch_user(client_ip);

        switch (type) {
        // ── REGISTER ──────────────────────────────────────────────────────────
        case MSG_REGISTER: {
            chat::Register req;
            if (!req.ParseFromString(payload)) break;
            std::string uname = req.username();
            std::string ip    = req.ip();

            bool registered = false;
            {
                std::lock_guard<std::mutex> lock(g_users_mutex);
                bool name_taken = g_users.count(uname) > 0;
                bool ip_taken   = false;
                for (auto& [u, s] : g_users)
                   if (s.ip == ip) { ip_taken = true; break; }

                if (name_taken) {
                    send_server_response(client_fd, 409, "Username already taken.", false);
                    goto cleanup;
                } else if (ip_taken) {
                   
                   send_server_response(client_fd, 409, "IP already registered.", false);
                   goto cleanup;
                } else {
                    UserSession sess;
                    sess.username    = uname;
                    sess.ip          = ip;
                    sess.fd          = client_fd;
                    sess.status      = chat::ACTIVE;
                    sess.last_active = std::chrono::steady_clock::now();
                    g_users[uname]   = sess;
                    std::cout << "[" << now_str() << "] [SERVER] Registered: " << uname << " @ " << ip << "\n";
                    send_server_response(client_fd, 200, "Welcome, " + uname + "!", true);
                    registered = true;
                }
            }

            if (registered) {
                // Broadcast "User joined" (using MSG_BROADCAST so clients print it)
                chat::BroadcastDelivery bcast;
                bcast.set_message("User '" + uname + "' has joined the chat.");
                bcast.set_username_origin("SERVER");
                std::string bpayload;
                bcast.SerializeToString(&bpayload);
                broadcast_all(bpayload, MSG_BROADCAST_DELIVERY);

                // Broadcast updated user list
                // broadcast_user_list(); // Disabled per request
            }
            break;
        }

        // ── MESSAGE GENERAL (broadcast) ───────────────────────────────────────
        case MSG_GENERAL: {
            chat::MessageGeneral req;
            if (!req.ParseFromString(payload)) break;
            std::cout << "[" << now_str() << "] [BROADCAST] "
              << req.username_origin() << ": " << req.message() << "\n";
            chat::BroadcastDelivery bcast;
            bcast.set_message(req.message());
            bcast.set_username_origin(req.username_origin());
            std::string bpayload;
            bcast.SerializeToString(&bpayload);
            broadcast_all(bpayload, MSG_BROADCAST_DELIVERY);
            break;
        }

        // ── MESSAGE DM ────────────────────────────────────────────────────────
        case MSG_DM: {
            chat::MessageDM req;
            if (!req.ParseFromString(payload)) break;

            std::string dest = req.username_des();
            int dest_fd = -1;
            std::string sender_name;
            {
                std::lock_guard<std::mutex> lock(g_users_mutex);
                if (g_users.count(dest)) dest_fd = g_users[dest].fd;
                for (auto& [u, s] : g_users)
                    if (s.fd == client_fd) { sender_name = u; break; }
            }

            if (dest_fd == -1) {
                send_server_response(client_fd, 404, "User '" + dest + "' not found.", false);
            } else {
                chat::ForDm fwd;
                fwd.set_username_des(sender_name);
                fwd.set_message(req.message());
                std::string fpayload;
                fwd.SerializeToString(&fpayload);
                send_frame(dest_fd, MSG_FOR_DM, fpayload);
                send_server_response(client_fd, 200, "DM sent.", true);
            }
            break;
        }

        // ── CHANGE STATUS ─────────────────────────────────────────────────────
        case MSG_CHANGE_STATUS: {
            chat::ChangeStatus req;
            if (!req.ParseFromString(payload)) break;
            
            std::string status_msg;
            std::string uname;
            {
                std::lock_guard<std::mutex> lock(g_users_mutex);
                if (g_users.count(req.username())) {
                    auto& s = g_users[req.username()];
                    s.status = req.status();
                    s.last_active = std::chrono::steady_clock::now();
                    uname = s.username;
                    
                    std::string st;
                    switch (s.status) {
                        case chat::ACTIVE: st = "ACTIVE"; break;
                        case chat::DO_NOT_DISTURB: st = "BUSY"; break;
                        case chat::INVISIBLE: st = "INACTIVE"; break;
                        default: st = "UNKNOWN"; break;
                    }
                    status_msg = "User '" + uname + "' is now " + st;
                }
            }
            send_server_response(client_fd, 200, "Status updated.", true);
            
            if (!uname.empty()) {
                // Broadcast status text
                chat::BroadcastDelivery bcast;
                bcast.set_message(status_msg);
                bcast.set_username_origin("SERVER");
                std::string bpayload;
                bcast.SerializeToString(&bpayload);
                broadcast_all(bpayload, MSG_BROADCAST_DELIVERY);

                // Broadcast updated user list
                // broadcast_user_list(); // Disabled
            }
            break;
        }

        // ── LIST USERS ────────────────────────────────────────────────────────
        case MSG_LIST_USERS: {
            chat::AllUsers au;
            {
                std::lock_guard<std::mutex> lock(g_users_mutex);
                for (auto& [uname, sess] : g_users) {
                    au.add_usernames(uname);
                    au.add_status(sess.status);
                }
            }
            std::string apayload;
            au.SerializeToString(&apayload);
            send_frame(client_fd, MSG_ALL_USERS, apayload);
            break;
        }

        // ── GET USER INFO ─────────────────────────────────────────────────────
        case MSG_GET_USER_INFO: {
            chat::GetUserInfo req;
            if (!req.ParseFromString(payload)) break;
            std::string dest = req.username_des();

            std::lock_guard<std::mutex> lock(g_users_mutex);
            if (g_users.count(dest)) {
                auto& s = g_users[dest];
                chat::GetUserInfoResponse resp;
                resp.set_ip_address(s.ip);
                resp.set_username(s.username);
                resp.set_status(s.status);
                std::string rpayload;
                resp.SerializeToString(&rpayload);
                send_frame(client_fd, MSG_GET_USER_INFO_RESPONSE, rpayload);
            } else {
                send_server_response(client_fd, 404, "User '" + dest + "' not found.", false);
            }
            break;
        }

        // ── QUIT ──────────────────────────────────────────────────────────────
        case MSG_QUIT: {
            chat::Quit req;
            if (!req.ParseFromString(payload)) break;
            if (req.quit()) {
                send_server_response(client_fd, 200, "Bye!", true);
                goto cleanup;
            }
            break;
        }

        default:
            std::cerr << "[SERVER] Unknown message type: " << (int)type << "\n";
            break;
        }
    }

cleanup:
    {
        std::string removed_user = remove_user_by_fd(client_fd);
        if (!removed_user.empty()) {
            // Broadcast user left
            chat::BroadcastDelivery bcast;
            bcast.set_message("User '" + removed_user + "' has left the chat.");
            bcast.set_username_origin("SERVER");
            std::string bpayload;
            bcast.SerializeToString(&bpayload);
            broadcast_all(bpayload, MSG_BROADCAST_DELIVERY);

            // Broadcast updated list
            // broadcast_user_list(); // Disabled
        }
    }
    close(client_fd);
    std::cout << "[" << now_str() << "] [SERVER] Connection closed fd=" << client_fd << "\n";
}

// ── Main ──────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <port>\n";
        return 1;
    }
    int port = std::stoi(argv[1]);

    // Ignore SIGPIPE so broken client pipes don't kill server
    signal(SIGPIPE, SIG_IGN);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(server_fd, 64) < 0) { perror("listen"); return 1; }

    std::cout << "[" << now_str() << "] [SERVER] Listening on port " << port << " (inactivity timeout: "
              << INACTIVITY_TIMEOUT_SEC << "s)\n";

    // Start inactivity monitor
    std::thread(inactivity_monitor).detach();

    while (g_running) {
        sockaddr_in client_addr{};
        socklen_t   client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (g_running) perror("accept");
            continue;
        }
        std::string client_ip = inet_ntoa(client_addr.sin_addr);
        // Spawn a thread per client (as required)
        std::thread(handle_client, client_fd, client_ip).detach();
    }

    close(server_fd);
    return 0;
}
