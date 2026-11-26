# SPDX-License-Identifier: GPL-2.0-only
properties() { '
kernel.string=Surya Linux 6.1 recovery repack diagnostic (no partition writes)
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

# The packaged core calls this instead of any block-device write.
surya_report_repack() {
  local report temp reference newimg;
  report="$AKHOME/surya-repack-report.txt";
  reference="$AKHOME/boot.img";
  newimg="$AKHOME/boot-new.img";
  temp="$AKHOME/surya-repack-check";
  mkdir -p "$temp/old" "$temp/new" || abort 'Cannot prepare diagnostic workspace.';
  {
    printf 'Surya 6.1 TWRP repack diagnostic: no partition writes\n';
    printf 'Verified Boot: %s / %s\n' "$(getprop ro.boot.vbmeta.device_state)" "$(getprop ro.boot.verifiedbootstate)";
    printf 'Reference and repacked size: %s / %s bytes\n' "$(wc -c < "$reference")" "$(wc -c < "$newimg")";
    printf 'Repacked boot header bytes 36–43: ';
    od -An -tx1 -j36 -N8 "$newimg";
    sha256sum "$newimg" "$AKHOME/dtb" "$AKHOME/dtbo.img";
    if (cd "$temp/old" && magiskboot unpack -n "$reference") &&
       (cd "$temp/new" && magiskboot unpack -n "$newimg"); then
      if cmp -s "$temp/new/dtb" "$AKHOME/dtb"; then
        printf 'DTB: replacement matches byte for byte\n';
      else
        printf 'DTB: missing or differs from replacement\n';
      fi;
      if [ -f "$temp/old/ramdisk.cpio" ] &&
         cmp -s "$temp/old/ramdisk.cpio" "$temp/new/ramdisk.cpio"; then
        printf 'Ramdisk: raw bytes unchanged\n';
      else
        printf 'Ramdisk: missing or raw bytes differ (may be recompressed)\n';
      fi;
    else
      printf 'Unable to unpack reference or candidate for comparison\n';
    fi;
  } > "$report" 2>&1;
  while IFS= read -r line; do ui_print "$line"; done < "$report";
  if [ -d /sdcard/Download ] && cp "$report" /sdcard/Download/surya-repack-report.txt; then
    ui_print 'Report saved to Download/surya-repack-report.txt';
  fi;
  return 0;
}

ui_print 'Diagnostic only: Boot, DTBO and vbmeta will not be written.';
# Refuse to run an unmodified AnyKernel3 core.
grep -Fq 'surya_report_repack; # SURYA_DIAGNOSTIC_NO_FLASH' tools/ak3-core.sh ||
  abort 'Diagnostic write guard missing.';
split_boot;
[ -s "$SPLITIMG/dtb" ] || abort 'Reference boot does not contain a separate DTB.';
flash_boot;
ui_print 'Diagnostic complete. No partitions were written.';
