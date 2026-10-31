#!/bin/bash
# Prints the first copy of each named function from the Wii U binary. Usage: disasm-fn.sh name...
source /etc/profile.d/devkit-env.sh
elf="$(dirname "$0")/../build-wiiu/flashplayeremu.elf"
dis=$("$DEVKITPPC/bin/powerpc-eabi-objdump" -d --no-show-raw-insn "$elf")
for f in "$@"; do
  awk -v f="<$f>:" '$2 == f { p = 1; n = 0 } p { print; if (++n > 30 || /\tblr/) exit }' <<<"$dis"
done
