#include <cxxopts.hpp>
#include <spdlog/spdlog.h>


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
    
    return 0;
}