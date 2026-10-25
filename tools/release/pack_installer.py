import io, os, struct, sys

DIST = r"C:\hot_rain\dist_v3"
STUB = r"C:\hot_rain\cpp\hotrainy\tools\installer\build\hr_installer_stub.exe"
OUT  = r"C:\hot_rain\hotrainy_installer.exe"
FILES = ["hotrainy.exe", "GlassTAP.dll", "GlassBoot.dll", "使用说明.txt", "uninstall.cmd"]
MAGIC = 0x31524648  # 'HRF1'

if not os.path.exists(STUB):
    print("!! 没找到 stub：%s" % STUB); sys.exit(1)

payload = bytearray()
count = 0
for name in FILES:
    p = os.path.join(DIST, name)
    if not os.path.exists(p):
        print("!! 缺文件: %s" % p); sys.exit(1)
    data = open(p, "rb").read()
    nb = name.encode("utf-8")
    payload += struct.pack("<I", len(nb)) + nb + struct.pack("<Q", len(data)) + data
    count += 1
    print("  + %-20s %10d B" % (name, len(data)))

stub = open(STUB, "rb").read()
offset = len(stub)
footer = struct.pack("<IIQ", MAGIC, count, offset)

with open(OUT, "wb") as f:
    f.write(stub)
    f.write(payload)
    f.write(footer)

sz = os.path.getsize(OUT)
print("\n  stub   : %d B" % len(stub))
print("  payload: %d B (%d 个文件)" % (len(payload), count))
print("  output : %s  (%.2f MB)" % (OUT, sz / 1024.0 / 1024.0))