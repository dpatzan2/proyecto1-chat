/*
 * client.cpp
 * Chat Client - CC3064 Proyecto 1
 * Compile: see Makefile
 * Run: ./client <username> <server_ip> <port>
 */

#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <sstream>
#include <cstring>
#include <csignal>
#include <ctime>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <ifaddrs.h>
#include <net/if.h>

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

// ── Colors ────────────────────────────────────────────────────────────────────
#define COLOR_RESET   "\033[0m"
#define COLOR_RED     "\033[31m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_BLUE    "\033[34m"
#define COLOR_MAGENTA "\033[35m"
#define COLOR_CYAN    "\033[36m"
#define COLOR_WHITE   "\033[37m"
#define COLOR_BOLD    "\033[1m"
#define COLOR_DIM     "\033[2m"

// ── Timestamp helper ──────────────────────────────────────────────────────────
static std::string now_str() {
    std::time_t t = std::time(nullptr);
    char buf[9];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&t));
    return std::string(buf);
}

// ── Globals ───────────────────────────────────────────────────────────────────
static std::string   g_username;
static std::string   g_my_ip;
static std::string   g_server_ip;
static int           g_server_port = 0;
static int           g_sock_fd = -1;
static std::atomic<bool> g_running{true};
static std::mutex    g_print_mutex;
static chat::StatusEnum g_my_status = chat::ACTIVE;

// ── Helpers ───────────────────────────────────────────────────────────────────
static std::string status_to_str(chat::StatusEnum s) {
    switch (s) {
        case chat::ACTIVE:        return COLOR_GREEN "ACTIVE" COLOR_RESET;
        case chat::DO_NOT_DISTURB:return COLOR_RED "BUSY" COLOR_RESET;
        case chat::INVISIBLE:     return COLOR_DIM "INACTIVE" COLOR_RESET;
        default:                  return "UNKNOWN";
    }
}

static void print_line(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_print_mutex);
    // Erase current line (where prompt might be)
    std::cout << "\r\033[2K";
    // Print the new line
    std::cout << line << "\n";
    // Reprint the prompt
    std::cout << COLOR_GREEN << g_username << COLOR_RESET 
              << " (" << status_to_str(g_my_status) << ") > " << COLOR_RESET;
    std::cout.flush();
}

static std::string get_local_ip() {
    struct ifaddrs* ifap;
    if (getifaddrs(&ifap) != 0) return "127.0.0.1";
    for (struct ifaddrs* ifa = ifap; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;
        char buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &((struct sockaddr_in*)ifa->ifa_addr)->sin_addr, buf, sizeof(buf));
        freeifaddrs(ifap);
        return std::string(buf);
    }
    freeifaddrs(ifap);
    return "127.0.0.1";
}

// ── Send helpers ─────────────────────────────────────────────────────────────
static bool do_register() {
    chat::Register req;
    req.set_username(g_username);
    req.set_ip(g_my_ip);
    std::string p; req.SerializeToString(&p);
    return send_frame(g_sock_fd, MSG_REGISTER, p);
}

static bool do_broadcast(const std::string& msg) {
    chat::MessageGeneral req;
    req.set_message(msg);
    req.set_status(g_my_status);
    req.set_username_origin(g_username);
    req.set_ip(g_my_ip);
    std::string p; req.SerializeToString(&p);
    return send_frame(g_sock_fd, MSG_GENERAL, p);
}

static bool do_dm(const std::string& dest, const std::string& msg) {
    chat::MessageDM req;
    req.set_message(msg);
    req.set_status(g_my_status);
    req.set_username_des(dest);
    req.set_ip(g_my_ip);
    std::string p; req.SerializeToString(&p);
    return send_frame(g_sock_fd, MSG_DM, p);
}

static bool do_change_status(chat::StatusEnum s) {
    chat::ChangeStatus req;
    req.set_status(s);
    req.set_username(g_username);
    req.set_ip(g_my_ip);
    std::string p; req.SerializeToString(&p);
    return send_frame(g_sock_fd, MSG_CHANGE_STATUS, p);
}

static bool do_list_users() {
    chat::ListUsers req;
    req.set_username(g_username);
    req.set_ip(g_my_ip);
    std::string p; req.SerializeToString(&p);
    return send_frame(g_sock_fd, MSG_LIST_USERS, p);
}

