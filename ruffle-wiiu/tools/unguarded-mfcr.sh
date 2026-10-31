#!/bin/bash
# Lists functions in the Wii U binary that have an mfcr without as-crfix's `mcrf 0,0` guard before it
# (code that didn't go through our assembler, e.g. prebuilt libraries). Count per function.
source /etc/profile.d/devkit-env.sh
elf="$(dirname "$0")/../build-wiiu/flashplayeremu.elf"
"$DEVKITPPC/bin/powerpc-eabi-objdump" -d --no-show-raw-insn "$elf" | awk '
    /^[0-9a-f]+ <.*>:$/ { fn = $2; prev = ""; next }
    /\tmfcr/ { if (prev !~ /mcrf[ \t]+cr0,cr0/) n[fn]++ }
    { prev = $0 }
    END { for (f in n) { print n[f], f; total += n[f] } print total, "TOTAL" }' | sort -n | tail -25
