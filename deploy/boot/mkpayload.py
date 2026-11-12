#!/usr/bin/env python3
"""mkpayload.py: build the runtime QML payloads that ggapply.sh writes into control.json (Mac side; run by
deploy/persist_install.sh at every install, so the payloads always come from the current JS files).

usage: python3 -I deploy/boot/mkpayload.py OUTDIR [--kbd FILE] [--clock FILE]
  defaults: tools/kbd_overlay.js and tools/clock12.js of this project

For each of kbd, clock, status and splash it writes
  OUTDIR/<name>.cmd  ONE line: the rest of control.json's "cmd" object after its id,
                     '"name": "eval", "arg": "<the JS, JSON-escaped, ASCII only>"}'
                     (ggapply.sh writes ' "cmd": {"id": N, ' + this line; the QML evals the arg)
  OUTDIR/<name>.ok   the fixed start of the string the JS returns; the QML logs
                     "XOVI-GG71 EVAL [<js>] => <result>" and ggapply.sh looks for "] => " + this text
The kbd and clock files must be one IIFE, "(function () { ... })()", ending in return "<text>" + ...
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(os.path.dirname(HERE))

STATUS_JS = "(function () { console.log('GGQ 97 status'); return 'status asked'; })()"
SPLASH_JS = "(function () { console.log('GGQ 95 rclean 0 0 1620 2160'); return 'splash clean asked'; })()"


def die(msg):
    sys.stderr.write("mkpayload: " + msg + "\n")
    sys.exit(1)


def expect_of(js, path):
    """the literal start of the IIFE's final return value"""
    found = list(re.finditer(r'\breturn\s+(["\'])((?:\\.|(?!\1).)*)\1', js))
    if not found:
        die("%s: no return \"...\" found" % path)
    text = found[-1].group(2)
    if len(text) < 4 or "\\" in text or "\n" in text:
        die("%s: the return text %r is not usable as a result check" % (path, text))
    return text


def check_iife(js, path):
    s = js.strip()
    if not (s.startswith("(function") and re.search(r"\}\)\(\);?$", s)):
        die("%s: not one IIFE '(function () { ... })()'" % path)
    if "`" in s:
        die("%s: template literals are not used in GhostGuard payloads (keep the JS as tested)" % path)


def write(outdir, name, js, ok):
    line = '"name": "eval", "arg": ' + json.dumps(js, ensure_ascii=True) + "}"
    if "\n" in line or "\r" in line:
        die(name + ": the escaped line still holds a newline")
    # round trip, exactly as ggapply.sh assembles it
    obj = json.loads('{"cmd": {"id": 1, ' + line + "}")
    if obj["cmd"]["arg"] != js or obj["cmd"]["name"] != "eval":
        die(name + ": round trip changed the JS")
    with open(os.path.join(outdir, name + ".cmd"), "w") as f:
        f.write(line + "\n")
    with open(os.path.join(outdir, name + ".ok"), "w") as f:
        f.write(ok + "\n")
    return len(line)


def main():
    args = sys.argv[1:]
    if not args or args[0].startswith("-"):
        die("usage: mkpayload.py OUTDIR [--kbd FILE] [--clock FILE]")
    outdir = args.pop(0)
    files = {"kbd": os.path.join(PROJECT, "tools", "kbd_overlay.js"),
             "clock": os.path.join(PROJECT, "tools", "clock12.js")}
    while args:
        opt = args.pop(0)
        if opt in ("--kbd", "--clock") and args:
            files[opt[2:]] = args.pop(0)
        else:
            die("unknown option or missing value: " + opt)
    if not os.path.isdir(outdir):
        die("no directory " + outdir)
    out = {}
    for name in ("kbd", "clock"):
        path = files[name]
        try:
            with open(path, encoding="utf-8") as f:
                js = f.read()
        except OSError as e:
            die("%s: %s" % (path, e))
        check_iife(js, path)
        out[name] = (js, expect_of(js, path))
    out["status"] = (STATUS_JS, "status asked")
    out["splash"] = (SPLASH_JS, "splash clean asked")
    for name in ("kbd", "clock", "status", "splash"):
        js, ok = out[name]
        n = write(outdir, name, js, ok)
        print("%-6s %5d chars, result starts %r" % (name, n, ok))


if __name__ == "__main__":
    main()
