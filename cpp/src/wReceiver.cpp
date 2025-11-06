#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/PacketHeader.hpp"
#include "common/Crc32.hpp"

using std::cerr;
using std::cout;
using std::endl;
namespace fs = std::filesystem;

// Ethernet MTU 1500, IP 20, UDP 8 => 1472 max payload (header + data we receive).
static constexpr size_t kMaxUDPPayload = 1500 - 20 - 8; // 1472
static constexpr size_t kMaxRecvBuf = kMaxUDPPayload;

struct Args {
    int port = -1;
    int window = -1;
    std::string outDir;
    std::string logPath;
};

static void usage_and_exit(const char* prog) {
    std::cerr << "Usage:\n  " << prog
              << " -p <port> -w <window-size> -d <output-dir> -o <output-log>\n";
    std::exit(1);
}

static Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string v = argv[i];
        if ((v == "-p" || v == "--port") && i + 1 < argc) {
            a.port = std::stoi(argv[++i]);
        } else if ((v == "-w" || v == "--window-size") && i + 1 < argc) {
            a.window = std::stoi(argv[++i]);
        } else if ((v == "-d" || v == "--output-dir") && i + 1 < argc) {
            a.outDir = argv[++i];
        } else if ((v == "-o" || v == "--output-log") && i + 1 < argc) {
            a.logPath = argv[++i];
        } else {
            usage_and_exit(argv[0]);
        }
    }
    if (a.port <= 0 || a.window <= 0 || a.outDir.empty() || a.logPath.empty()) {
        usage_and_exit(argv[0]);
    }
    return a;
}

// Log format: "<type> <seqNum> <length> <checksum>\n"
static void log_packet(std::ofstream& log, const PacketHeader& hdr) {
    log << hdr.type << ' ' << hdr.seqNum << ' ' << hdr.length << ' ' << hdr.checksum << '\n';
    log.flush();
}

struct ReceiverState {
    bool inConnection = false;
    uint32_t connStartSeq = 0;  // START/END seq for this connection
    uint32_t expectedSeq = 0;   // next in-order DATA seq we want
    int fileIndex = 0;

    std::unordered_map<uint32_t, std::vector<uint8_t>> buffer;

    sockaddr_in senderAddr{};
    socklen_t senderLen = sizeof(senderAddr);

    std::ofstream outFile;
};

static void flush_in_order(ReceiverState& st) {
    while (true) {
        auto it = st.buffer.find(st.expectedSeq);
        if (it == st.buffer.end()) break;
        const auto& data = it->second;
        if (st.outFile.is_open() && !data.empty()) {
            st.outFile.write(reinterpret_cast<const char*>(data.data()),
                             static_cast<std::streamsize>(data.size()));
        }
        st.buffer.erase(it);
        st.expectedSeq++;
    }
}

