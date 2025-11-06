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
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <spdlog/spdlog.h>

namespace fs = std::filesystem;

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

int main(int argc, char** argv) {
    Args args = parse_args(argc, argv);

    try {
        fs::create_directories(args.outDir);
    } catch (const std::exception& e) {
        std::cerr << "Failed to ensure output dir: " << e.what() << std::endl;
        return 1;
    }

    std::ofstream logFile(args.logPath, std::ios::out | std::ios::app);
    if (!logFile) {
        std::cerr << "Failed to open log file: " << args.logPath << std::endl;
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

    // Stub implementation - not yet implemented
    // TODO: Implement Part 3 optimizations with selective ACK
    spdlog::info("wReceiverOpt: Part 3 not yet implemented");
    
    // Keep socket open briefly then exit (stub behavior)
    close(sockfd);
    return 0;
}
