#!/usr/bin/env python3
"""Prepare/repeat local haze references on an operator-controlled TESTCTL viewer.

Nothing connects to a viewer unless --execute is given. Run ON the test machine;
this program never uses SSH, launches a viewer, or installs a build.

  haze_reference.py --init private-plan.json
  haze_reference.py private-plan.json --label before --out private-captures --execute
  haze_reference.py private-captures/before/resolved-plan.json --label after \
      --out private-captures --execute

The three Cloudsdale XY viewpoints are fixed below. Enter terrain elevations once
so cameras are 30 m above terrain; reuse resolved-plan.json for the second build.
At each sky checkpoint, select the specified FIXED local/personal sky and export
it to an LLSD XML file. The controller has no sky-selection/readback command, so
the operator must confirm the exported sky is the one actually applied. The file
is copied and hashed, with its exact LLSD values recorded beside the captures.
Never substitute an inventory guess for a reference, or edit/upload region skies.

Weather stays off. Physical on/off and fixed/automatic exposure are compared at
each viewpoint. Changed settings are restored even on failure; camera/location
and the operator-selected personal sky are left at the final reference. Output
contains private locations/sky assets: keep it local, outside source control.
"""

import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import xml.etree.ElementTree as ET


CASES = (
    ("midday-legacy", "Midday Legacy"),
    ("default-night", "Default night"),
    ("heavy-haze-night", "A fixed night sky with high haze density and density multiplier"),
    ("foggy-creator", "A different fixed foggy creator sky/local personal foggy sky"),
)
BASE_SETTINGS = {
    "WeatherEnabled": False,
    "WeatherFromEnvironment": False,
    "RenderVKVolumetricFog": 0,
    "RenderUseExposureSkySettings": False,
    "RenderExposure": 1.0,
    "RenderFarClip": 256.0,
    "CameraAngle": 1.0471975512,
}
OPTIONAL_SETTINGS = {"RenderVKHazeStartDistance": -1.0, "RenderVKHazeMaxOpacity": -1.0}
PHASES = (
    ("physical-fixed", True, False), ("legacy-fixed", False, False),
    ("physical-auto", True, True), ("legacy-auto", False, True),
)
SLUG = re.compile(r"[a-zA-Z0-9][a-zA-Z0-9_-]*\Z")


def template():
    return {
        "schema": 1, "region": "Cloudsdale", "settle_seconds": 5.0,
        "viewpoints": [
            {"name": "southwest", "xy": [64.0, 64.0], "terrain_z": None,
             "focus_xy": [128.0, 128.0], "focus_above_terrain": 8.0},
            {"name": "centre", "xy": [128.0, 128.0], "terrain_z": None,
             "focus_xy": [192.0, 128.0], "focus_above_terrain": 8.0},
            {"name": "northeast", "xy": [192.0, 192.0], "terrain_z": None,
             "focus_xy": [128.0, 128.0], "focus_above_terrain": 8.0},
        ],
        "skies": [{"id": key, "reference": name, "sky_file": None} for key, name in CASES],
    }


