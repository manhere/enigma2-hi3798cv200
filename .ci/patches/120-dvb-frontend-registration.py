# ---- lib/dvb/dvb.cpp + dvb.h：主线下核 C++ 前端注册 ----
# 上游 C++ 资源管理器只注册 adapter0；adapter1..N 仅当 isusb()
# 时走 eDVBUsbAdapter vtuner 桥，而该桥第一步就 open
# /proc/bus/nim_sockets，失败即放弃（也没有 /dev/vtunerN）。
# 盒子实测：histb-dmx 每个前端一个平台 adapter（0-3），
# USB HD2312 为 adapter4 → C++ 只注册了 1 个前端，
# allocateRawChannel(4) 失败 → "无法运行频道扫描"（2026-10-30）。
# 修复：adapter0 先注册，其余 adapter 按序直接注册（有 vtuner
# 基础设施时仍走原桥）；scanDevices 加 fe_base 基址保证
# eDVBFrontend slotid 全局唯一且与 feid / NimManager 一致。
p = 'lib/dvb/dvb.h'
s = open(p).read()
old = '\tvoid scanDevices();\n'
new = '\tvoid scanDevices(int fe_base = 0);\n'
if old in s:
    open(p, 'w').write(s.replace(old, new, 1))
elif 'scanDevices(int fe_base' in s:
    print('dvb.h already patched')
else:
    raise SystemExit('dvb.h scanDevices anchor not found - upstream changed, review needed')
print('dvb.h scanDevices(fe_base) patched')

p = 'lib/dvb/dvb.cpp'
s = open(p).read()
old = ('\tint num_adapter = 1;\n'
       '\twhile (eDVBAdapterLinux::exist(num_adapter))\n'
       '\t{\n'
       '\t\tif (eDVBAdapterLinux::isusb(num_adapter))\n'
       '\t\t{\n'
       '\t\t\teDVBAdapterLinux *adapter = new eDVBUsbAdapter(num_adapter);\n'
       '\t\t\taddAdapter(adapter);\n'
       '\t\t}\n'
       '\t\tnum_adapter++;\n'
       '\t}\n'
       '\n'
       '\n'
       '\tif (eDVBAdapterLinux::exist(0))\n'
       '\t{\n'
       '\t\teDVBAdapterLinux *adapter = new eDVBAdapterLinux(0);\n'
       '\t\tadapter->scanDevices();\n'
       '\t\taddAdapter(adapter, true);\n'
       '\t}\n')
new = ('\tif (eDVBAdapterLinux::exist(0))\n'
       '\t{\n'
       '\t\teDVBAdapterLinux *adapter = new eDVBAdapterLinux(0);\n'
       '\t\tadapter->scanDevices();\n'
       '\t\taddAdapter(adapter, true);\n'
       '\t}\n'
       '\n'
       '\t/*\n'
       '\t * DVBIP-1002 mainline kernel: histb-dmx registers one adapter\n'
       '\t * per frontend (platform subsystem, not usb) and there is no OE\n'
       '\t * vtuner infrastructure (/proc/bus/nim_sockets, /dev/vtunerN).\n'
       '\t * Upstream only registers adapter0 and routes every other\n'
       '\t * adapter through the eDVBUsbAdapter vtuner bridge, which bails\n'
       '\t * out without those files - leaving all adapters beyond 0\n'
       '\t * unregistered (allocateRawChannel failures on every tuner but\n'
       '\t * A). Register every adapter directly in ascending order so the\n'
       '\t * resource-manager frontend index (feid) and eDVBFrontend slot\n'
       '\t * ids stay 1:1 with the adapter numbers and NimManager; fall\n'
       '\t * back to the vtuner bridge only when its infrastructure exists.\n'
       '\t */\n'
       '\tint num_adapter = 1;\n'
       '\tint fe_base = eDVBAdapterLinux::exist(0) ? 1 : 0;\n'
       '\twhile (eDVBAdapterLinux::exist(num_adapter))\n'
       '\t{\n'
       '\t\teDVBAdapterLinux *adapter;\n'
       '\t\tif (eDVBAdapterLinux::isusb(num_adapter)\n'
       '\t\t\t&& access("/proc/bus/nim_sockets", R_OK) == 0\n'
       '\t\t\t&& (access("/dev/vtuner0", F_OK) == 0 || access("/dev/misc/vtuner0", F_OK) == 0))\n'
       '\t\t{\n'
       '\t\t\tadapter = new eDVBUsbAdapter(num_adapter);\n'
       '\t\t}\n'
       '\t\telse\n'
       '\t\t{\n'
       '\t\t\tadapter = new eDVBAdapterLinux(num_adapter);\n'
       '\t\t\tadapter->scanDevices(fe_base);\n'
       '\t\t\tfe_base += adapter->getNumFrontends();\n'
       '\t\t}\n'
       '\t\taddAdapter(adapter);\n'
       '\t\tnum_adapter++;\n'
       '\t}\n')
