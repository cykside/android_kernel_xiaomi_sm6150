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
  for src in /sys/fs/pstore/dmesg-ramoops-* /sys/fs/pstore/console-ramoops-* /sys/fs/pstore/pmsg-ramoops-* /sys/fs/pstore/ftrace-ramoops-*; do
    [ -f "$src" ] || continue;
    if cp "$src" "$dir/${src##*/}"; then count=$((count + 1)); fi;
  done;
  if cat /proc/last_kmsg > "$dir/last_kmsg.txt" 2>/dev/null && [ -s "$dir/last_kmsg.txt" ]; then
    count=$((count + 1));
  else
    rm -f "$dir/last_kmsg.txt";
  fi;
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
