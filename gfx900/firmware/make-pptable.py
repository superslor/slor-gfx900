#!/usr/bin/env python3
# Make a Vega 10 PowerPlay table from the card's own (a copy of /sys/class/drm/cardN/device/pp_table).
# Layout: Linux drivers/gpu/drm/amd/pm/powerplay/hwmgr/vega10_pptable.h (packed, little-endian).
import argparse, hashlib, struct

ap = argparse.ArgumentParser()
ap.add_argument("src", help="the card's own table (sudo cat /sys/class/drm/cardN/device/pp_table > card.pptable)")
ap.add_argument("out", help="output table; <out minus .pptable>.orig.sha256 is written beside it")
ap.add_argument("--watts", type=int, default=200, help="socket/battery/small power limit (default 200)")
ap.add_argument("--soc-stock", action="store_true",
                help="SoC clock levels 1-4 at 950/1000/1050/1100 mV and memory clock levels 2-3 at 1000/1050 mV")
a = ap.parse_args()

src = open(a.src, "rb").read()
b = bytearray(src)
u16 = lambda o: struct.unpack_from("<H", b, o)[0]
u32 = lambda o: struct.unpack_from("<I", b, o)[0]
table = lambda i: u16(48 + 2 * i)          # 3 SocclkDep, 4 MclkDep, 7 VddcLookup, 12 PowerTune

pt = table(12)
if b[pt] != 7:
    raise SystemExit(f"PowerTune table rev {b[pt]}, expected 7")
old = [u16(pt + o) for o in (1, 3, 5)]
for o in (1, 3, 5):
    struct.pack_into("<H", b, pt + o, a.watts)
print(f"power limits {old} -> {a.watts} W")

if a.soc_stock:
    look = [u16(table(7) + 2 + 2 * k) for k in range(b[table(7) + 1])]
    def records(i, size):
        o = table(i)
        return [o + 2 + k * size for k in range(b[o + 1])]
    for r, mhz, mv in zip(records(3, 5)[1:5], (720, 847, 960, 1028), (950, 1000, 1050, 1100)):
        if u32(r) // 100 != mhz or mv not in look:
            raise SystemExit("SoC clock levels differ from the MI25 table this was made for; leave out --soc-stock")
        b[r + 4] = look.index(mv)
    for r, mhz, mv in zip(records(4, 7)[2:4], (800, 945), (1000, 1050)):
        if u32(r) // 100 != mhz or mv not in look:
            raise SystemExit("memory clock levels differ from the MI25 table this was made for; leave out --soc-stock")
        b[r + 4] = look.index(mv)
    print("SoC / memory clock levels moved to the stock MI25 voltages")

open(a.out, "wb").write(bytes(b))
base = a.out[:-len(".pptable")] if a.out.endswith(".pptable") else a.out
open(base + ".orig.sha256", "w").write(hashlib.sha256(src).hexdigest() + "\n")
print(f"wrote {a.out} and {base}.orig.sha256")
