#!/bin/bash
# Maps Wii U code addresses (e.g. from a "trap callers:" log line) to function names in the ELF.
# Usage: addr2fn.sh 0204abcd 02050000 ...
source /etc/profile.d/devkit-env.sh
elf="$(dirname "$0")/../build-wiiu/flashplayeremu.elf"
for a in "$@"; do
  [ "$a" = "00000000" ] && continue
  printf '%s  ' "$a"
  "$DEVKITPPC/bin/powerpc-eabi-addr2line" -f -e "$elf" "0x$a" | head -1 | cut -c1-220
done
