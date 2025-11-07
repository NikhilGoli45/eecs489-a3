// wSender.cpp — WTP sender (Go-Back-N, cumulative ACKs) with robust ACK handling.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <cxxopts.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "common/PacketHeader.hpp"
#include "common/Crc32.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using ms    = std::chrono::milliseconds;

constexpr uint32_t TYPE_START = 0;
constexpr uint32_t TYPE_END   = 1;
constexpr uint32_t TYPE_DATA  = 2;
constexpr uint32_t TYPE_ACK   = 3;

constexpr size_t HEADER_SIZE      = sizeof(PacketHeader);                 // 16
constexpr size_t MAX_UDP_PAYLOAD  = 1472;                                 // 1500-20-8
constexpr size_t MAX_DATA_BYTES   = MAX_UDP_PAYLOAD - HEADER_SIZE;        // 1456
constexpr ms     RTO{500};                                                // 500ms

struct Args {
    std::string host;
    uint16_t    port = 0;
    int         window = 1;
    std::string inputFile;
    std::string logFile;
    bool        verbose = false;
};

Args parseArgs(int argc, char** argv) {
    cxxopts::Options opts("wSender", "WTP reliable sender over UDP (Go-Back-N)");
    opts.add_options()
        ("h,hostname",     "Receiver IPv4 address", cxxopts::value<std::string>())
        ("p,port",         "Receiver UDP port",     cxxopts::value<int>())
        ("w,window-size",  "Sliding window size",   cxxopts::value<int>()->default_value("1"))
        ("i,input-file",   "Input file to send",    cxxopts::value<std::string>())
        ("o,output-log",   "Path to sender log",    cxxopts::value<std::string>())
        ("v,verbose",      "Verbose logs",          cxxopts::value<bool>()->default_value("false"))
        ("help",           "Show help");

    auto res = opts.parse(argc, argv);
    if (res.count("help")) { std::cout << opts.help() << '\n'; std::exit(0); }

    Args a{};
    if (!res.count("hostname") || !res.count("port") ||
        !res.count("input-file") || !res.count("output-log")) {
        std::cerr << "Missing required arguments.\n" << opts.help() << '\n';
        std::exit(1);
    }

    a.host      = res["hostname"].as<std::string>();
    a.port      = static_cast<uint16_t>(res["port"].as<int>());
    a.window    = res["window-size"].as<int>();
    a.inputFile = res["input-file"].as<std::string>();
    a.logFile   = res["output-log"].as<std::string>();
    a.verbose   = res["verbose"].as<bool>();

    if (a.window <= 0) { std::cerr << "window-size must be positive\n"; std::exit(1); }
    if (a.port == 0)   { std::cerr << "port must be nonzero\n";         std::exit(1); }
    return a;
}

void initLogger(bool verbose) {
    auto logger = spdlog::stdout_color_mt("console");
    logger->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
    spdlog::set_default_logger(logger);
    spdlog::set_level(verbose ? spdlog::level::debug : spdlog::level::info);
}

// Assignment-required log line: "<type> <seqNum> <length> <checksum>"
inline void logAssignment(std::ofstream& log, const PacketHeader& h) {
    log << h.type << ' ' << h.seqNum << ' ' << h.length << ' ' << h.checksum << '\n';
    log.flush();
}

ssize_t sendPacket(int sockfd,
                   const sockaddr_in& peer,
                   const PacketHeader& hdrHost,
                   const uint8_t* data,
                   size_t len)
{
    std::vector<uint8_t> buf(HEADER_SIZE + len);

    PacketHeader net{};
    net.type     = htonl(hdrHost.type);
    net.seqNum   = htonl(hdrHost.seqNum);
    net.length   = htonl(hdrHost.length);
    net.checksum = htonl(hdrHost.checksum);

    std::memcpy(buf.data(), &net, HEADER_SIZE);
    if (len && data) std::memcpy(buf.data() + HEADER_SIZE, data, len);

    return ::sendto(sockfd, buf.data(), buf.size(), 0,
                    reinterpret_cast<const sockaddr*>(&peer), sizeof(peer));
}

