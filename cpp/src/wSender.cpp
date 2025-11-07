// wSender.cpp
// Reliable transport sender (WTP) over UDP, Go-Back-N with cumulative ACKs.
// Uses cxxopts for CLI parsing and spdlog for runtime logging.
//
// Build: add cxxopts + spdlog per CMake snippet below.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "PacketHeader.hpp"
#include "Crc32.hpp"

#include <cxxopts.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace {

using Clock = std::chrono::steady_clock;
using ms = std::chrono::milliseconds;

constexpr uint32_t TYPE_START = 0;
constexpr uint32_t TYPE_END   = 1;
constexpr uint32_t TYPE_DATA  = 2;
constexpr uint32_t TYPE_ACK   = 3;

constexpr size_t   MAX_UDP_PAYLOAD = 1472;               // bytes
constexpr size_t   HEADER_SIZE      = sizeof(PacketHeader);
static_assert(HEADER_SIZE == 16, "PacketHeader is expected to be 16 bytes");
constexpr size_t   MAX_DATA_BYTES   = MAX_UDP_PAYLOAD - HEADER_SIZE; // 1456 bytes

constexpr ms RTO{500};  // 500 ms retransmission timeout

struct Args {
    std::string host;
    uint16_t    port = 0;
    int         window = 1;
    std::string inputFile;
    std::string logFile;
    bool        verbose = false;
};

Args parseArgs(int argc, char** argv) {
    cxxopts::Options options("wSender", "WTP reliable sender over UDP (Go-Back-N)");
    options.add_options()
        ("h,hostname", "Receiver IPv4 address", cxxopts::value<std::string>())
        ("p,port",     "Receiver UDP port",      cxxopts::value<int>())
        ("w,window-size", "Sliding window size", cxxopts::value<int>()->default_value("1"))
        ("i,input-file",  "Input file to send",  cxxopts::value<std::string>())
        ("o,output-log",  "Path to sender log",  cxxopts::value<std::string>())
        ("v,verbose",     "Enable verbose (debug) logging", cxxopts::value<bool>()->default_value("false"))
        ("help",          "Show help");

    auto result = options.parse(argc, argv);

    if (result.count("help")) {
        fmt::print("{}\n", options.help());
        std::exit(0);
    }

    Args args{};
    if (!result.count("hostname") || !result.count("port") ||
        !result.count("input-file") || !result.count("output-log")) {
        fmt::print(stderr, "Missing required arguments.\n{}\n", options.help());
        std::exit(1);
    }

    args.host      = result["hostname"].as<std::string>();
    args.port      = static_cast<uint16_t>(result["port"].as<int>());
    args.window    = result["window-size"].as<int>();
    args.inputFile = result["input-file"].as<std::string>();
    args.logFile   = result["output-log"].as<std::string>();
    args.verbose   = result["verbose"].as<bool>();

    if (args.window <= 0) {
        fmt::print(stderr, "window-size must be positive\n");
        std::exit(1);
    }
    if (args.port == 0) {
        fmt::print(stderr, "port must be nonzero\n");
        std::exit(1);
    }
    return args;
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
                   const PacketHeader& hdr,
                   const uint8_t* data,
                   size_t len) {
    std::vector<uint8_t> buf(HEADER_SIZE + len);
    std::memcpy(buf.data(), &hdr, HEADER_SIZE);
    if (len > 0 && data) {
        std::memcpy(buf.data() + HEADER_SIZE, data, len);
    }
    return sendto(sockfd, buf.data(), buf.size(), 0,
                  reinterpret_cast<const sockaddr*>(&peer), sizeof(peer));
}

bool recvHeader(int sockfd, PacketHeader& outHdr, sockaddr_in& from) {
    std::vector<uint8_t> buf(MAX_UDP_PAYLOAD);
    socklen_t addrlen = sizeof(from);
    ssize_t n = recvfrom(sockfd, buf.data(), buf.size(), 0,
                         reinterpret_cast<sockaddr*>(&from), &addrlen);
    if (n < static_cast<ssize_t>(HEADER_SIZE)) {
        spdlog::debug("Dropped malformed packet: n={} (< header {})", n, HEADER_SIZE);
        return false;
    }
    std::memcpy(&outHdr, buf.data(), HEADER_SIZE);
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

uint32_t computeDataChecksum(const std::vector<uint8_t>& data) {
    if (data.empty()) return 0;
    return crc32(data.data(), data.size());
}

} // namespace

