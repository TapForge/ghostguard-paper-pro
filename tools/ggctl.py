#!/usr/bin/env python3
"""Send one GhostGuard QML command by rewriting only the "cmd" line of the device's control.json.

usage: ggctl.py <name> [--arg-file FILE | --arg TEXT]
The QML polls control.json every 3 s and runs a command whose id changed (any mode, even OFF).
"""
import json
import os
import re
import subprocess
import sys

DEV = "/home/root/ghostguard/control.json"


def ssh(cmd, data=None):
    return subprocess.run(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", os.environ.get("GG_HOST", "remarkable"), cmd],
                          input=data, capture_output=True, text=True, check=True).stdout


def main():
    name = sys.argv[1]
    arg = ""
    if "--arg-file" in sys.argv:
        arg = open(sys.argv[sys.argv.index("--arg-file") + 1]).read()
    elif "--arg" in sys.argv:
        arg = sys.argv[sys.argv.index("--arg") + 1]
    text = ssh("cat " + DEV)
    json.loads(text)  # must be valid before we touch it
    m = re.search(r'^( "cmd": )(\{.*\})\s*$', text, re.M)
    if not m:
        sys.exit("cmd line not found")
    old = json.loads(m.group(2))
    cmd = {"id": int(old.get("id", 0)) + 1, "name": name, "arg": arg}
    new = text[:m.start(2)] + json.dumps(cmd) + text[m.end(2):]
    json.loads(new)
    ssh("cat > %s.new && mv %s.new %s" % (DEV, DEV, DEV), data=new)
    print("sent cmd #%d %s (arg %d chars)" % (cmd["id"], name, len(arg)))


if __name__ == "__main__":
    main()
