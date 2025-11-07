// wReceiver.cpp  — Base (non-opt) Go-Back-N receiver: cumulative ACKs, no buffering.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

#include "common/PacketHeader.hpp"
#include "common/Crc32.hpp"

namespace fs = std::filesystem;

// Ethernet MTU 1500, IP 20, UDP 8
static constexpr size_t kMaxUDPPayload = 1500 - 20 - 8;              // 1472
static constexpr size_t kHeaderSize    = sizeof(PacketHeader);       // 16
static constexpr size_t kMaxDataBytes  = kMaxUDPPayload - kHeaderSize; // 1456

struct Args {
    uint16_t    port      = 0;
    uint32_t    window    = 0;
    fs::path    out_dir;
    fs::path    log_path;
};

static void usage(const char* prog) {
    std::cerr << "Usage: " << prog
              << " -p <port> -w <window-size> -d <output-dir> -o <output-log>\n";
}

static bool parse_args(int argc, char** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        std::string f = argv[i];
        if ((f == "-p" || f == "--port") && i + 1 < argc) {
            a.port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if ((f == "-w" || f == "--window-size") && i + 1 < argc) {
            a.window = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if ((f == "-d" || f == "--output-dir") && i + 1 < argc) {
            a.out_dir = fs::path(argv[++i]);
        } else if ((f == "-o" || f == "--output-log") && i + 1 < argc) {
            a.log_path = fs::path(argv[++i]);
        } else {
            // ignore
        }
    }
    if (a.port == 0 || a.window == 0 || a.out_dir.empty() || a.log_path.empty()) {
        usage(argv[0]);
        return false;
    }
    return true;
}

struct Logger {
    std::ofstream out;
    explicit Logger(const fs::path& p) : out(p, std::ios::out | std::ios::app) {
        if (!out) throw std::runtime_error("Failed to open log file: " + p.string());
    }
    inline void log_numeric(uint32_t type, uint32_t seq, uint32_t len, uint32_t cksum) {
        out << type << ' ' << seq << ' ' << len << ' ' << cksum << '\n';
        out.flush();
    }
    inline void log_header_host(const PacketHeader& h) {
        log_numeric(h.type, h.seqNum, h.length, h.checksum);
    }
};

struct Peer { in_addr addr{}; uint16_t port{}; };

struct ReceiverState {
    bool      have_peer     = false;
    Peer      peer{};
    uint32_t  start_seq     = 0;   // START seq for this connection
    uint32_t  next_expected = 0;   // N (cumulative ACK point)
    std::ofstream outfile;
    size_t    file_index    = 0;
};

// ---------- Byte order helpers ----------
static inline PacketHeader ntoh_header(const PacketHeader& net) {
    PacketHeader h{};
    h.type     = ntohl(net.type);
    h.seqNum   = ntohl(net.seqNum);
    h.length   = ntohl(net.length);
    h.checksum = ntohl(net.checksum);
    return h;
}
static inline PacketHeader hton_header(const PacketHeader& host) {
    PacketHeader n{};
    n.type     = htonl(host.type);
    n.seqNum   = htonl(host.seqNum);
    n.length   = htonl(host.length);
    n.checksum = htonl(host.checksum);
    return n;
}

// ---------- Utility ----------
static inline bool same_peer(const sockaddr_in& a, const Peer& b) {
    return a.sin_addr.s_addr == b.addr.s_addr && ntohs(a.sin_port) == b.port;
}

static int setup_socket(uint16_t port) {
    int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { perror("socket"); std::exit(1); }
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind");
        std::exit(1);
    }
    return s;
}

