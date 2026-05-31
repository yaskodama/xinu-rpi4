#!/usr/bin/env python3
"""gc_demo.py — no-reboot GC demonstration on a live Pi 4.

Walks through several spawn→sweep cycles WITHOUT rebooting the box,
showing that the actor-pool GC keeps the system clean indefinitely as
long as it's invoked.  Use this when you want to see the GC working
against a running system (vs. gc_bench.py which measures cost).

Sequence:
  1. read baseline /api/actors-gc
  2. for each N in {25, 50, 200, 800}:
       a. load gc_zombies.abcl with N zombies     (/actor/load)
       b. show inventory                         (/api/actors-gc)
       c. wait 2 s so zombies cross threshold
       d. dry-run /gc                            — what will be killed?
       e. real /gc                               — sweep
       f. confirm inventory is back to baseline
"""
import os, subprocess, sys, time, urllib.request

PI      = "192.168.3.100"
ROOT    = "/Users/kodamay/ocaml-app/abclcp-project"
AIPL2C  = os.path.join(ROOT, "_build/default/src/aipl2c.exe")
ZOMBIES = "/tmp/gc_zombies.abcl"
THRESHOLD = 1500   # ms; anything idle > 1.5 s gets killed

def http(path, body=None, timeout=15):
    url = f"http://{PI}{path}"
    if body is None:
        req = urllib.request.Request(url, method="GET")
    else:
        data = body if isinstance(body, bytes) else body.encode()
        req = urllib.request.Request(url, data=data, method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read().decode()

def compile_abcl(path, **subs):
    src = open(path).read()
    for k, v in subs.items():
        src = src.replace(f"__{k}__", str(v))
    open("/tmp/_z.abcl", "w").write(src)
    subprocess.run([AIPL2C, "/tmp/_z.abcl", "--xinu-jit", "--no-typecheck",
                    "-o", "/tmp/_z.c"], check=True, capture_output=True, text=True)
    return open("/tmp/_z.c", "rb").read()

def gc_sweep(threshold_ms, dry=0):
    t0 = time.time()
    r = http(f"/gc?threshold_ms={threshold_ms}&dry={dry}", body=b"")
    elapsed_ms = (time.time() - t0) * 1000.0
    parts = {}
    for tok in r.split():
        if "=" in tok and not tok.endswith("("):
            k, v = tok.split("=", 1)
            try: parts[k] = int(v)
            except: parts[k] = v
    return parts.get("killed", -1), parts.get("scanned", -1), elapsed_ms

def show_inventory(label, max_rows=8):
    """Print actor count plus a few inventory rows.  /api/actors-gc tops out
    at ~50 rows because tcp_send() is single-MTU; we ask /gc?dry=1 for the
    total (no inventory data needed) and only fetch the rows when small."""
    _, total, _ = gc_sweep(0, dry=1)            # threshold=0 -> "scan all"
    print(f"      {label}: {total} actor(s) live")
    if total <= 40:                              # safely below MTU cap
        try:
            txt = http("/api/actors-gc").strip().splitlines()
            for ln in txt[1:max_rows+1]:
                print(f"        {ln}")
            if len(txt) - 1 > max_rows:
                print(f"        ... ({len(txt)-1-max_rows} more)")
        except Exception as e:
            print(f"        (inventory fetch skipped: {e})")

def main():
    print(f"=== GC demo on live Pi 4 ({PI}) — no reboot ===\n")
    try:
        http("/", timeout=3)
    except Exception as e:
        print(f"!! Pi 4 not reachable at {PI}: {e}", file=sys.stderr); sys.exit(1)

    show_inventory("baseline (before any zombie)")

    total_killed = 0
    for n in [25, 50, 200, 800]:
        print(f"\n--- cycle: spawn {n} zombies, wait, sweep ---")

        csrc = compile_abcl(ZOMBIES, N=n)
        r = http("/actor/load", body=csrc, timeout=30).strip()
        print(f"   /actor/load -> {r[:80]}")

        show_inventory(f"after spawn (N={n})")

        print(f"   waiting 2 s for zombies to age past {THRESHOLD} ms...")
        time.sleep(2.0)

        # dry-run first
        d_killed, d_scanned, d_ms = gc_sweep(THRESHOLD, dry=1)
        print(f"   dry-run /gc?threshold_ms={THRESHOLD}: would kill "
              f"{d_killed} of {d_scanned} ({d_ms:.1f}ms)")

        # real sweep
        killed, scanned, ms = gc_sweep(THRESHOLD)
        per_us = (ms / max(scanned, 1) * 1000.0)
        print(f"   real    /gc?threshold_ms={THRESHOLD}: killed "
              f"{killed} of {scanned} ({ms:.1f}ms, {per_us:.1f}μs/actor)")

        show_inventory("after sweep")
        total_killed += killed

    print(f"\n=== demo done: killed {total_killed} total zombies, no reboot ===")
    print(f"Pi 4 still up — /api/actors-gc should show whatever survived.")

if __name__ == "__main__":
    main()