static bool do_get_user_info(const std::string& target) {
    chat::GetUserInfo req;
    req.set_username_des(target);
    req.set_username(g_username);
    req.set_ip(g_my_ip);
    std::string p; req.SerializeToString(&p);
    return send_frame(g_sock_fd, MSG_GET_USER_INFO, p);
}

static bool do_quit() {
    chat::Quit req;
    req.set_quit(true);
    req.set_ip(g_my_ip);
    std::string p; req.SerializeToString(&p);
    return send_frame(g_sock_fd, MSG_QUIT, p);
}

// ── Receiver thread ───────────────────────────────────────────────────────────
static void receiver_thread() {
    uint8_t     type;
    std::string payload;

    while (g_running) {
        if (!recv_frame(g_sock_fd, type, payload)) {
            if (g_running) {
                print_line("\n[!] Connection to server lost.");
                g_running = false;
            }
            break;
        }

        switch (type) {
        case MSG_SERVER_RESPONSE: {
            chat::ServerResponse resp;
            if (!resp.ParseFromString(payload)) break;
            std::string icon = resp.is_successful() ? COLOR_GREEN "[OK]" COLOR_RESET : COLOR_RED "[ERR]" COLOR_RESET;
            print_line(icon + " " + resp.message());
            break;
        }
        case MSG_ALL_USERS: {
            chat::AllUsers au;
            if (!au.ParseFromString(payload)) break;
            print_line(COLOR_BOLD COLOR_CYAN "─── Connected Users ───────────────────" COLOR_RESET);
            std::string user_list_str = "  ";
            for (int i = 0; i < au.usernames_size(); i++) {
                if (i > 0) user_list_str += " | ";
                user_list_str += au.usernames(i) + " [" + status_to_str(au.status(i)) + "]";
            }
            print_line(user_list_str);
            print_line(COLOR_BOLD COLOR_CYAN "───────────────────────────────────────" COLOR_RESET);
            break;
        }
        case MSG_FOR_DM: {
            chat::ForDm dm;
            if (!dm.ParseFromString(payload)) break;
            print_line(COLOR_DIM "[" + now_str() + "] " COLOR_RESET
                    + COLOR_MAGENTA "[DM from " + dm.username_des() + "]: " COLOR_RESET + dm.message());
            break;
        }
        case MSG_BROADCAST_DELIVERY: {
            chat::BroadcastDelivery bcast;
            if (!bcast.ParseFromString(payload)) break;
            std::string origin = bcast.username_origin();
            if (origin == "SERVER") {
                print_line(COLOR_DIM "[" + now_str() + "] " COLOR_RESET
                        + COLOR_YELLOW "*** " + bcast.message() + " ***" COLOR_RESET);
            } else {
                print_line(COLOR_DIM "[" + now_str() + "] " COLOR_RESET
                        + COLOR_CYAN "[" + origin + "]: " COLOR_RESET + bcast.message());
            }
            break;
        }
        case MSG_GET_USER_INFO_RESPONSE: {
            chat::GetUserInfoResponse resp;
            if (!resp.ParseFromString(payload)) break;
            print_line(COLOR_BOLD "─── User Info ─────────────────────────" COLOR_RESET);
            print_line("  Username: " + resp.username());
            print_line("  IP:       " + resp.ip_address());
            print_line("  Status:   " + status_to_str(resp.status()));
            print_line(COLOR_BOLD "───────────────────────────────────────" COLOR_RESET);
            break;
        }
        default:
            break;
        }
    }
}

// ── Help ──────────────────────────────────────────────────────────────────────
static void print_help() {
    std::cout << COLOR_CYAN
        "╔══════════════════════════════════════════════════╗\n"
        "║               Chat Client - Help                 ║\n"
        "╠══════════════════════════════════════════════════╣\n"
        "║  <message>               Send to all users       ║\n"
        "║  /dm <user> <message>    Send private message    ║\n"
        "║  /status <s>             Change your status:     ║\n"
        "║      s = active | busy | inactive                ║\n"
        "║  /list                   List connected users    ║\n"
        "║  /info <username>        Show user info          ║\n"
        "║  /help                   Show this help          ║\n"
        "║  /quit                   Disconnect and exit     ║\n"
        "╚══════════════════════════════════════════════════╝\n" COLOR_RESET;
}