if old in s:
    s = s.replace(old, new, 1)
elif 'DVBIP-1002 mainline kernel: histb-dmx registers one adapter' in s:
    print('dvb.cpp adapter registration already patched')
else:
    raise SystemExit('dvb.cpp adapter loop anchor not found - upstream changed, review needed')
old = 'void eDVBAdapterLinux::scanDevices()\n'
new = 'void eDVBAdapterLinux::scanDevices(int fe_base)\n'
if old in s:
    s = s.replace(old, new, 1)
elif 'scanDevices(int fe_base)' in s:
    pass
else:
    raise SystemExit('dvb.cpp scanDevices anchor not found - upstream changed, review needed')
# Upstream replaced the shared 'ok' flag with separate simulateOk / frontendOk
# probes and took ownership through ePtr. Both constructors still receive the
# slot id, so both need the fe_base offset to keep slot ids globally unique;
# the num_fe used for /sys and /dev paths above must stay unshifted.
FRONTEND_CTORS = {
    'simulate': (
        '\t\tePtr<eDVBFrontend> simulateFrontend = new eDVBFrontend(name.c_str(), num_fe, simulateOk, true);\n',
        '\t\tePtr<eDVBFrontend> simulateFrontend = new eDVBFrontend(name.c_str(), num_fe + fe_base, simulateOk, true);\n',
        '\t\t\tfe = new eDVBFrontend(name.c_str(), num_fe, ok, true);\n',
        '\t\t\tfe = new eDVBFrontend(name.c_str(), num_fe + fe_base, ok, true);\n',
        'num_fe + fe_base, simulateOk, true',
        'num_fe + fe_base, ok, true',
    ),
    'real': (
        '\t\tePtr<eDVBFrontend> frontend = new eDVBFrontend(name.c_str(), num_fe, frontendOk, false, simulateFrontend);\n',
        '\t\tePtr<eDVBFrontend> frontend = new eDVBFrontend(name.c_str(), num_fe + fe_base, frontendOk, false, simulateFrontend);\n',
        '\t\t\tfe = new eDVBFrontend(name.c_str(), num_fe, ok, false, fe);\n',
        '\t\t\tfe = new eDVBFrontend(name.c_str(), num_fe + fe_base, ok, false, fe);\n',
        'num_fe + fe_base, frontendOk, false',
        'num_fe + fe_base, ok, false',
    ),
}
for kind, (new_old, new_new, old_old, old_new, new_done, old_done) in FRONTEND_CTORS.items():
    if new_old in s:
        s = s.replace(new_old, new_new, 1)
        continue
    if old_old in s:
        s = s.replace(old_old, old_new, 1)
        continue
    if new_done in s or old_done in s:
        continue
    raise SystemExit('dvb.cpp %s frontend anchor not found - upstream changed, review needed' % kind)
open(p, 'w').write(s)
print('dvb.cpp mainline adapter registration patched')
