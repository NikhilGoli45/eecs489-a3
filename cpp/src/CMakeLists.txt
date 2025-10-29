# Create your own CMakeLists.txt file here that will build your project, based 
# on the directory structure you choose. You can use the CMakeLists.txt files
# we provided in the src directory of Project 1 and Project 2 as a starting point.

# You will also have to add a CMakelists.txt file in the common/ directory. 

# Running the standard CMake build sequence should result in the following executables:
#   wSender
#   wReceiver
#   wSenderOpt
#   wReceiverOpt

cmake_minimum_required(VERSION 3.28)

# Add the executable for wSender
add_executable(wSender wSender.cpp)

# Headers in common
target_include_directories(wSender PRIVATE ../${CMAKE_CURRENT_SOURCE_DIR}/common)

#link deps fetched in top-level CMakeLists.txt
target_link_libraries(wSender PRIVATE spdlog::spdlog cxxopts::cxxopts)