// ── Input loop ────────────────────────────────────────────────────────────────
static void input_loop() {
    // Clear screen
    std::cout << "\033[2J\033[1;1H";
    
    // Header
    std::cout << COLOR_BOLD COLOR_BLUE
          << "╔══════════════════════════════════════════════════╗\n"
          << "║            CC3064 Chat App - Proyecto 1          ║\n"
          << "╚══════════════════════════════════════════════════╝\n"
          << COLOR_RESET;
    std::cout << COLOR_DIM "  User   : " COLOR_RESET COLOR_GREEN << g_username << COLOR_RESET "\n";
    std::cout << COLOR_DIM "  Server : " COLOR_RESET << g_server_ip << ":" << g_server_port << "\n";
    std::cout << COLOR_DIM "  My IP  : " COLOR_RESET << g_my_ip << "\n\n";
    print_line("Type /help for usage.");
    do_list_users();

    std::string line;
    while (g_running) {
        std::cout << "\r" << COLOR_GREEN << g_username << COLOR_RESET 
                  << " (" << status_to_str(g_my_status) << ") > " << COLOR_RESET;
        std::cout.flush();
        if (!std::getline(std::cin, line)) {
            // EOF
            g_running = false;
            break;
        }
        if (line.empty()) continue;

        if (line[0] != '/') {
            // Treat as broadcast message
            do_broadcast(line);
            print_line(COLOR_DIM "[" + now_str() + "] " COLOR_RESET
                    + COLOR_CYAN "[" + g_username + "]: " COLOR_RESET + line);
            continue;
        }

        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;

        if (cmd == "/broadcast") {
            std::string msg;
            std::getline(iss >> std::ws, msg);
            if (msg.empty()) { print_line("Usage: /broadcast <message>"); continue; }
            do_broadcast(msg);
        } else if (cmd == "/dm") {
            std::string dest, msg;
            iss >> dest;
            std::getline(iss >> std::ws, msg);
            if (dest.empty() || msg.empty()) { print_line("Usage: /dm <user> <message>"); continue; }
            do_dm(dest, msg);
        } else if (cmd == "/status") {
            std::string s;
            iss >> s;
            chat::StatusEnum ns;
            if (s == "active")   ns = chat::ACTIVE;
            else if (s == "busy")     ns = chat::DO_NOT_DISTURB;
            else if (s == "inactive") ns = chat::INVISIBLE;
            else { print_line("Unknown status. Use: active | busy | inactive"); continue; }
            g_my_status = ns;
            do_change_status(ns);
        } else if (cmd == "/list") {
            do_list_users();
        } else if (cmd == "/info") {
            std::string target;
            iss >> target;
            if (target.empty()) { print_line("Usage: /info <username>"); continue; }
            do_get_user_info(target);
        } else if (cmd == "/help") {
            print_help();
        } else if (cmd == "/quit") {
            do_quit();
            g_running = false;
            break;
        } else {
            print_line("Unknown command. Type /help for usage.");
        }
    }
}

// ── Main ──────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    if (argc != 4) {
        std::cerr << "Usage: " << argv[0] << " <username> <server_ip> <port>\n";
        return 1;
    }
    g_username = argv[1];
    std::string server_ip = argv[2];
    int port = std::stoi(argv[3]);

    g_server_ip   = server_ip;
    g_server_port = port;
    g_my_ip = get_local_ip();

    // Connect
    g_sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_sock_fd < 0) { perror("socket"); return 1; }

    sockaddr_in srv_addr{};
    srv_addr.sin_family = AF_INET;
    srv_addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, server_ip.c_str(), &srv_addr.sin_addr) <= 0) {
        std::cerr << "Invalid server IP.\n"; return 1;
    }
    if (connect(g_sock_fd, (sockaddr*)&srv_addr, sizeof(srv_addr)) < 0) {
        perror("connect"); return 1;
    }

    std::cout << "Connected to " << server_ip << ":" << port
              << " as " << g_username << " (local IP: " << g_my_ip << ")\n";

    // Register
    do_register();

    // Start receiver thread
    std::thread rx(receiver_thread);

    // Input loop (main thread)
    input_loop();

    g_running = false;
    shutdown(g_sock_fd, SHUT_RDWR);
    close(g_sock_fd);
    if (rx.joinable()) rx.join();

    std::cout << "Disconnected. Bye!\n";
    return 0;
}
