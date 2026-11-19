"""Run the production CLI against independent loopback peers; inspect requests on the wire."""

import argparse
import copy
from contextlib import contextmanager
import json
from pathlib import Path
import subprocess
import tempfile
import threading

from communication_peer import Peer


@contextmanager
def serving(protocol, fault=None):
    with Peer(protocol, fault=fault) as peer:
        worker = threading.Thread(target=peer.serve_forever, kwargs={"poll_interval": 0.02})
        worker.start()
        try:
            yield peer
        finally:
            peer.shutdown()
            worker.join()


def node(report, uid):
    return next(item for item in report["nodes"] if item["id"] == uid)


def produced(call, port):
    slot = next(output for output in call["outputs"] if output["id"] == port)
    assert slot["status"] == "Produced", slot
    return slot["value"]


def error_outlet(flow, source, link):
    flow["nodes"][source]["execution_policy"] = {"timeout_ms": 0, "max_retries": 0, "error_output": "error"}
    flow["control_links"][link]["output"] = "error"
    flow["interface"]["outputs"] = []
    flow["outputs"] = []


class Fixture:
    def __init__(self, args, directory):
        self.cli = args.cli.resolve()
        self.template = json.loads(args.solution.read_bytes())
        self.manifest = Path(directory) / "solution.json"

    def setup(self, peer):
        self.solution = copy.deepcopy(self.template)
        resource = self.solution["resources"][int(peer.protocol == "modbus")]
        fields = resource["location"]["configuration"]["value"]
        next(field for field in fields if field["id"] == "port")["value"]["value"] = str(peer.server_address[1])
        return self.solution["workflows"]

    def invoke(self, flow, *args, command="run", expected=0):
        self.manifest.write_text(json.dumps(self.solution, ensure_ascii=False), encoding="utf-8")
        result = subprocess.run([self.cli, command, "--solution", self.manifest, "--workflow", flow["id"], *args],
                                capture_output=True, timeout=12)
        assert result.returncode == expected, (result.returncode, result.stderr.decode(), result.stdout[-800:])
        return json.loads(result.stdout) if result.stdout else json.loads(result.stderr)


def tcp_checks(suite):
    with serving("tcp") as peer:
        flow, _, parts, _ = suite.setup(peer)
        for packet in (b"A\0B", b"\x00\xff", b""):
            args = [] if packet == b"A\0B" else ["--input", "packet=" + packet.hex()]
            result = suite.invoke(flow, *args)
            calls = [node(result, n["id"]) for n in flow["nodes"]]
            assert result["quiescent"] and result["status"] == "Succeeded"
            assert produced(calls[0], "bytes_sent") == str(len(packet))
            assert produced(calls[1], "payload") == {"size": len(packet), "hex": packet.hex()}
            assert produced(calls[2], "payload") == {"size": 4, "hex": "4e455854"}
        assert peer.messages == [b"A\0B", b"\x00\xff", b""] and peer.connections == 3
        bad = suite.invoke(flow, "--input", "packet=0x12", expected=2)
        assert bad["stage"] == "arguments" and peer.connections == 3
        stream = suite.invoke(parts, "--frame", "41", "--frame", "42", command="trigger")
        assert [event["correlation"]["key"] for event in stream["events"]] == ["41", "42"]
        assert all(event["correlation"]["domain"] == "s611.workpiece" for event in stream["events"])
        with peer.lock:
            assert peer.lock.wait_for(lambda: len(peer.messages) >= 5, timeout=2)
            assert peer.messages[3:] == [text.encode("utf-8") for text in ("工件=41", "工件=42")]

    with serving("tcp", "mid-frame") as peer:
        flow = suite.setup(peer)[0]
        error_outlet(flow, 1, 1)
        result = suite.invoke(flow)
        interrupted = node(result, flow["nodes"][1]["id"])
        assert interrupted["error_handled"] and interrupted["outputs"][0]["status"] == "NotProduced"
        assert produced(node(result, flow["nodes"][2]["id"]), "payload")["hex"] == b"fresh".hex()
        assert peer.connections == 2 and peer.messages == [b"A\0B"]
    with serving("tcp", "oversize") as peer:
        flow = suite.setup(peer)[0]
        result = suite.invoke(flow, expected=1)
        failed = node(result, flow["nodes"][1]["id"])
        assert failed["diagnostics"] and failed["outputs"][0]["status"] == "NotProduced"
        assert len(result["call_order"]) == 2