bool recvHeader(int sockfd, PacketHeader& outHdrHost, size_t& outTotalLen) {
    std::vector<uint8_t> buf(MAX_UDP_PAYLOAD);
    sockaddr_in from{};
    socklen_t addrlen = sizeof(from);
    ssize_t n = ::recvfrom(sockfd, buf.data(), buf.size(), 0,
                           reinterpret_cast<sockaddr*>(&from), &addrlen);
    if (n < static_cast<ssize_t>(HEADER_SIZE)) return false;

    PacketHeader net{};
    std::memcpy(&net, buf.data(), HEADER_SIZE);
    outHdrHost.type     = ntohl(net.type);
    outHdrHost.seqNum   = ntohl(net.seqNum);
    outHdrHost.length   = ntohl(net.length);
    outHdrHost.checksum = ntohl(net.checksum);
    outTotalLen = static_cast<size_t>(n);
    return true;
}

bool readFileChunks(const std::string& path, std::vector<std::vector<uint8_t>>& chunks) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    while (true) {
        std::vector<uint8_t> block(MAX_DATA_BYTES);
        in.read(reinterpret_cast<char*>(block.data()), static_cast<std::streamsize>(block.size()));
        std::streamsize got = in.gcount();
        if (got <= 0) break;
        block.resize(static_cast<size_t>(got));
        chunks.emplace_back(std::move(block));
    }
    return true;
}

uint32_t checksumData(const std::vector<uint8_t>& data) {
    return data.empty() ? 0u : crc32(data.data(), data.size());
}

} // namespace

