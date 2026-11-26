"""Exercise the normal solution loader, image bindings, archive services and CLI."""

import argparse
import copy
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import uuid


def identifier():
    return str(uuid.uuid4())


def path_value(relative):
    return {"base": "solution_directory", "path": relative, "named_root": None}


def binding(target, name, source, cardinality="single_source"):
    return {"target": {"kind": target, "id": name}, "sources": [source],
            "cardinality": cardinality, "conversion": "exact", "accepted_absence": []}


def constant(name, value):
    if isinstance(value, bool):
        kind = "bool"
    elif isinstance(value, int):
        kind, value = "int64", str(value)
    else:
        kind = "string"
    return binding("parameter", name, {"kind": "constant", "value": {"type": {"kind": kind}, "value": value}})


def enum_value(name, value, options, schema_name=None):
    schema = {"kind": "enum", "schema": "visionpro.image.output." + (schema_name or name), "version": 1,
              "members": [{"id": x, "display_name": x} for x in options]}
    return binding("parameter", name, {"kind": "constant", "value": {"type": schema, "value": value}})


def subscribe(flow, node, port, target, multiple=False, system=False):
    selected = {"kind": "system", "field": port} if system else {"kind": "port", "id": port}
    ref = {"kind": "run_output", "value": {"workflow": flow, "node": node,
           "output": selected, "selector": []}}
    return binding("input", target, ref, "multiple_sources" if multiple else "single_source")


class Fixture:
    def __init__(self, cli, sample, root):
        self.cli = cli
        self.root = root
        self.manifest = root / "solution.json"
        shutil.copytree(sample, root, dirs_exist_ok=True)
        self.command("save", "--solution", self.manifest)
        self.solution = json.loads(self.manifest.read_bytes())
        self.flow = self.solution["workflows"][0]
        self.flow_id = self.flow["id"]
        source, threshold, blob, judge = self.flow["nodes"]
        assert source["configuration"]["type"] == "visionpro.image.source"
        self.judge = judge
        self.output = {"id": identifier(), "name": "输出图像 A", "enabled": True, "join": "all_activated",
                       "configuration": {"kind": "tool", "type": "visionpro.image.output", "config_version": 1},
                       "resources": [], "bindings": [
                           subscribe(self.flow_id, source["id"], "image", "image"),
                           subscribe(self.flow_id, blob["id"], "components", "bounds", True),
                           subscribe(self.flow_id, threshold["id"], "roi_mask", "masks", True),
                           constant("original_prefix", "底图"), constant("rendered_prefix", "标注")]}
        self.flow["nodes"].append(self.output)
        self.link(judge, self.output)
        for slot in ("original_path", "rendered_path", "original_ok_path", "rendered_ok_path",
                     "original_ng_path", "rendered_ng_path"):
            key = identifier()
            self.solution["resources"].append({"id": key, "name": slot, "kind": "output_path",
                "location": {"kind": "output_path", "directory": path_value("output/" + slot),
                "naming": "sequence_number", "stem": "image", "extension": "png",
                "overwrite": "unique_suffix", "capacity_bytes": "16777216"}})
            self.output["resources"].append({"slot": slot, "resource": {"id": key, "kind": "output_path", "revision": None}})

    def link(self, source, target):
        self.flow["control_links"].append({"id": identifier(), "source": source["id"], "output": None, "target": target["id"]})

    def command(self, *args):
        run = subprocess.run([self.cli, *args], capture_output=True, timeout=35)
        assert run.returncode == 0, (args, run.stderr.decode("utf-8", "replace"), run.stdout.decode("utf-8", "replace"))
        return json.loads(run.stdout) if run.stdout.strip() else None

    def report(self, success=True):
        self.manifest.write_text(json.dumps(self.solution, ensure_ascii=False), encoding="utf-8")
        process = subprocess.run([self.cli, "run", "--solution", self.manifest, "--workflow", self.flow_id],
                                 capture_output=True, timeout=35)
        assert (process.returncode == 0) == success, process.stderr.decode("utf-8", "replace")
        report = json.loads(process.stdout)
        assert report["status"] == ("Succeeded" if success else "Failed"), report
        return report

    def run(self):
        report = self.report()
        return {node["id"]: {entry["id"]: entry for entry in node["outputs"]} for node in report["nodes"]}

    def configure(self, **parameters):
        self.output["bindings"][:] = [x for x in self.output["bindings"]
                                      if x["target"]["kind"] != "parameter" or x["target"]["id"] not in parameters]
        self.output["bindings"].extend(constant(k, v) for k, v in parameters.items())

    def files(self):
        return set((self.root / "output").rglob("*.png"))


