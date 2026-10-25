#!/usr/bin/env python3
"""Client for the developer test-control channel (indra/newview/sltestctl.h).

Only works against a viewer built with `make TESTCTL=1`. Run it on the machine the viewer
runs on (the channel listens on 127.0.0.1 only), e.g. `ssh testbox slctl.py status`.

  slctl.py status                      state, region, position, fps
  slctl.py login                       press "Log In" on the login screen (saved credentials)
  slctl.py wait STATE [SECONDS]        poll until the startup state is reached (e.g. STATE_STARTED)
  slctl.py quit | forcequit            graceful logout+quit | immediate quit
  slctl.py get NAME                    read a saved setting
  slctl.py set NAME VALUE              write a saved setting (VALUE is JSON: 1, true, "x", [1,2])
  slctl.py tp REGION X Y Z             teleport without the confirmation dialog
  slctl.py cam PX PY PZ FX FY FZ       camera position + focus in region coordinates
  slctl.py cam reset                   back to the default follow camera
  slctl.py snap [PATH] [--noui]        save a PNG snapshot (default: logs/testctl_snapshot.png)
  slctl.py log TEXT                    write a marker line into Slipstream.log
  slctl.py event PUMP JSON [WAIT]      raw LLEventAPI call, e.g. event LLAgent '{"op":"getPosition"}'
  slctl.py raw JSON                    send one request object as-is
  slctl.py restore FILE.json           set every {"Setting": value} in the file (slsweep's original.json)

Environment: SLCTL_PORT (default 47800), SLCTL_TOKEN_FILE
(default ~/.slipstreamviewer/user_settings/testctl.token).
"""

import json
import os
import socket
import sys
import time

PORT = int(os.environ.get("SLCTL_PORT", "47800"))
TOKEN_FILE = os.path.expanduser(
    os.environ.get("SLCTL_TOKEN_FILE", "~/.slipstreamviewer/user_settings/testctl.token"))


class Channel:
    def __init__(self, timeout=60.0):
        with open(TOKEN_FILE) as f:
            token = f.readline().strip()
        self.sock = socket.create_connection(("127.0.0.1", PORT), timeout=timeout)
        self.reader = self.sock.makefile("r", encoding="utf-8")
        hello = self.call({"token": token})
        if not hello.get("ok"):
            raise SystemExit("auth failed: %s" % hello.get("error"))

    def call(self, req):
        self.sock.sendall((json.dumps(req) + "\n").encode())
        line = self.reader.readline()
        if not line:
            raise SystemExit("connection closed by viewer")
        return json.loads(line)


def connect(retry_sec=0.0):
    """Connect, optionally retrying while the viewer is still starting."""
    deadline = time.time() + retry_sec
    while True:
        try:
            return Channel()
        except (ConnectionRefusedError, FileNotFoundError):
            if time.time() >= deadline:
                raise
            time.sleep(1.0)


def parse_value(text):
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        return text


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return 0
    cmd, args = argv[0], argv[1:]

    if cmd == "wait":
        want = args[0]
        limit = float(args[1]) if len(args) > 1 else 300.0
        deadline = time.time() + limit
        ch = connect(retry_sec=limit)
        while True:
            st = ch.call({"cmd": "status"})
            if st.get("state") == want or (want == "STATE_STARTED" and st.get("logged_in")):
                print(json.dumps(st))
                return 0
            if time.time() >= deadline:
                print(json.dumps(st))
                return 1
            time.sleep(1.0)

    if cmd == "restore":
        ch = connect()
        bad = {}
        for k, v in json.load(open(args[0])).items():
            r = ch.call({"cmd": "set", "name": k, "value": v})
            if not r.get("ok"):
                bad[k] = r.get("error")
        print(json.dumps({"ok": not bad, "failed": bad}))
        return 0 if not bad else 1

    if cmd in ("status", "login", "quit", "forcequit", "ping"):
        req = {"cmd": cmd}
    elif cmd == "get":
        req = {"cmd": "get", "name": args[0]}
    elif cmd == "set":
        req = {"cmd": "set", "name": args[0], "value": parse_value(args[1])}
    elif cmd == "tp":
        req = {"cmd": "teleport", "region": args[0],
               "x": float(args[1]), "y": float(args[2]), "z": float(args[3])}
    elif cmd == "cam":
        if args and args[0] == "reset":
            req = {"cmd": "camera", "reset": True}
        else:
            v = [float(a) for a in args[:6]]
            req = {"cmd": "camera", "pos": v[0:3], "focus": v[3:6]}
    elif cmd == "snap":
        paths = [a for a in args if not a.startswith("--")]
        req = {"cmd": "snapshot", "showui": "--noui" not in args}
        if paths:
            req["path"] = os.path.abspath(paths[0])
    elif cmd == "log":
        req = {"cmd": "log", "text": " ".join(args)}
    elif cmd == "event":
        req = {"cmd": "event", "pump": args[0], "data": json.loads(args[1]) if len(args) > 1 else {}}
        if len(args) > 2:
            req["wait"] = float(args[2])
    elif cmd == "raw":
        req = json.loads(args[0])
    else:
        print("unknown command %r; see --help" % cmd, file=sys.stderr)
        return 2

    reply = connect().call(req)
    print(json.dumps(reply))
    return 0 if reply.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
