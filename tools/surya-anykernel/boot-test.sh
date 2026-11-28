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

# Name the build in the install log, which is part of the report the board sends
# back anyway; a boot that cannot say which kernel it ran is a boot that cannot
# be reasoned about.
if [ -s "$AKHOME/SURYA-VERSION" ]; then
  ui_print "$(cat "$AKHOME/SURYA-VERSION")";
else
  ui_print 'SURYA-VERSION missing: this zip did not come from the stamped build.';
fi;
ui_print 'Experimental: writing Boot and DTBO only. Do not reboot if installation fails.';
split_boot;
[ -s "$SPLITIMG/dtb" ] || abort 'Reference boot does not contain a separate DTB.';
[ -s "$SPLITIMG/header" ] || abort 'Missing boot header metadata.';
patch_cmdline 'earlycon=' 'earlycon=ramoops,0x9da00000,0x40000';
patch_cmdline 'reboot=' 'reboot=warm';
patch_cmdline 'panic=' 'panic=5';
# Keep every probe message, including the KERN_INFO deferral reasons.
patch_cmdline 'loglevel=' 'loglevel=8';
patch_cmdline 'msm_rtb.filter=' 'msm_rtb.filter=0x0';
patch_cmdline 'rcupdate.rcu_expedited=1' 'rcupdate.rcu_expedited=0';
# Probe-blocking and idle bring-up knobs, mirrored in the Surya DT chosen node:
# fw_devlink=permissive stops consumers (UFS first) from waiting on suppliers
# that can never report probed, cpuidle.off=1 keeps every CPU out of the PSCI
# idle path this firmware rejects.
patch_cmdline 'fw_devlink=' 'fw_devlink=permissive';
patch_cmdline 'cpuidle.off=' 'cpuidle.off=1';
# The 6.1 clock core runs clk_disable_unused() at late_initcall and gates every
# bootloader-enabled clock that has no 6.1 consumer; the downstream 4.14 driver
# never does, it holds its critical-clk list open instead.  gcc_ufs_phy_ice_core_
# clk, gcc_ufs_phy_unipro_core_clk and gcc_aggre_ufs_phy_axi_clk have no consumer
# here, so without this they are cut while the host is talking to the block.
patch_cmdline 'clk_ignore_unused' 'clk_ignore_unused';
# Panic on a soft lockup, so a report about a stuck CPU is the last thing in the
# prz instead of a line that scrolls out before init gives up.
patch_cmdline 'softlockup_panic=' 'softlockup_panic=1';
# initcall_debug is not patched on any more: one line per probe and per initcall
# wraps the 2 MiB console ramoops prz mid-boot (capture 11 came back interleaved
# with the next boot).  gcc_sm6150_probe() prints its own stage markers instead.
grep -Eq '^cmdline=.*earlycon=ramoops,0x9da00000,0x40000( |$)' "$SPLITIMG/header" ||
  abort 'Log argument missing; nothing written.';
# What the kernel actually runs with is the cmdline in the boot header, not the
# one in /chosen, so a knob that only reached the DTB is a knob that is not set.
# Check the boot-critical one in the repacked header and refuse to flash rather
# than spend a boot on a build that was never complete: the 2026-11-28 install
# log is exactly that case, where this file had not been given clk_ignore_unused
# at the same time the diagnostic installer got it.
grep -Eq '^cmdline=.*clk_ignore_unused( |$)' "$SPLITIMG/header" ||
  abort 'clk_ignore_unused missing from the repacked cmdline; the clock core would gate UFS at late_initcall.';
flash_boot;
flash_generic dtbo;
ui_print 'On boot loop, return directly to TWRP and run Surya-TWRP-Log-Collector.zip before restoring.';
ui_print 'If Android does not boot, restore the matching FSociety Boot and DTBO backups in TWRP.';
