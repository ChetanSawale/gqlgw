#!/usr/bin/env python3
"""Minimal DNS responder for the test suite: dns.py start|stop [port]

nginx's resolver does not read /etc/hosts, and not every system DNS answers
for "localhost" (containers, CI runners). t/006-config.t resolves its TLS
upstream by name through this server: A queries for "localhost" get
127.0.0.1, other names get NXDOMAIN, other query types get an empty answer.
"""
import os
import signal
import socket
import struct
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def answer(query):
    if len(query) < 12:
        return None
    qid, _flags, qdcount = struct.unpack("!HHH", query[:6])
    if qdcount != 1:
        return None
    labels, i = [], 12
    while i < len(query) and query[i] != 0:
        n = query[i]
        labels.append(query[i + 1:i + 1 + n].decode("ascii", "replace").lower())
        i += 1 + n
    if i + 5 > len(query):
        return None
    qtype = struct.unpack("!H", query[i + 1:i + 3])[0]
    question = query[12:i + 5]
    name = ".".join(labels)
    if name != "localhost":
        return struct.pack("!HHHHHH", qid, 0x8583, 1, 0, 0, 0) + question
    if qtype != 1:
        return struct.pack("!HHHHHH", qid, 0x8580, 1, 0, 0, 0) + question
    record = struct.pack("!HHHIH", 0xC00C, 1, 1, 60, 4) + socket.inet_aton("127.0.0.1")
    return struct.pack("!HHHHHH", qid, 0x8580, 1, 1, 0, 0) + question + record


def serve(port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", port))
    while True:
        data, peer = sock.recvfrom(512)
        resp = answer(data)
        if resp:
            sock.sendto(resp, peer)


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 21253
    pidfile = ROOT / "build" / f"dns-{port}.pid"
    pidfile.parent.mkdir(exist_ok=True)
    if cmd == "stop":
        try:
            os.kill(int(pidfile.read_text()), signal.SIGTERM)
        except (OSError, ValueError):
            pass
        pidfile.unlink(missing_ok=True)
        return 0
    if cmd != "start":
        print("usage: dns.py start|stop [port]", file=sys.stderr)
        return 2
    try:
        os.kill(int(pidfile.read_text()), 0)
        return 0                                   # already running
    except (OSError, ValueError):
        pass
    # bind before forking, so that a busy port is an error of "start"
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    probe.bind(("127.0.0.1", port))
    probe.close()
    if os.fork():
        time.sleep(0.2)
        print(f"dns listening on 127.0.0.1:{port}")
        return 0
    os.setsid()
    devnull = os.open(os.devnull, os.O_RDWR)
    for fd in (0, 1, 2):
        os.dup2(devnull, fd)
    pidfile.write_text(str(os.getpid()))
    serve(port)
    return 0


if __name__ == "__main__":
    sys.exit(main())