int main(int argc, char** argv) {
    const auto args = parseArgs(argc, argv);
    initLogger(args.verbose);

    std::ofstream logFile(args.logFile, std::ios::out | std::ios::trunc);
    if (!logFile) { spdlog::error("Failed to open output log '{}'", args.logFile); return 1; }

    std::vector<std::vector<uint8_t>> chunks;
    if (!readFileChunks(args.inputFile, chunks)) {
        spdlog::error("Failed to read input file '{}'", args.inputFile);
        return 1;
    }
    const uint32_t totalPackets = static_cast<uint32_t>(chunks.size());
    spdlog::info("Input '{}' -> {} data packet(s), window={}", args.inputFile, totalPackets, args.window);

    const int sockfd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) { spdlog::error("socket() failed: {}", strerror(errno)); return 1; }

    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port   = htons(args.port);
    if (inet_pton(AF_INET, args.host.c_str(), &peer.sin_addr) != 1) {
        spdlog::error("Invalid IPv4 address: {}", args.host);
        ::close(sockfd);
        return 1;
    }

    // Random START seq
    uint32_t startSeq = static_cast<uint32_t>(Clock::now().time_since_epoch().count());

    auto recvWithTimeout = [&](ms timeout, PacketHeader& hdr, size_t& totalLen) -> bool {
        fd_set rfds; FD_ZERO(&rfds); FD_SET(sockfd, &rfds);
        timeval tv{};
        tv.tv_sec  = static_cast<int>(timeout.count() / 1000);
        tv.tv_usec = static_cast<int>((timeout.count() % 1000) * 1000);
        int rv = ::select(sockfd + 1, &rfds, nullptr, nullptr, &tv);
        if (rv <= 0) return false;
        return recvHeader(sockfd, hdr, totalLen);
    };

    // ---- START handshake (retransmit on 500ms) ----
    while (true) {
        PacketHeader h{};
        h.type = TYPE_START; h.seqNum = startSeq; h.length = 0; h.checksum = 0;
        if (sendPacket(sockfd, peer, h, nullptr, 0) < 0) {
            spdlog::error("sendto(START) failed: {}", strerror(errno));
            ::close(sockfd); return 1;
        }
        logAssignment(logFile, h);

        PacketHeader rx{}; size_t rxLen = 0;
        if (recvWithTimeout(RTO, rx, rxLen)) {
            // ACKs must have length==0 per spec
            if (rx.type == TYPE_ACK && rx.length == 0 && rx.seqNum == startSeq) break;
        }
        // else timeout -> retransmit
    }

    // ---- Go-Back-N data transfer ----
    uint32_t base    = 0;                  // first unACKed seq
    uint32_t nextSeq = 0;                  // next seq to send (exclusive end of window)
    auto     now     = [] { return Clock::now(); };
    auto     timerStart = now();

    auto sendDataPacket = [&](uint32_t seq) {
        const auto& payload = chunks[seq];
        PacketHeader h{};
        h.type = TYPE_DATA;
        h.seqNum = seq;
        h.length = static_cast<uint32_t>(payload.size());
        h.checksum = checksumData(payload);

        if (HEADER_SIZE + payload.size() > MAX_UDP_PAYLOAD) {
            spdlog::critical("DATA exceeds UDP payload ({} + {})", HEADER_SIZE, payload.size());
            std::exit(1);
        }

        if (sendPacket(sockfd, peer, h, payload.data(), payload.size()) < 0) {
            spdlog::critical("sendto(DATA) failed: {}", strerror(errno));
            std::exit(1);
        }
        logAssignment(logFile, h);
    };

    // initial fill
    while (nextSeq < totalPackets && nextSeq < base + static_cast<uint32_t>(args.window)) {
        sendDataPacket(nextSeq);
        if (base == nextSeq) timerStart = now();
        ++nextSeq;
    }

    while (base < totalPackets) {
        auto elapsed = std::chrono::duration_cast<ms>(now() - timerStart);
        ms wait = (elapsed >= RTO) ? ms(0) : (RTO - elapsed);

        PacketHeader rx{}; size_t rxLen = 0;
        bool got = recvWithTimeout(wait, rx, rxLen);
        if (got) {
            // Robustness: only treat well-formed ACKs as ACKs
            if (rx.type == TYPE_ACK && rx.length == 0) {
                // CUMULATIVE ACK semantics: rx.seqNum means "next expected"
                const uint32_t ackNext = rx.seqNum;

                // *** Hardened rule: never advance past what we've actually sent. ***
                const uint32_t maxAdvance = std::min<uint32_t>(nextSeq, totalPackets);
                if (ackNext > base) {
                    base = std::min<uint32_t>(ackNext, maxAdvance);

                    // Refill window from the new base
                    while (nextSeq < totalPackets &&
                           nextSeq < base + static_cast<uint32_t>(args.window)) {
                        sendDataPacket(nextSeq);
                        ++nextSeq;
                    }
                    if (base < nextSeq) {
                        // still have outstanding packets; restart timer
                        timerStart = now();
                    }
                }
                // duplicates / stale ACKs naturally ignored
            }
            // Non-ACKs ignored per spec
        } else {
            // timeout -> retransmit whole window [base, nextSeq)
            auto waited = std::chrono::duration_cast<ms>(now() - timerStart);
            if (base < nextSeq && waited >= RTO) {
                for (uint32_t s = base; s < nextSeq; ++s) sendDataPacket(s);
                timerStart = now();
            }
        }
    }

    // ---- END handshake (ACK must echo START.seq) ----
    while (true) {
        PacketHeader h{};
        h.type = TYPE_END; h.seqNum = startSeq; h.length = 0; h.checksum = 0;
        if (sendPacket(sockfd, peer, h, nullptr, 0) < 0) {
            spdlog::error("sendto(END) failed: {}", strerror(errno));
            ::close(sockfd); return 1;
        }
        logAssignment(logFile, h);

        PacketHeader rx{}; size_t rxLen = 0;
        if (recvWithTimeout(RTO, rx, rxLen)) {
            if (rx.type == TYPE_ACK && rx.length == 0 && rx.seqNum == startSeq) break;
        }
        // else timeout -> retransmit
    }

    ::close(sockfd);
    return 0;
}
