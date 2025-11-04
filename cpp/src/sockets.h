#include <string>
#include <optional>
#include <fstream>
#include <sys/socket.h>
#include <netinet/in.h>


struct SenderEndpoint {
    int fd;
    sockaddr_storage peer;
    socklen_t peer_len;
    uint32_t startSeqNum;
};

std::optional<SenderEndpoint> start_sender_socket(const std::string& hostname, int port, std::ofstream& logf);