def local(f):
    def result():
        return f.run()[f.output["id"]]

    initial = result()
    assert initial["image"]["status"] == "Produced" and not f.files()
    assert initial["pixel_format"]["value"] == "35127316" and initial["module_status"]["value"] == "1"
    assert (initial["image_width"]["value"], initial["image_height"]["value"]) == ("12", "10")
    assert float(initial["elapsed_ms"]["value"]) >= 0 and initial["big_file_path"]["status"] != "Produced"
    for original, rendered in [(True, False), (False, True), (True, True)]:
        f.configure(save_enabled=True, save_original=original, save_rendered=rendered)
        previous = f.files()
        output = result()
        for kind, enabled in [("original", original), ("rendered", rendered)]:
            field = output[kind + "_path"]
            assert (field["status"] == "Produced") == enabled
            if enabled:
                actual = Path(field["value"]) / output[kind + "_name"]["value"]
                assert actual.is_file() and actual.name == output[kind + "_name"]["value"]
        assert len(f.files() - previous) == int(original) + int(rendered)
    original_file = Path(output["original_path"]["value"]) / output["original_name"]["value"]
    rendered_file = Path(output["rendered_path"]["value"]) / output["rendered_name"]["value"]
    assert original_file.read_bytes() != rendered_file.read_bytes()

    for mode, pattern in (("Sequence", r"底图-\d+\.png"), ("Time", r"底图-\d{8}-\d{6}\.png"),
                          ("None", r"底图\.png")):
        f.output["bindings"].append(enum_value("original_suffix", mode, ["Sequence", "Time", "None"], "suffix"))
        chosen = result()
        name = chosen["original_name"]["value"]
        assert re.fullmatch(pattern, name) and (Path(chosen["original_path"]["value"]) / name).is_file(), (mode, name)
        f.output["bindings"].pop()

    f.configure(trigger_enabled=True, date_directory=True)
    f.output["bindings"].append(subscribe(f.flow_id, f.judge["id"], "judgement", "trigger", system=True))
    for verdict in ["OK", "NG"]:
        # The two known components pass [2,2] and fail [0,0].
        for arg in f.judge["bindings"]:
            if arg["target"]["id"] in ("minimum", "maximum"):
                arg["sources"][0]["value"]["value"] = "2" if verdict == "OK" else "0"
        for condition in ("All", "OK", "NG", "None"):
            f.output["bindings"].append(enum_value("save_condition", condition, ["All", "OK", "NG", "None"]))
            before = f.files()
            output = result()
            created = f.files() - before
            hit = condition in ("All", verdict)
            assert len(created) == (2 if hit else 0), (verdict, condition, created)
            assert output["image"]["status"] == "Produced"
            if hit:
                for kind in ("original", "rendered"):
                    path = Path(output[kind + "_path"]["value"]) / output[kind + "_name"]["value"]
                    base = f.root / "output" / (kind + "_" + verdict.lower() + "_path")
                    assert any(path.samefile(file) for file in created), (path, created)
                    assert path.parent.parent.parent.samefile(base), (path, base)
                    if condition != "All":
                        assert verdict in path.name
            f.output["bindings"].pop()

    source, threshold = f.flow["nodes"][:2]
    f.configure(trigger_enabled=False, save_rendered=False, output_enabled=False)
    f.output["bindings"] = [item for item in f.output["bindings"]
                            if item["target"]["id"] not in ("image", "original_prefix")]
    f.output["bindings"].append(subscribe(f.flow_id, threshold["id"], "binary", "image"))
    dynamic_prefix = subscribe(f.flow_id, source["id"], "file_name", "original_prefix")
    dynamic_prefix["target"]["kind"] = "parameter"
    f.output["bindings"].append(dynamic_prefix)
    level = next(item for item in threshold["bindings"] if item["target"]["id"] == "threshold")
    saved_level = level["sources"][0]["value"]["value"]
    level["sources"][0]["value"]["value"] = "255"
    processed = result()
    processed_path = Path(processed["original_path"]["value"]) / processed["original_name"]["value"]
    assert processed_path.name.startswith("双区域 输入.png")
    assert all(processed[field]["status"] != "Produced" for field in ("image", "image_width", "image_height", "pixel_format"))
    assert processed_path.read_bytes() != original_file.read_bytes()
    f.configure(original_prefix="")
    failed = next(node for node in f.report(success=False)["nodes"] if node["id"] == f.output["id"])
    assert failed["module_status"] == 0 and failed["diagnostics"]
    assert all(slot["status"] != "Produced" for slot in failed["outputs"] if slot["id"] != "module_status")
    status = next(slot for slot in failed["outputs"] if slot["id"] == "module_status")
    assert status["status"] == "Produced" and status["value"] == "0"

    # Recover on the explicit error outlet. The failed node supplies the integer trigger,
    # while the recovery node takes its pixels from the original source of this run.
    recovery = copy.deepcopy(f.output)
    recovery.update(id=identifier(), name="归档失败现场", bindings=[
        subscribe(f.flow_id, source["id"], "image", "image"),
        subscribe(f.flow_id, f.output["id"], "module_status", "trigger"),
        enum_value("trigger_source_type", "Integer", ["Judgement", "Integer", "String"]),
        enum_value("save_condition", "NG", ["All", "OK", "NG", "None"]),
        constant("save_enabled", True), constant("trigger_enabled", True), constant("save_original", True),
        constant("original_prefix", "失败现场")])
    f.flow["nodes"].append(recovery)
    f.link(f.output, recovery)
    f.flow["control_links"][-1]["output"] = "error"
    f.output["execution_policy"] = {"timeout_ms": 0, "max_retries": 0, "error_output": "error"}
    variable = identifier()
    integer = {"kind": "int64"}
    f.flow.setdefault("local_variables", []).append({"id": variable, "name": "最近一次归档状态", "type": integer,
        "initial_value": {"type": integer, "value": "-1"}})
    f.output["result_variables"] = [{"output": "module_status", "scope": "local", "variable": variable}]
    recovered = f.report()
    failure = next(node for node in recovered["nodes"] if node["id"] == f.output["id"])
    assert failure["error_handled"] and failure["module_status"] == 0
    assert next(v for v in recovered["local_variables"] if v["id"] == variable)["value"] == "0"
    handler = next(node for node in recovered["nodes"] if node["id"] == recovery["id"])
    slots = {entry["id"]: entry for entry in handler["outputs"]}
    assert handler["module_status"] == 1 and "NG" in slots["original_name"]["value"]
    assert (Path(slots["original_path"]["value"]) / slots["original_name"]["value"]).is_file()
    f.flow["control_links"].pop()
    f.flow["nodes"].pop()
    del f.output["execution_policy"]

    # A downstream subscriber receives the exact annotated image from this invocation.
    f.configure(output_enabled=True, save_enabled=False)
    f.output["bindings"] = [item for item in f.output["bindings"] if item["target"]["id"] != "image"]
    f.output["bindings"].append(subscribe(f.flow_id, source["id"], "image", "image"))
    consumer = copy.deepcopy(f.output)
    consumer.update(id=identifier(), name="保存订阅图", bindings=[
        subscribe(f.flow_id, f.output["id"], "image", "image"), constant("save_enabled", True),
        constant("save_original", True), constant("original_prefix", "订阅")])
    f.flow["nodes"].append(consumer)
    f.link(f.output, consumer)
    level["sources"][0]["value"]["value"] = saved_level
    saved = f.run()[consumer["id"]]
    copy_path = Path(saved["original_path"]["value"]) / saved["original_name"]["value"]
    assert copy_path.read_bytes() == rendered_file.read_bytes()


