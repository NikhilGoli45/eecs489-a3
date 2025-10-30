#include <spdlog/spdlog.h>

#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include "sockets.h"

std::optional<SenderEndpoint> start_sender_socket(const std::string& hostname, int port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    addrinfo* result = nullptr;
    int err = getaddrinfo(hostname.c_str(), std::to_string(port).c_str(), &hints, &result);
    if (err != 0) {
        spdlog::error("Failed to get address info: {}", gai_strerror(err));
        return std::nullopt;
    }

    int sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (sock < 0) {
        spdlog::error("Failed to create socket: {}", strerror(errno));
        freeaddrinfo(result);
        return std::nullopt;
    }

    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = 500 * 1000;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        spdlog::warn("setsockopt(SO_RCVTIMEO) failed: {}", strerror(errno));
    }

    SenderEndpoint ep{};
    ep.fd = sock;
    ep.peer_len = static_cast<socklen_t>(result->ai_addrlen);
    std::memcpy(&ep.peer, result->ai_addr, result->ai_addrlen);
    freeaddrinfo(result);

    PacketHeader start{};
    start.type = 0;
    start.seqNum = 0;
    start.length = 0;
    start.checksum = 0;

    for (;;) {
        ssize_t sent = sendto(ep.fd, &start, sizeof(start), 0, reinterpret_cast<sockaddr*>(&ep.peer), ep.peer_len);
        if (sent != sizeof(start)) {
            spdlog::warn("Failed to send START: {}", strerror(errno));
        } else {
            spdlog::debug("SENT START: {} {} {} {}", start.type, start.seqNum, start.length, start.checksum);
        }

        PacketHeader ack{};
        sockaddr_storage from{};
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(ep.fd, &ack, sizeof(ack), 0, reinterpret_cast<sockaddr*>(&from), &fromlen);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            spdlog::error("recvfrom failed: {}", strerror(errno));
            close(ep.fd);
            return std::nullopt;
        }
        if (ack.type == 3 && ack.seqNum == start.seqNum) {
            spdlog::debug("RECV ACK: {} {} {} {}", ack.type, ack.seqNum, ack.length, ack.checksum);
            break;
        }
        spdlog::debug("Ignoring non-START-ACK (type {}, seq {})", ack.type, ack.seqNum);
    }

    return ep;
}