def finite(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def validate(plan):
    if plan.get("schema") != 1 or not isinstance(plan.get("region"), str) or not plan["region"].strip():
        raise ValueError("need schema 1 and a region")
    settle = plan.get("settle_seconds", 5.0)
    if not finite(settle) or not 0 <= settle <= 120:
        raise ValueError("settle_seconds must be finite and within 0..120")
    views = plan.get("viewpoints", [])
    if not 2 <= len(views) <= 3:
        raise ValueError("record two or three fixed viewpoints")
    names = set()
    for view in views:
        name = view.get("name", "")
        if not SLUG.fullmatch(name) or name in names:
            raise ValueError("viewpoint names must be unique filename-safe names")
        names.add(name)
        for key in ("xy", "focus_xy"):
            values = view.get(key)
            if not isinstance(values, list) or len(values) != 2 or not all(finite(x) for x in values):
                raise ValueError("each viewpoint needs finite xy and focus_xy pairs")
        if view.get("terrain_z") is not None and not finite(view["terrain_z"]):
            raise ValueError("terrain elevations must be finite")
        if not finite(view.get("focus_above_terrain", 8.0)):
            raise ValueError("focus height must be finite")
    if [sky.get("id") for sky in plan.get("skies", [])] != [key for key, _ in CASES]:
        raise ValueError("plan must contain the four ordered reference sky cases")
    for sky in plan["skies"]:
        if not isinstance(sky.get("reference"), str) or not sky["reference"].strip():
            raise ValueError("each sky needs its exact operator reference")


def write_json(path, value):
    # Captures and metadata may contain private locations or creator assets.
    with open(path, "w", encoding="utf-8") as stream:
        os.chmod(path, 0o600)
        json.dump(value, stream, indent=2, allow_nan=False)
        stream.write("\n")


def llsd(node):
    tag = node.tag
    if tag == "llsd":
        return llsd(node[0])
    if tag == "map":
        children = list(node)
        if len(children) % 2 or any(children[i].tag != "key" for i in range(0, len(children), 2)):
            raise ValueError("malformed LLSD map")
        return {children[i].text or "": llsd(children[i + 1]) for i in range(0, len(children), 2)}
    if tag == "array":
        return [llsd(child) for child in node]
    if tag in ("real", "integer"):
        value = float(node.text or "0") if tag == "real" else int(node.text or "0")
        if not math.isfinite(value):
            raise ValueError("nonfinite value in exported sky")
        return value
    if tag == "boolean":
        return (node.text or "").lower() in ("1", "true")
    if tag == "undef":
        return None
    return node.text or ""


class ControlError(RuntimeError):
    pass


class Client:
    def __init__(self, filename):
        self.filename = str(Path(filename).expanduser().resolve())

    def call(self, *args):
        run = subprocess.run([sys.executable, self.filename, *map(str, args)],
                             capture_output=True, text=True, timeout=75, check=False)
        try:
            reply = json.loads(run.stdout)
        except (ValueError, TypeError) as exc:
            raise ControlError("controller returned no JSON reply") from exc
        if not reply.get("ok") or run.returncode:
            raise ControlError(str(reply.get("error", "controller command failed")))
        return reply

    def set(self, name, value):
        reply = self.call("set", name, json.dumps(value, allow_nan=False))
        actual = reply.get("value")
        same = math.isclose(actual, value, rel_tol=1e-6, abs_tol=1e-6) if finite(actual) and finite(value) else actual == value
        if not same:
            raise ControlError("setting did not accept the requested value: " + name)


def wait_region(client, region, xy, timeout=90):
    deadline = time.monotonic() + timeout
    while True:
        status = client.call("status")
        pos = status.get("pos", [])
        arrived = len(pos) >= 2 and all(finite(x) for x in pos[:2]) and math.dist(pos[:2], xy) <= 8
        if status.get("logged_in") and str(status.get("region", "")).casefold() == region.casefold() and arrived:
            return status
        if time.monotonic() >= deadline:
            raise ControlError("teleport did not reach the reference region")
        time.sleep(1)


def prepare_sky(sky, directory, ask=input):
    print("Select the FIXED local/personal sky: " + sky["reference"])
    print("Disable its day cycle, hide HUDs, and export that exact applied sky locally.")
    filename = sky.get("sky_file")
    if not filename:
        filename = ask("Exported LLSD XML file: ").strip()
    path = Path(filename).expanduser().resolve()
    data = path.read_bytes()
    if len(data) > 4 * 1024 * 1024:
        raise ValueError("sky export exceeds the 4 MiB capture metadata budget")
    values = llsd(ET.fromstring(data))
    if not isinstance(values, dict):
        raise ValueError("export must be a sky LLSD map")
    digest = hashlib.sha256(data).hexdigest()
    if sky.get("sha256") and sky["sha256"] != digest:
        raise ValueError("reference sky changed since the first build's capture")
    if ask("Type APPLIED to confirm this exported sky is active: ").strip() != "APPLIED":
        raise ValueError("operator did not confirm the exact sky")
    target = directory / (sky["id"] + ".xml")
    target.write_bytes(data)
    target.chmod(0o600)
    sky.update(sky_file=str(target.resolve()), sha256=digest)
    write_json(directory / (sky["id"] + ".json"), {"sha256": digest, "values": values})
    return digest


def capture(plan, label, output, client, ask=input, sleep=time.sleep):
    plan = copy.deepcopy(plan)
    validate(plan)
    directory = output / label
    directory.mkdir(parents=True, exist_ok=False, mode=0o700)
    original = {}
    restored_errors = []
    metadata = {"label": label, "frames": [], "optional_missing": [], "complete": False}
    try:
        status = client.call("status")
        if not status.get("logged_in"):
            raise ControlError("a logged-in TESTCTL viewer must already be running")
        metadata["start_status"] = status
        if plan.get("viewport") and plan["viewport"] != status.get("window"):
            raise ControlError("viewport changed between builds; use the same window dimensions")
        plan["viewport"] = status.get("window")
        settings = dict(BASE_SETTINGS, RenderVKPhysicalSky=True, RenderDynamicExposureEnabled=False)
        for name in list(settings) + list(OPTIONAL_SETTINGS):
            try:
                original[name] = client.call("get", name)["value"]
            except ControlError as exc:
                if name in OPTIONAL_SETTINGS and str(exc) == "no such setting":
                    metadata["optional_missing"].append(name)
                else:
                    raise
        write_json(directory / "original-settings.json", original)
        for view in plan["viewpoints"]:
            if view.get("terrain_z") is None:
                value = float(ask("Terrain elevation (m) at %s %s: " % (view["name"], view["xy"])))
                if not finite(value):
                    raise ValueError("terrain elevation must be finite")
                view["terrain_z"] = value
        # One explicit teleport; never retry the operation on an uncertain reply.
        centre = plan["viewpoints"][len(plan["viewpoints"]) // 2]
        client.call("tp", plan["region"], *centre["xy"], centre["terrain_z"] + 2)
        wait_region(client, plan["region"], centre["xy"])
        digests = {}
        for sky in plan["skies"]:
            for name, value in BASE_SETTINGS.items():
                client.set(name, value)
            digest = prepare_sky(sky, directory, ask)
            if sky["id"] == "foggy-creator" and digest == digests.get("heavy-haze-night"):
                raise ValueError("heavy-haze and foggy references must be distinct sky exports")
            digests[sky["id"]] = digest
            write_json(directory / "resolved-plan.json", plan)
            for phase, physical, automatic in PHASES:
                values = dict(BASE_SETTINGS, RenderVKPhysicalSky=physical,
                              RenderDynamicExposureEnabled=automatic)
                values.update({key: value for key, value in OPTIONAL_SETTINGS.items() if key in original})
                for name, value in values.items():
                    client.set(name, value)
                for view in plan["viewpoints"]:
                    position = [*view["xy"], view["terrain_z"] + 30.0]
                    focus = [*view["focus_xy"], view["terrain_z"] + view.get("focus_above_terrain", 8.0)]
                    client.call("cam", *position, *focus)
                    sleep(plan.get("settle_seconds", 5.0))
                    for name in ("WeatherEnabled", "WeatherFromEnvironment"):
                        if client.call("get", name)["value"]:
                            raise ControlError("weather changed during reference capture")
                    frame = "%s-%s-%s.png" % (sky["id"], view["name"], phase)
                    path = directory / frame
                    client.call("snap", str(path.resolve()), "--noui")
                    if not path.is_file() or path.stat().st_size == 0:
                        raise ControlError("snapshot was not written")
                    metadata["frames"].append({"file": frame, "sky_sha256": digest,
                                               "camera": position, "focus": focus,
                                               "settings": values, "status": client.call("status")})
                    write_json(directory / "capture.json", metadata)
        metadata["complete"] = True
    finally:
        for name, value in original.items():
            try:
                client.set(name, value)
            except (ControlError, OSError, subprocess.SubprocessError) as exc:
                restored_errors.append({"setting": name, "error": str(exc)})
        metadata["restore_errors"] = restored_errors
        write_json(directory / "capture.json", metadata)
    if restored_errors:
        raise ControlError("some settings could not be restored; see capture.json")
    print("Captured %d frames. Reuse %s for the other build." %
          (len(metadata["frames"]), directory / "resolved-plan.json"))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("plan", nargs="?", type=Path)
    parser.add_argument("--init", type=Path, metavar="PRIVATE_PLAN")
    parser.add_argument("--label", default="before")
    parser.add_argument("--out", type=Path, default=Path("haze-captures"))
    parser.add_argument("--slctl", default="~/bin/slctl.py")
    parser.add_argument("--execute", action="store_true", help="operate the already-running local viewer")
    args = parser.parse_args(argv)
    if args.init:
        if args.init.exists():
            parser.error("refusing to overwrite an existing plan")
        write_json(args.init, template())
        return 0
    if args.plan is None or not SLUG.fullmatch(args.label):
        parser.error("provide a plan and a filename-safe label")
    plan = json.loads(args.plan.read_text())
    validate(plan)
    if not args.execute:
        print("DRY RUN: no viewer connection, setting changes, teleport or snapshots.")
        print(json.dumps({"label": args.label, "plan": plan, "phases": PHASES,
                          "settings": BASE_SETTINGS, "optional_settings": OPTIONAL_SETTINGS}, indent=2))
        return 0
    capture(plan, args.label, args.out.expanduser().resolve(), Client(args.slctl))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, ControlError, OSError, subprocess.SubprocessError) as error:
        print("Capture failed: " + str(error), file=sys.stderr)
        sys.exit(1)
