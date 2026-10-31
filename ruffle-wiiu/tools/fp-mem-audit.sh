#!/bin/bash
# Lists float loads/stores (lfs/lfd/stfs/stfd and their indexed/update forms) whose address register isn't
# the stack (r1, r31 as frame pointer) or the small-data base (r2/r13), per function, in the Wii U binary.
# On the Wii U's CPU a float access that isn't word-aligned is an alignment exception, so any such access to
# wasm memory (which may be unaligned) is a console-only crash. Expected: none in w2c_flashplayer_* code.
source /etc/profile.d/devkit-env.sh
elf="$(dirname "$0")/../build-wiiu/flashplayeremu.elf"
"$DEVKITPPC/bin/powerpc-eabi-objdump" -d --no-show-raw-insn "$elf" | awk '
    /^[0-9a-f]+ <.*>:$/ { fn = $2; next }
    $2 ~ /^(lf[sd]u?x?|stf[sd]u?x?|stfiwx|psq_lu?x?|psq_stu?x?)$/ {
        ops = $3
        if ($2 ~ /x$/) { n = split(ops, r, ","); base = r[2] }
        else { sub(/^[^(]*\(/, "", ops); sub(/\).*/, "", ops); base = ops }
        if (base != "r1" && base != "r31" && base != "r2" && base != "r13") n_fn[fn]++
    }
    END { for (f in n_fn) print n_fn[f], f }' | sort -rn | grep -i "w2c_flashplayer" | head -20
echo "---- total w2c functions with heap float accesses:"
"$DEVKITPPC/bin/powerpc-eabi-objdump" -d --no-show-raw-insn "$elf" | awk '
    /^[0-9a-f]+ <.*>:$/ { fn = $2; next }
    fn ~ /w2c_flashplayer/ && $2 ~ /^(lf[sd]u?x?|stf[sd]u?x?)$/ {
        ops = $3
        if ($2 ~ /x$/) { n = split(ops, r, ","); base = r[2] }
        else { sub(/^[^(]*\(/, "", ops); sub(/\).*/, "", ops); base = ops }
        if (base != "r1" && base != "r31" && base != "r2" && base != "r13") c++
    }
    END { print c + 0 }'
