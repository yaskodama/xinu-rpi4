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
t0 = time.time()
for off in range(0, n, CHUNK):
    chunk = data[off:off+CHUNK]
    req = urllib.request.Request(f"http://{host}/chainload?off={off}",
                                 data=chunk, method="POST",
                                 headers={"Content-Type": "application/octet-stream"})
    r = urllib.request.urlopen(req, timeout=20).read().decode("ascii", "replace").strip()
    if off % (CHUNK*16) == 0 or off + CHUNK >= n:
        print(f"  off={off:>8} -> {r}")
print(f"[chainload] staged {n} bytes in {time.time()-t0:.1f}s. triggering boot...")
try:
    urllib.request.urlopen(f"http://{host}/chainload?go=1&len={n}", timeout=8).read()
except Exception as e:
    print(f"  (connection dropped as expected — kernel jumped: {type(e).__name__})")
print("[chainload] done. Pi4 is booting the new kernel (~5-10s). Verify:")
print(f"           curl 'http://{host}/shell?cmd=help'")
