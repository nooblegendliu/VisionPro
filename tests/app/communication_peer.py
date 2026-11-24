"""Loopback TCP peer and one-register Modbus TCP simulator for S6.11 acceptance."""

import argparse
from collections import deque
import json
import socket
import socketserver
import struct
import sys
import threading
import time


def receive_exact(connection, count):
    data = bytearray()
    while len(data) < count:
        part = connection.recv(count - len(data))
        if not part:
            raise EOFError
        data.extend(part)
    return bytes(data)


def frame(data):
    return struct.pack(">I", len(data)) + data


class Peer(socketserver.ThreadingTCPServer):
    daemon_threads = True

    def __init__(self, protocol, port=0, fault=None, record_tcp=False):
        self.protocol = protocol
        self.record_tcp = record_tcp
        self.faults = deque([fault] if isinstance(fault, str) else (fault or []))
        self.lock = threading.Condition()
        self.connections = 0
        self.messages = []
        self.requests = []
        self.responses = []
        self.writes = []
        self.register = 0
        super().__init__(("127.0.0.1", port), Handler)

    def take_fault(self, name):
        with self.lock:
            if not self.faults or self.faults[0] != name:
                return False
            self.faults.popleft()
            return True


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(10)
        self.request.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        with self.server.lock:
            self.server.connections += 1
        self.transcript = []
        try:
            if self.server.protocol == "tcp":
                self.tcp()
            else:
                self.modbus()
        except (EOFError, ConnectionError, TimeoutError):
            pass
        finally:
            if self.server.record_tcp:
                print(json.dumps(self.transcript), flush=True)

    def tcp(self):
        if self.server.take_fault("resume-frame"):
            self.request.sendall(frame(b"fresh"))
        while True:
            length, = struct.unpack(">I", receive_exact(self.request, 4))
            if length > 65536:
                return
            payload = receive_exact(self.request, length)
            if self.server.record_tcp:
                self.transcript.append(payload.hex())
            with self.server.lock:
                self.server.messages.append(payload)
                self.server.lock.notify_all()
            if self.server.take_fault("mid-frame"):
                with self.server.lock:
                    self.server.faults.appendleft("resume-frame")
                self.request.sendall(frame(b"stale")[:-2])
                return
            if self.server.take_fault("oversize"):
                self.request.sendall(struct.pack(">I", 65537))
                return
            if payload.startswith("工件=".encode("utf-8")):
                continue
            # Split a header, split a payload, then combine the tail and the next frame in one send.
            first = frame(payload)
            self.request.sendall(first[:2])
            time.sleep(0.02)
            self.request.sendall(first[2:5])
            time.sleep(0.02)
            self.request.sendall(first[5:] + frame(b"NEXT"))

    def modbus(self):
        while True:
            header = receive_exact(self.request, 7)
            tx, protocol, length, unit = struct.unpack(">HHHB", header)
            if protocol != 0 or length != 6:
                return
            pdu = receive_exact(self.request, length - 1)
            function, address, value = struct.unpack(">BHH", pdu)
            with self.server.lock:
                self.server.requests.append(header + pdu)
            if self.server.take_fault("exception"):
                reply = bytes([function | 128, 2])
            elif self.server.take_fault("unknown-exception"):
                reply = bytes([function | 128, 0x42])
            elif address != 16:
                reply = bytes([function | 128, 2])
            elif function == 6:
                with self.server.lock:
                    self.server.register = value
                    self.server.writes.append((unit, address, value))
                if self.server.take_fault("lost-ack"):
                    return
                reply = pdu
                if self.server.take_fault("wrong-transaction"):
                    tx ^= 1
                if self.server.take_fault("wrong-echo"):
                    reply = struct.pack(">BHH", function, address, value ^ 1)
            elif function == 3 and value == 1:
                if self.server.take_fault("lost-read"):
                    return
                with self.server.lock:
                    reply = struct.pack(">BBH", 3, 2, self.server.register)
            else:
                reply = bytes([function | 128, 1])
            response = struct.pack(">HHHB", tx, 0, len(reply) + 1, unit) + reply
            with self.server.lock:
                self.server.responses.append(response)
            self.request.sendall(response)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--protocol", choices=("tcp", "modbus"), required=True)
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--record-tcp", action="store_true", help="print one JSON array of payload hex per closed TCP connection")
    parser.add_argument("--counts-on-stdin", action="store_true", help="report connection and Modbus request counts for each input line")
    parser.add_argument("--fault", action="append", choices=("mid-frame", "oversize", "lost-ack", "lost-read",
                                                            "exception", "unknown-exception", "wrong-transaction", "wrong-echo"))
    args = parser.parse_args()
    with Peer(args.protocol, args.port, args.fault, args.record_tcp) as peer:
        print(peer.server_address[1], flush=True)
        try:
            if args.counts_on_stdin:
                worker = threading.Thread(target=peer.serve_forever, daemon=True)
                worker.start()
                for _ in sys.stdin:
                    with peer.lock:
                        print(json.dumps([peer.connections, len(peer.requests)]), flush=True)
                peer.shutdown()
            else:
                peer.serve_forever()
        except KeyboardInterrupt:
            pass
