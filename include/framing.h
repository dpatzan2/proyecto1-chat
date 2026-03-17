#pragma once
#include <cstdint>
#include <string>
#include <stdexcept>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/socket.h>

/*
 * TCP Framing: 5-byte header
 *  [1 byte type][4 bytes length big-endian][N bytes protobuf payload]
 */

enum MessageType : uint8_t {
    // Client → Server
    MSG_REGISTER       = 1,
    MSG_GENERAL        = 2,
    MSG_DM             = 3,
    MSG_CHANGE_STATUS  = 4,
    MSG_LIST_USERS     = 5,
    MSG_GET_USER_INFO  = 6,
    MSG_QUIT           = 7,
    // Server → Client
    MSG_SERVER_RESPONSE       = 10,
    MSG_ALL_USERS             = 11,
    MSG_FOR_DM                = 12,
    MSG_BROADCAST_DELIVERY    = 13,
    MSG_GET_USER_INFO_RESPONSE= 14,
};

// Returns false on connection closed or error
inline bool send_frame(int fd, uint8_t type, const std::string& payload) {
    uint8_t header[5];
    header[0] = type;
    uint32_t len = htonl(static_cast<uint32_t>(payload.size()));
    memcpy(header + 1, &len, 4);

    // Send header
    ssize_t sent = 0;
    while (sent < 5) {
        ssize_t n = send(fd, header + sent, 5 - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += n;
    }
    // Send payload
    sent = 0;
    while (sent < (ssize_t)payload.size()) {
        ssize_t n = send(fd, payload.data() + sent, payload.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

inline bool recv_exact(int fd, void* buf, size_t len) {
    size_t received = 0;
    while (received < len) {
        ssize_t n = recv(fd, (char*)buf + received, len - received, 0);
        if (n <= 0) return false;
        received += n;
    }
    return true;
}

inline bool recv_frame(int fd, uint8_t& type, std::string& payload) {
    uint8_t header[5];
    if (!recv_exact(fd, header, 5)) return false;
    type = header[0];
    uint32_t len;
    memcpy(&len, header + 1, 4);
    len = ntohl(len);
    payload.resize(len);
    if (len > 0 && !recv_exact(fd, &payload[0], len)) return false;
    return true;
}
