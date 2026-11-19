"""Build the S6.12 five-lane inspection package using the S6.9 and S6.11 samples."""

import argparse
import copy
import json
from pathlib import Path
import shutil


SAMPLES = Path(__file__).resolve().parents[1]


def uid(number):
    return f"5ca4f87a-ea1e-4d62-89b0-{number:012d}"


def named(items, key, value):
    return next(item for item in items if item[key] == value)


def prepare(directory, port):
    inspection = SAMPLES / "s6-inspection"
    solution = json.loads((inspection / "solution.json").read_bytes())
    communication = json.loads((SAMPLES / "s6-communication" / "solution.json").read_bytes())
    flow = named(solution["workflows"], "id", uid(6910))
    layout = named(solution["layouts"], "workflow", flow["id"])
    source = named(communication["workflows"], "id", uid(61130))
    endpoint = named(communication["resources"], "id", uid(61101))
    named(endpoint["location"]["configuration"]["value"], "id", "port")["value"]["value"] = str(port)
    solution["id"] = uid(612000)
    solution["name"] = "S6.12 five-lane workpiece inspection and TCP delivery"
    solution["workflows"] = [flow]
    solution["layouts"] = [layout]
    solution["resources"] = [resource for resource in solution["resources"] if resource["id"] in (uid(6901), uid(6902))]
    solution["resources"].append(endpoint)
    flow["interface"]["inputs"] = source["interface"]["inputs"]

    def append_node(template_id, number, name, x, y):
        node = copy.deepcopy(named(source["nodes"], "id", uid(template_id)))
        node.update(id=uid(number), name=name)
        # The donor flow has one source per binding. Move its references into the inspection flow.
        for binding in node["bindings"]:
            for origin in binding["sources"]:
                if origin["kind"] in ("public_input", "run_output"):
                    origin["value"]["workflow"] = flow["id"]
        flow["nodes"].append(node)
        layout["nodes"].append({"node": node["id"], "x": x, "y": y})
        return node

    def link(number, parent, child):
        flow["control_links"].append({"id": uid(number), "source": uid(parent), "target": uid(child), "output": None})

    def input_binding(node, target):
        return next(b for b in node["bindings"] if b["target"]["id"] == target)["sources"][0]

    # Derive the queue correlation and the TCP verdict from the same per-run Int64 public input.
    key = append_node(61131, 6970, "Format workpiece key", -260, 320)
    input_binding(key, "format")["value"]["value"] = "part-{value}"
    flow["entries"] = [key["id"]]
    link(697001, 6970, 6911)
    for queued, number, y, verdict in ((6943, 6971, 220, "OK"), (6945, 6974, 420, "NG")):
        queue_node = named(flow["nodes"], "id", uid(queued))
        key_source = copy.deepcopy(input_binding(queue_node, "payload"))
        key_source["value"]["node"] = key["id"]
        input_binding(queue_node, "key").update(key_source)
        label = append_node(61131, number, f"Format {verdict} acknowledgement", 2040, y)
        input_binding(label, "format")["value"]["value"] = "工件={value} " + verdict
        encoder = append_node(61132, number + 1, f"Encode {verdict} for TCP", 2290, y)
        input_binding(encoder, "text")["value"]["node"] = label["id"]
        sender = append_node(61133, number + 2, f"Send {verdict} to TCP", 2540, y)
        input_binding(sender, "payload")["value"]["node"] = encoder["id"]
        for offset, (parent, child) in enumerate(((queued, number), (number, number + 1), (number + 1, number + 2))):
            link(100 * number + offset, parent, child)

    # Create a new package without replacing an existing solution or its resources.
    directory.mkdir(parents=True, exist_ok=False)
    (directory / "resources").mkdir()
    for resource in solution["resources"]:
        if resource["location"]["kind"] == "managed":
            relative = resource["location"]["path"]["path"]
            shutil.copyfile(inspection / relative, directory / relative)
    (directory / "solution.json").write_text(json.dumps(solution, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, help="new package directory")
    parser.add_argument("--port", type=int, required=True, help="loopback TCP peer port")
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("--port must be in 1..65535")
    prepare(args.directory, args.port)
