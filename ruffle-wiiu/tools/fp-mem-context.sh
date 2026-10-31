#!/bin/bash
# Prints float loads/stores in generated wasm code (w2c_flashplayer_*) whose address register was just
# computed with subf from the memory's data_end (lwz rX,20(..) = wasm_rt_memory_t.data_end): those are
# float accesses to wasm memory, which may be misaligned (an alignment exception on the console).
# Usage: fp-mem-context.sh [max lines]
source /etc/profile.d/devkit-env.sh
elf="$(dirname "$0")/../build-wiiu/flashplayeremu.elf"
"$DEVKITPPC/bin/powerpc-eabi-objdump" -d --no-show-raw-insn "$elf" | awk -v max="${1:-40}" '
    /^[0-9a-f]+ <.*>:$/ { fn = $2; nsub = 0; next }
    fn !~ /w2c_flashplayer/ { next }
    $2 == "subf" || $2 == "subfic" { split($3, r, ","); subreg[r[1]] = NR }
    # lis rX,... after that: rX now points at a constant table (float constants), not wasm memory.
    $2 == "lis" { split($3, r, ","); delete subreg[r[1]] }
    # An unconditional branch ends the block: the next lines are reached from elsewhere.
    $2 == "b" || $2 == "blr" || $2 == "bctr" { delete subreg }
    $2 ~ /^(lf[sd]u?x?|stf[sd]u?x?)$/ {
        ops = $3
        if ($2 ~ /x$/) { n = split(ops, r, ","); base = r[2] } else { sub(/^[^(]*\(/, "", ops); sub(/\).*/, "", ops); base = ops }
        if (base in subreg && NR - subreg[base] < 6) { hits++; if (hits <= max) print fn, $0 }
    }
    END { print "suspicious float accesses to wasm memory:", hits + 0 }'
