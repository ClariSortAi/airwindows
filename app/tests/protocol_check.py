"""Protocol-level checks for a running AirWindows, with no Apple device.

It plays the opening moves of a real AirPlay sender against the receiver:

  1. mDNS: query _airplay._tcp.local and _raop._tcp.local and read the answers.
  2. GET /info: fetch and decode the receiver's property list.
  3. POST /pair-setup: fetch the receiver's Ed25519 public key.
  4. POST /pair-verify (both steps): run the X25519 + Ed25519 handshake and
     verify the receiver's signature, which exercises the crypto build.
  5. POST /fp-setup (phase 1): check the FairPlay reply shape.

What it cannot do is mirror: FairPlay key delivery needs a real Apple sender.

Usage: python protocol_check.py --port PORT [--host 127.0.0.1] [--name NAME]
Requires the 'cryptography' package for step 4.
"""
import argparse
import hashlib
import plistlib
import select
import socket
import struct
import sys
import time

MDNS_GROUP = "224.0.0.251"
MDNS_PORT = 5353
TYPE_NAMES = {1: "A", 12: "PTR", 16: "TXT", 28: "AAAA", 33: "SRV", 47: "NSEC"}

results = []


def record(step, ok, detail):
    results.append((step, ok, detail))
    print(f"[{'PASS' if ok else 'FAIL'}] {step}: {detail}")


# --------------------------------------------------------------------------- mDNS

def encode_name(name):
    out = b""
    for label in name.strip(".").split("."):
        raw = label.encode("utf-8")
        out += bytes([len(raw)]) + raw
    return out + b"\x00"


def decode_name(packet, offset):
    labels = []
    jumped_end = None
    hops = 0
    while True:
        length = packet[offset]
        if length == 0:
            offset += 1
            break
        if length & 0xC0 == 0xC0:
            pointer = ((length & 0x3F) << 8) | packet[offset + 1]
            if jumped_end is None:
                jumped_end = offset + 2
            offset = pointer
            hops += 1
            if hops > 32:
                raise ValueError("compression loop")
            continue
        labels.append(packet[offset + 1:offset + 1 + length].decode("utf-8", "replace"))
        offset += 1 + length
    return ".".join(labels), (jumped_end if jumped_end is not None else offset)


def parse_records(packet):
    _id, _flags, qd, an, ns, ar = struct.unpack(">HHHHHH", packet[:12])
    offset = 12
    for _ in range(qd):
        _name, offset = decode_name(packet, offset)
        offset += 4
    records = []
    for _ in range(an + ns + ar):
        name, offset = decode_name(packet, offset)
        rtype, _rclass, _ttl, rdlen = struct.unpack(">HHIH", packet[offset:offset + 10])
        offset += 10
        rdata = packet[offset:offset + rdlen]
        if rtype == 12:
            value, _ = decode_name(packet, offset)
        elif rtype == 33:
            _prio, _weight, port = struct.unpack(">HHH", rdata[:6])
            target, _ = decode_name(packet, offset + 6)
            value = f"{target}:{port}"
        elif rtype == 16:
            items, pos = [], 0
            while pos < len(rdata):
                size = rdata[pos]
                items.append(rdata[pos + 1:pos + 1 + size].decode("utf-8", "replace"))
                pos += 1 + size
            value = items
        elif rtype == 1:
            value = socket.inet_ntoa(rdata)
        elif rtype == 28:
            value = socket.inet_ntop(socket.AF_INET6, rdata)
        else:
            value = rdata.hex()
        records.append((TYPE_NAMES.get(rtype, str(rtype)), name, value))
        offset += rdlen
    return records


def local_ipv4_addresses():
    # Loopback is left out: Windows cannot send multicast from 127.0.0.1.
    addresses = set()
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            if not info[4][0].startswith("127."):
                addresses.add(info[4][0])
    except OSError as exc:
        print(f"  (could not list local addresses: {exc})")
    return sorted(addresses)


