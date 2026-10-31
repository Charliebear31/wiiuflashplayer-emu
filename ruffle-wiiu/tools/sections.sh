#!/bin/bash
# Prints the Wii U binary's section sizes and addresses (to spot a section outgrowing its region).
source /etc/profile.d/devkit-env.sh
"$DEVKITPPC/bin/powerpc-eabi-objdump" -h "$(dirname "$0")/../build-wiiu/flashplayeremu.elf" | grep -E '^\s+[0-9]+ '
