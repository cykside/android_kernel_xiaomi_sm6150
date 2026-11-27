# SPDX-License-Identifier: GPL-2.0-only
properties() { '
kernel.string=Surya TWRP log collector (no partition writes)
do.devicecheck=1
do.modules=0
do.systemless=0
do.cleanup=1
do.cleanuponabort=0
device.name1=surya
'; }

# Use the recovery installer UI, but never source the boot flashing core.
ui_print() { printf 'ui_print %s\nui_print\n' "$*" > "/proc/self/fd/$OUTFD"; }
collect() {
  local dir src count;
  dir=$(mktemp -d "$1/surya-boot-logs.XXXXXX") || return 1;
  count=0;
  # Some recovery builds leave the pstore filesystem unmounted.
  # Mounting it does not write Boot, DTBO or erase persistent records.
  if ! grep -q ' /sys/fs/pstore pstore ' /proc/mounts; then
    mount -t pstore -o ro pstore /sys/fs/pstore > "$dir/pstore-mount.txt" 2>&1;
  fi;
  grep ' /sys/fs/pstore ' /proc/mounts >> "$dir/pstore-mount.txt";
  ls -la /sys/fs/pstore >> "$dir/pstore-mount.txt" 2>&1;
  for src in /sys/fs/pstore/dmesg-ramoops-* /sys/fs/pstore/console-ramoops-* /sys/fs/pstore/pmsg-ramoops-* /sys/fs/pstore/ftrace-ramoops-*; do
    [ -s "$src" ] || continue;
    if cp "$src" "$dir/${src##*/}"; then count=$((count + 1)); fi;
  done;
  if cat /proc/last_kmsg > "$dir/last_kmsg.txt" 2>/dev/null && [ -s "$dir/last_kmsg.txt" ]; then
    count=$((count + 1));
  else
    rm -f "$dir/last_kmsg.txt";
  fi;
  # pstore can show empty even with the ring data intact in RAM: a
  # ramoops driver may have zapped the 12-byte header cursor while the
  # earlycon anchor at the far end of the window still carries it.
  # Recovery images often omit the /dev/mem node without disabling
  # DEVMEM itself, so create one when it is missing.  Then dump the
  # whole 4 MiB ramoops region; the decoder scans it for either the
  # prz header or the earlycon anchor wherever they landed.
  memnode=/dev/mem;
  if [ ! -r "$memnode" ]; then
    mknod /dev/mem c 1 1 2>/dev/null || mknod "$dir/mem" c 1 1 2>/dev/null;
    [ -r "$memnode" ] || memnode="$dir/mem";
  fi;
  if [ -r "$memnode" ]; then
    if dd if="$memnode" of="$dir/ramoops-region.raw" bs=4096 \
         skip=$((0x9d800000 / 4096)) count=$((0x400000 / 4096)) 2>/dev/null &&
       [ -s "$dir/ramoops-region.raw" ]; then
      count=$((count + 1));
      printf 'raw ramoops region 0x9d800000+0x400000 captured\n' > "$dir/early-raw.txt";
    else
      rm -f "$dir/ramoops-region.raw";
      printf 'dd from %s failed\n' "$memnode" > "$dir/early-raw.txt";
    fi;
    printf 'kallsyms DEVMEM probe:\n' >> "$dir/early-raw.txt";
    grep -c devmem /proc/kallsyms >> "$dir/early-raw.txt" 2>&1 || true;
  else
    printf 'no /dev/mem even after mknod (CONFIG_DEVMEM off)\n' > "$dir/early-raw.txt";
  fi;
  ls -l /dev/mem >> "$dir/early-raw.txt" 2>&1;
  dmesg 2>/dev/null | grep -iE 'ramoops|pstore|persistent_ram|devmem' > "$dir/twrp-pstore-facts.txt" 2>&1;
  dmesg > "$dir/twrp-current-dmesg.txt" 2>/dev/null;
  printf 'Previous boot files copied: %s\ntwrp-current-dmesg.txt is from recovery, not the failed kernel.\n' "$count" > "$dir/README.txt";
  sync;
  ui_print "Previous boot files copied: $count";
  ui_print "Report saved in $dir";
  return 0;
}

for storage in /sdcard/Download /external_sd /usb_otg; do
  [ -d "$storage" ] || continue;
  collect "$storage" && exit 0;
done;
ui_print 'No writable storage. Mount microSD or decrypt Data in TWRP, then retry.';
exit 1;