int main(int argc, char** argv) {
    auto args = parseArgs(argc, argv);
    initLogger(args.verbose);

    std::ofstream logFile(args.logFile, std::ios::out | std::ios::trunc);
    if (!logFile) {
        spdlog::error("Failed to open output log '{}'", args.logFile);
        return 1;
    }

    std::vector<std::vector<uint8_t>> chunks;
    if (!readFileChunks(args.inputFile, chunks)) {
        spdlog::error("Failed to read input file '{}'", args.inputFile);
        return 1;
    }
    const size_t totalPackets = chunks.size();
    spdlog::info("Input '{}' -> {} data packet(s), window={}", args.inputFile, totalPackets, args.window);

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        spdlog::error("socket() failed: {}", strerror(errno));
        return 1;
    }

    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port   = htons(args.port);
    if (inet_pton(AF_INET, args.host.c_str(), &peer.sin_addr) != 1) {
        spdlog::error("Invalid IPv4 address: {}", args.host);
        close(sockfd);
        return 1;
    }

    std::mt19937_64 rng(std::random_device{}());
    uint32_t startSeq = static_cast<uint32_t>(rng());

    auto recvWithTimeout = [&](ms timeout, PacketHeader& hdrOut) -> bool {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sockfd, &rfds);
        timeval tv{};
        tv.tv_sec  = static_cast<int>(timeout.count() / 1000);
        tv.tv_usec = static_cast<int>((timeout.count() % 1000) * 1000);
        int rv = select(sockfd + 1, &rfds, nullptr, nullptr, &tv);
        if (rv <= 0) return false;
        sockaddr_in from{};
        if (!recvHeader(sockfd, hdrOut, from)) return false;
        return true;
    };

    // START handshake (retransmit on 500ms)
    while (true) {
        PacketHeader h{};
        h.type = TYPE_START;
        h.seqNum = startSeq;
        h.length = 0;
        h.checksum = 0;

        if (sendPacket(sockfd, peer, h, nullptr, 0) < 0) {
            spdlog::error("sendto(START) failed: {}", strerror(errno));
            close(sockfd);
            return 1;
        }
        logAssignment(logFile, h);
        spdlog::debug("Sent START seq={}", startSeq);

        PacketHeader rx{};
        if (recvWithTimeout(RTO, rx)) {
            logAssignment(logFile, rx);
            spdlog::debug("Rx type={} seq={} len={} cksum={}", rx.type, rx.seqNum, rx.length, rx.checksum);
            if (rx.type == TYPE_ACK && rx.seqNum == startSeq) {
                spdlog::info("START ACKed");
                break;
            }
        } else {
            spdlog::debug("START timeout -> retransmit");
        }
    }

    // Go-Back-N data transfer
    uint32_t base = 0;
    uint32_t nextSeq = 0;
    std::vector<bool> sent(totalPackets, false);

    auto now = [] { return Clock::now(); };
    auto timerStart = now();

    auto sendDataPacket = [&](uint32_t seq) {
        const auto& payload = chunks[seq];
        PacketHeader h{};
        h.type = TYPE_DATA;
        h.seqNum = seq;
        h.length = static_cast<uint32_t>(payload.size());
        h.checksum = computeDataChecksum(payload);

        if (HEADER_SIZE + payload.size() > MAX_UDP_PAYLOAD) {
            spdlog::critical("DATA size exceeds 1472 bytes ({} + {})", HEADER_SIZE, payload.size());
            std::exit(1);
        }

        if (sendPacket(sockfd, peer, h, payload.data(), payload.size()) < 0) {
            spdlog::critical("sendto(DATA) failed: {}", strerror(errno));
            std::exit(1);
        }
        logAssignment(logFile, h);
        sent[seq] = true;
        spdlog::debug("Sent DATA seq={} len={} cksum={}", h.seqNum, h.length, h.checksum);
    };

    // initial window fill
    while (nextSeq < totalPackets && nextSeq < base + static_cast<uint32_t>(args.window)) {
        sendDataPacket(nextSeq);
        if (base == nextSeq) {
            timerStart = now();
        }
        ++nextSeq;
    }

    while (base < totalPackets) {
        auto elapsed = std::chrono::duration_cast<ms>(now() - timerStart);
        ms wait = (elapsed >= RTO) ? ms(0) : (RTO - elapsed);

        PacketHeader rx{};
        bool got = recvWithTimeout(wait, rx);
        if (got) {
            logAssignment(logFile, rx);
            spdlog::debug("Rx type={} seq={} len={} cksum={}", rx.type, rx.seqNum, rx.length, rx.checksum);

            if (rx.type == TYPE_ACK) {
                uint32_t ackNext = rx.seqNum; // cumulative next expected
                if (ackNext > base) {
                    base = std::min<uint32_t>(ackNext, totalPackets);
                    spdlog::debug("Window advanced: base={} nextSeq={}", base, nextSeq);

                    // Refill window
                    while (nextSeq < totalPackets && nextSeq < base + static_cast<uint32_t>(args.window)) {
                        if (!sent[nextSeq]) {
                            sendDataPacket(nextSeq);
                        }
                        if (base == nextSeq) {
                            timerStart = now();
                        }
                        ++nextSeq;
                    }
                    if (base < nextSeq) {
                        timerStart = now();
                    }
                }
            }
        } else {
            // timeout
            auto waited = std::chrono::duration_cast<ms>(now() - timerStart);
            if (base < nextSeq && waited >= RTO) {
                spdlog::debug("RTO: retransmitting window [{}..{})", base, nextSeq);
                for (uint32_t s = base; s < nextSeq; ++s) {
                    sendDataPacket(s);
                }
                timerStart = now();
            }
        }
    }

    // END handshake (must be ACKed with same seq as START)
    while (true) {
        PacketHeader h{};
        h.type = TYPE_END;
        h.seqNum = startSeq;
        h.length = 0;
        h.checksum = 0;

        if (sendPacket(sockfd, peer, h, nullptr, 0) < 0) {
            spdlog::error("sendto(END) failed: {}", strerror(errno));
            close(sockfd);
            return 1;
        }
        logAssignment(logFile, h);
        spdlog::debug("Sent END seq={}", startSeq);

        PacketHeader rx{};
        if (recvWithTimeout(RTO, rx)) {
            logAssignment(logFile, rx);
            spdlog::debug("Rx type={} seq={} len={} cksum={}", rx.type, rx.seqNum, rx.length, rx.checksum);
            if (rx.type == TYPE_ACK && rx.seqNum == startSeq) {
                spdlog::info("END ACKed. Done.");
                break;
            }
        } else {
            spdlog::debug("END timeout -> retransmit");
        }
    }

    close(sockfd);
    return 0;
}
