# SPDX-License-Identifier: GPL-2.0-only
properties() { '
kernel.string=Surya Linux 6.1 boot test with ramoops (no KernelSU)
do.devicecheck=1
do.modules=0
do.systemless=0
do.cleanup=1
do.cleanuponabort=0
device.name1=surya
'; }

BLOCK=/dev/block/bootdevice/by-name/boot;
IS_SLOT_DEVICE=0;
RAMDISK_COMPRESSION=auto;
PATCH_VBMETA_FLAG=0;
. tools/ak3-core.sh;

# Fail closed if recovery does not confirm an unlocked boot chain.
[ "$(getprop ro.boot.vbmeta.device_state)" = unlocked ] &&
  [ "$(getprop ro.boot.verifiedbootstate)" = orange ] ||
  abort 'Surya test requires bootloader state unlocked/orange; nothing written.';

DTBO_BLOCK=/dev/block/bootdevice/by-name/dtbo;
[ -b "$BLOCK" ] && [ -b "$DTBO_BLOCK" ] || abort 'Expected Surya partitions not found.';
[ "$(blockdev --getsize64 "$BLOCK")" = 134217728 ] || abort 'Unexpected boot partition size.';
[ "$(od -An -tx1 -N8 "$BLOCK" | tr -d ' \n')" = 414e44524f494421 ] || abort 'Expected Android boot magic.';
[ "$(od -An -tx1 -j36 -N8 "$BLOCK" | tr -d ' \n')" = 0010000002000000 ] || abort 'Expected a 4096-byte Android boot v2 layout.';
[ "$(od -An -tx1 -N4 "$DTBO_BLOCK" | tr -d ' \n')" = d7b7ab1e ] || abort 'Unexpected active DTBO format.';
for path in /dev/block/mapper/dtbo /dev/block/by-name/dtbo; do
  if [ -e "$path" ] && [ "$(readlink -f "$path")" != "$(readlink -f "$DTBO_BLOCK")" ]; then
    abort 'Ambiguous DTBO target.';
  fi;
done;
(cd "$AKHOME" && sha256sum -c SURYA-SHA256SUMS) || abort 'Payload checksum verification failed.';
dtbo_capacity=$(blockdev --getsize64 "$DTBO_BLOCK") || abort 'Cannot read DTBO capacity.';
[ "$(wc -c < "$AKHOME/dtbo.img")" -le "$dtbo_capacity" ] || abort 'DTBO exceeds partition size.';

ui_print 'Experimental: writing Boot and DTBO only. Do not reboot if installation fails.';
split_boot;
[ -s "$SPLITIMG/dtb" ] || abort 'Reference boot does not contain a separate DTB.';
[ -s "$SPLITIMG/header" ] || abort 'Missing boot header metadata.';
patch_cmdline 'earlycon=' 'earlycon=ramoops,0x9da00000,0x40000';
patch_cmdline 'reboot=' 'reboot=warm';
patch_cmdline 'panic=' 'panic=5';
# Keep PSCI capability notices and warnings, but suppress routine info.
patch_cmdline 'loglevel=' 'loglevel=7';
patch_cmdline 'msm_rtb.filter=' 'msm_rtb.filter=0x0';
patch_cmdline 'rcupdate.rcu_expedited=1' 'rcupdate.rcu_expedited=0';
grep -Eq '^cmdline=.*earlycon=ramoops,0x9da00000,0x40000( |$)' "$SPLITIMG/header" ||
  abort 'Log argument missing; nothing written.';
flash_boot;
flash_generic dtbo;
ui_print 'On boot loop, return directly to TWRP and run Surya-TWRP-Log-Collector.zip before restoring.';
ui_print 'If Android does not boot, restore the matching FSociety Boot and DTBO backups in TWRP.';
