#!/usr/bin/env python3
import socket, struct, zlib, time, os

HOST = "127.0.0.1"
PORT = 8000

# WTP types
START, END, DATA, ACK = 0, 1, 2, 3

def hdr(t, seq, length, checksum):
    # Pack PacketHeader in little-endian: uint32, uint32, uint32, uint32
    return struct.pack("<IIII", t, seq, length, checksum)

def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF

def send_packet(sock, t, seq, data=b""):
    length = len(data)
    checksum = crc32(data) if t == DATA else 0
    packet = hdr(t, seq, length, checksum) + data
    sock.sendto(packet, (HOST, PORT))
    return length, checksum

def recv_ack(sock, timeout=1.0):
    sock.settimeout(timeout)
    try:
        pkt, _ = sock.recvfrom(2048)
        if len(pkt) < 16:  # header size
            return None
        t, seq, length, checksum = struct.unpack("<IIII", pkt[:16])
        return (t, seq, length, checksum)
    except socket.timeout:
        return None

def print_ack(label, ack):
    if ack is None:
        print(f"{label}: (no ACK)")
    else:
        t, seq, length, checksum = ack
        print(f"{label}: ACK type={t} seq={seq} len={length} cksum={checksum}")

def test_suite():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    # ---------- Test 1: START handshake ----------
    start_seq = 12345
    print("\n[TEST 1] START -> expect ACK(seq=START.seq)")
    send_packet(s, START, start_seq)
    ack = recv_ack(s)
    print_ack("  start-ack", ack)

    # ---------- Test 2: In-order DATA 0,1 ----------
    print("\n[TEST 2] DATA in-order 0,1 -> expect cumulative ACK(seq=2)")
    d0 = b"Hello "
    d1 = b"world!"
    send_packet(s, DATA, 0, d0)
    ack0 = recv_ack(s)
    print_ack("  after DATA 0", ack0)

    send_packet(s, DATA, 1, d1)
    ack1 = recv_ack(s)
    print_ack("  after DATA 1", ack1)

    # ---------- Test 3: Out-of-order DATA 3, then 2 -> expect buffering then flush to 4 ----------
    print("\n[TEST 3] Out-of-order: send 3 then 2 -> expect ACK stays at 2 then jumps to 4")
    d2 = b" This "
    d3 = b"is WTP."
    send_packet(s, DATA, 3, d3)
    ack3 = recv_ack(s)
    print_ack("  after DATA 3 (o-o-w)", ack3)

    send_packet(s, DATA, 2, d2)
    ack2 = recv_ack(s)
    print_ack("  after DATA 2 (fills gap)", ack2)

    # ---------- Test 4: Duplicate DATA 1 -> expect duplicate handled, ACK stays 4 ----------
    print("\n[TEST 4] Duplicate DATA 1 -> expect ACK(seq=4) again")
    send_packet(s, DATA, 1, d1)
    ack_dup = recv_ack(s)
    print_ack("  after duplicate DATA 1", ack_dup)

    # ---------- Test 5: Corrupted DATA -> expect no ACK ----------
    print("\n[TEST 5] Corrupted DATA -> expect NO ACK")
    bad = b"CORRUPT"
    # Manually build header with wrong checksum (e.g., checksum=0) so receiver drops and does not ACK
    packet = hdr(DATA, 4, len(bad), 0) + bad
    s.sendto(packet, (HOST, PORT))
    ack_bad = recv_ack(s, timeout=0.8)
    print_ack("  after corrupted DATA 4", ack_bad)

    # ---------- Test 6: Out-of-window DATA -> expect cumulative ACK for expectedSeq (still 4) ----------
    # Receiver window size is 3. expectedSeq should now be 4.
    # Send seq >= 4+3 => 7 is out-of-window => drop but receiver should still send cumulative ACK(4)
    print("\n[TEST 6] Out-of-window DATA 7 with window=3 & expected=4 -> expect ACK(seq=4)")
    send_packet(s, DATA, 7, b"OUT_OF_WINDOW")
    ack_oow = recv_ack(s)
    print_ack("  after DATA 7 (o-o-window)", ack_oow)

    # ---------- Test 7: Send missing DATA 4 to complete, then END ----------
    print("\n[TEST 7] DATA 4 then END -> expect ACK(seq=5) then START/END ACK")
    d4 = b" Done."
    send_packet(s, DATA, 4, d4)
    ack4 = recv_ack(s)
    print_ack("  after DATA 4", ack4)

    # END must use the SAME seq as START per spec
    send_packet(s, END, start_seq)
    ack_end = recv_ack(s)
    print_ack("  end-ack", ack_end)

    s.close()

if __name__ == "__main__":
    test_suite()
    print("\nCheck receiver log and file:")
    print("  Log  : /testing/receiver.log")
    print("  Files: /testing/FILE-0.out (should contain 'Hello world! This is WTP. Done.')")
