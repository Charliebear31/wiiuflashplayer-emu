#!/bin/bash
# Counts mfcr and the `mcrf 0,0` guards as-crfix put before them in the Wii U binary.
source /etc/profile.d/devkit-env.sh
elf="$(dirname "$0")/../build-wiiu/flashplayeremu.elf"
dis=$("$DEVKITPPC/bin/powerpc-eabi-objdump" -d "$elf")
echo "mfcr: $(grep -c $'\tmfcr' <<<"$dis")"
echo "mcrf cr0,cr0: $(grep -cE $'\tmcrf\\s+cr0,cr0' <<<"$dis")"
