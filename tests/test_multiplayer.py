#!/usr/bin/env python3
"""Online play: a host and clients run the same battle in lockstep on localhost.

Usage: python3 -I tests/test_multiplayer.py [ASSETS_DIR]
Three players (teams 0, 1, 0) fight the "fleet" scenario headless for 240 s with
scripted orders (--bot) sent over the network; every peer must end on the same tick
with the same state checksum, and the host must see no desync. A second run perturbs
one client's world mid-battle (--desync-test) and the host must detect it.
"""
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "engine", "build", "shrts")


def run(assets, port, perturb=None):
    common = ["--assets", assets, "--headless", "240", "--bot"]
    host = subprocess.Popen([EXE, "--host", str(port), "--players", "3", "--teams", "0,1,0",
                             "--scenario", "fleet", "--name", "host"] + common,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    # Wait until the host is listening.
    line = host.stdout.readline()
    if "hosting on port" not in line:
        print("FAIL host didn't start:", line)
        return None
    clients = []
    for k in range(2):
        extra = ["--desync-test", str(perturb)] if perturb is not None and k == 1 else []
        clients.append(subprocess.Popen([EXE, "--join", f"127.0.0.1:{port}", "--name", f"p{k + 1}"] + common + extra,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True))
    outs = [host.communicate(timeout=600)[0]] + [c.communicate(timeout=600)[0] for c in clients]
    results = []
    for out in outs:
        c = re.findall(r"chat lines (\d+)", out)
        if not c or int(c[-1]) != 3:
            print("FAIL chat: a peer got", c[-1] if c else "no", "lines (expected 3)")
            return None
        m = re.findall(r"final tick (\d+) checksum ([0-9a-f]+) desync (-?\d+)", out)
        if not m:
            print("FAIL no result from a peer:\n" + out[-2000:])
            return None
        results.append(m[-1])
    return results


def rejoin(assets, port):
    """A client drops out mid-battle and comes back: it replays the history and must
    end up with the same battle as everyone else."""
    common = ["--assets", assets, "--headless", "600", "--bot", "--pace", "40"]   # ~15 s of wall time
    # The host runs at 40x real time, so the battle is still on when p2 drops at 1.5 s.
    host = subprocess.Popen([EXE, "--host", str(port), "--players", "3", "--teams", "0,1,0", "--scenario", "fleet",
                             "--name", "host", "--pace", "40"] + common, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if "hosting on port" not in host.stdout.readline():
        return None
    p1 = subprocess.Popen([EXE, "--join", f"127.0.0.1:{port}", "--name", "p1"] + common,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    p2 = subprocess.Popen([EXE, "--join", f"127.0.0.1:{port}", "--name", "p2"] + common,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    # Paced at 40x real time: drop it 3 s in (two minutes of battle), then come back.
    while "on team" not in p2.stdout.readline():
        pass
    time.sleep(3)
    p2.kill()                                   # connection lost mid-battle
    p2.wait()
    time.sleep(0.5)
    p2 = subprocess.Popen([EXE, "--join", f"127.0.0.1:{port}", "--name", "p2"] + common,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    outs = [x.communicate(timeout=900)[0] for x in (host, p1, p2)]
    res = []
    for out in outs:
        m = re.findall(r"final tick (\d+) checksum ([0-9a-f]+)", out)
        if not m:
            print("FAIL rejoin: no result from a peer:\n" + out[-1500:])
            return None
        res.append(m[-1])
    return res


def main(assets):
    fails = 0
    port = 47000 + os.getpid() % 1000
    r = run(assets, port)
    if r is None:
        return 1
    print("lockstep:", r)
    if len({x[0] for x in r}) != 1 or len({x[1] for x in r}) != 1:
        print("FAIL peers disagree")
        fails += 1
    if r[0][2] != "-1":
        print("FAIL host reported a desync at turn", r[0][2])
        fails += 1
    r = run(assets, port + 1, perturb=2000)
    if r is None:
        return 1
    print("perturbed client:", r)
    if r[0][2] == "-1":
        print("FAIL host missed the desync")
        fails += 1
    r = rejoin(assets, port + 2)
    print("rejoined client:", r)
    if r is None or len({x[1] for x in r}) != 1:
        print("FAIL the rejoined client's battle differs")
        fails += 1
    print(f"{fails} failures")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "assets")))
