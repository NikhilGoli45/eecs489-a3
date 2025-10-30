#include <string>
#include <sys/socket.h>
#include <netinet/in.h>


struct SenderEndpoint {
    int fd;
    sockaddr_storage peer;
    socklen_t peer_len;
};

int start_sender_socket(const std::string& hostname, int port);