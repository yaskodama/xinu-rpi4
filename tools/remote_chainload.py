#!/usr/bin/env python3
"""Remote kernel chainload for Xinu Pi4 (RAM-only, no SD write -> no brick risk).

Splits a kernel image into chunks, POSTs each to the running Pi4's /chainload
route (staging RAM 0x4000000), then triggers GET /chainload?go=1 which relocates
and boots the staged kernel. A bad image just needs a power cycle (the SD kernel
is untouched).

  python3 tools/remote_chainload.py [host] [kernel.img]
  default host=192.168.3.100  kernel=compile/kernel8.img
"""
import sys, time, urllib.request

host = sys.argv[1] if len(sys.argv) > 1 else "192.168.3.100"
img  = sys.argv[2] if len(sys.argv) > 2 else "compile/kernel8.img"
CHUNK = 16384

data = open(img, "rb").read()
n = len(data)
print(f"[chainload] {img}: {n} bytes -> http://{host}  ({(n+CHUNK-1)//CHUNK} chunks of {CHUNK}B)")
def post_chunk(off, chunk):
    req = urllib.request.Request(f"http://{host}/chainload?off={off}",
                                 data=chunk, method="POST",
                                 headers={"Content-Type": "application/octet-stream"})
    return urllib.request.urlopen(req, timeout=20).read().decode("ascii", "replace").strip()

t0 = time.time()
for off in range(0, n, CHUNK):
    chunk = data[off:off+CHUNK]
    # Validate + retry: the running server occasionally mishandles the FIRST
    # POST after handoff (the request arrives split across TCP segments and
    # falls through to the generic 404 fallback), silently dropping that chunk.
    # If chunk 0 is lost the staged image keeps stale bytes 0..16KB and the
    # relocated kernel hangs on boot.  So require an "ok off=<off>" ack and
    # retry up to 5x before aborting.
    want = f"ok off={off}"
    r = ""
    for attempt in range(5):
        try:
            r = post_chunk(off, chunk)
        except Exception as e:
            r = f"<{type(e).__name__}>"
        if r.startswith(want):
            break
        time.sleep(0.3)
    if not r.startswith(want):
        sys.exit(f"[chainload] FATAL: chunk off={off} not acked after 5 tries "
                 f"(last reply: {r!r}). Aborting — staged image would be corrupt.")
    if off % (CHUNK*16) == 0 or off + CHUNK >= n:
        print(f"  off={off:>8} -> {r}")
print(f"[chainload] staged {n} bytes in {time.time()-t0:.1f}s. triggering boot...")
try:
    urllib.request.urlopen(f"http://{host}/chainload?go=1&len={n}", timeout=8).read()
except Exception as e:
    print(f"  (connection dropped as expected — kernel jumped: {type(e).__name__})")
print("[chainload] done. Pi4 is booting the new kernel (~5-10s). Verify:")
print(f"           curl 'http://{host}/shell?cmd=help'")
