"""Listen for the ACAP's G.711 A-law RTP and print packet stats."""

import argparse
import socket
import struct
import time


def alaw_to_linear(value):
    """G.711 A-law byte to 16-bit linear PCM (ITU-T G.711 / Sun g711.c alaw2linear)."""
    value ^= 0x55
    segment = (value & 0x70) >> 4
    sample = (value & 0x0F) << 4
    if segment == 0:
        sample += 8
    else:
        sample = (sample + 0x108) << (segment - 1)
    return sample if value & 0x80 else -sample


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=49000)
    parser.add_argument("--seconds", type=float, default=8)
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", args.port))
    sock.settimeout(1.0)
    deadline = time.time() + args.seconds
    count = 0
    last_seq = None
    gaps = 0
    peak = 0
    sources = set()
    while time.time() < deadline:
        try:
            packet, addr = sock.recvfrom(2048)
        except socket.timeout:
            continue
        if len(packet) < 12:
            continue
        version = packet[0] >> 6
        payload_type = packet[1] & 0x7F
        marker = packet[1] >> 7
        seq = struct.unpack("!H", packet[2:4])[0]
        if last_seq is not None and (last_seq + 1) & 0xFFFF != seq:
            gaps += 1
        last_seq = seq
        payload = packet[12:]
        for byte in payload:
            peak = max(peak, abs(alaw_to_linear(byte)))
        sources.add(addr[0])
        count += 1
        if count == 1:
            print(
                "first from %s:%d version=%d pt=%d marker=%d seq=%d bytes=%d"
                % (addr[0], addr[1], version, payload_type, marker, seq, len(payload))
            )
    print("packets=%d gaps=%d peak=%d sources=%s" % (count, gaps, peak, ",".join(sorted(sources)) or "-"))


if __name__ == "__main__":
    main()
