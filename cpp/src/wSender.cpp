#include <cxxopts.hpp>
#include <spdlog/spdlog.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>
#include <chrono>
#include <optional>
#include <fstream>
#include <vector>
#include <array>
#include <sys/select.h>
#include <sys/time.h>
#include <iostream>

#include "sockets.h"
#include "common/Crc32.hpp"
#include "common/PacketHeader.hpp"

struct OutPkt {
    PacketHeader hdr;
    std::vector<uint8_t> payload;
};

int main(int argc, char** argv) {
    
    cxxopts::Options options("wSender", "A simple reliable transport protocol");
    options.add_options()
        ("h,hostname", "The IP address of the host that wReceiver is running on", cxxopts::value<std::string>())
        ("p,port", "The port number on which wReceiver is listening", cxxopts::value<int>())
        ("w,window-size", "Maximum number of outstanding packets in the current window", cxxopts::value<int>())
        ("i,input-file", "Path to the file that has to be transferred. It can be a text file or binary file (e.g., image or video)", cxxopts::value<std::string>())
        ("o,output-log", "The file path to which you should log the messages", cxxopts::value<std::string>())
        ("help", "Print help");

    auto result = options.parse(argc, argv);

    if (result.count("help")) {
        std::cout << options.help() << std::endl;
        return 0;
    }

    if (!result.count("hostname") || !result.count("port") || !result.count("window-size") || !result.count("input-file") || !result.count("output-log")) {
        spdlog::error("Missing required arguments");
        return 1;
    }
    const std::string hostname = result["hostname"].as<std::string>();
    const int port = result["port"].as<int>();

    const int window_size = result["window-size"].as<int>();
    const std::string inputPath = result["input-file"].as<std::string>();
    const std::string logPath = result["output-log"].as<std::string>();

    std::ofstream logf(logPath, std::ios::out | std::ios::app);

    auto epOpt = start_sender_socket(hostname, port, logf);
    if (!epOpt) {
        spdlog::error("Failed to start sender socket");
        return 1;
    }
    SenderEndpoint ep = *epOpt;
    uint32_t startSeqNum = ep.startSeqNum;
    
    // Remove socket timeout for main loop (we'll use our own timer)
    timeval no_timeout{};
    no_timeout.tv_sec = 0;
    no_timeout.tv_usec = 0;
    setsockopt(ep.fd, SOL_SOCKET, SO_RCVTIMEO, &no_timeout, sizeof(no_timeout));

    std::ifstream in(inputPath, std::ios::binary);
    if (!in) {
        spdlog::error("Failed to open input file: {}", inputPath);
        return 1;
    }

    const size_t maxPacketBytes = 1472;
    const size_t maxPayload = maxPacketBytes - sizeof(PacketHeader);

    uint32_t baseSeqNum = 0;
    uint32_t nextSeq = 0;
    bool timerRunning = false;
    auto timerStart = std::chrono::steady_clock::now();

    std::vector<OutPkt> window;

    auto send_packet = [&](const OutPkt& p) {
        std::array<uint8_t, 1472> buf{};
        std::memcpy(buf.data(), &p.hdr, sizeof(PacketHeader));
        if (!p.payload.empty()) {
            std::memcpy(buf.data() + sizeof(PacketHeader), p.payload.data(), p.payload.size());
        }
        const size_t total = sizeof(PacketHeader) + p.payload.size();
        ssize_t sent = sendto(ep.fd, buf.data(), total, 0, reinterpret_cast<sockaddr*>(&ep.peer), ep.peer_len);
        (void)sent;

        logf << p.hdr.type << " " << p.hdr.seqNum << " " << p.hdr.length << " " << p.hdr.checksum << "\n";
        logf.flush();
    };

    bool eofReached = false;
    std::vector<uint8_t> readBuf(maxPayload);

    while (true) {
        while (!eofReached && (nextSeq - baseSeqNum) < static_cast<uint32_t>(window_size)) {
            in.read(reinterpret_cast<char*>(readBuf.data()), readBuf.size());
            std::streamsize n = in.gcount();
            if (n <= 0) {
                eofReached = true;
                break;
            }

            OutPkt pkt{};
            pkt.payload.assign(readBuf.begin(), readBuf.begin() + n);
            pkt.hdr.type = 2;
            pkt.hdr.seqNum = nextSeq;
            pkt.hdr.length = static_cast<uint32_t>(n);
            pkt.hdr.checksum = crc32(pkt.payload.data(), pkt.payload.size());
            
            if (window.size() < (nextSeq - baseSeqNum + 1)) {
                window.resize(nextSeq - baseSeqNum + 1);
            }
            window[nextSeq - baseSeqNum] = pkt;

            send_packet(pkt);

            if (!timerRunning) {
                timerRunning = true;
                timerStart = std::chrono::steady_clock::now();
            }
            nextSeq++;
        }

        if (eofReached && baseSeqNum == nextSeq) {
            break;
        }

        // Check timer and retransmit if needed
        if (timerRunning) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - timerStart).count();
            if (elapsed >= 500) {
                // retransmit all packets in current window
                for (size_t i = 0; i < window.size(); ++i) {
                    send_packet(window[i]);
                }
                // reset timer
                timerStart = std::chrono::steady_clock::now();
            }
        }

        // Check for incoming ACK with select (non-blocking check)
        fd_set readfds;
        struct timeval select_timeout;
        FD_ZERO(&readfds);
        FD_SET(ep.fd, &readfds);
        select_timeout.tv_sec = 0;
        select_timeout.tv_usec = 50000; // 50ms - allows timer check while waiting for ACK
        
        int select_result = select(ep.fd + 1, &readfds, nullptr, nullptr, &select_timeout);
        bool windowAdvanced = false;
        
        if (select_result > 0 && FD_ISSET(ep.fd, &readfds)) {
            PacketHeader ack{};
            sockaddr_storage from{};
            socklen_t fromlen = sizeof(from);
            ssize_t n = recvfrom(ep.fd, &ack, sizeof(ack), 0, reinterpret_cast<sockaddr*>(&from), &fromlen);

            if (n >= static_cast<ssize_t>(sizeof(PacketHeader)) && ack.type == 3) {

                logf << ack.type << " " << ack.seqNum << " " << ack.length << " " << ack.checksum << "\n";
                logf.flush();

                if (ack.seqNum > baseSeqNum && ack.seqNum <= nextSeq) {
                    size_t drop = static_cast<size_t>(ack.seqNum - baseSeqNum);
                    if (drop <= window.size()) {
                        window.erase(window.begin(), window.begin() + drop);
                    } else {
                        window.clear();
                    }
                    baseSeqNum = ack.seqNum;
                    windowAdvanced = true;

                    if (baseSeqNum == nextSeq) {
                        timerRunning = false;
                    } else {
                        timerStart = std::chrono::steady_clock::now();
                        timerRunning = true;
                    }
                }
            }
        }
    }

    PacketHeader end{};
    end.type = 1;
    end.seqNum = startSeqNum;  // END must use same seqNum as START
    end.length = 0;
    end.checksum = 0;

    for(;;) {
        ssize_t sent = sendto(ep.fd, &end, sizeof(end), 0, reinterpret_cast<sockaddr*>(&ep.peer), ep.peer_len);
        (void)sent;
        logf << end.type << " " << end.seqNum << " " << end.length << " " << end.checksum << "\n";
        logf.flush();

        PacketHeader ack{};
        sockaddr_storage from{};
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(ep.fd, &ack, sizeof(ack), 0, reinterpret_cast<sockaddr*>(&from), &fromlen);
        if (n >= static_cast<ssize_t>(sizeof(PacketHeader)) && ack.type == 3 && ack.seqNum == end.seqNum) {
            logf << ack.type << " " << ack.seqNum << " " << ack.length << " " << ack.checksum << "\n";
            logf.flush();
            break;
        }
    }

    close(ep.fd);
    return 0;
}