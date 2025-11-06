#pragma once

#include <cstdint>

struct __attribute__((packed)) PacketHeader {
    uint32_t type;     // 0: START; 1: END; 2: DATA; 3: ACK
    uint32_t seqNum;   // Described below
    uint32_t length;   // Length of data; 0 for ACK packets
    uint32_t checksum; // 32-bit CRC
};

// Byte order conversion functions for little-endian format (as used by tests)
// On Linux, use system-provided functions from <endian.h>
// On other systems, define our own

#if defined(__linux__) || (defined(__unix__) && !defined(__APPLE__))
    #include <endian.h>
    // Use system-provided htole32 and le32toh from <endian.h>
    // No need to define our own
#else
    // For other systems (macOS, Windows, etc.), define our own functions
    inline uint32_t htole32(uint32_t value) {
    #if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        return value;
    #elif defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        // Big-endian: swap bytes
        return ((value & 0xFF000000) >> 24) |
               ((value & 0x00FF0000) >> 8) |
               ((value & 0x0000FF00) << 8) |
               ((value & 0x000000FF) << 24);
    #elif defined(_WIN32) || defined(__x86_64__) || defined(__i386__) || defined(__aarch64__)
        // Most common platforms are little-endian
        return value;
    #else
        // Default: assume little-endian (most common)
        return value;
    #endif
    }

    inline uint32_t le32toh(uint32_t value) {
        // Same as htole32 (symmetric for 32-bit values)
        return htole32(value);
    }
#endif