def package(f):
    f.configure(save_enabled=True, save_original=True, save_rendered=True)
    reference = f.run()[f.output["id"]]
    expected = {}
    for kind in ("original", "rendered"):
        path = Path(reference[kind + "_path"]["value"]) / reference[kind + "_name"]["value"]
        expected[kind] = path.read_bytes()
        path.unlink()
    assert expected["original"] != expected["rendered"]
    f.configure(save_rendered=False)
    second = copy.deepcopy(f.output)
    second.update(id=identifier(), name="输出图像 B")
    # A global destination also works for a node with no local directory bindings.
    second["resources"] = []
    second["bindings"] = [x for x in second["bindings"]
                          if x["target"]["id"] not in ("save_original", "save_rendered", "rendered_prefix")]
    second["bindings"].extend([constant("save_original", False), constant("save_rendered", True),
                               constant("rendered_prefix", "第二节点")])
    f.flow["nodes"].append(second)
    f.link(f.output, second)
    f.solution["image_archive"] = {"directory": path_value("打包"), "images_per_file": 2}
    reports = f.run()
    packages = list((f.root / "打包").glob("*.dat"))
    assert len(packages) == 1 and not f.files(), packages
    assert reports[f.output["id"]]["big_file_path"]["status"] != "Produced"
    assert Path(reports[second["id"]]["big_file_path"]["value"]).samefile(packages[0])
    entries = f.command("archive-list", packages[0])
    assert len(entries) == 2 and len({entry["origin"] for entry in entries}) == 2, entries
    paths = [Path(name) for name in f.command("archive-extract", packages[0].parent, "--same-name-folder")]
    assert len(paths) == 2 and all(path.parent.name == packages[0].stem for path in paths)
    assert {path.read_bytes() for path in paths} == set(expected.values())
    assert next(path for path in paths if path.name.startswith("第二节点")).read_bytes() == expected["rendered"]

    # A configured global path supersedes both local destinations and the FTP target.
    second["resources"] = copy.deepcopy(f.output["resources"])
    f.configure(ftp_enabled=True, ftp_host="127.0.0.1", ftp_port=1)
    reports = f.run()
    produced = Path(reports[second["id"]]["big_file_path"]["value"])
    assert produced.is_file() and not produced.samefile(packages[0]) and not f.files()

    f.flow["nodes"].remove(second)
    f.flow["control_links"].pop()
    f.configure(save_rendered=True)
    for count in (1, 2):
        f.solution["image_archive"]["images_per_file"] = count
        receipt = f.run()[f.output["id"]]
        paths = {}
        for kind in expected:
            paths[kind] = Path(receipt[kind + "_big_file_path"]["value"])
            name = receipt[kind + "_name"]["value"]
            assert any(e["name"] == name for e in f.command("archive-list", paths[kind]))
        assert paths["original"].samefile(paths["rendered"]) == (count == 2)
        assert paths["rendered"].samefile(Path(receipt["big_file_path"]["value"]))
        for archive in set(paths.values()):
            files = [Path(p) for p in f.command("archive-extract", archive, "--same-name-folder")]
            for kind, path in paths.items():
                if path == archive:
                    entry = next(p for p in files if p.name == receipt[kind + "_name"]["value"])
                    assert entry.read_bytes() == expected[kind]