static void send_ack(int sockfd,
                     const sockaddr_in& to,
                     std::ofstream& logFile,
                     uint32_t ackSeq)
{
    PacketHeader ack{};
    ack.type = 3;     // ACK
    ack.seqNum = ackSeq;
    ack.length = 0;   // no payload
    ack.checksum = 0; // not used

    // Convert ACK packet header to little-endian before sending
    std::array<uint8_t, sizeof(PacketHeader)> ackBuf{};
    uint32_t* ack_fields = reinterpret_cast<uint32_t*>(ackBuf.data());
    ack_fields[0] = htole32(ack.type);
    ack_fields[1] = htole32(ack.seqNum);
    ack_fields[2] = htole32(ack.length);
    ack_fields[3] = htole32(ack.checksum);
    
    (void)sendto(sockfd, ackBuf.data(), sizeof(PacketHeader), 0,
                 reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    log_packet(logFile, ack);
}

int main(int argc, char** argv) {
    Args args = parse_args(argc, argv);

    try {
        fs::create_directories(args.outDir);
    } catch (const std::exception& e) {
        cerr << "Failed to ensure output dir: " << e.what() << endl;
        return 1;
    }

    std::ofstream logFile(args.logPath, std::ios::out | std::ios::app);
    if (!logFile) {
        cerr << "Failed to open log file: " << args.logPath << endl;
        return 1;
    }

    int sockfd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return 1;
    }

    sockaddr_in recvAddr{};
    recvAddr.sin_family = AF_INET;
    recvAddr.sin_addr.s_addr = INADDR_ANY;
    recvAddr.sin_port = htons(static_cast<uint16_t>(args.port));

    if (bind(sockfd, reinterpret_cast<sockaddr*>(&recvAddr), sizeof(recvAddr)) < 0) {
        perror("bind");
        close(sockfd);
        return 1;
    }

    ReceiverState st;

    std::vector<uint8_t> buf(kMaxRecvBuf);

    auto same_sender = [](const sockaddr_in& a, const sockaddr_in& b) -> bool {
        return a.sin_family == b.sin_family &&
               a.sin_addr.s_addr == b.sin_addr.s_addr &&
               a.sin_port == b.sin_port;
    };

    auto open_new_file = [&](int index) {
        const std::string fname =
            (fs::path(args.outDir) / ("FILE-" + std::to_string(index) + ".out")).string();
        st.outFile.close();
        st.outFile.clear();
        st.outFile.open(fname, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!st.outFile) {
            cerr << "Failed to open output file: " << fname << endl;
        }
    };

    while (true) {
        sockaddr_in from{};
        socklen_t fromLen = sizeof(from);
        ssize_t n = recvfrom(sockfd, buf.data(), buf.size(), 0,
                             reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("recvfrom");
            break;
        }
        if (n < static_cast<ssize_t>(sizeof(PacketHeader))) {
            // Malformed: too short → drop silently
            continue;
        }

        PacketHeader hdr{};
        std::memcpy(&hdr, buf.data(), sizeof(PacketHeader));
        
        // Convert received packet header from little-endian to host byte order
        uint32_t* hdr_fields = reinterpret_cast<uint32_t*>(&hdr);
        hdr.type = le32toh(hdr_fields[0]);
        hdr.seqNum = le32toh(hdr_fields[1]);
        hdr.length = le32toh(hdr_fields[2]);
        hdr.checksum = le32toh(hdr_fields[3]);

        if (sizeof(PacketHeader) + hdr.length != static_cast<size_t>(n)) {
            // Malformed length → drop silently
            continue;
        }

        const uint8_t* payload = buf.data() + sizeof(PacketHeader);

        switch (hdr.type) {
            case 0: { // START
                // Always ACK STARTs from the active sender, even mid-connection, to avoid deadlock
                if (!st.inConnection) {
                    // New connection
                    st.inConnection = true;
                    st.connStartSeq = hdr.seqNum;
                    st.expectedSeq = 0;
                    st.buffer.clear();
                    st.senderAddr = from;
                    st.senderLen = fromLen;

                    log_packet(logFile, hdr);
                    open_new_file(st.fileIndex);
                    send_ack(sockfd, st.senderAddr, logFile, hdr.seqNum);
                } else {
                    // If this START is from the same sender, ACK it (duplicate START / lost ACK case)
                    if (same_sender(from, st.senderAddr) && hdr.seqNum == st.connStartSeq) {
                        log_packet(logFile, hdr);               // it's valid, so log
                        send_ack(sockfd, st.senderAddr, logFile, hdr.seqNum); // re-ACK
                    }
                    // If it's from a different sender or wrong seq, ignore (no log, no ACK).
                }
                break;
            }

            case 2: { // DATA
                if (!st.inConnection) {
                    // No START yet → ignore
                    break;
                }
                if (!same_sender(from, st.senderAddr)) {
                    // Ignore other senders mid-connection
                    break;
                }

                // Validate checksum over data only
                if (hdr.length > 0) {
                    uint32_t calc = crc32(payload, hdr.length);
                    if (calc != hdr.checksum) {
                        // Corrupted: drop (no log, no ACK)
                        break;
                    }
                }

                // Enforce receiver window: drop seq >= N + window
                uint32_t N = st.expectedSeq;
                if (hdr.seqNum >= N + static_cast<uint32_t>(args.window)) {
                    // Drop (not logged). Still send cumulative ACK for N to help sender progress.
                    send_ack(sockfd, st.senderAddr, logFile, st.expectedSeq);
                    break;
                }

                // Log valid DATA
                log_packet(logFile, hdr);

                // Duplicate of already delivered data?
                if (hdr.seqNum < st.expectedSeq) {
                    // Don't rewrite; just ACK cumulatively
                    send_ack(sockfd, st.senderAddr, logFile, st.expectedSeq);
                    break;
                }

                // Buffer if new
                if (st.buffer.find(hdr.seqNum) == st.buffer.end()) {
                    st.buffer.emplace(hdr.seqNum, std::vector<uint8_t>(payload, payload + hdr.length));
                }

                // If we just got the next expected, flush forward
                if (hdr.seqNum == st.expectedSeq) {
                    flush_in_order(st);
                }

                // Send cumulative ACK with the next expected seq
                send_ack(sockfd, st.senderAddr, logFile, st.expectedSeq);
                break;
            }

            case 1: { // END
                if (!st.inConnection) break;
                if (!same_sender(from, st.senderAddr)) break;

                if (hdr.seqNum == st.connStartSeq) {
                    // Valid END for this connection
                    log_packet(logFile, hdr);
                    send_ack(sockfd, st.senderAddr, logFile, hdr.seqNum);

                    if (st.outFile.is_open()) st.outFile.close();
                    st.fileIndex++;
                    st.buffer.clear();
                    st.expectedSeq = 0;
                    st.inConnection = false;
                }
                // If wrong seqNum, ignore (no log, no ACK)
                break;
            }

            case 3: { // ACK (unexpected to receiver)
                // Log and ignore
                log_packet(logFile, hdr);
                break;
            }

            default:
                // Unknown type: drop silently
                break;
        }
    }

    if (st.outFile.is_open()) st.outFile.close();
    close(sockfd);
    return 0;
}
