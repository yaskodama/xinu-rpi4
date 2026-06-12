#!/usr/bin/env python3
"""nqueens_with_gc_demo.py — run N-Queens with a co-resident GC actor.

Loads the combined script once, then:
  1. snapshots /api/actors-gc (just GC + Root present)
  2. POSTs bootstrap(N) to Root, polls get_done(0)
  3. while N-Queens runs, every 200ms snapshot actor count + GC dry-run
     target count (should always be 0 — Solvers are too young)
  4. on done, read get_solutions(0) and verify
  5. tick the GC (manual sweep) to confirm 0 zombies left
"""
import os, subprocess, sys, time, urllib.request

PI      = "192.168.3.100"
ROOT    = "/Users/kodamay/ocaml-app/abclcp-project"
AIPL2C  = os.path.join(ROOT, "_build/default/src/aipl2c.exe")
ABCL    = "/Users/kodamay/projects/xinu-rpi4/examples/nqueens_with_gc.abcl"
N       = int(sys.argv[1]) if len(sys.argv) > 1 else 8
GC_SLOT = 0
ROOT_SLOT = 1

def http(path, body=None, timeout=15):
    url = f"http://{PI}{path}"
    if body is None: req = urllib.request.Request(url, method="GET")
    else:
        data = body if isinstance(body, bytes) else body.encode()
        req = urllib.request.Request(url, data=data, method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read().decode()

def compile_abcl(path):
    open("/tmp/_q.abcl", "w").write(open(path).read())
    subprocess.run([AIPL2C, "/tmp/_q.abcl", "--xinu-jit", "--no-typecheck",
                    "-o", "/tmp/_q.c"], check=True, capture_output=True, text=True)
    return open("/tmp/_q.c", "rb").read()

def gc_dry(threshold=0):
    r = http(f"/gc?threshold_ms={threshold}&dry=1", body=b"")
    parts = {}
    for tok in r.split():
        if "=" in tok and not tok.endswith("("):
            k, v = tok.split("=", 1)
            try: parts[k] = int(v)
            except: parts[k] = v
    return parts.get("killed", 0), parts.get("scanned", 0)

def send(to, m, arg=0, timeout=5):
    return http(f"/actor/send?to={to}&m={m}&arg={arg}", timeout=timeout).strip()

def main():
    print(f"=== N-Queens N={N} + GC actor co-resident ===\n")

    csrc = compile_abcl(ABCL)
    r = http("/actor/load", body=csrc, timeout=20).strip()
    print(f"[1] /actor/load -> {r}")

    time.sleep(0.3)
    targets0, total0 = gc_dry(0)
    print(f"    initial: total={total0} targets@0={targets0}  (GC slot 0, Root slot 1)")

    print(f"\n[2] POST bootstrap({N}) to Root (slot {ROOT_SLOT})")
    t0 = time.time()
    send(ROOT_SLOT, "bootstrap", N)

    print(f"\n[3] polling actor count + GC targets while N-Queens runs")
    print(f"    {'t_ms':>6}  {'total':>5}  {'targ@500':>8}  {'targ@5000':>9}  done")
    last_done = 0
    samples = 0
    while True:
        time.sleep(0.2)
        elapsed = int((time.time() - t0) * 1000)
        try:
            done = int(send(ROOT_SLOT, "get_done"))
        except Exception:
            done = -1
        _, total = gc_dry(99999999)
        t500, _  = gc_dry(500)
        t5000, _ = gc_dry(5000)
        print(f"    {elapsed:>6}  {total:>5}  {t500:>8}  {t5000:>9}  {done}", flush=True)
        samples += 1
        if done == 1: break
        if samples > 60:
            print("    TIMEOUT"); break

    sols = int(send(ROOT_SLOT, "get_solutions"))
    elapsed = (time.time() - t0) * 1000
    print(f"\n[4] N-Queens done: solutions={sols}  elapsed={elapsed:.0f}ms")

    print(f"\n[5] GC manual tick (slot {GC_SLOT}.tick)")
    send(GC_SLOT, "tick")
    last_killed = int(send(GC_SLOT, "get", 2))
    last_scanned = int(send(GC_SLOT, "get", 3))
    killed_total = int(send(GC_SLOT, "get", 0))
    print(f"    last_scanned={last_scanned}  last_killed={last_killed}  killed_total={killed_total}")

    _, after_total = gc_dry(99999999)
    print(f"\n[6] final inventory: total={after_total} (should be 2: GC + Root)")
    print(http("/api/actors-gc?threshold_ms=0&limit=10"))

if __name__ == "__main__":
    main()