def check_mdns(expected_name, expected_port):
    question = b""
    for service in ("_airplay._tcp.local", "_raop._tcp.local"):
        # 0x8001: class IN with the "unicast response wanted" bit.
        question += encode_name(service) + struct.pack(">HH", 12, 0x8001)
    query = struct.pack(">HHHHHH", 0, 0, 2, 0, 0, 0) + question

    sockets = []
    for address in local_ipv4_addresses():
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
            sock.bind((address, 0))
            sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(address))
            sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 255)
            sock.sendto(query, (MDNS_GROUP, MDNS_PORT))
            sockets.append(sock)
        except OSError as exc:
            print(f"  (mDNS query on {address} failed: {exc})")
    # Also listen on the multicast group, for responders that ignore the unicast bit.
    try:
        listener = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("", MDNS_PORT))
        for address in local_ipv4_addresses():
            try:
                membership = socket.inet_aton(MDNS_GROUP) + socket.inet_aton(address)
                listener.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, membership)
            except OSError:
                pass
        sockets.append(listener)
    except OSError as exc:
        print(f"  (could not listen on the mDNS group: {exc})")

    found = {}
    deadline = time.time() + 3.0
    while time.time() < deadline:
        readable, _, _ = select.select(sockets, [], [], 0.25)
        for sock in readable:
            try:
                packet, source = sock.recvfrom(9000)
                for rtype, name, value in parse_records(packet):
                    found[(rtype, name, str(value))] = source[0]
            except (OSError, ValueError, struct.error, IndexError) as exc:
                print(f"  (ignored a malformed mDNS packet: {exc})")
    for sock in sockets:
        sock.close()

    ours = sorted(key for key in found if expected_name.lower() in (key[1] + key[2]).lower())
    for rtype, name, value in ours:
        print(f"    {rtype:4} {name} -> {value}")
    airplay_ptr = any(k[0] == "PTR" and k[1].startswith("_airplay._tcp") for k in ours)
    raop_ptr = any(k[0] == "PTR" and k[1].startswith("_raop._tcp") for k in ours)
    srv_ports = {k[2].rsplit(":", 1)[-1] for k in ours if k[0] == "SRV"}
    record("mDNS _airplay._tcp advertisement", airplay_ptr, f"instance matching '{expected_name}' answered" if airplay_ptr else "no answer")
    record("mDNS _raop._tcp advertisement", raop_ptr, f"instance matching '{expected_name}' answered" if raop_ptr else "no answer")
    record("mDNS SRV port", str(expected_port) in srv_ports, f"SRV ports seen: {sorted(srv_ports)}; expected {expected_port}")


# --------------------------------------------------------------------------- RTSP

class Rtsp:
    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=5)
        self.cseq = 0

    def request(self, method, path, body=b"", content_type=None):
        self.cseq += 1
        lines = [f"{method} {path} RTSP/1.0", f"CSeq: {self.cseq}", "X-Apple-ProtocolVersion: 1",
                 "User-Agent: AirPlay/420.3", f"Content-Length: {len(body)}"]
        if content_type:
            lines.append(f"Content-Type: {content_type}")
        self.sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode("ascii") + body)

        data = b""
        while b"\r\n\r\n" not in data:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("connection closed while reading headers")
            data += chunk
        head, _, rest = data.partition(b"\r\n\r\n")
        header_lines = head.decode("latin-1").split("\r\n")
        status = int(header_lines[0].split()[1])
        headers = {}
        for line in header_lines[1:]:
            key, _, value = line.partition(":")
            headers[key.strip().lower()] = value.strip()
        length = int(headers.get("content-length", "0"))
        while len(rest) < length:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("connection closed while reading body")
            rest += chunk
        return status, headers, rest[:length]

    def close(self):
        self.sock.close()


def check_info(rtsp, expected_name):
    status, headers, body = rtsp.request("GET", "/info")
    if status != 200 or not body:
        record("GET /info", False, f"status {status}, {len(body)} bytes")
        return None
    info = plistlib.loads(body)
    shown = {k: info.get(k) for k in ("name", "model", "deviceID", "features", "sourceVersion", "pi", "pk", "vv", "statusFlags")}
    for key, value in shown.items():
        if isinstance(value, bytes):
            value = value.hex()
        print(f"    {key} = {value}")
    for display in info.get("displays", []):
        print("    display: " + ", ".join(f"{k}={display.get(k)}" for k in ("widthPixels", "heightPixels", "refreshRate", "maxFPS", "features")))
    for fmt in info.get("audioFormats", []):
        print(f"    audioFormat: type={fmt.get('type')} in=0x{fmt.get('audioInputFormats', 0):x} out=0x{fmt.get('audioOutputFormats', 0):x}")
    record("GET /info", info.get("name") == expected_name,
           f"binary plist, {len(body)} bytes, name='{info.get('name')}', model={info.get('model')}, features=0x{info.get('features', 0):X}")
    return info