def ftp(f):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "infrastructure"))
    from image_ftp_fixture import FtpServer

    remote = f.root / "server"
    remote.mkdir()
    with FtpServer(remote, port=0) as server:
        port = server.port
        f.configure(save_enabled=True, save_original=True, ftp_enabled=True, ftp_host="127.0.0.1",
                    ftp_port=port, ftp_user="anonymous", ftp_path="/line/images", original_prefix="online")
        online = f.run()[f.output["id"]]
        assert online["original_path"]["status"] != "Produced" and online["original_name"]["value"].startswith("online")
        fallback = f.output["resources"]
        f.output["resources"] = []
        f.configure(original_prefix="remote-only")
        remote_only = f.run()[f.output["id"]]
        assert (remote / "line/images" / remote_only["original_name"]["value"]).is_file()
        assert remote_only["original_path"]["status"] != "Produced"
        f.output["resources"] = fallback
        local_root = f.root / "output/original_path"
        local_root.parent.mkdir(parents=True, exist_ok=True)
        assert not local_root.exists(), "Online FTP must not need to create the fallback directory"
        local_root.write_text("暂时不能作为目录", encoding="utf-8")
        f.configure(original_prefix="unwritable")
        remote_only = f.run()[f.output["id"]]
        assert (remote / "line/images" / remote_only["original_name"]["value"]).is_file()
        assert local_root.is_file()
        local_root.unlink()
    assert len(list((remote / "line/images").glob("online*.png"))) == 1
    f.configure(original_prefix="offline")
    f.output["resources"] = []
    failure = next(n for n in f.report(success=False)["nodes"] if n["id"] == f.output["id"])
    assert failure["module_status"] == 0 and failure["diagnostics"]
    f.output["resources"] = fallback
    f.run()
    cached = list((f.root / "output").rglob("offline*.png"))
    assert len(cached) == 1
    previous = cached[0].read_bytes()
    with FtpServer(remote, port=port):
        f.configure(original_prefix="reconnected")
        f.run()
    delivered = list((remote / "line/images").glob("offline*.png"))
    assert len(delivered) == 1 and delivered[0].read_bytes() == previous and not cached[0].exists()
    assert len(list((remote / "line/images").glob("reconnected*.png"))) == 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("case", choices=("local", "package", "ftp"))
    parser.add_argument("cli", type=Path)
    parser.add_argument("sample", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="visionpro-image-output-") as scratch:
        fixture = Fixture(args.cli, args.sample, Path(scratch))
        {"local": local, "package": package, "ftp": ftp}[args.case](fixture)