static void send_ack(int sock, const sockaddr_in& to, uint32_t ack_seq_host, Logger& logger) {
    // Log host-order values (spec logging)
    logger.log_numeric(3u, ack_seq_host, 0u, 0u);
    // Send network-order header
    PacketHeader host{};
    host.type = 3u; host.seqNum = ack_seq_host; host.length = 0u; host.checksum = 0u;
    PacketHeader net = hton_header(host);
    (void)::sendto(sock, &net, sizeof(net), 0,
                   reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}

static void open_output_file(ReceiverState& st, const Args& args) {
    std::error_code ec;
    fs::create_directories(args.out_dir, ec); // ok if exists
    fs::path out_path = args.out_dir / ("FILE-" + std::to_string(st.file_index) + ".out");
    st.outfile.open(out_path, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!st.outfile) {
        std::cerr << "Failed to open output file: " << out_path << '\n';
    }
}

static void begin_new_connection(ReceiverState& st,
                                 const sockaddr_in& from,
                                 const PacketHeader& start_host,
                                 const Args& args) {
    st.peer.addr    = from.sin_addr;
    st.peer.port    = ntohs(from.sin_port);
    st.have_peer    = true;
    st.start_seq    = start_host.seqNum;
    st.next_expected= 0;  // DATA starts at 0 in spec
    if (st.outfile.is_open()) st.outfile.close();
    open_output_file(st, args);
}

static void finish_connection(ReceiverState& st) {
    if (st.outfile.is_open()) { st.outfile.flush(); st.outfile.close(); }
    st.have_peer     = false;
    st.start_seq     = 0;
    st.next_expected = 0;
    ++st.file_index;
}

static inline void write_chunk(ReceiverState& st, const uint8_t* data, size_t len) {
    if (st.outfile.is_open() && len > 0) {
        st.outfile.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
    }
}

// --------- Base (non-opt) GBN data handling: NO buffering, cumulative ACKs ---------
static void handle_data(int sock, const sockaddr_in& from, const PacketHeader& h,
                        const uint8_t* payload, size_t len,
                        ReceiverState& st, const Args& args, Logger& logger)
{
    const uint32_t seq = h.seqNum;
    const uint32_t W   = args.window;
    const uint32_t N   = st.next_expected;

    // Out-of-window: seq >= N + W → drop, ACK(N)
    if (seq >= N + W) {
        // no ack
        return;
    }

    if (seq < N) {
        // Duplicate/old → ACK(N)
        send_ack(sock, from, N, logger);
        return;
    }

    if (seq == N) {
        // In-order → deliver, advance, ACK(new N)
        write_chunk(st, payload, len);
        st.next_expected = N + 1;
        send_ack(sock, from, st.next_expected, logger);
        return;
    }

    // In-window but out-of-order (N < seq < N+W) — BASE RECEIVER DOES NOT BUFFER.
    // Just ACK(N) to trigger Go-Back-N retransmission of the missing packet.
    send_ack(sock, from, N, logger);
}

int main(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, args)) return 1;

    // Ensure log parent dir exists to avoid open failures on grader
    if (!args.log_path.parent_path().empty()) {
        std::error_code ec;
        fs::create_directories(args.log_path.parent_path(), ec);
    }

    try {
        Logger logger(args.log_path);
        ReceiverState st{};

        int sock = setup_socket(args.port);
        std::vector<uint8_t> recvbuf(65536);

        while (true) {
            sockaddr_in from{}; socklen_t fromlen = sizeof(from);
            ssize_t n = ::recvfrom(sock, recvbuf.data(), recvbuf.size(), 0,
                                   reinterpret_cast<sockaddr*>(&from), &fromlen);
            if (n < 0) {
                if (errno == EINTR) continue;
                perror("recvfrom");
                continue;
            }
            if (static_cast<size_t>(n) < kHeaderSize) continue; // malformed

            // Parse header (network -> host)
            PacketHeader net_hdr{};
            std::memcpy(&net_hdr, recvbuf.data(), kHeaderSize);
            PacketHeader h = ntoh_header(net_hdr);

            // Type sanity
            if (h.type > 3u) continue;

            // Length checks
            const size_t data_len = static_cast<size_t>(n) - kHeaderSize;
            if (h.length != data_len) continue;       // malformed
            if (data_len > kMaxDataBytes) continue;   // oversize

            // Checksum / control rules
            if (h.type == 2u) {
                // DATA: crc over payload only
                const uint32_t crc = crc32(recvbuf.data() + kHeaderSize, data_len);
                if (crc != h.checksum) continue;  // corrupt -> drop silently
            } else {
                // START/END/ACK must have length == 0
                if (h.length != 0u) continue;     // malformed control
            }

            // Log well-formed received packet
            logger.log_header_host(h);

            // If idle: only accept START to begin a connection
            if (!st.have_peer) {
                if (h.type == 0u) { // START
                    begin_new_connection(st, from, h, args);
                    // Spec: ACK seq == START.seq
                    send_ack(sock, from, h.seqNum, logger);
                }
                continue;
            }

            // Active connection: ignore other peers
            if (!same_peer(from, st.peer)) continue;

            switch (h.type) {
                case 0u:  // START mid-connection
                    // Re-ACK if duplicate START for this connection, ignore otherwise
                    if (h.seqNum == st.start_seq) {
                        send_ack(sock, from, h.seqNum, logger);
                    }
                    break;

                case 2u:  // DATA
                    handle_data(sock, from, h, recvbuf.data() + kHeaderSize, data_len, st, args, logger);
                    break;

                case 1u:  // END
                    // ACK with END.seq (same as START.seq), then finalize
                    send_ack(sock, from, h.seqNum, logger);
                    finish_connection(st);
                    break;

                case 3u:  // ACK — receiver ignores
                default:
                    break;
            }
        }
    } catch (const std::exception& ex) {
        std::cerr << "Fatal: " << ex.what() << '\n';
        return 1;
    }

    return 0;
}