def check_pairing(rtsp):
    try:
        from cryptography.hazmat.primitives import serialization
        from cryptography.hazmat.primitives.asymmetric import ed25519, x25519
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    except ImportError as exc:
        record("pair-verify", False, f"'cryptography' package missing: {exc}")
        return

    raw = serialization.Encoding.Raw
    raw_public = serialization.PublicFormat.Raw

    status, _, server_ed_public = rtsp.request("POST", "/pair-setup", b"\x00" * 32, "application/octet-stream")
    ok = status == 200 and len(server_ed_public) == 32
    record("POST /pair-setup", ok, f"status {status}, {len(server_ed_public)}-byte Ed25519 public key {server_ed_public.hex()[:16]}...")
    if not ok:
        return

    client_ed = ed25519.Ed25519PrivateKey.generate()
    client_x = x25519.X25519PrivateKey.generate()
    client_ed_public = client_ed.public_key().public_bytes(raw, raw_public)
    client_x_public = client_x.public_key().public_bytes(raw, raw_public)

    body = b"\x01\x00\x00\x00" + client_x_public + client_ed_public
    status, _, reply = rtsp.request("POST", "/pair-verify", body, "application/octet-stream")
    if status != 200 or len(reply) != 96:
        record("POST /pair-verify step 1", False, f"status {status}, {len(reply)} bytes (expected 96)")
        return
    server_x_public, encrypted_signature = reply[:32], reply[32:]
    shared = client_x.exchange(x25519.X25519PublicKey.from_public_bytes(server_x_public))
    key = hashlib.sha512(b"Pair-Verify-AES-Key" + shared).digest()[:16]
    iv = hashlib.sha512(b"Pair-Verify-AES-IV" + shared).digest()[:16]
    stream = Cipher(algorithms.AES(key), modes.CTR(iv)).encryptor()
    server_signature = stream.update(encrypted_signature)
    try:
        ed25519.Ed25519PublicKey.from_public_bytes(server_ed_public).verify(server_signature, server_x_public + client_x_public)
        record("POST /pair-verify step 1", True, "receiver's Ed25519 signature over the X25519 exchange verifies")
    except Exception as exc:  # cryptography raises InvalidSignature
        record("POST /pair-verify step 1", False, f"signature did not verify: {exc!r}")
        return

    # Step 2: our signature, encrypted with the continuation of the same AES-CTR stream.
    client_signature = client_ed.sign(client_x_public + server_x_public)
    body = b"\x00\x00\x00\x00" + stream.update(client_signature)
    status, _, reply = rtsp.request("POST", "/pair-verify", body, "application/octet-stream")
    record("POST /pair-verify step 2", status == 200, f"status {status} (receiver accepted the client's signature)" if status == 200 else f"status {status}")


def check_fairplay(rtsp):
    # Phase-1 request as sent by Apple senders: "FPLY", version 3, type 1, mode byte at offset 14.
    request = bytes([0x46, 0x50, 0x4C, 0x59, 0x03, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 0x02, 0x00, 0x00, 0xBB])
    status, _, reply = rtsp.request("POST", "/fp-setup", request, "application/octet-stream")
    ok = status == 200 and len(reply) == 142 and reply[:4] == b"FPLY"
    record("POST /fp-setup phase 1", ok, f"status {status}, {len(reply)} bytes, header {reply[:8].hex()}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--name", default=socket.gethostname().upper())
    parser.add_argument("--skip-mdns", action="store_true")
    args = parser.parse_args()

    if not args.skip_mdns:
        check_mdns(args.name, args.port)
    try:
        rtsp = Rtsp(args.host, args.port)
    except OSError as exc:
        record("TCP connect", False, f"{args.host}:{args.port}: {exc}")
    else:
        try:
            check_info(rtsp, args.name)
            check_pairing(rtsp)
            check_fairplay(rtsp)
        except (OSError, ValueError, ConnectionError, plistlib.InvalidFileException) as exc:
            record("RTSP exchange", False, repr(exc))
        finally:
            rtsp.close()

    failed = [step for step, ok, _ in results if not ok]
    print(f"\n{len(results) - len(failed)} of {len(results)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