def plc_checks(suite):
    with serving("modbus") as peer:
        flow = suite.setup(peer)[1]
        out = suite.invoke(flow)
        assert produced(node(out, flow["nodes"][0]["id"]), "confirmed") is True
        assert produced(node(out, flow["nodes"][1]["id"]), "value") == "4660"
        assert peer.writes == [(1, 16, 4660)] and peer.connections == 1
        assert [request[2:].hex() for request in peer.requests] == ["00000006010600101234", "00000006010300100001"]
        assert peer.requests[0][:2] != peer.requests[1][:2]
        assert peer.responses == [peer.requests[0], peer.requests[1][:2] + bytes.fromhex("000000050103021234")]
        write = node(out, flow["nodes"][0]["id"])
        assert write["adapter_duration"] == {"value": None, "unit": "unknown"} and int(write["host_elapsed_ns"]) >= 0
        print("FC06 request / echo:", peer.requests[0].hex(), peer.responses[0].hex(),
              "host_elapsed_ns:", write["host_elapsed_ns"], "device duration: unknown")
        print("FC03 request / response:", peer.requests[1].hex(), peer.responses[1].hex())

    # Keep the write unconfirmed in the trace. The explicit error outlet reads back
    # using a new connection. The independent peer counts every write, including repeats.
    with serving("modbus", "lost-ack") as peer:
        flow = suite.setup(peer)[1]
        error_outlet(flow, 0, 0)
        out = suite.invoke(flow)
        write = node(out, flow["nodes"][0]["id"])
        unknown = next(d for d in write["diagnostics"] if d["code"] == "ExternalEffectUnknown")
        assert unknown["operation_key"]["node"] == write["id"] and unknown["external"]["raw_code"] == "0", unknown
        assert write["error_handled"] and len(write["attempts"]) == 1 and write["outputs"][0]["status"] == "NotProduced"
        assert produced(node(out, flow["nodes"][1]["id"]), "value") == "4660"
        assert peer.connections == 2 and peer.writes == [(1, 16, 4660)] and len(peer.requests) == 2
        assert len(peer.responses) == 1 and peer.responses[0][7] == 3
        verification = suite.solution["workflows"][3]
        reread = suite.invoke(verification)
        assert produced(node(reread, verification["entries"][0]), "value") == "4660"
        assert [request[7] for request in peer.requests] == [6, 3, 3] and len(peer.writes) == 1

    with serving("modbus", "lost-read") as peer:
        flow = suite.setup(peer)[1]
        flow["nodes"][1]["execution_policy"] = {"timeout_ms": 0, "max_retries": 1, "error_output": None}
        out = suite.invoke(flow)
        read = node(out, flow["nodes"][1]["id"])
        assert len(read["attempts"]) == 2 and produced(read, "value") == "4660"
        assert [req[7] for req in peer.requests] == [6, 3, 3] and len(peer.writes) == 1 and peer.connections == 2

    for fault in ["exception", "unknown-exception", "wrong-transaction", "wrong-echo"]:
        with serving("modbus", fault) as peer:
            flow = suite.setup(peer)[1]
            out = suite.invoke(flow, expected=1)
            write = node(out, flow["nodes"][0]["id"])
            assert len(write["attempts"]) == 1 and write["outputs"][0]["status"] == "NotProduced"
            assert len(out["call_order"]) == 1
            if fault.endswith("exception"):
                code = 2 if fault == "exception" else 0x42
                d = next(d for d in write["diagnostics"] if d.get("external", {}).get("raw_code") == str(code))
                assert d["external"]["mapped"] == (code == 2) and not peer.writes
                if code == 0x42:
                    assert d["code"] == "ExternalEffectUnknown" and d["operation_key"]["node"] == write["id"]
            else:
                assert any(d["code"] == "ExternalEffectUnknown" for d in write["diagnostics"])
                assert peer.writes == [(1, 16, 4660)]

    with serving("modbus") as peer:
        flow = suite.setup(peer)[1]
        flow["nodes"][0]["execution_policy"] = {"timeout_ms": 0, "max_retries": 1, "error_output": None}
        rejected = suite.invoke(flow, expected=1)
        assert any(d["code"] == "MissingCapability" for d in rejected["diagnostics"])
        assert not peer.requests and not peer.writes

    with serving("modbus") as peer:
        flow = suite.setup(peer)[1]
        config = suite.solution["resources"][1]["location"]["configuration"]
        config["type"]["version"] = 2
        bad = suite.invoke(flow, expected=1)
        assert any(d["code"] == "IncompatibleSchemaVersion" for d in bad["diagnostics"])
        assert peer.connections == 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("protocol", choices=["tcp", "plc"])
    parser.add_argument("cli", type=Path)
    parser.add_argument("solution", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as directory:
        fixture = Fixture(args, directory)
        (tcp_checks if args.protocol == "tcp" else plc_checks)(fixture)
    print(args.protocol, "loopback CLI acceptance passed")
