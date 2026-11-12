#!/usr/bin/env python3
"""ggq.py: run one QML eval on the tablet through the GhostGuard control file and print its result (Mac side).

usage: python3 -I tools/ggq.py FILE.js [--sub KEY=VALUE ...] [--wait SECONDS]
       python3 -I tools/ggq.py --code 'EXPRESSION'
The QML side polls control.json every 3 s; the result is the text after "] => " in the journal line
"XOVI-GG71 EVAL [...] => <result>". --sub replaces the literal KEY in the file with VALUE (e.g. a search pattern).
"""
import os
import subprocess
import sys
import time

SSH = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", os.environ.get("GG_HOST", "remarkable")]
HERE = os.path.dirname(os.path.abspath(__file__))


def ssh(cmd):
    return subprocess.run(SSH + [cmd], capture_output=True, text=True, check=False).stdout


def main():
    args = sys.argv[1:]
    wait = 14.0
    subs = []
    code = None
    path = None
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--wait":
            wait = float(args[i + 1]); i += 2; continue
        if a == "--sub":
            k, _, v = args[i + 1].partition("="); subs.append((k, v)); i += 2; continue
        if a == "--code":
            code = args[i + 1]; i += 2; continue
        path = a; i += 1
    if code is None:
        code = open(path).read()
    for k, v in subs:
        code = code.replace(k, v)
    since = ssh("date +%s").strip()
    tmp = os.path.join(os.environ.get("TMPDIR", "/tmp"), "ggq_%d.js" % os.getpid())
    with open(tmp, "w") as f:
        f.write(code)
    try:
        r = subprocess.run([sys.executable, "-I", os.path.join(HERE, "ggctl.py"), "eval", "--arg-file", tmp],
                           capture_output=True, text=True)
        if r.returncode:
            sys.exit("send failed: " + r.stderr.strip())
    finally:
        os.remove(tmp)
    t_end = time.time() + wait
    while time.time() < t_end:
        time.sleep(1.5)
        out = ssh("journalctl -u xochitl --since @%s --no-pager -o cat | grep -a '\\] => ' | tail -n 1" % since)
        if "] => " in out:
            res = out.split("] => ", 1)[1]
            res = res.rsplit(" (log qrc:", 1)[0]
            print(res.strip())
            return
    sys.exit("no result within %.0f s" % wait)


if __name__ == "__main__":
    main()
