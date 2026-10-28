"""Verify the advertised AirPlay address on each specified local interface.

Run against one receiver: python mdns_interfaces_check.py --port PORT IP [IP ...]
The local test does not prove another device can traverse the firewall or Wi-Fi.
"""
import argparse
import socket
import struct
import time

from protocol_check import encode_name, parse_records


def check(address, port):
    query = (struct.pack(">HHHHHH", 0, 0, 1, 0, 0, 0)
             + encode_name("_airplay._tcp.local")
             + struct.pack(">HH", 12, 0x8001))
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind((address, 0))
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(address))
        sock.settimeout(0.25)
        sock.sendto(query, ("224.0.0.251", 5353))
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            try:
                packet, source = sock.recvfrom(9000)
                records = parse_records(packet)
                if not any(kind == "SRV" and str(value).endswith(f":{port}")
                           for kind, _, value in records):
                    continue
                addresses = {value for kind, _, value in records if kind == "A"}
                ok = addresses == {address} and source[0] == address
                print(f"[{'PASS' if ok else 'FAIL'}] {address}: source={source[0]}, A={addresses}")
                return ok
            except socket.timeout:
                pass
        print(f"[FAIL] {address}: no reply for port {port}")
        return False


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("addresses", nargs="+")
    args = parser.parse_args()
    raise SystemExit(0 if all([check(address, args.port) for address in args.addresses]) else 1)
